#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "soc/soc.h"
#include "driver/rmt.h"

// ==================== Konfigurasi ====================
#define WIFI_CHANNEL            5               // Channel Wi-Fi yang digunakan
#define NEOPIXEL_PIN            8               // GPIO untuk NeoPixel (sesuaikan)
#define NEO_DIM_COLOR           10              // Intensitas redup (0-255)

// Register RX DMA (berdasarkan analisis file .asm)
#define RX_DESC_HEAD            0x60033088      // Alamat head descriptor
#define RX_DESC_LAST_L          0x60033090      // Low 20-bit dari last descriptor
#define RX_DESC_LAST_H          0x60033c64      // High 12-bit dari last descriptor
#define RX_DESC_RELOAD          0x60033084      // Reload register (set bit 0 setelah memproses)

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

static rmt_item32_t neo_items[2];  // item untuk bit 0 dan 1

void neo_pixel_init(void) {
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

void neo_pixel_set_color(uint8_t r, uint8_t g, uint8_t b) {
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

// ==================== Fungsi Bantuan ====================
static const char *TAG = "RAW_RX";

// Mendapatkan alamat descriptor terakhir dari hardware (gabungan high + low)
static uint32_t get_rx_last(void) {
    uint32_t low = READ_PERI_REG(RX_DESC_LAST_L);
    uint32_t high = READ_PERI_REG(RX_DESC_LAST_H);
    high &= 0xfff00000;  // hanya 12 bit teratas
    return high | low;
}

// Mendapatkan alamat head descriptor
static uint32_t get_rx_head(void) {
    uint32_t low = READ_PERI_REG(RX_DESC_HEAD);
    uint32_t high = READ_PERI_REG(RX_DESC_LAST_H) & 0xfff00000;
    return high | low;
}

// Memeriksa apakah alamat berada dalam rentang RAM yang valid (sesuaikan dengan output heap)
static bool is_valid_addr(uint32_t addr) {
    // Rentang RAM untuk ESP32-S3 biasanya 0x3FC00000 - 0x40000000
    return (addr >= 0x3FC00000 && addr < 0x40000000);
}

// Queue untuk memberi sinyal ke task blink (agar tidak blocking di task polling)
static QueueHandle_t blink_queue;

// Task untuk menyalakan NeoPixel redup saat ada frame
void blink_task(void *arg) {
    int dummy;
    while (1) {
        if (xQueueReceive(blink_queue, &dummy, portMAX_DELAY)) {
            neo_pixel_set_color(NEO_DIM_COLOR, NEO_DIM_COLOR, NEO_DIM_COLOR); // putih redup
            vTaskDelay(pdMS_TO_TICKS(20));
            neo_pixel_set_color(0, 0, 0); // mati
        }
    }
}

// Task polling descriptor RX
void rx_poll_task(void *arg) {
    // Dapatkan head descriptor
    uint32_t head_addr = get_rx_head();
    if (!is_valid_addr(head_addr)) {
        ESP_LOGE(TAG, "Head descriptor tidak valid: 0x%08" PRIx32, head_addr);
        vTaskDelete(NULL);
    }
    rx_desc_t *head = (rx_desc_t*)head_addr;
    ESP_LOGI(TAG, "Head descriptor: 0x%08" PRIx32, head_addr);

    uint32_t last_addr = head_addr;
    rx_desc_t *last_desc = head;

    while (1) {
        uint32_t current = get_rx_last();
        if (current != last_addr && is_valid_addr(current)) {
            // Telusuri dari last->next hingga current
            rx_desc_t *desc = last_desc->next;
            int count = 0;
            while (desc != NULL && (uint32_t)desc != current && count < 32) {
                if (!is_valid_addr((uint32_t)desc)) break;

                uint32_t status = desc->status;
                if (!(status & 0x80000000)) {  // milik software
                    uint32_t len = status & 0xfff;
                    if (len > 0 && len < 1600 && is_valid_addr(desc->buffer)) {
                        uint8_t *frame = (uint8_t*)desc->buffer;

                        // Deteksi probe request
                        uint8_t fc = frame[0];
                        uint8_t type = (fc >> 2) & 0x03;
                        uint8_t subtype = (fc >> 4) & 0x0F;
                        if (type == 0 && subtype == 4) {
                            ESP_LOGI(TAG, "Probe request captured, len=%" PRIu32, len);
                        } else {
                            ESP_LOGI(TAG, "Frame captured, len=%" PRIu32, len);
                        }
                        ESP_LOG_BUFFER_HEX(TAG, frame, len < 64 ? len : 64);

                        // Kirim sinyal ke task blink
                        int dummy = 1;
                        xQueueSend(blink_queue, &dummy, 0);

                        // Kembalikan ownership ke hardware (bit 31 = 1)
                        desc->status = status | 0x80000000;
                    }
                }
                desc = desc->next;
                count++;
            }
            // Proses descriptor terakhir
            if (desc != NULL && (uint32_t)desc == current) {
                uint32_t status = desc->status;
                if (!(status & 0x80000000)) {
                    uint32_t len = status & 0xfff;
                    if (len > 0 && len < 1600 && is_valid_addr(desc->buffer)) {
                        uint8_t *frame = (uint8_t*)desc->buffer;
                        uint8_t fc = frame[0];
                        uint8_t type = (fc >> 2) & 0x03;
                        uint8_t subtype = (fc >> 4) & 0x0F;
                        if (type == 0 && subtype == 4) {
                            ESP_LOGI(TAG, "Probe request captured, len=%" PRIu32, len);
                        } else {
                            ESP_LOGI(TAG, "Frame captured, len=%" PRIu32, len);
                        }
                        ESP_LOG_BUFFER_HEX(TAG, frame, len < 64 ? len : 64);
                        int dummy = 1;
                        xQueueSend(blink_queue, &dummy, 0);
                        desc->status = status | 0x80000000;
                    }
                }
            }
            // Set reload bit agar hardware tahu descriptor tersedia
            WRITE_PERI_REG(RX_DESC_RELOAD, READ_PERI_REG(RX_DESC_RELOAD) | 1);

            last_addr = current;
            last_desc = (rx_desc_t*)current;
        }
        vTaskDelay(pdMS_TO_TICKS(5));  // polling setiap 5ms
    }
}

// Inisialisasi Wi-Fi dalam mode promiscuous
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

void app_main(void) {
    // Delay 10 detik setelah reset
    vTaskDelay(pdMS_TO_TICKS(10000));
    printf("=== RAW RX Sniffer starting ===\n");

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

    // Buat queue untuk blink
    blink_queue = xQueueCreate(5, sizeof(int));
    if (!blink_queue) {
        ESP_LOGE(TAG, "Gagal membuat queue");
        return;
    }

    // Buat task blink
    xTaskCreate(blink_task, "blink", 2048, NULL, 5, NULL);

    // Inisialisasi Wi-Fi
    wifi_promiscuous_init();

    // Buat task polling RX
    xTaskCreate(rx_poll_task, "rx_poll", 4096, NULL, 5, NULL);
}
