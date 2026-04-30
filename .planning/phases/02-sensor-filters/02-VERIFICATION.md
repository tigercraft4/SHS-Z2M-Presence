---
phase: 02-sensor-filters
status: passed
verified: 2026-04-30
must_haves_checked: 6/6
gaps: 0
human_verification: 3
---

# Phase 02: Sensor Filters — Verification

## Must-Haves Check

### Plan 02-01 Must-Haves

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | EMA smoothing active at α=0.3 | ✓ PASS | `#define EMA_ALPHA 0.3f` at shs01.c:270 |
| 2 | LD2410C reports valid presence when LD2450 disconnected | ✓ PASS | `shs_ld2450_connected` flag + energy filter fallback at shs01.c:741-750 |
| 3 | Position updates rate-limited to 200ms | ✓ PASS | `#define POSITION_UPDATE_INTERVAL_MS 200` at shs01.c:268 |
| 4 | X coordinates without +3000 bias | ✓ PASS | No `x_biased` variable, `shs_zb_set_analog_value(ep_base, (float)new_x)` sends directly |

### Plan 02-02 Must-Haves

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 5 | JS converter reads X directly | ✓ PASS | No `- 3000` in shs01_enhanced.js |
| 6 | MJS converter reads X directly | ✓ PASS | No `- 3000` in shs01_enhanced.mjs |

## Key Links Verification

| From | To | Via | Status |
|------|----|-----|--------|
| shs01.c (shs_on_ld2450_target_update) | Zigbee attributes | `shs_zb_set_analog_value(ep_base, (float)new_x)` | ✓ Direct signed value |
| shs01.c (shs_on_state_change) | LD2410C standalone | `!shs_ld2450_connected` → energy filter | ✓ Conditional path exists |
| JS converter | X decode | `Math.round(value)` without subtraction | ✓ Direct decode |
| MJS converter | X decode | `Math.round(value)` without subtraction | ✓ Direct decode |

## Requirement Traceability

| Req ID | Description | Plan | Status |
|--------|-------------|------|--------|
| SF-01 | EMA position smoothing | 02-01 | ✓ |
| SF-02 | LD2410C standalone filter | 02-01 | ✓ |
| SF-03 | Rate limiting | 02-01 | ✓ |
| CC-01 | Remove firmware bias | 02-01 | ✓ |
| CC-02 | Update Z2M converters | 02-02 | ✓ |

## Human Verification Items

These items require testing on physical hardware:

1. **Smooth position tracking**: Flash firmware, observe Z2M position values — should show smooth (not jittery) updates
2. **LD2410C standalone**: Disconnect LD2450 UART, verify LD2410C still reports occupancy via energy filter
3. **Correct X coordinates**: With both firmware and converter updated, verify X values in Z2M are correct signed values (−3000 to +3000)

## Result

**Status: PASSED** — All automated checks pass. 3 items need hardware verification.
