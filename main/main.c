#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"

#define PROBE_A  36
#define PROBE_B  35
#define PROBE_C  34
#define PIN_R     2
#define PIN_G     3
#define PIN_B     4
#define PROBE_CLK_KHZ  400

static const char *TAG = "EMMC_PROBE";

static void rgb_init(void) {
    gpio_config_t io = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL<<PIN_R) | (1ULL<<PIN_G) | (1ULL<<PIN_B),
    };
    gpio_config(&io);
}

static void rgb_set(int r, int g, int b) {
    gpio_set_level(PIN_R, r); gpio_set_level(PIN_G, g); gpio_set_level(PIN_B, b);
}

static esp_err_t probe_once(int clk_pin, int cmd_pin, int d0_pin, sdmmc_card_t *out_card) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = PROBE_CLK_KHZ;
    host.flags = SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = clk_pin;
    slot.cmd = cmd_pin;
    slot.d0  = d0_pin;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    if (sdmmc_host_init() != ESP_OK) return ESP_FAIL;
    if (sdmmc_host_init_slot(host.slot, &slot) != ESP_OK) {
        sdmmc_host_deinit();
        return ESP_FAIL;
    }

    // Menggunakan pointer card langsung tanpa double pointer yang membingungkan
    esp_err_t ret = sdmmc_card_init(&host, out_card);
    
    if (ret != ESP_OK) {
        sdmmc_host_deinit();
    }
    return ret;
}

void app_main(void) {
    rgb_init();
    rgb_set(0,0,0);
    
    const int probe[3] = { PROBE_A, PROBE_B, PROBE_C };
    const int perm[6][3] = {
        {0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0}
    };

    sdmmc_card_t card; // Alokasi memori di stack

    for (int i = 0; i < 6; i++) {
        int clk = probe[perm[i][0]];
        int cmd = probe[perm[i][1]];
        int d0  = probe[perm[i][2]];

        ESP_LOGI(TAG, "Mencoba kombinasi %d: CLK=%d CMD=%d D0=%d", i+1, clk, cmd, d0);

        if (probe_once(clk, cmd, d0, &card) == ESP_OK) {
            ESP_LOGI(TAG, "BERHASIL! eMMC Terdeteksi.");
            
            // Perbaikan untuk ESP-IDF v5.1: Menggunakan card.raw_cid
            ESP_LOGI(TAG, "CID: %08x %08x %08x %08x",
                     (unsigned int)card.raw_cid[0], 
                     (unsigned int)card.raw_cid[1],
                     (unsigned int)card.raw_cid[2], 
                     (unsigned int)card.raw_cid[3]);
            
            ESP_LOGI(TAG, "Nama Produk: %s", card.cid.name);

            rgb_set(0,1,0); // Hijau
            while(1) vTaskDelay(pdMS_TO_TICKS(1000));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGE(TAG, "Gagal menemukan eMMC.");
    rgb_set(1,0,0); // Merah
}
