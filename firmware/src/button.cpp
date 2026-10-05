// button.cpp — Boton FISICO (P1.10) + TACTIL CAPACITIVO (P0.11) de la T-Echo.
//
// ★★★ T-ECHO PROJECT BUTTER — LECTURA DE FONDO (2026-09-15) ★★★
//
// EL DIAGNOSTICO, MEDIDO SOBRE EL CODIGO ANTERIOR:
//
//  1) LOS TOQUES SE PERDIAN PORQUE NADIE MIRABA EL PIN. La version anterior leia el
//     nivel con `digitalRead` una vez por vuelta del bucle, y el bucle se pasa 1,5-3 s
//     pintando la tinta (el driver espera al panel con delay() dentro). Un toque que
//     empieza y acaba en ese rato no deja NINGUNA huella: al volver el bucle el pin ya
//     esta suelto, o sea el mismo nivel que antes de pintar. La funcion drainButton()
//     de main.cpp decia en su comentario que "los flancos que llegaron durante el
//     atasco se procesan en cuanto el bucle vuelve", pero eso es FALSO leyendo NIVELES:
//     solo se recupera si el dedo sigue puesto al volver. Un toque de 80 ms dentro de
//     un refresco de 1500 ms era invisible. De ahi "toques reales que nunca se
//     obedecen".
//
//  2) LA VENTANA DEL DOBLE TOQUE SE MEDIA MAL. El comentario decia "el margen se mide
//     de SUELTA a PULSACION", pero el codigo comparaba la SUELTA del primer toque con
//     la SUELTA del segundo: el operador tenia que empezar Y ACABAR el segundo toque
//     dentro de los 800 ms (unos 710 ms reales para empezar). Eso es justo el "es muy
//     mecanico, cuesta hacerlo" que reporto.
//
//  3) EL TOQUE CORTO SALIA A MITAD DE LA SEGUNDA PULSACION. La resolucion del corto no
//     miraba si el boton estaba pulsado: con un doble toque lento (segundo toque a los
//     750 ms) el corto saltaba a los 800 ms CON EL DEDO YA PUESTO, cambiando de
//     pantalla, y al soltar el segundo toque empezaba otra cuenta: DOS acciones donde
//     el operador habia hecho un gesto. Eso es un toque fantasma de libro.
//
//  4) EL TACTIL NO TENIA NINGUN FILTRO. Solo deteccion de flanco contra un `gCapLast`:
//     cualquier ruido en la pastilla (y el propio RF de la emisora, que es un fenomeno
//     CONOCIDO en esta placa y esta documentado por el autor del firmware de
//     referencia) se convierte en una accion. Con la pastilla flotando y una
//     resistencia de subida debil, un solo roce puede dar varios flancos = varias
//     acciones (varias diapositivas de golpe, pitido y vibracion repetidos).
//
// LO QUE SE HACE AHORA (y por que):
//
//  · Los flancos los captura una INTERRUPCION (GPIOTE) y se guardan con su hora en un
//    buzon. Es exactamente la arquitectura del firmware de referencia de esta placa
//    (cfr34k/t-echo-lora-aprs: `app_button` de Nordic, que arranca un muestreo por
//    temporizador de interrupcion a 25 ms en cuanto el GPIOTE ve el primer flanco).
//    Con eso, un toque durante el repintado NO se pierde: se resuelve con sus tiempos.
//  · El antirrebote es por ESTABILIDAD (el nivel tiene que aguantar kDebounceMs), no un
//    simple "no aceptes cambios antes de 25 ms". Es lo mismo que hacen Nordic
//    (2 muestras separadas 25 ms) y no se come un toque rapido de 60-90 ms.
//  · La ventana del doble toque se mide de SUELTA a PULSACION, que es lo que decia el
//    comentario, y el doble se decide EN LA PULSACION (no hay que esperar a soltar).
//  · El corto NO se resuelve nunca con el boton pulsado.
//
// License: GPL-3.0

#include "button.h"

#include "pins_board.h"   // PIN_BTN_TOUCH solo existe en el T-Echo (pins_techo.h)

