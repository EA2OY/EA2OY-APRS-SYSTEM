// hello_pad.cpp — APARATO DE MEDIDA DE LA PASTILLA TACTIL (2026-09-21)
//
// ===========================================================================
//  PARA QUE EXISTE ESTE FICHERO
//
//  El operador reporta: "el boton capacitivo no capta la pulsacion, ni se enciende la luz".
//  Y ademas, probado a fondo: con el firmware aleman, con el nuestro y con el de fabrica
//  ocurre lo mismo, DESPUES de reescribir el cargador y de hacer un erase completo. O sea que
//  el problema NO esta en la aplicacion.
//
//  Este firmware no intenta arreglar nada. MIDE. Pinta en la pantalla, en vivo, lo que el
//  chip ve de verdad en P0.11:
//
//     - el NIVEL logico con la resistencia interna de subida puesta (como lo lee el firmware)
//     - el NIVEL logico con la patilla SUELTA (sin ninguna resistencia interna)
//     - el numero de FLANCOS que se han visto (con interrupcion, como en el firmware bueno)
//     - el estado de los pines de ALIMENTACION (P0.12 MOSFET de periferia, P0.13 regulador)
//
//  CON ESO SE DISTINGUE, SIN CONJETURAS:
//
//    (a) Si con la patilla SUELTA la lectura se va a 0 y a 1 sola -> la pastilla y su
//        electronica estan VIVAS y hay senal. El problema estaria en como la leemos.
//    (b) Si con la patilla SUELTA no se mueve NUNCA (se queda clavada en 1 o en 0) ->
//        no hay nada conectado que tire de ella: la pastilla no esta dando senal.
//    (c) Si PWR_EN (P0.12) no esta en 1 -> el rail de periferia esta apagado, y de ese rail
//        cuelga la resistencia de subida de la pastilla (esta escrito en pins_techo.h).
//
//  ★ NO LLEVA RADIO, NI GPS, NI SENSORES, NI CONFIGURACION, NI FLASH. Solo pantalla y el pin.
//    Es el firmware mas pequeno que puede contestar la pregunta.
//
//  COMPILAR:  pio run -e techo_plus_pad
// ===========================================================================

#include <Arduino.h>
#include <nrf.h>

#include "display.h"
#include "pins_board.h"

namespace {

// La pastilla: la misma patilla que usa el firmware bueno.
constexpr int kPinPastilla = PIN_BTN_TOUCH;   // P0.11

volatile uint32_t gFlancos = 0;               // flancos vistos por interrupcion

void isrPastilla() {
  // Solo contar. Ni `millis()` ni nada que pueda tardar: dentro de una interrupcion.
  gFlancos++;
}

// Lectura del pin en un modo de resistencia concreto, SIN dejar rastro:
// se configura, se esperan 2 ms y se lee.
int leeCon(int pull) {
  nrf_gpio_cfg_input(kPinPastilla, (nrf_gpio_pin_pull_t)pull);
  delay(2);
  return (nrf_gpio_pin_read(kPinPastilla) != 0) ? 1 : 0;
}

}  // namespace

void setup() {
  Serial.begin(115200);

  // La pantalla, con el driver de verdad del proyecto (el mismo del firmware bueno).
  // ★ Se usa `displayArrancaPantalla()` y no `displayInitTrasRadio()`: el segundo espera que
  //   la radio haya configurado antes el periferico SPI, y aqui NO HAY RADIO (es un firmware
  //   de medida, lo mas pequeno posible). El primero arranca el panel por si mismo, que es
  //   justo lo que hace falta.
  displayArrancaPantalla();
  delay(300);

  pinMode(PIN_BTN_TOUCH, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_BTN_TOUCH), isrPastilla, CHANGE);
}

void loop() {
  static char texto[200];
  static int ultimoP = -1, ultimoN = -1;
  static uint32_t ultimoPintado = 0;
  static uint32_t ultimosFlancos = 0;
  static bool primera = true;

  // Las tres lecturas. La del medio (SIN resistencia) es la que de verdad informa: si algo
  // tira de la patilla, se ve; si no hay nada, se queda en el nivel que le deje el aire.
  const int conPullUp = leeCon(NRF_GPIO_PIN_PULLUP);
  const int suelta    = leeCon(NRF_GPIO_PIN_NOPULL);
  const int conPullDn = leeCon(NRF_GPIO_PIN_PULLDOWN);

  // Y se deja como la lee el firmware bueno, para que la interrupcion cuente lo mismo.
  nrf_gpio_cfg_input(kPinPastilla, NRF_GPIO_PIN_PULLUP);

  const uint32_t flancos = gFlancos;

  // Solo se repinta cuando cambia algo de lo que importa, y como mucho cada 4 s: la tinta
  // tarda ~2 s en un refresco y no tiene sentido martillearla.
  const bool cambio = (conPullUp != ultimoP) || (suelta != ultimoN) ||
                      (flancos != ultimosFlancos);
  if (primera || cambio || (millis() - ultimoPintado) > 4000) {
    primera = false;
    ultimoP = conPullUp; ultimoN = suelta; ultimosFlancos = flancos;
    ultimoPintado = millis();

    snprintf(texto, sizeof(texto),
             "P0.11 subida=%d suelta=%d bajada=%d flancos=%lu",
             conPullUp, suelta, conPullDn, (unsigned long)flancos);
    // ★ `epdBancoAvisoYRefresca()` hace las dos cosas: dibuja el aviso Y lo manda al panel.
    //   En el firmware de verdad el envio lo hace `displayRefresh()` desde el bucle; aqui no
    //   hay bucle de verdad, asi que se usa la puerta que el driver expone para los bancos.
    epdBancoAvisoYRefresca(texto);
    // Y por el cable, para quien lo tenga conectado. Sin comandos: sale solo.
    Serial.printf("PASTILLA: subida=%d suelta=%d bajada=%d flancos=%lu PWR_EN=%d REG_EN=%d\r\n",
                  conPullUp, suelta, conPullDn, (unsigned long)flancos,
                  (int)digitalRead(PIN_PWR_EN), (int)digitalRead(PIN_3V3_EN));
  }

  delay(30);
}
