# Summary: Plan 01-02 — Zigbee lock retry + NVS zone save routing

## What Was Built
1. **Retry-enabled Zigbee lock macros** — Both `SHS_ZB_LOCK_ACQUIRE_OR_RETURN()` and `SHS_ZB_LOCK_ACQUIRE_OR_RETURN_FALSE()` now retry 3 times with exponential backoff (50ms → 100ms → 200ms, 350ms total max wait) before giving up. Previously, a single 100ms timeout meant silent data loss on transient lock contention.

2. **Zone NVS saves routed through save_worker** — `shs_zone_cfg_apply_to_sensor()` no longer calls `shs_zone_cfg_save_to_nvs()` directly (which blocks the LD2450 task during flash I/O). Instead, it enqueues `SHS_SAVE_ZONE_CONFIG` to the save_worker queue. The save_worker (priority 3) handles the NVS write asynchronously.

## Key Decisions
- Kept `SHS_ZB_LOCK_TIMEOUT_MS` constant — still used by one-off lock acquire in boot button handler (L2125)
- Used `static const TickType_t _backoff[]` in macros — no heap allocation, evaluated at compile time
- Zone save uses immediate dispatch (no debounce) since zone apply is already debounced by 500ms

## Files Modified
- `main/shs01.c` — All changes in single file

## Commits
1. `fix(01-02): replace Zigbee lock macros with retry + exponential backoff`
2. `fix(01-02): route zone NVS saves through save_worker task`

## Requirements Addressed
- **ZB-01**: Zigbee lock failures retried with exponential backoff
- **ZB-02**: Lock diagnostic counters track retry attempts accurately
- **NVS-01**: Zone NVS saves happen in save_worker task, not in sensor/Zigbee callback context

## Verification
- `_backoff[]` array present in both macro variants with values 50ms, 100ms, 200ms
- `SHS_SAVE_ZONE_CONFIG` appears exactly 3 times (enum, enqueue, case handler)
- No direct `shs_zone_cfg_save_to_nvs()` call outside save_worker
- No IDE errors
