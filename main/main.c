#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

static const char *TAG = "ZEUS_S3";

// Register S3 hasil bedah hal_mac_rx.o
#define S3_WIFI_RX_CONFIG_REG 0x60033084
#define S3_WIFI_RX_FILTER_REG 0x60033000

extern int ets_printf(const char *fmt, ...);

// Fungsi untuk membuka semua gerbang hardware (True Monitor Mode)
void apply_open_mac_hijack() {
    // Matikan hardware filter (MAC Address Filter) -> Terima SEMUA
    *(volatile uint32_t*)(S3_WIFI_RX_FILTER_REG) = 0;

    // Aktifkan bit promiscuous & paksa DMA tetap ON (bit 0 & 1)
    *(volatile uint32_t*)(S3_WIFI_RX_CONFIG_REG) |= 0x00000003; 

    ets_printf("\n>>> Z_S3: Hardware Filter Bypassed! <<<\n");
}

// Handler Paket
void sniffer_handler(void* buf, wifi_promiscuous_pkt_type_t type) {
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    uint8_t *payload = pkt->payload;
    uint32_t len = pkt->rx_ctrl.sig_len;

    // Cetak Raw Frame ke Serial (Format Mini-Dump)
    ets_printf("L:%d | R:%d | DATA:", len, pkt->rx_ctrl.rssi);
    for (int i = 0; i < (len > 16 ? 16 : len); i++) {
        ets_printf(" %02X", payload[i]);
    }
    ets_printf("\n");
}

void app_main(void) {
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_NULL); 
    esp_wifi_start();

    // Set channel secara manual (misal channel 1)
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    // Aktifkan sniffer standar
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&sniffer_handler);

    // Terapkan teknik Zeus (Hijack Register)
    vTaskDelay(pdMS_TO_TICKS(100));
    apply_open_mac_hijack();

    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
