<div align="center">

<img src="assets/logo_kacho.svg" alt="Kacho System" width="170"/>

# 📡 Kacho System

### Firmware APRS-LoRa para 433 MHz — repetidor y rastreador

*Para placas **Faketec / ProMicro** (nRF52840) y **LilyGO T-Echo**, con radio SX1262*

<br/>

[![Licencia: GPL v3](https://img.shields.io/badge/Licencia-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Plataforma: nRF52840](https://img.shields.io/badge/Plataforma-nRF52840-green.svg)](https://www.nordicsemi.com/Products/nRF52840)
[![Banda: 433 MHz](https://img.shields.io/badge/Banda-433%20MHz-orange.svg)](#-ficha-técnica)
[![APRS: ecosistema LoRa](https://img.shields.io/badge/APRS-ecosistema%20LoRa-yellow.svg)](#-compatibilidad)
[![Manual: PDF](https://img.shields.io/badge/Manual-PDF%20en%20espa%C3%B1ol-critical.svg)](assets/Manual_Kacho_System.pdf)
[![Ko-fi](https://img.shields.io/badge/Ko--fi-Caf%C3%A9%20voluntario-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/ea2oy)

</div>

> 📻 **Qué es, en una frase**: un firmware que convierte una placa nRF52840 con radio SX1262 en un
> **nodo de APRS sobre LoRa** en 433 MHz — repite lo que oye, publica dónde está, informa del
> tiempo que hace y aguanta solo en el monte sin que tengas que subir a resetearlo.

<div align="center">

[**📘 Manual de uso (PDF)**](assets/Manual_Kacho_System.pdf) ·
[**⚙️ Configurador web (online)**](https://ea2oy.github.io/CONFIGURADOR-WEB-APRS-EA2OY/) ·
[**📥 Firmware compilado**](firmware/release) ·
[**📻 Cómo se instala**](#-guía-rápida-de-instalación)

</div>

---

## 🧠 ¿Qué lo hace distinto?

Un nodo de APRS bien instalado vive en un tejado, en un repetidor de monte o dentro de una mochila.
No puedes estar pendiente de él: si se queda sin batería, si publica una posición falsa o si gasta
el canal a lo tonto, el problema es tuyo. **Kacho System resuelve eso de raíz:**

☀️ **Se duerme solo y despierta con el sol.** Si la batería baja de un umbral, el nodo **avisa por
radio**, se apaga limpiamente y programa su despertar: cuando la tensión vuelva a subir (por
ejemplo, cuando el panel solar cargue), arranca solo y lo anuncia. **Sin que nadie suba a pulsar un
botón.** Y **nunca se duerme por una lectura falsa**: exige varias medidas seguidas por debajo del
umbral, porque con radiofrecuencia cerca el divisor de tensión puede mentir.

📍 **Nunca publica una posición que no sea real.** En modo rastreador, si el GPS aún no ha fijado,
el nodo **no manda posición**: manda un aviso de que está buscando satélites. Antes de esto, muchos
nodos publican la posición fija «de repuesto» y acabas apareciendo en un sitio donde no estás.
Aquí no. Y si quieres publicar dónde estuviste, un **doble toque** manda la última posición real
**con su hora de verdad** y el aviso **`ULTIMA CONOCIDA`**, para que nadie se confunda.

🧠 **Cadencia inteligente (SmartBeaconing).** Emite más a menudo cuando corres y menos cuando vas
despacio, con perfiles de **a pie, bici o coche**. Marca los **giros y rotondas** aunque no toque
por tiempo, y **parado sigue publicando cada 15 minutos** para que el mapa no se quede viejo. Todo
sin gastar aire por los saltos tontos del GPS: el disparo por distancia exige **movimiento
sostenido de verdad**.

🌦️ **Meteorología y telemetría estándar.** Cada 55 minutos manda el **paquete de tiempo** que hace
que aprs.fi y findu te reconozcan como estación meteorológica (con gráficas históricas), y cada 53
minutos **telemetría con canales y unidades**. Los canales se ajustan solos a los sensores que
tengas de verdad: si falta uno, su canal se llama `na` y **no se manda un cero falso**.

💾 **Registro de viaje en su propia memoria.** Guarda todo lo que hace (balizas, repetidos,
recepciones y eventos) con la hora del GPS, **sobrevive a los apagones**, se recicla solo al
llenarse y se descarga desde el navegador como **GPX, KML o CSV** — listo para Wikiloc, Google
Earth, Garmin o Strava. Cada recorrido va separado **por sesión**, con nombre
`INDICATIVO_AAAAMMDD_HHMM`, y cada punto dice **por qué salió** (por tiempo, por giro, por
distancia, a mano, parado...).

📺 **Pantalla propia y menú con un solo botón.** Las placas Faketec llevan OLED y el nodo la
aprovecha: escenas que van rotando (estado, últimas recibidas, sensores, estaciones oídas...) y un
**menú por categorías** navegable con un único botón. Las tres opciones del modo de trabajo, a la
vista. Y si te olvidas el menú abierto, **se cierra solo a los 20 segundos** avisando con una
cuenta atrás.

🔧 **Configurador web, sin instalar nada.** Un **solo fichero** que habla con el nodo por USB desde
Chrome o Edge: lee la configuración real del aparato, la edita con **ayuda en cada casilla** y te
avisa de lo que conviene saber antes de guardar (por ejemplo, si pides demasiados saltos para un
nodo fijo). Está servido como **página web** para que el navegador deje hablar con el USB y para
que funcione el mapa de las rutas.

✉️ **Mensajería APRS completa.** Manda mensajes a otra estación (con **acuses** y **reintentos**
automáticos cada 30 segundos), recibe los que te llegan, publica **boletines** para todos y
**objetos** en el mapa de los demás. Todo desde el navegador, por comandos o desde la propia
pantalla del nodo.

🔌 **También es un módem (TNC).** Conectado por USB puede hacer de **TNC en modo KISS** (lo que
piden APRSdroid y las apps modernas) o **TNC2** (texto, para programas clásicos). En modo KISS
**manda la aplicación** y el nodo se calla: solo repite y transmite lo que le entreguen.

🌍 **Habla el idioma de la red.** 433,775 MHz · SF12 · 125 kHz · CR 4/5, con el mismo entramado que
el resto del ecosistema LoRa APRS. Aparece en **aprs.fi**, findu y los mapas LoRa como cualquier
otro nodo, sin configurar nada raro.

> 🚫 **Lo que NO es**: **no es un iGate**. Un iGate conecta la radio con internet y necesita wifi o
> cable de red; el nRF52840 no tiene ninguno de los dos. Este nodo **solo habla por radio**: para
> llegar a los mapas necesita que un iGate lo oiga. Es una decisión de diseño, no un límite que se
> pueda parchear.

---

## ⚡ Guía rápida de instalación

> 💡 **El firmware no trae ningún indicativo grabado.** La configuración vive en el propio nodo y la
> pones tú: cada aparato se identifica con **la llamada de su dueño**.

```
1. Copia lo que traía tu placa   (por si acaso: siempre se puede volver atrás)
2. Graba el firmware de tu placa  (un fichero .uf2 a una unidad de memoria)
3. Configúralo                     (indicativo, coordenadas y modo, desde el navegador)
4. Comprueba que sale al aire      (mira si un iGate cercano te oye)
```

### 1️⃣ Respalda lo que traía tu placa

Antes de grabar nada: pon la placa en **modo grabación** (doble toque al botón de reset hasta que
aparezca una unidad de memoria) y **copia su contenido a tu ordenador**. El bootloader expone un
fichero con **todo** lo que lleva grabado, así que siempre podrás dejarla como estaba.

### 2️⃣ Graba el firmware

1. Descarga el fichero de **tu placa** de [`firmware/release/`](firmware/release):

| Tu placa | Fichero |
|---|---|
| Faketec V1-V6 + **HT-RA62** (SX1262) | `KachoSystem_v1.0alpha_b68_Faketec_HT-RA62_433.uf2` |
| Faketec / ProMicro + **E22P-433M30S** | `KachoSystem_v1.0alpha_b68_Faketec_E22P-433M30S.uf2` |
| **LilyGO T-Echo**, con cargador **S140 versión 7** | `KachoSystem_v1.0alpha_b68_LilyGO_T-Echo_S140v7.uf2` |
| **LilyGO T-Echo Plus**, con cargador **S140 versión 7** | `KachoSystem_v1.0alpha_b68_LilyGO_T-Echo-Plus_S140v7.uf2` |

> 📌 **Los cuatro ficheros de `firmware/release/` son estos** (los nombres exactos, para
> que no haya dudas al buscar en la carpeta):
> `KachoSystem_v1.0alpha_b68_Faketec_E22P-433M30S.uf2`,
> `KachoSystem_v1.0alpha_b68_Faketec_HT-RA62_433.uf2`,
> `KachoSystem_v1.0alpha_b68_LilyGO_T-Echo-Plus_S140v7.uf2` y
> `KachoSystem_v1.0alpha_b68_LilyGO_T-Echo_S140v7.uf2`.

> ✅ **Cada binario dice lo que lleva (contador de compilación arreglado el 2026-09-16)**: el
> número sube **cuando cambia el código**, así que el que contesta el nodo por USB
> (`1.0alpha b68`) identifica el firmware que lleva dentro, y coincide con el nombre del
> fichero. Hasta esa noche el contador estuvo clavado en `b9` (un fallo del propio contador:
> siete cambios de firmware seguidos salieron con el mismo número), así que **si tienes
> descargado un `..._b9_...`, es el mismo firmware que aquel `b13` pero con el número viejo**:
> quédate siempre con el número más alto.
>
> | Fichero | Fecha | Bytes | SHA-256 |
> |---|---|---|---|
> | `KachoSystem_v1.0alpha_b68_Faketec_E22P-433M30S.uf2` | 2026-09-21 | 742400 | `C28A26019966E8D4ADB574DADB884A2A43483CA55BD7C9D752F942DB88083B2D` |
> | `KachoSystem_v1.0alpha_b68_Faketec_HT-RA62_433.uf2` | 2026-09-21 | 742400 | `B35512953FECC4D41332A76B544620718A4A6518B873158673693C5CE35A034D` |
> | `KachoSystem_v1.0alpha_b68_LilyGO_T-Echo-Plus_S140v7.uf2` | 2026-09-21 | 732672 | `57DE1DC00CA8F8649BFB2D5C66FA3AA7119508DD01058EC8E70D3ACD4F9076F1` |
> | `KachoSystem_v1.0alpha_b68_LilyGO_T-Echo_S140v7.uf2` | 2026-09-21 | 735232 | `E7D2AD88142A22B8329EB147B5E6BB00C96FAB97D94A10998AA2F6DEE25B77F5` |
>
> Los cuatro son la compilación **b68** del **2026-09-21**, hecha desde el código de este mismo
> repositorio (etiqueta `b68-funcional`), y es la que está **verificada en placa**.
> Para comprobar la huella en tu ordenador (Windows, PowerShell):
> `Get-FileHash .\KachoSystem_v1.0alpha_b68_LilyGO_T-Echo_S140v7.uf2 -Algorithm SHA256`

> ⚠️ **El T-Echo necesita cargador S140 versión 7** y su fichero es el `..._S140v7.uf2`.
> Comprueba la versión en el fichero **`INFO_UF2.TXT`** de la unidad de grabación: debe
> poner **`SoftDevice: S140 version 7.x`**. Grabar el fichero equivocado **pisa los últimos
> 4 KB del cargador y deja la radio inservible**. Si tu placa lleva la versión 6, este
> proyecto **no** te sirve tal cual: hay que actualizar antes el cargador de arranque
> (no lo hace este proyecto).

2. Doble toque al **botón de reset** hasta que aparezca la unidad de memoria (`NICENANO` o
   `TECHOBOOT`).
3. **Copia el `.uf2`** a esa unidad y espera a que desaparezca sola: la placa se reinicia.

> ⚠️ En el **T-Echo** usa un cable **USB-A a USB-C** (con USB-C a USB-C algunas unidades no se
> alimentan bien; lo avisa el propio fabricante).

### 3️⃣ Configúralo

Abre el **[configurador web](https://ea2oy.github.io/CONFIGURADOR-WEB-APRS-EA2OY/)** con **Chrome o
Edge** (en un ordenador: WebSerial no existe en el móvil), pulsa **Conectar**, elige el puerto y
rellena:

| Ajuste | Por qué importa |
|---|---|
| **Tu indicativo** | Sin indicativo no eres nadie en el aire. Es lo primero |
| **Tus coordenadas** | Si están a cero aparecerás en medio del Atlántico |
| **Modo de trabajo** | Repetidor, rastreador o ambos |
| **Rutas (saltos)** | Deciden si te repiten los demás (y cuánto aire gastas) |

El botón **«Recuperar valores recomendados»** rellena el resto con ajustes sensatos de la red
— **menos el indicativo y las coordenadas**, que son tuyos.

### 4️⃣ Comprueba que funciona

Lo que de verdad dice si funciona **es la radio**, no la pantalla: mira en la web de tu iGate (o en
**aprs.fi**) si apareces. En modo rastreador, la primera posición sale **en cuanto el GPS fija**, y
mientras tanto el nodo avisa de que está buscando satélites.

---

## 📥 Tu placa y tu fichero

| Placa | Radio | Cómo va |
|---|---|---|
| **Faketec V1-V6** (nRF52840) | Heltec **HT-RA62** (SX1262, 22 dBm) | ✅ **Probada en el aire** (nodo EA2OY-7) |
| **Faketec / ProMicro** (nRF52840) | Ebyte **E22P-433M30S** (SX1262, 1 W de salida) | ✅ Compila · ⏳ falta el módulo para probarla |
| **LilyGO T-Echo** | SX1262 | ✅ Compila · ⏳ pendiente de grabar |
| **LilyGO T-Echo Plus** | SX1262 | ✅ **Grabada y probada**: la pantalla de tinta ya funciona |

> 📟 **Sobre el T-Echo**: lleva **pantalla de tinta electrónica** (200×200) en vez de OLED, y **ya se
> dibuja en las DOS placas**: escenas y **menú** en pantalla, igual en el T-Echo normal que en el
> Plus. Lo que solo tiene el **T-Echo Plus** son los **avisos por vibración y por sonido** (motor y
> zumbador). Ten en cuenta que la tinta
> es lenta: un refresco completo tarda **2 segundos**.

**Sensores** (todos opcionales, se detectan solos al arrancar): **BMP280 / BME280 / BME680**
(temperatura, presión, humedad y calidad del aire según el chip), **AHT20** (humedad), **INA219**
(tensión y consumo), **GPS** y **OLED** (SSD1306 o SH1106).

---

## 🎛️ Los tres modos de trabajo

| Modo | Qué hace |
|---|---|
| **Repetidor** | Escucha las tramas de otros y **las repite** para que lleguen más lejos. Además anuncia su posición fija |
| **Rastreador** | **Publica dónde estás**, con posición, rumbo, velocidad y altura. **No repite** a nadie |
| **Ambos** | Las dos cosas a la vez: repite a los demás **y** publica tu posición |

El repetidor y el rastreador son **independientes**: repetir es oír una trama y volver a emitirla;
rastrear es mandar tu posición cada cierto tiempo. El modo decide **si el rastreador trabaja** y
**qué posición se anuncia**, no si repites.

**Cuándo manda posición, en resumen:**

| Situación | ¿Manda posición? |
|---|---|
| Repetidor, al arrancar y cada X minutos | ✅ La posición fija configurada |
| Repetidor con GPS activado y fijación | ✅ La del GPS en vivo |
| Rastreador o Ambos, **sin** fijación | ❌ Solo el aviso «buscando satélites» |
| Rastreador o Ambos, **con** fijación | ✅ La real, la primera en cuanto fija |
| Rastreador o Ambos, en movimiento | ✅ Según cadencia, giros y distancia |
| Rastreador o Ambos, **parado** | ✅ Una baliza lenta **cada 15 minutos** |
| Doble toque del botón | ✅ La real, o la última conocida (marcada como histórica) |

---

## 🔧 Compilar desde el código

Necesitas [PlatformIO](https://platformio.org/) y Python.

```bash
cd firmware

pio run -e faketec_sx1262_433      # Faketec + HT-RA62 (SX1262)
pio run -e faketec_e22p_433        # Faketec + E22P-433M30S (1 W)
pio run -e techo_s140v7            # LilyGO T-Echo, cargador S140 v7
pio run -e techo_plus_s140v7       # LilyGO T-Echo Plus, cargador S140 v7 (pantalla de tinta)
```

En `firmware/release/` **solo se publican los del T-Echo con cargador S140 versión 7**
(`techo_s140v7`, `techo_plus_s140v7`). En `platformio.ini` siguen estando los entornos del
cargador v6 (`techo_plus_s140v6`, `techo_minimo_v6`) como referencia histórica, pero **no se
distribuyen**: este proyecto publica un único camino, la versión 7. El entorno
**`techo_plus_hello`** es un banco de pruebas de la pantalla y hoy **no compila**: le falta su
fuente en el repositorio.

El resultado queda en `.pio/build/<entorno>/firmware.uf2`, listo para copiar a la placa.

**Herramientas del proyecto**:

```powershell
powershell -File tools\verifica_memoria.ps1    # coherencia de la memoria y del numero de compilacion (0 = bien)
powershell -File tools\generar_pdf.ps1         # rehace el manual en PDF (necesita Pandoc y MiKTeX)
powershell -File tools\copia_techo.ps1         # copia de seguridad de un T-Echo antes de grabarlo
```

---

## 🧩 El ecosistema Kacho

Este firmware forma parte de una familia de proyectos del mismo autor, pensados para trabajar
juntos:

| Proyecto | Qué es |
|---|---|
| 📡 **Kacho System** (este) | El **firmware del nodo APRS-LoRa de 433 MHz**: repetidor y rastreador |
| ⚙️ **[Configurador web](https://github.com/EA2OY/CONFIGURADOR-WEB-APRS-EA2OY)** | El configurador, servido como página web (WebSerial y el mapa lo necesitan) |
| 🏔️ **[NavaTastic](https://github.com/EA2OY/NavaTastic)** | El **firmware del repetidor solar de Meshtastic** para infraestructura de montaña |
| 📱 **[MeshNavarra Utility](https://github.com/EA2OY/MeshNavarra-Utility)** | La **app Android** para administrar nodos Meshtastic/NavaTastic |
| 🎮 **[Kacho Contest System](https://github.com/EA2OY/KachoContestSystem-demo)** | El **sistema de pulsadores** para concursos y eventos |

**Nada de esto se necesita para lo otro**: cada proyecto funciona solo. Pero **APRS y Meshtastic se
complementan**: el APRS llega a los mapas y a los visores de radioaficionado, y Meshtastic da
mensajería de malla. Muchos nodos conviven con los dos.

---

## 📚 Documentación

| Documento | Qué encontrarás |
|---|---|
| 📕 **[Manual de uso (PDF)](assets/Manual_Kacho_System.pdf)** | **Empieza por aquí.** Para radioaficionados: qué es, cómo se usa, recomendaciones y preguntas frecuentes, en lenguaje llano y **sin código** |
| 📄 [`MANUAL_USUARIO.md`](MANUAL_USUARIO.md) | El mismo manual en texto, para leer o editar |
| 📥 **[Grabar el firmware, paso a paso (web)](https://ea2oy.github.io/CONFIGURADOR-WEB-APRS-EA2OY/flasher.html)** | La guía del flasher, con los ficheros `.uf2` de cada placa para descargar y la comprobación del estado del nodo |
| 📋 [`docs/FUNCIONES.md`](docs/FUNCIONES.md) | Todas las funciones, en una lista |
| 📖 [`docs/MANUAL_DE_USO.md`](docs/MANUAL_DE_USO.md) | ⚠️ **Versión antigua, superada por el manual de arriba** (se conserva porque explica las cosas con otras palabras). El manual vigente es el PDF / `MANUAL_USUARIO.md` |
| 🔌 [`docs/protocol_config_v1.md`](docs/protocol_config_v1.md) | El protocolo de configuración por USB (para quien quiera integrarlo) |
| 📶 [`docs/USO_DEL_AIRE.md`](docs/USO_DEL_AIRE.md) | Cuánto aire gasta el nodo, y las normas de la red APRS |
| 🛠️ [`docs/HARDWARE_TECHO.md`](docs/HARDWARE_TECHO.md) | El hardware del LilyGO T-Echo, con sus fuentes |
| 🔨 [`docs/PROYECTO_BUTTER_USB.md`](docs/PROYECTO_BUTTER_USB.md) y [`docs/PROYECTO_BUTTER_TECHO.md`](docs/PROYECTO_BUTTER_TECHO.md) | Cómo se quitaron los bloqueos del USB y de la pantalla (por qué el nodo responde y refresca sin tirones) |

---

## 📊 Ficha técnica

| Dato | Valor |
|---|---|
| Banda | **433 MHz** (aficionados, uso secundario) |
| Frecuencia de la red | **433,775 MHz** |
| Modulación | LoRa · **SF12** · ancho **125 kHz** · codificación **4/5** |
| Potencia | Hasta **22 dBm** (HT-RA62) · hasta **1 W de salida** en los módulos E22P, que llevan amplificador propio |
| Microcontrolador | Nordic **nRF52840** (Cortex-M4, 64 MHz) |
| Memoria | 1 MB de flash · 256 KB de RAM |
| Alimentación | Litio de 1 celda · 3 pilas NiMH · USB |
| Consumo dormido | ~**0,4 mA** (placa Faketec con SX1262) |
| Pantalla | OLED 128×64 (SSD1306/SH1106) · tinta electrónica 200×200 en el T-Echo |
| Sensores | BMP280 · BME280 · BME680 · AHT20 · INA219 · GPS |
| Licencia | **GPL-3.0** |

---

## ⚖️ Aviso legal

Proyecto **de radioaficionado**, distribuido **tal cual** y **sin garantía de ningún tipo** — mira
la licencia [GPL-3.0](LICENSE) completa.

- 🪪 **Necesitas licencia de radioaficionado** para transmitir, y **eres responsable** de lo que
  emita tu nodo.
- 🙅 **Usa tu indicativo, nunca el de otro.** Tampoco la clave de otra estación, ni para leer.
- 📡 **No transmitas sin antena**: además de no llegar, puedes dañar la etapa de salida.
- 📻 **Respeta el canal**: es compartido. El manual explica las cadencias recomendadas y por qué.
- 🔋 **El montaje es tuyo**: el dimensionado de batería y panel, y el cumplimiento de la normativa
  aplicable, son responsabilidad de quien instala el nodo.

---

## 🙏 Agradecimientos

Este firmware no habría sido posible sin la comunidad del APRS y del LoRa:

- **Ricardo (CA2RXU)** y su ecosistema **LoRa APRS** de 433 MHz, que es la referencia de
  interoperabilidad y la razón de que todos estos nodos se hablen entre sí.
- **Thomas Kolb (cfr34k)** y su firmware [t-echo-lora-aprs](https://github.com/cfr34k/t-echo-lora-aprs),
  que ha sido la guía para entender el hardware del T-Echo.
- A la **malla de Navarra** y a la gente del **APRS español**, por las pruebas, los avisos y las
  respuestas.

---

## ☕ Apoyo voluntario

Este proyecto es y será siempre **libre, abierto y gratuito**. Si alguien, de forma estrictamente
voluntaria, quiere invitar a un café para costear placas y componentes de pruebas:

<div align="center">

[![Apoyar en Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/ea2oy)

**73 de EA2OY**

</div>

---

# Kacho System (English)

**APRS-LoRa firmware for 433 MHz — digipeater and tracker**, for nRF52840 boards with an SX1262
radio (**Faketec / ProMicro** and **LilyGO T-Echo**).

It turns an nRF52840 board into an **APRS over LoRa** node on **433 MHz**, in three modes:
**digipeater**, **tracker** (GPS) or **both**. It speaks the standard 433 MHz LoRa APRS ecosystem,
so it shows up on **aprs.fi**, findu and the LoRa maps of your area.

**It is not an iGate**: the nRF52840 has no WiFi, so the node only talks over the air and needs an
iGate to hear it in order to reach the map. That is by design.

### What makes it different

- ☀️ **Solar-friendly sleep**: it warns over the air, sleeps on low battery and **wakes up when the
  panel recovers the voltage**, with no physical access needed — and it never sleeps on a single
  false reading.
- 📍 **Never publishes a fake position**: without a GPS fix the tracker stays quiet; the last known
  position can be sent by hand and is **flagged as historical**.
- 🧠 **SmartBeaconing**: speed-aware rate, corner pegging, a slow 15-minute beacon while parked, and
  no phantom beacons from GPS jitter.
- 🌦️ **Standard weather and telemetry** packets, charted on aprs.fi.
- 💾 **Trip log** in its own flash: survives power cuts, split per session, exportable as
  **GPX / KML / CSV**.
- 📺 **OLED menu** driven by a **single button** (Faketec boards).
- 🔧 **Single-file web configurator**, served
  [online](https://ea2oy.github.io/CONFIGURADOR-WEB-APRS-EA2OY/) (Chrome/Edge over USB).
- ✉️ **APRS messaging** with acks and retries, bulletins and objects.
- 🔌 **USB TNC** in **KISS** or **TNC2** mode, for APRSdroid and friends.

### Supported boards

| Board | Radio | Status |
|---|---|---|
| **Faketec V1-V6** (nRF52840) | Heltec **HT-RA62** (SX1262) | ✅ Field proven |
| **Faketec / ProMicro** (nRF52840) | Ebyte **E22P-433M30S** (1 W) | ✅ Builds · ⏳ module pending |
| **LilyGO T-Echo** | SX1262 | ✅ Builds · ⏳ flashing pending |
| **LilyGO T-Echo Plus** | SX1262 | ✅ Flashed and tested · e-paper screen working |

The T-Echo carries an **e-paper display**, and **both boards drive it** (scenes and on-screen
menu). What only the **T-Echo Plus** adds is that board's **vibration/sound alerts**. The T-Echo
needs an **S140 version 7 bootloader** (check `INFO_UF2.TXT` on the flashing drive); the version 6
bootloader is not supported by the published files.

### Firmware files (the four published files, and who each one is for)

| Your board | File |
|---|---|
| Faketec V1-V6 + **HT-RA62** (SX1262) | `KachoSystem_v1.0alpha_b68_Faketec_HT-RA62_433.uf2` |
| Faketec / ProMicro + **E22P-433M30S** | `KachoSystem_v1.0alpha_b68_Faketec_E22P-433M30S.uf2` |
| **LilyGO T-Echo**, bootloader **S140 version 7** | `KachoSystem_v1.0alpha_b68_LilyGO_T-Echo_S140v7.uf2` |
| **LilyGO T-Echo Plus**, bootloader **S140 version 7** | `KachoSystem_v1.0alpha_b68_LilyGO_T-Echo-Plus_S140v7.uf2` |

> ✅ **Every binary says what it carries (build counter fixed on 2026-09-16)**: the number goes up
> **when the code changes**, so what the node answers over USB (`1.0alpha b68`) identifies the
> firmware, and it matches the file name. Until that night the counter was stuck at `b9` (a bug in
> the counter itself: seven firmware changes in a row came out with the same number), so **if you
> downloaded a `..._b9_...` file, it is the same firmware as that old `b13` with the old number**:
> always keep the highest number. Date and SHA-256 to check what you got:
>
> | File | Date | Bytes | SHA-256 |
> |---|---|---|---|
> | `KachoSystem_v1.0alpha_b68_Faketec_E22P-433M30S.uf2` | 2026-09-21 | 742400 | `C28A26019966E8D4ADB574DADB884A2A43483CA55BD7C9D752F942DB88083B2D` |
> | `KachoSystem_v1.0alpha_b68_Faketec_HT-RA62_433.uf2` | 2026-09-21 | 742400 | `B35512953FECC4D41332A76B544620718A4A6518B873158673693C5CE35A034D` |
> | `KachoSystem_v1.0alpha_b68_LilyGO_T-Echo-Plus_S140v7.uf2` | 2026-09-21 | 732672 | `57DE1DC00CA8F8649BFB2D5C66FA3AA7119508DD01058EC8E70D3ACD4F9076F1` |
> | `KachoSystem_v1.0alpha_b68_LilyGO_T-Echo_S140v7.uf2` | 2026-09-21 | 735232 | `E7D2AD88142A22B8329EB147B5E6BB00C96FAB97D94A10998AA2F6DEE25B77F5` |
>
> All four are the **b68** build of **2026-09-21**, made from this repository's code (tag
> `b68-funcional`), and it is the one **verified on hardware**.

### Build

```bash
cd firmware
pio run -e faketec_sx1262_433   # or: faketec_e22p_433, techo_s140v7, techo_plus_s140v7
```

### Documentation

- 📕 **[User manual (PDF, Spanish)](assets/Manual_Kacho_System.pdf)**
- 📥 **[Flashing the firmware, step by step (web, Spanish)](https://ea2oy.github.io/CONFIGURADOR-WEB-APRS-EA2OY/flasher.html)**
- 📄 [`docs/`](docs) — features, configuration protocol, airtime analysis, T-Echo hardware

### License

**GPL-3.0** — see [LICENSE](LICENSE). Third-party notices in
[`firmware/THIRD_PARTY.md`](firmware/THIRD_PARTY.md).

<div align="center">

**73 de EA2OY** · *Made with ❤️ and a lot of coffee in Navarra*

</div>
