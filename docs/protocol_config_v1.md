# Config protocol v1 (USB-CDC / WebSerial)

Status: APRS beacon milestone (2026-09-09). v1 scope = station + digi + LoRa single profile + radio test helpers + APRS beacon + config persistence.

## Transport
- USB-CDC serial, 115200 8N1 (same as CA2RXU nRF52 firmware).
- Web configurator (WebSerial): `web/index.html` — bilingual ES/EN, schema-driven, read-back
  after every save, typed confirmation for destructive actions (see `web/README.md`).
- Three input kinds on the same port, decided byte by byte:
  - `0xC0` (KISS FEND) → a **binary KISS frame** (AX.25 UI): it is handed to the radio, and everything up to the closing FEND belongs to it. Needs `tncProtocol = 2`; harmless otherwise.
  - `{` → JSON object per line (machine mode, this spec). JSON replies.
  - anything else → **NavaCLI-style token command** (`ping`, `set digiMode 1`...) with human-readable text replies (see "Token mode" below).
- Max line: 1024 bytes (extra bytes ignored until newline). Max KISS frame: 340 bytes (dropped, stream kept in sync).
- JSON and the CLI keep answering while KISS is enabled, so the node can always be reconfigured from the same port: a KISS app must ignore anything that does not start with FEND. See `docs/APP_PROPIA_Y_COMPATIBILIDAD.md` §1.1.
- **Bluetooth LE is NOT available in this build** (2026-09-13): the BLE code exists (`ble_kiss.cpp/.h`) but is kept OUT of the compilation because linking it left the node with no screen and no USB. When it comes back it will be a second link (Nordic UART Service), not a mode of this port: see `docs/APP_PROPIA_Y_COMPATIBILIDAD.md` §1.2.
- Idle: no unsolicited traffic (boot banner, the opt-in `rxlog` stream and the periodic auto-beacon line are the only exceptions; the `rxlog` stream and the auto-beacon line are suppressed while the TNC bridge is on, and the `BLE ...` event lines are suppressed while the TNC bridge or the diagnostic stream is on).

## Token mode (NavaCLI-style, shared engine USB + RF remote)
Same engine `src/cli.cpp` serves USB (viaRemote=false) and RF-remote managers (viaRemote=true).
Commands: `ping`, `status`/`get`, `bat`, `reset_reason`, `rxlog`, `diag` (see below), `set <key> <val>`
(any config key, typed value, validated + persisted), `mute`, `unmute`, `beacon`, `wx`,
`trkbeacon`, `msg [destino texto]` (APRS message, no arguments = show the one waiting for
its ack), `bul [0-9] <texto>` (bulletin for everybody), `obj <nombre> <lat> <lon> [texto]` /
`objkill <nombre>` (APRS object), `q <consulta>` (**USB only**: runs the query engine without
transmitting, to test `?APRS?` / `?APRSH 2` / `?TX=?` with a single board),
`telemetry [meta]`, `help [cmd|key]`, `<cmd> ?` (usage + current value), and destructive with
`confirm`:
| cmd | policy |
|---|---|
| `reboot confirm` | Allowed (USB + remote). Soft reboot, config intact. |
| `reset confirm` | **Same as `reboot confirm`** (soft reboot, config intact). Since 2026-09-12 `reset` can NO LONGER wipe: the operator sent it believing it rebooted and lost his configuration. |
| `factory_reset confirm` | Allowed (USB + remote). **Erases the configuration** and restores factory defaults, **preserving callsign + managers + remoteEnabled** so the node stays manageable remotely (this was the old `reset confirm` behaviour). It does NOT reboot. |
| `wipe confirm` | **USB only** (rejected over RF). Full erase + reboot; node unreachable until physically reconfigured. |

