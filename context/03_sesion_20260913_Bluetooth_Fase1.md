# Sesión 2026-09-13 (bis) — FASE 1: por qué el Bluetooth enlazado rompe el nodo

**Estado**: propuesta. NO se ha tocado ni un fichero del firmware ni el hardware.
**Tarea única de la sesión**: averiguar por qué enlazar el código de Bluetooth
(Bluefruit/SoftDevice) rompe el nodo, y arreglarlo.

Se han leído, en este orden: `cerebro.md` §4, §5 y las entradas (39), (40) y (41);
`GUIA_AGENTE.md`; `docs/USO_DEL_AIRE.md`.

---

## 1. Lo que he comprobado leyendo código (no son teorías: son ficheros)

### 1.0 MEDIDO EN LA PLACA (2026-09-13, nodo en modo bootloader)

`INFO_UF2.TXT` de la unidad NICENANO (leído, **no se copió ningún UF2**):

```
UF2 Bootloader 0.9.2 lib/nrfx (v2.0.0) lib/tinyusb (0.12.0-145-g9775e7691)
Model: nice!nano
Board-ID: nRF52840-nicenano
Date: Jul 19 2024
SoftDevice: S140 6.1.1
```

**El SoftDevice SÍ está grabado, y es exactamente el S140 6.1.1 que pide nuestro
build** (`sd_fwid 0x00B6`). Por tanto la hipótesis «falta el SoftDevice» (que era
la principal de esta sesión) **queda descartada con prueba**. También queda
descartado el empaquetado DFU: nuestro `firmware.zip` ya pide `softdevice_req:
[182] = 0x00B6`, o sea el correcto.

También comprobado en el `.elf` actual: TinyUSB **sí** está compilado
(`TinyUSB_Device_Init`, `usb_device_task`, `power_event_handler`,
`nrfx_power_usbevt_init` presentes), así que el camino
`usb_softdevice_pre_enable()` / `sd_softdevice_enable()` del núcleo está activo.

---

### 1.1 La pista del operador, revisada: la definición de placa NO es la culpable

Comparado **nuestro** `boards/faketec-nrf52840.json` con el del proyecto que sí
funciona con Bluetooth en **esta misma placa**
(`C:\NavaTastic Codigo completo\boards\promicro-nrf52840.json`):

| Campo | Nuestro | NavaTastic |
|---|---|---|
| `build.arduino.ldscript` | `nrf52840_s140_v6.ld` | `nrf52840_s140_v6.ld` |
| `build.extra_flags` | `-DARDUINO_NRF52840_FEATHER -DNRF52840_XXAA` | **idéntico** |
| `build.softdevice.sd_name/sd_version/sd_fwid` | `s140` / `6.1.1` / `0x00B6` | **idéntico** |
| `build.bootloader.settings_addr` | `0xFF000` | **idéntico** |
| `connectivity` | `["bluetooth"]` | **idéntico** |
| `upload.maximum_size` / `maximum_ram_size` | `815104` / `248832` | **idéntico** |

Y su variante de placa (`variants/nrf52840/diy/nrf52_promicro_diy_tcxo/variant.h`)
usa **exactamente la misma elección de reloj que la nuestra**:

```c
// #define USE_LFXO // Board uses 32khz crystal for LF
#define USE_LFRC    // Board uses RC for LF      <-- igual que variants/faketec/variant.h
```

Su `variant.cpp` es también el mismo patrón (rail 3V3 P0.13 HIGH en `initVariant`,
botón P1.00 con SENSE_LOW en el apagado).

**Conclusión**: no falta nada en «reloj / arranque / tabla de vectores / RAM» que
se pueda arreglar en `variants/faketec/`. La hipótesis que quedó escrita en (41)
como «siguiente paso» **queda descartada por comparación directa**. El problema es
otro y está una capa más abajo.

### 1.2 El punto exacto donde muere el nodo: `Bluefruit.begin()`

Del registro de viaje volcado la noche del fallo (`log_20260913_025917.txt`, líneas
1503-1517):

