# ESTADO ACTUAL — Faketec_APRS_Igate_EA2OY

> **Fecha de este estado: 2026-09-14.** Este es el **ÚNICO documento de estado** del proyecto.
> Lo que está abierto, lo que está bloqueado, y lo que **NO** se toca.
> Se lee **siempre**, junto con `REGLAS.md`. Todo lo demás se busca cuando hace falta.
>
> Marcas que se usan en todo el documento:
> **[VERIFICADO]** = lo he comprobado yo (código, fichero o medida) · **[OPERADOR]** = dato que
> solo sabe el responsable del proyecto (no verificable desde aquí) · **[DEDUCIDO]** = encaja
> con los datos pero no está demostrado · **[NO CONSTA]** = no lo sé.

---

## 1. El nodo (lo que hay grabado y funcionando)

| Dato | Valor | Cómo se sabe |
|---|---|---|
| Indicativo | **EA2OY-7** | [OPERADOR] |
| Firmware grabado | **`b3`**, versión `1.0alpha` | [OPERADOR] (oído por radio) + [VERIFICADO] el binario compilado: `platformio.ini` declara `b3`, `.buildnum` = `3` y la cadena `b3` está dentro de `firmware.uf2` |
| Fichero compilado | `.pio\build\faketec_sx1262_433\firmware.uf2` (710.656 B) | [VERIFICADO] fecha 2026-09-14 01:05 |
| Estado | **Vivo y transmitiendo** | [OPERADOR] oído en la iGate de casa a las 23:20-23:22 UTC del 13/09: telemetría `T#000`, meteorología y baliza de posición |
| Modo de trabajo | **1 (rastreador) o 2 (ambos). NO es 0** | [DEDUCIDO, con prueba fuerte]: se oyó el aviso `>GPS OK`, y ese aviso solo se emite en modos 1/2 (`main.cpp`, guarda `trackerMode`) |
| `bleEnabled` | **true** en su configuración | [OPERADOR] lo puso a propósito. **Inerte**: ver §4 |
| Puerto serie del nodo | **NO CONSTA** cuál es | [VERIFICADO] hay dos puertos presentes en el PC (COM36 y COM37) y **no sé cuál es cuál**. Abrirlos reinicia la placa, así que no se prueban a ciegas |

**Regla:** el modo, el indicativo y la configuración guardada **son del operador**. No se cambian sin que lo pida.

## 2. Lo primero que hay que hacer (lo que está abierto)

**Confirmar que el USB obedece.** Es lo único que queda por comprobar del arreglo del USB.

- El fallo era: `gProtocol.feed(Serial);` **borrado** del bucle por un reemplazo de texto. Ya está repuesto y auditado el resto del bucle. [VERIFICADO en el código]
- El nodo **contestó una vez** por COM36 y luego se quedó mudo. **No se sabe** si COM36 era el bootloader y el puerto de la aplicación nunca llegó a presentarse. [NO CONSTA]
- **La prueba es del operador**: abrir `web\index.html` con Chrome/Edge, pulsar Conectar y ver **qué puerto ofrece y si obedece**.
- **Aviso importante**: el sello de versión del configurador **no se ve al abrir la página** (solo aparece al pulsar el botón de idioma ES/EN). Si no lo ves, **no** significa que tengas la página vieja. [VERIFICADO en el código]

## 3. Qué está hecho y qué no

