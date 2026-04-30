/*
 * SHS01 Presence Sensor — Sensor Module
 *
 * LD2410C and LD2450 tasks, callbacks, target processing,
 * zone occupancy logic, position smoothing.
 */

#include <string.h>
#include <math.h>
#include "shs_sensor.h"
#include "shs_state.h"
#include "shs_zigbee.h"
#include "shs_config.h"
#include "shs01.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "ld2410_enhanced.h"
#include "ld2450.h"

static const char *TAG = "SHS_SENS";
static shs_state_t *s_state = NULL;

/* Connectivity check interval */
#define SHS_ZB_CONNECTIVITY_CHECK_MS  60000

/* Forward declarations */
static void shs_on_state_change(const ld2410_state_t *state);
static void shs_on_ld2450_target_update(const ld2450_target_t *targets, uint8_t active_count);
static void shs_on_ld2450_zone_update(const ld2450_zone_t *zones, bool occupancy);

void shs_sensor_init(shs_state_t *state) {
    s_state = state;

    /* Register sensor callbacks */
    ld2410_register_state_callback(shs_on_state_change);
    ld2450_register_target_callback(shs_on_ld2450_target_update);
    ld2450_register_zone_callback(shs_on_ld2450_zone_update);
    ESP_LOGI(TAG, "Sensor callbacks registered");
}

/* ============================================================================
 * TARGET VALIDATION & ZONE HELPERS
 * ============================================================================ */

static bool is_valid_target(const ld2450_target_t *target) {
    if (!target->active) return false;
    if (target->x < -3000 || target->x > 3000) return false;
    if (target->y < 0 || target->y > 6000) return false;
    if (target->distance > 6000) return false;
    return true;
}

static bool shs_point_in_zone(int16_t x, int16_t y, int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
    int16_t min_x = (x1 < x2) ? x1 : x2;
    int16_t max_x = (x1 > x2) ? x1 : x2;
    int16_t min_y = (y1 < y2) ? y1 : y2;
    int16_t max_y = (y1 > y2) ? y1 : y2;
    return (x >= min_x && x <= max_x && y >= min_y && y <= max_y);
}

static bool shs_target_in_interference_zone(int16_t x, int16_t y) {
    for (int z = 0; z < SHS_NUM_ZONES; z++) {
        if (s_state->zones[z].enabled && s_state->zones[z].type == LD2450_ZONE_INTERFERENCE) {
            if (shs_point_in_zone(x, y, s_state->zones[z].x1, s_state->zones[z].y1,
                                  s_state->zones[z].x2, s_state->zones[z].y2)) {
                return true;
            }
        }
    }
    return false;
}

/* ============================================================================
 * LD2450 CALLBACKS
 * ============================================================================ */

/* Zone endpoint lookup arrays */
static const uint8_t zone_occ_eps[SHS_NUM_ZONES] = {
    SHS_EP_LD2450_ZONE1, SHS_EP_LD2450_ZONE2, SHS_EP_LD2450_ZONE3,
    SHS_EP_LD2450_ZONE4, SHS_EP_LD2450_ZONE5
};
static const uint8_t zone_target_eps[SHS_NUM_ZONES] = {
    SHS_EP_ZONE1_TARGETS, SHS_EP_ZONE2_TARGETS, SHS_EP_ZONE3_TARGETS,
    SHS_EP_ZONE4_TARGETS, SHS_EP_ZONE5_TARGETS
};

