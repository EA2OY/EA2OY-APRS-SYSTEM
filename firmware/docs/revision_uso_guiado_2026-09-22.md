# Revision de experiencia de uso del GUIADO POR TRACKS (2026-09-22)

Una revision **independiente** (un revisor que no escribio el codigo) leyo el modulo de tracks y
la pantalla de guiado con una sola pregunta: **¿esto es lo que un usuario esperaria al usarlo
andando por el monte, o funciona "como esta programado" pero resulta confuso o inutil?**

Su veredicto fue: *"el motor de navegacion hace lo que pidio el operador, pero la capa que el
usuario toca no"*. Encontro cinco cosas que ni el que escribio el codigo ni sus comprobadores
habian visto, porque todos miraban la MECANICA (¿hacen bien las cuentas?) y no el USO.

**Este documento existe para que no se pierdan.** Lo arreglado lleva la nota de como se arreglo;
lo pendiente esta marcado como PENDIENTE con lo que habria que decidir.

---

# SEGUNDA REVISION: los seis arreglos (2026-09-22, misma fecha)

Se aplicaron los seis arreglos de abajo y **otro revisor independiente** los reviso antes de
compilar. Encontro **dos fallos que rompian la funcion entera** y que no tenian nada que ver con
los arreglos: salieron al contar las paginas de la flash. Son los mas graves de toda la sesion.

### ★★★ (A) LOS PUNTOS DEL TRACK EN VIVO NO ESTABAN ALINEADOS A 4 KB — ROMPIA LA GRABACION
`flashBorraPagina()` usa ERASEPAGE, que borra la pagina de 4 KB que **CONTIENE** la direccion, no
"desde" ella. La cabecera de fecha ocupaba los primeros 64 bytes de la zona y los puntos
empezaban justo detras, en `0x98040`. Consecuencias, las tres reales:
- **cada cambio de pagina borraba los ultimos 64 bytes de la ANTERIOR**, o sea sus 6 ultimos
  puntos, DESPUES de haberlos dado por validos. Se leian como 0xFF = (-1,-1) grados: puntos en el
  Golfo de Guinea. Pasaba **ya a los 408 puntos, ~4 km de caminata**;
- la ultima pagina se metia 64 bytes DENTRO de la ranura 1 y le borraba la cabecera;
- `tracksVivoOlvida()` escribia la fecha y tres lineas despues borraba esa misma pagina, asi que
  el track se quedaba **sin fecha para siempre** ("Vivo sin hora").

ARREGLADO: la cabecera ocupa **su propia pagina entera** (`TRACK_VIVO_CAB = 0x1000`), asi que los
puntos empiezan en `0x99000`, alineados. La zona crece 4 KB y las ranuras se desplazan a
`0x0AD000`; el fin (`0x0C6000`) se actualizo TAMBIEN en `extra_scripts/nrf52_uf2.py`, que vigila
estas direcciones. Nuevo comprobador `tools/prueba_mapa_tracks.py`: mira las constantes del
firmware y verifica que **toda pagina que se pueda borrar este alineada**, que las zonas no se
pisen, y que el generador de UF2 vigile las MISMAS direcciones que usa el firmware.

### ★★★ (B) LA RANURA SE BORRABA SU PROPIA CABECERA — LA SUBIDA NO PODIA FUNCIONAR NUNCA
`tracksRanuraEmpieza()` borraba la pagina 0, escribia el `SlotCab` ('TLS1' + fecha + puntos) y
despues llamaba a `pagInicia()`, **que empieza borrando la misma pagina**: el magico quedaba en
0xFF. Resultado: la ranura se leia como vacia, todos los `track_chunk` se rechazaban, y **cargar
un track desde el configurador no podia funcionar nunca**. El sintoma visible habria sido las
cinco filas "Vacia" sin ninguna explicacion.

ARREGLADO: la cabecera de la pagina 0 se escribe **a mano, sin borrar** (la pagina ya esta
borrada en la linea de arriba). Y ademas **se borran las cinco paginas de la ranura**, no solo la
de la cabecera, por un motivo que el primer intento no tuvo en cuenta: **la flash no se puede
escribir dos veces** (NVMC solo pasa bits de 1 a 0), asi que una segunda subida encima de una
primera habria mezclado puntos viejos y nuevos. Cuesta ~140 ms una vez por subida, no por punto.

