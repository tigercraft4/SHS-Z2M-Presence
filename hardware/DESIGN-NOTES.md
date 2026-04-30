# SHS-Z2M-Presence PCB Design Notes

## Repositorio

- **Repo local**: `C:\Users\z004shhs\Documents\Scripts\1Personal\SHS-Z2M-Presence`
- **GitHub**: https://github.com/tigercraft4/SHS-Z2M-Presence
- **Branch hardware**: `hardware/add-pcb-designs` (push pendente — precisa autenticacao)
- **Projeto original (temp)**: `C:\Users\z004shhs\Documents\temp\SHS-Z2M-Presence-PCB`

## Versoes

### v1 — Fabricada e testada
- Usa ESP32-C6 **dev board** (nao modulo standalone)
- Footprint custom `ESP32_C6_DEVKITC1` (dev kit completo com USB, CH343, etc.)
- LD2410C + LD2450 com headers THT
- Board: 54.5 x 76mm, 2 camadas
- Gerbers e BOM JLCPCB incluidos em `hardware/v1/`
- Fabricada com sucesso, firmware funcional

### v2 — Work in progress (redesign standalone)
- ESP32-C6-WROOM-1-N8 **modulo standalone** (sem dev board)
- USB-C direto (GCT USB4085) com CC pull-downs 5.1k
- AP2112K-3.3 LDO (5V -> 3.3V)
- LD2410C + LD2450 mesmos sensores radar
- Ficheiros em `hardware/v2/`

## Analise da v2 — Problemas Encontrados e Correcoes

### CRITICOS (PCB — pendente)
1. **Routing nao iniciado** — 0 tracks, 0 vias, 16/17 nets unrouted
2. **Single layer** — so F.Cu, precisa minimo 2 camadas (ground plane para RF)
3. **5V/3.3V domain crossing** — UART e RADAR_OUT entre ESP32 (3.3V) e radares (5V VCC)
   - **Aceitavel**: LD2410C/LD2450 usam regulador interno, IO a 3.3V — sem level shifter necessario
4. **ESP32 EPAD sem thermal vias** — precisa minimo 9 vias 0.3mm drill

### CORRIGIDOS no schematic (2026-04-30)
5. ~~**10uF em 0402**~~ — **CORRIGIDO**: C3/C5/C7 mudados para `C_0805_2012Metric`
6. ~~**Sem ESD no USB**~~ — **CORRIGIDO**: Adicionado U5 USBLC6-2SC6 (SOT-23-6) com MPN
7. **53% partes sem MPN** — 8/17 unique parts tem MPN (melhorado de 7/24 apos remocao de TPs/MHs)
8. ~~**U4 EN pin floating**~~ — **CORRIGIDO**: Wire reconectado de EN (56.11, 37.54) a IN via +5V net
   - Nao era false positive — wire endpoint estava 5mm desalinhado do pin
9. ~~**Sem fiducials**~~ — **CORRIGIDO**: Adicionados FID1/FID2/FID3 (Fiducial_1mm_Dia_2mm_Outer)
10. ~~**LEDs desconectados**~~ — **CORRIGIDO**: LED1 rotacao 90°→180°, LED2 rotacao 0°→270°
    - Nao eram false positives — pins nao alinhavam com wires due to rotacao errada
    - Polaridade verificada: STAT_LED→R5→LED1.A→LED1.K→GND, +3V3→R6→LED2.A→LED2.K→GND
11. ~~**Test points e mounting holes**~~ — **REMOVIDOS**: TP1-TP8 e H1-H4 removidos per design decisions
    - Componentes reduzidos de 36 para 28

### MEDIOS (PCB — pendente)
12. **Courtyard overlaps** — C2/U3 (1.4mm2), C1/U2, C3/U2 — resolver no layout

### Analyzer false positives remanescentes
- SW1/SW2 aparecem floating — estao conectados via wires no schematic
- U4.EN ainda reporta floating — limitacao do analyzer com T-junctions (wire correto em 56.11, 35)

## Plano v2 Compacta — Decisoes do Utilizador

### Decisoes tomadas
- **Sem mounting holes** (montagem por encaixe/cola)
- **Sem test points** (debug via USB serial)
- **Sensores (LD2410C + LD2450) com headers THT em F.Cu** (frente)
- **Tudo o resto em B.Cu** (ESP32, LDO, caps, resistors, LEDs, switches)
- **2 camadas obrigatorio**

### Orientacao dos sensores (CRITICO)
- Do projeto GitHub: "The 4 antenna patches (gold squares) must be positioned at the top of the enclosure"
- **LD2450**: rodar 90° CW no PCB para que "Up" fique no topo da board
- **LD2410C**: orientacao padrao (antenna no topo)
- Ambas antenas devem apontar na mesma direcao

### Tamanho alvo
- **Atual**: 100 x 70 mm = 7000 mm2
- **Alvo**: ~39 x 49 mm = ~1900 mm2 (**73% reducao**)

