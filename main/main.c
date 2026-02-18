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

// State machine untuk sniffer
typedef enum {
    SNIFF_IDLE,
    SNIFF_ACTIVE,
} sniff_state_t;

static sniff_state_t current_state = SNIFF_IDLE;
static TimerHandle_t sniff_timer = NULL;

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

// ==================== Tombol Boot ====================
void button_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
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

// ==================== Task untuk menangani button dengan falling edge detection ====================
void button_task(void *pvParameter) {
    bool last_button_state = true;  // pull-up, true = tidak ditekan
    TickType_t last_debounce_time = 0;
    const TickType_t debounce_delay = pdMS_TO_TICKS(50);

    ESP_LOGI(TAG, "Button task started, monitoring GPIO%d", BOOT_BUTTON_GPIO);

    while (1) {
        bool button_state = is_button_pressed(); // 0 jika ditekan

        if (button_state != last_button_state) {
            last_debounce_time = xTaskGetTickCount();
        }

        if ((xTaskGetTickCount() - last_debounce_time) > debounce_delay) {
            // State stabil
            if (button_state == 0 && last_button_state == 1) {
                // Falling edge: tombol baru ditekan
                ESP_LOGI(TAG, "Button pressed detected!");
                if (current_state == SNIFF_IDLE) {
                    current_state = SNIFF_ACTIVE;
                    last_desc = get_last_dscr();  // reset descriptor
                    ESP_LOGI(TAG, "Sniffing started! (will run for %d seconds)", SNIFF_DURATION_MS/1000);
                    xTimerStart(sniff_timer, 0);
                } else {
                    ESP_LOGI(TAG, "Sniffing already active, ignoring button");
                }
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

    // Buat timer untuk mengakhiri sesi sniffing
    sniff_timer = xTimerCreate("sniff_timer", pdMS_TO_TICKS(SNIFF_DURATION_MS), 
                                pdFALSE, NULL, sniff_timer_callback);
    if (sniff_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create timer");
        return;
    }

    // Buat task untuk menangani button
    BaseType_t res = xTaskCreate(button_task, "button_task", 4096, NULL, 1, NULL);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button task");
        return;
    }

    // Pola deadbeef dalam little-endian (ef be ad de)
    uint32_t deadbeef = 0xdeadbeef;
    uint8_t *deadbeef_bytes = (uint8_t*)&deadbeef;

    // Variabel untuk LED blinking
    TickType_t last_blink = 0;
    bool led_state = false;

    ESP_LOGI(TAG, "System ready. Press BOOT button to start 10-second beacon sniffing");

    while (1) {
        if (current_state == SNIFF_ACTIVE) {
            // LED blinking selama sesi aktif
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
        } else {
            // LED mati saat idle
            led_clear();
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}