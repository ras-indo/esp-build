#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"

/* ================== KONFIGURASI ================== */
/* 3 GPIO PROBE (tiga kabel ke board target) */
#define PROBE_A  36
#define PROBE_B  35
#define PROBE_C  34

/* RGB LED (3 GPIO biasa, BUKAN WS2812) */
#define PIN_R     2
#define PIN_G     3
#define PIN_B     4

/* Clock rendah untuk probing aman */
#define PROBE_CLK_KHZ  400
/* ================================================= */

static const char *TAG = "EMMC_3GPIO_SWAP";

/* ---------- RGB ---------- */
static void rgb_init(void) {
    gpio_config_t io = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL<<PIN_R) | (1ULL<<PIN_G) | (1ULL<<PIN_B),
    };
    gpio_config(&io);
}
static void rgb_on(void)  { gpio_set_level(PIN_R,1); gpio_set_level(PIN_G,1); gpio_set_level(PIN_B,1); }
static void rgb_off(void) { gpio_set_level(PIN_R,0); gpio_set_level(PIN_G,0); gpio_set_level(PIN_B,0); }

/* ---------- PROBE SATU KOMBINASI ---------- */
static esp_err_t probe_once(int clk_pin, int cmd_pin, int d0_pin, sdmmc_card_t **out_card)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = PROBE_CLK_KHZ;
    host.flags = SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = clk_pin;
    slot.cmd = cmd_pin;
    slot.d0  = d0_pin;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret;
    ret = sdmmc_host_init();
    if (ret != ESP_OK) return ret;

    ret = sdmmc_host_init_slot(host.slot, &slot);
    if (ret != ESP_OK) return ret;

    sdmmc_card_t *card = NULL;
    ret = sdmmc_card_init(&host, &card);
    if (ret == ESP_OK && card && card->is_mmc) {
        *out_card = card;
        return ESP_OK;
    }
    return ESP_FAIL;
}

void app_main(void)
{
    rgb_init();
    rgb_off();

    ESP_LOGI(TAG, "Start 3-GPIO role-swap eMMC probe");

    /* Tiga GPIO probe */
    const int probe[3] = { PROBE_A, PROBE_B, PROBE_C };

    /* 6 permutasi peran (A/B/C -> CLK/CMD/D0) */
    const int perm[6][3] = {
        {0,1,2},
        {0,2,1},
        {1,0,2},
        {1,2,0},
        {2,0,1},
        {2,1,0}
    };

    for (int i = 0; i < 6; i++) {
        int clk = probe[perm[i][0]];
        int cmd = probe[perm[i][1]];
        int d0  = probe[perm[i][2]];

        ESP_LOGI(TAG, "Try: CLK=%d CMD=%d D0=%d", clk, cmd, d0);

        sdmmc_card_t *card = NULL;
        if (probe_once(clk, cmd, d0, &card) == ESP_OK) {
            ESP_LOGI(TAG, "VALID eMMC FOUND");
            ESP_LOGI(TAG, "Mapping: CLK=%d CMD=%d D0=%d", clk, cmd, d0);
            ESP_LOGI(TAG, "CID: %08lx %08lx %08lx %08lx",
                     card->cid[0], card->cid[1],
                     card->cid[2], card->cid[3]);
            rgb_on();
            while (1) vTaskDelay(pdMS_TO_TICKS(1000));
        }

        vTaskDelay(pdMS_TO_TICKS(150));
    }

    ESP_LOGW(TAG, "No valid eMMC detected with any permutation");
    rgb_off();
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
