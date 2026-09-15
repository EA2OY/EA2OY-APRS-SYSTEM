# N-03 — Energía, sueño profundo y resiliencia anti-brownout (nRF52)

Fuente principal: `C:\NavaTastic Codigo completo` → `src\Power.cpp`, `src\PowerFSM.cpp`, `src\sleep.cpp`, `src\platform\nrf52\main-nrf52.cpp`, `src\modules\NavaCLIModule.*`, `src\mesh\NodeDB.cpp`, `docs\cerebro\04_energia_bateria.md`, `docs\FAQ_RESILIENCIA_Y_DESPLIEGUE.md`.

## A) Medición de batería (cadena ADC)
- Pin: `BATTERY_PIN P0.31` (ADC1_GPIO4), divisor real **2x1M (0.5)**, `VBAT_DIVIDER_COMP 2.0` (docs:04:66; el 0.6 del comentario es viejo).
- 15 muestras promediadas, LPF `last += (scaled-last)*0.5`, throttle 5 s (Power.cpp:308-379); `BATTERY_SENSE_RESOLUTION_BITS 12`, `AREF_VOLTAGE 3.0`.
- Fórmula: `scaled = adcMultiplier * (1000*AREF_VOLTAGE)/2^bits * raw`; 1 LSB = 0.7324 mV ×2.
- "Batería presente" = % != -1; "sin batería" si V < OCV[último]-500 (Power.cpp:462,578).
- Químicas (set_chem, NavaCLIModule.cpp:3629-3649): lipo(corte 3500/OCV último 3100), nimh(3400), sodium(2600), **lifepo4(2800, vwake 5)**. `set_vbat 2400-3600`; `set_vwake 1-5`.
- LPCOMP en ProMicro/Faketec: dinámico por química; niveles vwake (main-nrf52.cpp:643-649): 1≈2.06 V, 2≈2.48 V, 3≈3.71 V (default), 4≈4.54 V, 5≈3.30 V. Seed/Xiao/T114 = fijo por #ifdef.

## B) Estados de sueño (PowerFSM)
- Estados: BOOT, ON, POWER, SERIAL, DARK, LS*, NB*, SDS, stateLowBattSDS, SHUTDOWN (*solo ESP32; nRF52 nunca usa light sleep/NB).
- `EVENT_LOW_BATTERY` (Power.h:22) → `stateLowBattSDS` → `doDeepSleep(sds_secs)`.
- Decisión de dormir: `Power::readPowerStatus()` (Power.cpp:897-1046): contador low_voltage_counter (solo si hasBattery && !getHasUSB() && V < OCV[último]) hasta 8 lecturas → evento/doDeepSleep.
- `cpuDeepSleep()` nRF52 (main-nrf52.cpp:477-598): Wire/SPI end, BLE off, **radio E22P a LOW vía RADIO_POWER_ENABLE_PIN** (solo E22P), `delay(3000)` antes de armar LPCOMP, LPCOMP `DETECT_UP` + histéresis 50 mV, LED off, `sd_power_system_off()`. Excepción TRACKER/SENSOR: LOWPWR + timer.
- `storm` (hibernación temporizada): System ON + LOWPWR + RTC2, bloques ≤500 s, despierta por COMPARE y `NVIC_SystemReset()` (NO usa system_off).

## C) Protecciones anti-brownout (objetivo central de nuestro proyecto)
1. `powerHAL_platformInit()`: POFCON 2.2 V último recurso (main-nrf52.cpp:129-155) — **ojo: POFCON buggy, dispara <2.8 V y corrompe flash/BLE**.
2. `powerHAL_isPowerLevelSafe()`: umbral 2.7 V + histéresis 0.2 V; `waitUntilPowerLevelSafe()` en setup antes de inicializar nada (main.cpp:289-308).
3. Pre-check de batería al arranque: 8 lecturas ×200 ms (force=true) antes de encender radio/flash; si < corte-100 mV → boot "Reserva" + re-sueño; en [corte-100, corte) → boot "Vivo" + re-sueño (main.cpp:537-597). **Solo aplica si NO USB**.
4. Monitor runtime: 8 lecturas (~160 s) bajo corte → sueño limpio (evita brownout con sistema encendido).
5. LPCOMP con histéresis: despierta solo con tensión estable; `delay(3000)` previo es CRÍTICO (no eliminar).
6. Consumos reales: **0.4 mA Faketec SX1262 System OFF**; 1.5 mA nRF52840+E22P+booster 5 V.

## D) Resiliencia: `/resilience.bin` (archivo que sobrevive a factory reset)
- Qué es: fichero binario en raíz del FS, estructura `ResiliencePrefs` (NavaCLIModule.h:75-179): magic 0x52455349, versión "NAV9", química/corte/vwake, tx_disabled, ble_disabled, rol, autofavoritos, claves, canales, LoRa física, nombre, intervalos, CRC32.
- Escritura atómica: `/resilience.tmp` → borra → renombra (NavaCLIModule.cpp:841-858). Se escribe SOLO en eventos raros (set_* comandos, sync App, evento batería crítica, pánico, primer deploy) — cero escrituras por tráfico.
- Carga con validación (magic, versión, CRC32, saneo); si inválido → **Clean Slate** purga y "Línea de Base de Supervivencia" (química por defecto del env).
- Sobrevive a factory reset porque `NodeDB::factoryReset()` solo hace `rmDir("/prefs")` y resilience.bin vive en la raíz. `wipe` sí lo borra.
- Patrón a replicar en nuestro firmware APRS: **config que sobrevive a reset y a batería agotada** (callsign, frecuencia, umbrales) con escritura atómica + CRC.

## E) Comandos nRF52-only (referencia conceptual para nuestra UI/serial)
- `set_chem <lipo|nimh|sodium|lifepo4>` (NavaCLIModule.cpp:3617-3659): tripleta química+corte+vwake + OCV. LiFePO4 rechazada en placas con LPCOMP fijo (Seed/Xiao/T114) por no poder cambiar umbral.
- `set_vbat <2400-3600>` (3660-3680), `set_vwake <1-5>` (3681-3715, valida wakeMv > corte).
- `storm [1-720]h|test1|test2` (3716-3749): hibernación temporizada con gracia 60 s.
- `ble on|off` (3762-3787): persiste ble_disabled.

## F) Lecciones de campo aplicables
- **Flasheo nRF52**: SOLO por unidad UF2 (doble reset → NICENANO) — touch 1200 bps/nrfutil falla error 22 Windows (L60, I1).
- Limpieza de pantalla/LED antes de system_off (GPIO enclavados ~10 mA) (L19-21).
- No escribir /resilience ni config con frecuencia (wear flash) (L7: FILE_O_WRITE no trunca → remove() antes).
- ESP32 (Heltec V4) colgaba con factoryReset+primera escritura LittleFS → en ESP32 usar applyProfileDefaults (sin rmDir); **nRF52 conserva factoryReset** (I20) — nosotros solo atacamos nRF52 de momento.
