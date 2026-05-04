/*
 * SHS01 Presence Sensor — Zigbee Module
 *
 * All Zigbee code: endpoint creation, ZB helpers, lock macros,
 * BDB commissioning, signal handler, force-update functions.
 */

#include <string.h>
#include "shs_zigbee.h"
#include "shs_state.h"
#include "shs01.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "esp_zigbee_attribute.h"
#include "esp_zigbee_cluster.h"
#include "zcl_utility.h"
#include "esp_timer.h"

static const char *TAG = "SHS_ZB";
static shs_state_t *s_state = NULL;

/* Forward declarations */
static void shs_bdb_start_top_level_commissioning_cb(uint8_t mode_mask);

/* Action handler — implemented in shs_config module, registered here */
extern esp_err_t shs_zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message);

/* ============================================================================
 * INIT
 * ============================================================================ */

void shs_zigbee_init(shs_state_t *state) {
    s_state = state;
}

/* ============================================================================
 * LOCK MACROS (reference module-static s_state)
 * ============================================================================ */

#define SHS_ZB_LOCK_ACQUIRE_OR_RETURN() \
    do { \
        bool _lock_ok = false; \
        static const TickType_t _backoff[] = { \
            pdMS_TO_TICKS(50), pdMS_TO_TICKS(100), pdMS_TO_TICKS(200) \
        }; \
        for (int _try = 0; _try < 3; _try++) { \
            if (esp_zb_lock_acquire(_backoff[_try]) == true) { \
                _lock_ok = true; \
                break; \
            } \
            ESP_LOGD(TAG, "Zigbee lock retry %d/3", _try + 1); \
        } \
        if (!_lock_ok) { \
            s_state->lock_fail_count++; \
            s_state->lock_consecutive_fails++; \
            if (s_state->lock_consecutive_fails == 1 || s_state->lock_consecutive_fails == 10 || \
                s_state->lock_consecutive_fails == 50 || (s_state->lock_consecutive_fails % 100) == 0) { \
                ESP_LOGW(TAG, "Zigbee lock FAILED after 3 retries #%lu (consecutive: %lu)", \
                         (unsigned long)s_state->lock_fail_count, (unsigned long)s_state->lock_consecutive_fails); \
            } \
            return; \
        } \
        s_state->lock_success_count++; \
        s_state->lock_consecutive_fails = 0; \
    } while(0)

#define SHS_ZB_LOCK_ACQUIRE_OR_RETURN_FALSE() \
    do { \
        bool _lock_ok = false; \
        static const TickType_t _backoff[] = { \
            pdMS_TO_TICKS(50), pdMS_TO_TICKS(100), pdMS_TO_TICKS(200) \
        }; \
        for (int _try = 0; _try < 3; _try++) { \
            if (esp_zb_lock_acquire(_backoff[_try]) == true) { \
                _lock_ok = true; \
                break; \
            } \
            ESP_LOGD(TAG, "Zigbee lock retry %d/3", _try + 1); \
        } \
        if (!_lock_ok) { \
            s_state->lock_fail_count++; \
            s_state->lock_consecutive_fails++; \
            if (s_state->lock_consecutive_fails == 1 || s_state->lock_consecutive_fails == 10 || \
                s_state->lock_consecutive_fails == 50 || (s_state->lock_consecutive_fails % 100) == 0) { \
                ESP_LOGW(TAG, "Zigbee lock FAILED after 3 retries #%lu (consecutive: %lu)", \
                         (unsigned long)s_state->lock_fail_count, (unsigned long)s_state->lock_consecutive_fails); \
            } \
            return false; \
        } \
        s_state->lock_success_count++; \
        s_state->lock_consecutive_fails = 0; \
    } while(0)

/* ============================================================================
 * ZIGBEE ATTRIBUTE HELPERS
 * ============================================================================ */

bool shs_zb_set_occ_bitmap(uint8_t endpoint, bool occupied) {
    if (!s_state->zb_ready) return false;
    uint8_t v = occupied ? 1 : 0;
    SHS_ZB_LOCK_ACQUIRE_OR_RETURN_FALSE();
    esp_zb_zcl_set_attribute_val(endpoint,
                                 ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID,
                                 &v, false);
    esp_zb_lock_release();
    return true;
}

void shs_zb_set_bool_attr(uint8_t endpoint, uint16_t cluster, uint16_t attr_id, bool value) {
    if (!s_state->zb_ready) return;
    bool v = value;
    SHS_ZB_LOCK_ACQUIRE_OR_RETURN();
    esp_zb_zcl_set_attribute_val(endpoint, cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr_id, &v, false);
    esp_zb_lock_release();
}

