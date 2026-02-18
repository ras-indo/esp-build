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
#define BOOT_BUTTON_GPIO   0
#define NEOPIXEL_GPIO      48
#define LED_BLINK_MS       100
#define LED_DIM_VALUE      5       // nilai redup (0-255)

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
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);  // matikan semua LED
}

void led_blink_dim(void) {
    // Nyalakan merah redup
    led_strip_set_pixel(led_strip, 0, LED_DIM_VALUE, 0, 0);
    led_strip_refresh(led_strip);
    vTaskDelay(pdMS_TO_TICKS(LED_BLINK_MS));
    led_strip_clear(led_strip);
    led_strip_refresh(led_strip);
}

// ==================== Tombol Boot ====================
void button_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

bool is_button_pressed(void) {
    return (gpio_get_level(BOOT_BUTTON_GPIO) == 0); // aktif low
}

// ==================== Aplikasi Utama ====================
void app_main(void) {
    led_init();
    button_init();
    wifi_init();

    // Baca alamat descriptor awal
    last_desc = get_last_dscr();
    ESP_LOGI(TAG, "Initial last descriptor: 0x%08" PRIx32, last_desc);

    while (1) {
        // Jika tombol ditekan, reset last_desc untuk memulai ulang deteksi
        if (is_button_pressed()) {
            ESP_LOGI(TAG, "Tombol ditekan, reset descriptor");
            last_desc = get_last_dscr();  // set ke nilai terkini
            vTaskDelay(pdMS_TO_TICKS(200)); // debounce
        }

        uint32_t new_desc = get_last_dscr();
        if (new_desc != last_desc) {
            rx_desc_t *desc = (rx_desc_t*)new_desc;
            uint32_t len = desc->status & 0xFFF;

            // Validasi panjang frame (minimal header 24 byte, maksimal 1600)
            if (len >= 24 && len <= 1600) {
                ESP_LOGI(TAG, "Frame received, len=%" PRIu32 ", buffer=0x%08" PRIx32, len, desc->buffer);
                ESP_LOG_BUFFER_HEX("RAW", (void*)(desc->buffer), len);
                led_blink_dim();  // indikasi visual
            } else {
                ESP_LOGW(TAG, "Invalid frame length: %" PRIu32, len);
            }
            last_desc = new_desc;
        }
        vTaskDelay(pdMS_TO_TICKS(5));  // polling cepat
    }
}