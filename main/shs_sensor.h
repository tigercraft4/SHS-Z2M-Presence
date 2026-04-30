/*
 * SHS01 Presence Sensor — Sensor Module
 *
 * LD2410C and LD2450 task entry points, callbacks, zone occupancy logic.
 */

#ifndef SHS_SENSOR_H
#define SHS_SENSOR_H

#include "shs_state.h"

void shs_sensor_init(shs_state_t *state);
void shs_ld2410_task(void *pvParameters);
void shs_ld2450_task(void *pvParameters);

#endif /* SHS_SENSOR_H */
