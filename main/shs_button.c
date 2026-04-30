/*
 * SHS01 Presence Sensor — Button Module
 *
 * BOOT button: triple-click config toggle, quad-click LD2410C reset,
 * long-press (6s) ESP factory reset. LED feedback via light_driver.
 */

#include "shs_button.h"
#include "shs_state.h"
#include "shs_zigbee.h"
#include "shs01.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "light_driver.h"
#include "ld2410_enhanced.h"
#include "ld2450.h"
#include "esp_zigbee_core.h"

static const char *TAG = "SHS_BTN";
static shs_state_t *s_state = NULL;

void shs_button_init(shs_state_t *state) {
    s_state = state;
}

static void shs_flash_led(int count, int on_ms, int off_ms) {
    for (int i = 0; i < count; i++) {
        light_driver_set_power(true);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        light_driver_set_power(false);
        if (off_ms > 0) vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}

void shs_boot_button_task(void *pv) {
    const TickType_t poll = pdMS_TO_TICKS(20);
    const uint32_t factory_reset_ticks = SHS_FACTORY_RESET_PRESS_MS / 20;
    const uint32_t click_min_ticks = SHS_CLICK_MIN_MS / 20;
    const uint32_t click_max_ticks = SHS_CLICK_MAX_MS / 20;
    const uint32_t triple_click_window_ticks = SHS_TRIPLE_CLICK_WINDOW_MS / 20;
    const uint32_t debounce_ticks = SHS_DEBOUNCE_MS / 20;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << SHS_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);

    ESP_LOGI(TAG, "BOOT button: Triple-click for config mode, hold 6s for factory reset");

    /* Wait for startup period — ignore button activity */
    ESP_LOGI(TAG, "Ignoring button for %dms startup period...", SHS_STARTUP_IGNORE_MS);
    vTaskDelay(pdMS_TO_TICKS(SHS_STARTUP_IGNORE_MS));

    uint32_t held = 0;
    bool reset_armed = false;
    int last_level = 1;

    /* Triple-click detection */
    uint8_t click_count = 0;
    uint32_t last_click_time = 0;
    uint32_t tick_counter = 0;

    /* Debounce */
    int debounced_level = 1;
    uint32_t level_stable_count = 0;

    while (1) {
        int raw_level = gpio_get_level(SHS_BOOT_BUTTON_GPIO);
        tick_counter++;

        /* Debounce */
        if (raw_level == debounced_level) {
            level_stable_count = 0;
        } else {
            level_stable_count++;
            if (level_stable_count >= debounce_ticks) {
                debounced_level = raw_level;
                level_stable_count = 0;
            }
        }

        int level = debounced_level;

        if (level == 0) {
            /* Button pressed */
            if (held < factory_reset_ticks + 100) held++;

            /* Factory reset threshold (6 seconds) */
            if (!reset_armed && held >= (factory_reset_ticks - 20) && held < factory_reset_ticks) {
                reset_armed = true;
                click_count = 0;
                ESP_LOGW(TAG, "Keep holding for factory reset...");
                light_driver_set_power(true);
            }
            if (held >= factory_reset_ticks) {
                ESP_LOGW(TAG, "BOOT long-press confirmed: factory reset...");
                light_driver_set_power(false);
                esp_zb_factory_reset();
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        } else {
            /* Button released */
            if (reset_armed) {
                light_driver_set_power(false);
                ESP_LOGI(TAG, "Factory reset cancelled (released too early)");
            }

            /* Valid click (short press) — on transition from pressed */
            if (last_level == 0 && held >= click_min_ticks && held <= click_max_ticks && !reset_armed) {
                uint32_t now = tick_counter;

                if (click_count > 0 && (now - last_click_time) > triple_click_window_ticks) {
                    ESP_LOGI(TAG, "Click window expired, resetting count");
                    click_count = 0;
                }

                click_count++;
                last_click_time = now;
                ESP_LOGI(TAG, "Click %d detected (held=%lu ticks)", click_count, (unsigned long)held);

                if (click_count == 4) {
                    /* QUADRUPLE-CLICK: Factory reset LD2410C */
                    click_count = 0;
                    ESP_LOGW(TAG, "QUADRUPLE-CLICK: Factory resetting LD2410C sensor...");
                    shs_flash_led(4, 100, 100);
                    esp_err_t err = ld2410_factory_reset();
                    if (err == ESP_OK) {
                        ESP_LOGI(TAG, "LD2410C factory reset SUCCESS");
                        shs_flash_led(1, 1000, 0);
                    } else {
                        ESP_LOGE(TAG, "LD2410C factory reset FAILED: %s", esp_err_to_name(err));
                        shs_flash_led(5, 50, 50);
                    }
                } else if (click_count == 3) {
                    /* TRIPLE-CLICK: Toggle config/position reporting mode */
                    click_count = 0;
                    s_state->position_reporting = !s_state->position_reporting;
                    ld2450_set_verbose_logging(s_state->position_reporting);
                    light_driver_set_power(s_state->position_reporting);

                    ESP_LOGI(TAG, "TRIPLE-CLICK: CONFIG MODE %s (light %s)",
                             s_state->position_reporting ? "ENABLED" : "DISABLED",
                             s_state->position_reporting ? "ON" : "OFF");

                    if (s_state->position_reporting) {
                        shs_flash_led(2, 100, 100);
                    } else {
                        shs_flash_led(1, 500, 0);
                    }

                    /* Update Zigbee attribute */
                    if (s_state->zb_ready && esp_zb_lock_acquire(pdMS_TO_TICKS(SHS_ZB_LOCK_TIMEOUT_MS))) {
                        esp_zb_zcl_set_attribute_val(
                            SHS_EP_LIGHT,
                            SHS_CL_CFG_ID,
                            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                            SHS_ATTR_POSITION_REPORTING,
                            &s_state->position_reporting,
                            true
                        );
                        esp_zb_lock_release();
                    }
                }
            } else if (last_level == 0 && held > click_max_ticks && !reset_armed) {
                if (click_count > 0) {
                    ESP_LOGI(TAG, "Long press detected, resetting click count");
                    click_count = 0;
                }
            }

            held = 0;
            reset_armed = false;
        }

        last_level = level;

        /* Reset click count if window expired (while released) */
        if (click_count > 0 && level == 1 && (tick_counter - last_click_time) > triple_click_window_ticks) {
            ESP_LOGI(TAG, "Click window timeout, resetting count");
            click_count = 0;
        }

        vTaskDelay(poll);
    }
}
