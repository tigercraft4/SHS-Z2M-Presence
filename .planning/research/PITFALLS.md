# Domain Pitfalls

**Domain:** ESP32-C6 Zigbee presence sensor firmware — optimization, bug fixing, refactoring  
**Researched:** 2026-04-30  

## Critical Pitfalls

Mistakes that cause crashes, bricked devices, or require full re-pairing.

---

### Pitfall 1: Race Conditions on Shared State Between FreeRTOS Tasks

**What goes wrong:** Multiple tasks (zigbee, ld2410, ld2450, save_worker) read/write the same static globals without synchronization. Variables like `shs_ld2450_target_count`, `shs_zone*_occupied`, `shs_occupancy_state`, and all zone config variables are accessed from both sensor callbacks (running in sensor task context) and the Zigbee task. The mutex only protects `current_targets[]` — the 60+ other statics are unprotected. Tearing occurs on 16-bit and 32-bit values on the 32-bit RISC-V core (reads during partial writes), producing garbage occupancy states or stale cooldown timestamps.

**Why it happens:** The original code grew organically. One mutex was added for `target_data_mutex` but the pattern wasn't extended to other shared state. ESP32-C6 is single-core RISC-V, which creates a false sense of safety — pre-emption still happens at any tick boundary, and sensor callbacks run in task context at priority 4 while Zigbee runs at priority 5.

**Consequences:**
- Occupancy reported as detected when room is empty (or vice versa)
- Zone target counts flicker between correct and zero
- Cooldown logic uses half-updated timestamps, causing premature or delayed occupancy clearing
- `shs_zb_connected` and `shs_zb_rejoin_pending` (volatile bool) are not atomic across compound read-modify-write sequences

**Warning signs:**
- Occupancy state oscillates rapidly in Z2M logs without physical presence change
- Target count jumps between valid values and zero
- `ESP_LOGW` about "report FAILED - will retry" followed by stale state

**Prevention:**
- Group related state into structs, protect each struct with a single mutex
- Sensor callbacks write to a "pending" struct; a dedicated update function copies to "active" under mutex
- Use `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()` for very short reads of simple flags (connected, ready)
- For the refactoring phase: define clear ownership — each variable written by exactly one task, read by others through accessor functions with mutex

