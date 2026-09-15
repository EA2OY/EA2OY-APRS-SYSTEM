# N-12 — Superficie de configuración del firmware (catálogo de "lo ajustable")

Fuentes SOLO LECTURA (2026-09-09):
- `_referencias\nrf52840-web-config\{Digipeater,Tracker,TNC}_Web_Configurator.html` (única doc del firmware nRF52 de CA2RXU; NO hay fuente C++).
- `_referencias\LoRa_APRS_iGate_HEAD\{common_settings.ini, include\configuration.h, src\configuration.cpp, data\igate_conf.json}` (superset ESP32).

## A) Protocolo de config nRF52 de CA2RXU (a replicar/mejorar)
- **USB-CDC 115200 8N1** vía WebSerial en navegador.
- **Una línea JSON + `\n`** por envío; reemplazo COMPLETO del snapshot; **sin lectura/ACK/handshake** (fire-and-forget; los HTML nunca abren reader).
- Claves JSON = `id` de los inputs del HTML. Tipos: string/number/boolean. Validación web: freq 430-928 MHz, SF 7-12, CR4 5-8, BW 62.5-500 kHz, potencia 2-22 dBm.
- Borrado de config: UF2 dedicado `NRF52840_Delete_Config.uf2` (no hay comando serial visible).

### Trama digi JSON exacta (referencia para nuestro formato):
```json
{"callsign":"NOCALL-11","path":"WIDE1-1","comment":"","status":"","symbol":"#","overlay":"L","beaconInterval":15,"latitude":0,"longitude":0,"ambiguityLevel":0,"digiMode":0,"digiUltraEcoMode":false,"cadActive":true,"blacklist":"","frequency":433775000,"spreadingFactor":12,"codingRate4":5,"signalBandwidth":125,"power":22,"sendBatteryTelemetry":false,"wxSensorActive":false,"heightCorrection":0,"temperatureCorrection":0}
```
Nota: `path` "(Empty)" envía string `"0"` (posible bug/convención); `signalBandwidth` en kHz (float); frequency en Hz.

## B) Catálogo Digipeater nRF52 (campos, rangos, defectos)
| Clave | Tipo/rango | Defecto |
|---|---|---|
| callsign | texto (UPPERCASE) | NOCALL-11 |
| path | select: 0/empty, WIDE1-1, WIDE1-1,WIDE2-1, WIDE1-1,WIDE2-2, WIDE2-1, WIDE2-2, RFONLY | WIDE1-1 |
| comment / status | texto libre | "" |
| symbol / overlay | 1 char | # / L |
| beaconInterval | int min ≥10 | 15 |
| latitude/longitude | float ±90/±180 | 0 |
| ambiguityLevel | 0=no,1=~110m,2=~1.1km,3=~11km | 0 |
| digiMode | 0=OFF,1=WIDE1-1,2=WIDE2-n | 0 |
| digiUltraEcoMode | bool | false |
| blacklist | callsigns por espacio, `*` comodín | "" |
| frequency | int Hz 430000000-928000000 | 433775000 |
| power | dBm 2-22 | 22 |
| spreadingFactor | 7-12 | 12 |
| codingRate4 | 5-8 | 5 |
| signalBandwidth | kHz: 62.5/125/250/500 | 125 |
| cadActive | bool | **true** |
| sendBatteryTelemetry | bool (voltaje telemetría Base91) | false |
| wxSensorActive | bool | false |
| heightCorrection | m ≥0 | 0 |
| temperatureCorrection | °C −5..5 step 0.1 | 0 |

## C) Adicionales Tracker (referencia futura tracker mode)
- beacons[0..2]: callsign/symbol/overlay/micE/comment/status + smartBeaconActive + smartBeaconSetting(0=Human,1=Bicycle,2=Car) + gpsEcoMode.
- other: path, cadActive, sendCommentAfterXBeacons(10), nonSmartBeaconRate(15), standingUpdateTime(15), email, rememberStationTime(30), sendAltitude, disableGPS, simplifiedTrackerMode.
- display: ecoMode, timeout(s), showSymbol, turn180.
- bluetooth: active, deviceName("LoRaTracker"), useKISS.
- lora[0..3] 4 perfiles: frequency/SF/CR/BW/power.
- battery: sendVoltage, voltageAsTelemetry, sendVoltageAlways, monitorVoltage, sleepVoltage(2.5-4.2, def 2.9).
- telemetry: active, sendTelemetry, temperatureCorrection.
- winlink: password. notification: led/buzzer/beeps/pines.

## D) Superset ESP32 iGate (lo que el nRF52 NO tiene — para decidir qué portamos)
- Config en `/igate_conf.json` (SPIFFS) con migración+restart; Web GUI.
- Extra vs nRF52-digi: tacticalCallsign, personalNote, rememberStationTime, startupDelay, rebootMode; beacon: sendViaAPRSIS/sendViaRF/beaconFreq/gpsActive/statusPacket; WiFi APs; APRS-IS (passcode/server/port/filter/messagesToRF/objectsToRF); digi.backupDigiMode; **LoRa RX/TX separados (rxFreq/txFreq...) + rxActive/txActive**; battery externa (divisores, sleepVoltage 10.9 V ext); wx: heightCorrection/temperatureCorrection; syslog/tnc/mqtt/ota/webadmin/ntp/remoteManagement(managers, rfOnly).
- `common_settings.ini` usa `RADIOLIB_EXCLUDE_*` para adelgazar RadioLib (patrón a copiar).

## E) Diferencias clave (para nuestro diseño)
- nRF52-digi CA2RXU: 1 perfil LoRa compartido; SIN GPS beacon; SIN internet; telemetría solo voltaje; beacon con símbolo `#` overlay `L` (en ESP32: símbolo `a`).
- Nuestro firmware deberá decidir qué copiamos del digi nRF52 (modelo serial JSON), qué mejoramos (ACK/lectura, sección de config versionada+CRC estilo resilience.bin N-03), y qué tomamos del superset ESP32 (perfiles RX/TX, tactical, eco/backup).
- Pendiente de diseño: ¿config en flash interna nRF52 (páginas dedicadas) + comando `wipe`?, ¿lectura de vuelta de la config actual?, ¿validación de llamada EA2OY?
