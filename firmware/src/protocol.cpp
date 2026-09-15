// protocol.cpp — WebSerial/USB-CDC config protocol v1 implementation
// License: GPL-3.0

#include "protocol.h"

#include <ArduinoJson.h>
#include <RadioLib.h>
#include <nrf.h>  // NVIC_SystemReset() del reinicio suave (igual que cli.cpp)

#include "cli.h"
#include "diag.h"
#include "display.h"
#include "gps.h"
#include "sensors.h"
#include "tnc.h"

/* CONTADORES DEL USB (instrumentacion, 2026-09-13).
   PARA QUE: el operador reporta que "al cabo de un rato el nodo deja de
   escucharlo, como si el USB se muriera, pero el nodo sigue vivo". Para saber si
   es que NO LLEGAN BYTES (el CDC esta roto) o que llegan y no se contestan, se
   cuentan aqui. Se consultan en el JSON de estado: status.usb.bytes / .lineas. */
uint32_t gUsbBytes = 0;
uint32_t gUsbLineas = 0;

/* ================= LATIDO DEL USB (diagnostico, ver protocol.h) ==============
   Una muestra por segundo en RAM: bytes recibidos, hueco mayor del bucle y si el
   segundo se quedo sin nada. NO se escribe en la flash a proposito: escribir en el
   registro es sospechoso de ser LA CAUSA del fallo, asi que usarlo de diario
   falsearia la prueba. Se consulta por RADIO, porque si el USB muere no se puede
   preguntar por el USB. */
UsbDiag gUsbDiag[kUsbDiagN];
int gUsbDiagIdx = 0;
int gUsbDiagTotal = 0;
uint32_t gUsbUltimoAlSanoMs = 0;
static uint32_t sUltimaMuestraMs = 0;
static uint32_t sUltimosBytes = 0;
static uint32_t sUltimoPasoMs = 0;

void usbDiagTick(uint32_t now) {
  const uint32_t hueco = (sUltimoPasoMs == 0) ? 0 : (now - sUltimoPasoMs);
  sUltimoPasoMs = now;
  const int ult = (gUsbDiagIdx + kUsbDiagN - 1) % kUsbDiagN;
  if (sUltimaMuestraMs != 0 && (now - sUltimaMuestraMs) < 1000) {
    // Mismo segundo: solo se apunta el hueco MAYOR del bucle. Eso es lo que caza
    // un atasco del NVMC (un salto grande entre dos pasadas del bucle).
    if (gUsbDiagTotal > 0 && hueco < 250 && hueco > gUsbDiag[ult].mayorHuecoMs) {
      gUsbDiag[ult].mayorHuecoMs = (uint8_t)hueco;
    }
    return;
  }
  const uint32_t delta = gUsbBytes - sUltimosBytes;
  sUltimosBytes = gUsbBytes;
  gUsbDiag[gUsbDiagIdx].bytes = (uint16_t)(delta > 65535u ? 65535u : delta);
  gUsbDiag[gUsbDiagIdx].mayorHuecoMs = (uint8_t)(hueco < 255 ? hueco : 255);
  gUsbDiag[gUsbDiagIdx].muerto = (delta == 0);
  gUsbDiagIdx = (gUsbDiagIdx + 1) % kUsbDiagN;
  if (gUsbDiagTotal < kUsbDiagN) gUsbDiagTotal++;
  sUltimaMuestraMs = now;
  if (delta > 0) gUsbUltimoAlSanoMs = now;
}

/* Resumen para el ?USB? por radio: cuantos segundos seguidos sin recibir nada y el
   hueco mayor del bucle en los ultimos 10 minutos. */
int usbDiagResumen(char *out, size_t n) {
  int muertosSeguidos = 0;
  for (int i = gUsbDiagTotal - 1; i >= 0; i--) {
    const int k = (gUsbDiagIdx + kUsbDiagN - 1 - i) % kUsbDiagN;
    if (!gUsbDiag[k].muerto) break;
    muertosSeguidos++;
  }
  uint8_t peor = 0;
  for (int i = 0; i < gUsbDiagTotal; i++) {
    const int k = (gUsbDiagIdx + kUsbDiagN - 1 - i) % kUsbDiagN;
    if (gUsbDiag[k].mayorHuecoMs > peor) peor = gUsbDiag[k].mayorHuecoMs;
  }
  return snprintf(out, n, "USB %s bytes %lu lineas %lu | sordos %ds | bucle peor %ums",
                  APP_BUILD_NUM, (unsigned long)gUsbBytes, (unsigned long)gUsbLineas,
                  muertosSeguidos, (unsigned)peor);
}



