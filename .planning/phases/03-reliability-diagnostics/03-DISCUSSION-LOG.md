# Phase 3: Reliability & Diagnostics - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-30
**Phase:** 03-reliability-diagnostics
**Areas discussed:** Task watchdog strategy, Sensor health exposure, NVS batching approach, Diagnostic counter exposure

---

## Task Watchdog Strategy

### Q1: How should frozen sensor tasks be detected?

| Option | Description | Selected |
|--------|-------------|----------|
| ESP-IDF TWDT | Use esp_task_wdt API — tasks subscribe and must call esp_task_wdt_reset() periodically. Built-in panic/reset on timeout. Minimal code. | ✓ |
| Custom heartbeat | Each task writes a timestamp to shared state; a monitor task checks for staleness. More flexible but more code to maintain. | |
| Both combined | TWDT for hard crash protection + custom heartbeat for soft monitoring (e.g. sensor stuck returning same data). | |

**User's choice:** Asked for recommendation → ESP-IDF TWDT selected
**Notes:** Native ESP-IDF support, sensor tasks already have 20ms loops making reset() trivial

### Q2: Which tasks should be monitored by TWDT?

| Option | Description | Selected |
|--------|-------------|----------|
| Só sensor tasks (2) | shs_ld2410_task + shs_ld2450_task — interact with UART hardware | ✓ |
| Sensor + save_worker (3) | Add save_worker for NVS write hang detection | |
| Todas exceto Zigbee (4) | All tasks except zigbee_main | |

**User's choice:** Asked for recommendation → Only sensor tasks (2) selected
**Notes:** save_worker and boot_button are low-risk (NVS/GPIO only)

### Q3: TWDT timeout value?

| Option | Description | Selected |
|--------|-------------|----------|
| 5 segundos | Fast detection, 250 iterations without reset | |
| 10 segundos | More tolerance for LD2450 reconnection delays | ✓ |
| 30 segundos | Very conservative, only catches complete crashes | |

**User's choice:** Asked for recommendation → 10 seconds selected
**Notes:** LD2450 reconnection has legitimate 500ms delays

### Q4: Action on TWDT timeout?

| Option | Description | Selected |
|--------|-------------|----------|
| Só log (sem reboot) | Log ESP_LOGW + reset TWDT, no restart | |
| Reboot automático | Full ESP32 reboot via CONFIG_ESP_TASK_WDT_PANIC=y | ✓ |
| Log primeiro, reboot depois | Log first trigger, reboot if not recovered | |

**User's choice:** Asked for recommendation → Automatic reboot selected
**Notes:** Mains-powered device, NVS retains config, Zigbee auto-rejoin

---

## Sensor Health Exposure

### Q1: How to expose sensor connected/disconnected via Zigbee?

| Option | Description | Selected |
|--------|-------------|----------|
| Atributos no config cluster 0xFDCD | Bool attrs on existing config cluster, no new endpoints | ✓ |
| Novo endpoint dedicado | New EP26 for health/diagnostics | |
| Reutilizar endpoints existentes | Use existing genBinaryInput endpoints | |

**User's choice:** Asked for recommendation → Config cluster 0xFDCD attributes selected
**Notes:** Preserves 25-endpoint model, minimal converter changes

### Q2: LD2410C disconnect detection?

| Option | Description | Selected |
|--------|-------------|----------|
| Timeout de frames UART | Track last valid frame timestamp, mark disconnected after threshold | ✓ |
| Polling ativo com comando | Send periodic read commands | |
| Sem callback = disconnected | Detect if callback never fires after boot | |

**User's choice:** Asked for recommendation → UART frame timeout selected
**Notes:** Passive monitoring, consistent with LD2450 approach

### Q3: LD2450 reconnection Zigbee sync?

| Option | Description | Selected |
|--------|-------------|----------|
| Report Zigbee no evento de reconexão | Update health attribute on reconnect event | |
| Re-report completo de estado | Full re-report of zones + target count | |
| You decide | Agent's discretion | ✓ |

**User's choice:** You decide
**Notes:** Agent has flexibility on sync depth

---

## NVS Batching Approach

### Q1: How to batch 31 zone writes?

| Option | Description | Selected |
|--------|-------------|----------|
| nvs_set_blob com struct packed | Single key with packed struct, 1 write + 1 commit | |
| Individual writes + single commit | Keep 31 nvs_set_* but single nvs_commit | |
| You decide | Agent's discretion | ✓ |

**User's choice:** You decide
**Notes:** Agent considers stack constraints (3072B save_worker)

### Q2: NVS error handling?

| Option | Description | Selected |
|--------|-------------|----------|
| Log + fallback silencioso | ESP_LOGW + continue with in-memory values | |
| Log + flag Zigbee | Log + expose error flag via Zigbee | |
| You decide | Agent's discretion | ✓ |

**User's choice:** You decide

---

## Diagnostic Counter Exposure

### Q1: How to expose diagnostic counters via Zigbee?

| Option | Description | Selected |
|--------|-------------|----------|
| Atributos no config cluster 0xFDCD | Add uint32 attrs on existing config cluster | |
| Novo cluster dedicado | New manufacturer-specific cluster for diagnostics | |
| You decide | Agent's discretion | ✓ |

**User's choice:** You decide

### Q2: Which diagnostic data to expose?

| Option | Description | Selected |
|--------|-------------|----------|
| Só contadores existentes (5) | lock_success, lock_fail, tx_success, tx_fail, consecutive_fails | |
| Contadores + uptime + heap (7) | 5 counters + uptime seconds + free heap | |
| You decide | Agent's discretion | ✓ |

**User's choice:** You decide

### Q3: ZB-04 connectivity check improvement?

| Option | Description | Selected |
|--------|-------------|----------|
| Refinar mecanismo existente | Improve existing 60s/180s logic, update health attrs on rejoin | |
| Reescrever do zero | Full rewrite with more robust retry logic | |
| You decide | Agent's discretion | ✓ |

**User's choice:** You decide

---

## Agent's Discretion

- NVS batching approach (blob vs grouped writes)
- NVS error handling granularity
- Diagnostic counter set and Zigbee exposure mechanism
- LD2450 reconnection Zigbee sync depth
- ZB-04 connectivity check refinements

## Deferred Ideas

- "Sensor stuck returning same data" detection — future milestone (data quality analysis)
- NVS wear leveling / partition health monitoring — out of scope