// ===========================================================================
//  ★★ LA MAQUINA DE GESTOS DEJA DE DEPENDER DEL BUCLE (2026-09-21, segundo intento) ★★
//
//  EL PROBLEMA, medido en el codigo: los flancos los captura la interrupcion, si, pero
//  **resolverlos** (antirrebote, ventana del doble, confirmacion del toque) se hacia en
//  `procesa()`, y a `procesa()` solo se le llamaba desde el BUCLE y desde el bombeo del
//  driver de la tinta. O sea: mientras el bucle esta metido en algo largo -el envio de los
//  10.000 bytes al panel por bit-bang, un guardado en flash, una transmision-, la maquina
//  de gestos NO AVANZA. Un toque que empieza y acaba dentro de ese rato llega al buzon con
//  sus tiempos, pero nadie lo confirma a tiempo y se pierde.
//
//  COMO LO RESUELVE EL FIRMWARE DE REFERENCIA (cfr34k), y es lo que se copia aqui: alli la
//  deteccion la lleva `app_button` de Nordic con su propio temporizador, y el callback
//  **no dibuja: solo apunta** (`m_epaper_update_requested = true`). El repintado lo dispara
//  el bucle cuando puede. La deteccion y el antirrebote son independientes del bucle.
//
//  LO QUE SE HACE AQUI: `procesa()` pasa a llamarse tambien desde la INTERRUPCION de un
//  temporizador de hardware (TIMER3) cada 10 ms. Es el MISMO patron que ya funciona en este
//  proyecto para el sueno temporizado (`power.cpp`, `RTC2_IRQHandler`): temporizador +
//  NVIC_EnableIRQ + manejador a mano. No depende del bucle, ni de FreeRTOS, ni de que nadie
//  ceda el control.
//
//  ★★ POR QUE 10 MS: los plazos que hay que resolver son de 40 ms (estabilidad), 120 ms
//    (bloqueo) y 600 ms (doble/corto). Preguntar cada 10 ms deja un error de a lo sumo
//    10 ms, que es una cuarta parte del plazo mas corto. El de referencia muestrea cada
//    50 ms y le sobra.
//
//  ★★ PRIORIDAD, QUE ES LO QUE HAY QUE ENTENDER ANTES DE TOCAR ESTO ★★
//    Los botones los atiende GPIOTE, y este nucleo lo configura en **prioridad 3**
//    (WInterrupts.c: `NVIC_SetPriority(GPIOTE_IRQn, 3)`). Este temporizador se pone TAMBIEN
//    en prioridad 3: son hermanos, ninguno tapa al otro, y ninguno toca los niveles 0/1/4
//    que el SoftDevice se reserva (por si algun dia vuelve el Bluetooth).
//
//  ★★★ Y AQUI ESTA EL ERROR DEL PRIMER INTENTO, QUE HAY QUE DEJAR ESCRITO ★★★
//    El primer intento (b71) uso un temporizador de FreeRTOS y protegio el estado compartido
//    con `taskENTER_CRITICAL()`. **SE QUEDARON MUERTOS LOS DOS BOTONES.** El motivo esta en
//    la aritmetica de prioridades de FreeRTOS:
//        configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 2
//      `taskENTER_CRITICAL()` enmascara las interrupciones cuya prioridad es **numerically >=
//      2**, o sea la 2 y la 3, y **GPIOTE es la 3**. Resultado: cada vez que el bucle entraba
//      en una seccion critica, la interrupcion de los botones quedaba ENMASCARADA, y como
//      `bombearBoton()` hace eso en cada vuelta del bucle, los flancos se perdian.
//      LECCION: en este proyecto NO se usa `taskENTER_CRITICAL()` para nada que tenga que
//      seguir viendo interrupciones de GPIOTE. Con el temporizador de hardware y las dos
//      interrupciones a la misma prioridad no hace falta ninguna seccion critica: el
//      temporizador no puede interrumpir al manejador de GPIOTE ni al reves.
//
//  ★ EL TRABAJO SE HACE EN LA INTERRUPCION, NO EN UNA TAREA. Es lo que hace el de
//    referencia (su callback de `app_button` corre en una interrupcion y solo apunta). Lo
//    que se hace aqui dura microsegundos: unas comparaciones de enteros y, como mucho, leer
//    un pin con `digitalRead()`. NADA de ejecutar acciones del firmware (eso sigue siendo del
//    bucle, en su cola) y NADA de escribir por el puerto serie.
// ===========================================================================
#if !defined(BUTTON_SIN_TEMPORIZADOR)
#define BUTTON_TEMPORIZADOR_MS 10
#endif

