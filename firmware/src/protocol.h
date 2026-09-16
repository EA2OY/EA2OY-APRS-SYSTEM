// protocol.h — WebSerial/USB-CDC config protocol v1
// Line-based JSON over USB-CDC 115200 8N1, LF terminated.
// Unlike CA2RXU (fire-and-forget) this protocol ALWAYS replies (ACK + read-back).
// The same port also carries the CLI (lines not starting with '{') and the TNC
// bridge: a 0xC0 byte starts a binary KISS frame and is diverted to
// tncHandleUsbByte() before any text handling, so KISS, JSON and the CLI coexist
// and none of them can lock the operator out.
// Full spec: docs/protocol_config_v1.md
// License: GPL-3.0

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "aprs.h"
#include "config.h"
#include "radio.h"
#include "store.h"
#include "usb_lector.h"

/* ================= LATIDO DEL USB (diagnostico, 2026-09-13) =================
   PARA QUE: el operador reporta que "tras un rato, el nodo deja de escuchar el
   USB, como si se muriera, pero el nodo sigue vivo". No se sabe cuando ni por que.
   Aqui se apunta un LATIDO POR SEGUNDO EN RAM (NUNCA en la flash: escribir en el
   registro es sospechoso de ser LA CAUSA, asi que usarlo de diario falsearia la
   prueba). De cada segundo se guardan los bytes recibidos, el HUECO MAYOR del
   bucle (eso caza los atascos del NVMC) y si ese segundo se quedo sin nada.
   Se consulta POR RADIO (?USB?), porque si el USB se muere no se puede preguntar
   por el USB. */
// 300 muestras = 5 minutos de historia, que sobra para ver QUE paso. La muestra se
// guarda COMPACTA (campos ordenados de mayor a menor para que el compilador no meta
// relleno, y los bytes en 16 bits): asi son 3 bytes por muestra en vez de 8, porque
// el nodo tiene 243 KB de RAM y no se puede gastar 5 KB en un diagnostico.
constexpr int kUsbDiagN = 300;
struct UsbDiag {
  uint16_t bytes;        // bytes de ese segundo (tope 65535)
  uint8_t mayorHuecoMs;  // hueco mayor del bucle en ese segundo (tope 255)
  uint8_t muerto;        // 1 = ese segundo no llego nada
};
extern UsbDiag gUsbDiag[kUsbDiagN];
extern int gUsbDiagIdx;
extern int gUsbDiagTotal;
extern uint32_t gUsbUltimoAlSanoMs;
extern uint32_t gUsbBytes;
extern uint32_t gUsbLineas;
void usbDiagTick(uint32_t now);           // llamar desde el bucle
int usbDiagResumen(char *out, size_t n);  // texto para la consulta ?USB?

/* ★ RESUMEN DEL BOMBEO DEL USB (T-Echo Project Butter II, 2026-09-16): lineas atendidas,
   bytes leidos DENTRO de las esperas del driver de la tinta, maximo que ha llegado a estar
   encolado y comandos ejecutados dentro de un repintado (esto ultimo tiene que ser 0).
   Lo usa el comando de taller `usb` del CLI, que es una sola palabra de escribir. */
int usbBombeoResumen(char *out, size_t n);

class ConfigProtocol {
 public:
  explicit ConfigProtocol(DigiConfig &cfg) : cfg_(cfg) {
    // El lector nos devuelve cada linea completa. Se engancha una sola vez.
    lector_.init(&hookTnc, &hookLinea, this);
  }

  // ★★ LAS DOS PUERTAS DEL USB, Y LA DIFERENCIA ENTRE ELLAS ES TODO EL ASUNTO ★★
  //
  // `feed()` (BUCLE PRINCIPAL): lee el puerto Y EJECUTA lo que venga. Es la unica puerta que
  // ejecuta: quien manda una baliza, cambia la configuracion o reinicia el nodo es el bucle.
  //
  // `bombea()` (DRIVER DE LA TINTA): LEE Y ENCOLA, y nada mas. Es lo que llama el driver en
  // sus esperas de 1,5-3 s (gancho `displaySetPumpBoton`, ver main.cpp), para que el puerto
  // no se quede sin mirar durante el repintado. **No puede ejecutar un comando**: hacerlo
  // seria pintar dentro de un pintado o escribir la flash en medio de una transaccion con el
  // panel. Mismo patron que el buzon de gestos del boton (Project Butter).
  void feed(Stream &s);
  void bombea(Stream &s, bool enEsperaDePantalla = true);

  // ★ COBRA LO ENCOLADO (solo desde el bucle). Se llama justo despues de `displayRefresh()`
  //   para que un comando que llego mientras el panel pintaba se atienda en cuanto el panel
  //   queda libre, sin esperar a la vuelta siguiente del bucle.
  void atiende() { lector_.atiende(); }

  // ★ MARCA DE "ESTOY DENTRO DEL DRIVER DE LA PANTALLA" (red de seguridad, ver usb_lector.h).
  //   La pone el gancho de la pantalla y la quita el bucle en cuanto `displayRefresh()`
  //   vuelve. Si un comando se ejecutara con la marca puesta, `status.usb.dentro` lo diria.
  void marcaEnPantalla(bool dentro) { lector_.marcaEnPantalla(dentro); }

  // Contadores del bombeo, para el diagnostico (`status.usb`) y para el banco de pruebas.
  const UsbLector &lector() const { return lector_; }

 private:
  void handleLine(const char *line);
  void replyGet();
  void replyStatus();
  void replyOkWithConfig(bool persisted);
  void replyError(const char *err);
  void handleRadio(const JsonVariantConst &body);
  void handleDiag(const JsonVariantConst &body);
  void handleBeacon();
  void handleMessage(const JsonDocument &doc);
  void sendLine(const String &s);

  // Ganchos del lector (estaticos: el lector es C++ puro y no sabe de esta clase).
  static bool hookTnc(void *ctx, uint8_t b);
  static void hookLinea(void *ctx, const char *linea, size_t n);
  void lineaRecibida(const char *linea, size_t n);

  DigiConfig &cfg_;

  // ★ AQUI VIVIA `String lineBuf_` (y su tope kMaxLine). El acumulador de linea y el anillo
  //   de bytes estan ahora en `UsbLector`, que es el MISMO objeto que bombea el driver: asi
  //   el bucle y la pantalla comparten un solo acumulador y un solo sitio donde se parten
  //   las lineas (no hay dos parsers leyendo el mismo puerto, que seria el desastre).
  UsbLector lector_;
};

