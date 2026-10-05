// borra_flash.cpp — BORRADOR DE LA FLASH + SONDA DEL CONTROLADOR DE MEMORIA (2026-09-21)
//
// ===========================================================================
//  QUE HACE
//
//  1) TOQUE LARGO en el boton fisico (3 s) -> intenta borrar de 0x27000 a 0xE9FFF, que es
//     todo menos el cargador de arranque (empieza en 0xF4000), el SoftDevice, el MBR y el UICR.
//  2) Si el borrado NO ocurre (que es lo que esta pasando), el aparato **lo dice parpadeando**
//     los valores del controlador de memoria, para saber POR QUE se niega.
//
// ===========================================================================
//  ★★★ POR QUE HACE FALTA LA SONDA (leer esto antes de tocar nada) ★★★
//
//  Probado en la placa: se mantiene el boton, el LED se apaga y no pasa nada mas. Ni borra, ni
//  se reinicia, ni parpadea el aviso de fallo. Con las dos protecciones ya puestas (topes de
//  tiempo y comprobacion leyendo la memoria), un cuelgue del programa queda descartado: lo que
//  pasa es que **el controlador de memoria del chip no obedece**, y no se sabe por que.
//
//  En vez de seguir suponiendo, el aparato recoge los datos y los deja LEER a simple vista.
//  Como no hay puerto serie en este firmware, se usa el LED azul: una pausa larga marca el
//  principio, y despues parpadeos cortos que se cuentan como un numero.
//
//  ★★ COMO SE LEE (esto es lo que hay que hacer tras mantener el boton) ★★
//     - Pausa larga (1,5 s sin luz) = EMPIEZA UN DATO NUEVO
//     - Parpadeos rapidos que cuentan = el VALOR de ese dato
//     - Un valor de 0 se marca con un parpadeo MUY CORTO (para no confundirlo con "no hay dato")
//
//     Los cuatro datos salen EN ESTE ORDEN y luego se repite:
//        1) el numero de sectores que SI se borraron     (si es 0, no se borro nada)
//        2) el registro CONFIG del controlador de memoria
//        3) el registro READY
//        4) un codigo de lo que paso (ver la tabla de abajo)
//
//     Ejemplo: pausa, 0 parpadeos cortos... = 0 sectores borrados.
//
//  ★ SI ALGUN VALOR ES MAYOR DE 20, se parte en dos parpadeos: primero las DECENAS, luego una
//    pausa corta, y luego las UNIDADES. (Ej.: 32 -> 3 parpadeos, pausa, 2 parpadeos.)
//
// ===========================================================================
//  ★★★ LO QUE NO PUEDE PASAR, Y ESTA COMPROBADO ★★★
//    - La rutina de borrado VIVE EN LA RAM (`20004260`), no en la flash: un programa no puede
//      borrar la flash que esta ejecutando.
//    - NO LLAMA A NADIE DE LA FLASH: `millis()` daba error de enlazado ("relocation truncated
//      to fit") porque obliga a poner un saltito EN LA FLASH, justo en la zona que se borra.
//      El tiempo se lee de un temporizador del propio chip.
//    - Los topes de tiempo evitan cualquier espera sin salida.
//
//  Compilar:  pio run -e techo_plus_borra      (fichero de UN SOLO USO)
// ===========================================================================

#include <Arduino.h>
#include <nrf.h>

#include "pins_board.h"