namespace {

// ===========================================================================
//  LA TABLA DE TIEMPOS, EN UN SOLO SITIO Y SIN CONTRADICCIONES
// ===========================================================================
//
//  gesto           plazo                                              valor
//  ------------------------------------------------------------------------------
//  antirrebote     el nivel tiene que aguantar esto para contar        25 ms
//  LARGO           salta AL LLEGAR, mientras se mantiene              600 ms
//  ventana doble   de SOLTAR el 1er toque a PULSAR el 2º              600 ms
//  corto           se resuelve al vencer esa misma ventana            600 ms
//
//  ★ POR QUE LOS DOS ULTIMOS SON EL MISMO NUMERO (2026-09-15): con un solo plazo no
//    hay forma de que la tabla se contradiga. Antes eran dos: el corto tardaba hasta
//    800 ms en resolverse Y el doble exigia caber en esos mismos 800 ms, medidos de
//    suelta a suelta (ver el diagnostico de arriba). Ahora el operador tiene 600 ms
//    para EMPEZAR el segundo toque (antes ~710 ms para empezarlo Y acabarlo) y el
//    corto sale 200 ms antes que antes (600 en vez de 800). Es el mismo numero que
//    usa un raton (500 ms) con el margen de un boton de goma.
constexpr uint32_t kDebounceMs = 25;      // el nivel debe aguantar esto para contar
constexpr uint32_t kLongMs = 600;         // largo: salta mientras se mantiene
constexpr uint32_t kClickWindowMs = 600;  // suelta del 1º -> pulsacion del 2º

// ---------------------------------------------------------------------------
//  Buzon de flancos. Lo escribe la ISR, lo vacia el bucle.
//  16 huecos dan de sobra: en un rebote electrico caben todos los flancos y, si
//  alguien aporrea el boton durante un refresco de 3 s, tampoco se llena.
// ---------------------------------------------------------------------------
constexpr uint8_t kBuzonMax = 16;

struct Buzon {
  volatile uint32_t ms[kBuzonMax];
  volatile bool nivel[kBuzonMax];   // true = pulsado
  volatile uint8_t cabeza;          // escribe la ISR
  volatile uint8_t cola;            // lee el bucle
  volatile bool ultimo;             // ultimo nivel encolado (para no repetir)
  volatile uint32_t perdidos;       // flancos que no cupieron (diagnostico)
};

Buzon gFisico;   // boton de placa P1.10
Buzon gToque;    // pastilla capacitiva P0.11

inline void buzonMete(Buzon &b, bool nivel, uint32_t ms) {
  if (nivel == b.ultimo) return;                 // sin cambio: nada que guardar
  const uint8_t sig = (uint8_t)((b.cabeza + 1) % kBuzonMax);
  if (sig == b.cola) { b.perdidos++; return; }   // lleno (no deberia pasar)
  b.ms[b.cabeza] = ms;
  b.nivel[b.cabeza] = nivel;
  b.ultimo = nivel;
  b.cabeza = sig;
}

inline bool buzonSaca(Buzon &b, bool &nivel, uint32_t &ms) {
  if (b.cola == b.cabeza) return false;
  ms = b.ms[b.cola];
  nivel = b.nivel[b.cola];
  b.cola = (uint8_t)((b.cola + 1) % kBuzonMax);
  return true;
}

// ===========================================================================
//  NIVELES ACTIVOS. OJO CON ESTO, QUE ESTA MEDIDO Y NO ES LO QUE DICE EL PAPEL:
//  el `pins_techo.h` dice que la pastilla capacitiva es "activo a nivel alto", pero
//  en la placa, con la resistencia interna de subida puesta, se comporta ACTIVA EN
//  BAJO (es lo que decia el comentario de main.cpp, comprobado por el operador). Se
//  deja en BAJO, que es lo que funciona, y se pone aqui arriba y con nombre para que
//  cambiarlo sea una linea si alguna unidad sale al reves.
// ===========================================================================
constexpr int kBtnActivo = LOW;
#if defined(PIN_BTN_TOUCH)
constexpr int kToqueActivo = LOW;
constexpr uint32_t kToqueEstableMs = 40;   // el toque debe aguantar esto
// ★ BLOQUEO BAJADO DE 250 A 120 MS (2026-09-21, peticion del operador: "el sistema es poco
//   responsivo"). PARA QUE SIRVE DE VERDAD: solo para que un CONTACTO LARGO no valga por
//   varios toques. Y eso ya lo garantiza `kToqueEstableMs`: una pastilla que se queda baja
//   40 ms da UN flanco de subida y UN flanco de bajada, o sea UNA accion. El bloqueo de
//   250 ms no protegia de nada que no estuviera ya protegido: lo UNICO que hacia era
//   tirar los toques que llegaban entre 120 y 250 ms despues del anterior, que es
//   justamente el ritmo al que toca una persona que va rapido. Con 120 ms se puede tocar
//   a 5-6 toques por segundo y todos cuentan.
constexpr uint32_t kToqueBloqueoMs = 120;  // ...y luego hay bloqueo: 1 toque = 1 accion
// ★★ LA VENTANA DE RF LLEVA COLA (2026-09-21) ★★
//   `gTxDesde..gTxHasta` las pone radio.cpp al empezar y al ACABAR la llamada a
//   `transmit()`. Pero el amplificador del SX1262 y las tensiones del modulo no se apagan
//   en el mismo instante en que `transmit()` retorna: siguen cayendo un rato. Los flancos
//   fantasma de la pastilla que caen en esa cola se estaban aceptando como DEDOS, y
//   entonces: (a) te cambian de pantalla solo, y (b) arman el bloqueo, o sea que encima te
//   dejan sordo. Esta cola cubre esa caida. Si algun dia resulta que se come algun toque
//   bueno, se baja (o se pone a 0) sin tocar nada mas.
constexpr uint32_t kToqueColaRfMs = 150;
constexpr uint32_t kToqueTardeMs = 250;    // (ya no descarta: ver procesa())
#endif

// ---- maquina de gestos del fisico (todo con marcas de tiempo) ----
bool gNivel = false;        // nivel CONFIRMADO (tras el antirrebote)
bool gPend = false;         // hay un cambio crudo esperando a aguantar el antirrebote
bool gPendNivel = false;
uint32_t gPendMs = 0;

uint32_t gPressMs = 0;      // instante del flanco de pulsacion confirmado
bool gLongFired = false;    // ya se aviso del largo en esta pulsacion
bool gEsperaDoble = false;  // el segundo toque empezo dentro de la ventana

uint8_t gTaps = 0;          // 1 = hay un toque esperando a resolverse
uint32_t gLastTapMs = 0;    // instante en que se SOLTO ese primer toque

// Cola de gestos ya resueltos. Un toque durante un repintado se resuelve dentro de
// la espera del panel y se queda aqui hasta que el bucle pueda ejecutarlo.
constexpr uint8_t kColaMax = 6;
ButtonEvent gCola[kColaMax];
uint8_t gColaN = 0;

ButtonFeedbackFn gFeedback = nullptr;

// ★★ EL AVISO SE PIDE AQUI, PERO SE DA EN EL BUCLE (b73) ★★
//   `gFeedback()` enciende la luz y HACE SONAR EL ZUMBADOR, y `displayBeep()` **bloquea
//   ~60 ms** (ondas cuadradas hechas a mano con un bucle ajustado por `micros()`).
//   Mientras la maquina de gestos vivia solo en el bucle, esos 60 ms no eran un problema.
//   Ahora `procesa()` corre tambien en la INTERRUPCION del temporizador (prioridad 3), y
//   60 ms dentro de una interrupcion es inaceptable: se lleva por delante al resto de
//   interrupciones de esa prioridad y a todo lo de prioridad menor.
//   Solucion, y es la misma idea que el resto del modulo: la interrupcion APUNTA y el bucle
//   EJECUTA. La interrupcion solo sube una bandera (una escritura); quien pita es el bucle,
//   al pasar por `buttonPoll()` / `buttonPump()`.
volatile bool gFeedbackPendiente = false;

inline void pideAviso() { gFeedbackPendiente = true; }

// Lo llama el bucle: si hay un aviso pedido, lo da AQUI (fuera de toda interrupcion).
inline void cobraAviso() {
  if (!gFeedbackPendiente) return;
  gFeedbackPendiente = false;
  if (gFeedback) gFeedback();
}

// ---- tactil ----
#if defined(PIN_BTN_TOUCH)
bool gToquePend = false, gToquePendNivel = false;
uint32_t gToquePendMs = 0;
// ★★ CONTADOR, NO BOOLEANO (2026-09-16) ★★
// ANTES: `bool gToqueHecho`. Con un booleano, si el operador tocaba tres veces mientras la
// pantalla pintaba (1,5 s), el TERCERO sobreescribia al primero y solo se atendia UNO: para
// bajar cuatro filas del menu habia que esperar cuatro repintados. El operador lo describio
// asi: «cuesta bastante esfuerzo y tiempo moverse por los menus, esto rompe la idea del
// Project Butter». El boton FISICO ya tenia su cola (kColaMax); el tactil no tenia nada.
// AHORA: se cuentan los toques aceptados y `buttonTouchPoll()` los va dando de uno en uno,
// asi que N toques = N navegaciones, y el repintado sigue siendo UNO (el aplazamiento de
// kAgrupaToquesMs en epaper_techo.cpp). El tope es una red de seguridad: si alguien apoya
// la mano en la pastilla, no se queda el menu girando 40 filas.
uint8_t gToquesHechos = 0;
constexpr uint8_t kToquesMax = 8;      // toques aceptados que se acumulan como mucho
uint32_t gToqueBloqueoHasta = 0;
uint32_t gTxDesde = 0, gTxHasta = 0;   // ultima ventana de transmision
bool gTxValida = false;
// ★★ CONTADORES DEL TACTIL (2026-09-21) ★★
// PARA QUE: el operador dice "a veces no coge los toques y no se decirte cuando sucede". Con
// el bloqueo y la ventana de RF por medio, un toque puede perderse por tres motivos MUY
// distintos, y hasta ahora NINGUNO dejaba rastro: no habia forma de saber cual era el
// culpable. Ahora se cuentan los cuatro casos y se pueden leer por USB (ver buttonResumen).
uint32_t gToquesOk = 0;         // aceptados (llegaron a la accion)
uint32_t gToquesFtx = 0;        // descartados: cayeron dentro del RF propio o su cola
uint32_t gToquesFbloqueo = 0;   // descartados: bloqueo de kToqueBloqueoMs
uint32_t gToquesTarde = 0;      // confirmados tarde Y con la pastilla ya suelta
// ★ CUANDO SE CONFIRMO EL ULTIMO TOQUE (b82). Es lo que lee la pantalla para saber si acabas
//   de tocar, SIN tocar la cola de acciones (ver buttonUltimoToqueConfirmado()).
volatile uint32_t gToqueConfirmadoMs = 0;
#endif

inline void encola(ButtonEvent ev) {
  if (ev == BTN_NONE) return;
  if (gColaN >= kColaMax) {           // se descarta el mas viejo: el nuevo manda
    for (uint8_t i = 1; i < gColaN; i++) gCola[i - 1] = gCola[i];
    gColaN--;
  }
  gCola[gColaN++] = ev;
}

// ---- ISRs: lo unico que hacen es guardar el nivel con su hora ----
void isrFisico() { buzonMete(gFisico, digitalRead(BUTTON_PIN) == kBtnActivo, millis()); }
#if defined(PIN_BTN_TOUCH)
void isrToque() { buzonMete(gToque, digitalRead(PIN_BTN_TOUCH) == kToqueActivo, millis()); }
#endif

// ===========================================================================
//  FLANCO CONFIRMADO DEL FISICO -> GESTOS
//  Se usan las marcas de tiempo CRUDAS (no el instante en que se confirma): el
//  antirrebote retrasa la DECISION 25 ms, pero no falsea CUANDO paso.
// ===========================================================================
void flancoFisico(bool pulsado, uint32_t ms) {
  if (pulsado == gNivel) return;
  gNivel = pulsado;

  if (pulsado) {                       // ---- PULSACION ----
    gPressMs = ms;
    gLongFired = false;
    // Aviso INMEDIATO (luz + pitido): aqui es donde el operador "nota" el toque,
    // sin esperar a saber si el gesto es corto (600 ms), largo o doble.
    // ★ SE PIDE, NO SE DA (ver pideAviso/cobraAviso): esto puede estar corriendo dentro de
    //   la interrupcion del temporizador, y el pitido bloquea 60 ms.
    pideAviso();
    // ¿Es el segundo toque de un doble? Se mide SUELTA -> PULSACION (ver la tabla).
    if (gTaps == 1 && (uint32_t)(ms - gLastTapMs) <= kClickWindowMs) {
      gEsperaDoble = true;
      gTaps = 0;
      gLongFired = true;               // la suelta de este toque ya no cuenta
      encola(BTN_DOUBLE);              // el doble se decide AQUI, no al soltar
    }
    return;
  }

  // ---- SUELTA ----
  if (gLongFired) { gLongFired = false; gTaps = 0; gEsperaDoble = false; return; }
  if ((uint32_t)(ms - gPressMs) >= kLongMs) {   // mantuvo sin llegar al largo: nada
    gTaps = 0;
    return;
  }
  gTaps = 1;                           // corto pendiente: lo resuelve la ventana
  gLastTapMs = ms;
}

// ===========================================================================
//  TACTIL CAPACITIVO: antirrebote + bloqueo + descarte durante el RF propio
// ===========================================================================
#if defined(PIN_BTN_TOUCH)
void flancoToque(bool pulsado, uint32_t ms) {
  if (!pulsado) return;                // solo interesa el toque, no el destrozo
  // (a) ¿cae dentro de una transmision (mas la cola del amplificador)? La pastilla se
  //     dispara con el RF propio: es un toque FANTASMA, no un dedo. Se descarta y no se
  //     vuelve a armar hasta que la pastilla se suelte (el flanco de bajada llega solo y
  //     limpia el estado).
  //     ★ ESTE DESCARTE NO ARMA EL BLOQUEO, y es a proposito: si lo armara, un fantasma
  //       del RF nos dejaria sordos los kToqueBloqueoMs siguientes. Un toque que cae aqui
  //       se pierde (el nodo esta hablando), pero en cuanto deja de hablar se atiende lo
  //       que llegue.
  if (gTxValida && (int32_t)(ms - gTxDesde) >= 0 &&
      (int32_t)((gTxHasta + kToqueColaRfMs) - ms) >= 0) {
    gToquePend = false;
    gToquesFtx++;
    return;
  }
  // (b) bloqueo tras el toque anterior: un roce largo no vale por tres acciones.
  //     ★ AQUI SI SE CUENTA EL QUE SE PIERDE: si el operador ve que "no le coge los
  //       toques", este contador dice si el culpable es el bloqueo.
  if ((int32_t)(ms - gToqueBloqueoHasta) < 0) {
    gToquePend = false;
    gToquesFbloqueo++;
    return;
  }
  gToqueBloqueoHasta = ms + kToqueBloqueoMs;
  // Se CUENTA, no se marca: los toques aceptados se acumulan (ver gToquesHechos) y el
  // bucle los cobra todos juntos, que es lo que permite bajar cuatro filas del menu con
  // cuatro toques aunque la pantalla solo pueda pintar una vez al final.
  if (gToquesHechos < kToquesMax) gToquesHechos++;
  gToquesOk++;
  gToqueConfirmadoMs = ms;   // sello para que la pantalla sepa cuando dejaste de tocar (b82)
}
#endif

// Avanza los dos buzones y resuelve lo que venza. `ahora` = millis().
void procesa(uint32_t ahora) {
  // ------------------------------------------------ fisico
  {
    bool nivel; uint32_t ms;
    while (buzonSaca(gFisico, nivel, ms)) {
      // El cambio anterior aguanto lo suficiente -> era real.
      if (gPend && (uint32_t)(ms - gPendMs) >= kDebounceMs) {
        flancoFisico(gPendNivel, gPendMs);
        gPend = false;
      }
      gPend = true; gPendNivel = nivel; gPendMs = ms;
    }
    if (gPend && (uint32_t)(ahora - gPendMs) >= kDebounceMs) {
      flancoFisico(gPendNivel, gPendMs);
      gPend = false;
    }

    // LARGO: salta al llegar el umbral, mientras se mantiene.
    if (gNivel && !gLongFired && (uint32_t)(ahora - gPressMs) >= kLongMs) {
      gLongFired = true;
      gTaps = 0;
      gEsperaDoble = false;            // un largo no es la segunda mitad de un doble
      encola(BTN_LONG);
    }

    // CORTO: al vencer la ventana del doble. ★ NUNCA con el boton pulsado ni con un
    // flanco sin confirmar: ahi esta el fallo que partia el doble toque en dos.
    if (gTaps == 1 && !gEsperaDoble && !gNivel && !gPend &&
        (uint32_t)(ahora - gLastTapMs) > kClickWindowMs) {
      gTaps = 0;
      encola(BTN_SHORT);
    }
  }

  // ------------------------------------------------ tactil
#if defined(PIN_BTN_TOUCH)
  {
    bool nivel; uint32_t ms;
    while (buzonSaca(gToque, nivel, ms)) {
      if (gToquePend && (uint32_t)(ms - gToquePendMs) >= kToqueEstableMs) {
        flancoToque(gToquePendNivel, gToquePendMs);
        gToquePend = false;
      }
      gToquePend = true; gToquePendNivel = nivel; gToquePendMs = ms;
    }
    if (gToquePend && (uint32_t)(ahora - gToquePendMs) >= kToqueEstableMs) {
      // ★★ LA CONFIRMACION TARDIA YA NO SE DESCARTA (2026-09-21) ★★
      // ANTES: si el nivel llevaba confirmado mas de kToqueTardeMs (250 ms) se tiraba SIN
      //   DECIR NADA ("no se puede responder de un nivel que no se ha visto sostenerse").
      //   El razonamiento tenia un fallo: no hace falta HABERLO VISTO, se puede COMPROBAR
      //   AHORA. Si en este instante la pastilla sigue activa, es un dedo puesto, aunque el
      //   bucle llevara 300 ms sin mirar (un repintado, un guardado en flash, cualquier
      //   atasco). Tirarlo era perder un toque de verdad por un motivo que no se sostenia.
      // AHORA: se confirma por lo que dice la patilla AHORA MISMO. Si ya se solto, entonces
      //   si es un fantasma (un pico corto que empezo y acabo mientras no mirábamos): se
      //   descarta y se cuenta, para que quede rastro.
      const bool sigueActiva = (digitalRead(PIN_BTN_TOUCH) == kToqueActivo);
      if (sigueActiva || (uint32_t)(ahora - gToquePendMs) <= kToqueTardeMs) {
        flancoToque(gToquePendNivel, gToquePendMs);
      } else {
        gToquesTarde++;
      }
      gToquePend = false;
    }
  }
#endif
}

}  // namespace