void shs_zb_report_attr(uint8_t endpoint, uint16_t cluster, uint16_t attr_id, bool is_mfr_specific) {
    if (!s_state->zb_ready) return;

    esp_zb_zcl_report_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .src_endpoint = endpoint,
            .dst_endpoint = 1,
            .dst_addr_u.addr_short = 0x0000,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = cluster,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
        .dis_default_resp = 1,
        .manuf_specific = is_mfr_specific ? 1 : 0,
        .manuf_code = is_mfr_specific ? 0x115F : 0,
        .attributeID = attr_id,
    };

    SHS_ZB_LOCK_ACQUIRE_OR_RETURN();
    esp_zb_zcl_report_attr_cmd_req(&cmd);
    s_state->last_successful_tx = (uint32_t)(esp_timer_get_time() / 1000);
    s_state->tx_success_count++;
    esp_zb_lock_release();
}

void shs_zb_set_i16_attr(uint8_t endpoint, uint16_t cluster, uint16_t attr_id, int16_t value) {
    if (!s_state->zb_ready) return;
    SHS_ZB_LOCK_ACQUIRE_OR_RETURN();
    esp_zb_zcl_set_attribute_val(endpoint, cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr_id, &value, true);
    esp_zb_lock_release();
}

void shs_zb_set_ou_delay_ep2(uint16_t seconds) {
    if (!s_state->zb_ready) return;
    SHS_ZB_LOCK_ACQUIRE_OR_RETURN();
    esp_zb_zcl_set_attribute_val(SHS_EP_OCC,
                                 ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 SHS_ZCL_ATTR_OCC_PIR_OU_DELAY,
                                 &seconds, false);
    esp_zb_lock_release();
}

bool shs_zb_set_analog_value(uint8_t endpoint, float value) {
    if (!s_state->zb_ready) return false;
    SHS_ZB_LOCK_ACQUIRE_OR_RETURN_FALSE();
    esp_zb_zcl_set_attribute_val(endpoint, SHS_CLUSTER_ANALOG_INPUT,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 SHS_ATTR_PRESENT_VALUE, &value, false);
    esp_zb_lock_release();
    return true;
}

bool shs_zb_report_analog_attr(uint8_t endpoint) {
    if (!s_state->zb_ready) return false;
    esp_zb_zcl_report_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .src_endpoint = endpoint,
            .dst_endpoint = 1,
            .dst_addr_u.addr_short = 0x0000,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = SHS_CLUSTER_ANALOG_INPUT,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
        .dis_default_resp = 1,
        .manuf_specific = 0,
        .manuf_code = 0,
        .attributeID = SHS_ATTR_PRESENT_VALUE,
    };
    SHS_ZB_LOCK_ACQUIRE_OR_RETURN_FALSE();
    esp_zb_zcl_report_attr_cmd_req(&cmd);
    s_state->last_successful_tx = (uint32_t)(esp_timer_get_time() / 1000);
    s_state->tx_success_count++;
    esp_zb_lock_release();
    return true;
}

bool shs_zb_set_binary_value(uint8_t endpoint, bool value) {
    if (!s_state->zb_ready) return false;
    uint8_t val = value ? 1 : 0;
    SHS_ZB_LOCK_ACQUIRE_OR_RETURN_FALSE();
    esp_zb_zcl_set_attribute_val(endpoint, SHS_CLUSTER_BINARY_INPUT,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 SHS_ATTR_PRESENT_VALUE_BINARY, &val, false);
    esp_zb_lock_release();
    return true;
}

bool shs_zb_report_binary_attr(uint8_t endpoint) {
    if (!s_state->zb_ready) return false;
    esp_zb_zcl_report_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .src_endpoint = endpoint,
            .dst_endpoint = 1,
            .dst_addr_u.addr_short = 0x0000,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = SHS_CLUSTER_BINARY_INPUT,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
        .dis_default_resp = 1,
        .manuf_specific = 0,
        .manuf_code = 0,
        .attributeID = SHS_ATTR_PRESENT_VALUE_BINARY,
    };
    SHS_ZB_LOCK_ACQUIRE_OR_RETURN_FALSE();
    esp_zb_zcl_report_attr_cmd_req(&cmd);
    s_state->last_successful_tx = (uint32_t)(esp_timer_get_time() / 1000);
    s_state->tx_success_count++;
    esp_zb_lock_release();
    return true;
}

