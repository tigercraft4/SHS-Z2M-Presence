---
phase: 02-sensor-filters
plan: 01
subsystem: firmware
tags: [ema, smoothing, ld2410c, ld2450, zigbee, position]

requires:
  - phase: 01-critical-bug-fixes
    provides: Stable Zigbee stack with proper mutex initialization

provides:
  - EMA position smoothing at α=0.3
  - LD2410C standalone operation when LD2450 disconnected
  - Raw signed X coordinate encoding (no bias)
  - 200ms rate-limited position updates

affects: [02-sensor-filters]

tech-stack:
  added: []
  patterns:
    - "shs_ld2450_connected flag for sensor connectivity tracking"
    - "Energy filter fallback for standalone LD2410C mode"

key-files:
  created: []
  modified:
    - main/shs01.c

key-decisions:
  - "Keep float analog value for X coordinate (remove bias only) rather than switching to int16 cluster — preserves Z2M endpoint compatibility"
  - "Energy filter used only in standalone mode — cross-validation handles noise when LD2450 connected"

patterns-established:
  - "Sensor connectivity tracking: static bool flag set true on first callback, used to gate cross-validation"

requirements-completed: [SF-01, SF-02, SF-03, CC-01]

duration: 5min
completed: 2026-04-30
---

# Plan 02-01: Sensor Filters & Coordinate Encoding Summary

**Re-enabled EMA smoothing (α=0.3, 200ms rate limit), added LD2410C standalone filtering with energy thresholds, and removed +3000 X coordinate bias hack.**

## What Was Built

### Task 1: EMA Smoothing & Rate Limiting
- Changed `EMA_ALPHA` from 1.0 (disabled) to 0.3 — position data is now smoothed
- Reduced `POSITION_UPDATE_INTERVAL_MS` from 300ms to 200ms for more responsive updates
- Removed diagnostic comments

### Task 2: LD2410C Standalone Filter
- Added `shs_ld2450_connected` flag — set true on first LD2450 callback
- Cross-validation now only silences LD2410C when LD2450 is actively connected AND sees 0 targets
- When LD2450 is offline: LD2410C uses energy filters (`shs_min_moving_energy`/`shs_min_static_energy` = 40) as standalone fallback
- Removed the `#if 0` disabled block — energy filter code is now live in the standalone path

### Task 3: Coordinate Bias Removal
- Removed `float x_biased = (float)(new_x + 3000)` — X sent as raw signed float
- Removed `3000.0f` from inactive target zero-send — sends `0.0f` directly
- Removed `__attribute__((unused))` from `shs_zb_set_i16_attr` (future use)
- Simplified log message to `"ZB T%d: X=%d Y=%d D=%d"`

## Self-Check: PASSED

- [x] `EMA_ALPHA` = 0.3f confirmed
- [x] `POSITION_UPDATE_INTERVAL_MS` = 200 confirmed
- [x] `shs_ld2450_connected` declared, set, and used in cross-validation
- [x] No `x_biased` variable in file
- [x] No bias-related `3000` in position reporting code
- [x] `#if 0` count = 0

## Deviations

None. Plan executed as specified.
