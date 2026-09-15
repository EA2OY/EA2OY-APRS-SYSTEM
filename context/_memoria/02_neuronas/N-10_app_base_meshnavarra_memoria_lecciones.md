# N-10 — App base MeshNavarra Utility: memoria, lecciones y proceso (errores a no cometer)

Fuente SOLO LECTURA: `C:\Users\Jesus\Desktop\MeshKachoUtility\Cerebro_MeshKachoUtility\` (cerebro.md 154 KB, ~60 baks, subnotas 01-09) + guías del repo. Minería 2026-09-09. Estado del cerebro hermano: último cambio 2026-08-30 (v1.2.3 GitHub + MR F-Droid !45843 pendiente de pipeline).

## Reglas de oro del proyecto app (a imitar)
1. Flujo 2 fases: PHASE 1 plan sin editar → confirmación → PHASE 2. Token diet.
2. Código mínimo, CERO dependencias nuevas; verificar que la feature no exista ya.
3. Bilingüe EN/ES obligatorio en TODA string; UTF-8 puro (mojibake = fallo).
4. Versionado: cada feature/fix → bump versionCode/versionName + BUILD_DATE + manual + changelogs; publicar es un paso aparte (`/publicar-release`).
5. Backup antes y después de cada tarea (backup.ps1: 60 baks rodantes + snaps).
6. Verificar tamaño de cerebro.md tras cada escritura (se vació 2 veces por fallos PowerShell).
7. Comando destructivo → SIEMPRE puerta CONFIRMAR explícita (fail-closed, no fail-open).
8. No tocar: package id, compuertas de seguridad, repos hermanos (solo lectura), secretos/PINs.

## Lecciones/errores a NO repetir (lista con refs)
1. **NUNCA editar archivos UTF-8 con PowerShell** (Get-Content+WriteAllLines doble-codifica cp1252→UTF-8; corrompió MainActivity.kt entero y cerebro.md 2 veces). Escribir con herramientas UTF-8 nativas o `\uXXXX`.
2. Doble-encoding de emojis/acentos/ñ en literales Kotlin → mojibake en pantalla (usar strings.xml).
3. UI timers: NO depender de ValueAnimator (escala de animación 0 en sistema → colapsan); usar Handler+SystemClock.
4. NO validar IDs de nodo con `<=0`: uint32 ≥ 0x80000000 son Int negativos VÁLIDOS (solo -1 broadcast y 0 son inválidos). → Para APRS: cuidado al parsear IDs numéricos.
5. BLE: `completeConnect()` duplicado en onServicesDiscovered+onMtuChanged → duplicaba want_config; hacerlo idempotente.
6. Rutas de texto libre: fail-closed si el comando no matchea catálogo (no enviar sin filtro de peligro).
7. Views tocadas desde hilo BLE sin runOnUiThread → CalledFromWrongThreadException.
8. set_config parcial borra secciones del nodo → SIEMPRE read-modify-write.
9. XML: apóstrofes `\'`, strings sin cerrar rompen merge con error engañoso.
10. F-Droid: hash de commit 40 chars (nunca tag); sin `output:` incorrecto; `Binaries`/`AllowedAPKSigningKeys` matan job sin builds reproducibles; nada de rutas locales en gradle.properties (CI Linux).
11. PowerShell `>` corrompe binarios (screencap); adb `--es` con espacios trunca; uiautomator falla en MIUI.
12. Keystore PKCS12: keyPassword = storePassword; contraseñas solo alfanuméricas; Properties.load se come `\`.
13. BLE fabricantes: no fiarse de ACTION_BOND_STATE_CHANGED (Samsung/MIUI retrasan) → polling 500 ms; aceptar DEVICE_TYPE_UNKNOWN/DUAL.
14. PIN correcto nodos Navarrico = **654321** (nunca reintroducir 123457); no subir claves a chats/repos.
15. Firmware: ACK de routing ≠ procesado (verificar con PONG).

## Compatibilidad dispositivos (resumen operativo)
- minSdk 26+, target/compile 35. Probada: A10 (A11), Mi 10 (A12/MIUI13), Poco F6 (A15/16 HyperOS).
- MIUI/HyperOS: toggle "USB debugging (Security)" para input tap; IME no acepta input text; tab-bar ignora taps sintéticos.
- Permisos: BLUETOOTH_SCAN+CONNECT (12+), FINE_LOCATION (≤11, neverForLocation), runtime API 33+ con RECEIVER_NOT_EXPORTED y getParcelableExtra con clase.
- Android 16: crash por PendingIntent implícito → FLAG_MUTABLE. USB: permiso se re-pide cada relanzamiento.
- BLE nRF52 Adafruit VID 0x239A no está en device_filter USB (no restringir por VID).

## Proceso de publicación (10 pasos, versión completa)
GitHub: tag vX.Y.Z + Release con assets APK firmado + AAB + verificación SHA-256 (protocolo `/publicar-release`). F-Droid: MR con metadata (reglas L10). Play: pendiente (cuenta $25 + 12 testers 14 días). Push seguro SIEMPRE desde clon temp (nunca repo local con historial de secretos). Fingerprint firma público: `237c9051…4ab94` (solo para F-Droid).

## Sobre la consola CLI / auditorías (patrón a copiar en APRS)
- Catálogo de comandos con metadatos (categoría, peligro, fragmentación, pacing del firmware 12 s/190 chars Navadmin; rate-limit Navadmin 30-50 s).
- Puerta CONFIRMAR en destructivos; preview de respuesta; "?" interroga; validación por prefijo OK/ERR/TIMEOUT (auditorías).
- **Lección de diseño**: los tiempos/pacing del firmware condicionan TODA la UX → definir rate-limits del firmware APRS ANTES de diseñar la app.
- RemoteControlReceiver por adb broadcast (solo test).
- El agente del firmware NO toca código de la app y viceversa (solo lectura cruzada).

## Para nuestra app APRS (síntesis de qué copiar/evitar)
COPIAR: patrón ConnectionListener (BLE+USB), drenado BLE + watchdog 25 s, write-with-ack + read-modify-write, backup JSON atómico, pestañas + consola CLI con catálogo, puerta CONFIRMAR, help long-press, paleta/UX, bilingüe, versionado+manual sincronizados, protocolo de publicación seguro, F-Droid rules.
EVITAR: monolito MainActivity 8k líneas (nosotros: arquitectura por módulos aunque sea Views), ValueAnimator, PowerShell para editar, set parcial de config, fail-open, secrets en repo.
ADAPTAR: UUIDs GATT propios del firmware APRS (no Meshtastic), protocolo propio sin protobuf Meshtastic (o protobuf propio), sin PKI Meshtastic (usar pairing BLE + PIN en OLED), admin local-first.
