# Phase 3: Reliability & Diagnostics - Context

**Gathered:** 2026-04-30
**Status:** Ready for planning

<domain>
## Phase Boundary

Add task watchdog monitoring, expose sensor health status via Zigbee, batch NVS zone writes into atomic transactions, and surface existing diagnostic counters through Zigbee attributes. All changes add monitoring/reliability infrastructure to the existing monolithic shs01.c — no structural refactoring.

</domain>

<decisions>
## Implementation Decisions

### Task Watchdog Strategy (SH-01)
- **D-01:** Use ESP-IDF Task Watchdog Timer (TWDT) via `esp_task_wdt` API — built-in panic/reset support, minimal code
- **D-02:** Monitor only the 2 sensor tasks: `shs_ld2410_task` and `shs_ld2450_task` — these interact with external UART hardware and are the primary crash risk. `save_worker` and `boot_button` are low-risk (NVS/GPIO only). Zigbee task runs `esp_zb_stack_main_loop()` with its own internal mechanisms.
- **D-03:** Timeout: 10 seconds — sensor task loops run at 20ms but LD2450 reconnection has legitimate 500ms delays (`vTaskDelay` for stabilization, firmware version read). 10s = 500 missed iterations, clearly frozen.
- **D-04:** Action on timeout: Automatic reboot via `CONFIG_ESP_TASK_WDT_PANIC=y` — device is mains-powered, NVS retains all config, Zigbee rejoin is automatic. A frozen sensor task means corrupted state; full reboot is safer than partial recovery.

### Sensor Health Exposure (SH-02)
- **D-05:** Expose sensor connected/disconnected status as bool attributes on the existing config cluster 0xFDCD (EP1) — consistent with existing config attribute pattern, no new endpoints (preserves 25-endpoint model per PROJECT.md decision), minimal converter changes
- **D-06:** LD2410C disconnect detection via UART frame timeout — the LD2410C sends continuous frames (~100ms interval). Track `last_valid_frame_timestamp` and mark disconnected if no frame received within threshold (e.g., 3 seconds). Passive monitoring, no intrusive polling commands.
- **D-07:** LD2450 health uses existing `connected` tracking already in `shs_ld2450_task` — just needs to be surfaced via the new Zigbee attribute

### LD2450 Reconnection Sync (SH-03)
- **D-08:** Agent's discretion — choose between simple Zigbee health attribute report on reconnect event, or full state re-report (zones + target count) for consistency. The existing reconnection logic (restart command + zone re-apply) provides the foundation.

### NVS Batching (NVS-02)
- **D-09:** Agent's discretion on approach — options are: (a) `nvs_set_blob` with packed struct for single-key storage, or (b) keep individual `nvs_set_*` calls but group under single `nvs_open`/`nvs_commit`. Consider save_worker stack at 3072B (noted borderline in STATE.md) when choosing.
- **D-10:** Requirement: zone configuration save must complete atomically — no partial writes visible on power loss. This is the core NVS-02 success criterion.

### NVS Error Handling (NVS-03)
- **D-11:** Agent's discretion — log failures with `ESP_LOGW` and continue operating with in-memory values (defaults loaded at boot). Optionally expose NVS error state via Zigbee if it fits naturally with the diagnostic attributes.

### Diagnostic Counter Exposure (ZB-03)
- **D-12:** Agent's discretion on Zigbee exposure mechanism — config cluster 0xFDCD attributes or new manufacturer-specific cluster. Choose based on attribute ID space availability and converter complexity.
- **D-13:** Agent's discretion on which counters to expose — at minimum the 5 existing counters (`shs_lock_success_count`, `shs_lock_fail_count`, `shs_lock_consecutive_fails`, `shs_tx_success_count`, `shs_tx_fail_count`). May add uptime and/or free heap if useful for remote troubleshooting.

### Zigbee Connectivity Recovery (ZB-04)
- **D-14:** Agent's discretion — refine the existing connectivity check mechanism (60s interval, 180s TX timeout → rejoin). Ensure rejoin updates health attributes. No full rewrite needed.

### Agent's Discretion
- NVS batching approach (blob vs grouped writes) — choose based on stack and code complexity tradeoffs
- NVS error handling granularity — log-only vs log+Zigbee flag
- Diagnostic counter set — 5 existing vs 5+uptime+heap
- Diagnostic Zigbee exposure mechanism — config cluster vs new cluster
- LD2450 reconnection Zigbee sync depth — health attr only vs full state re-report
- ZB-04 connectivity check refinements — keep existing logic or add retry before rejoin

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Task Architecture & Watchdog
- `main/shs01.c` §L3160-3230 — `app_main()` initialization order and task creation (5 tasks with priorities)
- `main/shs01.c` §L2200-2300 — `shs_ld2450_task()` main loop with connection monitoring and reconnection logic
- `main/shs01.c` §L3188 — Zigbee task creation at priority 5 (NOT to be monitored by TWDT)
- `main/shs01.c` §L3213-3215 — Sensor task creation at priority 4 (TWDT targets)
- ESP-IDF docs: `esp_task_wdt.h` API — `esp_task_wdt_init()`, `esp_task_wdt_add()`, `esp_task_wdt_reset()`