**Which phase should address it:** Bug fix phase (first priority — the PROJECT.md already identifies uninitialized mutex as the #1 crash bug). Struct grouping happens during the module refactoring phase.

**Confidence:** HIGH — directly observed in codebase analysis. Lines 46-48, 186-201, 126-145 show unprotected shared state.

---

### Pitfall 2: NVS Flash I/O in Sensor Callback Context

**What goes wrong:** `shs_zone_cfg_save_to_nvs()` performs 31 sequential NVS writes (open, 31× set, commit, close) which takes 50-200ms of blocking I/O on the SPI flash. When called from the Zigbee attribute handler (which runs in the Zigbee task at priority 5), it blocks the Zigbee stack, causing missed ZCL frames, report timeouts, and coordinator disconnection. The save worker exists for basic config but zone config bypasses it entirely.

**Why it happens:** Zone config save was added after the save worker pattern was established for basic config. The debounce flag (`shs_zone_cfg_save_needed`) defers the actual write to `shs_zone_cfg_check_pending()` in the LD2450 task loop — but that task runs at priority 4, meaning it can still block if NVS flash is contended with the save_worker task doing its own writes simultaneously.

**Consequences:**
- Zigbee stack misses incoming attribute writes during zone config save (user changes two zones, second one is lost)
- NVS flash wear — each zone config change writes 31 keys individually instead of batching
- ESP32-C6 has 24KB NVS partition; 31 keys × ~40 bytes = ~1.2KB per zone save. NVS page size is 4KB; frequent writes cause page compaction stalls
- Two different NVS namespaces (`"cfg"` and `"shs_cfg"`) with separate open/close cycles add overhead

**Warning signs:**
- Zone configuration changes from Z2M sometimes "don't stick" after reboot
- Zigbee lock timeout warnings spike during zone configuration
- Flash write latency visible in `esp_timer` deltas

**Prevention:**
- Route ALL NVS writes through the save_worker task queue
- Batch zone config: single nvs_open, all writes, single commit, single close
- Unify the two NVS namespaces (`"cfg"` and `"shs_cfg"`) into one — two handles means two page sets
- Consider a single binary blob for zone config (`nvs_set_blob`) instead of 31 individual keys

**Which phase should address it:** Bug fix phase (NVS in callbacks) → Optimization phase (batching, namespace unification).

**Confidence:** HIGH — directly observed. Lines 365-430 show 31 individual NVS writes; lines 285-304 show the blocking helpers.

---

### Pitfall 3: Zigbee Lock Starvation Causing Silent Data Loss

**What goes wrong:** The `SHS_ZB_LOCK_ACQUIRE_OR_RETURN()` macro silently drops updates when lock acquisition fails (100ms timeout). There is no retry mechanism or queuing — when a sensor callback can't acquire the lock, the attribute update is silently lost. The only evidence is a log message, which is itself throttled (only on count 1, 10, 50, then every 100).

**Why it happens:** The Zigbee stack's internal lock (`esp_zb_lock_acquire`) is contended by: (1) the Zigbee stack itself processing frames, (2) sensor callbacks trying to report, (3) the force update functions during rejoin. The 100ms timeout is too short when the Zigbee stack is processing a multi-attribute config write from Z2M (which can hold the lock for 200+ ms while processing the cluster write callback).

**Consequences:**
- Occupancy changes are lost during Zigbee network maintenance (route discovery, link status)
- Zone state desynchronizes between firmware and Z2M
- The "only update local state if Zigbee report succeeds" pattern (lines ~960-970) means a failed report leaves firmware in an inconsistent state where the sensor detected a change but the state variable wasn't updated — so the next callback won't try again because the values appear unchanged

**Warning signs:**
- `shs_lock_fail_count` climbs steadily
- Z2M shows stale occupancy state that doesn't match physical presence
- After a Zigbee rejoin, some endpoints show correct state but others are stuck

**Prevention:**
- Implement a retry queue: failed updates go into a ring buffer, retried on next successful lock acquisition
- Separate "sensor truth" state from "last reported to Zigbee" state — compare against sensor truth, not Zigbee-reported state
- Increase lock timeout for non-time-critical updates (zone config can wait 500ms)
- Consider `esp_zb_scheduler_alarm()` to schedule attribute updates from within the Zigbee task context, avoiding lock contention entirely

**Which phase should address it:** Bug fix phase (retry with backoff is already in PROJECT.md Active requirements).

**Confidence:** HIGH — directly observed in macro at lines 148-163 and the conditional state update pattern at lines 960-976.

---

### Pitfall 4: Refactoring Monolithic File Breaks Zigbee Endpoint Registration Order

**What goes wrong:** When splitting `shs01.c` (2691+ lines) into modules, developers change the order in which Zigbee endpoints are registered, or accidentally duplicate/skip endpoint IDs. The Zigbee stack requires endpoints to be registered before `esp_zb_start()`, and the endpoint list ordering affects the cluster interview sequence. Changing registration order causes Z2M to receive endpoints in a different order during interview, which can make it associate clusters with wrong endpoints.

**Why it happens:** The 25-endpoint registration block is ~600 lines of repetitive boilerplate. When extracting to a separate module (`zigbee_endpoints.c`), copy-paste errors change the order. The endpoint IDs are non-sequential (1,2,3,4,5,6,7,22,23,8-16,17,18,19,20,21,24,25) which makes reordering tempting.

**Consequences:**
- Z2M shows occupancy on wrong endpoint after firmware update
- Existing Z2M device pairings break → requires delete and re-pair on every update
- External converter mapping becomes inconsistent

**Warning signs:**
- After flashing updated firmware, Z2M shows "Interview failed" or wrong entity names
- Cluster list in Z2M device page shows different order than before

**Prevention:**
- Extract endpoint registration to its own function, but keep the EXACT call order
- Add a compile-time assertion or runtime check that endpoint IDs match expected values
- Document the endpoint ID scheme and why IDs 22-25 are non-sequential (added after initial 1-21 range)
- Test: after refactoring, do a Z2M re-interview and compare cluster layout with pre-refactor

**Which phase should address it:** Module refactoring phase.

**Confidence:** HIGH — observed that endpoint registration is the most fragile part of the Zigbee setup. The non-sequential ID assignment (EP22-25 added later) confirms organic growth.

---

### Pitfall 5: Changing Zigbee Attribute Types or Cluster IDs Breaks Z2M External Converter

**What goes wrong:** During refactoring, a developer changes an attribute's ZCL type (e.g., from `uint16` to `int16` for zone coordinates) or modifies the manufacturer-specific cluster ID (`0xFDCD`). The firmware compiles fine, but the Z2M external converter (`shs01_enhanced.js`/`.mjs`) has hardcoded attribute IDs, types, and endpoint mappings. Any mismatch causes Z2M to silently ignore the attribute or parse it with wrong endianness/signedness.

**Why it happens:** The firmware and Z2M converter are maintained as separate files with no shared schema or contract. The converter uses magic numbers (`0xFDCD`, `0x0001`-`0x0009`) that must exactly match the firmware's `#define` values.

**Consequences:**
- Configuration changes from Z2M have no effect (firmware receives garbage values)
- Sensor data appears in Z2M but with wrong values (signed/unsigned confusion)
- The coordinate biasing hack (adding 3000 to X to avoid negative floats) is deeply embedded in both firmware and converter — removing it from one side without the other breaks position display

**Warning signs:**
- Z2M debug logs show "Unknown attribute" or unexpected values
- Position coordinates suddenly show as negative or offset by 3000mm
- Configuration sliders in Z2M have no visible effect on sensor behavior

**Prevention:**
- Create a shared `attribute_map.h` (or comment block) that documents every attribute ID, type, and range — reference it from both firmware and converter
- When changing attribute encoding (e.g., removing the +3000 bias), update BOTH firmware AND converter atomically
- Test attribute round-trip: set value in Z2M → verify firmware received correct value → verify Z2M displays firmware's reported value correctly
- Version the config cluster: add a firmware version attribute that the converter can check

**Which phase should address it:** The "Remove coordinate biasing hack" task specifically, and any phase that touches attribute definitions.

**Confidence:** HIGH — the biasing hack at line ~1040 and the dual converter files (`.js` + `.mjs`) demonstrate this exact coupling.

---

### Pitfall 6: Stack Overflow in FreeRTOS Tasks During Refactoring

**What goes wrong:** When adding code to existing tasks (logging, error handling, new data structures), the fixed stack sizes overflow. Current allocations: zigbee_task=8192, ld2410_task=4096, ld2450_task=4096, boot_button=8192, save_worker=3072. The ESP32-C6 RISC-V port uses more stack than Xtensa (ESP32/S3) for the same code due to larger register save frames.

**Why it happens:** Stack sizes were tuned for the current code. Adding string formatting for diagnostic messages, additional local variables for zone processing, or calling new functions that have their own stack frames pushes past the limit. The overflow is silent — FreeRTOS stack canary detection only works if `configCHECK_FOR_STACK_OVERFLOW` is enabled, and even then it only catches overflow at context switch, not mid-function.

**Consequences:**
- Random crashes with no clear backtrace (stack corruption)
- Variables in adjacent memory get corrupted — symptoms appear in unrelated code
- Hard fault during NVS operations (which use significant stack for flash I/O internally)

**Warning signs:**
- Crash during string formatting (`ESP_LOGI` with multiple format specifiers)
- Guru Meditation Error with `StoreProhibited` or `LoadProhibited` on RISC-V
- Task works in debug build (larger stack due to debug symbols) but crashes in release

**Prevention:**
- Enable `CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2` (canary method) in sdkconfig
- After refactoring, use `uxTaskGetStackHighWaterMark()` to measure actual usage — add 25% margin
- save_worker at 3072 bytes is tight for NVS batch operations — increase to 4096 if adding zone batch writes
- Avoid large local arrays in task functions; use heap or static allocation for buffers >256 bytes

**Which phase should address it:** All phases — stack size validation should be a standard check after every task modification.

**Confidence:** HIGH — the save_worker at 3072 is already borderline for NVS operations that internally use ~1KB stack.

---

## Moderate Pitfalls

### Pitfall 7: NVS Namespace Inconsistency

**What goes wrong:** The firmware uses two different NVS namespaces: `"cfg"` (for basic sensor config) and `"shs_cfg"` (for zone config). These are opened/closed independently. If `nvs_flash_erase()` is called during factory reset, both are wiped. But if NVS compaction occurs, they compete for pages. A factory reset via boot button might erase one namespace's data but fail to properly reset the other's runtime state.

**Prevention:** Unify into a single namespace. Add a "config version" key to detect schema migrations after firmware updates. Ensure factory reset clears runtime state AND NVS atomically.

---

### Pitfall 8: Sensor Task Initialization Order Dependencies

**What goes wrong:** The current boot sequence has implicit timing dependencies: Zigbee task starts first (priority 5), then LD2410 init, then LD2450 init, then save_worker, then zone config load, then sensor tasks. The LD2450 task has a hardcoded `vTaskDelay(2500)` at startup to "wait for sensor to stabilize." If this timing changes (e.g., Zigbee takes longer to start, or zone config load is slower due to more NVS keys), the sensor starts before zone config is applied.

**Prevention:**
- Replace hardcoded delays with event-driven synchronization (FreeRTOS event groups or task notifications)
- Define explicit "ready" signals: Zigbee ready → sensor config apply → sensor tasks start
- The `shs_zb_ready` flag is a step in this direction but is checked via polling, not event-driven

---

### Pitfall 9: 100+ Static Globals Make Testing Impossible

**What goes wrong:** Every piece of state is a file-scope `static` variable in `shs01.c`. After splitting into modules, these become either (a) non-static globals with `extern` declarations (creating tight coupling) or (b) opaque state passed through function parameters (clean but requires significant refactoring of every function signature).

**Prevention:**
- Define state structs: `sensor_state_t`, `zigbee_state_t`, `zone_config_t`, `diagnostics_t`
- Each module owns its struct, exposes accessor functions
- Don't try to refactor all 100+ variables at once — group by module, move one module at a time
- Keep backward compatibility: during transition, the old static can wrap the new struct member

---

### Pitfall 10: Disabling Filters/Smoothing Without Remembering to Re-enable

**What goes wrong:** The codebase has `EMA_ALPHA = 1.0f` with a comment "DIAGNOSTIC: Disabled for raw data testing" and energy filtering wrapped in `#if 0`. These diagnostic changes ship in production firmware. During refactoring, these get preserved because developers treat existing code as "working" and don't audit diagnostic overrides.

**Prevention:**
- Use `sdkconfig` Kconfig options for diagnostic toggles (not source-code `#if 0`)
- Create a pre-release checklist: search for `DIAGNOSTIC`, `TODO`, `HACK`, `WORKAROUND`, `#if 0`
- The EMA_ALPHA and min_energy filters are specifically called out in PROJECT.md Active requirements — prioritize them

---

### Pitfall 11: Zigbee Stack Memory Budget on ESP32-C6

**What goes wrong:** ESP32-C6 has 512KB SRAM. The Zigbee stack (ZBOSS) is a major consumer (~60-80KB). With 25 endpoints, each with its own cluster list and attribute storage, the Zigbee memory footprint is significantly larger than a typical 2-3 endpoint device. Adding more attributes, longer strings, or deeper cluster hierarchies during refactoring can exhaust heap.

**Prevention:**
- Monitor `esp_get_free_heap_size()` and `esp_get_minimum_free_heap_size()` at boot and periodically
- The 1536KB app partition (partitions.csv) is generous for code, but RAM is the constraint
- When adding diagnostic endpoints or attributes, verify heap doesn't drop below 30KB (Zigbee needs ~20KB for network operations)
- Consider removing unused attributes rather than adding new ones during optimization

---

### Pitfall 12: Force Update Flooding After Rejoin

**What goes wrong:** After Zigbee rejoin, `shs_ld2450_force_update()` and `shs_ld2410c_force_update()` send ~15 attribute reports in rapid succession with only 10ms delays between lock acquisitions. This floods the coordinator's APS queue. If the coordinator is busy (processing other devices), frames are dropped and some endpoints show stale data.

**Prevention:**
- Increase inter-report delay to 50-100ms during force update
- Implement progressive sync: report critical endpoints first (EP2/EP3 occupancy), then zones, then optional data
- Use reporting configuration (min/max reporting intervals) instead of explicit report commands where possible

---

## Minor Pitfalls

### Pitfall 13: Duplicate Zone Check Logic

**What goes wrong:** Zone point-in-rect checking is implemented in THREE places: `ld2450_point_in_zone()` (component), `shs_point_in_zone()` (main), and inline checks in `shs_target_in_interference_zone()`. During refactoring, a bug fix applied to one copy doesn't propagate to others.

**Prevention:** Consolidate to a single implementation in the LD2450 component. Remove the duplicate in main.

---

### Pitfall 14: Hardcoded Coordinator Address (0x0000)

**What goes wrong:** All attribute report commands target `dst_addr_u.addr_short = 0x0000` and `dst_endpoint = 1`. This assumes a single coordinator at address 0 with endpoint 1. While this is standard for Z2M, it fails with Zigbee networks using multiple coordinators or non-standard coordinator configurations.

**Prevention:** Use binding table-based reporting instead of hardcoded destination. Configure reporting through standard ZCL reporting configuration. Lower priority since Z2M is the only target, but important for future-proofing.

---

### Pitfall 15: Boot Button Task Stack Size Over-Allocation

**What goes wrong:** `shs_boot_button_task` is allocated 8192 bytes — the same as the Zigbee task — but it only polls a GPIO and counts presses. This wastes ~4KB of RAM that could be used by heap.

**Prevention:** Reduce to 2048 bytes. Verify with high-water mark measurement.

---

## Phase-Specific Warnings

| Phase Topic | Likely Pitfall | Mitigation |
|-------------|---------------|------------|
| Fix uninitialized mutex | Pitfall 1 — must create mutex BEFORE registering callbacks | Move `xSemaphoreCreateMutex()` before `ld2450_init()` |
| Remove NVS from callbacks | Pitfall 2 — route through save_worker queue | Extend save_worker queue message types for zone config |
| Zigbee lock retry | Pitfall 3 — retry queue must be bounded | Use ring buffer (8-16 entries), drop oldest on overflow |
| Module refactoring | Pitfalls 4, 9 — endpoint order and global state | Extract one module at a time, test Z2M interview each time |
| Remove coordinate bias | Pitfall 5 — must update firmware AND converter atomically | Flash firmware + deploy converter in same maintenance window |
| Re-enable smoothing | Pitfall 10 — test with real sensor data, not just compile | Verify EMA_ALPHA=0.3f doesn't cause latency >500ms in occupancy response |
| Add diagnostics | Pitfalls 6, 11 — stack and heap budget | Add `uxTaskGetStackHighWaterMark()` logging before adding diagnostic features |
| Sensor health check | Pitfall 8 — health check must handle startup race | Use event group bit for "sensor initialized" before starting health monitoring |

## Sources

- Direct codebase analysis of `shs01.c` (2691+ lines), `ld2450.c`, `ld2410_enhanced.c`
- ESP-IDF FreeRTOS documentation — task stack sizing on RISC-V vs Xtensa
- ESP-IDF NVS documentation — page compaction behavior, namespace isolation
- Espressif Zigbee SDK (`esp_zigbee_core`) — lock semantics, endpoint registration
- ESP32-C6 Technical Reference Manual — 512KB SRAM budget, single-core RISC-V pre-emption model
- PROJECT.md Active requirements — known bugs and planned fixes

**All pitfalls verified through direct source code analysis — HIGH confidence overall.**