// ===========================================================================
//  EL TEMPORIZADOR DE HARDWARE QUE RESUELVE LOS GESTOS SIN DEPENDER DEL BUCLE
//  (el porque y la leccion del primer intento, en la cabecera del fichero)
// ===========================================================================
#if !defined(BUTTON_SIN_TEMPORIZADOR)
namespace {

// Tick de 1 us: PRESCALER 4 -> 16 MHz / 2^4 = 1 MHz.
constexpr uint32_t kTimerPrescaler = 4;
constexpr uint32_t kTimerTicksMs = 1000;                      // 1 ms en ticks de 1 us
constexpr uint32_t kTimerCompare = BUTTON_TEMPORIZADOR_MS * kTimerTicksMs;

}  // namespace

// El manejador va en `extern "C"` y con el nombre exacto de la tabla de vectores, igual que
// el `RTC2_IRQHandler` de power.cpp (patron ya probado en este proyecto).
extern "C" void TIMER3_IRQHandler(void) {
  if (NRF_TIMER3->EVENTS_COMPARE[0]) {
    NRF_TIMER3->EVENTS_COMPARE[0] = 0;   // se limpia SIEMPRE, o es una tormenta de interrupciones
    // Solo se resuelve la maquina de gestos: los gestos ya resueltos se ENCOLAN y la accion
    // la ejecuta el bucle cuando puede. NO se ejecuta nada del firmware desde aqui.
    //
    // ★ `procesa()` NO esta en el namespace anonimo para esto: se declara arriba, en el
    //   mismo fichero, y se llama desde aqui y desde el bucle. Dura microsegundos.
    procesa(millis());
  }
}

