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
#include "driver/gpio.h"

// ==================== Konfigurasi ====================
#define WIFI_CHANNEL            5
#define NEOPIXEL_PIN            8       // Ganti sesuai pin NeoPixel Anda
#define NEO_DIM_COLOR           10      // Intensitas redup (0-255)

// Register RX DMA (berdasarkan analisis)
#define RX_DESC_HEAD_LOW        0x60033088
#define RX_DESC_LAST_LOW        0x60033090
#define RX_DESC_LAST_HIGH       0x60033c64
#define RX_DESC_RELOAD          0x60033084
#define RX_DESC_STATE           0x600330a8

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

// ==================== Wi-Fi dan RX Polling ====================
static QueueHandle_t blink_queue;
static const char *TAG = "RAW_RX";

// Fungsi untuk mendapatkan alamat descriptor terakhir dari hardware
static uint32_t get_rx_last_dscr(void) {
    uint32_t low = READ_PERI_REG(RX_DESC_LAST_LOW);
    uint32_t high = READ_PERI_REG(RX_DESC_LAST_HIGH);
    high &= 0xfff00000;  // hanya 12 bit teratas
    return high | low;
}

// Fungsi untuk mendapatkan alamat head descriptor
static uint32_t get_rx_head_dscr(void) {
    uint32_t low = READ_PERI_REG(RX_DESC_HEAD_LOW);
    uint32_t high = READ_PERI_REG(RX_DESC_LAST_HIGH) & 0xfff00000;
    return high | low;
}

// Task untuk memproses frame dari descriptor
void rx_poll_task(void *arg) {
    // Dapatkan head descriptor
    uint32_t head_addr = get_rx_head_dscr();
    if (head_addr == 0) {
        ESP_LOGE(TAG, "Gagal mendapatkan head descriptor");
        vTaskDelete(NULL);
    }
    rx_desc_t *head = (rx_desc_t*)head_addr;

    // Inisialisasi last descriptor dengan head
    uint32_t last_dscr = head_addr;
    rx_desc_t *last_desc = head;

    while (1) {
        uint32_t current = get_rx_last_dscr();
        if (current != last_dscr) {
            // Ada frame baru, telusuri dari last->next hingga current
            rx_desc_t *desc = last_desc->next;
            int count = 0;
            while ((uint32_t)desc != current && count < 100) {  // batasi agar tidak infinite
                // Proses descriptor
                uint32_t status = desc->status;
                if (!(status & 0x80000000)) {  // bit 31 = 0 berarti frame siap
                    uint32_t len = status & 0xfff;
                    if (len > 0 && len < 1600) {
                        uint8_t *frame = (uint8_t*)desc->buffer;

                        // Cek apakah probe request
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

                        // Set ownership kembali ke hardware (bit 31 = 1)
                        desc->status = status | 0x80000000;
                    }
                }
                desc = desc->next;
                count++;
            }
            // Proses descriptor terakhir
            uint32_t status = desc->status;
            if (!(status & 0x80000000)) {
                uint32_t len = status & 0xfff;
                if (len > 0 && len < 1600) {
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
            // Set reload bit agar hardware tahu descriptor tersedia
            WRITE_PERI_REG(RX_DESC_RELOAD, READ_PERI_REG(RX_DESC_RELOAD) | 1);

            last_dscr = current;
            last_desc = desc;
        }
        vTaskDelay(pdMS_TO_TICKS(10));  // polling setiap 10ms
    }
}

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