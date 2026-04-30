# Project Research Summary

**Project:** SHS-Z2M-Presence — Optimization & Hardening
**Domain:** ESP32-C6 Zigbee mmWave presence sensor firmware
**Researched:** 2026-04-30
**Confidence:** HIGH

## Executive Summary

SHS-Z2M-Presence is a multi-sensor (LD2410C + LD2450) presence detection firmware running on a single-core ESP32-C6 RISC-V MCU with Zigbee connectivity. The codebase is functional but has grown organically into a 2691-line monolith (`shs01.c`) with critical concurrency bugs, silent data loss on Zigbee reporting, and disabled safety filters that shipped in production. The primary goal of this milestone is hardening — not new features — and the research confirms this is the correct priority. The device needs to work reliably 24/7 before anything else.

The recommended approach is: **fix bugs first, then restructure, then add observability**. Race conditions on shared state between 5 FreeRTOS tasks are the root cause of the most severe user-visible issues (phantom occupancy, stale zone states, flickering target counts). The uninitialized mutex bug (Pitfall #1) and silent Zigbee lock failures (Pitfall #3) must be addressed before any refactoring, because restructuring broken code doubles the risk of introducing regressions. The stack is locked — no dependency changes needed, only better usage of existing ESP-IDF APIs (mutexes, task notifications, batched NVS writes, Zigbee lock retry).

Key risks are: (1) breaking Zigbee endpoint registration order during refactoring, which forces users to re-pair devices; (2) changing attribute types without updating the Z2M external converter atomically, which silently breaks the user interface; and (3) stack overflow when adding diagnostic code to existing tasks with tight stack allocations (save_worker at 3072B is already borderline). All three are avoidable with the phased approach and verification steps detailed below.

## Key Findings

### Recommended Stack

No stack changes — all technologies are locked. Research focused on optimal use of existing APIs.

**Core technologies (unchanged):**
- **ESP-IDF ≥5.0.0** (recommend pinning ≥5.3.0 for Zigbee SDK stability): Core SDK providing FreeRTOS, NVS, UART, GPIO
- **esp-zboss-lib / esp-zigbee-lib ~1.6.0**: Zigbee stack with single-threaded main loop and lock-based thread safety
- **FreeRTOS (IDF v10.5.1-based)**: RTOS kernel — single-core mode (`UNICORE`) simplifies concurrency (spinlocks degenerate to interrupt-disable)
- **ESP32-C6**: Single-core RISC-V — pre-emption still occurs at tick boundaries, mutexes still required for task-level exclusion

**Key API patterns identified:**
- `xSemaphoreCreateMutex()` with priority inheritance (not binary semaphores) for shared data
- `esp_zb_lock_acquire` with retry/backoff instead of silent return-on-fail
- `esp_zigbee_task_queue_post()` for non-urgent Zigbee updates (avoids lock contention)
- Batched NVS writes: single `nvs_open`/`nvs_commit`/`nvs_close` instead of per-key cycles
- `xTaskDelayUntil` for fixed-period sensor polling (replaces drifting `vTaskDelay`)

### Expected Features

**Must have (table stakes — broken or missing):**
- Thread-safe shared state — mutex init before callbacks, consistent locking across all 60+ shared globals
- No blocking NVS I/O in sensor callbacks — route ALL writes through save_worker
- Zigbee TX retry with backoff — eliminate silent data loss on lock failure
- Sensor health monitoring — LD2410C has zero health tracking; LD2450 has partial watchdog
- Task watchdog integration — no custom tasks are subscribed to ESP-IDF TWDT
- Re-enable position smoothing (EMA_ALPHA back to 0.3f) and energy-based false positive filtering
- Standalone LD2410 fallback when LD2450 goes offline
- Fix int16 coordinate encoding (remove +4000 bias hack)
- NVS write batching for zone config (31 individual writes → 1 commit)

**Should have (differentiators, if time):**
- Zigbee-exposed diagnostic counters (lock stats, TX stats, UART frame stats, uptime)
- Sensor firmware version reporting over Zigbee
- Restart reason tracking (`esp_reset_reason()`)
- Stack high-water mark logging for right-sizing allocations
- Modular code structure (split monolith into 5 focused modules)

**Defer (anti-features for this milestone):**
- OTA firmware update, power management, Zigbee binding/scenes
- Web config UI, BLE configuration, per-gate energy exposure
- Configuration backup/restore, tamper detection, multi-coordinator support

### Architecture Approach

The monolithic `shs01.c` should be split into 5 modules under `main/` (NOT as separate components — circular dependencies prevent clean component boundaries). A central `shs_state_t` struct replaces 100+ scattered static globals, protected by a single mutex. Zone config consolidates from 30+ individual variables to a `shs_zone_t zones[5]` array, eliminating ~200 lines of duplicate code.

**Major components:**
1. **shs_state** (`shs_state.h/.c`) — Shared state struct, mutex creation, typed accessors. Zero logic. Foundation for everything else.
2. **shs_config** (`shs_config.h/.c`) — NVS load/save (batched), save_worker task, config setters, debounce logic. Owns persistence.
3. **shs_sensor** (`shs_sensor.h/.c`) — LD2410/LD2450 callbacks, cross-validation, zone occupancy, position smoothing, force-update. Writes sensor state through mutex.
4. **shs_zigbee** (`shs_zigbee.h/.c`) — 25 endpoint creation (exact order preserved), attribute write handler dispatch, report helpers, lock macros, signal handler. Reads state, reports to coordinator.
5. **shs_button** (`shs_button.h/.c`) — Boot button task, triple-click/long-press state machine, LED feedback. ~100 lines, trivial.

**Post-refactor `shs01.c`:** ~80 lines — `app_main()` only: init state → load config → start Zigbee → init sensors → start tasks.

### Critical Pitfalls

1. **Race conditions on shared state** — 60+ unprotected globals accessed by 5 tasks. Fix: struct grouping + mutex. Create mutex BEFORE registering any callbacks. *Phase: bug fixes (first priority).*
2. **NVS flash I/O in callback context** — Zone config save performs 31 sequential NVS writes blocking for 50-200ms, causing UART frame loss and Zigbee timeouts. Fix: route all writes through save_worker, batch with single `nvs_commit()`. *Phase: bug fixes.*
3. **Zigbee lock starvation → silent data loss** — 100ms timeout macro silently drops updates; state variable not updated on failure so retries don't happen. Fix: retry with backoff + separate "sensor truth" from "last reported" state. *Phase: bug fixes.*
4. **Endpoint registration order** — Reordering the 25 non-sequential endpoint IDs (1-7,22,23,8-21,24,25) during refactoring breaks Z2M interview and forces re-pairing. Fix: extract to function preserving exact order, add runtime assertions. *Phase: refactoring.*
5. **Firmware↔Converter attribute contract** — Any change to attribute types/IDs/encoding must be mirrored in `shs01_enhanced.mjs`. The +4000 coordinate bias is embedded in both. Fix: document attribute map, update both sides atomically. *Phase: any attribute change.*
6. **Stack overflow during refactoring** — save_worker at 3072B is borderline for NVS batch operations. RISC-V uses more stack than Xtensa. Fix: enable `CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2`, measure with `uxTaskGetStackHighWaterMark()`, add 25% margin. *Phase: all phases.*

## Implications for Roadmap

### Phase 1: Critical Bug Fixes
**Rationale:** Must fix data corruption and silent data loss before any restructuring. Broken foundations make refactoring impossible to verify.
**Delivers:** Stable concurrency, reliable Zigbee reporting, safe NVS persistence.
**Addresses:** Thread-safe shared state, NVS out of callbacks, Zigbee TX retry with backoff, mutex initialization order.
**Avoids:** Pitfalls 1, 2, 3 — the three critical crash/data-loss issues.

### Phase 2: Re-enable Disabled Safety Features
**Rationale:** Position smoothing (EMA) and energy filtering are already implemented but disabled. Quick wins with major impact on false-positive rate. Low risk because code exists and was tested previously.
**Delivers:** Reduced phantom occupancy, cleaner position data, proper int16 coordinate encoding.
**Addresses:** EMA_ALPHA re-enable, energy-based filtering, +4000 bias removal (with converter update).
**Avoids:** Pitfall 5 — converter must be updated atomically with firmware for the bias change.

### Phase 3: Reliability Infrastructure
**Rationale:** Task watchdog, sensor health monitoring, and NVS batching prevent silent failures. These are table stakes for 24/7 operation but lower urgency than data-loss bugs.
**Delivers:** TWDT integration, LD2410C health monitoring, LD2450 fallback mode, batched NVS zone config writes.
**Addresses:** Task watchdog, sensor health monitoring, standalone LD2410 fallback, NVS write batching.
**Avoids:** Pitfalls 6 (stack overflow — measure first), 8 (init order — use event-driven sync), 11 (heap budget — monitor before adding features).

### Phase 4: Shared State Restructuring
**Rationale:** Foundation for modular refactoring. Replace 100+ static globals with `shs_state_t` struct and zone array. Pure data relocation — no behavioral changes. Must come before module extraction.
**Delivers:** `shs_state.h/.c` with struct, mutex, accessors. All zone variables consolidated to `zones[5]` array.
**Avoids:** Pitfall 9 — move one set of globals at a time, verify compilation at each step.

### Phase 5: Module Extraction
**Rationale:** Depends on stable state struct (Phase 4). Extract modules in dependency order: config → sensor → zigbee → button. One module at a time, test Z2M interview after each.
**Delivers:** 5 focused modules under `main/`, `shs01.c` reduced to ~80 lines.
**Avoids:** Pitfall 4 — preserve exact endpoint registration order; Pitfall 9 — extract one module at a time.

### Phase 6: Observability & Diagnostics
**Rationale:** Only after the codebase is stable and modular. Adding diagnostic attributes to a broken monolith would make debugging harder, not easier.
**Delivers:** Zigbee-exposed lock/TX/UART counters, restart reason, sensor firmware versions, stack high-water mark logging.
**Avoids:** Pitfalls 6, 11 — verify stack and heap headroom before adding attributes.

### Phase Ordering Rationale

- **Bug fixes before refactoring** (Anti-Pattern 4 from ARCHITECTURE.md): mixing structural and behavioral changes makes regressions unattributable. Fix the concurrency bugs in the monolith where the behavior is well-understood, then move code.
- **Safety features before infrastructure**: re-enabling existing filters (Phase 2) is lower risk than adding new monitoring systems (Phase 3). Quick wins first.
- **State restructuring before module extraction**: every module depends on `shs_state_t`. Extracting modules before the struct exists means each module creates its own state pattern — inconsistency.
- **Diagnostics last**: adding observability to a stable, modular codebase is trivial. Adding it to a broken monolith creates more code to move later.

### Research Flags

Phases likely needing deeper research during planning:
- **Phase 1 (Bug Fixes):** Zigbee lock retry strategy — the `esp_zigbee_task_queue_post` vs retry-queue tradeoff needs prototyping with actual lock contention measurements.
- **Phase 3 (Reliability):** LD2410C health monitoring — no existing pattern in codebase; needs research on what LD2410 failure modes look like over UART.
- **Phase 5 (Module Extraction):** Endpoint registration order preservation — needs careful mapping of current order before extraction begins.

Phases with standard patterns (skip research-phase):
- **Phase 2 (Re-enable Filters):** Code exists, just needs constant changes and `#if 0` removal. Well-understood.
- **Phase 4 (State Restructuring):** Mechanical search-and-replace. Standard embedded C pattern.
- **Phase 6 (Diagnostics):** Standard Zigbee attribute addition. Counters already tracked internally.

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | All technologies locked. Research focused on API usage patterns from official ESP-IDF/Zigbee docs. |
| Features | HIGH | Based on direct codebase analysis — bugs confirmed in source, disabled features verified. Commercial sensor comparison (SONOFF, Tuya, Aqara) validates table stakes. |
| Architecture | HIGH | Module split designed from actual code analysis (2691 lines mapped). Build system verified (`SRC_DIRS "."` auto-discovers). Anti-patterns identified from ESP-IDF component system constraints. |
| Pitfalls | HIGH | All 15 pitfalls verified through direct source code inspection with line references. Phase-specific warnings mapped to mitigations. |

**Overall confidence:** HIGH

### Gaps to Address

- **Zigbee lock contention measurement:** No data on actual lock hold times during commissioning or route discovery. The 100ms vs 500ms timeout debate needs empirical testing. Address during Phase 1 planning with `esp_timer`-based lock timing.
- **LD2410C failure modes:** Unlike LD2450 (which has frame watchdog and error counters), LD2410C health monitoring has no prior art in this codebase. Need to characterize UART-level failures during Phase 3 planning.
- **Heap budget after 25 endpoints:** No current measurement of `esp_get_minimum_free_heap_size()` after full Zigbee initialization. Must verify >30KB free before adding diagnostic attributes in Phase 6.
- **Z2M converter testing:** The converter file (`shs01_enhanced.mjs`) is maintained separately. No automated test for firmware↔converter attribute contract. Manual verification required for Phase 2 (bias removal).

## Sources

### Primary (HIGH confidence)
- ESP-IDF v6.0.1 FreeRTOS docs — task notifications, mutex, single-core behavior
- ESP-IDF v6.0.1 NVS docs — page/entry structure, commit semantics, thread safety
- ESP Zigbee SDK developing guide — lock API (`esp_zb_lock_acquire`), `esp_zigbee_task_queue_post`, endpoint registration
- Direct codebase analysis — `main/shs01.c` (2691 lines), `main/shs01.h`, `ld2450.c`, `ld2410_enhanced.c`
- ESP32-C6 Technical Reference Manual — 512KB SRAM, single-core RISC-V, `UNICORE` mode

### Secondary (MEDIUM confidence)
- ESPHome LD2410 component — settle filters, throttling patterns, engineering mode (reference implementation, not direct dependency)
- Commercial sensor comparison (SONOFF SNZB-06P, Tuya ZG-205ZL, Aqara FP2) — feature baseline validation

---
*Research completed: 2026-04-30*
*Ready for roadmap: yes*
