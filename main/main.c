#include <stdio.h>
#include <string.h>
#include <inttypes.h>                     // untuk PRIu32, PRIx32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "soc/soc.h"
#include "driver/uart.h"
#include "esp_log.h"

// Alamat register MAC untuk ESP32-S3 (berdasarkan reverse engineering)
#define MAC_REG_BASE        0x60033000
#define RX_DESC_START       (MAC_REG_BASE + 0x88)   // Pointer ke descriptor pertama (head)
#define RX_CUR_DESC         (MAC_REG_BASE + 0x90)   // (opsional) descriptor yang sedang aktif

// Struktur descriptor RX (12 byte)
typedef struct {
    uint32_t word0;    // bit 0-11: panjang frame, bit 30: kepemilikan (1=hardware)
    uint32_t buf_addr; // Alamat buffer tempat frame disimpan
    uint32_t next;     // Pointer ke descriptor berikutnya (circular)
} rx_desc_t;

static const char *TAG = "SNIFF_ALL";

// MUX untuk critical section
static portMUX_TYPE my_mux = portMUX_INITIALIZER_UNLOCKED;

// Inisialisasi Wi-Fi dalam mode promiscuous
static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));  // Diperlukan untuk promiscuous

    // Aktifkan filter untuk menerima semua frame (data, manajemen, kontrol)
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));    // Mode monitor

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi promiscuous mode started");
}

// Task pembaca descriptor
void sniff_task(void *pvParameters)
{
    uint32_t head_desc = 0;
    uint32_t desc_count = 0;
    uint32_t first_head = 0;

    while (1) {
        portENTER_CRITICAL(&my_mux);   // Masuk critical section

        head_desc = READ_PERI_REG(RX_DESC_START);
        if (head_desc && head_desc != first_head) {
            // Hitung jumlah descriptor dengan mengikuti next hingga kembali ke head
            first_head = head_desc;
            desc_count = 0;
            rx_desc_t *d = (rx_desc_t*)head_desc;
            do {
                desc_count++;
                d = (rx_desc_t*)d->next;
            } while ((uint32_t)d != head_desc && desc_count < 64); // batas aman
            ESP_LOGI(TAG, "RX descriptor ring: head=0x%08" PRIx32 ", count=%" PRIu32, head_desc, desc_count);
        }

        if (head_desc && desc_count > 0) {
            rx_desc_t *desc = (rx_desc_t*)head_desc;
            for (int i = 0; i < desc_count; i++) {
                uint32_t word0 = desc->word0;

                // Bit 30 = 1 berarti frame masih milik hardware (belum diproses driver)
                if (word0 & (1 << 30)) {
                    uint32_t len = word0 & 0xFFF;  // 12 bit panjang
                    if (len > 0 && len < 2048) {   // Batas aman
                        uint8_t *buf = (uint8_t*)desc->buf_addr;

                        // Salin frame ke buffer lokal (masih dalam critical section)
                        uint8_t temp[len];
                        memcpy(temp, buf, len);

                        // Keluar critical sebelum mencetak agar tidak memblokir lama
                        portEXIT_CRITICAL(&my_mux);

                        // Cetak frame
                        ESP_LOGI(TAG, "Frame[%d] len=%" PRIu32, i, len);
                        ESP_LOG_BUFFER_HEX(TAG, temp, (len > 64) ? 64 : len);

                        // Masuk critical lagi untuk lanjut scan descriptor berikutnya
                        portENTER_CRITICAL(&my_mux);
                    }
                }
                desc = (rx_desc_t*)desc->next;
            }
        }

        portEXIT_CRITICAL(&my_mux);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    // Inisialisasi NVS (diperlukan Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Inisialisasi Wi-Fi
    wifi_init();

    // Buat task pembaca
    xTaskCreate(sniff_task, "sniff_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Sniffer started. Capturing all frames...");
}