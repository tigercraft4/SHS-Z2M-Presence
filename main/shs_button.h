/*
 * SHS01 Presence Sensor — Button Module
 *
 * BOOT button handling: triple-click for config mode, quad-click for
 * LD2410C factory reset, long-press for ESP factory reset.
 */

#ifndef SHS_BUTTON_H
#define SHS_BUTTON_H

#include "shs_state.h"

void shs_button_init(shs_state_t *state);
void shs_boot_button_task(void *pv);

#endif /* SHS_BUTTON_H */
