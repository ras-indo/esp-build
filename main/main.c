#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "soc/soc.h"
#include "driver/rmt.h"

// ==================== Konfigurasi ====================
#define WIFI_CHANNEL            5               // Channel yang dipantau
#define NEOPIXEL_PIN            8               // Pin NeoPixel (sesuaikan)
#define NEO_DIM_COLOR           10              // Intensitas redup (0-255)
#define RINGBUF_SIZE            (32 * 1024)     // Ring buffer 32KB untuk frame

// Register RX DMA (berdasarkan analisis hal_mac_rx.o)
#define RX_DESC_HEAD_LOW        0x60033088
#define RX_DESC_LAST_LOW        0x60033090
#define RX_DESC_LAST_HIGH       0x60033c64
#define RX_DESC_RELOAD          0x60033084

// Rentang RAM ESP32-S3 (dari heap_init)
#define DRAM_START              0x3FC00000
#define DRAM_END                0x40000000

// Struktur descriptor RX (12 byte)
typedef struct rx_desc_s {
    uint32_t status;    // bit 0-11: panjang frame, bit 31: ownership (1=hw, 0=sw)
    uint32_t buffer;    // alamat fisik buffer
    struct rx_desc_s *next;
} rx_desc_t;

// ==================== RMT untuk NeoPixel ====================
#define RMT_TX_CHANNEL      RMT_CHANNEL_0
#define RMT_CLK_DIV         4       // 0.05us per tick (80MHz / 4 = 20MHz)
#define T0H_TICKS           7       // 350ns / 50ns = 7
#define T0L_TICKS           16      // 800ns / 50ns = 16
#define T1H_TICKS           14      // 700ns / 50ns = 14
#define T1L_TICKS           12      // 600ns / 50ns = 12

static rmt_item32_t neo_items[2];

static void neo_pixel_init(void) {
    rmt_config_t config = {
        .rmt_mode = RMT_MODE_TX,
        .channel = RMT_TX_CHANNEL,
        .gpio_num = NEOPIXEL_PIN,
        .clk_div = RMT_CLK_DIV,
        .mem_block_num = 1,
        .tx_config = {
            .loop_en = false,
            .carrier_en = false,
            .idle_output_en = true,
            .idle_level = RMT_IDLE_LEVEL_LOW,
        }
    };
    rmt_config(&config);
    rmt_driver_install(config.channel, 0, 0);

    // Item untuk bit 0
    neo_items[0].duration0 = T0H_TICKS;
    neo_items[0].level0 = 1;
    neo_items[0].duration1 = T0L_TICKS;
    neo_items[0].level1 = 0;

    // Item untuk bit 1
    neo_items[1].duration0 = T1H_TICKS;
    neo_items[1].level0 = 1;
    neo_items[1].duration1 = T1L_TICKS;
    neo_items[1].level1 = 0;
}

static void neo_pixel_set_color(uint8_t r, uint8_t g, uint8_t b) {
    // WS2812 urutan GRB
    uint32_t grb = (g << 16) | (r << 8) | b;
    rmt_item32_t items[24];
    for (int i = 0; i < 24; i++) {
        if (grb & (1 << (23 - i))) {
            items[i] = neo_items[1];  // bit 1
        } else {
            items[i] = neo_items[0];  // bit 0
        }
    }
    rmt_write_items(RMT_TX_CHANNEL, items, 24, true);  // blocking
}

// ==================== Akses Register ====================
static inline uint32_t get_rx_last_dscr(void) {
    uint32_t low = READ_PERI_REG(RX_DESC_LAST_LOW);
    uint32_t high = READ_PERI_REG(RX_DESC_LAST_HIGH);
    high &= 0xfff00000;  // hanya 12 bit teratas
    return high | low;
}

static inline uint32_t get_rx_head_dscr(void) {
    uint32_t low = READ_PERI_REG(RX_DESC_HEAD_LOW);
    uint32_t high = READ_PERI_REG(RX_DESC_LAST_HIGH) & 0xfff00000;
    return high | low;
}