// Version que se anuncia si nadie la define en la compilacion. La de verdad la
// pone platformio.ini (-DAPP_VERSION_STR): esto es solo el valor de socorro, y se
// mantiene igual para que no haya dos versiones distintas segun quien compile.
#ifndef APP_VERSION_STR
#define APP_VERSION_STR "1.0alpha"
#endif
// Numero de compilacion interno (lo pone platformio.ini). Valor de socorro por si
// alguien compila a mano: asi nunca falta el dato para saber que se grabo.
#ifndef APP_BUILD_NUM
#define APP_BUILD_NUM "b0"
#endif

void ConfigProtocol::feed(Stream &s) {
  // Abandon a KISS frame the host left half-sent (killed app, reset mid-write)
  // before looking at new bytes: otherwise it would eat every following line as
  // KISS payload and the operator would be locked out of their own node.
  tncUsbPoll();
  while (s.available()) {
    const uint8_t b = (uint8_t)s.read();
      if (gUsbBytes < 0xFFFFFFFFu) gUsbBytes++;   // instrumentacion USB
    // Byte-stream coexistence (operator requirement): 0xC0 is the KISS FEND, so
    // from that byte on everything goes to the KISS state machine until the
    // closing FEND; any other byte is text and follows the line paths below.
    // This is what lets a KISS app, the JSON protocol and the CLI share the same
    // USB port, and it means KISS can never lock the operator out of the node.
    if (tncHandleUsbByte(b)) {
      lineBuf_ = "";  // a half-typed text line dies with the binary frame
      continue;
    }
    const char c = (char)b;
    if (c == '\r') continue;               // tolerate CRLF
    if (c == '\n') {
      if (lineBuf_.length() > 0 && !tncHandleLine(lineBuf_)) {
        handleLine(lineBuf_);
          gUsbLineas++;
      }
      lineBuf_ = "";
      continue;
    }
    if (lineBuf_.length() < kMaxLine) lineBuf_ += c;
  }
}

void ConfigProtocol::sendLine(const String &s) { Serial.println(s); }

static void sendJsonDoc(JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  Serial.println(out);
}

void ConfigProtocol::replyOkWithConfig(bool persisted) {
  JsonDocument doc;
  doc["ok"] = true;
  configToJson(cfg_, doc["config"].to<JsonObject>());
  doc["persisted"] = persisted;
  sendJsonDoc(doc);
}

void ConfigProtocol::replyGet() { replyOkWithConfig(false); }

void ConfigProtocol::replyStatus() {
  JsonDocument doc;
  doc["ok"] = true;
  doc["status"]["version"] = APP_VERSION_STR;
  doc["status"]["build"] = APP_BUILD_NUM;   // numero de compilacion interno
  // Instrumentacion del USB: si al nodo "se le muere el USB", estos numeros dicen
  // si los bytes siguen llegando (el problema seria de respuestas) o si se han
  // parado (entonces el CDC esta roto).
  {
    JsonObject usb = doc["status"]["usb"].to<JsonObject>();
    usb["bytes"] = gUsbBytes;
    usb["lineas"] = gUsbLineas;
  }
  doc["status"]["uptimeMs"] = millis();
  JsonObject radio = doc["status"]["radio"].to<JsonObject>();
  radio["module"] = radioModuleName();  // "HT-RA62" / "E22P" (web: power limits)
  radio["state"] = radioState();
  radio["powerDbm"] = radioPowerDbm();  // what the radio is really set to
  radio["err"] = radioLastErr();
  radio["rxCount"] = radioRxCount();
  radio["txCount"] = radioTxCount();
  radio["digiCount"] = aprsDigiCount();
  radio["lastRssi"] = radioLastRssi();
  radio["lastSnr"] = radioLastSnr();
  radio["rxlog"] = radioRxLog();
  // NOTE: "txtest" (periodic raw-LoRa TX test) used to be reported here. It was
  // removed from the firmware on 2026-09-15: see radioSendFrame() in radio.cpp.
  radio["tnc"] = (int)tncProtocol();
  radio["kissPaused"] = tncKissPaused();   // override del operador (kissoff)

  JsonObject sensors = doc["status"]["sensors"].to<JsonObject>();
  sensors["wx"] = gSensorCache.wxOk;
  sensors["wxChip"] = sensorsWxName();  // which probe was detected
  sensors["ina"] = gSensorCache.inaOk;
  sensors["tempC"] = gSensorCache.tempC;
  sensors["hum"] = gSensorCache.hum;
  sensors["hPa"] = gSensorCache.pressHpa;
  sensors["vbatDivV"] = gSensorCache.vbatDivV;
  sensors["inaBusV"] = gSensorCache.inaBusV;
  sensors["inaMa"] = gSensorCache.inaCurrentMa;
  doc["status"]["display"] = displayPresent();
  doc["status"]["diag"] = diagStreaming();

  // GPS: the web configurator offers "use the GPS coordinates" for a fixed
  // digipeater and shows whether the module has a fix yet.
  const GpsData &g = gpsGet();
  JsonObject gps = doc["status"]["gps"].to<JsonObject>();
  gps["present"] = gpsPowered();
  gps["fix"] = g.fix;
  gps["sats"] = g.sats;
  gps["inView"] = g.satsInView;
  gps["hdop"] = g.hdop;
  gps["lat"] = g.lat;
  gps["lon"] = g.lon;
  gps["altM"] = g.altValid ? g.altM : 0.0f;
  gps["speedKmh"] = g.speedKmh;
  gps["courseDeg"] = g.courseDeg;
  gps["timeValid"] = (g.timeValid && g.dateValid);
  sendJsonDoc(doc);
}

