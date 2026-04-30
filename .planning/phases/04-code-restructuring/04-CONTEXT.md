# Phase 4: Code Restructuring - Context

**Gathered:** 2026-04-30
**Status:** Ready for planning

<domain>
## Phase Boundary

Split the 2980-line monolithic shs01.c into 5 focused modules with a shared state struct. No behavioral changes — all existing Zigbee endpoints, attribute IDs, and Z2M converter compatibility preserved. The zone attribute handler and endpoint creation blocks are de-duplicated using computed offsets and helper functions/loops.

</domain>

<decisions>
## Implementation Decisions

### Module Boundaries (CS-02)
- **D-01:** Split into exactly 5 modules:
  1. **shs_state** (.h/.c) — `shs_state_t` struct definition, `shs_state_init()`, mutexes
  2. **shs_config** (.h/.c) — NVS load/save, attribute handler (`shs_zb_attribute_handler`), save_worker task, zone apply/schedule logic
  3. **shs_sensor** (.h/.c) — LD2410/LD2450 tasks, callbacks (`shs_on_state_change`, `shs_on_ld2450_target_update`, `shs_on_ld2450_zone_update`), target processing, zone occupancy, force_update functions, health reporting
  4. **shs_zigbee** (.h/.c) — Endpoint creation, Zigbee task, ZB helpers (`shs_zb_set_*`, `shs_zb_report_*`, lock macros), BDB commissioning, signal handler
  5. **shs_button** (.h/.c) — Boot button task, LED flash, factory reset logic

- **D-02:** Cross-module access via **shared state struct passed by pointer**:
  - `shs_state_t` allocated in `app_main()` (stack or static)
  - Each module receives pointer via `xxx_init(shs_state_t *state)` and stores it as `static shs_state_t *s_state`
  - Direct field access with no getter/setter overhead: `s_state->zones[i].x1`
  - Mutexes live inside the struct: `s_state->target_data_mutex`, `s_state->zone_config_mutex`

- **D-03:** shs01.c reduced to ~100 lines: `app_main()` → init NVS/flash → `shs_state_init()` → `shs_config_load()` → create tasks (zigbee, ld2410, ld2450, button, save_worker)

### State Struct Design (CS-01)
- **D-04:** **Flat struct with zone array** — `shs_state_t` has flat fields for sensor/diagnostic/config state plus nested `shs_zone_t zones[SHS_NUM_ZONES]` array
- **D-05:** `shs_zone_t` sub-struct:
  ```c
  typedef struct {
      bool    enabled;
      int16_t x1, y1, x2, y2;
      uint8_t type;
      uint8_t targets;
      bool    occupied;
  } shs_zone_t;
  ```
- **D-06:** All other state fields remain flat in `shs_state_t`: `moving_state`, `lock_success_count`, `movement_cooldown_sec`, etc. No deeper nesting.

### Zone Handler De-duplication (CS-03)
- **D-07:** Agent's discretion — compute zone index from attribute ID using the existing stride pattern in shs01.h (zone1=0x0020, zone2=0x0030, etc. → 0x10 stride). The 30 copy-paste switch cases (~390 lines) collapse to ~15 lines of computed offset logic. Agent may adjust approach if attribute layout doesn't perfectly divide.

### Endpoint Creation Pattern (CS-04)
- **D-08:** Agent's discretion — replace 12+ identical endpoint blocks with loops or factory helper functions. EP1 (Light+Config) and EP2 (OCC+Distance) remain hand-coded (unique cluster composition). Agent chooses between helper functions (`shs_zb_add_binary_ep()`, `shs_zb_add_analog_ep()`) or table-driven registration based on code clarity.

### Endpoint Registration Order (CC-03)
- **D-09:** Endpoint registration order in `esp_zb_ep_list_add_ep()` calls MUST be preserved exactly: EP1, EP2, EP3, EP4, EP5, EP6, EP7, EP22, EP23, EP8-16, EP17, EP18, EP19, EP20, EP21, EP24, EP25. Changing order breaks existing Z2M pairings.

