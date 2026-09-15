// cli.cpp - NavaCLI-style token commands (USB + RF remote, shared engine).
// Mechanics ported from NavaTastic NavaCLIModule.cpp concepts: grouped help,
// "<cmd> ?" = usage + current state, typed `set <key> <val>` (validated and
// persisted), confirm gate for destructive commands. License: GPL-3.0

#include "cli.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <RadioLib.h>
#include <nrf.h>
#include <string.h>

#include "aprs.h"
#include "diag.h"
#include "display.h"
#include "flog.h"
#include "gps.h"
#include "haptic.h"
#include "pins_board.h"
#include "power.h"
#include "radio.h"
#include "sensors.h"
#include "store.h"
#include "tnc.h"
#include "tracker.h"

namespace {

void cmdPing(const DigiConfig &cfg, String &out) {
  char b[180];
  snprintf(b, sizeof(b),
           "PONG v%s %s | %s | Bat: %.2fV | UP: %lus | RX:%lu TX:%lu DG:%lu",
           APP_VERSION_STR, APP_BUILD_NUM, cfg.callsign, sensorsBatteryVolt(gSensorCache),
           (unsigned long)(millis() / 1000), (unsigned long)radioRxCount(),
           (unsigned long)radioTxCount(), (unsigned long)aprsDigiCount());
  out = b;
}

void cmdStatus(const DigiConfig &cfg, String &out) {
  // El numero de compilacion va aqui: consultando el nodo se sabe QUE firmware
  // lleva, sin depender de que las versiones se llamen distinto.
  out = APP_VERSION_STR;
  out += " ";
  out += APP_BUILD_NUM;
  out += " ";
  out += cfg.callsign;
  out += " D";
  out += String((int)cfg.digiMode);
  out += " M";
  out += String((int)cfg.mode);
  out += " GPS:";
  out += gpsGet().fix ? "fix" : "no";
  out += " RX:";
  out += String((unsigned long)radioRxCount());
  out += " TX:";
  out += String((unsigned long)radioTxCount());
  out += " DG:";
  out += String((unsigned long)aprsDigiCount());
  out += " Bat:";
  float bv = sensorsBatteryVolt(gSensorCache);
  if (bv > 0.0f) out += String(bv, 2);
  else out += "--";
  if (gSensorCache.inaOk) {
    out += "V I:";
    out += (gSensorCache.inaCurrentMa < 0.0f ? "-" : "+");
    out += String((long)(fabsf(gSensorCache.inaCurrentMa) + 0.5f));
    out += "mA";
  }
  if (gSensorCache.wxOk) {
    out += " T:";
    out += String(gSensorCache.tempC, 1);
    out += " P:";
    out += String(gSensorCache.pressHpa, 0);
  }
  out += cfg.txDisabled ? " MUTED" : " LIVE";
}

void cmdBat(const DigiConfig &cfg, String &out) {
  char b[120];
  snprintf(b, sizeof(b),
           "Bat: %.2fV | INA: %.2fV %+.0fmA | cut %d mV wake %d mV (%s)",
           sensorsBatteryVolt(gSensorCache), gSensorCache.inaBusV,
           gSensorCache.inaCurrentMa, cfg.sleepCutMv, cfg.sleepWakeMv,
           powerUsbPresent() ? "USB" : "battery");
  out = b;
}

void cmdResetReason(String &out) {
  uint32_t r = NRF_POWER->RESETREAS;
  char b[120];
  const char *why = "?";
  if (r & POWER_RESETREAS_OFF_Msk) why = "systemoff";
  else if (r & POWER_RESETREAS_LPCOMP_Msk) why = "lpcomp";
  else if (r & POWER_RESETREAS_DOG_Msk) why = "watchdog";
  else if (r & POWER_RESETREAS_RESETPIN_Msk) why = "pin";
  else if (r & POWER_RESETREAS_SREQ_Msk) why = "soft-request";
  else if (r & POWER_RESETREAS_VBUS_Msk) why = "usb";
  snprintf(b, sizeof(b), "RESETREAS 0x%X (%s)", (unsigned)r, why);
  out = b;
}

void cmdRxLog(String &out) {
  char b[140];
  snprintf(b, sizeof(b),
           "RX:%lu TX:%lu DG:%lu | LAST: %s rssi %.0f snr %.1f ferr %.0f crc %lu",
           (unsigned long)radioRxCount(), (unsigned long)radioTxCount(),
           (unsigned long)aprsDigiCount(), aprsLastFrom(), radioLastRssi(),
           radioLastSnr(), (double)radioLastFreqErr(),
           (unsigned long)radioCrcErrCount());
  out = b;
}

bool typedSet(DigiConfig &cfg, const String &key, const String &val,
              String &errOut) {
  JsonDocument doc;
  // Compatibilidad: "tncMode" es la clave vieja (booleana) del puente TNC y ya no
  // existe en la configuracion; su sustituta es "tncProtocol" (0/1/2). Un script
  // o el operador que escriba "set tncMode 1" sigue funcionando.
  const String kKey = (key == "tncMode") ? String("tncProtocol") : key;
  const char *k = kKey.c_str();

  // Reject unknown keys: a typo must not silently report OK.
  JsonDocument cur;
  configToJson(cfg, cur.to<JsonObject>());
  if (cur[k].isNull()) {
    errOut = "unknown key";
    return false;
  }

  // The TYPE comes from the current configuration, not from a hand-kept list:
  // that list was the source of silent failures (a new text key was treated as
  // a number, the merge ignored it and the CLI still answered OK - seen with
  // the path keys and value "0", and before with the booleans).
  if (cur[k].is<bool>()) {
    // Boolean key: accept 1/0 and the friendly on/off, si/no, true/false. Any
    // other text is an error.
    String v = val;
    v.toLowerCase();
    bool b;
    if (v == "1" || v == "on" || v == "si" || v == "true" || v == "yes") {
      b = true;
    } else if (v == "0" || v == "off" || v == "no" || v == "false") {
      b = false;
    } else {
      errOut = "usa 1/0 (on/off, si/no)";
      return false;
    }
    doc["config"][k] = b;
  } else if (cur[k].is<const char *>()) {
    doc["config"][k] = val.c_str();  // text key: se guarda tal cual
  } else if (val == "true" || val == "false") {
    doc["config"][k] = (val == "true");
  } else if (val.indexOf('.') >= 0) {
    doc["config"][k] = val.toFloat();
  } else {
    long iv = val.toInt();
    if (val.length() > 0 && String(iv) == val) {
      doc["config"][k] = (int32_t)iv;
    } else {
      doc["config"][k] = val.c_str();
    }
  }
  return configFromJson(cfg, doc["config"].as<JsonObjectConst>(), errOut);
}

String helpText(bool viaRemote) {
  if (viaRemote) {
    return "CMDS: ping status bat rxlog | set <k> <v> | mute unmute beacon | "
           "reboot/reset confirm (reinicio) | factory_reset confirm (fabrica) | help";
  }
  return "CMDS:\nping | status | bat | reset_reason | rxlog | diag\n"
         "set <clave> <valor> | mute | unmute | beacon | wx | trkbeacon\n"
         "msg [destino texto] | bul [0-9] <texto> | obj <nombre> <lat> <lon> [texto]\n"
         "objkill <nombre> | q <consulta APRS> | telemetry [meta]\n"
         "rxs <de> <para> <texto> (simula una recibida, solo USB)\n"
         "battest <mV> (aviso de bateria baja, solo USB)\n"
         "i2cscan (escaneo del bus I2C, solo USB)\n"
         "vibra [0..123] (motor haptico del Plus, solo USB)\n"
         "tono (melodia de bateria baja, solo USB)\n"
         "log dump | log stats | log clear | log on|off | gps info|send|coldstart\n"
         "reboot confirm | reset confirm (reinicio suave, NO borra nada)\n"
         "factory_reset confirm (borra la config; conserva callsign+managers+remote)\n"
         "wipe confirm (USB only) | dfu confirm\n"
         "help [comando|clave] | <cmd> ?\n"
         "Claves: ver docs/protocol_config_v1.md (cualquier clave de config)";
}

}  // namespace