void ConfigProtocol::handleRadio(const JsonVariantConst &body) {
  if (body["rxlog"].is<bool>()) radioSetRxLog(body["rxlog"] | false);
  // ★ EL COMANDO "txtest" YA NO EXISTE (2026-09-15). Encendia una emision
  //   periodica de "RADIOBLINK" por el camino CRUDO de la radio: sin prefijo
  //   LoRa, sin formato APRS y sin indicativo. Se elimino del firmware por
  //   decision del operador. Si llega la clave se IGNORA (no se contesta error,
  //   para no romper a un configurador viejo) y el estado de abajo ya no la
  //   informa: el campo desaparece de la respuesta y el panel deja de ofrecerlo.
  JsonDocument doc;
  doc["ok"] = true;
  JsonObject radio = doc["radio"].to<JsonObject>();
  radio["rxlog"] = radioRxLog();
  radio["muted"] = cfg_.txDisabled;
  sendJsonDoc(doc);
}

void ConfigProtocol::handleDiag(const JsonVariantConst &body) {
  if (body["active"].is<bool>()) diagSetActive(body["active"] | false);
  if (body["nmea"].is<bool>()) diagSetNmea(body["nmea"] | false);
  JsonDocument doc;
  doc["ok"] = true;
  doc["diag"]["active"] = diagActive();
  doc["diag"]["nmea"] = diagNmea();
  doc["diag"]["streaming"] = diagStreaming();
  sendJsonDoc(doc);
}

void ConfigProtocol::handleBeacon() {
  // Tracker modes with a live fix do not need configured coordinates: the beacon
  // carries the GPS position, exactly like the CLI verb and the OLED menu. The
  // gate stays for mode 0 and for the no-fix case.
  const bool trackerHasFix = (cfg_.mode != 0) && gpsPowered() && gpsGet().fix;
  if (!aprsCanBeacon(cfg_) && !trackerHasFix) {
    replyError("beacon not configured (callsign/position)");
    return;
  }
  int16_t st = aprsSendManualBeacon(cfg_);
  JsonDocument doc;
  doc["ok"] = true;
  JsonObject beacon = doc["beacon"].to<JsonObject>();
  beacon["tx"] = (st == RADIOLIB_ERR_NONE);
  beacon["code"] = st;
  sendJsonDoc(doc);
}

// APRS message started from the web panel: {"cmd":"msg","to":"EA2XXX-7",
// "text":"hola"}. Without "to" the node writes to the last heard station.
void ConfigProtocol::handleMessage(const JsonDocument &doc) {
  String to = doc["to"] | "";
  String text = doc["text"] | "";
  to.trim();
  text.trim();
  if (to.length() == 0) to = aprsLastHeardCall();
  if (to.length() == 0) {
    replyError("no station heard yet");
    return;
  }
  if (text.length() == 0) {
    replyError("empty message");
    return;
  }
  int16_t st = aprsSendMessage(cfg_, to.c_str(), text.c_str());
  JsonDocument out;
  const bool ok = (st == RADIOLIB_ERR_NONE);
  out["ok"] = ok;
  JsonObject m = out["msg"].to<JsonObject>();
  m["to"] = to;
  m["text"] = text;
  m["tx"] = ok;
  m["code"] = st;
  if (!ok) out["error"] = "message not sent";
  sendJsonDoc(out);
}

void ConfigProtocol::replyError(const char *err) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = err;
  sendJsonDoc(doc);
}

