// Faketec_APRS_Igate_EA2OY — main
// APRS-LoRa digipeater/tracker firmware for Faketec V1-V6 (nRF52840) + SX1262
// License: GPL-3.0
//
// Phases: config v1 + WebSerial protocol + CLI, SX1262 @433.775, persistence,
// beacon (WX/battery), digi core, sensors (INA219/AHT20/BMP280), OLED UI
// (scenes/popups/menu), tracker mode (GPS + SmartBeaconing), sleep/resilience.

#include <Arduino.h>
#include <RadioLib.h>

#include "aprs.h"
#include "button.h"
#include "config.h"
#include "diag.h"
#include "display.h"
#include "flog.h"
#include "gps.h"
#include "haptic.h"
#include "pins_board.h"
#include "power.h"
#include "protocol.h"
#include "radio.h"
#include "sensors.h"
#include "store.h"
#include "tnc.h"
#include "tracker.h"

static DigiConfig gConfig;
static ConfigProtocol gProtocol(gConfig);

// Ultimo valor de "GPS n/N" que se ha enseñado de la sesion "Fijar coords".
// Vive aqui fuera del bucle para poder limpiarlo cuando la sesion termina (ver el
// comentario en loop()): dentro del `if` no habia forma de resetearlo.
static uint8_t gSetCoordsShown = 255;

// --- KISS mode: the host application drives the node ------------------------
// OPERATOR DECISION (explicit request, 2026-09-13): while the USB TNC bridge is
// in KISS (cfg.tncProtocol == 2) the node must NOT put its own automatic packets
// on the air. No scheduled position beacons, no telemetry, no weather, no
// periodic status, no node-sent notices: the APP decides what is transmitted and
// when, and the node only sends what the host hands it. Digipeating is NOT
// affected (that is reception-driven, not a packet of our own), and neither is
// TNC2 (protocol 1), which keeps its beacons exactly as it always did.
// This one flag is the single gate every automatic transmission asks first.
static bool tncHostDriven() {
  // KISS manda cuando el selector esta en KISS Y no se ha abierto con kissoff.
  // `kissoff` (tncKissPause) devuelve el mando al operador sin tocar el selector:
  // el nodo vuelve a obedecer sus ordenes normales por USB. Ver tnc.h.
  return gConfig.tncProtocol == CFG_TNC_KISS && !tncKissPaused();
}

