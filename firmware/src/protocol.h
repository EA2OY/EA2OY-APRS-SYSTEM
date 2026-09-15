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

class ConfigProtocol {
 public:
  explicit ConfigProtocol(DigiConfig &cfg) : cfg_(cfg) {}

  // Feed serial bytes; processes complete LF-terminated lines.
  void feed(Stream &s);

 private:
  void handleLine(const String &line);
  void replyGet();
  void replyStatus();
  void replyOkWithConfig(bool persisted);
  void replyError(const char *err);
  void handleRadio(const JsonVariantConst &body);
  void handleDiag(const JsonVariantConst &body);
  void handleBeacon();
  void handleMessage(const JsonDocument &doc);
  void sendLine(const String &s);

  DigiConfig &cfg_;
  String lineBuf_;
  static constexpr size_t kMaxLine = 1024;
};
