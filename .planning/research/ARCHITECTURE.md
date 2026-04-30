# Architecture Patterns

**Domain:** ESP32-C6 multi-sensor Zigbee presence firmware
**Researched:** 2026-04-30
**Confidence:** HIGH (based on direct codebase analysis + ESP-IDF component patterns)

## Current State Analysis

`shs01.c` is 2691 lines containing 5 responsibilities mixed together:

| Concern | Lines (approx) | What It Does |
|---------|----------------|--------------|
| Shared state | 1–260 | ~100 static globals: sensor state, zone config, diagnostics |
| NVS persistence | 260–500 | Load/save config & zone params, save worker queue |
| Zigbee helpers | 600–800 | Lock macros, attribute setters, report senders |
| Sensor callbacks | 800–1500 | LD2410/LD2450 callbacks, cross-validation, zone occupancy, position smoothing, force-update |
| Zigbee infra | 1500–2960 | Attribute write handler (300 lines of switch/case), 25 endpoint creation (~600 lines of boilerplate), signal handler, commissioning, connectivity monitoring |
| Boot button | 1860–1960 | Triple-click/long-press state machine |
| Tasks + main | 1960–3080 | LD2410/LD2450/Zigbee tasks, save worker, `app_main` |

**Key problems driving refactoring:**
1. Race conditions — `shs_ld2450_target_count` read without mutex in LD2410 callback
2. Zone config uses 30+ individual static variables instead of an array
3. Sensor callbacks directly call Zigbee lock+set+report (tight coupling)
4. NVS I/O in some code paths blocks sensor processing
5. Everything depends on everything — no compilation firewall

## Recommended Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        app_main (shs01.c)                   │
│  Initialization, task creation, wiring callbacks            │
└─────────┬──────────┬──────────┬──────────┬─────────────────┘
          │          │          │          │
          ▼          ▼          ▼          ▼
   ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────┐
   │ shs_zigbee│ │shs_sensor│ │shs_config│ │  shs_button  │
   │          │ │          │ │          │ │              │
   │ EP/cluster│ │ callbacks│ │ NVS load │ │ triple-click │
   │ creation │ │ cross-val│ │ NVS save │ │ factory reset│
   │ attr write│ │ zone occ │ │ save wkr │ │              │
   │ reports  │ │ smoothing│ │ defaults │ │              │
   └─────┬────┘ └─────┬────┘ └─────┬────┘ └──────┬───────┘
         │            │            │              │
         ▼            ▼            ▼              ▼
   ┌──────────────────────────────────────────────────────┐
   │                   shs_state.h                        │
   │  Shared state struct + accessor API (mutex-guarded)  │
   │  Zone config array + sensor data + diagnostics       │
   └──────────────────────────────────────────────────────┘
         │            │            │
         ▼            ▼            ▼
   ┌──────────┐ ┌──────────┐ ┌──────────────┐
   │ ld2410   │ │ ld2450   │ │ light_driver  │  (existing components,
   │(component)│ │(component)│ │ zcl_utility  │   unchanged)
   └──────────┘ └──────────┘ └──────────────┘
```

### Component Boundaries

| Module | File(s) | Responsibility | Depends On |
|--------|---------|---------------|------------|
| **shs_state** | `shs_state.h`, `shs_state.c` | Shared state struct, mutex creation, thread-safe accessors. Zero business logic. | FreeRTOS only |
| **shs_config** | `shs_config.h`, `shs_config.c` | NVS load/save for all params (sensor + zone), save worker task, config defaults, debounce logic | shs_state, nvs_flash |
| **shs_zigbee** | `shs_zigbee.h`, `shs_zigbee.c` | EP/cluster creation, attribute write handler dispatch, report helpers, lock macros, signal handler, commissioning, connectivity monitoring | shs_state, shs_config, esp_zigbee |
| **shs_sensor** | `shs_sensor.h`, `shs_sensor.c` | LD2410/LD2450 callbacks, cross-validation, interference zone filtering, position smoothing/rate-limiting, zone occupancy evaluation, force-update | shs_state, ld2410, ld2450 |
| **shs_button** | `shs_button.h`, `shs_button.c` | Boot button task, triple-click detection, factory reset, LED feedback | shs_state, light_driver, esp_zigbee |
| **shs01** | `shs01.c` (main) | `app_main()` only: NVS init, create state, create tasks, wire callbacks, start. <100 lines. | All modules |

### What Does NOT Move

These stay as existing ESP-IDF components under `components/`:
- `ld2410_enhanced/` — LD2410C UART driver (already isolated)
- `ld2450/` — LD2450 UART driver with zone engine (already isolated)
- `light_driver/` — GPIO LED driver (already isolated)
- `zcl_utility/` — Zigbee cluster helper (already isolated)

`shs01.h` stays as the **constants header** (endpoint IDs, attribute IDs, cluster IDs). These are consumed by both `shs_zigbee` and the Z2M converter, so they must remain stable.

## Shared State Pattern

**Problem:** 100+ individual static globals with no synchronization.

**Solution:** Single struct + mutex + typed accessors.

```c
/* shs_state.h */