## Commands (client -> node, JSON mode)
| cmd | body | effect |
|---|---|---|
| `{"cmd":"get"}` | — | Reply with current config (read-back) |
| `{"cmd":"set","config":{...}}` | full or partial snapshot | Validate+apply atomically, persist to internal flash, reply with applied config |
| `{"cmd":"factory_reset"}` | - | **Erases the configuration**: restore factory defaults, persist, reply with applied config. It does NOT reboot |
| `{"cmd":"reboot"}` | - | Soft reboot (`NVIC_SystemReset`) with no data loss. Answers `REBOOT` and restarts the USB-CDC port, so no JSON reply |
| `{"cmd":"reset"}` | - | Alias of `reboot`, kept on purpose so the old, dangerous name can never wipe again |
| `{"cmd":"status"}` | — | Reply version/uptime/radio state |
| `{"cmd":"radio","rxlog":true\|false}` | — | Opt-in RX log stream (off by default) |
| `{"cmd":"diag","diag":{"active":true\|false,"nmea":true\|false}}` | — | Real-time diagnostics JSON stream (runtime only, off at boot; auto-muted while TNC mode is on) |
| `{"cmd":"radio","txtest":true\|false}` | — | Periodic raw-LoRa TX test every 10 s (off by default; fails closed) |
| `{"cmd":"beacon"}` | — | Transmit one APRS position beacon now (CAD-lite first when cadActive). In tracker modes (1/2) it uses the live GPS position when there is a fix; mode 0 and a tracker without a fix use the configured coordinates |
| `{"cmd":"msg","to":"EA2XXX-7","text":"hola"}` | `to` optional | Send one APRS message to that station; without `to` it goes to the **last station heard**. `text` is capped at 200 chars and gets a `{NNN` message id so the peer can ack. The node then **resends it every 30 s** (same id) until the ack arrives or `msgRetries` extra attempts are spent |

Unknown cmd / malformed JSON -> `{"ok":false,"error":"..."}`.

## Replies (node -> client)
- Success: `{"ok":true,"config":{...},"persisted":true}` — **always echoes the applied (validated/clamped) config** and whether it was stored to internal flash (CA2RXU does fire-and-forget; we do read-back).
- Error: `{"ok":false,"error":"<reason>"}`.
- Radio cmd reply: `{"ok":true,"radio":{"rxlog":..,"txtest":..}}` (current state after apply).
- Beacon cmd reply: `{"ok":true,"beacon":{"tx":true,"code":0}}` (`code`: RadioLib return; -100 = not configured, -101 = channel busy, CAD aborted).
- Message cmd reply: `{"ok":true,"msg":{"to":"EA2XXX-7","text":"hola","tx":true,"code":0}}` (`ok:false` + `error` when nobody has been heard yet or the text is empty).
- Status cmd reply also carries the **radio module** (`radio.module`: `HT-RA62 (SX1262)` or `E22P-433M30S`, so the web can warn about the 22/12 dBm limit) and the **GPS** (`gps.{present,fix,sats,inView,hdop,lat,lon,altM,speedKmh,courseDeg,timeValid}`), which the configurator uses for the “use GPS coordinates” button.
- RX log stream (while `rxlog` is on): `{"radio":"rx","len":n,"rssi":-xx.x,"snr":x.x}` per received LoRa packet.

### Diagnostics stream (Token `diag on|off|nmea on|nmea off|?`)
- `diag on` / `diag off`: runtime toggle (never persisted; OFF at every boot).
- `diag nmea on|off`: additionally echoes the raw GPS NMEA on Serial.
- 1 Hz snapshot: `{"diag":"snap","ms":..,"mode":..,"tnc":..,"call":"..","gps":{on,fix,sats,hdop,lat,lon,spd,crs,alt,ageMs,utc},"trk":{first,mv,lastBcnMs,nextInMs,cornerMs},"pwr":{mv,usb,low,cut,wake},"rdo":{rssi,snr,fErr,crc,rx,tx,dg,from},"sens":{wx,temp,hum,hpa,vbat,ima},"ui":{on,menu}}`.
- Per frame: `{"diag":"rx","ms":..,"from":..,"rssi":..,"snr":..,"info":"<full frame>"}` and `{"diag":"tx","ms":..,"code":..,"len":..,"frame":"<full frame>"}` (code `-101` channel busy, `-102` muted).
- The stream is automatically muted while the TNC bridge is on (`tncProtocol` != 0: the TNC line/KISS stream must stay clean) and resumes when TNC is disabled.
- Capture + analysis helpers: `tools/monitor.ps1` (USB capture to `.jsonl`) and `tools/diag_report.js` (summary report).
- Periodic auto-beacon line (every `beaconInterval` minutes, min 10): `{"radio":"beacon","auto":true,"tx":true,"code":0}`. In tracker modes (1/2) the automatic position is GPS-driven, and the configured position is only a boot fallback (once, about 10 minutes after boot, when the GPS still has no fix).

