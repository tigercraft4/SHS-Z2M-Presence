---
phase: 04-code-restructuring
plan: 02
status: complete
---

# Plan 04-02 Summary: Zigbee Module

## What Was Built
Created `shs_zigbee.h` and `shs_zigbee.c` — extracts all Zigbee code from shs01.c into a dedicated module:

- **Lock macros** with `s_state->` references (retry + exponential backoff preserved)
- **All ZB helper functions** (set_analog_value, set_binary_value, set_occ_bitmap, report_attr, etc.)
- **Endpoint factory helpers** (`add_binary_ep`, `add_analog_ep`) replacing copy-paste blocks
- **Zone endpoint loops** using `zone_occ_eps[]` and `zone_target_eps[]` lookup arrays
- **BDB commissioning**, signal handler, metadata publish
- **Force-update functions** for LD2410C and LD2450 (also use zone loops)
- **Health/diagnostics** reporting functions

## Key Improvements (CS-04)
- Zone occupancy endpoints (5/6/7/22/23) created via loop instead of 5 copy-paste blocks
- Zone target count endpoints (19/20/21/24/25) created via loop
- Position data endpoints (8-16) created via nested loop
- 5 zone attributes registered in loop with computed `base` address
- Endpoint registration order preserved exactly: EP1,2,3,4,5,6,7,22,23,8-16,17,18,19,20,21,24,25

## Key Files
- `main/shs_zigbee.h` — Prototypes for all ZB helpers, force-update, health/diagnostics
- `main/shs_zigbee.c` — Full implementation (~817 lines)

## Self-Check: PASSED
- `shs_zigbee.h` contains `void shs_zigbee_init(shs_state_t *state)` ✓
- `shs_zigbee.h` contains `void shs_zigbee_task(void *pvParameters)` ✓
- `shs_zigbee.c` contains `zone_occ_eps[SHS_NUM_ZONES]` and `zone_target_eps[SHS_NUM_ZONES]` ✓
- `shs_zigbee.c` contains lock macros with `s_state->` ✓
- Endpoint registration uses factory helpers + loops ✓