### ★★ (C) EL TOPE QUE SE ANUNCIABA NO ERA EL QUE SE APLICABA: 2040 contra 2037
`TRACK_SLOT_PUNTOS` se calculaba en la cabecera como si las 5 paginas fueran iguales (2040), pero
la pagina 0 lleva delante la cabecera de ranura (32 bytes) y le caben menos: 405 + 4x408 = **2037**.
El numero que de verdad se aplicaba vivia escondido en `tracks.cpp` (`SLOT_PUNTOS_REAL`). Consecuencia:
el configurador simplificaba un GPX a 2040 puntos **porque el nodo le dijo que ese era el tope**, y
el nodo rechazaba la subida con "no se pudo preparar la ranura". La culpa no era del configurador.

ARREGLADO: **hay un solo numero**, en `tracks.h`, y lo usan los dos (el que valida y el que se
anuncia). Un `static_assert` comprueba que la cuenta de la cabecera coincida con
`sizeof(SlotCab)`: si alguien cambia la estructura y no la constante, **no compila**.

### ★ (D) El rotulo "(ruta)" se imprimia ENCIMA de la altitud
Iba en (8,140) y "ALT" en (8,142): a escala 1 cada linea ocupa 7 filas, asi que se pisaban y la
franja de la altitud salia ilegible. Y no habia hueco debajo (6 px entre lineas, y hacen falta 7).

ARREGLADO de otra forma, porque **no cabe en ningun sitio**: se intento a la derecha y el numero a
escala 2 llega a 14 caracteres (178 px desde x=8), asi que tampoco. La solucion **no ocupa nada**:
en kilometros se ponen **dos cifras decimales** ("FALTAN 2.40 km") y en metros no ("FALTAN 500 m").
Ese punto ya distingue una distancia medida de una cuenta redondeada, que es el convenio de
cualquier aparato de estos. Las cuatro filas de datos se declararon como constantes en un solo
sitio, para que la disposicion se lea de una vez.

### ★ (E) El aviso "GRAB OFF" desaparecia al salir de la pantalla, y la pausa seguia
Estaba solo en la pantalla de guiado. Con una pulsacion larga se pasa a la lista de tracks (donde
esta "Finalizar guiado"), el aviso desaparecia, **y la grabacion seguia parada**. Peor: desde la
lista se puede volver al carrusel, y ahi no habia ninguna indicacion.

ARREGLADO: el aviso se pinta en **`pintaCabecera()`**, que sale en TODAS las pantallas. Y de paso
se corrigio `tracksVivoGrabando()`: decia `!guiando`, o sea "no hay guiado" = "esta grabando",
**que no es lo mismo** (con el GPS apagado o sin fijacion tampoco se graba, y `tracksTick` sale
antes de escribir). Ahora replica las mismas condiciones que `tracksTick`.

### ★ (F) Dos numeros mas de los comentarios
`tracks.cpp` decia "(son 120 KB)" cuando la zona de puntos son 80 KB, y `tracks.h` decia que antes
ponia "12288 puntos" cuando eran 12240. Corregidos.

### LO QUE APRENDIMOS DE ESTA REVISION (para la proxima vez)

1. ★★★ **`ERASEPAGE` BORRA LA PAGINA QUE *CONTIENE* LA DIRECCION, NO "DESDE" ELLA.** Toda
   direccion que se borre tiene que estar **ALINEADA a 4 KB**. No es opcional ni es un detalle:
   saltarselo rompio la grabacion a los 4 km de caminata, metio puntos en el Golfo de Guinea y
   borro las cabeceras de las ranuras. Si hace falta sitio para una cabecera, se le da **su propia
   pagina entera**; ahorrar 64 bytes sale carisimo. Ahora lo vigila `tools/prueba_mapa_tracks.py`.
2. ★★ **UN NUMERO CALCULADO EN DOS SITIOS ACABA DISCREPANDO.** El tope de puntos por ranura vivia
   en `tracks.h` (2040, mal calculado) y en `tracks.cpp` (2037, el que se aplicaba): el nodo
   anunciaba uno y aplicaba el otro, asi que el configurador mandaba un track que el nodo
   rechazaba. Ahora hay **uno solo**, y un `static_assert` impide que se separen.
