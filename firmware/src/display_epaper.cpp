// display_epaper.cpp — sustituto SIN PANTALLA para el LilyGO T-Echo / Plus.
//
// POR QUE EXISTE ESTO: la capa de pantalla de este firmware (`display.cpp`) esta
// escrita para una OLED SSD1306/SH1106 por I2C. El T-Echo lleva **tinta
// electronica** (Good Display GDEH0154D67, controlador SSD1681, 200x200), que es
// otro chip, otro bus (SPI1) y otras reglas: un refresco completo tarda **2
// segundos**. Eso no es un ajuste, es escribir la interfaz otra vez.
//
// Para que los nodos puedan probarse YA (radio, GPS, sensores, registro,
// configurador, repetidor, rastreador), esta version implementa las MISMAS
// funciones que `display.h` pero **no dibuja nada**. El nodo funciona igual; lo
// unico que no hay es pantalla, y `displayPresent()` dice la verdad (false).
//
// La interfaz de tinta electronica se hara en su propia sesion, con el simulador
// de pantalla de cfr34k como referencia de como se dibuja en estos trastos.
//
// License: GPL-3.0

#include "display.h"

// Las firmas de abajo son EXACTAMENTE las de src/display.h. Si alli cambia una,
// aqui tiene que cambiar igual: el compilador lo canta en cuanto falte.

void displayInit() {}
void displayInitTrasRadio() {}            // no aplica: en la tinta electronica lo hace epaper_techo.cpp
void displayArrancaPantalla() {}
void displayDiagTexto(char *out, size_t n) {
  if (n) snprintf(out, n, "display: en esta placa no hay tinta electronica");
}
bool displayPresent() { return false; }   // la verdad: aqui no hay OLED
bool displayIsOn() { return false; }
void displayWake() {}
void displaySleep() {}

void displaySplash() {}

void displayPinSplash(const char *pin, bool fromStack) {
  (void)pin;
  (void)fromStack;
}
void displayPinSplashClear() {}
bool displayPinSplashActive() { return false; }

void displayBindConfig(DigiConfig *cfg) { (void)cfg; }

bool menuIsOpen() { return false; }
bool menuIsEditing() { return false; }
void menuOpen() {}
void menuClose() {}
void menuShort() {}
void menuLong() {}
void menuEditCancel() {}

void displayNextScene() {}
void displayPopup(const char *text) { (void)text; }

bool displaySaveCoords(double lat, double lon) {
  (void)lat;
  (void)lon;
  return false;
}

// Aviso sonoro de bateria baja: en el sustituto vacio no hay ni pantalla ni zumbador.
// Tiene que existir igual porque lo llama power.cpp en todas las placas.
void displayLowBatTone() {}

// Luz de fondo del panel: en el sustituto vacio no hay panel. Tienen que existir porque
// main.cpp las llama en el bucle (ver el mismo aviso en display.cpp).
void displayBacklightKick() {}
void displayBacklightTick(uint32_t nowMs) { (void)nowMs; }

// Zumbador y navegacion por el boton capacitivo: en el sustituto vacio tampoco hay nada.
void displayBeep() {}
void menuNavigate() {}

void displayPopupWait(const char *text, uint32_t totalMs) {
  (void)text;
  (void)totalMs;
}

void displayNoteRx(const char *from, float rssi, float snr, const char *kind) {
  (void)from;
  (void)rssi;
  (void)snr;
  (void)kind;
}
void displayNoteDigi(const char *from, float rssi, float snr) {
  (void)from;
  (void)rssi;
  (void)snr;
}
void displayNoteTx(const char *what) { (void)what; }

void displayRefresh(const DigiConfig &cfg, uint32_t rxCount, uint32_t txCount,
                    uint32_t digiCount, const SensorReadings &r) {
  (void)cfg;
  (void)rxCount;
  (void)txCount;
  (void)digiCount;
  (void)r;
}