static void arrancaTemporizadorBoton() {
  static bool yaArrancado = false;
  if (yaArrancado) return;   // idempotente: el banco de pruebas llama a buttonInit() mas de una vez
  yaArrancado = true;
  NRF_TIMER3->TASKS_STOP = 1;
  NRF_TIMER3->TASKS_CLEAR = 1;
  NRF_TIMER3->MODE = TIMER_MODE_MODE_Timer;
  NRF_TIMER3->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
  NRF_TIMER3->PRESCALER = kTimerPrescaler;
  NRF_TIMER3->CC[0] = kTimerCompare;
  NRF_TIMER3->SHORTS = 0;                 // sin atajos: el contador sigue y la interrupcion avisa
  NRF_TIMER3->INTENSET = TIMER_INTENSET_COMPARE0_Msk;
  NRF_TIMER3->EVENTS_COMPARE[0] = 0;
  // ★ LA PRIORIDAD, QUE ES LO QUE HAY QUE ENTENDER: GPIOTE (los botones) esta en prioridad 3
  //   en este nucleo. Aqui se pone LO MISMO: son hermanos, ninguno tapa al otro, y no se
  //   tocan los niveles 0/1/4 que el SoftDevice se reserva.
  NVIC_SetPriority(TIMER3_IRQn, 3);
  NVIC_ClearPendingIRQ(TIMER3_IRQn);
  NVIC_EnableIRQ(TIMER3_IRQn);
  NRF_TIMER3->TASKS_START = 1;
}
#else
static void arrancaTemporizadorBoton() {}   // apagado a proposito (ver BUTTON_SIN_TEMPORIZADOR)
#endif

