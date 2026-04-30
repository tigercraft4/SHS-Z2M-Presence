---
phase: 04-code-restructuring
plan: 03
status: complete
started: 2025-01-XX
completed: 2025-01-XX
---

# Plan 04-03 Summary: Config, Sensor, Button Modules + Slim app_main

## Objective
Extract NVS/config logic, sensor callbacks/tasks, and boot button handling into dedicated modules. Rewrite shs01.c from ~2980 lines to ~85 lines containing only `app_main()`.

## What Was Done

### Task 1: shs_config.h/c — Configuration & NVS Module (~430 lines)
- **NVS helpers**: `shs_cfg_save_u16`, `shs_cfg_save_u8`, `shs_save_enqueue`, `shs_cfg_sync_sens_proxies`
- **NVS load**: `shs_cfg_load_from_nvs()` loads basic LD2410C config from NVS
- **Zone blob format**: `shs_zone_cfg_save_to_nvs()` / `shs_zone_cfg_load_from_nvs()` with legacy per-key fallback + auto-migration
- **Zone debounce**: `shs_zone_cfg_schedule_apply()` / `shs_zone_cfg_check_pending()` with 500ms debounce, `shs_zone_cfg_apply_to_sensor()` applies to LD2450
- **LD2410 config**: `shs_apply_ld2410_config()` — public, called by sensor task at boot/recovery
- **Zone attr handler**: `handle_zone_attr()` uses computed offsets (CS-03) — ~30 lines replaces ~220 lines of switch/case
- **Attribute handler**: `shs_zb_attribute_handler()` handles light on/off, config cluster attrs, delegates zone attrs to computed handler
- **Action handler**: `shs_zb_action_handler()` — registered by zigbee module via `extern`
- **Save worker**: `shs_save_worker()` — debounced NVS write task with immediate and debounced save modes

### Task 2: shs_sensor.h/c — Sensor Module (~380 lines)
- **Init**: `shs_sensor_init()` stores state pointer AND registers all three sensor callbacks
- **Target helpers**: `is_valid_target()`, `shs_point_in_zone()`, `shs_target_in_interference_zone()` (loop over zones instead of 5x copy-paste)
- **LD2450 target callback**: `shs_on_ld2450_target_update()` — interference zone filtering, target count update, overall occupancy, position reporting with EMA smoothing, change threshold
- **LD2450 zone callback**: `shs_on_ld2450_zone_update()` — single loop over 5 zones using endpoint lookup arrays (replaces 5x copy-paste blocks)
- **LD2410C callback**: `shs_on_state_change()` — cross-validation with LD2450, moving/static cooldown timers, occupancy update
- **LD2410 task**: `shs_ld2410_task()` — boot config, TWDT registration, connection monitoring/recovery, frame-based disconnect detection
- **LD2450 task**: `shs_ld2450_task()` — zone config apply, TWDT, connection monitoring, periodic ZB heartbeat/diagnostics, rejoin logic via `shs_zb_schedule_rejoin()`

### Task 2 (cont): shs_button.h/c — Button Module (~175 lines)
- **Init**: `shs_button_init()` stores state pointer
- **Flash LED**: `shs_flash_led()` — feedback helper
- **Button task**: `shs_boot_button_task()` — GPIO init, debounce, triple-click config toggle, quad-click LD2410C factory reset, 6s hold ESP factory reset

### Task 2 (cont): shs01.c — Slim Entry Point (85 lines)
- `static shs_state_t g_state` — single shared state
- `app_main()`: NVS init → `shs_state_init()` → module inits → `shs_cfg_load_from_nvs()` → light driver → TWDT → `xTaskCreate(shs_zigbee_task)` → sensor driver inits → `shs_sensor_init()` → save worker task → zone config load → 3x sensor/button tasks

### Supporting Changes
- **shs01.h**: Added `SHS_ZB_LOCK_TIMEOUT_MS`, `SHS_LD2410C_FRAME_TIMEOUT_MS`, button timing constants
- **shs_zigbee.h/c**: Added `shs_zb_schedule_rejoin()` wrapper for thread-safe BDB commissioning from non-Zigbee tasks; removed duplicate `SHS_ZB_LOCK_TIMEOUT_MS` define
- **shs_state.c**: Save queue size increased from 8 to 16 (matches original)

## Key Design Decisions
- **Callback registration in `shs_sensor_init()`** — not in `app_main()`, because callbacks are static in shs_sensor.c. Clean encapsulation.
- **`shs_apply_ld2410_config()` public in shs_config** — shared between config module (attr handler) and sensor module (boot/recovery). Avoids duplication.
- **`shs_zb_schedule_rejoin()` wrapper** — the BDB commissioning callback is static in shs_zigbee.c. Public wrapper uses `esp_zb_scheduler_alarm` for thread-safe scheduling from sensor tasks.
- **Zone endpoint lookup arrays** — `zone_occ_eps[]` and `zone_target_eps[]` eliminate switch/case in zone callback.
- **Debounce variables in button task** — moved from `static` in original to local in task function (same scope, cleaner).

## Files Created
- `main/shs_config.h` (25 lines)
- `main/shs_config.c` (430 lines)
- `main/shs_sensor.h` (14 lines)
- `main/shs_sensor.c` (380 lines)
- `main/shs_button.h` (16 lines)
- `main/shs_button.c` (175 lines)

## Files Modified
- `main/shs01.c` (2980 → 85 lines, -97% reduction)
- `main/shs01.h` (+10 lines: shared constants)
- `main/shs_zigbee.c` (+5 lines: schedule_rejoin, -1 line: duplicate define)
- `main/shs_zigbee.h` (+3 lines: schedule_rejoin prototype)
- `main/shs_state.c` (queue size 8 → 16)

## Metrics
- **Net line change**: -1965 lines (3346 removed, 1381 added)
- **Module count**: 1 monolith → 7 focused files (state, zigbee, config, sensor, button, shs01, shs01.h)
- **Largest module**: shs_config.c at ~430 lines (config + NVS + attr handler)
- **Zone handler**: ~30 lines replaces ~220 lines of switch/case (CS-03 computed offsets)
- **Zone callback**: single loop replaces 5x copy-paste blocks

## Verification Status
- Code review: all cross-module references verified (includes, externs, function signatures)
- Callback type signatures verified against component headers
- Build not available on this machine (no ESP-IDF toolchain)
- **Requires build verification on device/CI before shipping**
