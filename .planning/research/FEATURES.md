# Feature Landscape: Firmware Hardening

**Domain:** ESP32-C6 Zigbee mmWave presence sensor — reliability & diagnostics
**Researched:** 2026-04-30
**Focus:** What makes presence sensor firmware production-quality vs hobby-grade

## Table Stakes

Features required for reliable 24/7 operation. Missing = device needs manual intervention or silently fails.

| Feature | Why Expected | Complexity | Current State |
|---------|--------------|------------|---------------|
| **Thread-safe shared state** | Race conditions cause crashes, data corruption, and false presence readings. Every production RTOS firmware protects shared state with mutexes. | Med | BROKEN — `target_data_mutex` created but never initialized before first callback can fire; `current_target_count` read without mutex in multiple places |
| **No blocking I/O in callbacks** | NVS writes in sensor callbacks block the UART processing loop, causing frame loss and sensor watchdog timeouts. ESPHome solved this with async component loops. | Med | PARTIAL — save worker queue exists for some configs, but zone config NVS writes still happen in callback context |
| **Zigbee TX retry with backoff** | Silent data loss when `esp_zb_lock_acquire` fails. The lock macro silently returns, dropping the occupancy update. Production sensors (SONOFF, Tuya) never silently drop state changes. | Med | BROKEN — lock failure = data silently lost. No retry, no queue, no backoff. Heartbeat partially mitigates but doesn't fix missed transitions. |
| **Sensor health monitoring** | Both LD2410C and LD2450 can hang, lose UART sync, or return garbage data. Users need to know when a sensor is unhealthy without checking serial logs. | Med | PARTIAL — LD2450 has frame watchdog (10s timeout) and error counter. LD2410C has no health monitoring at all. Neither reports health status over Zigbee. |
| **Position smoothing (EMA filter)** | Raw mmWave coordinates jitter ±50mm causing flooding of Zigbee reports and noisy automations. Every commercial sensor filters output. | Low | DISABLED — `EMA_ALPHA` set to 1.0f (no-op) with comment "DIAGNOSTIC". Was 0.3f, needs re-enabling. |
| **Energy-based false positive filtering** | LD2410C reports "presence" at gate 0 with energy 1-5 (noise floor). Without filtering, empty rooms show phantom occupancy. ESPHome includes settle filters for exactly this. | Low | DISABLED — `min_moving_energy`/`min_static_energy` attributes exist but filtering code is wrapped in `#if 0` |
| **Standalone LD2410 fallback** | When LD2450 goes offline (UART hang, HW fault), cross-validation breaks and occupancy may go permanently false. Device should degrade to single-sensor mode gracefully. | Med | MISSING — no fallback logic. LD2450 offline = zones stop updating, but LD2410 occupancy continues independently on EP2 only |
| **Proper int16 encoding for coordinates** | Current code biases coordinates by +4000 to avoid negative Zigbee attribute values. This is a data integrity issue — clients must know the magic offset. | Low | HACK — `+4000` bias applied before Zigbee write, converter subtracts it. Proper fix: use int16 ZCL attribute type |
| **NVS write batching** | Zone config changes arrive as 5-7 individual Zigbee writes. Each one currently triggers a separate NVS write cycle, wearing flash and blocking. | Low | PARTIAL — 500ms debounce timer exists for zone config, but individual attribute changes (sensitivity, gates) each trigger immediate NVS writes |
| **Task watchdog integration** | ESP-IDF Task WDT detects stuck tasks. Without it, a hung sensor task or Zigbee deadlock runs forever silently. Production ESP32 devices subscribe critical tasks to TWDT. | Low | MISSING — no `esp_task_wdt_add()` calls. Default idle task WDT may be enabled via sdkconfig but custom tasks (ld2410, ld2450, zigbee) are unwatched |

## Differentiators

Nice-to-have quality improvements. Not expected, but make debugging and maintenance significantly easier.

| Feature | Value Proposition | Complexity | Notes |
|---------|-------------------|------------|-------|
| **Zigbee-exposed diagnostic counters** | Lock success/fail ratio, TX success/fail, UART frame/error counts, uptime, last restart reason — queryable from Z2M without serial console. Aqara FP2 exposes similar diagnostics. | Med | Counters already tracked internally (`shs_lock_*_count`, `shs_tx_*_count`, `ld2450_state.error_count`). Just need Zigbee attributes + converter exposure. |
| **Modular code structure** | Current 2691-line `shs01.c` monolith makes bugs hard to find and changes risky. Splitting into zigbee_clusters.c, sensor_processing.c, config_manager.c, diagnostics.c reduces cognitive load. | High | Active requirement in PROJECT.md. High impact but high risk — must be done after bug fixes. |
| **Sensor firmware version reporting** | Expose LD2410C and LD2450 firmware versions as Zigbee text attributes. Helps diagnose sensor-specific issues remotely. ESPHome exposes this for LD2410. | Low | `shs_firmware_version` already read from LD2410C. LD2450 firmware read on connection. Not exposed over Zigbee. |
| **Zigbee connectivity auto-recovery** | When the coordinator goes offline or network topology changes, device should rejoin automatically. Current 3-minute TX timeout detection is coarse. | Med | PARTIAL — rejoin logic exists but triggers only after 180s of no successful TX. Could be more responsive with Zigbee signal handler improvements. |
| **LED status indication** | Blink patterns for: joining network, connected, sensor error, factory reset in progress. SONOFF SNZB-06P and Tuya sensors all have status LEDs. | Low | Light driver exists and used for factory reset indication. No systematic status LED protocol. |
| **Configurable report rate limiting** | Global throttle for Zigbee attribute reports (e.g., max 1 report/second per endpoint). Prevents network flooding during high-activity periods. Tuya ZY-M100-24G is notorious for flooding without this. | Med | PARTIAL — `POSITION_UPDATE_INTERVAL_MS` (300ms) and `POSITION_CHANGE_THRESHOLD` (30mm) exist for position data. No throttling for occupancy or zone state changes. |
| **Restart reason tracking** | Log `esp_reset_reason()` at boot and optionally expose via Zigbee. Distinguishes power cycle, watchdog reset, panic, SW reset. Critical for diagnosing field failures. | Low | MISSING — easy to add in `app_main()`. |
| **Stack high-water mark logging** | Periodic `uxTaskGetStackHighWaterMark()` logging for all tasks. Detects if any task is close to stack overflow before it crashes. | Low | MISSING — useful during hardening to right-size stack allocations (currently 4096-8192 per task). |

