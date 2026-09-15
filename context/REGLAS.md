# REGLAS Y VERDADES — Faketec_APRS_Igate_EA2OY (CAPA 1: LECTURA OBLIGATORIA)

> **Qué es este fichero**: lo que un agente **DEBE** saber antes de tocar nada.
> Es corto a propósito: se lee entero, siempre.
> **Cómo se usa la memoria**: Capa 1 (esto) + `ESTADO.md` (Capa 2) se leen **siempre**;
> la Capa 3 (`_memoria\`) **NO se lee entera**: se busca cuando el caso lo pide.
> **Última revisión: 2026-09-14.**

---

## 0. LA REGLA QUE MANDA SOBRE TODAS

**Si un documento dice X y el código (o el nodo) dice Y, GANA EL CÓDIGO, y el documento se corrige en ese mismo momento.**

Sin esta regla, los documentos viejos se convierten en ley. Ya ha pasado en este proyecto: el
manual estuvo días diciendo que el toque largo eran 800 ms, cuando el código decía 600.

---

## 1. Qué es este proyecto, en seis líneas

Firmware propio de **APRS sobre LoRa en 433 MHz** para placas **Faketec (nRF52840)**, con radio
**Heltec HT-RA62 (SX1262)** o **Ebyte E22P-433M30S (SX1262)**. Hace tres trabajos, a elección:
**repetidor** (digipeater), **rastreador** (tracker con GPS) o **los dos**. Añade extras que el
ecosistema no tiene: registro de viaje en flash, menú en pantalla OLED, configurador web por USB,
sueño con protección de batería, sensores de clima y corriente, y KISS/TNC por USB.

- **No es un iGate y no puede serlo**: el nRF52840 **no tiene WiFi**. Es **repetidor + rastreador**.
  El nombre del proyecto («iGate») viene del repositorio de origen y confunde.
- **Cómo llega al mapa**: nodo → radio → iGate de la casa (ESP32, firmware de CA2RXU) → APRS-IS → visores.
- **Licencia**: GPL-3.0. Referencias de código de terceros en `THIRD_PARTY.md`.

## 2. Las reglas de oro (no se negocian)

1. **Interoperabilidad**: el entramado y los parámetros de radio no se inventan. El ecosistema es
   433,775 MHz / SF12 / BW125 / CR4-5, prefijo LoRa `3C FF 01` y tramas APRS de texto.
2. **Nunca el indicativo ni el passcode de otra estación.** Los oyentes de APRS-IS van en modo
   lectura (`pass -1`).
3. **Nunca grabar por radio sin un rescate probado.**
4. **Dos fases**: primero el plan, se espera confirmación, y después se ejecuta. Lo evidente y sin
   riesgo se resuelve sin preguntar.
5. **Honestidad de método** (regla del operador, textual): *«prefiero que me digas no sé qué ha
   pasado, a que inventes»*. Obligatorio marcar cada afirmación como **[VERIFICADO]**,
   **[DEDUCIDO]**, **[OPERADOR]** (dato que solo sabe él) o **[NO CONSTA]**. Nunca una deducción
   presentada como hecho.

## 3. Lista negra: lo que NO se toca

| No se toca | Por qué |
|---|---|
| **La configuración guardada del nodo**, su indicativo, su modo de trabajo | Son del operador. El modo actual (1 o 2) se cambia solo si lo pide. |
| **El puerto serie de la iGate de casa** (el ESP32 de 192.168.3.236) | **Abrirlo reinicia la placa.** Se consulta por su web, nunca por serie. |
| `C:\NavaTastic Codigo completo` y `MeshKachoUtility` | Repositorios del operador, **solo lectura**. |
| `src\ble_kiss.cpp.off` y `src\ble_kiss.h.off` | Es el Bluetooth **escrito y apagado**. No se renombran ni se "arreglan" sin orden expresa. |
| `data\rescue\` | Firmware de rescate. **No se borra.** |
| `_archivo\backups\` | Copias para rescate. **No se leen como documentación** y no se editan. |
| Los binarios UF2 de CA2RXU | Distribuye GPL-3.0 sin fuente: copiarlos no da derechos. |
| **La política de teselas de OpenStreetMap** | Prohíbe el uso desde fichero local. **No se burla**; se sirve la página por `localhost`. |

## 4. Reglas de trabajo con este proyecto

1. **Editar UTF-8 con la herramienta de edición, nunca con idas y vueltas por PowerShell.** Los
   reemplazos de una sola línea ya rompieron el código **tres veces** (una dejó el nodo sordo al
   USB). Después de editar, **comprobar que la línea buscada sigue existiendo**.
2. **No hacer commits ni publicar** sin que lo ordene el operador.
3. **No arrancar servidores sustitutos**: el servidor local del configurador lo arranca él.
4. **No grabar en el nodo sin que lo pida.**
5. **Los avisos del compilador se leen SIEMPRE**: dos de ellos eran fallos de verdad.
6. **Hablar en español llano.** Nada de jerga de programador cuando se hable del nodo.
7. **Cuando se arregla un fallo, buscar el mismo fallo en todo el proyecto.** El del registro de
   viaje estaba copiado en tres sitios.
8. **Cuando una prueba dice que todo está mal, sospechar primero de la prueba.**
9. **No afirmar de memoria lo que se puede calcular o mirar.**
10. **Un documento histórico se marca como histórico** en su primera línea. Y toda afirmación lleva
    **fecha** y **de dónde sale**.
11. **Referencias por NOMBRE, nunca por número de línea** (se escribe el nombre de la función,
    `gpsManage()`, y no «el fichero tal, línea cual»): los números caducan con cada edición. Si un
    nombre ya no existe, **buscar el comportamiento**, no el nombre que citaba el documento.

## 5. Cifras verificadas (las que la gente cita mal)

| Dato | Valor | Fecha / cómo se sabe |
|---|---|---|
| Radio | 433,775 MHz · SF12 · BW125 · CR4-5 · preámbulo 8 · CRC on | código, 2026-09-14 |
| Prefijo LoRa | `3C FF 01` | código, 2026-09-14 |
| **Toque largo del botón** | **600 ms** (se dispara mientras se mantiene) | `button.cpp`, 2026-09-14 |
| **Doble toque** | ventana de **800 ms**, rebote 25 ms | `button.cpp`, 2026-09-14 |
| Cierre del menú OLED | 20 s sin tocar nada (cuenta atrás de 3 s) | `display.cpp`, 2026-09-14 |
| Maquetación de pantallas | `kRowH = 12`, `kTextOff = 1`, `kRowsTop = 14` | `display.cpp`, 2026-09-14 |
| Corte de batería | 3400 mV (SX1262) / 3500 mV (E22P) · despertar 3710 mV | `config.h`, 2026-09-14 |
| Zona del registro de viaje | 0xC8000-0xE7FFF (32 páginas) | código, 2026-09-14 |
| Configuración guardada | 0xE8000 / 0xE9000 · última posición 0xE7000 | código, 2026-09-14 |
| Límite del bootloader | 0xEA000 (el build aborta si la app llega ahí) | `extra_scripts\nrf52_uf2.py` |
| Consumo dormido | 0,4 mA (Faketec SX1262) · 1,5 mA (E22P con su elevador) | [OPERADOR], 2026-09-12 |
| Número de compilación | `b3` grabado en el nodo | binario compilado, 2026-09-14 |

## 6. Cómo se hace cada cosa (órdenes, no párrafos)

Desde `Faketec_APRS_Igate_EA2OY\`:

```
COMPILAR     $env:PLATFORMIO_CORE_DIR="C:\Users\Jesus\Desktop\LoRa_APRS_iGate-main\_pio_core"
             & "C:\Users\Jesus\.platformio\penv\Scripts\platformio.exe" run -e faketec_sx1262_433 -e faketec_e22p_433
             (sin permisos ampliados)

SUBIR NUMERO .buildnum +1  y  platformio.ini  -DAPP_BUILD_NUM=\"bN\"   (a mano: no es automatico)

GRABAR       doble toque al reset -> volumen UF2 en E: -> copiar .pio\build\faketec_sx1262_433\firmware.uf2
             (el bootloader se sale a los pocos minutos; el DFU por serie NO funciona con la app corriendo)

COMPROBAR    node tools\web_check.js ; node tools\prueba_avisos.js
             node tools\prueba_registro.js ; node tools\prueba_mapa.js     <- los CUATRO en verde

VERIFICAR POR RADIO   http://192.168.3.236/received-packets.json  (la iGate de casa)
             horas en UTC = las mismas que escribe el firmware; el reloj local va +2 h

DIAGNOSTICO SIN USB   consulta ?USB? por radio -> "USB <build> bytes N lineas N | sordos Ns | bucle peor Nms"

MEMORIA      powershell -File tools\verifica_memoria.ps1     <- comprueba estas capas (0 = todo bien)
```

## 7. Mapa: dónde vive cada cosa

**Memoria del proyecto** (`Cerebro_Faketec_APRS_Igate_EA2OY\`)

| Fichero | Qué es | ¿Obligatorio? |
|---|---|---|
| `GUIA_AGENTE.md` | Punto de entrada para un agente nuevo | Sí (2 minutos) |
| `REGLAS.md` | **Capa 1**: esto | **Sí, siempre** |
| `ESTADO.md` | **Capa 2**: el único estado del proyecto | **Sí, siempre** |
| `cerebro.md` | Señal de tráfico: dice dónde fue a parar el fichero viejo | No |
| `_memoria\historia.md` | Capa 3: histórico de sesiones (el antiguo `cerebro.md`, íntegro) | No: se busca |
| `_memoria\PENDIENTE.md` | Tareas apuntadas por el operador | Al empezar una tanda |
| `_memoria\02_neuronas\` | Capa 3: conocimiento de hardware y patrones | No: se busca |
| `_memoria\01_subnotas\` | Capa 3: contexto inicial del proyecto (**histórico**) | No: se busca |
| `_archivo\backups\` | Copias de rescate | **No se leen** |

**Código y documentos** (`Faketec_APRS_Igate_EA2OY\`)

| Dónde | Qué hay |
|---|---|
| `src\` | El firmware (40 ficheros). `main.cpp` es el orquestador del bucle |
| `src\ble_kiss.*.off` | Bluetooth **escrito y apagado** (fuera de la compilación) |
| `variants\faketec\` · `boards\` · `platformio.ini` | Pinout, definición de placa y entornos de compilación |
| `web\index.html` | **El configurador web entero, en un solo fichero, sin dependencias externas** |
| `tools\` | Herramientas: comprobadores, cliente KISS, volcado del registro, aire, oyente APRS-IS |
| `docs\` | Documentación viva: manual de uso, funciones, protocolo, uso del aire, app futura |
| `data\rescue\` | Firmware de rescate (no borrar) |
| `_archivo\` | Copias y material retirado |

**Fuera del proyecto pero del proyecto**: `..\_BLE_DIAG\` (banco de pruebas del Bluetooth, **en la
raíz del repositorio, no aquí**), `..\_referencias\` (clones de referencia, solo lectura),
`..\_pio_core\` (caché de PlatformIO para compilar sin permisos ampliados).

## 8. La prueba final (si esto falla, la memoria está mal)

Un agente nuevo, leyendo **solo** `REGLAS.md` y `ESTADO.md`, debe poder:
decir qué es el proyecto y qué versión corre · saber qué **no** debe tocar ·
encontrar dónde vive cada cosa · saber qué está abierto y qué está bloqueado ·
y saber que el resto se busca, y cómo.