/* ============================================================================
 * HEALTH & DIAGNOSTICS REPORTING
 * ============================================================================ */

void shs_zb_report_sensor_health(void) {
    SHS_ZB_LOCK_ACQUIRE_OR_RETURN();
    bool ld2410c = s_state->ld2410c_connected;
    bool ld2450 = s_state->ld2450_connected;
    esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT, SHS_CL_CFG_ID,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, SHS_ATTR_LD2410C_CONNECTED, &ld2410c, false);
    esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT, SHS_CL_CFG_ID,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, SHS_ATTR_LD2450_CONNECTED, &ld2450, false);
    esp_zb_lock_release();
}

void shs_zb_report_diagnostics(void) {
    SHS_ZB_LOCK_ACQUIRE_OR_RETURN();
    uint32_t uptime = (uint32_t)(esp_timer_get_time() / 1000000);
    uint32_t heap = (uint32_t)esp_get_free_heap_size();
    esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT, SHS_CL_CFG_ID,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, SHS_ATTR_DIAG_LOCK_SUCCESS, &s_state->lock_success_count, false);
    esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT, SHS_CL_CFG_ID,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, SHS_ATTR_DIAG_LOCK_FAIL, &s_state->lock_fail_count, false);
    esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT, SHS_CL_CFG_ID,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, SHS_ATTR_DIAG_LOCK_CONSEC_FAIL, &s_state->lock_consecutive_fails, false);
    esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT, SHS_CL_CFG_ID,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, SHS_ATTR_DIAG_TX_SUCCESS, &s_state->tx_success_count, false);
    esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT, SHS_CL_CFG_ID,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, SHS_ATTR_DIAG_TX_FAIL, &s_state->tx_fail_count, false);
    esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT, SHS_CL_CFG_ID,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, SHS_ATTR_DIAG_UPTIME_S, &uptime, false);
    esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT, SHS_CL_CFG_ID,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, SHS_ATTR_DIAG_FREE_HEAP, &heap, false);
    esp_zb_lock_release();
}

/* ============================================================================
 * FORCE UPDATE FUNCTIONS
 * ============================================================================ */

#define SHS_FORCE_UPDATE_LOCK_TIMEOUT_MS  500

/* Endpoint lookup arrays for zones */
static const uint8_t zone_occ_eps[SHS_NUM_ZONES] = {
    SHS_EP_LD2450_ZONE1, SHS_EP_LD2450_ZONE2, SHS_EP_LD2450_ZONE3,
    SHS_EP_LD2450_ZONE4, SHS_EP_LD2450_ZONE5
};
static const uint8_t zone_target_eps[SHS_NUM_ZONES] = {
    SHS_EP_ZONE1_TARGETS, SHS_EP_ZONE2_TARGETS, SHS_EP_ZONE3_TARGETS,
    SHS_EP_ZONE4_TARGETS, SHS_EP_ZONE5_TARGETS
};