## APRS beacon (wire format, CA2RXU-ecosystem)
LoRa payload = `\x3C\xFF\x01` + ASCII frame:
`EA2OY-10>APLRG1,WIDE1-1:!3000.00S/14000.00W# H37% 26.1C 969.0hPa Tint:28.0C B3.87V`
- Position: uncompressed APRS, symbol table `/`, symbol char = config `symbol`.
- WX (when `wxSensorActive` and a sensor is present): **human-readable comment** appended after the symbol (` H37% 26.1C 969.0hPa`: humidity first, then temperature and pressure, with `temperatureCorrection` and `heightCorrection` applied). Machine-readable values travel in the APRS telemetry T# channels (see below).
- **Weather packet (separate, every 15 min)**: positionless report `_DDHHMMz` + the standard block (`.../...g...tTTThHHbPPPPP`, missing readings as dots), sent only when the GPS provides the stamp (`wx` CLI command forces one). It carries no position and no symbol, so aprs.fi/findu record the readings as weather **without changing the station symbol** (digipeater/car). The block is deliberately NOT embedded in the position comment: there it was read as plain text and no weather service recognised it.
- Power (when `sendBatteryTelemetry`): ` Bx.xxV` (INA219 bus voltage when in 2.4-4.5 V, else internal divider P0.31), appended after the comment. The INA219 current is no longer part of the comment (` I=+/-xxxmA` was dropped): it still travels in the telemetry T# channel (see below).
- TX gating: beacons (auto and `beacon` cmd) are blocked unless `callsign` is a real callsign (not `NOCALL*`) and a position is configured (`latitude`/`longitude` != 0) — defaults never go on air.
- RadioLib defaults: SF12/BW125/CR4/5, CRC on, sync word 0x12 (private 0x1424), preamble 8 — same as CA2RXU.

## APRS telemetry (aprs.fi channels)
When `sendBatteryTelemetry` or `wxSensorActive` is enabled, the node sends APRS
telemetry (CA2RXU-style):
- `T#<seq>,<Vbat>,<I>,<Temp>,<Hum>,<Press>,00000000` on every telemetry trigger
  (button, `telemetry` command, auto interval).
