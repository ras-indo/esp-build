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
#define RX_DESC_LAST_LOW   0x60033090   // register low dari alamat descriptor terakhir
#define RX_DESC_LAST_HIGH  0x60033c64   // register high (digabung dengan low)
#define CHANNEL            5             // channel Wi-Fi yang dipantau
#define BOOT_BUTTON_GPIO   0             // tombol boot (biasanya GPIO0)
#define NEOPIXEL_GPIO      48            // GPIO untuk NeoPixel (banyak board ESP32-S3 menggunakan 48)
#define LED_BLINK_MS       100           // durasi LED menyala (ms)
#define LED_DIM_VALUE      5             // nilai kecerahan redup (0-255)

// Struktur descriptor RX (12 byte) sesuai inisialisasi driver
typedef struct {
    uint32_t status;   // bit 0-11 = panjang frame, bit lainnya = flag
    uint32_t buffer;   // alamat buffer tempat frame disalin
    uint32_t next;     // pointer ke descriptor berikutnya
} rx_desc_t;

static const char *TAG = "RX_MONITOR";
static led_strip_handle_t led_strip;
static uint32_t last_desc = 0;

// ==================== Fungsi Hardware ====================
// Membaca alamat descriptor terakhir dari register hardware
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

    // Filter semua jenis frame (data, manajemen, kontrol)
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
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);  // matikan LED
}

// Menyalakan LED merah redup selama LED_BLINK_MS
void led_blink_dim(void) {
    led_strip_set_pixel(led_strip, 0, LED_DIM_VALUE, 0, 0); // merah
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

    // Pola deadbeef dalam little-endian (ef be ad de)
    uint32_t deadbeef = 0xdeadbeef;
    uint8_t *deadbeef_bytes = (uint8_t*)&deadbeef;

    while (1) {
        // Tombol boot: reset last_desc ke nilai terbaru (mulai ulang deteksi)
        if (is_button_pressed()) {
            ESP_LOGI(TAG, "Tombol ditekan, reset descriptor");
            last_desc = get_last_dscr();
            vTaskDelay(pdMS_TO_TICKS(200)); // debounce
        }

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

            // Validasi panjang frame dan pastikan buffer bukan deadbeef
            if (len >= 24 && len <= 1600 && !is_deadbeef) {
                ESP_LOGI(TAG, "Frame received, len=%" PRIu32 ", buffer=0x%08" PRIx32, len, desc->buffer);
                ESP_LOG_BUFFER_HEX("RAW", buf, len);
                led_blink_dim();  // indikasi visual
            } else if (is_deadbeef) {
                // Abaikan buffer yang masih berisi deadbeef (belum terisi)
                ESP_LOGD(TAG, "Skipping deadbeef buffer at desc 0x%08" PRIx32, new_desc);
            } else {
                ESP_LOGW(TAG, "Invalid frame length: %" PRIu32, len);
            }
            last_desc = new_desc;
        }
        vTaskDelay(pdMS_TO_TICKS(5));  // polling cepat (5 ms)
    }
}