// ==================== Validasi Alamat ====================
static bool is_valid_addr(uint32_t addr) {
    return (addr >= DRAM_START && addr < DRAM_END);
}

static bool is_valid_descriptor(rx_desc_t *desc) {
    if (desc == NULL) return false;
    uint32_t addr = (uint32_t)desc;
    if (!is_valid_addr(addr)) return false;
    if (desc->next != NULL && !is_valid_addr((uint32_t)desc->next)) return false;
    if (!is_valid_addr(desc->buffer)) return false;
    return true;
}

// ==================== Global Data ====================
static RingbufHandle_t frame_ringbuf = NULL;
static QueueHandle_t blink_queue;
static const char *TAG = "RAW_RX";

// ==================== Task Pemroses Frame ====================
static void frame_processor_task(void *arg) {
    while (1) {
        size_t item_size;
        uint8_t *frame = (uint8_t*)xRingbufferReceive(frame_ringbuf, &item_size, portMAX_DELAY);
        if (frame && item_size > 0) {
            // Analisis tipe frame
            uint8_t fc = frame[0];
            uint8_t type = (fc >> 2) & 0x03;
            uint8_t subtype = (fc >> 4) & 0x0F;

            if (type == 0 && subtype == 4) {
                ESP_LOGI(TAG, "Probe request, len=%" PRIu32, (uint32_t)item_size);
            } else {
                ESP_LOGD(TAG, "Frame type %d, len=%" PRIu32, type, (uint32_t)item_size);
            }

            // Tampilkan hex dump (max 64 byte)
            ESP_LOG_BUFFER_HEX(TAG, frame, item_size < 64 ? item_size : 64);

            vRingbufferReturnItem(frame_ringbuf, frame);
        }
    }
}

// ==================== Task Blink NeoPixel ====================
static void blink_task(void *arg) {
    int dummy;
    while (1) {
        if (xQueueReceive(blink_queue, &dummy, portMAX_DELAY)) {
            neo_pixel_set_color(NEO_DIM_COLOR, NEO_DIM_COLOR, NEO_DIM_COLOR); // putih redup
            vTaskDelay(pdMS_TO_TICKS(20));
            neo_pixel_set_color(0, 0, 0); // mati
        }
    }
}