### Agent's Discretion
- Zone handler offset computation mechanism (0x10 stride or alternative)
- Endpoint creation pattern (factory functions vs table-driven vs extended loops)
- Whether `shs_state_t` is stack-allocated or static in `app_main()`
- Header include strategy (per-module headers vs single shared shs_state.h + per-module)
- Whether existing `#define` constants stay in shs01.h or move to shs_state.h

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Current Monolith Structure
- `main/shs01.c` — Full 2980-line source, all functions and globals
- `main/shs01.h` — Endpoint defines, cluster IDs, attribute IDs (zone stride visible here)

### Zone Attribute Handler (CS-03 target)
- `main/shs01.c` §L1633-2020 — `shs_zb_attribute_handler()` — 30 zone switch cases to de-duplicate

### Endpoint Creation (CS-04 target)
- `main/shs01.c` §L2599-3260 — `shs_zigbee_task()` — 25 endpoint creation blocks

### Shared State Variables
- `main/shs01.c` §L44-283 — All ~100 static globals to consolidate into `shs_state_t`
- `main/shs01.c` §L49-50 — Mutexes: `target_data_mutex`, `zone_config_mutex`

### NVS / Config Infrastructure
- `main/shs01.c` §L313-530 — NVS helpers, zone blob save/load, config load
- `main/shs01.c` §L3261-3320 — `shs_save_worker()` task

### Sensor Tasks
- `main/shs01.c` §L2272-2440 — `shs_ld2410_task()` and `shs_ld2450_task()`
- `main/shs01.c` §L769-1310 — Sensor callbacks (`shs_on_state_change`, target/zone update)

### Button Task
- `main/shs01.c` §L2040-2210 — `shs_boot_button_task()` and `shs_flash_led()`

### External Components (don't modify)
- `components/ld2410_enhanced/` — LD2410C driver (header API stays)
- `components/ld2450/` — LD2450 driver (header API stays)
- `components/zcl_utility/` — Zigbee cluster utility helpers
- `components/light_driver/` — LED control

### Z2M Converter (verify compatibility)
- `zigbee2mqtt/external_converters/shs01_enhanced.js` — CommonJS converter
- `zigbee2mqtt/external_converters/shs01_enhanced.mjs` — ESM converter

### Prior Phase Decisions
- `.planning/phases/01-critical-bug-fixes/01-CONTEXT.md` — Mutex strategy, volatile for single-core
- `.planning/phases/03-reliability-diagnostics/03-CONTEXT.md` — Health/diagnostic attributes on 0xFDCD

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- EP8-16 already uses a loop pattern (L2980-3070) — extend this to zone occupancy and target count EPs
- `shs_zone_cfg_blob_t` packed struct (Phase 3) already groups zone config for NVS — reusable as serialization format
- `shs_save_enqueue()` / `shs_save_worker()` pattern — stays in shs_config module

### Established Patterns
- All Zigbee attribute access through `SHS_ZB_LOCK_ACQUIRE_OR_RETURN()` macros with retry — macros move to shs_zigbee.h
- Zone config debounce via `shs_zone_cfg_schedule_apply()` + `shs_zone_cfg_check_pending()` — stays in shs_config
- Sensor callbacks registered via driver init (ld2410_init, ld2450_init) — callbacks move to shs_sensor, driver init stays in app_main

### Integration Points
- `app_main()` is the integration hub: init order matters (NVS → state → config load → create mutexes → create tasks)
- `shs_zigbee_task` calls `shs_zb_attribute_handler` for incoming writes → attribute handler calls config/sensor functions
- Sensor tasks call Zigbee helpers (`shs_zb_set_binary_value`, `shs_zb_report_attr`) → cross-module dependency sensor→zigbee
- Save worker called from config module → needs queue handle from state

### Critical Constraint
- **Endpoint registration order is sacred** — Z2M uses endpoint order from interview to map entities. Reordering would require re-pairing all devices.

</code_context>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches. User deferred all implementation-level choices (zone handler mechanism, endpoint pattern, struct allocation) to agent discretion.

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 04-code-restructuring*
*Context gathered: 2026-04-30*