static void handleButton() {
  ButtonEvent ev = buttonPoll();
  // CAUTION con la luz: cualquier pulsacion del boton fisico (corta, larga o doble)
  // enciende la retroiluminacion 5 s, da igual que luego cambie de diapositiva o mande
  // baliza. Asi el segundo toque SIEMPRE vuelve a encenderla (antes se perdia si el
  // toque caia en la rama del doble toque, que no pasaba por displayNextScene()).
  if (ev != BTN_NONE) { displayBacklightKick(); displayBeep(); }
  if (ev == BTN_NONE) {
    // Boton CAPACITIVO (T-Echo P0.11): el operador lo pidio como el "toque largo"
    // equivalente (2026-09-15). Sin menu aun, actua como cambiar de diapositiva y
    // enciende la luz 5 s. Con deteccion de flanco: mantenerlo no repite paginas.
    // ★ ACTIVO en BAJO, y se le pone una resistencia interna de subida (INPUT_PULLUP):
    //   la pastilla tactil flota y sin resistencia el digitalRead no decanta a un nivel
    //   fiable (por eso ni en ALTO ni en BAJO daba señal de vida). Con el pull-up queda
    //   en ALTO en reposo y baja al tocarla (si hace tierra al poner el dedo).
#if defined(PIN_BTN_TOUCH)
    static bool gCapIni = false;
    if (!gCapIni) { pinMode(PIN_BTN_TOUCH, INPUT_PULLUP); gCapIni = true; }
    static bool gCapLast = false;
    const bool cap = (digitalRead(PIN_BTN_TOUCH) == LOW);   // activo en BAJO
    if (cap && !gCapLast) {
      displayBacklightKick();
      displayBeep();
      // ★ Vibracion en el toque CAPACITIVO (2026-09-15, peticion del operador): es el unico
      //   boton que NAVEGA, y con una pantalla de tinta que tarda 1,5 s por refresco no se
      //   sabe si el toque ha contado hasta que la pantalla cambia. El fisico NO vibra.
      hapticAviso(HAP_TOQUE);
      if (menuIsOpen()) menuNavigate();           // capacitivo navega en el menu
      else if (!displayIsOn()) displayWake();
      else displayNextScene();
    }
    gCapLast = cap;
#endif
    return;
  }

  if (ev == BTN_SHORT) {
    if (!displayIsOn()) {
      displayWake();
      if (!tncActive()) Serial.println(F("{\"button\":\"display\"}"));
    } else if (menuIsOpen()) {
      menuShort();
    } else {
      displayNextScene();
    }
    return;
  }

  if (ev == BTN_LONG) {
    if (!displayIsOn()) {
      displayWake();
    } else if (menuIsOpen()) {
      menuLong();  // enter / edit / execute / confirm
    } else {
      menuOpen();
    }
    return;
  }

  if (ev == BTN_DOUBLE) {
    // Double tap = manual beacon (its "field" gesture). Dentro del menu el doble toque NO
    // dispara nada descontrolado: si estas editando, solo cancela la edicion (vuelve a la
    // lista); si no estas editando, no hace nada. Antes hacia menuShort() dos veces, lo que
    // podia EJECUTAR DOS VECES una accion (dos balizas, reinicio, borrado) — incoherente.
    if (displayIsOn() && menuIsOpen()) {
      if (menuIsEditing()) menuEditCancel();
      return;
    }
    if (tncHostDriven()) return;  // KISS: the app sends, the node stays quiet
    // Doble toque = baliza de posicion a mano. Peticion del operador
    // (2026-09-13): si el GPS tiene fijacion se manda la posicion REAL; si no la
    // tiene, se manda la ULTIMA POSICION CONOCIDA (la del pico mas reciente,
    // recordada en la flash y con SU hora). Nunca se manda la posicion fija
    // configurada: esa no es una posicion real, y publicarla seria mentir en el
    // mapa. Si no hay nada recordado, no se manda nada y se avisa en pantalla.
    int16_t st;
    const char *what;
    if (gpsPowered() && gpsGet().fix) {
      st = aprsSendTrackerBeacon(gConfig, gpsGet());
      what = "tracker_beacon";
    } else if (trackerHasLastKnown()) {
      st = trackerBeaconLastKnown(gConfig);
      what = "last_known";
    } else {
      displayPopup("Sin posicion GPS conocida");
      st = -111;
      what = "none";
    }
    if (!tncActive()) {
      Serial.print(F("{\"button\":\""));
      Serial.print(what);
      Serial.print(F("\",\"tx\":"));
      Serial.print(st == RADIOLIB_ERR_NONE ? "true" : "false");
      Serial.print(F(",\"code\":"));
      Serial.print(st);
      Serial.println(F("}"));
    }
  }
}

/* DRENA EL BOTON: mira el boton varias veces seguidas durante unos milisegundos.
   PARA QUE (problema reportado por el operador, 2026-09-13: "el doble toque cuesta
   hacerlo"): dentro del bucle hay operaciones que BLOQUEAN — escribir en el
   registro de la flash espera a que el NVMC este listo (un borrado de pagina son
   ~85 ms), y una transmision de radio son cientos de ms. En esos ratos NADIE mira
   el boton, asi que un toque rapido (60-90 ms) puede caer entero dentro del atasco
   y perderse. Cuanto mas rapido es el doble toque, mas facil es que se pierda.
   Llamando aqui a buttonPoll() en rafaga, los flancos que llegaron durante el
   atasco se procesan en cuanto el bucle vuelve: el toque no se pierde.
   Cuesta microsegundos por vuelta, y solo se llama alrededor de lo que bloquea. */
static void drainButton() {
  const uint32_t fin = millis() + 12;
  while ((int32_t)(millis() - fin) < 0) {
    handleButton();
    delay(1);
  }
}



