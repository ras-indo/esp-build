#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

static const char *TAG = "OPEN_MAC_S3";

// Register khusus ESP32-S3 hasil reverse engineering hal_mac_rx.o
#define S3_WIFI_RX_CONFIG_REG 0x60033084
#define S3_WIFI_RX_FILTER_REG 0x60033000

// Fungsi ROM untuk cetak cepat tanpa lock
extern int ets_printf(const char *fmt, ...);

/**
 * Fungsi untuk memaksa hardware membuka semua filter.
 * Ini adalah inti dari metode "Open MAC" Zeus.
 */
void apply_zeus_open_mac() {
    // 1. Matikan semua hardware filter (MAC Address Filter)
    // Nilai 0 berarti terima semua paket (Promiscuous murni)
    *(volatile uint32_t*)(S3_WIFI_RX_FILTER_REG) = 0;

    // 2. Modifikasi RX Configuration
    // Bit 0 biasanya mengaktifkan mesin RX DMA secara paksa
    *(volatile uint32_t*)(S3_WIFI_RX_CONFIG_REG) |= 0x00000001;
    
    // 3. Tambahan: Mematikan Filter FCS (agar paket rusak/corrupted tetap terlihat)
    // Di S3, bit tertentu di register ini sering digunakan untuk skip_crc_error
    *(volatile uint32_t*)(S3_WIFI_RX_CONFIG_REG) |= (1 << 1); 

    ets_printf(">>> Hardware Hijacked: Filter Disabled, Open MAC Active <<<\n");
}

/**
 * Callback Promiscuous Standar.
 * Karena kita sudah hijack registernya, callback ini sekarang akan menerima
 * jauh lebih banyak paket daripada monitor mode bawaan ESP-IDF.
 */
void wifi_sniffer_packet_handler(void* buf, wifi_promiscuous_pkt_type_t type) {
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    uint8_t *payload = pkt->payload;
    uint32_t len = pkt->rx_ctrl.sig_len;

    // Cetak Header Paket secara Raw (Hex Dump)
    ets_printf("LEN: %d | RSSI: %d | RAW: ", len, pkt->rx_ctrl.rssi);
    
    // Cetak 24 byte pertama (MAC Header 802.11)
    for (int i = 0; i < (len > 24 ? 24 : len); i++) {
        ets_printf("%02x ", payload[i]);
    }
    
    // Tanda paket sukses/error
    if (pkt->rx_ctrl.rx_state == 0) ets_printf("[OK]\n");
    else ets_printf("[ERR:%d]\n", pkt->rx_ctrl.rx_state);
}

void app_main(void) {
    // 1. Inisialisasi Flash & Network
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 2. Inisialisasi WiFi (Wajib agar clock hardware menyala)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 3. Pindah ke Channel yang ingin di-monitor (Misal Channel 1)
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    // 4. Aktifkan Sniffer Bawaan dulu
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler));

    // 5. HIJACK! Timpa konfigurasi driver asli dengan Open MAC Zeus
    vTaskDelay(pdMS_TO_TICKS(500));
    apply_zeus_open_mac();

    ESP_LOGI(TAG, "Monitor Mode aktif dengan Register Hijacking S3.");
    
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
