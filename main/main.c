#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"

/* ================== KONFIGURASI ================== */
#define PROBE_A  36
#define PROBE_B  35
#define PROBE_C  34

#define PIN_R     2
#define PIN_G     3
#define PIN_B     4

#define PROBE_CLK_KHZ  400
/* ================================================= */

static const char *TAG = "EMMC_PROBE";

/* ---------- RGB ---------- */
static void rgb_init(void) {
    gpio_config_t io = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL<<PIN_R) | (1ULL<<PIN_G) | (1ULL<<PIN_B),
        .intr_type = GPIO_INTR_DISABLE,
        .pull_down_en = 0,
        .pull_up_en = 0
    };
    gpio_config(&io);
}

static void rgb_set(int r, int g, int b) {
    gpio_set_level(PIN_R, r);
    gpio_set_level(PIN_G, g);
    gpio_set_level(PIN_B, b);
}

/* ---------- PROBE SATU KOMBINASI ---------- */
static esp_err_t probe_once(int clk_pin, int cmd_pin, int d0_pin, sdmmc_card_t *out_card)
{
    esp_err_t ret;

    // 1. Konfigurasi Host
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = PROBE_CLK_KHZ;
    host.flags = SDMMC_HOST_FLAG_1BIT;

    // 2. Konfigurasi Slot
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = clk_pin;
    slot.cmd = cmd_pin;
    slot.d0  = d0_pin;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    // 3. Inisialisasi Host
    ret = sdmmc_host_init();
    if (ret != ESP_OK) return ret;

    ret = sdmmc_host_init_slot(host.slot, &slot);
    if (ret != ESP_OK) {
        sdmmc_host_deinit();
        return ret;
    }

    // 4. Inisialisasi Kartu
    // Perbaikan: out_card adalah pointer ke struct yang sudah dialokasikan di app_main
    ret = sdmmc_card_init(&host, out_card);

    if (ret == ESP_OK) {
        if (out_card->is_mmc) {
            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "Device found but not an eMMC");
            ret = ESP_FAIL;
        }
    }

    // Bersihkan driver jika gagal agar bisa dicoba lagi di loop berikutnya
    sdmmc_host_deinit();
    return ret;
}

void app_main(void)
{
    rgb_init();
    rgb_set(0, 0, 0); // Off

    ESP_LOGI(TAG, "Starting 3-GPIO role-swap eMMC probe...");

    const int probe[3] = { PROBE_A, PROBE_B, PROBE_C };

    // 6 permutasi peran (A/B/C -> CLK/CMD/D0)
    const int perm[6][3] = {
        {0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0}
    };

    sdmmc_card_t card; // Alokasi di stack agar aman dari memory leak

    for (int i = 0; i < 6; i++) {
        int clk = probe[perm[i][0]];
        int cmd = probe[perm[i][1]];
        int d0  = probe[perm[i][2]];

        ESP_LOGI(TAG, "Trial %d/6: CLK=%d CMD=%d D0=%d", i+1, clk, cmd, d0);

        if (probe_once(clk, cmd, d0, &card) == ESP_OK) {
            ESP_LOGI(TAG, "*******************************");
            ESP_LOGI(TAG, "SUCCESS! VALID eMMC FOUND");
            ESP_LOGI(TAG, "Final Mapping: CLK=%d CMD=%d D0=%d", clk, cmd, d0);
            
            // Perbaikan akses CID: Menggunakan .raw_data[i]
            ESP_LOGI(TAG, "CID Raw: %08lx %08lx %08lx %08lx",
                     card.cid.raw_data[0], 
                     card.cid.raw_data[1],
                     card.cid.raw_data[2], 
                     card.cid.raw_data[3]);
            
            ESP_LOGI(TAG, "Product Name: %s", card.cid.name);
            ESP_LOGI(TAG, "Capacity: %llu MB", ((uint64_t)card.csd.capacity) * card.csd.sector_size / (1024 * 1024));
            ESP_LOGI(TAG, "*******************************");

            rgb_set(0, 1, 0); // Hijau (Berhasil)
            while (1) vTaskDelay(pdMS_TO_TICKS(1000));
        }

        rgb_set(1, 0, 0); // Merah sebentar jika gagal
        vTaskDelay(pdMS_TO_TICKS(100));
        rgb_set(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGE(TAG, "No eMMC detected. Check wiring or target power.");
    rgb_set(1, 0, 0); // Tetap merah jika semua gagal
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