3. ★★ **LA FLASH NO SE PUEDE ESCRIBIR DOS VECES.** NVMC solo pasa bits de 1 a 0. Antes de
   escribir una zona hay que **borrarla entera**, aunque "parezca" vacia: una segunda subida
   encima de una primera habria mezclado puntos viejos y nuevos.
4. ★ **LOS FALLOS DE "CUENTA DE PAGINAS" NO LOS CAZA NINGUN COMPROBADOR DE COMPORTAMIENTO.** Los
   dos peores de esta sesion (alineacion y borrado doble) se encontraron **contando**, no probando:
   ningun test de "¿navega bien?" los habria visto, porque el motor de navegacion era correcto.
5. ★ **CONGELAR EL CODIGO ANTES DE MANDARLO A REVISAR.** El primer revisor aviso de que los
   ficheros se editaron A MITAD de su revision, asi que su informe vale para unas versiones que ya
   no eran las ultimas. Se pierde parte del trabajo y hay que fiarse menos del resultado.
6. ★ **UN COMPROBADOR QUE DA ALARMAS FALSAS SE ACABA IGNORANDO.** El de disposicion de textos
   denuncio un solape que no existia (dos textos en ramas distintas) y, al mismo tiempo, **dejo
   pasar uno real** (el aviso de la flecha y el de "track demasiado" coincidian de verdad, porque
   el primero se pinta FUERA del if/else). La regla que quedo: **no se declara nada como
   "excluyente"**; se comparan todos con todos y cada texto tiene que tener su sitio de verdad.

---

## ARREGLADO EN LA PRIMERA REVISION

### 1. La pantalla de guiado se cerraba sola a los 15 segundos — GRAVE
El auto-cierre por inactividad es una regla de MENU, y se habia heredado a una pantalla de
TRABAJO. Vas andando, la pantalla iba bien, y a los 15 s sin tocar nada volvia el carrusel. Para
recuperarla hacian falta cuatro gestos. Y lo peor: **el guiado NO se paraba**, asi que el aparato
seguia guiando sin ensenarlo.

Arreglado: la pantalla de guiado **no se cierra por tiempo** en ninguna de las tres puertas del
boton. Se sale cuando el usuario quiere (pulsacion larga). El auto-cierre se queda para la lista y
la pantalla de accion, que si son menus.

### 2. La escala del dibujo ignoraba el ALTO — GRAVE (fallo de bulto)
`altoM` se calculaba **y no se usaba en ninguna parte**: la escala salia solo del ancho. Como los
puntos que no caben se RECORTAN al borde (no se encogen), cualquier ruta que se extendiera en
vertical se convertia en **dos rayas pegadas a los cantos** del recuadro. Es decir: practicamente
cualquier ruta de montana de un dia.

Arreglado: metros por pixel = el MAYOR de (ancho/anchoDib, alto/altoDib), que garantiza que quepa
en las dos direcciones. Comprobado con `tools/prueba_encuadre_track.py`: 8 rutas (de 1,5 a 50 km,
rectas y retorcidas) **0 recortes** y usando el 99-100% del recuadro.

### 3. Con mas de 500 m de desviacion el aparato se quedaba mudo — GRAVE (conceptual)
Ponia "MUY LEJOS", **quitaba la flecha** y ademas sustituia los metros por ese texto. O sea: en el
unico momento en que de verdad hace falta saber hacia donde tirar (te has desviado, hay niebla,
se cierra la senda), el aparato no decia ni cuanto ni hacia donde.

Arreglado: la flecha **se sigue pintando**, pero apuntando DE VUELTA al punto mas cercano de la
ruta, y los metros de desviacion se dicen SIEMPRE. Es lo contrario de "no pintar una flecha
falsa": aqui la flecha es verdadera (calculada desde tu posicion real al punto de la ruta).

### 4. El track en vivo no se podia empezar ni borrar desde ningun sitio — GRAVE
`tracksVivoOlvida()` solo se llamaba internamente, y al arrancar el nodo "rescataba" el track
anterior. Consecuencia real: el nodo grababa desde que cogia posicion en casa, y al llegar al
monte **"Volver a casa" te mandaba a tu casa** (el punto mas antiguo del anillo), no al coche
donde dejaste el nodo. Sin forma de arreglarlo desde el aparato.

