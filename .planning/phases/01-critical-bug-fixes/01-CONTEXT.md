# Phase 1: Critical Bug Fixes & Concurrency Safety - Context

**Gathered:** 2026-04-30
**Status:** Ready for planning

<domain>
## Phase Boundary

Fix race conditions on shared sensor state between FreeRTOS tasks, add Zigbee lock retry with backoff, and route zone NVS writes through the existing save worker task. All changes are behavioral fixes in the current monolithic shs01.c — no structural refactoring.

</domain>

<decisions>
## Implementation Decisions

### TS-01 Re-evaluation
- **D-01:** TS-01 is ALREADY RESOLVED — `target_data_mutex` is initialized at shs01.c:3031 (`xSemaphoreCreateMutex()`) BEFORE `ld2450_init()` at L3053 and callback registration at L3054. Remove from phase scope.

### Mutex Strategy (TS-02, TS-03, TS-04)
- **D-02:** Use 2 mutexes + volatile for simple flags:
  - **`target_data_mutex` (existing, L48)** — extend to also protect `shs_ld2450_target_count` (currently written at L963 in LD2450 callback, read at L710 in LD2410 callback without protection)
  - **`zone_config_mutex` (NEW)** — protect the 31 zone configuration variables (`shs_zone1_enabled`, `shs_zone1_x1`, ..., `shs_zone5_type`) written by Zigbee attribute handler and read by LD2450 task in `shs_zone_cfg_apply_to_sensor()`
  - **`volatile` for `shs_position_reporting`** — single bool written by button task (L1770), read by LD2450 callback (L1166). ESP32-C6 is single-core; volatile is sufficient for atomic bool access
- **D-03:** Rationale: Global mutex would create unnecessary contention between independent zone and target operations. Per-resource mutex is overkill for single-core MCU.

### Zigbee Retry Logic (ZB-01, ZB-02)
- **D-04:** Replace `SHS_ZB_LOCK_ACQUIRE_OR_RETURN()` macro with retry version:
  - 3 attempts with exponential backoff: 50ms → 100ms → 200ms
  - After 3 failures, return (same as current behavior) but with WARNING log
  - Keep existing diagnostic counters (`shs_lock_fail_count`, `shs_lock_consecutive_fails`, `shs_lock_success_count`)
  - Same change for `SHS_ZB_LOCK_ACQUIRE_OR_RETURN_FALSE()` variant
- **D-05:** NO queue-based retry worker — that's over-engineering for this phase. Lock-free `esp_zigbee_task_queue_post()` approach deferred to Phase 4 (refactoring).

### NVS Zone Save Routing (NVS-01)
- **D-06:** Integrate zone NVS saves into existing `shs_save_worker`:
  - Add `SHS_SAVE_ZONE_CONFIG` type to `shs_save_msg_t` enum
  - In `shs_zone_cfg_apply_to_sensor()` (L586): replace direct `shs_zone_cfg_save_to_nvs()` call with `xQueueSend(shs_save_q, &zone_save_msg, 0)`
  - Save worker receives message and calls `shs_zone_cfg_save_to_nvs()` in its own context (priority 3, won't block Zigbee task at priority 5)
  - Existing 500ms debounce in `shs_zone_cfg_schedule_apply()` still applies — save worker only gets the request after debounce expires
- **D-07:** The 31 individual NVS writes inside `shs_zone_cfg_save_to_nvs()` stay as-is this phase. Batching into single commit is Phase 3 (NVS-02).

### Agent's Discretion
- Lock timeout value (currently 100ms at L153) — agent may adjust if retry backoff changes the effective total wait time
- Whether to add `configASSERT(target_data_mutex != NULL)` as a safety check at boot — defensive but optional
- Log levels for retry attempts (DEBUG vs INFO)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Concurrency & Task Architecture
- `main/shs01.c` §L140-180 — Lock macros `SHS_ZB_LOCK_ACQUIRE_OR_RETURN()` and diagnostic counters
- `main/shs01.c` §L185-240 — Shared state variables (target count, zone vars, flags)
- `main/shs01.c` §L3020-3070 — `app_main()` initialization order (mutex → Zigbee → LD2410 → LD2450 → save_worker → tasks)

### NVS Save Infrastructure
- `main/shs01.c` §L280-310 — NVS helper functions (`shs_cfg_save_u16`, `shs_cfg_save_u8`, `shs_cfg_save_i16`)
- `main/shs01.c` §L365-420 — `shs_zone_cfg_save_to_nvs()` — the 31-write function to route through save_worker
- `main/shs01.c` §L2962-3010 — `shs_save_worker()` — existing worker task with message queue and debounce

### Race Condition Sites
- `main/shs01.c` §L710 — `shs_ld2450_target_count` read in LD2410 callback (no mutex)
- `main/shs01.c` §L963 — `shs_ld2450_target_count` write in LD2450 callback (no mutex)
- `main/shs01.c` §L505-590 — Zone config apply function (reads zone vars set by Zigbee handler)

### Research
- `.planning/research/STACK.md` — ESP-IDF concurrency patterns, Zigbee lock best practices
- `.planning/research/PITFALLS.md` — Known risks for this type of refactoring

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `shs_save_worker` (L2962): FreeRTOS task with message queue — extend with new message type for zone saves
- `shs_save_msg_t` enum: Existing message types for sensitivity/gate debouncing — add zone config type
- `target_data_mutex` (L48): Already initialized and used for `current_targets[]` — extend scope to cover `shs_ld2450_target_count`
- Diagnostic counters (L140-145): `shs_lock_success_count`, `shs_lock_fail_count`, `shs_lock_consecutive_fails` — already in place, just need better retry before counting as failure

### Established Patterns
- Lock-and-return macro pattern: `SHS_ZB_LOCK_ACQUIRE_OR_RETURN()` at L148 — modify in-place
- NVS debounce pattern: 500ms zone debounce via `shs_zone_cfg_schedule_apply()` at L505 — keep as-is
- Save worker queue pattern: `xQueueSend(shs_save_q, &msg, 0)` — follow same pattern for zone saves

### Integration Points
- `shs_on_state_change()` (LD2410 callback) — wrap `shs_ld2450_target_count` read with `target_data_mutex`
- `shs_on_ld2450_target_update()` — wrap `shs_ld2450_target_count` write with `target_data_mutex`
- `shs_zone_cfg_apply_to_sensor()` — add `zone_config_mutex` around zone var reads, replace NVS save with queue send
- Zigbee attribute write handler — add `zone_config_mutex` around zone var writes

</code_context>

<specifics>
## Specific Ideas

- ESP32-C6 is single-core (RISC-V) — no SMP-specific concerns, but preemptive scheduling still requires mutexes for multi-word state
- `volatile` is explicitly chosen for `shs_position_reporting` because it's a single bool — mutex overhead not warranted
- The retry backoff (50/100/200ms) totals 350ms max — acceptable latency for Zigbee attribute reports

</specifics>

<deferred>
## Deferred Ideas

- Lock-free Zigbee updates via `esp_zigbee_task_queue_post()` — Phase 4 (requires full report function refactoring)
- NVS batch commit (single `nvs_commit()` for 31 zone writes) — Phase 3 (NVS-02)
- Expose diagnostic counters via Zigbee — Phase 3 (ZB-03)

</deferred>

---

*Phase: 01-critical-bug-fixes*
*Context gathered: 2026-04-30*