- Metadata (sent with the first T# and every 10th): `PARM.Vbat,I,Temp,Hum,Press`,
  `UNIT.V,mA,C,%,hPa`,
  `EQNS.0,0.02,0,0,10,-1280,0,0.5,-20,0,0.5,0,0,1,880`, `BITS.00000000`.
  Channels: V (0.02 V step), I (10 mA step, signed via -1280 offset), Temp
  (0.5 C, -20 offset), Hum (0.5 %), Press (1 hPa, -880 offset).
- aprs.fi then charts "Voltage" and "Intensidad" (plus T/H/P) as telemetry.
- Button (when the OLED is already on) forces a telemetry T#; CLI/RF `telemetry [meta]`.

## Digipeater (RX path, same frequency)
Logic ported from CA2RXU `digi_utils.cpp` (GPL-3.0), trimmed (no third-party, no cross-freq, no messages/queries yet).
- Every received LoRa payload is validated: header `\x3C\xFF\x01`, frame structure `SRC>DST,path:INFO`, sender callsign, `NOGATE`/third-party (`:}`) skipped, blacklist (config `blacklist`, space-separated, `*` wildcard), self-frames skipped, duplicate suppression by sender+info hash for 25 s (also swallows our own digi echoes).
- `digiMode`: `0`=OFF (listen only), `1`=repeat packets whose path contains `WIDE1-1` (replace with own callsign `*`), `2`=`1` + WIDE2-n (replace `WIDE2-1` with own `*`, or `WIDE2-2` with own `*,WIDE2-1`). If the path already carries `*` or has no remaining hop, the packet is not repeated.
- Digi TX uses the same CAD-lite bounded LBT as the beacon (`cadActive`).
- Digi log line (always, when repeated): `{"aprs":"digi","from":"<SRC>","tx":true,"packet":"<full frame>"}`.
- Status counter: `radio.digiCount` = frames digipeated since boot.

## Status object
`{"ok":true,"status":{"version":..,"uptimeMs":..,"radio":{...},"sensors":{...},"display":bool}}`
radio block keys:
| key | meaning |
|---|---|
| state | `OFF` / `RX` / `TX` / `ERR` |
| err | last RadioLib error code (0 = none) |
| rxCount / txCount / digiCount | packets received / frames sent / digipeated (since boot) |
| lastRssi / lastSnr | last received packet (dBm / dB) |
| rxlog / txtest | current switch state |

sensors block: `wx`/`ina` (present), `tempC`, `hum`, `hPa`, `vbatDivV` (P0.31 divider), `inaBusV`, `inaMa` (signed). `display` = OLED present.

## Config persistence
- Stored on `set`/`factory_reset` in internal flash, two 4 KB pages at 0xE8000/0xE9000 (below the Adafruit bootloader at 0xEA000; the core InternalFileSystem at 0xED000 would corrupt the bootloader and is NOT used). Header: magic/version/seq + CRC32 over the JSON payload; alternate-page atomic writes; invalid/CRC-mismatch -> defaults.
- RF parameters are applied at boot from `config` (frequency/SF/CR/BW/power). Changing them via `set` takes effect on next reboot.

## Config object (keys, ranges, defaults)
See `src/config.h` (single source of truth). Digest (N-12).
Type legend (plain language): **entero** = whole number; **si/no** = on/off switch;
**texto** = free text; **decimal** = number with decimals; **opciones** = pick from a list.

### Nombres del menú OLED (amigables) ↔ clave técnica
The on-device menu shows friendly names; the JSON key is what USB/WebSerial uses.

| Menú OLED | clave JSON |
|---|---|
| Modo | mode |
| Enviar baliza ahora / Baliza de rastreador | (acciones) |
| Baliza cada (min) | beaconInterval |
| Baliza cada (seg) / Distancia mínima (m) | trackerIntervalSecs / trackerMinDistanceM |
| Perfil de movimiento | smartBeaconPreset |
| Enviar altitud / Ahorro de GPS / Dormir entre balizas | sendAltitude / gpsEco / trackerSleep |
| Frecuencia / Velocidad (SF) / Codificación (CR) | frequency / spreadingFactor / codingRate4 |
| Ancho de banda (kHz) / Potencia (dBm) / Escuchar antes de hablar | signalBandwidth / power / cadActive |
| Modo repetidor / Lista negra / Silenciar (no emitir) | digiMode / blacklist / txDisabled |
| Indicativo / Ruta (path) / Símbolo / Comentario | callsign / path / symbol / comment |
| Latitud / Longitud | latitude / longitude |
| Responder consultas | queriesEnabled |
| Rotar pantallas solo / Apagar pantalla (seg) / Avisos en pantalla | sceneAutoAdvance / screenTimeoutSecs / popups |
| Tensión de apagado / despertar (mV) | sleepCutMv / sleepWakeMv (el mismo par vale para LiPo y NiMH) |
| GPS en repetidor / Ahorro de GPS | gpsInDigi / gpsEco |
| Fijar coords actuales / Ver GPS | (acciones: guardan lat/lon del GPS o muestran fix) |
| Control por radio / Operadores autorizados | remoteEnabled / managers |
| Reiniciar / Modo grabación (USB) / Valores de fábrica / Borrado total | (acciones) |

| key | type | range | default |
|---|---|---|---|
| callsign | string (uppercased) | 3..15 chars | NOCALL-11 |
| tocall | string (uppercased, alnum, max 6) | device identifier sent as the AX.25 destination of every frame. `APLRG1` = the LoRa APRS ecosystem marker (aprs.fi then labels the station as “Ricardo, CA2RXU: ESP32 LoRa iGate”); `APZ???` = experimental range for unpublished projects (e.g. `APZFKT`); empty = APLRG1 | APLRG1 |
| path | string | "0"/WIDE1-1/WIDE1-1,WIDE2-1/WIDE1-1,WIDE2-2/WIDE2-1/WIDE2-2/RFONLY (fallback, see the three below) | WIDE1-1 |
| pathDigi | string | same list: hops asked for in digipeater mode | WIDE1-1 |
| pathTracker | string | same list: hops asked for in tracker mode | WIDE1-1,WIDE2-1 |
| pathBoth | string | same list: hops asked for in both mode | WIDE1-1,WIDE2-1 |
| comment / status | string | <=63 | "" |
| msgText | string | <=47 | "" |
| msgRetries | int | 0..5 (resends every 30 s while there is no ack) | 3 |
| overlay | string (1 char) | "/" primary, "\" alternate, 0-9/A-Z = alternate + overlay | "/" |
| posAmbiguity | int | 0..4 digits hidden in the position | 0 |
| compressedPos | bool | tracker beacon in compressed APRS form (shorter; APRS101 ch.9). Position and altitude are decoded everywhere, but **findu displays a wrong course/speed** for compressed frames (verified with aprslib: the frame itself is correct) | false |
| symbol | string (1st char) | — | # |
| beaconInterval | int (min) | >=10 | 15 |
| latitude / longitude | float | ±90 / ±180 | 0 |
| digiMode | int | 0=OFF,1=WIDE1-1,2=WIDE1-1+WIDE2-n | 2 |
| blacklist | string | space sep, `*` wildcard | "" |
| frequency | int (Hz) | 430000000..928000000 | 433775000 |
| spreadingFactor | int | 5..12 (SX1262) | 12 |
| codingRate4 | int | 5..8 | 5 |
| signalBandwidth | float (kHz) | 62.5/125/250/500 | 125 |
| power | int (dBm) | 2..22 (clamped 12 on E22P env) | 22 (12 E22P) |
| cadActive | bool | — | true |
| sendBatteryTelemetry | bool | — | false |
| telemetryIntervalMin | int (min) | 0..120 (0 = solo manual) | 10 |
| wxSensorActive | bool | — | false |
| heightCorrection | int (m) | >=0 | 0 |
| temperatureCorrection | float (C) | -5..5 | 0 |
| chipTempOffset | float (C) | -10..10 | -3 |
| sleepCutMv | int | 2500..4200 | 3400 (HT-RA62) / 3500 (E22P). The same pair works for **1S LiPo/Li-ion and 3x NiMH** (3.40 V = 1.13 V/cell, i.e. the end of the NiMH curve) |
| sleepWakeMv | int | 2600..4500 (snapped to LPCOMP n/16) | 3710 (3.71 V = 1.24 V/cell on NiMH, ~30-40% on LiPo) |
| txDisabled | bool | mute global TX | false |
| remoteEnabled | bool | accept RF remote commands | false |
| tncProtocol | int | USB TNC bridge: 0=off, 1=TNC2 text, 2=**KISS** (binary AX.25 + KISS framing, for APRSdroid, APRSIS32, LoRa APRS App). In KISS the host app commands: the node sends no automatic beacons/telemetry/weather, it only digipeats and transmits what the app gives it. AX.25 limits: 6-character callsigns, SSID 0-15 (reported, never truncated). USB JSON/CLI keep working in every mode | 0 |
| tncMode | bool (legacy) | Old boolean key, still accepted on input: `true` migrates to `tncProtocol = 1` (TNC2, what it used to be), `false` to 0, **never 2**. `configToJson()` still publishes it as a mirror (0 = off, anything else = on) | false |
| bleEnabled | bool | Bluetooth LE host link (KISS over the Nordic UART Service). **NOT WORKING in the current build** (2026-09-13): the value is stored but the BLE code is out of the compilation, so nothing happens either way. Factory value in `config.h` is **false**. When it works again: independent of `tncProtocol`, and turning it off stops advertising at once (no reboot) | false |
| blePin | string, 6 digits | Pairing PIN of the Bluetooth link (**stored but unused today**: see bleEnabled) (`Bluefruit.Security.setPIN`), shown on the OLED while a pairing is in progress. The KISS characteristics need an encrypted link with MITM, so an unpaired device can neither read nor write them. Rejected unless it is exactly 6 digits (0-9); a number is accepted too (`123456`) | "123456" |
| managers | string (uppercased) | space-separated callsigns | "" |
| mode | int | 0=digi, 1=tracker, 2=both | 0 |
| sceneAutoAdvance | bool | OLED scenes auto-rotate (~5 s) | true |
| screenTimeoutSecs | int | 0..3600 (0=never off) | 0 |
| popups | bool | OLED event popups | true |
| trackerIntervalSecs | int | 10..3600 (fixed interval / sleep-between) | 120 |
| trackerMinDistanceM | int | 0..5000 | 0 |
| smartBeaconPreset | int | 0=off, 1=human, 2=bike, 3=car | 0 |
| sendAltitude | bool | altitude in tracker beacon | true |
| gpsEco | bool | GPS duty-cycle when idle (20 s on / 120 s off, movement>=3 km/h keeps it on) | false (always on) |
| trackerSleep | bool | timed sleep between tracker beacons (advanced; bypasses GPS eco) | false |
| gpsInDigi | bool | digi mode: use GPS position instead of fixed coords | false |
| queriesEnabled | bool | answer ?APRS? / ?APRSP / ?APRSS / ?APRSD / ?IGATE? | false |

Notes:
- `set` is atomic: invalid input -> nothing changes (error reply).
- Bandwidth is sent in kHz (as CA2RXU); firmware converts to Hz for RadioLib.
- JSON keys match the web configurator `id`s (N-12) for a drop-in compatible form.

