# Phase 4: Code Restructuring - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-30
**Phase:** 04-code-restructuring
**Areas discussed:** Module boundaries, State struct design, Zone handler de-duplication, Endpoint creation pattern

---

## Module Boundaries

### Cross-module state access

| Option | Description | Selected |
|--------|-------------|----------|
| Single shared header with extern globals | Move globals into a shared shs_state.h/.c — other modules #include and access directly via extern. Simplest migration, preserves current access pattern. | |
| Opaque state with accessor functions | shs_state.c owns all state, exposes getters/setters. Clean API but adds function-call overhead on every access and more boilerplate. | |
| Shared state struct passed by pointer | Single shs_state_t allocated in main, pointer passed to init functions. Modules store the pointer and access fields directly. Testable, moderate effort. | ✓ |

**User's choice:** Asked for recommendation. Agent recommended struct-by-pointer for balance of testability and low overhead on single-core ESP32-C6.
**Notes:** User accepted recommendation.

### Module split

| Option | Description | Selected |
|--------|-------------|----------|
| 5 modules (state, config, sensor, zigbee, button) | Split as described: state struct, NVS/config, sensor tasks/callbacks, Zigbee endpoint/helpers, button/LED | ✓ |
| Adjust | User-defined alternative split | |

**User's choice:** Looks good
**Notes:** None

---

## State Struct Design

### Nesting depth

| Option | Description | Selected |
|--------|-------------|----------|
| Flat + zone array | shs_state_t with flat fields for sensor/diagnostic/config state + nested shs_zone_t zones[5] array | ✓ |
| Deeply nested sub-structs | Full nesting: state->sensor.ld2410c.moving_state, state->diag.lock_success_count. Cleaner grouping but deeper access paths. | |

**User's choice:** Asked for recommendation. Agent recommended flat + zone array for embedded hot-path clarity. User confirmed.
**Notes:** Only zones warrant a sub-struct (5 identical instances). Everything else stays flat.

---

## Zone Handler De-duplication

### Offset computation

| Option | Description | Selected |
|--------|-------------|----------|
| Use existing 0x10 stride | Keep the existing 0x10 stride already defined in shs01.h. Zone1=0x0020, zone2=0x0030, etc. | |
| You decide | Let the agent choose the most practical approach based on existing attribute IDs | ✓ |

**User's choice:** You decide
**Notes:** Agent has discretion on implementation approach.

---

## Endpoint Creation Pattern

### Replacement approach

| Option | Description | Selected |
|--------|-------------|----------|
| Factory helper functions | Small helpers: shs_zb_add_binary_ep() and shs_zb_add_analog_ep(). Unique EPs stay hand-coded. | |
| Table-driven registration | Static table of {ep_num, type, init_ptr} → loop. Maximum compression but harder to read. | |
| You decide | Agent picks the approach that best fits the code | ✓ |

**User's choice:** You decide
**Notes:** Agent has discretion on implementation approach.

---

## Agent's Discretion

- Zone handler offset mechanism
- Endpoint creation pattern
- shs_state_t allocation strategy (stack vs static)
- Header include strategy
- Constant relocation (shs01.h vs shs_state.h)

## Deferred Ideas

None — discussion stayed within phase scope.