Arreglado: fila nueva **"Empezar nuevo"** en la seccion Tracks, que tira el track en vivo y
empieza uno desde donde estas. Va como accion de menu (`ACT_TRK_NUEVO`) para **reutilizar el
mecanismo de confirmacion** de las acciones destructivas: no puede pasar a la primera pulsacion.
Y si se estaba guiando, se para el guiado (apuntaba al track viejo, que acaba de desaparecer).

### 5. Un texto se salia del panel por 2 px (lo cazo el comprobador al medir el peor caso)
`EMPAREJAR: ------` son 17 caracteres y a escala 2 el tope son 16 (202 px de 200). Corregido a 5
guiones. Y de paso el comprobador de textos ahora mide **lo que se construye en tiempo de
ejecucion** (`snprintf` + `drawText`) y **la escala real** con la que se pinta, que era el hueco
por donde se colaba.

---

## PENDIENTE (decidir con el operador)

### A. Elegir "Hacia adelante" arranca el guiado con UNA pulsacion — MEDIO
Estando en la pantalla de accion, una pulsacion corta ya empieza a guiar. Lo caro no es eso: es
que **arrancar el guiado pausa la grabacion del track en vivo**, y no se reanuda hasta hacer
"Finalizar guiado". Un usuario curioseando el menu puede pasarse la caminata sin grabar nada.
**Decidir:** ¿pedir confirmacion, o avisar en la pantalla de guiado de que la grabacion esta
pausada? (Lo segundo es mas util: el aviso haria falta igual.)

### B. "Volver a casa" no avisa si el principio del anillo ya se perdio — MEDIO
`tracksVivoDioLaVuelta()` existe y solo lo usa el configurador web. Si el anillo ha dado la vuelta
(con el nodo grabando en casa a 1 punto/60 s se llena en ~5,7 dias), "Volver a casa" guia hacia
un punto cualquiera del recorrido **con toda su confianza**. Ademas, tras un reinicio la garantia
no existe: `gVivoVueltas` se pone a 0 a proposito porque no se puede saber.
**Decidir:** avisar en la pantalla de guiado cuando `tracksVivoDioLaVuelta()` sea cierto, y decir
en el menu que tras un reinicio el inicio no esta garantizado.

### C. "Volver a casa" abre una lista en vez de llevar a casa — MEDIO
El rotulo promete un atajo y entrega el mismo menu de siempre. **Decidir:** ¿que abra
directamente "Hacia atras" sobre el track en vivo?

### D. La fecha no lleva el ano, y el vivo no se distingue de una ranura — MENOR
Seis filas con el mismo formato "12/07 18:42". Un track del ano pasado es indistinguible de uno
de esta manana, y el vivo no se puede etiquetar (los puntos no caben a escala 2).

### E. "FALTAN X km" es longitud de RUTA, no distancia en linea recta — MENOR
Es el dato correcto para seguir una ruta, pero quien lo lee como "cuanto me queda para llegar"
se desconcierta si la ruta da una vuelta a un barranco. **Decidir:** ¿rotularlo "(ruta)"?

### F. Numeros equivocados en los comentarios — MENOR (documentacion)
`tracks.h` dice "120 KB dan para 12.240 puntos" y "1 punto/4 s", que son restos de una version
anterior: son 8160 puntos y la cadencia real es 10 m / 60 s. `tracks.cpp` dice "32 KB con 8
paginas": son 20 KB y 5 paginas. **El codigo cumple la especificacion; los comentarios no.**

---

## SOLO SE PUEDE JUZGAR CON EL APARATO EN LA MANO

1. Cuanto tarda el usuario en notar que algo ha cambiado y cuanto en reaccionar.
2. Si la linea del track se lee de verdad con los fantasmas que el operador acepta (guiones 3/3,
   mas la flecha encima, mas los restos de la pantalla anterior).
3. Si el rumbo del GPS es utilizable a pie, y si el aparato oscila entre flecha y "ANDA UNOS
   PASOS" alrededor del umbral de 1 km/h.
4. Si el repintado se dispara cuando toca (la huella usa lat/lon a 5 decimales, ~1 m).
5. El consumo con la pantalla de guiado repintandose a menudo y el GPS encendido.
