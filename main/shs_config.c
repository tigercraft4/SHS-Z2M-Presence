/*
 * SHS01 Presence Sensor — Configuration & NVS Module
 *
 * NVS helpers, zone config save/load/debounce, attribute handler,
 * save_worker task. Zone handler uses computed offsets (CS-03).
 */

#include <string.h>
#include "shs_config.h"
#include "shs_state.h"
#include "shs_zigbee.h"
#include "shs01.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "ld2410_enhanced.h"
#include "ld2450.h"
#include "light_driver.h"

static const char *TAG = "SHS_CFG";
static shs_state_t *s_state = NULL;

/* NVS keys */
#define SHS_NVS_NAMESPACE       "cfg"
#define SHS_NVS_KEY_MV_CD       "mv_cd"
#define SHS_NVS_KEY_OCC_CD      "occ_cd"
#define SHS_NVS_KEY_MV_SENS     "mv_sens"
#define SHS_NVS_KEY_ST_SENS     "st_sens"
#define SHS_NVS_KEY_MV_GATE     "mv_gate"
#define SHS_NVS_KEY_ST_GATE     "st_gate"
#define SHS_NVS_KEY_ZONE_TYPE   "z_type"
#define SHS_NVS_KEY_ZONE_BLOB   "zone_blob"

/* Legacy per-key zone NVS keys */
#define SHS_NVS_KEY_Z1_EN   "z1_en"
#define SHS_NVS_KEY_Z1_X1   "z1_x1"
#define SHS_NVS_KEY_Z1_Y1   "z1_y1"
#define SHS_NVS_KEY_Z1_X2   "z1_x2"
#define SHS_NVS_KEY_Z1_Y2   "z1_y2"
#define SHS_NVS_KEY_Z1_TYPE "z1_type"
#define SHS_NVS_KEY_Z2_EN   "z2_en"
#define SHS_NVS_KEY_Z2_X1   "z2_x1"
#define SHS_NVS_KEY_Z2_Y1   "z2_y1"
#define SHS_NVS_KEY_Z2_X2   "z2_x2"
#define SHS_NVS_KEY_Z2_Y2   "z2_y2"
#define SHS_NVS_KEY_Z2_TYPE "z2_type"
#define SHS_NVS_KEY_Z3_EN   "z3_en"
#define SHS_NVS_KEY_Z3_X1   "z3_x1"
#define SHS_NVS_KEY_Z3_Y1   "z3_y1"
#define SHS_NVS_KEY_Z3_X2   "z3_x2"
#define SHS_NVS_KEY_Z3_Y2   "z3_y2"
#define SHS_NVS_KEY_Z3_TYPE "z3_type"
#define SHS_NVS_KEY_Z4_EN   "z4_en"
#define SHS_NVS_KEY_Z4_X1   "z4_x1"
#define SHS_NVS_KEY_Z4_Y1   "z4_y1"
#define SHS_NVS_KEY_Z4_X2   "z4_x2"
#define SHS_NVS_KEY_Z4_Y2   "z4_y2"
#define SHS_NVS_KEY_Z4_TYPE "z4_type"
#define SHS_NVS_KEY_Z5_EN   "z5_en"
#define SHS_NVS_KEY_Z5_X1   "z5_x1"
#define SHS_NVS_KEY_Z5_Y1   "z5_y1"
#define SHS_NVS_KEY_Z5_X2   "z5_x2"
#define SHS_NVS_KEY_Z5_Y2   "z5_y2"
#define SHS_NVS_KEY_Z5_TYPE "z5_type"

/* Zone config debounce */
#define SHS_ZONE_CFG_DEBOUNCE_MS  500

/* ============================================================================
 * INIT
 * ============================================================================ */

void shs_config_init(shs_state_t *state) {
    s_state = state;
}

/* ============================================================================
 * NVS HELPERS
 * ============================================================================ */

static inline bool shs_time_reached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