void ConfigProtocol::handleLine(const String &line) {
  if (line.length() == 0) return;

  // Hybrid (operator decision): a line that does NOT start with '{' is a
  // NavaCLI-style token command -> human-readable text reply (cli.cpp).
  if (line[0] != '{') {
    String t = cliExecute(cfg_, line.c_str(), false);
    if (t.length() > 0) {
      Serial.print(t);
      Serial.println();
    }
    return;
  }

  JsonDocument doc;
  DeserializationError derr = deserializeJson(doc, line);
  if (derr) {
    replyError("bad json");
    return;
  }
  const char *cmd = doc["cmd"] | "";
  if (strcmp(cmd, "get") == 0) {
    replyGet();
  } else if (strcmp(cmd, "status") == 0) {
    replyStatus();
  } else if (strcmp(cmd, "reboot") == 0 || strcmp(cmd, "reset") == 0) {
    // Reinicio SUAVE, sin tocar la configuracion. "reset" es alias a proposito:
    // el operador lo mando creyendo que reiniciaba el nodo y perdio su
    // configuracion, asi que "reset" ya NO puede borrar nada. El borrado de
    // fabrica vive en "factory_reset" (siguiente rama). Mismo patron que el
    // verbo "reboot confirm" del CLI: avisar, vaciar el serial y resetear.
    Serial.println("REBOOT");
    Serial.flush();
    delay(100);
    NVIC_SystemReset();
  } else if (strcmp(cmd, "factory_reset") == 0) {
    // ★ FACTORY RESET CONSERVANDO EL ACCESO (2026-09-15, decision del operador).
    // POR QUE: esta rama borraba ABSOLUTAMENTE TODO, incluido el indicativo. El
    // aparato se quedaba sin identidad y sin forma de entrar: habia que empezar
    // de cero desde un ordenador, con el cable. El "factory_reset confirm" de la
    // consola (cli.cpp) ya lo hacia bien desde el principio, asi que aqui se
    // REUTILIZA ESE MISMO PATRON: se guardan las tres cosas que dan acceso,
    // se restaura la configuracion de fabrica y se devuelven las tres.
    //   - callsign    : identidad de la estacion (sin el, el nodo no transmite)
    //   - managers    : lista de operadores autorizados (ACL del control remoto)
    //   - remoteEnabled: si el control remoto esta aceptado
    // Lo demas SI se borra: es un reset de fabrica de verdad.
    // OJO: esto NO es el `wipe` de la consola (cli.cpp), que a proposito borra
    // TODO, incluido el indicativo, y ademas exige `wipe confirm` y ser USB.
    // NO "ARREGLAR" ESTO EN SENTIDO CONTRARIO: no es un descuido, es la norma.
    char keepCall[16], keepMgrs[64];
    bool keepRemote = cfg_.remoteEnabled;
    strncpy(keepCall, cfg_.callsign, sizeof(keepCall) - 1);
    keepCall[sizeof(keepCall) - 1] = 0;
    strncpy(keepMgrs, cfg_.managers, sizeof(keepMgrs) - 1);
    keepMgrs[sizeof(keepMgrs) - 1] = 0;
    configSetDefaults(cfg_);
    strncpy(cfg_.callsign, keepCall, sizeof(cfg_.callsign) - 1);
    cfg_.callsign[sizeof(cfg_.callsign) - 1] = 0;
    strncpy(cfg_.managers, keepMgrs, sizeof(cfg_.managers) - 1);
    cfg_.managers[sizeof(cfg_.managers) - 1] = 0;
    cfg_.remoteEnabled = keepRemote;
    bool p = storeSave(cfg_);
    radioSetMuted(cfg_.txDisabled);
    radioApplyPower(cfg_.powerDbm);
    replyOkWithConfig(p);
  } else if (strcmp(cmd, "radio") == 0) {
    handleRadio(doc["radio"].as<JsonObjectConst>());
  } else if (strcmp(cmd, "diag") == 0) {
    handleDiag(doc["diag"].as<JsonObjectConst>());
  } else if (strcmp(cmd, "beacon") == 0) {
    handleBeacon();
  } else if (strcmp(cmd, "msg") == 0) {
    handleMessage(doc);
  } else if (strcmp(cmd, "set") == 0) {
    if (!doc["config"].is<JsonObject>()) {
      replyError("missing config object");
      return;
    }
    String err;
    if (!configFromJson(cfg_, doc["config"].as<JsonObjectConst>(), err)) {
      replyError(err.c_str());
      return;
    }
    bool p = storeSave(cfg_);  // atomic save to internal flash (store.h)
    radioSetMuted(cfg_.txDisabled);
    radioApplyPower(cfg_.powerDbm);
    replyOkWithConfig(p);
  } else {
    replyError("unknown cmd");
  }
}
