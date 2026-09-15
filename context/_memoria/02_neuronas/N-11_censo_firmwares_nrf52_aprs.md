# N-11 — Censo de firmwares APRS nRF52840 + hallazgos CA2RXU (2026-09-09)

Investigación web verificada (API GitHub + raw + grep.app). Conclusión clave: **NO existe fuente compilable del firmware nRF52 de CA2RXU; solo UF2 + configuradores web**. Hay 1 firmware nRF52840 completo y compilable: cfr34k.

## A) CA2RXU nRF52: solo binarios UF2 (sospecha CONFIRMADA)
- Repo: `github.com/richonguzman/nrf52840-web-config` (GPL-3.0, HTML + UF2, activo 2026-08). Contiene 3 configuradores web (Digipeater/Tracker/TNC) + `firmwares/` con **32 UF2** (NRF52840: Faketec digi, Faketec 1W 400M30S, TNC, Trackers SSD1306/SH1106, Delete_Config; + 3 RP2040). CERO código C/C++ en los 14 repos de richonguzman (árboles completos escaneados).
- Descarga directa: `raw.githubusercontent.com/richonguzman/nrf52840-web-config/main/firmwares/NRF52840_Digipeater_FAKETEC_2026_08_16.uf2` (SHA-256 66679528…f83f74; ~358 KB). También `NRF52840_Digipeater_FAKETEC_1W_400M30S_2026_08_16.uf2` (para E22P-433 1W).
- Config del firmware = **USB-CDC serial 115200 vía WebSerial en navegador** (no ficheros/WiFi/web AP). Los 3 HTML **sin minificar** revelan el protocolo de configuración completo → fuente documental valiosa para replicar formato de config (callsign, path, símbolo, frequency libre 433/868/915, SF/BW/CR, power, digiMode, **digiUltraEcoMode**, cadActive, blacklist, beaconInterval, wxSensorActive, telemetría).
- Tracker nRF52: perfiles LoRa lora1..4, beacons SmartBeacon, eco GPS, batería, pantalla SSD1306/SH1106, y **BLE** (`bluetooth_active`, deviceName "LoRaTracker", `bluetooth_useKISS`).
- Ingeniería inversa: VIABLE (UF2 sin cifrar, Cortex-M4; extraer con uf2conv.py). NOTA legal: GPL-3.0 exige fuente → el propio autor incumple la GPL distribuyendo binarios sin fuente; no copiar su binario.
- Contexto: existió `LoRa_APRS_NRF52840` (digi/tracker RAK4631) hoy **404**; snapshot binario UF2 en `jafo2128/LoRa_APRS_NRF52840` (MIT, solo binarios + README: RAK4631 digi, EcoMode 7 mA idle, 433.775 MHz, SF7-12/BW125/CR5/22 dBm).

## B) Censo nRF52840-APRS (código real)
| Proyecto | Licencia | Estado | Placa/Radio | Valor |
|---|---|---|---|---|
| **cfr34k/t-echo-lora-aprs** | MIT (+Nordic main.c) | Activo (43★) | T-Echo nRF52840 + SX1262 | **LA JOYA**: único firmware nRF52840 completo y compilable. Bare-metal C, Nordic nRF5 SDK + make, UF2 con SoftDevice s140. APRS 433.775 (SF12/BW125/CR4/5, hasta +22 dBm), deep sleep ~100 µA con BLE, GPS, e-paper, menú + BLE (passkey 6.0) + cliente Python. SOLO tracker+receptor (sin digi/iGate). `src/`: aprs.c, lora.c, periph_pwr.c, tracker.c, menusystem.c… `config/pinout.h` |
| richonguzman/LoRa_APRS_iGate | GPL-3.0 | Muy activo | ESP32 (variants ESP32) | Lógica de RED APRS completa (digi WIDE/cross-freq, mensajes/queries/filtros, telemetría) validada en hardware objetivo; portar su capa C++ |
| richonguzman/LoRa_APRS_Tracker | GPL-3.0 | Activo | ESP32 | Tracker/mensajes/telemetría |
| richonguzman/nrf52840-web-config | GPL-3.0 | Activo | — | UF2 + protocolo de config serial documentado en HTML |
| jafo2128/LoRa_APRS_NRF52840 | MIT | Snapshot | RAK4631 | Solo UF2 (sin fuente) |
| F4AVI/LoRa-TEcho-APRSTracker | GPL-3.0 | Inactivo 2022 | T-Echo Arduino | Histórico |
| lora-aprs/* (org OE5BPA archivada) | MIT | Archivado | ESP32 | Libs APRS-Decoder-Lib, APRS-IS-Lib → lógica APRS reutilizable |

## C) Drivers/librerías reutilizables
- **RadioLib 7.7.1** (MIT): `architectures=*` incluye nRF52; SX126x + **AX25Client y APRSClient** → vía rápida para framing/decodificación sobre Arduino Adafruit nRF52. Verificar en banco la interoperabilidad con iGates CA2RXU.
- lora-aprs/APRS-Decoder-Lib, APRS-IS-Lib (MIT, archivadas): lógica APRS de OE5BPA.
- No-nRF52 de referencia: `afourney/aprstastic` (APRS↔Meshtastic, Python — útil para el concepto bridge), `lightaprs/LightGateway-1.0` + `LightTracker-Plus-1.0` (ESP32/SX1268 1W, activo), `dj1an/Cubecell_LoRa_APRS_Tracker`.

## D) Estrategia recomendada (para nuestro firmware)
1. Esqueleto MCU/energía/BLE: **cfr34k t-echo** (MIT) → adaptar `config/pinout.h` a Faketec (N-01) y drivers de radio/APRS frame.
2. Capa de red APRS (digi, mensajes, queries): **portar la capa de richonguzman/LoRa_APRS_iGate (ESP32, GPL-3.0)** → implica licencia GPL-3.0 de nuestro firmware. Alternativa si queremos MIT: reimplementar con APRSPacketLib/decoder (MIT) + RadioLib APRSClient.
3. Config: replicar el modelo serial 115200 de CA2RXU (documentado en sus HTML) + extender a BLE (patrón cfr34k/Meshtastic).
4. NO copiar binarios UF2 de CA2RXU (sin fuente, GPL rota).
- DECISIÓN DE LICENCIA PENDIENTE (operador): GPL-3.0 (portando CA2RXU) vs MIT (reimplementando con libs MIT). Nota: CA2RXU tracker original fue MIT hasta 2025-06-19.