void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Faketec_APRS_Igate_EA2OY v" APP_VERSION_STR);
  Serial.println("APRS-LoRa digipeater/tracker (433 MHz)");
  Serial.println("WebSerial config protocol v1 (get/set/factory_reset/reboot/status/radio/beacon/telemetry)");
  configSetDefaults(gConfig);
  if (storeLoad(gConfig)) {
    Serial.println("config: loaded from flash");
  } else {
    Serial.println("config: defaults (none stored)");
  }
  aprsBindConfig(&gConfig);
  tncBindConfig(&gConfig);
  diagBindConfig(&gConfig);
  displayBindConfig(&gConfig);
  radioSetMuted(gConfig.txDisabled);

  powerInit();              // POFCON 2.2 V
  buttonInit();
  sensorsInit(gSensorCache);  // Wire.begin + INA219/AHT20/BMP280 + divider P0.31
  displayInit();              // SSD1306/SH1106 autodetect (before boot popup)

  // Radio first: if the boot check decides to sleep, the SX1262 must already be
  // initialised so it receives the SPI sleep command (NavaTastic lesson:
  // sleeping before radio init left the SX1262 listening at 5-10 mA).
  if (radioSetup(gConfig)) {
    Serial.print("radio: OK @");
    Serial.print(gConfig.frequencyHz);
    Serial.println(" Hz (SF12/BW125/CR4/5)");
    radioSetRxCallback(aprsHandleRadioPacket);
  } else {
    Serial.print("radio: init FAILED (err ");
    Serial.print(radioLastErr());
    Serial.println(")");
  }

  // ★ SEGUNDA PARTE DE LA PANTALLA, Y AQUI ES DONDE TIENE QUE IR (2026-09-15).
  //
  // En este core el objeto global `SPI` de Arduino vive en SPIM3, el MISMO periferico que
  // usa la pantalla de tinta electronica del T-Echo. radioSetup() llama a SPI.begin(), que
  // reconfigura ese periferico con los pines de la radio. Si la pantalla se configura antes
  // (como estaba), el arranque de la radio le quita el periferico y la pantalla se queda sin
  // recibir un solo byte: su pin BUSY no se mueve nunca y parece "muerta" sin estarlo.
  // Se comprobo leyendo los registros PSEL: al configurarla, P0.31/P0.29; despues de la
  // radio, P0.19/P0.22. Por eso la parte del panel va DESPUES.
  displayInitTrasRadio();

  powerBootCheck(gConfig);    // sleeps itself when the battery is too low
  trackerInit();              // GPS off until needed

  flogInit();
  flogSetEnabled(gConfig.mode != 0);
  // El numero de compilacion va en la linea de arranque del registro: asi, mirando un
// volcado, se sabe EXACTAMENTE que firmware escribio esas lineas.
flogLine("EVT boot v%s %s mode=%u", APP_VERSION_STR, APP_BUILD_NUM,
           (unsigned)gConfig.mode);

  // 2026-09-13 RESCATE: Bluetooth (ble_kiss.cpp/.h) has been taken OUT OF THE
  // BUILD -- the files are renamed to *.off, so the compiler and the linker
  // never see them and no Bluetooth/SoftDevice code can run. bleEnabled in the
  // stored config is now an inert value: nothing reads it.
  // (Before that, Bluetooth used to start here, last in setup().)

  displaySplash();  // 4 s boot splash (non-blocking)

  // ★ AVISO DE ARRANQUE por vibracion (2026-09-15). Va AQUI, al final del setup, y no
  //   cuando la pantalla esta lista: la pantalla de tinta tarda ~17 s (6 s de espera + dos
  //   refrescos completos), y el operador quiere saber YA que el nodo ha arrancado -- sobre
  //   todo desde que se despierta con el boton de RESET, que no da ninguna otra señal.
  hapticAviso(HAP_ARRANQUE);
}