typedef struct {
    int16_t x1, y1, x2, y2;
    uint8_t type;       /* 0=off, 1=detection, 2=filter, 3=interference */
    bool    enabled;
    bool    occupied;
    uint8_t target_count;
} shs_zone_t;

typedef struct {
    /* LD2410 state */
    bool    moving_state;
    bool    static_state;
    bool    occupancy_state;
    uint32_t moving_cooldown_until;
    uint32_t static_cooldown_until;

    /* LD2450 state */
    bool     ld2450_occupancy;
    uint8_t  ld2450_target_count;
    ld2450_target_t targets[3];
    uint8_t  valid_target_count;

    /* Zone config + state (array replaces 30+ individual vars) */
    uint8_t  zone_type_global;      /* 0=disabled, 1=detection, 2=filter */
    shs_zone_t zones[5];

    /* Position smoothing (only used when position_reporting=true) */
    float    smoothed_x[3];
    float    smoothed_y[3];
    int16_t  last_reported_x[3];
    int16_t  last_reported_y[3];
    uint16_t last_reported_dist[3];
    uint32_t last_position_update_ms;

    /* Zigbee connection state */
    volatile bool zb_ready;
    volatile bool zb_connected;
    volatile bool zb_rejoin_pending;
    uint32_t last_successful_tx;

    /* Diagnostics */
    uint32_t lock_success_count;
    uint32_t lock_fail_count;
    uint32_t lock_consecutive_fails;
    uint32_t tx_success_count;
    uint32_t tx_fail_count;

    /* Config values */
    uint16_t movement_cooldown_sec;
    uint16_t occupancy_clear_sec;
    uint8_t  moving_sens_0_100;
    uint8_t  static_sens_0_100;
    uint16_t moving_max_gate;
    uint16_t static_max_gate;
    uint16_t sens_mv_0_10;
    uint16_t sens_st_0_10;
    bool     position_reporting;
    uint16_t min_moving_energy;
    uint16_t min_static_energy;
    char     firmware_version[20];

    /* Zone config debounce */
    uint32_t zone_cfg_pending_until;
    bool     zone_cfg_pending;
    bool     zone_cfg_save_needed;
} shs_state_t;

/* Global instance + mutex */
shs_state_t *shs_state_get(void);           /* Returns pointer (caller locks) */
SemaphoreHandle_t shs_state_mutex(void);     /* For fine-grained locking */

/* Convenience accessors for hot-path reads */
uint8_t  shs_state_target_count(void);       /* Atomic read, no mutex needed */
bool     shs_state_zb_ready(void);           /* Atomic read */
```

**Why a single struct:**
- One `xSemaphoreCreateMutex()` instead of scattered individual mutexes
- Zone data becomes `state->zones[i]` instead of 5 copies of everything
- Clear ownership: sensor callbacks write sensor fields, Zigbee reads them
- Config module writes config fields, sensor module reads them

**Locking strategy:**
- `target_data_mutex` (existing) → becomes `shs_state_mutex()` for target + zone state writes
- `volatile` for `zb_ready`/`zb_connected` — single-word atomic on ESP32-C6, no mutex needed
- Zigbee lock (`esp_zb_lock_acquire`) remains separate — it's the Zigbee stack's own lock
- Config fields: written only during init or from save_worker, read from sensor callbacks → mutex optional, but struct grouping makes it safe

## Data Flow

### Sensor → State → Zigbee (primary path)

```
LD2410C UART RX (20ms poll)          LD2450 UART RX (20ms poll)
       │                                    │
       ▼                                    ▼
