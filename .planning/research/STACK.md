# Technology Stack

**Project:** SHS-Z2M-Presence — Optimization & Hardening
**Researched:** 2026-04-30
**Focus:** ESP-IDF FreeRTOS concurrency, Zigbee SDK resource management, NVS optimization

## Current Stack (Locked — No Changes)

| Technology | Version | Purpose | Status |
|------------|---------|---------|--------|
| ESP-IDF | ≥5.0.0 (stable v6.0.1 available) | Core SDK | Locked |
| esp-zboss-lib | ~1.6.0 | Zigbee ZBOSS stack | Locked |
| esp-zigbee-lib | ~1.6.0 | Zigbee application API | Locked |
| led_strip | ~2.0.0 | Status LED driver | Locked |
| FreeRTOS (IDF) | v10.5.1-based | RTOS kernel | Locked (part of IDF) |
| ESP32-C6 | Single-core RISC-V | Target MCU | Locked (hardware) |

> **Note:** This milestone does NOT change the stack. It optimizes usage of the existing stack. All recommendations below are patterns/APIs within the current dependencies.

## ESP32-C6 Single-Core Implications

**Critical fact:** ESP32-C6 is a **single-core** device. `CONFIG_FREERTOS_UNICORE` is always enabled. This simplifies concurrency:

- No cross-core cache coherency issues
- `portMUX_TYPE` spinlocks degenerate to interrupt-disable (no actual spinning)
- `taskENTER_CRITICAL()` just disables interrupts — no spinlock parameter needed
- Scheduler suspension (`vTaskSuspendAll`) IS a valid exclusion method on single-core
- **Mutexes** still needed for task-level mutual exclusion (preemption)
- **Priority inversion** still occurs — use mutexes (with priority inheritance) not binary semaphores for shared data

**Source:** ESP-IDF v6.0.1 FreeRTOS docs — "When building in single-core mode, IDF FreeRTOS is designed to be identical to Vanilla FreeRTOS" | **Confidence: HIGH**

---

## 1. FreeRTOS Task Synchronization — Prescriptive Patterns

### Current Task Architecture

| Task | Priority | Stack | Purpose |
|------|----------|-------|---------|
| `shs_zigbee_main` | 5 (highest) | 8192B | Zigbee stack main loop |
| `shs_ld2410_task` | 4 | 4096B | LD2410C UART polling |
| `shs_ld2450_task` | 4 | 4096B | LD2450 UART polling |
| `shs_boot_button` | 4 | 8192B | Factory reset button |
| `shs_save_worker` | 3 (lowest) | 3072B | Debounced NVS writes |

### 1.1 Use Task Notifications Instead of Queues for Simple Signaling

**Pattern:** Replace `xQueueSend`/`xQueueReceive` with `xTaskNotifyGive`/`ulTaskNotifyTake` when passing a single event or small value between a known producer and consumer.

**Why:** Task notifications are ~45% faster than queues and use zero extra heap (no queue structure allocated). FreeRTOS docs: "In many usage scenarios it is faster and more memory efficient to use a direct to task notification in place of a binary semaphore."