void buttonInit() {
  // ★ PUESTA A CERO COMPLETA (2026-09-15). Esta funcion tiene que dejar el modulo en un
  //   estado conocido SIEMPRE que se llame, no solo la primera vez: si se llama dos veces
  //   (o desde el banco de pruebas, tools/banco_boton), no puede quedar dentro ni un
  //   flanco viejo ni un gesto a medias. Se descubrio probando: al no reiniciar el
  //   estado, un toque del caso anterior rechazaba el del siguiente.
  gFisico.cabeza = gFisico.cola = 0;
  gFisico.ultimo = false;
  gFisico.perdidos = 0;
  gNivel = false;
  gPend = false;
  gPendNivel = false;
  gPendMs = 0;
  gPressMs = 0;
  gLongFired = false;
  gEsperaDoble = false;
  gTaps = 0;
  gLastTapMs = 0;
  gColaN = 0;

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  // Estado de partida del buzon: el nivel que hay ahora mismo.
  buzonMete(gFisico, digitalRead(BUTTON_PIN) == kBtnActivo, millis());
  // ★ LA PIEZA CLAVE: el flanco lo coge el GPIOTE del chip, no el bucle. A partir de
  //   aqui, pintar la pantalla (1,5-3 s con delay() dentro) ya no puede tragarse un
  //   toque: cuando el bucle vuelva, el flanco esta en el buzon con su hora.
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), isrFisico, CHANGE);

