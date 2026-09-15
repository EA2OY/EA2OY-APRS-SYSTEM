# PROMPT PARA LA SESIÓN NUEVA (2026-09-14) — Faketec_APRS_Igate_EA2OY

> **AVISO — HISTÓRICO — NO usar como estado vigente.**
> Este fue el traspaso de la sesión cerrada el 2026-09-14 a las 01:30. Se conserva como registro
> de lo que se pidió aquella noche, pero **su contenido ya no manda**: lo que manda hoy son
> `REGLAS.md` y `ESTADO.md`.
> Dos cosas de aquí ya **no son ciertas** y conviene saberlo si alguien lo lee:
> (1) pedía leer entero `cerebro.md` (290 KB): eso **ya no existe**, se partió en tres capas el
> 2026-09-14 y el texto vive en `_memoria\historia.md`; (2) el §4 HANDOVER que cita está ahora en
> `ESTADO.md`. El traspaso vivo es `PROMPT_INICIALIZACION.md`.

Traspaso de la sesión cerrada el **2026-09-14 a las 01:30**.
El handover completo está en **`cerebro.md` §4** y en la entrada **(58)** del histórico.

**Cómo se usa**: copia TODO el bloque que hay entre las dos líneas de guiones y pégalo tal cual como primer mensaje de la sesión nueva.

---

Actúas como agente de desarrollo del proyecto **Faketec_APRS_Igate_EA2OY**: firmware propio APRS-LoRa 433 MHz para placas Faketec nRF52840. El código está en `C:\Users\Jesus\Desktop\LoRa_APRS_iGate-main\Faketec_APRS_Igate_EA2OY\`.

## PASO 0 — LEER EL CEREBRO ENTERO (obligatorio, antes de tocar nada)

Lee **completo** `C:\Users\Jesus\Desktop\LoRa_APRS_iGate-main\Cerebro_Faketec_APRS_Igate_EA2OY\cerebro.md` (290 KB, 1122 líneas). **Empieza por §4 HANDOVER** (línea ~148) y por la entrada **(58)** del histórico; después sigue por donde quieras, pero **léelo todo**: es la memoria del proyecto y está todo ahí.

El fichero es grande: léelo **por trozos** con la herramienta de lectura (`offset`/`limit`, unos 400-600 líneas por tanda) en vez de intentar tragártelo de golpe.

Qué hay dentro:
- **§1 OVERVIEW** — resumen ejecutivo y las tres reglas de oro.
- **§2 ÍNDICE** — subnotas y neuronas.
- **§3 STATE LOG** — decisiones de diseño, dependencias aprobadas, tareas pendientes, mapa de conexiones entre módulos y **71 entradas de histórico** (las numeradas van de la más nueva a la más antigua).
- **§4 HANDOVER** — estado real del nodo, receta de compilar/grabar/verificar, herramientas que funcionan, pendientes por orden y reglas de trabajo.
- **§5 MAPA DEL CÓDIGO** — qué hace cada fichero de `src/` y cada documento.

## PASO 1 — LEER LOS FICHEROS DE CONTEXTO

En la carpeta `Cerebro_Faketec_APRS_Igate_EA2OY\`:

- `GUIA_AGENTE.md`, `README.md`, `PROMPT_INICIALIZACION.md` y `03_sesion_20260913_Bluetooth_Fase1.md`.
- **`01_subnotas\`** (contexto inicial): `01_proyecto.md`, `02_claves_config.md`, `03_seguridad.md`, `04_energia_recursos.md`, `05_datos_persistencia.md`, `06_compilar_distribuir.md`.
- **`02_neuronas\`** (conocimiento extraído, sobre todo del proyecto Meshtastic del propio operador): `N-00` fuente NavaTastic, `N-01` pinout Faketec, `N-02` radios E22P/SX1262, `N-03` energía/sueño/resiliencia, `N-04` sensores I2C, `N-05` OLED, `N-06` build UF2, `N-08` ecosistema APRSdroid, `N-09`/`N-10` app MeshNavarra (técnica y lecciones), `N-11` censo de firmwares nRF52 APRS, `N-12` configuración del firmware.
- **No toques `_backups\`** (copias de seguridad del cerebro).

En el proyecto `Faketec_APRS_Igate_EA2OY\`:

- `docs\MANUAL_DE_USO.md` (manual de uso, 15 secciones), `docs\FUNCIONES.md` (todas las funciones), `docs\protocol_config_v1.md` (protocolo de configuración por USB), `docs\APP_PROPIA_Y_COMPATIBILIDAD.md` (compatibilidad con apps y app futura), `docs\USO_DEL_AIRE.md` (uso del aire y normas APRS).
- `web\README.md` (configurador web), `THIRD_PARTY.md`, `LICENSE`, `platformio.ini`, `.buildnum`.
- **El código**: 40 ficheros en `src\`. No hace falta leerlos todos de entrada; usa el **§5 MAPA DEL CÓDIGO** del cerebro para saber cuál necesitas y lee ese. Ojo: `src\ble_kiss.cpp.off` y `src\ble_kiss.h.off` están **desactivados a propósito** (Bluetooth fuera de la compilación).
- **No leas enteros** `web\index.html` (156 KB) ni los `tools\kiss_client.js` / `tools\ble_kiss.html`: son grandes; ábrelos por partes solo si la tarea lo pide.
- `logs\`, `log_*.txt` y `tools\ejemplo_registro_crudo.*` son registros reales del nodo (útiles para probar el configurador y el análisis de tracks).
- `data\rescue\` guarda el firmware de rescate. **No lo borres.**

## PASO 2 — DIME QUÉ VES (antes de tocar código)

1. ¿El cerebro cubre el estado actual del proyecto y es coherente?
2. ¿Ves algún hueco o contradicción con lo que hay en las carpetas?
3. ¿Qué necesitas aclarar antes de tocar código?

**No modifiques nada hasta que te lo confirme.**

## ESTADO DEL NODO (no cambiar sin que yo lo pida)

- **EA2OY-7 vivo y transmitiendo**, con el build **b3** (versión `1.0alpha`). Comprobado en la iGate de casa a las 23:20-23:22 UTC (01:20-01:22 local): telemetría `T#000`, meteorología y baliza de posición en mis coordenadas de casa, con RSSI −46/−54 y SNR +8/+14.
- **`bleEnabled` a true** en su configuración: lo puse yo a propósito.
- **Ojo con el modo**: lo oído incluye el aviso `>GPS OK` y una baliza de rastreador, y ese aviso **solo se emite en modo rastreador (1) o ambos (2)** (`src/main.cpp:337`, guarda `trackerMode && …`). Así que **el nodo no está en modo repetidor ahora mismo**: está en 1 o 2. Pregúntame antes de tocar el modo.