void shs_ld2450_force_update(void) {
    ESP_LOGI(TAG, "Forcing LD2450 state update to Zigbee");
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* EP3: LD2450 occupancy */
    if (esp_zb_lock_acquire(pdMS_TO_TICKS(SHS_FORCE_UPDATE_LOCK_TIMEOUT_MS))) {
        uint8_t occ_val = s_state->ld2450_occupancy ? 1 : 0;
        esp_zb_zcl_set_attribute_val(SHS_EP_LD2450_OCC,
            ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID, &occ_val, true);
        esp_zb_lock_release();
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* EP4: Target count */
    if (esp_zb_lock_acquire(pdMS_TO_TICKS(SHS_FORCE_UPDATE_LOCK_TIMEOUT_MS))) {
        float count_val = (float)s_state->ld2450_target_count;
        esp_zb_zcl_set_attribute_val(SHS_EP_LD2450_TARGET_COUNT,
            SHS_CLUSTER_ANALOG_INPUT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            SHS_ATTR_PRESENT_VALUE, &count_val, true);
        esp_zb_lock_release();
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Zone occupancy endpoints (loop) */
    for (int z = 0; z < SHS_NUM_ZONES; z++) {
        if (esp_zb_lock_acquire(pdMS_TO_TICKS(SHS_FORCE_UPDATE_LOCK_TIMEOUT_MS))) {
            uint8_t val = s_state->zones[z].occupied ? 1 : 0;
            esp_zb_zcl_set_attribute_val(zone_occ_eps[z],
                SHS_CLUSTER_BINARY_INPUT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                SHS_ATTR_PRESENT_VALUE_BINARY, &val, true);
            esp_zb_lock_release();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* Zone target count endpoints (loop) */
    for (int z = 0; z < SHS_NUM_ZONES; z++) {
        if (esp_zb_lock_acquire(pdMS_TO_TICKS(SHS_FORCE_UPDATE_LOCK_TIMEOUT_MS))) {
            float targets_val = (float)s_state->zones[z].targets;
            esp_zb_zcl_set_attribute_val(zone_target_eps[z],
                SHS_CLUSTER_ANALOG_INPUT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                SHS_ATTR_PRESENT_VALUE, &targets_val, true);
            esp_zb_lock_release();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_state->last_successful_tx = (uint32_t)(esp_timer_get_time() / 1000);
    ESP_LOGI(TAG, "LD2450 force update complete");
}

void shs_ld2410c_force_update(void) {
    ESP_LOGI(TAG, "Force update LD2410C: moving=%d static=%d occ=%d",
             s_state->moving_state, s_state->static_state, s_state->occupancy_state);

    /* EP2: LD2410C occupancy */
    if (esp_zb_lock_acquire(pdMS_TO_TICKS(SHS_FORCE_UPDATE_LOCK_TIMEOUT_MS))) {
        uint8_t occ_val = s_state->occupancy_state ? 1 : 0;
        esp_zb_zcl_set_attribute_val(SHS_EP_OCC,
            ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID, &occ_val, true);
        esp_zb_lock_release();
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* EP17: Moving (genBinaryInput) */
    if (esp_zb_lock_acquire(pdMS_TO_TICKS(SHS_FORCE_UPDATE_LOCK_TIMEOUT_MS))) {
        uint8_t moving_val = s_state->moving_state ? 1 : 0;
        esp_zb_zcl_set_attribute_val(SHS_EP_LD2410C_MOVING,
            SHS_CLUSTER_BINARY_INPUT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            SHS_ATTR_PRESENT_VALUE_BINARY, &moving_val, true);
        esp_zb_lock_release();
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* EP18: Static (genBinaryInput) */
    if (esp_zb_lock_acquire(pdMS_TO_TICKS(SHS_FORCE_UPDATE_LOCK_TIMEOUT_MS))) {
        uint8_t static_val = s_state->static_state ? 1 : 0;
        esp_zb_zcl_set_attribute_val(SHS_EP_LD2410C_STATIC,
            SHS_CLUSTER_BINARY_INPUT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            SHS_ATTR_PRESENT_VALUE_BINARY, &static_val, true);
        esp_zb_lock_release();
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Mfr-specific attrs for backwards compatibility */
    if (esp_zb_lock_acquire(pdMS_TO_TICKS(SHS_FORCE_UPDATE_LOCK_TIMEOUT_MS))) {
        uint8_t moving_val = s_state->moving_state ? 1 : 0;
        uint8_t static_val = s_state->static_state ? 1 : 0;
        esp_zb_zcl_set_attribute_val(SHS_EP_OCC, ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, SHS_ATTR_OCC_MOVING_TARGET, &moving_val, true);
        esp_zb_zcl_set_attribute_val(SHS_EP_OCC, ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, SHS_ATTR_OCC_STATIC_TARGET, &static_val, true);
        esp_zb_lock_release();
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* EP1: Config cluster attributes */
    if (esp_zb_lock_acquire(pdMS_TO_TICKS(SHS_FORCE_UPDATE_LOCK_TIMEOUT_MS))) {
        esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT, SHS_CL_CFG_ID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            SHS_ATTR_MOVEMENT_COOLDOWN, &s_state->movement_cooldown_sec, true);
        esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT, SHS_CL_CFG_ID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            SHS_ATTR_OCC_CLEAR_COOLDOWN, &s_state->occupancy_clear_sec, true);
        esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT, SHS_CL_CFG_ID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            SHS_ATTR_MOVING_SENS_0_10, &s_state->sens_mv_0_10, true);
        esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT, SHS_CL_CFG_ID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            SHS_ATTR_STATIC_SENS_0_10, &s_state->sens_st_0_10, true);
        esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT, SHS_CL_CFG_ID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            SHS_ATTR_MOVING_MAX_GATE, &s_state->moving_max_gate, true);
        esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT, SHS_CL_CFG_ID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            SHS_ATTR_STATIC_MAX_GATE, &s_state->static_max_gate, true);
        esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT, SHS_CL_CFG_ID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            SHS_ATTR_POSITION_REPORTING, (void *)&s_state->position_reporting, true);
        esp_zb_lock_release();
    }

    s_state->last_successful_tx = (uint32_t)(esp_timer_get_time() / 1000);
    ESP_LOGI(TAG, "LD2410C/Config force update complete");
}

/* ============================================================================
 * BDB COMMISSIONING & METADATA
 * ============================================================================ */

static void shs_bdb_start_top_level_commissioning_cb(uint8_t mode_mask) {
    if (esp_zb_bdb_start_top_level_commissioning(mode_mask) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start Zigbee commissioning");
    }
}

void shs_zb_schedule_rejoin(void) {
    esp_zb_scheduler_alarm((esp_zb_callback_t)shs_bdb_start_top_level_commissioning_cb,
                           ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
}

static void shs_basic_publish_metadata_ep1(void) {
    if (!s_state->zb_ready) return;
    const char *date_code = SHS_BASIC_DATE_CODE;
    const char *sw_build = SHS_BASIC_SW_BUILD_ID;
    uint8_t power_src = 0x01;
    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(SHS_FORCE_UPDATE_LOCK_TIMEOUT_MS))) {
        ESP_LOGW(TAG, "Failed to acquire lock for metadata publish");
        return;
    }
    esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT, ESP_ZB_ZCL_CLUSTER_ID_BASIC,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_BASIC_POWER_SOURCE_ID, &power_src, false);
    esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT, ESP_ZB_ZCL_CLUSTER_ID_BASIC,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_BASIC_DATE_CODE_ID, (void *)date_code, false);
    esp_zb_zcl_set_attribute_val(SHS_EP_LIGHT, ESP_ZB_ZCL_CLUSTER_ID_BASIC,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID, (void *)sw_build, false);
    esp_zb_lock_release();
}