**Where to apply in SHS01:**
- The `shs_save_q` queue passes small `shs_save_msg_t` structs (8 bytes). Since the save worker is the only consumer and messages encode type+value, this can remain a queue (notifications can't queue multiple distinct messages). **Keep the queue here.**
- For sensor-to-Zigbee signaling (e.g., "new data available"), use task notification bits instead of polling with `vTaskDelay`. Each sensor task can notify the Zigbee task via `xTaskNotify(zigbee_task_handle, BIT_LD2410|BIT_LD2450, eSetBits)`.

**API:**
```c
// Producer (sensor task)
xTaskNotifyGive(save_worker_handle);  // simple wakeup

// Consumer (worker task)
ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // block until notified, clear on exit

// For multi-bit event flags
xTaskNotify(target_handle, EVENT_BITS, eSetBits);
uint32_t bits;
xTaskNotifyWait(0, ULONG_MAX, &bits, timeout);
```

**Confidence: HIGH** — Official FreeRTOS recommendation, verified in ESP-IDF v6.0.1 docs.

### 1.2 Mutex for Shared Sensor Data — Fix the Uninitialized Bug

**Current bug:** `target_data_mutex` is created in `app_main()` but the LD2450 callback (`shs_on_ld2450_target_update`) may fire before `app_main` reaches the mutex creation. The LD2450 driver is initialized and callbacks registered BEFORE the mutex is created.

**Fix pattern:**
```c
// Create mutex BEFORE initializing any driver that uses it
target_data_mutex = xSemaphoreCreateMutex();
assert(target_data_mutex != NULL);

// THEN initialize LD2450 and register callbacks
ESP_ERROR_CHECK(ld2450_init());
ld2450_register_target_callback(shs_on_ld2450_target_update);
```

**Mutex usage pattern (correct):**
```c
// Writer (sensor callback)
if (xSemaphoreTake(target_data_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    memcpy(current_targets, new_targets, sizeof(current_targets));
    current_target_count = count;
    xSemaphoreGive(target_data_mutex);
}

// Reader (Zigbee report task)
if (xSemaphoreTake(target_data_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    ld2450_target_t local_targets[3];
    memcpy(local_targets, current_targets, sizeof(local_targets));
    uint8_t local_count = current_target_count;
    xSemaphoreGive(target_data_mutex);
    // Process local_targets outside the lock
}
```

**Key rules:**
- Use `xSemaphoreCreateMutex()` (not binary semaphore) — provides priority inheritance
- Keep lock hold time minimal — copy data out, release, then process
- Use short timeout (5ms) for sensor callbacks to avoid blocking UART parsing
- **Never call NVS or Zigbee APIs while holding the target_data_mutex** (deadlock risk)

**Confidence: HIGH** — Standard FreeRTOS mutex pattern, bug confirmed in codebase.

### 1.3 Avoid Volatile for Shared Multi-Byte State

**Current antipattern:** `static volatile bool shs_zb_ready`, `static volatile bool shs_zb_connected` — `volatile` does NOT provide atomicity or memory ordering guarantees for multi-step operations.

**For single boolean flags** written by one task and read by another on single-core ESP32-C6: `volatile` is technically sufficient since reads/writes to aligned bool/uint32_t are single-instruction on RISC-V. However, for clarity and future-proofing:

**Recommended pattern:**
```c
// For simple flags (one writer, one or more readers): volatile is OK on single-core
static volatile bool shs_zb_ready = false;  // Keep as-is

// For compound state (multiple related fields): use mutex or atomic section
// Example: occupancy + target_count must be consistent
if (xSemaphoreTake(target_data_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    shs_ld2450_occupancy = any_target;
    shs_ld2450_target_count = count;
    xSemaphoreGive(target_data_mutex);
}
```

**Confidence: MEDIUM** — Volatile behavior on single-core RISC-V is well-defined but the pattern is a known source of bugs when code is modified later.

### 1.4 xTaskDelayUntil for Periodic Sensor Polling

**Current antipattern:** Sensor tasks likely use `vTaskDelay(pdMS_TO_TICKS(100))` which creates timing drift (delay is relative to when the call executes, not the loop start).

**Use instead:**
```c
void shs_ld2450_task(void *pv) {
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        // Process UART data...
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(100));  // Fixed 100ms period
    }
}
```

**Why:** Ensures consistent polling interval regardless of processing time variation.

**Confidence: HIGH** — Standard FreeRTOS periodic task pattern.

---

## 2. Zigbee SDK Lock Management — Prescriptive Patterns

### 2.1 Threading Model

The Zigbee SDK has a **single-threaded main loop** running in `shs_zigbee_task`. All Zigbee APIs are **NOT thread-safe**. The SDK provides a lock (`esp_zb_lock_acquire`/`esp_zb_lock_release`) that coordinates access between the main loop and application tasks.

**Rules (from official Espressif Zigbee SDK docs):**
1. **Callbacks from the Zigbee stack main loop**: Do NOT need the lock — already running in Zigbee context
2. **All other tasks** (sensor tasks, save worker, button task): MUST acquire lock before any `esp_zb_*` / `esp_zigbee_*` call
3. **`esp_zigbee_task_queue_post()`**: Alternative to lock — posts a callback to execute in the Zigbee task context

**Source:** ESP Zigbee SDK docs section 2.4.1 "Zigbee API Lock" | **Confidence: HIGH**

### 2.2 Lock Acquisition with Exponential Backoff

**Current pattern (problematic):**
```c
#define SHS_ZB_LOCK_TIMEOUT_MS 100
if (esp_zb_lock_acquire(pdMS_TO_TICKS(SHS_ZB_LOCK_TIMEOUT_MS)) != true) {
    return;  // Silent data loss!
}
```

**Problem:** 100ms timeout is aggressive for a device with 25 endpoints reporting. When the Zigbee stack is busy (commissioning, route discovery, attribute reporting), the lock may be held for longer periods. Returning silently loses the attribute update.

**Recommended pattern — retry with backoff:**
```c
static bool shs_zb_lock_acquire_with_retry(uint32_t initial_ms, uint8_t max_retries) {
    uint32_t wait_ms = initial_ms;
    for (uint8_t i = 0; i <= max_retries; i++) {
        if (esp_zb_lock_acquire(pdMS_TO_TICKS(wait_ms))) {
            return true;
        }
        wait_ms = (wait_ms < 500) ? wait_ms * 2 : 500;  // Cap at 500ms
    }
    return false;
}

// Usage
if (!shs_zb_lock_acquire_with_retry(50, 3)) {
    ESP_LOGW(TAG, "Zigbee lock failed after retries — data loss for EP%d", endpoint);
    shs_lock_fail_count++;
    return;
}
// ... do work ...
esp_zb_lock_release();
```

**Confidence: HIGH** — Backoff is standard practice; timeout values tuned for Zigbee stack behavior.

### 2.3 Prefer esp_zigbee_task_queue_post for Non-Urgent Updates

**Pattern:** Instead of acquiring the lock from sensor tasks, post a callback to the Zigbee task queue. This avoids lock contention entirely.

```c
// Define the callback
static void zb_update_occupancy_cb(void *arg) {
    // Runs in Zigbee task context — no lock needed
    bool occupied = (bool)(uintptr_t)arg;
    uint8_t v = occupied ? 1 : 0;
    esp_zb_zcl_set_attribute_val(SHS_EP_OCC,
        ESP_ZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID,
        &v, false);
}

// From sensor callback
esp_zigbee_task_queue_post(zb_update_occupancy_cb, (void*)(uintptr_t)occupied);
```

**Tradeoff:** Slightly higher latency (queued, not immediate) but zero lock contention. Best for non-time-critical updates like zone occupancy changes.

**Confidence: HIGH** — Official Zigbee SDK API, documented in developing.html.

### 2.4 Batch Attribute Updates Under a Single Lock

**Current antipattern:** Each attribute update acquires and releases the lock independently. For position reporting (9 attributes for 3 targets), this means 9 lock acquire/release cycles.

**Better pattern:**
```c
SHS_ZB_LOCK_ACQUIRE_OR_RETURN();
// Update all target position attributes in one lock hold
for (int i = 0; i < 3; i++) {
    esp_zb_zcl_set_attribute_val(ep_x[i], cluster, role, attr, &x[i], true);
    esp_zb_zcl_set_attribute_val(ep_y[i], cluster, role, attr, &y[i], true);
    esp_zb_zcl_set_attribute_val(ep_d[i], cluster, role, attr, &d[i], true);
}
esp_zb_lock_release();
```

**Rule:** Hold the lock for as short as possible, but batch related operations to avoid repeated acquire/release overhead.

**Confidence: HIGH** — Reduces lock contention by 9x for position updates.

---

## 3. NVS Flash I/O Optimization — Prescriptive Patterns

### 3.1 NVS Architecture on ESP32-C6

- **Flash sector size:** 4096 bytes
- **NVS page:** Maps 1:1 to flash sector (126 entries per page + header)
- **Entry size:** 32 bytes (key + value for primitives)
- **Current NVS partition:** 0x6000 = 24KB = 6 pages (plenty for ~100 config keys)
- **NVS is internally thread-safe** (uses its own mutex) — safe to call from any task
- **Each `nvs_commit()` may trigger a page compaction** (erase + rewrite) — this blocks for ~20-40ms

**Source:** ESP-IDF v6.0.1 NVS docs, "Pages and Entries" section | **Confidence: HIGH**

### 3.2 Batch NVS Writes — Open Once, Write Many, Commit Once

**Current antipattern (zone config save):**
```c
// Each helper opens, writes ONE key, commits, closes
static void shs_cfg_save_u16(const char *key, uint16_t v) {
    nvs_handle_t h;
    if (nvs_open(SHS_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, key, v);
    nvs_commit(h);   // ← May trigger flash erase (20-40ms)
    nvs_close(h);
}
```

When saving 5 zones × 6 keys = 30 individual open/commit/close cycles. Each `nvs_commit()` may trigger flash compaction.

**Correct pattern:**
```c
static void shs_zone_cfg_save_to_nvs(void) {
    nvs_handle_t h;
    if (nvs_open("shs_cfg", NVS_READWRITE, &h) != ESP_OK) return;

    // Write ALL keys with one handle — no intermediate commits
    nvs_set_u8(h, "z_type", shs_zone_type);
    nvs_set_u8(h, "z1_en", shs_zone1_enabled ? 1 : 0);
    nvs_set_i16(h, "z1_x1", shs_zone1_x1);
    // ... all zone keys ...

    nvs_commit(h);  // Single commit for ALL changes
    nvs_close(h);
}
```

**Impact:** Reduces flash wear by ~30x for zone config saves. One erase cycle instead of 30.

**Note:** The zone save function `shs_zone_cfg_save_to_nvs()` already does this correctly! The per-key helpers (`shs_cfg_save_u16`, `shs_cfg_save_u8`) are the problem — used by the save worker for individual config changes.

**Confidence: HIGH** — NVS docs: "actual storage will not be updated until nvs_commit is called."

### 3.3 Consolidate Save Worker NVS Writes

**Current save worker pattern:** Each debounced save calls a helper that opens/commits/closes independently:

```c
// After debounce expires:
shs_cfg_save_u8(SHS_NVS_KEY_MV_SENS, mv_sens_val);  // open, write, commit, close
shs_cfg_save_u8(SHS_NVS_KEY_ST_SENS, st_sens_val);  // open, write, commit, close (again!)
```

**Better pattern for save worker:**
```c
static void shs_save_worker(void *pv) {
    // ... debounce logic ...

    // Check if ANY pending saves expired
    bool any_save = false;
    if (pend_mv_sens && expired) { any_save = true; pend_mv_sens = false; }
    if (pend_st_sens && expired) { any_save = true; pend_st_sens = false; }
    // ...

    if (any_save) {
        nvs_handle_t h;
        if (nvs_open(SHS_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            // Write ALL pending values in one batch
            if (save_mv_sens) nvs_set_u8(h, SHS_NVS_KEY_MV_SENS, mv_sens_val);
            if (save_st_sens) nvs_set_u8(h, SHS_NVS_KEY_ST_SENS, st_sens_val);
            if (save_mv_gate) nvs_set_u8(h, SHS_NVS_KEY_MV_GATE, mv_gate_val);
            if (save_st_gate) nvs_set_u8(h, SHS_NVS_KEY_ST_GATE, st_gate_val);
            nvs_commit(h);  // Single commit
            nvs_close(h);
        }
    }
}
```

**Confidence: HIGH** — Direct optimization of existing code pattern.

### 3.4 Never Call NVS from Sensor Callbacks

**Rule:** NVS open/commit/close can block for 20-40ms (flash erase). Sensor UART callbacks parse frames at ~10Hz. Any blocking in the callback path causes UART buffer overflow and frame loss.

**Current architecture already has a save worker** — ensure ALL NVS writes go through it. Audit for any direct NVS calls in callback paths.

**Confirmed safe:** The Zigbee attribute write handler (`shs_zone_cfg_schedule_apply`) sets a flag and debounce timer, deferring the actual NVS write. This is correct.

**Confidence: HIGH** — Fundamental embedded systems constraint.

### 3.5 Use Two Separate NVS Namespaces

**Current issue:** Two namespaces are already in use (`"cfg"` for basic config, `"shs_cfg"` for zone config) but the partition is the same default `"nvs"`. This is fine — NVS namespaces share the same partition and the 24KB partition has plenty of room.

**No change needed.** The namespace separation is correct.

**Confidence: HIGH** — Verified in codebase.

---

## 4. UART Sensor Driver Patterns

### 4.1 UART Event-Driven vs Polling

**Current pattern:** Both LD2410 and LD2450 drivers use `uart_read_bytes()` in a polling loop with `vTaskDelay`. This is acceptable but not optimal.

**Better pattern (ESP-IDF UART event queue):**
```c
QueueHandle_t uart_queue;
uart_driver_install(UART_NUM, BUF_SIZE, 0, 20, &uart_queue, 0);

// In task loop:
uart_event_t event;
if (xQueueReceive(uart_queue, &event, pdMS_TO_TICKS(100))) {
    if (event.type == UART_DATA) {
        int len = uart_read_bytes(UART_NUM, buf, event.size, 0);
        // Parse frame...
    }
}
```

**Why:** Event-driven approach wakes the task only when data arrives, reducing CPU usage. Polling with `vTaskDelay(10)` wastes ~10% CPU on empty reads.

**Recommendation:** Keep current polling for this milestone — the refactoring risk is higher than the benefit. Consider for a future milestone.

**Confidence: MEDIUM** — Event-driven is better but the current approach works; change is optional.

### 4.2 Frame Parser State Machine Robustness

Both drivers already implement:
- ✅ Frame timeout detection (`LD2450_FRAME_TIMEOUT_MS = 200`)
- ✅ Watchdog timeout (`LD2450_WATCHDOG_TIMEOUT_MS = 10000`)
- ✅ UART flush and reset on watchdog trigger
- ✅ Diagnostic counters (total bytes, valid frames, errors)

**No changes needed** to the UART driver architecture.

**Confidence: HIGH** — Verified in driver source code.

---

## 5. Module Refactoring Guidance (shs01.c → 5 modules)

### Recommended Module Split

| Module | File | Responsibility | Shared State Access |
|--------|------|---------------|-------------------|
| **main** | `shs01_main.c` | `app_main()`, task creation, initialization | Owns all state creation |
| **zigbee** | `shs01_zigbee.c` | Zigbee task, endpoint creation, attribute helpers, callbacks | Via `esp_zb_lock` |
| **sensors** | `shs01_sensors.c` | LD2410/LD2450 callbacks, occupancy logic, smoothing | Via `target_data_mutex` |
| **config** | `shs01_config.c` | NVS load/save, zone config, save worker task | Via `shs_save_q` |
| **diagnostics** | `shs01_diag.c` | Lock counters, connectivity checks, health endpoint | Read-only access |

### Shared State Pattern

```c
// shs01_state.h — shared state declarations
typedef struct {
    // Sensor data (protected by target_data_mutex)
    ld2450_target_t targets[3];
    uint8_t target_count;
    SemaphoreHandle_t mutex;

    // Occupancy state (written by sensor task, read by Zigbee task)
    volatile bool ld2410_moving;
    volatile bool ld2410_static;
    volatile bool ld2450_occupancy;
    volatile uint8_t ld2450_target_count;

    // Zone state
    bool zone_occupied[5];
    uint8_t zone_targets[5];

    // Config (written by config module via save worker, read by sensors)
    uint16_t movement_cooldown_sec;
    uint16_t occupancy_clear_sec;
    // ...

    // Zigbee readiness
    volatile bool zb_ready;
    volatile bool zb_connected;
} shs_state_t;

extern shs_state_t g_shs;
```

**Confidence: HIGH** — Standard embedded firmware modularization pattern.

---

## 6. Recommended idf_component.yml Update

```yaml
dependencies:
  espressif/esp-zboss-lib: "~1.6.0"
  espressif/esp-zigbee-lib: "~1.6.0"
  espressif/led_strip: "~2.0.0"
  idf:
    version: ">=5.3.0"  # Minimum for latest Zigbee SDK stability fixes
```

**Note:** Consider pinning to `>=5.3.0` instead of `>=5.0.0` to ensure Zigbee SDK compatibility. The `~1.6.0` Zigbee libraries work best with IDF 5.3+.

**Confidence: MEDIUM** — Based on Zigbee SDK release notes and IDF compatibility matrix.

---

## Alternatives Considered

| Category | Recommended | Alternative | Why Not |
|----------|-------------|-------------|---------|
| Task sync | Mutex + Queue | Event Groups | Overkill for 2-task producer/consumer; adds complexity |
| Zigbee updates | Lock + batch | `esp_zigbee_task_queue_post` for all | Queue has size limit; direct lock is fine for bulk position reports |
| NVS writes | Batched commit via save worker | Direct writes in callbacks | Blocks UART parsing; causes frame loss |
| UART driver | Polling (current) | Event-driven queue | Higher refactoring risk for marginal benefit this milestone |
| Shared state | Global struct + mutex | Message passing | Too much refactoring for existing 100+ globals; migrate incrementally |
| Config persistence | NVS (current) | SPIFFS/LittleFS JSON | NVS is purpose-built for small key-value; no reason to change |

---

## Sources

- ESP-IDF v6.0.1 FreeRTOS documentation: https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/system/freertos_idf.html (**HIGH** confidence — official, current)
- ESP-IDF v6.0.1 NVS documentation: https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/storage/nvs_flash.html (**HIGH** confidence — official, current)
- ESP Zigbee SDK developing guide: https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32c6/developing.html (**HIGH** confidence — official, section 2.4.1 "Zigbee API Lock")
- SHS-Z2M-Presence codebase analysis: `main/shs01.c`, `main/shs01.h`, component drivers (**HIGH** confidence — direct source code inspection)
