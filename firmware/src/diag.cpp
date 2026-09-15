// diag.cpp — real-time diagnostics stream. See diag.h.
// Output is line-based JSON, one object per line (JSONL), so it can be logged
// and post-analysed without breaking the WebSerial protocol/CLI replies.
// License: GPL-3.0

#include "diag.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include "aprs.h"
#include "display.h"
#include "gps.h"
#include "power.h"
#include "radio.h"
#include "sensors.h"
#include "tnc.h"
#include "tracker.h"

namespace {

DigiConfig *gCfg = nullptr;
bool gActive = false;
bool gNmea = false;
uint32_t gLastSnapMs = 0;

void jsonEscapePrint(const char *s) {
  for (const char *p = s; p && *p; p++) {
    char c = *p;
    if (c == '"' || c == '\\') Serial.print('\\');
    if ((uint8_t)c >= 0x20) Serial.print(c);
  }
}

}  // namespace

void diagBindConfig(DigiConfig *cfg) { gCfg = cfg; }

void diagSetActive(bool on) { gActive = on; }
bool diagActive() { return gActive; }
void diagSetNmea(bool on) { gNmea = on; }
bool diagNmea() { return gNmea; }

bool diagStreaming() { return gActive && !tncActive(); }

void diagRxFrame(const char *from, const char *info, float rssi, float snr) {
  if (!diagStreaming()) return;
  Serial.print(F("{\"diag\":\"rx\",\"ms\":"));
  Serial.print(millis());
  Serial.print(F(",\"from\":\""));
  jsonEscapePrint(from);
  Serial.print(F("\",\"rssi\":"));
  Serial.print(rssi, 0);
  Serial.print(F(",\"snr\":"));
  Serial.print(snr, 1);
  Serial.print(F(",\"info\":\""));
  jsonEscapePrint(info);
  Serial.println(F("\"}"));
}

void diagTxFrame(const char *frame, size_t len, int code) {
  if (!diagStreaming()) return;
  Serial.print(F("{\"diag\":\"tx\",\"ms\":"));
  Serial.print(millis());
  Serial.print(F(",\"code\":"));
  Serial.print(code);
  Serial.print(F(",\"len\":"));
  Serial.print((unsigned)len);
  Serial.print(F(",\"frame\":\""));
  jsonEscapePrint(frame);
  Serial.println(F("\"}"));
}

void diagNote(const char *what) {
  if (!diagStreaming()) return;
  Serial.print(F("{\"diag\":\"note\",\"what\":\""));
  jsonEscapePrint(what);
  Serial.println(F("\"}"));
}

namespace {

void emitSnapshot() {
  const GpsData &g = gpsGet();
  const TrackerDiag td = trackerDiag(*gCfg);

  JsonDocument d;
  d["diag"] = "snap";
  d["ms"] = millis();
  d["mode"] = gCfg->mode;
  d["tnc"] = gCfg->tncProtocol;  // 0 off / 1 TNC2 / 2 KISS
  // KISS health: frames replayed to the host, bytes that really left through USB
  // and the error code of the last drop. A listener that prints nothing while
  // kiss.out stays at 0 points at the node; if out/bytes grow, the node did its
  // part and the missing frames are a host-side problem.
  JsonObject kiss = d["kiss"].to<JsonObject>();
  kiss["out"] = tncKissFramesOut();
  kiss["bytes"] = tncKissUsbTxBytes();
  kiss["lastErr"] = tncKissLastDrop();
  // 2026-09-13 RESCATE: the "ble" object of the snapshot (on/state/adv/conn/
  // ready/name/in/out/bytes/drop/pairReq/pairOk/pairFail/pairStatus) is gone
  // with the Bluetooth code: there is no stack to report on any more. The
  // config fields bleEnabled/blePin stay in the JSON, inert and unread.
  d["call"] = gCfg->callsign;
  d["bcnMin"] = gCfg->beaconIntervalMin;

  JsonObject gps = d["gps"].to<JsonObject>();
  gps["on"] = gpsPowered();
  gps["fix"] = g.fix;
  gps["sats"] = g.sats;
  gps["hdop"] = g.hdop;
  gps["lat"] = g.lat;
  gps["lon"] = g.lon;
  gps["spd"] = g.speedKmh;
  gps["crs"] = g.courseDeg;
  gps["alt"] = g.altM;
  gps["ageMs"] = g.fix ? (millis() - g.lastFixMs) : 0;
  gps["inView"] = g.satsInView;  // satellites seen (works with no fix)
  gps["snr"] = g.bestSnr;
  if (g.timeValid) {
    char t[12];
    snprintf(t, sizeof(t), "%02u:%02u:%02u", g.utcH, g.utcM, g.utcS);
    gps["utc"] = t;
  }

  JsonObject trk = d["trk"].to<JsonObject>();
  trk["first"] = td.firstFix;
  trk["mv"] = td.moving;
  trk["lastBcnMs"] = td.lastBeaconMs;
  trk["nextInMs"] = td.nextBeaconMs;
  trk["cornerMs"] = td.lastCornerMs;

  JsonObject pw = d["pwr"].to<JsonObject>();
  pw["mv"] = powerReadMv();
  pw["usb"] = powerUsbPresent();
  pw["low"] = powerLowCount();
  pw["cut"] = gCfg->sleepCutMv;
  pw["wake"] = gCfg->sleepWakeMv;

  JsonObject rd = d["rdo"].to<JsonObject>();
  rd["rssi"] = radioLastRssi();
  rd["snr"] = radioLastSnr();
  rd["fErr"] = radioLastFreqErr();
  rd["crc"] = radioCrcErrCount();
  rd["rx"] = radioRxCount();
  rd["tx"] = radioTxCount();
  rd["dg"] = aprsDigiCount();
  rd["from"] = aprsLastFrom();

  JsonObject se = d["sens"].to<JsonObject>();
  se["wx"] = gSensorCache.wxOk;
  se["temp"] = gSensorCache.tempC;
  se["ctemp"] = gSensorCache.chipTempC;  // raw die temperature (offset aside)
  se["hum"] = gSensorCache.hum;
  se["hpa"] = gSensorCache.pressHpa;
  se["vbat"] = gSensorCache.vbatDivV;
  se["ima"] = gSensorCache.inaCurrentMa;

  JsonObject ui = d["ui"].to<JsonObject>();
  ui["on"] = displayIsOn();
  ui["menu"] = menuIsOpen();

  serializeJson(d, Serial);
  Serial.println();
}

}  // namespace

void diagLoop() {
  // Keep the NMEA echo in sync with the effective stream state.
  gpsSetRawEcho(gNmea && diagStreaming());
  if (!diagStreaming()) return;

  uint32_t now = millis();
  if (now - gLastSnapMs < 1000) return;
  gLastSnapMs = now;
  emitSnapshot();
}
