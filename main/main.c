#include <stdio.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"

// Deklarasi fungsi ROM
extern int ets_printf(const char *fmt, ...);

// Fungsi asli di libpp.a
extern void __real_wdevProcessRxSucDataAll(void *arg);

void __wrap_wdevProcessRxSucDataAll(void *arg) {
    uint32_t *desc = (uint32_t *)arg;
    
    // Intip 3 word pertama dari descriptor
    ets_printf("RX_DESC: 0x%08X | 0x%08X | 0x%08X\n", desc[0], desc[1], desc[2]);

    // Jalankan fungsi asli
    __real_wdevProcessRxSucDataAll(arg);
}

// Fungsi untuk membuka blokir hardware (The Zeus Way)
void hijack_hardware() {
    ESP_LOGI("RE", "Memulai Injeksi Register Hardware...");
    
    // 1. Matikan filter hardware agar semua paket lewat
    *(volatile uint32_t*)(S3_WIFI_RX_FILTER_REG) = 0;
    
    // 2. Paksa bit promiscuous pada RX_CONFIG
    *(volatile uint32_t*)(S3_WIFI_RX_CONFIG_REG) |= 0x1;
    
    ESP_LOGI("RE", "Hardware telah dipaksa terbuka.");
}

void app_main(void) {
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_NULL); // Mode kosong agar kita bisa kontrol manual
    esp_wifi_start();

    // Tunggu sebentar agar hardware stabil
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Eksekusi Hijack
    hijack_hardware();

    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