/* ============================================================================
 * ZIGBEE SIGNAL HANDLER
 * ============================================================================ */

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            s_state->zb_ready = true;
            s_state->last_successful_tx = (uint32_t)(esp_timer_get_time() / 1000);
            shs_basic_publish_metadata_ep1();
            shs_zb_set_ou_delay_ep2(s_state->occupancy_clear_sec);
            shs_zb_set_bool_attr(SHS_EP_OCC, ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
                                SHS_ATTR_OCC_MOVING_TARGET, s_state->moving_state);
            shs_zb_set_bool_attr(SHS_EP_OCC, ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
                                SHS_ATTR_OCC_STATIC_TARGET, s_state->static_state);
            shs_zb_set_occ_bitmap(SHS_EP_OCC, s_state->occupancy_state);
            ESP_LOGI(TAG, "Device started up in%s factory-reset mode",
                     esp_zb_bdb_is_factory_new() ? "" : " non");
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Start network steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device rebooted - already joined network");
                s_state->zb_connected = true;
                shs_ld2410c_force_update();
                shs_ld2450_force_update();
            }
        } else {
            ESP_LOGW(TAG, "Failed to initialize Zigbee stack (%s)", esp_err_to_name(err_status));
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(TAG, "Joined network (PAN:0x%04hx, Ch:%d)",
                     esp_zb_get_pan_id(), esp_zb_get_current_channel());
            s_state->zb_connected = true;
            s_state->zb_rejoin_pending = false;
            s_state->last_successful_tx = (uint32_t)(esp_timer_get_time() / 1000);
            shs_ld2410c_force_update();
            shs_ld2450_force_update();
        } else {
            ESP_LOGW(TAG, "Network steering not successful (%s)", esp_err_to_name(err_status));
            s_state->zb_connected = false;
            esp_zb_scheduler_alarm((esp_zb_callback_t)shs_bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;

    case ESP_ZB_ZDO_DEVICE_UNAVAILABLE:
        s_state->zb_connected = false;
        if (!s_state->zb_rejoin_pending) {
            ESP_LOGW(TAG, "Device unavailable signal (0x3c) - scheduling rejoin");
            s_state->zb_rejoin_pending = true;
            esp_zb_scheduler_alarm((esp_zb_callback_t)shs_bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING, 5000);
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_LEAVE:
        s_state->zb_connected = false;
        if (!s_state->zb_rejoin_pending) {
            ESP_LOGW(TAG, "Leave signal received - scheduling rejoin");
            s_state->zb_rejoin_pending = true;
            esp_zb_scheduler_alarm((esp_zb_callback_t)shs_bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING, 2000);
        }
        break;

    case ESP_ZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT:
        s_state->zb_connected = false;
        if (!s_state->zb_rejoin_pending) {
            ESP_LOGW(TAG, "No active network links - scheduling rejoin");
            s_state->zb_rejoin_pending = true;
            esp_zb_scheduler_alarm((esp_zb_callback_t)shs_bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING, 3000);
        }
        break;

    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s",
                 esp_zb_zdo_signal_to_string(sig_type), sig_type, esp_err_to_name(err_status));
        break;
    }
}