## Anti-Features

Things to deliberately NOT build this milestone. Tempting but out of scope for hardening.

| Anti-Feature | Why Avoid | What to Do Instead |
|--------------|-----------|-------------------|
| **OTA firmware update** | Significant scope (OTA partition, rollback, security). Separate milestone. Would consume hardening budget. | Keep as future milestone. Fix reliability first. |
| **Power management / sleep modes** | Device is mains-powered. Sleep adds complexity with no benefit. Can cause Zigbee routing issues. | N/A — explicitly out of scope per PROJECT.md |
| **Zigbee Binding / Scenes** | Not required for presence sensors. Adds endpoint complexity. | N/A |
| **Web configuration UI** | Would need WiFi stack alongside Zigbee (not possible on ESP32-C6 simultaneously). ESPHome approach, not native Zigbee. | Z2M external converter handles all config |
| **Additional sensor types** | Adding illuminance, temperature, etc. is feature work not hardening. | Future milestone if desired |
| **Per-gate energy exposure over Zigbee** | ESPHome exposes g0-g8 move/still energy. Requires 18+ additional attributes. Network flooding risk. Only useful during calibration. | Keep engineering mode as serial-console-only feature. Gate thresholds already configurable via existing sensitivity attributes. |
| **Bluetooth configuration** | LD2410C supports BLE config app. Adding BLE stack to ESP32-C6 alongside Zigbee is not advisable — resource contention. | Zigbee-only configuration via Z2M |
| **Configuration backup/restore** | JSON export/import of settings. Nice-to-have but not reliability-critical. | Future milestone per PROJECT.md |
| **Tamper detection** | No hardware support in current PCB design (v1 or v2). | Would require HW revision |
| **Multi-coordinator support** | Some Zigbee devices support binding to multiple coordinators. Adds significant complexity. | Single coordinator is the standard model |

## Feature Dependencies

```
Thread-safe shared state ──→ NVS write batching (safe state required before optimizing writes)
Thread-safe shared state ──→ Position smoothing (needs consistent target data)
Thread-safe shared state ──→ Standalone LD2410 fallback (cross-sensor state must be safe)
Bug fixes (all table stakes) ──→ Modular refactor (don't restructure broken code)
Sensor health monitoring ──→ Zigbee-exposed diagnostics (need health data before exposing it)
Proper int16 encoding ──→ Converter update (breaking change in shs01_enhanced.mjs)
```

## MVP Recommendation (Hardening Milestone)

**Priority 1 — Fix crashes and data loss (table stakes):**
1. Thread-safe shared state (mutex init + consistent locking)
2. Remove NVS I/O from sensor callbacks
3. Zigbee TX retry with backoff for state transitions

**Priority 2 — Re-enable disabled features (table stakes):**
4. Re-enable position smoothing (EMA_ALPHA → 0.3f)
5. Re-enable energy-based false positive filtering
6. Fix int16 coordinate encoding (remove +4000 hack)

**Priority 3 — Reliability infrastructure (table stakes):**
7. Task watchdog integration for custom tasks
8. Sensor health monitoring + standalone LD2410 fallback
9. NVS write batching for zone config

**Priority 4 — Observability (differentiators, if time permits):**
10. Zigbee-exposed diagnostic counters
11. Restart reason tracking
12. Modular code restructure

**Defer:** All anti-features. OTA, sleep, binding, web UI, BLE, per-gate exposure.

## Sources

- Codebase analysis: [main/shs01.c](main/shs01.c), [main/shs01.h](main/shs01.h), component drivers
- ESP-IDF watchdog docs: https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/system/wdts.html
- ESPHome LD2410 component: https://esphome.io/components/sensor/ld2410.html (settle filters, throttling, engineering mode patterns)
- SONOFF SNZB-06P Z2M page: occupancy_timeout, occupancy_sensitivity, OTA, illumination
- Tuya ZG-205ZL Z2M page: fading_time, multi-sensitivity, motion_state enum, LED mode control
- Tuya ZY-M100-24G Z2M page: presence_timeout, max_range, known 1Hz report flooding issue
- PROJECT.md active requirements alignment