```
2026-09-13 00:58:18 EVT ble 10/11: latch SET (GPREGRET=0x7B)
2026-09-13 00:58:18 EVT ble 1/11: init del stack Bluetooth
2026-09-13 00:58:18 BLE init: bleEnabled=true, arrancando Bluetooth
2026-09-13 00:58:18 EVT ble 2/11: Bluefruit.begin()      <-- último renglón
1s EVT boot v0.3.0-dev mode=2                            <-- el nodo se REINICIÓ
```

La marca 2/11 se escribe **justo antes** de llamar a `Bluefruit.begin()` (líneas
325-326 de `src/ble_kiss.cpp.off`). Nunca aparece la 3/11. La línea siguiente es
un arranque nuevo. **El nodo se cae y se reinicia exactamente ahí**, en las dos
ocasiones registradas. Y en el arranque siguiente, cuando el pestillo saltaba el
Bluetooth, el nodo **sí** balizaba normalmente (`TX STS`, `TX TRK`, `TX TLM` en
las líneas 1523-1526). Eso encaja con todo lo que vio el operador: USB que
desaparece, pantalla negra y, con el tiempo, el nodo que «ni arranca».

### 1.3 Corrección importante a (40): el «build de rescate» NO apagaba el Bluetooth

En (40) se apuntó que un build con `bleInit()` retornando inmediato seguía
dejando el USB muerto, y de ahí salió la conclusión «basta con enlazarlo». Leyendo
`ble_kiss.cpp.off` + `main.cpp` se ve que **ese build seguía arrancando el
Bluetooth igualmente**:

- `bleInit()` retornaba pronto (línea 563), sí; pero
- `bleLoop()` (línea 598) hace `if (gCfg->bleEnabled != gEnabled) bleApply(...)`
  y `gEnabled` empieza en `false` mientras la configuración guardada tenía
  `bleEnabled=true` → **`bleApply(true)` → `bleStartStack()` → `Bluefruit.begin()`**.

Es decir: **todos** los builds que fallaron llamaron a `Bluefruit.begin()`. La
frase «el fallo aparece solo con el código enlazado» no está demostrada; lo que
está demostrado es que **todos los que fallaron llamaron a `Bluefruit.begin()`**.

---

## 2. La hipótesis que tenía… y que la placa ha tumbado

*(Se deja escrito el razonamiento porque explica por qué se midió lo que se midió,
pero **el resultado de la medición lo desmiente**: ver 1.0.)*

`Bluefruit.begin()` lo primero que hace es `sd_softdevice_enable(&clock_cfg, ...)`
(`bluefruit.cpp:319`). Y `sd_softdevice_enable` **no es una función: es una
instrucción `SVC`** (`nrf_sdm.h:322` la declara con el macro `SVCALL`).

Una `SVC` no llama a ninguna función del programa: salta a **la dirección que haya
en la tabla de vectores** para el vector de SVC. Si el SoftDevice está grabado, el
MBR desvía la SVC al SoftDevice; si no lo estuviera, el vector apuntaría al
manejador de SVC de la aplicación — que en este proyecto es el de FreeRTOS
(`SVC_Handler` en 0x0006b110, desensamblado: restaura contexto y `bx lr`), una
rutina que da por hecho que la SVC significa «arranca la primera tarea».

**Pero el SoftDevice está** (1.0), así que esta explicación **no vale** y hay que
buscar la causa dentro de la propia llamada. Lo único que se sostiene de este
apartado es un dato comprobado: el núcleo de Adafruit **no** lleva ningún
desviador propio (`nrf_sdh*`, `svc_service_call`): buscado en todo
`framework-arduinoadafruitnrf52` y solo existen las cabeceras de la API.


---

## 3. Plan propuesto (Fase 1: nada se toca hasta que digas sí)

