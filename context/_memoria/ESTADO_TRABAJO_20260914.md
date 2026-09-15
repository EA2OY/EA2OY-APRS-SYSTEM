# Estado del trabajo — noche del 2026-09-14

> **Para el operador**: esto es lo que hice mientras dormías, lo que está comprobado, lo que falta
> y **lo único que tienes que hacer tú** (activar GitHub Pages). Se lee en tres minutos.

---

## ✅ Lo que está hecho y comprobado

### 1. La memoria del proyecto, ordenada (era el lío que te preocupaba)

El problema medido: la lectura obligatoria eran **446 KB (~112.000 tokens)**, y no cabe en el
contexto de un agente. Ahora son **22 KB (~5.400 tokens)**, en tres capas:

| Capa | Fichero | Qué es |
|---|---|---|
| 1 | `Cerebro_Faketec_APRS_Igate_EA2OY\REGLAS.md` | Reglas, lista negra (lo que no se toca), cifras verificadas, mapa del proyecto |
| 2 | `...\ESTADO.md` | El **único** estado: qué lleva el nodo, qué está hecho y qué está bloqueado |
| 3 | `...\_memoria\` | Histórico completo (1122 líneas, intactas), neuronas, subnotas, pendientes |

- El histórico se partió **con git**, y está **demostrado** que no se perdió ni una línea
  (`--numstat` da **0 borrados**).
- **Verificador mecánico**: `tools\verifica_memoria.ps1` → **0 fallos**. Comprueba las capas, la
  codificación, las referencias por número de línea, los datos obsoletos y que el número de
  compilación cuadre con el binario.
- **Corregidos** los datos que estaban mal: el toque largo del manual (decía 800 ms; son **600**),
  los nombres de dos claves de configuración, y los documentos que decían que el firmware no existía.

### 2. Git en el proyecto

Repositorio **local** en `LoRa_APRS_iGate-main` (6 commits). Se queda fuera todo lo generado
(128 MB de compilaciones, 83 MB de cachés) **y toda tu información personal**.

### 3. Dos entornos nuevos: LilyGO T-Echo y T-Echo Plus

- **Los dos compilan** (35,5 % de flash). La Faketec sigue igual (43,6 %).
- Pinout sacado de **tres fuentes que coinciden** (el firmware de cfr34k que corre en esa placa, la
  tabla oficial de LilyGO y Meshtastic), documentado en `docs\HARDWARE_TECHO.md`.
- **Aviso importante**: la lista de pines que te dio el otro asistente de IA tenía **~40 % de
  errores**, incluido dar los pines de la **radio** como pines de la **pantalla**. No se usó.
- **Lo que NO llevan** (y está dicho claro, no escondido): la pantalla (tinta electrónica: necesita
  su propia capa de dibujo), y el IMU, motor y zumbador del Plus.
- **Copia de seguridad preparada**: `tools\copia_techo.ps1` y la receta en
  `docs\PROCEDIMIENTO_TECHO.md`. **Es el primer paso obligatorio** antes de grabar los de tus amigos.

### 4. Manual de usuario y generador de PDF

- **`docs\MANUAL_USUARIO.md`** (el manual para radioaficionados: 3 reglas de oro, modos, botón,
  pantalla, GPS, rutas, mensajería, registro, sueño, TNC, T-Echo, actualización, recomendaciones,
  preguntas frecuentes, ficha técnica y glosario). **Sin código.**
- **`tools\generar_pdf.ps1` + `tools\plantilla_kacho.tex`**: genera el PDF con **tu paleta** (fondo
  azul marino, dorado, verde), el **logo dibujado en vector** (el del splash, sin depender de
  imágenes) e **índice automático**. Aprendido de tu plantilla de NavaTastic y de sus lecciones.
- Probado: **17 páginas, sin un solo carácter corrupto** (comprobado leyendo el PDF).

### 5. Los dos repositorios publicados

| Repositorio | Qué lleva | Estado |
|---|---|---|
| **[EA2OY-APRS-SYSTEM](https://github.com/EA2OY/EA2OY-APRS-SYSTEM)** | Firmware (código + 4 entornos + firmware compilado), documentación, manual (texto y PDF), assets y `context/` (la memoria) | ✅ Subido, **133 ficheros** |
| **[CONFIGURADOR-WEB-APRS-EA2OY](https://github.com/EA2OY/CONFIGURADOR-WEB-APRS-EA2OY)** | El configurador como página web, el manual al lado y una portada | ✅ Subido |

**README del firmware**: reescrito al estilo de NavaTastic y MeshNavarra (portada con insignias, «qué
lo hace distinto» contado como problemas resueltos, guía rápida con el respaldo como primer paso,
tabla de placas con su estado real, el ecosistema Kacho, ficha técnica, aviso legal,
agradecimientos y versión en inglés).

---

## 🔴 LO ÚNICO QUE TIENES QUE HACER TÚ

**Activar GitHub Pages en el repositorio del configurador**, o la web no existirá:

1. Entra en <https://github.com/EA2OY/CONFIGURADOR-WEB-APRS-EA2OY/settings/pages>
2. En **Source** elige **Deploy from a branch**
3. Branch: **`main`** · carpeta: **`/ (root)`** → **Save**
4. Espera un minuto y abre: <https://ea2oy.github.io/CONFIGURADOR-WEB-APRS-EA2OY/>

*(Hoy esa dirección da 404 porque Pages está apagado. Sin eso, el configurador no se puede usar desde
el navegador: WebSerial necesita un sitio seguro y el mapa necesita cabecera `Referer`.)*

---

## 🔒 Lo que se quedó FUERA de internet (y por qué)

**Tus datos no se han publicado.** Comprobado con una auditoría que busca en **todos** los ficheros,
incluidos los binarios, y en **los dos formatos de coordenadas** (el decimal y el de APRS
`ddmm.mmN`):

| Qué | Dónde está | Por qué no se publica |
|---|---|---|
| Tu **passcode de APRS-IS** y la **clave de tu WiFi** | `data\igate_conf.json` | Cualquiera entraría en tu red y se identificaría como tú |
| **Respaldo del nodo** (`.uf2`) | `data\rescue\` | **Un `.uf2` de rescate lleva la configuración dentro**: indicativo y coordenadas |
| **Registros y volcados** de tus paseos | `logs\`, `log_*.txt` | Son tus movimientos reales |
| Tus **coordenadas** en los ejemplos y el histórico | varios | **Desplazadas de forma uniforme** (`tools\anonimiza_coords.ps1`): la ruta conserva su forma y sus distancias, pero no apunta a ningún sitio real. Es **reversible** |
| El **indicativo del iGate** de un amigo en una herramienta de pruebas | `tools\aprsis_fake_server.js` | Cambiado por uno genérico |

**Y una cosa que arreglé en el firmware**: la pantalla de arranque del nodo ponía **tu indicativo**
fijo. Ahora pone **el del usuario** (`cfg.callsign`). El configurador también se llamaba «APRS EA2OY
System» y pasa a **Kacho System**. El firmware **no trae ningún indicativo grabado**: cada uno pone
el suyo al configurarlo.

---

## ⏳ Lo que queda pendiente

### De esta noche (no me dio tiempo)
- **Los otros manuales**: querías varios (uso, comandos, montaje, instalación). He hecho **el
  principal** (el de uso, que es el que lee un radioaficionado) y **el generador ya está listo**,
  así que los demás son escribir el texto y volver a ejecutar el generador.
- **Release en GitHub** con los `.uf2`: los ficheros están en el repositorio
  ([`firmware/release/`](https://github.com/EA2OY/EA2OY-APRS-SYSTEM/tree/main/firmware/release)),
  pero no he creado la «Release» (necesita tu cuenta desde la web).

### De siempre (necesitan hardware o tu decisión)
1. **Confirmar que el USB obedece** (tú, con el configurador y el nodo enchufado).
2. **Grabar y probar los dos T-Echo** con tus amigos (con la copia de seguridad primero).
3. **Pantalla de tinta electrónica** del T-Echo: es un trabajo propio, no un ajuste.
4. **`factory_reset`: ¿borra todo o conserva el acceso?** Hoy la consola conserva y la web borra.
5. **Cuál es el firmware de rescate bueno** (el fichero no coincide con el que citaba el histórico).
6. Cola de banco: sueño con fuente de laboratorio, control remoto contra un segundo equipo,
   APRSdroid con cable OTG.

---

## 📌 Cómo retomar esto

1. Lee `Cerebro_Faketec_APRS_Igate_EA2OY\ESTADO.md` (la Capa 2): ahí está el estado real.
2. Mira `_memoria\PENDIENTE.md`: tiene la lista de tareas, lo hecho y lo que espera tu decisión.
3. Si algo no cuadra: `powershell -File tools\verifica_memoria.ps1` te dice si la memoria está
   coherente (0 = bien).
