/*
 * SHS01 Presence Sensor — Application Entry Point
 *
 * Initializes NVS, shared state, module subsystems, sensor drivers,
 * and creates FreeRTOS tasks. All logic lives in dedicated modules.
 */

#include "shs01.h"
#include "shs_state.h"
#include "shs_zigbee.h"
#include "shs_config.h"
#include "shs_sensor.h"
#include "shs_button.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_task_wdt.h"
#include "light_driver.h"
#include "ld2410_enhanced.h"
#include "ld2450.h"

static const char *TAG = "SHS01";

/* Single global state structure — all modules receive a pointer */
static shs_state_t g_state;

void app_main(void) {
    /* Initialize NVS */
    esp_err_t nvs_rc = nvs_flash_init();
    if (nvs_rc == ESP_ERR_NVS_NO_FREE_PAGES || nvs_rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_rc = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_rc);

    /* Initialize shared state (mutexes, queue, defaults) */
    shs_state_init(&g_state);

    /* Initialize all modules with shared state pointer */
    shs_zigbee_init(&g_state);
    shs_config_init(&g_state);
    shs_button_init(&g_state);

    /* Load basic configuration from NVS */
    shs_cfg_load_from_nvs();

    /* Initialize light driver */
    light_driver_init(LIGHT_DEFAULT_OFF);

    /* Initialize Task Watchdog Timer */
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = 10000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    ESP_ERROR_CHECK(esp_task_wdt_init(&twdt_config));
    ESP_LOGI(TAG, "Task watchdog initialized (10s timeout, panic on trigger)");

    /* Start Zigbee task FIRST — must be responsive before coordinator interview */
    xTaskCreate(shs_zigbee_task, "shs_zigbee_main", 8192, NULL, 5, NULL);

    /* Initialize sensor drivers */
    ESP_ERROR_CHECK(ld2410_init());
    ESP_LOGI(TAG, "Initializing LD2450 on UART0 GPIO18/19...");
    ESP_ERROR_CHECK(ld2450_init());
    ESP_LOGI(TAG, "LD2450 initialization SUCCESS!");

    /* Initialize sensor module (registers callbacks) */
    shs_sensor_init(&g_state);

    /* Create NVS save worker task */
    xTaskCreate(shs_save_worker, "shs_save_worker", 3072, NULL, 3, NULL);

    /* Load zone configuration from NVS */
    shs_zone_cfg_load_from_nvs();

    /* Create sensor and button tasks (priority 4, below Zigbee at 5) */
    xTaskCreate(shs_ld2410_task, "shs_ld2410_task", 4096, NULL, 4, NULL);
    xTaskCreate(shs_ld2450_task, "shs_ld2450_task", 4096, NULL, 4, NULL);
    xTaskCreate(shs_boot_button_task, "shs_boot_button", 8192, NULL, 4, NULL);

    ESP_LOGI(TAG, "SHS01 firmware started - Position reporting via Zigbee (enable in Z2M)");
}
