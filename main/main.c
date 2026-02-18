#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "soc/soc.h"
#include "led_strip.h"

// Alamat register untuk akses langsung
#define RX_DESC_LAST_LOW   0x60033090
#define RX_DESC_LAST_HIGH  0x60033c64
#define RX_END_STATE       0x600330a8

// Pin untuk NeoPixel dan tombol boot
#define NEOPIXEL_PIN       48
#define BOOT_BUTTON_PIN    0

// Struktur descriptor RX (12 byte) – sesuai inisialisasi driver
typedef struct {
    uint32_t status;   // bit 0-11 = panjang frame
    uint32_t buffer;   // alamat buffer data
    uint32_t next;     // pointer ke descriptor berikutnya
} rx_desc_t;

static const char *TAG = "RX_MONITOR";
static uint32_t last_state = 0;
static led_strip_handle_t led_strip;

// --- Fungsi akses register ---
static uint32_t get_last_dscr(void) {
    uint32_t low  = READ_PERI_REG(RX_DESC_LAST_LOW);
    uint32_t high = READ_PERI_REG(RX_DESC_LAST_HIGH);
    high &= 0xfff00000;               // hanya 20 bit teratas
    return high | low;
}

static uint32_t get_rx_end_state(void) {
    return READ_PERI_REG(RX_END_STATE) & 0xFF;
}

// --- Inisialisasi NeoPixel ---
void led_init(void) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = NEOPIXEL_PIN,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);  // matikan LED
}

void led_set(uint8_t r, uint8_t g, uint8_t b) {
    led_strip_set_pixel(led_strip, 0, r, g, b);
    led_strip_refresh(led_strip);
}

// --- Inisialisasi Wi-Fi dalam mode promiscuous (channel 5) ---
void wifi_init(void) {
    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Set channel 5
    ESP_ERROR_CHECK(esp_wifi_set_channel(5, WIFI_SECOND_CHAN_NONE));

    // Aktifkan promiscuous mode
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    // Filter semua frame (data, manajemen, kontrol)
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL
    };
    esp_wifi_set_promiscuous_filter(&filter);
    wifi_promiscuous_filter_t ctrl_filter = {
        .filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL
    };
    esp_wifi_set_promiscuous_ctrl_filter(&ctrl_filter);

    ESP_LOGI(TAG, "WiFi promiscuous mode aktif di channel 5");
}

// --- Task untuk tombol boot ---
void button_task(void *arg) {
    gpio_set_direction(BOOT_BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_BUTTON_PIN, GPIO_PULLUP_ONLY);

    while (1) {
        if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));       // debounce
            if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
                ESP_LOGI(TAG, "Tombol ditekan → set channel 5");
                esp_wifi_set_channel(5, WIFI_SECOND_CHAN_NONE);

                led_set(0, 10, 0);    // hijau redup
                vTaskDelay(pdMS_TO_TICKS(100));
                led_set(0, 0, 0);     // mati

                // tunggu tombol dilepas
                while (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void) {
    // Inisialisasi LED (merah redup sebagai tanda mulai)
    led_init();
    led_set(10, 0, 0);

    // Inisialisasi Wi-Fi
    wifi_init();

    // Baca state awal RX
    last_state = get_rx_end_state();
    ESP_LOGI(TAG, "RX end state awal: %" PRIu32, last_state);

    // Buat task tombol
    xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);

    // Loop utama polling frame
    while (1) {
        uint32_t current_state = get_rx_end_state();
        if (current_state != last_state) {
            // Ada frame baru
            uint32_t desc_addr = get_last_dscr();
            rx_desc_t *desc = (rx_desc_t*)desc_addr;
            uint32_t len = desc->status & 0xFFF;   // panjang frame (12 bit)

            if (len > 0 && len < 2048) {           // batas aman
                ESP_LOGI(TAG, "Frame diterima, panjang: %" PRIu32 ", buffer: 0x%08" PRIx32, len, desc->buffer);
                ESP_LOG_BUFFER_HEX("RAW", (void*)(desc->buffer), len);

                // Kedip biru redup
                led_set(0, 0, 10);
                vTaskDelay(pdMS_TO_TICKS(5));
                led_set(0, 0, 0);
            } else {
                ESP_LOGW(TAG, "Panjang frame tidak valid: %" PRIu32, len);
            }
            last_state = current_state;
        }
        vTaskDelay(pdMS_TO_TICKS(1));   // polling cepat (1 ms)
    }
}