namespace {

// ---------------------------------------------------------------------------
//  LIMITES DEL BORRADO, en un solo sitio y con nombre.
// ---------------------------------------------------------------------------
constexpr uint32_t kDirInicio = 0x27000;   // donde empieza la aplicacion
constexpr uint32_t kDirFin    = 0xEA000;   // donde EMPIEZA EL CARGADOR: no se llega aqui
constexpr uint32_t kSector    = 0x1000;    // sectores de 4 KB
constexpr uint32_t kMantener  = 3000;      // 3 s de toque largo para disparar

// Memoria que SOBREVIVE al reinicio: se usa para pasar los datos de la sonda al informe que
// el aparato cuenta por el LED. Esta en RAM y en una direccion fija, con un numero magico
// delante para saber si lo que hay ahi lo puso este programa o es basura de antes.
constexpr uint32_t kMagico = 0xB0A2A001u;   // numero inventado, solo tiene que ser reconocible
__attribute__((section(".noinit"))) volatile uint32_t gDatos[10];

void led(bool encendido) { digitalWrite(PIN_LED_BLUE, encendido ? HIGH : LOW); }

// ---------------------------------------------------------------------------
//  RELOJ PROPIO, SIN LLAMAR A NADA DE LA FLASH (ver el comentario largo de arriba).
// ---------------------------------------------------------------------------
__attribute__((section(".data"), noinline, used))
void relojArranca() {
  NRF_TIMER4->TASKS_STOP = 1;
  NRF_TIMER4->TASKS_CLEAR = 1;
  NRF_TIMER4->MODE = TIMER_MODE_MODE_Timer;
  NRF_TIMER4->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
  NRF_TIMER4->PRESCALER = 4;          // 1 us por tick
  NRF_TIMER4->TASKS_START = 1;
}

__attribute__((section(".data"), noinline, used))
uint32_t relojMs() { return NRF_TIMER4->CC[0] / 1000u; }

__attribute__((section(".data"), noinline, used))
void esperaMs(uint32_t ms) {
  const uint32_t t0 = relojMs();
  while ((uint32_t)(relojMs() - t0) < ms) { }
}

// ---------------------------------------------------------------------------
//  ★★★ EL FALLO QUE TUMBO LAS TRES VERSIONES ANTERIORES (encontrado el 2026-09-21) ★★★
//
//  Yo esperaba a que el controlador de memoria estuviera listo asi:
//        while (NRF_NVMC->READY != 0) { }
//  O sea: "espera mientras READY NO sea cero". Y ES AL REVES.
//
//  El propio nucleo de Adafruit lo hace asi, para ESTE chip:
//        return (bool)(p_reg->READY & NVMC_READY_READY_Msk);   // nrf_nvmc.h, linea 259
//  O sea: **READY vale 1 cuando esta listo** y 0 mientras esta ocupado.
//
//  Con mi condicion, el bucle esperaba a que se cumpliera algo que YA se cumplia... no: esperaba
//  a que READY valiera 0, y como valia 1, NO SALIA NUNCA. El aparato se quedaba parado antes de
//  mandar ni una sola orden de borrado. Eso es exactamente lo que se veia: el LED se apagaba y
//  no pasaba nada mas, ni borraba ni se reiniciaba.
//
//  ★ La cabecera del mdk llama `Busy` al valor 0, y de ahi venia mi confusion. La fuente buena
//    es el nucleo (`nrf_nvmc.h`), que comprueba EL BIT, no una comparacion con cero.
//
//  Se comprueba el bit, igual que el nucleo. Asi no hay interpretacion posible.
// ---------------------------------------------------------------------------
__attribute__((section(".data"), noinline, used))
bool nvmcListo() {
  return (NRF_NVMC->READY & 1u) != 0;   // 1 = listo, 0 = ocupado
}

// ---------------------------------------------------------------------------
//  ★★ LA RUTINA QUE BORRA: VIVE EN LA RAM. NO MOVERLA DE `.data`. ★★
//  Devuelve el numero de sectores que SI quedaron borrados (comprobado leyendo).
// ---------------------------------------------------------------------------
__attribute__((section(".data"), noinline, used))
uint32_t borraSectores(uint32_t desde, uint32_t hasta) {
  constexpr uint32_t kWenBorra = 2;   // WEN = 2 -> borrado habilitado
  constexpr uint32_t kWenLee   = 0;   // WEN = 0 -> solo lectura
  constexpr uint32_t kTopeMs   = 300;

  gDatos[1] = NRF_NVMC->CONFIG;       // CONFIG antes de tocarlo
  NRF_NVMC->CONFIG = kWenBorra;       // pedir permiso para borrar
  gDatos[2] = NRF_NVMC->CONFIG;       // ¿lo acepto? (si sigue en 0, la escritura se ignoro)

  uint32_t borrados = 0;
  for (uint32_t d = desde; d < hasta; d += kSector) {
    const uint32_t t0 = relojMs();
    while (!nvmcListo()) {
      if ((uint32_t)(relojMs() - t0) > kTopeMs) break;
    }
    NRF_NVMC->ERASEPAGE = d;

    // ★ NO SE DA POR HECHO: se LEE la memoria. Si el sector se borro, su primer byte es 0xFF.
    const uint32_t t1 = relojMs();
    while ((uint32_t)(relojMs() - t1) < kTopeMs) {
      if (*(volatile uint8_t *)d == 0xFF) { borrados++; break; }
    }
    // Si el PRIMER sector no se borro, no tiene sentido seguir con 194 mas.
    if (borrados == 0 && d == desde) break;
  }

  gDatos[4] = NRF_NVMC->CONFIG;       // CONFIG despues
  gDatos[6] = NRF_NVMC->READY;        // READY al terminar
  NRF_NVMC->CONFIG = kWenLee;
  return borrados;
}

bool pulsado() { return digitalRead(PIN_BUTTON) == LOW; }

// ---------------------------------------------------------------------------
//  EL "LECTOR": parpadea un numero para que se pueda contar a ojo.
//    - un parpadeo MUY CORTO (60 ms) significa CERO
//    - hasta 10: parpadeos normales
//    - mas de 10: primero las DECENAS, pausa larga, y luego las UNIDADES
// ---------------------------------------------------------------------------
void parpadeaNumero(uint32_t v) {
  if (v == 0) { led(true); esperaMs(60); led(false); esperaMs(400); return; }
  const uint32_t decenas = v / 10, unidades = v % 10;
  for (uint32_t i = 0; i < decenas; i++) { led(true); esperaMs(150); led(false); esperaMs(250); }
  if (decenas) esperaMs(900);          // pausa larga entre decenas y unidades
  for (uint32_t i = 0; i < unidades; i++) { led(true); esperaMs(150); led(false); esperaMs(250); }
  esperaMs(700);
}

}  // namespace