### Sensor Health & Connectivity
- `main/shs01.c` §L2240-2260 — LD2450 connection/disconnection detection with restart recovery
- `main/shs01.c` §L130-135 — `shs_zb_connected`, `shs_zb_rejoin_pending` flags
- `main/shs01.c` §L2265-2295 — Zigbee connectivity check, heartbeat, and auto-rejoin logic
- `main/shs01.h` — Config cluster 0xFDCD attribute definitions (add new health attrs in same block)
- `components/ld2410_enhanced/include/ld2410_enhanced.h` — LD2410C driver API (check for frame timing)
- `components/ld2450/include/ld2450.h` — LD2450 driver API (connection status)

### NVS Infrastructure
- `main/shs01.c` §L390-430 — `shs_zone_cfg_save_to_nvs()` — the 31-write function to batch
- `main/shs01.c` §L3100-3160 — `shs_save_worker()` — existing worker with message queue
- `main/shs01.c` §L280-310 — NVS helper functions (`shs_cfg_save_u16/u8/i16`)
- `main/shs01.c` §L3206 — save_worker stack size 3072B (monitor with `uxTaskGetStackHighWaterMark()`)

### Diagnostic Counters
- `main/shs01.c` §L144-149 — Existing counters: `shs_lock_success_count`, `shs_lock_fail_count`, `shs_lock_consecutive_fails`, `shs_tx_success_count`, `shs_tx_fail_count`
- `main/shs01.c` §L150-205 — Lock macros `SHS_ZB_LOCK_ACQUIRE_OR_RETURN()` that update counters
- `main/shs01.c` §L2265-2270 — Periodic diagnostic stats logging (60s interval)

### Z2M Converter
- `zigbee2mqtt/external_converters/shs01_enhanced.js` — External converter (needs health + diag attrs)
- `zigbee2mqtt/external_converters/shs01_enhanced.mjs` — ESM variant of converter

### Prior Phase Context
- `.planning/phases/01-critical-bug-fixes/01-CONTEXT.md` — Phase 1 decisions (mutex strategy, retry logic, save worker routing)
- `.planning/research/STACK.md` — ESP-IDF concurrency patterns
- `.planning/research/PITFALLS.md` — Known risks

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `shs_save_worker` (L3100): FreeRTOS task with message queue and debounce — handles `SHS_SAVE_ZONE_CONFIG` message type already (added in Phase 1)
- Diagnostic counters (L144-149): 5 counters already maintained by lock macros — just need Zigbee attribute binding
- LD2450 connection tracking (L2240-2260): `was_connected`/`connected` state machine with restart recovery — model for LD2410C health
- Zigbee connectivity check (L2265-2295): 60s heartbeat + 180s TX timeout → rejoin — foundation for ZB-04 refinement
- Config cluster 0xFDCD attribute pattern: well-established in shs01.h with consistent attribute ID layout (0x00XX for general, 0x00[2-6]X for zones)

### Established Patterns
- Lock-and-return macro: `SHS_ZB_LOCK_ACQUIRE_OR_RETURN()` with retry + diagnostic counting
- NVS save via queue: `shs_save_enqueue()` → `shs_save_worker` → per-type handler
- Zigbee attribute report: `shs_zb_set_analog_value()` + `shs_zb_report_analog_attr()` for float values
- Boolean Zigbee attribute pattern: `esp_zb_zcl_set_attribute_val()` for bool attrs on config cluster

### Integration Points
- `shs_ld2410_task` loop — add `esp_task_wdt_reset()` call + frame timestamp tracking
- `shs_ld2450_task` loop — add `esp_task_wdt_reset()` call + surface existing connection state via Zigbee
- `app_main()` — add `esp_task_wdt_init()` before task creation
- `sdkconfig.defaults` — add `CONFIG_ESP_TASK_WDT_PANIC=y` and timeout config
- Config cluster 0xFDCD EP1 — register new health + diagnostic attributes
- External converter — add fromZigbee/toZigbee entries for new attributes

</code_context>

<specifics>
## Specific Ideas

- ESP32-C6 is single-core (RISC-V) — TWDT monitors the idle task by default, sensor tasks must be explicitly added with `esp_task_wdt_add(NULL)` from within each task
- save_worker stack at 3072B is borderline — if batching uses blob approach, verify with `uxTaskGetStackHighWaterMark()` that the struct doesn't overflow
- LD2410C frame interval is ~100ms — a 3-second timeout means ~30 consecutive missed frames before marking disconnected

</specifics>

<deferred>
## Deferred Ideas

- "Sensor stuck returning same data" detection (data quality analysis) — could be a future milestone feature, not Phase 3 scope
- NVS wear leveling / partition health monitoring — out of scope for this milestone

None — discussion stayed within phase scope

</deferred>

---

*Phase: 03-reliability-diagnostics*
*Context gathered: 2026-04-30*
