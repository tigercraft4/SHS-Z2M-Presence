---
phase: 02-sensor-filters
plan: 02
subsystem: zigbee2mqtt
tags: [z2m, converter, coordinates, bias]

requires:
  - phase: 02-sensor-filters
    provides: Raw signed X coordinates from firmware (no +3000 bias)

provides:
  - Z2M converters decode X coordinates directly without bias subtraction

affects: []

tech-stack:
  added: []
  patterns: []

key-files:
  created: []
  modified:
    - zigbee2mqtt/external_converters/shs01_enhanced.js
    - zigbee2mqtt/external_converters/shs01_enhanced.mjs

key-decisions:
  - "Identical changes to both .js and .mjs — kept both converters in sync"

patterns-established: []

requirements-completed: [CC-02]

duration: 2min
completed: 2026-04-30
---

# Plan 02-02: Z2M Converter Bias Removal Summary

**Removed −3000 bias subtraction from both JS and MJS external converters to match firmware's raw signed X coordinate encoding.**

## What Was Built

### Task 1: JS Converter
- Removed WORKAROUND comment about +3000 bias
- Changed `Math.round(value) - 3000` → `Math.round(value)` for target1_x, target2_x, target3_x
- Y and distance parsing unchanged

### Task 2: MJS Converter
- Applied identical changes to the ESM (.mjs) version
- Both converters now read X coordinates directly

## Self-Check: PASSED

- [x] No `- 3000` in JS converter code
- [x] No `- 3000` in MJS converter code
- [x] No "WORKAROUND" or "bias" comments in position parsing
- [x] Y and distance parsing unchanged
- [x] Description strings correctly kept (`-3000 to +3000` range descriptions)

## Deviations

None.