// ==================== Task Polling RX DMA ====================
static void rx_poll_task(void *arg) {
    // Dapatkan head descriptor dengan validasi
    uint32_t head_addr = get_rx_head_dscr();
    if (!is_valid_addr(head_addr)) {
        ESP_LOGE(TAG, "Head descriptor tidak valid: 0x%08" PRIx32, head_addr);
        // Coba baca alternatif (gabungan langsung)
        head_addr = READ_PERI_REG(RX_DESC_HEAD_LOW) | (READ_PERI_REG(RX_DESC_LAST_HIGH) & 0xfff00000);
        if (!is_valid_addr(head_addr)) {
            ESP_LOGE(TAG, "Gagal mendapatkan head descriptor");
            vTaskDelete(NULL);
        }
    }

    rx_desc_t *head = (rx_desc_t*)head_addr;
    ESP_LOGI(TAG, "Head descriptor: 0x%08" PRIx32, head_addr);

    uint32_t last_dscr = head_addr;
    rx_desc_t *last_desc = head;

    while (1) {
        uint32_t current = get_rx_last_dscr();

        if (current != last_dscr && is_valid_addr(current)) {
            // Mulai dari descriptor setelah last
            rx_desc_t *desc = last_desc->next;
            int processed = 0;

            while (desc != NULL && is_valid_addr((uint32_t)desc) &&
                   (uint32_t)desc != current && processed < 32) {
                if (!is_valid_descriptor(desc)) {
                    ESP_LOGE(TAG, "Descriptor invalid: 0x%08" PRIx32, (uint32_t)desc);
                    break;
                }

                uint32_t status = desc->status;
                if (!(status & 0x80000000)) {  // milik software
                    uint32_t len = status & 0xfff;
                    if (len > 0 && len < 1600) {
                        uint8_t *frame = (uint8_t*)desc->buffer;
                        if (is_valid_addr((uint32_t)frame)) {
                            // Salin frame ke ring buffer
                            uint8_t *frame_copy = malloc(len);
                            if (frame_copy) {
                                memcpy(frame_copy, frame, len);
                                if (xRingbufferSend(frame_ringbuf, frame_copy, len, 0) != pdTRUE) {
                                    free(frame_copy);
                                    ESP_LOGW(TAG, "Ringbuffer penuh");
                                } else {
                                    // Trigger blink
                                    int dummy = 1;
                                    xQueueSend(blink_queue, &dummy, 0);
                                }
                            }
                        }
                        // Kembalikan ownership ke hardware
                        desc->status = status | 0x80000000;
                    }
                }
                desc = desc->next;
                processed++;
            }

            // Proses descriptor terakhir (current)
            if (desc != NULL && is_valid_addr((uint32_t)desc) && (uint32_t)desc == current) {
                if (is_valid_descriptor(desc)) {
                    uint32_t status = desc->status;
                    if (!(status & 0x80000000)) {
                        uint32_t len = status & 0xfff;
                        if (len > 0 && len < 1600) {
                            uint8_t *frame = (uint8_t*)desc->buffer;
                            if (is_valid_addr((uint32_t)frame)) {
                                uint8_t *frame_copy = malloc(len);
                                if (frame_copy) {
                                    memcpy(frame_copy, frame, len);
                                    if (xRingbufferSend(frame_ringbuf, frame_copy, len, 0) != pdTRUE) {
                                        free(frame_copy);
                                    } else {
                                        int dummy = 1;
                                        xQueueSend(blink_queue, &dummy, 0);
                                    }
                                }
                            }
                            desc->status = status | 0x80000000;
                        }
                    }
                }
            }

            // Set reload bit agar hardware tahu descriptor tersedia
            WRITE_PERI_REG(RX_DESC_RELOAD, READ_PERI_REG(RX_DESC_RELOAD) | 1);

            last_dscr = current;
            last_desc = (rx_desc_t*)current;
        }

        vTaskDelay(pdMS_TO_TICKS(5));  // polling 5ms
    }
}

// ==================== Inisialisasi Wi-Fi Promiscuous ====================
static void wifi_promiscuous_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Set channel dan promiscuous
    ESP_ERROR_CHECK(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    ESP_LOGI(TAG, "WiFi promiscuous started on channel %d", WIFI_CHANNEL);
}

// ==================== Main ====================
void app_main(void) {
    // Delay 10 detik setelah reset agar sempat buka serial monitor
    vTaskDelay(pdMS_TO_TICKS(10000));
    printf("=== RAW RX Sniffer with Ringbuffer ===\n");

    // Inisialisasi NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Inisialisasi NeoPixel
    neo_pixel_init();
    neo_pixel_set_color(0, 0, 0); // mati

    // Buat ring buffer
    frame_ringbuf = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (!frame_ringbuf) {
        ESP_LOGE(TAG, "Gagal membuat ring buffer");
        return;
    }

    // Buat queue untuk blink
    blink_queue = xQueueCreate(10, sizeof(int));
    if (!blink_queue) {
        ESP_LOGE(TAG, "Gagal membuat queue");
        return;
    }

    // Buat task blink (bisa di core mana saja)
    xTaskCreate(blink_task, "blink", 2048, NULL, 5, NULL);

    // Buat task pemroses frame di core 1 (agar tidak mengganggu polling)
    xTaskCreatePinnedToCore(frame_processor_task, "frame_proc", 4096, NULL, 5, NULL, 1);

    // Buat task polling RX di core 0
    xTaskCreatePinnedToCore(rx_poll_task, "rx_poll", 4096, NULL, 6, NULL, 0);

    // Inisialisasi Wi-Fi (akan memulai penerimaan frame)
    wifi_promiscuous_init();

    // Task utama selesai, scheduler akan menjalankan task-task di atas
}
