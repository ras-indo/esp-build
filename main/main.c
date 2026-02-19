#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Deklarasi fungsi-fungsi low-level dari binary blob */
extern void hal_init(void);
extern void mac_txrx_init(void);
extern void mac_rxbuf_init(void);
extern void hal_mac_rx_enable(void);
extern void wDev_SetCurChannel(uint8_t *ch);
extern void hal_mac_rx_get_end_info(uint32_t*, uint32_t*, uint32_t*, uint32_t*, uint32_t*, uint32_t*);

/* Alamat register Wi-Fi (berdasarkan hasil reverse engineering) */
#define REG_RX_BASE       (*(volatile uint32_t*)0x60033088)
#define REG_RX_NEXT       (*(volatile uint32_t*)0x6003308c)
#define REG_RX_LAST       (*(volatile uint32_t*)0x60033090)
#define REG_RX_CTRL       (*(volatile uint32_t*)0x60033084)
#define REG_RX_EVENT      (*(volatile uint32_t*)0x60033c3c)
#define REG_RX_EVENT_CLR  (*(volatile uint32_t*)0x60033c40)

/* Memory barrier untuk akses register */
static inline void memw(void) {
    asm volatile ("memw" ::: "memory");
}

void app_main(void) {
    printf("ESP32-S3 Raw Frame Capture (Channel 5)\n");

    /* Inisialisasi hardware Wi-Fi */
    printf("Inisialisasi HAL...\n");
    hal_init();
    printf("Inisialisasi MAC TXRX...\n");
    mac_txrx_init();
    printf("Inisialisasi RX buffer...\n");
    mac_rxbuf_init();
    printf("Aktifkan RX...\n");
    hal_mac_rx_enable();

    /* Set channel 5 */
    uint8_t channel[2] = {5, 0};  // channel, bandwidth (0 = 20 MHz)
    wDev_SetCurChannel(channel);
    printf("Channel 5 aktif\n");

    /* Bersihkan interupsi yang mungkin tertunda */
    memw();
    REG_RX_EVENT_CLR = 0xFFFFFFFF;
    memw();

    /* Loop utama: polling RX */
    while (1) {
        memw();
        uint32_t rx_next = REG_RX_NEXT;
        uint32_t rx_last = REG_RX_LAST;
        memw();

        if (rx_next != rx_last) {
            /* Ada paket baru */
            uint32_t desc_addr = rx_next;                // alamat descriptor
            uint32_t desc0 = *(uint32_t*)desc_addr;      // word 0: alamat buffer + flag
            uint32_t desc1 = *(uint32_t*)(desc_addr+4);  // word 1: (tidak digunakan)
            uint32_t desc2 = *(uint32_t*)(desc_addr+8);  // word 2: pointer ke descriptor berikutnya

            /* Baca informasi paket dari register khusus */
            uint32_t info[6];
            hal_mac_rx_get_end_info(&info[0], &info[1], &info[2], &info[3], &info[4], &info[5]);

            /* Alamat buffer (20 bit MSB dari desc0) */
            uint8_t *buffer = (uint8_t*)(desc0 & 0xFFFFF000);
            /* Panjang paket (asumsi 12 bit LSB dari info[0]) */
            uint16_t len = info[0] & 0xFFF;
            if (len == 0) len = info[1] & 0xFFF;  // cadangan jika info[0] bukan panjang

            if (len > 0 && len < 2048) {
                printf("\n=== RAW FRAME (len=%d) ===\n", len);
                for (int i = 0; i < len; i++) {
                    printf("%02x ", buffer[i]);
                    if ((i+1) % 16 == 0) printf("\n");
                }
                printf("\n");
            }

            /* Tandai descriptor telah diproses (reload) */
            memw();
            REG_RX_CTRL |= 1;  // set bit 0
            memw();

            /* Bersihkan event interupsi (jika ada) */
            memw();
            uint32_t evt = REG_RX_EVENT;
            if (evt) {
                REG_RX_EVENT_CLR = evt;
                memw();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));  // polling interval
    }
}
