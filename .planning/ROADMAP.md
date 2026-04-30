# Roadmap: SHS-Z2M-Presence — Optimization & Hardening

## Overview

This milestone hardens an existing working dual-sensor presence detection firmware. The journey follows research-validated ordering: fix crash/data-loss bugs in the current monolith (where behavior is well-understood), re-enable disabled safety features, add reliability infrastructure, then restructure the code — never the reverse. Every phase delivers verifiable improvements to the running device.

## Phases

**Phase Numbering:**
- Integer phases (1, 2, 3): Planned milestone work
- Decimal phases (2.1, 2.2): Urgent insertions (marked with INSERTED)

Decimal phases appear between their surrounding integers in numeric order.

- [ ] **Phase 1: Critical Bug Fixes & Concurrency Safety** - Eliminate crashes, race conditions, and silent Zigbee data loss
- [ ] **Phase 2: Sensor Filters & Converter Fix** - Re-enable disabled smoothing/filtering and fix coordinate encoding
- [ ] **Phase 3: Reliability & Diagnostics** - Add watchdog, health monitoring, NVS batching, and diagnostic counters
- [ ] **Phase 4: Code Restructuring** - Split monolith into modules with preserved endpoint compatibility

## Phase Details

### Phase 1: Critical Bug Fixes & Concurrency Safety
**Goal**: Device operates without crashes, race conditions, or silent data loss
**Depends on**: Nothing (first phase)
**Requirements**: TS-01, TS-02, TS-03, TS-04, NVS-01, ZB-01, ZB-02
**Success Criteria** (what must be TRUE):
  1. Device boots and runs 24+ hours without crash or panic — no uninitialized mutex access
  2. Zone occupancy state never shows phantom occupancy from stale target counts
  3. Every Zigbee attribute update reaches the coordinator — failed locks retry with backoff instead of being silently dropped
  4. Sensor callbacks complete without NVS blocking — all writes routed through save_worker task
**Plans:** 2 plans
Plans:
- [x] 01-01-PLAN.md — Mutex protection for shared state (TS-02, TS-03, TS-04)
- [x] 01-02-PLAN.md — Zigbee lock retry + NVS zone save routing (ZB-01, ZB-02, NVS-01)

### Phase 2: Sensor Filters & Converter Fix
**Goal**: Clean, accurate position data with proper encoding across firmware and converter
**Depends on**: Phase 1
**Requirements**: SF-01, SF-02, SF-03, CC-01, CC-02
**Success Criteria** (what must be TRUE):
  1. Target position updates in Z2M are smooth (no jitter) — EMA smoothing active at α=0.3
  2. Device reports valid presence from LD2410C alone when LD2450 is disconnected
  3. Target coordinates in Z2M show correct signed values (−3000..+3000mm) without bias artifacts
  4. Z2M external converter decodes all attributes correctly after int16 encoding change
**Plans:** 2 plans
Plans:
- [x] 02-01-PLAN.md — EMA smoothing, LD2410C standalone filter, int16 coordinate encoding ✓ 2026-04-30
- [x] 02-02-PLAN.md — Z2M converter bias removal (.js + .mjs) ✓ 2026-04-30

### Phase 3: Reliability & Diagnostics
**Goal**: Device self-monitors, recovers from sensor failures, and exposes health status
**Depends on**: Phase 2
**Requirements**: SH-01, SH-02, SH-03, NVS-02, NVS-03, ZB-03, ZB-04
**Success Criteria** (what must be TRUE):
  1. Task watchdog detects and logs frozen sensor tasks within configured timeout
  2. Sensor connected/disconnected status visible in Z2M device page per sensor
  3. LD2450 UART reconnection resumes reporting without requiring device reboot
  4. Zone configuration save completes in a single NVS transaction — no partial writes on power loss
  5. Zigbee lock/TX statistics accessible via Z2M for remote troubleshooting
**Plans**: TBD

### Phase 4: Code Restructuring
**Goal**: Monolithic shs01.c replaced by focused modules without breaking any existing functionality
**Depends on**: Phase 3
**Requirements**: CS-01, CS-02, CS-03, CS-04, CC-03
**Success Criteria** (what must be TRUE):
  1. shs01.c reduced from 2691 lines to ≤100 (app_main only) — all logic in 5 purpose-specific modules
  2. Existing Z2M device pairings remain valid — no re-interview or re-pair needed after firmware update
  3. Zone attribute handler uses computed offsets — adding a 6th zone requires changing one constant, not 30 switch cases
  4. All 25 endpoints created in preserved registration order — verified by Zigbee interview match
**Plans**: TBD

## Progress

**Execution Order:**
Phases execute in numeric order: 1 → 2 → 3 → 4

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 1. Critical Bug Fixes & Concurrency Safety | 2/2 | Complete | 2026-04-30 |
| 2. Sensor Filters & Converter Fix | 0/? | Not started | - |
| 3. Reliability & Diagnostics | 0/? | Not started | - |
| 4. Code Restructuring | 0/? | Not started | - |