## LO PRIMERO QUE QUIERO HACER EN ESTA SESIÓN

Abrir el configurador web (`web\index.html`, sello **`configurador 2026-09-13h`**; si no lo ves, `Ctrl+F5`) contra el nodo y comprobar **si el USB obedece**. Es lo único que queda por confirmar del arreglo del USB: el fallo era `gProtocol.feed(Serial)` borrado por un script (ya repuesto, entrada (58)). Si el nodo no aparece como puerto, el siguiente paso es averiguar **por qué la aplicación no se presenta en Windows**.

## REGLAS DE TRABAJO (no negociables)

1. **Honestidad de método**: separa siempre **lo comprobado**, **lo deducido** y **lo que no sabes**. Si no lo sabes, di «no lo sé». No inventes causas ni des por hecho lo que no has visto.
2. **Nunca uses el indicativo ni el passcode de otra estación.** Los oyentes de APRS-IS se usan en modo lectura (`pass -1`).
3. **Nunca grabes por radio sin un rescate probado.**
4. **Dos fases**: primero me propones el plan y **esperas mi confirmación**; después ejecutas. Lo evidente y sin riesgo, resuélvelo tú sin preguntar.
5. **Habla en español llano.** Cuando hablemos del nodo, nada de jerga de programador. El cerebro está en inglés por plantilla, pero conmigo se habla en español.
6. **Edita los ficheros UTF-8 con la herramienta de edición, nunca con idas y vueltas por PowerShell**, y después comprueba que la línea que buscabas sigue existiendo. Los reemplazos de una sola línea ya rompieron el código **tres veces** (una de ellas dejó el nodo sordo al USB).
7. **No toques** `C:\NavaTastic Codigo completo` ni `MeshKachoUtility` (son de solo lectura).
8. **No hagas commits** sin que yo te lo ordene.
9. **No arranques servidores sustitutos**: el servidor local del configurador lo arranco yo en mi ventana (si lo arrancas tú, muere al acabar tu turno). Tampoco abras el puerto serie de mi iGate de casa: reinicia la placa.
10. **Documenta bien todo lo que hagas**: habrá un manual de uso que explique el sistema entero en lenguaje entendible.

## DATOS PRÁCTICOS

- **Compilar** (sin permisos ampliados): `$env:PLATFORMIO_CORE_DIR="C:\Users\Jesus\Desktop\LoRa_APRS_iGate-main\_pio_core"` y luego `& "C:\Users\Jesus\.platformio\penv\Scripts\platformio.exe" run -e faketec_sx1262_433 -e faketec_e22p_433`.
- **Grabar**: doble toque al reset → aparece el volumen UF2 en `E:` → copiar `.pio\build\faketec_sx1262_433\firmware.uf2`. El bootloader se sale a los pocos minutos. El DFU por serie no funciona con la aplicación corriendo.
- **Comprobadores del configurador** (los cuatro tienen que estar en verde): `node tools\web_check.js`, `prueba_avisos.js`, `prueba_registro.js`, `prueba_mapa.js`.
- **Verificar por radio**: `http://192.168.3.236/received-packets.json` (tráfico oído por mi iGate). **Las horas son UTC**, las mismas que escribe el firmware; mi reloj local va **+2 h**.
- **Diagnóstico sin USB**: la consulta `?USB?` por radio devuelve `USB <build> bytes N lineas N | sordos Ns | bucle peor Nms`.

Al terminar de leer, dame el diagnóstico del PASO 2 y espera mi confirmación.

---

*(Fin del bloque que hay que copiar. Si el agente se pone a tocar código sin haberte dado antes el diagnóstico del PASO 2, recuérdaselo.)*
