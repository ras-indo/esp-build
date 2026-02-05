#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

// Register ESP32-S3 (Open MAC Zeus Logic)
#define S3_WIFI_RX_CONFIG_REG 0x60033084
#define S3_WIFI_RX_FILTER_REG 0x60033000

// Fungsi ROM untuk output cepat
extern int ets_printf(const char *fmt, ...);

// Fungsi untuk bypass filter hardware
void apply_open_mac_hijack() {
    // Matikan hardware filter (Terima semua alamat MAC)
    *(volatile uint32_t*)(S3_WIFI_RX_FILTER_REG) = 0;

    // Paksa mesin RX DMA untuk menangkap paket Management & Control
    *(volatile uint32_t*)(S3_WIFI_RX_CONFIG_REG) |= 0x00000003; 

    ets_printf("\n[SYSTEM] Zeus Open MAC Hijack Applied to S3\n");
}

// Callback untuk menangkap frame mentah
void sniffer_handler(void* buf, wifi_promiscuous_pkt_type_t type) {
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    uint8_t *payload = pkt->payload;
    uint32_t len = pkt->rx_ctrl.sig_len;

    // Cetak Raw Frame Header (24 byte pertama)
    ets_printf("LEN:%d | RSSI:%d | DATA:", len, pkt->rx_ctrl.rssi);
    for (int i = 0; i < (len > 24 ? 24 : len); i++) {
        ets_printf(" %02X", payload[i]);
    }
    ets_printf("\n");
}

void app_main(void) {
    // Inisialisasi dasar
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    // Inisialisasi WiFi (Menyalakan clock radio)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_NULL); 
    esp_wifi_start();

    // Set ke Channel 1 (Bisa diubah sesuai target)
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    // Aktifkan mode promiscuous bawaan
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&sniffer_handler);

    // Terapkan Hijack Register untuk membuka filter yang dikunci blob
    vTaskDelay(pdMS_TO_TICKS(200));
    apply_open_mac_hijack();

    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
