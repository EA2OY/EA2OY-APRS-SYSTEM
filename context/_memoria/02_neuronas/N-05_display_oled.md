# N-05 — Display OLED I2C en nRF52 (cómo lo hace Meshtastic/NavaTastic)

Fuente: `C:\NavaTastic Codigo completo` → `src\graphics\Screen.cpp`, librería `esp8266-oled-ssd1306` (ThingPulse), `src\detect\ScanI2CTwoWire.cpp:52-87`, `src\PowerFSM.cpp`, `src\platform\nrf52\architecture.h`.

## Driver y autodetección
- Librería ThingPulse `esp8266-oled-ssd1306`; drivers disponibles: `SSD1306Wire.h`, `SH1106Wire.h`, `SH1107` y **`AutoOLEDWire.h`** (autodetección SSD1306/SH1106/SH1107, I2C por Wire/Wire1, 700 kHz por defecto).
- Selección: `Screen.cpp:415-479` por macros de variante (`USE_SH1106/USE_SH1107/USE_SSD1306/USE_SPISSD1306/USE_EINK*`...). **El caso por defecto (sin macros) es `AutoOLEDWire`** (Screen.cpp:476-478) → es el camino que siguen las placas nRF52 de NavaTastic.
- Detección en `main.cpp:644-662` (`firstScreen()`): escanea **0x3C/0x3D** (SSD1306_ADDRESS_L/H) y `probeOLED` (ScanI2CTwoWire.cpp:52-87) distingue SH1106 de SSD1306 leyendo reg 0x00 del display; mapea a `config.display.oled`. El objeto `Screen` solo se crea si hay display en el bus (main.cpp:910-927).

## Pines
- Faketec/Promicro: sin display en la variante base (headless); I2C SDA P1.04 / SCL P0.11, `WIRE_INTERFACES_COUNT 1`. El OLED I2C externo funcionará en esos pines si responde en 0x3C/0x3D durante el scan.
- `HAS_SCREEN` por defecto = 1 en nRF52 (`architecture.h:30-31`), anulable con `MESHTASTIC_EXCLUDE_SCREEN` (configuration.h:559-563).

## Apagado y ahorro de energía
- `Screen::setOn(false)` → `handleSetOn(false)` → `dispdev->displayOff()` (Screen.cpp:593, comando 0xAE del controlador), tras liberar el thread de UI (Screen.cpp:996-1000).
- Timeout gestionado por PowerFSM según `config.display.screen_on_secs` (default **30 s**, NodeDB.cpp:959/1001; PowerFSM.cpp:362-406): `screen->setOn(false)` al dormir pantalla y `setOn(true)` al despertar.
- Deep sleep: `Screen::doDeepSleep()` (Screen.cpp:500-508). Power.cpp:1089 apaga pantalla en condición de batería baja.
- Brillo como contraste (Screen.cpp:650-659, 696-700; `screen_brightness`).

## Para nuestro firmware APRS
- Reutilizar patrón: autodetección SSD1306/SH1106 en 0x3C/0x3D + displayOff (0xAE) al entrar en eco/sleep + timeout 30 s → el OLED en un digipeater solar debe apagarse agresivamente (ver N-03).
