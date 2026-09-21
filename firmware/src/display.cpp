// display.cpp - OLED diagnostics UI (SSD1306/SH1106 128x64 autodetect).
// Phase A/B: Meshtastic-style scenes (auto-advance + manual), event popups,
// screen timeout, event rings and a schema-driven on-device menu (every config
// key + actions). Probe pattern from Meshtastic ScanI2C (N-05). License: GPL-3.0
//
// OJO, ESTE FICHERO ES SOLO PARA PLACAS CON OLED (las Faketec). Las que no la
// tienen (LilyGO T-Echo, que lleva tinta electronica) compilan en su lugar
// `display_epaper.cpp`, que implementa las mismas funciones sin dibujar nada.
// El interruptor es `HAS_OLED` (1 en la Faketec, 0 en el T-Echo).

#include "display.h"

#if HAS_OLED

#include <algorithm>  // std::min/max used by the SSD1306/SH1106 lib headers
#include <Arduino.h>
#include <ArduinoJson.h>
#include <RadioLib.h>
#include <SH1106Wire.h>
#include <SSD1306Wire.h>
#include <math.h>
#include <nrf.h>
#include <stdio.h>
#include <string.h>

#include "aprs.h"
#include "diag.h"     // diagTrazaArranque(): el saludo del arranque no se cuela en modo TNC
#include "gps.h"
#include "power.h"
#include "radio.h"
#include "store.h"
#include "tnc.h"
#include "tracker.h"

namespace {

constexpr int kSda = 36;  // P1.04
constexpr int kScl = 11;  // P0.11
constexpr uint8_t kAddrs[2] = {0x3C, 0x3D};
constexpr int kI2cFreq = 400000;  // AHT20 max 400 kHz

constexpr uint32_t kSceneAutoMs = 4000;  // auto-advance per scene

// Menu auto-close: after this long without touching the button the menu closes
// and the screen goes back to the usual slideshow (operator request,
// 2026-09-13). It is deliberately SHORTER than the default screen timeout
// (30 s), so a node left alone comes back to the carousel on its own instead of
// sitting on the menu forever. The last kMenuCountdownMs show the seconds left.
constexpr uint32_t kMenuIdleCloseMs = 20000;
constexpr uint32_t kMenuCountdownMs = 3000;
constexpr uint32_t kAutoAdvanceHoldMs = 15000;  // pause after a manual change
constexpr uint32_t kPopupMs = 1500;
constexpr uint32_t kTxBadgeMs = 3000;  // keep the TX badge on after a send
constexpr uint32_t kSplashMs = 4000;
constexpr uint8_t kRingSize = 5;
constexpr uint8_t kSceneCount = 8;
constexpr int kPopupMaxLines = 3;    // popup box: wrapped, centred lines
constexpr int kPopupMaxChars = 48;

SSD1306Wire *gSsd = nullptr;
SH1106Wire *gSh = nullptr;
OLEDDisplay *gOled = nullptr;
bool gDisplayOn = false;

DigiConfig *gCfg = nullptr;

uint8_t gScene = 0;
uint32_t gLastSceneMs = 0;
uint32_t gLastDrawMs = 0;
uint32_t gLastActivityMs = 0;
uint32_t gSplashUntil = 0;
uint32_t gAutoAdvanceHoldUntil = 0;  // auto-advance paused until this moment

// Bluetooth pairing splash: the PIN, shown while a pairing is in progress. It
// is armed by ble.cpp (main loop) and expires on its own after kPinSplashMaxMs
// even if no event ever arrives, so the panel cannot get stuck on it.
constexpr uint32_t kPinSplashMaxMs = 60000;
char gPinSplash[8] = "";
bool gPinSplashFromStack = false;
uint32_t gPinSplashUntil = 0;

char gPopup[40] = "";
uint32_t gPopupUntil = 0;
uint32_t gPopupCountdownUntil = 0;  // > now: show the remaining seconds

struct RxEntry {
  char call[12];
  float rssi;
  float snr;
  char kind[6];
  uint32_t ms;
};
RxEntry gRxRing[kRingSize];
uint8_t gRxHead = 0, gRxUsed = 0;

struct TxEntry {
  char what[10];
  char call[12];
  uint32_t ms;
};
TxEntry gTxRing[kRingSize];
uint8_t gTxHead = 0, gTxUsed = 0;

// ---------------------------------------------------------------- menu ------

enum MenuKind {
  MK_HEADER,
  MK_INT,
  MK_BOOL,
  MK_ENUM,
  MK_ENUM_F,
  MK_FLOAT,
  MK_STRING,
  MK_PATH,   // lista de rutas APRS: se elige a toques, sin escribir letra a letra
  // Lista CORTA de opciones (el modo de trabajo, el perfil de movimiento, el modo
  // repetidor): se recorren a toques y el boton largo GUARDA Y SALE. Peticion del
  // operador (2026-09-13): con el editor de letras habia que recorrer el abecedario
  // para elegir "Rastreador", que es una tortura con un solo boton.
  MK_ENUM_CYCLE,
  MK_ACTION,
};

enum {
  ACT_NONE = 0,
  ACT_BEACON,
  ACT_TELEM,
  ACT_TELEM_META,
  ACT_MUTE,
  ACT_SLEEP,
  ACT_REBOOT,
  ACT_DFU,
  ACT_RESET,
  ACT_WIPE,
  ACT_TRACKER_BEACON,
  ACT_BAT,
  ACT_SET_COORDS,
  ACT_GPS_INFO,
  ACT_MSG_SEND,
  ACT_PROFILE_ICON,   // ★ icono del mapa del perfil activo (2026-09-15)
};

struct MenuItem {
  const char *label;
  MenuKind kind;
  const char *key;
  int min, max, step;
  const char *const *opts;
  const int *optVals;
  const float *optValsF;
  int action;
  // En que modos de trabajo sale esta fila (mascara: bit 0 = repetidor,
  // bit 1 = rastreador, bit 2 = ambos). 0x07 = en todos.
  //
  // PARA QUE SIRVE (peticion del operador, 2026-09-13): habia ajustes que se
  // podian cambiar pero NO hacian nada en el modo actual, asi que el usuario los
  // tocaba y no pasaba nada (el peor tipo de ajuste). Ahora esos se ocultan.
  // Ejemplo: "Ahorro de GPS" y "GPS en repetidor" solo actuan en modo repetidor
  // (en rastreador el GPS va siempre encendido, que es lo correcto), asi que en
  // rastreador y en ambos ya no aparecen.
  uint8_t modes;
};

// Mascaras de modo, para que las filas se lean solas.
constexpr uint8_t kMDigi = 0x01;   // solo repetidor
constexpr uint8_t kMTrk = 0x02;    // solo rastreador
constexpr uint8_t kMBoth = 0x04;   // solo ambos
constexpr uint8_t kMAll = 0x07;    // siempre

const char *kModeOpts[] = {"Repetidor", "Rastreador", "Ambos", nullptr};
const int kModeVals[] = {0, 1, 2};
const char *kSmartOpts[] = {"Sin perfil", "A pie", "Bici", "Coche", nullptr};
const int kSmartVals[] = {0, 1, 2, 3};
const char *kDigiOpts[] = {"Apagado", "WIDE1-1", "WIDE1+WIDE2", nullptr};
const int kDigiVals[] = {0, 1, 2};
// USB TNC bridge: the same three positions as cfg.tncProtocol (0/1/2 in config.h).
const char *kTncOpts[] = {"Apagado", "TNC2 texto", "KISS", nullptr};
const int kTncVals[] = {CFG_TNC_OFF, CFG_TNC_TNC2, CFG_TNC_KISS};
const char *kFreqOpts[] = {"433.775", "433.900", "868.200", nullptr};
const int kFreqVals[] = {433775000, 433900000, 868200000};
const char *kBwOpts[] = {"62.5", "125", "250", "500", nullptr};
  const float kBwVals[] = {62.5f, 125.0f, 250.0f, 500.0f};
// Rutas APRS admitidas (campo "path"): de menos a más saltos.
const char *kPathOpts[] = {"0", "WIDE1-1", "WIDE1-1,WIDE2-1", "WIDE1-1,WIDE2-2",
                           "WIDE2-1", "WIDE2-2", "RFONLY", nullptr};

// ★★ ICONO DEL MAPA POR PERFIL DE USO (2026-09-15) ★★
// El firmware tiene CUATRO perfiles (0=fijo/digi, 1=peaton, 2=bici, 3=coche) y el
// icono que sale en el mapa va con el perfil activo (cfg.profileSymbol[] /
// profileOverlay[], ver config.h y aprsProfileIcon() en aprs.cpp). Aqui van los
// CUATRO iconos que se pueden elegir, con el NOMBRE que se enseña en la pantalla:
// el PAR de codigos ("/#", "/[", "/b", "/>") no se escribe a mano con un solo
// boton, se elige de esta lista.
// Códigos comprobados en la tabla de WA8LMF (la que usa go-aprs) y en los apuntes
// de overlays de aprs.org; el detalle y las fuentes estan en config.h.
const char *kIconoNombre[] = {"Repetidor", "Persona", "Bici", "Coche", nullptr};
const char kIconoCodigo[] = {'#', '[', 'b', '>'};
constexpr int kIconoOpciones = 4;