**Punto de partida tras la medición**: el SoftDevice está, el framework es el
oficial de Adafruit (con TinyUSB), la definición de placa es equivalente a la de
NavaTastic y la variante también. Ninguna de las explicaciones «de escritorio»
aguanta. Lo único que queda es **medir en la placa qué pasa dentro de
`Bluefruit.begin()`**, y para eso hace falta grabar. Propuesta: **dos builds de
diagnóstico pequeños, aislados y con rescate ya probado**.

### Experimento 1 — «¿arranca el Bluetooth a solas en esta placa?»

Proyecto aparte (no toca `src/` ni el firmware bueno) con un `main.cpp` mínimo:
SOLO `Serial.begin` + `Bluefruit.begin(1,0)` + `Bluefruit.setName` +
`Bluefruit.Advertising.start(0)`. Ni radio, ni GPS, ni OLED, ni sensores, ni
nuestro registro. Anuncia un nombre inconfundible: **`BLE-TEST-FAKETEC`**.

- **Si funciona** → el problema está en NUESTRO código o en el orden de arranque,
  no en el framework ni en el SoftDevice. Y además el móvil/PC lo vería anunciado
  (aunque desde este PC no se puede auditar Bluetooth: haría falta el navegador
  con Web Bluetooth).
- **Si también se cae** → el problema está en la combinación
  framework-oficial + TinyUSB + `Bluefruit.begin()` en esta placa, y ya no
  perdemos más tiempo mirando nuestro firmware.

### Experimento 2 — «¿es la escritura en flash de justo antes?»

El mismo proyecto mínimo, pero haciendo `Bluefruit.begin()` **después de una
escritura en flash igual que la nuestra** (la marca 2/11 del registro de viaje).
Es la única diferencia real entre nuestro arranque y el de NavaTastic: nosotros
escribimos en flash inmediatamente antes de arrancar el Bluetooth, y ellos no.

### Cómo leeremos el resultado aunque el nodo se caiga

Los dos builds llevan un **diario a prueba de reinicio**: un bloque en RAM que
**sobrevive al reset** (`__attribute__((section(".noinit")))`) donde se anota cada
paso. Al arrancar de nuevo, el nodo lo imprime por USB:

```
BLE-DIAG last=<paso donde murió> reboots=<n> hf=<si hubo HardFault>
         pc=<dirección que falló> cfsr=<registro de fallo>
```

Más un código de LED visible sin cables:
`1 parpadeo` = arrancó el bucle · `2` = se cayó ANTES de Bluefruit.begin ·
`3` = se cayó DENTRO · `4` = Bluetooth arriba y anunciando.

**Alcance honesto**: el manejador de HardFault que captura `pc`/`cfsr` solo
funciona si la caída es un HardFault clásico. Si es un *assert* del SoftDevice
(el núcleo de Adafruit lo convierte en un bucle infinito a propósito), veremos
«se cayó dentro de Bluefruit.begin() en el paso N» pero no la dirección. En ese
caso el siguiente paso sería bisecar el `Bluefruit.begin()` llamada a llamada.

### Rescate (ya está probado: anoche se hizo tres veces)

1. Doble toque al reset → unidad `NICENANO`.
2. Copiar `.pio\build\faketec_sx1262_433\firmware.uf2` (el build bueno SIN
   Bluetooth, que sigue intacto en `.pio`) → esperar.
3. Si el puerto serie está mudo: desenchufar y enchufar; si sigue mudo, corte de
   alimentación completo (batería incluida).

Nada de esto se ejecuta hasta que digas que sí.

---

## 4. Lo que NO voy a hacer

- No voy a renombrar los `.off` ni a tocar `main.cpp`, `power.cpp`, `tnc.cpp`,
  `diag.cpp`, `sensors.cpp` ni `display.cpp` en esta fase.
- No voy a grabar en el nodo hasta que digas que sí.
- No voy a presentar como hecho nada que solo sea deducción: el punto 1 está leído
  en ficheros o medido en la placa, y va marcado cuál es cuál; el punto 2 está
  marcado expresamente como deducción **y de hecho ya ha quedado desmentido en su
  parte principal** (el SoftDevice sí está).

