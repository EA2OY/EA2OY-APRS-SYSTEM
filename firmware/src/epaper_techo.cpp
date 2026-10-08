// epaper_techo.cpp - Pantalla de tinta electronica del LilyGO T-Echo / T-Echo Plus.
//
// QUE ES: el driver de la pantalla GDEH0154D67 (controlador SSD1681, 200x200) que llevan
// el T-Echo y el T-Echo Plus, integrado en el firmware completo (radio, GPS, sensores,
// config, registro, USB).
//
// ============================================================================
//  ESTA VERSION ES UNA REESCRITURA DELIBERADAMENTE MINIMA (2026-09-14)
//
//  El fichero anterior llevaba 1386 lineas con TODOS los experimentos que se
//  hicieron para averiguar por que la pantalla no pintaba: bit-bang, pruebas de
//  pines, "recetas" A/B/C/D/E, lectura del registro de estado 0x2F, ciclos de
//  corriente... Todo eso ya dio su respuesta y NO TIENE NADA QUE HACER en el
//  producto final: sobraba codigo, tocaba pines a mano con el periferico ya
//  configurado y era la causa mas probable del cuelgue del arranque.
//
//  Aqui queda SOLO el camino que esta demostrado que pinta en esta unidad:
//  el mismo que el banco de pruebas src/hello_techo.cpp, que ya escribio un
//  "hello world" en este panel.
// ============================================================================
//
// TRES COSAS QUE HAY QUE ENTENDER DE ESTA PANTALLA (si no, se falla):
//   1. Refresco completo = ~2 SEGUNDOS (parpadea en negro y blanco). Por eso NO se
//      puede repintar a cada pulsacion como en la OLED: se dibuja en memoria y se
//      manda al panel SOLO cuando algo cambia de verdad.
//   2. Si se hacen muchos refrescos parciales seguidos la imagen se ensucia
//      (fantasmas); el parcial queda DESACTIVADO a proposito hasta validar el completo.
//   3. El controlador se duerme (0x10 0x01) y conserva la imagen: la tinta electronica
//      es bistable, o sea que lo que se ve puede ser de un refresco viejo.
//
// ----------------------------------------------------------------------------
//  QUE HABIA MAL EN LA VERSION ANTERIOR, EN UNA LINEA CADA COSA
// ----------------------------------------------------------------------------
//  (a) nrfx_spim_xfer() CON MANEJADOR NULL ESPERA SIN TOPE. Su implementacion
//      (nrfx_spim.c, linea 598) es literalmente
//          while (!nrf_spim_event_check(p_spim, NRF_SPIM_EVENT_END)){}
//      Si el periferico no arranca (deshabilitado, sin reloj, con el pin robado),
//      ESA ESPERA NO TERMINA NUNCA y el firmware se queda mudo: el puerto USB
//      existe, pero no imprime ni obedece. Es exactamente el sintoma que teniamos.
//      -> AQUI NO SE USA nrfx_spim_xfer: se programan los registros del periferico
//         y se sondea EVENT_END **CON TOPE DE TIEMPO**. Si vence, se ABORTA la
//         transferencia (TASKS_STOP + disable) y se sigue vivo.
//
//  (b) LA BATERIA SE MIDE EN EL MISMO PIN QUE EL RELOJ DE LA PANTALLA.
//      src/sensors.cpp hace analogRead(31) cada 2 s y el SCK del panel es P0.31.
//      analogRead() reconfigura ese pin como entrada ANALOGICA, o sea que DESENGANCHA
//      el SCK del periferico SPIM2. A partir de ahi el periferico no puede dar reloj:
//      la transferencia se queda esperando su evento y (con nrfx) cuelga el nodo.
//      -> AQUI se reconfigura el periferico (pines + PSEL) ANTES DE CADA COMANDO.
//         Cuesta microsegundos y deja el driver a prueba de que alguien le toque los pines.
//
//  (c) epdLeerEstado() / epdPruebaPines() / bit-bang ponian los pines EN MODO GPIO A MANO
//      con el periferico SPIM2 ya inicializado. Eso deja el periferico y los pines en un
//      estado que no es el que espera el controlador.
//      -> ELIMINADAS. No queda ni una linea de bit-bang ni de prueba de pines.
//
//  (d) Cuatro inicializaciones del periferico (displayInit, displayInitTrasRadio,
//      displayArrancaPantalla...) con nrfx_spim_uninit() + nrfx_spim_init() cada vez.
//      El valor que devolvia (0x0BAD0000, que es NRFX_ERROR_BASE_NUM con los bits
//      bajos del enum perdidos) era sintoma de ese ir y venir de estado.
//      -> UNA SOLA vez, idempotente, y NO se hace uninit nunca.
//
//  (e) P1.11 (PIN_EPD_PWR) se empujaba a 3,3 V. El firmware de fabrica NO lo toca
//      (esta comentado en su epaper_init) y Meshtastic lo deja en bajo. Empujarlo
//      puede meter corriente por donde no toca.
//      -> NO SE TOCA. Se deja como entrada.
//
// PINES Y SECUENCIA: sacados del firmware de cfr34k (que corre en esta placa) y de la
// tabla oficial de LilyGO. Ver docs/HARDWARE_TECHO.md.
//
// License: GPL-3.0

#include "display.h"

#if !HAS_OLED

#include <Arduino.h>
#include <math.h>       // isfinite(): comprobacion de la posicion a guardar
#include <nrfx_spim.h>
#include <stdio.h>
#include <string.h>     // strcmp/strncpy/strchr: el fichero ya los usaba sin incluirlo

#include "epd_font5x7.h"
// â˜…â˜… LA FUENTE DIN 10pt, DE ANCHO VARIABLE (2026-09-22) â˜…â˜…
//   Es la que trae el firmware aleman (licencia MIT, aviso de copyright dentro del fichero). Se
//   usa para el TERCER tamano de letra (escala 0): tiene la misma altura que la escala 2 (14 px)
//   pero es mas estrecha, que es justo lo que hacia falta para que los titulos luzcan sin que los
//   textos largos se salgan del panel. Ver epd_font_din10.h.
#include "epd_font_din10.h"
#include "flog.h"
#include "gps.h"
#include "haptic.h"
#include "pins_board.h"
// â˜… Solo para buttonUltimoToqueConfirmado(): el SELLO de cuando se confirmo el ultimo toque.
//   Es lo unico que el driver puede saber del boton. NO se usan aqui las funciones de la cola
//   de toques (buttonTouchPoll / buttonTouchPending): esa cola es del bucle, y quitarsela de
//   las manos fue el fallo del b77/b78. Ver el comentario largo del aplazamiento, mas abajo.
#include "button.h"
#include "power.h"
#include "radio.h"
#include "aprs.h"
#include "cli.h"
#include "diag.h"       // diagTrazaTaller() / diagTrazaArranque(): las trazas del USB
#include "sensors.h"
#include "store.h"
#include "tnc.h"
#include "tracker.h"
#include "tracks.h"     // modulo de tracks: solo existe en el T-Echo (ver tracks.h)
#include "tracks_sesiones.h"   // ★ las SALIDAS: partir el registro en tracks (2026-10-06)
#include <RadioLib.h>

namespace {

// ---------------------------------------------------------------- la pantalla
constexpr int EPD_W = 200;
constexpr int EPD_H = 200;
constexpr int EPD_STRIDE = EPD_W / 8;              // 25 bytes por fila
constexpr int EPD_BUFSZ = EPD_STRIDE * EPD_H;      // 5000 bytes

// Pines (ver pins_techo.h). El numero de Arduino ya es el de GPIO.
constexpr int PIN_SCK = PIN_EPD_SCK;    // P0.31
constexpr int PIN_MOSI = PIN_EPD_MOSI;  // P0.29 (SDI)
constexpr int PIN_CS = PIN_EPD_CS;      // P0.30
constexpr int PIN_DC = PIN_EPD_DC;      // P0.28
constexpr int PIN_RST = PIN_EPD_RST;    // P0.02
constexpr int PIN_BUSY = PIN_EPD_BUSY;  // P0.03

// El framebuffer: 1 bit por punto. Bit a 1 = BLANCO (el panel es "1 = blanco"),
// bit a 0 = NEGRO.
uint8_t gBuf[EPD_BUFSZ];

// â˜…â˜… EL PLANO ANTERIOR: LA CLAVE DEL REFRESCO PARCIAL (Paso 1, 2026-09-15) â˜…â˜…
//
// El SSD1681 tiene DOS memorias de imagen: la "actual" (comando 0x24) y la "anterior"
// (comando 0x26). Al refrescar, el controlador LAS COMPARA y solo mueve los pixeles que
// han cambiado. Mandando las dos memorias iguales (lo que se hacia antes) la comparacion
// sale "nada ha cambiado" y el panel hace el refresco completo con su parpadeo de ~2 s.
//
// Mandando la imagen VIEJA en 0x26 y la NUEVA en 0x24, el controlador solo toca lo que
// cambia: eso es el refresco parcial, y por eso NO parpadea.
//
// `gPrevValido` dice si `gBufPrev` contiene de verdad lo que hay pintado en el panel. Al
// arrancar NO se sabe (la tinta es bistable y puede haber cualquier cosa de un refresco
// viejo), asi que el primer refresco de cada arranque es COMPLETO a proposito.
uint8_t gBufPrev[EPD_BUFSZ];
bool gPrevValido = false;

// â˜… REFRESCO COMPLETO FORZADO (quita los fantasmas del parcial).
//   `kForzarCompleto` lo pide quien sabe que la imagen va a quedar sucia (por ejemplo,
//   volver de la pantalla de prueba o de la lista de escenas).
bool gForzarCompleto = false;

// ===========================================================================
//  â˜…â˜… REFRESCO CON VENTANA (2026-09-21): MENOS BYTES, MENOS RATO SIN MIRAR EL BOTON â˜…â˜…
//
//  QUE HACIA ANTES: en CADA refresco se mandaban las 200 filas de los dos planos = 10.000
//  bytes, y con el transporte por bit-bang eso son ~80-125 ms de bucle cerrado. Cambiar un
//  digito de la bateria costaba exactamente lo mismo que cambiar la pantalla entera.
//
//  QUE HACE AHORA: se comparan los dos planos EN MEMORIA, se saca el rectangulo que de
//  verdad cambia y se manda SOLO eso (ver `EpdVentana`). El panel sigue pintando con su
//  onda y su tiempo (eso no se toca: es fisica), pero el nodo deja de mover bytes que no
//  han cambiado y, sobre todo, deja de estar ciego tanto rato.
//
//  â˜… `gVentanaOn` es un INTERRUPTOR: se puede apagar en caliente desde el cable (`epdventana
//    0`) y el driver vuelve al camino de siempre, el que esta probado en placa. Si algo sale
//    mal en la prueba, se apaga y ya esta: no hay que volver a grabar.
//  â˜…â˜… APAGADA POR DEFECTO, Y ESTO ES UNA VUELTA ATRAS MEDIDA (2026-09-21) â˜…â˜…
//
//  Se compilo con la ventana ENCENDIDA (b69) y **LA PANTALLA SE QUEDO SIN IMAGEN** en la placa
//  del operador. El tÃ¡ctil tampoco respondia, y las dos cosas van juntas: el unico sitio donde
//  se atiende el tactil durante un repintado es el bombeo del driver (ver ePDBombea), asi que
//  si el refresco se atasca, el tactil se queda mudo con el. Es el MISMO cuadro que ya vimos al
//  bajar las esperas del panel: cuando se toca el camino del refresco, se cae todo junto.
//
//  â˜… POR QUE FALLA, LEYENDO A LOS QUE LO TIENEN PROBADO: ni el firmware de referencia (cfr34k)
//    ni GxEPD2 recortan la ventana de RAM. Los dos escriben SIEMPRE la ventana completa
//    200x200 (comandos 0x44/0x45 con 0..24 y 0..199). El de referencia ademas fuerza un
//    refresco COMPLETO cada hora. O sea que el panel se maneja con la ventana entera y el
//    ahorro de bytes se busca por otro lado, no recortando la ventana. Nuestro driver hacia
//    exactamente eso (0..24 y 0..199 clavados) y funcionaba.
//
//  SE QUEDA EL CODIGO, PERO APAGADO: sirve para probarlo con la placa delante y con medida
//  (comando `epdventana 1` por el cable), y la vuelta atras es instantanea. Lo que NO se hace
//  es dejarlo encendido en un firmware que va a la mano del operador.
bool gVentanaOn = false;
// Si el rectangulo sucio es mas grande que esto (en filas x ancho), no compensa: se manda
// la pantalla entera como antes. 600 celdas ~ una cuarta parte del panel.
constexpr uint32_t kMaxCeldasVentana = 600;

// Contadores para poder MEDIR el ahorro y ver si la ventana trabaja (comando `epd` / `pantalla`).
uint32_t gVentanasHechas = 0;        // refrescos que se hicieron con ventana
uint32_t gRefrescosVacios = 0;       // refrescos que se saltaron (ni un byte distinto)
uint32_t gCeldasUltimaVentana = 0;   // filas x ancho del ultimo rectangulo
uint32_t gBytesUltimoRefresco = 0;   // bytes que se mandaron al panel en el ultimo refresco
int gVentanaUltima[4] = {0, 0, 0, 0};  // x0, x1, y0, y1 del ultimo rectangulo

bool gReady = false;      // el panel ha recibido su secuencia de arranque
bool gDirty = false;      // hay algo dibujado sin mandar
bool gAsleep = false;     // el panel esta en deep sleep
bool gSpiListo = false;   // el periferico SPIM2 esta configurado en nuestros pines

// ------------------------------------------------------------------- SPI
// â˜…â˜… SPIM2, CON INSTANCIA PROPIA DE nrfx, Y NUNCA SPIM3 (2026-09-14) â˜…â˜…
//
// En este core el objeto global `SPI` de Arduino vive en **SPIM3**
// (libraries/SPI/SPI.cpp: `#define _SPI_DEV NRF_SPIM3`, y la variante del T-Echo
// declara SPI_INTERFACES_COUNT 1, asi que `SPI1` ni existe). La radio usa ese `SPI`.
// SPIM2 es OTRO periferico, esta libre y su bloque de hardware NO es el mismo que
// TWIM0/TWIM1 (esos son SPIM0/SPIM1), asi que no choca con el I2C de los sensores.
//    -> La pantalla se queda con SPIM2 y la radio con SPIM3. No se pelean.
//
// SPIM0 queda PROHIBIDO: su bloque de hardware es el mismo que TWIM0 y habilitarlo
// colgo el nodo (ya probado, ver el historial del proyecto).
// P1.06. La pantalla del T-Echo NO lee nada (es solo escritura), pero el periferico
// pide un pin de MISO de todas formas. Se configura y no se usa.
constexpr int EPD_MISO_PIN = 38;

// Tope de seguridad por transferencia. A 4 MHz, 1 byte tarda 2 us, asi que 5000 bytes
// tardan ~10 ms. 60 ms es margen de sobra y evita cualquier espera eterna. Ademas es
// CORTO a proposito: aunque TODAS las transferencias fallaran, el refresco entero
// costaria menos de un segundo y el nodo no se quedaria nunca mudo.
constexpr uint32_t EPD_XFER_TIMEOUT_MS = 60;

// Diagnostico (lo enseÃ±a el comando "epd" del USB).
uint32_t gErrTimeout = 0;     // transferencias abortadas por tope de tiempo (0 = bien)
uint32_t gErrSpi = 0;         // errores varios del periferico
uint32_t gMsUltimo = 0;       // lo que tardo el ultimo refresco (completo o parcial)
uint32_t gMsUltimoCompleto = 0;  // el ultimo COMPLETO (para comparar)
uint32_t gMsUltimoParcial = 0;   // el ultimo PARCIAL (0 = todavia no ha habido ninguno)
uint32_t gMsAntesDePintar = 0;   // lo NUESTRO de este refresco: hasta mandar el "a pintar" (b75)
uint32_t gMsCompletoMs = 0;      // millis() en que EMPEZO el ultimo refresco COMPLETO
bool gHuboCompleto = false;      // Â¿ha habido algun completo? (ver epdTocaCompleto)
uint32_t gBusyTimeouts = 0;      // veces que BUSY no solto dentro del tope (0 = BUSY informa)
uint32_t gBusyAvisos = 0;        // refrescos en los que BUSY estaba ALTO (el panel informa)
uint32_t gNCompletos = 0;     // refrescos completos hechos
uint32_t gNParciales = 0;     // refrescos parciales hechos
uint32_t gNParcialesSeguidos = 0;  // parciales desde el ultimo completo (fantasmas)
uint32_t gCmdActual = 0;      // comando que se esta mandando ahora (para el diagnostico)
uint32_t gCmdTimeout = 0;     // el PRIMER comando que se atasco (carta blanca del fallo)
uint32_t gFaseTimeout = 0;    // 1 = se atasco el BYTE de comando; 2 = los DATOS
uint32_t gErrSeguidos = 0;    // transferencias falladas desde la ultima que salio bien
uint32_t gErrSeguidosMax = 0; // el peor tramo seguido
uint32_t gFaseActual = 0;     // que parte de la transferencia se esta mandando
uint32_t gTxVistoEndtx = 0;   // transferencias que terminaron por EVENTS_ENDTX
uint32_t gTxVistoEnd = 0;     // ... por EVENTS_END
uint32_t gTxVistoAmount = 0;  // ... por TXD.AMOUNT == 0 (red de seguridad)
uint32_t gBytesBitBang = 0;   // bytes que ha movido el bit-bang (prueba de que trabaja)

inline void epdCs(bool activo) { digitalWrite(PIN_CS, activo ? LOW : HIGH); }
inline void epdDc(bool datos)  { digitalWrite(PIN_DC, datos ? HIGH : LOW); }

// ============================================================================
//  â˜…â˜… DOS TRANSPORTES, Y EL QUE SE USA ES EL BIT-BANG (2026-09-14) â˜…â˜…
//
//  El camino del periferico SPIM2 esta AQUI AL LADO, entero y con diagnostico, pero
//  NO se usa para pintar. Motivo, medido en esta placa con el comando "epdsonda":
//
//      SPIM2->ENABLE = 1, PSEL.SCK = P0.31, PSEL.MOSI = P0.29, FREQ = 4 MHz
//      (o sea: la configuracion esta bien y se deja escribir)
//
//      pero al arrancar una transferencia de 5000 bytes:
//        TASKS_START ............ no produce EVENTS_STARTED
//        TASKS_STOP / SUSPEND ... no producen EVENTS_STOPPED
//        TXD.AMOUNT ............. se queda clavado en 1 y no baja
//        EVENTS_ENDTX / END ..... no suben nunca
//
//  Un periferico que NO OBEDECE A SUS PROPIAS TAREAS esta muerto: no es cuestion de
//  eventos ni de esperas, es que no arranca. (Y es justo el blocaje que describia el
//  proyecto: el periferico de la pantalla se queda tomado/apagado en esta integracion).
//
//  â˜… Y ESTO ES LO IMPORTANTE: el bit-bang SI funciona en esta placa. El encargo lo da
//    por descartado ("con PSEL apuntando a un pin lo gobierna el periferico y
//    digitalWrite() no hace nada"), pero esa conclusion se saco cuando el bit-bang
//    todavia estaba mal por otras dos razones que se descubrieron DESPUES:
//      - mandaba un byte de mas en 0x11, 0x44 y 0x45 (LEN() cuenta el comando)
//      - el framebuffer iba con el orden de bits equivocado
//    Con las dos cosas corregidas, el banco de pruebas pinto el "hello world" con
//    bit-bang (su comando "tag 0"), y el pin no lo gobierna SPIM2 porque SPIM2 esta
//    muerto: nadie mueve esos pines.
//
//  Se deja seleccionable en caliente (gTransporte) para poder comparar los dos desde el
//  USB con el comando "epdtransporte 0|1".
// ============================================================================

// 0 = bit-bang por GPIO (el que se usa), 1 = periferico SPIM2
int gTransporte = 0;

// ------------------------------------------------------------------- bit-bang
// Medio periodo del reloj en microsegundos. 1 us => ~500 kHz: TODAVIA MUY SOBRADO para el
// SSD1681 (aguanta hasta ~10 MHz), y ya es el doble de rapido que antes. La ganancia se nota
// al pulsar un boton o moverse por el menu, donde el trasvasaje de los dos planos (10.000
// bytes) dominaba el tiempo (~0,8 s). Con la escritura combinada de SCK+MOSI de abajo, el
// trasvase baja a ~0,1-0,2 s. Se deja 1 us a proposito (no menos) por integridad de seÃ±al.
constexpr uint32_t EPD_BIT_US = 1;

// Escribe en el puerto 0 o 1 segun el pin. Los numeros de Arduino de esta placa son
// directos: N = P0.N y 32+N = P1.N (lo dice variants/techo/variant.h).
inline void epdGpioEscribe(int pin, bool alto) {
  NRF_GPIO_Type *p = (pin < 32) ? NRF_P0 : NRF_P1;
  const uint32_t b = 1u << (pin & 31);
  if (alto) p->OUTSET = b; else p->OUTCLR = b;
}

// â˜…â˜… RECONFIGURACION POR "LEER-MODIFICAR-ESCRIBIR", QUE ES LO QUE HACE FALTA â˜…â˜…
//
// El primer intento reconfiguraba los pines con `nrf_gpio_cfg()` / `pinMode()`, que
// ESCRIBEN TODO el registro PIN_CNF. Eso tiene un efecto lateral grave en este firmware:
// entre que se borra la configuracion vieja y se escribe la nueva, el pin se queda un
// instante como ENTRADA, y con el reloj de la pantalla flotando en mitad de una
// transferencia se pierden flancos (o se cuelga la conversacion con el panel).
//
// Aqui solo se tocan los bits que hacen falta (DIR, INPUT, DRIVE) y, sobre todo, no se
// pasa nunca por un estado en el que el pin no este gobernado. Ademas se deja el buffer
// de entrada CONECTADO en los pines que empujamos, para poder LEERLOS: es lo que permite
// comprobar en el propio chip que el bit-bang esta moviendo de verdad el pin.
inline void epdPinSalidaFuerte(int pin) {
  NRF_GPIO_Type *g = (pin < 32) ? NRF_P0 : NRF_P1;
  const uint32_t b = pin & 31;
  // DIR=salida (bit0), INPUT=conectado (bit1), PULL=ninguna (bits2-3),
  // DRIVE=H0H1 alta corriente (bits8-10), SENSE=desconectado (bits16-17)
  g->PIN_CNF[b] = (1u << 0) | (1u << 1) | (3u << 8);
}

// Saca un byte, MSB primero, con el flanco de subida en medio (el panel lee ahi).
// â˜… ACELERACION (2026-09-15): SCK (P0.31) y MOSI (P0.29) estan los DOS en el puerto 0,
//   asi que se escriben con un solo acceso a OUTSET/OUTCLR combinando las mascaras. Antes
//   cada bit hacia 3 llamadas de GPIO + 3 delayMicroseconds; ahora 4 escrituras de registro
//   y UNA espera corta por bit. Es lo que hace que cambiar de escena o moverse por el menu
//   responda notablemente mas rapido (el trasvase de los dos planos dominaba el refresco).
//   El `delayMicroseconds(EPD_BIT_US)` (1 us) mantiene un ritmo ~500 kHz, de sobra dentro
//   del limite del SSD1681 y sin arriesgar la integridad de la seÃ±al.
#define EPD_MSK_SCK  (1u << 31)
#define EPD_MSK_MOSI (1u << 29)
inline void epdBitBangByte(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    const uint32_t m = (b & (1u << i)) ? EPD_MSK_MOSI : 0u;
    NRF_P0->OUTCLR = EPD_MSK_SCK | EPD_MSK_MOSI;   // sck y mosi bajos (filo de bajada)
    NRF_P0->OUTSET = m;                             // pongo el dato en MOSI
    NRF_P0->OUTSET = EPD_MSK_SCK;                    // subo SCK (filo de subida: se lee)
    delayMicroseconds(EPD_BIT_US);
    NRF_P0->OUTCLR = EPD_MSK_SCK;                    // bajo SCK
  }
}

// Deja los pines de la pantalla como SALIDAS DE ALTA CORRIENTE, que es como los pone
// GxEPD2 y el propio core ("el SPI a 8 MHz necesita salidas de alta corriente").
//
// â˜…â˜…â˜… AQUI ESTABA EL FALLO QUE FALTABA, Y ES SUTIL (2026-09-14) â˜…â˜…â˜…
//
// **UN PERIFERICO CON `PSEL` APUNTANDO A UN PIN SE QUEDA ESE PIN, AUNQUE ESTE
// DESHABILITADO.** No basta con `nrf_spim_disable()`: mientras `PSEL.SCK` sea 0x1F
// (P0.31), el GPIO **no** puede gobernar ese pin y todas las lecturas dan 0. Es
// exactamente la trampa que el proyecto ya tenia documentada ("un pin con PSEL
// apuntandole lo gobierna el periferico, no el GPIO") pero que aqui se colaba por otra
// puerta: este firmware llama a `epdSpiConfigura()` (que apunta PSEL a P0.31/P0.29) al
// arrancar, y despues el bit-bang intentaba mandar en esos mismos pines. No mandaba.
//
// Medido en esta unidad, y por eso se sabe:
//     con PSEL apuntando a P0.31 -> "SCK=0/00"  (parecia sujeto a masa)
//     con PSEL desconectado      -> "SCK=0/01"  (se gobierna perfectamente)
//
// Por eso, en modo bit-bang, **se APAGA y se DESENGANCHA el periferico** antes de tocar
// los pines. Es lo que hace el banco de pruebas que pinta (nunca inicializa SPIM2 cuando
// va por bit-bang) y es la diferencia que lo explica todo.
#define EPD_PSEL_DESCONECTADO 0x80000000u

// â˜…â˜… EL RELOJ SE DEJA EN BAJO (MODO 0) â˜…â˜…
// En modo 0 el reloj reposa en BAJO y el panel lee en el flanco de SUBIDA: arrancando en
// alto, el primer flanco que ve el panel es de bajada y pierde el primer bit de cada byte.
// El banco de pruebas pone `SCK=0` explicitamente antes de empezar; aqui se hace igual.
void epdPinesBitBang() {
  // 1) el periferico fuera: deshabilitado Y con PSEL desconectado.
  NRF_SPIM2->ENABLE = 0;
  NRF_SPIM2->PSEL.SCK = EPD_PSEL_DESCONECTADO;
  NRF_SPIM2->PSEL.MOSI = EPD_PSEL_DESCONECTADO;
  NRF_SPIM2->PSEL.MISO = EPD_PSEL_DESCONECTADO;

  // 2) y ahora si, los pines son nuestros.
  epdPinSalidaFuerte(PIN_CS);
  epdPinSalidaFuerte(PIN_DC);
  epdPinSalidaFuerte(PIN_SCK);
  epdPinSalidaFuerte(PIN_MOSI);
  nrf_gpio_pin_set(PIN_CS);      // en reposo el CS esta ALTO
  nrf_gpio_pin_set(PIN_DC);
  nrf_gpio_pin_clear(PIN_SCK);   // modo 0: el reloj en reposo, BAJO
  nrf_gpio_pin_clear(PIN_MOSI);
  gErrSeguidos = 0;
}

// Deja el reset como entrada con subida (es como lo deja el firmware de fabrica tras
// soltarlo) y BUSY como entrada.
void epdPinesReposo() {
  pinMode(PIN_RST, INPUT_PULLUP);
  pinMode(PIN_BUSY, INPUT);
}

// ------------------------------------------------------------------- SPIM2 (sin usar)
// Configura el periferico SPIM2 en nuestros pines. Se deja por si algun dia se
// averigua por que sus tareas no arrancan en esta integracion (ver la nota de arriba).
void epdSpiConfigura() {
  // â˜…â˜… UN SOSPECHOSO MAS, Y ES FUERTE (2026-09-14): EL SAADC â˜…â˜…
  //
  // src/sensors.cpp mide la bateria con `analogRead(31)`, o sea el canal AIN7, que es
  // **P0.31: el SCK DE LA PANTALLA**. Y en el T-Echo eso es un error, porque su bateria
  // esta en P0.04 (lo dicen variants/techo/variant.h y src/pins_techo.h, y el propio
  // firmware lo confirma: `bat` contesta 0.00V en vez de la tension real).
  //
  // El driver del ADC (analogRead_internal) deja CH[0].PSELP apuntando a ese pin y ASI SE
  // QUEDA (leido en el registro: `CH0.PSELP=8` = AIN7) aunque el ADC quede deshabilitado.
  // Con la entrada analogica enganchada a un pin que ADEMAS empujamos como salida digital,
  // el pin no puede subir (medido: PIN_CNF correcto, OUT a 1, y el pin clavado en 0), y
  // sin reloj no hay conversacion posible con el panel.
  //
  // Aqui se suelta ese canal antes de configurar los pines de la pantalla, antes de cada
  // comando. No se rompe nada nuevo: la lectura de bateria de esta placa ya estaba mal.
  NRF_SAADC->ENABLE = 0;
  NRF_SAADC->CH[0].PSELP = SAADC_CH_PSELP_PSELP_NC;
  NRF_SAADC->CH[0].PSELN = SAADC_CH_PSELP_PSELP_NC;

  nrf_gpio_cfg(PIN_SCK,
               NRF_GPIO_PIN_DIR_OUTPUT,
               NRF_GPIO_PIN_INPUT_CONNECT,
               NRF_GPIO_PIN_NOPULL,
               NRF_GPIO_PIN_H0H1,
               NRF_GPIO_PIN_NOSENSE);
  nrf_gpio_pin_clear(PIN_SCK);

  nrf_gpio_cfg(PIN_MOSI,
               NRF_GPIO_PIN_DIR_OUTPUT,
               NRF_GPIO_PIN_INPUT_DISCONNECT,
               NRF_GPIO_PIN_NOPULL,
               NRF_GPIO_PIN_H0H1,
               NRF_GPIO_PIN_NOSENSE);
  nrf_gpio_pin_clear(PIN_MOSI);

  NRF_SPIM2->PSEL.SCK = PIN_SCK;
  NRF_SPIM2->PSEL.MOSI = PIN_MOSI;
  NRF_SPIM2->PSEL.MISO = EPD_MISO_PIN;

  if (!gSpiListo) {
    nrfx_spim_config_t cfg = {};
    cfg.sck_pin      = (uint32_t)PIN_SCK;
    cfg.mosi_pin     = (uint32_t)PIN_MOSI;
    cfg.miso_pin     = (uint32_t)EPD_MISO_PIN;
    cfg.ss_pin       = NRFX_SPIM_PIN_NOT_USED;   // el CS lo gobierno yo a mano
    cfg.frequency    = NRF_SPIM_FREQ_4M;
    cfg.mode         = NRF_SPIM_MODE_0;
    cfg.bit_order    = NRF_SPIM_BIT_ORDER_MSB_FIRST;
    cfg.orc          = 0xFF;
    cfg.irq_priority = 3;
    static nrfx_spim_t spim = NRFX_SPIM_INSTANCE(2);
    // El valor de retorno de nrfx_spim_init NO es fiable en este core (devuelve
    // 0x0BAD0000, que es la BASE de los codigos de nrfx y no un error real). Lo que
    // manda es que los registros queden bien, y eso se comprueba justo debajo.
    if (nrfx_spim_init(&spim, &cfg, NULL, NULL) != NRFX_SUCCESS) gErrSpi++;
    nrf_spim_enable(NRF_SPIM2);
    gSpiListo = true;
  }

  if (NRF_SPIM2->PSEL.SCK != (uint32_t)PIN_SCK ||
      NRF_SPIM2->PSEL.MOSI != (uint32_t)PIN_MOSI ||
      (NRF_SPIM2->ENABLE & 1u) == 0) {
    gErrSpi++;
    NRF_SPIM2->PSEL.SCK = PIN_SCK;
    NRF_SPIM2->PSEL.MOSI = PIN_MOSI;
    nrf_spim_enable(NRF_SPIM2);
  }
}

// â˜…â˜… T-ECHO PROJECT BUTTER: EL GANCHO DEL BOTON (2026-09-15) â˜…â˜…
//
// El refresco de este panel BLOQUEA: desde que se manda 0x20 hasta que la tinta se ha
// movido pasan 0,35-2 s de espera, y antes eso era `delay()` a secas. En ese rato la
// maquina de gestos del boton no corria, asi que un toque que caia al principio de un
// refresco se resolvia (y se atendia) un refresco ENTERO mas tarde. Con el gancho, esas
// esperas se aprovechan para bombear el boton: los flancos ya los coge la interrupcion
// (button.cpp), pero aqui ademas se resuelven los plazos vencidos y el gesto queda
// ENCOLADO, listo para que el bucle lo ejecute en cuanto el panel quede libre.
//
// â˜… El gancho NO pinta y NO ejecuta acciones (lo pone main.cpp): solo encola. Entrar a
//   pintar desde dentro de un pintado seria un lio; ejecutar una baliza, tambien.
//   Se declara AQUI ARRIBA, antes de la primera funcion del driver que lo usa.
//   â˜… `displaySetPumpBoton()`, que es quien lo pone, se define FUERA del espacio de
//     nombres anonimo (mas abajo, junto a las sondas de taller).
static void (*gPumpBoton)(void) = nullptr;
inline void ePDBombea() { if (gPumpBoton) gPumpBoton(); }

// â˜… TRANSFERENCIA POR EL PERIFERICO, CON TOPE DE TIEMPO. NO se usa para pintar (ver la
//   nota de arriba), pero se deja entera: si algun dia se arregla, basta con poner
//   gTransporte = 1. El tope es lo que impide que el firmware se quede mudo.
bool epdXferSpim(const uint8_t *tx, size_t n) {
  if (n == 0) return true;
  if (tx == nullptr) return false;
  const uint32_t dir = (uint32_t)(uintptr_t)tx;
  if (dir < 0x20000000u || dir >= 0x20040000u) { gErrSpi++; return false; }  // no es RAM

  NRF_SPIM2->TXD.PTR = dir;
  NRF_SPIM2->TXD.MAXCNT = (uint32_t)n;
  NRF_SPIM2->RXD.PTR = 0;
  NRF_SPIM2->RXD.MAXCNT = 0;
  NRF_SPIM2->EVENTS_END = 0;
  NRF_SPIM2->EVENTS_ENDTX = 0;
  NRF_SPIM2->EVENTS_STARTED = 0;
  NRF_SPIM2->TASKS_START = 1;

  const uint32_t t0 = millis();
  for (;;) {
    if (NRF_SPIM2->EVENTS_ENDTX) { gTxVistoEndtx++; break; }
    if (NRF_SPIM2->EVENTS_END)   { gTxVistoEnd++;   break; }
    if (NRF_SPIM2->TXD.AMOUNT == 0) { gTxVistoAmount++; break; }
    if (millis() - t0 > EPD_XFER_TIMEOUT_MS) {
      NRF_SPIM2->TASKS_STOP = 1;
      NRF_SPIM2->EVENTS_END = 0;
      NRF_SPIM2->EVENTS_ENDTX = 0;
      NRF_SPIM2->EVENTS_STOPPED = 0;
      nrf_spim_disable(NRF_SPIM2);
      nrf_spim_enable(NRF_SPIM2);
      gErrTimeout++;
      if (gCmdTimeout == 0) { gCmdTimeout = gCmdActual; gFaseTimeout = gFaseActual; }
      gErrSeguidos++;
      if (gErrSeguidos > gErrSeguidosMax) gErrSeguidosMax = gErrSeguidos;
      return false;
    }
  }
  NRF_SPIM2->EVENTS_END = 0;
  NRF_SPIM2->EVENTS_ENDTX = 0;
  gErrSeguidos = 0;
  return true;
}

// La transferencia que se usa de verdad: elige el transporte.
bool epdXfer(const uint8_t *tx, size_t n) {
  if (n == 0) return true;
  if (tx == nullptr) return false;
  if (gTransporte == 1) return epdXferSpim(tx, n);
  for (size_t i = 0; i < n; i++) epdBitBangByte(tx[i]);
  gBytesBitBang += (uint32_t)n;
  gErrSeguidos = 0;
  return true;
}

// Comando con sus datos, con el CS bajo todo el rato (igual que el firmware de fabrica:
// su send_command() lo deja bajo y lo sube al terminar).
void epdCmdData(uint8_t cmd, const uint8_t *datos, size_t n) {
  // â˜… T-ECHO PROJECT BUTTER: entre comando y comando se bombea el boton. El trasvase
  //   de un plano (5.000 bytes por bit-bang) son ~80-125 ms de bucle cerrado: es la
  //   ventana ciega mas larga que queda, y asi se parte en trozos mas cortos.
  ePDBombea();
  gCmdActual = cmd;
  // En modo bit-bang NO hace falta reconfigurar nada antes de cada comando: nadie le
  // quita los pines al GPIO (SPIM2 esta desenganchado) y el ADC de la bateria ya no usa
  // P0.31 (ver sensors.cpp). En modo SPIM2 si se reengancha, por si acaso.
  if (gTransporte == 1) epdSpiConfigura();
  epdCs(true);
  epdDc(false);
  gFaseActual = 1;
  epdXfer(&cmd, 1);
  if (n) {
    epdDc(true);
    gFaseActual = 2;
    epdXfer(datos, n);
  }
  epdCs(false);
}

void epdCmd(uint8_t cmd) { epdCmdData(cmd, nullptr, 0); }

void epdCmd1(uint8_t cmd, uint8_t d) { epdCmdData(cmd, &d, 1); }

void epdCmd2(uint8_t cmd, uint8_t d1, uint8_t d2) {
  const uint8_t d[2] = {d1, d2};
  epdCmdData(cmd, d, 2);
}

void epdCmd3(uint8_t cmd, uint8_t d1, uint8_t d2, uint8_t d3) {
  const uint8_t d[3] = {d1, d2, d3};
  epdCmdData(cmd, d, 3);
}

void epdCmd4(uint8_t cmd, uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4) {
  const uint8_t d[4] = {d1, d2, d3, d4};
  epdCmdData(cmd, d, 4);
}

// El framebuffer entero (5000 bytes) en una sola conversacion con el panel: CS bajo,
// el comando, y los datos.
void epdCmdBuf(uint8_t cmd, const uint8_t *buf, size_t n) {
  gCmdActual = cmd;
  if (gTransporte == 1) epdSpiConfigura();
  epdCs(true);
  epdDc(false);
  gFaseActual = 1;
  epdXfer(&cmd, 1);
  epdDc(true);
  gFaseActual = 2;
  epdXfer(buf, n);
  epdCs(false);
}

// ------------------------------------------------------------------ arranque
// El reset, COPIADO del firmware que funciona (cfr34k):
//   RST a 0 (reset activo) -> 20 ms -> RST a 1 -> se SUELTA (entrada con pull-up).
// Sueltan el pin a proposito: el panel tiene que ver un flanco de subida limpio.
void epdReset() {
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW);
  delay(20);
  digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_RST, INPUT_PULLUP);   // se suelta
  delay(20);
}

// ---------------------------------------------------------------------------
//  â˜…â˜… PASO 1: REFRESCO PARCIAL â€” LAS DOS FORMAS DE REFRESCAR (2026-09-15) â˜…â˜…
//
//  Patron copiado del firmware de referencia que funciona en esta placa
//  (firm_ref_techo/t-echo-lora-aprs-main/src/epaper.c, FULL_UPDATE_SEQUENCE y
//  PARTIAL_UPDATE_SEQUENCE). La secuencia de comandos es LA MISMA en los dos casos y solo
//  cambian dos valores:
//
//    COMPLETO  ->  0x3C = 0x05 (borde)  y  0x22 = 0xF7 (secuencia de refresco)  ~2,7 s, parpadea
//    PARCIAL   ->  0x3C = 0x80 (borde)  y  0x22 = 0xFF (secuencia de refresco)  ~0,4 s, NO parpadea
//
//  Y lo que de verdad hace que el parcial no parpadee no son esos dos bytes: es mandar los
//  DOS PLANOS distintos (0x26 = lo que habia pintado, 0x24 = lo que se quiere pintar). El
//  controlador compara y solo mueve los pixeles que cambian. Mandando los dos planos
//  iguales (lo que se hacia antes) el controlador cree que no hay nada que hacer y se
//  comporta como un completo.
//
//  `kMaxParcialesSeguidos` y `kMaxMsSinCompleto` fuerzan un COMPLETO de vez en cuando para
//  quitar los fantasmas que deja el parcial. El de referencia lo hace cada 60 minutos
//  (main.c, `m_epaper_force_full_refresh`); aqui ademas se pone un tope por numero de
//  parciales, que es lo que protege cuando la pantalla se repinta muy seguido.
//  720 parciales es una red de seguridad pensada para el peor caso: si algo que se dibuja
//  cambia cada pocos segundos (por ejemplo un contador de trafico en la escena de radio), el
//  refresco puede caer cada 5 s, y 720 son 60 minutos a ese ritmo. El repintado de refresco
//  (`kRepintadoMaxMs`) va aparte y no depende de este contador.
// ---------------------------------------------------------------------------
constexpr uint8_t kOndaBordeCompleto = 0x05;
constexpr uint8_t kOndaBordeParcial  = 0x80;
constexpr uint8_t kSecuenciaCompleto = 0xF7;
constexpr uint8_t kSecuenciaParcial  = 0xFF;

// â˜… TOPE DE PARCIALES SEGUIDOS: BAJADO DE 720 A 20 (2026-09-21).
//   Medido por otros (especificacion del panel y Meshtastic, que fuerza un completo cada 20
//   refrescos rapidos): el parcial deja FANTASMAS (restos de la imagen anterior) que se van
//   acumulando, y el completo es lo unico que los quita. 720 parciales seguidos entre dos
//   completos es muchisimo mas de lo que aguanta el panel: con el carrusel a un refresco por
//   minuto son doce horas sin limpiar. 20 es el numero que usa el que mas ha probado esto.
constexpr uint32_t kMaxParcialesSeguidos = 20;         // tope por numero de parciales
constexpr uint32_t kMaxMsSinCompleto = 60UL * 60UL * 1000UL;   // 60 min (como el de referencia)

// ---------------------------------------------------------------------------
//  â˜…â˜… P.6: LA ESPERA PREVIA DE BUSY, BAJADA DE 2.000 ms A 300 ms (2026-09-15) â˜…â˜…
//
//  Medido en hardware: el parcial pintaba y NO parpadeaba (confirmado a ojo por el
//  operador), pero tardaba 3.217 ms. La cuenta salia clavada:
//
//      2.000 ms (espera previa agotando su tope) + ~1.200 ms (secuencia del parcial)
//
//  O sea: **BUSY esta en ALTO justo antes de empezar** (el panel sale de su sueno profundo
//  con el pin arriba) y la espera previa se pasaba 2 segundos enteros esperando a que
//  bajara, sin que bajara. No hay ningun refresco solapado que evitar, y esos 2 s hacian
//  inviable el carrusel.
//
//  SE QUEDA UNA ESPERA, PERO CORTA: sigue habiendo red de seguridad (nunca una espera sin
//  salida) y si algun dia el panel contesta de verdad, se aprovecha.
//  â˜…â˜… VELOCIDAD (2026-09-21): los 300 ms de esta espera se han quitado, y aqui vivia la
//     constante que los ponia (`kEsperaPreviaBusyMs`). El motivo, medido en el codigo: en
//     ESTA unidad BUSY no informa NUNCA, asi que la espera no esperaba a nada: vencia el
//     tope y seguia. Se pagaban 300 ms en CADA refresco para nada. Ahora se sondea con un
//     tope corto (ver `kSondeoBusyRapidoMs` en epdRefresca): si BUSY informa, se aprovecha;
//     si no, se pierden milisegundos en vez de 300.
// ---------------------------------------------------------------------------

// Espera a que el panel suelte BUSY, CON TOPE. En ESTA unidad BUSY no informa, asi que
// esto NO es la espera principal: sirve para dos cosas:
//   (a) si el panel contesta, se aprovecha y se termina antes;
//   (b) si BUSY se queda clavado, se sale por el tope y el nodo sigue vivo (nunca se
//       vuelve al bucle sin salida de la version anterior).
bool epdEsperaBusy(uint32_t topeMs) {
  const uint32_t t0 = millis();
  while (digitalRead(PIN_BUSY) == HIGH) {
    if ((millis() - t0) > topeMs) {
      gBusyTimeouts++;
      return false;
    }
    ePDBombea();   // â˜… el boton se sigue mirando: este tope puede ser de 300-1500 ms
    delay(2);
  }
  return true;
}

// Tiempo que se espera al panel DESPUES de mandar 0x20 (a pintar).
//   * si BUSY se mueve: se espera a que baje, con tope (y si vence el tope, se sigue).
//   * si BUSY NO se mueve (lo medido en esta unidad): se espera a ciegas un tiempo prudente.
// En los dos casos el `delay()` final es lo que garantiza que el panel haya terminado antes
// de mandarlo a dormir (0x10 0x01): dormirlo a mitad de un refresco dejaria la imagen a
// medias. Es tiempo de la pantalla, no del nodo: la radio y el GPS siguen en el bucle.
//
// â˜… AQUI ESTABA LA MAYOR PARTE DE LA LATENCIA DE LOS TOQUES (T-Echo Project Butter,
//   2026-09-15): este `delay(minimoMs - gastado)` es UN TIRON DE 350 ms (parcial) o
//   2.000 ms (completo) sin mirar el boton. Ahora se espera lo mismo, pero en trozos de
//   2 ms y bombeando el boton en cada trozo: el panel tarda igual y el toque no se
//   queda esperando a que acabe.
void epdEsperaPintado(uint32_t minimoMs, uint32_t topeBusyMs) {
  const uint32_t t0 = millis();
  const bool busyInformo = epdEsperaBusy(topeBusyMs);
  // â˜… Si `epdEsperaBusy()` devuelve false es que encontro BUSY en ALTO (el panel estaba
  //   trabajando) y lo vio bajar: eso es la prueba DIRECTA de que en esta unidad BUSY
  //   informa. Si devuelve true, BUSY ya estaba bajo al mirar. Se cuentan las dos cosas.
  if (!busyInformo) gBusyAvisos++;
  while ((uint32_t)(millis() - t0) < minimoMs) {
    ePDBombea();
    delay(2);
  }
}

// La secuencia del SSD1681, byte a byte la de GxEPD2_154_D67 / cfr34k.
// OJO AL DETALLE QUE COSTO DOS DIAS: en su tabla LEN() CUENTA EL BYTE DE COMANDO.
//   LEN(1) = comando y nada mas      -> epdCmd()
//   LEN(2) = comando + 1 dato        -> epdCmd1()
//   LEN(3) = comando + 2 datos       -> epdCmd2()
//   LEN(4) = comando + 3 datos       -> epdCmd3()
// Mandar un byte de mas deja la ventana de RAM mal configurada y el panel no arranca.
//
// â˜… El PARCIAL usa esta MISMA secuencia y solo cambia la onda de borde (0x3C), que es
//   exactamente lo que hace el de referencia: alli las dos tablas son identicas salvo
//   0x3C y 0x22. Tambien necesita su reset hardware, y lo tiene (epdReset()).
void epdInitPanel(bool parcial) {
  epdReset();

  epdCmd(0x12);                       // soft reset
  delay(10);
  epdCmd3(0x01, 0xC7, 0x00, 0x00);    // driver output control
  epdCmd1(0x3C, parcial ? kOndaBordeParcial : kOndaBordeCompleto);  // border waveform
  epdCmd1(0x18, 0x80);                // sensor de temperatura interno
  epdCmd1(0x11, 0x03);                // entry mode: x e y incrementan
  epdCmd2(0x44, 0x00, (EPD_W - 1) / 8);                     // RAM x: 0..24
  epdCmd4(0x45, 0x00, 0x00, (EPD_H - 1) % 256, (EPD_H - 1) / 256);  // RAM y: 0..199
  epdCmd1(0x4E, 0x00);                // contador X = 0
  epdCmd2(0x4F, 0x00, 0x00);          // contador Y = 0
}

// El cuerpo comun de los dos refrescos: los dos planos y el "a pintar".
//   `bufAnterior` = lo que hay pintado; con nullptr se manda `bufActual` en los dos planos
//   (que es lo que se hacia en el completo de la version anterior).
//
// â˜…â˜… VENTANA DE RAM: POR QUE ES UN RECTANGULO Y NO "LO QUE CAMBIA" (2026-09-21) â˜…â˜…
//   El controlador recorre la ventana FILA A FILA y, al llegar al final de una fila, vuelve
//   SOLO al principio de la siguiente. O sea que si se le manda una fila a medias, lo que
//   venga detras se escribe DESPLAZADO y la imagen sale torcida. Por eso lo que se manda es
//   un RECTANGULO COMPLETO (todas las filas con el mismo ancho), y de cada fila se mandan
//   todos los bytes del ancho elegido: asi el salto de fila siempre cuadra.
//   Lo que SI se ahorra: las filas que no cambian no se mandan. Para un cambio de bateria
//   (3-4 filas de 25 bytes) son ~100 bytes por plano en vez de 5.000.
struct EpdVentana {
  int y0 = 0, y1 = -1;      // filas que se van a mandar
  int x0 = 0, x1 = -1;      // columnas de byte (0..24) del ancho comun
  uint32_t celdas = 0;      // filas x ancho (para decidir si compensa)
  bool vacia() const { return y1 < y0 || x1 < x0; }
};

// Rectangulo que cubre TODO lo que cambia entre lo pintado y lo nuevo. Se calcula sobre el
// formato real del bufer (bitidx = y*200 + x, o sea 25 bytes por fila), que es justo como
// direcciona el panel: x = columna de byte (0..24), y = fila (0..199).
EpdVentana epdVentanaSucia(const uint8_t *viejo, const uint8_t *nuevo) {
  EpdVentana v;
  int x0 = EPD_STRIDE, x1 = -1, y0 = -1, y1 = -1;
  for (int y = 0; y < EPD_H; y++) {
    const uint8_t *a = viejo + y * EPD_STRIDE;
    const uint8_t *b = nuevo + y * EPD_STRIDE;
    int f0 = -1, f1 = -1;
    for (int k = 0; k < EPD_STRIDE; k++) {
      if (a[k] != b[k]) { if (f0 < 0) f0 = k; f1 = k; }
    }
    if (f0 < 0) continue;            // esta fila no cambia: no se manda
    if (y0 < 0) y0 = y;
    y1 = y;
    if (f0 < x0) x0 = f0;
    if (f1 > x1) x1 = f1;
  }
  if (y0 < 0) return v;              // vacia: no hay ni un byte distinto
  v.y0 = y0; v.y1 = y1; v.x0 = x0; v.x1 = x1;
  v.celdas = (uint32_t)(y1 - y0 + 1) * (uint32_t)(x1 - x0 + 1);
  return v;
}

// Manda SOLO las filas de la ventana, el ancho completo de la ventana en cada una.
void epdCmdVentana(uint8_t cmd, const uint8_t *buf, const EpdVentana &v) {
  gCmdActual = cmd;
  if (gTransporte == 1) epdSpiConfigura();
  epdCs(true);
  epdDc(false);
  gFaseActual = 1;
  epdXfer(&cmd, 1);
  epdDc(true);
  gFaseActual = 2;
  const size_t ancho = (size_t)(v.x1 - v.x0 + 1);
  for (int y = v.y0; y <= v.y1; y++) {
    epdXfer(buf + (size_t)y * EPD_STRIDE + v.x0, ancho);
  }
  epdCs(false);
}

void epdRefresca(bool parcial, const uint8_t *bufAnterior, const uint8_t *bufActual) {
  const uint32_t t0 = millis();

  // â˜…â˜… LA ESPERA PREVIA A BUSY, ELIMINADA (b75, 2026-09-21) â˜…â˜…
  //
  // QUE HABIA AQUI: 300 ms de espera con tope, ANTES de inicializar el panel, en CADA
  // refresco (parcial y completo). Y esa espera no esperaba a nada:
  //   - se ejecuta cuando el panel esta DORMIDO (el refresco anterior lo mando a deep sleep),
  //     asi que no hay ningun trabajo suyo en curso que haya que dejar terminar;
  //   - y en esta unidad BUSY no informa NUNCA, asi que vencia el tope: 300 ms de reloj
  //     tirados a la basura, siempre, en cada repintado.
  //
  // â˜… POR QUE SE PUEDE QUITAR SIN MIEDO, Y ESTO ES LO QUE HAY QUE LEER: el firmware de
  //   referencia (cfr34k) **no tiene ninguna espera previa**. Su secuencia empieza por el
  //   reset, aplica la corriente al panel y espera **10 ms**, y sigue. Nosotros esperabamos
  //   300 ms antes de empezar a hablar. La diferencia entre un menu que responde en medio
  //   segundo y uno que responde en uno esta, casi toda, en estas dos esperas nuestras
  //   (esta y la de tensiones, ver mas abajo).
  //
  // â˜… OJO, Y ES IMPORTANTE NO CONFUNDIRSE: cuando esta espera se bajo a 12 ms y LA PANTALLA
  //   SE QUEDO EN NEGRO, el cambio no vino de aqui. Aquel build tocaba TRES cosas a la vez
  //   (esta espera, la de tensiones de 200 a 40 ms y el tiempo de pintado). Lo que se ha
  //   comprobado despues es que la de TENSIONES es la que no se puede tocar a la ligera: es
  //   la que garantiza que el panel tiene con que mover la tinta. Esta, en cambio, se ejecuta
  //   sobre un panel dormido y no protege de nada. Se quita SOLO esta.
  //
  // â˜… SI ALGUN DIA HAY QUE VOLVER ATRAS: poner otra vez
  //       epdEsperaBusy(300);
  //   en esta misma linea. Es una linea, y el porque esta escrito aqui.

  epdInitPanel(parcial);

  // â˜…â˜… (0) LA VENTANA: SOLO SE MANDAN LAS FILAS QUE CAMBIAN (2026-09-21) â˜…â˜…
  //   Se decide AQUI, despues de epdInitPanel() (que es quien deja la ventana en 200x200)
  //   y antes de mandar los datos. Si no compensa, se deja la ventana completa de siempre:
  //   el camino viejo sigue siendo el camino por defecto cuando el rectangulo es grande.
  EpdVentana vAnt, vAct;
  bool usarVentana = false;
  if (gVentanaOn) {
    // â˜…â˜… LA MISMA VENTANA PARA LOS DOS PLANOS (y esto no es un detalle) â˜…â˜…
    //   El controlador decide que pixel mover comparando las DOS memorias. Si se escribiera
    //   en el plano anterior solo la zona "que era sucia antes" y en el actual solo la "que
    //   es sucia ahora", las zonas que DEJARON de estar sucias se quedarian con el valor
    //   viejo en 0x26 y el panel las veria "iguales" y no las borraria: quedarian restos.
    //   Por eso el rectangulo es la UNION de las dos diferencias, y se manda el mismo a los
    //   dos planos. Se mandan unos bytes de mas en el plano anterior; a cambio, borrar sale
    //   bien, que es lo que de verdad se nota en la pantalla.
    vAct = epdVentanaSucia(bufAnterior ? bufAnterior : bufActual, bufActual);
    if (!vAct.vacia()) {
      vAnt = bufAnterior ? epdVentanaSucia(bufActual, bufAnterior) : vAct;
      // El rectangulo comun: union de los dos.
      if (vAnt.y0 < vAct.y0) vAct.y0 = vAnt.y0;
      if (vAnt.y1 > vAct.y1) vAct.y1 = vAnt.y1;
      if (vAnt.x0 < vAct.x0) vAct.x0 = vAnt.x0;
      if (vAnt.x1 > vAct.x1) vAct.x1 = vAnt.x1;
      vAct.celdas = (uint32_t)(vAct.y1 - vAct.y0 + 1) * (uint32_t)(vAct.x1 - vAct.x0 + 1);
      vAnt = vAct;
      // Compensa solo si el rectangulo es pequeno: si cambia media pantalla, el completo
      // de siempre hace menos comandos y no se queda peor.
      usarVentana = (vAct.celdas <= kMaxCeldasVentana);
      gCeldasUltimaVentana = vAct.celdas;
    } else {
      // Ni un byte distinto entre lo pintado y lo nuevo. â˜… NO SE SALE POR AQUI (2026-09-21):
      // la version que se grabo en la placa (b69) devolvia sin mas, y eso metia un camino
      // nuevo (salir de epdRefresca sin mandar nada ni refrescar `gBufPrev`) justo en el sitio
      // que se estaba tocando. Se cuenta y se sigue por el camino de siempre, que es el que
      // esta probado: si de verdad no hay nada que cambiar, el panel recibe lo mismo que ya
      // tiene y no se mueve ni un pixel.
      gRefrescosVacios++;
    }
  }
  if (usarVentana) {
    epdCmd2(0x44, (uint8_t)vAct.x0, (uint8_t)vAct.x1);             // RAM x: columnas de byte
    epdCmd4(0x45, (uint8_t)(vAct.y0 & 0xFF), (uint8_t)(vAct.y0 >> 8),
                  (uint8_t)(vAct.y1 & 0xFF), (uint8_t)(vAct.y1 >> 8));  // RAM y: filas
    epdCmd1(0x4E, (uint8_t)vAct.x0);                               // contador X = x0
    epdCmd2(0x4F, (uint8_t)(vAct.y0 & 0xFF), (uint8_t)(vAct.y0 >> 8));  // contador Y = y0
    gVentanasHechas++;
    gVentanaUltima[0] = vAct.x0; gVentanaUltima[1] = vAct.x1;
    gVentanaUltima[2] = vAct.y0; gVentanaUltima[3] = vAct.y1;
  }

  // (1) ENCENDER LAS TENSIONES DEL PANEL â€” â˜…â˜… SOLO EN EL REFRESCO COMPLETO (b84) â˜…â˜…
  //
  // QUE HABIA: `0x22 0xE0` + `0x20` y despues **200 ms de espera, en CADA refresco**, parcial
  // incluido. Esa espera no esperaba a nada (BUSY no informa en esta unidad), asi que eran
  // 200 ms de reloj tirados en cada toque. Pero quitarla a lo bruto dejo la pantalla en negro
  // (probado en placa), asi que llevaba un aviso escrito: "NO SE TOCA SIN MEDIRLO".
  //
  // â˜…â˜… AHORA YA ESTA MEDIDO, Y EL DATO VIENE DEL FIRMWARE DE REFERENCIA (cfr34k) â˜…â˜…
  //   Su secuencia PARCIAL **no manda este 0xE0**, y funciona. El motivo esta en su propia
  //   tabla: el que duerme al panel es `0x10 0x01` (deep sleep del CONTROLADOR), y eso **no
  //   apaga el elevador de tensiones del panel**. O sea que el elevador se queda levantado del
  //   refresco anterior, y por eso no hay que volver a levantarlo ni esperar a que suba.
  //   Nuestro comentario de antes decia "no consta que al despertar conserve las tensiones", y
  //   ahora SI consta: el de referencia lleva anos sin mandarlo en los parciales.
  //
  //   â˜… El COMPLETO si lo manda y si espera: ahi el panel acaba de arrancar de cero (o viene de
  //     un refresco en el que pudo perder las tensiones) y es donde la espera hace su trabajo.
  //     Ese es el caso que se probo y que se rompio al bajarlo: se queda como estaba.
  //
  //   AHORRO: 200 ms en cada refresco parcial, o sea en CADA toque del menu. Con el cambio del
  //   aplazamiento del b83, un toque suelto pasa de ~700-950 ms a ~350-500 ms.
  if (!parcial) {
    epdCmd1(0x22, 0xE0);   // copiado de `GxEPD2::_PowerOn()`
    epdCmd(0x20);          // y el chispazo que levanta el elevador
    // 200 ms a que las tensiones suban. Se bombea el boton mientras (ePDBombea).
    constexpr uint32_t kEsperaTensionesMs = 200;
    epdEsperaBusy(kEsperaTensionesMs);
  }

  // â˜…â˜… MEDIDA: CUANTO TARDAMOS NOSOTROS ANTES DE MANDAR EL "A PINTAR" (b75) â˜…â˜…
  //   Todo lo que hay hasta aqui es NUESTRO (inicializar el panel, levantar tensiones, mandar
  //   los bytes), no del panel. A partir de aqui empieza lo que tarda la tinta, que eso si es
  //   fisica. Este numero es el que hay que mirar para saber si de verdad hemos quitado la
  //   espera previa de 300 ms: antes valia ~630 ms, y con la espera fuera tiene que quedarse
  //   en ~330 ms. Sale por la traza de taller (`diag on`).
  gMsAntesDePintar = millis() - t0;

  // (2) LOS DOS PLANOS, EN EL ORDEN DEL DE REFERENCIA: 0x26 (anterior) y despues 0x24
  //     (actual). â˜… ESTE ORDEN ES EL QUE IMPORTA: antes se mandaba la misma imagen en los
  //     dos, que es justo lo que impide que el controlador detecte los cambios.
  const uint32_t fallosAntes = gErrTimeout + gErrSpi;
  const uint32_t amountAntes = gTxVistoAmount;
  const uint32_t bytesAntes = gBytesBitBang;
  if (usarVentana) {
    // Solo las filas del rectangulo. El plano anterior se manda con SU rectangulo (puede
    // ser distinto del de ahora) para que el controlador vea el cambio en los dos.
    epdCmdVentana(0x26, bufAnterior ? bufAnterior : bufActual, vAnt);
    epdCmdVentana(0x24, bufActual, vAct);
  } else {
    epdCmdBuf(0x26, bufAnterior ? bufAnterior : bufActual, EPD_BUFSZ);
    epdCmdBuf(0x24, bufActual, EPD_BUFSZ);
  }
  const uint32_t fallosAhora = (gErrTimeout + gErrSpi) - fallosAntes;
  gBytesUltimoRefresco = gBytesBitBang - bytesAntes;

  // â˜… TRAZA DE TALLER (2026-09-16): va agrupada en el MODO DIAGNOSTICO y se calla en modo
  //   TNC. Antes salia SIEMPRE, una por cada repintado (~1,5 s), y eso llenaba la consola
  //   del configurador y ensuciaba el puerto cuando el nodo trabaja de TNC. Ver diag.h.
  if (diagTrazaTaller()) {
    Serial.printf("PANTALLA: refresco %s: fallos=%lu amount0=%lu bytesBitBang=%lu transporte=%d "
                  "PSEL.SCK=0x%08lX CS=%d DC=%d SCK=%d MOSI=%d BUSY=%d\r\n",
                  parcial ? "PARCIAL" : "COMPLETO",
                  (unsigned long)fallosAhora,
                  (unsigned long)(gTxVistoAmount - amountAntes),
                  (unsigned long)gBytesUltimoRefresco, gTransporte,
                  (unsigned long)NRF_SPIM2->PSEL.SCK,
                  (int)digitalRead(PIN_CS), (int)digitalRead(PIN_DC),
                  (int)digitalRead(PIN_SCK), (int)digitalRead(PIN_MOSI),
                (int)digitalRead(PIN_BUSY));
    // â˜… VENTANA (2026-09-21): sin esta linea no hay forma de saber si el ahorro de bytes
    //   esta ocurriendo de verdad. ventana=si/no, celdas del rectangulo y sus limites.
    Serial.printf("PANTALLA: ventana=%s celdas=%lu x=%d..%d y=%d..%d bytes=%lu\r\n",
                  usarVentana ? "si" : "no",
                  (unsigned long)gCeldasUltimaVentana,
                  gVentanaUltima[0], gVentanaUltima[1], gVentanaUltima[2], gVentanaUltima[3],
                  (unsigned long)gBytesUltimoRefresco);
    // â˜… LA MEDIDA DE VELOCIDAD (b75): `nuestro` = lo que tardamos ANTES de que el panel
    //   empiece a mover la tinta (esperas + init + bytes). `total` = el refresco entero.
    //   La diferencia entre los dos es lo que tarda el panel, que eso no se puede tocar.
    Serial.printf("PANTALLA: tiempo nuestro=%lums  total=%lums  (el resto es del panel)\r\n",
                  (unsigned long)gMsAntesDePintar, (unsigned long)gMsUltimo);
  }

  // (3) Â¡A PINTAR! La unica diferencia de comandos entre completo y parcial.
  if (parcial) {
    epdCmd1(0x22, kSecuenciaParcial);
    epdCmd(0x20);
    epdEsperaPintado(350, 1500);   // parcial: ~0,4 s
  } else {
    epdCmd1(0x22, kSecuenciaCompleto);
    epdCmd(0x20);
    epdEsperaPintado(2000, 4000);  // completo: ~1,9-2,7 s
  }

  epdCmd1(0x10, 0x01);   // deep sleep: la imagen se queda (tinta bistable)
  gAsleep = true;

  const uint32_t ms = millis() - t0;
  gMsUltimo = ms;
  if (parcial) {
    gMsUltimoParcial = ms;
    gNParciales++;
    gNParcialesSeguidos++;
  } else {
    gMsUltimoCompleto = ms;
    gNCompletos++;
    gNParcialesSeguidos = 0;
    gMsCompletoMs = t0;   // cuando EMPEZO este completo (millis() al entrar en epdRefresca)
    gHuboCompleto = true;
  }

  // (4) El plano de ahora pasa a ser el "anterior" para el proximo refresco. Solo si los
  //     datos han viajado bien: si hubo transferencias abortadas, lo que hay en el panel NO
  //     es lo que dice `gBuf`, y hay que volver a pintar en COMPLETO para no arrastrar el
  //     error a todos los parciales siguientes.
  if (fallosAhora == 0) {
    memcpy(gBufPrev, gBuf, EPD_BUFSZ);
    gPrevValido = true;
  } else {
    gPrevValido = false;
  }
}

// Refresco COMPLETO con lo que haya en gBuf.
//
// â˜… NO SE ESPERA AL PIN BUSY A CIEGAS. Esta medido en ESTA unidad: BUSY no se mueve NUNCA
//   en los refrescos completos (0 altos en 39.565 muestras, con digitalRead, con el registro
//   IN y tambien con resistencia de subida) y la pantalla se refresca igual. Por eso la
//   espera es por TIEMPO (con el tope de BUSY solo como red de seguridad si algun dia
//   contestara). Un refresco completo real dura ~2 s.
void epdFullRefresh() {
  // â˜… OJO: aqui NO se comprueba `gSpiListo`. Esa bandera solo se pone en modo SPIM2, y en
  //   modo bit-bang se quedaba en false: el refresco salia por la puerta de atras sin
  //   mandar ni un byte (sintoma: "bytesBitBang=0" y "ultimoRefresco=0ms"). El estado que
  //   de verdad importa es que los pines esten preparados, y de eso se encarga
  //   `epdPinesBitBang()` / `epdSpiConfigura()`, que ya se han llamado antes.
  //
  //   El completo manda la MISMA imagen en los dos planos (`bufAnterior = nullptr`): asi el
  //   controlador no tiene nada que comparar y hace el barrido entero, que es lo que quita
  //   los fantasmas. Es tambien el refresco del arranque, cuando todavia no se sabe que hay
  //   pintado.
  epdRefresca(false, nullptr, gBuf);
  gForzarCompleto = false;
}

// â˜…â˜… REFRESCO PARCIAL: manda el plano VIEJO y el NUEVO y el controlador solo mueve lo que
//    cambia. Si no hay un plano anterior fiable (primer refresco tras el arranque, o un
//    refresco anterior que salio mal), cae al completo sin pensarlo.
void epdPartialRefresh() {
  if (!gPrevValido) { epdFullRefresh(); return; }
  epdRefresca(true, gBufPrev, gBuf);
}

// â˜…â˜… EL COMPLETO DE LIMPIEZA YA NO TE SALTA MIENTRAS NAVEGAS (b85) â˜…â˜…
//
// QUE PASABA: cada `kMaxParcialesSeguidos` (20) parciales, el driver metia un refresco
// COMPLETO para quitar los fantasmas que deja el parcial. Un completo son **~2,5 segundos** de
// pantalla. Moviendote por el menu, eso significa que **cada 20 toques te comes 2,5 segundos**,
// y siempre en el peor momento: en mitad de la navegacion.
//
// QUE SE HACE AHORA: el completo de limpieza SIGUE HACIENDOSE (los fantasmas hay que quitarlos,
// y el parcial los acumula), pero **se aplaza mientras el operador esta tocando**. En cuanto
// pasa el rato sin tocar, el refresco que toque sale completo y la pantalla se limpia.
//
// â˜… POR QUE ES SEGURO APLAZARLO: el parcial solo deja fantasmas DONDE PINTA, y solo pinta
//   cuando algo cambia. Si el operador esta tocando, esta cambiando cosas y acumulando
//   fantasmas... pero tambien los esta viendo, asi que no es una sorpresa. Y en cuanto para,
//   el siguiente repintado limpia. Lo que NO se hace es esperar indefinidamente: hay un tope
//   de parciales (`kMaxParcialesSinCompletoTope`) por si el operador no parase nunca.
//
// â˜…â˜… SE LEE EL SELLO DEL ULTIMO TOQUE, NO LA COLA DE ACCIONES. Es la misma regla que en el
//    aplazamiento del repintado: la cola de toques es del bucle y quitarsela de las manos dejo
//    el tactil muerto en el b77/b78. `buttonUltimoToqueConfirmado()` solo lee una marca de
//    tiempo y no se lleva nada por delante.
constexpr uint32_t kInteraccionRecienteMs = 3000;   // "esta tocando" = toco hace menos de esto
constexpr uint32_t kMaxParcialesSinCompletoTope = 60;   // tope duro: nunca mas de esto sin limpiar

bool epdTocaCompleto() {
  if (gForzarCompleto) return true;
  if (!gPrevValido) return true;

  // Â¿Esta el operador interactuando ahora mismo?
  const uint32_t ultimoToque = buttonUltimoToqueConfirmado();
  const bool interactuando =
      (ultimoToque != 0) &&
      ((uint32_t)(millis() - ultimoToque) < kInteraccionRecienteMs);

  if (gNParcialesSeguidos >= kMaxParcialesSeguidos) {
    // Toca limpiar. Si el operador esta tocando, se espera... salvo que ya se haya pasado del
    // tope duro, y entonces se limpia aunque moleste (mejor eso que una pantalla con fantasmas).
    if (!interactuando || gNParcialesSeguidos >= kMaxParcialesSinCompletoTope) {
      if (interactuando && diagTrazaTaller()) {
        Serial.printf("PANTALLA: completo de limpieza APLAZADO %lu parciales (tope duro)\r\n",
                      (unsigned long)gNParcialesSeguidos);
      }
      return true;
    }
    return false;   // se aplaza: el operador esta navegando
  }

  if (gNParcialesSeguidos > 0 && gHuboCompleto &&
      (millis() - gMsCompletoMs) > kMaxMsSinCompleto) return true;
  return false;
}

// Manda a la pantalla lo dibujado, si hace falta. Elige completo o parcial.
void epdFlush() {
  if (!gReady || !gDirty) return;
  gDirty = false;
  if (epdTocaCompleto()) {
    if (diagTrazaTaller()) {
      Serial.printf("PANTALLA: completo forzado (motivo: %s)\r\n",
                    gForzarCompleto ? "a peticion"
                    : (!gPrevValido ? "no hay plano anterior fiable"
                       : (gNParcialesSeguidos >= kMaxParcialesSeguidos ? "tope de parciales"
                          : "tope de tiempo")));
    }
    epdFullRefresh();
  } else {
    epdPartialRefresh();
  }
}

// ------------------------------------------------------------------ dibujo
// â˜…â˜… LA ORIENTACION (2026-09-14) â€” CONFIRMADA A OJO POR EL OPERADOR â˜…â˜…
//
// El contenido se dibuja en coordenadas "logicas" (x a la derecha, y hacia abajo, como en
// cualquier pantalla) y aqui se traduce a la trama que entiende el panel.
//
// â˜…â˜… QUE SIGNIFICA CADA NUMERO, Y POR QUE ES ASI â˜…â˜…
//
// El operador confirmo a ojo que la posicion de pie es **la 1**, y pidio que esa fuera la de
// fabrica. **El numero NO se ha renumerado a proposito**, y el motivo es importante:
//
//   La configuracion vive en la FLASH del nodo. Si se cambiara el significado de los
//   numeros, las unidades ya grabadas que tuvieran guardado un valor **se pondrian torcidas
//   solas** al actualizar el firmware (un `1` guardado pasaria de "de pie" a "girada 90").
//   Eso es una migracion silenciosa y es justo el tipo de trampa que hay que evitar.
//
// Asi que el significado se queda como esta (**1 = de pie, la de fabrica**) y la trampa se
// evita **diciendolo claro en la interfaz**: en el configurador web la opcion se llama
// "1 - de pie (por defecto)" y las otras "2 - girada 90", "3 - boca abajo (180)",
// "0 - girada 270". Ensenar el numero sin decir que significa es lo que confunde.
//
// La tabla de abajo traduce el numero a la transformacion de la trama. Las cuatro son
// rotaciones puras (ninguna espeja) y los `bitidx` del primer pixel salen distintos en las
// cuatro, comprobado en hardware:  R0=1206  R1=38208  R2=38391  R3=1791
int gRotacion = 1;   // 1 = DE PIE (posicion natural / de fabrica)
// La ultima rotacion que se llego a PINTAR. Sirve para dos cosas: (a) no repintar si la
// configuracion no ha cambiado, y (b) detectar el cambio en caliente cuando el usuario
// guarda `epdRotation` desde el configurador web o con `set epdRotation N`.
int gRotacionAplicada = -1;
// â˜… PRUEBA DE QUE EL VALOR LLEGA AL MAPA DE PIXELES (2026-09-14).
// No basta con enseÃ±ar la etiqueta ni la variable global: hay que demostrar que el numero
// que se USA dentro de `px()` cambia de verdad. Aqui se guarda, para las CUATRO primeras
// rotaciones que se pinten, el valor con el que se calculo el primer pixel. Si salieran
// dos iguales, el bug estaria aqui y no en la formula.
uint32_t gBitIdxR[4] = {0, 0, 0, 0};
bool gBitIdxHecho[4] = {false, false, false, false};

inline void px(int x, int y, bool negro) {
  if (x < 0 || y < 0 || x >= EPD_W || y >= EPD_H) return;
  int xr = x, yr = y;
  switch (gRotacion) {
    case 0:  break;                                           // 0 = girada 270
    case 2:  xr = EPD_W - 1 - x; yr = EPD_H - 1 - y; break;   // 2 = boca abajo (180)
    case 3:  xr = EPD_W - 1 - y; yr = x;             break;   // 3 = girada 90
    default: xr = y;             yr = EPD_H - 1 - x; break;   // 1 = DE PIE (natural)
  }
  // El bit 7 es el primero de cada byte.
  const uint32_t bitidx = (uint32_t)yr * EPD_W + (uint32_t)xr;
  // â˜… Aqui esta la prueba de que el valor llega: se guarda el bitidx del PRIMER pixel que
  //   se pinta con cada rotacion. Si dos rotaciones distintas guardaran el mismo numero,
  //   el fallo estaria en este punto y no en la formula.
  {
    const int r = (gRotacion >= 0 && gRotacion <= 3) ? gRotacion : 0;
    if (!gBitIdxHecho[r]) { gBitIdxHecho[r] = true; gBitIdxR[r] = bitidx; }
  }
  const int idx = (int)(bitidx >> 3);
  const uint8_t bit = (uint8_t)(0x80u >> (bitidx & 7u));
  if (negro) gBuf[idx] &= (uint8_t)~bit;   // 0 = negro
  else       gBuf[idx] |= bit;             // 1 = blanco
}

void clearBuf(bool blanco) {
  memset(gBuf, blanco ? 0xFF : 0x00, sizeof(gBuf));
}

void hLine(int x0, int x1, int y, int grosor = 1) {
  for (int g = 0; g < grosor; g++)
    for (int x = x0; x <= x1; x++) px(x, y + g, true);
}

// â˜… Traza de una LINEA entre dos puntos (Bresenham), con o sin guiones (2026-09-22).
//   La necesita la pantalla de guiado: la linea del track es lo que se mira al seguir una ruta,
//   y va con guiones para distinguir lo que QUEDA de lo ya recorrido (en blanco y negro, sin
//   colores, ese es el recurso que hay).
//   `guiones`: 0 = linea continua; N > 0 = pinta N pixeles y salta N (discontinua).
void trazaLinea(int x0, int y0, int x1, int y1, int guiones = 0) {
  const int dx = abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
  const int dy = -abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
  int err = dx + dy;
  int n = 0;
  for (;;) {
    // El guion se decide por la distancia recorrida, no por el pixel: asi los huecos son
    // regulares aunque la linea sea muy inclinada.
    if (guiones <= 0 || ((n / guiones) % 2) == 0) px(x0, y0, true);
    n++;
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// â˜…â˜…â˜… ESCALA 0: LA FUENTE DIN 10pt, EL TERCER TAMANO (2026-09-22) â˜…â˜…â˜…
//
// POR QUE: el operador hecho en falta un tamano de letra INTERMEDIO. La fuente de 5x7 solo se
// puede doblar (escala 1 = 7 px, escala 2 = 14 px) y no hay nada en medio. La DIN 10pt del
// firmware aleman tiene la MISMA altura de letra que la escala 2 (14 px) pero es de ANCHO
// VARIABLE, asi que ocupa bastante menos a lo ancho: es el tamano que faltaba.
//     "Finalizar guiado"   escala 2 = 190 px (NO CABE)   DIN 10 = 129 px
//
// â˜… SE USA CON `escala == 0`, y todas las funciones de texto lo entienden. Asi no hay que tocar
//   ninguna de las 167 llamadas que ya existen: las que quieran el tamano nuevo solo tienen que
//   poner 0 donde antes ponian 2.
//
// â˜…â˜… EL DATO QUE HAY QUE SABER ANTES DE TOCAR `dibujaGlyph`: LOS BITS NO VAN FILA A FILA â˜…â˜…
//   Llevan un contador CONTINUO por todo el caracter, y el byte avanza cada 8 bits. NO se
//   reinicia el byte en cada fila, que es lo que haria cualquiera. Esta copiado LITERALMENTE de
//   como lo hace el firmware aleman al pintar (y costo dos intentos dar con ello): si alguien lo
//   "arregla" para que cada fila empiece en byte nuevo, las letras salen con la parte de arriba
//   rota.
//
// â˜… `yOffset` del glifo es NEGATIVO: es relativo a la LINEA BASE, no al borde de arriba. Por eso
//   `y` en estas funciones es la parte de ARRIBA de la caja de la linea, y la linea base se
//   calcula como `y + kDin10Ascenso`.
static constexpr int kDin10Ascenso = 14;   // de la fuente: alto 23 - descenso 9

void dibujaGlyphDin(int x, int lineaBase, char c, bool negro) {
  if (c < (char)kDin10.first || c > (char)kDin10.last) return;   // fuera del juego: se ignora
  const EpdGfxGlyph &g = kDin10.glyph[(uint8_t)c - kDin10.first];
  uint32_t bitidx = 0;
  uint8_t byte = 0;
  for (uint8_t fila = 0; fila < g.height; fila++) {
    for (uint8_t col = 0; col < g.width; col++) {
      // â˜… El byte se recarga cada 8 bits, NO al empezar cada fila (ver el aviso de arriba).
      if ((bitidx & 7u) == 0u) byte = kDin10.bitmap[g.bitmapOffset + (bitidx >> 3)];
      if (byte & 0x80u)
        px(x + g.xOffset + (int)col, lineaBase + g.yOffset + (int)fila, negro);
      byte = (uint8_t)(byte << 1);
      bitidx++;
    }
  }
}

int drawCharDin(int x, int y, char c, bool negro) {
  dibujaGlyphDin(x, y + kDin10Ascenso, c, negro);
  if (c < (char)kDin10.first || c > (char)kDin10.last) return 0;
  return kDin10.glyph[(uint8_t)c - kDin10.first].xAdvance;
}

// Un caracter de 5x7, al tamano que se pida (escala 2 = 10 puntos de ancho, legible
// de sobra en 200x200).
int drawChar(int x, int y, char c, int escala) {
  if (escala == 0) return drawCharDin(x, y, c, true);
  const uint8_t *g = EpdFont5x7::glyph(c);
  for (int col = 0; col < 5; col++) {
    const uint8_t bits = g[col];
    for (int fila = 0; fila < 7; fila++) {
      if (!(bits & (1u << fila))) continue;
      const int px0 = x + col * escala;
      const int py0 = y + fila * escala;
      for (int dy = 0; dy < escala; dy++)
        for (int dx = 0; dx < escala; dx++) px(px0 + dx, py0 + dy, true);
    }
  }
  return 6 * escala;   // 5 columnas + 1 de separacion
}

// Texto. Devuelve el ancho que ha ocupado.
int drawText(int x, int y, const char *s, int escala = 2) {
  int cx = x;
  for (const char *p = s; *p; p++) {
    if (*p == '\n') { y += (escala == 0) ? kDin10.yAdvance : 8 * escala; cx = x; continue; }
    cx += drawChar(cx, y, *p, escala);
  }
  return cx - x;
}

// Texto INVERSO (borra, deja blanco): para leer sobre una pastilla rellena de negro
// (el "TX" de la cabecera, el aviso de transmision). Es la misma fuente pero escribiendo
// pixeles BLANCOS en vez de negros.
int drawCharInv(int x, int y, char c, int escala) {
  if (escala == 0) return drawCharDin(x, y, c, false);
  const uint8_t *g = EpdFont5x7::glyph(c);
  for (int col = 0; col < 5; col++) {
    const uint8_t bits = g[col];
    for (int fila = 0; fila < 7; fila++) {
      if (!(bits & (1u << fila))) continue;
      const int px0 = x + col * escala;
      const int py0 = y + fila * escala;
      for (int dy = 0; dy < escala; dy++)
        for (int dx = 0; dx < escala; dx++) px(px0 + dx, py0 + dy, false);
    }
  }
  return 6 * escala;
}

int drawTextInv(int x, int y, const char *s, int escala = 2) {
  int cx = x;
  for (const char *p = s; *p; p++) {
    if (*p == '\n') { y += (escala == 0) ? kDin10.yAdvance : 8 * escala; cx = x; continue; }
    cx += drawCharInv(cx, y, *p, escala);
  }
  return cx - x;
}

// Ancho que va a ocupar el texto: el avance por caracter es 6*escala, igual que en
// drawText (antes esto no contaba la escala y el texto "centrado" salia descuadrado).
// â˜… Con escala 0 el avance lo dice la propia fuente, caracter a caracter (ancho variable).
int textWidth(const char *s, int escala = 2) {
  if (!s) return 0;
  int anchoMax = 0, ancho = 0;
  for (const char *p = s; *p; p++) {
    if (*p == '\n') {
      if (ancho > anchoMax) anchoMax = ancho;
      ancho = 0;
      continue;
    }
    if (escala == 0) {
      if (*p >= (char)kDin10.first && *p <= (char)kDin10.last)
        ancho += kDin10.glyph[(uint8_t)*p - kDin10.first].xAdvance;
      continue;
    }
    ancho += 6 * escala;
  }
  if (ancho > anchoMax) anchoMax = ancho;
  // La ultima columna de separacion no se "ve": se descuenta para que el centrado
  // quede de verdad centrado. Con escala 0 no hay columna de separacion que descontar:
  // cada caracter ya trae su propio avance.
  if (escala == 0) return anchoMax;
  return (anchoMax > 0) ? anchoMax - escala : 0;
}

// Texto centrado en la pantalla.
void drawTextCenter(int y, const char *s, int escala = 2) {
  int x = (EPD_W - textWidth(s, escala)) / 2;
  if (x < 0) x = 0;
  drawText(x, y, s, escala);
}

// ===========================================================================
//  â˜…â˜…â˜… QUE EL TEXTO QUEPA EN EL PANEL (2026-09-22) â˜…â˜…â˜…
//
//  EL PROBLEMA, que reporto el operador: "los popups de bateria baja y creo que alguno mas se
//  salen de pantalla en los T-Echo; son mas grandes que lo que permite la pantalla".
//
//  LA CAUSA, leida en el codigo: `drawText()` NO recorta. Escribe caracter a caracter con
//  `drawChar()`, y `px()` descarta lo que cae fuera del panel. O sea que un texto mas largo que
//  la pantalla **pierde los caracteres del final EN SILENCIO**, sin avisar y sin que se pueda
//  leer nada raro por el cable. Y el recuadro del aviso SI se recortaba (`bw`, en `pintaAviso`),
//  asi que lo que se veia era un recuadro del ancho correcto con el texto cortado por el borde.
//
//  LA CUENTA QUE HAY QUE RESPETAR: el panel son 200 px y el avance por caracter es `6*escala`.
//    escala 2 -> 12 px por caracter -> **16 caracteres** como mucho
//    escala 1 ->  6 px por caracter -> **33 caracteres**
//  Y ejemplo medido: "DURMIENDO: BATERIA BAJA" son 22 caracteres = 252 px. **52 px fuera.**
//
//  LAS TRES HERRAMIENTAS DE AQUI, en orden de preferencia (recortar es el ultimo recurso: se
//  pierde informacion, y en un aviso de bateria baja la informacion es lo unico que hay):
//    1. `cabeTexto()`      -> Â¿cabe a esta escala?
//    2. `parteEnEscalas()` -> partir en DOS por el mejor espacio Y decir con que escala se pinta
//                             cada trozo. Es lo que se prefiere cuando hay sitio vertical: el
//                             aviso crece hacia arriba en vez de perder letras. **Es la buena**:
//                             elige el corte que deja las dos lineas lo mas grandes posible (las
//                             dos a escala 2 si se puede), y no el simple "mas centrado", que
//                             daba el corte PEOR en el aviso de bateria baja.
//    3. `recortaA()`       -> y si no hay mas remedio, cortar ANADIENDO un aviso visible de que
//                             falta texto, para que nadie lea una frase a medias creyendo que
//                             esta entera.
// ===========================================================================
constexpr int kPanelW = EPD_W;   // 200 px: el ancho util de verdad

// Â¿Cabe este texto a esta escala?
bool cabeTexto(const char *s, int escala, int anchoMax = kPanelW) {
  return textWidth(s, escala) <= anchoMax;
}

// Recorta `s` para que quepa a `escala` dentro de `anchoMax`. Si recorta, anade ".." al final
// (lo que cabe es ".." mas el texto), para que se VEA que falta algo.
void recortaA(const char *s, int escala, int anchoMax, char *dst, size_t n) {
  if (!s || !dst || n == 0) return;
  if (textWidth(s, escala) <= anchoMax) {
    snprintf(dst, n, "%s", s);
    return;
  }
  const int cabe = anchoMax / (6 * escala);       // caracteres que caben
  char tmp[256];
  snprintf(tmp, sizeof(tmp), "%s", s);
  // Se cortan los bytes justos: los acentos ocupan 2 bytes y no se parten por la mitad.
  size_t corte = strlen(tmp);
  if ((int)corte > cabe) {
    corte = (size_t)(cabe > 2 ? cabe - 2 : 0);
    while (corte > 0 && ((unsigned char)tmp[corte] & 0xC0) == 0x80) corte--;   // no partir UTF-8
  }
  tmp[corte] = '\0';
  snprintf(dst, n, "%s..", tmp);
}

// Parte el texto en dos lineas buscando el espacio MAS CENTRADO (no el primero): asi las dos
// lineas quedan parecidas y el aviso no queda con una larga y otra de dos letras.
// Si no hay ningun espacio, devuelve false y quien llama decide (normalmente recortar).
// â˜…â˜… CORREGIDO EL 2026-09-22 (DOS VECES, y las dos por lo mismo: mirar solo una mitad) â˜…â˜…
//   1) La version anterior comprobaba que las dos lineas cupieran **a escala 1**:
//          return (textWidth(l1, 1) <= kPanelW && textWidth(l2, 1) <= kPanelW);
//      y eso es FALSO para la primera, que `pintaAviso` pinta a ESCALA 2. Con
//      "DURMIENDO: BATERIA BAJA" partia en "DURMIENDO: BATERIA" (18 caracteres = 214 px), que
//      no cabia: acababa recortado con ".." perdiendo el final del aviso.
//   2) Y elegia el espacio MAS CENTRADO sin mirar si el trozo cabia. Con
//      "DURMIENDO: BATERIA BAJA" el mas centrado es el de "BATERIA|BAJA" (18 caracteres),
//      pero hay OTRO espacio (el de "DURMIENDO:|BATERIA") que da dos trozos de 10 y 12: los
//      dos caben A ESCALA 2, o sea que se leen grandes. Se estaba eligiendo el corte PEOR.
//
//   AHORA: de todos los cortes posibles por espacio, se queda con el que deja las dos lineas
//   MAS GRANDES posibles (escala 2 las dos > escala 2 y 1 > escala 1 las dos), y dentro de eso,
//   el mas centrado. Y ademas las COLOCA en su orden natural (`parteEnEscalas`).
//
//   Esta funcion solo dice si se puede partir. Quien coloca las lineas y decide la escala es
//   `parteEnEscalas`, que es la que se usa desde `pintaAviso` y `displayPopupWait`.
// â˜…â˜… EL CORTE BUENO: el que deja las dos lineas MAS GRANDES posibles (2026-09-22) â˜…â˜…
//   De todos los espacios del texto, se prueban todos y se elige el mejor corte:
//     - primero los que dejan las DOS lineas a escala 2 (se leen grandes);
//     - si no hay, los que dejan las dos a escala 1 (mas pequeno, pero ENTERO);
//     - y dentro de cada grupo, el mas CENTRADO (que las dos lineas queden parecidas).
//   Devuelve false si no hay ningun corte que quepa, y entonces quien llama recorta.
//
//   â˜… POR QUE ASI: elegir "el mas centrado" a secas daba el corte PEOR en el aviso que reporto
//     el operador. "DURMIENDO: BATERIA BAJA" tiene dos espacios; el mas centrado parte en
//     "DURMIENDO: BATERIA" (18 caracteres = 214 px, no cabe a escala 2) y el otro en
//     "DURMIENDO:" (10) + "BATERIA BAJA" (12), que SI caben los dos a escala 2. Con el criterio
//     viejo el aviso acababa en letra pequena; con este se lee grande.
// â˜…â˜… EL MEJOR TAMANO AL QUE CABE UN TEXTO, DE MAYOR A MENOR (2026-09-22) â˜…â˜…
//   Devuelve la escala MAS GRANDE a la que el texto cabe en el ancho, o -1 si no cabe ni con la
//   mas pequena. El orden de preferencia es 2 (grande), 0 (DIN 10: igual de alta que la 2 pero
//   mas estrecha) y 1 (pequena).
//   â˜… POR QUE HACIA FALTA: hasta ahora la eleccion estaba escrita a mano como
//   `cabeTexto(x,2) ? 2 : (cabeTexto(x,1) ? 1 : 0)`, y ese `0` final significaba "no cabe" --
//   chocaba con el 0 nuevo, que es la fuente DIN. Con una sola funcion no hay numeros magicos
//   repartidos por el fichero y anadir un tamano el dia de manana es tocar UN sitio.
int mejorEscalaPara(const char *s, int anchoMax = kPanelW) {
  if (!s) return -1;
  if (cabeTexto(s, 2, anchoMax)) return 2;
  if (cabeTexto(s, 0, anchoMax)) return 0;    // el tercer tamano: alta pero estrecha
  if (cabeTexto(s, 1, anchoMax)) return 1;
  return -1;
}

bool parteEnEscalas(const char *s, char *a, size_t na, int *ea,
                    char *b, size_t nb, int *eb) {
  if (!s || !a || !na || !b || !nb || !ea || !eb) return false;

  char mejorA[64] = "", mejorB[64] = "";
  int mejorE1 = 0, mejorE2 = 0;
  long mejorCentrado = 0x7FFFFFFF;
  bool hay = false;

  for (const char *p = s; *p; p++) {
    if (*p != ' ') continue;
    char x[64], y[64];
    const int izq = (int)(p - s);
    if (izq <= 0 || izq >= (int)sizeof(x)) continue;      // ni vacio ni larguisimo
    snprintf(x, sizeof(x), "%.*s", izq, s);
    snprintf(y, sizeof(y), "%s", p + 1);
    if (y[0] == '\0') continue;                           // no partir dejando la 2a vacia

    // Escala de cada linea: la mas grande a la que quepa (2 -> 0 -> 1).
    const int e1 = mejorEscalaPara(x);
    const int e2 = mejorEscalaPara(y);
    if (e1 < 0 || e2 < 0) continue;                       // por este espacio no cabe

    // Â¿Es mejor que lo que ya teniamos? Primero por "las dos grandes", luego por centrado.
    // Se llama "grande" a la escala 2 o a la DIN 10, que tienen la misma altura.
    auto esGrande = [](int e) { return e == 2 || e == 0; };
    const int categoriaNueva = (esGrande(e1) && esGrande(e2)) ? 2 : 1;
    const int categoriaVieja = (esGrande(mejorE1) && esGrande(mejorE2)) ? 2 : (hay ? 1 : 0);
    const long centrado = labs((long)izq - (long)(strlen(s) - (size_t)izq - 1));
    if (!hay || categoriaNueva > categoriaVieja ||
        (categoriaNueva == categoriaVieja && centrado < mejorCentrado)) {
      snprintf(mejorA, sizeof(mejorA), "%s", x);
      snprintf(mejorB, sizeof(mejorB), "%s", y);
      mejorE1 = e1; mejorE2 = e2;
      mejorCentrado = centrado;
      hay = true;
    }
  }

  if (!hay) return false;
  snprintf(a, na, "%s", mejorA);
  snprintf(b, nb, "%s", mejorB);
  *ea = mejorE1;
  *eb = mejorE2;
  return true;
}

// â˜… AQUI VIVIA `parteEnDos()`, que partia el texto en dos SIN decidir la escala de cada linea.
//   Se quito el 2026-09-22 al quedarse sin uso: la logica buena es `parteEnEscalas()`, que
//   ademas dice con que escala se pinta cada trozo. Dejar dos funciones que parten textos, una
//   de ellas muerta, es una trampa para el que lea esto despues: se quitaria la equivocada.
//   Lo que hacia falta de ella (partir "a lo bruto", sin escalas) no lo usa nadie.

// Rectangulo RELLENO: para marcas de esquina. Se usa en la prueba de orientacion.
void relleno(int x0, int y0, int w, int h) {
  for (int y = y0; y < y0 + h; y++)
    for (int x = x0; x < x0 + w; x++) px(x, y, true);
}

// Grueso vertical / horizontal: para dibujar una letra grande a mano.
void barraV(int x, int y0, int y1, int grosor) {
  for (int g = 0; g < grosor; g++)
    for (int y = y0; y <= y1; y++) px(x + g, y, true);
}
void barraH(int x0, int x1, int y, int grosor) {
  for (int g = 0; g < grosor; g++)
    for (int x = x0; x <= x1; x++) px(x, y + g, true);
}

// â˜…â˜… IMAGEN DE PRUEBA DE ORIENTACION (2026-09-14) â˜…â˜…
//
// SE QUEDA EN LAS HERRAMIENTAS DE TALLER A PROPOSITO. Esta imagen ahorro horas: con texto
// centrado y simetrico, dos rotaciones distintas parecen la misma y las descripciones
// ("girado", "boca abajo") se vuelven ambiguas â€” que es exactamente lo que paso durante
// toda una tarde. Una "F" es **totalmente asimetrica**: se distingue sin ninguna duda en
// cual de las cuatro orientaciones esta, y ademas se sabe si esta ESPEJADA (cosa que una
// rotacion no puede provocar). Si algun dia hay que volver a ajustar la orientacion, se
// lanza con `epdrot N` y se mira la F.
//
// Referencia de como se ve la F **de pie** en la pantalla:
//     - el palo vertical va por la IZQUIERDA
//     - las dos barras salen hacia la DERECHA
//     - la barra de ARRIBA es la mas larga
// Y ademas hay dos marcas que no se pueden confundir:
//     - un CUADRO NEGRO RELLENO en la esquina superior izquierda
//     - una BARRA NEGRA a lo largo del borde inferior
// Si el cuadro no esta arriba a la izquierda, la imagen esta girada.
void dibujaPruebaOrientacion() {
  clearBuf(true);
  // Cuadro negro relleno en la esquina SUPERIOR IZQUIERDA (referencia inequivoca).
  relleno(8, 8, 44, 44);
  // Barra a lo largo del borde INFERIOR.
  barraH(8, EPD_W - 9, EPD_H - 16, 8);
  // La "F" grande, en el centro-derecha.
  const int fx = 80, fy = 50, alto = 110, grosor = 14;
  barraV(fx, fy, fy + alto, grosor);                 // palo vertical
  barraH(fx, fx + 84, fy, grosor);                   // barra de arriba (larga)
  barraH(fx, fx + 56, fy + 46, grosor);              // barra de en medio (corta)
  // El numero de rotacion, para no depender de la etiqueta: si esto sale legible, ya se
  // sabe que rotacion esta puesta.
  char b[8];
  snprintf(b, sizeof(b), "R%d", gRotacion);
  drawText(8, EPD_H - 48, b, 2);
}

// â˜… Dibuja en `gBuf` lo que toque AHORA (el aviso reciente si lo hay, o la escena del
//   carrusel) y deja `gDirty` puesto. Es el UNICO sitio donde se decide que se ve.
//   Lo usan `displayRefresh()` (el bucle normal) y la prueba del carrusel del comando
//   `epdparcial` (herramienta de taller): asi lo que se prueba es exactamente lo mismo que
//   se vera luego en el carrusel automatico del Paso 2, y no una copia que puede divergir.
//   Se define al final del fichero, junto a `pintaEstado/Radio/Ultimo`, que son de este
//   mismo espacio de nombres anonimo.
void dibujaEscena();

// Voltaje de bateria que se dibuja y que entra en la huella del contenido. Se define mas
// abajo (junto a `huellaContenido()`), pero `pintaEstado()` lo usa antes.
uint16_t bateriaMv();

// Saca la huella del contenido y sus ingredientes (se define junto a `huellaContenido()`).
void epdHuellaTexto(char *out, size_t n);

}  // namespace

// Sonda cruda del periferico SPI (herramienta de taller, comando "epdsonda" del USB).
// Se declara AQUI, en el espacio de nombres global, y no dentro del anonimo: si se declara
// dentro, el enlazador busca una version interna que no existe y no compila.
// OJO: `dibujaPruebaOrientacion()` NO se declara aqui. Ya esta definida mas arriba, dentro
// del espacio de nombres anonimo, y declararla tambien fuera crea una segunda version con
// enlazado externo: el compilador no sabe cual usar y da "call is ambiguous".
void epdSonda(char *out, size_t n);
void epdSondaPines(char *out, size_t n);
void epdVolcadoPines(char *out, size_t n);

// Gancho del boton para las esperas del driver (T-Echo Project Butter). La variable
// `gPumpBoton` vive arriba, dentro del espacio de nombres anonimo (el driver la usa en
// sus esperas); la funcion que la pone tiene que estar AQUI FUERA, en el espacio global,
// o el enlazador busca una version interna que no existe (es el mismo caso que la sonda
// de arriba). Ver la nota del gancho, junto a epdXferSpim().
void displaySetPumpBoton(void (*fn)(void)) { gPumpBoton = fn; }

// ============================================================================
//  Interfaz de pantalla del firmware (las mismas funciones que display.h)
// ============================================================================

// Estado que nos va dando el resto del firmware, para poder pintarlo.
namespace {
DigiConfig *gCfg = nullptr;
SensorReadings gSens{};
uint32_t gRx = 0, gTx = 0, gDg = 0;
char gLinea1[40] = "";      // "ultima recibida" o un aviso
char gLinea2[40] = "";
uint32_t gLineaMs = 0;
// â˜…â˜… CUANTO DURA UN AVISO: 1,5 S, Y EN UN SOLO REFRESCO (2026-09-15) â˜…â˜…
//
// ANTES esto eran 8000 ms y el aviso SUSTITUIA a la escena entera (`dibujaEscena` pintaba
// solo el aviso). Eso obligaba a DOS refrescos de tinta por cada RX/TX (cada uno ~1,5 s):
//   1) llega el RX  -> el aviso entra en la huella -> refresco A (solo el aviso)
//   2) a los 8 s    -> el aviso caduca (la linea pasa a vacia) -> la huella cambia ->
//                      refresco B (vuelve la escena)
// Resultado medido por el operador: "un solo RX ocupa la pantalla ~3 segundos" y, con
// trafico, el panel pintando casi todo el rato (parecia que el nodo "se quedaba tonto").
//
// AHORA: el aviso se dibuja ENCIMA de la escena (que se sigue pintando debajo) y dura
// 1500 ms. Sigue habiendo DOS refrescos --uno cuando aparece y otro cuando se va, porque en
// tinta un pixel no se borra solo-- pero deja de haber un estado intermedio en el que la
// pantalla se queda SOLO con el aviso: el segundo refresco ya es "la escena, limpia".
// Ver el detalle y la cuenta exacta de refrescos en `dibujaEscena()`.
constexpr uint32_t kAvisoMs = 1500;
// Los avisos que NO son de trafico (bateria baja al arrancar, "Solo sin cable USB", textos
// del menu) se quedan como estaban: 8 s. Son pocos y hay que poder LEERLOS.
constexpr uint32_t kLineaMs = 8000;
uint32_t gUltimoPintado = 0;
uint32_t gUltimoToqueMs = 0;     // ultima vez que se pulso un boton (para aplazar el repintado)
constexpr uint32_t kDebounceToqueMs = 400;  // si tocas antes de esto, no repinta (se encola)
// â˜…â˜… T-ECHO PROJECT BUTTER (2026-09-15): EL APLAZAMIENTO ES SOLO PARA EL TACTIL â˜…â˜…
// `gUltimoToqueAgrupaMs` es "se esta tocando seguido, agrupa el repintado". Solo lo
// escriben el TACTIL CAPACITIVO (menuNavigate / displayNextScene(true)), que es el unico
// que puede ir rapido: no tiene ventana de doble toque, asi que se pueden encadenar
// toques cada ~150 ms y no tiene sentido pagar 1,5 s de panel por cada uno.
// El BOTON FISICO ya no lo escribe: su toque corto solo existe cuando han pasado 600 ms
// desde que solto (la ventana del doble), asi que nunca llega en rafaga y aplazarle el
// repintado 400 ms era LATENCIA PURA (medida: 400 ms de nada antes de empezar a pintar).
// â˜… VUELTA ATRAS DEL 2026-09-21: de 250 a 400 ms otra vez. Se habia bajado a 250 ms dando
//   por hecho que el refresco iba a ser mas rapido (se le habian quitado dos esperas), pero
//   esas esperas RESULTARON NECESARIAS y se han restaurado: sin ellas la pantalla se queda
//   en negro. Asi que la ventana vuelve a su valor, que es el que estaba medido y probado.
constexpr uint32_t kAgrupaToquesMs = 400;   // solo tactil: espera a que dejes de tocar
uint32_t gUltimoToqueAgrupaMs = 0;
// â˜…â˜… EL APLAZAMIENTO: DOS INTENTOS DE ARREGLARLO, LOS DOS REVERTIDOS (b79, 2026-09-21) â˜…â˜…
//   El aplazamiento de 400 ms tiene un defecto conocido y medido: un toque SUELTO paga los
//   400 ms enteros antes de que se empiece a pintar ("desde que pulso hasta que lo veo
//   dibujado pasa un poquito de tiempo", dijo el operador). Se intento arreglar dos veces y
//   las dos salio peor:
//     - b76, con un tope de 150 ms: empezaba a pintar antes de que el operador acabara de
//       tocar, pintaba estados INTERMEDIOS y con el bucle ocupado ~700 ms los toques que
//       quedaban se ejecutaban tarde -> "no guarda si pulso varias veces".
//     - b77/b78, "por estado": la pantalla miraba/vaciaba la cola de toques del boton, y esa
//       cola NO es un aviso de trabajo: es EL CONTADOR DE ACCIONES PENDIENTES que el bucle
//       cobra y ejecuta. Quitarselas = el toque se pierde -> "no funciona el boton capacitivo".
//   CONCLUSION, y queda escrita para no volver a intentarlo igual: el driver de la pantalla
//   NO TOCA la cola de toques. El unico dato que puede usar es el sello de tiempo
//   `gUltimoToqueAgrupaMs`, que no se lleva nada por delante. Para bajar esta latencia hay
//   que cambiar el MECANISMO, no el numero: ver la nota de la regla, mas abajo.
constexpr uint8_t kNumEscenas = 8;
uint8_t gEscena = 0;

// ===========================================================================
//  â˜…â˜… "FIJAR COORDS" EN LA PANTALLA DE TINTA (2026-09-15) â˜…â˜…
// ===========================================================================
// QUE HACE: el operador elige "Fijar coords" en el menu y el nodo hace TODO el
// trabajo: enciende el GPS, espera a que fije, deja que la posicion se asiente
// (varias lecturas seguidas), guarda la posicion como posicion FIJA del aparato y
// vuelve a apagar el GPS. Es la via para dejar un repetidor publicado en el mapa
// sin escribir numeros a mano ni depender del configurador web.
//
// â˜… LA SESION NO SE PROGRAMA AQUI: la lleva el RASTREADOR
//   (trackerSetCoordsStart/Tick, ver tracker.h y tracker.cpp), que es quien manda
//   sobre el GPS. Esta pantalla solo 1) da la orden, 2) ENSENA lo que esta pasando
//   y 3) guarda la posicion cuando el rastreador la da por buena
//   (displaySaveCoords, mas abajo). En la OLED ese mismo camino ya funcionaba.
//
// â˜… POR QUE HACE FALTA UNA PANTALLA PROPIA Y NO UN AVISO: un aviso de los de
//   `pintaAviso()` caduca a los 8 s y la captura dura MINUTOS (fijar + 20
//   lecturas). Ademas el bucle principal (main.cpp) ya manda el progreso
//   ("GPS 3/20") por displayPopup() en CADA muestra, y en tinta eso serian ~20
//   refrescos de 1,5 s (30 segundos de panel pintando) mas un texto que se pisa a
//   si mismo. Por eso:
//     - el progreso se pinta como PANTALLA COMPLETA (no como aviso encima), y
//     - solo CAMBIA en los escalones que se ven (0, 25, 50, 75, 100 %) porque el
//       repintado va por la huella del contenido: si el texto no cambia, no se
//       manda nada al panel,
//     - y los avisos "GPS n/N" del bucle se descartan mientras dura la sesion
//       (ver displayPopup), que es lo unico que quedaba por atar.
//
// â˜… COMO SE SALE: la pantalla NO se cierra sola NUNCA por tiempo.
//   - Mientras BUSCA o ASIENTA (la captura puede tardar lo que necesite): un toque
//     de boton CANCELA la sesion, apaga el GPS si lo encendio ella y vuelve al
//     menu. Era la condicion que puso el operador: "ya que puede tardar lo que
//     tarde, que el usuario pueda salir de ahi con un boton".
//   - Cuando ya hay resultado (guardado o error), el primer toque quita la
//     pantalla y vuelve al menu/carrusel. El panel es bistable, asi que el
//     resultado se queda a la vista hasta que el operador quiera.
//
// â˜… EL GPS LO ENCIENDE Y LO APAGA LA SESION (2026-09-15), no este fichero: ver
//   trackerSetCoordsStart() y el guardia de gpsManage() en tracker.cpp. Por eso
//   aqui ya NO hay ninguna comprobacion previa del tipo "activa GPS en repetidor":
//   en modo repetidor la captura funciona igual, sin tocar nada.
enum {
  COORDS_OCULTA = 0,   // no hay sesion (o ya se ha cerrado la pantalla)
  COORDS_BUSCANDO,
  COORDS_ASENTANDO,
  COORDS_GUARDADO,
  COORDS_FALLO,        // no se pudo guardar en la configuracion
};
uint8_t gCoordsPantalla = COORDS_OCULTA;
// Ultima posicion GUARDADA por la sesion (solo para poder ensenarla al terminar:
// el operador tiene que ver lo que se ha guardado, no un "vale" a secas).
double gCoordsLat = 0.0, gCoordsLon = 0.0;
// millis() en que se lanzo la sesion: sirve para NO confundir el toque con el que
// el operador acaba de elegir "Fijar coords" en el menu con un toque de cancelar.
uint32_t gCoordsInicioMs = 0;

void coordsPantallaInicia() {
  gCoordsPantalla = COORDS_BUSCANDO;
  gCoordsInicioMs = millis();
  gDirty = true;
}
bool coordsPantallaActiva() { return gCoordsPantalla != COORDS_OCULTA; }
void coordsPantallaCierra() {
  if (gCoordsPantalla == COORDS_OCULTA) return;
  gCoordsPantalla = COORDS_OCULTA;
  gDirty = true;
}

// ---------------------------------------------------------------------------
//  HISTORIAL RECIENTE RX / TX (2026-09-15): la OLED tenia escenas de "ULTIMOS RX" y
//  "ULTIMOS TX" listando varias entradas. La tinta las necesita con su PROPIO registro
//  (las listas de la OLED no se compilan aqui), asi que se mantiene un anillo pequeno que
//  alimentan los hooks displayNoteRx/Digi/Tx. No se borra entre escenas.
// ---------------------------------------------------------------------------
struct RxLog { char call[12]; float rssi; float snr; uint32_t ms; };
struct TxLog { char what[20]; uint32_t ms; };
constexpr int kRxLogMax = 6;
constexpr int kTxLogMax = 6;
RxLog gRxLog[kRxLogMax]; int gRxLogN = 0; int gRxLogHead = 0;
TxLog gTxLog[kTxLogMax]; int gTxLogN = 0; int gTxLogHead = 0;

void rxLogPush(const char *call, float rssi, float snr) {
  RxLog &e = gRxLog[gRxLogHead];
  snprintf(e.call, sizeof(e.call), call ? "%s" : "?", call ? call : "?");
  e.rssi = rssi; e.snr = snr; e.ms = millis();
  gRxLogHead = (gRxLogHead + 1) % kRxLogMax;
  if (gRxLogN < kRxLogMax) gRxLogN++;
}
void txLogPush(const char *what) {
  TxLog &e = gTxLog[gTxLogHead];
  snprintf(e.what, sizeof(e.what), "%s", what ? what : "");
  e.ms = millis();
  gTxLogHead = (gTxLogHead + 1) % kTxLogMax;
  if (gTxLogN < kTxLogMax) gTxLogN++;
}

// ---------------------------------------------------------------------------
//  â˜…â˜… PASO 2: EL CARRUSEL AUTOMATICO (2026-09-15) â˜…â˜…
//
//  Con el refresco parcial ya hecho (1,5 s y SIN parpadeo), cambiar de pantalla solo de vez
//  en cuando es comodo. Antes NO lo era: cada cambio era un parpadeo de 2 s.
//
//  â˜… POR QUE NO SE COPIA EL NUMERO DE LA OLED: alli el auto-avance es de 4 SEGUNDOS
//  (`display.cpp`, kSceneAutoMs), porque su pantalla se redibuja instantaneamente. Aqui cada
//  cambio se PAGA en el panel: 1,5 s de parcial y, cada cierto numero, un completo de 3,1 s
//  para limpiar fantasmas (el HANDOVER pide forzar completos de vez en cuando). Ademas, el
//  firmware de referencia del T-Echo refresca cada 15 s porque **la pantalla comparte
//  alimentacion con el GPS** (`periph_pwr.c`): refrescar muy seguido obliga a tener el GPS
//  encendido. Por eso aqui el carrusel va LENTO a proposito: 45 s.
//
//  â˜… NO SE REPINTA SI NO HA CAMBIADO NADA: de eso se encarga la huella del contenido
//  (`huellaContenido()`, ver mas abajo) y el limite `kRepintadoMaxMs`. El carrusel solo
//  PIDE el cambio de escena; el que decide pintar es el mismo camino de siempre.
//
//  â˜… PAUSA AL TOCAR EL BOTON (mismo patron que la OLED, adaptado a esta pantalla): si acabas
//  de elegir una pantalla con el boton, el carrusel se calla 2,5 MINUTOS para que no te
//  cambie la pantalla mientras la estas leyendo. Cada pulsacion renueva la pausa. El numero
//  de la OLED (15 s) es demasiado corto para una pantalla que tarda 1,5 s en cambiar.
//
//  â˜… SE APAGA CON EL AJUSTE QUE YA EXISTE (`sceneAutoAdvance`): hasta ahora ese ajuste no
//  hacia NADA en la pantalla de tinta electronica (es el campo de la OLED), asi que la web
//  ensenaba un interruptor inerte. Con esto pasa a mandar de verdad.
// ---------------------------------------------------------------------------
constexpr uint32_t kEscenaAutoMs = 45000;         // 45 s por escena
constexpr uint32_t kPausaTrasBotonMs = 150000;    // 2,5 min sin carrusel tras una pulsacion
uint32_t gUltimoCambioEscenaMs = 0;
uint32_t gCarruselPausadoHasta = 0;
bool gAutoAvance = false;      // lo pone la config (sceneAutoAdvance) en cada refresco
}  // namespace

void displayBindConfig(DigiConfig *cfg) {
  gCfg = cfg;
  // â˜… La rotacion viene de la CONFIGURACION PERSISTENTE (config.h: `epdRotation`), no de una
  //   constante: el panel puede ir montado con distinta orientacion segun la unidad, y
  //   entonces la elige el usuario desde el configurador web o con `set epdRotation N`.
  if (gCfg) {
    const uint8_t r = gCfg->epdRotation;
    gRotacion = (r <= 3) ? (int)r : 0;
    gRotacionAplicada = -1;   // fuerza a repintar con la rotacion nueva
  }
}

// Indicativo del operador SOLO (sin el SSID, p.ej. "N0CALL" y no "N0CALL-3"). Peticion del
// operador (2026-09-15): en la pantalla se muestra su indicativo, sin el sufijo numerico.
char gCallSinSSID[16];
const char *callSinSSID() {
  const char *raw = (gCfg && gCfg->callsign[0]) ? gCfg->callsign : "NOCALL";
  int n = 0;
  for (; raw[n] && raw[n] != '-' && n < (int)sizeof(gCallSinSSID) - 1; n++)
    gCallSinSSID[n] = raw[n];
  gCallSinSSID[n] = '\0';
  return gCallSinSSID;
}

// ---------------------------------------------------------------------------
//  SPLASH DE ARRANQUE (2026-09-15) â€” portado de la OLED y adaptado a la 200x200:
//  logo de antena + indicativo + nombre del sistema + version + modo, y una BARRA DE
//  PROGRESO abajo que se va llenando mientras dura (~6 s). Es la "animacion" de la
//  OLED: en tinta solo se puede redibujar cada ~1,5 s (el tiempo del panel), asi que
//  la barra avanza en unos pocos saltos, suficiente para que se vea crecer.
//  `pct` va de 0 a 100.
void dibujaSplash(int pct) {
  clearBuf(true);
  // Marca del firmware arriba (asustada que el splash aluda al firmware, no a un garabato).
  drawTextCenter(40, "EA2OY APRS SYSTEM", 1);
  hLine(50, EPD_W - 50, 56, 1);

  // El indicativo, EN GRANDE y CENTRADO: SOLO el indicativo del operador, sin el SSID
  // (callSinSSID). Cada persona instalara el firmware y pondra el suyo.
  const char *call = callSinSSID();
  drawTextCenter(72, call, 3);

  char b[36];
  snprintf(b, sizeof(b), "v%s  %s", APP_VERSION_STR, __DATE__);
  drawTextCenter(112, b, 1);
  {
    uint8_t m = (uint8_t)(gCfg ? gCfg->mode : 0);
    const char *mn = (m == 1) ? "Rastreador" : ((m == 2) ? "Digi+Tracker" : "Repetidor");
    snprintf(b, sizeof(b), "Modo %d: %s", (int)m, mn);
    drawTextCenter(132, b, 1);
  }

  // â˜…â˜… EL MENSAJE DE DORMIDO, TAMBIEN EN EL ARRANQUE (peticion del operador, 2026-09-16) â˜…â˜…
  //   El mismo texto y el mismo tamaÃ±o que la pantalla que se queda fija al mandar el nodo
  //   a dormir (`displaySleepScene`: `sleepMsg` en escala 2), puesto ENTRE la linea del modo
  //   y la barra de progreso, centrado en ese hueco:
  //     - la linea del modo va en y=132 y una letra de escala 1 mide 7 px -> acaba en 139;
  //     - la barra de progreso empieza en y=174;
  //     - luego el hueco util es 140..173, y su centro cae en 157;
  //     - una linea de escala 2 mide 14 px (7*2), asi que su parte de ARRIBA va en 157-7=150.
  //   Y 150 es, casualidad util, la MISMA y que usa la pantalla de dormido: el mensaje sale
  //   en el mismo sitio de la pantalla en las dos, asi que se reconoce de un vistazo.
  //   Si no hay mensaje configurado (vacio), no se pinta nada y el splash queda como estaba.
  //   Se dibuja con `drawTextCenter`, que ya descuenta la ultima columna de separacion: el
  //   centrado es de verdad. Con letras de 12 px de ancho salen ~16 caracteres; mas largo se
  //   recorta por los lados, igual que en la pantalla de dormido.
  if (gCfg && gCfg->sleepMsg[0]) {
    drawTextCenter(150, gCfg->sleepMsg, 2);
  }

  // Barra de progreso: enmarcada y rellena segun pct (0..100). Es la animacion.
  const int by = 174, bw = 150, bh = 8;
  barraH(25, 25 + bw, by, 1); barraH(25, 25 + bw, by + bh, 1);
  barraV(25, by, by + bh, 1); barraV(25 + bw, by, by + bh, 1);
  if (pct > 0) relleno(25 + 1, by + 1, (bw - 2) * pct / 100, bh - 2);
}

// ---------------------------------------------------------------------------
//  ENSAYO VISUAL DE ARRANQUE: el SPLASH con su barra animada (~6 s) y despues el
//  estado del nodo.
//
//  El panel se enciende desde aqui a proposito, cuando el nodo YA lleva 6 segundos
//  andando: asi el USB esta enumerado y, pase lo que pase con la pantalla, se puede
//  seguir hablando con el nodo y volver a grabarlo por software ("dfu confirm").
//  El banco de pruebas hacia lo mismo y por eso nunca se quedo mudo.
// ---------------------------------------------------------------------------
void displayArrancaPantalla() {
  // â˜… TRAZA DE ARRANQUE (herramienta de taller). Este texto sale por el USB en cuanto el
  //   nodo lleva 6 s andando, asi que SI se ve al abrir el puerto.
  //   â˜… Va con `diagTrazaArranque()` (2026-09-16): se sigue viendo siempre (el modo
  //   diagnostico esta apagado al arrancar, asi que no puede depender de el), pero NO se
  //   le cuela a un programa host: en modo TNC el puerto es suyo. Ver diag.h.
  if (diagTrazaArranque()) {
    Serial.printf("PANTALLA: displayArrancaPantalla() entra (gReady=%d transporte=%d)\r\n",
                  gReady ? 1 : 0, gTransporte);
    Serial.flush();
  }
  if (gReady) return;

  // Corriente: el MOSFET de periferia y el regulador de 3,3 V. La variante ya los
  // enciende en initVariant(), pero se reafirman aqui (no cuesta nada y deja claro
  // de que depende la pantalla).
  pinMode(PIN_PWR_EN, OUTPUT);
  digitalWrite(PIN_PWR_EN, HIGH);
  pinMode(PIN_REG_EN, OUTPUT);
  digitalWrite(PIN_REG_EN, HIGH);
  // P1.11 NO SE TOCA (ver la nota (e) de la cabecera).
  delay(50);

  // CS y DC como salidas (en reposo, altos) y BUSY como entrada sin pull.
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT);
  digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_BUSY, INPUT);

  // El bit-bang pone los pines a mano: no depende de ningun periferico (ver la nota
  // larga del principio). â˜… ORDEN IMPORTANTE: si algun dia se quiere ENSEÃ‘AR como esta
  // SPIM2, hay que mirarlo ANTES de dejarselo al GPIO, porque `epdPinesBitBang()`
  // desconecta a proposito el PSEL del periferico.
  if (gTransporte == 1) {
    epdSpiConfigura();
    if (diagTrazaArranque()) {
      Serial.printf("PANTALLA: transporte=SPIM2 PSEL.SCK=P0.%d PSEL.MOSI=P0.%d ENABLE=%u\r\n",
                    (int)(NRF_SPIM2->PSEL.SCK & 0x1F), (int)(NRF_SPIM2->PSEL.MOSI & 0x1F),
                    (unsigned)(NRF_SPIM2->ENABLE & 1u));
    }
  } else {
    epdPinesBitBang();
    if (diagTrazaArranque()) {
      Serial.println("PANTALLA: transporte=bit-bang (SPIM2 APAGADO y desenganchado de los pines)");
    }
  }
  epdPinesReposo();

  // (1) SPLASH con su barra de progreso animada (~6 s en total). El primer refresco es
  //     COMPLETO (aun no hay plano anterior) y los siguientes PARCIALES; como entre paso y
  //     paso solo cambia la barra, el panel solo mueve la barra (poco fantasma). El tiempo
  //     que tarda el panel en escribir (~1,5 s por parcial) marca el ritmo, asi que con 4
  //     pasos queda en torno a 6 s.
  gReady = true;        // a partir de aqui epdFlush ya manda de verdad
  displayBacklightKick();   // luz encendida durante el splash
  {
    const int pasos[] = {0, 34, 67, 100};
    for (int i = 0; i < 4; i++) {
      dibujaSplash(pasos[i]);
      gDirty = true;
      epdFlush();       // el primer refuerzo es COMPLETO, los siguientes parciales
    }
  }
  displayBacklightTick(millis());  // que apague cuando toque (splash ya terminado)

  // (2) Despues del splash, se deja la escena de Estado lista, que es la que se ve casi
  //     siempre. El carrusel la pinta en su primera pasada (ya no hay texto de prueba:
  //     "PANTALLA OK / T-Echo Plus" era de cuando se verificaba que la pantalla pintaba,
  //     y esta demostrado que funciona).
  gEscena = 0;
  gDirty = true;
  gUltimoPintado = 0;     // fuerza a que pinte Estado de inmediato
  gAsleep = false;

  if (diagTrazaArranque()) {
    Serial.printf("PANTALLA: arrancada. ultimo refresco %lums, timeouts=%lu\r\n",
                  (unsigned long)gMsUltimo, (unsigned long)gErrTimeout);
  }
}

// Sustitutos vacios: la parte de alimentacion de la pantalla va toda en
// displayArrancaPantalla(), para que el USB este vivo antes de tocar el panel.
void displayInit() {}
void displayInitTrasRadio() {}

// Diagnostico de la pantalla en una linea, a peticion (comando "epd" del USB).
// Existe porque los mensajes del arranque no se ven: cuando se abre el puerto, el
// nRF52 se reinicia y el arranque ya ha pasado.
void displayDiagTexto(char *out, size_t n) {
  if (!out || !n) return;
  // Si la primera letra que trae el buffer es 'S', el que llama quiere la SONDA del
  // periferico (herramienta de taller), no el resumen. Ver cli.cpp ("epdsonda").
  if (out[0] == 'S') { epdSonda(out, n); return; }
  // 'T' = cambiar de transporte (ver cli.cpp, "epdtrans"): T0 = bit-bang, T1 = SPIM2.
  if (out[0] == 'T') { gTransporte = (out[1] == '1') ? 1 : 0; return; }
  // 'w' (minuscula) = INTERRUPTOR DE LA VENTANA (ver cli.cpp, "epdventana"): w1 = ventana
  // (solo se mandan las filas que cambian), w0 = pantalla entera. Es la vuelta atras si la
  // ventana diera problemas: se apaga desde el cable, sin volver a grabar.
  if (out[0] == 'w') { gVentanaOn = (out[1] == '1'); return; }
  // 'P' = sonda de PINES: Â¿el bit-bang puede gobernar de verdad SCK/MOSI/CS/DC/RST?
  if (out[0] == 'P') { epdSondaPines(out, n); return; }
  // 'G' = ROTACION en caliente: out[1] = '0'..'3'. Ver gRotacion/px().
  // Herramienta de taller: permite enderezar la imagen sin volver a grabar.
  if (out[0] == 'G') {
    const int r = out[1] - '0';
    gRotacion = (r >= 0 && r <= 3) ? r : 0;
    epdPinesBitBang();
    epdPinesReposo();
    gReady = true;
    dibujaPruebaOrientacion();
    epdFullRefresh();
    // â˜… Se informa del valor que se ha aplicado DE VERDAD, no solo de la etiqueta pintada:
    //   si la 0 y la 1 se vieran igual, aqui tiene que verse por que.
    snprintf(out, n,
             "EPD rotacion APLICADA=%d | bitidx del primer pixel con cada rotacion: "
             "R0=%lu R1=%lu R2=%lu R3=%lu (tienen que ser CUATRO numeros distintos) | "
             "la F tiene que salir con el palo a la IZQUIERDA y las barras a la DERECHA",
             gRotacion,
             (unsigned long)gBitIdxR[0], (unsigned long)gBitIdxR[1],
             (unsigned long)gBitIdxR[2], (unsigned long)gBitIdxR[3]);
    return;
  }
  // 'R' = REPINTAR ahora mismo el estado del nodo, y decir cuantos bytes ha movido.
  // Herramienta de taller: sirve para lanzar un refresco a peticion y ver el resultado sin
  // depender de lo que pasÃ³ en el arranque (que no se ve: abrir el puerto reinicia la placa).
  //
  // â˜…â˜… ES LA PRUEBA DEL REFRESCO PARCIAL (Paso 1, 2026-09-15) â˜…â˜…
  // La primera vez que se llama en un arranque el refresco es COMPLETO (todavia no se sabe
  // que hay pintado). La SEGUNDA vez ya hay plano anterior fiable, asi que el mismo comando
  // hace un PARCIAL: el operador ve la pantalla cambiar sin el parpadeo largo de 2 s. La
  // respuesta dice cual de los dos ha hecho y cuanto ha tardado, que es lo que hay que
  // apuntar en la bitacora.
  if (out[0] == 'R') {
    const uint32_t antes = gBytesBitBang;
    const uint32_t nCompletosAntes = gNCompletos;
    const uint32_t nParcialesAntes = gNParciales;
    const uint32_t t0 = millis();
    epdPinesBitBang();
    epdPinesReposo();
    gReady = true;
    clearBuf(true);
    drawText(6, 6, callSinSSID(), 3);
    hLine(4, EPD_W - 4, 34, 2);
    drawTextCenter(48, "PANTALLA OK", 3);
    drawTextCenter(84, "T-Echo Plus", 2);
    // Prueba visible de que la pantalla se ha repintado: la hora (en segundos desde el
    // arranque) cambia en cada llamada, asi que el parcial tiene pixeles que mover.
    {
      char b[32];
      snprintf(b, sizeof(b), "refresco %lu", (unsigned long)(gNCompletos + gNParciales + 1));
      drawTextCenter(112, b, 2);
      snprintf(b, sizeof(b), "t=%lus", (unsigned long)(millis() / 1000));
      drawTextCenter(140, b, 2);
    }
    drawTextCenter(176, "N0CALL-3  iGate", 1);
    gDirty = true;
    epdFlush();
    const bool fueParcial = (gNParciales > nParcialesAntes) && (gNCompletos == nCompletosAntes);
    snprintf(out, n,
             "EPD repintado %s: bytes=%lu fallos=%lu ms=%lu | parciales=%lu (ultimo %lums) "
             "completos=%lu (ultimo %lums) seguidos=%lu/%lu | PSEL.SCK=0x%08lX CS=%d DC=%d SCK=%d MOSI=%d",
             fueParcial ? "PARCIAL (no debe parpadear)" : "COMPLETO (parpadea, es normal)",
             (unsigned long)(gBytesBitBang - antes),
             (unsigned long)(gErrTimeout + gErrSpi), (unsigned long)(millis() - t0),
             (unsigned long)gNParciales, (unsigned long)gMsUltimoParcial,
             (unsigned long)gNCompletos, (unsigned long)gMsUltimoCompleto,
             (unsigned long)gNParcialesSeguidos, (unsigned long)kMaxParcialesSeguidos,
             (unsigned long)NRF_SPIM2->PSEL.SCK,
             (int)digitalRead(PIN_CS), (int)digitalRead(PIN_DC),
             (int)digitalRead(PIN_SCK), (int)digitalRead(PIN_MOSI));
    return;
  }
  // 'V' = VOLCADO de los registros PIN_CNF de cada pin (herramienta de taller).
  if (out[0] == 'V') { epdVolcadoPines(out, n); return; }
  // 'W' = Â¿MUEVE EL BIT-BANG LOS PINES? Se empuja SCK y MOSI y se LEEN (por eso se dejan
  // con el buffer de entrada conectado). Si no se mueven, no hay transporte posible.
  if (out[0] == 'W') {
    // Contador de discrepancias entre la lectura por registro y digitalRead().
    static uint32_t gDiscrepancia = 0;
    gDiscrepancia = 0;
    epdPinesBitBang();
    epdSpiConfigura();          // suelta el SAADC de P0.31 y reengancha los pines
    epdPinesBitBang();          // y deja el bit-bang mandando

    // â˜…â˜… LA PRUEBA QUE ZANJA LA CONTRADICCION â˜…â˜…
    // El banco de pruebas lee los pines EMPUJADOS con el buffer de entrada DESCONECTADO y
    // le salen altos; aqui, con el buffer CONECTADO, salen bajos. Para saber cual de las
    // dos cosas es la verdad se mide de las dos maneras, y ademas con pull-up:
    //   * si con el buffer DESCONECTADO y pull-up sale 1 -> nadie lo sujeta a masa
    //   * si con el buffer CONECTADO y pull-up sale 0     -> el que lo sujeta es el propio pin
    // Se leen las tres formas: registro IN y digitalRead().
    uint32_t v[5][4];   // [pin][0=sueltoDesc 1=subidaDesc 2=subidaCon 3=empujadoCon]
    const int pines3[3] = {PIN_SCK, PIN_MOSI, PIN_BUSY};
    for (int i = 0; i < 3; i++) {
      const int p = pines3[i];
      // (1) entrada con el buffer DESCONECTADO, sin resistencia
      pinMode(p, INPUT);
      delay(2); v[i][0] = nrf_gpio_pin_read(p);
      // (2) entrada con el buffer DESCONECTADO y pull-up
      pinMode(p, INPUT_PULLUP);
      delay(2); v[i][1] = nrf_gpio_pin_read(p);
      // (3) entrada con el buffer CONECTADO y pull-up
      nrf_gpio_cfg_input(p, NRF_GPIO_PIN_PULLUP);
      delay(2); v[i][2] = nrf_gpio_pin_read(p);
      // (4) empujado a 1 con el buffer CONECTADO
      epdPinSalidaFuerte(p);
      nrf_gpio_pin_set(p);
      delay(2);
      v[i][3] = nrf_gpio_pin_read(p);
      // â˜… Y LA MISMA LECTURA CON digitalRead(), que es lo que usa el core. Si los dos
      //   metodos no dicen lo mismo, el que esta roto es el instrumento (ya ha pasado hoy).
      const int dr = (digitalRead(p) == HIGH) ? 1 : 0;
      if (dr != (int)v[i][3]) gDiscrepancia++;
      v[i][3] |= ((uint32_t)dr << 8);
      pinMode(p, INPUT_PULLUP);
    }
    epdPinesBitBang();
    const uint32_t cnfSck = (uint32_t)NRF_P0->PIN_CNF[PIN_SCK];
    const uint32_t saadcPsel = (uint32_t)NRF_SAADC->CH[0].PSELP;
    snprintf(out, n,
             "EPD mueve? SCK: sueltoDesc=%lu subidaDesc=%lu subidaCon=%lu empujado(reg/dr)=%lu/%lu | "
             "MOSI: %lu/%lu/%lu/%lu/%lu | BUSY: %lu/%lu/%lu/%lu/%lu | discrepancias=%lu | "
             "CNF SCK=0x%08lX SAADC.PSELP=%lu",
             (unsigned long)(v[0][0]), (unsigned long)(v[0][1]), (unsigned long)(v[0][2]),
             (unsigned long)(v[0][3] & 0xFF), (unsigned long)((v[0][3] >> 8) & 0xFF),
             (unsigned long)(v[1][0]), (unsigned long)(v[1][1]), (unsigned long)(v[1][2]),
             (unsigned long)(v[1][3] & 0xFF), (unsigned long)((v[1][3] >> 8) & 0xFF),
             (unsigned long)(v[2][0]), (unsigned long)(v[2][1]), (unsigned long)(v[2][2]),
             (unsigned long)(v[2][3] & 0xFF), (unsigned long)((v[2][3] >> 8) & 0xFF),
             (unsigned long)gDiscrepancia,
             (unsigned long)cnfSck, (unsigned long)saadcPsel);
    return;
  }
  // 'B' = estado del interruptor del panel P1.11: B0 = BAJO, B1 = ALTO, B2 = suelto.
  // Va con el numero en out[1] y despues se vuelve a sondar los pines, que es lo que
  // dice si con ese estado al panel le llega corriente.
  if (out[0] == 'B') {
    const int modo = out[1] - '0';
    if (modo == 0)      { pinMode(PIN_EPD_PWR, OUTPUT); digitalWrite(PIN_EPD_PWR, LOW); }
    else if (modo == 1) { pinMode(PIN_EPD_PWR, OUTPUT); digitalWrite(PIN_EPD_PWR, HIGH); }
    else                { pinMode(PIN_EPD_PWR, INPUT); }
    delay(100);
    epdSondaPines(out, n);
    return;
  }
  // 'C' = PRUEBA DEL CARRUSEL con refresco PARCIAL (Paso 1, 2026-09-15).
  //   `epdparcial N` dibuja N cambios de escena seguidos; cada cambio va con `epdFlush()`,
  //   que elige completo o parcial. El PRIMERO tras el arranque es completo (no hay plano
  //   anterior); los siguientes son parciales y NO deben parpadear.
  //   Es la prueba que pide el Paso 1: sin parcial, cada cambio de escena seria un parpadeo
  //   de 2 s y el carrusel (Paso 2) no tendria sentido.
  //   El primer byte del buffer dice cuantos cambios se quieren (1..9); el numero de escenas
  //   es fijo (kNumEscenas) para que lo que se ve sea exactamente el carrusel de verdad.
  if (out[0] == 'C') {
    // â˜… OJO CON EL NOMBRE: el numero de escenas NO puede llamarse `n` en esta funcion,
    //   porque `n` es el TAMANO DEL BUFFER que viene del que llama. Llamarlo `n` hacia que
    //   el snprintf del final recibiera 3 como tamano de destino y truncara el mensaje
    //   (lo cazo el aviso del compilador, -Wformat-truncation). Se llama `nEscenas`.
    const int nEscenas = (out[1] >= '1' && out[1] <= '9') ? (out[1] - '0') : 3;
    epdPinesBitBang();
    epdPinesReposo();
    gReady = true;

    const uint32_t completosAntes = gNCompletos;
    const uint32_t parcialesAntes = gNParciales;
    const uint32_t tTodo = millis();

    // Primero las escenas que pide el operador, y al final SIEMPRE la de estado (que es la
    // que tiene que quedar en pantalla cuando termine la prueba).
    for (int i = 0; i < nEscenas; i++) {
      gEscena = (uint8_t)(i % kNumEscenas);
      dibujaEscena();          // deja gBuf con la escena y marca gDirty
      epdFlush();
      delay(500);              // que al operador le de tiempo a mirar cada escena
    }
    gEscena = 0;
    dibujaEscena();
    epdFlush();

    const uint32_t completosHechos = gNCompletos - completosAntes;
    const uint32_t parcialesHechos = gNParciales - parcialesAntes;
    snprintf(out, n,
             "EPD carrusel: %lu refrescos (%lu completos + %lu parciales) en %lums | "
             "el ULTIMO ha sido %s (%lums) | parcial mas reciente=%lums | "
             "en los PARCIALES la pantalla no debe parpadear ni ponerse en blanco",
             (unsigned long)(completosHechos + parcialesHechos),
             (unsigned long)completosHechos, (unsigned long)parcialesHechos,
             (unsigned long)(millis() - tTodo),
             (gMsUltimo == gMsUltimoParcial && gMsUltimoParcial > 0) ? "PARCIAL" : "COMPLETO",
             (unsigned long)gMsUltimo, (unsigned long)gMsUltimoParcial);
    return;
  }
  // 'F' = LA HUELLA DEL CONTENIDO y todos sus ingredientes (herramienta de taller).
  //   Se llama dos veces y se comparan los numeros: el que cambie es el que hace que la
  //   pantalla se repinte "sin motivo". Ver `epdHuellaTexto()`.
  if (out[0] == 'F') { epdHuellaTexto(out, n); return; }
  const uint32_t sck = NRF_SPIM2->PSEL.SCK;
  const uint32_t mosi = NRF_SPIM2->PSEL.MOSI;
  // Segundos que le quedan a la pausa del carrusel (0 = sin pausa). Se calcula con una
  // resta con signo: `millis()` da la vuelta a los 49 dias y una resta sin signo daria un
  // numero gigante si el instante ya ha pasado.
  const uint32_t restaPausa = (uint32_t)(int32_t)(gCarruselPausadoHasta - millis());
  const uint32_t pausaSeg = (restaPausa < 0x80000000u) ? (restaPausa / 1000u) : 0u;
  snprintf(out, n,
           "EPD listo=%d transporte=%s rotacion=%d bytesBitBang=%lu | SPIM2 PSEL.SCK=P%d.%d MOSI=P%d.%d ENABLE=%u | "
           "timeouts=%lu errores=%lu ultimoRefresco=%lums | "
           "parciales=%lu (ultimo %lums) completos=%lu (ultimo %lums) seguidos=%lu/%lu "
           "BUSYvisto=%lu BUSYtimeouts=%lu | "
           "carrusel=%s escena=%d (%lus) pausa=%lus | "
           "BUSY=%d CS=%d DC=%d RST=%d PWR_EN=%d REG_EN=%d P1.11=%d",
           gReady ? 1 : 0, gTransporte == 0 ? "bit-bang" : "SPIM2", gRotacion,
           (unsigned long)gBytesBitBang,
           (int)((sck >> 5) & 1), (int)(sck & 0x1F),
           (int)((mosi >> 5) & 1), (int)(mosi & 0x1F),
           (unsigned)(NRF_SPIM2->ENABLE & 1u),
           (unsigned long)gErrTimeout, (unsigned long)gErrSpi, (unsigned long)gMsUltimo,
           (unsigned long)gNParciales, (unsigned long)gMsUltimoParcial,
           (unsigned long)gNCompletos, (unsigned long)gMsUltimoCompleto,
           (unsigned long)gNParcialesSeguidos, (unsigned long)kMaxParcialesSeguidos,
           (unsigned long)gBusyAvisos, (unsigned long)gBusyTimeouts,
           gAutoAvance ? "si" : "no", (int)gEscena,
           (unsigned long)((millis() - gUltimoCambioEscenaMs) / 1000),
           (unsigned long)pausaSeg,
           (int)digitalRead(PIN_BUSY), (int)digitalRead(PIN_CS), (int)digitalRead(PIN_DC),
           (int)digitalRead(PIN_RST), (int)digitalRead(PIN_PWR_EN),
           (int)digitalRead(PIN_REG_EN), (int)digitalRead(PIN_EPD_PWR));
}

bool displayPresent() { return gReady; }
bool displayIsOn() { return gReady; }

// â˜…â˜… VOLCADO DE REGISTROS DE LOS PINES (herramienta de taller) â˜…â˜…
//
// Cuando "un pin no sube" hay que mirar los registros, no suponer. Esto saca, para cada
// pin de la pantalla y para dos pines de control (P0.14 LED y P0.05 libre):
//   * PIN_CNF: DIR (0=entrada 1=salida), INPUT (buffer conectado), PULL, DRIVE
//   * y que se lee con el pin SUELTO, con PULL-UP y con PULL-DOWN
// Si con PULL-UP se lee 0, alguien esta sujetando el pin a masa de verdad (y entonces no
// tiene nada que ver con la configuracion ni con el bit-bang).
struct PinInfo { int pin; const char *nom; };

void epdVolcadoPines(char *out, size_t n) {
  if (!out || !n) return;
  const PinInfo pines[8] = {
      {PIN_SCK, "SCK31"},   {PIN_MOSI, "MOSI29"}, {PIN_CS, "CS30"},
      {PIN_DC, "DC28"},     {PIN_RST, "RST2"},    {PIN_BUSY, "BUSY3"},
      {PIN_LED_BLUE, "LED14"}, {5, "P0.05"}};
  int pos = 0;
  out[0] = '\0';

  for (int i = 0; i < 8; i++) {
    const int p = pines[i].pin;
    const NRF_GPIO_Type *g = (p < 32) ? NRF_P0 : NRF_P1;
    const uint32_t b = p & 31;

    // SUELTO: entrada sin resistencia
    nrf_gpio_cfg_input(p, NRF_GPIO_PIN_NOPULL);
    delay(2);
    const int suelto = nrf_gpio_pin_read(p);
    const uint32_t cnfSuelto = g->PIN_CNF[b];

    // PULL-DOWN: si algo lo empuja a 1, se vera 1
    nrf_gpio_cfg_input(p, NRF_GPIO_PIN_PULLDOWN);
    delay(2);
    const int conBajada = nrf_gpio_pin_read(p);

    // PULL-UP: si algo lo sujeta a masa, saldra 0
    nrf_gpio_cfg_input(p, NRF_GPIO_PIN_PULLUP);
    delay(2);
    const int conSubida = nrf_gpio_pin_read(p);

    // Y EMPUJADO por nosotros a 1 (salida), para ver si gana al que lo sujeta
    nrf_gpio_cfg_output(p);
    nrf_gpio_pin_set(p);
    delay(2);
    const uint32_t cnfSalida = g->PIN_CNF[b];
    nrf_gpio_cfg_input(p, NRF_GPIO_PIN_NOPULL);
    delay(2);
    const int empujado1 = nrf_gpio_pin_read(p);

    pos += snprintf(out + pos, n - pos,
                    "%s suelto=%d bajada=%d subida=%d empujado=%d cnfSuelto=0x%08lX cnfSalida=0x%08lX | ",
                    pines[i].nom, suelto, conBajada, conSubida, empujado1,
                    (unsigned long)cnfSuelto, (unsigned long)cnfSalida);
    if (pos >= (int)n - 120) break;
  }

  // Se dejan como estaban.
  nrf_gpio_cfg_output(PIN_SCK);  nrf_gpio_pin_clear(PIN_SCK);
  nrf_gpio_cfg_output(PIN_MOSI); nrf_gpio_pin_clear(PIN_MOSI);
  nrf_gpio_cfg_output(PIN_CS);   nrf_gpio_pin_set(PIN_CS);
  nrf_gpio_cfg_output(PIN_DC);   nrf_gpio_pin_set(PIN_DC);
  pinMode(PIN_RST, INPUT_PULLUP);
  nrf_gpio_cfg_output(PIN_LED_BLUE); nrf_gpio_pin_set(PIN_LED_BLUE);
}

//
// Â¿Se puede GOBERNAR de verdad cada pin de la pantalla, o algo lo sujeta?
//
// COMO SE MIDE, y esto hay que hacerlo bien (el primer intento salio mal): no vale
// `nrf_gpio_cfg_output()` + leer, porque esa configuracion DESCONECTA el buffer de
// entrada del pin y la lectura sale SIEMPRE 0 (con lo que TODO parecia sujeto a masa,
// incluido el LED azul, que no lo esta). La forma buena es:
//     1) configurar el pin como entrada SIN resistencia y leerlo con el pin suelto
// â˜…â˜… SONDA DE PINES (comando "epdpines") â€” LA PRUEBA QUE SEPARA DOS MUNDOS â˜…â˜…
//
// Â¿Se puede GOBERNAR de verdad cada pin de la pantalla, o algo lo sujeta?
//
// â˜…â˜… DOS TRAMPAS DE INSTRUMENTACION, Y HE CAIDO EN LAS DOS (2026-09-14) â˜…â˜…
//
//  1. `nrf_gpio_cfg_output()` DESCONECTA el buffer de entrada del pin, asi que leer despues
//     da SIEMPRE 0: con eso, TODO parecia sujeto a masa, incluido el LED azul.
//  2. **`nrf_gpio_pin_read()` NO ES FIABLE EN ESTE CORE.** Medido en esta misma placa, con
//     los pines empujados a 1: `nrf_gpio_pin_read()` devuelve 0 y `digitalRead()` devuelve 1
//     **para los mismos tres pines y en el mismo instante** (3 discrepancias de 3). Es
//     exactamente el mismo fallo que el proyecto ya habia documentado para el registro `IN`
//     (docs/HELLO_WORLD_TECHO.md Â§4.1) y en el que yo he vuelto a caer.
//     -> POR ESO AQUI SE LEE SIEMPRE CON `digitalRead()`.
//
// Esto importa porque con el instrumento malo llegue a conclusiones falsas ("los pines
// estan sujetos a masa, al panel no le llega corriente"). Con `digitalRead()` los pines
// **SI se gobiernan** (fallos=0x00): el bus estaba bien y la pantalla no pintaba porque
// SPIM2 se quedaba los pines por PSEL (ver la nota de `epdPinesBitBang()`).
//
// La prueba de CONTROL es el LED azul (P0.14): lleva una resistencia y un LED, no lo sujeta
// ningun chip. Si el LED no da 0/01, el que falla es el METODO y no la pantalla.
void epdSondaPines(char *out, size_t n) {
  if (!out || !n) return;
  // â˜… PRIMERO SE SUELTA EL PERIFERICO DE LOS PINES. Si SPIM2 tiene PSEL apuntando a
  //   P0.31/P0.29, el GPIO no manda ahi y la sonda diria "no obedecen" siendo mentira.
  NRF_SPIM2->ENABLE = 0;
  NRF_SPIM2->PSEL.SCK = 0x80000000u;    // desconectado
  NRF_SPIM2->PSEL.MOSI = 0x80000000u;
  NRF_SPIM2->PSEL.MISO = 0x80000000u;

  const int pines[5] = {PIN_SCK, PIN_MOSI, PIN_CS, PIN_DC, PIN_RST};
  const char *nom[5] = {"SCK", "MOSI", "CS", "DC", "RST"};
  uint32_t fallos = 0;
  char det[180];
  int pos = 0;
  det[0] = '\0';

  for (int i = 0; i < 5; i++) {
    const int p = pines[i];
    // suelto (sin pull) -> si algo lo sujeta, aqui ya se ve
    nrf_gpio_cfg_input(p, NRF_GPIO_PIN_NOPULL);
    delay(2);
    const int suelto = (digitalRead(p) == HIGH) ? 1 : 0;
    // empujado a 0
    nrf_gpio_cfg_output(p);
    nrf_gpio_pin_clear(p);
    delay(2);
    nrf_gpio_cfg_input(p, NRF_GPIO_PIN_NOPULL);
    delay(2);
    const int tras0 = (digitalRead(p) == HIGH) ? 1 : 0;
    // empujado a 1
    nrf_gpio_cfg_output(p);
    nrf_gpio_pin_set(p);
    delay(2);
    nrf_gpio_cfg_input(p, NRF_GPIO_PIN_NOPULL);
    delay(2);
    const int tras1 = (digitalRead(p) == HIGH) ? 1 : 0;
    if (tras0 != 0 || tras1 != 1) fallos |= (1u << i);
    pos += snprintf(det + pos, sizeof(det) - pos, " %s=%d/%d%d", nom[i], suelto, tras0, tras1);
  }

  // Prueba de CONTROL: LED azul P0.14 (tiene que dar 0/01).
  const int pLed = PIN_LED_BLUE;
  nrf_gpio_cfg_input(pLed, NRF_GPIO_PIN_NOPULL);
  delay(2);
  const int ledSuelto = nrf_gpio_pin_read(pLed);
  nrf_gpio_cfg_output(pLed);
  nrf_gpio_pin_clear(pLed);
  delay(2);
  nrf_gpio_cfg_input(pLed, NRF_GPIO_PIN_NOPULL);
  delay(2);
  const int led0 = nrf_gpio_pin_read(pLed);
  nrf_gpio_cfg_output(pLed);
  nrf_gpio_pin_set(pLed);
  delay(2);
  nrf_gpio_cfg_input(pLed, NRF_GPIO_PIN_NOPULL);
  delay(2);
  const int led1 = nrf_gpio_pin_read(pLed);

  // â˜… Â¿QUIEN SUJETA P0.31? La bateria se mide con analogRead(31) en src/sensors.cpp, y el
  //   SCK de la pantalla ES P0.31. Aqui se lee el registro PSELP del SAADC: si apunta a
  //   AIN7, el ADC tiene ese pin cogido (y el ADC apunta a un pin mientras esta activo).
  const uint32_t saadcPsel = (uint32_t)NRF_SAADC->CH[0].PSELP;
  const uint32_t saadcEn = (uint32_t)NRF_SAADC->ENABLE;
  char saadc[80];
  snprintf(saadc, sizeof(saadc), "SAADC ENABLE=%lu CH0.PSELP=%lu%s",
           (unsigned long)saadcEn, (unsigned long)saadcPsel,
           (saadcPsel == SAADC_CH_PSELP_PSELP_AnalogInput7) ? " (Â¡apunta a AIN7 = P0.31!)" : "");

  // Se dejan como estaban (reposo: CS y DC altos, reloj y datos bajos, reset arriba).
  nrf_gpio_cfg_output(PIN_SCK);  nrf_gpio_pin_clear(PIN_SCK);
  nrf_gpio_cfg_output(PIN_MOSI); nrf_gpio_pin_clear(PIN_MOSI);
  nrf_gpio_cfg_output(PIN_CS);   nrf_gpio_pin_set(PIN_CS);
  nrf_gpio_cfg_output(PIN_DC);   nrf_gpio_pin_set(PIN_DC);
  pinMode(PIN_RST, INPUT_PULLUP);
  nrf_gpio_cfg_output(pLed); nrf_gpio_pin_set(pLed);   // LED apagado (activo a nivel bajo)

  snprintf(out, n,
           "EPD pines (suelto/tras0/tras1):%s | CONTROL LED P0.14=%d/%d%d (tiene que ser 0/01) | "
           "PWR_EN=%d REG_EN=%d P1.11=%d | %s | fallos=0x%02X -> %s",
           det, ledSuelto, led0, led1,
           (int)digitalRead(PIN_PWR_EN), (int)digitalRead(PIN_REG_EN),
           (int)digitalRead(PIN_EPD_PWR), saadc, (unsigned)fallos,
           fallos ? ((led0 == 0 && led1 == 1)
                         ? "EL PANEL NO SUBE PERO EL LED SI -> a la pantalla NO le llega corriente"
                         : "ni el LED sube: fallo del METODO, no del panel")
                  : "TODOS SE GOBIERNAN: el bit-bang llega a los pines y al panel le llega corriente");
}

// â˜… SONDA DEL PERIFERICO, A PETICION (comando "epdsonda" del USB) â€” HERRAMIENTA DE TALLER.
//
// Existe porque "la transferencia se atasca" es un sintoma que no dice POR QUE. Esto
// lanza UNA transferencia de 1 byte y va leyendo los registros del periferico, para
// distinguir tres casos MUY distintos:
//   * TASKS_START no hace nada                -> AMOUNT se queda en 1 (no arranca)
//   * arranca pero se queda a medias          -> AMOUNT baja pero no llega a 0
//   * arranca y termina pero nadie se entera  -> AMOUNT=0 y EVENTS_END=0
// Se puede quitar cuando la pantalla este validada: no la usa el firmware.
void epdSonda(char *out, size_t n) {
  if (!out || !n) return;
  static uint8_t buf[8];
  memset(buf, 0xA5, sizeof(buf));

  const uint32_t iEn = (uint32_t)NRF_SPIM2->ENABLE;
  const uint32_t iSck = (uint32_t)NRF_SPIM2->PSEL.SCK;
  const uint32_t iMosi = (uint32_t)NRF_SPIM2->PSEL.MOSI;
  const uint32_t iFreq = (uint32_t)NRF_SPIM2->FREQUENCY;
  const uint32_t iCfg = (uint32_t)NRF_SPIM2->CONFIG;

  epdSpiConfigura();

  // â˜… ENABLE tiene que ser exactamente 1. Se limpia a mano (no con nrf_spim_disable,
  //   que hace read-modify-write y conserva los bits raros).
  NRF_SPIM2->ENABLE = 0;
  NRF_SPIM2->PSEL.SCK = (uint32_t)PIN_SCK;
  NRF_SPIM2->PSEL.MOSI = (uint32_t)PIN_MOSI;
  NRF_SPIM2->PSEL.MISO = (uint32_t)EPD_MISO_PIN;
  NRF_SPIM2->FREQUENCY = SPIM_FREQUENCY_FREQUENCY_M4;
  NRF_SPIM2->CONFIG = (SPIM_CONFIG_ORDER_MsbFirst << SPIM_CONFIG_ORDER_Pos) |
                      (SPIM_CONFIG_CPOL_ActiveHigh << SPIM_CONFIG_CPOL_Pos) |
                      (SPIM_CONFIG_CPHA_Leading << SPIM_CONFIG_CPHA_Pos);
  NRF_SPIM2->EVENTS_END = 0;
  NRF_SPIM2->EVENTS_ENDTX = 0;
  NRF_SPIM2->EVENTS_STARTED = 0;
  NRF_SPIM2->STALLSTAT = 0xFFFFFFFFu;   // limpiar los avisos de atasco de EasyDMA
  NRF_SPIM2->ENABLE = 1;

  const uint32_t jEn = (uint32_t)NRF_SPIM2->ENABLE;
  const uint32_t stall0 = (uint32_t)NRF_SPIM2->STALLSTAT;
  const uint32_t ram0 = (uint32_t)NRF_POWER->RAM[0].POWER;
  const uint32_t ram1 = (uint32_t)NRF_POWER->RAM[1].POWER;
  const uint32_t ram7 = (uint32_t)NRF_POWER->RAM[7].POWER;

  // â˜… Â¿RESPONDE EL PERIFERICO A SUS TAREAS? Prueba que no depende de EasyDMA ni de los
  //   pines: TASKS_SUSPEND tiene que producir EVENTS_STOPPED. Si esto no pasa, el
  //   periferico no esta gobernando NADA (esta apagado o alguien lo tiene tomado).
  NRF_SPIM2->TASKS_SUSPEND = 1;
  const uint32_t evSusp = (uint32_t)NRF_SPIM2->EVENTS_STOPPED;
  NRF_SPIM2->EVENTS_STOPPED = 0;
  NRF_SPIM2->TASKS_RESUME = 1;

  // Otra tarea que no usa datos: TASKS_STOP -> EVENTS_STOPPED.
  NRF_SPIM2->TASKS_STOP = 1;
  const uint32_t evStop = (uint32_t)NRF_SPIM2->EVENTS_STOPPED;
  NRF_SPIM2->EVENTS_STOPPED = 0;

  // â˜… LA PRUEBA QUE DECIDE: transferencia LARGA (5000 bytes) vigilando el contador.
  //   Si el periferico arranca, MAXCNT se queda en 5000 y AMOUNT va bajando; si esta
  //   muerto, los dos se quedan raros y ademas EVENTS_STARTED no sube nunca.
  static uint8_t grande[EPD_BUFSZ];
  const uint32_t t0 = micros();
  NRF_SPIM2->TXD.PTR = (uint32_t)(uintptr_t)grande;
  NRF_SPIM2->TXD.MAXCNT = EPD_BUFSZ;
  NRF_SPIM2->RXD.PTR = 0;
  NRF_SPIM2->RXD.MAXCNT = 0;
  NRF_SPIM2->EVENTS_END = 0;
  NRF_SPIM2->EVENTS_ENDTX = 0;
  NRF_SPIM2->EVENTS_STARTED = 0;
  const uint32_t maxcntTras = (uint32_t)NRF_SPIM2->TXD.MAXCNT;
  epdCs(true);
  NRF_SPIM2->TASKS_START = 1;

  uint32_t am1 = 0, am2 = 0, am3 = 0, st1 = 0, amFin = 0;
  const uint32_t tas = micros();
  while (micros() - tas < 1000) {}          // 1 ms: a 4 MHz eso son ~500 bytes
  am1 = (uint32_t)NRF_SPIM2->TXD.AMOUNT;
  st1 = (uint32_t)NRF_SPIM2->EVENTS_STARTED;
  while (micros() - tas < 2000) {}
  am2 = (uint32_t)NRF_SPIM2->TXD.AMOUNT;
  while (micros() - tas < 3000) {}
  am3 = (uint32_t)NRF_SPIM2->TXD.AMOUNT;
  const uint32_t te = micros();
  while (!NRF_SPIM2->EVENTS_ENDTX && !NRF_SPIM2->EVENTS_END && micros() - te < 50000) {}
  const uint32_t dur = micros() - t0;
  const uint32_t endtx = NRF_SPIM2->EVENTS_ENDTX;
  const uint32_t fin = NRF_SPIM2->EVENTS_END;
  amFin = (uint32_t)NRF_SPIM2->TXD.AMOUNT;

  NRF_SPIM2->TASKS_STOP = 1;
  NRF_SPIM2->EVENTS_END = 0;
  NRF_SPIM2->EVENTS_ENDTX = 0;
  NRF_SPIM2->EVENTS_ENDTX = 0;
  NRF_SPIM2->EVENTS_STOPPED = 0;
  NRF_SPIM2->STALLSTAT = 0xFFFFFFFFu;
  NRF_SPIM2->ENABLE = 0;
  NRF_SPIM2->ENABLE = 1;
  epdCs(false);
  const uint32_t stall = (uint32_t)NRF_SPIM2->STALLSTAT;
  const uint32_t kEn = (uint32_t)NRF_SPIM2->ENABLE;

  snprintf(out, n,
           "EPD sonda: ENABLE %lu->%lu->%lu | PSEL SCK %lu->%lu MOSI %lu->%lu | FREQ 0x%08lX | "
           "FLANCO GPIO SCK=%d MOSI=%d | RAM[0]=0x%08lX | tareas SUSPEND=%lu STOP=%lu | "
           "xfer5000: MAXCNT=%lu AMOUNT a1ms=%lu a2ms=%lu a3ms=%lu fin=%lu | dur=%luus "
           "STARTED=%lu ENDTX=%lu END=%lu | STALL=0x%08lX",
           (unsigned long)iEn, (unsigned long)jEn, (unsigned long)kEn,
           (unsigned long)iSck, (unsigned long)NRF_SPIM2->PSEL.SCK,
           (unsigned long)iMosi, (unsigned long)NRF_SPIM2->PSEL.MOSI,
           (unsigned long)iFreq, (int)digitalRead(PIN_SCK), (int)digitalRead(PIN_MOSI),
           (unsigned long)ram0, (unsigned long)evSusp, (unsigned long)evStop,
           (unsigned long)maxcntTras, (unsigned long)am1, (unsigned long)am2,
           (unsigned long)am3, (unsigned long)amFin, (unsigned long)dur,
           (unsigned long)st1, (unsigned long)endtx, (unsigned long)fin,
           (unsigned long)stall);
}

void displayWake() { gAsleep = false; }

// ---- RETROILUMINACION (P1.11) ----
// Peticion del operador (2026-09-15): la luz de la pantalla se enciende al tocar un boton
// y se apaga sola a los 5 s. P1.11 es el interruptor de la retroiluminacion del T-Echo.
// Se deja APAGADA en reposo (el firmware de fabrica tambien la deja baja) y solo sube
// mientras el temporizador esta vivo.
static bool gLuzOn = false;
static uint32_t gLuzHasta = 0;
static bool gLuzIni = false;
void displayBacklightKick() {
  if (!gLuzIni) { pinMode(PIN_EPD_PWR, OUTPUT); gLuzIni = true; }
  gLuzOn = true;
  gLuzHasta = millis() + 5000;     // 5 s encendida
  digitalWrite(PIN_EPD_PWR, HIGH);
}
void displayBacklightTick(uint32_t nowMs) {
  if (!gLuzOn) return;
  if ((int32_t)(nowMs - gLuzHasta) >= 0) {
    gLuzOn = false;
    digitalWrite(PIN_EPD_PWR, LOW);   // se apaga sola a los 5 s
  }
}

// Pitido corto del buzzer (T-Echo Plus P0.06), sonido audible pero NO agudo (2026-09-15).
// Se hace a mano con ondas cuadradas: ~2,2 kHz durante ~60 ms. Bloquea muy poco. Solo
// existe en el Plus (PIN_BUZZER definido).
void displayBeep() {
#if defined(PIN_BUZZER)
  // â˜… EL ZUMBADOR SOLO SUENA SI ESTA PLACA ES UN PLUS (2026-09-15).
  //   El zumbador es un GPIO normal (P0.06): NO se puede preguntar si existe. Pero el motor
  //   haptico va con el (solo los lleva el Plus) y ese SI se detecta por I2C, asi que se usa
  //   de carne de identidad. En un T-Echo normal el firmware NO toca P0.06 en absoluto: no
  //   se mueve un pin del que no consta a que va en esa placa.
  if (!hapticEsPlus()) return;
  static bool ini = false;
  if (!ini) { pinMode(PIN_BUZZER, OUTPUT); ini = true; }
  const uint32_t t0 = micros();
  const uint32_t periodUs = 450;          // ~2,2 kHz (no muy agudo)
  bool hi = true;
  while ((uint32_t)(micros() - t0) < 60000) {
    digitalWrite(PIN_BUZZER, hi);
    const uint32_t half = periodUs / 2;
    uint32_t w = micros();
    while ((uint32_t)(micros() - w) < half) {}
    hi = !hi;
  }
  digitalWrite(PIN_BUZZER, LOW);          // reposo
#endif
}

// â˜… AVISO SONORO DE BATERIA BAJA (2026-09-15). Melodia descendente "triste", dos frases,
// la segunda mas grave y una nota final larga: el clasico aviso de bateria baja de los
// Nokia viejos (peticion del operador). Se genera igual que el pitido, con ondas cuadradas
// a mano, asi que las notas se cambian AQUI: (frecuencia en Hz, duracion en ms), 0 = pausa.
// BLOQUEA mientras suena (~1,3 s). Solo se llama justo antes de dormir por bateria baja,
// donde ese segundo no importa. En placas sin buzzer (PIN_BUZZER sin definir) no hace nada.
void displayLowBatTone() {
#if defined(PIN_BUZZER)
  // Misma regla que en displayBeep(): sin motor no es un Plus, y sin Plus no hay zumbador.
  if (!hapticEsPlus()) return;
  static bool ini = false;
  if (!ini) { pinMode(PIN_BUZZER, OUTPUT); ini = true; }
  static const uint16_t notas[][2] = {
    {494, 150}, {370, 250}, {0, 90},     // primera frase: si4 -> fa#4
    {494, 150}, {330, 280}, {0, 90},     // segunda, mas grave: si4 -> mi4
    {262, 450},                          // nota final larga y grave ("se acaba")
  };
  for (size_t i = 0; i < sizeof(notas) / sizeof(notas[0]); i++) {
    const uint16_t f  = notas[i][0];
    const uint16_t ms = notas[i][1];
    if (f == 0) { delay(ms); continue; }
    const uint32_t half = (1000000UL / f) / 2;   // medio periodo = un flanco
    const uint32_t t0 = micros();
    bool hi = true;
    while ((uint32_t)(micros() - t0) < (uint32_t)ms * 1000UL) {
      digitalWrite(PIN_BUZZER, hi);
      const uint32_t w = micros();
      while ((uint32_t)(micros() - w) < half) {}
      hi = !hi;
    }
  }
  digitalWrite(PIN_BUZZER, LOW);          // reposo
#endif
}

void displaySleep() {
  // En tinta electronica "apagar" no es borrar: la imagen se queda. Se manda el panel
  // a dormir para que no gaste.
  if (!gReady) return;
  epdCmd1(0x10, 0x01);   // deep sleep
  gAsleep = true;
}

// â˜… PANTALLA DE DORMIDO (2026-09-15): se dibuja ANTES de entrar en System OFF. La tinta es
//   bistable, asi que la imagen queda fijada aunque el nodo este apagado. Muestra: zzz,
//   el indicativo del dueÃ±o EN GRANDE (sin SSID) y el mensaje libre (si lo dejo puesto).
void displaySleepScene() {
  clearBuf(true);
  // zzz dormido (en grande)
  drawTextCenter(40, "zZz Zzz", 3);
  // indicativo del dueÃ±o, en GRANDE (sin SSID), como el splash de inicio
  drawTextCenter(88, callSinSSID(), 4);
  // mensaje libre (telefono u otro), si lo dejo configurado
  if (gCfg && gCfg->sleepMsg[0]) {
    drawTextCenter(150, gCfg->sleepMsg, 2);
  }
  gDirty = true;
  gReady = true;
  // refresco COMPLETO (forzado) para que se vea la escena y luego el nodo duerme.
  epdFullRefresh();
  gDirty = false;
}

void displaySplash() { /* la pantalla se pinta entera en el primer refresco */ }

void displayPinSplash(const char *pin, bool fromStack) {
  (void)fromStack;
  // â˜… EL RELLENO SON 5 GUIONES, NO 6 (2026-09-22): "EMPAREJAR: ------" son 17 caracteres y a
  //   escala 2 el tope son 16 (200 px), o sea 202 px: se salia 2 px por el borde. Con 5 guiones
  //   son 16 exactos y cabe. Lo cazo el comprobador de textos al medir el PEOR CASO de cada
  //   conversion (no al mirar los literales): por eso hay que medir el peor caso.
  //   Un PIN de verdad son 6 digitos, asi que el hueco no se nota: los guiones solo salen
  //   cuando todavia no hay PIN.
  snprintf(gLinea1, sizeof(gLinea1), "EMPAREJAR: %s", pin ? pin : "-----");
  gLinea2[0] = '\0';
  gLineaMs = millis();
  gDirty = true;
  epdFlush();
}
void displayPinSplashClear() { gLinea1[0] = '\0'; gLineaMs = 0; gDirty = true; }
bool displayPinSplashActive() { return false; }

// ===========================================================================
//  â˜…â˜… MENU EN PANTALLA (2026-09-15) â€” PORTADO DE LA OLED a la tinta electronica â˜…â˜…
// ===========================================================================
// Two levels, like the OLED: a list of CATEGORIES, then the items of one category.
//   - Capacitive (P0.11) = navigate (move selection / change edit value)
//   - FÃ­sico corto (P1.10 short) = enter / confirm
//   - FÃ­sico largo (P1.10 long) = go back
// Data model translated 1:1 from display.cpp (see Â§P.11 in the bitÃ¡cora). Values
// are persisted through the SAME path as the `set` command (typedSet + storeSave),
// and actions call the SAME functions as the OLED menu.
namespace {

// ------------------------------------------------------------ menu data -----
enum MenuKind {
  MK_HEADER, MK_INT, MK_BOOL, MK_ENUM, MK_ENUM_F, MK_FLOAT,
  MK_STRING, MK_PATH, MK_ENUM_CYCLE, MK_ACTION,
};
// â˜… `ACT_SHUTDOWN` = APAGAR de verdad, a mano (2026-09-22). Es DISTINTA de `ACT_SLEEP`:
//   dormir despierta al subir la tension (nodo solar); apagar no despierta con nada y solo
//   vuelve con el boton de reset. Ver el comentario largo de `powerShutdownNow()`.
enum { ACT_NONE = 0, ACT_BEACON, ACT_TELEM, ACT_TELEM_META, ACT_MUTE, ACT_SLEEP,
       ACT_SHUTDOWN,
       ACT_REBOOT, ACT_DFU, ACT_RESET, ACT_WIPE, ACT_TRACKER_BEACON, ACT_BAT,
       ACT_SET_COORDS, ACT_GPS_INFO, ACT_MSG_SEND, ACT_PROFILES,
       // â˜… Las filas de la seccion Tracks (2026-09-22). TRES de ellas NO se ejecutan solas: lo
       //   que hacen es ABRIR una de las pantallas del modulo de tracks (ver `pantallaTracks()`),
       //   y cual de ellas se decide alli dentro. Por eso no aparecen en `menuEjecutaAccion`.
       //   â˜… ACT_TRK_NUEVO es la CUARTA y SI se ejecuta aqui: tira el track en vivo y empieza uno
       //     nuevo desde donde estas. Se hace como accion de menu (y no como fila dentro de la
       //     pantalla de tracks) para REUTILIZAR el mecanismo de confirmacion que ya existe para
       //     las acciones destructivas: esto borra algo que el operador ha grabado andando, asi
       //     que no puede pasar a la primera pulsacion.
       ACT_TRACKS, ACT_SLOT, ACT_HOME, ACT_TRK_NUEVO };
struct MenuItem {
  const char *label; MenuKind kind; const char *key;
  int min, max, step;
  const char *const *opts; const int *optVals; const float *optValsF;
  int action; uint8_t modes;
};
constexpr uint8_t kMDigi=0x01, kMTrk=0x02, kMBoth=0x04, kMAll=0x07;

const char *kModeOpts[] = {"Repetidor","Rastreador","Ambos",nullptr};
const int kModeVals[] = {0,1,2};
const char *kSmartOpts[] = {"Sin perfil","A pie","Bici","Coche",nullptr};
const int kSmartVals[] = {0,1,2,3};
const char *kDigiOpts[] = {"Apagado","WIDE1-1","WIDE1+WIDE2",nullptr};
const int kDigiVals[] = {0,1,2};
const char *kTncOpts[] = {"Apagado","TNC2 texto","KISS",nullptr};
const int kTncVals[] = {CFG_TNC_OFF, CFG_TNC_TNC2, CFG_TNC_KISS};
const char *kFreqOpts[] = {"433.775","433.900","868.200",nullptr};
const int kFreqVals[] = {433775000,433900000,868200000};
const char *kBwOpts[] = {"62.5","125","250","500",nullptr};
const float kBwVals[] = {62.5f,125.0f,250.0f,500.0f};
const char *kPathOpts[] = {"0","WIDE1-1","WIDE1-1,WIDE2-1","WIDE1-1,WIDE2-2",
                           "WIDE2-1","WIDE2-2","RFONLY",nullptr};

// â˜…â˜… ICONO DEL MAPA POR PERFIL (2026-09-15) â˜…â˜…
// Los codigos NO se escriben aqui: se LEEN de la configuracion
// (gCfg->profileSymbol[]/profileOverlay[]), que es donde viven los valores de
// fabrica (config.h). Asi, si algun dia se cambia un icono por defecto, el menu y
// el firmware no pueden discrepar. Estas etiquetas son solo para la pantalla.
//   0 = repetidor (/#)   1 = persona (/[)   2 = bici (/b)   3 = coche (/>)
// Origen de los codigos: ver config.h (tabla de WA8LMF + apuntes de overlays de
// aprs.org). El numero de opciones es CUATRO y coincide con el numero de perfiles:
// se puede elegir CUALQUIERA de los cuatro iconos para CUALQUIER perfil.
constexpr int kIconoOpciones = 4;
const char *kIconoNombre[kIconoOpciones] = {"Repetidor","Persona","Bici","Coche"};

const MenuItem kMenu[] = {
  {"Modo", MK_ENUM_CYCLE, "mode", 0,2,1, kModeOpts, kModeVals, nullptr, 0, kMAll},
  {"GPS en repetidor", MK_BOOL, "gpsInDigi", 0,0,0, nullptr, nullptr, nullptr, 0, kMDigi},
  {"Ahorro de GPS", MK_BOOL, "gpsEco", 0,0,0, nullptr, nullptr, nullptr, 0, kMDigi},
  {"Fijar coords", MK_ACTION, nullptr, 0,0,0, nullptr, nullptr, nullptr, ACT_SET_COORDS, kMAll},
  {"Ver GPS", MK_ACTION, nullptr, 0,0,0, nullptr, nullptr, nullptr, ACT_GPS_INFO, kMAll},
  {"Enviar baliza", MK_ACTION, nullptr, 0,0,0, nullptr, nullptr, nullptr, ACT_BEACON, kMAll},
  {"Baliza rastreador", MK_ACTION, nullptr, 0,0,0, nullptr, nullptr, nullptr, ACT_TRACKER_BEACON, kMAll},
  {"Telemetria", MK_ACTION, nullptr, 0,0,0, nullptr, nullptr, nullptr, ACT_TELEM, kMAll},
  {"Baliza cada (min)", MK_INT, "beaconInterval", 15,240,5, nullptr, nullptr, nullptr, 0, kMAll},
  {"Posicion comprimida", MK_BOOL, "compressedPos", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Msg rapido", MK_STRING, "msgText", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Reintentos", MK_INT, "msgRetries", 0,5,1, nullptr, nullptr, nullptr, 0, kMAll},
  {"Tracker cada (seg)", MK_INT, "trackerIntervalSecs", 10,3600,10, nullptr, nullptr, nullptr, 0, kMAll},
  {"Distancia (m)", MK_INT, "trackerMinDistanceM", 0,5000,50, nullptr, nullptr, nullptr, 0, kMAll},
  {"Tiempo min (seg)", MK_INT, "trackerMinSpacing", 10,120,5, nullptr, nullptr, nullptr, 0, kMAll},
  {"Perfil", MK_ACTION, nullptr, 0,0,0, nullptr, nullptr, nullptr, ACT_PROFILES, kMAll},
  {"Enviar altitud", MK_BOOL, "sendAltitude", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Dormir entre", MK_BOOL, "trackerSleep", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Frecuencia", MK_ENUM, "frequency", 0,0,0, kFreqOpts, kFreqVals, nullptr, 0, kMAll},
  {"SF", MK_INT, "spreadingFactor", 5,12,1, nullptr, nullptr, nullptr, 0, kMAll},
  {"CR", MK_INT, "codingRate4", 5,8,1, nullptr, nullptr, nullptr, 0, kMAll},
  {"Ancho banda", MK_ENUM_F, "signalBandwidth", 0,0,0, kBwOpts, nullptr, kBwVals, 0, kMAll},
  {"Potencia", MK_INT, "power", 2,22,1, nullptr, nullptr, nullptr, 0, kMAll},
  {"CAD", MK_BOOL, "cadActive", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Modo digi", MK_ENUM_CYCLE, "digiMode", 0,2,1, kDigiOpts, kDigiVals, nullptr, 0, kMAll},
  {"Blacklist", MK_STRING, "blacklist", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Silenciar", MK_BOOL, "txDisabled", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Indicativo", MK_STRING, "callsign", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Tocall", MK_STRING, "tocall", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Path", MK_STRING, "path", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Path digi", MK_PATH, "pathDigi", 0,0,0, kPathOpts, nullptr, nullptr, 0, kMAll},
  {"Path tracker", MK_PATH, "pathTracker", 0,0,0, kPathOpts, nullptr, nullptr, 0, kMAll},
  {"Path ambos", MK_PATH, "pathBoth", 0,0,0, kPathOpts, nullptr, nullptr, 0, kMAll},
  {"Simbolo", MK_STRING, "symbol", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Comentario", MK_STRING, "comment", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Latitud", MK_FLOAT, "latitude", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Longitud", MK_FLOAT, "longitude", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Consultas", MK_BOOL, "queriesEnabled", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"TNC usb", MK_ENUM_CYCLE, "tncProtocol", 0,2,1, kTncOpts, kTncVals, nullptr, 0, kMAll},
  // == Bluetooth (pareja) ==
  {"Bluetooth", MK_BOOL, "bleEnabled", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"PIN BT", MK_STRING, "blePin", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  // == Sensores ==
  {"Enviar WX", MK_BOOL, "wxSensorActive", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Enviar telem", MK_BOOL, "sendBatteryTelemetry", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Telem cada (min)", MK_INT, "telemetryIntervalMin", 0,720,15, nullptr, nullptr, nullptr, 0, kMAll},
  // â˜… INTERVALO DE METEOROLOGIA (2026-09-15): ajuste NUEVO, justo debajo del de
  //   telemetria y con su mismo formato (MK_INT, 0..720, paso 15: 0 = no automatico,
  //   el resto 15..720 min, igual que la regla de config.cpp). El paquete WX iba fijo
  //   a 15 minutos dentro de main.cpp y no habia manera de tocarlo desde aqui.
  {"WX cada (min)", MK_INT, "wxIntervalMin", 0,720,15, nullptr, nullptr, nullptr, 0, kMAll},
  {"Corr sonda", MK_FLOAT, "temperatureCorrection", -5,5,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Ajuste chip", MK_FLOAT, "chipTempOffset", -10,10,0, nullptr, nullptr, nullptr, 0, kMAll},
  // == Pantalla ==
  {"Auto-avance", MK_BOOL, "sceneAutoAdvance", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Apagar pantalla(s)", MK_INT, "screenTimeoutSecs", 0,3600,15, nullptr, nullptr, nullptr, 0, kMAll},
  {"Avisos", MK_BOOL, "popups", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  // == Energia ==
  {"Corte (mV)", MK_INT, "sleepCutMv", 2500,4200,50, nullptr, nullptr, nullptr, 0, kMAll},
  {"Despertar (mV)", MK_INT, "sleepWakeMv", 2600,4500,50, nullptr, nullptr, nullptr, 0, kMAll},
  {"Ver bateria", MK_ACTION, nullptr, 0,0,0, nullptr, nullptr, nullptr, ACT_BAT, kMAll},
  {"Dormir", MK_ACTION, nullptr, 0,0,0, nullptr, nullptr, nullptr, ACT_SLEEP, kMAll},
  // == Remoto ==
  {"Control remoto", MK_BOOL, "remoteEnabled", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Operadores", MK_STRING, "managers", 0,0,0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Silenciar/act", MK_ACTION, nullptr, 0,0,0, nullptr, nullptr, nullptr, ACT_MUTE, kMAll},
  // == Ajustes ==
  {"Reiniciar", MK_ACTION, nullptr, 0,0,0, nullptr, nullptr, nullptr, ACT_REBOOT, kMAll},
  {"Valores fabrica", MK_ACTION, nullptr, 0,0,0, nullptr, nullptr, nullptr, ACT_RESET, kMAll},
  {"Borrado", MK_ACTION, nullptr, 0,0,0, nullptr, nullptr, nullptr, ACT_WIPE, kMAll},
  // == Tracks (2026-09-22) ==
  // â˜… Las tres filas de la seccion. NO son ajustes: son PUERTAS a las pantallas del modulo de
  //   tracks, y por eso van con `ACT_NONE` y una `key` propia:
  //     - el texto de cada fila lo genera el pintado de seccion (caso especial, mas abajo),
  //       porque cambia segun lo que haya guardado: cuantos puntos lleva el track en vivo, o
  //       cuantas ranuras hay cargadas;
  //     - y al pulsarlas, `menuShort()` las intercepta por su `key` y ABRE la pantalla que
  //       toca, sin pasar por `menuEjecutaAccion` (que es para las acciones de verdad, como
  //       reiniciar o borrar). Con `ACT_NONE` no hay forma de que acaben en el editor.
  // ★★ LAS DOS FILAS DE TRACKS DEL MENU, REORGANIZADAS (2026-10-06) ★★
  //   Antes eran TRES: "Track en vivo", "Ranuras: N/5" y "Volver a casa". Se ha quitado la del
  //   track en vivo, y el motivo lo dijo el operador: **"Empezar nuevo" y la mezcla de cosas no
  //   tenian sentido**. Los TRACKS GRABADOS (incluido el de ahora mismo) se miran DENTRO de
  //   "Volver a casa", que es donde se usan; y aqui arriba se queda lo de siempre: por donde
  //   vuelves, y las rutas que te has bajado.
  {"Volver a casa", MK_ACTION, "tkCasa", 0,0,0, nullptr, nullptr, nullptr, ACT_NONE, kMAll},
  {"Ranuras: -", MK_ACTION, "tkSlots", 0,0,0, nullptr, nullptr, nullptr, ACT_NONE, kMAll},
};
constexpr int kMenuCount = (int)(sizeof(kMenu)/sizeof(kMenu[0]));
const char *kMenuSections[] = {
  "Modo","GPS","Balizas","Mensajes","Tracker","Radio","Digi","APRS",
  // â˜… "Tracks" VA LA ULTIMA A PROPOSITO (2026-09-22): asi solo hay que tocar DOS numeros
  //   (el "Salir" de en medio y el total) en vez de desplazar las 14 secciones de delante.
  //   Y es una seccion de las de verdad: sus filas se pintan con el mismo codigo que las
  //   demas, a la misma escala y con las mismas dos filas de navegacion.
  "Bluetooth","Sensores","Pantalla","Energia","Remoto","Ajustes","Tracks", nullptr
};
// item index of the first item of each section
// â˜… OJO (2026-09-15): estos dos arrays van por INDICE ABSOLUTO dentro de kMenu[], asi
//   que al anadir "WX cada (min)" en Sensores (posicion 44) hay que desplazar +1 todas
//   las secciones siguientes: Pantalla 46->47, Energia 49->50, Remoto 53->54, Ajustes 56->57.
const int kMenuSectionFirst[] = {0,1,5,10,12,18,24,27,39,41,47,50,54,57,60};

// --------------- menu state ---------------
bool gMenuOn = false;
bool gMenuEditing = false;
int gMenuCat = -1;        // -1 = category list, else section index
int gMenuIdx = 0;         // selected row
char gEditBuf[32] = "";   // buffer while editing a value
int gEditPos = 0;
int gMenuEditItemAbs = 0; // indice absoluto del item que se esta editando
uint32_t gMenuLastActMs = 0;  // ultima interaccion con el menu (para el auto-cierre de 15 s)
// â˜… SUBMENU DE PERFILES (2026-09-15): editor dedicado para 1) elegir el perfil activo y
//   2) editar su SSID/tiempos/metros. gMenuPerfMode: 0=off, 1=lista de perfiles,
//   2=editar los campos del perfil seleccionado.
int gMenuPerfMode = 0;
// â˜…â˜… OJO: `gMenuPerfIdx` SIGNIFICA DOS COSAS DISTINTAS SEGUN EL MODO (2026-09-22) â˜…â˜…
//     - modo 1 (lista de perfiles): la FILA resaltada, 0..kPerfFilas-1, o sea
//          0="< Volver", 1="Salir", 2..5=perfiles, 6="Editar ajs."
//          (antes eran 0..4 y el sentido de la fila 0 era OTRO: elegia un perfil)
//     - modo 2 (editando campos): el PERFIL que se edita, 0..3, y NO se usa para pintar filas
//   Por eso hay que tener cuidado al tocarlo: en modo 1 vale hasta 6 y no puede indexar ningun
//   array de 4; en modo 2 siempre vale 0..3. Se reasigna al entrar en cada modo.
int gMenuPerfIdx = 0;
// â˜… CUANTAS FILAS TIENE LA LISTA DE PERFILES (2026-09-22): 2 de navegacion ("< Volver" y
//   "Salir"), los 4 perfiles y la de editar. Vive AQUI, junto al indice, y no mas abajo con el
//   resto del editor, porque `menuShort()` la necesita para el tope del resaltado y esta antes
//   en el fichero. (Definirla abajo fue un error de compilacion: 'was not declared in this
//   scope'.)
constexpr int kPerfFilas = 7;
int gMenuPerfField = 0; // 0=SSID 1=slow 2=fast 3=dist 4=ICONO (ver kPerfCampos)
// â˜… CAMPOS DEL PERFIL (2026-09-15): eran CUATRO y ahora son CINCO. El quinto es el
//   ICONO DEL MAPA, que va con el perfil (config.h: profileSymbol/profileOverlay).
//   El numero se usa en la navegacion (menuNavigate/menuShort) y en el dibujo
//   (menuPinta): si se anade otro campo, se cambia AQUI y en las etiquetas de
//   menuPinta, y los tres sitios siguen de acuerdo.
constexpr int kPerfCampos = 5;
// â˜… SUBMENU DE OPCIONES (2026-09-15): para los enum (Modo, TNC, Frecuencia...) se abre un
//   submenu donde CADA opcion ocupa una linea, con "Volver" y "Salir" encima, la activa
//   marcada con ">", y scroll. Misma letra y estilo que el resto del menu.
int gMenuEnumAbs = -1;      // item cuyo submenu de opciones esta abierto
int gMenuEnumIdx = 0;       // fila del submenu: 0=Volver 1=Salir 2+=opcion
// â˜… CONFIRMACION DE ACCIONES DESTRUCTIVAS (2026-09-15, arreglo de la auditoria G2).
//   Mismo par de variables que la OLED (display.cpp: gConfirmAction/gConfirmUntil): la
//   accion NO se ejecuta a la primera pulsacion; se pide confirmacion y solo la
//   SEGUNDA pulsacion de la MISMA accion dentro de la ventana la lleva a cabo.
//   â˜…â˜… CUAL PULSACION, EN ESTA PANTALLA: LA CORTA (corregido el 2026-09-16; hasta entonces
//      estos comentarios decian "larga", que es justo el gesto que CANCELA). Aqui el mapa es
//      CORTO = entrar/ejecutar/confirmar y LARGO = volver atras, al reves que en la OLED:
//      ver el comentario largo de la pantalla de confirmacion (menuPinta) y menuLong().
//   0 = no hay nada pendiente (ACT_NONE).
int gConfirmAction = 0;
uint32_t gConfirmUntil = 0;

// Valor ACTUAL de la clave `it` como texto (para mostrar a la derecha del Ã­tem).
// Para los enum se muestra la ETIQUETA (opts[i]) que coincide con el valor actual.
const char *menuValorTexto(const MenuItem &it) {
  static char buf[24];
  if (!it.key) { buf[0]='\0'; return buf; }
  JsonDocument doc;
  configToJson(*gCfg, doc.to<JsonObject>());
  JsonVariantConst v = doc[it.key];
  if (v.isNull()) { snprintf(buf,sizeof buf,"?"); return buf; }
  if (it.opts) {
    // buscar la ETIQUETA de la opcion que coincide con el valor actual (int o float)
    if (v.is<int>() && it.optVals) {
      int val = v.as<int>();
      for (int i=0; it.opts[i]; i++) if (it.optVals[i]==val) return it.opts[i];
    } else if ((v.is<float>()||v.is<int>()) && it.optValsF) {
      float val = v.as<float>();
      for (int i=0; it.opts[i]; i++)
        if (fabsf(it.optValsF[i] - val) < 0.001f) return it.opts[i];
    } else if (it.kind == MK_PATH) {
      const char *s = v.as<const char*>();
      for (int i=0; it.opts[i]; i++) if (s && !strcmp(it.opts[i], s)) return it.opts[i];
    }
    // si no hay match exacto (p.ej. valor numerico que no esta en las opts), cae abajo
  }
  if (v.is<bool>())      snprintf(buf,sizeof buf, "%s", v.as<bool>() ? "Si" : "No");
  else if (v.is<float>()||v.is<double>()) snprintf(buf,sizeof buf, "%.2f", v.as<float>());
  else if (v.is<int>())  snprintf(buf,sizeof buf, "%d", v.as<int>());
  else { const char *s=v.as<const char*>(); return s?s:""; }
  return buf;
}

// â˜…â˜… EL PROTOCOLO DEL TNC, EN PALABRA Y NO EN NUMERO (2026-09-15) â˜…â˜…
// La configuracion guarda `tncProtocol` como NUMERO (config.h: 0 = OFF, 1 = TNC2,
// 2 = KISS) y el operador quiere LEERLO en la pantalla como palabra.
//   â˜… LA TABLA ES CORTA A PROPOSITO: la palabra se pinta en una fila de la lista, que
//     va a escala 2, o sea 6*2 = 12 px por caracter y la fila empieza en x=14: caben
//     15 caracteres de los 200 del panel. Con el "TNC: " delante, lo mas largo es
//     "TNC: TNC2" / "TNC: KISS" (9 caracteres = 108 px), asi que sobra sitio. NO se
//     usan las etiquetas largas del submenu ("Apagado", "TNC2 texto"): esas viven en
//     kTncOpts y alli si caben, porque el submenu pinta una opcion por linea.
//   â˜… SE BUSCA POR VALOR, NO POR INDICE: si la configuracion trae un numero que no es
//     ninguno de los tres (lo puede dejar un configurador tocado a mano), se contesta
//     "?" en vez de ensenar una palabra que no le corresponde. El valor se lee de gCfg,
//     que es la MISMA copia que usa el resto del firmware: no hay una segunda verdad.
const char *tncProtocoloPalabra() {
  const int v = gCfg ? (int)gCfg->tncProtocol : (int)CFG_TNC_OFF;
  switch (v) {
    case CFG_TNC_OFF:  return "OFF";
    case CFG_TNC_TNC2: return "TNC2";
    case CFG_TNC_KISS: return "KISS";
    default:           return "?";
  }
}

// Guarda un valor con cliTypedSet + storeSave (mismo motor que `set`, en cli.cpp).
bool menuSave(const char *key, const char *val) {
  if (!gCfg || !key) return false;
  String err;
  bool ok = cliTypedSet(*gCfg, String(key), String(val ? val : ""), err);
  // â˜… EL GUARDADO EN LA FLASH VA APARTE DE LA TRAZA (2026-09-16): estaban en la MISMA
  //   linea (`if (ok) { storeSave(*gCfg); Serial.printf(...); }`) y al agrupar la traza en
  //   el modo diagnostico se lo llevaba por delante. Aqui NO se toca: guardar es la funcion
  //   de esta funcion; lo unico que se calla es el aviso por el USB.
  if (ok) storeSave(*gCfg);
  if (diagTrazaTaller()) {
    if (ok) Serial.printf("MENU %s=%s ok\r\n", key, val?val:"");
    else    Serial.printf("MENU %s=%s ERR %s\r\n", key, val?val:"", err.c_str());
  }
  return ok;
}

// ---- ICONO DEL PERFIL: leer / ciclar / guardar (ver kIconoNombre arriba) -----
// El valor VIVE en gCfg (profileOverlay[i] + profileSymbol[i]), que es lo que lee
// la baliza (aprs.cpp). Aqui NO hay una segunda copia del ajuste: si el operador
// lo cambia desde el configurador web, el menu ensena el valor nuevo en el acto.
//
// â˜… NO se pasa por cliTypedSet()/configFromJson(): `profileSymbol` no es una clave
//   suelta de la configuracion (es un array de cuatro), asi que el motor de `set`
//   no la conoce. Se escribe en la configuracion y se guarda con storeSave(), que
//   es el mismo final del camino (la copia se serializa entera a la flash).
//
// El orden de la rueda (repetidor, persona, bici, coche) es el de kIconoNombre.
const char kIconoCodigo[kIconoOpciones] = {'#', '[', 'b', '>'};
// La tabla de los cuatro por defecto: primaria ('/'). Ver config.h.
const char kIconoTabla[kIconoOpciones] = {'/', '/', '/', '/'};

void iconoPerfilTexto(int perfil, char *out, size_t n) {
  if (!out || n == 0) return;
  out[0] = '\0';
  if (!gCfg || perfil < 0 || perfil > 3) return;
  snprintf(out, n, "%c%c", gCfg->profileOverlay[perfil][0], gCfg->profileSymbol[perfil][0]);
}

int iconoPerfilIndice(int perfil) {
  if (!gCfg || perfil < 0 || perfil > 3) return -1;
  for (int i = 0; i < kIconoOpciones; i++) {
    if (gCfg->profileSymbol[perfil][0] == kIconoCodigo[i]) return i;
  }
  return -1;   // un icono que no es de los cuatro (p.ej. puesto por el configurador)
}

void iconoPerfilPon(int perfil, int idx) {
  if (!gCfg || perfil < 0 || perfil > 3) return;
  if (idx < 0 || idx >= kIconoOpciones) return;
  gCfg->profileSymbol[perfil][0] = kIconoCodigo[idx];
  gCfg->profileSymbol[perfil][1] = '\0';
  // Los cuatro iconos son de la TABLA PRIMARIA ('/'). Se reafirma a proposito: si
  // el perfil traia una tabla rara de una configuracion manipulada, al elegir un
  // icono desde el menu queda coherente con lo que se ve en la pantalla.
  gCfg->profileOverlay[perfil][0] = kIconoTabla[idx];
  gCfg->profileOverlay[perfil][1] = '\0';
}

}  // namespace (datos del menu)

// ---------------------------------------------------------------------------
//  MOTOR DEL MENU (funciones de display.h) â€” NAVEGACION + EDICION + ACCIONES.
//  Mapa de botones (operador, 2026-09-15): capacitivo = navegar / cambiar valor;
//  fisico corto = entrar / confirmar; fisico largo = volver atras. Pinta sobre gBuf.
//  Modelo de edicion: al entrar se edita el item; NAVEGAR (capacitivo) cambia y GUARDA
//  el valor (memoria + flash via typedSet/storeSave); CONFIRMAR (fisico corto) sale de la
//  edicion. Para BOOL, entrar o navegar alterna y guarda; confirmar sale.
// ---------------------------------------------------------------------------

#define kMenuSectores 15   // 14 secciones + el final (indice de kMenuSectionFirst)
// Mismo desplazamiento +1 que en kMenuSectionFirst por el item nuevo "WX cada (min)".
//
// ★★★ AQUI ESTABA EL FALLO QUE TUMBABA EL APARATO (2026-10-06) ★★★
//   Ponia `..., 60, 63, 63` y TENIA QUE PONER `..., 60, 62, 62`.
//   LA CUENTA: la ultima seccion ("Tracks") empieza en la posicion 60. Sus items son 2 (se le
//   quito uno: "Empezar nuevo"), asi que ACABA EN LA 61, y el valor de la tabla —que apunta a
//   "uno mas alla del ultimo"— es **62**. Y hay **62 items** en total (posiciones 0..61), asi que
//   el ultimo valor tambien es 62.
//   ★ CON EL 63, el pintado de la seccion hacia `kMenu[a + iAbs]` con `iAbs` hasta 2 (el bucle va
//     de `r = top` a `total = itemCount + 2 = 4`), o sea **leia `kMenu[62]`, que NO EXISTE**:
//     memoria que no es suya. **El aparato se caia justo al entrar en Tracks**, que es lo que
//     reporto el operador: «entro a menu, bajo a tracks, confirmo, se queda bloqueado».
//   ★ Y LA LECCION: **al quitar una fila hay que tocar la tabla de indices**, y esa tabla es un
//     numero suelto en OTRO sitio del fichero, lejos de la fila que quitas. No lo avisa nadie.
//     Ahora lo vigila `tools/cuenta_items_menu.py`, que cuenta los items de `kMenu[]` y comprueba
//     que ningun indice se salga.
const int kMenuSectionEnd[] = {1,5,10,12,18,24,27,39,41,47,50,54,57,60,62,62};

bool menuIsOpen() { return gMenuOn; }
bool menuIsEditing() { return gMenuEditing; }

static bool menuItemVisible(const MenuItem &it) {
  (void)it;
  // 2026-09-15: se muestran TODOS los items siempre (sin ocultar por modo). La ocultacion
  // por modo era un detalle de la OLED; aqui simplifica el mapeo fila<->item y el operador
  // quiere ver todas las opciones. Si mas adelante se quiere ocultar las que no aplican,
  // se restaura la logica por mascara.
  return true;
}

// â˜…â˜… ITEMS QUE EN ESTA PLACA NO HACEN NADA (2026-09-15, arreglo de la auditoria G3) â˜…â˜…
//
// El problema que arregla: hay tres ajustes que el menu de la tinta GUARDA en la flash y
// que despues NADIE lee en la tinta, asi que el operador los toca, ve que se guardan y cree
// que ha cambiado algo. La OLED ya lo dice por escrito en su tabla ("these two entries are
// INERT values in the stored config: the menu still shows and edits them, nothing reads
// them", refiriendose al Bluetooth): aqui se hace lo mismo, pero ADEMAS se avisa en la
// pantalla, que es donde el operador lo necesita.
//
// Por que no se ocultan: el ajuste es compartido con la OLED y con el configurador web (la
// configuracion vive en la misma flash), y quien tenga las dos placas espera encontrar la
// misma lista. Ocultarlos haria que el menu de la tinta pareciera incompleto.
//
// Se senala por CLAVE y no metiendo un campo nuevo en MenuItem: asi no hay que tocar las 60
// filas de kMenu (que ademas son datos con un orden de inicializacion delicado).
static bool avisoItemInerte(const char *key) {
  if (!key) return false;
  // "Apagar pantalla(s)": la tinta NO tiene apagado por tiempo. En la tinta electronica la
  // imagen es bistable (se queda sin gastar) y displaySleep() solo se usa al dormir el nodo;
  // cfg.screenTimeoutSecs no se lee en NINGUN sitio de este fichero.
  if (!strcmp(key, "screenTimeoutSecs")) return true;
  // "Avisos": cfg.popups solo lo lee la OLED (display.cpp). Aqui los avisos son la banda
  // "TX" y la linea de estado, y no se pueden desactivar con ese ajuste.
  if (!strcmp(key, "popups")) return true;
  // "Bluetooth" y "PIN BT": el BLE esta FUERA DE COMPILACION (src/ble_kiss.cpp.off), asi que
  // los dos valores son inertes en las DOS placas, no solo en esta.
  if (!strcmp(key, "bleEnabled") || !strcmp(key, "blePin")) return true;
  return false;
}

// Texto que se anade a la etiqueta en la lista. â˜… TIENE QUE SER MUY CORTO: las filas se
// pintan a escala 2 (una letra ~12 px en un panel de 200), y "Apagar pantalla(s)" ya mide
// 19 caracteres = 228 px, o sea que se sale por la derecha EL SOLO. Por eso no cabe un
// "(no)" detras: se pone un asterisco de una letra y el significado se explica en la LEYENDA
// que se pinta debajo del titulo de la categoria (ver seccionTieneInerte), que es lo que hace
// que el aviso se entienda sin comerse la etiqueta.
static const char *etiquetaInerte(const char *key) {
  return avisoItemInerte(key) ? " *" : "";
}

// Â¿Esta CATEGORIA tiene algun item inerte? Sirve para pintar la leyenda solo donde hace
// falta, en vez de en las 14 secciones.
static bool seccionTieneInerte(int catAbs) {
  if (catAbs < 0) return false;
  const int a = kMenuSectionFirst[catAbs];
  const int b = kMenuSectionEnd[catAbs];
  for (int i = a; i < b; i++) {
    if (avisoItemInerte(kMenu[i].key)) return true;
  }
  return false;
}

// ---- ACCIONES (menuEjecutaAccion) ----
// â˜… CONFIRMACION: COPIADA DEL PATRON QUE YA FUNCIONA EN LA OLED (ver display.cpp:
//   menuIsDestructive() + gConfirmAction/gConfirmUntil + el manejo en menuLong).
//   En la OLED, la accion que apaga o borra no se ejecuta a la primera: se enseÃ±a
//   "Pulsa largo: confirmar" y solo una SEGUNDA pulsacion larga dentro de 3 s la hace.
//   En la tinta NO existia y una pulsacion larga de mas en "Borrado" borraba la
//   configuracion y reiniciaba sin preguntar (auditoria G2).
//
// OJO CON EL AVISO EN ESTA PANTALLA: cada refresco cuesta ~1,5 s, asi que un popup que
// caduque solo no vale (el operador se lo pierde). Al pedir la confirmacion se PINTA UNA
// PANTALLA DE CONFIRMACION ENTERA (ver menuPinta) y se queda fija; si vence la ventana, el
// aviso desaparece solo en el siguiente refresco.
//
// â˜… LA VENTANA NO SON 3 S COMO EN LA OLED, SINO 15 (2026-09-15). El motivo es la pantalla,
//   no el patron: el aviso tarda ~1,5 s en aparecer (mas ~0,5-1 s hasta que displayRefresh
//   lo manda), asi que con 3 s al operador le quedaban ~1,5 s para LEER el aviso y volver a
//   pulsar: una carrera que se pierde. El patron es el mismo (segunda pulsacion CORTA de la
//   MISMA accion dentro de la ventana), solo cambia el tiempo. 15 s es ademas el mismo plazo
//   que el auto-cierre del menu, asi que el aviso nunca se queda mas rato que el menu.
constexpr uint32_t kConfirmMs = 15000;

// â˜… DORMIR YA NO PIDE CONFIRMACION (peticion del operador, 2026-09-16).
//   Si el operador elige "Dormir" en el menu, el nodo se duerme y ya: la eleccion de la
//   fila del menu ES la confirmacion. Motivo: dormir NO es destructivo (no borra nada, no
//   apaga la radio para siempre y se sale dando al boton), y la pantalla de confirmacion
//   obligaba a dos pulsaciones para algo que se hace a menudo (p. ej. al guardar el
//   nodo en la mochila). El aviso de "Solo sin cable USB" se sigue dando donde toca (dentro
//   del case ACT_SLEEP), asi que si hay cable el operador se entera igual.
//   Lo que SI sigue pidiendo confirmacion: reiniciar, valores de fabrica, borrado total y
//   modo grabacion, que es donde una pulsacion de mas cuesta la configuracion.
//
// â˜…â˜… Y APAGAR SI PIDE CONFIRMACION (2026-09-22) â˜…â˜…
//   Aunque el motivo de "Dormir" (no obligar a dos pulsaciones) tambien valdria aqui, hay una
//   diferencia que manda: **de dormir se sale dando al boton, y de apagado NO** (solo con el
//   boton de RESET o quitando la alimentacion). Una pulsacion de mas en un menu es barata; que
//   el nodo se apague sin querer y haya que ir a buscar el boton de reset, no. Ademas, apagar
//   es lo que se hace justo ANTES de guardarlo, y ahi una confirmacion es lo esperado.
static bool esAccionDestructiva(int act) {
  // â˜… ACT_TRK_NUEVO entra aqui porque BORRA el track en vivo (lo que el operador ha grabado
  //   andando). Es la misma razon que ACT_WIPE, en pequeno.
  return act == ACT_REBOOT || act == ACT_RESET ||
         act == ACT_WIPE || act == ACT_DFU || act == ACT_TRK_NUEVO;
}

// Titulo del aviso de confirmacion. nullptr = esa accion no pide confirmacion.
// (ACT_SLEEP no aparece aqui a proposito: ver el comentario largo de arriba.
//  ACT_SHUTDOWN SI, y por un motivo distinto: de apagar no se sale con el boton.)
static const char *tituloConfirmacion(int act) {
  switch (act) {
    case ACT_SHUTDOWN: return "APAGAR";
    case ACT_REBOOT: return "REINICIAR";
    case ACT_RESET:  return "VALORES FABRICA";
    case ACT_WIPE:   return "BORRADO TOTAL";
    case ACT_DFU:    return "MODO GRABACION";
    case ACT_TRK_NUEVO: return "TRACK NUEVO";
    default:         return nullptr;
  }
}

// Â¿Hay una confirmacion pendiente y todavia dentro de la ventana? La comparacion se hace
// en aritmetica SIN SIGNO (como en la OLED), que es inmune al desbordamiento de millis().
static bool confirmacionPendiente() {
  return gConfirmAction != 0 && (int32_t)(millis() - gConfirmUntil) < 0;
}

static void menuEjecutaAccion(int act) {
  if (!gCfg) return;

  // â˜… PRIMERA pulsacion CORTA: NO se ejecuta, se PIDE CONFIRMACION.
  //   SEGUNDA pulsacion CORTA de la MISMA accion dentro de la ventana: se ejecuta.
  //   (La LARGA, mientras hay confirmacion pendiente, la CANCELA: ver menuLong.)
  //   Cualquier otra accion cancela la peticion pendiente y empieza de cero.
  if (esAccionDestructiva(act)) {
    if (confirmacionPendiente() && gConfirmAction == act) {
      gConfirmAction = 0;        // confirmada: se cae al switch y se ejecuta
    } else {
      gConfirmAction = act;
      gConfirmUntil = millis() + kConfirmMs;
      gLinea1[0] = '\0';         // que el aviso de la pantalla no lo tape un popup viejo
      gLinea2[0] = '\0';
      gDirty = true;
      return;
    }
  } else {
    gConfirmAction = 0;          // una accion normal cancela lo que hubiera pendiente
  }

  switch (act) {
    case ACT_BEACON:
      // â˜… CABE EN EL PANEL (2026-09-22): "KISS: manda la app" son 18 caracteres = 214 px, o
      //   sea 14 px fuera del panel de 200. A escala 2 el tope son 16 caracteres.
      if (tncKissActive() && !tncKissPaused()) { snprintf(gLinea1,sizeof gLinea1,"KISS activo"); }
      else {
        int16_t st = aprsSendManualBeacon(*gCfg);
        snprintf(gLinea1,sizeof gLinea1, st==RADIOLIB_ERR_NONE ? "Baliza OK" : "Baliza ERR");
      }
      break;
    case ACT_TRACKER_BEACON:
      snprintf(gLinea1,sizeof gLinea1, trackerBeaconNow(*gCfg)==RADIOLIB_ERR_NONE ? "Trk beacon" : "Sin fix");
      break;
    case ACT_TELEM:
      snprintf(gLinea1,sizeof gLinea1, aprsSendTelemetry(*gCfg)==RADIOLIB_ERR_NONE ? "Telem OK" : "Telem ERR");
      break;
    case ACT_TELEM_META:
      snprintf(gLinea1,sizeof gLinea1, aprsSendTelemetryMeta(*gCfg)==RADIOLIB_ERR_NONE ? "Meta OK" : "Meta ERR");
      break;
    case ACT_MUTE: {
      gCfg->txDisabled = !gCfg->txDisabled;
      storeSave(*gCfg);
      radioSetMuted(gCfg->txDisabled);
      snprintf(gLinea1,sizeof gLinea1, gCfg->txDisabled ? "MUTE on" : "MUTE off");
      break;
    }
    case ACT_BAT: {
      float bv = sensorsBatteryVolt(gSens);
      // â˜… CABE EN EL PANEL (2026-09-22): "Bat %.2fV INA %.2fV" da 20 caracteres con valores
      //   reales ("Bat 4.03V INA 4.02V") = 240 px, o sea 40 px FUERA de los 200 del panel.
      //   Se quitan las "V" (el campo ya se llama asi) y el espacio de "INA":
      //   "Bat 4.03 INA4.02" son 17 caracteres = 202 px... todavia 2 de mas, asi que los
      //   voltios van con un decimal: "Bat 4.0 INA4.0" = 16 caracteres exactos = 190 px. Cabe.
      snprintf(gLinea1,sizeof gLinea1, "Bat %.1f INA%.1f", (double)bv, (double)gSens.inaBusV);
      break;
    }
    case ACT_GPS_INFO: {
      const GpsData &g = gpsGet();
      if (g.fix) snprintf(gLinea1,sizeof gLinea1, "fix %u %.5f", (unsigned)g.sats, g.lat);
      else snprintf(gLinea1,sizeof gLinea1, "GPS sin fix");
      break;
    }
    case ACT_SET_COORDS:
      // â˜… FIJAR COORDS EN LA TINTA (2026-09-15): antes esto solo pintaba "No
      //   soportado en tinta" y no hacia nada. Ahora se lanza LA MISMA sesion que
      //   ya funcionaba en la OLED: la lleva el rastreador (trackerSetCoordsStart,
      //   ver tracker.h), porque es quien gobierna el GPS y quien decide cuando la
      //   posicion esta asentada. Esta pantalla solo da la orden, ensena el
      //   progreso (pintaSesionCoords) y guarda al final (displaySaveCoords).
      //
      //   â˜… AQUI HABIA UNA COMPROBACION PREVIA QUE YA NO HACE FALTA (quitada el
      //   2026-09-15): si el nodo estaba en modo repetidor sin "GPS en repetidor",
      //   se avisaba "activa GPS en repetidor" y NO se arrancaba la captura. Aquel
      //   aviso existia porque la sesion no podia encender el GPS por su cuenta.
      //   Ahora SI puede (la sesion manda sobre el modo; ver el guardia de
      //   gpsManage() en tracker.cpp), asi que pedirle al operador que activara un
      //   ajuste a mano era pedirle algo que el aparato hace solo. Se arranca
      //   directamente y el GPS se enciende y se apaga solo.
      trackerSetCoordsStart();
      coordsPantallaInicia();
      break;
    case ACT_MSG_SEND: {
      String to = aprsLastHeardCall();
      if (gCfg->msgText[0]=='\0') snprintf(gLinea1,sizeof gLinea1, "Mensaje vacio");
      else if (to.length()==0) snprintf(gLinea1,sizeof gLinea1, "Nadie escuchado");
      else snprintf(gLinea1,sizeof gLinea1, aprsSendMessage(*gCfg, to.c_str(), gCfg->msgText)==RADIOLIB_ERR_NONE ? "Enviado" : "Fallo TX");
      break;
    }
    case ACT_SLEEP: {
      if (powerUsbPresent()) {
        // â˜… Con el USB conectado NO se dibuja la pantalla fija de dormido (dejaria el panel
        //   con el zzz pegado y al soltar el cable un estado raro): se avisa con un popup.
        //   Detalle que pidio el operador (2026-09-15).
        // â˜… CABE EN EL PANEL (2026-09-22): "Solo sin cable USB" son 18 caracteres = 214 px,
        //   14 px fuera del panel de 200. Con "Solo sin USB" son 12 = 142 px y dice lo mismo.
        //   (El popup de abajo si puede ser largo: `displayPopupWait` ya lo parte en dos lineas.)
        snprintf(gLinea1, sizeof gLinea1, "Solo sin USB"); gLinea2[0] = '\0';
        displayPopupWait("Disponible solo sin cable USB", 3000);   // ~3 s, diseno hermosa
      } else {
        displaySleepScene();   // escena de dormido fija (tinta bistable) y System OFF
        powerSleepNow(*gCfg);
      }
      break;
    }
    case ACT_REBOOT: NVIC_SystemReset(); break;

    // â˜…â˜… APAGAR DE VERDAD, a mano (2026-09-22) â˜…â˜…
    //   NO mira el USB a proposito: es UN SOLO CAMINO con cable y sin cable, igual que el
    //   firmware de referencia del T-Echo (cfr34k). Asi el nodo no tiene que "decidir" ninguna
    //   escena, que es justo la duda que preocupaba al operador. Ver `powerShutdownNow()`.
    //
    //   Secuencia pensada para que el operador VEA que se apaga, no para que sea instantaneo:
    //     1. melodia (el mismo tono del aviso de bateria baja: se reconoce sin mirar),
    //     2. pantalla "APAGADO" con el porque, 4 s, que quede escrita en la tinta,
    //     3. y solo entonces se corta la corriente.
    //   El texto se pinta en UNA linea a escala 2 ("APAGADO A MANO" son 14 caracteres, caben):
    //   si algun dia se alarga, `pintaAviso()` lo parte en dos o lo recorta, nunca se sale.
    // â˜… EMPEZAR TRACK NUEVO (2026-09-22): tira el track en vivo y empieza uno DESDE
    //   AQUI. Sin esto no habia forma de empezar de cero, y "Volver a casa" te mandaba
    //   al punto mas antiguo del anillo (tu casa), no a donde dejaste el nodo.
    //   Llega hasta aqui SOLO tras la segunda pulsacion corta (es destructiva).
    //   â˜…â˜… Y VA GUARDADO POR `TRACKS_DISPONIBLE`, QUE ES OBLIGATORIO â˜…â˜…
    //     La API de tracks (`tracksVivoOlvida`, `tracksGuiaActivo`...) solo EXISTE en el T-Echo
    //     (ver el `#if` de tracks.h). Sin esta guarda, en las Faketec -- que no tienen ni el
    //     codigo ni las funciones -- este `case` no compila. Se descubrio al construir el entorno
    //     de diagnosis `techo_plus_diag_sintracks`, que deja el modulo fuera a proposito.
#ifdef TRACKS_DISPONIBLE
    case ACT_TRK_NUEVO: {
      tracksVivoOlvida();
      // Y si se estaba guiando, se para: el guiado apuntaba al track viejo, que acaba de
      // desaparecer. Dejarlo en marcha seria guiar hacia un track que ya no existe.
      if (tracksGuiaActivo()) tracksGuiaTermina();
      displayPopupWait("Track nuevo desde aqui", 2500);
      return;
    }
#endif
    case ACT_SHUTDOWN: {
      displayLowBatTone();                 // se reutiliza: es el tono de "me voy a apagar"
      snprintf(gLinea1, sizeof gLinea1, "APAGADO A MANO");
      gLinea2[0] = '\0';
      displayPopupWait("APAGADO: vuelve con RESET", 4000);
      powerShutdownNow(*gCfg);             // no vuelve
      break;
    }

    // ACT_DFU no tiene fila en kMenu (en esta placa el modo grabacion se pide por USB con
    // "dfu confirm", ver cli.cpp). La rama se deja porque esAccionDestructiva() ya lo
    // contempla y asi la lista de acciones destructivas queda completa, como en la OLED.
    case ACT_DFU: break;
    case ACT_PROFILES:
      // Abre el editor de perfiles (estado dedicado del menu de la tinta).
      gMenuPerfMode = 1; gMenuPerfIdx = 0; gMenuPerfField = 0;
      gMenuLastActMs = millis();
      gDirty = true;
      break;
    case ACT_RESET: {
      // Mismo camino que la OLED (display.cpp, ACT_RESET), pero con el terminador
      // EXPLICITO: aqui `strncpy` no lo ponia y luego se leia keepCall/keepMgrs con
      // strncpy otra vez, que recorre el origen hasta el '\0' (lectura fuera del buffer
      // si el indicativo o los operadores llenaban el campo entero).
      char keepCall[16], keepMgrs[64]; bool keepRemote = gCfg->remoteEnabled;
      strncpy(keepCall, gCfg->callsign, sizeof(keepCall) - 1);
      keepCall[sizeof(keepCall) - 1] = '\0';
      strncpy(keepMgrs, gCfg->managers, sizeof(keepMgrs) - 1);
      keepMgrs[sizeof(keepMgrs) - 1] = '\0';
      *gCfg = DigiConfig();
      strncpy(gCfg->callsign, keepCall, sizeof(gCfg->callsign) - 1);
      gCfg->callsign[sizeof(gCfg->callsign) - 1] = '\0';
      strncpy(gCfg->managers, keepMgrs, sizeof(gCfg->managers) - 1);
      gCfg->managers[sizeof(gCfg->managers) - 1] = '\0';
      gCfg->remoteEnabled = keepRemote;
      storeSave(*gCfg);
      snprintf(gLinea1,sizeof gLinea1, "Reset OK");
      break;
    }
    case ACT_WIPE:
      storeWipe(); *gCfg = DigiConfig(); storeSave(*gCfg); NVIC_SystemReset();
      break;
    default: break;
  }
  gLineaMs = millis();
  gDirty = true;
}

// ---- EDICION ----
static void comenzarEdicion(const MenuItem &it) {
  gMenuEditing = true;
  // Carga el valor ACTUAL en el buffer (ARREGLO G1 de la auditoria 2026-09-15): sin esto,
  // editar un string (callsign, symbol, msgText...) arrancaba vacio y no dejaba ver/corregir
  // lo que hay. Se rellena aqui para todos los tipos editables; para string/float se usa.
  strncpy(gEditBuf, menuValorTexto(it), sizeof(gEditBuf) - 1);
  gEditBuf[sizeof(gEditBuf) - 1] = '\0';
  gEditPos = 0;
  gDirty = true;
}

// â˜…â˜… EL NUMERO QUE SE VE TIENE QUE SER EL QUE SE ACABA DE GUARDAR (2026-09-15) â˜…â˜…
// HALLAZGO al anadir el RESTAR (ver editaResta): `gEditBuf` solo se rellena al ENTRAR en
// el item (comenzarEdicion) y al girar la rueda de letras (MK_STRING). En los numeros
// quien guarda es menuSave() directamente en la configuracion, y NADIE volvia a escribir
// el buffer: la pantalla de edicion (menuPinta, "drawTextCenter(70, gEditBuf, 3)") seguia
// enseÃ±ando el valor de cuando se entro, asi que se podia tocar veinte veces y el numero
// grande no se movia aunque la configuracion si cambiaba. Con el sumar eso ya era malo;
// con el restar es peor: el operador no puede ver a donde va ni comprobar que la
// correccion a la baja ha entrado.
// Se rellena desde la MISMA copia que se acaba de guardar (gCfg, a traves de
// menuValorTexto, que es de donde salia el texto al entrar), asi que pantalla y
// configuracion no pueden discrepar. Y si menuSave() ha RECHAZADO el valor (fuera de
// rango para configFromJson), el buffer enseÃ±a el que de verdad hay, no el intento.
// Solo se llama en los tipos NUMERICOS: el texto tiene su propio buffer (la rueda).
static void refrescaBufferNumerico(const MenuItem &it) {
  snprintf(gEditBuf, sizeof gEditBuf, "%s", menuValorTexto(it));
}

// Cambia el valor del item (ediciÃ³n live) y GUARDA.
static void editaSiguiente(const MenuItem &it) {
  JsonDocument doc;
  configToJson(*gCfg, doc.to<JsonObject>());
  char buf[24];
  switch (it.kind) {
    case MK_BOOL: {
      bool b = doc[it.key] | false;
      menuSave(it.key, b ? "0" : "1");
      break;
    }
    case MK_INT: {
      long v = (long)(doc[it.key] | 0);
      long step = it.step ? it.step : 1;
      long nv = v + step; if (nv > it.max) nv = it.min; if (nv < it.min) nv = it.min;
      snprintf(buf,sizeof buf,"%ld",nv); menuSave(it.key, buf);
      break;
    }
    // â˜…â˜… TOPE DE RANGO EN LOS FLOAT (2026-09-15, arreglo de la auditoria G4) â˜…â˜…
    //   Antes: `float nv = v + step;` a secas. No habia tope por ARRIBA ni por ABAJO, y
    //   ademas menuSave() NO valida rangos (guarda directo en gCfg + storeSave, a
    //   diferencia de cliTypedSet()/configFromJson(), que si validan). Resultado: con
    //   "Corr sonda" (limite +-5) o "Ajuste chip" (limite +-10) unos pocos toques dejaban
    //   un valor fuera de rango que el firmware aceptaba, y que luego el configurador web
    //   RECHAZABA al leerlo ("chipTempOffset -10..10"): la configuracion quedaba en un
    //   estado que el propio firmware no habria aceptado por el camino normal.
    //   Ahora se respeta el [min,max] del item: si el paso se sale por arriba, el valor da
    //   la vuelta al minimo (igual que hace el MK_INT de arriba y que la OLED), y si el
    //   valor guardado ya venia fuera de rango se mete dentro en el primer toque.
    //   Si un item no declara limites (min==max==0), no se toca nada: es el caso de
    //   Latitud/Longitud, que se validan en configFromJson (-90..90 / -180..180).
    case MK_FLOAT: {
      float v = doc[it.key] | 0.0f;
      float step = it.step ? (float)it.step : 0.1f;
      float nv = v + step;
      if (it.max > it.min) {                      // el item SI declara limites
        const float fmin = (float)it.min, fmax = (float)it.max;
        if (v < fmin || v > fmax) nv = fmin;      // venia fuera de rango: se mete dentro
        else if (nv > fmax) nv = fmin;            // tope por arriba: vuelve al minimo
        else if (nv < fmin) nv = fmin;            // tope por abajo
      }
      snprintf(buf,sizeof buf,"%.1f",(double)nv); menuSave(it.key, buf);
      break;
    }
    case MK_ENUM_CYCLE: case MK_ENUM: case MK_ENUM_F: {
      int n = 0; while (it.opts && it.opts[n]) n++;
      int idx = 0;
      if (it.optValsF && !it.optVals) {
        // opciones FLOAT (p.ej. ancho de banda): buscar el indice del valor float actual
        float v = doc[it.key] | 0.0f;
        for (int i=0;i<n;i++) if (fabsf(it.optValsF[i]-v) < 0.001f) { idx=i; break; }
        int ni = n ? (idx+1) % n : 0;
        // ARREGLO G3 (auditoria 2026-09-15): escribir el float SIN truncar (62.5, no "62"),
        // o la validacion de configFromJson (exige 62.5/125/250/500 exactos) lo rechaza.
        char fbuf[16];
        snprintf(fbuf,sizeof fbuf,"%.1f",(double)it.optValsF[ni]);
        menuSave(it.key, fbuf);
      } else if (it.optVals) {
        long v = (long)(doc[it.key] | 0);
        for (int i=0;i<n;i++) if (it.optVals[i]==v) { idx=i; break; }
        int ni = n ? (idx+1) % n : 0;
        snprintf(buf,sizeof buf,"%d", it.optVals[ni]);
        menuSave(it.key, buf);
      } else {
        long v = (long)(doc[it.key] | 0);
        idx = (v >= 0 && v < n) ? v : 0;
        int ni = (idx+1) % n;
        snprintf(buf,sizeof buf,"%d", ni);
        menuSave(it.key, buf);
      }
      break;
    }
    case MK_PATH: {
      const char *cur = menuValorTexto(it);
      int n=0; while (it.opts && it.opts[n]) n++;
      int idx=0; for (int i=0;i<n;i++) if(!strcmp(it.opts[i],cur)) { idx=i; break; }
      int ni = n ? (idx+1)%n : idx;
      menuSave(it.key, it.opts[ni]);
      break;
    }
    case MK_STRING: {
      // incrementar un carÃ¡cter: se guarda en gEditBuf (sin persistir hasta confirmar)
      //
      // â˜…â˜… COMPROBADO AL ARREGLAR G5 (2026-09-15): EL SIGNO MENOS YA ESTABA AQUI â˜…â˜…
      //   '-' figura en la rueda desde el principio (entre el '9' y el '/'), asi que los
      //   campos de TEXTO ya admitian negativos. Lo que NO admitia negativos era el editor
      //   de NUMEROS: MK_FLOAT no usa esta rueda, usa el atajo de "sumar 0.1" (ver
      //   editaSiguiente), y por eso "Latitud"/"Longitud"/"Corr sonda"/"Ajuste chip" se
      //   quedaban clavados en positivo. El arreglo de G5 es, por tanto, en el formato y
      //   en el rango de MK_FLOAT (y en el min/max de esos items en kMenu), NO aqui.
      //   Se deja escrito para que nadie vuelva a "anadir el menos" a esta rueda: ya esta.
      static const char kC[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/#.,_:~";
      String s = gEditBuf;
      if ((int)s.length() <= gEditPos) s += ' ';
      char c = s[gEditPos];
      int ci = 0; for (; kC[ci] && kC[ci]!=c; ci++);
      s[gEditPos] = kC[(ci+1) % (int)(sizeof(kC)-1)];
      strncpy(gEditBuf, s.c_str(), sizeof(gEditBuf)-1); gEditBuf[sizeof(gEditBuf)-1]='\0';
      gDirty = true;
      break;
    }
    case MK_HEADER: case MK_ACTION: break;
  }
  // â˜… (2026-09-15) Que el numero grande de la pantalla de edicion sea el que se acaba de
  //   guardar (ver refrescaBufferNumerico). En el texto NO se toca el buffer: lo construye
  //   la rueda de letras y se persiste al confirmar con el fisico corto.
  if (it.kind == MK_INT || it.kind == MK_FLOAT) refrescaBufferNumerico(it);
}

// â˜…â˜… RESTAR EN LOS AJUSTES NUMERICOS (2026-09-15, peticion del operador) â˜…â˜…
//
// EL PROBLEMA: el editor de numeros solo sumaba. El toque CAPACITIVO llama a
// menuNavigate() -> editaSiguiente(), y eso siempre suma un paso (+0,1 en los FLOAT),
// asi que un ajuste que empieza en 0 (Latitud, Longitud, Corr sonda, Ajuste chip) NO
// podia bajar a negativo desde el aparato: habia que ir al configurador web o al cable.
//
// GESTO ELEGIDO: DOBLE TOQUE FISICO mientras se esta editando. Mapa de gestos que hay
// hoy (button.cpp + main.cpp + este fichero) y por que ese y no otro:
//   - CAPACITIVO (menuNavigate, main.cpp:73-83): navega y suma el paso. Ahi solo llega
//     el FLANCO del toque (no su duracion), asi que no se puede distinguir toque corto
//     de toque largo: no hay sitio para un segundo sentido sin tocar main.cpp.
//   - FISICO CORTO (menuShort): entra en el item, CONFIRMA y sale de la edicion (y
//     persiste el texto). Es el gesto que mas se usa: no se le toca.
//   - FISICO LARGO (menuLong): volver atras, el gesto de "volver" de TODO el menu. En la
//     edicion hace exactamente lo mismo que menuEditCancel() (gMenuEditing = false).
//   - DOBLE TOQUE (menuEditCancel): hoy solo CANCELA la edicion, y main.cpp:117-120 lo
//     entrega SOLO si se esta editando (`if (menuIsEditing()) menuEditCancel();`).
// El doble toque es, por tanto, el gesto que MENOS conflictos crea: 1) fuera de la
// edicion no llega a este codigo, asi que no puede disparar nada de otras pantallas, y
// 2) NO quita ninguna capacidad, porque salir de la edicion sin guardar sigue estando en
// el toque LARGO con el mismo efecto (menuLong). Al corto y al largo no se les toca nada.
//   â˜… CONFLICTO CONOCIDO (se dice, no se esconde): el toque corto tarda hasta 800 ms en
//     resolverse (button.cpp: kClickWindowMs espera a ver si el toque es doble). Si el
//     operador toca una vez para confirmar y, al no ver nada, toca otra vez, eso ES un
//     doble toque: en vez de confirmar, resta un paso. No se pierde nada (sigue dentro de
//     la edicion y sale con el largo), y la pantalla de edicion avisa del gesto (ver
//     menuPinta). ALTERNATIVA si aun asi molesta: dejar el doble toque como cancelar y
//     poner el RESTAR en el toque LARGO; se descarta porque el largo dejaria de ser
//     "volver" justo en la pantalla donde mas se usa.
//   â˜… SOLO NUMEROS: en texto y path el doble toque sigue CANCELANDO la edicion (restar
//     texto no tiene sentido). Lo decide menuEditCancel(), que es quien filtra.
//
// TOPES: los MISMOS it.min/it.max de kMenu que usa el sumar, con su misma regla de rueda:
// si el paso se sale por abajo, el valor da la vuelta al maximo, igual que al sumar da la
// vuelta al minimo cuando se pasa por arriba (regla que ya estaba y NO se toca). El valor
// NUNCA queda fuera de [min,max], y si la configuracion traia un valor ya fuera de rango
// (lo puede dejar un configurador), el primer doble toque lo mete dentro. Los items sin
// limites declarados (min==max==0: Latitud/Longitud) se validan en configFromJson
// (-90..90 / -180..180), igual que al sumar.
static void editaResta(const MenuItem &it) {
  JsonDocument doc;
  configToJson(*gCfg, doc.to<JsonObject>());
  char buf[24];
  switch (it.kind) {
    case MK_INT: {
      long v = (long)(doc[it.key] | 0);
      long step = it.step ? it.step : 1;
      long nv = v - step;
      if (it.max > it.min) {                      // el item SI declara limites
        if (v < it.min || v > it.max) nv = it.max;   // venia fuera de rango: se mete dentro
        else if (nv < it.min) nv = it.max;           // tope por abajo: vuelve al maximo
        else if (nv > it.max) nv = it.max;           // red de seguridad
      }
      snprintf(buf,sizeof buf,"%ld",nv); menuSave(it.key, buf);
      break;
    }
    case MK_FLOAT: {
      float v = doc[it.key] | 0.0f;
      float step = it.step ? (float)it.step : 0.1f;
      float nv = v - step;
      if (it.max > it.min) {                      // el item SI declara limites
        const float fmin = (float)it.min, fmax = (float)it.max;
        if (v < fmin || v > fmax) nv = fmax;      // venia fuera de rango: se mete dentro
        else if (nv < fmin) nv = fmax;            // tope por abajo: vuelve al maximo
        else if (nv > fmax) nv = fmax;            // red de seguridad
      }
      snprintf(buf,sizeof buf,"%.1f",(double)nv); menuSave(it.key, buf);
      break;
    }
    default: break;   // texto/path/acciones no se restan (menuEditCancel los filtra antes)
  }
  // â˜… (2026-09-15) Mismo remate que en editaSiguiente: lo que se ve tiene que ser lo que
  //   se acaba de guardar, o el operador resta a ciegas (ver refrescaBufferNumerico).
  if (it.kind == MK_INT || it.kind == MK_FLOAT) refrescaBufferNumerico(it);
}

// ---- NAVEGACIÃ“N / edit ----
// Modelo de filas (2026-09-15):
//   MENU PRINCIPAL (gMenuCat<0): filas = [Salir] + secciones[0..mid-1] + [Salir] +
//     secciones[mid..].  mid = n/2. Con n filas de secciones hay n+2 filas en total.
//   SUBMENU (gMenuCat>=0): filas = [Volver atras] + [Salir] + items.
static int menuMainSecCount() { int n=0; for(; kMenuSections[n]; n++) {} return n; }
// â˜…â˜… MODELO DEL MENU PRINCIPAL, con TRES filas virtuales delante (2026-09-22) â˜…â˜…
//   [0] Salir, [1] Dormir, [2] Apagar, secciones[0..mid-1], [kSalirMid] Salir, secciones[rest].
//
//   â˜… OJO AL TOCAR ESTO: `menuMainFilaSeccion()` traduce fila -> seccion, y su aritmetica
//     depende de CUANTAS filas virtuales hay delante. Antes eran DOS (Salir y Dormir) y valia
//     `(r < mid) ? (r-2) : (r-3)`. Al anadir "Apagar" pasan a ser TRES, asi que TODOS los
//     desplazamientos suben uno: `mid`, el total, y los dos tramos de la traduccion. Si alguno
//     se queda atras, el menu ensena la seccion equivocada o se salta una, y no avisa.
//   El "Salir" de en medio sigue yendo a `mid`, para que la lista no quede larga sin salida.
static int kSalirMid(int n)           { return 3 + n/2; }
static int menuMainTotFilas(int n)    { return n + 4; }
static bool menuMainFilaSalir(int r, int n) { return r==0 || r==kSalirMid(n); }
static bool menuMainEsDormir(int r)          { return r==1; }
static bool menuMainEsApagar(int r)          { return r==2; }
static int  menuMainFilaSeccion(int r, int n) {
  if (menuMainFilaSalir(r, n) || menuMainEsDormir(r) || menuMainEsApagar(r)) return -1;
  int mid = kSalirMid(n);
  return (r < mid) ? (r-3) : (r-4);
}

// ===========================================================================
//  â˜…â˜…â˜… PANTALLAS DE TRACKS (2026-09-22) â˜…â˜…â˜…
//
//  Van aparte del menu de ajustes porque no son ajustes: son pantallas de TRABAJO con datos
//  que cambian (cuantos puntos lleva el track, que ranuras hay cargadas). Pero se pintan con
//  **el mismo estilo exacto** que el resto del menu, que es lo que pidio el operador:
//    - titulo a escala 1 arriba,
//    - fila 0 "< Volver" y fila 1 "Salir",
//    - filas a escala 2, con recuadro en la seleccionada,
//    - y el auto-cierre de los 15 s, igual que el menu.
//
//  EL ESTADO ES PROPIO Y PEQUENO (`gTrkPant` + `gTrkFila`), para no mezclarlo con el del menu
//  de ajustes. Se entra desde la seccion Tracks del menu (ver `menuShort`).
// ===========================================================================
#ifdef TRACKS_DISPONIBLE
// ★ DECLARACION PREVIA (2026-10-06): las pantallas de tracks VUELVEN al menu con esta funcion, y
//   esta definida mas abajo (junto a `menuOpen`). Sin declararla aqui, el compilador no la conoce
//   en este punto y el fichero no compila.
void menuOpenEnSeccion(int seccion);

enum TrkPant : uint8_t {
  TRK_PANT_NADA = 0,      // las pantallas de tracks NO estan abiertas
  TRK_PANT_LISTA,         // lista: track en vivo, las 5 ranuras, volver a casa
  TRK_PANT_ACCION,        // tras elegir un track: hacia adelante / atras / finalizar
  TRK_PANT_GUIA,          // ★ PANTALLA DE GUIADO: brujula + linea del track + datos
  TRK_PANT_SALIDAS,       // ★★ LAS SALIDAS (2026-10-06): elegir por cual volver a casa
};
TrkPant gTrkPant = TRK_PANT_NADA;
int     gTrkFila = 0;       // fila resaltada
int     gTrkElegido = -1;   // que se ha elegido: -1 = vivo, 0..4 = ranura
// â˜… Vista de la pantalla de guiado: false = VENTANA DE CERCA (1,5 km, la de navegar), true =
//   track COMPLETO (la de mirar la ruta entera). Una pulsacion corta cambia de una a otra, como
//   pidio el operador. Se queda en false por defecto: al empezar a guiar lo que se quiere es
//   navegar, no ver el plano general.
bool    gTrkZoomCompleto = false;

// ---------------------------------------------------------------------------
//  ★★ LA PANTALLA DE SALIDAS: por cual vuelves a casa (2026-10-06) ★★
//
//  QUE ES: el aparato graba tus paseos en un solo track continuo, y el REGISTRO de viaje los parte
//  en "salidas" (cada vez que pasa mucho tiempo sin balizas = se acabo la salida). Esta pantalla
//  ensena esas salidas para que elijas por cual quieres volver.
//
//  ★ SE REHACE AL ABRIR, NO EN CADA REPINTADO: recorrer el registro y buscar las puntas de cada
//    salida dentro del track cuesta (miles de lecturas de flash). Se hace una vez al entrar.
// ---------------------------------------------------------------------------
uint8_t gSalidasCuenta = 0;      // cuantas salidas hay en la lista
bool    gSalidasLeidas = false;  // false = hay que rehacer la lista al pintar
uint8_t gSalidasDelRegistro = 0; // ★ cuantas se sacaron del REGISTRO (para poder decirlo en pantalla)

// ===========================================================================
//  ★★ LAS FILAS DE LA LISTA DE TRACKS, REORGANIZADAS (2026-10-06) ★★
//
//  COMO ESTABA: 0=Volver, 1=Salir, 2=track vivo, 3="Empezar nuevo", 4..8=ranuras, 9=Volver a casa.
//  Un popurri: TUS tracks grabados (el vivo), una ACCION ("Empezar nuevo") y las RUTAS QUE TE HAS
//  BAJADO (las ranuras), todo mezclado. El operador lo dijo claro: «empezar nuevo es como raro»,
//  y tenia razon, porque la pantalla mezclaba dos cosas distintas.
//
//  COMO ESTA AHORA, y es idea suya:
//    0        "< Volver"
//    1        "Salir"
//    2        "Volver a casa"   -> abre LA LISTA DE TUS TRACKS GRABADOS
//    3..7     las 5 ranuras     -> las rutas de Wikiloc que te has bajado
//
//  ★ POR QUE "EMPEZAR NUEVO" YA NO ESTA: servia para tirar el track y empezar donde estabas, y
//    hacia falta cuando el track seguia creciendo entre salidas. Pero el aparato **se apaga solo
//    cuando la bateria baja**, asi que cada salida ya empieza donde toca. **El aparato ya hace lo
//    que hacia ese boton.** Su codigo se queda (ver ACT_TRK_NUEVO): solo se ha quitado la fila,
//    y volver a ponerla es una linea.
//
//  ★ Y "VOLVER A CASA" ES UNA METAFORA, como dijo el operador: no es "casa", es **el punto donde
//    empezaste**. Se usa el track de verdad, al reves, para volver a por donde viniste.
// ===========================================================================
constexpr int kTrkFilaCasa    = 2;
constexpr int kTrkFilaSlot0   = 3;
constexpr int kTrkFilasLista  = 8;    // 2 de navegacion + 1 de casa + 5 ranuras
// Pantalla de accion: 0=Volver, 1=Salir, 2=adelante, 3=atras, 4=finalizar
constexpr int kTrkFilasAccion = 5;

// Â¿Estan abiertas las pantallas de tracks? Lo mira `menuPinta` y lo mira `displayRefresh`.
bool pantallaTracksActiva() { return gTrkPant != TRK_PANT_NADA; }

void pantallaTracksAbre(TrkPant cual, int fila) {
  gTrkPant = cual;
  gTrkFila = fila;
  gMenuOn = false;          // las pantallas de tracks sustituyen al menu de ajustes
  gMenuCat = -1;
  gDirty = true;
}

void pantallaTracksCierra() {
  gTrkPant = TRK_PANT_NADA;
  gTrkElegido = -1;
  gDirty = true;
}

// ---------------------------------------------------------------------------------------
//  PINTADO de las pantallas de tracks. Mismo estilo que el menu: titulo pequeno arriba,
//  fila 0 "< Volver", fila 1 "Salir", filas a escala 2 con recuadro en la seleccionada.
// ---------------------------------------------------------------------------------------
namespace {

void filaTrk(int r, int y, const char *texto, bool sel) {
  // â˜…â˜… ESCALA 0: LA FUENTE DIN 10 (2026-09-22) â˜…â˜…
  //   Es el tercer tamano: la misma altura de letra que la escala 2 (14 px) pero de ancho
  //   variable. Las filas de este menu llevan textos largos ("Finalizar guiado", 16 caracteres)
  //   que a escala 2 NO caben (190 px sobre 187 utiles) y con esta fuente caben de sobra (129).
  //   â˜… Y LAS FILAS SUBEN DE 18 A 20 px: la DIN 10 llega mas abajo que los 14 px de la escala 2
  //     (las letras con rabo como la 'g' bajan 9 px por debajo de la linea base), asi que con 18
  //     px de salto se tocarian las lineas.
  const int esc2 = 0;
  const int rowH2 = 20;
  if (sel) {
    barraH(6, EPD_W - 6, y - 1, 1);
    barraH(6, EPD_W - 6, y + rowH2 - 2, 1);
    barraV(6, y - 1, y + rowH2 - 2, 1);
    barraV(EPD_W - 6, y - 1, y + rowH2 - 2, 1);
  }
  drawText(14, y, texto, esc2);
}

// Nombre corto de una ranura: â˜… NUMERO + FECHA Y HORA (2026-09-22).
//   Las vacias ya llevaban el numero ("1  Vacia"), pero las LLENAS no, y ahi es donde hace
//   falta: con las cinco ranuras cargadas el mismo dia, seis filas con "12/07 18:42" no se
//   distinguen entre si. El numero las separa sin teclear nada (el operador descarto los nombres
//   a proposito: bautizar rutas en una pantalla de tinta con un boton es un suplicio).
//   El ano NO se pone: a escala 2 no cabe y no ayuda a elegir una ruta recien cargada.
const char *nombreRanura(uint8_t slot, char *b, size_t n) {
  TrackRanuraInfo inf;
  tracksRanuraInfo(slot, &inf);
  if (!inf.valida) { snprintf(b, n, "%d  Vacia", (int)slot + 1); return b; }
  // "4  12/07 18:42" = 14 caracteres a escala 2: cabe (el tope son 16).
  snprintf(b, n, "%d  %02u/%02u %02u:%02u", (int)slot + 1, (unsigned)inf.day,
           (unsigned)inf.month, (unsigned)inf.hour, (unsigned)inf.minute);
  return b;
}

// ---------------------------------------------------------------------------
//  ★★ LA PANTALLA DE SALIDAS (2026-10-06) ★★
//
//  Se entra desde "Volver a casa". Ensena las salidas que el aparato tiene grabadas, de la mas
//  reciente a la mas vieja, con su FECHA, su HORA y sus METROS. Eliges una y te guia hacia atras
//  por ella.
//
//  ★ LAS FILAS SON: "< Volver", "Salir" y luego una por salida. Se navega con el mismo boton que
//    todo lo demas (pulsacion corta para bajar de fila), como pidio el operador.
//
//  ★ SOLO SALEN LAS SALIDAS QUE TODAVIA TIENEN CAMINO. El track es un anillo de ~68 km: cuando se
//    llena empieza a borrar por el principio, y las salidas mas viejas se quedan sin puntos. Una
//    lista que ofrece algo que ya no existe es mentir, asi que esas no aparecen.
// ---------------------------------------------------------------------------
void pantallaSalidasPinta() {
  static constexpr int rowH2 = 20;
  static constexpr int y0 = 30;

  // ★ LA LISTA SE REHA CE AQUI, UNA VEZ AL ENTRAR: recorrer el registro y anclar cada salida al
  //   track cuesta miles de lecturas de flash, y no se puede hacer en cada repintado.
  if (!gSalidasLeidas) {
    gSalidasCuenta = tracksSesionesRehace();
    gSalidasDelRegistro = tracksSesionesDelRegistro();
    gSalidasLeidas = true;
  }

  drawTextCenter(12, "SALIDAS", 0);   // escala 0: la fuente DIN 10

  const int total = 2 + (int)gSalidasCuenta;
  const int visibles = 7;
  int top = gTrkFila - (visibles / 2);
  if (top < 0) top = 0;
  if (top > total - visibles) top = (total - visibles < 0) ? 0 : total - visibles;

  int y = y0;
  for (int r = top; r < total && y < 186; r++, y += rowH2) {
    const bool sel = (r == gTrkFila);
    if (r == 0) { filaTrk(r, y, "< Volver", sel); continue; }
    if (r == 1) { filaTrk(r, y, "Salir", sel); continue; }

    const uint8_t idx = (uint8_t)(r - 2);
    TrackSesion s;
    if (!tracksSesionesLee(idx, &s)) { filaTrk(r, y, "?", sel); continue; }

    // ★ "06/10 08:42 3420m" = 18 caracteres. A escala 0 (la DIN 10, de ancho variable) cabe de
    //   sobra en los 200 px del panel; a escala 2 NO cabria (18 x 12 = 216 px).
    char b[28];
    if (s.year == 0) {
      // ★ SIN HORA GPS: se dice ESO, no "00/00 00:00".
      //   El track vivo se empieza a grabar en cuanto hay fijacion, y la hora del GPS puede
      //   tardar un rato mas en llegar. Antes se ponia "Sin hora GPS" en la fila del track en
      //   vivo; esa fila ya no existe, asi que el texto se trae aqui: un "00/00 00:00" no dice
      //   nada y parece un fallo del aparato.
      snprintf(b, sizeof b, "sin hora GPS  %lum", (unsigned long)s.metros);
    } else {
      snprintf(b, sizeof b, "%02u/%02u %02u:%02u %lum", (unsigned)s.day, (unsigned)s.month,
               (unsigned)s.hour, (unsigned)s.minute, (unsigned long)s.metros);
    }
    filaTrk(r, y, b, sel);
  }

  // El pie: si no hay ninguna salida, se dice POR QUE, no se deja la pantalla vacia.
  // ★ ESTO FALTABA, Y ES LA RAZON DE QUE EL OPERADOR VIERA "UNA PANTALLA QUE NO HACE NADA": sin
  //   salidas, la pantalla se pintaba VACIA (solo el titulo) y no habia forma de saber si estaba
  //   rota, sin datos, o esperando algo. Una pantalla en blanco nunca es una respuesta.
  //
  // ★★ Y AHORA DICE LOS NUMEROS (2026-10-06) ★★
  //   El operador reporto «sale sin tracks grabados, pero el aparato tiene un track de hoy y otros
  //   de otros dias». Para averiguar POR QUE sin poder conectar un depurador, la pantalla ensena
  //   los dos numeros que deciden todo:
  //     - puntos: cuantos puntos dice el modulo que tiene el track. Si son 0 o 1, la lista se
  //       queda vacia A PROPOSITO (con menos de dos puntos no hay camino que seguir), y el aviso
  //       es correcto aunque el operador recuerde haber andado.
  //     - del registro: cuantas salidas se han podido sacar del registro de viaje.
  //   ★ Es la misma idea que el resto del proyecto: **que el aparato diga lo que le pasa**, en vez
  //     de dejarnos adivinar desde el ordenador. Un dato en pantalla vale mas que una teoria.
  if (gSalidasCuenta == 0) {
    drawText(14, 56, "Sin tracks grabados", 0);
    char linea[32];
    snprintf(linea, sizeof linea, "puntos: %lu", (unsigned long)tracksVivoPuntos());
    drawText(14, 80, linea, 0);
    snprintf(linea, sizeof linea, "del registro: %u", (unsigned)gSalidasDelRegistro);
    drawText(14, 100, linea, 0);
    drawText(14, 130, "Anda con el nodo", 0);
    drawText(14, 148, "encendido y saldra", 0);
    drawText(14, 166, "tu ruta aqui.", 0);
  } else {
    char pie[32];
    snprintf(pie, sizeof pie, "%u track%s", (unsigned)gSalidasCuenta,
             (gSalidasCuenta == 1) ? "" : "s");
    drawTextCenter(178, pie, 0);
  }
}

void pantallaTracksPinta() {  // ★★ EL SALTO DE FILA, EN UN SOLO SITIO (2026-09-22) ★★
  //   `filaTrk` dibuja el recuadro de 20 px de alto, pero el bucle que coloca las filas avanzaba
  //   de 18 en 18. El recuadro de una fila se metia 2 px en la siguiente, y el desfase se acumula
  //   hacia abajo: en las ultimas filas (donde esta "Volver a casa") el recuadro quedaba
  //   descuadrado respecto a su texto. Lo vio el operador.
  //   ★ AHORA HAY UNA SOLA CONSTANTE, y la usan el recuadro y el bucle: no pueden discrepar.
  static constexpr int rowH2 = 20;
  static constexpr int y0 = 30;
  if (gTrkPant == TRK_PANT_LISTA) {
    // â˜… El titulo es CORTO a proposito, como el de los demas submenus ("PERFILES", "MENU"): la
    //   coherencia de estilo la pidio el operador. El aviso de que la lista se desplaza NO va
    //   aqui (un titulo largo rompia el estilo): va en la fila del track EN VIVO, que es donde
    //   se ve que hay mas abajo.
    drawTextCenter(12, "TRACKS", 0);   // escala 0: la fuente DIN 10 (ver epd_font_din10.h)
    const int total = kTrkFilasLista;
    // â˜… 7 FILAS Y NO 8 (2026-09-22): al subir el salto de 18 a 20 px por la fuente DIN, la
    //   octava fila empezaba en y=170 y su texto (23 px de caja) llegaba a 193: se metia debajo
    //   del pie de la pantalla. Con 7 filas la ultima acaba en 176 y queda sitio de sobra.
    const int visibles = 7;
    int top = gTrkFila - (visibles / 2);
    if (top < 0) top = 0;
    if (top > total - visibles) top = (total - visibles < 0) ? 0 : total - visibles;
    int y = y0;
    for (int r = top; r < total && y < 186; r++, y += rowH2) {
      const bool sel = (r == gTrkFila);
      if (r == 0) { filaTrk(r, y, "< Volver", sel); continue; }
      if (r == 1) { filaTrk(r, y, "Salir", sel); continue; }
      // ★ "VOLVER A CASA" ES UNA METAFORA, como dijo el operador: no es "casa", es EL PUNTO DONDE
      //   EMPEZASTE. Y al pulsarla NO se arranca nada: se abre la lista de TUS tracks grabados
      //   (`TRK_PANT_SALIDAS`), que es donde de verdad se elige por cual volver.
      if (r == kTrkFilaCasa) { filaTrk(r, y, "Volver a casa", sel); continue; }
      // Y debajo, las rutas que el operador se ha bajado de Wikiloc: otra cosa distinta.
      if (r >= kTrkFilaSlot0 && r < kTrkFilaSlot0 + (int)TRACK_SLOTS) {
        char b[24];
        filaTrk(r, y, nombreRanura((uint8_t)(r - kTrkFilaSlot0), b, sizeof b), sel);
        continue;
      }
      if (r == kTrkFilaCasa) { filaTrk(r, y, "Volver a casa", sel); continue; }
    }
    return;
  }

  if (gTrkPant == TRK_PANT_ACCION) {
    drawTextCenter(12, "GUIAR", 0);    // escala 0: la fuente DIN 10    // â˜… Se dice QUE track se ha elegido: sin esto, con cinco ranuras iguales en pantalla el
    //   operador no sabe cual esta a punto de seguir.
    char tit[24];
    if (gTrkElegido < 0) {
      snprintf(tit, sizeof tit, "Track en vivo");
    } else {
      TrackRanuraInfo inf;
      tracksRanuraInfo((uint8_t)gTrkElegido, &inf);
      snprintf(tit, sizeof tit, "%02u/%02u %02u:%02u", (unsigned)inf.day, (unsigned)inf.month,
               (unsigned)inf.hour, (unsigned)inf.minute);
    }
    drawTextCenter(26, tit, 1);

    int y = y0 + 14;
    for (int r = 0; r < kTrkFilasAccion && y < 186; r++, y += rowH2) {
      const bool sel = (r == gTrkFila);
      if (r == 0) { filaTrk(r, y, "< Volver", sel); continue; }
      if (r == 1) { filaTrk(r, y, "Salir", sel); continue; }
      if (r == 2) { filaTrk(r, y, "Hacia adelante", sel); continue; }
      if (r == 3) { filaTrk(r, y, "Hacia atras", sel); continue; }
      if (r == 4) { filaTrk(r, y, "Finalizar guiado", sel); continue; }
    }
    return;
  }

  // =====================================================================================
  //  â˜…â˜…â˜… LA PANTALLA DE GUIADO (2026-09-22) â˜…â˜…â˜…
  //
  //  Reparto del panel de 200x200:
  //     y=0..11    titulo (el sentido: "ADELANTE" / "ATRAS") y el rumbo del GPS
  //     y=14..92   ZONA DE DIBUJO (206x78 utiles): la LINEA del track + la FLECHA encima
  //     y=96..104  separador
  //     y=108..144 desviacion / lo que falta / altitud + coordenadas
  //     y=186      pie (bateria), como en las demas escenas
  //
  //  â˜… LA FLECHA VA EN UNA ESQUINA, ENCIMA DE LA LINEA. No se puede dibujar una rosa de los
  //    vientos girando en este panel, y ademas la flecha es lo que se mira de un vistazo: se
  //    pinta en la esquina superior izquierda (44x44) para no tapar la linea del track.
  // =====================================================================================
  if (gTrkPant == TRK_PANT_GUIA) {
    TrackGuia g;
    tracksGuiaEstado(&g);
    const bool sinFix = !gpsGet().fix;
    const float vel = gpsGet().speedKmh;

    // ---- linea 1: el sentido del guiado, y el rumbo del GPS si lo hay ----
    {
      char b[24];
      if (sinFix) snprintf(b, sizeof b, "%s  SIN GPS", g.alReves ? "ATRAS" : "ADELANTE");
      else if (vel < kGuiaAndandoKmh) snprintf(b, sizeof b, "%s  PARADO", g.alReves ? "ATRAS" : "ADELANTE");
      else snprintf(b, sizeof b, "%s  rumbo %03d", g.alReves ? "ATRAS" : "ADELANTE",
                    (int)(gpsGet().courseDeg + 0.5f) % 360);
      drawTextCenter(2, b, 1);
    }

    // ---- la zona de dibujo ----
    constexpr int DIB_X0 = 6, DIB_X1 = EPD_W - 7;
    constexpr int DIB_Y0 = 16, DIB_Y1 = 92;
    constexpr float kGuiaVentanaM = 1500.0f;   // â˜… el radio de la VENTANA DE CERCA

    if (tracksGuiaDibujable()) {
      // ---- 1) limites de lo que se va a dibujar ----
      //   De cerca: la ventana alrededor de ti. Lo lejos: TODO el track.
      //   El eje "a lo largo" se mide con la distancia ACUMULADA y el "a lo ancho" con la
      //   desviacion, los dos ya calculados por el motor: asi no hay que proyectar nada aqui.
      double latMin = 90.0, latMax = -90.0, lonMin = 180.0, lonMax = -180.0;
      uint32_t desde = 0, hasta = 0;
      if (!gTrkZoomCompleto) {
        // Desde un poco antes de tu punto hasta la ventana por delante.
        desde = (g.idxCerca > 20) ? (g.idxCerca - 20) : 0;
        // Hasta: se avanza hasta juntar la ventana. Se limita el numero de puntos leidos para
        // que un track enorme no bloquee el refresco.
        double acum = 0.0;
        TrackPunto p0, p1;
        hasta = desde;
        if (tracksGuiaLee(desde, &p0)) {
          for (uint32_t i = desde; i + 1 < tracksGuiaPuntos() && acum < kGuiaVentanaM; i++) {
            if (!tracksGuiaLee(i + 1, &p1)) break;
            acum += gpsDistanceM((double)p0.lat1e7 / 1e7, (double)p0.lon1e7 / 1e7,
                                 (double)p1.lat1e7 / 1e7, (double)p1.lon1e7 / 1e7);
            p0 = p1;
            hasta = i + 1;
          }
        }
      } else {
        desde = 0;
        hasta = tracksGuiaPuntos() - 1;
      }

      for (uint32_t i = desde; i <= hasta; i++) {
        TrackPunto p;
        if (!tracksGuiaLee(i, &p)) continue;
        const double la = (double)p.lat1e7 / 1e7, lo = (double)p.lon1e7 / 1e7;
        if (la < latMin) latMin = la;
        if (la > latMax) latMax = la;
        if (lo < lonMin) lonMin = lo;
        if (lo > lonMax) lonMax = lo;
      }
      // Y tu posicion entra en los limites SIEMPRE: si no, la marca de posicion se saldria.
      {
        const double la = gpsGet().lat, lo = gpsGet().lon;
        if (sinFix) { /* sin fix no se dibuja la marca, pero los limites son del track */ }
        else {
          if (la < latMin) latMin = la;
          if (la > latMax) latMax = la;
          if (lo < lonMin) lonMin = lo;
          if (lo > lonMax) lonMax = lo;
        }
      }

      // ---- 2) la escala: TIENE QUE ENTRAR EN ANCHO **Y** EN ALTO ----
      // â˜…â˜… AQUI HABIA UN FALLO DE BULTO (2026-09-22; lo cazo una revision independiente) â˜…â˜…
      //   La escala se sacaba SOLO del ancho (`200.0 / anchoM`) y el alto se calculaba... para
      //   nada: `altoM` no se usaba en ninguna parte. Resultado: una ruta que se extiende en
      //   vertical (cualquier ruta de montana de un dia) NO cabia, y como `aPantalla` recorta
      //   cada punto al borde, la linea se convertia en dos rayas pegadas a los cantos. El
      //   usuario no podia saber por donde iba la ruta ni si el estaba dentro.
      //   Con la zona de dibujo de 187x77 px, el ALTO es lo que manda casi siempre.
      //   AHORA: metros por pixel = el MAYOR de los dos (ancho/anchoDib, alto/altoDib), que es lo
      //   que garantiza que quepa en las dos direcciones.
      const double cosLat = cos(((latMin + latMax) / 2.0) * M_PI / 180.0);
      const double anchoM = (lonMax - lonMin) * 111320.0 * cosLat;
      const double altoM  = (latMax - latMin) * 110540.0;
      const double dibAncho = (double)(DIB_X1 - DIB_X0);
      const double dibAlto  = (double)(DIB_Y1 - DIB_Y0);
      double esc = 1.0;
      if (dibAncho > 1.0 && dibAlto > 1.0) {
        const double escX = anchoM / dibAncho;
        const double escY = altoM / dibAlto;
        esc = (escX > escY) ? escX : escY;     // â˜… el que mas aprieta: cabe en las dos
      }
      // â˜… SOLO se dice "no se puede dibujar" cuando de verdad no aporta nada: si sale mas de
      //   2 km por pixel, la linea no dice nada. (Antes se avisaba a partir de 200 km, que con
      //   el alto ya no hace falta tan pronto.)
      const bool escalaRid = (esc > 2000.0);
      if (escalaRid) esc = 1.0;

      auto aPantalla = [&](double la, double lo, int *px_, int *py_) {
        const double xm = (lo - (lonMin + lonMax) / 2.0) * 111320.0 * cosLat;
        const double ym = (la - (latMin + latMax) / 2.0) * 110540.0;
        int x = (int)((xm / esc) + (DIB_X0 + DIB_X1) / 2);
        int y = (DIB_Y1 + DIB_Y0) / 2 - (int)(ym / esc);
        if (x < DIB_X0) x = DIB_X0;
        if (x > DIB_X1) x = DIB_X1;
        if (y < DIB_Y0) y = DIB_Y0;
        if (y > DIB_Y1) y = DIB_Y1;
        *px_ = x; *py_ = y;
      };

      // ---- 3) la LINEA: continua lo ya recorrido, a guiones lo que queda ----
      int xa = 0, ya = 0, xb = 0, yb = 0;
      bool hayA = false;
      for (uint32_t i = desde; i <= hasta && i + 1 <= tracksGuiaPuntos() - 1; i++) {
        TrackPunto p;
        if (!tracksGuiaLee(i, &p)) break;
        aPantalla((double)p.lat1e7 / 1e7, (double)p.lon1e7 / 1e7, &xb, &yb);
        if (hayA) {
          // â˜… El corte entre "hecho" y "queda" es tu punto mas cercano: lo de atras va continuo
          //   (ya lo has andado) y lo de delante a guiones (te queda por andar). En blanco y
          //   negro, sin colores, ese es el recurso que hay para distinguirlos.
          const bool queda = (i >= g.idxCerca);
          trazaLinea(xa, ya, xb, yb, queda ? 3 : 0);
        }
        xa = xb; ya = yb; hayA = true;
      }

      // ---- 4) la marca de TU posicion (una cruz) ----
      if (!sinFix) {
        int mx, my;
        aPantalla(gpsGet().lat, gpsGet().lon, &mx, &my);
        for (int d = -3; d <= 3; d++) { px(mx + d, my, true); px(mx, my + d, true); }
      }
    } else {
      // â˜…â˜… ESTAS DOS LINEAS SUBEN A y=22 (2026-09-22) â˜…â˜…
      //   Estaban en DIB_Y0+30 (y=46) y DIB_Y0+42 (y=58)... y el AVISO DE LA FLECHA se pinta en
      //   y=46 TAMBIEN, pero FUERA de este if/else (o sea, aunque no haya track que dibujar).
      //   O sea que SI pueden coincidir: un track demasiado largo para dibujar y el GPS parado
      //   sacaban las dos cosas encima. Lo caza el comprobador de disposicion de textos, que para
      //   eso esta: a ojo no se ve, porque cada rama por separado parece bien.
      //   Subirlas a y=22 las saca de la fila del aviso (que va en y=46) sin salirse de la zona.
      drawTextCenter(DIB_Y0 + 6, "Track demasiado", 1);
      drawTextCenter(DIB_Y0 + 18, "largo para dibujar", 1);
    }

    // ---- 4b) EL AVISO DE QUE NO SE ESTA GRABANDO (2026-09-22) ----
    //   El guiado PAUSA la grabacion del track en vivo (punto 7 de la especificacion), y eso hay
    //   que DECIRLO en la pantalla, no solo apuntarlo en el registro de viaje (que se lee por
    //   USB). El caso peor no es curiosear el menu: es acabar la ruta y olvidarse de "Finalizar
    //   guiado", y quedarse sin grabar el resto de la caminata.
    //   â˜…â˜… ESTE AVISO YA NO SE PINTA AQUI (2026-09-22) â˜…â˜…
    //     Se pinta en `pintaCabecera()`, que sale en TODAS las pantallas. Aqui solo estaba en
    //     esta, y al salir a la lista (o al carrusel) el usuario se quedaba sin saber que su
    //     track habia dejado de grabarse. Ver la explicacion larga en `pintaCabecera`.
    
    // ---- 5) LA FLECHA, en la esquina ----
    //   â˜…â˜… LAS CINCO REGLAS â˜…â˜…
    //     - sin fix                -> no se pinta flecha (no hay posicion)
    //     - parado (<1 km/h)       -> "ANDA UNOS PASOS": el rumbo del GPS vale 0 parado y una
    //                                 flecha al norte con toda su confianza manda al sitio
    //                                 contrario
    //     - desviacion > 500 m     -> â˜… SE SIGUE PINTANDO FLECHA, pero de VUELTA a la ruta (ver
    //                                 abajo: esto se corrigio el 2026-09-22)
    //     - a menos de 25 m del final -> "LLEGADA"
    //     - si no, flecha = rumbo de la ruta - rumbo de marcha
    //
    //   â˜…â˜… EL FALLO QUE SE CORRIGIO AQUI (lo cazo una revision independiente) â˜…â˜…
    //     Antes, con mas de 500 m de desviacion, se ponia "MUY LEJOS" y **se dejaba de pintar
    //     flecha**. Y la linea de datos ponia "DESV MUY LEJOS" en vez de los metros, aunque el
    //     motor SI tenia el numero. O sea: en el unico momento en que de verdad hace falta saber
    //     hacia donde tirar (te has desviado, hay niebla, se cierra la senda), el aparato se
    //     quedaba mudo. Es justo lo contrario de lo que se busca.
    //     AHORA: se pinta la flecha HACIA LA RUTA (no "MUY LEJOS" y a buscarte la vida), y los
    //     metros de desviacion se siguen diciendo.
    {
      const int cxf = 28, cyf = 42;   // centro de la flecha
      const char *aviso = nullptr;
      bool flechaDeVuelta = false;    // true = apuntar al punto de la ruta mas cercano
      if (sinFix) aviso = "SIN GPS";
      else if (vel < kGuiaAndandoKmh) aviso = "ANDA UNOS PASOS";
      else if (g.faltanM < kGuiaCercaM) aviso = "LLEGADA";
      else if (g.desviacionM > kGuiaMuyLejosM) flechaDeVuelta = true;

      if (aviso) {
        // Sin flecha: se dice por que. Es mejor no pintar nada que pintar una flecha falsa.
        drawText(8, DIB_Y0 + 30, aviso, 1);
      } else {
        // A donde apuntar: al trozo de ruta que viene, o DE VUELTA a la ruta si te has ido
        // lejos. El rumbo se mide desde tu posicion real al punto elegido.
        float rumboObjetivo = g.rumboRutaDeg;
        if (flechaDeVuelta) {
          TrackPunto p;
          if (tracksGuiaLee(g.idxCerca, &p)) {
            rumboObjetivo = gpsBearingDeg(gpsGet().lat, gpsGet().lon,
                                          (double)p.lat1e7 / 1e7, (double)p.lon1e7 / 1e7);
          }
        }
        // Flecha de 8 puntas: se elige la mas parecida a la diferencia y se pinta con barras,
        // que es lo unico que hay para dibujar en este panel sin meter una tabla mas.
        float dif = rumboObjetivo - gpsGet().courseDeg;
        while (dif < 0) dif += 360.0f;
        while (dif >= 360.0f) dif -= 360.0f;
        const int oct = (int)((dif + 22.5f) / 45.0f) % 8;
        // 0=norte(arriba) y en sentido horario
        static const int8_t dx8[8] = { 0,  1,  1,  1,  0, -1, -1, -1};
        static const int8_t dy8[8] = {-1, -1,  0,  1,  1,  1,  0, -1};
        const int largo = 16;
        const int fx = cxf + dx8[oct] * largo, fy = cyf + dy8[oct] * largo;
        trazaLinea(cxf, cyf, fx, fy, 0);
        // La punta: dos trazos cortos en diagonal, que es lo unico que hace falta para que la
        // raya se lea como una flecha y no como un palo.
        trazaLinea(fx, fy, fx - dx8[oct] * 6 + dy8[oct] * 5, fy - dy8[oct] * 6 - dx8[oct] * 5, 0);
        trazaLinea(fx, fy, fx - dx8[oct] * 6 - dy8[oct] * 5, fy - dy8[oct] * 6 + dx8[oct] * 5, 0);
      }
    }

    // ---- 5b) AVISO: EL PRINCIPIO DEL ANILLO YA SE PERDIO (2026-09-22) ----
    //   "Volver a casa" guia hacia el PRINCIPIO del track en vivo. Si el anillo ha dado la
    //   vuelta, ese principio ya no es donde empezaste: es un punto cualquiera del recorrido. El
    //   motor lo sabe, y callarlo seria guiar con toda la confianza hacia el sitio equivocado.
    //   Se avisa SOLO en el caso que importa: yendo HACIA ATRAS con el track EN VIVO.
    //   â˜… Y tras un reinicio no se puede saber (`gVivoVueltas` se pone a 0 a proposito), asi que
    //     este aviso no promete nada que no pueda cumplir.
    if (g.alReves && g.fuente == TRK_FUENTE_VIVO && tracksVivoDioLaVuelta()) {
      drawText(8, DIB_Y1 - 10, "Inicio perdido", 1);
    }
    
    // ---- 6) separador y datos ----
    hLine(6, EPD_W - 7, 96, 1);
    {
      // â˜…â˜… LA DISPOSICION DE ESTA FRANJA, EN UN SOLO SITIO (2026-09-22) â˜…â˜…
      //   Estas constantes son las filas de los cuatro datos, y se declaran ANTES de usarlas para
      //   que la disposicion se lea de un vistazo. Antes habia una fila escrita a mano en un sitio
      //   y otra en otro, y asi fue como se colaron los solapes.
      //     DESV   102..115   (escala 2 = 14 px de alto)
      //     FALTAN 118..131   (escala 2)   -> 18 px de salto: no se tocan
      //     ALT    136..142   (escala 1 = 7 px)
      //     coords 150..156   (escala 1)
      static constexpr int kFilaDesv   = 102;
      static constexpr int kFilaFaltan = 118;
      static constexpr int kFilaAlt    = 136;
      static constexpr int kFilaCoord  = 150;
      // â˜… `gp` VA AQUI ARRIBA: las coordenadas se pintan mas abajo y lo usan. La primera version
      //   lo declaro dentro de un bloque interior y el compilador lo caza
      //   ('gp' was not declared in this scope) -- despiste que conviene dejar anotado.
      const GpsData &gp = gpsGet();
      char b[32];

      // La desviacion: es EL numero que se mira al seguir un track, y se dice SIEMPRE, tambien
      // cuando es grande. â˜… Antes, con mas de 500 m ponia "MUY LEJOS" en vez de los metros: justo
      //   cuando el numero es mas util (para saber si te acercas a la ruta o te alejas) se lo
      //   quitaba al usuario. Corregido el 2026-09-22.
      if (g.desviacionM >= 1000.0f) snprintf(b, sizeof b, "DESV   %.1f km", (double)(g.desviacionM / 1000.0f));
      else snprintf(b, sizeof b, "DESV   %d m", (int)(g.desviacionM + 0.5f));
      drawText(8, kFilaDesv, b, 2);

      // â˜…â˜… "FALTAN" ES LONGITUD DE RUTA, Y ESO SE DICE CON EL PUNTO DEL DECIMAL (2026-09-22) â˜…â˜…
      //   El numero es lo que queda POR LA RUTA, no la distancia en linea recta al final. Es el
      //   dato correcto para seguir un track, pero quien lo lee como "cuanto me queda para llegar"
      //   se desconcierta si la ruta da una vuelta a un barranco (4,2 km de ruta con el final a
      //   1,5 km en recta).
      //   Se intento rotularlo con "(ruta)" y NO CABE en ningun sitio:
      //     - debajo: se imprimia ENCIMA de "ALT" (lo cazo una revision independiente midiendo las
      //       filas: entre las dos lineas solo quedaban 6 px y una linea de texto necesita 7);
      //     - a la derecha: el numero a escala 2 llega a 14 caracteres ("FALTAN 99.99 km" son 178
      //       px desde x=8), asi que no queda hueco para nada detras sin pisarlo (lo dijo el
      //       comprobador de disposicion, que para eso esta).
      //   LA SOLUCION NO OCUPA NADA: en kilometros SE PONE EL DECIMAL (dos cifras) y en metros no.
      //   Ese punto ya dice que es una distancia medida y no una cuenta redondeada, que es el
      //   convenio de cualquier aparato de estos. Cero caracteres de mas.
      {
        char bf[24];
        if (g.faltanM >= 1000.0f) snprintf(bf, sizeof bf, "FALTAN %.2f km", (double)(g.faltanM / 1000.0f));
        else snprintf(bf, sizeof bf, "FALTAN %d m", (int)(g.faltanM + 0.5f));
        drawText(8, kFilaFaltan, bf, 2);
      }

      if (gp.altValid) snprintf(b, sizeof b, "ALT %d m", (int)(gp.altM + 0.5f));
      else snprintf(b, sizeof b, "ALT --");
      drawText(8, kFilaAlt, b, 1);
      snprintf(b, sizeof b, "%.5f %.5f", gp.lat, gp.lon);
      drawText(8, kFilaCoord, b, 1);
    }

    // ---- 7) COMO SE SALE DE AQUI (2026-10-07) ----
    //   ★★ ESTO FALTABA, Y ES UN FALLO DE DISEÑO, NO UN ADORNO ★★
    //     La pantalla de guiado **NO se cierra sola**, y es a proposito: es una pantalla de trabajo
    //     que se mira de reojo mientras andas, y si se cerrara a los 15 segundos habria que dar
    //     cuatro gestos para volver a ver el rumbo. Pero **la pantalla no decia como se sale**, y
    //     el operador tuvo que preguntarlo:
    //         «una vez dentro del guiado, como salgo?»
    //     Y es facil de entender por que: **la pulsacion CORTA no sale** (cambia entre la vista de
    //     cerca y la del track entero), asi que quien prueba el corto ve que la pantalla hace cosas
    //     y no encuentra la salida. **Una pantalla de la que no sabes salir es una trampa**, por muy
    //     bien que funcione todo lo demas.
    //   ★ AHORA SE DICE EN LA PROPIA PANTALLA, que es donde hace falta saberlo. Va en letra pequeña
    //     (escala 1) y en la ultima fila libre, para no quitarle sitio a los datos: lo que se mira
    //     andando es la flecha y los metros, no esto.
    //   ★ Y EL TEXTO ESTA MEDIDO, NO ELEGIDO A OJO: a escala 1 cada caracter son 5 px, el panel
    //     tiene 200 y se empieza en x=8, o sea que caben **30 caracteres**. El primer intento ponia
    //     "LARGO: salir   CORTO: cerca/entero", que son 204 px: **se salia del panel**. Este son
    //     156 px y dice lo mismo con menos letras.
    //     (Medido con `tools/prueba_disposicion_guia.py`, que vigila que ningun texto se salga.)
    drawText(8, 170, "LARGO: salir  CORTO: vista", 1);

    return;
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------
//  NAVEGACION de las pantallas de tracks. Se llama desde `menuNavigate`, `menuShort` y
//  `menuLong` cuando `pantallaTracksActiva()`: asi el boton se comporta igual que en el menu.
// ---------------------------------------------------------------------------------------
void pantallaTracksNavega() {
  int total;
  if (gTrkPant == TRK_PANT_SALIDAS) {
    total = 2 + (int)gSalidasCuenta;      // "< Volver", "Salir" y una por salida
  } else if (gTrkPant == TRK_PANT_LISTA) {
    total = kTrkFilasLista;
  } else {
    total = kTrkFilasAccion;
  }
  if (total < 1) total = 1;
  gTrkFila = (gTrkFila + 1) % total;      // wrap, como en el resto del menu
  gDirty = true;
}

void pantallaTracksCorto() {
  // ---------------------------------------------------------------------
  //  ★ LA PANTALLA DE SALIDAS (2026-10-06)
  // ---------------------------------------------------------------------
  if (gTrkPant == TRK_PANT_SALIDAS) {
    // ★ "< Volver" VUELVE AL MENU, a la seccion de Tracks (2026-10-06).
    //   Antes volvia a la lista intermedia (`TRK_PANT_LISTA`), y como esa lista tambien tenia un
    //   "Volver a casa", el operador se quedaba dando vueltas entre dos pantallas. Su queja:
    //   «volver no funciona». Tenia razon: no volvia, **rebotaba**.
    if (gTrkFila == 0) {
      pantallaTracksCierra();
      menuOpenEnSeccion(14);        // 14 = la seccion "Tracks" (ver kMenuSections)
      return;
    }
    if (gTrkFila == 1) { pantallaTracksCierra(); menuClose(); return; }   // "Salir"
    // Una salida: se guia HACIA ATRAS por su trozo de track (del final al principio, que es
    // lo que significa "volver a casa").
    const uint8_t idx = (uint8_t)(gTrkFila - 2);
    TrackSesion s;
    if (!tracksSesionesLee(idx, &s) || !s.valida) {
      displayPopupWait("Esa salida ya no tiene track", 3000);
      return;
    }
    const uint32_t desde = tracksSesionDesde(idx);
    const uint32_t hasta = tracksSesionHasta(idx);
    if (!tracksGuiaEmpiezaTrozo(desde, hasta, true /* alReves */)) {
      displayPopupWait("Esa salida no tiene puntos suficientes", 3000);
      return;
    }
    gTrkZoomCompleto = false;
    pantallaTracksAbre(TRK_PANT_GUIA, 0);
    return;
  }

  if (gTrkPant == TRK_PANT_LISTA) {
    if (gTrkFila == 0) { pantallaTracksCierra(); return; }     // "< Volver"
    if (gTrkFila == 1) { pantallaTracksCierra(); menuClose(); return; }   // "Salir"
    if (gTrkFila == kTrkFilaCasa) {
      // ★★ "VOLVER A CASA" AHORA ENSENA LAS SALIDAS (2026-10-06) ★★
      //   ANTES arrancaba el guiado directamente sobre el track en vivo ENTERO. El problema, que
      //   encontro el operador: el track en vivo es UNA sola tirada con las salidas de varios dias
      //   pegadas, asi que "el principio" podia ser el de anteayer, y no habia forma de elegir por
      //   cual volver.
      //   AHORA abre la pantalla de SALIDAS: el aparato parte su registro en salidas (cada vez que
      //   pasa mucho tiempo sin balizas = se acabo la salida) y el operador elige la suya. Al
      //   elegirla se guia HACIA ATRAS por el trozo de track de ESA salida.
      gSalidasLeidas = false;        // que se rehaga la lista al entrar (el track va cambiando)
      gSalidasCuenta = 0;
      pantallaTracksAbre(TRK_PANT_SALIDAS, 2);   // fila 2 = la salida mas reciente
      return;
    }
    // ★ Y DEBAJO SOLO QUEDAN LAS RANURAS: las rutas de Wikiloc que se ha bajado el operador.
    //   (Antes habia aqui tambien el track en vivo y "Empezar nuevo", que se han quitado: el
    //   primero esta ahora dentro de "Volver a casa", y el segundo ya no hace falta. Ver la nota
    //   larga donde se declaran las filas, arriba del todo.)
    if (gTrkFila >= kTrkFilaSlot0 && gTrkFila < kTrkFilaSlot0 + (int)TRACK_SLOTS) {
      const uint8_t slot = (uint8_t)(gTrkFila - kTrkFilaSlot0);
      TrackRanuraInfo inf;
      tracksRanuraInfo(slot, &inf);
      if (!inf.valida) {
        displayPopupWait("Esa ranura esta vacia", 3000);
        return;
      }
      gTrkElegido = slot;
      pantallaTracksAbre(TRK_PANT_ACCION, 2);
      return;
    }
    return;
  }

  if (gTrkPant == TRK_PANT_ACCION) {
    // ★★ OJO CON ESTE `2`, QUE AHORA ESTA MAL (2026-10-06) ★★
    //   Esta pantalla se abre SIEMPRE desde una ranura, y antes la primera ranura de la lista era
    //   la fila 2... pero al reorganizar las filas, **la 2 es ahora "Volver a casa"**. Volver con
    //   el `2` dejaba el cursor en "Volver a casa", o sea que el operador **creia estar en una
    //   ranura y estaba en otra fila**. Se usa la constante, que no se puede quedar desfasada.
    if (gTrkFila == 0) { pantallaTracksAbre(TRK_PANT_LISTA, kTrkFilaSlot0); return; }   // "< Volver"
    if (gTrkFila == 1) { pantallaTracksCierra(); menuClose(); return; }     // "Salir"
    if (gTrkFila == 4) {
      // "Finalizar guiado": se para el motor y se vuelve a la lista de tracks, que es donde
      // estaba. Si no habia guiado en marcha, se dice (no se finge que se ha hecho algo).
      if (!tracksGuiaActivo()) { displayPopupWait("No hay guiado en marcha", 3000); return; }
      tracksGuiaTermina();
      pantallaTracksAbre(TRK_PANT_LISTA, kTrkFilaSlot0);
      return;
    }
    // â˜…â˜… "Hacia adelante" (2) y "Hacia atras" (3): AQUI ARRANCA EL GUIADO DE VERDAD â˜…â˜…
    //   Se le dice al motor de donde sale el track y en que sentido, y se abre la pantalla de
    //   guiado. El motor exige al menos 2 puntos y, si no los hay, no arranca: por eso se mira
    //   el resultado en vez de dar por hecho que ha ido bien.
    {
      const TrackFuente fuente = (gTrkElegido < 0) ? TRK_FUENTE_VIVO : TRK_FUENTE_RANURA;
      const bool alReves = (gTrkFila == 3);
      if (!tracksGuiaEmpieza(fuente, gTrkElegido, alReves)) {
        displayPopupWait("Ese track no tiene puntos suficientes", 3000);
        return;
      }
      gTrkZoomCompleto = false;     // se empieza de CERCA: al guiar se quiere navegar
      pantallaTracksAbre(TRK_PANT_GUIA, 0);
      return;
    }
  }
}

// ---------------------------------------------------------------------------------------
//  La pantalla de GUIADO tiene sus propias reglas de boton:
//    - CORTO: cambia entre la VENTANA DE CERCA y el TRACK COMPLETO (lo pidio el operador).
//    - LARGO: vuelve al menu de Tracks, para poder "Finalizar guiado". â˜… No se cierra el
//      guiado: solo se sale a mirarlo, que es lo que se espera al pulsar largo.
// ---------------------------------------------------------------------------------------
void pantallaGuiaCorto() {
  gTrkZoomCompleto = !gTrkZoomCompleto;
  gDirty = true;
}

void pantallaGuiaLargo() {
  // ★ DE LA PANTALLA DE GUIADO SE VUELVE AL MENU DE TRACKS, no a la lista intermedia
  //   (2026-10-06). La lista intermedia ya no existe como puerta: se entraba a ella desde el menu
  //   y llevaba a las mismas dos cosas que el menu, asi que sobraba. Volver aqui deja al operador
  //   donde puede elegir otra vez, que es lo que espera al pulsar largo.
  pantallaTracksCierra();
  menuOpenEnSeccion(14);        // 14 = la seccion "Tracks"
}

void pantallaTracksLargo() {
  // La pulsacion LARGA sube un nivel, igual que en el menu: de la pantalla de accion a la
  // lista, y de la lista al menu. Asi el gesto dice lo mismo en todas partes.
  if (gTrkPant == TRK_PANT_ACCION) { pantallaTracksAbre(TRK_PANT_LISTA, kTrkFilaSlot0); return; }
  // ★ Y DE LAS SALIDAS SE VUELVE AL MENU (2026-10-06), por el mismo motivo que en la de guiado:
  //   la lista intermedia sobraba, y volver a ella dejaba al operador dando vueltas entre dos
  //   pantallas que ensenaban lo mismo.
  if (gTrkPant == TRK_PANT_SALIDAS) {
    pantallaTracksCierra();
    menuOpenEnSeccion(14);
    return;
  }
  pantallaTracksCierra();
}

#endif  // TRACKS_DISPONIBLE

void menuNavigate() {
  // â˜… Las pantallas de TRACKS van primero: cuando estan abiertas SUSTITUYEN al menu, asi que
  //   se llevan el boton. Navegan por sus filas con wrap, como el menu.
#ifdef TRACKS_DISPONIBLE
  if (pantallaTracksActiva()) {
    // ★ NINGUNA PANTALLA DE TRACKS SE CIERRA POR TIEMPO (2026-10-06).
    //   Antes la de lista y la de accion se cerraban a los 15 s, como el menu. Y estaba mal por
    //   dos motivos que se juntan:
    //     1. Una LISTA se lee, y el aparato no se refresca solo (es tinta electronica): se queda
    //        pintada mientras el operador la mira. A los 15 s, la siguiente pulsacion **cerraba
    //        la pantalla en vez de moverse por la lista**: el boton parecia no hacer nada.
    //     2. Y lo peor: si el guiado estuviera en marcha, cerrarse por tiempo lo dejaria guiando
    //        **sin ensenarlo**.
    //   Se sale como en la pantalla de guiado: pulsacion larga (sube un nivel) o la fila «Salir».
    if (gTrkPant == TRK_PANT_GUIA) { pantallaGuiaCorto(); return; }
    pantallaTracksNavega();
    return;
  }
#endif
  if (!gMenuOn) return;
  gMenuLastActMs = millis();
  gUltimoToqueMs = millis();   // sello de "aqui ha habido mano del operador"
  gUltimoToqueAgrupaMs = millis();   // el tactil SI agrupa: puede ir en rafaga (Butter)
  displayBacklightKick();
  // ---- CONFIRMACION PENDIENTE: navegar NO hace nada ----
  // Mientras se esta preguntando "seguro?", mover el cursor seria invisible (la pantalla
  // enseÃ±a el aviso, no la lista) y ademas dejaria al operador sin saber donde esta al
  // volver. Se IGNORA el toque, igual que la OLED ignora el segundo "largo" cuando ya ha
  // pedido la confirmacion. Se sale confirmando (pulsacion CORTA del fisico: ver
  // menuEjecutaAccion) o dejando vencer la ventana. (Este comentario decia "largo" hasta el
  // 2026-09-16: era el mismo error que la pantalla, que mandaba pulsar largo, que es cancelar.)
  if (confirmacionPendiente()) return;
  // ---- SUBMENU DE OPCIONES (enum): navegar va por [Volver, Salir, opcion...] ----
  if (gMenuEnumAbs >= 0 && gMenuEnumAbs < kMenuCount) {
    const MenuItem &it = kMenu[gMenuEnumAbs];
    int nopt = 0; while (it.opts && it.opts[nopt]) nopt++;
    int total = 2 + nopt;                 // Volver(0) Salir(1) opciones(2..)
    gMenuEnumIdx = (gMenuEnumIdx + 1) % total;   // wrap: al llegar abajo vuelve a Volver
    gDirty = true; return;
  }
  // ---- EDITOR DE PERFILES ----
  if (gMenuPerfMode == 1) {        // 0="< Volver"; 1="Salir"; 2..5=perfiles; 6="Editar ajs."
    // â˜…â˜… EL TOPE TIENE QUE SER `kPerfFilas`, NO UN 5 A MANO (2026-09-22) â˜…â˜…
    //   Aqui ponia `% 5` porque entonces la lista tenia 5 filas (4 perfiles + editar). Al
    //   pasar a la estructura comun son 7, asi que con el 5 las dos ultimas filas quedaban
    //   INALCANZABLES: el resaltado daba la vuelta antes de llegar. Es el tipo de fallo que no
    //   se ve en una lectura rapida del codigo y que en la pantalla se nota como "no me deja
    //   bajar hasta abajo".
    gMenuPerfIdx = (gMenuPerfIdx + 1) % kPerfFilas;
    gDirty = true; return;
  }
  if (gMenuPerfMode == 2) {        // editar un campo del perfil: ciclo de valor
    int p = gMenuPerfIdx;
    if (gMenuPerfField == 0) {     // SSID 1..15 (anti-duplicado lo decide al guardar via config)
      int s = gCfg->profileSsid[p] + 1; if (s > 15) s = 0;
      gCfg->profileSsid[p] = (uint8_t)s;
    } else if (gMenuPerfField == 1) {  // tiempo lento (0..3600, paso 30)
      int v = gCfg->profileSlowSec[p] + 30; if (v > 3600) v = 0;
      gCfg->profileSlowSec[p] = v;
    } else if (gMenuPerfField == 2) {  // tiempo rapido
      int v = gCfg->profileFastSec[p] + 10; if (v > 3600) v = 0;
      gCfg->profileFastSec[p] = v;
    } else if (gMenuPerfField == 3) {  // metros (0..5000, paso 50)
      int v = gCfg->profileDistM[p] + 50; if (v > 5000) v = 0;
      gCfg->profileDistM[p] = v;
    } else {                            // â˜… ICONO DEL MAPA de este perfil (2026-09-15)
      // Se recorre la MISMA rueda que ensena la pantalla (kIconoNombre). Si el
      // valor guardado no es uno de los cuatro (lo puso el configurador), el
      // primer toque entra por el principio de la rueda en vez de quedarse quieto.
      int idx = iconoPerfilIndice(p);
      idx = (idx < 0) ? 0 : ((idx + 1) % kIconoOpciones);
      iconoPerfilPon(p, idx);
    }
    storeSave(*gCfg);
    gDirty = true; return;
  }
  if (gMenuEditing) {
    const MenuItem &it = kMenu[gMenuEditItemAbs];
    editaSiguiente(it);
    if (it.kind == MK_STRING) { gEditPos++; if (gEditPos > 15) gEditPos = 0; }
    gDirty = true;
    return;
  }
  if (gMenuCat < 0) {
    int n = menuMainSecCount();
    gMenuIdx = (gMenuIdx + 1) % menuMainTotFilas(n);   // wrap
  } else {
    int a = kMenuSectionFirst[gMenuCat];
    int b = kMenuSectionEnd[gMenuCat];
    int itemCount = b - a;
    int total = itemCount + 2;                  // + Volver + Salir
    // lineas: 0=Volver, 1=Salir, 2..=items
    int idx = gMenuIdx; int guard = total;
    do { idx = (idx + 1) % total; guard--; }
    while (guard > 0 && idx >= 2 && idx < total && !menuItemVisible(kMenu[a + (idx-2)]));
    gMenuIdx = idx;
  }
  gDirty = true;
}

void menuShort() {
#ifdef TRACKS_DISPONIBLE
  // ★ Las pantallas de tracks se llevan el boton cuando estan abiertas (sustituyen al menu).
  if (pantallaTracksActiva()) {
    if (gTrkPant == TRK_PANT_GUIA) { pantallaGuiaCorto(); return; }   // no se cierra por tiempo
    pantallaTracksCorto();
    return;
  }
#endif
  if (!gMenuOn) return;
  gMenuLastActMs = millis();
  displayBacklightKick();
  // ---- SUBMENU DE OPCIONES (enum) ----
  if (gMenuEnumAbs >= 0 && gMenuEnumAbs < kMenuCount) {
    const MenuItem &it = kMenu[gMenuEnumAbs];
    if (gMenuEnumIdx == 0) { gMenuEnumAbs = -1; gDirty = true; return; }   // Volver
    if (gMenuEnumIdx == 1) { menuClose(); return; }                        // Salir
    // guardar la opcion elegida (la fila 2+i)
    int optIdx = gMenuEnumIdx - 2;
    char buf[24];
    if (it.optVals) snprintf(buf,sizeof buf,"%d", it.optVals[optIdx]);
    else if (it.optValsF) snprintf(buf,sizeof buf,"%.1f",(double)it.optValsF[optIdx]);
    else snprintf(buf,sizeof buf,"%d", optIdx);
    menuSave(it.key, buf);
    gMenuEnumAbs = -1;
    gDirty = true;
    return;
  }
  // ---- EDITOR DE PERFILES ----
  // â˜…â˜… MISMAS FILAS QUE LOS DEMAS SUBMENUS (2026-09-22) â˜…â˜…
  //   Los indices son los mismos que en el submenu de opciones, a proposito:
  //       0 = "< Volver"  -> sube un nivel (lo mismo que el largo del boton fisico)
  //       1 = "Salir"     -> cierra el menu
  //       2..5            -> los cuatro perfiles (elegir uno lo hace ACTIVO y lo guarda)
  //       6 = "Editar ajs." -> editar los campos del perfil activo
  //   Antes solo existian 0..4 y su sentido era OTRO (0..3 = perfil, 4 = editar ajustes), o
  //   sea que la fila 0 elegia un perfil en vez de volver. Ese desplazamiento es justo lo que
  //   hacia que este submenu pareciera de otro firmware.
  if (gMenuPerfMode == 1) {
    if (gMenuPerfIdx == 0) { gMenuPerfMode = 0; gDirty = true; return; }   // Volver
    if (gMenuPerfIdx == 1) { menuClose(); return; }                        // Salir
    if (gMenuPerfIdx < 2 + 4) {
      gCfg->smartBeaconPreset = (uint8_t)(gMenuPerfIdx - 2);   // elegir perfil = hacerlo activo
      storeSave(*gCfg);
      gDirty = true; return;
    }
    // fila 6: editar los ajustes del perfil ACTIVO (no del resaltado): se entra siempre en el
    // que esta en uso, que es lo que el operador espera al decir "editar este perfil".
    int activo = gCfg->smartBeaconPreset; if (activo > 3) activo = 0;
    gMenuPerfIdx = activo; gMenuPerfField = 0; gMenuPerfMode = 2;
    gDirty = true; return;
  }
  if (gMenuPerfMode == 2) {
    gMenuPerfField++; if (gMenuPerfField > kPerfCampos - 1) { gMenuPerfField = 0; gMenuPerfMode = 1; }
    gDirty = true; return;
  }
  if (gMenuEditing) {
    // ARREGLO G1 (auditoria 2026-09-15): al confirmar una edicion de STRING se debe
    // PERSISTIR el buffer. Para INT/ENUM/PATH/FLOAT el valor ya se guardo "live" en
    // cada navegacion; para STRING el buffer se construye en memoria y aqui se escribe.
    if (gMenuEditItemAbs >= 0 && gMenuEditItemAbs < kMenuCount) {
      const MenuItem &it = kMenu[gMenuEditItemAbs];
      if (it.kind == MK_STRING) {
        char cleaned[sizeof(gEditBuf)];
        int p = 0;
        for (int i=0; gEditBuf[i] && gEditBuf[i]!='~' && i<(int)sizeof(cleaned)-1; i++)
          cleaned[p++] = gEditBuf[i];
        cleaned[p] = '\0';
        menuSave(it.key, cleaned);
      }
    }
    gMenuEditing = false;
    gDirty = true;
    return;
  }
  if (gMenuCat < 0) {
    int n = menuMainSecCount();
    if (menuMainFilaSalir(gMenuIdx, n)) { menuClose(); return; }
    // Dormir (fila virtual del menu principal): pasa por el MISMO camino que las demas
    // acciones, asi que tambien pide confirmacion (auditoria G2).
    if (menuMainEsDormir(gMenuIdx)) { menuEjecutaAccion(ACT_SLEEP); return; }
    if (menuMainEsApagar(gMenuIdx)) { menuEjecutaAccion(ACT_SHUTDOWN); return; }
    int sec = menuMainFilaSeccion(gMenuIdx, n);
    // â˜… SECCION "MODO" abre DIRECTAMENTE la lista de modos (2026-09-15): como esa seccion
    //   solo tiene el Ã­tem "Modo", se salta un nivel y van los modos por linea, sin la
    //   pantalla intermedia de "Modo -> Modo".
    if (sec == 0) {
      gMenuEnumAbs = 0;              // Ã­tem "Modo" (kMenu[0])
      gMenuEnumIdx = 2;
      gDirty = true; return;
    }
    gMenuCat = sec; gMenuIdx = 0; gDirty = true;
    return;
  }
  // submenu
  int a = kMenuSectionFirst[gMenuCat];
  int b = kMenuSectionEnd[gMenuCat];
  int idx = gMenuIdx;
  if (idx == 0) { gMenuCat = -1; gMenuIdx = 0; gDirty = true; return; }      // Volver atras
  if (idx == 1) { menuClose(); return; }                                     // Salir
  int iAbs = idx - 2;
  if (iAbs < (b - a)) {
    const MenuItem &it = kMenu[a + iAbs];
#ifdef TRACKS_DISPONIBLE
    // ★★ LAS TRES FILAS DE TRACKS ABREN SU PANTALLA, NO EJECUTAN UNA ACCION (2026-09-22) ★★
    //   Se interceptan AQUI, antes del despacho de acciones, y por su `key`. Asi no hay forma
    //   de que acaben en `menuEjecutaAccion` (que es para reiniciar, borrar y demas) ni en el
    //   editor de valores: son puertas, no ajustes. Ver `pantallaTracks()`.
    //
    // ★★★ «VOLVER A CASA» LLEVABA A UNA RANURA VACIA, Y NO HACIA NADA (2026-10-06) ★★★
    //   ESTABA PUESTO UN `7` CLAVADO A MANO. Y la fila de «Volver a casa» es la **9**
    //   (`kTrkFilaCasa`). Resultado exacto de lo que vio el operador: pulsabas «Volver a casa» en
    //   el menu, la pantalla abria la lista **posada en la RANURA 4** (una fila de ranura), y las
    //   ranuras piden **pulsacion LARGA**: al pulsar corto **NO PASABA ABSOLUTAMENTE NADA**.
    //   Lo conto asi: «he probado a activar la opcion pero no sale el track actual, de hecho en
    //   ese menu no funciona ninguna opcion». Las dos frases son este `7`.
    //   ★ Y LA LECCION, que es la de siempre en este proyecto: **un numero clavado a mano al lado
    //     de una constante acaba separandose de ella**. Aqui el numero era 7 y la constante 9, y
    //     no habia nada que lo comprobara. Ahora se usan LAS CONSTANTES, y ademas hay un
    //     comprobador (`tools/prueba_filas_menu_tracks.py`) que mira que estas filas apunten a
    //     filas que existen y que sean las suyas.
    //
    // ★★ Y SON DOS FILAS, NO TRES (2026-10-06): se ha quitado la del track en vivo, que ahora se
    //   mira dentro de "Volver a casa" (que es donde se usa). Ver la nota larga donde se declaran
    //   las filas de la lista de tracks.
    //
    // ★★★ Y AQUI SE ABRE LA PANTALLA DE VERDAD, NO UNA LISTA INTERMEDIA (2026-10-06) ★★★
    //   El primer intento abria `TRK_PANT_LISTA` (la lista con "Volver a casa" y las ranuras), y
    //   eso dejaba al operador con **dos pantallas para lo mismo**: el menu le llevaba a una lista
    //   que no servia para nada, y dentro estaba otra vez "Volver a casa". Su queja, textual:
    //     «dentro de volver a casa sale volver salir volver a casa (no tiene sentido esto)»
    //   ★ Y encima "< Volver" no volvia, porque iba de una de esas dos pantallas a **la otra**:
    //     un bucle. Ahora "< Volver" vuelve AL MENU.
    //   ★ AHORA: "Volver a casa" abre directo la pantalla de TUS TRACKS GRABADOS, y "Ranuras"
    //     abre las rutas de Wikiloc. Una pantalla cada cosa, y ninguna de mas.
    if (it.key && !strcmp(it.key, "tkCasa")) {
      gSalidasLeidas = false;      // que se rehaga la lista al entrar (el track va cambiando)
      gSalidasCuenta = 0;
      pantallaTracksAbre(TRK_PANT_SALIDAS, 0);   // fila 0 = "< Volver"
      return;
    }
    if (it.key && !strcmp(it.key, "tkSlots")) {
      pantallaTracksAbre(TRK_PANT_LISTA, kTrkFilaSlot0);
      return;
    }
#endif
    gMenuEditItemAbs = a + iAbs;
    if (it.kind == MK_ACTION) { menuEjecutaAccion(it.action); }
    else if (it.kind == MK_BOOL) { editaSiguiente(it); }
    else if (it.kind == MK_ENUM_CYCLE || it.kind == MK_ENUM || it.kind == MK_ENUM_F) {
      gMenuEnumAbs = a + iAbs;
      gMenuEnumIdx = 2;
      // DIAGNOSTICO (2026-09-15): ver en el USB que opciones tiene el submenu, porque se
      // reporto que en "Modo" solo sale un "Modo". Netif enhorabuena.
      // Va con el modo diagnostico (2026-09-16): es una traza de taller, no algo que tenga
      // que ver el operador cada vez que abre un desplegable.
      if (diagTrazaTaller()) {
        int nd = 0; while (it.opts && it.opts[nd]) nd++;
        Serial.printf("MENU enum %s key=%s nopt=%d | %s / %s / %s\r\n",
                      it.label, it.key ? it.key : "?", nd,
                      (nd>0)?it.opts[0]:"", (nd>1)?it.opts[1]:"", (nd>2)?it.opts[2]:"");
      }
      gDirty = true;
    }
    else { comenzarEdicion(it); }
  }
}

void menuLong() {
#ifdef TRACKS_DISPONIBLE
  if (pantallaTracksActiva()) {
    if (gTrkPant == TRK_PANT_GUIA) { pantallaGuiaLargo(); return; }
    pantallaTracksLargo();
    return;
  }
#endif
  if (!gMenuOn) return;
  gMenuLastActMs = millis();
  displayBacklightKick();
  // ---- CONFIRMACION PENDIENTE: largo = cancelar (no se ejecuta nada) ----
  // Se mira ANTES que lo demas: la pulsacion larga en la pantalla de confirmacion es un
  // "no". Es el equivalente al gesto con el que la OLED cancela (moverse a otra fila).
  if (confirmacionPendiente()) { gConfirmAction = 0; gDirty = true; return; }
  // ---- SUBMENU DE OPCIONES: largo = volver ----
  if (gMenuEnumAbs >= 0) { gMenuEnumAbs = -1; gDirty = true; return; }
  // ---- EDITOR DE PERFILES: largo = volver un nivel ----
  if (gMenuPerfMode == 2) { gMenuPerfMode = 1; gDirty = true; return; }
  if (gMenuPerfMode == 1) { gMenuPerfMode = 0; gDirty = true; return; }
  if (gMenuEditing) { gMenuEditing = false; gDirty = true; return; }
  if (gMenuCat < 0) { menuClose(); }
  else { gMenuCat = -1; gMenuIdx = 0; gDirty = true; }   // vuelve a categorias
}

void menuOpen() { gMenuOn = true; gMenuCat = -1; gMenuIdx = 0; gMenuPerfMode = 0; gMenuLastActMs = millis(); displayBacklightKick(); gDirty = true; }

/**
 * Abre el menu DIRECTAMENTE en una seccion, con el cursor en su primera fila (2026-10-06).
 *
 * PARA QUE: al salir de las pantallas de tracks con "< Volver" hay que dejar al operador **donde
 * estaba**, o sea en la seccion de Tracks del menu, no en la lista de secciones. Si volviera a la
 * lista de secciones, tendria que bajar otra vez hasta Tracks cada vez: con un boton, eso son
 * diez pulsaciones por vuelta.
 *
 * ★ Se reutiliza `menuOpen()` a proposito en vez de tocar `gMenuOn` a mano: asi se hace TODO lo que
 *   hay que hacer al abrir el menu (marcar actividad, encender la luz, pedir repintado). Copiar
 *   esas cuatro lineas aqui seria la forma segura de que un dia una se quede atras.
 */
void menuOpenEnSeccion(int seccion) {
  menuOpen();
  const int n = menuMainSecCount();
  if (seccion < 0 || seccion >= n) return;      // seccion imposible: se queda en la lista
  gMenuCat = seccion;
  gMenuIdx = 0;                                  // fila 0 = "< Volver" de la seccion
  gDirty = true;
}
// Al cerrar se olvida cualquier confirmacion pendiente: si no, al volver a entrar en el
// menu con la ventana de 3 s todavia viva, la primera pulsacion larga ejecutaria el
// borrado sin preguntar (justo lo que este arreglo viene a impedir).
void menuClose() { gMenuOn = false; gMenuEditing = false; gMenuIdx = 0; gMenuPerfMode = 0; gConfirmAction = 0; gConfirmUntil = 0; gDirty = true; }
// â˜…â˜… DOBLE TOQUE = RESTAR (2026-09-15) â˜…â˜…
// Esta funcion es el UNICO camino por el que entra el DOBLE TOQUE fisico en el menu:
// main.cpp (handleButton, rama BTN_DOUBLE) solo la llama `if (menuIsEditing())`, asi que
// desde aqui no se puede estropear nada de las otras pantallas. El porque del gesto y el
// mapa completo de botones estan contados en editaResta().
//   - Campo NUMERICO (MK_INT/MK_FLOAT): resta un paso y SE QUEDA editando, para poder
//     seguir corrigiendo con el capacitivo y ver el valor nuevo en el acto.
//   - Cualquier otro campo (texto, path): CANCELA la edicion, como hasta hoy.
// Lo que NO se toca: la cancelacion de la sesion "Fijar coords" (vive en displayRefresh y
// mira gUltimoToqueMs, que aqui no se escribe) ni el antirrebote. Y el cierre del menu por
// inactividad (15 s) se refresca a proposito cuando se resta: esto ES mano del operador y
// no debe contar como "15 s sin tocar".
// Salir sin guardar tampoco se pierde en los numericos: el toque LARGO hace lo mismo que
// esta funcion hacia antes (menuLong: `if (gMenuEditing) { gMenuEditing = false; ... }`).
void menuEditCancel() {
  if (gMenuEditing && gMenuEditItemAbs >= 0 && gMenuEditItemAbs < kMenuCount) {
    const MenuItem &it = kMenu[gMenuEditItemAbs];
    if (it.kind == MK_INT || it.kind == MK_FLOAT) {
      editaResta(it);
      gMenuLastActMs = millis();   // ha habido mano del operador: el menu no se cierra por 15 s
      gDirty = true;
      return;
    }
  }
  gMenuEditing = false;   // texto/path (y demas casos): sigue siendo CANCELAR la edicion
  gDirty = true;
}

// ---- RENDER ----
// Auto-cierre del menu a los 15 s sin tocar (peticion del operador, 2026-09-15): si nadie
// interactua, el menu se cierra solo y la pantalla vuelve al carrusel.
// â˜…â˜… LOS PERFILES PASAN A LA ESTRUCTURA COMUN DEL MENU (2026-09-22) â˜…â˜…
//
// QUE PASABA (lo cazo el operador): el submenu de perfiles era el UNICO que no seguia el patron
// de los demas. Dos diferencias, y las dos se ven en la pantalla:
//
//   1. NO TENIA "Volver" NI "Salir" EN PANTALLA. Los demas submenus los llevan en las filas 0
//      y 1 (`< Volver` / `Salir`, ver `menuShort` y el pintado del submenu de opciones). Aqui
//      no habia nada: para salir habia que saber que la PULSACION LARGA del boton fisico sube
//      un nivel (`menuLong`), cosa que la pantalla no decia por ningun lado. El nodo no se
//      quedaba atrapado, pero el operador no tenia forma de saberlo mirando.
//   2. EL TEXTO ERA MAS PEQUENO: escala 1, cuando el resto del menu va a escala 2
//      (`const int esc = 2;   // letra mas grande (peticion del operador)`). Se veia
//      claramente mas pequeno que el menu del que venias.
//
// AHORA usa las MISMAS filas que los demas submenus, con los MISMOS indices:
//      fila 0        -> "< Volver"        (sube un nivel, igual que el largo del fisico)
//      fila 1        -> "Salir"           (cierra el menu)
//      filas 2..5    -> los cuatro perfiles
//      fila 6        -> "Editar ajs."     (editar los ajustes del perfil ACTIVO)
// para que el gesto y la pantalla digan lo mismo en todo el menu.
const char *kPerfName[4] = {"Fijo/Digi", "Peaton", "Bicicleta", "Coche"};

void menuPinta() {
  if (gMenuOn && (uint32_t)(millis() - gMenuLastActMs) > 15000) {
    menuClose();
    return;
  }
  clearBuf(true);
  const int esc = 2;                       // letra mas grande (peticion del operador)
  const int rowH = 18;                     // alto de fila para escala 2
  const int y0 = 30;

  // â˜…â˜… PANTALLA DE CONFIRMACION (2026-09-15, arreglo de la auditoria G2) â˜…â˜…
  //   En la OLED el aviso es el popup "Pulsa largo: confirmar", que dura lo que dura un
  //   popup. Aqui NO vale: el panel tarda ~1,5 s por refresco y el operador se lo perderia.
  //   Por eso el aviso es una PANTALLA ENTERA que se queda fija (tinta bistable) hasta que
  //   se confirme, se cancele con una pulsacion larga o venza la ventana, con la misma letra
  //   y el mismo estilo que el resto del menu:
  //       "CONFIRMAR"                          (escala 1, arriba)
  //       [nombre de la accion]                (escala 2, dentro de un recuadro)
  //       "pulsa CORTO otra vez"               (escala 1)
  //       "para confirmar"
  //       "fisico largo: cancelar"             (escala 1, abajo)
  //   Si vence la ventana (kConfirmMs) sin confirmar, esto se apaga solo en el siguiente
  //   refresco (menuPinta se llama desde displayRefresh) y vuelve a verse la lista.
  //
  //   â˜…â˜… OJO: AQUI PUSO "PULSA LARGO OTRA VEZ" Y ESTUVO MAL HASTA EL 2026-09-16 â˜…â˜…
  //     Ese texto venia copiado de la OLED, donde el gesto que confirma SI es el largo. En
  //     esta pantalla es AL REVES, porque el mapa del menu de tinta es el otro (el que dice
  //     el manual): CORTO = entrar / ejecutar / confirmar; LARGO = volver atras, y aqui
  //     volver atras es CANCELAR (ver menuLong: `if (confirmacionPendiente()) gConfirmAction = 0`).
  //     O sea que la pantalla mandaba hacer justo el gesto que cancela, y ademas se
  //     contradecia con su propia ultima linea ("fisico largo: cancelar"). Lo cazo el
  //     operador: Â«mantengo pulsado y solo sale del menuÂ». NO es del Project Butter: el
  //     gesto no se ha tocado, lo que estaba mal era la instruccion.
  if (gMenuOn && confirmacionPendiente()) {
    const char *tit = tituloConfirmacion(gConfirmAction);
    if (tit) {
      drawTextCenter(16, "CONFIRMAR", 1);
      // Recuadro alrededor del nombre de la accion, para que se lea de un vistazo.
      const int tw = textWidth(tit, 2);
      int cx0 = (EPD_W - tw) / 2 - 8; if (cx0 < 4) cx0 = 4;
      int cx1 = (EPD_W + tw) / 2 + 8; if (cx1 > EPD_W - 4) cx1 = EPD_W - 4;
      barraH(cx0, cx1, 58, 2);
      barraH(cx0, cx1, 84, 2);
      barraV(cx0, 58, 84, 2);
      barraV(cx1, 58, 84, 2);
      drawTextCenter(64, tit, 2);
      drawTextCenter(120, "pulsa CORTO otra vez", 1);
      drawTextCenter(134, "para confirmar", 1);
      drawTextCenter(168, "fisico largo: cancelar", 1);
      return;
    }
  }

  // â˜… PANTALLA DE EDICION LIMPIA (2026-09-15): al editar un valor se dibuja SOLO esta
  //   pantalla (etiqueta + valor grande + cursor), NO el menu detras. Sustituye al antiguo
  //   banner que se montaba encima del menu arruinando la lectura.
  if (gMenuOn && gMenuEditing && gMenuEditItemAbs >= 0 && gMenuEditItemAbs < kMenuCount) {
    const MenuItem &it = kMenu[gMenuEditItemAbs];
    drawTextCenter(12, it.label, 2);
    // â˜… AVISO DE AJUSTE QUE NO HACE NADA (2026-09-15, arreglo de la auditoria G3).
    //   Aqui es donde el operador se entera: no en la lista (donde el "(no)" se pierde
    //   entre comillas), sino al ENTRAR a tocarlo. Se dice que se guarda (es verdad: se
    //   guarda en la flash y lo puede usar la OLED o el configurador) pero que en esta
    //   placa no cambia nada. Se pinta ANTES del "return" de la pantalla de edicion para
    //   que salga en el mismo refresco.
    if (avisoItemInerte(it.key)) {
      drawTextCenter(34, "se guarda, pero en esta", 1);
      drawTextCenter(46, "pantalla NO hace nada", 1);
    }
    // el valor que se esta construyendo, en grande y centrado
    int ww = textWidth(gEditBuf, 3);
    if (ww > EPD_W - 16) ww = EPD_W - 16;
    drawTextCenter(70, gEditBuf, 3);
    int xx = (EPD_W - textWidth(gEditBuf, 3)) / 2; if (xx < 4) xx = 4;
    int cxl = xx + gEditPos * textWidth("W", 3);
    barraV(cxl, 96, 100, 6);              // subrayado del caracter que se edita
    // â˜… PISTA DEL GESTO DE RESTAR (2026-09-15): el doble toque no tiene ninguna marca en
    //   la pantalla, asi que se dice aqui. SOLO en los campos numericos, que son los
    //   unicos en los que el doble toque resta: en el texto sigue siendo "cancelar la
    //   edicion" (ver menuEditCancel) y ponerlo ahi seria mentir. Escala 1 y centrado:
    //   "capacitivo suma, doble resta" son 29 caracteres = 174 px de los 200 del panel, y
    //   cae en el hueco que hay entre el cursor (y=96..100) y el pie (y=150).
    if (it.kind == MK_INT || it.kind == MK_FLOAT) {
      drawTextCenter(126, "capacitivo suma, doble resta", 1);
    }
    drawTextCenter(150, "fisico largo: volver", 1);
    return;
  }

  // ---- SUBMENU DE OPCIONES (enum): Volver/Salir arriba, cada opcion en su linea ----
  if (gMenuEnumAbs >= 0 && gMenuEnumAbs < kMenuCount) {
    const MenuItem &it = kMenu[gMenuEnumAbs];
    drawTextCenter(12, it.label, 1);
    int nopt = 0; while (it.opts && it.opts[nopt]) nopt++;
    int total = 2 + nopt;
    int vis = 8;
    int top = gMenuEnumIdx - (vis / 2); if (top < 0) top = 0;
    if (top > total - vis) top = (total - vis < 0) ? 0 : total - vis;
    int y = y0;
    for (int r = top; r < total && y < 186; r++, y += rowH) {
      if (r == gMenuEnumIdx) { barraH(6, EPD_W-6, y-1,1); barraH(6, EPD_W-6, y+rowH-2,1); barraV(6,y-1,y+rowH-2,1); barraV(EPD_W-6,y-1,y+rowH-2,1); }
      if (r == 0) { drawText(14, y, "< Volver", esc); continue; }
      if (r == 1) { drawText(14, y, "Salir", esc); continue; }
      const char *opt = it.opts ? it.opts[r - 2] : "";
      const char *cur = menuValorTexto(it);
      bool activa = opt && !strcmp(opt, cur);
      char row[40];
      snprintf(row, sizeof(row), "%s%s", opt ? opt : "", activa ? "   >" : "");
      drawText(14, y, row, esc);
    }
    return;
  }

  // ---- EDITOR DE PERFILES (2026-09-15) ----
  if (gMenuPerfMode == 1) {
    // â˜…â˜… MISMA ESTRUCTURA QUE LOS DEMAS SUBMENUS (2026-09-22) â˜…â˜…
    //   Antes esto era un bucle de 4 filas a ESCALA 1 y sin "< Volver" / "Salir": el unico
    //   submenu del T-Echo que no seguia el patron, y se notaba en las dos cosas (no habia
    //   salida a la vista y la letra era mas pequena que el resto del menu). Ver la nota de
    //   `kPerfName` para el detalle completo.
    //   Ahora: fila 0 = "< Volver", fila 1 = "Salir", filas 2..5 = los cuatro perfiles,
    //   fila 6 = "Editar ajs.", y todo a `esc` (escala 2) como los otros submenus.
    drawTextCenter(12, "PERFILES", 1);   // el titulo sigue a escala 1, como los demas
    int y = y0;
    for (int r = 0; r < kPerfFilas && y < 186; r++, y += rowH) {
      if (r == gMenuPerfIdx) { barraH(6, EPD_W-6, y-1,1); barraH(6, EPD_W-6, y+rowH-2,1); barraV(6,y-1,y+rowH-2,1); barraV(EPD_W-6,y-1,y+rowH-2,1); }
      if (r == 0) { drawText(14, y, "< Volver", esc); continue; }
      if (r == 1) { drawText(14, y, "Salir", esc); continue; }
      if (r < 2 + 4) {
        char row[40];
        snprintf(row, sizeof(row), "%s%s", kPerfName[r - 2],
                 (r - 2 == (int)gCfg->smartBeaconPreset) ? "  >" : "");
        drawText(14, y, row, esc);
        continue;
      }
      // fila 6: editar los ajustes del perfil ACTIVO. "Editar ajs." son 12 caracteres y caben
      // a escala 2 (tope 16): "Editar ajustes" serian 14 y tambien caben, pero con el "  >" de
      // marcas y para dejar aire se usa la forma corta.
      drawText(14, y, "Editar ajs.", esc);
    }
    return;
  }
  if (gMenuPerfMode == 2) {
    // â˜…â˜… TAMBIEN A ESCALA 2 (2026-09-22) â˜…â˜…
    //   Estaba a escala 1, como la lista de perfiles, y por el mismo motivo se veia mas pequeno
    //   que el resto del menu. Al subir a escala 2 hay que acortar los nombres de los campos:
    //   a escala 2 el tope son 16 caracteres, y las filas llevan "etiqueta: valor" mas el "  <"
    //   de "pulsa corto para cambiar".
    //
    //   â˜… LAS CUENTAS, porque aqui no cabe todo lo que cabia:
    //     "SSID: 15  <"        = 12  CABE
    //     "Lento(s): 300  <"   = 17  NO CABE  -> "Lento: 300  <"   = 14  CABE
    //     "Rapido(s): 30  <"   = 17  NO CABE  -> "Rapido: 30  <"   = 14  CABE
    //     "Metros: 150  <"     = 14  CABE
    //     "Icono: Coche  (/[)" = 19  NO CABE  -> "Icono: Coche  (/[)" no cabe; se deja el
    //                                            nombre solo cuando el par no cabe
    //   Lo que NO se puede acortar es el VALOR (los "Tiempo lent(s)" llegan a 3600 s), asi que
    //   el ancho se recorta por la etiqueta, no por el dato.
    char t[24];
    snprintf(t, sizeof(t), "Perfil %s", kPerfName[gMenuPerfIdx]);
    drawTextCenter(12, t, 1);
    const char *fn[kPerfCampos] = {"SSID (1-15)", "Lento", "Rapido", "Metros", "Icono"};
    int y = y0;
    for (int i = 0; i < kPerfCampos && y < 170; i++, y += rowH) {
      if (i == gMenuPerfField) { barraH(6, EPD_W-6, y-1,1); barraH(6, EPD_W-6, y+rowH-2,1); barraV(6,y-1,y+rowH-2,1); barraV(EPD_W-6,y-1,y+rowH-2,1); }
      char row[44];
      if (i == 0) {
        snprintf(row, sizeof(row), "%s: %lu  <", fn[i], (unsigned long)gCfg->profileSsid[gMenuPerfIdx]);
      } else if (i == 1) {
        snprintf(row, sizeof(row), "%s: %d  <", fn[i], gCfg->profileSlowSec[gMenuPerfIdx]);
      } else if (i == 2) {
        snprintf(row, sizeof(row), "%s: %d  <", fn[i], gCfg->profileFastSec[gMenuPerfIdx]);
      } else if (i == 3) {
        snprintf(row, sizeof(row), "%s: %d  <", fn[i], gCfg->profileDistM[gMenuPerfIdx]);
      } else {
        const int idx = iconoPerfilIndice(gMenuPerfIdx);
        char par[4];
        iconoPerfilTexto(gMenuPerfIdx, par, sizeof(par));
        // Si el icono guardado no es uno de los cuatro de la rueda (lo puso el
        // configurador), se ensena el codigo tal cual en vez de mentir con un
        // nombre que no le corresponde.
        // â˜… El par de codigos ("(/[") solo se puede ensenar si cabe: "Icono: Repetidor  (/[)"
        //   son 24 caracteres y a escala 2 NO caben (el tope son 16). Se ensena el nombre, que
        //   es lo que se elige con un solo boton; el par sale al aire y se consulta en el
        //   configurador.
        //   â˜… Y el nombre se pega a "Icono:" en el formato, para no gastar un caracter: el peor
        //     de los cuatro es "Icono: Repetidor" = 16 caracteres EXACTOS = 190 px de los 200
        //     del panel. Cabe justo, asi que no se puede anadir nada mas a esta fila.
        const char *nom = (idx >= 0) ? kIconoNombre[idx] : par;
        char conPar[44];
        snprintf(conPar, sizeof(conPar), "Icono: %s  (%s)", nom, par);
        if (textWidth(conPar, 2) <= EPD_W - 14) snprintf(row, sizeof(row), "%s", conPar);
        else snprintf(row, sizeof(row), "Icono: %s", nom);
      }
      // Si aun asi se pasara, `dibujaFilaMenu` lo deja en dos lineas o lo recorta con "..":
      // nunca se pierde texto por el borde del panel (ver `parteEnEscalas`).
      drawText(14, y, row, 2);
    }
    drawText(14, y, "< Volver", 2);
    return;
  }

  int y = y0;

  // scroll: calcular la fila superior para que la seleccion sea visible (8 filas caen)
  const int visibles = 8;
  if (gMenuCat < 0) {
    drawTextCenter(12, "MENU", 1);
    int n = menuMainSecCount();
    int total = menuMainTotFilas(n);
    int top = gMenuIdx - (visibles / 2); if (top < 0) top = 0;
    if (top > total - visibles) top = (total - visibles < 0) ? 0 : total - visibles;
    y = y0;
    for (int r = top; r < total && y < 186; r++, y += rowH) {
      if (r == gMenuIdx) { barraH(6, EPD_W - 6, y - 1, 1); barraH(6, EPD_W - 6, y + rowH - 2, 1); barraV(6, y - 1, y + rowH - 2, 1); barraV(EPD_W - 6, y - 1, y + rowH - 2, 1); }
      if (menuMainFilaSalir(r, n)) drawText(14, y, "Salir", esc);
      else if (menuMainEsDormir(r)) drawText(14, y, "Dormir", esc);
      else if (menuMainEsApagar(r)) drawText(14, y, "Apagar", esc);
      else {
        int sec = menuMainFilaSeccion(r, n);
        if (sec >= 0) drawText(14, y, kMenuSections[sec], esc);
      }
    }
  } else {
    int a = kMenuSectionFirst[gMenuCat];
    int b = kMenuSectionEnd[gMenuCat];
    drawTextCenter(12, kMenuSections[gMenuCat], 1);
    // â˜… LEYENDA DEL ASTERISCO (2026-09-15, arreglo G3): solo en las categorias que tienen
    //   algun ajuste inerte, y en letra pequena para no quitar sitio a las filas. Dice que
    //   significa el " *" que llevan esas etiquetas. La explicacion larga sale al ENTRAR
    //   en el item (ver la pantalla de edicion).
    if (seccionTieneInerte(gMenuCat)) {
      drawTextCenter(24, " * se guarda, no hace nada aqui", 1);
    }
    // filas: 0=Volver, 1=Salir, 2+i=item
    int itemCount = b - a;
    int total = itemCount + 2;
    int top = gMenuIdx - (visibles / 2); if (top < 0) top = 0;
    if (top > total - visibles) top = (total - visibles < 0) ? 0 : total - visibles;
    y = y0;
    for (int r = top; r < total && y < 186; r++, y += rowH) {
      if (r == gMenuIdx) { barraH(6, EPD_W - 6, y - 1, 1); barraH(6, EPD_W - 6, y + rowH - 2, 1); barraV(6, y - 1, y + rowH - 2, 1); barraV(EPD_W - 6, y - 1, y + rowH - 2, 1); }
      if (r == 0) { drawText(14, y, "< Volver", esc); continue; }
      if (r == 1) { drawText(14, y, "Salir", esc); continue; }
      int iAbs = r - 2;
      if (iAbs < itemCount) {
        const MenuItem &it = kMenu[a + iAbs];
        if (!menuItemVisible(it)) { continue; }
        // â˜… (2026-09-15) Solo la ETIQUETA, en grande. Se elimina el valor a la derecha:
        //   con textos largos se pisaba, y el valor en minuscula rompÃ­a la coherencia.
        //   Para ver/cambiar el valor se ENTRA en la opcion (submenu o pantalla de edicion).
        //
        // â˜… TNC EN PALABRA (2026-09-15, peticion del operador): el protocolo del puerto
        //   USB se lee como "TNC: OFF" / "TNC: TNC2" / "TNC: KISS", no como un numero de
        //   la configuracion. Es el UNICO item con valor en la fila a proposito: la regla
        //   de esta lista es "solo la etiqueta" (los valores largos se pisaban), y esta
        //   cabe de sobra (9 caracteres de los 15 que entran a escala 2). El valor se lee
        //   de la configuracion en el momento de pintar, asi que si se cambia desde el
        //   configurador web, la fila lo dice sin reiniciar. Ver tncProtocoloPalabra().
        if (it.key && !strcmp(it.key, "tncProtocol")) {
          char conTnc[20];
          snprintf(conTnc, sizeof conTnc, "TNC: %s", tncProtocoloPalabra());
          drawText(14, y, conTnc, esc);
          continue;   // fila ya pintada: el "continue" del for no se salta nada mas
        }
        // â˜…â˜… LAS TRES FILAS DE TRACKS (2026-09-22) â˜…â˜…
        //   Su texto se calcula al pintar porque depende de lo que haya guardado. Va con guarda
        //   `TRACKS_DISPONIBLE`: en las Faketec esta seccion no existe (ver tracks.h), y sus
        //   filas se pintan con la etiqueta de respaldo de `kMenu[]`.
#ifdef TRACKS_DISPONIBLE
        if (it.key && !strcmp(it.key, "tkSlots")) {
          char b[24];
          int n = 0;
          for (uint8_t i = 0; i < TRACK_SLOTS; i++) {
            TrackRanuraInfo inf;
            tracksRanuraInfo(i, &inf);
            if (inf.valida) n++;
          }
          snprintf(b, sizeof b, "Ranuras: %d/%d", n, (int)TRACK_SLOTS);
          drawText(14, y, b, esc);
          continue;
        }
#endif
        // â˜… AVISO DE AJUSTE INERTE (2026-09-15, arreglo G3): los items que en esta placa se
        //   guardan pero no hacen nada llevan un " *" pegado a la etiqueta, y la leyenda de
        //   arriba dice que significa. Al ENTRAR en el item, la pantalla de edicion lo
        //   explica con todas las letras ("se guarda, pero en esta pantalla NO hace nada").
        //   La marca es de UNA letra a proposito: "Apagar pantalla(s)" ya roza el borde del
        //   panel y cualquier sufijo mas largo se saldria.
        if (avisoItemInerte(it.key)) {
          char conAviso[32];
          snprintf(conAviso, sizeof conAviso, "%s%s", it.label, etiquetaInerte(it.key));
          drawText(14, y, conAviso, esc);
        } else {
          drawText(14, y, it.label, esc);
        }
      }
    }
  }
}

void displayNextScene(bool porToque) {
  if (menuIsOpen()) return;   // con el menÃº abierto, el short no cambia de diapositiva
  gEscena = (uint8_t)((gEscena + 1) % kNumEscenas);
  gUltimoCambioEscenaMs = millis();
  gUltimoToqueMs = millis();     // sello de "aqui ha habido mano del operador"
  // â˜… Solo el TACTIL agrupa repintados (ver kAgrupaToquesMs). El fisico llega con su
  //   ventana de 600 ms ya cumplida: aplazarle el pintado no agrupa nada, solo retrasa.
  if (porToque) gUltimoToqueAgrupaMs = millis();
  gCarruselPausadoHasta = millis() + kPausaTrasBotonMs;
  displayBacklightKick();
  gDirty = true;
  // â˜… DIAGNOSTICO (2026-09-16): cada cambio de diapositiva A MANO se anuncia por el USB,
  //   igual que el del carrusel automatico (ver mas abajo). Es lo que permite COMPROBAR
  //   que cuatro toques seguidos son CUATRO cambios y no uno, sin depender de la vista:
  //   se cuentan las lineas y se miran los repintados que hay entre ellas.
  if (diagTrazaTaller()) {
    Serial.printf("PANTALLA: escena %d (%s)\r\n", (int)gEscena, porToque ? "toque" : "boton");
  }
}

void displayPopup(const char *text) {
  if (!text) return;
  // â˜…â˜… LOS AVISOS DE PROGRESO DE "FIJAR COORDS" LOS PINTA LA PANTALLA DE LA
  //   SESION, NO ESTE CAMINO (2026-09-15) â˜…â˜…
  //   El bucle principal (main.cpp) manda "GPS 3/20" por CADA muestra mientras la
  //   sesion esta en fase 2. En la OLED eso es un aviso por segundo y se ve; en
  //   tinta cada aviso seria un refresco de 1,5 s (unos 30 s de panel pintando
  //   para ver cambiar un digito) y ademas taparia la pantalla de progreso, que ya
  //   lleva su contador y su barra. Se descartan SOLO esos avisos y SOLO mientras
  //   dura la sesion: cualquier otro texto (una baliza, un RX, un error) sigue
  //   pintandose igual. El aviso por USB lo sigue dando el rastreador.
  if (coordsPantallaActiva()) {
    const char *p = text;
    while (*p == ' ') p++;
    const bool esProgreso = (p[0] == 'G' && p[1] == 'P' && p[2] == 'S' && p[3] == ' ') &&
                            strchr(p, '/') != nullptr;
    if (esProgreso) return;
  }
  snprintf(gLinea1, sizeof(gLinea1), "%s", text);
  gLinea2[0] = '\0';
  gLineaMs = millis();
  gDirty = true;   // displayRefresh pinta una vez y actualiza la huella (B4)
}

// Guardar coordenadas desde la pantalla: ESTO ES LO QUE FALTABA (2026-09-15).
//
// Antes esta funcion devolvia false a secas ("no existe en esta version"), asi que
// la sesion de captura del rastreador (trackerSetCoordsTick) llegaba al final,
// pedia guardar y recibia un "no": en el T-Echo, "Fijar coords" no guardaba NADA
// (y el propio menu lo decia: "No soportado en tinta").
//
// COMO SE GUARDA: por el MISMO camino que el resto del menu (cliTypedSet +
// storeSave, ver menuSave), o sea pasando por la validacion de configFromJson. No
// se escribe en la configuracion a mano: una latitud fuera de rango tiene que dar
// error, no colarse en la flash.
//
// â˜… PRIMERO SE COMPRUEBA TODO, DESPUES SE GUARDA UNA SOLA VEZ: son DOS claves
//   (latitude y longitude) y cada una es un merge completo, asi que si la latitud
//   entrara y la longitud no, el nodo se quedaria con una posicion a medias (la
//   latitud nueva con la longitud vieja: un punto que no existe). Por eso:
//     1) se comprueban los limites (-90..90 / -180..180, los MISMOS que aplica
//        configFromJson, aqui solo se adelanta el rechazo),
//     2) se validan las dos claves con cliTypedSet (que NO toca la flash: solo
//        aplica el valor en RAM y dice si es valido),
//     3) y solo entonces se llama a storeSave(), UNA vez, con las dos ya dentro.
//   Si algo falla en (1) o (2) se dejan las dos como estaban (no se guarda nada).
bool displaySaveCoords(double lat, double lon) {
  if (!isfinite(lat) || !isfinite(lon) || lat < -90.0 || lat > 90.0 ||
      lon < -180.0 || lon > 180.0) {
    if (diagTrazaTaller()) {
      Serial.printf("TINTA fijar coords: posicion fuera de rango (%.5f %.5f)\r\n", lat, lon);
    }
    return false;
  }
  // OJO CON EL TIPO, QUE ES LA TRAMPA DE ESTE CAMINO: si el texto lleva punto
  // decimal, cliTypedSet lo manda como FLOAT y configFromJson lo acepta (acepta
  // float e int en latitude/longitude). Seis decimales es mas de lo que el GPS
  // puede decir (~0,1 m) y lo que se guarda acaba en un float, asi que la
  // precision de sobra no estorba y evita redondear a metros.
  char la[24], lo[24];
  snprintf(la, sizeof(la), "%.6f", lat);
  snprintf(lo, sizeof(lo), "%.6f", lon);

  const float latAntes = gCfg->latitude;
  const float lonAntes = gCfg->longitude;
  String err;
  const bool okLat = cliTypedSet(*gCfg, "latitude", la, err);
  const bool okLon = okLat ? cliTypedSet(*gCfg, "longitude", lo, err) : false;
  if (!okLat || !okLon) {
    // Se deshace lo que hubiera entrado en RAM: la configuracion tiene que quedar
    // EXACTAMENTE como estaba (en la flash no se ha escrito nada todavia).
    gCfg->latitude = latAntes;
    gCfg->longitude = lonAntes;
    if (diagTrazaTaller()) {
      Serial.printf("TINTA fijar coords: ERROR al guardar (lat=%d lon=%d) %s\r\n",
                    okLat ? 1 : 0, okLon ? 1 : 0, err.c_str());
    }
    return false;
  }
  storeSave(*gCfg);   // las dos claves validadas: una sola escritura en la flash
  gCoordsLat = lat;
  gCoordsLon = lon;
  if (diagTrazaTaller()) {
    Serial.printf("TINTA fijar coords: guardado %.5f %.5f\r\n", lat, lon);
  }
  return true;
}

// â˜… PUERTA UNICA PARA LOS BANCOS DE PRUEBA (2026-09-21): dibuja un aviso y lo manda al panel.
//   Existe para que un firmware de medida (hello_pad.cpp, entorno techo_plus_pad) pueda usar
//   el driver de verdad sin arrastrar todo el programa. NO se exponen `epdFlush()` ni
//   `epdFullRefresh()`: viven en el espacio de nombres anonimo de arriba, y declararlas en la
//   cabecera crea ambiguedad en cada llamada de este fichero (medido: "call of overloaded
//   epdFlush() is ambiguous"). Con una sola puerta y un nombre propio no hay ambiguedad.
void epdBancoAvisoYRefresca(const char *texto) {
  if (!texto) return;
  displayPopup(texto);   // dibuja en memoria y deja `gDirty`
  epdFlush();            // y esto lo manda al panel (parcial o completo, lo decide el driver)
}

void displayPopupWait(const char *text, uint32_t totalMs) {
  // â˜… POPUP BONITO (2026-09-15): recuadro negro con texto invertido, centrado, que dura
  //   `totalMs` y luego vuelve al carrusel. Se usa p.ej. para avisar del modo Dormir con
  //   el USB conectado ("solo sin cable USB").
  if (text) {
    // pintar la escena de fondo limpia y el recuadro encima
    if (gReady) {
      dibujaEscena();                       // fondo (carrusel/aviso actual)
      // â˜…â˜… QUE EL TEXTO QUEPA EN EL PANEL (2026-09-22) â˜…â˜…
      //   ANTES: `bw = tw + 40` y `drawTextInv()` NO recorta, asi que un texto de mas de ~13
      //   caracteres a escala 2 sacaba el recuadro por la derecha y perdia letras por el borde.
      //   Y este es justo el popup que se usa ANTES DE DORMIR ("DURMIENDO: BATERIA BAJA" son
      //   252 px): el momento en que el aviso tiene que leerse entero.
      //   AHORA: si no cabe a escala 2 se parte en DOS LINEAS (hay sitio de sobra: el recuadro
      //   va centrado en 200 px de alto y solo ocupa 38) y crece hacia arriba. Si no hubiera
      //   ningun espacio donde partir, se recorta anadiendo ".." para que se vea que falta algo.
      char l1[64], l2[64];
      snprintf(l1, sizeof(l1), "%s", text);
      l2[0] = '\0';
      int e1 = 2, e2 = 1;
      if (!cabeTexto(l1, 2, EPD_W - 40)) {
        char a[64], b[64];
        int ea = 0, eb = 0;
        if (parteEnEscalas(l1, a, sizeof(a), &ea, b, sizeof(b), &eb)) {
          // â˜… Se parte igual que en `pintaAviso`: las dos lineas lo mas grandes que quepan.
          snprintf(l1, sizeof(l1), "%s", a);
          snprintf(l2, sizeof(l2), "%s", b);
          e1 = ea; e2 = eb;
        } else {
          recortaA(l1, 2, EPD_W - 40, l1, sizeof(l1));
        }
      }
      const bool dosLineas = (l2[0] != '\0');
      int tw = textWidth(l1, e1);
      if (dosLineas) { const int t2w = textWidth(l2, e2); if (t2w > tw) tw = t2w; }
      int bx = (EPD_W - tw - 40) / 2; if (bx < 6) bx = 6;
      int bw = tw + 40;
      if (bw > EPD_W - 12) bw = EPD_W - 12;
      const int bh = dosLineas ? 56 : 38;
      // El de dos lineas SUBE 18 px para no salirse por abajo: centrado ocupa 38 y crece 18 mas.
      const int by = dosLineas ? ((EPD_H - bh) / 2 - 18) : ((EPD_H - bh) / 2);
      // Recuadro negro con las esquinas cortadas (pastilla), hecho a mano porque
      // pastillaRellena vive en el namespace de dibujo y no es accesible aqui.
      relleno(bx, by, bw, bh);                     // cuerpo
      px(bx, by, false); px(bx + 1, by, false);   // esquina superior izquierda
      px(bx, by + 1, false);
      px(bx + bw - 1, by, false); px(bx + bw - 2, by, false);   // superior derecha
      px(bx + bw - 1, by + 1, false);
      px(bx, by + bh - 1, false); px(bx + 1, by + bh - 1, false);   // inferior izquierda
      px(bx, by + bh - 2, false);
      px(bx + bw - 1, by + bh - 1, false); px(bx + bw - 2, by + bh - 1, false);   // inferior derecha
      px(bx + bw - 1, by + bh - 2, false);
      if (dosLineas) {
        const int t1 = textWidth(l1, e1);
        const int t2 = textWidth(l2, e2);
        drawTextInv((EPD_W - t1) / 2, by + 8, l1, e1);
        drawTextInv((EPD_W - t2) / 2, by + 34, l2, e2);
      } else {
        drawTextInv((EPD_W - tw) / 2, by + 12, l1, 2);   // texto invertido, centrado
      }
      // â˜… OJO (2026-09-15): aqui estaba `gDirty = false;` ANTES de epdFlush(), y
      //   epdFlush() empieza con `if (!gReady || !gDirty) return;` -> salia sin pintar
      //   NADA. Por eso el popup "no salia": se perdian los 3 s sin dibujar. La bandera
      //   la apaga epdFlush() el solo; aqui hay que DEJARLA EN ALTA para que pinte.
      gDirty = true;
      epdFlush();
    }
  }
  delay(totalMs);
  gDirty = true;   // volver a pintar el carrusel tras el aviso
}

void displayNoteRx(const char *from, float rssi, float snr, const char *kind) {
  rxLogPush(from, rssi, snr);
  snprintf(gLinea1, sizeof(gLinea1), "RX %s", from ? from : "?");
  snprintf(gLinea2, sizeof(gLinea2), "%.0f dBm  %.1f dB", (double)rssi, (double)snr);
  (void)kind;
  gLineaMs = millis();
  gDirty = true;   // displayRefresh pinta una vez (B4)
}
void displayNoteDigi(const char *from, float rssi, float snr) {
  rxLogPush(from, rssi, snr);   // un repetido tambien entra en la lista de RX
  snprintf(gLinea1, sizeof(gLinea1), "REPITE %s", from ? from : "?");
  snprintf(gLinea2, sizeof(gLinea2), "%.0f dBm", (double)rssi);
  (void)snr;
  gLineaMs = millis();
  gDirty = true;   // displayRefresh pinta una vez (B4)
}
void displayNoteTx(const char *what) {
  txLogPush(what);
  snprintf(gLinea1, sizeof(gLinea1), "TX %s", what ? what : "");
  gLinea2[0] = '\0';
  gLineaMs = millis();
  gDirty = true;   // displayRefresh pinta una vez (B4) y el TX-reciente entra en la huella
}

// ---------------------------------------------------------------------------
//  El pintado de las pantallas
// ---------------------------------------------------------------------------
namespace {

// â˜… (subida a escala 2) `modoNombre()` se queda aqui aunque la escena de Estado ya no lo
//   use: el modo se quito de la linea de contadores porque a escala 2 no cabe junto a
//   ellos (ver `pintaEstado()`). El atributo es solo para que el compilador no avise de
//   "definida y no usada" (-Wunused-function) mientras no haya ninguna pantalla que la use.
[[maybe_unused]] const char *modoNombre(uint8_t m) {
  switch (m) {
    case 0: return "Repetidor";
    case 1: return "Rastreador";
    case 2: return "Digi+Tracker";   // antes "Ambos" (etiqueta mas clara, 2026-09-15)
    default: return "Apagado";
  }
}

// ---------------------------------------------------------------------------
//  â˜… CONTADOR EN TRES CARACTERES COMO MUCHO (subida a escala 2)
//
//  POR QUE HACE FALTA: a escala 2 el paso por caracter es 6*2 = 12 px, o sea que en los
//  200 px de ancho caben 16 caracteres. La linea que pedia el encargo
//  ("RX %lu  TX %lu  DG %lu") solo cabe con contadores de UNA cifra: con los contadores
//  reales de un iGate ("RX 1234  TX 567  DG 89") son 22 caracteres = 262 px, y los 62 px
//  que sobran se recortarian contra el borde (drawTextCenter pega el texto a x=0 y todo lo
//  que pasa de x=199 desaparece: se perderia el contador de DG entero).
//
//  Los contadores de este firmware dan la vuelta a 10.000 (radio.cpp kCounterWrap y
//  aprs.cpp), asi que sus valores estan en 0..9999 y TRES caracteres cubren todos:
//        999 -> "999"        1234 -> "1k"        9999 -> "10k"
//  Con eso la linea se queda como mucho en "R10k T10k D10k" = 14 caracteres (166 px de los
//  200), SIEMPRE dentro de la pantalla. Los valores EXACTOS se siguen viendo en la escena
//  de Radio ("RX 1234", "TX 567", "Repetidas 89") y por el USB.
// ---------------------------------------------------------------------------
void contadorCorto(uint32_t v, char *dst, size_t n) {
  if (v < 1000) snprintf(dst, n, "%lu", (unsigned long)v);
  else          snprintf(dst, n, "%.0fk", (double)v / 1000.0);
}

// ---------------------------------------------------------------------------
//  Utilidades de dibujo y datos que usa la cabecera y el pie.
// ---------------------------------------------------------------------------

// Rectangulo SOLO CONTORNO (marco de la pastilla RX, de los puntos y de la bateria).
void rectVacio(int x0, int y0, int x1, int y1) {
  barraH(x0, x1, y0, 1);
  barraH(x0, x1, y1, 1);
  barraV(x0, y0, y1, 1);
  barraV(x1, y0, y1, 1);
}

// Porcentaje de bateria para la barrita (0..100; 0 si no hay lectura creible).
// Misma regla que la OLED (display.cpp, drawBatteryFooter): 3,0..4,2 V -> 0..100 %.
int bateriaPct() {
  const float bv = sensorsBatteryVolt(gSens);
  if (bv <= 0.0f) return 0;
  int p = (int)((bv - 3.0f) / 1.2f * 100.0f);
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  return p;
}

// ---------------------------------------------------------------------------
//  â˜…â˜… CABECERA Y PIE â€” EL ESTILO DE LA OLED, adaptado a este panel 200x200 vertical â˜…â˜…
//
//  Se replica la interfaz de la OLED (display.cpp) pero con las zonas pensadas para
//  nuestra resolucion y con un UNICO escalado de texto (coherencia):
//
//    ARRIBA  (y 4..25)
//      â”Œâ”€ pastilla con el titulo de la pantalla: indicativo + modo
//      â””â”€ a la derecha: pastilla RX/TX (se R-ellen-0 en negro al transmitir; MUTE si
//         el nodo esta silenciado; "--" si la radio no esta lista)
//    DEBAJO  separador discontinuo (guiÃ±o al aleman)
//    ABAJO   (y ~184..196)
//      â”Œâ”€ rectangulitos del carrusel: el de la pantalla actual relleno
//      â””â”€ a la derecha: bateria con su % y el cuerpo que se llena/vacia
//
//  La leccion de la OLED: "lo invertido comunica estado" (RX contorneado, TX relleno).
// ---------------------------------------------------------------------------

// â”€â”€ pastilla (ovalo aproximado) de la cabecera: rectangulo relleno con las esquinas
// redondeadas a mano (en esta resolucion basta con cortar las esquinas).
void pastillaRellena(int x0, int y0, int x1, int y1) {
  relleno(x0, y0, x1 - x0, y1 - y0);          // cuerpo
  px(x0, y0, false); px(x0 + 1, y0, false);   // esquina superior izquierda (corta)
  px(x0, y0 + 1, false);
  px(x1, y0, false); px(x1 - 1, y0, false);   // superior derecha
  px(x1, y0 + 1, false);
  px(x0, y1, false); px(x0 + 1, y1, false);   // inferior izquierda
  px(x0, y1 - 1, false);
  px(x1, y1, false); px(x1 - 1, y1, false);   // inferior derecha
  px(x1, y1 - 1, false);
}

// â”€â”€ OVALO de la cabecera: el titulo de la pantalla (tu indicativo + modo) en una pastilla
// blanca con texto negro, como la `headerBar` de la OLED. Se adapta a la longitud.
void pintaOvalo(const char *titulo) {
  const int pad = 8;                 // aire interior
  const int y0 = 4;                  // alto de la zona del titulo (se mantiene para el
                                     // alineado del texto, ya sin el rectangulo)
  // â˜… (2026-09-15) Se ELIMINA el rectangulo del titulo: ahora las diapositivas muestran el
  //   titulo como texto simple, sin la caja. Se conserva la columna x (8+pad) y la altura
  //   para que el dibujo quede igual de legible; la pastilla RX/TX de la derecha no cambia.
  drawText(8 + pad, y0 + 3, titulo, 2);
}

// â”€â”€ pastilla RX/TX a la derecha de la cabecera, como `statusPill` de la OLED:
// RX = solo contorno (texto negro, no transmite); TX = rellena de negro con texto blanco
// cuando estoy transmitiendo (se mantiene 3 s tras el envio); MUTE si el nodo esta
// silenciado; "--" si la radio no lista.
void pintaPillRX() {
  const uint32_t last = radioLastTxMs();
  bool tx = (last != 0 && (uint32_t)(millis() - last) < 3000);
  const char *txt = "RX";
  if (gCfg && gCfg->txDisabled) txt = "MUTE";
  else if (tx) txt = "TX";
  else if (!radioReady()) txt = "--";

  const int pad = 8;
  const int y0 = 4, y1 = 22;
  const int tw = textWidth(txt, 2);
  const int x1 = EPD_W - 8;
  const int x0 = x1 - tw - pad * 2;
  if (tx) {
    pastillaRellena(x0, y0, x1, y1);            // rellena de negro
    const int dx = x0 + pad + 2;                 // texto BLANCO (inverso) centrado aprox
    drawTextInv(dx, y0 + 3, txt, 2);
  } else {
    rectVacio(x0, y0, x1, y1);                   // solo contorno
    drawText(x0 + pad, y0 + 3, txt, 2);
  }
}

// â”€â”€ CABECERA completa: ovalo con el titulo + pastilla RX/TX + separador discontinuo.
void pintaCabecera(const char *titulo) {
  pintaOvalo(titulo);
  pintaPillRX();
  // separador discontinuo a y=25 (5 on / 3 off), guiÃ±o al aleman
  for (int x0 = 4; x0 < EPD_W - 4; x0 += 8) {
    int x1 = x0 + 4;
    if (x1 > EPD_W - 4) x1 = EPD_W - 4;
    hLine(x0, x1, 25, 1);
  }
  // â˜…â˜… "GRAB OFF": EL AVISO DE QUE NO SE ESTA GRABANDO â˜…â˜…
  //   â˜…â˜… AQUI NO, Y ESTA QUITADO A PROPOSITO (2026-09-22) â˜…â˜…
  //     Estuvo pintado en esta cabecera, por el motivo de que "la cabecera sale en todas las
  //     pantallas". Y estaba mal, por dos razones que el operador vio en el aparato antes que yo:
  //       1) `tracksVivoGrabando()` es falso tambien SIN GPS, asi que el aviso se encendia al
  //          arrancar y se quedaba FIJO para siempre: nada lo apagaba. Basura permanente en la
  //          cabecera, detras de la pastilla RX.
  //       2) Un aviso permanente no informa: se acaba ignorando.
  //   â˜… DONDE VA AHORA: en la LISTA de tracks, que es donde se habla de la grabacion y donde el
  //     operador mira cuando quiere saber que esta pasando. Ver la fila del track en vivo.
  //   â˜… LA LECCION: una funcion que dice "no esta grabando" NO es lo mismo que "hay un problema".
  //     Sin GPS no se graba, y eso ya lo dice la pantalla de guiado con "SIN GPS".
}

// â”€â”€ â˜…â˜… EL AVISO, EN UN SOLO PASO (2026-09-15) â˜…â˜… â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// SUSTITUYE a `pintaAvisoTX()`, que dibujaba una banda "TX" mientras
// `radioLastTxMs() < 3000`. Aquel era un SEGUNDO mecanismo de aviso con su PROPIO reloj
// (3000 ms) distinto del de las lineas de aviso (`kLineaMs`, 8000 ms antes / 1500 ms
// ahora), y ademas metia su propio cambio de estado en la huella del contenido: o sea, un
// TX podia provocar TRES refrescos de tinta (aviso TX de la linea + banda TX + caducidad de
// la banda). Ahora hay UN solo camino: `displayNoteTx()` deja su texto en `gLinea1` y este
// recuadro es el unico aviso. La pastilla RX/TX de la cabecera (pintaPillRX) se mantiene:
// esa es informacion de estado, no un aviso, y no cuesta un refresco extra porque su cambio
// ya viaja en la huella.
//
// COMO SE PINTA: recuadro negro con texto invertido, centrado en la mitad baja de la
// pantalla, ENCIMA de la escena (que se sigue viendo alrededor) y SIN tocar el pie
// (y=186 en adelante), que es donde vive la bateria.
void pintaAviso() {
  if (!gLinea1[0]) return;

  // â˜…â˜… QUE EL TEXTO QUEPA DE VERDAD (2026-09-22, arreglo del aviso que se salia) â˜…â˜…
  //   ANTES: el recuadro se recortaba a lo ancho del panel (`bw`), pero el texto se dibujaba a
  //   su ancho natural y `drawText()` no recorta: lo que pasaba del borde se perdia. Medido:
  //   "DURMIENDO: BATERIA BAJA" (22 caracteres) son 252 px sobre un panel de 200 -> 52 px fuera.
  //
  //   AHORA se prueban cuatro formas EN ESTE ORDEN, y solo se recorta como ultimo recurso:
  //     1. UNA linea a escala 2  (lo normal: hasta 16 caracteres)
  //     2. DOS lineas, las dos a escala 2 (la primera hasta 16, la segunda hasta 16)
  //     3. DOS lineas a escala 1 (hasta 33 cada una): se lee mas pequeno, pero NO SE PIERDE
  //        NADA. Es lo que cae casi siempre, y perder informacion es peor que leer pequeno
  //        sobre todo en un aviso de bateria baja.
  //     4. UNA linea a escala 1
  //     5. y si aun asi no cabe, se recorta dejando ".." para que se VEA que falta texto.
  char l1[64], l2[64];
  snprintf(l1, sizeof(l1), "%s", gLinea1);
  l2[0] = '\0';
  if (gLinea2[0]) snprintf(l2, sizeof(l2), "%s", gLinea2);

  const bool haySegunda = (l2[0] != '\0');
  bool dosLineas = haySegunda;
  int esc1 = 2, esc2 = 1;

  if (!haySegunda) {
    if (cabeTexto(l1, 2)) {
      // 1) una linea a escala 2: lo normal
    } else {
      char a[64], b[64];
      int ea = 0, eb = 0;
      if (parteEnEscalas(l1, a, sizeof(a), &ea, b, sizeof(b), &eb)) {
        // 2) DOS LINEAS con la escala que la propia funcion ha decidido (2 y 2 si caben, y si
        //    no las dos a 1). Se lee lo mas grande posible sin perder nada.
        snprintf(l1, sizeof(l1), "%s", a);
        snprintf(l2, sizeof(l2), "%s", b);
        dosLineas = true; esc1 = ea; esc2 = eb;
      } else if (cabeTexto(l1, 1)) {
        esc1 = 1;                                     // 3) una linea a escala 1
      } else {
        recortaA(l1, 2, kPanelW - 8, l1, sizeof(l1));     // 4) ultimo recurso
      }
    }
  } else if (!cabeTexto(l1, 2)) {
    // El aviso que YA traia segunda linea (RX/TX/REPITE con sus datos): si la primera no cabe
    // a 2, se prueba a 1 antes de recortar, porque recortar pierde el dato.
    if (cabeTexto(l1, 1)) esc1 = 1;
    else recortaA(l1, 2, kPanelW - 8, l1, sizeof(l1));
  }

  const int bh = dosLineas ? 58 : 38;
  const int by = dosLineas ? 104 : 114;

  // El ancho lo manda la linea mas larga, con margen a los lados y topes para que quepa.
  int twMax = textWidth(l1, esc1);
  if (dosLineas) {
    const int tw2 = textWidth(l2, esc2);
    if (tw2 > twMax) twMax = tw2;
  }
  int bw = twMax + 24;
  if (bw > EPD_W - 12) bw = EPD_W - 12;
  if (bw < 40) bw = 40;
  const int bx = (EPD_W - bw) / 2;
  // Recuadro negro (pastillaRellena corta las esquinas; el texto va invertido dentro).
  pastillaRellena(bx, by, bx + bw - 1, by + bh - 1);
  const int t1 = textWidth(l1, esc1);
  drawTextInv((EPD_W - t1) / 2, by + (dosLineas ? 8 : 12), l1, esc1);
  if (dosLineas) {
    const int t2 = textWidth(l2, esc2);
    drawTextInv((EPD_W - t2) / 2, by + 36, l2, esc2);
  }
}

// â”€â”€ PIE: rectangulitos del carrusel (el actual relleno) a la izquierda + bateria a la
// derecha, como `footerDots` + `drawBatteryFooter` de la OLED.
void pintaPie() {
  const int yf = 186;                 // fila del pie
  // ---- rectangulitos del carrusel (8 escenas: pequenios para que no choquen con la bateria)
  {
    const int dotW = 8, gap = 4, y = yf;
    int x0 = 12;
    for (int i = 0; i < kNumEscenas; i++) {
      int x = x0 + i * (dotW + gap);
      if (i == (int)gEscena) relleno(x, y, dotW, 6);
      else                   rectVacio(x, y, x + dotW, y + 6);
    }
  }
  // ---- bateria, REDISENADA (2026-09-15): el % va ENCIMA del icono y el cuerpo se v aci
  // segun el nivel (se ve con poca y con mucha carga). El polo positivo es un borne
  // compacto a la derecha. Nada se pinta encima de otra cosa.
  {
    const int p = bateriaPct();
    char bb[8];
    if (bateriaMv() > 0) snprintf(bb, sizeof(bb), "%d%%", p);
    else                 snprintf(bb, sizeof(bb), "--");

    // cuerpo de la bateria, anclado a la derecha; el borne positivo sobresale a la derecha
    const int by = 186, bh = 14;
    const int bw = 44;
    const int bx = EPD_W - 16 - 6 - bw;          // hueco para el borne (6 px)
    rectVacio(bx, by, bx + bw - 1, by + bh - 1);
    relleno(bx + bw, by + bh / 2 - 3, 5, 6);     // borne positivo
    if (p > 0) relleno(bx + 2, by + 2, (bw - 4) * p / 100, bh - 4);   // relleno segun el %

    // texto del % exactamente ENCIMA del cuerpo (mismo tamano, escala 2), centrado sobre el
    // cuerpo para que nunca choque con el relleno ni con los puntos del carrusel.
    const int twTxt = textWidth(bb, 2);
    drawText(bx + (bw - twTxt) / 2, by - 14, bb, 2);
  }
}

// ---------------------------------------------------------------------------
//  â˜…â˜… ICONO DEL PERFIL ACTIVO â€” cabecera de la escena "Estado" (2026-09-15) â˜…â˜…
//
//  QUE PROBLEMA RESUELVE: el operador cambia de perfil (digi / peaton / bici / coche) y eso
//  cambia el SSID con el que sale al aire, pero en la pantalla no habia NADA que lo dijera:
//  habia que entrar al menu para saber con que perfil estaba trabajando el nodo.
//
//  DONDE VA: en la cabecera queda un hueco libre entre el indicativo y la pastilla RX/TX.
//  El indicativo lo pinta `pintaOvalo()` con `drawText(8 + pad, y0 + 3, titulo, 2)` (pad=8,
//  y0=4), o sea que EMPIEZA EN x=16 y avanza 6*2 = 12 px por caracter. La cabecera ocupa de
//  y=4 a y=22 (18 px de alto), asi que un icono de 16x16 centrado va de y=5 a y=20.
//
//  â˜… EL COLOR, QUE NO ES EVIDENTE (leido en el codigo, no supuesto): el ultimo argumento de
//    `px()` es un booleano que significa "negro", NO "pinta":
//        px(x, y, true)  -> gBuf &= ~bit -> el bit queda a 0 -> NEGRO (tinta)
//        px(x, y, false) -> gBuf |=  bit -> el bit queda a 1 -> BLANCO (borra)
//    Y se usan LOS DOS: `drawChar()` y `relleno()` pintan tinta con `true` (por eso el texto
//    sale negro); `drawCharInv()` pinta blanco con `false` (el texto de la pastilla TX) y
//    `pastillaRellena()` usa `false` para RECORTAR las esquinas. Aqui la figura tiene que
//    quedar en TINTA, igual que el texto, asi que TODO va con `true`.
//
//  â˜… NADA SE DIBUJA ENCIMA DE NADA: el icono solo se pinta si CABE en el hueco (la cuenta y
//    la comprobacion estan en `pintaEstado()`); con un indicativo largo no se dibuja nada y
//    la cabecera se queda exactamente como estaba.
// ---------------------------------------------------------------------------

// Pinta una figura de 16x16 descrita como arte ASCII: '#' = tinta, cualquier otra cosa =
// blanco (no se pinta). Se para en el '\0' de cada fila, asi que una fila corta no se sale.
void pintaFigura16(int x0, int y0, const char *const *filas) {
  for (int fy = 0; fy < 16; fy++) {
    const char *fila = filas[fy];
    if (!fila) continue;
    for (int fx = 0; fx < 16 && fila[fx]; fx++)
      if (fila[fx] == '#') px(x0 + fx, y0 + fy, true);   // true = TINTA (ver px())
  }
}

// â”€â”€ PERFIL 0 = digi / fijo: ESTRELLA de 5 puntas con una "D" dentro (el simbolo clasico del
//    digipeater). La "D" va RECORTADA EN BLANCO sobre la estrella rellena: a 16x16 una "D" de
//    tinta sobre estrella de contorno deja las dos figuras en hilachas y no se lee ninguna.
//    OJO: la "D" esta dibujada a mano (5x7, trazos de 1 px) y NO es la letra de `EpdFont5x7`.
//    Se probaron las dos recortadas sobre esta misma estrella, y la de la fuente --que tiene
//    el lado derecho redondeado-- deja el trazo tan fino que a 1:1 se lee peor que esta.
//                            0123456789012345
const char *const kIconoDigi[16] = {
  "................",   // 0
  "................",   // 1
  ".......##.......",   // 2   punta de arriba
  ".......##.......",   // 3
  "......####......",   // 4
  "....#....###....",   // 5   hombro izquierdo / cuello
  ".####.###.#####.",   // 6   brazo izq. | hueco de la D | brazo der.
  "..###.###.####..",   // 7
  "...##.###.###...",   // 8
  "....#.###.##....",   // 9
  "....#.###.##....",   // 10
  "....#....###....",   // 11
  "....########....",   // 12  base de la D + union de las patas
  "....##....##....",   // 13  las dos patas de abajo
  "................",   // 14
  "................"    // 15
};

// â”€â”€ PERFIL 1 = PEATON: cabeza, cuerpo con los brazos abiertos, y las dos piernas.
//                            0123456789012345
const char *const kIconoPeaton[16] = {
  "................",   // 0
  "......####......",   // 1   cabeza (4x4)
  "......####......",   // 2
  "......####......",   // 3
  "......####......",   // 4
  ".......##.......",   // 5   cuello
  "....########....",   // 6   hombros
  "..############..",   // 7   brazos abiertos
  "......####......",   // 8   tronco
  "......####......",   // 9
  "......####......",   // 10
  "......####......",   // 11
  ".....##..##.....",   // 12  las dos piernas, que se abren hacia abajo
  "....##....##....",   // 13
  "...##......##...",   // 14
  "..###......###.."    // 15  pies
};

// â”€â”€ PERFIL 2 = BICI: las dos ruedas (aros de 7x7) y el cuadro en rombo, con el manillar
//    arriba a la derecha y el sillin arriba a la izquierda.
//                            0123456789012345
const char *const kIconoBici[16] = {
  "................",   // 0
  "................",   // 1
  "................",   // 2
  ".........###....",   // 3   manillar
  "...####.........",   // 4   sillin
  "....#######.....",   // 5   tubo superior
  ".....#....#.....",   // 6   tubo del sillin | horquilla
  ".....#...#.#....",   // 7
  "......#..#.#....",   // 8   tubo diagonal
  "..###..#.#.###..",   // 9   parte de arriba de las dos ruedas
  ".#...#.##.#.#.#.",   // 10  aros
  "#..#######..#..#",   // 11  vaina al buje trasero + caja de pedalier
  "#.....#..#.....#",   // 12  aros
  "#.....#..#.....#",   // 13
  ".#...#....#...#.",   // 14
  "..###......###.."    // 15  parte de abajo de las dos ruedas
};

// â”€â”€ PERFIL 3 = COCHE: silueta de perfil (techo, parabrisas recortado, carroceria) con las
//    dos ruedas debajo.
//                            0123456789012345
const char *const kIconoCoche[16] = {
  "................",   // 0
  "................",   // 1
  "................",   // 2
  ".....######.....",   // 3   techo
  "....##....##....",   // 4   montantes con la ventanilla en blanco
  "...##......##...",   // 5
  "..############..",   // 6   capo / maletero
  ".##############.",   // 7   carroceria
  ".##############.",   // 8
  ".##############.",   // 9
  ".##############.",   // 10
  "..###......###..",   // 11  las dos ruedas
  "..###......###..",   // 12
  "..###......###..",   // 13
  "................",   // 14
  "................"    // 15
};

// Las cuatro figuras, en el MISMO orden que `smartBeaconPreset` (ver config.h):
//   0 = off (fijo/digi), 1 = human (peaton), 2 = bike (bici), 3 = car (coche)
const char *const *const kIconosPorPerfil[4] = {
  kIconoDigi, kIconoPeaton, kIconoBici, kIconoCoche
};

// Dibuja en (x, y) el icono del perfil activo (`gCfg->smartBeaconPreset`). Un valor fuera de
// 0..3 (flash de otra version) se pinta como digi, que es el perfil 0.
void iconoPerfil(int x, int y, int perfil) {
  if (perfil < 0 || perfil > 3) perfil = 0;
  pintaFigura16(x, y, kIconosPorPerfil[perfil]);
}

// Escena 0: estado general (lo que se ve casi siempre). La cabecera lleva tu indicativo
// (como la barra de titulo de la OLED).
// â˜… (subida a escala 2) El modo de trabajo YA NO se dibuja en esta escena: a escala 2 no
//   cabe en la misma linea que los contadores (ver el comentario de abajo). La cabecera
//   pinta SOLO el indicativo, asi que en esta escena el modo ya no se ve.
void pintaEstado() {
  const char *call = callSinSSID();   // solo el indicativo, sin el SSID
  pintaCabecera(call);

  // â˜… ICONO DEL PERFIL ACTIVO (2026-09-15): en la cabecera queda un hueco libre entre el
  //   indicativo y la pastilla RX/TX. Ahi se dibuja (DESPUES de la cabecera, que es quien
  //   pinta el indicativo) un icono de 16x16 con el perfil con el que trabaja el nodo.
  //
  //   LA CUENTA, CON LAS MEDIDAS YA VERIFICADAS:
  //     - el indicativo empieza en x = 8 + pad = 8 + 8 = 16 y avanza 12 px por caracter
  //       (escala 2), asi que ocupa hasta 16 + textWidth(call, 2);
  //     - el icono empieza 8 px despues de la ultima letra: xIcono = 16 + ancho + 8;
  //     - el icono mide 16 px de ancho, o sea que acaba en xIcono + 16;
  //     - la pastilla RX/TX NO se toca ni se desplaza: si el icono no cabe en el hueco,
  //       simplemente NO se dibuja.
  //
  //   â˜… EL LIMITE DE LA DERECHA NO ES SIEMPRE 154. Leyendo `pintaPillRX()`: la pastilla se
  //     dimensiona con su texto, `x0 = (EPD_W - 8) - textWidth(txt, 2) - pad*2` = 192 - tw -
  //     16. Con "RX", "TX" y "--" sale x0 = 154 (el valor medido), pero con "MUTE" (nodo
  //     silenciado) el texto es mas largo y sale x0 = 192 - 46 - 16 = 130. Para que el icono
  //     no la pise NUNCA se comprueba contra el PEOR caso (130), que es el unico que garantiza
  //     el hueco en los cuatro estados de la pastilla.
  {
    const int xIcono = 16 + textWidth(call, 2) + 8;   // 16 = inicio del indicativo, 8 = margen
    const int kPastillaXIzq = 130;                    // peor caso de pintaPillRX() ("MUTE")
    if (xIcono + 16 <= kPastillaXIzq) {
      const int perfil = gCfg ? (int)gCfg->smartBeaconPreset : 0;
      iconoPerfil(xIcono, 5, perfil);                 // y = 5..20, centrado en la cabecera
    }
  }

  int y = 32;

  const GpsData &g = gpsGet();
  char b[40];
  // Contadores de trafico (como la escena Status de la OLED).
  //
  // â˜… SUBIDA A ESCALA 2 (encargo del operador: el tamano mas pequeno se lee mal). A escala 2
  //   el paso por caracter es 6*2 = 12 px, o sea 16 caracteres como mucho en 200 px. El texto
  //   anterior ("Repetidor   RX 12 TX 3 DG 0") son ~30 caracteres: ni de lejos. Se quita el
  //   modo de la linea y los contadores van abreviados con `contadorCorto()` (3 caracteres
  //   como mucho cada uno), con lo que la linea nunca pasa de 14 caracteres:
  //        "R1k T345 D67"      "R10k T10k D10k" (el peor caso) = 166 px
  //   Las etiquetas van en una letra (R, T, D) a proposito: con "RX/TX/DG" y los valores
  //   reales la linea se saldria de la pantalla (ver la nota de `contadorCorto()`).
  char cr[8], ct[8], cd[8];
  contadorCorto(gRx, cr, sizeof(cr));
  contadorCorto(gTx, ct, sizeof(ct));
  contadorCorto(gDg, cd, sizeof(cd));
  snprintf(b, sizeof(b), "R%s T%s D%s", cr, ct, cd);
  drawTextCenter(y, b, 2); y += 20;

  if (g.fix) {
    snprintf(b, sizeof(b), "%.5f", g.lat);
    drawTextCenter(y, b, 2); y += 20;
    snprintf(b, sizeof(b), "%.5f", g.lon);
    drawTextCenter(y, b, 2); y += 20;
    snprintf(b, sizeof(b), "%.0f km/h   %d sat", (double)g.speedKmh, (int)g.sats);
    drawTextCenter(y, b, 2); y += 20;
  } else {
    drawTextCenter(y, "GPS: buscando", 2); y += 22;
    snprintf(b, sizeof(b), "Vista %d sat", (int)g.satsInView);
    drawTextCenter(y, b, 2); y += 20;
  }

  // â˜… LOS DOS SALTOS DEL SEPARADOR, ESTRECHADOS (subida a escala 2). No son saltos de linea
  //   de texto (esos van a 20, como pide el encargo): son el aire de arriba y de abajo de la
  //   raya. Habia 6 + 8 = 14 px y ahora 2 + 4 = 6, o sea 8 px menos. Hacen falta porque al
  //   subir a escala 2 cada linea ocupa 14 px (antes 7) y la ultima linea (la bateria) se
  //   metia 8 px DENTRO del texto del % de la bateria del pie, que se dibuja en y=172..185
  //   (pintaPie(): `drawText(..., by - 14, bb, 2)` con by=186). Medido pixel a pixel: la
  //   ultima linea acababa en y=179 y el % empieza en y=172, o sea 12 pixeles de tinta
  //   encima del otro. Con 8 px menos acaba en y=171 y no se tocan.
  //   La raya queda 8 px por debajo de la tinta de la linea de velocidad y 3 px por encima
  //   de la de meteorologia (antes 12 y 7): sigue separando los dos bloques.
  y += 2;
  hLine(6, EPD_W - 6, y, 1); y += 4;

  // Meteorologia, en texto legible (lo que mide de verdad: nada inventado). Si no hay sonda
  // externa, se muestra la temperatura del chip (la de dentro de la caja), como la OLED.
  b[0] = '\0';
  if (gSens.tempOk)  snprintf(b + strlen(b), sizeof(b) - strlen(b), "%.1fC ", (double)gSens.tempC);
  if (gSens.humOk)   snprintf(b + strlen(b), sizeof(b) - strlen(b), "%.0f%% ", (double)gSens.hum);
  if (gSens.pressOk) snprintf(b + strlen(b), sizeof(b) - strlen(b), "%.0fhPa", (double)gSens.pressHpa);
  if (b[0]) drawTextCenter(y, b, 2);
  y += 20;

  // Radio-link quality (RSSI/SNR) de la ultima trama recibida: un dato que la OLED enseÃ±aba
  // y que aqui faltaba.
  snprintf(b, sizeof(b), "%.0fdBm  %.1fdB", (double)radioLastRssi(), (double)radioLastSnr());
  drawTextCenter(y, b, 2); y += 20;

  uint16_t mv = bateriaMv();
  if (mv > 0) snprintf(b, sizeof(b), "Bat %.2fV", mv / 1000.0);
  else        snprintf(b, sizeof(b), "Bat --");
  drawTextCenter(y, b, 2);
}

// Escena 1: radio y trafico.
void pintaRadio() {
  pintaCabecera("Radio");
  char b[40];
  int y = 34;
  // â˜… EL MODO DE TRABAJO SE MUESTRA AQUI (2026-09-15, decision del operador).
  //   En la escena de Estado NO cabe: al subir el texto a escala 2 (por la vision
  //   deficiente), el nombre del modo y los contadores no entran juntos en la misma linea
  //   -- la pantalla son 200 px y a escala 2 el paso es 12 px por caracter. El operador
  //   prefiere que Estado muestre los contadores y que el modo se consulte aqui, que tiene
  //   sitio de sobra. Va el primero, para verlo nada mas entrar en la diapositiva.
  snprintf(b, sizeof(b), "%s", modoNombre(gCfg ? gCfg->mode : 0));
  drawTextCenter(y, b, 2); y += 20;

  snprintf(b, sizeof(b), "%.3f MHz", (gCfg ? gCfg->frequencyHz : 433775000) / 1000000.0);
  drawTextCenter(y, b, 2); y += 20;
  snprintf(b, sizeof(b), "SF%d  BW%dkHz", (int)(gCfg ? gCfg->spreadingFactor : 12),
           (int)(gCfg ? gCfg->signalBandwidthKhz : 125));
  drawTextCenter(y, b, 2); y += 22;

  snprintf(b, sizeof(b), "RX %lu", (unsigned long)gRx);
  drawText(10, y, b, 2); y += 20;
  snprintf(b, sizeof(b), "TX %lu", (unsigned long)gTx);
  drawText(10, y, b, 2); y += 20;
  snprintf(b, sizeof(b), "Repetidas %lu", (unsigned long)gDg);
  drawText(10, y, b, 2);
}

// Escena 2: sensores (la OLED tenia su pantalla "SENSORES": chip, sonda, presion,
// bateria y corriente). La temperatura del chip se muestra SIEMPRE; la sonda externa
// SOLO si de verdad hay un sensor detectado (unas unidades lo llevan y otras no).
void pintaSensores() {
  pintaCabecera("Sensores");
  char b[40];
  int y = 34;
  snprintf(b, sizeof(b), "Chip %.1fC", (double)sensorsChipTemp(gSens, gCfg ? gCfg->chipTempOffsetC : 0.0f));
  drawText(12, y, b, 2); y += 26;
  if (gSens.tempOk) {
    snprintf(b, sizeof(b), "Sonda %.1fC", (double)gSens.tempC);
    drawText(12, y, b, 2); y += 26;
    if (gSens.humOk) {
      snprintf(b, sizeof(b), "Humedad %.0f%%", (double)gSens.hum);
      drawText(12, y, b, 2); y += 26;
    }
    if (gSens.pressOk) {
      snprintf(b, sizeof(b), "Presion %.0fhPa", (double)gSens.pressHpa);
      drawText(12, y, b, 2); y += 26;
    }
  }
  // Sin sonda externa: NO se escribe ninguna linea que se salga de la pantalla; basta
  // con la temperatura del chip de arriba.
}

// Escena 3: sistema. El operador pidio (2026-09-15) aqui casi vacia, sin la version (ocupa
// y no es necesaria). Solo tiempo encendido.
void pintaSistema() {
  pintaCabecera("Sistema");
  char b[40];
  snprintf(b, sizeof(b), "Encendido %lum", (unsigned long)(millis() / 60000));
  drawTextCenter(52, b, 2);
}

// Escena 4: estaciones oidas (la OLED tenia "ESTACIONES": indicativo + distancia y rumbo
// cuando hay fijacion GPS, o edad si no).
//
// â˜…â˜… EL MISMO FALLO DE ANCHO ESTABA AQUI (revisado y arreglado 2026-09-15) â˜…â˜…
//   QUE PASABA: `"%-9s %s"` NO recortaba el indicativo (9 es un ancho MINIMO), asi que un
//   `N0CALL-3` salia entero; el recorte a 6 caracteres era solo de "Ultimos RX". Pero el
//   ancho estaba calculado a ojo y el peor caso REAL no cabia: `N0CALL-15` (9) + 1 espacio +
//   una distancia de 7 caracteres ("999.9km") = 17 caracteres x 12 px (esta escena va a
//   ESCALA 2) = 204 px sobre un panel de 200. Con un indicativo de 9 el dato se salia por la
//   derecha. No se habia visto porque los indicativos de las pruebas eran de 7.
//   AHORA, con la cuenta hecha y el peor caso garantizado:
//     - indicativo: ancho de campo 10 + recorte EXPLICITO a 9. Nueve es el peor caso real de
//       este sistema (6 caracteres + guion + 2 cifras = "N0CALL-15"); mas largo no lo admite
//       ni AX.25, asi que no hay nada legitimo que recortar aqui;
//     - dato (distancia o edad): 5 caracteres. El formato se ha elegido PARA QUE NUNCA pase
//       de ahi: metros enteros por debajo de 1 km ("742m"), kilometros enteros de 1 a 4
//       cifras ("9km", "999km") y, por encima de 9999 km (imposible en LoRa, es una red de
//       decenas de km), ">9999". Se pierde el decimal del kilometro, que a esas distancias
//       no aporta nada;
//     - 10 + 1 + 5 = 16 caracteres = 192 px de 200, CON EL PEOR CASO. Y la columna del dato
//       empieza siempre en x = 12 + 11*12 = 144, asi que las filas se leen en columna.
void pintaEstaciones() {
  pintaCabecera("Estaciones");
  HeardStation hs[8];
  uint8_t n = aprsHeardStations(hs, 8);
  const GpsData &g = gpsGet();
  int y = 34;
  char b[48];
  if (n == 0) {
    drawTextCenter(90, "Ninguna aun", 2);
    return;
  }
  // Anchos en caracteres para la letra ESCALA 2 de esta escena (12 px por caracter).
  constexpr int kCallCol = 10;   // 9 de indicativo + 1 de aire
  constexpr int kDatoCol = 5;    // "999km" / "9999m" / "59min" / "23h"
  for (uint8_t i = 0; i < n && i < 5; i++) {
    if (g.fix && hs[i].hasPos) {
      float d = gpsDistanceM(g.lat, g.lon, hs[i].lat, hs[i].lon);
      char ds[10];
      if (d < 1000.0f) snprintf(ds, sizeof(ds), "%.0fm", (double)d);
      else if (d < 9999.0f * 1000.0f) snprintf(ds, sizeof(ds), "%.0fkm", (double)(d / 1000.0f));
      else snprintf(ds, sizeof(ds), ">9999");   // inalcanzable en LoRa: no se miente con un numero
      // %-10.9s = 10 de ancho MINIMO y recorte a 9 (el peor caso real con SSID).
      snprintf(b, sizeof(b), "%-*.*s %*s", kCallCol, kCallCol - 1, hs[i].call, kDatoCol, ds);
    } else {
      uint32_t s = (millis() - hs[i].ms) / 1000;
      char a[12];
      if (s < 60)      snprintf(a, sizeof(a), "%lus", (unsigned long)s);
      else if (s<3600) snprintf(a, sizeof(a), "%lum", (unsigned long)(s/60));
      else             snprintf(a, sizeof(a), "%luh", (unsigned long)(s/3600));
      snprintf(b, sizeof(b), "%-*.*s %*s", kCallCol, kCallCol - 1, hs[i].call, kDatoCol, a);
    }
    drawText(12, y, b, 2); y += 20;
  }
}

// Escena 5: ULTIMOS RX â€” lista de lo que se ha recibido (la OLED tenia "ULTIMOS RX").
//
// â˜… ESCALA 1 Y SALTO DE 20 px (2026-09-15, peticion del operador): antes iba a ESCALA 2
//   (letra de 14 px de alto) con salto 20. Ahora la letra es de 7 px (escala 1) y se MANTIENE
//   el salto de 20, o sea que las filas quedan mas aireadas y se leen de un vistazo.
//
// â˜…â˜… EL INDICATIVO YA NO SE CORTA (arreglado 2026-09-15) â˜…â˜…
//   QUE PASABA: la linea se montaba con `"%.6s  %.0f/%.0f"`, y ese `%.6s` RECORTA el
//   indicativo a 6 caracteres. Un `N0CALL-3` (7) perdia el SSID ENTERO y en la pantalla se
//   leia "N0CALL-" y nada mas; un `N0CALL-15` (9) perdia mas todavia. El operador lo vio
//   como "N0CALL-      -106/-4" y creyo que era cosa del tamano de letra (por eso se bajo a
//   escala 1), pero NO era el tamano: era el formato. Por eso se seguia cortando igual.
//   AHORA: ancho de campo con RECORTE EXPLICITO a la derecha ("%-11.11s"), asi que los
//   indicativos normales salen ENTEROS CON SU SSID y las columnas quedan alineadas (que es
//   de lo que va esta pantalla: poder comparar de un vistazo).
//
//   LA CUENTA DEL HUECO, que es lo que hay que respetar (panel de 200x200):
//     - cabecera: y = 4..25 (la pinta pintaCabecera)
//     - contenido: empieza en y = 30
//     - PIE: de y = 172 en adelante. El texto del "%" de la bateria dibuja en `pintaPie()`
//       con `drawText(..., by - 14, bb, 2)` y `by = 186`, o sea y = 172..185 (14 px de alto
//       a escala 2); el cuerpo de la bateria va en y = 186..199. **NADA de la lista puede
//       pasar de y = 172.**
//     6 filas con salto 20 desde y = 30: la ultima EMPIEZA en 30 + 5*20 = 130 y su letra
//       (7 px) termina en y = 136. Quedan 36 px de aire hasta el pie: cabe con holgura.
//       (A escala 1 el texto NO tiene descendentes: drawChar pinta 7 filas exactas, asi que
//       el calculo es el alto de la letra y nada mas.)
//     ANCHO, CON EL PEOR CASO REAL (`N0CALL-15`, que son 6 caracteres + guion + 2 cifras):
//         2 espacios de margen + [indicativo 11] + 1 espacio + [rssi/snr 9] =
//         2 + 11 + 1 + 9 = 23 caracteres x 6 px = 138 px de los 200. Cabe de sobra.
//         La columna del indicativo empieza en x=12 y la de rssi/snr SIEMPRE en el mismo
//         pixel (x = 12 + 12*6 = 84), que es lo que hace que las columnas se lean en columna.
//   OJO: NO se ha tocado `pintaUltimosTX` (sigue a escala 2) porque el operador pidio
//   expresamente esta escena; alli no hay indicativos que recortar (lo que se pinta son
//   etiquetas cortas: "BEACON", "TELEM"...), asi que no tiene este problema.
void pintaUltimosRX() {
  pintaCabecera("Ultimos RX");
  if (gRxLogN == 0) { drawTextCenter(90, "Nada recibido", 2); return; }
  constexpr int kEscala = 1;   // antes 2
  constexpr int kSalto = 20;   // el mismo que antes: mas aire entre filas
  // Ancho de las columnas, en caracteres (a escala 1 cada caracter mide 6 px).
  constexpr int kCallCol = 12;   // 11 de indicativo + 1 de aire (ver la cuenta de arriba)
  constexpr int kDatoCol = 9;    // "-120/-20" = 9 como mucho
  int y = 30;
  for (int i = 0; i < gRxLogN && i < 6; i++) {
    // del mas reciente al mas viejo
    int idx = (gRxLogHead - 1 - i + kRxLogMax) % kRxLogMax;
    const RxLog &e = gRxLog[idx];
    char b[40];
    // %-12.11s = 12 de ancho como MINIMO y recorte a 11 (el peor caso real: "N0CALL-15");
    // %*.*f    = el dato alineado a la derecha en su columna (anchura 9, 0 decimales).
    snprintf(b, sizeof(b), "%-*.*s %*.*f/%.0f",
             kCallCol, kCallCol - 1, e.call,
             kDatoCol, 0, (double)e.rssi, (double)e.snr);
    drawText(12, y, b, kEscala); y += kSalto;
  }
}

// Escena 6: ULTIMOS TX â€” lista de lo que se ha transmitido (la OLED tenia "ULTIMOS TX").
void pintaUltimosTX() {
  pintaCabecera("Ultimos TX");
  if (gTxLogN == 0) { drawTextCenter(90, "Nada transmitido", 2); return; }
  int y = 30;
  for (int i = 0; i < gTxLogN && i < 6; i++) {
    int idx = (gTxLogHead - 1 - i + kTxLogMax) % kTxLogMax;
    const TxLog &e = gTxLog[idx];
    char b[40];
    snprintf(b, sizeof(b), "TX %s", e.what);
    drawText(12, y, b, 2); y += 20;
  }
}

// Escena 7: GPS / TRACKER â€” la posicion en grande (la OLED tenia "GPS FIX").
void pintaGPS() {
  char t[24];
  const GpsData &g = gpsGet();
  snprintf(t, sizeof(t), "GPS %s", g.fix ? "FIX" : "sin fix");
  pintaCabecera(t);
  char b[40];
  int y = 34;
  if (g.fix) {
    snprintf(b, sizeof(b), "%.5f", g.lat);
    drawTextCenter(y, b, 2); y += 20;
    snprintf(b, sizeof(b), "%.5f", g.lon);
    drawTextCenter(y, b, 2); y += 20;
    // â˜… PARTIDO EN DOS LINEAS A ESCALA 2 (encargo del operador: el tamano mas pequeno se lee
    //   mal). El texto anterior "1234m  45km/h  9sat" son 19 caracteres y a escala 2 el paso
    //   por caracter es 12 px: 16 caracteres son 190 px, los 200 px de la pantalla. Una linea
    //   de 19 caracteres (226 px) se saldria. Se parte como pidio el operador: primero
    //   velocidad + satelites, y debajo la altitud.
    snprintf(b, sizeof(b), "%.0fkm/h  %usat", (double)g.speedKmh, (unsigned)g.sats);
    drawTextCenter(y, b, 2); y += 20;
    snprintf(b, sizeof(b), "Alt %.0fm", (double)(g.altValid ? g.altM : 0.0));
    drawTextCenter(y, b, 2); y += 20;
    // â˜… Y aqui lo mismo: "rumbo 359  HDOP 1.2" (19 caracteres) se parte en dos lineas.
    snprintf(b, sizeof(b), "Rumbo %.0f", (double)g.courseDeg);
    drawTextCenter(y, b, 2); y += 20;
    snprintf(b, sizeof(b), "HDOP %.1f", (double)g.hdop);
    drawTextCenter(y, b, 2);
  } else {
    drawTextCenter(y, "Buscando posic.", 2); y += 22;
    snprintf(b, sizeof(b), "%u sat a la vista", (unsigned)g.satsInView);
    drawTextCenter(y, b, 2);
  }
}

// â˜… Â¿Este aviso es de TRAFICO (RX / REPITE / TX)? Los tres empiezan por una letra que
//   ningun otro texto del firmware usa al principio, asi que la comprobacion es exacta:
//     displayNoteRx   -> "RX <indicativo>"
//     displayNoteDigi -> "REPITE <indicativo>"
//     displayNoteTx   -> "TX <QUE>"
//   Los de trafico son los que duran kAvisoMs (1,5 s) porque pueden llegar en rafaga y son
//   los que hacian que el panel estuviera pintando sin parar; los demas (bateria, menu,
//   arranque) duran kLineaMs y hay que poder leerlos.
static bool avisoDeTrafico(const char *s) {
  if (!s || !s[0]) return false;
  if (s[0] == 'R' || s[0] == 'T') return true;   // "RX ..." / "REPITE ..." / "TX ..."
  return false;
}

// Cuanto dura el aviso que hay ahora mismo en pantalla.
static uint32_t duracionAviso() { return avisoDeTrafico(gLinea1) ? kAvisoMs : kLineaMs; }

// ---------------------------------------------------------------------------
//  PANTALLA DE LA SESION "FIJAR COORDS" (2026-09-15)
//
//  Se pinta EN LUGAR del menu o del carrusel mientras dura la captura. Esta
//  arriba del todo en `dibujaEscena()` a proposito: la captura la lanza el menu y
//  sin esto el operador se quedaria mirando una lista quieta (que en tinta parece
//  colgada) durante los minutos que puede tardar el GPS.
//
//  â˜…â˜… LO QUE PIDE EL OPERADOR CON ESTA PANTALLA (sus palabras) â˜…â˜…
//  "Â¿hay una ventana de estado mientras se realiza la tarea? Pues que muestre como
//  va y en que parte del proceso, para que el usuario este tranquilo."
//  Por eso NINGUN estado se queda sin decir algo que CAMBIE:
//    - BUSCANDO: "Buscando GPS..." + LOS SATELITES QUE VE AHORA (que es lo que
//      demuestra que el aparato esta trabajando, aunque aun no haya fijado) + el
//      aviso de que puede tardar y de que se puede salir con un boton.
//    - ASENTANDO: "Asentando..." + "Muestra n/N" + BARRA DE PROGRESO + satelites.
//    - GUARDADO: latitud y longitud guardadas (la prueba de que se hizo).
//    - FALLO: que no se pudo guardar.
//  Y en todos: "Toca un boton para salir" / "para cancelar". La sesion NO se cierra
//  sola por tiempo: la fijacion tarda lo que tarde (decision del operador).
//
//  â˜… EL CONTADOR ES EL DEL RASTREADOR (trackerSetCoordsNeed), no un numero escrito
//    aqui: si alli se cambia el numero de muestras, la pantalla lo dice sola.
// ---------------------------------------------------------------------------
void pintaSesionCoords() {
  pintaCabecera("FIJAR COORDS");

  const GpsData &g = gpsGet();
  char b[40];

  if (gCoordsPantalla == COORDS_BUSCANDO || (gCoordsPantalla == COORDS_ASENTANDO && !g.fix)) {
    drawTextCenter(48, "Buscando GPS...", 2);
    // â˜… LOS SATELITES A LA VISTA, EN GRANDE Y SIEMPRE: es el unico numero que se
    //   mueve mientras no hay fijacion, y es lo que le dice al operador que el
    //   receptor esta oyendo el cielo y que solo falta esperar. Sin esto, una
    //   pantalla quieta durante minutos parece un cuelgue.
    snprintf(b, sizeof(b), "%u sat a la vista", (unsigned)g.satsInView);
    drawTextCenter(78, b, 2);
    drawTextCenter(106, "puede tardar minutos", 1);
    drawTextCenter(120, "cuanto mas cielo, antes", 1);
    drawTextCenter(168, "Toca un boton para cancelar", 1);
  } else if (gCoordsPantalla == COORDS_ASENTANDO) {
    // Hay fijacion: se cuentan las lecturas SEGUIDAS que pide el rastreador antes
    // de dar la posicion por buena (las primeras traen el salto tipico del fix).
    const uint8_t need = trackerSetCoordsNeed() ? trackerSetCoordsNeed() : 1;
    const uint8_t done = trackerSetCoordsDone();
    drawTextCenter(44, "Asentando...", 2);
    snprintf(b, sizeof(b), "Muestra %u/%u", (unsigned)done, (unsigned)need);
    drawTextCenter(74, b, 2);
    // Barra de progreso: marco + relleno. Con 20 muestras avanza de 5 en 5 %.
    const int bx0 = 20, bx1 = EPD_W - 20, by0 = 100, by1 = 114;
    rectVacio(bx0, by0, bx1, by1);
    const int w = (bx1 - bx0 - 2);
    if (done > 0 && w > 0) relleno(bx0 + 1, by0 + 1, w * (int)done / (int)need, by1 - by0 - 1);
    // Los satelites y la posicion que se esta midiendo: la pantalla sigue contando
    // algo nuevo en cada escalon, no solo el numero de muestras.
    snprintf(b, sizeof(b), "%u sat  %.5f", (unsigned)g.sats, g.lat);
    drawTextCenter(126, b, 1);
    drawTextCenter(168, "Toca un boton para cancelar", 1);
  } else if (gCoordsPantalla == COORDS_GUARDADO) {
    // Se guardo: se ensena LO QUE SE HA GUARDADO, que es la comprobacion que el
    // operador necesita (y asi no hay que ir al menu a mirar Latitud/Longitud).
    drawTextCenter(44, "GUARDADO", 2);
    hLine(20, EPD_W - 20, 68, 1);
    snprintf(b, sizeof(b), "%.5f", gCoordsLat);
    drawTextCenter(78, b, 2);
    snprintf(b, sizeof(b), "%.5f", gCoordsLon);
    drawTextCenter(98, b, 2);
    drawTextCenter(126, "es la posicion fija del nodo", 1);
    drawTextCenter(140, "GPS apagado otra vez", 1);
    drawTextCenter(168, "Toca un boton para salir", 1);
  } else if (gCoordsPantalla == COORDS_FALLO) {
    drawTextCenter(54, "ERROR AL GUARDAR", 2);
    drawTextCenter(88, "la posicion no se ha", 1);
    drawTextCenter(102, "guardado en la config", 1);
    drawTextCenter(130, "prueba otra vez", 1);
    drawTextCenter(168, "Toca un boton para salir", 1);
  }
}

// â˜… Dibuja en `gBuf` lo que toque AHORA (la escena del carrusel, con el aviso reciente
//   ENCIMA si lo hay) y deja `gDirty` puesto. Es el UNICO sitio donde se decide que se ve.
//   Lo usan `displayRefresh()` (el bucle normal) y la prueba del carrusel del comando
//   `epdparcial` (herramienta de taller): asi lo que se prueba es exactamente lo mismo que
//   se vera luego en el carrusel automatico del Paso 2, y no una copia que puede divergir.
void dibujaEscena() {
  clearBuf(true);
  // â˜…â˜… LA SESION "FIJAR COORDS" MANDA SOBRE TODO (2026-09-15) â˜…â˜…
  //   Va ANTES del menu a proposito: la captura se lanza desde el menu y dura
  //   minutos, asi que mientras esta en marcha lo que hay que ver es su progreso,
  //   no la lista (que en tinta parece colgada). Ver pintaSesionCoords().
  if (coordsPantallaActiva()) {
    pintaSesionCoords();
    pintaPie();
    gDirty = true;
    return;
  }
  // â˜… Si el menÃº esta abierto, se pinta el MENU en lugar del carrusel (2026-09-15).
#ifdef TRACKS_DISPONIBLE
  // â˜…â˜… Y SI ESTAN ABIERTAS LAS PANTALLAS DE TRACKS, SE PINTAN ELLAS (2026-09-22) â˜…â˜…
  //   Van antes que el menu porque lo SUSTITUYEN (cuando se abren, `gMenuOn` se pone a false).
  //   Mismo patron que el menu, incluido el auto-cierre: si al pintar resultara que ya se han
  //   cerrado solas por el timeout, se sigue hacia abajo y se pinta el carrusel en vez de
  //   dejar el panel en blanco.
  if (pantallaTracksActiva()) {
    // â˜…â˜… LA PANTALLA DE GUIADO **NO** SE CIERRA SOLA (2026-09-22) â˜…â˜…
    //   El auto-cierre de 15 s es una regla de MENU (para que no se quede abierto sin querer),
    //   y para un menu esta bien. Pero la pantalla de guiado es una pantalla de TRABAJO que se
    //   mira de reojo mientras se anda: si se cerrara a los 15 s, el usuario tendria que dar
    //   CUATRO gestos (largo, navegar, corto, corto) para volver a ver el rumbo, cada 15 s. Y lo
    //   peor: el guiado NO se para al cerrarse, asi que el aparato seguiria guiando sin
    //   ensenarlo. Lo cazo una revision independiente.
    //   Se sale cuando el usuario quiere (pulsacion larga), no por un reloj.
    if (gTrkPant == TRK_PANT_GUIA) {
      pantallaTracksPinta(); gDirty = true; return;
    }
    // ★★ G4 (2026-10-06): Y AQUI HABIA OTRO CIERRE POR TIEMPO, QUE SOBRABA ★★
    //   Estaba puesto en las pantallas de tracks igual que en el menu, con 15 segundos. Pero una
    //   LISTA no es un menu: al menu se entra, se elige y se sale, y cerrarlo solo si te olvidas
    //   esta bien. Una lista se LEE, y el aparato no se refresca solo (es tinta electronica): se
    //   queda pintada. Asi que pasaban los 15 segundos mientras el operador miraba, y **la
    //   siguiente pulsacion CERRABA la pantalla en vez de moverse por la lista**.
    //   El operador lo conto asi: «muevo hasta ahi, toque corto en boton, no hace nada, no elige
    //   la ruta ni hace nada, simplemente se queda ahi».
    //   ★ AHORA NO HAY CIERRE POR TIEMPO. Se sale como en la pantalla de guiado: pulsacion larga
    //     (sube un nivel) o la fila «Salir». Es coherente con que la de guiado tampoco se cierre,
    //     y quita de en medio la unica forma de que el boton «no haga nada».
    if (gTrkPant == TRK_PANT_SALIDAS) { pantallaSalidasPinta(); gDirty = true; return; }
    pantallaTracksPinta(); gDirty = true; return;
  }
#endif
  if (menuIsOpen()) {
    menuPinta();    // ARREGLO del "pantalla en blanco" (2026-09-15): menuPinta() puede AUTO-CERRAR el menu
    // por el timeout de 15 s y dejar gBuf en blanco. Si tras pintar el menu ya NO esta
    // abierto, se CONTINUA abajo y se dibuja la escena del carrusel, en vez de devolver.
    if (menuIsOpen()) { gDirty = true; return; }
  }
  // â˜…â˜… LA ESCENA SE PINTA SIEMPRE; EL AVISO VA ENCIMA (2026-09-15) â˜…â˜…
  //   Antes esto era un "if (aviso) ... else { escena }": con un aviso en pantalla la escena
  //   NO se dibujaba, asi que el primer refresco dejaba el panel con el aviso solo y hacia
  //   falta un SEGUNDO refresco (al caducar) para volver a ver la escena. Pintando la escena
  //   debajo y el aviso encima, el paso 2 ya es "la escena limpia": un aviso, un refresco de
  //   contenido, y a los 1,5 s la escena de vuelta.
  switch (gEscena) {
    case 1: pintaRadio(); break;
    case 2: pintaSensores(); break;
    case 3: pintaSistema(); break;
    case 4: pintaEstaciones(); break;
    case 5: pintaUltimosRX(); break;
    case 6: pintaUltimosTX(); break;
    case 7: pintaGPS(); break;
    default: pintaEstado(); break;   // 0 = Estado (la que se ve casi siempre)
  }
  // El pie (PIE DE LA OLED): rectangulitos del carrusel + bateria, SIEMPRE debajo
  // del contenido y por encima de la huella, para que acompaÃ±e a todas las escenas.
  pintaPie();
  // El aviso, ENCIMA de todo y sin tapar el pie (que es donde vive la bateria).
  pintaAviso();
  gDirty = true;
}

// Voltaje de bateria que se ENSEÃ‘A (en milivoltios, redondeado a 0,01 V).
//
// â˜…â˜… POR QUE NO SE USA `powerReadMv()` AQUI (2026-09-15) â˜…â˜…
// `powerReadMv()` lee el ADC CADA VEZ que se llama y le pasa un filtro suavizador. En esta
// unidad el divisor de la bateria NO esta poblado (ya estaba documentado: `bat` contesta
// 0,00 V aunque el pin ya es el correcto, P0.04), asi que el convertidor lee RUIDO que cambia
// en cada muestra: el numero que se dibujaba era distinto a cada vuelta del bucle.
// Consecuencia medida: la huella del contenido (`huellaContenido()`) cambiaba sola y el
// firmware **repintaba casi cada segundo** (66 repintados en 60 s), que es justo lo que el
// Paso 2 tiene que evitar y lo que ensucia la pantalla.
// Se usa la MISMA lectura cacheada que el resto del firmware (`sensorsBatteryVolt()`, que ya
// descarta lo que no es un voltaje de bateria plausible) y se redondea a 0,01 V, que es la
// precision con la que se dibuja. Asi lo que se ve y lo que se compara son lo mismo.
//   Devuelve 0 cuando no hay lectura creible (y entonces se escribe "sin lectura").
uint16_t bateriaMv() {
  const float v = sensorsBatteryVolt(gSens);
  if (v <= 0.0f) return 0;
  return (uint16_t)(v * 1000.0f + 0.5f);
}

// ===========================================================================
//  â˜…â˜… PASO 2: "NO REPINTAR SI NO HA CAMBIADO NADA" (2026-09-15) â˜…â˜…
//
//  POR QUE EXISTE ESTO: el firmware repintaba cada 5 s por reloj.
//  cambiara algo o no. Medido en hardware con el carrusel puesto: **19 repintados en 50
//  segundos** (16 de ellos sin motivo). Cada repintado deja su resto de tinta y gasta
//  bateria, y el propio HANDOVER avisa de que el parcial ensucia si se abusa de el.
//
//  COMO SE ARREGLA: antes de pintar se calcula una **huella** de TODO lo que se va a ver en
//  la pantalla que toca â€”la escena, el modo, el GPS, los sensores, la bateria, los
//  contadores de radio y el aviso de "ultimo"â€” y se compara con la huella de lo ultimo
//  pintado. Si son iguales, **no se manda nada al panel**.
//
//  â˜… DOS COSAS QUE HAY QUE ENTENDER DE ESTA HUELLA, o se rompe sola:
//    1. Se construye con los MISMOS valores y los MISMOS redondeos que usa el dibujo
//       (coordenadas a 5 decimales, velocidad a 0 decimales, bateria a 2...). Si aqui se
//       redondea distinto que alli, o se repinta de mas o â€”peorâ€” no se repinta cuando la
//       pantalla si ha cambiado.
//    2. Los avisos ("hace Ns") NO entran con su reloj a proposito: entrarian cambiando cada
//       segundo y volveriamos al repintado continuo. El aviso caduca solo (a los 1,5 s si es
//       de trafico, a los 8 s si no: ver `duracionAviso()`) y ese cambio SI se detecta porque
//       la linea pasa a estar vacia. Ese es el segundo y ULTIMO refresco de un aviso: no hay
//       ningun estado intermedio que obligue a un tercero.
//    3. â˜… EL AVISO NO CAMBIA LO QUE HAY DEBAJO (2026-09-15): `dibujaEscena()` pinta SIEMPRE
//       la escena y el aviso encima. Si algun dia se volviera a "con aviso, no pintes la
//       escena", volveria el doble paso y esta huella no lo detectaria (la escena no entra en
//       la huella cuando hay aviso: solo entra el aviso).
//
//  Y queda un repintado "de refresco" cada `kRepintadoMaxMs` (60 s) por si algo se escapa de
//  la huella: la pantalla nunca se queda con un dato viejo para siempre.
// ===========================================================================
constexpr uint32_t kRepintadoMaxMs = 60000;   // repintado de refresco, aunque no cambie nada
uint64_t gHuellaPintada = 0;
bool gHuellaValida = false;

void huellaAnade(uint64_t &h, const void *datos, size_t n) {
  const uint8_t *p = (const uint8_t *)datos;
  for (size_t i = 0; i < n; i++) {
    h ^= (uint64_t)p[i];
    h *= 1099511628211ULL;   // FNV-1a de 64 bits
  }
}

uint64_t huellaContenido() {
  uint64_t h = 1469598103934665603ULL;
  huellaAnade(h, &gEscena, sizeof(gEscena));
  const uint8_t modo = (uint8_t)(gCfg ? gCfg->mode : 0);
  huellaAnade(h, &modo, sizeof(modo));

  // Si hay un aviso reciente, lo que se ve es el aviso (no la escena): misma regla que
  // `dibujaEscena()`.
  const uint8_t aviso = (gLinea1[0] != '\0') ? 1u : 0u;
  huellaAnade(h, &aviso, sizeof(aviso));
  huellaAnade(h, gLinea1, sizeof(gLinea1));
  huellaAnade(h, gLinea2, sizeof(gLinea2));

  const GpsData &g = gpsGet();
  const uint8_t fix = g.fix ? 1u : 0u;
  huellaAnade(h, &fix, sizeof(fix));
  const long lat = lround(g.lat * 100000.0);     // 5 decimales, como en pantalla
  const long lon = lround(g.lon * 100000.0);
  huellaAnade(h, &lat, sizeof(lat));
  huellaAnade(h, &lon, sizeof(lon));
  const int vel = (int)lround(g.speedKmh);       // 0 decimales, como en pantalla
  const int sat = (int)g.sats;
  huellaAnade(h, &vel, sizeof(vel));
  huellaAnade(h, &sat, sizeof(sat));

  // â˜… `satVista` (los satelites "a la vista", aun sin fijacion) NO entra en la huella.
  //   Medido con el instrumento `epdhuella`: ese numero baila de 5 a 11 en pocos segundos y
  //   era el que hacia que la pantalla se repintara sin parar aunque la linea que se dibuja
  //   apenas cambie. La posicion y la velocidad, que si importan, siguen entrando. La
  //   pantalla pierde un "sat a la vista" que parpadeaba solo; gana no repintarse cada
  //   segundo (2026-09-15).

  const int temp = gSens.tempOk ? (int)lround(gSens.tempC * 10.0) : -9999;
  const int hum  = gSens.humOk ? (int)lround(gSens.hum) : -9999;
  const int pres = gSens.pressOk ? (int)lround(gSens.pressHpa) : -9999;
  huellaAnade(h, &temp, sizeof(temp));
  huellaAnade(h, &hum, sizeof(hum));
  huellaAnade(h, &pres, sizeof(pres));

  // La MISMA lectura redondeada que se dibuja (ver `bateriaMv()`): el ruido del ADC de esta
  // unidad no puede entrar aqui o la huella cambiaria sola y se repintaria sin parar.
  const uint16_t mv = bateriaMv();
  huellaAnade(h, &mv, sizeof(mv));
  huellaAnade(h, &gRx, sizeof(gRx));
  huellaAnade(h, &gTx, sizeof(gTx));
  huellaAnade(h, &gDg, sizeof(gDg));

  // â˜… ESTADO DE "RECIEN TRANSMITIDO" + radio lista/mute (ARREGLO A1 de la auditoria,
  //   2026-09-15): la pastilla RX/TX y el aviso de TX se deciden con
  //   `radioLastTxMs()<3000` y `txDisabled`/`radioReady()`, pero esos datos NO entraban en
  //   la huella, asi que tras una emision la pastilla negra "TX" se quedaba CONGELADA hasta
  //   los 60 s (el panel es bistable). Meter aqui el flag hace que a los 3 s la huella
  //   cambie y el repintado la devuelva a RX/borre el aviso.
  const uint8_t txReciente = (radioLastTxMs() != 0 &&
                             (uint32_t)(millis() - radioLastTxMs()) < 3000) ? 1u : 0u;
  huellaAnade(h, &txReciente, sizeof(txReciente));
  const uint8_t txMudo = (gCfg && gCfg->txDisabled) ? 1u : 0u;
  huellaAnade(h, &txMudo, sizeof(txMudo));
  const uint8_t radioListo = radioReady() ? 1u : 0u;
  huellaAnade(h, &radioListo, sizeof(radioListo));

  // â˜… PERFIL ACTIVO (2026-09-15): la escena Estado dibuja un icono que depende de
  //   `smartBeaconPreset`. Elegirlo desde el MENU ya fuerza repintado (`gDirty`), pero se
  //   puede cambiar tambien desde el configurador web o por CLI, y ahi nadie avisa: el panel
  //   es bistable, asi que sin meterlo en la huella el icono se quedaria enseÃ±ando el perfil
  //   VIEJO hasta el repintado de refresco de 60 s. Es el mismo fallo --y el mismo arreglo--
  //   que el "arreglo A1" de la pastilla TX de arriba.
  const uint8_t perfilIcono = (uint8_t)(gCfg ? gCfg->smartBeaconPreset : 0);
  huellaAnade(h, &perfilIcono, sizeof(perfilIcono));
  // â˜… ICONO DEL MAPA DEL PERFIL (2026-09-15): el par (tabla, codigo) de cada perfil
  //   se puede cambiar desde el MENU, desde el configurador web o con `set`, y el
  //   icono de la ESCENA tambien es un dato del perfil: si se cambia por web, sin
  //   esto la pantalla se quedaria con el dibujo viejo hasta el repintado de 60 s.
  if (gCfg) {
    for (int i = 0; i < 4; i++) {
      huellaAnade(h, gCfg->profileOverlay[i], 1);
      huellaAnade(h, gCfg->profileSymbol[i], 1);
    }
  }
  // â˜… SESION "FIJAR COORDS" (2026-09-15): la pantalla de la captura solo puede
  //   cambiar en unos pocos numeros (fase, muestras hechas, muestras que hacen
  //   falta y la posicion guardada). Con ellos en la huella, el progreso se
  //   repinta SOLO cuando cambia de escalon y no en cada vuelta del bucle.
  const uint8_t coordsPantalla = gCoordsPantalla;
  huellaAnade(h, &coordsPantalla, sizeof(coordsPantalla));
  if (coordsPantalla != COORDS_OCULTA) {
    const uint8_t need = trackerSetCoordsNeed();
    const uint8_t done = trackerSetCoordsDone();
    huellaAnade(h, &need, sizeof(need));
    huellaAnade(h, &done, sizeof(done));
    huellaAnade(h, &gCoordsLat, sizeof(gCoordsLat));
    huellaAnade(h, &gCoordsLon, sizeof(gCoordsLon));
  }
  return h;
}

// â˜…â˜… INSTRUMENTO: Â¿QUE DATO DE LA HUELLA ESTA CAMBIANDO? (2026-09-15) â˜…â˜…
//
// Cuando el firmware repinta "sin motivo", el problema esta en un dato que entra en la huella
// y que cambia solo. Adivinar cual cuesta tardes; medirlo, dos comandos. Esto saca la huella
// y TODOS sus ingredientes en una linea: se llama dos veces y se comparan los numeros.
//
// Existe porque con la huella puesta se midieron 52 repintados en 60 segundos y el primer
// sospechoso (la bateria, cuyo ADC lee ruido en esta unidad) NO era el unico: hay que seguir
// midiendo hasta que el numero de repintados baje a lo que cambia de verdad.
void epdHuellaTexto(char *out, size_t n) {
  const GpsData &g = gpsGet();
  snprintf(out, n,
           "EPD huella=0x%08lX%08lX escena=%d aviso=%d | GPS fix=%d lat5=%ld lon5=%ld "
           "vel=%d sat=%d satVista=%d | temp=%d hum=%d pres=%d | batMv=%u rx=%lu tx=%lu dg=%lu",
           (unsigned long)(uint32_t)(huellaContenido() >> 32),
           (unsigned long)(uint32_t)(huellaContenido() & 0xFFFFFFFFULL),
           (int)gEscena, (gLinea1[0] != '\0') ? 1 : 0,
           g.fix ? 1 : 0, lround(g.lat * 100000.0), lround(g.lon * 100000.0),
           (int)lround(g.speedKmh), (int)g.sats, (int)g.satsInView,
           gSens.tempOk ? (int)lround(gSens.tempC * 10.0) : -9999,
           gSens.humOk ? (int)lround(gSens.hum) : -9999,
           gSens.pressOk ? (int)lround(gSens.pressHpa) : -9999,
           (unsigned)bateriaMv(), (unsigned long)gRx, (unsigned long)gTx, (unsigned long)gDg);
}

}  // namespace

void displayRefresh(const DigiConfig &cfg, uint32_t rxCount, uint32_t txCount,
                    uint32_t digiCount, const SensorReadings &r) {
  if (!gReady) return;
  gRx = rxCount; gTx = txCount; gDg = digiCount; gSens = r;

  // â˜…â˜… SESION "FIJAR COORDS": SEGUIMIENTO DESDE LA PANTALLA (2026-09-15) â˜…â˜…
  // El rastreador es quien manda sobre el GPS (trackerSetCoordsStart/Tick, ver
  // tracker.cpp); aqui solo se MIRA en que punto esta y se ensena. El aviso por
  // USB ya lo da el rastreador ("{\"setcoords\":...}"), asi que aqui no se repite.
  //   TRK_COORDS_BUSCANDO  -> pantalla "Buscando GPS..." (sin tope de tiempo)
  //   TRK_COORDS_ASENTANDO -> pantalla "Asentando... n/N" con barra
  //   TRK_COORDS_GUARDADO  -> pantalla con la posicion guardada
  //   TRK_COORDS_ERROR     -> pantalla de error al guardar
  // â˜… SOLO SE DECIDE REPINTAR POR ESCALONES (0, 25, 50, 75, 100 %): cada refresco
  //   de este panel cuesta 1,5 s y lo manda la HUELLA, que es la que compara el
  //   texto que se vera. Con el numero de muestras exacto se repintaria en cada
  //   muestra (~20 refrescos, 30 s de panel pintando) para ver cambiar un digito.
  //   Los SATELITES a la vista son la excepcion a proposito: cuando no hay
  //   fijacion son el unico dato que se mueve, y sin ellos la pantalla parece
  //   colgada (que es justo lo que el operador no quiere). Se refrescan de 30 en
  //   30 s como mucho.
  {
    static uint8_t pasoPintado = 255;
    static uint32_t ultimoSatsMs = 0;
    const uint8_t estado = trackerSetCoordsEstado();
    if (!coordsPantallaActiva()) {
      pasoPintado = 255;   // sesion cerrada: el proximo aviso empieza limpio
    } else if (estado == TRK_COORDS_GUARDADO || estado == TRK_COORDS_ERROR) {
      gCoordsPantalla = (estado == TRK_COORDS_GUARDADO) ? COORDS_GUARDADO : COORDS_FALLO;
      pasoPintado = 255;
      gDirty = true;
      if (diagTrazaTaller()) {
        Serial.printf("PANTALLA: fijar coords -> %s\r\n",
                      (estado == TRK_COORDS_GUARDADO) ? "guardado" : "error");
      }
    } else {
      const bool asentando = (estado == TRK_COORDS_ASENTANDO);
      const uint8_t need = trackerSetCoordsNeed() ? trackerSetCoordsNeed() : 1;
      const uint8_t done = trackerSetCoordsDone();
      const uint8_t paso = asentando ? (uint8_t)((done * 4u) / need) : 0;   // 0..4
      const uint8_t quiero = asentando ? COORDS_ASENTANDO : COORDS_BUSCANDO;
      // Mientras se busca, el dibujo lleva los satelites a la vista: se repinta
      // cada 30 s para que el operador VEA que el receptor trabaja (y solo si el
      // numero ha cambiado, que de eso se encarga la huella).
      const bool tocaSats = !asentando &&
                            (uint32_t)(millis() - ultimoSatsMs) >= 30000u;
      if (paso != pasoPintado || quiero != gCoordsPantalla || tocaSats) {
        if (tocaSats) ultimoSatsMs = millis();
        pasoPintado = paso;
        gCoordsPantalla = quiero;
        gDirty = true;
      }
    }
  }

  // â˜… TOQUE EN LA PANTALLA DE LA SESION (2026-09-15).
  //   - CON LA CAPTURA EN MARCHA: el toque CANCELA. Es la condicion que puso el
  //     operador al quitar el tope de tiempo ("que el usuario pueda salir de ahi
  //     con un boton"): la sesion espera lo que haga falta, asi que la salida la
  //     decide el, no un reloj. trackerSetCoordsCancel() apaga el GPS si lo
  //     encendio la sesion y lo deja como estaba si ya estaba encendido.
  //     â˜… El guardia de kDebounceToqueMs compara con el momento en que se LANZO la
  //     sesion (no solo con el ultimo toque): asi el toque con el que el operador
  //     acaba de elegir "Fijar coords" en el menu no cancela lo que acaba de
  //     arrancar.
  //   - CON RESULTADO YA EN PANTALLA (guardado o error): el toque la quita y
  //     vuelve al menu o al carrusel, sin tocar nada mas.
  {
    const bool toque = (uint32_t)(millis() - gUltimoToqueMs) < kDebounceToqueMs;
    if (coordsPantallaActiva() && toque) {
      if (gCoordsPantalla == COORDS_GUARDADO || gCoordsPantalla == COORDS_FALLO) {
        coordsPantallaCierra();
      } else if ((uint32_t)(millis() - gCoordsInicioMs) >= kDebounceToqueMs) {
        trackerSetCoordsCancel();   // fuera de la sesion y con el GPS devuelto
        coordsPantallaCierra();
        if (diagTrazaTaller()) {
          Serial.println("PANTALLA: fijar coords cancelado por el operador");
        }
      }
    }
  }

  // â˜…â˜… CAMBIO DE ROTACION EN CALIENTE (2026-09-14) â˜…â˜…
  // Si el usuario guarda `epdRotation` (configurador web, `set epdRotation N`), la
  // configuracion cambia y aqui se aplica SIN REINICIAR: se repinta con la orientacion
  // nueva. La variable se lee de la config, no de una constante, y se compara con la
  // ultima que se pinto.
  {
    const uint8_t r = (cfg.epdRotation <= 3) ? cfg.epdRotation : 0;
    if ((int)r != gRotacion) {
      gRotacion = (int)r;
      gDirty = true;
      gUltimoPintado = 0;   // que el repintado no se posponga por el limitador de tiempo
      if (diagTrazaTaller()) {
        Serial.printf("PANTALLA: rotacion cambiada a %d, repintando\r\n", gRotacion);
      }
    }
  }

  // â˜…â˜… PASO 2: EL CARRUSEL AUTOMATICO (2026-09-15) â˜…â˜…
  // El ajuste `sceneAutoAdvance` (el mismo de la OLED) manda aqui de verdad. La pausa tras
  // una pulsacion del boton se respeta siempre: si el operador acaba de elegir pantalla, el
  // carrusel no se la cambia.
  gAutoAvance = cfg.sceneAutoAdvance;
  {
    const uint32_t ahoraCarrusel = millis();
    if (gUltimoCambioEscenaMs == 0) gUltimoCambioEscenaMs = ahoraCarrusel;   // arranque de la cuenta
    const bool pausado = (int32_t)(ahoraCarrusel - gCarruselPausadoHasta) < 0;
    if (gAutoAvance && !pausado && (ahoraCarrusel - gUltimoCambioEscenaMs) >= kEscenaAutoMs) {
      gUltimoCambioEscenaMs = ahoraCarrusel;
      gEscena = (uint8_t)((gEscena + 1) % kNumEscenas);
      gDirty = true;
      if (diagTrazaTaller()) {
        Serial.printf("PANTALLA: carrusel -> escena %d\r\n", (int)gEscena);
      }
    }
  }

  // El aviso caduca solo: asi la pantalla vuelve a la escena normal. â˜… UN SOLO PASO
  // (2026-09-15): los avisos de TRAFICO (RX / REPITE / TX) duran `kAvisoMs` (1,5 s) y los
  // demas `kLineaMs` (8 s). Este es el UNICO sitio donde expira un aviso: la banda "TX" que
  // tenia su propio reloj de 3000 ms ya no existe (ver pintaAviso).
  if (gLinea1[0] && (millis() - gLineaMs) > duracionAviso()) {
    gLinea1[0] = '\0';
    gLinea2[0] = '\0';
    gDirty = true;
  }

  // â˜…â˜… PASO 2: SOLO SE REPINTA SI LO QUE SE VA A VER ES DISTINTO (2026-09-15) â˜…â˜…
  // Antes esto era "repintar cada 5 s pase lo que pase", y con el carrusel se midieron 19
  // repintados en 50 s (16 sin motivo). Ahora manda la huella del contenido; el reloj solo
  // fuerza un repintado de refresco cada `kRepintadoMaxMs`, por si algo se escapa.
  const uint32_t ahora = millis();
  const uint64_t huella = huellaContenido();
  const bool cambioElContenido = (!gHuellaValida || huella != gHuellaPintada);
  const bool tocaPorTiempo = (ahora - gUltimoPintado) > kRepintadoMaxMs;

  // â˜… TOQUE RAPIDO (2026-09-15): si acabas de TOCAR EL TACTIL hace menos de
  //   kAgrupaToquesMs, se APLAZA el repintado (se mantiene gDirty sin pintar). Asi, si
  //   tocas varias veces seguidas en el carrusel o el menu, la posicion LOGICA avanza al
  //   instante en cada toque y solo se pinta UNA vez cuando dejas de tocar, mostrando la
  //   posicion final.
  //   â˜… T-ECHO PROJECT BUTTER (2026-09-15): esto SOLO cuenta para el tactil capacitivo
  //     (`gUltimoToqueAgrupaMs`). El boton fisico ya no lo escribe: su toque corto sale
  //     600 ms despues de soltar (ventana del doble) y dos cortos nunca caen dentro de
  //     estos 400 ms, asi que lo unico que hacia era retrasar 400 ms CADA cambio de
  //     diapositiva pedido con el boton. Ver la nota de kAgrupaToquesMs.
  // â˜…â˜… EL APLAZAMIENTO, MEDIDO DESDE QUE DEJASTE DE TOCAR (b82) â˜…â˜…
  //
  // HISTORIA, porque aqui me he equivocado DOS veces y las dos estan documentadas:
  //   (1) REGLA ORIGINAL: `if (toqueReciente) return;` con un reloj que empezaba AL PRIMER
  //       TOQUE. Un toque suelto pagaba los 400 ms enteros ("desde que pulso hasta que lo veo
  //       dibujado pasa un poquito de tiempo", dijo el operador). Funcionaba, pero lento.
  //   (2) b76, con un tope de 150 ms desde el primer toque: empezaba a pintar ANTES de que el
  //       operador acabara -> pintaba estados INTERMEDIOS y, con el bucle ocupado ~700 ms
  //       pintando, los toques siguientes se ejecutaban tarde: "no guarda si pulso varias veces".
  //   (3) b77/b78, "por estado": la pantalla miraba/vaciaba LA COLA DE ACCIONES del boton.
  //       Esa cola no es un aviso de trabajo: es lo que el bucle cobra y ejecuta. Quitarsela =
  //       el toque se pierde: "no funciona el boton capacitivo".
  //
  // â˜…â˜… LA REGLA BUENA (esta): se aplaza mientras el ULTIMO TOQUE CONFIRMADO sea reciente. El
  //    sello lo pone el boton (`buttonUltimoToqueConfirmado()`) y la pantalla SOLO LO LEE: no
  //    vacia nada, no se lleva ninguna accion por delante. Es la pieza que faltaba en (1), que
  //    medÃ­a desde que EMPEZABAS a tocar en vez de desde que acababas.
  //
  //    RESULTADO: un toque suelto se pinta kAplazoTrasToqueMs despues de soltar (150 ms), en
  //    vez de 400 ms despues de empezar. Y una rafaga sigue pagando UN solo repintado, porque
  //    cada toque nuevo renueva el sello. Ademas NUNCA pinta un estado intermedio: si has
  //    vuelto a tocar dentro de la ventana, el sello es reciente y se sigue esperando.
  //
  //    â˜… `kAplazoTrasToqueMs` tiene que ser MAYOR que el bloqueo del tactil (120 ms en
  //      button.cpp) para que una rafaga no se cuele entre dos toques. 150 lo cumple.
  constexpr uint32_t kAplazoTrasToqueMs = 150;
  const uint32_t ultimoToque = buttonUltimoToqueConfirmado();
  const bool toqueReciente =
      (ultimoToque != 0) && ((uint32_t)(ahora - ultimoToque) < kAplazoTrasToqueMs);

  if (!gDirty && !cambioElContenido && !tocaPorTiempo) return;
  if (toqueReciente && !gMenuEditing && gDirty) return;   // espera a que dejes de tocar

  gUltimoPintado = ahora;
  gHuellaPintada = huella;
  gHuellaValida = true;
  gDirty = false;
  gRotacionAplicada = gRotacion;   // queda constancia de con cual se ha pintado

  // â˜…â˜… OJO CON EL ORDEN, QUE AQUI SE COLO UN FALLO (2026-09-15) â˜…â˜…
  // `dibujaEscena()` dibuja en memoria y deja puesto `gDirty` ("hay algo que mandar").
  // `epdFlush()` es quien mira ese aviso y pinta de verdad. La version anterior de este
  // bloque borraba `gDirty` DESPUES de dibujar y ANTES de llamar a `epdFlush()`, asi que
  // `epdFlush()` se salia por su primera linea y **no mandaba nada al panel**.
  // Sintoma exacto que se midio en hardware: el carrusel avanzaba de escena (se veia el
  // mensaje "carrusel -> escena N" en el USB) pero `parciales=0` y `completos=2`: la
  // pantalla no cambiaba NUNCA. No se toca este orden sin entender esto.
  dibujaEscena();
  epdFlush();
}

#endif  // !HAS_OLED