static void shs_on_ld2450_target_update(const ld2450_target_t *targets, uint8_t active_count) {
    s_state->ld2450_connected = true;

    /* Calculate effective count excluding interference zones */
    uint8_t effective_count = 0;
    for (int i = 0; i < 3; i++) {
        if (is_valid_target(&targets[i]) && targets[i].active) {
            if (!shs_target_in_interference_zone(targets[i].x, targets[i].y)) {
                effective_count++;
            }
        }
    }

    /* Update target count */
    uint8_t old_target_count = effective_count;
    if (xSemaphoreTake(s_state->target_data_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        old_target_count = s_state->ld2450_target_count;
        xSemaphoreGive(s_state->target_data_mutex);
    }
    if (old_target_count != effective_count) {
        if (shs_zb_set_analog_value(SHS_EP_LD2450_TARGET_COUNT, (float)effective_count) &&
            shs_zb_report_analog_attr(SHS_EP_LD2450_TARGET_COUNT)) {
            if (xSemaphoreTake(s_state->target_data_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                s_state->ld2450_target_count = effective_count;
                xSemaphoreGive(s_state->target_data_mutex);
            }
            ESP_LOGI(TAG, "LD2450 target count: %d (raw: %d)", effective_count, active_count);
        } else if (s_state->zb_ready) {
            ESP_LOGW(TAG, "LD2450 target count report FAILED");
        }
    }

    /* Update overall occupancy */
    bool new_occupancy = (effective_count > 0);
    if (s_state->ld2450_occupancy != new_occupancy) {
        if (shs_zb_set_occ_bitmap(SHS_EP_LD2450_OCC, new_occupancy)) {
            s_state->ld2450_occupancy = new_occupancy;
            ESP_LOGI(TAG, "LD2450 occupancy: %s", new_occupancy ? "OCCUPIED" : "CLEAR");
        }
    }

    /* Store current target data (thread-safe) */
    if (xSemaphoreTake(s_state->target_data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        uint8_t valid_count = 0;
        for (int i = 0; i < 3; i++) {
            if (is_valid_target(&targets[i])) {
                s_state->current_targets[valid_count++] = targets[i];
            }
        }
        for (int i = valid_count; i < 3; i++) {
            memset(&s_state->current_targets[i], 0, sizeof(ld2450_target_t));
        }
        s_state->current_target_count = valid_count;
        xSemaphoreGive(s_state->target_data_mutex);
    }

    /* Position reporting with EMA smoothing */
    if (s_state->position_reporting) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        for (int i = 0; i < 3; i++) {
            if (is_valid_target(&targets[i]) && targets[i].active) {
                float raw_x = (float)targets[i].x;
                float raw_y = (float)targets[i].y;
                if (s_state->smoothed_x[i] == 0 && s_state->smoothed_y[i] == 0) {
                    s_state->smoothed_x[i] = raw_x;
                    s_state->smoothed_y[i] = raw_y;
                } else {
                    s_state->smoothed_x[i] = EMA_ALPHA * raw_x + (1.0f - EMA_ALPHA) * s_state->smoothed_x[i];
                    s_state->smoothed_y[i] = EMA_ALPHA * raw_y + (1.0f - EMA_ALPHA) * s_state->smoothed_y[i];
                }
            } else {
                s_state->smoothed_x[i] = 0;
                s_state->smoothed_y[i] = 0;
            }
        }

        if ((now_ms - s_state->last_position_update_ms) >= POSITION_UPDATE_INTERVAL_MS) {
            s_state->last_position_update_ms = now_ms;

            for (int i = 0; i < 3; i++) {
                uint8_t ep_base = SHS_EP_LD2450_T1_X + (i * 3);

                if (is_valid_target(&targets[i]) && targets[i].active) {
                    int16_t new_x = (int16_t)s_state->smoothed_x[i];
                    int16_t new_y = (int16_t)s_state->smoothed_y[i];
                    uint16_t new_dist = targets[i].distance;

                    int16_t dx = new_x - s_state->last_reported_x[i];
                    int16_t dy = new_y - s_state->last_reported_y[i];
                    int16_t dd = (int16_t)new_dist - (int16_t)s_state->last_reported_dist[i];
                    if (dx < 0) dx = -dx;
                    if (dy < 0) dy = -dy;
                    if (dd < 0) dd = -dd;

                    if (dx >= POSITION_CHANGE_THRESHOLD || dy >= POSITION_CHANGE_THRESHOLD || dd >= POSITION_CHANGE_THRESHOLD) {
                        shs_zb_set_analog_value(ep_base, (float)new_x);
                        shs_zb_report_analog_attr(ep_base);
                        shs_zb_set_analog_value(ep_base + 1, (float)new_y);
                        shs_zb_report_analog_attr(ep_base + 1);
                        shs_zb_set_analog_value(ep_base + 2, (float)new_dist);
                        shs_zb_report_analog_attr(ep_base + 2);
                        s_state->last_reported_x[i] = new_x;
                        s_state->last_reported_y[i] = new_y;
                        s_state->last_reported_dist[i] = new_dist;
                    }
                } else if (s_state->last_reported_x[i] != 0 || s_state->last_reported_y[i] != 0 || s_state->last_reported_dist[i] != 0) {
                    shs_zb_set_analog_value(ep_base, 0.0f);
                    shs_zb_set_analog_value(ep_base + 1, 0.0f);
                    shs_zb_set_analog_value(ep_base + 2, 0.0f);
                    shs_zb_report_analog_attr(ep_base);
                    shs_zb_report_analog_attr(ep_base + 1);
                    shs_zb_report_analog_attr(ep_base + 2);
                    s_state->last_reported_x[i] = 0;
                    s_state->last_reported_y[i] = 0;
                    s_state->last_reported_dist[i] = 0;
                }
            }
        }
    }
}

static void shs_on_ld2450_zone_update(const ld2450_zone_t *zones, bool occupancy) {
    for (int z = 0; z < SHS_NUM_ZONES; z++) {
        /* Update zone occupancy */
        if (zones[z].enabled && s_state->zones[z].occupied != zones[z].occupied) {
            if (shs_zb_set_binary_value(zone_occ_eps[z], zones[z].occupied) &&
                shs_zb_report_binary_attr(zone_occ_eps[z])) {
                s_state->zones[z].occupied = zones[z].occupied;
                ESP_LOGI(TAG, "Zone %d: %s", z + 1, zones[z].occupied ? "OCCUPIED" : "CLEAR");
            } else if (s_state->zb_ready) {
                ESP_LOGW(TAG, "Zone %d occupancy report FAILED", z + 1);
            }
        }

        /* Update zone target count */
        if (zones[z].enabled && s_state->zones[z].targets != zones[z].target_count) {
            if (shs_zb_set_analog_value(zone_target_eps[z], (float)zones[z].target_count) &&
                shs_zb_report_analog_attr(zone_target_eps[z])) {
                s_state->zones[z].targets = zones[z].target_count;
                ESP_LOGI(TAG, "Zone %d targets: %d", z + 1, s_state->zones[z].targets);
            } else if (s_state->zb_ready) {
                ESP_LOGW(TAG, "Zone %d targets report FAILED", z + 1);
            }
        }
    }
}

/* ============================================================================
 * LD2410C CALLBACK
 * ============================================================================ */

static void shs_on_state_change(const ld2410_state_t *state) {
    bool raw_moving = (state->target.target_state & 0x01) != 0;
    bool raw_static = (state->target.target_state & 0x02) != 0;

    /* LD2450 cross-validation */
    uint8_t ld2450_count = 0;
    if (xSemaphoreTake(s_state->target_data_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        ld2450_count = s_state->ld2450_target_count;
        xSemaphoreGive(s_state->target_data_mutex);
    }
    if (s_state->ld2450_connected && ld2450_count == 0) {
        raw_moving = false;
        raw_static = false;
    } else if (!s_state->ld2450_connected) {
        if (raw_moving && state->target.moving_energy < s_state->min_moving_energy)
            raw_moving = false;
        if (raw_static && state->target.static_energy < s_state->min_static_energy)
            raw_static = false;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    /* MOVING with cooldown */
    bool report_moving;
    if (s_state->movement_cooldown_sec == 0) {
        report_moving = raw_moving;
    } else {
        if (raw_moving) {
            report_moving = true;
            s_state->moving_cooldown_until = now_ms + (s_state->movement_cooldown_sec * 1000);
        } else if (now_ms < s_state->moving_cooldown_until) {
            report_moving = true;
        } else {
            report_moving = false;
        }
    }

    if (report_moving != s_state->moving_state) {
        s_state->moving_state = report_moving;
        ESP_LOGI(TAG, "Moving Target -> %s", s_state->moving_state ? "DETECTED" : "CLEAR");
        shs_zb_set_bool_attr(SHS_EP_OCC, ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
                            SHS_ATTR_OCC_MOVING_TARGET, s_state->moving_state);
    }

    /* STATIC with cooldown */
    bool report_static;
    if (s_state->occupancy_clear_sec == 0) {
        report_static = raw_static;
    } else {
        if (raw_static) {
            report_static = true;
            s_state->static_cooldown_until = now_ms + (s_state->occupancy_clear_sec * 1000);
        } else if (now_ms < s_state->static_cooldown_until) {
            report_static = true;
        } else {
            report_static = false;
        }
    }

    if (report_static != s_state->static_state) {
        s_state->static_state = report_static;
        ESP_LOGI(TAG, "Static Target -> %s", s_state->static_state ? "DETECTED" : "CLEAR");
        shs_zb_set_bool_attr(SHS_EP_OCC, ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
                            SHS_ATTR_OCC_STATIC_TARGET, s_state->static_state);
    }

    /* OCCUPANCY */
    bool presence = report_moving || report_static;
    if (presence != s_state->occupancy_state) {
        s_state->occupancy_state = presence;
        ESP_LOGI(TAG, "Occupancy -> %s", presence ? "DETECTED" : "CLEAR");
        shs_zb_set_occ_bitmap(SHS_EP_OCC, presence);
    }
}

/* ============================================================================
 * SENSOR TASKS
 * ============================================================================ */

void shs_ld2410_task(void *pvParameters) {
    ESP_LOGI(TAG, "LD2410 processing task started");
    vTaskDelay(pdMS_TO_TICKS(500));
    shs_apply_ld2410_config();

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    uint32_t last_connected_time = 0;
    bool was_connected = false;
    const uint32_t RECOVERY_INTERVAL_MS = 30000;

    while (1) {
        ld2410_process();
        esp_task_wdt_reset();

        bool connected = ld2410_is_connected();
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        if (connected) {
            last_connected_time = now;
            if (!was_connected) {
                ESP_LOGI(TAG, "LD2410 connection restored");
            }
        } else if (was_connected) {
            ESP_LOGW(TAG, "LD2410 disconnected - will attempt recovery");
        } else if ((now - last_connected_time) > RECOVERY_INTERVAL_MS && last_connected_time > 0) {
            ESP_LOGW(TAG, "LD2410 still disconnected - sending restart command");
            ld2410_restart();
            last_connected_time = now;
            vTaskDelay(pdMS_TO_TICKS(500));
            shs_apply_ld2410_config();
        }

        /* LD2410C frame-based disconnect detection */
        if (connected) {
            s_state->ld2410c_last_frame_ms = now;
            if (!s_state->ld2410c_connected) {
                s_state->ld2410c_connected = true;
                ESP_LOGI(TAG, "LD2410C connected (frame received)");
            }
        } else if (s_state->ld2410c_connected && s_state->ld2410c_last_frame_ms > 0 &&
                   (now - s_state->ld2410c_last_frame_ms) > SHS_LD2410C_FRAME_TIMEOUT_MS) {
            s_state->ld2410c_connected = false;
            ESP_LOGW(TAG, "LD2410C disconnected (no frame for %lu ms)",
                     (unsigned long)(now - s_state->ld2410c_last_frame_ms));
        }

        was_connected = connected;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void shs_ld2450_task(void *pvParameters) {
    ESP_LOGI(TAG, "LD2450 processing task started");
    vTaskDelay(pdMS_TO_TICKS(2500));

    /* Apply initial zone config */
    shs_zone_cfg_check_pending();
    /* Force initial apply by calling it directly */
    {
        /* Trigger a pending apply for boot-time config */
        s_state->zone_cfg_pending = true;
        s_state->zone_cfg_pending_until = 0;  /* Apply immediately */
        shs_zone_cfg_check_pending();
    }

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    uint32_t last_connected_time = 0;
    bool was_connected = false;
    const uint32_t RECOVERY_INTERVAL_MS = 30000;
    uint32_t last_zb_check_time = 0;

    while (1) {
        ld2450_process();
        esp_task_wdt_reset();

        shs_zone_cfg_check_pending();

        bool connected = ld2450_is_connected();
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        if (connected) {
            last_connected_time = now;
            if (!was_connected) {
                ESP_LOGI(TAG, "LD2450 connection established");
                vTaskDelay(pdMS_TO_TICKS(500));
                ld2450_read_firmware_version();
                if (s_state->zb_ready) {
                    shs_zb_report_sensor_health();
                }
            }
        } else if (was_connected) {
            ESP_LOGW(TAG, "LD2450 disconnected - will attempt recovery");
            if (s_state->zb_ready) {
                shs_zb_report_sensor_health();
            }
        } else if ((now - last_connected_time) > RECOVERY_INTERVAL_MS && last_connected_time > 0) {
            ESP_LOGW(TAG, "LD2450 still disconnected - sending restart command");
            ld2450_restart();
            last_connected_time = now;
            vTaskDelay(pdMS_TO_TICKS(500));
            /* Re-apply zone config */
            s_state->zone_cfg_pending = true;
            s_state->zone_cfg_pending_until = 0;
            shs_zone_cfg_check_pending();
        }

        was_connected = connected;

        /* Periodic connectivity check and heartbeat */
        if (s_state->zb_ready && (now - last_zb_check_time) >= SHS_ZB_CONNECTIVITY_CHECK_MS) {
            last_zb_check_time = now;

            uint32_t time_since_last_tx = now - s_state->last_successful_tx;
            ESP_LOGI(TAG, "Zigbee stats: lock_ok=%lu lock_fail=%lu tx_ok=%lu last_tx=%lums ago",
                     (unsigned long)s_state->lock_success_count, (unsigned long)s_state->lock_fail_count,
                     (unsigned long)s_state->tx_success_count, (unsigned long)time_since_last_tx);

            if (s_state->zb_connected && !s_state->zb_rejoin_pending) {
                shs_zb_set_analog_value(SHS_EP_LD2450_TARGET_COUNT, (float)s_state->ld2450_target_count);
                shs_zb_report_analog_attr(SHS_EP_LD2450_TARGET_COUNT);
            }

            shs_zb_report_sensor_health();
            shs_zb_report_diagnostics();

            if (time_since_last_tx > 180000 && s_state->last_successful_tx > 0 && !s_state->zb_rejoin_pending) {
                ESP_LOGW(TAG, "Zigbee: No successful TX for %lu ms - scheduling rejoin",
                         (unsigned long)time_since_last_tx);
                s_state->zb_rejoin_pending = true;
                s_state->zb_connected = false;
                shs_zb_report_sensor_health();
                s_state->lock_fail_count = 0;
                s_state->lock_success_count = 0;
                s_state->tx_success_count = 0;
                shs_zb_schedule_rejoin();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