ld2410_process()                     ld2450_process()
       │                                    │
       ▼                                    ▼
shs_on_state_change()                shs_on_ld2450_target_update()
[shs_sensor.c]                       [shs_sensor.c]
       │                                    │
       │  ┌─────────────────────┐           │
       ├──► cross-validate with  ◄──────────┤
       │  │ ld2450_target_count │           │
       │  └─────────────────────┘           │
       │                                    │
       ▼                                    ▼
mutex_take(state)                    mutex_take(state)
  state->occupancy = ...              state->targets[] = ...
  state->moving = ...                 state->ld2450_target_count = ...
mutex_give(state)                    mutex_give(state)
       │                                    │
       ▼                                    ▼
shs_zigbee_report_occupancy()        shs_zigbee_report_targets()
[shs_zigbee.c]                       [shs_zigbee.c]
       │                                    │
       ▼                                    ▼
esp_zb_lock_acquire()                esp_zb_lock_acquire()
esp_zb_zcl_set_attribute_val()       esp_zb_zcl_set_attribute_val()
esp_zb_zcl_report_attr_cmd_req()     esp_zb_zcl_report_attr_cmd_req()
esp_zb_lock_release()                esp_zb_lock_release()
```

### Zigbee → Config → Sensor (configuration path)

```
Z2M writes attribute
       │
       ▼
shs_zb_action_handler()         [shs_zigbee.c]
       │
       ▼
shs_zb_attribute_handler()      [shs_zigbee.c - dispatch switch]
       │
       ├─── sensor params ───► shs_config_set_*()     [shs_config.c]
       │                              │
       │                              ├─► state->moving_sens = ...
       │                              ├─► ld2410_set_all_sensitivity()
       │                              └─► shs_save_enqueue()
       │
       └─── zone params ────► shs_config_set_zone_*() [shs_config.c]
                                      │
                                      ├─► state->zones[i].x1 = ...
                                      └─► shs_zone_cfg_schedule_apply()
                                                │ (after 500ms debounce)
                                                ▼
                                          ld2450_set_zone()
                                          ld2450_apply_zones()
                                          shs_zone_cfg_save_to_nvs()
```

### NVS Save (background worker)

```
shs_save_enqueue(type, value)     [called from Zigbee task context]
       │
       ▼
shs_save_q (FreeRTOS queue)      [16 items, fire-and-forget]
       │
       ▼
shs_save_worker task (pri 3)     [shs_config.c]
       │
       ├─► IMMEDIATE: nvs_open → set → commit → close
       └─► DEBOUNCED: wait 500ms since last, then save