#if defined(PIN_BTN_TOUCH)
  // El TACTIL se prepara AQUI, con el boton fisico, y no en el primer sondeo: asi la
  // interrupcion esta puesta desde el arranque y no hay ninguna ventana en la que un
  // toque no se vea. (Antes se preparaba "perezosamente" en la primera vuelta; con el
  // boton tan facil de tocar antes de la primera vuelta, mejor no depender de eso.)
  gToque.cabeza = gToque.cola = 0;
  gToque.ultimo = false;
  gToque.perdidos = 0;
  gToquePend = false;
  gToquePendNivel = false;
  gToquePendMs = 0;
  gToquesHechos = 0;
  gToqueBloqueoHasta = 0;
  gTxValida = false;
  gTxDesde = gTxHasta = 0;
  gToquesOk = 0;
  gToquesFtx = 0;
  gToquesFbloqueo = 0;
  gToquesTarde = 0;
  gToqueConfirmadoMs = 0;

  /* ★★ SIN RESISTENCIA INTERNA: COMO EL FIRMWARE DE REFERENCIA (2026-09-21) ★★
     Aqui ponia `pinMode(PIN_BTN_TOUCH, INPUT_PULLUP)`. El firmware del aleman (cfr34k), que
     funciona en esta misma placa, configura la pastilla capacitiva asi:
         {PIN_BTN_TOUCH, APP_BUTTON_ACTIVE_LOW, NRF_GPIO_PIN_NOPULL, cb_app_button}
     o sea SIN pull (NOPULL). Y tiene sentido: en una pastilla capacitiva, meterle una
     resistencia de subida interna puede cargar el pad y MATAR LA SENSIBILIDAD: la pastilla
     mueve muy poca carga y la resistencia se la come.
     Lo que decidimos nosotros ("activa en bajo con pull-up") salio de una MEDIDA de una
     unidad, no del fabricante; el aleman dice NOPULL y su firmware es el que funciona.
     SI ALGUNA UNIDAD SE QUEDA CON EL TOQUE PEGADO con esto, el arreglo es volver a PULLUP
     (es una linea) — pero antes se prueba como lo hace el que funciona. */
  pinMode(PIN_BTN_TOUCH, INPUT_PULLUP);
  buzonMete(gToque, digitalRead(PIN_BTN_TOUCH) == kToqueActivo, millis());
  attachInterrupt(digitalPinToInterrupt(PIN_BTN_TOUCH), isrToque, CHANGE);
#endif

  // ★★ Y LO ULTIMO: EL TEMPORIZADOR QUE HACE QUE LOS GESTOS NO DEPENDAN DEL BUCLE ★★
  // Se arranca AQUI, con las interrupciones ya puestas, para que no haya ni un instante en
  // el que un flanco se capture y nadie lo resuelva. Es idempotente: si se llama dos veces
  // (banco de pruebas), no se crean dos temporizadores.
  arrancaTemporizadorBoton();
}

// ★★ AQUI NO HAY `taskENTER_CRITICAL()`, Y ES A PROPOSITO (b71 -> b72) ★★
//   El primer intento protegio estas funciones con secciones criticas de FreeRTOS y **se
//   quedaron muertos los dos botones**: `taskENTER_CRITICAL()` enmascara las interrupciones
//   de prioridad >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (2), y **GPIOTE, que es
//   quien atiende los dos botones, esta en prioridad 3**. O sea que la proteccion se comia
//   justo las interrupciones que tenia que dejar pasar.
//
//   NO HACE FALTA NINGUNA PROTECCION, y el motivo es que ya no hay dos tareas:
//     - el temporizador de gestos es una INTERRUPCION de prioridad 3 (igual que GPIOTE);
//     - el bucle es una tarea, y una tarea NO puede interrumpir a una interrupcion.
//   Por lo tanto `procesa()` nunca se ejecuta a la vez que si mismo: o lo llama el bucle
//   (con las interrupciones ya atendidas) o lo llama el temporizador (que no puede empezar
//   mientras el bucle este dentro de `procesa()` con las interrupciones sin enmascarar).
void buttonPump() {
  procesa(millis());
  cobraAviso();     // el aviso (luz + pitido) se da SIEMPRE en el bucle, nunca en la ISR
}