static void shs_cfg_save_u16(const char *key, uint16_t v) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(SHS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGW(TAG, "NVS save_u16 open failed: %s", esp_err_to_name(err)); return; }
    nvs_set_u16(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static void shs_cfg_save_u8(const char *key, uint8_t v) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(SHS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGW(TAG, "NVS save_u8 open failed: %s", esp_err_to_name(err)); return; }
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static inline void shs_save_enqueue(shs_save_evt_t t, uint16_t v) {
    if (!s_state->save_q) return;
    shs_save_msg_t m = {.type = t, .u16 = v};
    (void)xQueueSend(s_state->save_q, &m, 0);
}

static void shs_cfg_sync_sens_proxies(void) {
    uint8_t mv_thresh = (s_state->moving_sens_0_100 > 100) ? 100 : s_state->moving_sens_0_100;
    uint8_t st_thresh = (s_state->static_sens_0_100 > 100) ? 100 : s_state->static_sens_0_100;
    s_state->sens_mv_0_10 = (uint16_t)(10 - (mv_thresh / 10));
    s_state->sens_st_0_10 = (uint16_t)(10 - (st_thresh / 10));
}

/* ============================================================================
 * NVS LOAD/SAVE
 * ============================================================================ */

void shs_cfg_load_from_nvs(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(SHS_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS open RO failed (%s), using defaults", esp_err_to_name(err));
        return;
    }

    uint16_t u16tmp; uint8_t u8tmp;

    if (nvs_get_u16(h, SHS_NVS_KEY_MV_CD, &u16tmp) == ESP_OK)
        s_state->movement_cooldown_sec = (u16tmp > SHS_COOLDOWN_MAX_SEC) ? SHS_COOLDOWN_MAX_SEC : u16tmp;
    if (nvs_get_u16(h, SHS_NVS_KEY_OCC_CD, &u16tmp) == ESP_OK)
        s_state->occupancy_clear_sec = u16tmp;
    if (nvs_get_u8(h, SHS_NVS_KEY_MV_SENS, &u8tmp) == ESP_OK)
        s_state->moving_sens_0_100 = (u8tmp > 100) ? 100 : u8tmp;
    if (nvs_get_u8(h, SHS_NVS_KEY_ST_SENS, &u8tmp) == ESP_OK)
        s_state->static_sens_0_100 = (u8tmp > 100) ? 100 : u8tmp;
    if (nvs_get_u8(h, SHS_NVS_KEY_MV_GATE, &u8tmp) == ESP_OK) {
        if (u8tmp < 1) u8tmp = 8;
        else if (u8tmp > 8) u8tmp = 8;
        s_state->moving_max_gate = u8tmp;
    }
    if (nvs_get_u8(h, SHS_NVS_KEY_ST_GATE, &u8tmp) == ESP_OK) {
        if (u8tmp < 2) u8tmp = 2; else if (u8tmp > 8) u8tmp = 8;
        s_state->static_max_gate = u8tmp;
    }

    nvs_close(h);
    shs_cfg_sync_sens_proxies();

    ESP_LOGI(TAG, "NVS loaded: mv_cd=%us, occ_cd=%us, mv_sens=%u, st_sens=%u, mv_gate=%u, st_gate=%u",
             (unsigned)s_state->movement_cooldown_sec, (unsigned)s_state->occupancy_clear_sec,
             (unsigned)s_state->moving_sens_0_100, (unsigned)s_state->static_sens_0_100,
             (unsigned)s_state->moving_max_gate, (unsigned)s_state->static_max_gate);
}

/* ============================================================================
 * ZONE NVS (BLOB FORMAT)
 * ============================================================================ */

static void shs_zone_cfg_save_to_nvs(void) {
    shs_zone_cfg_blob_t blob;
    blob.zone_type = s_state->zone_type;

    for (int i = 0; i < SHS_NUM_ZONES; i++) {
        blob.zones[i].enabled = s_state->zones[i].enabled ? 1 : 0;
        blob.zones[i].x1 = s_state->zones[i].x1;
        blob.zones[i].y1 = s_state->zones[i].y1;
        blob.zones[i].x2 = s_state->zones[i].x2;
        blob.zones[i].y2 = s_state->zones[i].y2;
        blob.zones[i].type = s_state->zones[i].type;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open("shs_cfg", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed for zone save: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(h, SHS_NVS_KEY_ZONE_BLOB, &blob, sizeof(blob));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS zone blob write failed: %s", esp_err_to_name(err));
        nvs_close(h);
        return;
    }

    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS zone blob commit failed: %s", esp_err_to_name(err));
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Zone config saved to NVS (atomic blob, %u bytes)", (unsigned)sizeof(blob));
}

void shs_zone_cfg_load_from_nvs(void) {
    nvs_handle_t h;
    if (nvs_open("shs_cfg", NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No zone config in NVS, using defaults");
        return;
    }

    /* Try blob format first */
    shs_zone_cfg_blob_t blob;
    size_t blob_len = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, SHS_NVS_KEY_ZONE_BLOB, &blob, &blob_len);
    if (err == ESP_OK && blob_len == sizeof(blob)) {
        s_state->zone_type = blob.zone_type;
        for (int i = 0; i < SHS_NUM_ZONES; i++) {
            s_state->zones[i].enabled = blob.zones[i].enabled ? true : false;
            s_state->zones[i].x1 = blob.zones[i].x1;
            s_state->zones[i].y1 = blob.zones[i].y1;
            s_state->zones[i].x2 = blob.zones[i].x2;
            s_state->zones[i].y2 = blob.zones[i].y2;
            s_state->zones[i].type = blob.zones[i].type;
        }
        nvs_close(h);
        ESP_LOGI(TAG, "Zone config loaded from NVS (blob format)");
        return;
    }

    /* Fall back to legacy per-key format */
    ESP_LOGI(TAG, "No zone blob in NVS, trying legacy keys...");

    uint8_t u8tmp;
    int16_t i16tmp;

    if (nvs_get_u8(h, SHS_NVS_KEY_ZONE_TYPE, &u8tmp) == ESP_OK)
        s_state->zone_type = u8tmp;

    /* Legacy zone keys — structured arrays for the loop */
    static const char *en_keys[]   = {SHS_NVS_KEY_Z1_EN,   SHS_NVS_KEY_Z2_EN,   SHS_NVS_KEY_Z3_EN,   SHS_NVS_KEY_Z4_EN,   SHS_NVS_KEY_Z5_EN};
    static const char *x1_keys[]   = {SHS_NVS_KEY_Z1_X1,   SHS_NVS_KEY_Z2_X1,   SHS_NVS_KEY_Z3_X1,   SHS_NVS_KEY_Z4_X1,   SHS_NVS_KEY_Z5_X1};
    static const char *y1_keys[]   = {SHS_NVS_KEY_Z1_Y1,   SHS_NVS_KEY_Z2_Y1,   SHS_NVS_KEY_Z3_Y1,   SHS_NVS_KEY_Z4_Y1,   SHS_NVS_KEY_Z5_Y1};
    static const char *x2_keys[]   = {SHS_NVS_KEY_Z1_X2,   SHS_NVS_KEY_Z2_X2,   SHS_NVS_KEY_Z3_X2,   SHS_NVS_KEY_Z4_X2,   SHS_NVS_KEY_Z5_X2};
    static const char *y2_keys[]   = {SHS_NVS_KEY_Z1_Y2,   SHS_NVS_KEY_Z2_Y2,   SHS_NVS_KEY_Z3_Y2,   SHS_NVS_KEY_Z4_Y2,   SHS_NVS_KEY_Z5_Y2};
    static const char *type_keys[] = {SHS_NVS_KEY_Z1_TYPE, SHS_NVS_KEY_Z2_TYPE, SHS_NVS_KEY_Z3_TYPE, SHS_NVS_KEY_Z4_TYPE, SHS_NVS_KEY_Z5_TYPE};

    for (int i = 0; i < SHS_NUM_ZONES; i++) {
        if (nvs_get_u8(h, en_keys[i], &u8tmp) == ESP_OK)
            s_state->zones[i].enabled = (u8tmp != 0);
        if (nvs_get_i16(h, x1_keys[i], &i16tmp) == ESP_OK)
            s_state->zones[i].x1 = i16tmp;
        if (nvs_get_i16(h, y1_keys[i], &i16tmp) == ESP_OK)
            s_state->zones[i].y1 = i16tmp;
        if (nvs_get_i16(h, x2_keys[i], &i16tmp) == ESP_OK)
            s_state->zones[i].x2 = i16tmp;
        if (nvs_get_i16(h, y2_keys[i], &i16tmp) == ESP_OK)
            s_state->zones[i].y2 = i16tmp;
        if (nvs_get_u8(h, type_keys[i], &u8tmp) == ESP_OK)
            s_state->zones[i].type = u8tmp;
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Zone config loaded from NVS (legacy format, will migrate to blob)");
    shs_zone_cfg_save_to_nvs();
}

/* ============================================================================
 * ZONE CONFIG DEBOUNCE & APPLY
 * ============================================================================ */

static void shs_zone_cfg_apply_to_sensor(void);

void shs_zone_cfg_schedule_apply(void) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_state->zone_cfg_pending_until = now_ms + SHS_ZONE_CFG_DEBOUNCE_MS;
    s_state->zone_cfg_pending = true;
    s_state->zone_cfg_save_needed = true;
    ESP_LOGD(TAG, "Zone config scheduled (debounce %dms)", SHS_ZONE_CFG_DEBOUNCE_MS);
}

void shs_zone_cfg_check_pending(void) {
    if (!s_state->zone_cfg_pending) return;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms >= s_state->zone_cfg_pending_until) {
        s_state->zone_cfg_pending = false;
        shs_zone_cfg_apply_to_sensor();
    }
}

static void shs_zone_cfg_apply_to_sensor(void) {
    /* Snapshot zone config under mutex */
    uint8_t zt;
    shs_zone_t local_zones[SHS_NUM_ZONES];

    if (xSemaphoreTake(s_state->zone_config_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        zt = s_state->zone_type;
        for (int i = 0; i < SHS_NUM_ZONES; i++) {
            local_zones[i] = s_state->zones[i];
        }
        xSemaphoreGive(s_state->zone_config_mutex);
    } else {
        ESP_LOGW(TAG, "Zone config mutex timeout - skipping apply");
        return;
    }

    ESP_LOGI(TAG, "Applying zone config to LD2450: type=%d", zt);
    ld2450_set_zone_type((ld2450_zone_type_t)zt);

    for (int i = 0; i < SHS_NUM_ZONES; i++) {
        if (local_zones[i].enabled) {
            ld2450_set_zone(i, local_zones[i].x1, local_zones[i].y1,
                           local_zones[i].x2, local_zones[i].y2);
            ESP_LOGI(TAG, "Zone %d: (%d,%d) to (%d,%d) type=%d",
                     i + 1, local_zones[i].x1, local_zones[i].y1,
                     local_zones[i].x2, local_zones[i].y2, local_zones[i].type);
        } else {
            ld2450_clear_zone(i);
        }
    }

    ld2450_apply_zones();

    if (s_state->zone_cfg_save_needed) {
        shs_save_enqueue(SHS_SAVE_ZONE_CONFIG, 0);
        s_state->zone_cfg_save_needed = false;
    }
}

/* ============================================================================
 * LD2410 CONFIGURATION
 * ============================================================================ */

void shs_apply_ld2410_config(void) {
    ld2410_set_max_gate_timeout(
        (uint8_t)s_state->moving_max_gate,
        (uint8_t)s_state->static_max_gate,
        s_state->occupancy_clear_sec
    );
    ld2410_set_all_sensitivity(s_state->moving_sens_0_100, s_state->static_sens_0_100);
    ld2410_set_moving_cooldown(s_state->movement_cooldown_sec);
    ld2410_set_occupancy_delay(s_state->occupancy_clear_sec);

    ld2410_read_firmware_version();
    const ld2410_state_t *state = ld2410_get_state();
    if (state->firmware.valid) {
        snprintf(s_state->firmware_version, sizeof(s_state->firmware_version),
                 "V%d.%02d", state->firmware.major, state->firmware.minor);
    }
    ld2410_read_config();
    ESP_LOGI(TAG, "LD2410 configuration applied");
}

/* ============================================================================
 * ZONE ATTRIBUTE HANDLER — COMPUTED OFFSETS (CS-03)
 * ============================================================================ */

static esp_err_t handle_zone_attr(uint16_t attr_id, uint8_t v8, int16_t v16s) {
    int zone_idx = -1;
    int field_offset = -1;

    /* Zone 1: 0x0021-0x0027 */
    if (attr_id >= SHS_ATTR_ZONE1_ENABLED && attr_id <= SHS_ATTR_ZONE1_TYPE_CFG) {
        zone_idx = 0;
        field_offset = attr_id - SHS_ATTR_ZONE1_ENABLED;
    } else {
        /* Zones 2-5: base = 0x0020 + z * 0x10 */
        for (int z = 1; z < SHS_NUM_ZONES; z++) {
            uint16_t base = 0x0020 + z * 0x10;
            if (attr_id >= base && attr_id <= base + 6) {
                zone_idx = z;
                field_offset = attr_id - base;
                break;
            }
        }
    }

    if (zone_idx < 0 || zone_idx >= SHS_NUM_ZONES) return ESP_ERR_NOT_FOUND;

    shs_zone_t *zone = &s_state->zones[zone_idx];
    if (xSemaphoreTake(s_state->zone_config_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
        return ESP_OK;

    switch (field_offset) {
        case 0: zone->enabled = (v8 != 0); break;
        case 1: zone->x1 = v16s; break;
        case 2: zone->y1 = v16s; break;
        case 3: zone->x2 = v16s; break;
        case 4: zone->y2 = v16s; break;
        /* case 5: targets — read-only, skip */
        case 6: zone->type = v8; break;
        default: break;
    }
    xSemaphoreGive(s_state->zone_config_mutex);

    ESP_LOGI(TAG, "Zone %d field %d updated", zone_idx + 1, field_offset);
    shs_zone_cfg_schedule_apply();
    return ESP_OK;
}

/* ============================================================================
 * ZIGBEE ATTRIBUTE WRITE HANDLER
 * ============================================================================ */

static esp_err_t shs_zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message) {
    if (!message) return ESP_OK;

    /* EP1: genOnOff (light) */
    if (message->info.dst_endpoint == SHS_EP_LIGHT &&
        message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
        if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID &&
            message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL) {
            bool light_state = *(bool *)message->attribute.data.value;
            ESP_LOGI(TAG, "Light -> %s", light_state ? "ON" : "OFF");
            light_driver_set_power(light_state);
            return ESP_OK;
        }
    }

    /* EP1: Config cluster (0xFDCD) */
    if (message->info.dst_endpoint == SHS_EP_LIGHT &&
        message->info.cluster == SHS_CL_CFG_ID) {

        uint16_t v = 0;
        uint8_t v8 = 0;
        int16_t v16s = 0;

        if (message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16) {
            v = *(uint16_t *)message->attribute.data.value;
        } else if (message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_S16) {
            v16s = *(int16_t *)message->attribute.data.value;
        } else if (message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8 ||
                   message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL) {
            v8 = *(uint8_t *)message->attribute.data.value;
            v = v8;
        }

        switch (message->attribute.id) {
            case SHS_ATTR_MOVEMENT_COOLDOWN:
                if (v > SHS_COOLDOWN_MAX_SEC) v = SHS_COOLDOWN_MAX_SEC;
                s_state->movement_cooldown_sec = v;
                ld2410_set_moving_cooldown(v);
                shs_save_enqueue(SHS_SAVE_IMMEDIATE_U16, (SHS_ATTR_MOVEMENT_COOLDOWN << 8));
                ESP_LOGI(TAG, "Set Movement Cooldown = %us", (unsigned)v);
                return ESP_OK;

            case SHS_ATTR_OCC_CLEAR_COOLDOWN:
                s_state->occupancy_clear_sec = v;
                ld2410_set_max_gate_timeout((uint8_t)s_state->moving_max_gate,
                                           (uint8_t)s_state->static_max_gate, v);
                shs_zb_set_ou_delay_ep2(v);
                shs_save_enqueue(SHS_SAVE_IMMEDIATE_U16, (SHS_ATTR_OCC_CLEAR_COOLDOWN << 8));
                ESP_LOGI(TAG, "Set Occupancy Cooldown = %us", (unsigned)v);
                return ESP_OK;

            case SHS_ATTR_MOVING_SENS_0_10:
                if (v > 10) v = 10;
                s_state->sens_mv_0_10 = v;
                s_state->moving_sens_0_100 = (uint8_t)((10 - v) * 10);
                ld2410_set_all_sensitivity(s_state->moving_sens_0_100, s_state->static_sens_0_100);
                shs_save_enqueue(SHS_SAVE_DEBOUNCE_SENS_MOVE, s_state->moving_sens_0_100);
                ESP_LOGI(TAG, "Set Moving Sensitivity = %u/10 (threshold=%u)", (unsigned)v, (unsigned)s_state->moving_sens_0_100);
                return ESP_OK;

            case SHS_ATTR_STATIC_SENS_0_10:
                if (v > 10) v = 10;
                s_state->sens_st_0_10 = v;
                s_state->static_sens_0_100 = (uint8_t)((10 - v) * 10);
                ld2410_set_all_sensitivity(s_state->moving_sens_0_100, s_state->static_sens_0_100);
                shs_save_enqueue(SHS_SAVE_DEBOUNCE_SENS_STATIC, s_state->static_sens_0_100);
                ESP_LOGI(TAG, "Set Static Sensitivity = %u/10 (threshold=%u)", (unsigned)v, (unsigned)s_state->static_sens_0_100);
                return ESP_OK;

            case SHS_ATTR_MOVING_MAX_GATE:
                if (v > 8) v = 8;
                s_state->moving_max_gate = v;
                ld2410_set_max_gate_timeout((uint8_t)v, (uint8_t)s_state->static_max_gate,
                                           s_state->occupancy_clear_sec);
                shs_save_enqueue(SHS_SAVE_DEBOUNCE_GATE_MOVE, v);
                ESP_LOGI(TAG, "Set Movement Detection Range = %u", (unsigned)v);
                return ESP_OK;

            case SHS_ATTR_STATIC_MAX_GATE:
                if (v < 2) v = 2; else if (v > 8) v = 8;
                s_state->static_max_gate = v;
                ld2410_set_max_gate_timeout((uint8_t)s_state->moving_max_gate, (uint8_t)v,
                                           s_state->occupancy_clear_sec);
                shs_save_enqueue(SHS_SAVE_DEBOUNCE_GATE_STATIC, v);
                ESP_LOGI(TAG, "Set Static Detection Range = %u", (unsigned)v);
                return ESP_OK;

            case SHS_ATTR_POSITION_REPORTING:
                s_state->position_reporting = (v8 != 0);
                ld2450_set_verbose_logging(s_state->position_reporting);
                light_driver_set_power(s_state->position_reporting);
                ESP_LOGI(TAG, "Position Reporting = %s", s_state->position_reporting ? "ON" : "OFF");
                return ESP_OK;

            case SHS_ATTR_MIN_MOVING_ENERGY:
                if (v > 100) v = 100;
                s_state->min_moving_energy = v;
                ESP_LOGI(TAG, "Min Moving Energy = %d", (int)s_state->min_moving_energy);
                return ESP_OK;

            case SHS_ATTR_MIN_STATIC_ENERGY:
                if (v > 100) v = 100;
                s_state->min_static_energy = v;
                ESP_LOGI(TAG, "Min Static Energy = %d", (int)s_state->min_static_energy);
                return ESP_OK;

            case SHS_ATTR_ZONE_TYPE_CFG:
                if (xSemaphoreTake(s_state->zone_config_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    s_state->zone_type = v8;
                    xSemaphoreGive(s_state->zone_config_mutex);
                }
                ESP_LOGI(TAG, "Zone Type = %d", s_state->zone_type);
                shs_zone_cfg_schedule_apply();
                return ESP_OK;

            default:
                break;
        }

        /* Zone attributes — computed offset handler (CS-03) */
        if (message->attribute.id >= SHS_ATTR_ZONE1_ENABLED) {
            esp_err_t ret = handle_zone_attr(message->attribute.id, v8, v16s);
            if (ret == ESP_OK) return ESP_OK;
        }
    }

    return ESP_OK;
}

esp_err_t shs_zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message) {
    if (callback_id == ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID) {
        return shs_zb_attribute_handler((const esp_zb_zcl_set_attr_value_message_t *)message);
    }
    return ESP_OK;
}

/* ============================================================================
 * SAVE WORKER TASK
 * ============================================================================ */

void shs_save_worker(void *pv) {
    ESP_LOGI(TAG, "NVS save worker started");

    uint32_t mv_debounce_until = 0, st_debounce_until = 0;
    uint32_t gate_mv_debounce_until = 0, gate_st_debounce_until = 0;
    uint16_t pending_mv_sens = 0, pending_st_sens = 0;
    uint16_t pending_gate_mv = 0, pending_gate_st = 0;
    bool mv_pending = false, st_pending = false;
    bool gate_mv_pending = false, gate_st_pending = false;

    while (1) {
        shs_save_msg_t m;

        /* Process any enqueued save requests */
        while (xQueueReceive(s_state->save_q, &m, pdMS_TO_TICKS(100)) == pdTRUE) {
            switch (m.type) {
                case SHS_SAVE_IMMEDIATE_U16: {
                    uint16_t attr = m.u16 >> 8;
                    if (attr == SHS_ATTR_MOVEMENT_COOLDOWN)
                        shs_cfg_save_u16(SHS_NVS_KEY_MV_CD, s_state->movement_cooldown_sec);
                    else if (attr == SHS_ATTR_OCC_CLEAR_COOLDOWN)
                        shs_cfg_save_u16(SHS_NVS_KEY_OCC_CD, s_state->occupancy_clear_sec);
                    break;
                }
                case SHS_SAVE_DEBOUNCE_SENS_MOVE:
                    pending_mv_sens = m.u16;
                    mv_pending = true;
                    mv_debounce_until = (uint32_t)(esp_timer_get_time() / 1000) + SHS_NVS_DEBOUNCE_MS;
                    break;
                case SHS_SAVE_DEBOUNCE_SENS_STATIC:
                    pending_st_sens = m.u16;
                    st_pending = true;
                    st_debounce_until = (uint32_t)(esp_timer_get_time() / 1000) + SHS_NVS_DEBOUNCE_MS;
                    break;
                case SHS_SAVE_DEBOUNCE_GATE_MOVE:
                    pending_gate_mv = m.u16;
                    gate_mv_pending = true;
                    gate_mv_debounce_until = (uint32_t)(esp_timer_get_time() / 1000) + SHS_NVS_DEBOUNCE_MS;
                    break;
                case SHS_SAVE_DEBOUNCE_GATE_STATIC:
                    pending_gate_st = m.u16;
                    gate_st_pending = true;
                    gate_st_debounce_until = (uint32_t)(esp_timer_get_time() / 1000) + SHS_NVS_DEBOUNCE_MS;
                    break;
                case SHS_SAVE_ZONE_CONFIG:
                    shs_zone_cfg_save_to_nvs();
                    break;
            }
        }

        /* Process debounced saves */
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (mv_pending && shs_time_reached(now, mv_debounce_until)) {
            shs_cfg_save_u8(SHS_NVS_KEY_MV_SENS, (uint8_t)pending_mv_sens);
            mv_pending = false;
            ESP_LOGI(TAG, "NVS saved moving sensitivity: %u", (unsigned)pending_mv_sens);
        }
        if (st_pending && shs_time_reached(now, st_debounce_until)) {
            shs_cfg_save_u8(SHS_NVS_KEY_ST_SENS, (uint8_t)pending_st_sens);
            st_pending = false;
            ESP_LOGI(TAG, "NVS saved static sensitivity: %u", (unsigned)pending_st_sens);
        }
        if (gate_mv_pending && shs_time_reached(now, gate_mv_debounce_until)) {
            shs_cfg_save_u8(SHS_NVS_KEY_MV_GATE, (uint8_t)pending_gate_mv);
            gate_mv_pending = false;
            ESP_LOGI(TAG, "NVS saved moving gate: %u", (unsigned)pending_gate_mv);
        }
        if (gate_st_pending && shs_time_reached(now, gate_st_debounce_until)) {
            shs_cfg_save_u8(SHS_NVS_KEY_ST_GATE, (uint8_t)pending_gate_st);
            gate_st_pending = false;
            ESP_LOGI(TAG, "NVS saved static gate: %u", (unsigned)pending_gate_st);
        }
    }
}