/* ============================================================================
 * ENDPOINT FACTORY HELPERS
 * ============================================================================ */

static void add_binary_ep(esp_zb_ep_list_t *list, uint8_t ep_num,
                           bool init_val, const zcl_basic_manufacturer_info_t *info) {
    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(NULL);
    esp_zb_cluster_list_add_basic_cluster(cl, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_binary_input_cluster_cfg_t binary_cfg = {
        .out_of_service = false,
        .present_value = init_val ? 1 : 0,
        .status_flags = 0,
    };
    esp_zb_attribute_list_t *binary_input = esp_zb_binary_input_cluster_create(&binary_cfg);
    esp_zb_cluster_list_add_binary_input_cluster(cl, binary_input, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = ep_num,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0
    };
    esp_zb_ep_list_add_ep(list, cl, ep_cfg);
    esp_zcl_utility_add_ep_basic_manufacturer_info(list, ep_num, info);
}

static void add_analog_ep(esp_zb_ep_list_t *list, uint8_t ep_num,
                           float init_val, const zcl_basic_manufacturer_info_t *info) {
    esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(NULL);
    esp_zb_cluster_list_add_basic_cluster(cl, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_analog_input_cluster_cfg_t analog_cfg = {
        .out_of_service = false,
        .present_value = init_val,
        .status_flags = 0,
    };
    esp_zb_attribute_list_t *analog_input = esp_zb_analog_input_cluster_create(&analog_cfg);
    esp_zb_cluster_list_add_analog_input_cluster(cl, analog_input, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = ep_num,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0
    };
    esp_zb_ep_list_add_ep(list, cl, ep_cfg);
    esp_zcl_utility_add_ep_basic_manufacturer_info(list, ep_num, info);
}

/* ============================================================================
 * ZIGBEE TASK — ENDPOINT & CLUSTER CREATION
 * ============================================================================ */

void shs_zigbee_task(void *pvParameters) {
    esp_zb_cfg_t zb_nwk_cfg = SHS_ZR_CONFIG();
    esp_zb_init(&zb_nwk_cfg);

    zcl_basic_manufacturer_info_t info = {
        .manufacturer_name = SHS_MANUFACTURER_NAME,
        .model_identifier = SHS_MODEL_IDENTIFIER,
    };

    esp_zb_ep_list_t *dev_ep_list = esp_zb_ep_list_create();

    /* ========== EP1: Light + Config Cluster ========== */
    {
        esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();

        esp_zb_on_off_cluster_cfg_t on_off_cfg = {.on_off = ESP_ZB_ZCL_ON_OFF_ON_OFF_DEFAULT_VALUE};
        esp_zb_attribute_list_t *onoff = esp_zb_on_off_cluster_create(&on_off_cfg);

        esp_zb_cluster_list_add_basic_cluster(cl, esp_zb_basic_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
        esp_zb_cluster_list_add_identify_cluster(cl, esp_zb_identify_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
        esp_zb_cluster_list_add_on_off_cluster(cl, onoff, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

        /* Custom Config Cluster (0xFDCD) */
        esp_zb_attribute_list_t *cfg_cl = esp_zb_zcl_attr_list_create(SHS_CL_CFG_ID);

        /* Original attributes */
        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_MOVEMENT_COOLDOWN,
            ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s_state->movement_cooldown_sec);
        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_OCC_CLEAR_COOLDOWN,
            ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s_state->occupancy_clear_sec);
        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_MOVING_SENS_0_10,
            ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s_state->sens_mv_0_10);
        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_STATIC_SENS_0_10,
            ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s_state->sens_st_0_10);
        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_MOVING_MAX_GATE,
            ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s_state->moving_max_gate);
        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_STATIC_MAX_GATE,
            ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s_state->static_max_gate);
        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_POSITION_REPORTING,
            ESP_ZB_ZCL_ATTR_TYPE_BOOL, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, (void *)&s_state->position_reporting);
        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_MIN_MOVING_ENERGY,
            ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s_state->min_moving_energy);
        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_MIN_STATIC_ENERGY,
            ESP_ZB_ZCL_ATTR_TYPE_U16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s_state->min_static_energy);

        /* Zone type */
        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_ZONE_TYPE_CFG,
            ESP_ZB_ZCL_ATTR_TYPE_U8, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s_state->zone_type);

        /* Zone attributes — loop over zones */
        for (int z = 0; z < SHS_NUM_ZONES; z++) {
            uint16_t base = (z == 0) ? 0x0021 : (0x0020 + z * 0x10);
            esp_zb_custom_cluster_add_custom_attr(cfg_cl, base,
                ESP_ZB_ZCL_ATTR_TYPE_BOOL, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s_state->zones[z].enabled);
            esp_zb_custom_cluster_add_custom_attr(cfg_cl, base + 1,
                ESP_ZB_ZCL_ATTR_TYPE_S16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s_state->zones[z].x1);
            esp_zb_custom_cluster_add_custom_attr(cfg_cl, base + 2,
                ESP_ZB_ZCL_ATTR_TYPE_S16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s_state->zones[z].y1);
            esp_zb_custom_cluster_add_custom_attr(cfg_cl, base + 3,
                ESP_ZB_ZCL_ATTR_TYPE_S16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s_state->zones[z].x2);
            esp_zb_custom_cluster_add_custom_attr(cfg_cl, base + 4,
                ESP_ZB_ZCL_ATTR_TYPE_S16, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s_state->zones[z].y2);
            esp_zb_custom_cluster_add_custom_attr(cfg_cl, base + 5,
                ESP_ZB_ZCL_ATTR_TYPE_U8, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, &s_state->zones[z].targets);
            esp_zb_custom_cluster_add_custom_attr(cfg_cl, base + 6,
                ESP_ZB_ZCL_ATTR_TYPE_U8, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &s_state->zones[z].type);
        }

        /* Health attributes */
        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_LD2410C_CONNECTED,
            ESP_ZB_ZCL_ATTR_TYPE_BOOL, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, &s_state->health_init_false);
        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_LD2450_CONNECTED,
            ESP_ZB_ZCL_ATTR_TYPE_BOOL, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, &s_state->health_init_false);

        /* Diagnostic counters */
        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_DIAG_LOCK_SUCCESS,
            ESP_ZB_ZCL_ATTR_TYPE_U32, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, &s_state->diag_init_zero);
        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_DIAG_LOCK_FAIL,
            ESP_ZB_ZCL_ATTR_TYPE_U32, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, &s_state->diag_init_zero);
        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_DIAG_LOCK_CONSEC_FAIL,
            ESP_ZB_ZCL_ATTR_TYPE_U32, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, &s_state->diag_init_zero);
        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_DIAG_TX_SUCCESS,
            ESP_ZB_ZCL_ATTR_TYPE_U32, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, &s_state->diag_init_zero);
        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_DIAG_TX_FAIL,
            ESP_ZB_ZCL_ATTR_TYPE_U32, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, &s_state->diag_init_zero);
        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_DIAG_UPTIME_S,
            ESP_ZB_ZCL_ATTR_TYPE_U32, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, &s_state->diag_init_zero);
        esp_zb_custom_cluster_add_custom_attr(cfg_cl, SHS_ATTR_DIAG_FREE_HEAP,
            ESP_ZB_ZCL_ATTR_TYPE_U32, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, &s_state->diag_init_zero);

        esp_zb_cluster_list_add_custom_cluster(cl, cfg_cl, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

        esp_zb_endpoint_config_t ep_cfg = {
            .endpoint = SHS_EP_LIGHT,
            .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
            .app_device_id = ESP_ZB_HA_ON_OFF_LIGHT_DEVICE_ID,
            .app_device_version = 0
        };
        esp_zb_ep_list_add_ep(dev_ep_list, cl, ep_cfg);
        esp_zcl_utility_add_ep_basic_manufacturer_info(dev_ep_list, SHS_EP_LIGHT, &info);
    }

    /* ========== EP2: Occupancy + Distance + Gates ========== */
    {
        esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();
        esp_zb_attribute_list_t *occ = esp_zb_occupancy_sensing_cluster_create(NULL);
        esp_zb_custom_cluster_add_custom_attr(occ, SHS_ATTR_OCC_MOVING_TARGET,
            ESP_ZB_ZCL_ATTR_TYPE_BOOL, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
            &s_state->moving_state);
        esp_zb_custom_cluster_add_custom_attr(occ, SHS_ATTR_OCC_STATIC_TARGET,
            ESP_ZB_ZCL_ATTR_TYPE_BOOL, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
            &s_state->static_state);
        esp_zb_cluster_list_add_occupancy_sensing_cluster(cl, occ, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
        esp_zb_endpoint_config_t ep_cfg = {
            .endpoint = SHS_EP_OCC,
            .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
            .app_device_id = 0x0107,
            .app_device_version = 0
        };
        esp_zb_ep_list_add_ep(dev_ep_list, cl, ep_cfg);
    }

    /* ========== EP3: LD2450 Occupancy (msOccupancySensing) ========== */
    {
        esp_zb_cluster_list_t *cl = esp_zb_zcl_cluster_list_create();
        esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(NULL);
        esp_zb_cluster_list_add_basic_cluster(cl, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
        esp_zb_occupancy_sensing_cluster_cfg_t occ_cfg = {
            .occupancy = s_state->ld2450_occupancy ? 0x01 : 0x00,
        };
        esp_zb_attribute_list_t *occ_cluster = esp_zb_occupancy_sensing_cluster_create(&occ_cfg);
        esp_zb_cluster_list_add_occupancy_sensing_cluster(cl, occ_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
        esp_zb_endpoint_config_t ep_cfg = {
            .endpoint = SHS_EP_LD2450_OCC,
            .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
            .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
            .app_device_version = 0
        };
        esp_zb_ep_list_add_ep(dev_ep_list, cl, ep_cfg);
        esp_zcl_utility_add_ep_basic_manufacturer_info(dev_ep_list, SHS_EP_LD2450_OCC, &info);
    }

    /* ========== EP4: LD2450 Target Count (genAnalogInput) ========== */
    add_analog_ep(dev_ep_list, SHS_EP_LD2450_TARGET_COUNT, (float)s_state->ld2450_target_count, &info);

    /* ========== EP5-7: Zone 1-3 Occupancy (genBinaryInput) ========== */
    for (int z = 0; z < 3; z++) {
        add_binary_ep(dev_ep_list, zone_occ_eps[z], s_state->zones[z].occupied, &info);
    }

    /* ========== EP22-23: Zone 4-5 Occupancy (genBinaryInput) ========== */
    for (int z = 3; z < SHS_NUM_ZONES; z++) {
        add_binary_ep(dev_ep_list, zone_occ_eps[z], s_state->zones[z].occupied, &info);
    }

    /* ========== EP8-16: LD2450 Position Data (genAnalogInput) ========== */
    for (int i = 0; i < 3; i++) {
        uint8_t ep_base = SHS_EP_LD2450_T1_X + (i * 3);
        for (int j = 0; j < 3; j++) {
            add_analog_ep(dev_ep_list, ep_base + j, 0.0f, &info);
        }
    }

    /* ========== EP17: LD2410C Moving (genBinaryInput) ========== */
    add_binary_ep(dev_ep_list, SHS_EP_LD2410C_MOVING, s_state->moving_state, &info);

    /* ========== EP18: LD2410C Static (genBinaryInput) ========== */
    add_binary_ep(dev_ep_list, SHS_EP_LD2410C_STATIC, s_state->static_state, &info);

    /* ========== EP19-21: Zone 1-3 Target Count (genAnalogInput) ========== */
    for (int z = 0; z < 3; z++) {
        add_analog_ep(dev_ep_list, zone_target_eps[z], (float)s_state->zones[z].targets, &info);
    }

    /* ========== EP24-25: Zone 4-5 Target Count (genAnalogInput) ========== */
    for (int z = 3; z < SHS_NUM_ZONES; z++) {
        add_analog_ep(dev_ep_list, zone_target_eps[z], (float)s_state->zones[z].targets, &info);
    }

    /* Register device */
    esp_zb_device_register(dev_ep_list);
    esp_zb_core_action_handler_register(shs_zb_action_handler);
    esp_zb_set_primary_network_channel_set(SHS_PRIMARY_CHANNEL_MASK);
    esp_zb_secur_network_min_join_lqi_set(0);

    ESP_LOGI(TAG, "Zigbee device registered with 25 endpoints");
    esp_zb_start(false);
    esp_zb_stack_main_loop();
}