void loop() {
  static uint32_t lastBeaconMs = 0;
  static uint32_t lastSensorsMs = 0;
  static uint8_t lastLogMode = 255;
  static bool pantallaArrancada = false;

  // ★ EL ARRANQUE DE LA PANTALLA DE TINTA ELECTRONICA, AQUI Y NO EN setup() (2026-09-15).
  //   Se hace a los 6 s de estar andando, cuando el USB ya esta enumerado y el nodo
  //   obedece. Si el panel se bloquea, el nodo sigue vivo y se puede diagnosticar (y volver
  //   a grabar por software) en vez de quedarse mudo. Ver displayArrancaPantalla().
  //   Desde el 2026-09-14 esta ACTIVADO: el driver ya no puede quedarse esperando para
  //   siempre (sus transferencias SPI llevan tope de tiempo y se abortan solas).
  if (!pantallaArrancada && millis() > 6000UL) {   // 6 s: el USB ya esta vivo de sobra
    pantallaArrancada = true;
    displayArrancaPantalla();
  }

  usbDiagTick(millis());   // latido del USB (diagnostico, ver protocol.h)
  gProtocol.feed(Serial);   // LEER EL USB: sin esta linea el nodo no obedece a nada
  drainButton();            // y drenar el boton despues

  // The trip log follows the working mode (on in tracker/both, off in digi).
  if (gConfig.mode != lastLogMode) {
    lastLogMode = gConfig.mode;
    flogSetEnabled(gConfig.mode != 0);
    flogLine("EVT mode=%u log=%s", (unsigned)gConfig.mode,
             gConfig.mode ? "on" : "off");
  }

  uint32_t now = millis();

  // ★ Aviso por VIBRACION al coger fijacion GPS (2026-09-15, idea del operador).
  //   No hace nada si el fix no ha cambiado desde la vuelta anterior, asi que se puede
  //   llamar siempre. Lleva dentro las dos guardas (solo al cogerla, y como mucho una vez
  //   por minuto): ver hapticTickGPS() en haptic.h.
  hapticTickGPS(gpsGet().fix);
  if (now - lastSensorsMs >= 2000) {
    lastSensorsMs = now;
    sensorsRead(gSensorCache);
    drainButton();   // I2C: puede tardar si un sensor no responde
    // Keep looking for an external probe while none is present (a module may
    // need time after power-up, or be connected later). Once found, its
    // readings take over automatically and the chip sensor stays as TINT.
    if (!sensorsHasExternal(gSensorCache)) {
      static uint32_t lastDetectMs = 0;
      if (lastDetectMs == 0 || now - lastDetectMs >= 60000u) {
        lastDetectMs = now;
        sensorsRetryDetect(gSensorCache);
      }
    }
    powerLoop(gConfig);  // anti-brownout monitor (2 s cadence, fix #1)
    drainButton();   // el powerLoop puede tardar leyendo la bateria
  }

  uint32_t intervalMs = (uint32_t)gConfig.beaconIntervalMin * 60000u;

  // GPS management: tracker modes use the tracker loop; in digipeater mode the
  // GPS is used only when "GPS en repetidor" is enabled (overrides fixed coords).
  // The tracker loop is an AUTOMATIC TRANSMITTER (first-fix, rate, distance,
  // corner and parked beacons), so while the host drives the node only the GPS
  // bookkeeping runs: in tracker modes gpsManage() takes that job (it is the
  // same call the digipeater makes). Hardware log, 2026-09-13: this call used to
  // sit outside the gate and the node still sent "TX TRK ... ok F" in KISS mode.
  if (gConfig.mode != 0 && !tncHostDriven()) {
    trackerLoop(gConfig, now);
  drainButton();   // el tracker puede TRANSMITIR aqui: cientos de ms sin mirar el boton
  } else {
    uint32_t elapsed = now - lastBeaconMs;
    uint32_t remain = (intervalMs > elapsed) ? (intervalMs - elapsed) : 0;
    gpsManage(gConfig, now, remain);
  }

  // Sesion "Fijar coordenadas actuales" del menu: avanza si esta en marcha
  // (enciende el GPS, espera fijacion, deja que se asiente, guarda y apaga).
  // Ver trackerSetCoordsStart()/Tick() en tracker.h.
  // ★ EL GPS LO GOBIERNA LA SESION, NO EL MODO (2026-09-15): por eso esto se llama
  //   siempre, tambien en modo repetidor, y es gpsManage() el que atiende a la
  //   sesion POR DELANTE del modo. Antes, en repetidor, no servia de nada salvo
  //   que estuviera activo "GPS en repetidor".
  trackerSetCoordsTick(now);
  if (trackerSetCoordsEstado() == TRK_COORDS_ASENTANDO) {
    // Progreso en pantalla: sin esto, tres minutos mirando una pantalla quieta
    // parecen un cuelgue. La OLED lo enseña tal cual; la tinta tiene su PROPIA
    // pantalla de sesion (con barra) y descarta este aviso a proposito, para no
    // repintar 20 veces un panel que tarda 1,5 s (ver displayPopup en
    // epaper_techo.cpp). El aviso se sigue mandando igual porque es tambien la
    // linea de estado por USB.
    // ★ El "ultimo valor enseñado" vive FUERA del bucle y se limpia al empezar la
    //   sesion (2026-09-15): antes era un static de dentro del `if` y se quedaba
    //   con el ultimo numero de la captura ANTERIOR, asi que si la nueva empezaba
    //   por el mismo numero no se veia el primer aviso.
    if (trackerSetCoordsDone() != gSetCoordsShown) {
      gSetCoordsShown = trackerSetCoordsDone();
      char b[32];
      snprintf(b, sizeof(b), "GPS %u/%u", (unsigned)gSetCoordsShown,
               (unsigned)trackerSetCoordsNeed());
      displayPopup(b);
    }
  } else if (gSetCoordsShown != 255) {
    gSetCoordsShown = 255;   // sesion terminada (o sin empezar): a cero para la proxima
  }

  // ===================== KISS mode: no automatic packets =====================
  // (Operator decision, see tncHostDriven() at the top of this file.) Everything
  // below, down to the matching comment at the end, is a packet the NODE sends
  // on its own: tracker beacons, the boot and periodic position beacon, the
  // status notices, the battery telemetry and the weather report. While the KISS
  // protocol is on, none of it may go out: the host app is the one that commands,
  // and a node beaconing on its own would publish a position the operator did not
  // ask for. Reception-driven work (digipeating in aprsHandleRadioPacket, the USB
  // KISS path in tnc.cpp) is deliberately OUTSIDE this gate, as are the CLI, the
  // web panel and the OLED menu, which are the operator asking for one packet.
  if (!tncHostDriven()) {

  // Baliza de arranque. CAMBIO DE COMPORTAMIENTO (operador, 2026-09-13):
  //   - modo repetidor (0): igual que siempre. Anuncia su posicion configurada
  //     poco despues de arrancar y luego cada intervalMs. Un repetidor fijo
  //     tiene que decir donde esta: esa posicion es su razon de ser.
  //   - modo rastreador (1) y dual (2): NUNCA se publica una posicion que no sea
  //     real. Sin fijacion no sale posicion: ni la fija configurada, ni una
  //     "ultima oportunidad" a los 10 minutos. Sale solo el aviso "En marcha,
  //     buscando satelites" (para que se sepa que el nodo esta vivo) y, en cuanto
  //     el GPS fija, el rastreador empieza a mandar posiciones de verdad.
  //     Si el operador quiere mandar la ultima posicion conocida a mano, tiene el
  //     doble toque del boton (ver handleButton).
  // NOTE: the first one waits a few seconds on purpose. Transmitting blocks the
  // loop for several seconds (listen-before-talk + SF12 airtime) and would eat
  // the 4 s boot splash, leaving a black screen.
  constexpr uint32_t kBootBeaconDelayMs = 7000;
  const bool trackerMode = (gConfig.mode != 0);
  bool trackerOwnsPosition = trackerMode && gpsPowered() && gpsGet().fix;
  static bool bootBeaconDone = false;
  // Aviso de estado "GPS OK" en modo rastreador: una sola vez por arranque (lo
  // enciende el bloque de abajo, en cuanto hay fix). Vive aqui, al lado del
  // one-shot del arranque, y en modo 0 nunca se toca.
  static bool gpsOkStatusSent = false;
  if (radioReady() && !gConfig.txDisabled && !trackerOwnsPosition &&
      intervalMs >= 600000u) {
    const bool bootDue = !bootBeaconDone && now >= kBootBeaconDelayMs;
    const bool periodicDue =
        !trackerMode && bootBeaconDone && now - lastBeaconMs >= intervalMs;
    if (bootDue || periodicDue) {
      bootBeaconDone = true;
      lastBeaconMs = now;
      bool sent = false;
      int16_t st = RADIOLIB_ERR_NONE;
      if (trackerMode) {
        // Sin fijacion (con fijacion este bloque ni se toca): NUNCA se adivina
        // una posicion. Solo se avisa de que el nodo esta en marcha y buscando.
        if (!gpsOkStatusSent && aprsCanBeacon(gConfig)) {
          st = aprsSendStatus(gConfig, "En marcha, buscando satelites");
          sent = true;
        }
      } else if (gConfig.mode == 0 && gConfig.gpsInDigi && gpsPowered() &&
                 gpsGet().fix) {
        st = aprsSendTrackerBeacon(gConfig, gpsGet());  // GPS overrides fixed
        sent = true;
      } else if (aprsCanBeacon(gConfig)) {
        st = aprsSendBeacon(gConfig);
        sent = true;
      }
      if (sent && !tncActive()) {
        Serial.print(F("{\"radio\":\"beacon\",\"auto\":true,\"tx\":"));
        Serial.print(st == RADIOLIB_ERR_NONE ? "true" : "false");
        Serial.print(F(",\"code\":"));
        Serial.print(st);
        Serial.println(F("}"));
      }
    }
  }

  // Aviso "GPS OK" del modo rastreador (1/2), UNA sola vez por arranque: sale
  // cuando ya hay fix al arrancar (entonces el bloque de arriba ni se toca) y,
  // si no lo habia, en cuanto el GPS fija. Motivo: los clientes APRS conservan
  // el ultimo estado recibido, asi que sin este "GPS OK" el aviso "En marcha,
  // buscando satelites" se quedaba horas en el mapa aunque la posicion ya fuese
  // correcta. Mismos guardas que el aviso de arranque (radio lista, sin mute,
  // aprsCanBeacon) y nunca se repite: con el fix en la mano no vuelve a salir.
  if (trackerMode && !gpsOkStatusSent && radioReady() && !gConfig.txDisabled &&
      gpsPowered() && gpsGet().fix && aprsCanBeacon(gConfig)) {
    gpsOkStatusSent = true;  // one shot: no se repite mientras se mantenga el fix
    aprsSendStatus(gConfig, "GPS OK");
  }

  // Periodic telemetry, the APRS way: channels + metadata every N minutes
  // (metadata every 10th sequence inside aprsSendTelemetry). The first one also
  // waits: transmitting blocks the loop for seconds and would swallow the boot
  // splash and the startup beacon.
  constexpr uint32_t kFirstTelemDelayMs = 13000;
  static uint32_t lastTelemMs = 0;
  if (gConfig.sendBatteryTelemetry && gConfig.telemetryIntervalMin > 0 &&
      radioReady() && !gConfig.txDisabled && now >= kFirstTelemDelayMs) {
    const uint32_t telemMs = (uint32_t)gConfig.telemetryIntervalMin * 60000u;
    if (lastTelemMs == 0 || now - lastTelemMs >= telemMs) {
      lastTelemMs = now;
      aprsSendTelemetry(gConfig);
    }
  }

  // Weather packet of its own: every wxIntervalMin minutes once the GPS provides the
  // stamp (retries every minute until then). This is what makes the station appear as
  // a weather station in aprs.fi / findu.
  // ★ ANTES IBA FIJO A 15 MINUTOS (900000 ms) escrito aqui dentro (2026-09-15): en un
  //   repetidor fijo eso es aire tirado. Ahora manda el ajuste del operador
  //   (wxIntervalMin, por defecto 55) y hay TRES intervalos desincronizados: baliza 30,
  //   telemetria 53 y meteo 55, sin factores comunes, para que no coincidan.
  //   0 = NO automatico: no se programa ningun envio y el paquete solo sale a mano
  //   (el comando `wx` del CLI). El reintento de 60 s de abajo NO cambia: sigue igual
  //   mientras el envio falle (todavia no hay sello de hora del GPS).
  static uint32_t nextWxMs = 0;
  if (gConfig.wxSensorActive && gConfig.wxIntervalMin > 0 && radioReady() &&
      !gConfig.txDisabled && (int32_t)(now - nextWxMs) >= 0) {
    int16_t wxSt = aprsSendWeather(gConfig);
    nextWxMs = now + ((wxSt == RADIOLIB_ERR_NONE)
                          ? (uint32_t)gConfig.wxIntervalMin * 60000u
                          : 60000u);
  }

  // Status packet: once at boot, then every 24 h (CA2RXU behaviour).
  static uint32_t lastStatusMs = 0;
  if (gConfig.status[0] != '\0' && radioReady() && !gConfig.txDisabled &&
      (lastStatusMs == 0 || now - lastStatusMs >= 86400000u)) {
    lastStatusMs = now;
    aprsSendStatus(gConfig);
  }

  }  // ============ end of the "no automatic packets in KISS" gate ============

  radioLoop();
  diagLoop();
  // (Bluetooth byte pump + pairing splash used to run here: bleLoop(). It is
  // out of the build for now, see the note in setup().)
  // Resending an unacknowledged message is a packet the node decides to send on
  // its own, so the same KISS rule applies (see tncHostDriven()).
  if (!tncHostDriven()) aprsMsgTick(gConfig, now);
  handleButton();
  drainButton();   // la pantalla se dibuja por I2C: unos ms sin mirar el boton
  displayRefresh(gConfig, radioRxCount(), radioTxCount(), aprsDigiCount(),
                 gSensorCache);
  // Retroiluminacion de la tinta (T-Echo): la enciende el boton y esto la apaga sola
  // cuando vencen los 5 s.
  displayBacklightTick(millis());
  delay(5);
}
