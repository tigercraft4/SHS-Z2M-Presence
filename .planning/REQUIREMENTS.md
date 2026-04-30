# Requirements: SHS-Z2M-Presence Optimization & Hardening

**Defined:** 2026-04-30
**Core Value:** Reliable, crash-free presence detection — every occupancy event reported correctly to Zigbee2MQTT, zero silent data loss.

## v1 Requirements

Requirements for this milestone. Each maps to roadmap phases.

### Thread Safety

- [ ] **TS-01**: `target_data_mutex` initialized before LD2450 driver starts — prevents crash on first target callback
- [ ] **TS-02**: `shs_ld2450_target_count` access synchronized with mutex — prevents stale cross-validation data
- [ ] **TS-03**: Zone configuration variables protected from concurrent read/write between Zigbee task and LD2450 task
- [ ] **TS-04**: `shs_position_reporting` toggle atomic or mutex-protected — prevents partial position data sequences

### Zigbee Reliability

- [ ] **ZB-01**: Zigbee lock acquisition uses exponential backoff (50ms initial, 3 retries, 500ms cap) — prevents silent data loss
- [ ] **ZB-02**: Failed attribute reports queued for retry instead of silently dropped
- [ ] **ZB-03**: Lock acquisition diagnostic counters (success/fail/consecutive) exposed via Zigbee attributes
- [ ] **ZB-04**: Connectivity check recovers from stale connection state without manual intervention

### NVS Optimization

- [ ] **NVS-01**: NVS writes removed from sensor callback context — all writes via dedicated worker task
- [ ] **NVS-02**: Zone configuration saves batched into single NVS commit (was 31 individual writes)
- [ ] **NVS-03**: NVS error handling logs failures and provides default fallback indication

### Sensor Filters

- [ ] **SF-01**: Position smoothing re-enabled with EMA_ALPHA = 0.3f — reduces Zigbee traffic 4-10x
- [ ] **SF-02**: Standalone LD2410C error filtering active when LD2450 is offline or disconnected
- [ ] **SF-03**: Position reporting rate-limited to 100-300ms instead of per-frame

### Sensor Health

- [ ] **SH-01**: Task watchdog monitors LD2410 and LD2450 tasks — detects frozen/crashed sensor processing
- [ ] **SH-02**: Sensor connectivity status exposed via Zigbee attributes (connected/disconnected per sensor)
- [ ] **SH-03**: LD2450 UART reconnection synchronized with Zigbee state after recovery

### Code Structure

- [ ] **CS-01**: Shared state consolidated into `shs_state_t` struct with zone array (`shs_zone_t zones[5]`)
- [ ] **CS-02**: shs01.c split into ≤5 modules: state, config, sensor, zigbee, button
- [ ] **CS-03**: Zone attribute handler uses computed offsets (0x10 stride) instead of 30 copy-paste switch cases
- [ ] **CS-04**: Endpoint creation uses loop/factory pattern instead of 25 boilerplate blocks

### Converter Compatibility

- [ ] **CC-01**: Remove coordinate biasing hack (+3000mm) — use proper int16 encoding in firmware
- [ ] **CC-02**: Update Z2M external converter to match int16 encoding change
- [ ] **CC-03**: Endpoint registration order preserved through refactoring — existing Z2M pairings remain valid

## v2 Requirements

Deferred to future milestone. Tracked but not in current roadmap.

### Future Features

- **OTA-01**: Over-the-air firmware update via Zigbee OTA cluster
- **DIAG-01**: Configuration backup/restore as JSON via Zigbee attribute
- **BIND-01**: Zigbee Binding and Scene support for direct device-to-device automation
- **ENERGY-01**: LD2410 per-gate energy values exposed via Zigbee attributes

## Out of Scope

Explicitly excluded. Documented to prevent scope creep.

| Feature | Reason |
|---------|--------|
| Power management / sleep modes | Device is mains powered, always-on router |
| BLE provisioning | Zigbee-only device; BLE adds complexity |
| Web configuration UI | Companion HA add-on handles visual configuration |
| Tamper detection | No hardware support in current PCB design |
| Additional zone types | 3 types (detection/filter/interference) sufficient |
| Endpoint count changes | Would break existing Z2M pairings; preserve 25 endpoints |

## Traceability

| Requirement | Phase |
|-------------|-------|
| *(filled by roadmap)* | |
