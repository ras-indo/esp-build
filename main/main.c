#include <stdio.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

// 1. Deklarasi fungsi ROM (ets_printf lebih aman digunakan dalam low-level hook)
extern int ets_printf(const char *fmt, ...);

// 2. Definisi Register Hardware ESP32-S3 (Hasil Reverse Engineering hal_mac_rx.o)
// Alamat dasar WiFi S3 berada di 0x60030000
#define S3_WIFI_RX_CONFIG_REG 0x60033084
#define S3_WIFI_RX_FILTER_REG 0x60033000

// 3. Deklarasi fungsi asli dari libpp.a yang akan kita "bungkus"
// Fungsi ini dipanggil tepat setelah hardware mengisi DMA descriptor dengan data paket
extern void __real_wdevProcessRxSucDataAll(void *arg);

// 4. Implementasi Wrapper (Hook)
void __wrap_wdevProcessRxSucDataAll(void *arg) {
    // Argumen 'arg' biasanya menunjuk ke RX Descriptor
    uint32_t *desc = (uint32_t *)arg;
    
    // Cetak 4 word pertama dari descriptor untuk menganalisis strukturnya
    // Di S3, kita mencari tahu di mana pointer buffer dan panjang paket disimpan
    ets_printf("RX_DESC [%p]: 0x%08X | 0x%08X | 0x%08X | 0x%08X\n", 
               arg, desc[0], desc[1], desc[2], desc[3]);

    // Lanjutkan alur kerja asli agar stack WiFi tidak macet
    __real_wdevProcessRxSucDataAll(arg);
}

// 5. Logika Memaksa Hardware Membuka Filter (The Zeus Way)
void hijack_hardware() {
    ESP_LOGI("RE_S3", "Menerapkan konfigurasi Open MAC pada register...");
    
    // Matikan semua hardware filter agar paket dengan MAC target manapun diterima
    *(volatile uint32_t*)(S3_WIFI_RX_FILTER_REG) = 0x00000000;
    
    // Aktifkan bit promiscuous dan pastikan mesin RX DMA terus berjalan
    // Berdasarkan disassembly wdev.o, bit terendah seringkali mengontrol state RX
    *(volatile uint32_t*)(S3_WIFI_RX_CONFIG_REG) |= 0x00000001;
    
    ESP_LOGI("RE_S3", "Filter hardware telah dinonaktifkan.");
}

void app_main(void) {
    // Inisialisasi NVS (diperlukan untuk WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Inisialisasi Stack Network dasar
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Inisialisasi WiFi dengan konfigurasi default agar clock hardware menyala
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    
    // Set mode NULL atau Station agar driver menginisialisasi hardware radio
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI("RE_S3", "WiFi Started. Menunggu stabilitas hardware...");
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Eksekusi pemaksaan register
    hijack_hardware();

    ESP_LOGI("RE_S3", "Memulai pemantauan descriptor... Dekatkan perangkat WiFi lain!");
    
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        // Opsional: cetak status register secara berkala untuk melihat perubahan
        // uint32_t current_conf = *(volatile uint32_t*)(S3_WIFI_RX_CONFIG_REG);
        // ESP_LOGI("RE_S3", "RX_CONF: 0x%08X", current_conf);
    }
}
