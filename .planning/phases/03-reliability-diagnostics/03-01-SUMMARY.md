---
phase: 03-reliability-diagnostics
plan: 01
status: complete
started: 2026-04-30T21:30:00Z
completed: 2026-04-30T21:45:00Z
---

# Summary: Task Watchdog + NVS Zone Config Batching

## What Was Built

1. **Task Watchdog Timer (TWDT)** — Both sensor tasks (`shs_ld2410_task`, `shs_ld2450_task`) are now subscribed to ESP-IDF TWDT with 10s timeout and panic-on-trigger. If either task freezes (UART hardware hang), the device auto-reboots.

2. **LD2410C Frame Disconnect Detection** — Passive disconnect detection via frame timestamp tracking. If no LD2410C frame is received for 3 seconds (~30 missed frames at 100ms interval), the sensor is marked disconnected with `ESP_LOGW`.

3. **Atomic NVS Zone Config Blob** — Replaced 31 individual `nvs_set_u8`/`nvs_set_i16` calls with a single `nvs_set_blob` using a 51-byte packed struct. Power loss during save cannot produce partial zone configurations.

4. **Legacy NVS Migration** — `shs_zone_cfg_load_from_nvs()` tries blob format first, falls back to legacy per-key reads, then auto-migrates to blob on next save.

5. **NVS Error Logging** — All NVS helper functions (`shs_cfg_save_u16`, `shs_cfg_save_u8`, `shs_cfg_save_i16`) now log `ESP_LOGW` on open failures instead of silently returning.

## Key Files

| File | Changes |
|------|---------|
| sdkconfig.defaults | Added TWDT config (4 entries) |
| main/shs01.c | TWDT init, subscribe, reset; LD2410C frame tracking; NVS blob save/load; NVS error logging |

## Commits

- `feat(03-01): add task watchdog + LD2410C frame disconnect detection`
- `feat(03-01): NVS zone config atomic blob + error logging`

## Deviations

None.

## Self-Check: PASSED