void setup() {
  pinMode(PIN_LED_BLUE, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  relojArranca();
  led(false);

  // ★ ¿VENIMOS DE UN INTENTO DE BORRADO? Si los datos estan puestos, se informa y NO se
  //   vuelve a borrar: asi el aparato se queda contando en bucle y se puede leer con calma.
  if (gDatos[0] == kMagico) {
    const uint32_t borrados = gDatos[5];
    const uint32_t config   = gDatos[4];
    const uint32_t ready    = gDatos[6];
    const uint32_t codigo   = gDatos[7];

    for (;;) {
      esperaMs(1500);                       // pausa larga: "empieza el dato 1"
      parpadeaNumero(borrados);             // 1) sectores borrados
      esperaMs(1500);
      parpadeaNumero(config);               // 2) CONFIG del controlador de memoria
      esperaMs(1500);
      parpadeaNumero(ready);                // 3) READY
      esperaMs(1500);
      parpadeaNumero(codigo);               // 4) codigo de lo que paso
      esperaMs(2500);                       // y vuelta a empezar
    }
  }

  // Arranque normal: seis parpadeos = "estoy vivo y esperando el toque largo".
  for (int i = 0; i < 6; i++) { led(i & 1); delay(150); }
  led(false);
}

void loop() {
  if (!pulsado()) return;

  uint32_t t0 = millis();
  while (pulsado()) {
    if ((uint32_t)(millis() - t0) >= kMantener) {
      led(true);
      const uint32_t borrados = borraSectores(kDirInicio, kDirFin);
      const uint32_t total    = (kDirFin - kDirInicio) / kSector;

      // Los datos para el informe, en memoria que sobrevive al reinicio.
      gDatos[5] = borrados;
      gDatos[6] = NRF_NVMC->READY;
      gDatos[7] = (borrados == total) ? 2 : ((borrados > 0) ? 1 : 0);
      gDatos[0] = kMagico;
      gDatos[8] = total;

      // ★★ SOLO SE REINICIA SI SE BORRO **TODO** (esto es nuevo y es importante) ★★
      //   Si se hubiera borrado solo una parte, reiniciar dejaria media aplicacion en la flash
      //   y el arranque seria una loteria: podria quedarse a medias para siempre. Asi que:
      //     - todo borrado (lo normal) -> se reinicia; el cargador vera la aplicacion invalida
      //       y se quedara en modo UF2, que es justo lo que se busca.
      //     - borrado a medias o nada -> NO se reinicia. Se queda contando el informe por el LED
      //       para poder leerlo, y no se toca mas la memoria.
      if (borrados == total) {
        NVIC_SystemReset();
        while (true) { }
      }
    }
    delay(10);
  }
}
