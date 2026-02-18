#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
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
#define SNIFF_DURATION_MS  10000      // 10 detik
#define LED_BLINK_INTERVAL 250        // kedip setiap 250ms
#define LED_DIM_VALUE      10         // nilai redup (0-255)

// Struktur descriptor RX (12 byte)
typedef struct {
    uint32_t status;   // bit 0-11 = panjang frame
    uint32_t buffer;   // alamat buffer data
    uint32_t next;     // pointer ke descriptor berikutnya
} rx_desc_t;

static const char *TAG = "RX_MONITOR";
static led_strip_handle_t led_strip;
static uint32_t last_desc = 0;

// State machine
typedef enum {
    SNIFF_IDLE,
    SNIFF_ACTIVE,
    SNIFF_COOLDOWN
} sniff_state_t;

static sniff_state_t current_state = SNIFF_IDLE;
static TimerHandle_t sniff_timer = NULL;
static bool button_pressed_flag = false;

// Pola deadbeef (little-endian)
static const uint8_t deadbeef_pattern[4] = {0xef, 0xbe, 0xad, 0xde};

// ==================== Fungsi Hardware ====================
static uint32_t get_last_dscr(void) {
    uint32_t low = READ_PERI_REG(RX_DESC_LAST_LOW);
    uint32_t high = READ_PERI_REG(RX_DESC_LAST_HIGH);
    high &= 0xfff00000;          // hanya 20 bit teratas
    return high | low;
}

// Validasi alamat buffer (kira-kira di rentang DRAM/PSRAM)
static bool is_valid_buffer_addr(uint32_t addr) {
    // Rentang umum untuk RAM pada ESP32-S3: 0x3FC00000 - 0x3FF00000 (DRAM) dan 0x3F800000 - 0x3FC00000 (PSRAM)
    // Sesuaikan dengan kebutuhan
    return (addr >= 0x3F800000 && addr < 0x40000000);
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
    return (gpio_get_level(BOOT_BUTTON_GPIO) == 0);
}

// ==================== Timer untuk mengakhiri sesi sniffing ====================
void sniff_timer_callback(TimerHandle_t xTimer) {
    current_state = SNIFF_IDLE;
    led_clear();
    ESP_LOGI(TAG, "Sniffing session ended (timeout)");
}

// ==================== Task untuk menangani button ====================
void button_task(void *pvParameter) {
    bool last_button_state = true;
    TickType_t last_debounce_time = 0;
    const TickType_t debounce_delay = pdMS_TO_TICKS(50);

    while (1) {
        bool button_state = is_button_pressed();

        if (button_state != last_button_state) {
            last_debounce_time = xTaskGetTickCount();
        }

        if ((xTaskGetTickCount() - last_debounce_time) > debounce_delay) {
            if (!button_state && current_state == SNIFF_IDLE) {
                // Tombol ditekan saat idle -> mulai sniffing
                current_state = SNIFF_ACTIVE;
                last_desc = get_last_dscr();  // reset descriptor
                ESP_LOGI(TAG, "Sniffing started! (will run for %d seconds)", SNIFF_DURATION_MS/1000);
                xTimerStart(sniff_timer, 0);
            }
        }

        last_button_state = button_state;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ==================== Aplikasi Utama ====================
void app_main(void) {
    led_init();
    button_init();
    wifi_init();

    sniff_timer = xTimerCreate("sniff_timer", pdMS_TO_TICKS(SNIFF_DURATION_MS), 
                                pdFALSE, NULL, sniff_timer_callback);
    xTaskCreate(button_task, "button_task", 2048, NULL, 1, NULL);

    TickType_t last_blink = 0;
    bool led_state = false;

    ESP_LOGI(TAG, "System ready. Press BOOT button to start 10-second sniffing session");

    while (1) {
        if (current_state == SNIFF_ACTIVE) {
            // LED blinking
            TickType_t now = xTaskGetTickCount();
            if ((now - last_blink) > pdMS_TO_TICKS(LED_BLINK_INTERVAL)) {
                led_state = !led_state;
                if (led_state) {
                    led_set_color(LED_DIM_VALUE, 0, 0);
                } else {
                    led_clear();
                }
                last_blink = now;
            }

            // Sniffing logic
            uint32_t new_desc = get_last_dscr();
            if (new_desc != last_desc && new_desc != 0) {
                rx_desc_t *desc = (rx_desc_t*)new_desc;
                uint32_t len = desc->status & 0xFFF;
                uint8_t *buf = (uint8_t*)desc->buffer;

                // Validasi dasar
                bool valid = (len >= 24 && len <= 1600) && 
                             (desc->buffer != 0) && 
                             is_valid_buffer_addr(desc->buffer);

                // Periksa deadbeef (4 byte pertama)
                if (valid && memcmp(buf, deadbeef_pattern, 4) == 0) {
                    valid = false;  // buffer masih deadbeef
                }

                if (valid) {
                    ESP_LOGI(TAG, "Frame received, len=%" PRIu32 ", buffer=0x%08" PRIx32, len, desc->buffer);
                    ESP_LOG_BUFFER_HEX("RAW", buf, len);
                } else {
                    ESP_LOGD(TAG, "Skipping invalid frame: len=%" PRIu32 ", buffer=0x%08" PRIx32, len, desc->buffer);
                }

                last_desc = new_desc;
            }
        } else {
            led_clear();  // pastikan LED mati saat idle
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}