  const MenuItem kMenu[] = {
    {"== MODO DE TRABAJO ==", MK_HEADER, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Modo", MK_ENUM_CYCLE, "mode", 0, 2, 1, kModeOpts, kModeVals, nullptr, 0, kMAll},

    {"== GPS ==", MK_HEADER, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    // Solo en modo repetidor: en rastreador y en ambos el GPS va SIEMPRE
    // encendido (a proposito), asi que estos dos ajustes no harian nada alli.
    // Ocultarlos evita el peor tipo de ajuste: el que se puede tocar y no hace
    // nada. Peticion del operador, 2026-09-13.
    {"GPS en repetidor", MK_BOOL, "gpsInDigi", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMDigi},
    {"Ahorro de GPS", MK_BOOL, "gpsEco", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMDigi},
    {"Fijar coords actuales", MK_ACTION, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, ACT_SET_COORDS, kMAll},
    {"Ver GPS", MK_ACTION, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, ACT_GPS_INFO, kMAll},

    {"== BALIZAS ==", MK_HEADER, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Enviar baliza ahora", MK_ACTION, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, ACT_BEACON, kMAll},
    {"Baliza de rastreador", MK_ACTION, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, ACT_TRACKER_BEACON, kMAll},
    {"Enviar telemetria", MK_ACTION, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, ACT_TELEM, kMAll},
    {"Formato de telemetria", MK_ACTION, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, ACT_TELEM_META, kMAll},
    {"Baliza cada (min)", MK_INT, "beaconInterval", 15, 240, 5, nullptr, nullptr, nullptr, 0, kMAll},
    {"Posicion comprimida", MK_BOOL, "compressedPos", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Ocultar digitos (0-4)", MK_INT, "posAmbiguity", 0, 4, 1, nullptr, nullptr, nullptr, 0, kMAll},

    {"== MENSAJES ==", MK_HEADER, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Mensaje rapido", MK_STRING, "msgText", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Reintentos sin acuse", MK_INT, "msgRetries", 0, 5, 1, nullptr, nullptr, nullptr, 0, kMAll},
    {"Enviar al ultimo oido", MK_ACTION, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, ACT_MSG_SEND, kMAll},

    {"== RASTREADOR ==", MK_HEADER, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Baliza cada (seg)", MK_INT, "trackerIntervalSecs", 10, 3600, 10, nullptr, nullptr, nullptr, 0, kMAll},
    {"Distancia (m) cada", MK_INT, "trackerMinDistanceM", 0, 5000, 50, nullptr, nullptr, nullptr, 0, kMAll},
    {"Tiempo minimo (seg)", MK_INT, "trackerMinSpacing", 10, 120, 5, nullptr, nullptr, nullptr, 0, kMAll},
    {"Perfil de movimiento", MK_ENUM_CYCLE, "smartBeaconPreset", 0, 3, 1, kSmartOpts, kSmartVals, nullptr, 0, kMAll},
    // ★ ICONO DEL MAPA DEL PERFIL ACTIVO (2026-09-15): la fila va JUSTO DEBAJO del
    //   perfil, que es donde el operador lo busca. Un toque corto (entrar) recorre
    //   los cuatro iconos y lo GUARDA en el perfil activo; el aviso dice cual ha
    //   quedado. Se hace desde el perfil ACTIVO y no desde una lista de los cuatro
    //   perfiles porque este menu tiene un solo boton: primero se elige el perfil
    //   (la fila de arriba) y luego el icono que le toca.
    {"Icono del perfil", MK_ACTION, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, ACT_PROFILE_ICON, kMAll},
    {"Enviar altitud", MK_BOOL, "sendAltitude", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Dormir entre balizas", MK_BOOL, "trackerSleep", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},

    {"== RADIO ==", MK_HEADER, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Frecuencia", MK_ENUM, "frequency", 0, 0, 0, kFreqOpts, kFreqVals, nullptr, 0, kMAll},
    {"Velocidad (SF)", MK_INT, "spreadingFactor", 5, 12, 1, nullptr, nullptr, nullptr, 0, kMAll},
    {"Codificacion (CR)", MK_INT, "codingRate4", 5, 8, 1, nullptr, nullptr, nullptr, 0, kMAll},
    {"Ancho de banda (kHz)", MK_ENUM_F, "signalBandwidth", 0, 0, 0, kBwOpts, nullptr, kBwVals, 0, kMAll},
    {"Potencia (dBm)", MK_INT, "power", 2, 22, 1, nullptr, nullptr, nullptr, 0, kMAll},
    {"Escuchar antes de hablar", MK_BOOL, "cadActive", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},

    {"== REPETIDOR ==", MK_HEADER, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Modo repetidor", MK_ENUM_CYCLE, "digiMode", 0, 2, 1, kDigiOpts, kDigiVals, nullptr, 0, kMAll},
    {"Lista negra", MK_STRING, "blacklist", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Silenciar (no emitir)", MK_BOOL, "txDisabled", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},

    {"== APRS ==", MK_HEADER, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Indicativo", MK_STRING, "callsign", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Identificador (tocall)", MK_STRING, "tocall", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Ruta (path)", MK_STRING, "path", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Ruta en repetidor", MK_PATH, "pathDigi", 0, 0, 0, kPathOpts, nullptr, nullptr, 0, kMAll},
    {"Ruta en rastreador", MK_PATH, "pathTracker", 0, 0, 0, kPathOpts, nullptr, nullptr, 0, kMAll},
    {"Ruta en ambos", MK_PATH, "pathBoth", 0, 0, 0, kPathOpts, nullptr, nullptr, 0, kMAll},
  {"Simbolo", MK_STRING, "symbol", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Comentario", MK_STRING, "comment", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Latitud", MK_FLOAT, "latitude", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Longitud", MK_FLOAT, "longitude", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Responder consultas", MK_BOOL, "queriesEnabled", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
  {"Modo TNC (USB)", MK_ENUM, "tncProtocol", 0, 2, 1, kTncOpts, kTncVals, nullptr, 0, kMAll},

    {"== BLUETOOTH ==", MK_HEADER, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    // 2026-09-13 RESCATE: Bluetooth is out of the build (ble_kiss.cpp/.h are
    // renamed to *.off), so these two entries are INERT values in the stored
    // config: the menu still shows and edits them, nothing reads them. The rest
    // of the firmware is untouched and keeps working exactly as before.
    {"Bluetooth", MK_BOOL, "bleEnabled", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    // Pairing PIN, 6 digits (that is what Bluetooth asks for when it exists).
    {"PIN Bluetooth", MK_STRING, "blePin", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},

    {"== SENSORES ==", MK_HEADER, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Enviar tiempo (WX)", MK_BOOL, "wxSensorActive", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Enviar telemetria", MK_BOOL, "sendBatteryTelemetry", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Telemetria cada (min)", MK_INT, "telemetryIntervalMin", 0, 720, 15, nullptr, nullptr, nullptr, 0, kMAll},
    // ★ INTERVALO DE METEOROLOGIA (2026-09-15): ajuste NUEVO, al lado del de telemetria
    //   y con su mismo formato (MK_INT, 0..720, paso 15: 0 = no automatico). El paquete
    //   WX iba fijo a 15 minutos dentro de main.cpp y no se podia cambiar.
    {"Meteorologia cada (min)", MK_INT, "wxIntervalMin", 0, 720, 15, nullptr, nullptr, nullptr, 0, kMAll},
    {"Corregir sonda (C)", MK_FLOAT, "temperatureCorrection", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Ajuste chip (C)", MK_FLOAT, "chipTempOffset", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},

    {"== PANTALLA ==", MK_HEADER, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Rotar pantallas solo", MK_BOOL, "sceneAutoAdvance", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Apagar pantalla (seg)", MK_INT, "screenTimeoutSecs", 0, 3600, 15, nullptr, nullptr, nullptr, 0, kMAll},
    {"Avisos en pantalla", MK_BOOL, "popups", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},

    {"== ENERGIA ==", MK_HEADER, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Tension de apagado (mV)", MK_INT, "sleepCutMv", 2500, 4200, 50, nullptr, nullptr, nullptr, 0, kMAll},
    {"Tension de despertar (mV)", MK_INT, "sleepWakeMv", 2600, 4500, 50, nullptr, nullptr, nullptr, 0, kMAll},
    {"Ver bateria", MK_ACTION, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, ACT_BAT, kMAll},
    {"Apagar (dormir)", MK_ACTION, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, ACT_SLEEP, kMAll},

    {"== CONTROL REMOTO ==", MK_HEADER, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Control por radio", MK_BOOL, "remoteEnabled", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Operadores autorizados", MK_STRING, "managers", 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Silenciar / Reactivar", MK_ACTION, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, ACT_MUTE, kMAll},

    {"== AJUSTES ==", MK_HEADER, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, 0, kMAll},
    {"Reiniciar", MK_ACTION, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, ACT_REBOOT, kMAll},
    {"Modo grabacion (USB)", MK_ACTION, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, ACT_DFU, kMAll},
    {"Valores de fabrica", MK_ACTION, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, ACT_RESET, kMAll},
    {"Borrado total (solo USB)", MK_ACTION, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, ACT_WIPE, kMAll},
};

constexpr int kMenuCount = (int)(sizeof(kMenu) / sizeof(kMenu[0]));
// FILAS VISIBLES del menu. Bajado de 5 a 4 para poder usar el mismo alto de fila
// que la pantalla del modo de trabajo (12 px con el texto a 1 px), que es la que el
// operador dijo que se ve BIEN: "los textos estan bonitos y centrados dentro del
// recuadro". Con 5 filas de 10 px el recuadro quedaba del alto justo de la letra y
// el texto parecia pegado a los bordes. La lista se desplaza igual que antes.
/* ============================================================================
   MAQUETACION DE TODAS LAS PANTALLAS (una sola regla, 2026-09-13)
   El operador lo pidio asi: "no solamente ese menu, sino TODO el sistema de
   menus". Antes cada pantalla colocaba el texto a su aire — en el splash las
   lineas iban con saltos de 18, 12, 10 y 8 px, y el menu con filas de 10 px y el
   texto pegado al borde del recuadro — y el resultado era que unas se veian bien
   y otras apelotonadas.
   REGLA UNICA: cada linea va en una banda de kRowH px de alto, con el texto
   kTextOff px dentro (1 px de aire arriba y abajo). Esto es exactamente lo que
   hace la pantalla del modo de trabajo, que es la que el operador aprobo:
   "los textos estan bonitos y centrados dentro del recuadro".
   ============================================================================ */
constexpr int kRowH = 10;      // alto de una banda de texto
constexpr int kTextOff = 1;    // (kRowH - 10 de letra) / 2, redondeado a 1
constexpr int kRowsTop = 14;   // primera banda, debajo de la barra de titulo
// y = kRowsTop + fila * kRowH
//
// ★★ EL PASO BAJO DE 12 A 10 PX (2026-09-15): ARREGLA EL PIE ★★
// CON LA LETRA DE VERDAD DELANTE (tablas de ArialMT_Plain_10, caja de 13 px): la
// tinta de una linea va de y+3 a y+10 (mayusculas, digitos y las minusculas sin
// cola), y la libreria ADEMAS deja escrito el byte de la fila +8 en la pagina
// siguiente, que pinta arrastre en y+11..y+15. Con el paso viejo de 12 px:
//   - las cuatro bandas desde y=14 acababan en y=51, o sea tinta hasta y=61 y
//     arrastre hasta y=66: el porcentaje de la bateria (y=54) y el cuerpo de la
//     barra (y=57..63) le caian ENCIMA en 5 de las 8 escenas;
//   - y las escenas de lista (paso 10 a mano) acababan en y=44, tinta hasta
//     y=54: justo donde empieza el porcentaje.
// Con el paso de 10 px: bandas en y=15, 25, 35 y 45. La LETRA de las escenas
// (mayusculas, digitos y minusculas sin cola) acaba en y=55 y las colas ('y', 'g',
// 'm', 'p') bajan hasta y=57-59 en las escenas de lista. OJO: ademas de la letra,
// la libreria deja escritos los bytes de RELLENO de cada glifo (los que caen por
// debajo de su caja de 13 px, ver `glifos.py`), y esos rellenos llegan hasta y=63
// en la columna de cada caracter: por eso el pie no se puede subir mas. El
// porcentaje de la bateria se dibuja en y=54 (su primera tinta cae en la fila 57 y
// a la derecha del todo: x>=100) y el cuerpo de la barra en y=58..63, a la
// izquierda de ese texto. Medido con el simulador: 0 px de tinta fuera de la
// pantalla en las 8 escenas y 0 px de roce con el visor. El detalle, en el
// comentario de drawBatteryFooter.
// ES EL MISMO PASO QUE YA USABAN LAS ESCENAS DE LISTA (14 + i*10) y el mismo que
// el menu (fila de 12 px con el texto a 1), asi que ahora la regla es una sola.
// OJO SI ALGUIEN TOCA ESTOS TEXTOS: sigue sin haber margen para estirar; el
// porque de cada numero esta en el comentario del pie (drawBatteryFooter).
// EL PIE ES APARTE (visor de diapositivas + bateria): el visor ocupa las filas
// 62..63 (marca de 2 px la activa, 1 px las demas) y la bateria va a la derecha,
// de x=89 en adelante en su peor caso. Con el paso de 10 no se cruzan con
// ninguna de las 8 escenas.
constexpr int kRowsTopSplash = 2;   // el splash no lleva barra de titulo
constexpr int kMenuRows = 4;

bool gMenuOpen = false;

/* SUBLISTA DE OPCIONES (peticion del operador, 2026-09-13):
   "deberian aparecer las tres opciones, no solo una; el usuario se mueve a la que
   quiere y confirma". Al pulsar largo en un campo de lista corta (modo de trabajo,
   perfil de movimiento, modo repetidor) se abre esta pantalla con TODAS las
   opciones a la vista; el toque corto mueve el cursor y el toque largo confirma.
   El doble toque (dos toques cortos seguidos) baja dos, que es lo comodo. */
struct MenuSub {
  bool activa;
  const char *titulo;
  const char *const *opts;
  const int *vals;
  const char *key;
  int sel;
};
MenuSub gSub = {false, nullptr, nullptr, nullptr, nullptr, 0};

/* PANTALLA DEL MODO DE TRABAJO (rediseñada a peticion del operador, 2026-09-13:
   "las 3 opciones seleccionables mas el volver deberian estar al ENTRAR en modo de
   trabajo, no en un submenu que se entra desde una unica opcion; no tiene sentido").
   Asi que al entrar en la categoria "MODO DE TRABAJO" no se enseña una lista con
   una sola fila "Modo", sino las TRES opciones a la vez mas "< Volver":
     - toque corto: mueve el cursor por las tres (y por Volver)
     - toque largo: CONFIRMA la elegida (la guarda) y vuelve a la lista de categorias
     - toque largo en "Volver": sale sin cambiar nada
     - doble toque: dos toques cortos, o sea que baja dos
   La marca gModoFlag recuerda en que modo estaba al entrar, para poder señalar la
   eleccion actual incluso despues de moverse con el cursor. */
bool gModoFlag[3] = {false, false, false};   // repetidor / rastreador / ambos
int gModoRow = 0;                            // 0..2 modos, 3 = "Volver"
// Two-level menu (only one button on the board): gMenuCat < 0 = category list,
// otherwise the absolute index of the "== CATEGORY ==" header being browsed.
// gMenuSel/gMenuTop are VIEW indexes (not absolute), so both levels scroll the
// same way and the last row is always the virtual "Salir" / "Volver".
int gMenuCat = -1;
int gMenuSel = 0;
int gMenuTop = 0;
int gMenuCatRow = 1;  // category row to restore when coming back from a submenu
bool gEditing = false;
int gEditPos = 0;
char gEditBuf[40] = "";
int gConfirmAction = 0;
uint32_t gConfirmUntil = 0;

// ------------------------------------------------------------- helpers ------

bool addrAcks(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool addrIsSh1106(uint8_t addr) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)0x00);
  Wire.endTransmission();
  Wire.requestFrom((int)addr, (uint8_t)1);
  if (!Wire.available()) return false;
  uint8_t r = Wire.read() & 0x0F;
  return (r == 0x08 || r == 0x00);
}

void pushRx(const char *from, float rssi, float snr, const char *kind) {
  RxEntry &e = gRxRing[gRxHead];
  strncpy(e.call, from, sizeof(e.call) - 1);
  e.call[sizeof(e.call) - 1] = '\0';
  e.rssi = rssi;
  e.snr = snr;
  strncpy(e.kind, kind, sizeof(e.kind) - 1);
  e.kind[sizeof(e.kind) - 1] = '\0';
  e.ms = millis();
  gRxHead = (gRxHead + 1) % kRingSize;
  if (gRxUsed < kRingSize) gRxUsed++;
}

void pushTx(const char *what, const char *call) {
  TxEntry &e = gTxRing[gTxHead];
  strncpy(e.what, what, sizeof(e.what) - 1);
  e.what[sizeof(e.what) - 1] = '\0';
  strncpy(e.call, call, sizeof(e.call) - 1);
  e.call[sizeof(e.call) - 1] = '\0';
  e.ms = millis();
  gTxHead = (gTxHead + 1) % kRingSize;
  if (gTxUsed < kRingSize) gTxUsed++;
}

void drawLine(int y, const String &s) {
  // centered text line (scenes)
  int w = (int)gOled->getStringWidth(s);
  int x = (128 - w) / 2;
  if (x < 0) x = 0;
  gOled->drawString(x, y, s);
}

String fmtInt(long v) { return String((long)v); }

String ageStr(uint32_t ms) {
  uint32_t s = (millis() - ms) / 1000;
  String o;
  if (s < 60) o = fmtInt((long)s) + "s";
  else if (s < 3600) o = fmtInt((long)(s / 60)) + "m";
  else o = fmtInt((long)(s / 3600)) + "h";
  return o;
}

String fmtUp() {
  uint32_t s = millis() / 1000;
  String o;
  if (s >= 86400) o += fmtInt((long)(s / 86400)) + "d";
  o += fmtInt((long)((s % 86400) / 3600)) + "h";
  o += fmtInt((long)((s % 3600) / 60)) + "m";
  return o;
}

const char *modeChar(uint8_t mode) {
  switch (mode) {
    case 1: return "T";
    case 2: return "B";
    default: return "D";
  }
}

// ------------------------------------------------------------- scenes -------

// Meshtastic-like helpers ---------------------------------------------------

// Fake "bold" on bitmap fonts: draw the text twice with 1 px offset.
void drawBold(int x, int y, const String &s) {
  gOled->drawString(x, y, s);
  gOled->drawString(x + 1, y, s);
}

const char *modeName(uint8_t mode) {
  switch (mode) {
    case 1: return "Tracker";
    case 2: return "Digi+Tracker";
    default: return "Digipeater";
  }
}

// ★★ ICONO DEL PERFIL DE USO EN LA CABECERA (peticion del operador, 2026-09-15) ★★
// "en la primera diapositiva falta el simbolo correspondiente al modo activado,
//  coche, peaton... bici...". El perfil activo es `smartBeaconPreset` (ver
// config.h: 0=fijo/digi, 1=peaton, 2=bici, 3=coche) y cada perfil tiene su icono
// de mapa en `profileSymbol[]`/`profileOverlay[]` (/# estrella del digi, /[ persona,
// /b bici, /> coche). Aqui van las CUATRO figuras de 8x8 dibujadas a mano, en el
// MISMO orden que `smartBeaconPreset` y que los iconos de 16x16 de la pantalla de
// tinta (kIconosPorPerfil en epaper_techo.cpp), para que las dos pantallas del
// proyecto enseñen lo mismo: estrella = fijo/digi, persona = peaton, bici y coche.
//
// POR QUE EN EL HUECO DE LA CABECERA Y NO EN UNA FILA DE DATOS:
//   - la escena 0 (la del estado) tiene las CUATRO filas de datos ocupadas (RX/TX,
//     sensores, bateria, RSSI/SNR) y el pie es justo lo que se esta arreglando en
//     esta misma tanda: meterlo ahi seria volver a pisar cosas;
//   - el hueco de la cabecera (a la derecha del indicativo y a la izquierda de la
//     pastilla RX/TX) esta libre, es donde el operador mira de un vistazo el
//     indicativo y el modo, y es el mismo sitio que usa la pantalla de tinta para
//     este mismo icono (ver `pintaEstado` en epaper_techo.cpp);
//   - y el hueco se RESERVA: el icono solo se pinta si cabe entero sin tocar la
//     pastilla RX/TX (ver kPastillaRX0Min y headerBar). Si no cabe, ese refresco
//     sale sin icono; nunca se pisa el indicativo ni la pastilla.
// ★ (2026-09-15) CORRECCION: antes el icono se dibujaba DENTRO de la pastilla del
//   titulo y DELANTE del indicativo. El operador lo rechazo (ver la nota larga de
//   headerBar): ahora va DETRAS del indicativo, en el hueco. El dibujo y el tamano
//   NO cambian (8x8), que es lo que ya estaba aprobado.
constexpr int kPerfilIconoW = 8;   // 8x8 px, como las figuras de abajo
constexpr int kPerfilIconoY = 2;   // filas 2..9: centrado con la caja de la letra
                                   // del titulo (que va de y=3 a y=10) y con la
                                   // pastilla RX/TX (que ocupa y=0..12)

// QUE SE DIBUJA CON CADA VALOR DE `smartBeaconPreset` (respuesta exacta a la duda
// del operador "que yo sepa es el coche"): el valor que llega aqui es el perfil
// ACTIVO de la configuracion, leido en cada refresco (`cfg.smartBeaconPreset`,
// por defecto 0 = fabrica). El icono de la pantalla NO depende del simbolo del
// mapa (`profileSymbol[]`), que es un ajuste aparte del menu "Icono del perfil";
// solo depende del perfil de movimiento. La tabla:
//   0 -> estrella (fijo/digi, el de fabrica)     2 -> bici
//   1 -> persona (a pie)                          3 -> coche
//   cualquier otro valor -> estrella (perfil 0), igual que la pantalla de tinta
// Si el operador tiene el coche seleccionado y ve la estrella, lo que hay que
// mirar es `smartBeaconPreset` en su configuracion (menu "Perfil de movimiento"),
// no esta funcion: aqui el valor se lee fresco en cada refresco, no se guarda.
const char *const kPerfilDibujo[4][8] = {
  { // 0 = fijo/digi: estrella de 5 puntas (el simbolo APRS del perfil 0, /#)
    "...##...",
    "...##...",
    ".######.",
    "########",
    ".######.",
    "..####..",
    ".##..##.",
    "##....##",
  },
  { // 1 = peaton: cabeza, brazos abiertos, tronco y las dos piernas
    "..####..",
    "..####..",
    "########",
    "...##...",
    "...##...",
    "..####..",
    ".##..##.",
    "##....##",
  },
  { // 2 = bici: las dos ruedas con el buje marcado, el cuadro y el manillar
    // ★ REDIBUJADA (2026-09-15): la figura anterior no se reconocia. El operador
    //   la describio tal cual: "un borron con dos ruedas". Tres defectos medidos
    //   sobre la figura vieja (contando pixels encendidos):
    //     - las dos ruedas eran DESIGUALES: la izquierda empezaba en la columna 0
    //       y la derecha en la 5, asi que a la derecha le sobraba una columna y a
    //       la izquierda le faltaba; dos manchones distintos no se leen como dos
    //       ruedas;
    //     - la rueda izquierda no tenia buje (la derecha si): otra asimetria;
    //     - el manillar iba pegado a la rueda derecha, y las dos diagonales del
    //       cuadro caian entre las ruedas, con lo que todo el centro era un
    //       amasijo de 20 px de trazo.
    // COMO ES AHORA:
    //     - las dos ruedas son IGUALES y simetricas (columnas 0..2 y 5..7, filas
    //       4..6), cada una con su buje en el centro: un aro de 3x3 con el punto
    //       dentro se lee como rueda;
    //     - el conjunto sube de 20 a 26 px de trazo, pero REPARTIDOS: antes eran 20
    //       px amontonados entre las dos ruedas y ahora son dos aros limpios con su
    //       buje; a 8x8 no gana el que menos pinta, sino el que pinta donde se ve;
    //     - el cuadro es la barra de 4 px de la fila 1, que une las dos ruedas por
    //       arriba, y debajo queda el hueco de las columnas 3 y 4 (filas 2 y 3), que
    //       es lo que deja ver que son DOS ruedas separadas;
    //     - el sillin es el saliente de las columnas 2 y 3 de la fila 1 y el
    //       manillar el de las columnas 6 y 7, tambien en la fila 1.
    // LIMITE HONESTO DE ESTA FIGURA: a 8x8 px lo que se reconoce sin duda son las
    // DOS RUEDAS IGUALES (o sea, "un vehiculo de dos ruedas"); el cuadro, el
    // sillin y el manillar son una insinuacion. Una bici de perfil de verdad NO
    // cabe: reduciendo a 8x8 una bici dibujada con ruedas de 3 a 5 px sale una
    // mancha (esta comprobado con imagenes). Si el operador sigue sin verla clara,
    // la salida NO es apretar mas pixeles, sino cambiar la iconografia del perfil 2.
    "........",
    "..####..",
    ".#....#.",
    ".#.##.#.",
    "##.##.##",
    "#.#..#.#",
    "###..###",
    "........",
  },
  { // 3 = coche: techo, ventanilla, carroceria y las dos ruedas
    "........",
    "..####..",
    ".##..##.",
    "########",
    "########",
    "########",
    "##....##",
    "##....##",
  },
};

// Pinta la figura del perfil `perfil` con la esquina de arriba a la izquierda en
// (x0, y0). El color lo pone quien llama: ahora el icono va en el HUECO de la
// cabecera, sobre el fondo NEGRO de la pantalla, asi que se llama con WHITE (los
// '#' de las figuras son los pixels encendidos).
// Un valor fuera de 0..3 NO se inventa nada: se pinta el perfil 0 (fijo/digi), que
// es lo que hace la pantalla de tinta (ver iconoPerfil() en epaper_techo.cpp) y lo
// que vale por defecto de fabrica (config.h: smartBeaconPreset = 0).
void drawPerfilIcono(int x0, int y0, int perfil) {
  if (perfil < 0 || perfil > 3) perfil = 0;
  for (int fy = 0; fy < 8; fy++) {
    const char *fila = kPerfilDibujo[perfil][fy];
    for (int fx = 0; fx < 8 && fila[fx]; fx++) {
      if (fila[fx] == '#') gOled->setPixel(x0 + fx, y0 + fy);
    }
  }
}

// The LoRa send blocks the main loop, so the radio is already back in RX by
// the time the screen can repaint: the TX badge would never be seen. Keep it
// on for a short window after a successful send (operator request).
bool txBadgeActive() {
  if (!radioReady()) return false;
  if (strcmp(radioState(), "TX") == 0) return true;
  uint32_t last = radioLastTxMs();
  return last != 0 && (uint32_t)(millis() - last) < kTxBadgeMs;
}

// Left edge of the right-hand RX/TX pill (used so the left pill can adapt).
int statusPillX0() {
  bool tx = txBadgeActive();
  const char *txt = !radioReady() ? "--" : (tx ? "TX" : "RX");
  if (gCfg && gCfg->txDisabled) txt = "MUTE";
  int w = (int)gOled->getStringWidth(txt) + 10;
  if (w < 20) w = 20;
  int x0 = 126 - w;
  if (x0 < 76) x0 = 76;
  return x0;
}

// Right side of the header: rounded pill with plain-text RX / TX inside
// (or MUTE when silenced). RX = outline; TX = inverted (filled), like the
// active scene dot.
void statusPill() {
  bool tx = txBadgeActive();
  const char *txt = !radioReady() ? "--" : (tx ? "TX" : "RX");
  if (gCfg && gCfg->txDisabled) txt = "MUTE";

  int tw = (int)gOled->getStringWidth(txt);
  const int x1 = 126;
  const int x0 = statusPillX0();
  const int r = 6;
  const int tx0 = x0 + (x1 - x0 - tw) / 2;  // text x

  gOled->setColor(WHITE);
  gOled->fillCircle(x0 + r, r, r);
  gOled->fillCircle(x1 - r, r, r);
  gOled->fillRect(x0 + r, 0, (x1 - r) - (x0 + r), 12);

  if (tx) {
    // inverted: filled pill + black text
    gOled->setColor(BLACK);
    gOled->drawString(tx0, 0, txt);
  } else {
    // outline pill + white text
    gOled->setColor(BLACK);
    gOled->fillCircle(x0 + r, r, r - 1);
    gOled->fillCircle(x1 - r, r, r - 1);
    gOled->fillRect(x0 + r, 1, (x1 - r) - (x0 + r), 10);
    gOled->setColor(WHITE);
    gOled->drawString(tx0, 0, txt);
  }
  gOled->setColor(WHITE);
}

// ★★ DONDE VA EL ICONO DEL PERFIL EN LA OLED (corregido 2026-09-15) ★★
//
// ANTES iba DENTRO de la pastilla del titulo, DELANTE del indicativo. El operador
// lo vio en su aparato y lo rechazo con estas palabras: "en la primera diapositiva
// aparece una estrella [...] y no va ahi, iria como en la epaper de los T-Echo,
// entre el indicativo y el rx/tx, en ese hueco entre ambos". O sea: el sitio es el
// HUECO que queda a la DERECHA del indicativo y a la IZQUIERDA de la pastilla
// RX/TX, que es exactamente donde lo pinta la pantalla de tinta (`pintaEstado` de
// epaper_techo.cpp: indicativo primero, icono 8 px despues de la ultima letra,
// pastilla RX/TX al final SIN moverse).
//
// COMO SE RESERVA EL HUECO (que es lo que pide el encargo: que nada se pise, por
// largo que sea el indicativo). La fila tiene tres cosas y el reparto es este:
//   1. el INDICATIVO (+ el modo) dentro de la pastilla blanca, que empieza en x=2.
//      Su texto empieza en x0+pad = 6 y la pastilla termina en x1 = 6 + ancho + 4;
//   2. el ICONO, 6 px despues del borde derecho de la pastilla (xIcono = x1 + 6),
//      o sea en el hueco y nunca encima de la letra;
//   3. la PASTILLA RX/TX, que NO se mueve ni se encoge: la decide `statusPillX0()`
//      con su propio texto: x0 = 126 - max(20, ancho(texto) + 10). Los cuatro
//      textos posibles y su borde izquierdo REAL, medidos:
//         "MUTE" -> ancho 26 -> x0 = 126 - 36 =  90   <-- EL PEOR (es el mas ancho)
//         "RX"   -> ancho 14 -> x0 = 126 - 24 = 102
//         "TX"   -> ancho 14 -> x0 = 126 - 24 = 102
//         "--"   -> ancho  6 -> x0 = 126 - 20 = 106   (actua el minimo de 20)
//      El peor caso es por tanto 90, NO 102: con "RX"/"TX" (los textos normales)
//      la pastilla empieza en 102, pero en cuanto el nodo queda silenciado pasa a
//      "MUTE" y arranca en 90. Si el limite se pusiera en 102, el icono se meteria
//      DEBAJO DE LA PASTILLA en cuanto se silenciara el nodo. Por eso el limite es
//      el peor de los cuatro.
// Con esos tres numeros el icono se pinta SOLO si `xIcono + 8 <= kPastillaRX0Min`
// (90). Con indicativos normales ("EA2KR-3 B" mide 50 px -> x1 = 60, icono en
// x=66..73) sobra sitio. Solo si el indicativo es tan largo que su pastilla llega
// a x=76 (texto de 66 px, unos 10-11 caracteres) el hueco se queda sin los 8 px
// del icono y ese refresco sale SIN icono: se sacrifica el icono, nunca el
// indicativo ni la pastilla RX/TX, que son informacion.
constexpr int kPastillaRX0Min = 90;   // peor caso real de statusPillX0() ("MUTE")
constexpr int kIconoAire = 6;         // aire entre la pastilla del titulo y el icono

// Rounded white "pill" behind only the text (indicativo + modo), black text.
// The pill adapts to the text length; if the text is too long it is trimmed.
//
// `perfilIcono` (opcional, -1 = sin icono, que es como la llaman todas las
// pantallas menos la primera diapositiva): dibuja la figura del perfil de uso
// (8x8) en el HUECO de la derecha, entre la pastilla del titulo y la de RX/TX.
// La pastilla NO se ensancha por el icono: el icono va FUERA, en el hueco, asi
// que el indicativo no pierde ni un pixel de sitio por culpa del dibujo.
void headerBar(const String &text, int perfilIcono = -1) {
  const int pad = 4;
  const int x0 = 2;
  int maxX1 = statusPillX0() - 2;
  // Con el menu abierto y el cierre por inactividad a punto de llegar, se reserva
  // el hueco del aviso ("18s") desde el principio de esos ultimos segundos: asi el
  // titulo no encoge de golpe justo cuando aparece el numero. Ver
  // drawMenuCountdown().
  if (gMenuOpen && !gEditing &&
      millis() - gLastActivityMs + kMenuCountdownMs >= kMenuIdleCloseMs) {
    int w = (int)gOled->getStringWidth("00s") + 4;
    if (maxX1 - w > x0 + 20) maxX1 -= w;
  }
  const int maxTextW = maxX1 - x0 - pad * 2 - 1;

  String s = text;
  while (s.length() > 3 && (int)gOled->getStringWidth(s) > maxTextW) {
    s.remove(s.length() - 1);
  }
  int tw = (int)gOled->getStringWidth(s);
  int x1 = x0 + tw + pad * 2;
  if (x1 > maxX1) x1 = maxX1;
  const int r = 6;

  gOled->setColor(WHITE);
  gOled->fillCircle(x0 + r, r, r);
  gOled->fillCircle(x1 - r, r, r);
  gOled->fillRect(x0 + r, 0, (x1 - r) - (x0 + r), 12);
  gOled->setColor(BLACK);
  gOled->drawString(x0 + pad, 0, s);  // no bold: cleaner at this resolution
  // El icono va DESPUES de la pastilla (a la derecha del indicativo) y ANTES de
  // la pastilla RX/TX. Si no cabe en el hueco no se pinta: la pastilla RX/TX manda.
  if (perfilIcono >= 0) {
    const int xIcono = x1 + kIconoAire;
    if (xIcono + kPerfilIconoW <= kPastillaRX0Min) {
      gOled->setColor(WHITE);   // el icono ya NO va dentro de la pastilla blanca:
      drawPerfilIcono(xIcono, kPerfilIconoY, perfilIcono);  // va sobre fondo negro
    }
  }
  gOled->setColor(WHITE);
  statusPill();
}

// VISOR DE DIAPOSITIVAS (rediseñado, peticion del operador 2026-09-15): "en vez de
// ese visor que ocupa tanto, simplemente ponemos unas lineas que ocupen solo una
// fila de pixeles, y la que se muestra en ese momento, que tenga dos filas de
// altura". O sea: UNA marca por escena, de 1 px de alto, y la escena actual de 2 px.
//
// QUE HABIA ANTES Y POR QUE ESTABA MAL: ocho recuadros de 7x4 px en y=58..61. Ese
// visor ocupaba cuatro filas del pie y, ademas, PISABA la ultima linea de texto de
// las escenas largas: con la rejilla de entonces esa linea se dibujaba en y=51 y su
// tinta llegaba hasta y=60, asi que los recuadros le caian ENCIMA. El operador lo
// vio en la OLED y lo conto tal cual: "la linea de abajo de algunas diapositivas
// pisa el visor de que diapositiva se muestra".
//
// LAS CUENTAS DE LA PANTALLA (128x64; comprobadas contra las tablas de la fuente
// ArialMT_Plain_10 de la libreria, y ademas medidas con el simulador de la OLED):
//   - la caja de la fuente mide 13 px, pero la tinta de mayusculas, digitos y
//     minusculas sin cola cae en las filas 3..10 de esa caja, o sea de y+3 a y+10
//     de la linea. OJO: la libreria ADEMAS deja escrito el byte de la fila +8 en
//     la pagina siguiente (su drawInternal no recorta por abajo), asi que cada
//     linea pinta arrastre en y+11..y+15. Con el paso de 10 px ese arrastre ya no
//     llega a la linea de abajo (queda en el hueco) y tampoco al pie.
//   - la tinta mas baja de CUALQUIER escena esta en y=55 (las escenas de cuatro
//     filas, que con el paso de 10 dibujan su ultima linea en y=45); el arrastre
//     mas bajo llega a y=60;
//   - el visor va en la fila base y=63 y la marca activa crece hacia ARRIBA
//     (62..63), que es lo unico que cabe sin salirse de la pantalla;
//   - quedan 3 px de aire entre la tinta de la ultima linea (y=55) y el visor
//     (y=62): NO se pisan en ninguna de las ocho. Antes el visor viejo se metia en
//     la tinta de la ultima linea en 5 de las 8 escenas (13..25 px por escena,
//     medido con las tablas de la fuente); ahora son 0 en las ocho.
//   - OJO SI ALGUIEN TOCA ESOS TEXTOS: el pie no tiene margen de sobra. Con el
//     paso nuevo, la ultima linea de las escenas de cuatro filas se dibuja en
//     y=45 y su arrastre llega a y=60: si alguien anadiera una quinta banda, o
//     subiera el pie, volveria el cruce. Las cuentas del pie estan en el
//     comentario de drawBatteryFooter.
//
// EL ANCHO, Y POR QUE ASI (8 marcas x 7 px + 7 huecos x 2 px = 70 px, de x=2 a
// x=71): la marca tiene que verse como una linea, no como un punto, pero el pie
// comparte la ultima fila con la BATERIA, que va a la derecha. El cuerpo de la
// barra empieza en x=73 en su peor caso (la pastilla mas ancha, "100%"; con "--"
// o "9%" empieza mas a la derecha todavia), asi que quedan 2 px de aire hasta las
// marcas. Se alinean a la izquierda en x=2 para cuadrar con la pastilla del titulo
// (headerBar tambien empieza en x=2) y con el sitio donde estaban los recuadros
// viejos: asi el pie no se mueve de sitio, solo cambia de forma.
constexpr int kPaginaY = 63;    // fila base del visor (la ultima de la pantalla)
constexpr int kPaginaW = 7;     // largo de cada marca
constexpr int kPaginaGap = 2;   // aire entre marcas
constexpr int kPaginaX0 = 2;    // alineado con la pastilla del titulo

// Una marca por escena: la activa de 2 px de alto (y crece hacia arriba) y las
// demas de 1 px. Antes de este cambio la funcion se llamaba footerDots() y pintaba
// ocho recuadros de 7x4; se ha rediseñado la MISMA funcion, no hay otra al lado.
void footerPagina() {
  gOled->setColor(WHITE);   // el color no se hereda de la escena anterior
  for (int i = 0; i < kSceneCount; i++) {
    int x = kPaginaX0 + i * (kPaginaW + kPaginaGap);
    if (i == gScene) {
      gOled->fillRect(x, kPaginaY - 1, kPaginaW, 2);  // la diapositiva actual
    } else {
      gOled->fillRect(x, kPaginaY, kPaginaW, 1);      // las demas, 1 px
    }
  }
}

// Bottom-right battery: graphical bar + percentage.
//
// ★★ EL PIE DE LA BATERIA YA NO SE CRUZA CON LA ULTIMA LINEA DE TEXTO
//    (arreglado 2026-09-15, medido con el simulador de la OLED) ★★
// QUE PASABA (primera vuelta): el texto del porcentaje se dibujaba en y=54 y el
// cuerpo de la barra en y=57..63, los dos alineados a la DERECHA. La ultima linea
// de datos cae justo ahi: con la rejilla vieja (paso 12) las escenas de cuatro
// filas la dibujaban en y=51 (tinta hasta y=61 y arrastre hasta y=66) y las de
// lista en y=44 (tinta hasta y=54), asi que el porcentaje, el cuerpo de la barra y
// la propia letra compartian filas Y columnas. El operador lo vio como "el pie
// pisa la ultima linea".
// COMO SE ARREGLO ENTONCES: (1) el paso de la rejilla de texto bajo de 12 a 10 px
// (ver kRowH mas arriba), con lo que la tinta de la ultima linea acaba en y=57 y
// el arrastre en y=60; y (2) el texto del porcentaje y la barra se bajaron a
// y=58 y y=61..67 respectivamente.
//
// ★★ SEGUNDA VUELTA: AQUELLO SE PASO DE FRECUENTE (2026-09-15, el operador lo vio
//    en su aparato) ★★
// Sus palabras: "la bateria: has bajado tanto la pila y el porcentaje que se SALEN
// de la pantalla. Súbelos solo un poquito, a ver si no se pisan pero se intuye
// mejor el valor de porcentaje de bateria".
// TENIA RAZON, Y LAS CUENTAS LO DICEN: la OLED tiene 64 filas (0..63) y la libreria
// NO recorta por abajo (`drawInternal` escribe el byte de la fila +8 en la pagina
// siguiente y los bits que pasan de y=63 se pierden):
//   - el texto en y=58 con la caja de 13 px llega a y=70: sus filas 64..70 se
//     perdian, y lo que se veia del porcentaje eran SOLO sus 6 filas de arriba
//     (y=58..63), o sea el numero cortado;
//   - el cuerpo de la barra iba de y=61 a y=67: 4 de sus 7 filas fuera de pantalla,
//     incluida la fila de abajo del recuadro y el terminal.
// POSICION NUEVA, QUE ES EL PUNTO MEDIO (medida, no estimada, con el simulador de
// `_revision_pantalla_ea2oy\sim`; los numeros estan en el informe):
//   - TEXTO: y=54. Es la fila mas ALTA a la que puede subir el porcentaje sin
//     pisar la ultima linea de las escenas de lista. El porque, medido: con la
//     caja de 13 px, la tinta de los digitos y del simbolo '%' de esta fuente
//     empieza en la fila +3 de la caja, o sea que el texto de y=54 tiene su
//     primera tinta en y=57; y la tinta mas baja de la ultima linea de las
//     escenas de lista ("R EA1XYZ-7 -101/1  5m", dibujada en y=44) esta en y=56.
//     Un pixel de aire entre las dos. Con y=53 el texto entraria en la fila 56 y
//     pisaria esa linea; con y=51 (que es lo mas alto que cabe sin cortarse) el
//     roce con el cuerpo de la barra sube de 2 px a 13 en CINCO escenas, medido.
//   - BARRA: cuerpo de 6 px de alto (antes 7) en y=58..63 y el terminal dentro
//     (y=60..62). Se acorta UN pixel de alto y no siete de bajada: asi la barra
//     queda pegada al borde de abajo (como estaba) pero ENTRA ENTERA. Con 7 px de
//     alto acabaria en y=64, o sea una fila FUERA de la pantalla: es exactamente
//     el fallo que el operador vio ("se salen de la pantalla").
//   - LA BARRA NO SE MUEVE DE COLUMNA al cambiar el estado de la bateria (se
//     calcula desde el borde derecho), asi que el aspecto es estable.
// RESULTADO MEDIDO (simulador, 8 escenas x 5 estados del porcentaje = 40 casos):
//   * 0 px de tinta fuera de la pantalla, ni por abajo (y>63) ni por ningun lado,
//     en los 40 casos, ni del texto, ni de la barra, ni de las lineas de las
//     escenas. Antes: el texto dejaba sus 3 filas de abajo fuera y el cuerpo de la
//     barra 4 (la barra acababa en y=67).
//   * 0 px de tinta compartida con el VISOR en las 8 escenas (el visor sigue
//     acabando en x=71 y el pie empieza en x=76): era la queja original del
//     operador y sigue a cero.
//   * El TEXTO del porcentaje entra entero en los 5 estados ("100%": x=100..125,
//     y=54..63 con tinta hasta y=63; "9%": 7 filas de tinta, y=57..63).
//   * Roce con el cuerpo de la barra: 1 a 3 px por escena, y solo en las escenas
//     cuyas lineas llevan letras con cola ('y', 'g', 'm', 'p'): son los ultimos
//     bytes de relleno de esos glifos, que la libreria escribe y caen en las filas
//     58-60 a la derecha (x=79..91). El que mas tiene son 3 px en "SENSORES".
//   * Roce con el TEXTO del porcentaje: 2 px y solo en la escena "ULTIMOS RX" con
//     el porcentaje mas ancho, el "100%": (100,57) y (101,59), la punta de la cola
//     de la '5' y de la 'm' de "R EA1XYZ-7 -101/1  5m" rozando el borde de arriba
//     del '1' y del '0'.
//   Ninguno de esos 1-3 px se ve a simple vista, y quitarlos exigiria subir el
//   texto por encima de las colas (volver a pisar la ultima linea) o bajarlo fuera
//   de la pantalla (el fallo original). El punto medio elegido es el que menos
//   roce tiene de todas las colocaciones posibles con el porcentaje LEGIBLE: se
//   midieron las 130 combinaciones de fila de texto (48..57) y fila de barra
//   (55..59) con 3 altos distintos; subir el texto a y=51 baja el roce con la
//   barra pero lo sube con el texto del porcentaje (13 px en 5 escenas).
// NOTA PARA EL QUE VUELVA A TOCAR ESTO: no hay mas margen. Si alguien cambia
// kRowH, kRowsTop o el alto de la barra, que vuelva a medir con el simulador
// (`_revision_pantalla_ea2oy\sim\verifica_pie.py`) antes de tocar la pantalla.
constexpr int kBateraGap = 10;     // aire entre el porcentaje y el cuerpo de la barra
constexpr int kBateraAncho = 14;   // ancho del cuerpo de la barra (igual que antes)
constexpr int kBateraAlto = 6;     // alto del cuerpo (era 7; con 7 no entra entero)
constexpr int kBateraTextoY = 54;  // fila del texto del porcentaje (era 58)
constexpr int kBateraCuerpoY = 58; // fila de arriba del cuerpo de la barra (era 61)

void drawBatteryFooter() {
  float bv = sensorsBatteryVolt(gSensorCache);
  bool ok = bv > 0.0f;
  int pct = 0;
  if (ok) {
    pct = (int)((bv - 3.0f) / 1.2f * 100.0f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
  }
  char t[8];
  if (ok) {
    snprintf(t, sizeof(t), "%d%%", pct);
  } else {
    snprintf(t, sizeof(t), "--");
  }

  gOled->setColor(WHITE);
  int tw = (int)gOled->getStringWidth(t);
  int tx = 127 - tw;
  gOled->drawString(tx, kBateraTextoY, t);

  // El cuerpo de la barra va a la IZQUIERDA del porcentaje, separado kBateraGap
  // px. Se calcula desde el borde derecho de la pantalla (127) y no desde `tx`,
  // para que la barra no se mueva al cambiar el ancho del porcentaje ("100%" es
  // mas ancho que "9%").
  int bx = 127 - tw - kBateraGap - kBateraAncho;
  int by = kBateraCuerpoY;
  gOled->drawRect(bx, by, kBateraAncho, kBateraAlto);
  gOled->fillRect(bx + kBateraAncho, by + 2, 2, 3);  // + terminal
  if (ok) gOled->fillRect(bx + 1, by + 1, (int)(12.0f * pct / 100.0f), 5);
}

// Small bearing arrow (0 deg = up) used by the stations scene.
void drawArrow(int x, int y, float deg) {
  float r = deg * PI / 180.0f;
  int dx = (int)(sinf(r) * 6.0f);
  int dy = (int)(-cosf(r) * 6.0f);
  gOled->drawLine(x, y, x + dx, y + dy);
  float r1 = (deg + 150.0f) * PI / 180.0f;
  float r2 = (deg - 150.0f) * PI / 180.0f;
  gOled->drawLine(x + dx, y + dy, x + dx + (int)(sinf(r1) * 4.0f),
                  y + dy + (int)(-cosf(r1) * 4.0f));
  gOled->drawLine(x + dx, y + dy, x + dx + (int)(sinf(r2) * 4.0f),
                  y + dy + (int)(-cosf(r2) * 4.0f));
}

void drawStatusScene(const DigiConfig &cfg, uint32_t rx, uint32_t tx,
                     uint32_t dg, const SensorReadings &r) {
  // El icono del perfil de uso (fijo/peaton/bici/coche) va en el HUECO de la
  // cabecera: a la derecha del indicativo (+ el modo) y a la izquierda de la
  // pastilla RX/TX, que es donde lo pinta la pantalla de tinta y donde el operador
  // dijo que va ("entre el indicativo y el rx/tx"). No le quita hueco a ninguna de
  // las cuatro filas de datos ni al pie. El valor que se pinta es
  // `smartBeaconPreset` (config.h), LEIDO AQUI EN CADA REFRESCO (no se guarda en
  // ninguna variable de arranque), el mismo que decide los tiempos del smart beacon
  // y el icono del mapa; de fabrica vale 0, asi que lo que se ve por defecto es la
  // estrella del digi. Ver la tabla de los cuatro valores en kPerfilDibujo.
  headerBar(String(cfg.callsign) + " " + modeChar(cfg.mode),
            (int)cfg.smartBeaconPreset);

  char b[40];
  snprintf(b, sizeof(b), "RX %lu  TX %lu  DG %lu", (unsigned long)rx,
           (unsigned long)tx, (unsigned long)dg);
  drawLine(kRowsTop + kTextOff, b);

  if (r.wxOk) {
    char hb[16] = "";
    if (r.humOk) snprintf(hb, sizeof(hb), " H%.0f%%", r.hum);
    if (r.pressOk) {
      snprintf(b, sizeof(b), "T%.1fC%s P%.0f", r.tempC, hb, r.pressHpa);
    } else {
      snprintf(b, sizeof(b), "T%.1fC%s", r.tempC, hb);
    }
    drawLine(kRowsTop + kRowH + kTextOff, b);
  } else {
    // No probe: show the chip sensor (inside the box) instead of a blank line.
    snprintf(b, sizeof(b), "TINT %.1f\xC2\xB0" "C", sensorsChipTemp(r, cfg.chipTempOffsetC));
    drawLine(kRowsTop + kRowH + kTextOff, b);
  }

  float bv = sensorsBatteryVolt(r);
  if (r.inaOk) {
    snprintf(b, sizeof(b), "Bat %.2fV  I %+.0fmA", bv, r.inaCurrentMa);
  } else if (bv > 0.0f) {
    snprintf(b, sizeof(b), "Bat %.2fV", bv);
  } else {
    snprintf(b, sizeof(b), "Bat --");
  }
  drawLine(kRowsTop + 2 * kRowH + kTextOff, b);

  snprintf(b, sizeof(b), "RSSI %.0f  SNR %.1f", radioLastRssi(), radioLastSnr());
  drawLine(kRowsTop + 3 * kRowH + kTextOff, b);
}

void drawRxScene() {
  headerBar(String("ULTIMOS RX (") + fmtInt(gRxUsed) + ")");
  if (gRxUsed == 0) {
    drawLine(kRowsTop + kRowH + kTextOff, "  (vacio)");
    return;
  }
  char b[40];
  for (int i = 0; i < gRxUsed && i < 4; i++) {
    int idx = (gRxHead - 1 - i + kRingSize) % kRingSize;
    RxEntry &e = gRxRing[idx];
    snprintf(b, sizeof(b), "%c %-9s %4.0f/%.0f %s", e.kind[0], e.call, e.rssi,
             e.snr, ageStr(e.ms).c_str());
    drawLine(14 + i * 10, b);
  }
}

void drawTxScene() {
  headerBar(String("ULTIMOS TX (") + fmtInt(gTxUsed) + ")");
  if (gTxUsed == 0) {
    drawLine(kRowsTop + kRowH + kTextOff, "  (vacio)");
    return;
  }
  for (int i = 0; i < gTxUsed && i < 4; i++) {
    int idx = (gTxHead - 1 - i + kRingSize) % kRingSize;
    TxEntry &e = gTxRing[idx];
    String s = e.what;
    if (e.call[0]) {
      s += " ";
      s += e.call;
    }
    s += "  ";
    s += ageStr(e.ms);
    drawLine(14 + i * 10, s);
  }
}

void drawRadioScene(const DigiConfig &cfg) {
  headerBar("RADIO");
  char b[32];
  snprintf(b, sizeof(b), "%.3f MHz  SF%d", cfg.frequencyHz / 1000000.0,
           cfg.spreadingFactor);
  drawLine(kRowsTop + kTextOff, b);
  snprintf(b, sizeof(b), "BW%.0f  CR4/%d  P%ddBm", cfg.signalBandwidthKhz,
           cfg.codingRate4, cfg.powerDbm);
  drawLine(kRowsTop + kRowH + kTextOff, b);
  snprintf(b, sizeof(b), "CAD %s  err %d  %s", cfg.cadActive ? "on" : "off",
           radioLastErr(), radioState());
  drawLine(kRowsTop + 2 * kRowH + kTextOff, b);
  drawLine(kRowsTop + 3 * kRowH + kTextOff, String("UP ") + fmtUp());
}

void drawSensorsScene(const SensorReadings &r) {
  headerBar("SENSORES");
  char b[40];
  // TINT = chip sensor (inside the enclosure), TEXT = external probe (air).
  // The degree sign is written as UTF-8 ("\xC2\xB0" + "C"): the font ships
  // 224 glyphs and drawString() decodes UTF-8, so it renders properly.
  float tint = sensorsChipTemp(r, gCfg ? gCfg->chipTempOffsetC : 0.0f);
  snprintf(b, sizeof(b), "TINT %.1f\xC2\xB0" "C", tint);
  drawLine(kRowsTop + kTextOff, b);
  if (r.wxOk) {
    float text = r.tempC + (gCfg ? gCfg->temperatureCorrectionC : 0.0f);
    if (r.humOk) {
      snprintf(b, sizeof(b), "TEXT %.1f\xC2\xB0" "C  H %.0f%%", text, r.hum);
    } else {
      snprintf(b, sizeof(b), "TEXT %.1f\xC2\xB0" "C", text);
    }
  } else {
    snprintf(b, sizeof(b), "TEXT --  (sin sonda)");
  }
  drawLine(kRowsTop + kRowH + kTextOff, b);
  if (r.pressOk) {
    snprintf(b, sizeof(b), "P %.1f  Bat %.2fV", r.pressHpa, r.vbatDivV);
  } else {
    snprintf(b, sizeof(b), "Bat %.2fV", r.vbatDivV);
  }
  drawLine(kRowsTop + 2 * kRowH + kTextOff, b);
  if (r.hasBs6) {
    snprintf(b, sizeof(b), "AIRE %.0fk  %+.0fmA", r.gasKohm, r.inaCurrentMa);
  } else if (r.inaOk) {
    snprintf(b, sizeof(b), "INA %.2fV  %+.0fmA", r.inaBusV, r.inaCurrentMa);
  } else {
    snprintf(b, sizeof(b), "INA --");
  }
  drawLine(kRowsTop + 3 * kRowH + kTextOff, b);
}

void drawSystemScene(const DigiConfig &cfg, const SensorReadings &r) {
  String ver = APP_VERSION_STR;
  if (ver.length() > 5) ver = ver.substring(0, 5);
  headerBar(String("SISTEMA v") + ver);
  uint32_t rr = NRF_POWER->RESETREAS;
  const char *why = "?";
  if (rr & POWER_RESETREAS_OFF_Msk) why = "systemoff";
  else if (rr & POWER_RESETREAS_LPCOMP_Msk) why = "lpcomp";
  else if (rr & POWER_RESETREAS_DOG_Msk) why = "watchdog";
  else if (rr & POWER_RESETREAS_RESETPIN_Msk) why = "pin";
  else if (rr & POWER_RESETREAS_SREQ_Msk) why = "soft";
  drawLine(kRowsTop + kTextOff, String("UP ") + fmtUp());
  drawLine(kRowsTop + kRowH + kTextOff, String("Rearranque: ") + why);
  drawLine(kRowsTop + 2 * kRowH + kTextOff, String("Sleep ") + cfg.sleepCutMv + " Wake " + cfg.sleepWakeMv);
  drawLine(kRowsTop + 3 * kRowH + kTextOff, String("Rem... ") + (cfg.remoteEnabled ? "on" : "off") +
                   "  INA " + (r.inaOk ? "si" : "no"));
}

void drawStationsScene() {
  headerBar("ESTACIONES");
  HeardStation hs[kRingSize];
  uint8_t n = aprsHeardStations(hs, kRingSize);
  if (n == 0) {
    drawLine(kRowsTop + kRowH + kTextOff, "  (ninguna)");
    return;
  }
  const GpsData &g = gpsGet();
  char b[40];
  for (uint8_t i = 0; i < n && i < 4; i++) {
    int y = 14 + i * 10;
    if (g.fix && hs[i].hasPos) {
      float d = gpsDistanceM(g.lat, g.lon, hs[i].lat, hs[i].lon);
      float brg = gpsBearingDeg(g.lat, g.lon, hs[i].lat, hs[i].lon);
      char dstr[8];
      if (d < 1000.0f) snprintf(dstr, sizeof(dstr), "%.0fm", d);
      else snprintf(dstr, sizeof(dstr), "%.1fk", d / 1000.0f);
      snprintf(b, sizeof(b), "%-9s  %s", hs[i].call, dstr);
      gOled->drawString(0, y, b);
      drawArrow(120, y + 5, brg);
    } else {
      snprintf(b, sizeof(b), "%-9s  %s", hs[i].call, ageStr(hs[i].ms).c_str());
      drawLine(y, b);
    }
  }
}

void drawTrackerScene(const DigiConfig &cfg) {
  const GpsData &g = gpsGet();
  headerBar(String("GPS ") + (g.fix ? "FIX" : "sin fix"));
  if (g.fix) {
    gOled->setFont(ArialMT_Plain_16);
    // lat y lon con la letra grande (16 px), en bandas de kRowH con el mismo aire
    gOled->drawString(0, kRowsTop + 2, String(g.lat, 5));
    gOled->drawString(0, kRowsTop + 2 + 16, String(g.lon, 5));
    gOled->setFont(ArialMT_Plain_10);
    // Altitude, speed and satellites on one line; the course is the arrow on
    // the left (an arrow needs no room for digits).
    char b[40];
    if (g.altValid) {
      snprintf(b, sizeof(b), "%.0fm  %.0fkm/h  %usat", g.altM, g.speedKmh,
               (unsigned)g.sats);
    } else {
      snprintf(b, sizeof(b), "%.0fkm/h  %usat", g.speedKmh, (unsigned)g.sats);
    }
    drawArrow(6, kRowsTop + 3 * kRowH + kTextOff + 3, g.courseDeg);
    drawLine(kRowsTop + 3 * kRowH + kTextOff, b);
  } else {
    // No fix yet: still show signs of life (satellites in view and signal), so
    // it is clear whether the GPS is working or simply has no sky.
    char b[40];
    if (!gpsPowered()) {
      drawLine(kRowsTop + kTextOff, "GPS en pausa");
      drawLine(kRowsTop + 2 * kRowH + kTextOff, "(ahorro, reintenta)");
    } else {
      drawLine(kRowsTop + kTextOff, g.satsInView > 0 ? "Buscando posicion" : "Sin satelites");
      snprintf(b, sizeof(b), "%u sat a la vista", (unsigned)g.satsInView);
      drawLine(kRowsTop + kRowH + kTextOff, b);
      if (g.bestSnr > 0) {
        snprintf(b, sizeof(b), "senal %u   modo %s", (unsigned)g.bestSnr,
                 modeChar(cfg.mode));
      } else {
        snprintf(b, sizeof(b), "modo %s", modeChar(cfg.mode));
      }
      drawLine(kRowsTop + 2 * kRowH + kTextOff, b);
    }
  }
}

void drawScene(const DigiConfig &cfg, uint32_t rx, uint32_t tx, uint32_t dg,
               const SensorReadings &r) {
  switch (gScene) {
    case 0: drawStatusScene(cfg, rx, tx, dg, r); break;
    case 1: drawRxScene(); break;
    case 2: drawTxScene(); break;
    case 3: drawRadioScene(cfg); break;
    case 4: drawSensorsScene(r); break;
    case 5: drawSystemScene(cfg, r); break;
    case 6: drawStationsScene(); break;
    default: drawTrackerScene(cfg); break;
  }
}

// Boot splash (4 s, non-blocking): antenna logo + firmware name, version,
// build date and active mode, with a progress bar.
void drawSplash(const DigiConfig &cfg, uint32_t now) {
  int ax = 20, ay = 16;
  gOled->setColor(WHITE);
  gOled->fillCircle(ax, ay, 2);
  gOled->drawLine(ax, ay + 2, ax, 42);
  gOled->drawLine(ax - 7, 42, ax + 7, 42);
  gOled->drawCircleQuads(ax, ay, 7, 0x3);
  gOled->drawCircleQuads(ax, ay, 11, 0x3);

  // TODAS las lineas con la MISMA regla de maquetacion (kRowH/kTextOff). Antes iban
  // con saltos de 18, 12, 10 y 8 px: la ultima quedaba pegada a la de arriba.
  // La primera linea es EL INDICATIVO DEL USUARIO (cfg.callsign), no uno fijo: este
  // firmware lo usa cualquiera y en su pantalla tiene que poner SU indicativo. Si aun
  // no ha configurado ninguno, sale NOCALL (el de fabrica), que es la verdad.
  gOled->setFont(ArialMT_Plain_16);
  drawBold(40, kRowsTopSplash, String(cfg.callsign));
  gOled->setFont(ArialMT_Plain_10);
  const int y0 = kRowsTopSplash + 20;   // debajo del nombre, que es mas alto
  drawBold(40, y0, "APRS SYSTEM");
  gOled->drawString(40, y0 + kRowH, String("v") + APP_VERSION_STR);
  gOled->drawString(40, y0 + 2 * kRowH, __DATE__);
  gOled->drawString(0, y0 + 3 * kRowH, String("Modo: ") + modeName(cfg.mode));

  int elapsed = (int)(kSplashMs - (gSplashUntil - now));
  if (elapsed < 0) elapsed = 0;
  if (elapsed > (int)kSplashMs) elapsed = (int)kSplashMs;
  gOled->drawProgressBar(0, 61, 128, 3, (uint8_t)(elapsed * 100 / (int)kSplashMs));
}

// Bluetooth pairing splash (Meshtastic behaviour): a clean screen with the PIN
// while a pairing is in progress. See displayPinSplash() below.
void drawPinSplash() {
  // Misma maquetacion que el resto: barra de titulo + bandas de kRowH.
  headerBar("Emparejar BT");

  // PIN in the big font, one digit at a time with a space between them so six
  // digits are readable from a step back.
  String spaced;
  for (const char *p = gPinSplash; *p != '\0'; p++) {
    if (spaced.length() > 0) spaced += ' ';
    spaced += *p;
  }
  // El PIN con la letra grande; las lineas de debajo, con la regla comun.
  gOled->setFont(ArialMT_Plain_16);
  drawLine(kRowsTop + 4, spaced);

  gOled->setFont(ArialMT_Plain_10);
  gOled->setFont(ArialMT_Plain_10);
  drawLine(kRowsTop + 2 * kRowH + kTextOff, "Escribe este PIN");

  // Where the PIN comes from, so the operator knows what to expect: the same
  // configured PIN (static pairing) or one the stack generated.
  drawLine(kRowsTop + 3 * kRowH + kTextOff,
           gPinSplashFromStack ? "PIN del nodo" : "PIN configurado");
}

// --------------------------------------------------------------- menu -------

String menuValueStr(const MenuItem &it, JsonObjectConst doc) {
  if (!gCfg || it.kind == MK_HEADER || it.kind == MK_ACTION) return "";
  switch (it.kind) {
    case MK_BOOL:
      return doc[it.key].as<bool>() ? "Si" : "No";
    case MK_INT:
      return fmtInt(doc[it.key].as<long>());
    case MK_FLOAT: {
      float v = doc[it.key].as<float>();
      return String(v, 6);
    }
    case MK_ENUM: {
      int v = doc[it.key].as<int>();
      for (int i = 0; it.opts && it.opts[i]; i++) {
        if (it.optVals && it.optVals[i] == v) return it.opts[i];
      }
      return fmtInt(v);
    }
    case MK_ENUM_F: {
      float v = doc[it.key].as<float>();
      for (int i = 0; it.opts && it.opts[i]; i++) {
        if (it.optValsF && fabsf(it.optValsF[i] - v) < 0.01f) return it.opts[i];
      }
      return String(v, 1);
    }
    case MK_STRING:
    case MK_PATH: {
      const char *s = doc[it.key].as<const char *>();
      return s ? String(s) : String("");
    }
    default:
      return "";
  }
}

// Single-value helper for the few callers that need just one key.
String menuValueStr(const MenuItem &it) {
  if (!gCfg) return "";
  JsonDocument doc;
  configToJson(*gCfg, doc.to<JsonObject>());
  return menuValueStr(it, doc.as<JsonObjectConst>());
}

bool menuSave(const char *key, JsonDocument &doc) {
  String err;
  if (!configFromJson(*gCfg, doc["config"].as<JsonObjectConst>(), err)) {
    String p = "ERR ";
    p += err;
    displayPopup(p.c_str());
    return false;
  }
  storeSave(*gCfg);
  radioApplyPower(gCfg->powerDbm);
  radioSetMuted(gCfg->txDisabled);
  displayPopup("Guardado");
  return true;
}

void menuExecute(int action) {
  if (!gCfg) return;
  // KISS mode: the host application commands the transmitter, so the node does
  // not send from its own menu either (operator decision, tncHostDriven() in
  // main.cpp). The mute switch stays usable: it is not a transmission.
  if (tncKissActive() && (action == ACT_BEACON || action == ACT_TRACKER_BEACON ||
                          action == ACT_TELEM || action == ACT_TELEM_META)) {
    displayPopup("KISS: manda la app");
    return;
  }
  switch (action) {
    case ACT_BEACON:
      displayPopup(aprsSendManualBeacon(*gCfg) == RADIOLIB_ERR_NONE ? "Beacon OK"
                                                                    : "Beacon ERR");
      break;
    case ACT_TRACKER_BEACON:
      displayPopup(trackerBeaconNow(*gCfg) == RADIOLIB_ERR_NONE ? "Trk beacon"
                                                                : "Sin fix");
      break;
    case ACT_TELEM:
      displayPopup(aprsSendTelemetry(*gCfg) == RADIOLIB_ERR_NONE ? "Telem OK"
                                                                 : "Telem ERR");
      break;
    case ACT_TELEM_META:
      displayPopup(aprsSendTelemetryMeta(*gCfg) == RADIOLIB_ERR_NONE
                      ? "Meta OK"
                      : "Meta ERR");
      break;
    case ACT_MUTE:
      gCfg->txDisabled = !gCfg->txDisabled;
      storeSave(*gCfg);
      radioSetMuted(gCfg->txDisabled);
      displayPopup(gCfg->txDisabled ? "MUTE on" : "MUTE off");
      break;
    case ACT_BAT: {
      char b[40];
      snprintf(b, sizeof(b), "%.2fV INA %.2fV %+.0fmA",
               sensorsBatteryVolt(gSensorCache), gSensorCache.inaBusV,
               gSensorCache.inaCurrentMa);
      displayPopup(b);
      break;
    }
    case ACT_SET_COORDS: {
      // Sesion completa de captura (encender GPS, fijar, asentar, guardar y
      // apagar). La lleva el rastreador porque es quien gobierna el GPS; aqui
      // solo se le da la orden. Ver trackerSetCoordsStart() en tracker.h.
      trackerSetCoordsStart();
      displayPopup("Buscando GPS...");
      break;
    }
    // ★ ICONO DEL MAPA DEL PERFIL ACTIVO (2026-09-15): un toque = siguiente icono
    //   de la lista, guardado en el acto en `profileSymbol`/`profileOverlay` del
    //   perfil activo. Se escribe en la configuracion y se guarda con storeSave(),
    //   que es el mismo final que el resto del menu.
    //   ★ NO se pasa por cliTypedSet(): `profileSymbol` no es una clave suelta de
    //   la configuracion (es un array de cuatro), asi que el motor de `set` no la
    //   conoce. El aviso dice el NOMBRE y el par de codigos, que es lo que sale al
    //   aire y lo que se ve en el mapa.
    case ACT_PROFILE_ICON: {
      if (!gCfg) break;
      int p = (int)gCfg->smartBeaconPreset;
      if (p < 0 || p > 3) p = 0;
      int idx = 0;
      for (int i = 0; i < kIconoOpciones; i++) {
        if (gCfg->profileSymbol[p][0] == kIconoCodigo[i]) {
          idx = (i + 1) % kIconoOpciones;   // el siguiente de la lista
          break;
        }
      }
      gCfg->profileSymbol[p][0] = kIconoCodigo[idx];
      gCfg->profileSymbol[p][1] = '\0';
      gCfg->profileOverlay[p][0] = '/';   // los cuatro elegibles son de la tabla primaria
      gCfg->profileOverlay[p][1] = '\0';
      storeSave(*gCfg);
      char b[40];
      const char *perfil =
          (p == 1) ? "Peaton" : (p == 2) ? "Bici" : (p == 3) ? "Coche" : "Fijo";
      snprintf(b, sizeof(b), "%s: %s /%c", perfil, kIconoNombre[idx], kIconoCodigo[idx]);
      displayPopup(b);
      break;
    }
    case ACT_GPS_INFO: {
      const GpsData &g = gpsGet();
      char b[40];
      if (g.fix) {
        snprintf(b, sizeof(b), "fix %usat %.5f", g.sats, g.lat);
      } else {
        snprintf(b, sizeof(b), "GPS sin fix");
      }
      displayPopup(b);
      break;
    }
    case ACT_MSG_SEND: {
      const char *to = aprsLastHeardCall();
      if (gCfg->msgText[0] == '\0') {
        displayPopup("Mensaje vacio");
      } else if (to == nullptr || to[0] == '\0') {
        displayPopup("Nadie escuchado");
      } else {
        int16_t st = aprsSendMessage(*gCfg, to, gCfg->msgText);
        displayPopup(st == RADIOLIB_ERR_NONE ? "Mensaje enviado" : "Fallo TX");
      }
      break;
    }
    case ACT_SLEEP:
      if (powerUsbPresent()) {
        // fix #8: never sleep while USB is connected (consistent with the
        // battery monitor; avoids a node that cannot be reached until button).
        displayPopup("USB conectado: no duerme");
        break;
      }
      displayPopup("Durmiendo...");
      delay(800);
      powerSleepNow(*gCfg);
      break;
    case ACT_REBOOT:
      NVIC_SystemReset();
      break;
    case ACT_DFU:
      enterUf2Dfu();
      break;
    case ACT_RESET: {
      char keepCall[16], keepMgrs[64];
      bool keepRemote = gCfg->remoteEnabled;
      strncpy(keepCall, gCfg->callsign, sizeof(keepCall) - 1);
      keepCall[sizeof(keepCall) - 1] = 0;
      strncpy(keepMgrs, gCfg->managers, sizeof(keepMgrs) - 1);
      keepMgrs[sizeof(keepMgrs) - 1] = 0;
      *gCfg = DigiConfig();
      strncpy(gCfg->callsign, keepCall, sizeof(gCfg->callsign) - 1);
      strncpy(gCfg->managers, keepMgrs, sizeof(gCfg->managers) - 1);
      gCfg->remoteEnabled = keepRemote;
      storeSave(*gCfg);
      displayPopup("Reset OK");
      break;
    }
    case ACT_WIPE:
      storeWipe();
      *gCfg = DigiConfig();
      storeSave(*gCfg);
      NVIC_SystemReset();
      break;
  }
}

bool menuIsDestructive(int action) {
  return action == ACT_SLEEP || action == ACT_REBOOT || action == ACT_DFU ||
         action == ACT_RESET || action == ACT_WIPE;
}

// ---- Two-level menu helpers (single-button navigation) ---------------------
bool menuIsHeader(int abs) {
  return abs >= 0 && abs < kMenuCount && kMenu[abs].kind == MK_HEADER;
}

// Mascara del modo de trabajo actual (bit 0 repetidor, bit 1 rastreador,
// bit 2 ambos). Sin configuracion se ensena todo, para no dejar al usuario sin
// menu si algo fuera mal.
uint8_t menuModeBit() {
  if (!gCfg) return kMAll;
  switch (gCfg->mode) {
    case 0: return kMDigi;
    case 1: return kMTrk;
    case 2: return kMBoth;
    default: return kMAll;
  }
}

// Se ensena esta fila en el modo actual? (ver MenuItem::modes)
bool menuVisible(int abs) {
  if (abs < 0 || abs >= kMenuCount) return false;
  return (kMenu[abs].modes & menuModeBit()) != 0;
}

int menuHeaderCount() {
  int n = 0;
  for (int i = 0; i < kMenuCount; i++)
    if (menuIsHeader(i)) n++;
  return n;
}

// Absolute index of the nth category header (-1 when out of range).
int menuHeaderAbs(int nth) {
  int n = 0;
  for (int i = 0; i < kMenuCount; i++) {
    if (menuIsHeader(i)) {
      if (n == nth) return i;
      n++;
    }
  }
  return -1;
}

// First absolute index past the category that starts at catAbs.
int menuCatEnd(int catAbs) {
  for (int i = catAbs + 1; i < kMenuCount; i++)
    if (menuIsHeader(i)) return i;
  return kMenuCount;
}

// Cuantas filas VISIBLES hay en la categoria que empieza en catAbs.
int menuCatVisibleCount(int catAbs) {
  int n = 0;
  for (int i = catAbs + 1; i < menuCatEnd(catAbs); i++) {
    if (menuVisible(i)) n++;
  }
  return n;
}

// Indice ABSOLUTO de la n-esima fila visible de la categoria (0-based), o -1 si
// no existe. El menu se navega por filas visibles (v), pero el array del menu es
// fijo: esta es la traduccion entre los dos mundos.
int menuCatVisibleAbs(int catAbs, int nth) {
  int n = 0;
  for (int i = catAbs + 1; i < menuCatEnd(catAbs); i++) {
    if (!menuVisible(i)) continue;
    if (n == nth) return i;
    n++;
  }
  return -1;
}

// Rows in the current view: a virtual "back" row at the TOP and at the BOTTOM,
// with the real items in between. Operator request: with a single button,
// walking to the end of the list just to go back is a pain, and the very-long
// press is no longer used for navigation.
int menuViewCount() {
  if (gMenuCat < 0) return menuHeaderCount() + 2;
  // Solo cuentan las filas visibles en el modo actual (ver menuVisible).
  return menuCatVisibleCount(gMenuCat) + 2;
}

bool menuVirtualRow(int v) { return v <= 0 || v >= menuViewCount() - 1; }

// Item under the cursor, or nullptr on the virtual "back" rows.
const MenuItem *menuCurrent() {
  int vc = menuViewCount();
  if (gMenuSel <= 0 || gMenuSel >= vc - 1) return nullptr;
  if (gMenuCat < 0) {
    int abs = menuHeaderAbs(gMenuSel - 1);
    return (abs >= 0) ? &kMenu[abs] : nullptr;
  }
  // gMenuSel - 1 = fila visible; se traduce a indice absoluto del array.
  int abs = menuCatVisibleAbs(gMenuCat, gMenuSel - 1);
  return (abs >= 0) ? &kMenu[abs] : nullptr;
}

String menuCatTitle(int catAbs) {
  if (!menuIsHeader(catAbs)) return String("MENU");
  String l = kMenu[catAbs].label;
  l.replace("=", "");
  l.trim();
  return l;
}

// RED DE SEGURIDAD para las filas que se esconden segun el modo: si el usuario
// cambia de modo TENIENDO EL MENU ABIERTO, el numero de filas visibles cambia
// de golpe y el cursor (gMenuSel) puede quedarse apuntando a una fila que ya no
// existe. Eso dejaria la pantalla sin resaltado: el usuario veria el menu pero
// no sabria donde esta el cursor, y con un solo boton eso es quedarse atascado.
//
// Se reengancha el cursor y la ventana de scroll a la lista que hay AHORA. Se
// llama al navegar y al dibujar, porque el numero de filas puede cambiar sin que
// el usuario toque el boton (al cambiar el modo de trabajo).
void menuClampCursor() {
  const int vc = menuViewCount();
  if (vc <= 0) return;
  if (gMenuSel > vc - 1) gMenuSel = vc - 1;  // esa fila ya no existe
  if (gMenuSel < 0) gMenuSel = 0;
  // Ventana de scroll: comparaciones directas, sin sumas (sumar kMenuRows hace
  // que el compilador avise de posible desbordamiento).
  if (gMenuTop < 0) gMenuTop = 0;
  if (gMenuSel < gMenuTop) gMenuTop = gMenuSel;
  if (gMenuSel - gMenuTop > kMenuRows - 1) gMenuTop = gMenuSel - (kMenuRows - 1);
}

void menuMove(int dir) {
  int vc = menuViewCount();
  if (vc <= 0) return;
  gMenuSel = (gMenuSel + dir + vc) % vc;
  menuClampCursor();
}

/* En que opcion esta un campo de lista corta (0..n-1). */
int editIndex(const MenuItem &it) {
  if (!it.opts || !it.optVals) return 0;
  JsonDocument doc;
  if (gCfg) configToJson(*gCfg, doc.to<JsonObject>());
  const int cur = doc[it.key].as<int>();
  for (int i = 0; it.opts[i]; i++) if (it.optVals[i] == cur) return i;
  return 0;
}

/* Texto de la opcion que hay AHORA en el buffer de edicion (para la pantalla). */
const char *editOptLabel(const MenuItem &it) {
  if (!it.opts) return "";
  const int i = atoi(gEditBuf);
  if (i < 0) return it.opts[0];
  for (int k = 0; it.opts[k]; k++) if (k == i) return it.opts[k];
  return it.opts[0];
}

/* Pasa a la opcion siguiente (la ultima vuelve a la primera). */
void editNextOpt(const MenuItem &it) {
  if (!it.opts) return;
  int n = 0;
  while (it.opts[n]) n++;
  if (n <= 0) return;
  int i = atoi(gEditBuf);
  i = (i + 1) % n;
  snprintf(gEditBuf, sizeof(gEditBuf), "%d", i);
}

/* Primera fila REAL de la categoria abierta (la fila 1: la 0 es "< Volver").
   Sirve para reconocer la categoria del modo de trabajo, que tiene su propia
   pantalla. Devuelve nullptr si no hay. */
const MenuItem *menuCurrent0() {
  if (gMenuCat < 0) return nullptr;
  const int abs = menuCatVisibleAbs(gMenuCat, 0);
  return (abs >= 0) ? &kMenu[abs] : nullptr;
}

void menuBeginEdit(const MenuItem &it) {
  gEditing = true;
  gEditPos = 0;
  // Las listas cortas (mode, smartBeaconPreset, digiMode) trabajan con el INDICE
  // de la opcion en el buffer: el toque corto pasa a la siguiente y la pantalla
  // ensena la palabra. Asi no hay que recorrer el abecedario para elegir
  // "Rastreador" con un solo boton.
  if (it.kind == MK_ENUM_CYCLE) {
    snprintf(gEditBuf, sizeof(gEditBuf), "%d", editIndex(it));
    gEditPos = (int)strlen(gEditBuf);
    return;
  }
  String v = menuValueStr(it);
  strncpy(gEditBuf, v.c_str(), sizeof(gEditBuf) - 1);
  gEditBuf[sizeof(gEditBuf) - 1] = '\0';
  gEditPos = (int)strlen(gEditBuf);
}

const char *kEditChars = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/#.,_:~";

void menuEditShort() {
  const MenuItem *itp = menuCurrent();
  if (!itp) return;
  const MenuItem &it = *itp;
  if (it.kind == MK_INT) {
    long v = atol(gEditBuf);
    v += it.step;
    if (v > it.max) v = it.min;
    if (v < it.min) v = it.min;
    snprintf(gEditBuf, sizeof(gEditBuf), "%ld", v);
    return;
  }
  if (it.kind == MK_ENUM_CYCLE) {
    // Lista corta: un toque = siguiente opcion. El boton largo guarda y sale.
    editNextOpt(it);
    return;
  }
  if (it.kind == MK_PATH) {
    // Rutas APRS: se recorren a toques (escribir "WIDE1-1,WIDE2-1" a mano con
    // un solo botón sería una tortura). El botón largo guarda.
    int idx = 0;
    if (it.opts) {
      for (int i = 0; it.opts[i]; i++) {
        if (strcmp(it.opts[i], gEditBuf) == 0) {
          idx = i + 1;
          break;
        }
      }
      if (it.opts[idx] == nullptr) idx = 0;  // vuelta al principio
      strncpy(gEditBuf, it.opts[idx], sizeof(gEditBuf) - 1);
      gEditBuf[sizeof(gEditBuf) - 1] = '\0';
    }
    return;
  }
  // string/float: cycle character at cursor
  if (gEditPos >= (int)sizeof(gEditBuf) - 1) return;
  char cur = gEditBuf[gEditPos];
  char next = kEditChars[0];
  if (cur != '\0') {
    const char *p = strchr(kEditChars, cur);
    if (p && p[1] != '\0') next = p[1];
  }
  if (gEditPos == (int)strlen(gEditBuf)) gEditBuf[gEditPos + 1] = '\0';
  gEditBuf[gEditPos] = next;
}

void menuEditLong() {
  const MenuItem *itp = menuCurrent();
  if (!itp) return;
  const MenuItem &it = *itp;
  if (it.kind == MK_INT || it.kind == MK_PATH || it.kind == MK_ENUM_CYCLE) {
    // Numbers, path lists and the short option lists: the long press is the
    // "accept" gesture (there is nothing else to write: the short press already
    // picked the value). En las listas cortas, el indice del buffer se convierte
    // al VALOR de la opcion (mode: 0/1/2...).
    JsonDocument doc;
    if (it.kind == MK_INT) doc["config"][it.key] = atol(gEditBuf);
    else if (it.kind == MK_ENUM_CYCLE) doc["config"][it.key] = it.optVals[atoi(gEditBuf)];
    else doc["config"][it.key] = gEditBuf;
    if (menuSave(it.key, doc)) {
      gEditing = false;
    }
    return;
  }
  // Character editor. The wheel ends with two special characters that the long
  // press applies:
  //   '<' = delete the character before the cursor
  //   '~' = finish and save
  const char cur = gEditBuf[gEditPos];
  if (cur == '~') {
    char clean[sizeof(gEditBuf)];
    strncpy(clean, gEditBuf, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    char *cut = strpbrk(clean, "~<");  // never store the control characters
    if (cut) *cut = '\0';
    JsonDocument doc;
    if (it.kind == MK_FLOAT) {
      doc["config"][it.key] = atof(clean);
    } else {
      doc["config"][it.key] = clean;
    }
    if (menuSave(it.key, doc)) {
      gEditing = false;
    }
    return;
  }
  if (cur == '<') {
    if (gEditPos > 0) {
      memmove(&gEditBuf[gEditPos - 1], &gEditBuf[gEditPos],
              strlen(&gEditBuf[gEditPos]) + 1);
      gEditPos--;
    }
    return;
  }
  if (gEditPos < (int)strlen(gEditBuf)) gEditPos++;
}

// Double tap while editing: leave the value as it was (public wrapper below).

/* PANTALLA DEL MODO DE TRABAJO: las tres opciones y "Volver", de una vez. */
void drawModoScreen() {
  headerBar(String(gSub.titulo ? gSub.titulo : "Modo"));
  const int filas = 4;   // 3 modos + Volver
  for (int k = 0; k < filas; k++) {
    int y = 14 + k * 12;
    // La fila 3 es "Volver"; las otras tres son los modos.
    // OJO: gModoFlag tiene 3 elementos (los 3 modos) y el bucle recorre 4 filas
    // (la cuarta es "Volver"), asi que hay que comprobar el indice. Sin esto el
    // compilador avisaba de comportamiento indefinido, y con razon.
    bool esVolver = (k == 3);
    bool elegido = (!esVolver && k < 3) ? gModoFlag[k] : false;
    String s = esVolver ? String("< Volver") : String((gSub.opts && gSub.opts[k]) ? gSub.opts[k] : "");
    if (elegido) s += "  *";     // marca de "este es el modo que tienes puesto"
    if ((int)s.length() > 20) s = s.substring(0, 20);
    if (k == gModoRow) {
      gOled->setColor(WHITE);
      gOled->fillRect(0, y, 128, 12);
      gOled->setColor(BLACK);
      gOled->drawString(1, y + 1, s);
      gOled->setColor(WHITE);
    } else {
      gOled->drawString(1, y + 1, s);
    }
  }
}
void drawMenu() {
  String title = "MENU";
  if (gEditing) title = "EDITANDO";
  else if (gMenuCat >= 0) title = menuCatTitle(gMenuCat);
  headerBar(title);

  // Build the values snapshot once per redraw (improvement: avoid 5 JSON
  // serializations per frame).
  JsonDocument valsDoc;
  if (gCfg) configToJson(*gCfg, valsDoc.to<JsonObject>());
  JsonObjectConst vals = valsDoc.as<JsonObjectConst>();
  int vc = menuViewCount();
  // Reengancha el cursor a las filas que hay AHORA (ver menuClampCursor): el
  // numero de filas cambia solo, sin tocar el boton, al cambiar el modo.
  menuClampCursor();
  for (int row = 0; row < kMenuRows; row++) {
    int v = gMenuTop + row;
    if (v >= vc) break;
    // LA FILA MIDE 12 px Y LA LETRA 10: se dibuja con 2 px desde arriba, asi que
    // entre el texto de una fila y el de la siguiente quedan 2 px de aire, y por
    // arriba y por abajo lo mismo. Antes se dibujaba con y+1 dentro de una fila de
    // 9: el texto quedaba pegado a la linea de abajo y se veia feo (lo cazo el
    // operador: "esta como pegada a la parte inferior, sin dejar ni un pixel").
    // Cuentas de la pantalla (128x64): la barra de titulo ocupa hasta y=13, asi que
    // las filas empiezan en 14. Con 5 filas de 13 px: 14 + 13*4 + 10 = 76... no
    // cabe, pero la ultima fila solo necesita 10 px de texto: 14 + 13*4 = 66, mas
    // 10 = 76 > 64. Por eso la fila es de 10 px y el texto se centra DENTRO de su
    // fila: 5 filas x 10 = 50 px, de 14 a 64. Lo que cambia respecto a antes es que
    // el texto va centrado (offset 0 = pegado arriba, offset 1 = un pixel de aire
    // arriba Y abajo), en vez de quedar pegado a la linea de abajo.
    // MISMO dibujo que la pantalla del modo de trabajo (la que el operador aprobo):
    // fila de 12 px y el texto a 1 px dentro, asi el recuadro del cursor tiene un
    // pixel de aire por arriba y por abajo. Antes era fila de 10 y texto a 0: el
    // recuadro quedaba del alto justo de la letra y parecia pegado.
    const int kFilaAlto = 12;
    const int kTextoOff = 1;
    int y = 14 + row * kFilaAlto;
    String s;
    const bool virtualRow = menuVirtualRow(v);
    if (virtualRow) {
      s = (gMenuCat < 0) ? "Salir" : "< Volver";
    } else if (gMenuCat < 0) {
      s = menuCatTitle(menuHeaderAbs(v - 1));
      s += " >";
    } else if (gEditing && v == gMenuSel) {
      const MenuItem *it = menuCurrent();
      if (!it) {
        s = "";
      } else if (it->kind == MK_ENUM_CYCLE) {
        // Lista corta: se ensena la PALABRA entera de la opcion elegida, no el
        // numero ni un trozo del nombre del campo (que era lo que salia antes).
        s = it->label;
        s += ":";
        s += editOptLabel(*it);
      } else {
        s = String(it->label).substring(0, 6) + ":" + gEditBuf;
      }
    } else {
      // Traduccion de "fila visible" a indice del array del menu (las filas que
      // no aplican al modo actual no se cuentan ni se dibujan).
      const int abs = menuCatVisibleAbs(gMenuCat, v - 1);
      const MenuItem *it = (abs >= 0) ? &kMenu[abs] : nullptr;
      if (!it) {
        s = "";
      } else if (it->kind == MK_ACTION) {
        s = it->label;
      } else {
        s = it->label;
        s += "=";
        s += menuValueStr(*it, vals);
      }
    }
    if (s.length() > 21) s = s.substring(0, 21);
    if (v == gMenuSel) {
      // real highlight on a monochrome panel: white bar + black text
      gOled->setColor(WHITE);
      gOled->fillRect(0, y, 128, kFilaAlto);
      gOled->setColor(BLACK);
      gOled->drawString(1, y + kTextoOff, s);
      gOled->setColor(WHITE);
    } else {
      gOled->drawString(1, y + kTextoOff, s);
    }
  }
}

}  // namespace

// --------------------------------------------------------------- API --------

// Guarda una posicion en la configuracion (y por tanto en la flash). Lo usan el
// menu ("Fijar coords actuales") y la sesion de captura del rastreador
// (trackerSetCoordsTick), para no tener el JSON duplicado en dos sitios.
// OJO: va FUERA del namespace anonimo A PROPOSITO (por eso esta aqui abajo, y no
// junto a menuSave()): dentro tendria enlace interno y el enlazador no lo veria
// desde tracker.cpp ("undefined reference to displaySaveCoords").
bool displaySaveCoords(double lat, double lon) {
  JsonDocument doc;
  doc["config"]["latitude"] = lat;
  doc["config"]["longitude"] = lon;
  return menuSave("coords", doc);
}

void displayBindConfig(DigiConfig *cfg) { gCfg = cfg; }

// La OLED va por I2C y no comparte periferico con la radio, asi que no necesita nada de
// despues de radioSetup(). Esa segunda parte solo hace falta en la tinta electronica, que
// comparte SPIM3 con el arranque de la radio (ver epaper_techo.cpp).
void displayInitTrasRadio() {}
void displayArrancaPantalla() {}

void displayDiagTexto(char *out, size_t n) {
  if (n) snprintf(out, n, "display: OLED, no hay tinta electronica");
}

void displayInit() {
  Wire.begin();  // variant pins
  for (uint8_t addr : kAddrs) {
    if (!addrAcks(addr)) continue;
    if (addrIsSh1106(addr)) {
      gSh = new SH1106Wire(addr, kSda, kScl, GEOMETRY_128_64, I2C_ONE, kI2cFreq);
      gOled = gSh;
      if (!gSh->init()) {
        delete gSh;
        gSh = nullptr;
        gOled = nullptr;
        continue;
      }
    } else {
      gSsd = new SSD1306Wire(addr, kSda, kScl, GEOMETRY_128_64, I2C_ONE,
                             kI2cFreq);
      gOled = gSsd;
      if (!gSsd->init()) {
        delete gSsd;
        gSsd = nullptr;
        gOled = nullptr;
        continue;
      }
    }
    gOled->displayOn();
    gOled->clear();
    gOled->setTextAlignment(TEXT_ALIGN_LEFT);
    gOled->setFont(ArialMT_Plain_10);
    gDisplayOn = true;
    gLastActivityMs = millis();
    if (diagTrazaArranque()) {
      Serial.print("display: OLED OK @0x");
      Serial.println(addr, HEX);
    }
    return;
  }
  if (diagTrazaArranque()) Serial.println("display: not found");
}

bool displayPresent() { return gOled != nullptr; }
bool displayIsOn() { return gOled && gDisplayOn; }

void displayWake() {
  if (!gOled) return;
  gOled->displayOn();
  gDisplayOn = true;
  gLastDrawMs = 0;
  gLastActivityMs = millis();
}

void displaySleep() {
  if (!gOled) return;
  gOled->displayOff();
  gDisplayOn = false;
}

// Arm the 4 s boot splash (drawn by displayRefresh, non-blocking).
void displaySplash() {
  if (!gOled) return;
  gSplashUntil = millis() + kSplashMs;
  gLastDrawMs = 0;
  gLastActivityMs = millis();
}

// Bluetooth pairing splash. Wakes the panel (the operator has to read the PIN
// even if the screen had timed out) and arms a hard 60 s limit.
void displayPinSplash(const char *pin, bool fromStack) {
  if (!gOled || pin == nullptr || pin[0] == '\0') return;
  if (!gDisplayOn) displayWake();
  strncpy(gPinSplash, pin, sizeof(gPinSplash) - 1);
  gPinSplash[sizeof(gPinSplash) - 1] = '\0';
  gPinSplashFromStack = fromStack;
  gPinSplashUntil = millis() + kPinSplashMaxMs;
  gLastDrawMs = 0;
  gLastActivityMs = millis();
}

void displayPinSplashClear() {
  gPinSplashUntil = 0;
  gPinSplash[0] = '\0';
  gLastDrawMs = 0;
  gLastActivityMs = millis();
}

bool displayPinSplashActive() { return gPinSplashUntil != 0; }

// ★ T-ECHO PROJECT BUTTER: en la OLED este gancho no hace falta para el refresco (su
//   pantalla se redibuja por I2C en ~25 ms y no bloquea 1,5 s como la tinta), pero se
//   guarda y se llama en la UNICA espera larga que hay aqui: el popup con cuenta atras.
static void (*gPumpBoton)(void) = nullptr;
void displaySetPumpBoton(void (*fn)(void)) { gPumpBoton = fn; }

void displayNextScene(bool porToque) {
  (void)porToque;   // la OLED no aplaza repintados: no hay nada que agrupar
  gScene = (gScene + 1) % kSceneCount;
  gLastSceneMs = millis();
  // The user is browsing: hold the auto-advance for a while so the screen does
  // not change under their eyes (every tap renews the hold).
  gAutoAdvanceHoldUntil = millis() + kAutoAdvanceHoldMs;
  gLastDrawMs = 0;
  gLastActivityMs = millis();
}

void displayPopup(const char *text) {
  strncpy(gPopup, text, sizeof(gPopup) - 1);
  gPopup[sizeof(gPopup) - 1] = '\0';
  gPopupUntil = millis() + kPopupMs;
  gLastDrawMs = 0;
  gLastActivityMs = millis();
}

// ===========================================================================
//  ★★ "FIJAR COORDS" EN LA OLED: QUE SE VEA MIENTRAS TARDA (2026-09-15) ★★
// ===========================================================================
// POR QUE HACE FALTA: la sesion de captura la lleva el rastreador
// (trackerSetCoordsStart/Tick) y ahora puede tardar LO QUE HAGA FALTA -- sin tope
// de tiempo, decision del operador --. Con el aviso normal de 1,5 s (kPopupMs) el
// operador veia "Buscando GPS..." tres segundos y despues una pantalla que no
// decia NADA durante minutos: parecia colgada. Y tampoco habia forma de salir.
//
// QUE HACE ESTO: mientras hay captura, el aviso se RENUEVA en cada refresco con el
// paso en el que esta (satelites a la vista, muestras n/N); al terminar deja el
// resultado puesto unos segundos (coordenadas guardadas, error o cancelacion).
// Aqui NO hay problema de refrescos: la OLED se redibuja a 400 ms y no cuesta nada
// (en la tinta esto mismo se hace con una pantalla completa y a escalones, porque
// cada refresco son 1,5 s: ver pintaSesionCoords en epaper_techo.cpp).
//
// ★ LA SALIDA: el menu de la OLED sigue respondiendo (el aviso es solo un recuadro
//   encima). Si la captura se cancela desde ahi (o desde el USB con `set`, que no
//   toca la sesion), el estado pasa a IDLE y el bucle de abajo lo dice con el
//   aviso "Fijar coords cancelado".
void displayCoordsPopup(const char *text) {
  if (!text) return;
  strncpy(gPopup, text, sizeof(gPopup) - 1);
  gPopup[sizeof(gPopup) - 1] = '\0';
  gPopupUntil = millis() + 3000;   // se renueva en cada refresco mientras dura
  gLastDrawMs = 0;
}

void displayCoordsEstadoTick() {
  // Ultimo estado de la sesion que se ha visto (TRK_COORDS_IDLE = ninguna).
  static uint8_t gCoordsVisto = TRK_COORDS_IDLE;
  static uint32_t gCoordsSatsMs = 0;
  const uint8_t estado = trackerSetCoordsEstado();
  char b[40];

  // ★ MIENTRAS BUSCA, LOS SATELITES A LA VISTA SE RENUEVAN CADA SEGUNDO (2026-09-15):
  //   sin fijacion son el UNICO dato que se mueve, y ver el numero subir es lo que
  //   le dice al operador que el receptor trabaja y que solo falta esperar. No
  //   cuesta nada (la OLED se redibuja a 400 ms), y evita la sensacion de cuelgue
  //   que era justo lo que habia que quitar.
  if (estado == TRK_COORDS_BUSCANDO) {
    const uint32_t ahora = millis();
    if (gCoordsVisto != TRK_COORDS_BUSCANDO || (ahora - gCoordsSatsMs) >= 1000u) {
      gCoordsSatsMs = ahora;
      gCoordsVisto = TRK_COORDS_BUSCANDO;
      snprintf(b, sizeof(b), "Buscando GPS... %usat",
               (unsigned)gpsGet().satsInView);
      displayCoordsPopup(b);
    }
    return;
  }

  if (estado == gCoordsVisto) return;
  const uint8_t antes = gCoordsVisto;   // hay que guardarlo ANTES de pisarlo
  gCoordsVisto = estado;
  switch (estado) {
    case TRK_COORDS_ASENTANDO:
      snprintf(b, sizeof(b), "GPS %u/%u asentando", (unsigned)trackerSetCoordsDone(),
               (unsigned)trackerSetCoordsNeed());
      displayCoordsPopup(b);
      break;
    case TRK_COORDS_GUARDADO: {
      const GpsData &g = gpsGet();
      snprintf(b, sizeof(b), "Guardado %.5f %.5f", g.lat, g.lon);
      displayCoordsPopup(b);   // se queda unos segundos y luego caduca solo
      break;
    }
    case TRK_COORDS_ERROR:
      displayCoordsPopup("Fijar coords: error al guardar");
      break;
    case TRK_COORDS_IDLE:
      // Solo interesa si ANTES habia una sesion EN MARCHA: es el caso de que la
      // hayan cancelado (boton de la tinta, o el USB). Si antes estaba en un
      // estado final, volver a IDLE es solo "se cerro la pantalla del resultado".
      if (antes == TRK_COORDS_BUSCANDO || antes == TRK_COORDS_ASENTANDO) {
        displayCoordsPopup("Fijar coords cancelado");
      }
      break;
    default:
      break;
  }
}

void displayNoteRx(const char *from, float rssi, float snr, const char *kind) {
  pushRx(from, rssi, snr, kind);
  gLastActivityMs = millis();
  char b[40];
  snprintf(b, sizeof(b), "%s: %s %.0f/%.1f", kind, from, rssi, snr);
  displayPopup(b);
}

void displayNoteDigi(const char *from, float rssi, float snr) {
  pushTx("DIGI", from);
  gLastActivityMs = millis();
  char b[40];
  snprintf(b, sizeof(b), "Digi: %s %.0f/%.1f", from, rssi, snr);
  displayPopup(b);
}

void displayNoteTx(const char *what) {
  pushTx(what, "");
  gLastActivityMs = millis();
  char b[40];
  snprintf(b, sizeof(b), "%s TX", what);
  displayPopup(b);
}

// Popup (NavaTastic-style notification box): black box with a white frame,
// sized to the text and centred on the screen, with the corners clipped.
// The message is split on ':' (kind / detail) and, when a line still does not
// fit, wrapped by words into at most kPopupMaxLines centred lines.
int popupWrapLines(const char *text, char out[][kPopupMaxChars], int maxLines,
                   int maxWidth) {
  char work[64];
  strncpy(work, text, sizeof(work) - 1);
  work[sizeof(work) - 1] = '\0';

  int n = 0;
  char *rest = work;
  char *colon = strchr(work, ':');
  if (colon != nullptr && colon != work) {
    *colon = '\0';
    char *t = work;
    while (*t == ' ') t++;
    strncpy(out[n], t, kPopupMaxChars - 1);
    out[n][kPopupMaxChars - 1] = '\0';
    n++;
    rest = colon + 1;
  }

  char line[kPopupMaxChars] = "";
  char *save = nullptr;
  for (char *w = strtok_r(rest, " ", &save); w != nullptr;
       w = strtok_r(nullptr, " ", &save)) {
    if (n >= maxLines) break;
    char test[kPopupMaxChars];
    if (line[0] != '\0') {
      snprintf(test, sizeof(test), "%s %s", line, w);
    } else {
      snprintf(test, sizeof(test), "%s", w);
    }
    // Fits (or the line is still empty: a single long word is kept as is).
    if (line[0] == '\0' || (int)gOled->getStringWidth(test) <= maxWidth) {
      strncpy(line, test, sizeof(line) - 1);
      line[sizeof(line) - 1] = '\0';
      continue;
    }
    strncpy(out[n], line, kPopupMaxChars - 1);
    out[n][kPopupMaxChars - 1] = '\0';
    n++;
    line[0] = '\0';
  }
  if (line[0] != '\0' && n < maxLines) {
    strncpy(out[n], line, kPopupMaxChars - 1);
    out[n][kPopupMaxChars - 1] = '\0';
    n++;
  }
  return n;
}

void drawPopupBox() {
  gOled->setFont(ArialMT_Plain_10);  // measure and draw with the same font
  gOled->setTextAlignment(TEXT_ALIGN_LEFT);

  const int hPad = 5;
  const int vPad = 3;
  const int lineH = 10;  // ArialMT_Plain_10 line pitch (same as the scenes)
  const int maxTw = 116;  // widest text that still fits a 126 px box

  char lines[kPopupMaxLines][kPopupMaxChars];
  int n = popupWrapLines(gPopup, lines, kPopupMaxLines, maxTw);
  if (n == 0) return;

  // Countdown line (used by the "going to sleep" notice).
  char cd[12] = "";
  uint32_t now = millis();
  if (gPopupCountdownUntil > now && n < kPopupMaxLines) {
    uint32_t leftMs = gPopupCountdownUntil - now;
    snprintf(cd, sizeof(cd), "%us", (unsigned)((leftMs + 999) / 1000));
    strncpy(lines[n], cd, kPopupMaxChars - 1);
    lines[n][kPopupMaxChars - 1] = '\0';
    n++;
  }

  int widths[kPopupMaxLines];
  int tw = 0;
  for (int i = 0; i < n; i++) {
    widths[i] = (int)gOled->getStringWidth(lines[i]);
    if (widths[i] > tw) tw = widths[i];
  }
  int bw = tw + hPad * 2;
  if (bw > 126) bw = 126;
  int bh = n * lineH + vPad * 2;
  int bx = (128 - bw) / 2;
  int by = (64 - bh) / 2;

  gOled->setColor(BLACK);  // clear a slightly larger area around the box
  gOled->fillRect(bx - 2, by - 2, bw + 4, bh + 4);
  gOled->setColor(WHITE);
  gOled->drawRect(bx, by, bw, bh);
  gOled->setColor(BLACK);  // clipped corners (NavaTastic look)
  gOled->fillRect(bx, by, 1, 1);
  gOled->fillRect(bx + bw - 1, by, 1, 1);
  gOled->fillRect(bx, by + bh - 1, 1, 1);
  gOled->fillRect(bx + bw - 1, by + bh - 1, 1, 1);
  gOled->setColor(WHITE);
  for (int i = 0; i < n; i++) {
    gOled->drawString(bx + (bw - widths[i]) / 2, by + vPad + i * lineH, lines[i]);
  }
}

// CUENTA ATRAS DE CIERRE DEL MENU.
//
// PARA QUE SIRVE (peticion del operador, 2026-09-13): el menu se cierra solo tras
// 20 s sin tocar nada y la pantalla vuelve al carrusel de siempre. Se avisa con
// los segundos que quedan, porque un menu que se cierra SIN AVISAR es de lo mas
// desesperante: el usuario no sabe si se ha cerrado solo o si ha pulsado algo.
//
// El hueco del aviso se RESERVA recortando el titulo un poco antes de tiempo, para
// que el titulo no cambie de tamano de golpe justo al aparecer los numeros.
void drawMenuCountdown() {
  if (gEditing) return;  // mientras se escribe un valor, no se agobia con avisos
  const uint32_t t = millis() - gLastActivityMs;
  if (t + kMenuCountdownMs < kMenuIdleCloseMs) return;
  uint32_t seg = (kMenuIdleCloseMs - t + 999u) / 1000u;
  if (seg == 0) seg = 1;
  gOled->setFont(ArialMT_Plain_10);
  int x0 = statusPillX0() - 4 - (int)gOled->getStringWidth("00s");
  if (x0 < 0) return;
  gOled->setColor(WHITE);
  gOled->drawString(x0, 0, String((unsigned)seg) + "s");
}

// ★ AVISO SONORO DE BATERIA BAJA: en esta version (pantalla OLED, placas Faketec) NO hay
// zumbador: el pin PIN_BUZZER solo existe en el T-Echo Plus, donde la implementacion de
// verdad vive en src/epaper_techo.cpp. Aqui tiene que EXISTIR igual, porque quien la llama
// es power.cpp, que se compila en todas las placas (si falta, el enlace de las Faketec
// falla con "undefined reference to displayLowBatTone()").
void displayLowBatTone() {}

// ★★ AQUI FALTABAN DOS FUNCIONES Y LAS FAKETEC NO COMPILABAN (arreglado 2026-09-15) ★★
// `main.cpp` llama a estas dos en el bucle desde el commit d16d092 (la sesion de la pantalla
// de tinta), pero solo estaban escritas en `epaper_techo.cpp`, que en las Faketec NO se
// compila. Resultado: "undefined reference to displayBacklightKick/Tick" al enlazar las
// Faketec, y nadie lo habia visto porque desde entonces solo se compilaba el T-Echo.
// Aqui (pantalla OLED) no hay luz de fondo que gobernar, asi que no hacen nada: es justo lo
// que dice el comentario de display.h.
void displayBacklightKick() {}
void displayBacklightTick(uint32_t nowMs) { (void)nowMs; }

// Pitido del zumbador: solo el T-Echo Plus tiene zumbador (PIN_BUZZER, P0.06), y alli la
// implementacion de verdad esta en epaper_techo.cpp. Aqui tiene que existir porque
// main.cpp la llama al pulsar un boton. Esta placa no tiene zumbador: no suena.
void displayBeep() {}

// El boton capacitivo (que NAVEGA por el menu) solo existe en el T-Echo: en esta placa el
// menu se maneja con los botones de siempre (menuUp/menuDown). No hace nada, pero tiene que
// existir porque main.cpp la llama.
void menuNavigate() {}

// Blocking popup: shows the box (on a clean screen) for totalMs and returns.
// Used right before going to sleep, so the countdown is actually seen.
void displayPopupWait(const char *text, uint32_t totalMs) {
  if (!gOled) return;
  if (!gDisplayOn) displayWake();
  gLastActivityMs = millis();
  strncpy(gPopup, text, sizeof(gPopup) - 1);
  gPopup[sizeof(gPopup) - 1] = '\0';
  uint32_t end = millis() + totalMs;
  gPopupUntil = end;
  gPopupCountdownUntil = end;
  while ((int32_t)(millis() - end) < 0) {
    gOled->clear();
    drawPopupBox();
    gOled->display();
    delay(100);
    if (gPumpBoton) gPumpBoton();   // el boton no se queda sin mirar ni aqui
  }
  gPopupCountdownUntil = 0;
  gPopupUntil = 0;
  gPopup[0] = '\0';
  gLastDrawMs = 0;
}

bool menuIsOpen() { return gMenuOpen; }

void menuOpen() {
  gMenuOpen = true;
  gMenuCat = -1;  // start at the category list
  // Start on the virtual "Salir" row: if the user opened the menu by mistake one
  // long press gets out, without scrolling the whole list (operator request).
  gMenuSel = 0;
  gMenuTop = 0;
  gMenuCatRow = 1;
  gEditing = false;
  gLastDrawMs = 0;
  gLastActivityMs = millis();
}

void menuClose() {
  gMenuOpen = false;
  gSub.activa = false;
  gEditing = false;
  gLastDrawMs = 0;
  gLastActivityMs = millis();
}

void menuShort() {
  if (gSub.activa) {
    // Moverse por las TRES opciones y la fila "Volver" (4 filas).
    gModoRow = (gModoRow + 1) % 4;
    gLastDrawMs = 0;
    gLastActivityMs = millis();
    return;
  }
  if (gEditing) {
    menuEditShort();
  } else {
    menuMove(1);
  }
  gLastDrawMs = 0;
  gLastActivityMs = millis();
}

void menuLong() {
  if (gSub.activa) {
    // CONFIRMAR. Si la fila es "Volver" (3), se sale sin cambiar nada. Si es una de
    // las tres opciones, se guarda el modo y se vuelve a la lista de categorias.
    if (gModoRow < 3) {
      JsonDocument doc;
      doc["config"][gSub.key] = gSub.vals[gModoRow];
      menuSave(gSub.key, doc);
      gModoFlag[0] = gModoFlag[1] = gModoFlag[2] = false;
      if (gModoRow < 3) gModoFlag[gModoRow] = true;
    }
    gSub.activa = false;
    gMenuCat = -1;               // vuelve a la lista de categorias
    gMenuSel = gMenuCatRow;
    gMenuTop = 0;
    gLastDrawMs = 0;
    gLastActivityMs = millis();
    return;
  }
  if (gEditing) {
    menuEditLong();
    gLastDrawMs = 0;
    return;
  }
  // Virtual "back" rows (top and bottom of every list): "Salir" for the
  // category list, "< Volver" inside a category.
  if (menuVirtualRow(gMenuSel)) {
    if (gMenuCat < 0) {
      menuClose();
    } else {
      gMenuCat = -1;
      gMenuSel = gMenuCatRow;  // back on the category we came from
      gMenuTop = 0;
      gLastDrawMs = 0;
    }
    return;
  }
  if (gMenuCat < 0) {  // entering a category
    int abs = menuHeaderAbs(gMenuSel - 1);
    if (abs >= 0) {
      gMenuCatRow = gMenuSel;  // remember it for the way back
      gMenuCat = abs;
      gMenuSel = 0;  // start on the "< Volver" row (safe default)
      gMenuTop = 0;
      gLastDrawMs = 0;
      // LA CATEGORIA "MODO DE TRABAJO" ES SU PROPIA PANTALLA: al entrar se ven las
      // tres opciones y "Volver" de una vez, en lugar de una lista con una unica
      // fila "Modo" que llevaba a otro submenu (peticion del operador: "no tiene
      // sentido"). Se reconoce porque su primera fila es el campo "mode".
      const MenuItem *prim = menuCurrent0();
      if (prim && prim->key && strcmp(prim->key, "mode") == 0) {
        gSub.activa = true;
        gSub.titulo = "Modo de trabajo";
        gSub.opts = prim->opts;
        gSub.vals = prim->optVals;
        gSub.key = prim->key;
        gModoRow = 0;
        gModoFlag[0] = gModoFlag[1] = gModoFlag[2] = false;
        int cur = 0;
        if (gCfg) {
          JsonDocument tmp;
          configToJson(*gCfg, tmp.to<JsonObject>());
          cur = tmp["mode"].as<int>();
        }
        gModoRow = (cur >= 0 && cur < 3) ? cur : 0;
        gModoFlag[gModoRow] = true;   // marcar el modo que esta puesto
      }
    }
    return;
  }
  const MenuItem *itp = menuCurrent();
  if (!itp) return;
  const MenuItem &it = *itp;
  if (it.kind == MK_HEADER) return;

  if (it.kind == MK_ENUM_CYCLE) {
    // Se abre la lista con TODAS las opciones y el cursor en la que esta puesta.
    gSub.activa = true;
    gSub.titulo = it.label;
    gSub.opts = it.opts;
    gSub.vals = it.optVals;
    gSub.key = it.key;
    gSub.sel = editIndex(it);
    gLastDrawMs = 0;
    gLastActivityMs = millis();
    return;
  }

  if (it.kind == MK_ACTION) {
    if (menuIsDestructive(it.action)) {
      if (gConfirmAction == it.action && millis() < gConfirmUntil) {
        gConfirmAction = 0;
        menuExecute(it.action);
      } else {
        gConfirmAction = it.action;
        gConfirmUntil = millis() + 3000;
        displayPopup("Pulsa largo: confirmar");
      }
    } else {
      menuExecute(it.action);
    }
    return;
  }

  if (it.kind == MK_BOOL) {
    JsonDocument doc;
    doc["config"][it.key] = !menuValueStr(it).equals("Si");
    menuSave(it.key, doc);
    return;
  }
  if (it.kind == MK_ENUM || it.kind == MK_ENUM_F) {
    JsonDocument doc;
    JsonDocument tmp;
    configToJson(*gCfg, tmp.to<JsonObject>());
    if (it.kind == MK_ENUM) {
      int cur = tmp[it.key].as<int>();
      int idx = 0;
      for (int i = 0; it.opts && it.opts[i]; i++) {
        if (it.optVals[i] == cur) {
          idx = i;
          break;
        }
      }
      int n = 0;
      while (it.opts && it.opts[n]) n++;
      idx = (idx + 1) % n;
      doc["config"][it.key] = it.optVals[idx];
    } else {
      float cur = tmp[it.key].as<float>();
      int idx = 0, n = 0;
      for (int i = 0; it.opts && it.opts[i]; i++) {
        if (fabsf(it.optValsF[i] - cur) < 0.01f) idx = i;
        n++;
      }
      idx = (idx + 1) % n;
      doc["config"][it.key] = it.optValsF[idx];
    }
    menuSave(it.key, doc);
    return;
  }
  if (it.kind == MK_INT || it.kind == MK_FLOAT || it.kind == MK_STRING ||
      it.kind == MK_PATH) {
    menuBeginEdit(it);
    gLastDrawMs = 0;
    return;
  }
}

bool menuIsEditing() { return gEditing; }

// Double tap while editing: leave the value as it was.
void menuEditCancel() {
  gEditing = false;
  displayPopup("Cancelado");
  gLastDrawMs = 0;
  gLastActivityMs = millis();
}

void displayRefresh(const DigiConfig &cfg, uint32_t rxCount, uint32_t txCount,
                    uint32_t digiCount, const SensorReadings &r) {
  if (!gOled || !gDisplayOn) return;
  uint32_t now = millis();

  // In tracker/both mode the GPS scene is the interesting one, so start there.
  static bool sceneInit = false;
  if (!sceneInit) {
    sceneInit = true;
    if (cfg.mode != 0) gScene = kSceneCount - 1;
  }

  // Boot splash takes over the panel for a few seconds (non-blocking)
  if (gSplashUntil != 0) {
    if ((int32_t)(now - gSplashUntil) < 0) {
      if (now - gLastDrawMs < 150) return;
      gLastDrawMs = now;
      gOled->clear();
      drawSplash(cfg, now);
      gOled->display();
      return;
    }
    gSplashUntil = 0;
    gLastSceneMs = now;  // restart the auto-advance timer after the splash
  }

  // Bluetooth pairing splash: takes over the panel while a pairing is in
  // progress and disappears the moment it succeeds, fails or is abandoned (that
  // clear comes from ble.cpp). The expiry here is the last-resort safety net.
  if (gPinSplashUntil != 0) {
    if ((int32_t)(now - gPinSplashUntil) < 0) {
      gLastActivityMs = now;  // the screen timeout must not eat the PIN
      if (now - gLastDrawMs < 200) return;
      gLastDrawMs = now;
      gOled->clear();
      drawPinSplash();
      gOled->display();
      return;
    }
    gPinSplashUntil = 0;
    gPinSplash[0] = '\0';
    gLastDrawMs = 0;
    gLastSceneMs = now;
  }

  // ★★ SESION "FIJAR COORDS": QUE LA PANTALLA DIGA EN QUE PASO VA (2026-09-15) ★★
  // Va ANTES del dibujo y despues de la ventana del PIN (si se esta emparejando, el
  // PIN manda). Mientras hay captura renueva el aviso en cada refresco; al terminar
  // deja puesto el resultado. Ver displayCoordsEstadoTick().
  {
    const uint8_t estado = trackerSetCoordsEstado();
    if (estado == TRK_COORDS_BUSCANDO || estado == TRK_COORDS_ASENTANDO) {
      displayCoordsEstadoTick();   // solo avisa cuando CAMBIA de paso
      // Renovacion: el aviso de la captura no puede caducar a los 1,5 s mientras
      // el operador espera a que fije (que es cuando mas falta le hace saber que
      // el aparato sigue trabajando).
      if (gPopup[0] != '\0') gPopupUntil = now + 3000;
      gLastActivityMs = now;   // con la captura en marcha la pantalla no se apaga
    } else {
      displayCoordsEstadoTick();   // recoge el final (guardado / error / cancelado)
    }
  }

  // CIERRE DEL MENU POR INACTIVIDAD (peticion del operador, 2026-09-13): si el
  // usuario no toca el boton en 20 s, el menu se cierra solo y la pantalla vuelve
  // al carrusel de diapositivas de siempre. Se hace AQUI (en el refresco) y no en
  // el manejo del boton, porque el tiempo pasa aunque no se pulse nada.
  // Al cerrar se reinicia el temporizador de escenas para que el carrusel empiece
  // su ciclo limpiamente en vez de saltar de diapositiva al instante.
  if (gMenuOpen && now - gLastActivityMs >= kMenuIdleCloseMs) {
    menuClose();
    gLastSceneMs = now;
  }

  // Auto-advance, unless the user just changed scene with the button (the hold
  // gives them time to read the screen they picked).
  const bool browsing = (int32_t)(now - gAutoAdvanceHoldUntil) < 0;
  if (!gMenuOpen && cfg.sceneAutoAdvance && !browsing &&
      now - gLastSceneMs >= kSceneAutoMs) {
    gLastSceneMs = now;
    gScene = (gScene + 1) % kSceneCount;
    gLastDrawMs = 0;
  }


  if (cfg.screenTimeoutSecs > 0 &&
      now - gLastActivityMs >= (uint32_t)cfg.screenTimeoutSecs * 1000u) {
    displaySleep();
    return;
  }

  if (now - gLastDrawMs < 400) return;
  gLastDrawMs = now;

  gOled->clear();
  if (gMenuOpen && gSub.activa) {
    drawModoScreen();
  } else if (gMenuOpen) {
    drawMenu();
  } else {
    drawScene(cfg, rxCount, txCount, digiCount, r);
    footerPagina();
    drawBatteryFooter();
  }

  // Aviso de cierre (solo tiene sentido con el menu abierto y sin popup encima).
  if (gMenuOpen && !(cfg.popups && gPopupUntil > now && gPopup[0])) {
    drawMenuCountdown();
  }

  if (cfg.popups && gPopupUntil > now && gPopup[0]) {
    drawPopupBox();
  }

  gOled->display();
}

#endif  // HAS_OLED  (las placas sin OLED usan display_epaper.cpp)
