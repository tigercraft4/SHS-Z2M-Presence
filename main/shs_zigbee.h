/*
 * SHS01 Presence Sensor — Zigbee Module
 *
 * Endpoint creation, ZB helpers, lock macros, BDB commissioning.
 * Lock macros are defined in the .c file (reference module-static s_state).
 */

#ifndef SHS_ZIGBEE_H
#define SHS_ZIGBEE_H

#include "shs_state.h"
#include "esp_zigbee_core.h"

/* Init — stores state pointer, must be called before any ZB function */
void shs_zigbee_init(shs_state_t *state);

/* Zigbee task entry point (passed to xTaskCreate) */
void shs_zigbee_task(void *pvParameters);

/* Attribute helpers — called by config and sensor modules */
bool shs_zb_set_analog_value(uint8_t endpoint, float value);
bool shs_zb_report_analog_attr(uint8_t endpoint);
bool shs_zb_set_binary_value(uint8_t endpoint, bool value);
bool shs_zb_report_binary_attr(uint8_t endpoint);
bool shs_zb_set_occ_bitmap(uint8_t endpoint, bool occupied);
void shs_zb_set_bool_attr(uint8_t ep, uint16_t cluster, uint16_t attr, bool val);
void shs_zb_set_i16_attr(uint8_t ep, uint16_t cluster, uint16_t attr, int16_t val);
void shs_zb_report_attr(uint8_t ep, uint16_t cluster, uint16_t attr, bool is_mfr);
void shs_zb_set_ou_delay_ep2(uint16_t seconds);

/* Sensor health/diagnostics report (called by sensor module) */
void shs_zb_report_sensor_health(void);
void shs_zb_report_diagnostics(void);

/* Force-update functions (called after ZB join/rejoin) */
void shs_ld2450_force_update(void);
void shs_ld2410c_force_update(void);

#endif /* SHS_ZIGBEE_H */
