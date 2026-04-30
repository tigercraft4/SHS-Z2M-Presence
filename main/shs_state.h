/*
 * SHS01 Presence Sensor — Shared State Definitions
 *
 * Consolidates all static globals from shs01.c into a single struct.
 * All modules receive a pointer to shs_state_t from app_main.
 */

#ifndef SHS_STATE_H
#define SHS_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "ld2450.h"

/* Zone and target limits */
#define SHS_NUM_ZONES       5
#define SHS_MAX_TARGETS     3

/* LD2410C disconnect detection threshold */
#define SHS_LD2410C_FRAME_TIMEOUT_MS  3000

/* Position smoothing and rate limiting */
#define POSITION_UPDATE_INTERVAL_MS  200
#define POSITION_CHANGE_THRESHOLD    30
#define EMA_ALPHA                    0.3f

/* NVS save event types */
typedef enum {
    SHS_SAVE_IMMEDIATE_U16,
    SHS_SAVE_DEBOUNCE_SENS_MOVE,
    SHS_SAVE_DEBOUNCE_SENS_STATIC,
    SHS_SAVE_DEBOUNCE_GATE_MOVE,
    SHS_SAVE_DEBOUNCE_GATE_STATIC,
    SHS_SAVE_ZONE_CONFIG,
} shs_save_evt_t;

typedef struct {
    shs_save_evt_t type;
    uint16_t       u16;
} shs_save_msg_t;

/* Per-zone state */
typedef struct {
    bool    enabled;
    int16_t x1, y1, x2, y2;
    uint8_t type;       /* 0=off, 1=detection, 2=filter, 3=interference */
    uint8_t targets;    /* current target count in zone */
    bool    occupied;   /* current occupancy state */
} shs_zone_t;

/* Packed zone config for atomic NVS blob save */
typedef struct __attribute__((packed)) {
    uint8_t zone_type;
    struct __attribute__((packed)) {
        uint8_t enabled;
        int16_t x1, y1, x2, y2;
        uint8_t type;
    } zones[SHS_NUM_ZONES];
} shs_zone_cfg_blob_t;

/* Main application state — single instance shared by all modules */
typedef struct {
    /* Mutexes */
    SemaphoreHandle_t target_data_mutex;
    SemaphoreHandle_t zone_config_mutex;

    /* Save worker queue */
    QueueHandle_t save_q;

    /* Current targets (from LD2450) */
    ld2450_target_t current_targets[SHS_MAX_TARGETS];
    uint8_t current_target_count;

    /* LD2410C config */
    uint16_t movement_cooldown_sec;
    uint16_t occupancy_clear_sec;
    uint32_t moving_cooldown_until;
    uint32_t static_cooldown_until;
    uint8_t  moving_sens_0_100;
    uint8_t  static_sens_0_100;
    uint16_t moving_max_gate;
    uint16_t static_max_gate;
    uint16_t sens_mv_0_10;
    uint16_t sens_st_0_10;
    volatile bool position_reporting;
    uint16_t min_moving_energy;
    uint16_t min_static_energy;

    /* LD2450 state */
    bool ld2450_connected;
    char firmware_version[20];
    bool ld2450_occupancy;
    uint8_t ld2450_target_count;

    /* LD2410C state */
    bool moving_state;
    bool static_state;
    bool occupancy_state;

    /* Zigbee state */
    volatile bool zb_ready;
    volatile bool zb_connected;
    volatile bool zb_rejoin_pending;
    uint32_t last_successful_tx;

    /* Diagnostic counters */
    uint32_t lock_success_count;
    uint32_t lock_fail_count;
    uint32_t lock_consecutive_fails;
    uint32_t tx_success_count;
    uint32_t tx_fail_count;

    /* LD2410C health */
    uint32_t ld2410c_last_frame_ms;
    bool ld2410c_connected;

    /* Zones */
    uint8_t zone_type;  /* global zone mode: 0=disabled, 1=detection, 2=filter */
    shs_zone_t zones[SHS_NUM_ZONES];

    /* Zone config scheduling */
    uint32_t zone_cfg_pending_until;
    bool zone_cfg_pending;
    bool zone_cfg_save_needed;

    /* Position smoothing */
    uint32_t last_position_update_ms;
    float smoothed_x[SHS_MAX_TARGETS];
    float smoothed_y[SHS_MAX_TARGETS];
    int16_t last_reported_x[SHS_MAX_TARGETS];
    int16_t last_reported_y[SHS_MAX_TARGETS];
    uint16_t last_reported_dist[SHS_MAX_TARGETS];

    /* Static init values for Zigbee attribute registration */
    uint32_t diag_init_zero;
    bool health_init_false;
} shs_state_t;

/* Initialize state struct: creates mutexes, sets defaults */
void shs_state_init(shs_state_t *state);

#endif /* SHS_STATE_H */
