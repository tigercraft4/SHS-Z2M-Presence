/*
 * SHS01 Presence Sensor — Shared State Initialization
 */

#include <string.h>
#include "shs_state.h"

void shs_state_init(shs_state_t *state) {
    memset(state, 0, sizeof(shs_state_t));

    /* Create mutexes */
    state->target_data_mutex = xSemaphoreCreateMutex();
    state->zone_config_mutex = xSemaphoreCreateMutex();

    /* Create save worker queue */
    state->save_q = xQueueCreate(16, sizeof(shs_save_msg_t));

    /* LD2410C config defaults */
    state->moving_sens_0_100 = 60;
    state->static_sens_0_100 = 50;
    state->moving_max_gate   = 8;
    state->static_max_gate   = 8;
    state->sens_mv_0_10      = 4;   /* Matches threshold 60: 10 - (60/10) = 4 */
    state->sens_st_0_10      = 5;   /* Matches threshold 50: 10 - (50/10) = 5 */
    state->min_moving_energy  = 40;
    state->min_static_energy  = 40;

    /* Firmware version default */
    strncpy(state->firmware_version, "Unknown", sizeof(state->firmware_version) - 1);

    /* Zone defaults: all disabled, default bounds */
    for (int i = 0; i < SHS_NUM_ZONES; i++) {
        state->zones[i].enabled = false;
        state->zones[i].x1 = -1500;
        state->zones[i].y1 = 0;
        state->zones[i].x2 = 1500;
        state->zones[i].y2 = 3000;
        state->zones[i].type = 0;
        state->zones[i].targets = 0;
        state->zones[i].occupied = false;
    }

    /* Init values for Zigbee attribute registration */
    state->diag_init_zero = 0;
    state->health_init_false = false;
}