// Envoltorio publ del motor interno para que el menu de la tinta electronica pueda
// validar/guardar un valor por clave con el mismo criterio que `set <clave> <valor>`.
bool cliTypedSet(DigiConfig &cfg, const String &key, const String &val, String &errOut) {
  return typedSet(cfg, key, val, errOut);
}

String cliExecute(DigiConfig &cfg, const char *line, bool viaRemote) {
  String out;

  String cmd = line;
  cmd.trim();
  if (cmd.length() == 0) return "ERR: vacio (help)";

  int sp = cmd.indexOf(' ');
  String verb = (sp < 0) ? cmd : cmd.substring(0, sp);
  String verbKey = verb;  // original case: config keys are case-sensitive
  String rest = (sp < 0) ? "" : cmd.substring(sp + 1);
  rest.trim();
  verb.toUpperCase();

  // "<cmd> ?" -> usage, or current value when the verb is a config key
  if (rest == "?" || rest == "HELP") {
    if (verb == "HELP") return helpText(viaRemote);
    if (verb == "SET") return "set <clave> <valor> | ejemplo: set digiMode 1";
    if (verb == "MUTE") return "mute | apaga toda TX (RX sigue)";
    if (verb == "UNMUTE") return "unmute | reactiva TX";
    if (verb == "BEACON") return "beacon | baliza APRS inmediata";
    if (verb == "REBOOT") return "reboot confirm | reinicia el nodo";
    if (verb == "RESET")
      return "reset confirm | reinicia el nodo (suave: NO borra la configuracion)";
    if (verb == "FACTORY_RESET")
      return "factory_reset confirm | fabrica conservando callsign+managers+remote";
    if (verb == "WIPE")
      return "wipe confirm | borrado total (solo USB)";
    if (verb == "DIAG")
      return "diag on|off|nmea on|nmea off|? | diagnostico JSON por USB";
    // config key? show current value
    JsonDocument doc;
    configToJson(cfg, doc.to<JsonObject>());
    bool found = false;
    for (JsonPair kv : doc.as<JsonObject>()) {
      if (strcmp(kv.key().c_str(), verbKey.c_str()) == 0) {
        out = verbKey;
        out += " = ";
        String v;
        serializeJson(kv.value(), v);
        out += v;
        found = true;
        break;
      }
    }
    if (found) return out;
    return "ERR: comando desconocido (" + verb + ")";
  }

  if (verb == "HELP") return helpText(viaRemote);

  // rate limit pings over RF (NavaCLI urgent pattern)
  if (verb == "PING" && viaRemote) {
    static uint32_t lastPing = 0;
    uint32_t now = millis();
    if (now - lastPing < 10000) return "";
    lastPing = now;
  }

  if (verb == "PING") {
    cmdPing(cfg, out);
    return out;
  }
  if (verb == "STATUS" || verb == "GET") {
    cmdStatus(cfg, out);
    return out;
  }
  if (verb == "BAT") {
    cmdBat(cfg, out);
    return out;
  }
  if (verb == "RESET_REASON") {
    cmdResetReason(out);
    return out;
  }
  if (verb == "RXLOG") {
    cmdRxLog(out);
    return out;
  }
  if (verb == "DIAG") {
    if (rest == "on") {
      diagSetActive(true);
      return "DIAG on";
    }
    if (rest == "off") {
      diagSetActive(false);
      return "DIAG off";
    }
    if (rest == "nmea on") {
      diagSetNmea(true);
      return "DIAG nmea on";
    }
    if (rest == "nmea off") {
      diagSetNmea(false);
      return "DIAG nmea off";
    }
    char b[72];
    snprintf(b, sizeof(b), "DIAG active=%d nmea=%d tnc=%d", diagActive() ? 1 : 0,
             diagNmea() ? 1 : 0, (int)tncProtocol());
    return b;
  }
  if (verb == "EPD") {
    // Diagnostico de la pantalla de tinta electronica A PETICION. Hace falta porque los
    // mensajes del arranque no se ven nunca (el USB no esta enumerado todavia) y el registro
    // no se puede usar en el arranque (escribir flash ahi rompe el USB). Ver epaper_techo.cpp.
    static char b[320];
    displayDiagTexto(b, sizeof(b));
    return b;
  }
  if (verb == "EPDSONDA") {
    // ★ HERRAMIENTA DE TALLER (2026-09-14): sonda CRUDA del periferico SPI de la pantalla.
    // Lanza una transferencia de 5000 bytes y va leyendo los registros (ENABLE, PSEL,
    // FREQUENCY, TAREAS, EVENTS_*, TXD.AMOUNT) para saber si el periferico arranca de
    // verdad. El 'S' inicial es la senal para epaper_techo.cpp.
    // El firmware no la usa para nada: se puede quitar cuando la pantalla este validada.
    static char sb[640];
    sb[0] = 'S';
    displayDiagTexto(sb, sizeof(sb));
    return sb;
  }
  if (verb == "EPDMUEVE") {
    // ★ ¿MUEVE EL BIT-BANG LOS PINES? Empuja SCK y MOSI y los lee (herramienta de taller).
    static char wb[300];
    wb[0] = 'W';
    displayDiagTexto(wb, sizeof(wb));
    return wb;
  }
  if (verb == "EPDVOLCADO") {
    // ★ VOLCADO DE REGISTROS de los pines (herramienta de taller): PIN_CNF de cada pin y
    // que se lee con el pin suelto, con pull-up y con pull-down. Es lo que dice si un pin
    // esta sujeto a masa por algo o si simplemente esta mal configurado.
    static char vb[700];
    vb[0] = 'V';
    displayDiagTexto(vb, sizeof(vb));
    return vb;
  }
  if (verb == "EPDROT") {
    // ★ ROTACION EN CALIENTE (herramienta de taller): `epdrot 0..3` repinta con esa
    // orientacion. Sirve para enderezar la imagen sin volver a grabar. El 'G' inicial es
    // la senal para epaper_techo.cpp.
    static char gb[220];
    gb[0] = 'G';
    gb[1] = (rest.length() && rest[0] >= '0' && rest[0] <= '3') ? rest[0] : '3';
    displayDiagTexto(gb, sizeof(gb));
    return gb;
  }
  if (verb == "EPDREPINTA") {
    // ★ Repinta el estado del nodo AHORA y dice cuantos bytes ha movido (herramienta de taller).
    // La PRIMERA vez de cada arranque repinta en COMPLETO y las siguientes en PARCIAL: asi se
    // ve y se mide la diferencia con el mismo comando (Paso 1, 2026-09-15).
    static char rb[420];
    rb[0] = 'R';
    displayDiagTexto(rb, sizeof(rb));
    return rb;
  }
  if (verb == "EPDHUELLA") {
    // ★ ¿QUE DATO HACE QUE LA PANTALLA SE REPINTE SIN MOTIVO? (herramienta de taller).
    // Saca la huella del contenido y todos sus ingredientes. Se llama dos veces y se
    // comparan: el numero que cambie es el culpable. El 'F' inicial es la senal para
    // epaper_techo.cpp. Existe porque con la huella puesta se midieron repintados de mas
    // y hay que saber CUAL de los datos cambia en vez de suponerlo.
    static char fb[420];
    fb[0] = 'F';
    displayDiagTexto(fb, sizeof(fb));
    return fb;
  }
  if (verb == "EPDPARCIAL") {
    // ★ PRUEBA DEL REFRESCO PARCIAL / CARRUSEL (Paso 1, 2026-09-15).
    // `epdparcial N` (N = 1..9, por defecto 3) va cambiando de escena N veces; cada cambio
    // pinta con `epdFlush()`, que el primero tras el arranque es COMPLETO (parpadea) y los
    // siguientes PARCIALES (no deben parpadear). Es la prueba que pide el Paso 1 antes de
    // hacer el carrusel automatico (Paso 2). El 'C' inicial es la senal para epaper_techo.cpp.
    static char cb[420];
    cb[0] = 'C';
    cb[1] = (rest.length() && rest[0] >= '1' && rest[0] <= '9') ? rest[0] : '3';
    displayDiagTexto(cb, sizeof(cb));
    return cb;
  }
  if (verb == "EPDPWR") {
    // ★ INTERRUPTOR DE ALIMENTACION DE LA PANTALLA, P1.11 (herramienta de taller).
    //   epdpwr 0 -> BAJO   epdpwr 1 -> ALTO   epdpwr 2 -> sin tocar (entrada)
    // Despues de cambiarlo SONDA LOS PINES, que es lo que dice si con ese estado al panel
    // le llega corriente (si sus pines se pueden empujar a 1, si; si los sujeta a masa, no).
    // Lo hace asi epaper_techo.cpp: 'B' + el numero en el primer byte del buffer.
    static char bb[460];
    bb[0] = 'B';
    bb[1] = (rest == "1") ? '1' : ((rest == "2") ? '2' : '0');
    displayDiagTexto(bb, sizeof(bb));
    return bb;
  }
  if (verb == "EPDPINES") {
    // ★ Sonda de PINES (herramienta de taller): empuja cada pin de la pantalla a 0 y a 1
    // y lee su propio estado. Sirve de detector de alimentacion del panel: si al panel no
    // le llega Vdd, sus diodos de proteccion sujetan los pines y no suben a 1.
    static char pb[420];
    pb[0] = 'P';
    displayDiagTexto(pb, sizeof(pb));
    return pb;
  }
  if (verb == "EPDTRANS") {
    // Cambia el transporte de la pantalla en caliente, para poder compararlos sin volver a
    // grabar:   epdtrans 0  -> bit-bang (GPIO a mano, el que se usa)
    //           epdtrans 1  -> periferico SPIM2
    // El 'T' inicial es la senal para epaper_techo.cpp. (Herramienta de taller.)
    static char tb[4];
    tb[0] = 'T';
    tb[1] = (rest == "1") ? '1' : '0';
    displayDiagTexto(tb, sizeof(tb));
    return (tb[1] == '1') ? "EPD transporte = SPIM2" : "EPD transporte = bit-bang";
  }
  if (verb == "CAP") {
    // SONDA del boton tactil capacitivo P0.11 (herramienta de taller, 2026-09-15).
    // Sirve para cazar por que el toque no dispara nada: la sube como entrada con
    // pull-up y dice el nivel que lee AHORA, y asi queda claro si al tocar cambia
    // (y a que nivel) o si el pin esta muerto. Se llama dos veces: una sin tocar y
    // otra tocando.
    static char cb[90];
#if defined(PIN_BTN_TOUCH)
    pinMode(PIN_BTN_TOUCH, INPUT_PULLUP);
    // dos muestras con un margen, por si es un nivel lento
    int a = digitalRead(PIN_BTN_TOUCH);
    delay(30);
    int b = digitalRead(PIN_BTN_TOUCH);
    snprintf(cb, sizeof(cb), "CAP P0.11 nivel=%d/%d (touch=P%d.%d INPUT_PULLUP)",
             a, b, (PIN_BTN_TOUCH >> 5) & 1, PIN_BTN_TOUCH & 31);
#else
    snprintf(cb, sizeof(cb), "CAP: PIN_BTN_TOUCH no definido en esta placa");
#endif
    return cb;
  }
  if (verb == "LOG") {
    // Trip log stored in flash (survives power cycles). USB only: a dump over
    // RF would flood the channel.
    if (rest == "dump") {
      if (viaRemote) return "ERR: log dump solo por USB";
      flogDump(Serial);
      return "LOG dump ok";
    }
    if (rest == "stats" || rest == "") {
      flogStats(Serial);
      return "";
    }
    if (rest == "clear") {
      if (viaRemote) return "ERR: log clear solo por USB";
      return flogClear() ? "LOG cleared" : "ERR: bateria baja";
    }
    if (rest == "on") {
      flogSetEnabled(true);
      return "LOG on";
    }
    if (rest == "off") {
      flogSetEnabled(false);
      return "LOG off";
    }
    return "ERR: log dump|stats|clear|on|off";
  }
  if (verb == "GPS") {
    // Talk to the GNSS module: info / send <sentence> / coldstart.
    if (rest == "info" || rest == "") {
      const GpsData &g = gpsGet();
      char b[160];
      snprintf(b, sizeof(b),
               "GPS %s [%s] fix=%d vista=%u senal=%u usados=%u sat=%s sent=%s",
               gpsPowered() ? "on" : "off", gpsVendor(), g.fix ? 1 : 0,
               (unsigned)g.satsInView, (unsigned)g.bestSnr, (unsigned)g.sats,
               gpsSeenSentences(), g.timeValid ? "hora ok" : "sin hora");
      return b;
    }
    if (rest.startsWith("send ")) {
      String s = rest.substring(5);
      s.trim();
      bool ok = gpsSendNmea(s.c_str());
      return ok ? ("GPS send: " + s) : "ERR: GPS apagado o sentencia vacia";
    }
    if (rest == "coldstart") {
      bool ok = gpsColdStart();
      return ok ? String("GPS coldstart (") + gpsVendor() + ")" : "ERR: GPS apagado";
    }
    return "ERR: gps info|send <sentencia>|coldstart";
  }
  if (verb == "WX") {
    int16_t st = aprsSendWeather(cfg);
    char b[48];
    snprintf(b, sizeof(b), "WX code=%d%s", (int)st,
             (st == -110) ? " (sin hora GPS)" : "");
    return b;
  }
  if (verb == "BUL" || verb == "BULLETIN") {
    // bul [n] <texto>: bulletin for everybody (message to BLNn, n = 0..9).
    rest.trim();
    if (rest.length() == 0) return "ERR: bul [0-9] <texto>";
    char group = '0';
    String txt = rest;
    if (rest.length() > 2 && rest[1] == ' ') {
      group = rest[0];
      txt = rest.substring(2);
      txt.trim();
    }
    if (txt.length() == 0) return "ERR: bul [0-9] <texto>";
    int16_t st = aprsSendBulletin(cfg, txt.c_str(), group);
    char b[80];
    snprintf(b, sizeof(b), "BOLETIN BLN%c: %s (code=%d)", group, txt.c_str(),
             (int)st);
    return b;
  }
  if (verb == "OBJ" || verb == "OBJECT") {
    // obj <nombre> <lat> <lon> [comentario]
    int sp1 = rest.indexOf(' ');
    if (sp1 <= 0) return "ERR: obj <nombre> <lat> <lon> [texto]";
    String name = rest.substring(0, sp1);
    String r2 = rest.substring(sp1 + 1);
    r2.trim();
    int sp2 = r2.indexOf(' ');
    if (sp2 <= 0) return "ERR: obj <nombre> <lat> <lon> [texto]";
    double lat = atof(r2.substring(0, sp2).c_str());
    String r3 = r2.substring(sp2 + 1);
    r3.trim();
    int sp3 = r3.indexOf(' ');
    String lonStr = (sp3 < 0) ? r3 : r3.substring(0, sp3);
    String cmt = (sp3 < 0) ? String("") : r3.substring(sp3 + 1);
    cmt.trim();
    double lon = atof(lonStr.c_str());
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0 ||
        (lat == 0.0 && lon == 0.0)) {
      return "ERR: coordenadas fuera de rango";
    }
    int16_t st = aprsSendObject(cfg, name.c_str(), lat, lon, cmt.c_str(), false);
    char b[96];
    snprintf(b, sizeof(b), "OBJETO %s %.5f,%.5f (code=%d)", name.c_str(), lat,
             lon, (int)st);
    return b;
  }
  if (verb == "OBJKILL") {
    rest.trim();
    if (rest.length() == 0) return "ERR: objkill <nombre>";
    int16_t st = aprsSendObject(cfg, rest.c_str(), 0, 0, "", true);
    char b[64];
    snprintf(b, sizeof(b), "OBJETO %s borrado (code=%d)", rest.c_str(), (int)st);
    return b;
  }
  if (verb == "Q" || verb == "QUERY") {
    // q <consulta>: prueba el motor de consultas APRS SIN emitir (solo USB).
    if (viaRemote) return "ERR: q solo por USB";
    rest.trim();
    if (rest.length() == 0) return "ERR: q <consulta> (ej: q ?APRS?)";
    String r = aprsQueryText(cfg, rest);
    if (r.length() == 0) r = "(sin respuesta)";
    return r;
  }
  if (verb == "RXS" || verb == "RXSIM") {
    // rxs <de> <para> <texto>: inyecta una trama recibida (solo USB). Sirve
    // para probar con una sola placa los acuses, las consultas y el control
    // remoto, sin necesidad de un segundo equipo.
    if (viaRemote) return "ERR: rxs solo por USB";
    rest.trim();
    int sp1 = rest.indexOf(' ');
    if (sp1 <= 0) return "ERR: rxs <de> <para> <texto>";
    String from = rest.substring(0, sp1);
    String r2 = rest.substring(sp1 + 1);
    r2.trim();
    int sp2 = r2.indexOf(' ');
    String to = (sp2 < 0) ? r2 : r2.substring(0, sp2);
    String body = (sp2 < 0) ? String("") : r2.substring(sp2 + 1);
    if (to.length() == 0) return "ERR: rxs <de> <para> <texto>";
    bool consumed = aprsInjectMessage(cfg, from.c_str(), to.c_str(), body.c_str());
    char b[96];
    snprintf(b, sizeof(b), "RXS %s -> %s: %s = %s", from.c_str(), to.c_str(),
             body.c_str(), consumed ? "consumido" : "no era para nosotros");
    return b;
  }
  if (verb == "TONO") {
    // ★ HERRAMIENTA DE TALLER (2026-09-15): reproduce la melodia de bateria baja para
    // OIRLA sin gastar bateria. La de verdad suena sola cuando el nodo se va a dormir por
    // bateria baja; esto es para ajustarla de oido (las notas estan en displayLowBatTone()).
    // OJO: bloquea ~1,3 s, que es lo que dura la melodia.
    if (viaRemote) return "ERR: tono solo por USB";
    // Se pregunta ANTES: el zumbador solo suena si la placa es un Plus, y eso se sabe por
    // el motor (ver hapticEsPlus()). Asi el comando dice si de verdad ha sonado o no.
    const bool plus = hapticEsPlus();
    displayLowBatTone();
    static char tb[140];
    snprintf(tb, sizeof(tb), "TONO: melodia %s | %s", plus ? "reproducida" : "NO reproducida",
             plus ? "zumbador del Plus disponible"
                  : "esta placa no es un Plus (sin motor ni zumbador)");
    return tb;
  }
  if (verb == "VIBRA") {
    // ★ HERRAMIENTA DE TALLER (2026-09-15): motor haptico del T-Echo Plus (DRV2605, I2C 0x5A).
    //   `vibra N`  -> dispara el efecto N de la ROM interna (0..123)
    //   `vibra`    -> dice si el chip contesta y como se usa
    // POR QUE EXISTE: los patrones hay que ELEGIRLOS DE OIDO (cual se nota, cual molesta).
    // Esta es la forma de probarlos uno a uno antes de engancharlos a los avisos, en vez de
    // adivinar que efecto queda bien. NO bloquea: el chip reproduce el efecto el solo.
    if (viaRemote) return "ERR: vibra solo por USB";
    static char vb[140];
    if (rest.length() == 0) {
      bool ok = hapticInit();
      snprintf(vb, sizeof(vb),
               "VIBRA: motor %s | uso: vibra <efecto 0..123> (el chip lo reproduce solo)",
               ok ? "LISTO (DRV2605 en 0x5A)" : "NO responde (en esta placa no hay motor)");
      return vb;
    }
    int e = rest.toInt();
    if (e < 0 || e > 123) return "ERR: el efecto va de 0 a 123";
    bool ok = hapticEffect((uint8_t)e);
    snprintf(vb, sizeof(vb), "VIBRA: efecto %d %s", e,
             ok ? "disparado" : "FALLO (el chip no contesta)");
    return vb;
  }
  if (verb == "I2CSCAN") {
    // ★ HERRAMIENTA DE TALLER (2026-09-15): escaneo del bus I2C. Dice que direcciones
    // contestan y el chip ID del sensor meteorologico en 0x76/0x77.
    // POR QUE EXISTE: "el BME280 no se detecta" tiene dos causas que desde fuera NO se
    // pueden separar (el chip no esta / el bus no funciona). En el T-Echo hay dos chips
    // en ese bus: el BME280 (0x76/0x77) y el reloj PCF8563 (0x51). Si sale el 0x51 y no
    // el 0x77, el bus vive y el que falla es el BME280.
    // SOLO LEE: no escribe en ningun chip ni en la configuracion.
    if (viaRemote) return "ERR: i2cscan solo por USB";
    static char ib[240];
    sensorsI2cScan(ib, sizeof(ib));
    return ib;
  }
  if (verb == "BATTEST") {
    // battest <mV>: enseña el aviso de batería baja tal como saldría en la
    // baliza con esa tensión y sin USB (para probarlo sin gastar batería).
    if (viaRemote) return "ERR: battest solo por USB";
    int mv = rest.length() ? rest.toInt() : 0;
    if (mv <= 0) {
      char b[80];
      snprintf(b, sizeof(b), "Uso: battest <mV> | corte=%d wake=%d aviso=%d",
               cfg.sleepCutMv, cfg.sleepWakeMv, aprsLowBatWarnMv());
      return b;
    }
    String t = aprsLowBatText(cfg, mv);
    char b[128];
    snprintf(b, sizeof(b), "bat %d mV -> \"%s\" (baliza: ...%s)", mv,
             t.length() ? t.c_str() : "(sin aviso)", t.c_str());
    return b;
  }
  if (verb == "MSG" && rest.length() == 0) {
    char to[12];
    uint16_t id = 0;
    uint8_t tries = 0;
    if (!aprsMsgPending(to, sizeof(to), &id, &tries)) return "MSG: ninguno pendiente";
    char b[80];
    snprintf(b, sizeof(b), "MSG pendiente -> %s #%03u (%u intentos)", to,
             (unsigned)id, (unsigned)tries);
    return b;
  }
  if (verb == "MSG") {
    // msg <destino> <texto>: APRS message started by the operator.
    int sp = rest.indexOf(' ');
    if (sp <= 0) return "ERR: msg <destino> <texto>";
    String to = rest.substring(0, sp);
    String txt = rest.substring(sp + 1);
    to.trim();
    txt.trim();
    if (to.length() == 0 || txt.length() == 0) return "ERR: msg <destino> <texto>";
    int16_t st = aprsSendMessage(cfg, to.c_str(), txt.c_str());
    char b[80];
    snprintf(b, sizeof(b), "MSG -> %s: %s (code=%d)", to.c_str(), txt.c_str(),
             (int)st);
    return b;
  }
  if (verb == "TRKBEACON") {
    // Force a tracker beacon (with the GPS position) - bench/testing helper.
    int16_t st = trackerBeaconNow(cfg);
    char b[48];
    snprintf(b, sizeof(b), "TRKBEACON code=%d", (int)st);
    return b;
  }
  if (verb == "MUTE") {
    cfg.txDisabled = true;
    storeSave(cfg);
    radioSetMuted(true);
    displayNoteTx("MUTE");
    return "MUTED";
  }
  if (verb == "UNMUTE") {
    cfg.txDisabled = false;
    storeSave(cfg);
    radioSetMuted(false);
    displayNoteTx("UNMUTE");
    return "UNMUTED";
  }
  if (verb == "KISSOFF") {
    // PAUSA DEL MODO KISS (override del operador, 2026-09-15). Con el selector en
    // KISS, el nodo trataba todo como ordenes del programa host y negaba los
    // comandos normales (baliza manual, WX, telemetria). `kissoff` devuelve el
    // mando al operador SIN tocar el selector: el nodo vuelve a obedecer sus
    // ordenes por USB. EN MEMORIA: se pierde al reiniciar. `kisson` lo rearma.
    tncKissPause();
    return "KISS PAUSED (el nodo obedece sus ordenes normales; 'kisson' para volver)";
  }
  if (verb == "KISSON") {
    tncKissResume();
    return "KISS ACTIVE (el programa host vuelve a mandar)";
  }
  if (verb == "BEACON") {
    // Manual beacon: GPS position in tracker modes, configured one otherwise.
    int16_t st = aprsSendManualBeacon(cfg);
    char b[40];
    snprintf(b, sizeof(b), "BEACON code=%d", (int)st);
    return b;
  }
  if (verb == "TELEMETRY" || verb == "TELEM") {
    int16_t st;
    if (rest == "meta") st = aprsSendTelemetryMeta(cfg);
    else st = aprsSendTelemetry(cfg);
    char b[48];
    snprintf(b, sizeof(b), "TELEM%s code=%d", rest == "meta" ? " meta" : "",
             (int)st);
    return b;
  }
  if (verb == "DFU") {
    if (rest != "confirm") return "ERR: dfu confirm (entra en bootloader UF2)";
    Serial.println("DFU: reboot into UF2 bootloader");
    Serial.flush();
    delay(100);
    enterUf2Dfu();
    return "";  // never reached
  }
  if (verb == "SET" && rest.length() > 0) {
    int eq = rest.indexOf(' ');
    if (eq < 0) return "ERR: set <clave> <valor>";
    String key = rest.substring(0, eq);
    String val = rest.substring(eq + 1);
    val.trim();
    String err;
    if (!typedSet(cfg, key, val, err)) {
      char b[96];
      snprintf(b, sizeof(b), "ERR %s", err.c_str());
      return b;
    }
    bool saved = storeSave(cfg);
    radioSetMuted(cfg.txDisabled);
    radioApplyPower(cfg.powerDbm);
    char b[128];
    snprintf(b, sizeof(b), "OK %s=%s%s", key.c_str(), val.c_str(),
             saved ? "" : " (not saved)");
    return b;
  }
  // Reinicio suave. "reset" es alias de "reboot" a proposito: el operador lo
  // mando creyendo que reiniciaba el nodo y perdio su configuracion, asi que
  // "reset" YA NO puede borrar nada. El borrado de fabrica es el verbo
  // "factory_reset confirm" (justo debajo). Mismo patron que "dfu confirm":
  // avisar por el serial, vaciarlo y resetear.
  if (verb == "REBOOT" || verb == "RESET") {
    if (rest != "confirm") return "ERR: " + verbKey + " confirm";
    Serial.println("REBOOT");
    Serial.flush();
    delay(100);
    NVIC_SystemReset();
    return "";  // never reached
  }
  if (verb == "FACTORY_RESET") {
    if (rest != "confirm") return "ERR: factory_reset confirm";
    // Factory defaults PRESERVING the access layer (NavaTastic resilience.bin idea)
    char keepCall[16], keepMgrs[64];
    bool keepRemote = cfg.remoteEnabled;
    strncpy(keepCall, cfg.callsign, sizeof(keepCall) - 1);
    keepCall[sizeof(keepCall) - 1] = 0;
    strncpy(keepMgrs, cfg.managers, sizeof(keepMgrs) - 1);
    keepMgrs[sizeof(keepMgrs) - 1] = 0;
    cfg = DigiConfig();
    strncpy(cfg.callsign, keepCall, sizeof(cfg.callsign) - 1);
    strncpy(cfg.managers, keepMgrs, sizeof(cfg.managers) - 1);
    cfg.remoteEnabled = keepRemote;
    storeSave(cfg);
    radioSetMuted(cfg.txDisabled);
    return "OK factory_reset (acceso conservado: callsign+managers+remote)";
  }
  if (verb == "WIPE") {
    if (viaRemote) return "ERR: wipe solo USB";
    if (rest != "confirm") return "ERR: wipe confirm";
    storeWipe();
    cfg = DigiConfig();
    storeSave(cfg);
    radioSetMuted(false);
    Serial.println("OK wipe -> reboot");
    Serial.flush();
    delay(100);
    NVIC_SystemReset();
    return "";
  }

  return "ERR: comando desconocido (" + verb + ") (help)";
}
