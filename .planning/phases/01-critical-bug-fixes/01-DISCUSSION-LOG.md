# Phase 1: Critical Bug Fixes & Concurrency Safety - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-30
**Phase:** 01-critical-bug-fixes
**Areas discussed:** TS-01 Re-evaluation, Mutex Strategy, Zigbee Retry Logic, NVS Zone Save Routing

---

## TS-01 Re-evaluation

| Option | Description | Selected |
|--------|-------------|----------|
| Marcar como já resolvido | Remover do scope — não há bug aqui | ✓ |
| Manter como verificação | Adicionar assert/log que confirma init order no boot | |
| Expandir scope | Verificar TODOS os semaphores/mutexes para init order correcta | |

**User's choice:** Marcar como já resolvido
**Notes:** Confirmado que `target_data_mutex = xSemaphoreCreateMutex()` (L3031) é chamado antes de `ld2450_init()` (L3053). Não há crash.

---

## Mutex Strategy

| Option | Description | Selected |
|--------|-------------|----------|
| Mutex único global | Um mutex para target_count + zone vars + position_reporting | |
| 2 mutexes + volatile | target_data_mutex (existente) + zone_config_mutex (novo) + volatile para bools | ✓ |
| Mutex por recurso | Um mutex por recurso (targets, zones, flags) | |

**User's choice:** 2 mutexes + volatile para flags
**Notes:** User pediu recomendação. Agent recomendou esta opção por balancear simplicidade vs granularidade no ESP32-C6 single-core.

---

## Zigbee Retry Logic

| Option | Description | Selected |
|--------|-------------|----------|
| Retry inline com backoff | 3 tentativas (50ms, 100ms, 200ms) no macro existente | ✓ |
| Queue + retry worker | Se lock falhar, enqueue para retry posterior | |
| Lock-free via task queue | Usar esp_zigbee_task_queue_post() sem lock | |

**User's choice:** Retry inline com backoff
**Notes:** User pediu recomendação. Agent recomendou por ser mudança localizada no macro sem estado extra. Lock-free deferred to Phase 4.

---

## NVS Zone Save Routing

| Option | Description | Selected |
|--------|-------------|----------|
| Integrar no save_worker | Novo tipo SHS_SAVE_ZONE_CONFIG no enum existente | ✓ |
| Mover chamada para worker | Mesma função, contexto diferente | |
| Deixar como está | Zone config só muda raramente | |

**User's choice:** Integrar no save_worker
**Notes:** User pediu recomendação. Agent recomendou por re-usar infra existente. 31 NVS writes mantidas (batching é Phase 3).

---

## Agent's Discretion

- Lock timeout adjustment (if retry changes effective wait time)
- Optional configASSERT at boot for mutex init verification
- Log levels for retry attempts

## Deferred Ideas

- Lock-free Zigbee via `esp_zigbee_task_queue_post()` → Phase 4
- NVS batch commit (single nvs_commit for zones) → Phase 3
- Diagnostic counters via Zigbee → Phase 3
