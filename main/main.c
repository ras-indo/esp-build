#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "soc/soc.h"

// ==================== Konfigurasi ====================
#define RX_DESC_LAST_LOW   0x60033090
#define RX_DESC_LAST_HIGH  0x60033c64
#define CHANNEL            5
#define NEOPIXEL_GPIO      48
#define START_DELAY_MS     12000     // 12 detik sebelum mulai sniffing
#define LED_BLINK_INTERVAL 250       // kedip setiap 250ms
#define LED_DIM_VALUE      10        // nilai redup (0-255)

// Struktur descriptor RX (12 byte)
typedef struct {
    uint32_t status;   // bit 0-11 = panjang frame
    uint32_t buffer;   // alamat buffer data
    uint32_t next;     // pointer ke descriptor berikutnya
} rx_desc_t;

static const char *TAG = "RX_MONITOR";
static led_strip_handle_t led_strip;
static uint32_t last_desc = 0;

// ==================== Fungsi Hardware ====================
static uint32_t get_last_dscr(void) {
    uint32_t low = READ_PERI_REG(RX_DESC_LAST_LOW);
    uint32_t high = READ_PERI_REG(RX_DESC_LAST_HIGH);
    high &= 0xfff00000;          // hanya 20 bit teratas (sesuai assembly)
    return high | low;
}

// ==================== Inisialisasi Wi-Fi ====================
void wifi_init(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL
    };
    esp_wifi_set_promiscuous_filter(&filter);

    wifi_promiscuous_filter_t ctrl_filter = {
        .filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL
    };
    esp_wifi_set_promiscuous_ctrl_filter(&ctrl_filter);

    ESP_LOGI(TAG, "WiFi promiscuous mode aktif pada channel %d", CHANNEL);
}

// ==================== Inisialisasi LED NeoPixel ====================
void led_init(void) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = NEOPIXEL_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);
}

void led_set_color(uint8_t r, uint8_t g, uint8_t b) {
    led_strip_set_pixel(led_strip, 0, r, g, b);
    led_strip_refresh(led_strip);
}

void led_clear(void) {
    led_strip_clear(led_strip);
    led_strip_refresh(led_strip);
}

// ==================== Aplikasi Utama ====================
void app_main(void) {
    led_init();
    wifi_init();

    // Delay 12 detik sebelum memulai sniffing
    ESP_LOGI(TAG, "Menunggu %d detik sebelum memulai sniffing...", START_DELAY_MS/1000);
    vTaskDelay(pdMS_TO_TICKS(START_DELAY_MS));

    last_desc = get_last_dscr();
    ESP_LOGI(TAG, "Sniffing dimulai! (hanya beacon)");

    // Pola deadbeef dalam little-endian (ef be ad de)
    uint32_t deadbeef = 0xdeadbeef;
    uint8_t *deadbeef_bytes = (uint8_t*)&deadbeef;

    // Variabel untuk LED blinking
    TickType_t last_blink = 0;
    bool led_state = false;

    while (1) {
        // LED blinking setiap 250ms
        TickType_t now = xTaskGetTickCount();
        if ((now - last_blink) > pdMS_TO_TICKS(LED_BLINK_INTERVAL)) {
            led_state = !led_state;
            if (led_state) {
                led_set_color(LED_DIM_VALUE, 0, 0);  // merah redup
            } else {
                led_clear();
            }
            last_blink = now;
        }

        // Sniffing logic
        uint32_t new_desc = get_last_dscr();
        if (new_desc != last_desc) {
            rx_desc_t *desc = (rx_desc_t*)new_desc;
            uint32_t len = desc->status & 0xFFF;   // panjang frame (12 bit)
            uint8_t *buf = (uint8_t*)desc->buffer;

            // Periksa apakah buffer masih berisi deadbeef (4 byte pertama)
            bool is_deadbeef = (buf[0] == deadbeef_bytes[0] &&
                                buf[1] == deadbeef_bytes[1] &&
                                buf[2] == deadbeef_bytes[2] &&
                                buf[3] == deadbeef_bytes[3]);

            // Validasi panjang frame dan filter beacon (frame control byte pertama = 0x80)
            if (len >= 24 && len <= 1600 && !is_deadbeef && buf[0] == 0x80) {
                ESP_LOGI(TAG, "Beacon received, len=%" PRIu32 ", buffer=0x%08" PRIx32, len, desc->buffer);
                ESP_LOG_BUFFER_HEX("BEACON", buf, len);
            }
            last_desc = new_desc;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}