```

## Suggested Refactoring Order

Order matters because each module must compile and link independently before the next is extracted.

### Phase 1: Extract `shs_state` (foundation — zero logic)

**What:** Move all static globals into `shs_state_t` struct. Create `shs_state.h` (struct + accessors) and `shs_state.c` (instance + mutex creation). Replace `shs_zone1_x1`, `shs_zone2_x1`, ... with `state->zones[0].x1`, `state->zones[1].x1`.

**Why first:** Every other module depends on this. No behavior changes — pure data relocation. Zone array consolidation eliminates ~200 lines of copy-paste (5 zones × ~40 lines of load/save/apply each).

**Risk:** LOW. Mechanical search-and-replace. No concurrency changes yet.

**Dependencies:** None.

**Verification:** Compiles. All static globals removed from shs01.c.

### Phase 2: Extract `shs_config` (NVS persistence)

**What:** Move `shs_cfg_load_from_nvs`, `shs_zone_cfg_load_from_nvs`, `shs_zone_cfg_save_to_nvs`, `shs_cfg_save_u8/u16/i16`, `shs_save_worker` task, save queue, debounce logic. Config setters (`shs_config_set_sensitivity`, etc.) that update state + call sensor APIs.

**Why second:** Decouples persistence from application logic. Enables batch zone writes (one `nvs_open`/`nvs_commit`/`nvs_close` instead of per-field). The save worker task moves here.

**Risk:** LOW. NVS functions are self-contained.

**Dependencies:** shs_state.

**Verification:** NVS save/load works. Config changes via Z2M persist across reboots.

### Phase 3: Extract `shs_sensor` (callbacks + processing)

**What:** Move `shs_on_state_change`, `shs_on_ld2450_target_update`, `shs_on_ld2450_zone_update`, cross-validation logic, `is_valid_target`, `shs_point_in_zone`, `shs_target_in_interference_zone`, position smoothing/EMA, force-update functions.

**Why third:** This is where the concurrency bugs live. Extracting to its own file makes the mutex usage auditable. With state in a struct, adding proper mutex around `shs_ld2450_target_count` reads becomes straightforward.

**Risk:** MEDIUM. Must preserve callback registration order and cross-validation semantics. Test sensor reporting after extraction.

**Dependencies:** shs_state, ld2410, ld2450.

**Verification:** Sensor callbacks fire. Occupancy reported correctly. Zone filtering works.

### Phase 4: Extract `shs_zigbee` (endpoint + attribute handling)

**What:** Move all 25 endpoint/cluster creation, attribute write handler (the 300-line switch/case), report helpers (`shs_zb_set_analog_value`, `shs_zb_report_analog_attr`, etc.), lock macros, signal handler, commissioning logic, connectivity monitoring.

**Why fourth:** Largest single block (~1200 lines). Depends on both state and config being stable first. The attribute write handler calls into `shs_config_set_*()` functions (from Phase 2).

**Risk:** MEDIUM. Zigbee endpoint creation order matters. Signal handler timing is sensitive. Must test pairing and attribute read/write end-to-end.

**Dependencies:** shs_state, shs_config.

**Verification:** Z2M pairs. Attributes readable/writable. Reports received.

### Phase 5: Extract `shs_button` (trivial, do last)

**What:** Move `shs_boot_button_task`, `shs_flash_led`, click detection state machine.

**Why last:** Small (~100 lines), self-contained, low-risk. Just needs state for `position_reporting` toggle and `esp_zb_factory_reset()`.

**Risk:** LOW.

**Dependencies:** shs_state, light_driver, esp_zigbee.

**Verification:** Triple-click toggles config mode. Long-press factory resets.

### Final shs01.c (~80 lines)

After all extractions:

```c
void app_main(void) {
    nvs_flash_init();
    shs_state_init();                    // Create struct + mutex
    shs_config_load();                   // Load NVS → state
    light_driver_init(LIGHT_DEFAULT_OFF);

    shs_zigbee_start();                  // xTaskCreate(zigbee_task)
    shs_sensor_init();                   // Init LD2410 + LD2450, register callbacks
    shs_config_start_save_worker();      // xTaskCreate(save_worker)
    shs_config_load_zones();             // Load zone NVS
    shs_sensor_start_tasks();            // xTaskCreate(ld2410_task + ld2450_task)
    shs_button_start();                  // xTaskCreate(boot_button)
}
```

## Build Structure

All new modules go under `main/` (not as separate components) because they share internal state types:

```
main/
    CMakeLists.txt          ← add new .c files to SRC_DIRS
    shs01.c                 ← app_main only
    shs01.h                 ← constants (unchanged, public API)
    shs_state.h             ← shared state struct + accessors
    shs_state.c             ← state instance + mutex
    shs_config.h            ← config load/save/set API
    shs_config.c            ← NVS persistence + save worker
    shs_sensor.h            ← sensor callback registration API
    shs_sensor.c            ← callbacks + processing logic
    shs_zigbee.h            ← Zigbee init + report API
    shs_zigbee.c            ← endpoints + handlers + signals
    shs_button.h            ← button task start API
    shs_button.c            ← click detection + LED feedback
```

The existing `CMakeLists.txt` already uses `SRC_DIRS "."` so new `.c` files are auto-discovered.

**Why not components?** These modules share `shs_state_t` (an internal struct). ESP-IDF components have separate compilation units with explicit dependency declarations. Making each a component would force `shs_state` into a component too, with circular dependency between config↔sensor↔zigbee. Keeping them in `main/` avoids this while still achieving the compilation firewall via separate `.c` files.

## Patterns to Follow

### Pattern 1: Struct Array for Zones

Replace 30+ individual zone variables with an indexed array:

```c
/* BEFORE: 30 individual variables */
static bool    shs_zone1_enabled = false;
static int16_t shs_zone1_x1 = -1500;
/* ... repeat 5 times ... */

