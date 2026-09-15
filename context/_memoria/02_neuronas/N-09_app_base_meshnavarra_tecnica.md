# N-09 — App base MeshNavarra Utility (MeshKachoUtility): técnica

Fuente SOLO LECTURA: `C:\Users\Jesus\Desktop\MeshKachoUtility` (repo EA2OY/MeshNavarra-Utility, GPL-3.0; cerebro propio en `Cerebro_MeshKachoUtility\`). Minería técnica 2026-09-09.

## Ficha stack
- Kotlin 2.1.0, UI **Views XML clásicas** (AppCompatActivity + TabLayout inferior), Material Components, **sin Compose**, sin DI, sin ViewModel, sin Navigation.
- minSdk 26 / target 35 / compile 35, JVM 17, sin minify. AGP 8.2.2, protobuf plugin (protoc 3.25.1, java-lite+kotlin-lite).
- App id `com.meshkachoutility` (INMUTABLE: no renombrar o huérfana la app instalada), nombre visible "MeshNavarra Utility", v1.2.3/code 16 (APK raíz v1.0.9 obsoleto).
- Transportes: BLE GATT (android.bluetooth nativo, **sin librería externa**) + USB-serial (librería mik3y vendored en source set: CP210x/CH34x/FTDI/CDC). Sin WiFi/red.
- Protos Meshtastic versionados en `app\src\main\proto\meshtastic\` (13 ficheros, fetch_protos.ps1), generados java-lite/kotlin-lite.
- Código: 8 Kotlin + 1 **MainActivity monolito 8.202 líneas** (deuda aceptada) + 3 tests JVM.

## Arquitectura (patrones VALIOSOS para nuestra app APRS)
- Transportes intercambiables bajo un **ConnectionListener común** (`UsbConnectionManager.kt:31-38`, `BleConnectionManager.kt`): BLE y USB comparten la misma lógica de arriba. Para nosotros: BLE obligatorio (nRF52), USB opcional.
- BLE: servicio GATT Meshtastic `6ba1b218-…`, TORADIO write `f75c76d2-…`, FROMRADIO read `2c55e69e-…`, FROMNUM notify `ed9da18c-…`; MTU 512 best-effort; **push/pull drenado**: notify FROMNUM o write OK → read FROMRADIO en bucle hasta vacío; **watchdog anti-stall 25 s**; scan solo bondedDevices + LE opcional; TRANSPORT_LE explícito.
- Escritura con ACK: **write-with-response + timeout 20 s** + cola de jobs; **read-modify-write** para set_config (¡NUNCA set parcial: borra secciones!).
- USB: framing `0x94 0xC3` + len BE16 (StreamApiFramer/Unframer) — no aplica a nosotros (BLE raw).
- Admin remoto: MeshPacket PortNum ADMIN_APP; dirigido → **siempre pkiEncrypted + wantAck** (si nuestro APRS usa BLE sin PKI, sustituir por canal seguro propio).
- Backup/Restore completo: 8 secciones Config + 14 ModuleConfig con progreso y JSON atómico.

## Pantallas (8 pestañas, 1 Activity)
Utilities (Buenas Prácticas ETSI/presets/Backup), Commands (telemetría/pos/traceroute/texto), Administration (reboot/wipe/fav/ign/remove/admin PKI/factory reset), **NavaTastic CLI (flagship: ~90 comandos /nava en 9 categorías con preview, puerta CONFIRMAR para destructivos, fragmentación 190 chars/12 s, reensamblado, historial)**, Chat, Nodes (radar+11 acciones), Log (petición/respuesta + app_log.txt), Debug oculta (7 taps; auditorías automatizadas).
- FLAGSHIP a replicar: consola tipo "NavaTastic CLI" que envía `/nava …` por canal Navadmin (lectura) o DM PKI (control). Para APRS: una "APRS CLI" del firmware por BLE (concepto: N-08).

## Build/distribución
- `build_apk.ps1` (JAVA_HOME al JDK local + gradlew assembleDebug), JDK empaquetado `jdk-17\`; firma release leyendo `local.properties` (NO extraer secretos); sin flavors; sin CI (`.github` solo FUNDING); F-Droid `fdroid\metadata.yml` (reglas: hash 40, subdir app, SIN Binaries/AllowedAPKSigningKeys hasta reproducible); `play\store_listing.txt` + fastlane metadata EN/ES; GitHub Releases (APK+AAB) con `/publicar-release` 10 pasos; push seguro desde clon temp (historial del repo local tiene secretos).
- Herramientas: `backup.ps1` (baks rodantes cerebro + snap-*.zip), `fetch_protos.ps1`, `unpack_project.ps1`.

## Estética/UX (identidad "MeshNavarra", decisiones con refs)
- Material 3 DayNight NoActionBar, edge-to-edge con insets; doble theme values/values-night.
- Paleta "tactical HUD": verde malla (light #1A7A44 / dark #32D77B), dorado (#8A6A1F/#D8B255), carmesí (#B0121C/#C81D25), fondo navy claro #F2F6F3 / oscuro #0B131C; tarjetas #131E2A.
- Títulos `sans-serif-condensed` bold; monospace en logs/consolas; iconos vector XML propios (escudo como launcher, fondo #0B192B).
- UX: ayuda long-press en TODOS los controles, explicadores por pestaña, demos guiadas, spinners 2 líneas, divisor arrastrable en la CLI, bilingüe EN/ES obligatorio (strings.xml + values-es, idioma por preferencia en attachBaseContext), CERO mojibake (UTF-8/`\uXXXX`).
- Manual → PDF con pandoc+xelatex+plantilla; PDFs embebidos en assets.

## Decisiones de compatibilidad (Android)
- Probada: Android 11 Samsung A10, Android 12 MIUI 13 Xiaomi Mi 10, Android 15/16 HyperOS Poco F6.
- Trampas conocidas: stacks MIUI/Samsung reportan peers BLE UNKNOWN/DUAL (filtro estricto oculta nodos); ACTION_BOND_STATE_CHANGED retrasado en MIUI (polling 500 ms); TRANSPORT_LE evita GATT 133; permisos BLUETOOTH_SCAN/CONNECT 12+, FINE_LOCATION 11- con neverForLocation; Android 16: PendingIntent implícito requiere FLAG_MUTABLE (crash); re-pedir permiso USB cada relanzamiento; adb no está en PATH.
- nRF52840 Adafruit VID 0x239A NO está en device_filter USB (no restringir por VID).
