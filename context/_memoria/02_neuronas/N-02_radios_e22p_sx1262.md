# N-02 — Radios E22P vs HT-RA62/SX1262 (nRF52): diferencias de manejo

## Comparativa (misma placa ProMicro/Faketec en NavaTastic)
| Aspecto | E22P (Ebyte) | HT-RA62 (Heltec SX1262) |
|---|---|---|
| Macro env | `NAVARICO_RADIO_E22P` | `NAVARICO_RADIO_SX1262` |
| Pin P0.17 | `RADIO_POWER_ENABLE_PIN` (alimentación módulo; LOW en sleep/storm) | `SX126X_RXEN` (RF switch ext.; sin pin de alimentación: la radio se apaga por SPI `lora.sleep`) |
| RXEN/TXEN | `SX126X_RXEN RADIOLIB_NC`, `TXEN RADIOLIB_NC` (variant.h:159,163) | RXEN activo en P0.17; TXEN `RADIOLIB_NC` (DIO2 interno hace de RF switch en HT-RA62) |
| Potencia máx | `SX126X_MAX_POWER 12` + `HARDWARE_TX_POWER_LIMIT 12` (variant.h:179-181) | `22` (variant.h:183-184) |
| TX por defecto (Meshtastic) | 8 dBm conservador (Channels.cpp:111-112) | 22 dBm (Channels.cpp:117) |
| Rango set_txpower | 0-12 (NavaCLIModule.cpp:3999-4006) | 0-22 (NavaCLIModule.cpp:4007-4014) |
| TCXO | DIO3 1.8 V + `TCXO_OPTIONAL` (reintenta sin TCXO si falla) | Ídem |
| Corte OCV batería | 3500 mV | 3400 mV |

Nota banco (NavaTastic): **picos de corriente del E22P en TX** >1 dBm corrompían frames en bancada (BITACORA L14); el E22P con booster 5 V consume ~1.5 mA en System OFF vs 0.4 mA SX1262 (docs/cerebro/04_energia_bateria.md:30-32).

## Cómo se inicializa SX1262 en nRF52 (fuente NavaTastic)
1. SPI global: `src\main.cpp:868-894` — `SPI.begin()` sin args; el core Adafruit usa macros de variant (`_SPI_DEV, PIN_SPI_MISO, PIN_SPI_SCK, PIN_SPI_MOSI`), 4 MHz mode 0.
2. `src\mesh\RadioInterface.cpp:234` `initLoRa()` → si `USE_SX1262`: `new SX1262Interface(loraHal, SX126X_CS, SX126X_DIO1, SX126X_RESET, SX126X_BUSY)` (:353-355); aplica `setTCXOVoltage(SX126X_DIO3_TCXO_VOLTAGE)` y con `TCXO_OPTIONAL` reintenta sin TCXO (:366-376).
3. `src\mesh\SX126xInterface.cpp:36` init → `limitPower(SX126X_MAX_POWER)`, `lora.begin(freq, bw, sf, cr, syncWord, power, preamble, tcxoVoltage, useRegulatorLDO=false)`, `lora.setDio2AsRfSwitch(...)` (:124-134), `lora.setRfSwitchPins(SX126X_RXEN, SX126X_TXEN)` (:156-157), `setRxBoostedGainMode`, patch RX reg 0x8B5 (:170), `startReceive()`.
4. Interrupción: `lora.setDio1Action(callback)` (SX126xInterface.h:54) → `RadioLibInterface::isrRxLevel0`.
5. Alimentación E22P: HIGH en arranque (`main-nrf52.cpp:405-408`), LOW en `cpuDeepSleep()` (:532-535) y en storm (:722-726), precedido de dormir radio por SPI (`notifyDeepSleep`, :710).

## RF switch / módulos E22 con PA externa (referencia de configuración.h:119-130)
- `EBYTE_E22_900M30S` → `TX_GAIN_LORA 7`, `SX126X_MAX_POWER 22`.
- `EBYTE_E22_900M33S` → `TX_GAIN_LORA 25`, máx 8.
- E22 necesita cableado ext. RXEN o DIO2→TXEN (readme de la variante, `diy\xiao_ble\platformio.ini` con `-DEBYTE_E22` y pinout E22 D0-CS/D1-DIO1/D2-BUSY/D3-RST/D7-RXEN en seeed_xiao variant.h:125-131).

## Para nuestro proyecto APRS (433 MHz)
- Objetivo: **E22P-433M30S = SX1262** → mismo driver RadioLib SX126x; la banda (433 vs 868) NO es un flag de placa en NavaTastic (región/canal vía perfil: EU_868/869.618 SFNarrow; 869.545 solo banco). Para nosotros la frecuencia es la del estándar APRS-LoRa 433 de CA2RXU → decidir en diseño (banda EU 433).
- **TX_GAIN_LORA / potencia**: respetar límites del módulo (E22P-433M30S ~1 W con booster: mirar tabla `EBYTE_E22_900M30S` como equivalente) y la regla de no superar potencia que corrompa (L14) ni violar licencia.
- Referencia APRS con E22-400M30S (433) dentro del propio repo NavaTastic (ESP32, no nRF52): `variants\esp32\diy\9m2ibr_aprs_lora_tracker\platformio.ini` — útil como fuente de parámetros LoRa APRS.
