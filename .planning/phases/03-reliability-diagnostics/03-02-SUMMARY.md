---
phase: 03-reliability-diagnostics
plan: 02
status: complete
started: 2026-04-30T21:50:00Z
completed: 2026-04-30T22:10:00Z
---

# Summary: Sensor Health + Diagnostics Zigbee Exposure + Converter

## What Was Built

1. **Health Attribute Defines** — Added `SHS_ATTR_LD2410C_CONNECTED` (0x0070) and `SHS_ATTR_LD2450_CONNECTED` (0x0071) to shs01.h.

2. **Diagnostic Attribute Defines** — Added 7 diagnostic counters (0x0080-0x0086): lock success/fail/consecutive, TX success/fail, uptime seconds, free heap bytes.

3. **Attribute Registration** — All 9 attributes registered on config cluster 0xFDCD EP1 as read-only with reporting access. Used static zero-init variables for initial values.

4. **Reporting Functions** — `shs_zb_report_sensor_health()` reports both sensor connected booleans. `shs_zb_report_diagnostics()` reports all 7 counters including computed uptime and free heap.

5. **Event-Driven Health Reporting** — Health attributes update immediately on LD2450 connect/disconnect transitions and on Zigbee connectivity loss (rejoin trigger).

6. **Periodic Reporting** — Both health and diagnostics reported every 60s in the existing Zigbee connectivity check block.

7. **Z2M Converter (both .js and .mjs)** — Added 9 attribute constants, fromZigbee decode cases, 9 new exposes (2 binary + 7 numeric), and configure read for initial values.

## Key Files

| File | Changes |
|------|---------|
| main/shs01.h | 9 attribute defines (0x0070-0x0086) |
| main/shs01.c | Reporting functions, attribute registration, event + periodic calls |
| zigbee2mqtt/external_converters/shs01_enhanced.js | Constants, fromZigbee, exposes, configure read |
| zigbee2mqtt/external_converters/shs01_enhanced.mjs | Identical changes (ESM variant) |

## Commits

- `feat(03-02): add health + diagnostic Zigbee attributes on 0xFDCD`
- `feat(03-02): add Z2M converter decode for health + diagnostic attributes`

## Deviations

None.

## Self-Check: PASSED