### Layout conceito
```
=== F.Cu (FRENTE - sensores, virado para zona de detecao) ===

     antenas ↑ (topo da board = direcao de detecao)
  ┌──────────────────────────────────────┐
  │  LD2410C      │      LD2450          │
  │  16x16mm      │      15x44mm        │
  │  antenna ↑    │      (rotated 90°)  │
  │               │      antenna ↑      │
  │  pins (THT)↓  │                     │
  │───────────────│                     │
  │ (espaco       │                     │
  │  l────────────────────────────────┘
       ~39mm largura x ~49mm altura

=== B.Cu (TRAS - componentes SMD, vista espelhada) ===

  ┌──────────────────────────────────────┐
  │  C1 C4 C5     │  C2 C3              │
  │  (decoupling) │  (decoupling)       │
  │               │                     │
  │  ┌──────────────────────┐           │
  │  │   ESP32-C6-WROOM     │  R5 LED1  │
  │  │   (U1) 18x20mm      │  R6 LED2  │
  │  │   EPAD + thermal     │           │
  │  │   vias               │           │
  │  └──────────────────────┘           │
  │  U4(LDO) R1 C8  R4                 │
  │  SW1(RST) SW2(BOOT)                │
  │  C9(bulk) R2 R3  C6 C7             │
  │  ═══[USB-C J1]═══ (edge mount)     │
  └──────────────────────────────────────┘
```

## Componentes v2 (28 total — atualizado 2026-04-30)

| Ref | Valor | Footprint | Funcao | MPN |
|-----|-------|-----------|--------|-----|
| U1 | ESP32-C6-WROOM-1-N8 | Custom castellated | MCU WiFi/BLE/Zigbee | ESP32-C6-WROOM-1-N8 |
| U2 | LD2410C | Custom THT 5-pin | Radar presenca 24GHz | LD2410C |
| U3 | LD2450 | Custom THT 8-pin | Radar multi-target 24GHz | LD2450 |
| U4 | AP2112K-3.3 | SOT-23-5 | LDO 5V->3.3V 600mA | AP2112K-3.3TRG1 |
| U5 | USBLC6-2SC6 | SOT-23-6 | USB ESD protection | USBLC6-2SC6 |
| J1 | USB_C_Receptacle | GCT USB4085 | Alimentacao + USB data | USB4085-GF-A |
| R1 | 10k | 0402 | Pull-up ESP_EN | - |
| R2 | 5.1k | 0402 | CC1 pull-down USB-C | - |
| R3 | 5.1k | 0402 | CC2 pull-down USB-C | - |
| R4 | 10k | 0402 | Pull-up BOOT | - |
| R5 | 330R | 0402 | LED1 current limit | - |
| R6 | 330R | 0402 | LED2 current limit | - |
| C1 | 100nF | 0402 | Decoupling U2 +5V | - |
| C2 | 100nF | 0402 | Decoupling U3 +5V | - |
| C3 | 10uF | 0805 | Bulk U3 +5V | - |
| C4 | 100nF | 0402 | Decoupling U4 IN | - |
| C5 | 10uF | 0805 | Bulk U4 IN | - |
| C6 | 100nF | 0402 | Decoupling U4 OUT | - |
| C7 | 10uF | 0805 | Bulk U4 OUT | - |
| C8 | 1uF | 0402 | Reset RC filter | - |
| C9 | 22uF | 0805 | VBUS bulk (Espressif ref) | - |
| LED1 | LED_G | 0402 | Status/Zigbee (IO2) | - |
| LED2 | LED_R | 0402 | Power OK (+3V3) | - |
| SW1 | B3U-1000P | SMD | Reset button | B3U-1000P |
| SW2 | B3U-1000P | SMD | Boot button | B3U-1000P |
| FID1 | Fiducial | 1mm/2mm | SMD assembly ref | - |
| FID2 | Fiducial | 1mm/2mm | SMD assembly ref | - |
| FID3 | Fiducial | 1mm/2mm | SMD assembly ref | - |

## Wiring (GPIO assignments — compativel com firmware v1)

| ESP32 Pin | GPIO | Funcao | Ligado a |
|-----------|------|--------|----------|
| 3 | EN | Reset | R1 pull-up + C8 + SW1 |
| 4 | IO4 | UART1_TX | U2 (LD2410C) RX |
| 5 | IO5 | UART1_RX | U2 (LD2410C) TX |
| 13 | IO12 | USB D- | J1 D- |
| 14 | IO13 | USB D+ | J1 D+ |
| 15 | IO9 | BOOT | R4 pull-up + SW2 |
| 16 | IO18 | UART0_TX | U3 (LD2450) RX |
| 17 | IO19 | UART0_RX | U3 (LD2450) TX |
| 26 | IO3 | RADAR_OUT | U2 (LD2410C) OUT |
| 27 | IO2 | STAT_LED | R5 -> LED1 |

## Thermal

- U4 (AP2112K): Tj estimado 68°C, margem 57°C — sem problemas termicos
- U1 (ESP32): EPAD precisa thermal vias (minimo 9x 0.3mm drill)

## Interferencia Radar (do firmware)

- LD2450 interfere com LD2410C
- Firmware mitiga: "LD2410C presence is only reported if LD2450 has also detected at least one target"
- No PCB: manter separacao fisica entre os dois modulos radar
