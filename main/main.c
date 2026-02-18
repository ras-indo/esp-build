#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "soc/soc.h"
#include "esp_task_wdt.h"          // untuk watchdog reset

// Alamat register untuk mendapatkan descriptor terakhir
#define RX_DESC_LAST_LOW   0x60033090
#define RX_DESC_LAST_HIGH  0x60033c64

// Struktur descriptor RX (12 byte)
typedef struct {
    uint32_t status;   // bit 0-11 = panjang frame, bit 31 = kepemilikan (1=software, 0=hardware)
    uint32_t buffer;   // alamat buffer data
    uint32_t next;     // pointer ke descriptor berikutnya
} rx_desc_t;

static const char *TAG = "RX_MONITOR";

// Membaca alamat descriptor terakhir dari hardware
static uint32_t get_last_dscr(void) {
    uint32_t low = READ_PERI_REG(RX_DESC_LAST_LOW);
    uint32_t high = READ_PERI_REG(RX_DESC_LAST_HIGH);
    high &= 0xfff00000;          // hanya 20 bit teratas (sesuai assembly)
    return high | low;
}

// Inisialisasi Wi-Fi dalam mode promiscuous
void wifi_init(void) {
    // Inisialisasi NVS
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

    // Atur channel (misal channel 1)
    ESP_ERROR_CHECK(esp_wifi_set_channel(5, WIFI_SECOND_CHAN_NONE));

    // Aktifkan mode promiscuous
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    // Set filter agar menerima semua jenis frame
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL
    };
    esp_wifi_set_promiscuous_filter(&filter);

    wifi_promiscuous_filter_t ctrl_filter = {
        .filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL
    };
    esp_wifi_set_promiscuous_ctrl_filter(&ctrl_filter);

    ESP_LOGI(TAG, "WiFi initialized in promiscuous mode on channel 1");
}

void app_main(void) {
    // Inisialisasi Wi-Fi
    wifi_init();

    // Baca alamat descriptor awal
    uint32_t last_desc = get_last_dscr();
    ESP_LOGI(TAG, "Initial last descriptor address: 0x%08" PRIx32, last_desc);

    // Loop polling dengan watchdog reset
    while (1) {
        uint32_t new_desc = get_last_dscr();
        rx_desc_t *desc = (rx_desc_t*)new_desc;

        // Periksa apakah hardware telah menulis frame (bit 31 = 0)
        if (!(desc->status & 0x80000000)) {
            uint32_t len = desc->status & 0xFFF;
            if (len > 0 && len < 2048) {
                ESP_LOGI(TAG, "Frame received, length: %" PRIu32, len);
                ESP_LOG_BUFFER_HEX("RAW", (void*)(desc->buffer), len);
            } else {
                ESP_LOGW(TAG, "Invalid frame length: %" PRIu32, len);
            }
            last_desc = new_desc;
        }

        // Reset watchdog agar tidak timeout
        esp_task_wdt_reset();

        // Beri kesempatan task lain berjalan (delay 1 ms)
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}