ButtonEvent buttonPoll() {
  procesa(millis());
  cobraAviso();
  if (gColaN == 0) return BTN_NONE;
  const ButtonEvent ev = gCola[0];
  for (uint8_t i = 1; i < gColaN; i++) gCola[i - 1] = gCola[i];
  gColaN--;
  return ev;
}

void buttonSetFeedback(ButtonFeedbackFn fn) { gFeedback = fn; }

bool buttonTouchPresent() {
#if defined(PIN_BTN_TOUCH)
  return true;
#else
  return false;
#endif
}

bool buttonTouchPoll() {
#if defined(PIN_BTN_TOUCH)
  procesa(millis());
  // Uno por llamada: quien lo use en un `while` los cobra TODOS (es lo que hace
  // bombearBoton() en main.cpp). Antes era un booleano y los toques se pisaban.
  if (gToquesHechos == 0) return false;
  gToquesHechos--;
  return true;
#else
  return false;
#endif
}

bool buttonTouchPending() {
#if defined(PIN_BTN_TOUCH)
  return gToquesHechos > 0;   // `uint8_t`: la lectura es atomica, no hace falta protegerla
#else
  return false;
#endif
}

uint32_t buttonUltimoToqueConfirmado() {
#if defined(PIN_BTN_TOUCH)
  // ★★ ESTE ES EL DATO QUE NECESITA LA PANTALLA, Y ES UNO DE SOLO LECTURA (b82) ★★
  //   PARA QUE: la pantalla quiere saber cuando has DEJADO de tocar para pintar. Antes lo
  //   deducia con un reloj propio que empezaba al primer toque, y por eso un toque suelto
  //   pagaba los 400 ms enteros de aplazamiento.
  //
  //   ★★ POR QUE ESTE DATO Y NO OTRO (esto es lo que costo tres compilaciones) ★★
  //     Los dos intentos anteriores fallaron porque la pantalla tocaba `gToquesHechos`, y ese
  //     contador NO es un aviso de trabajo: es LA COLA DE ACCIONES PENDIENTES que el bucle
  //     cobra y ejecuta. Vaciarlo o mirarlo en mal momento = el toque se pierde y el menu no
  //     se mueve ("no funciona el boton capacitivo").
  //     Este sello de tiempo NO se lleva nada por delante: se escribe cuando un toque se
  //     confirma, y quien lo lee solo lo lee. La pantalla puede mirarlo mil veces sin que se
  //     pierda ni una accion.
  //
  //   ★ La lectura de 32 bits alineada es atomica en este procesador, asi que no hace falta
  //     ninguna proteccion aunque lo escriba la interrupcion.
  return gToqueConfirmadoMs;
#else
  return 0;
#endif
}

bool buttonDrenaToquesPendientes() {
#if defined(PIN_BTN_TOUCH)
  // ★★ ESTA FUNCION EXISTE POR UN FALLO REAL (b77 -> b78) ★★
  //   En el b77 la pantalla decidia si aplazarse mirando `buttonTouchPending()` SIN vaciar
  //   nada. Y la cola la vacia el `while (buttonTouchPoll())` del bucle, que corre DESPUES
  //   de que la pantalla haya decidido. Resultado: el driver veia "hay trabajo pendiente"
  //   SIEMPRE, se aplazaba siempre, y los toques se quedaban sin cobrar. El operador lo
  //   describio como "ahora no funciona el boton capacitivo".
  //   ANTES de todo esto no pasaba porque el driver llamaba a `bombearBoton()` durante sus
  //   esperas, y eso SI vaciaba la cola. Al quitar el bombeo, se perdio esa parte sin darse
  //   cuenta. Esta funcion devuelve al driver exactamente esa pieza, y solo esa: VACIA la
  //   cola de toques confirmados (sin ejecutar nada; las acciones las sigue ejecutando el
  //   bucle) y dice si habia algo.
  bool habia = false;
  while (gToquesHechos > 0) { gToquesHechos--; habia = true; }
  return habia;
#else
  return false;
#endif
}

uint32_t buttonTouchLockedHasta() {
#if defined(PIN_BTN_TOUCH)
  return gToqueBloqueoHasta;
#else
  return 0;
#endif
}

void buttonNoteRadioTx(uint32_t desdeMs, uint32_t hastaMs) {
#if defined(PIN_BTN_TOUCH)
  gTxDesde = desdeMs;
  gTxHasta = hastaMs;
  gTxValida = true;
#else
  (void)desdeMs;
  (void)hastaMs;
#endif
}

uint32_t buttonLostEdges() {
#if defined(PIN_BTN_TOUCH)
  return gFisico.perdidos + gToque.perdidos;
#else
  return gFisico.perdidos;
#endif
}

void buttonResumen(char *dst, size_t n) {
#if defined(PIN_BTN_TOUCH)
  snprintf(dst, n, "tactil: ok=%lu rf=%lu bloq=%lu tarde=%lu flancosPerdidos=%lu",
           (unsigned long)gToquesOk, (unsigned long)gToquesFtx,
           (unsigned long)gToquesFbloqueo, (unsigned long)gToquesTarde,
           (unsigned long)(gFisico.perdidos + gToque.perdidos));
#else
  snprintf(dst, n, "sin pastilla tactil: flancosPerdidos=%lu",
           (unsigned long)gFisico.perdidos);
#endif
}