/* AFTER: single array */
shs_zone_t zones[5];  /* in shs_state_t */

/* Usage: */
for (int i = 0; i < 5; i++) {
    if (state->zones[i].enabled) {
        ld2450_set_zone(i, state->zones[i].x1, state->zones[i].y1,
                           state->zones[i].x2, state->zones[i].y2);
    }
}
```

**Eliminates:** ~200 lines of duplicate zone load/save/apply code. The attribute write handler switch/case shrinks from 90 zone cases to a computed-offset pattern.

### Pattern 2: Module Init Functions Return Handles

Each module exposes a minimal public API:

```c
/* shs_sensor.h */
void shs_sensor_init(shs_state_t *state);
void shs_sensor_start_tasks(void);
void shs_sensor_force_update_ld2410(void);
void shs_sensor_force_update_ld2450(void);
```

The `shs_state_t *` pointer is passed at init and stored module-internally. No global state leakage.

### Pattern 3: Report Functions Abstract Zigbee Lock

Sensor callbacks should not acquire the Zigbee lock directly. Instead:

```c
/* shs_zigbee.h - clean reporting API */
bool shs_zigbee_report_occupancy(uint8_t endpoint, bool occupied);
bool shs_zigbee_report_analog(uint8_t endpoint, float value);
bool shs_zigbee_report_binary(uint8_t endpoint, bool value);
```

This keeps the Zigbee lock acquisition pattern in one place. If retry-with-backoff is added later, it's a single change point.

## Anti-Patterns to Avoid

### Anti-Pattern 1: Moving to Separate Components

**What:** Creating `components/shs_config/`, `components/shs_sensor/`, etc.
**Why bad:** Circular dependencies. `shs_sensor` needs `shs_state`, `shs_zigbee` needs `shs_state`, and `shs_config` needs both. ESP-IDF component dependency system does not handle cycles well.
**Instead:** Keep all modules in `main/`. Use header includes for the compilation firewall.

### Anti-Pattern 2: Event Bus / Publish-Subscribe

**What:** Adding an event system between sensor and Zigbee.
**Why bad:** Over-engineering for a 5-task system. Adds latency, memory overhead (event queues), and debugging complexity. The current direct-call model is correct for real-time sensor reporting.
**Instead:** Direct function calls from sensor callbacks to `shs_zigbee_report_*()`.

### Anti-Pattern 3: Dynamic Allocation for State

**What:** Using `malloc()` for `shs_state_t`.
**Why bad:** ESP32-C6 has 320KB RAM. Static allocation is deterministic, debuggable, and avoids fragmentation.
**Instead:** Single static `shs_state_t` instance in `shs_state.c`.

### Anti-Pattern 4: Refactoring Logic During Extraction

**What:** Fixing the target_count race condition while moving code to shs_sensor.c.
**Why bad:** Mixing structural changes with behavioral changes makes bugs impossible to attribute. If something breaks, was it the refactoring or the fix?
**Instead:** Phase 1-5 are pure code moves. Bug fixes are separate phases that happen before or after.

## Scalability Considerations

| Concern | Current (25 EPs) | Future (if more sensors) |
|---------|-------------------|--------------------------|
| Endpoint count | 25 endpoints, static | Would need endpoint factory pattern |
| Zone count | 5 zones, hardcoded array | Increase `SHS_MAX_ZONES` constant |
| Target count | 3 targets (LD2450 hardware limit) | Fixed by sensor hardware |
| RAM usage | ~8KB for state + stacks | ~320KB available, ample headroom |
| NVS writes | Per-field writes | Batch writes per config change (Phase 2 improvement) |
| Zigbee traffic | Report per change | Already rate-limited (300ms for position) |

## Sources

- Direct analysis of `main/shs01.c` (2691 lines, current codebase)
- ESP-IDF component build system: `SRC_DIRS "."` auto-discovers `.c` files
- ESP-IDF FreeRTOS task model: single-core on ESP32-C6, cooperative multitasking
- Espressif Zigbee SDK: `esp_zb_lock_acquire/release` pattern for thread safety
- ESP32-C6 technical reference: 320KB SRAM, single RISC-V core