### Hecho y verificado en el aire
- Balizas de posición (fijas y de rastreador), con meteorología legible y aviso de batería.
- Paquete meteorológico propio y telemetría APRS automática (canales T#).
- Repetidor (digipeater) y **KISS por USB verificado de punta a punta** (radio→USB y USB→radio).
- Mensajes APRS de salida con acuses y reintentos; consultas; boletines; objetos.
- Registro de viaje en flash, con tracks por sesión y exportación GPX/KML/CSV.
- Configurador web con mapa propio y avisos de radioaficionado.

### Placas soportadas (2026-09-14)

| Placa | Entorno | Estado |
|---|---|---|
| Faketec / ProMicro nRF52840 + **HT-RA62** | `faketec_sx1262_433` | **Probado en el aire** (el nodo EA2OY-7) |
| Faketec / ProMicro nRF52840 + **E22P-433M30S** | `faketec_e22p_433` | Compila; **sin probar en hardware** (falta el módulo) |
| **LilyGO T-Echo** | `techo` | **Compila** (35,5 % de flash). **Sin probar en hardware** |
| **LilyGO T-Echo Plus** | `techo_plus` | **Compila**. **Sin probar en hardware** |

Los dos T-Echo **compilan pero nadie los ha grabado todavía**: la sesión para probarlos está
preparada en `docs\PROCEDIMIENTO_TECHO.md` (con la copia de seguridad del firmware original como
primer paso obligatorio).
**Lo que NO llevan los T-Echo**: pantalla (tienen tinta electrónica, que necesita su propia capa de
dibujo; dibujan la pantalla (el driver real entra en los cuatro entornos techo)), ni el IMU/motor/zumbador del Plus. El resto
—radio, GPS, BME280, batería, config, registro, digi y rastreador— sí.
Detalle de hardware, con fuentes: `docs\HARDWARE_TECHO.md`.

### Hecho pero **pendiente de probar en la pantalla** (nodo grabado, sin verificar a ojo)
- Menú principal con el espaciado nuevo y la pantalla del modo de trabajo.
- El doble toque del botón con el margen nuevo.
- La marca `S` de arranque de sesión, con un paseo real.

### Escrito pero **APAGADO** (no está en el binario)
Ver §4. Es la trampa número uno de este proyecto.

### Bloqueado / pendiente de banco
- Sueño de protección con fuente de laboratorio (hay que medir el umbral real de despertar).
- Control remoto por radio y consultas contra un segundo equipo (hacen falta dos nodos).
- Sensor INA3221 y perfiles de química para LiFePO4 (los otros dos no hacen falta).

### Publicado en GitHub (2026-09-14)

| Repositorio | Qué lleva |
|---|---|
| **[EA2OY/EA2OY-APRS-SYSTEM](https://github.com/EA2OY/EA2OY-APRS-SYSTEM)** | El firmware (código, cuatro entornos, firmware compilado), la documentación, el manual (texto y PDF), los assets y `context/` (esta memoria) |
| **[EA2OY/CONFIGURADOR-WEB-APRS-EA2OY](https://github.com/EA2OY/CONFIGURADOR-WEB-APRS-EA2OY)** | El configurador servido como **página web** (GitHub Pages), con el manual al lado |

**Por qué el configurador va en su propio repositorio**: WebSerial (lo que permite hablar con el nodo
por USB) **solo funciona en un sitio seguro** (`https://` o `localhost`), y el **mapa** necesita que
las peticiones lleven cabecera `Referer` (política de OpenStreetMap, que prohíbe el uso desde fichero
local). Servido por web, las dos cosas funcionan.
**Pendiente del operador**: activar **GitHub Pages** en ese repositorio
(Settings → Pages → Deploy from branch → `main` → `/ (root)`), o la web no existirá.

**Regla de privacidad que se aplicó al publicar** (y que hay que repetir si se vuelve a subir):
no se publica **ningún** fichero con datos del operador. En concreto: fuera los ficheros de
configuración (`igate_conf.json`), los registros, los respaldos del firmware (un `.uf2` de rescate
lleva la configuración dentro) y las copias del cerebro; y las coordenadas de los ficheros de
ejemplo, **desplazadas de forma uniforme** con `tools\anonimiza_coords.ps1` (reversible: el
desplazamiento queda apuntado con el script). Comprobado con una auditoría que busca coordenadas en
los **dos formatos** (decimal y APRS `ddmm.mmN`), claves y passcodes.


## 4. Lo que está escrito pero APAGADO (leer esto antes de creerse cualquier documento)

| Cosa | Estado real | Consecuencia |
|---|---|---|
| **Bluetooth (BLE)** | El código existe en `src\ble_kiss.cpp.off` y `src\ble_kiss.h.off`. **Renombrados a `.off` a propósito: el compilador no los ve.** | No hay Bluetooth. No se puede emparejar nada. |
| `bleEnabled` / `blePin` | Se guardan, se editan en la web **y salen en el menú de la pantalla**… y **el firmware no los lee**. | **Valor inerte.** La web avisa de que lo es; **la pantalla no avisa**. No esperes que haga nada. |
| Motivo de que esté apagado | Al enlazar el Bluetooth, el nodo se quedaba **sin pantalla y sin USB**. Sigue **sin resolver**. El banco de pruebas está en `..\_BLE_DIAG\` (en la **raíz del repositorio**, no dentro del proyecto). | — |

**Regla de lenguaje:** de esto **no se dice «hecho»**. Se dice «escrito y apagado».

## 5. Cómo se comprueba el estado (órdenes, no párrafos)

Desde `Faketec_APRS_Igate_EA2OY\`:

```
Compilar:      $env:PLATFORMIO_CORE_DIR="C:\Users\Jesus\Desktop\LoRa_APRS_iGate-main\_pio_core"
               & "C:\Users\Jesus\.platformio\penv\Scripts\platformio.exe" run -e faketec_sx1262_433 -e faketec_e22p_433 -e techo -e techo_plus

Comprobadores: node tools\web_check.js ; node tools\prueba_avisos.js
               node tools\prueba_registro.js ; node tools\prueba_mapa.js     (los cuatro en verde)

Por radio:     http://192.168.3.236/received-packets.json   <- trafico oido por la iGate de casa
               (horas en UTC, las mismas que escribe el firmware; el reloj local va +2 h)

Sin USB:       consulta ?USB? por radio -> "USB <build> bytes N lineas N | sordos Ns | bucle peor Nms"

Grabar:        doble toque al reset -> volumen UF2 en E: -> copiar .pio\build\faketec_sx1262_433\firmware.uf2
```

**Prohibido**: abrir el puerto serie de la iGate de casa (la reinicia).

## 6. Cabos sueltos conocidos (para que no sorprendan)

- **`factory_reset` no hace lo mismo por consola que por la web.** Por consola conserva indicativo, operadores autorizados y control remoto; por JSON (el botón de la web) **borra todo**. [VERIFICADO en el código] **Pendiente de decisión del operador**: cuál de los dos comportamientos es el bueno.
- **El sello del configurador** solo aparece tras pulsar el botón de idioma, y se duplica en cada pulsación. [VERIFICADO]
- **Fallo sin cazar en el configurador**: si cambias de idioma estando en la pestaña «Registro de viaje», te devuelve a «Estado en vivo». [VERIFICADO]
- **El firmware de rescate no coincide con el que cita el histórico**: el fichero es de 658.432 B y su SHA256 empieza por `F8284DE8…`, no por `F2835770…`. **Pendiente de decidir cuál es el bueno.** [VERIFICADO]
- **`?USB?` no está documentado** en `docs\` (solo vive en el código y en el traspaso). [VERIFICADO]
- **Las subnotas `_memoria\01_subnotas\`** y algunos párrafos de los documentos del proyecto describen todavía la etapa inicial (cuando el firmware no existía y no había ni plataforma elegida). Ya no es cierto: están marcadas como **históricas** y no deben leerse como estado.

## 7. Cola de banco (necesita al operador o instrumentos)

1. Confirmar que el USB obedece (operador, configurador). ← **lo primero**
2. Probar en la pantalla: menú, pantalla del modo y doble toque.
3. Paseo real con la marca `S` y ver el corte por sesión en la lista de tracks.
4. Borrado por sesión completa en el registro (Fase 3).
5. Sueño con fuente de laboratorio: medir el umbral real de despertar.
6. Control remoto y consultas contra un segundo equipo.
7. APRSdroid real con cable OTG (KISS por USB ya está probado sin móvil).
8. ~~Compilar el entorno E22P~~ **hecho el 2026-09-14** (compila; falta el módulo para probarlo).
9. **T-Echo y T-Echo Plus**: grabar y probar. La receta, con la copia de seguridad como
   primer paso obligatorio, está en `docs\PROCEDIMIENTO_TECHO.md`. **Sin probar todavía.**
10. ~~Pantalla de tinta electrónica del T-Echo~~ **HECHA el 2026-09-14**: driver propio del
    panel (SSD1681) escrito desde cero, con tres pantallas (estado, radio y último suceso),
    refresco parcial de 0,26 s y completo cada 20 para limpiar los fantasmas. Compila en los
    dos entornos del T-Echo. **Sin probar en hardware** (nadie lo ha grabado todavía).

## 7-bis. Decisiones que esperan al operador

Son cosas que **no puede resolver un agente**: hay que elegir y decirlo.

1. **`factory_reset`: ¿borra todo o conserva el acceso?** Hoy la **consola** conserva
   indicativo, operadores autorizados y control remoto; el **botón de la web** borra todo.
   Hay que elegir uno y que los tres sitios digan lo mismo (código y documentos).
2. **¿Cuál es el firmware de rescate bueno?** El fichero
   `data\rescue\firmware_bueno_sin_bluetooth.uf2` **no coincide** con el SHA256 que citaba el
   histórico. Hay que decidir cuál se da por bueno y anotarlo.

## 7-ter. Lo que NO se publica en GitHub (y por qué)

El repositorio público es una **copia saneada** del proyecto. Se quedan fuera, a propósito:

| Qué | Por qué |
|---|---|
| `data\igate_conf.json` | Lleva el **passcode de APRS-IS**, la **clave del WiFi** y la ubicación del iGate |
| `logs\`, `log_*.txt`, `diag_*.jsonl` | Son los movimientos reales del operador |
| `data\rescue\` (los `.uf2`) | **Una copia de seguridad del nodo lleva su configuración dentro** |
| `_archivo\backups\` | Copias del cerebro con el texto antiguo |
| Las **coordenadas** de los ejemplos y del histórico | **Desplazadas de forma uniforme** con `tools\anonimiza_coords.ps1` (la ruta conserva forma y distancias, pero no apunta a ningún sitio real; el desplazamiento queda apuntado para poder deshacerlo) |
| `_memoria\PENDIENTE.md` | Es la lista de tareas **personal** del operador: habla de sus amigos, de su material y de sus decisiones. No es información del proyecto |

**Si algún día se vuelve a subir**: repetir la auditoría (busca coordenadas en **los dos
formatos** —el decimal y el de APRS `ddmm.mmN`—, claves y passcodes, **incluso dentro de los
binarios**, porque un `.uf2` lleva texto legible dentro).

## 8. Cambios de este documento

- **2026-09-14 (3)**: la pantalla de tinta electrónica del T-Echo **pasa a hecha** (compila, sin
  probar en hardware); añadidas las decisiones que esperan al operador y la lista de lo que **no**
  se publica en GitHub; retirado `_memoria\PENDIENTE.md` del repositorio público por ser la lista
  personal del operador.
- **2026-09-14 (2)**: añadidos los entornos `techo` y `techo_plus` (compilan los dos), el
  documento de hardware `docs\HARDWARE_TECHO.md`, el procedimiento con la copia de seguridad
  `docs\PROCEDIMIENTO_TECHO.md` y el script `tools\copia_techo.ps1`. Retirado de la cola el
  E22P (ya compila) y anotada la pantalla del T-Echo como trabajo aparte.
- **2026-09-14**: creado. Sustituye al §4 «HANDOVER» de `cerebro.md`, que pasa a histórico.
