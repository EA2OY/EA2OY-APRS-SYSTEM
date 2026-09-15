# N-00 — Fuente de conocimiento: repo NavaTastic (SOLO LECTURA)

Mapa del yacimiento técnico usado para nutrir estas neuronas. **NUNCA escribir/ejecutar nada destructivo allí.**

## Identificación
- Repo: `C:\NavaTastic Codigo completo` — fork de Meshtastic 2.7.26, rama master (NavaTastic/Navarrico).
- Permiso: **SOLO LECTURA** (regla del operador). Escritura únicamente en el brain `Cerebro_Faketec_APRS_Igate_EA2OY\`.
- Relación: Meshtastic es GPLv3. Si importamos código (drivers, lógica), nuestro firmware debe ser compatible GPLv3. Referencias/arquitectura no copiada = sin obligación.
- Documentación "operativa" del repo (protegida, NO reescribir nunca): `docs/cerebro/*`, `docs/Guia_para_agente_sobre_NavaTastic.md`, `docs/BITACORA_TECNICA.md`, `docs/INCIDENCIAS_AUDITORIA_BANCO.md`, `docs/METODO_AUDITORIA_BANCO.md`, `docs/PLAN_DE_TRABAJO.md`, `docs/FAQ_RESILIENCIA_Y_DESPLIEGUE.md`, `docs/Manual_uso_NavaTastic.md`, `docs/DIFERENCIAS_VS_UPSTREAM.md`.

## Árbol clave para nosotros
- Envs nRF52: `variants\nrf52840\navarrico.ini` (12 envs `navarrico_<placa>_<radio>_<rama>`).
- Base nRF52 común: `variants\nrf52840\nrf52.ini` y `variants\nrf52840\nrf52840.ini`.
- Placa Faketec/Promicro: `variants\nrf52840\diy\nrf52_promicro_diy_tcxo\` (variant.h, readme con esquemático PDF).
- Xiao Kit I2C: `variants\nrf52840\seeed_xiao_nrf52840_kit\variant.h` (sirve de referencia OLED/E22P).
- Radio: `src\mesh\RadioInterface.cpp`, `src\mesh\SX126xInterface.{h,cpp}`, `src\mesh\RadioLibInterface.cpp`.
- Energía/sueño: `src\Power.cpp`, `src\PowerFSM.cpp`, `src\sleep.cpp`, `src\platform\nrf52\main-nrf52.cpp`.
- Resiliencia: `src\modules\NavaCLIModule.{h,cpp}`, `src\mesh\NodeDB.cpp`.
- Sensores: `src\modules\Telemetry\{EnvironmentTelemetry,PowerTelemetry}.cpp`, `src\modules\Telemetry\Sensor\*.cpp`, `src\detect\ScanI2CTwoWire.cpp`, `src\configuration.h`.
- Pantalla: `src\graphics\Screen.cpp`, librería `esp8266-oled-ssd1306` (`AutoOLEDWire.h`).
- Build/UF2: `extra_scripts\nrf52_extra.py`, `bin\uf2conv.py`, `distribuir.ps1`, `verificar_paridad.ps1`, `build.ps1`, `boards\promicro-nrf52840.json`.

## Fuentes duplicadas / históricas SOLO LECTURA (no tocar)
- `Desktop\NavaTastic 4.3 120826` (binarios V4), `Desktop\NavaTastic V5 4.3.4`, `C:\Firmware Navarrico 4.3`, `Desktop\firmware`.
- `280_alpha\` y `ESTUDIO_IMPLEMENTACION_280.MD` = **sesión paralela del operador: nunca abrir/citar**.

## Enlaces entre neuronas (qué se extrae de dónde)
| Neurona | Fuente principal en NavaTastic |
|---|---|
| N-01 Faketec pinout | variant.h promicro_diy_tcxo + navarrico.ini |
| N-02 Radio E22P vs SX1262 | variant.h, RadioInterface/SX126xInterface, DIFERENCIAS_VS_UPSTREAM |
| N-03 Energía/sleep/resiliencia | Power.cpp, PowerFSM, main-nrf52, NavaCLIModule, NodeDB, 04_energia_bateria, FAQ_RESILIENCIA |
| N-04 Sensores I2C (INA/BME/AHT) | Telemetry/Sensor/*, ScanI2CTwoWire, configuration.h |
| N-05 Display OLED | Screen.cpp, oled-ssd1306 AutoOLEDWire, PowerFSM |
| N-06 Build/UF2/paridad | nrf52.ini, nrf52_extra.py, distribuir.ps1, verificar_paridad.ps1, BITACORA L59-L66 |
| N-07 Lecciones e incidencias | BITACORA_TECNICA, INCIDENCIAS_AUDITORIA_BANCO, METODO_AUDITORIA_BANCO |
