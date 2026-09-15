# N-04 — Sensores I2C (electrica y clima) en el stack Meshtastic/NavaTastic

Fuente: `C:\NavaTastic Codigo completo\src\modules\Telemetry\` y `src\detect\ScanI2CTwoWire.cpp`, `src\configuration.h:231-277`.

## Arquitectura de sensores (cómo se enganchan)
- Framework: `Sensor\TelemetrySensor.{h,cpp}` (base) + `Sensor\AddI2CSensorTemplate.h` (`addSensor<T>()`).
- Autodetección: `src\detect\ScanI2CTwoWire.cpp` + `ScanI2C.{h,cpp}` → llena `nodeTelemetrySensorsMap[..]` (main.cpp:230); los módulos consultan `hasSensor()`.
- Direcciones centralizadas en `configuration.h:231-277`.

## Clima (nuestro objetivo: BMP280, AHT20, BME280, BME680)
| Sensor | Driver | Dir. I2C | Detección |
|---|---|---|---|
| BME280 | `Sensor\BME280Sensor.cpp` | 0x76/0x77 | reg 0xD0=0x60 |
| BMP280 | `Sensor\BMP280Sensor.cpp` | 0x76/0x77 | reg 0xD0=0x58 |
| BME680 | `Sensor\BME680Sensor.cpp` | 0x76/0x77 (id 0x61) | vía BSEC2 o Adafruit_BME680 |
| BMP3XX/085 | BMP3XX/BMP085Sensor | 0x76/0x77 | 0x50/0x60 / 0x55 |
| **AHT20** | `Sensor\AHT10` (AHT10.cpp/.h) | **0x38** | simple case en scanner:422-425 — "AHT10 y AHT20 soportados sin modificación" (librería Adafruit_AHTX0) |
| SHT31/4X, SHTC3, MCP9808, DPS310, LPS22HB, OPT3001, MLX90614... | varios | — | — |

- En Meshtastic **no existe "AHT20Sensor" separado**: se reporta como tipo protobuf `AHT10=23`. Para APRS no importa el protobuf: nos quedamos con el driver (Adafruit_AHTX0, addr 0x38) tal cual.
- Modo lectura BME280 (referencia de bajo consumo): `MODE_FORCED`, oversampling X1, STANDBY_MS_1000, `takeForcedMeasurement()`, presión en hPa (BME280Sensor.cpp:13-44).
- BME680 con BSEC2 persiste estado a FS (carga/guardado) y publica IAQ; sin BSEC usa Adafruit + log de gas.

## Monitor electrico (nuestro objetivo: INA219, INA3221)
- Objetos globales por chip: `src\power.h:34-66` (extern) definidos en `Power.cpp:109-140`; heredan `TelemetrySensor + VoltageSensor + CurrentSensor`.
- Detección `ScanI2CTwoWire.cpp:428-474`: reg 0xFE MFG ID 0x5449(TI) → INA226(0x2260)/INA260/INA219. Direcciones INA219: 0x40/0x41/0x43 (`configuration.h:237-239`); INA3221: 0x42 (`INA3221_ADDR`).
- **INA219**: `Adafruit_INA219`; `begin(bus)`; lee bus voltage (V) y current (mA)×`INA219_MULTIPLIER` (INA219Sensor.cpp:16-51). Sirve también a la lógica de batería (`Power.cpp:599-657`, si `config.power.device_battery_ina_address` coincide).
- **INA3221**: librería `sgtwilko/INA3221`; `setShuntRes(100,100,100)`; 3 canales; canal 1 = entorno (`INA3221_ENV_CH`), resto potencia (INA3221Sensor.cpp:12-64).
- Los INA se activan con `moduleConfig.telemetry.power_measurement_enabled`; intervalo `power_update_interval`; `power_screen_enabled` para pantalla. `device_battery_ina_address` (tag 9 de PowerConfig) elige cuál INA reporta la batería del sistema.

## Protobuf/config (referencia para nuestro firmware: no necesitamos protobuf APRS, solo conceptos)
- Telemetría: `module_config.pb.h:363-401` (environment/power measurement enabled + intervalos).
- Display: `config.pb.h:516-549` (`screen_on_secs`, `oled` tipo, `wake_on_tap_or_motion`); brillo en `device_ui.pb.h:160`.
- Power: `config.pb.h:449,472` (`is_power_saving`, `device_battery_ina_address`).
- Defaults: intervalos base en `src\mesh\Default.h:18` (env 12 h) y NodeDB.cpp:959-1170.
- nRF52 en NavaTastic excluye AQ y health por compilación (`-DMESHTASTIC_EXCLUDE_AIR_QUALITY_SENSOR=1` nrf52.ini:29, `-DMESHTASTIC_EXCLUDE_HEALTH_TELEMETRY=1` platformio.ini:71) → estrategia para adelgazar también en nuestro APRS.

## Para nuestro firmware APRS (qué importar)
- Driver I2C genérico de detección por dirección + librerías Adafruit ya probadas en nRF52.
- Los 4 sensores objetivo existen y funcionan en nRF52: BME280/BMP280/BME680 (Adafruit) y AHT20 (Adafruit_AHTX0 vía driver AHT10). INA219/INA3221 igualmente probados.
- Bus I2C único en Faketec (P1.04/P0.11, WIRE_INTERFACES_COUNT 1) → display y sensores comparten bus.
