# SHS-Z2M-Presence — Optimization & Hardening

## What This Is

Dual mmWave presence sensor firmware (ESP32-C6 + LD2410C + LD2450) with Zigbee2MQTT support. Tracks up to 3 simultaneous targets across 5 configurable zones with cross-validated occupancy detection. This milestone focuses on code quality, reliability, and performance improvements for the existing working firmware.

## Core Value

Reliable, crash-free presence detection — every occupancy event reported correctly to Zigbee2MQTT, zero silent data loss, no false positives from sensor noise.

## Requirements

### Validated

- ✓ Dual sensor cross-validation (LD2410C + LD2450) — existing
- ✓ Multi-zone support (5 zones, detection/filter/interference types) — existing
- ✓ Multi-target tracking (3 simultaneous targets, X/Y/distance) — existing
- ✓ Zigbee Router mode with 25 endpoints — existing
- ✓ NVS persistent configuration — existing
- ✓ Boot button factory reset (triple-click) — existing
- ✓ Zigbee2MQTT external converter — existing
- ✓ Position reporting toggle (config mode) — existing

### Active

- [ ] Fix uninitialized `target_data_mutex` (crash on LD2450 callback)
- [ ] Remove NVS I/O from sensor callbacks (move to worker task)
- [ ] Add Zigbee lock retry with backoff (prevent silent data loss)
- [ ] Synchronize LD2450 target count access with mutex
- [ ] Re-enable position smoothing (EMA_ALPHA → 0.3f)
- [ ] Refactor monolithic shs01.c (2691 lines → 5 modules)
- [ ] Add sensor health check / diagnostics endpoint
- [ ] Batch zone NVS writes (single write per config change)
- [ ] Remove coordinate biasing hack (use proper int16 encoding)
- [ ] Add standalone LD2410 error filtering (fallback when LD2450 offline)

### Out of Scope

- OTA firmware update — separate feature milestone
- Power management / sleep modes — device is mains powered
- Zigbee Binding/Scenes support — not required for presence sensor
- Tamper detection — no hardware support in current design
- Configuration backup/restore to JSON — nice-to-have, not critical

## Context

- **Platform**: ESP-IDF with Espressif Zigbee SDK (esp_zigbee_core)
- **Hardware**: ESP32-C6 + LD2410C (UART1) + LD2450 (UART0)
- **Architecture**: FreeRTOS tasks (zigbee, ld2410, ld2450, boot_button, save_worker)
- **Key issues**: Race conditions on shared state, NVS blocking in callbacks, disabled filters/smoothing, 100+ unencapsulated static globals
- **Custom PCB**: v1 + v2 designs in hardware/ directory
- **Companion**: SHS Z2M Presence Zones HA add-on for visual zone configuration

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| Fix crash bugs before refactoring | Uninitialized mutex causes immediate crash; must be first | — Pending |
| Refactor into modules after bug fixes | Reduces risk of introducing new bugs during restructure | — Pending |
| Keep 25 endpoint model | Changing endpoint count breaks existing Z2M pairings | — Pending |

## Evolution

This document evolves at phase transitions and milestone boundaries.

**After each phase transition:**
1. Requirements invalidated? → Move to Out of Scope with reason
2. Requirements validated? → Move to Validated with phase reference
3. New requirements emerged? → Add to Active
4. Decisions to log? → Add to Key Decisions
5. "What This Is" still accurate? → Update if drifted

**After each milestone:**
1. Full review of all sections
2. Core Value check — still the right priority?
3. Audit Out of Scope — reasons still valid?
4. Update Context with current state

---
*Last updated: 2026-04-30 after initialization*
