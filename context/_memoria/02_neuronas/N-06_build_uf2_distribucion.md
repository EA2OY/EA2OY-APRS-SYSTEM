# N-06 — Compilar nRF52 y distribuir UF2 (receta probada de NavaTastic)

Fuente: `C:\NavaTastic Codigo completo` → `variants\nrf52840\nrf52.ini`, `navarrico.ini`, `extra_scripts\nrf52_extra.py`, `boards\promicro-nrf52840.json`, `distribuir.ps1`, `build.ps1`, `verificar_paridad.ps1`, BITACORA L59-L66, INCIDENCIAS I1-I3.

## Stack (nuestro firmware podría copiar EXACTAMENTE este stack)
- PlatformIO platform: `nordicnrf52@10.11.0`; framework fork `Adafruit_nRF52_Arduino #cpp17-platform` (C++17, `-Os`).
- `board_build.partitions`, `-DLFS_NO_ASSERT`, `-include lfs_util.h`; flags de exclusión para adelgazar.
- Board: `promicro-nrf52840` (core nRF5, cortex-m4, SoftDevice **S140 6.1.1**, `maximum_size 815104`).
- Upload: nrfutil + use_1200bps_touch (teórico) — **en la práctica SOLO UF2 por drive** (ver flasheo).

## UF2 (receta)
- Post-build `extra_scripts\nrf52_extra.py` genera el .uf2 con: `python ./bin/uf2conv.py "<firmware.hex>" -c -f 0xADA52840 -o "<env>.uf2"`.
- Salida: `.pio\build\<env>\firmware.uf2` (+ .hex, .zip OTA, .mt.json).
- **Límite crítico del bootloader Adafruit real (unidad NICENANO, family 0xADA52840)**: el bootloader empieza en **0xEA000** → la app debe quedar **< 0xEA000 (~794 KB / ≤3103 bloques de 256 B)**. Builds mayores pisan el bootloader → bootloop con LED rápido (I2, L61). Tras adelgazar sensores: ~766 KB (94%) con ~37 KB de margen (INCIDENCIAS:409).
- Recuperación de ladrillo: flashear por drive un UF2 conocido bueno y pequeño.
- Flasheo usuario final: **doble pulsación RESET** → aparece drive `NICENANO` → copiar .uf2 → esperar ~50 s. NUNCA touch 1200 bps/nrfutil (error 22 en Windows; I1, L60).

## Paridad MD5 12/12 y canary (garantía de no-rotura)
- Reproducibilidad: fijar `NAVARICO_BUILD_EPOCH` (epoch 12/08/2026), `NAVARICO_APP_VERSION` (2.7.26.54e0d8d), y `NAVARICO_BUILD_TIME/DATE` idénticos a la referencia; inyección en `bin/platformio-custom.py`: BUILD_EPOCH, APP_ENV canónico y **`-ffile-prefix-map` de libdeps a los LIB BUILDERS** (env.GetLibBuilders(), :303-319; los build_flags de PIO destruyen backslashes — F4).
- Comparación: MD5 estricto del `.uf2`; `.zip` informativo. Resultado: `PARIDAD 12/12`.
- Canary `.o` para verificar "no cambié comportamiento nRF52": comparar `.o` de los TU modificados contra build baseline; p.ej. tras el fix I20: NodeDB.o difiere solo 64 B (símbolo muerto) y NavaCLIModule.o 4 B (weak codegen) → comportamiento nRF52 inalterado (INCIDENCIAS:435-438).
- Herramientas: `verificar_paridad.ps1`, `build.ps1 -Paridad`, `findstamp.ps1` (localiza .o con __TIME__/__DATE__).

## Lecciones de build (L) que NO repetir
- L5: Start-Process da ExitCode vacío en PS5.1 → verificar con logs/artefactos. L6: `.pio/build` acumula UF2 viejos → distribuir SIEMPRE el más reciente. L32: APP_VERSION es token crudo → usar optstr()/xstr(). L35: UTF-8 estricto (mojibake CP1252) al escribir .md con PowerShell → NO usar Get-Content/Set-Content para .md. L59: pio purga build de envs "huérfanos" al cambiar el ini → no alternar estados. L60-62: flasheo drive UF2 y límite bootloader. L64: adelgazar quitando librerías y con macros EXCLUDE. L66: pio 6.1.19 purga `.pio/build` entre invocaciones con distinto PLATFORMIO_CORE_DIR → una sola invocación, mismo core dir (junction).

## Incidencias de banco (I) clave
- **I1 (MATIZADA el 2026-09-14 por el operador)**: el *touch* de 1200 bps es el método estándar de las placas con bootloader de Adafruit y **debe funcionar**; lo que falló fue **en el PC de desarrollo** («Uno de los dispositivos conectados al sistema no funciona» al abrir a 1200). Es decir: **no es un fallo de la placa ni del firmware, es de ese Windows y su controlador**. Queda **disponible como opción** en la página del flasher (`flasher.html`, botón «Entrar en modo grabación (1200 baudios)»), y el doble toque al reset sigue siendo el camino que funciona siempre. **No volver a escribir que «los 1200 no sirven» sin decir que fue en este PC.**
- I2: límite bootloader <0xEA000. I3: formato UF2 de PIO válido (0xADA52840). I4: adelgazar quitando drivers no usados por macros. I12/I16: tras factory reset/reboot exprés, RX muerta hasta power-cycle → limpiar radio antes (en nuestro APRS: cuidado con reset de radio). I18: el nombre del canal es case-sensitive (no aplica a APRS).

## Para nuestro proyecto APRS
- Adoptar: stack nRF52 (platform nordicnrf52 + framework Adafruit fork), flujo UF2 (uf2conv family 0xADA52840, límite <0xEA000), distribución por drive NICENANO, y desde el principio una **verificación de paridad/canary** cuando toquemos código compartido.
- Distribución a usuarios: UF2 + método de configuración posterior (nuestro firmware APRS no tendrá app móvil: ver diseño).
