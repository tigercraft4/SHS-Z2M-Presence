/*
 * SHS01 Presence Sensor — Configuration & NVS Module
 *
 * NVS load/save, zone config debounce, attribute handler.
 * Zone handler uses computed offsets instead of 30 switch cases (CS-03).
 */

#ifndef SHS_CONFIG_H
#define SHS_CONFIG_H

#include "shs_state.h"
#include "esp_zigbee_core.h"

void shs_config_init(shs_state_t *state);
void shs_cfg_load_from_nvs(void);
void shs_zone_cfg_load_from_nvs(void);
void shs_zone_cfg_check_pending(void);
void shs_zone_cfg_schedule_apply(void);
void shs_save_worker(void *pv);

/* Apply LD2410 sensor configuration (called at boot and on recovery) */
void shs_apply_ld2410_config(void);

/* Zigbee action handler (registered by zigbee module) */
esp_err_t shs_zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message);

#endif /* SHS_CONFIG_H */
