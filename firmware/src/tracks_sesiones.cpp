// tracks_sesiones.cpp — partir el registro de viaje en salidas y anclarlas al track fino.
//
// Ver la explicacion larga en `tracks_sesiones.h`. En resumen:
//   - el REGISTRO dice cuando empezo y acabo cada salida (primera y ultima baliza),
//   - el TRACK de la flash tiene el camino fino (un punto cada 10 m) pero sin horas,
//   - y aqui se juntan: se buscan las dos puntas de cada salida DENTRO del track.
//
// License: GPL-3.0

#include "tracks_sesiones.h"

#include "flog.h"
#include "tracks.h"          // ★ ESTE INCLUDE VA ANTES DEL `#ifdef`: es quien define TRACKS_DISPONIBLE

// ★★ EL `#ifdef` TIENE QUE IR **DESPUES** DE `tracks.h` (2026-10-06) ★★
//   `TRACKS_DISPONIBLE` no esta en el `platformio.ini`: lo define `tracks.h` cuando la placa es un
//   T-Echo. Si el `#ifdef` va antes del include, la macro NO existe todavia, el fichero ENTERO se
//   compila a nada... **y no da ni un error**: el compilador se queda tan contento y el modulo
//   simplemente no esta.
//   ★ Se descubrio mirando el tamaño del objeto: 796 bytes para un fichero de 300 lineas. Un
//     objeto sospechosamente pequeño es un aviso, y aqui lo era.
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef TRACKS_DISPONIBLE

namespace {

// ---------------------------------------------------------------------------
//  Lo que se va sacando del registro
// ---------------------------------------------------------------------------

/** Una salida tal como se ve en el REGISTRO (sin mirar el track todavia). */
struct SalidaRegistro {
  bool     hay;          // se ha visto al menos una baliza con posicion
  uint16_t year;
  uint8_t  month, day, hour, minute;   // de la PRIMERA baliza
  double   latIni, lonIni;             // primera baliza
  double   latFin, lonFin;             // ultima baliza
  uint32_t ultimaSec;                  // hora en segundos, de la ultima baliza (para el hueco)
};

/** El recorrido de las lineas del registro: se para en la PRIMERA linea de cada salida. */
struct Recorrido {
  SalidaRegistro *salidas;
  uint8_t         cuantas;
  uint8_t         maximo;

  // Estado de la salida que se esta montando.
  bool     abierta;
  uint32_t ultimaSec;       // de la ultima baliza vista (para medir el hueco)
  bool     hayUltimaSec;

  // Estadisticas, para poder decir QUE ha pasado sin adivinar.
  uint32_t lineas;
  uint32_t balizas;
  uint32_t saltadas;        // balizas con la linea rara (sin coordenadas legibles)
};

/**
 * Saca la hora de una linea del registro y devuelve true si la tiene.
 *
 * Los tres formatos posibles, tal como los escribe `flogLine`:
 *   `2026-10-06 08:42:11 ...`  hora Y fecha  -> de aqui sale la fecha de la salida
 *   `08:42:11 ...`             solo hora     -> vale para el hueco, no para la fecha
 *   `47s ...`                  segundos      -> NO vale (no hay reloj)
 */
bool horaDeLinea(const char *line, uint16_t *year, uint8_t *month, uint8_t *day,
                 uint8_t *hh, uint8_t *mm, uint8_t *ss) {
  int y = 0, mo = 0, d = 0, h = 0, m = 0, s = 0;
  if (strlen(line) >= 19 && line[4] == '-') {
    if (sscanf(line, "%4d-%2d-%2d %2d:%2d:%2d", &y, &mo, &d, &h, &m, &s) != 6) return false;
    if (year) *year = (uint16_t)y;
    if (month) *month = (uint8_t)mo;
    if (day) *day = (uint8_t)d;
  } else if (strlen(line) >= 8 && line[2] == ':') {
    if (sscanf(line, "%2d:%2d:%2d", &h, &m, &s) != 3) return false;
  } else {
    return false;         // `47s`: el nodo no tenia hora cuando escribio esto
  }
  if (hh) *hh = (uint8_t)h;
  if (mm) *mm = (uint8_t)m;
  if (ss) *ss = (uint8_t)s;
  return true;
}

/** El segundo del dia (0..86399), o 0 si la linea no trae hora util. */
uint32_t segundosDeLinea(const char *line, bool *tieneHora) {
  uint8_t h = 0, m = 0, s = 0;
  if (!horaDeLinea(line, nullptr, nullptr, nullptr, &h, &m, &s)) {
    if (tieneHora) *tieneHora = false;
    return 0;
  }
  if (tieneHora) *tieneHora = true;
  return (uint32_t)h * 3600u + (uint32_t)m * 60u + (uint32_t)s;
}/** Las dos coordenadas de una linea `... TX TRK <lat>,<lon> ...`. */
bool coordsDeLinea(const char *line, double *lat, double *lon) {
  const char *p = strstr(line, "TX TRK ");
  if (p == nullptr) return false;
  p += 7;
  double la = 0.0, lo = 0.0;
  if (sscanf(p, "%lf,%lf", &la, &lo) != 2) return false;
  if (!(la >= -90.0 && la <= 90.0 && lo >= -180.0 && lo <= 180.0)) return false;
  *lat = la;
  *lon = lo;
  return true;
}

/**
 * El recorrido del registro, linea a linea (lo llama `flogEachLine`).
 *
 * ★ LA REGLA DE CORTE ES UNA SOLA: un hueco de `TRACK_SESION_HUECO_S` o mas entre dos balizas.
 *   Antes habia tambien "cortar al ver `EVT boot`", y se ha quitado A PROPOSITO: esa linea se
 *   escribe justo al arrancar, cuando el nodo **todavia no tiene hora del GPS**, asi que lleva
 *   `47s` en vez de una fecha... y una regla que necesite la hora de esa linea **no puede
 *   funcionar nunca**. Se comprobo ejecutando el parser del configurador con un registro de
 *   ejemplo: aquella regla no cortaba ni una vez. Aqui no se repite ese error.
 */
void recorreLinea(const char *line, void *ctx) {
  Recorrido *r = (Recorrido *)ctx;
  r->lineas++;

  double lat = 0.0, lon = 0.0;
  if (!coordsDeLinea(line, &lat, &lon)) return;      // no es una baliza de posicion
  r->balizas++;

  bool tieneHora = false;
  const uint32_t sec = segundosDeLinea(line, &tieneHora);
  if (!tieneHora) { r->saltadas++; return; }         // sin reloj no se puede medir el hueco

  // ★ ¿HAY QUE EMPEZAR UNA SALIDA NUEVA? Cuando la anterior lleva mucho tiempo callada.
  bool nueva = false;
  if (!r->abierta) {
    nueva = true;
  } else if (r->hayUltimaSec) {
    uint32_t hueco = (sec >= r->ultimaSec) ? (sec - r->ultimaSec)
                                           : (86400u - r->ultimaSec + sec);
    if (hueco >= TRACK_SESION_HUECO_S) nueva = true;
  }

  if (nueva) {
    if (r->cuantas >= r->maximo) {
      // La lista esta llena: la mas vieja se cae. Como se recorre de viejo a nuevo, la mas vieja
      // es la primera de la lista: se desplaza todo una posicion hacia atras.
      memmove(&r->salidas[0], &r->salidas[1], sizeof(SalidaRegistro) * (r->maximo - 1));
      r->cuantas = (uint8_t)(r->maximo - 1);
    }
    SalidaRegistro *s = &r->salidas[r->cuantas];
    memset(s, 0, sizeof(*s));
    s->hay = true;
    // ★ Se piden hora, minuto Y SEGUNDO (el segundo se descarta con `nullptr`): la funcion pide
    //   los seis campos, y aqui solo hacen falta los cinco primeros. Un argumento de menos es un
    //   error de compilacion, no un valor por defecto.
    horaDeLinea(line, &s->year, &s->month, &s->day, &s->hour, &s->minute, nullptr);
    s->latIni = lat; s->lonIni = lon;
    s->latFin = lat; s->lonFin = lon;
    s->ultimaSec = sec;
    r->cuantas++;
    r->abierta = true;
  } else {
    // La baliza es de la salida que ya estaba abierta: solo mueve el final.
    SalidaRegistro *s = &r->salidas[r->cuantas - 1];
    s->latFin = lat; s->lonFin = lon;
    s->ultimaSec = sec;
    // ★ Y SI LA PRIMERA BALIZA NO TENIA FECHA (raro, pero puede pasar: la hora llega antes que la
    //   fecha), se aprovecha esta, que es la primera que si la trae. Mejor una fecha de un minuto
    //   despues que ninguna.
    if (s->year == 0) horaDeLinea(line, &s->year, &s->month, &s->day, &s->hour, &s->minute, nullptr);
  }
  r->ultimaSec = sec;
  r->hayUltimaSec = true;
}

// ---------------------------------------------------------------------------
//  Buscar una posicion dentro del track fino
// ---------------------------------------------------------------------------

double metrosEntre(double lat1, double lon1, double lat2, double lon2) {
  const double cosLat = cos((lat1 + lat2) * 0.5 * M_PI / 180.0);
  const double dy = (lat2 - lat1) * 110540.0;
  const double dx = (lon2 - lon1) * 111320.0 * cosLat;
  return sqrt(dx * dx + dy * dy);
}

/**
 * El indice del punto del track mas cercano a una posicion, buscando SOLO entre `desde` y `hasta`.
 *
 * ★ SE BUSCA EN UN TROZO, NO EN TODO EL TRACK, y por un motivo de velocidad: el track vivo tiene
 *   6.800 puntos y esto se llama una vez por punta de cada salida (hasta 20 veces). Buscando en
 *   todo el track serian 136.000 lecturas de flash cada vez que se abre la pantalla.
 */
int32_t puntoMasCerca(double lat, double lon, uint32_t desde, uint32_t hasta, double *distOut) {
  int32_t mejor = -1;
  double mejorD = 1e12;
  TrackPunto p;
  for (uint32_t i = desde; i < hasta; i++) {
    if (!tracksVivoLee(i, &p)) break;
    const double d = metrosEntre(lat, lon, (double)p.lat1e7 / 1e7, (double)p.lon1e7 / 1e7);
    if (d < mejorD) { mejorD = d; mejor = (int32_t)i; }
  }
  if (distOut) *distOut = mejorD;
  return mejor;
}

/** La longitud en metros de un trozo del track (sumando los tramos de punto a punto). */
uint32_t metrosDelTrozo(uint32_t desde, uint32_t hasta) {
  if (hasta <= desde + 1) return 0;
  double total = 0.0;
  TrackPunto a, b;
  if (!tracksVivoLee(desde, &a)) return 0;
  for (uint32_t i = desde + 1; i < hasta; i++) {
    if (!tracksVivoLee(i, &b)) break;
    total += metrosEntre((double)a.lat1e7 / 1e7, (double)a.lon1e7 / 1e7,
                         (double)b.lat1e7 / 1e7, (double)b.lon1e7 / 1e7);
    a = b;
  }
  return (uint32_t)(total + 0.5);
}

// ---------------------------------------------------------------------------
//  Estado del modulo
// ---------------------------------------------------------------------------

TrackSesion gLista[TRACK_SESIONES_MAX];
uint8_t     gCuenta = 0;

/** El trozo del track de cada salida de la lista (ver `tracksSesionDesde`). */
uint32_t    gDesde[TRACK_SESIONES_MAX];
uint32_t    gHasta[TRACK_SESIONES_MAX];

/**
 * Cuantas salidas se sacaron del REGISTRO en la ultima llamada (para poder decirlo en pantalla).
 * Ver `tracksSesionesDelRegistro()`.
 */
uint8_t     gDelRegistro = 0;

}  // namespace

uint8_t tracksSesionesCuenta() { return gCuenta; }

uint8_t tracksSesionesDelRegistro() { return gDelRegistro; }

bool tracksSesionesLee(uint8_t idx, TrackSesion *out) {
  if (!out || idx >= gCuenta) return false;
  *out = gLista[idx];
  return true;
}

uint8_t tracksSesionesRehace() {
  gCuenta = 0;

  const uint32_t puntosTrack = tracksVivoPuntos();
  if (puntosTrack < 2) return 0;      // sin track no hay nada que anclar

  // ---- 1) partir el registro en salidas ----
  // Se reservan unas cuantas mas de las que se ensenan: las viejas se caen solas al llenarse, y
  // asi la lista final siempre lleva las MAS RECIENTES.
  static SalidaRegistro bruto[TRACK_SESIONES_MAX + 4];
  memset(bruto, 0, sizeof(bruto));

  Recorrido r;
  memset(&r, 0, sizeof(r));
  r.salidas = bruto;
  r.maximo = (uint8_t)(sizeof(bruto) / sizeof(bruto[0]));
  flogEachLine(recorreLinea, &r);

  // ★ OJO: AQUI NO SE SALE AUNQUE EL REGISTRO ESTE VACIO (2026-10-06) ★★
  //   Antes habia un `if (r.cuantas == 0) return 0;`, y estaba MAL: si el registro esta vacio pero
  //   el aparato lleva un rato grabando (un nodo recien estrenado, o el registro borrado), el
  //   operador **tiene un track que quiere seguir** y no lo veria. Ahora se sigue hacia abajo y el
  //   trozo de "ahora" se anyade igual, que es lo unico que hay y lo unico que hace falta.
  //
  // ★ Y ESTO SE APUNTA SOLO PARA PODER DECIRLO EN PANTALLA (ver `tracksSesionesDelRegistro`): es lo
  //   que permite distinguir «no hay ni una baliza en el registro» de «hay balizas pero sus salidas
  //   ya no tienen camino». Sin ese numero, las dos cosas se ven igual: una lista vacia.
  gDelRegistro = r.cuantas;

  // ---- 2) anclar cada salida al track, DE LA MAS RECIENTE A LA MAS VIEJA ----
  //   Se va de la mas nueva a la mas vieja para poder acotar la busqueda: el FINAL de una salida
  //   tiene que estar ANTES del PRINCIPIO de la siguiente (los puntos van en orden de grabacion).
  //   Asi la busqueda de cada punta solo mira su trozo, en vez de todo el track.
  uint32_t limiteFin = puntosTrack;      // por encima de esto ya es de una salida mas nueva
  uint32_t limiteIni = puntosTrack;

  for (int k = (int)r.cuantas - 1; k >= 0 && gCuenta < TRACK_SESIONES_MAX; k--) {
    const SalidaRegistro *s = &bruto[k];
    if (!s->hay) continue;

    double dIni = 1e12, dFin = 1e12;
    const int32_t iFin = puntoMasCerca(s->latFin, s->lonFin, 0, limiteFin, &dFin);
    if (iFin < 0) continue;
    const int32_t iIni = puntoMasCerca(s->latIni, s->lonIni, 0, (uint32_t)iFin + 1, &dIni);
    if (iIni < 0) continue;

    // ★ LAS DOS PUNTAS TIENEN QUE SER CREIBLES. Si la baliza "mas cercana" esta a kilometros, es
    //   que esa salida ya no tiene camino en el anillo (se lo comio el reciclado) o que algo no
    //   cuadra: se deja fuera de la lista en vez de guiar a un sitio que no es.
    const uint32_t kMaxDesvioM = 2000;
    if (dFin > (double)kMaxDesvioM || dIni > (double)kMaxDesvioM) continue;
    if (iIni >= iFin) continue;        // un trozo de menos de dos puntos no es un camino

    TrackSesion *dst = &gLista[gCuenta];
    memset(dst, 0, sizeof(*dst));
    dst->valida = true;
    dst->year = s->year; dst->month = s->month; dst->day = s->day;
    dst->hour = s->hour; dst->minute = s->minute;
    dst->puntos = (uint32_t)(iFin - iIni + 1);
    dst->metros = metrosDelTrozo((uint32_t)iIni, (uint32_t)iFin + 1);
    // ★ Y SE GUARDA EL TROZO PARA PODER GUIAR POR EL. El motor de guiado necesita saber desde
    //   donde hasta donde leer; se le pasa al empezar (ver `tracksGuiaEmpiezaTrozo`).
    gDesde[gCuenta] = (uint32_t)iIni;
    gHasta[gCuenta] = (uint32_t)iFin + 1;
    gCuenta++;

    limiteFin = (uint32_t)iIni;        // la siguiente (mas vieja) acaba antes de donde empieza esta
  }

  // =========================================================================
  //  ★★ Y EL TRACK DE AHORA MISMO, SIEMPRE EL PRIMERO (2026-10-06) ★★
  //
  //  POR QUE HACE FALTA, y es la razon de que el operador no viera su track actual:
  //    El registro solo apunta una posicion CUANDO EL NODO MANDA UNA BALIZA, y la baliza de
  //    fabrica sale cada 30 minutos parado. Asi que desde la ultima baliza hasta AHORA hay un
  //    tramo de track que **todavia no esta en el registro**... y sin esto no salia en la lista.
  //    Consecuencia: llegabas al sitio, mirabas "Volver a casa" y **tu paseo de hoy no estaba**.
  //
  //  COMO SE RESUELVE: lo que hay DESPUES del final de la ultima salida anclada es,
  //  necesariamente, el trozo que estas grabando ahora. Se anyade como una salida mas, la primera
  //  de la lista, con la fecha y hora de AHORA (que es cuando empezo a grabar este trozo).
  //
  //  ★ Y SI NO HAY NADA DESPUES (acabas de mandar baliza), no se anyade nada: no se inventa una
  //    salida de dos puntos para llenar la pantalla.
  // =========================================================================
  const uint32_t kMinimoPuntosAhora = 2;
  // ★★★ AQUI ESTABA EL FALLO DE «SIN TRACKS GRABADOS» CON EL TRACK LLENO (2026-10-06) ★★★
  //   `limiteFin` empieza valiendo `puntosTrack` (el final del track) y **solo baja cuando una
  //   salida se ancla bien**. Si el registro tiene salidas pero NINGUNA encaja con el track (por
  //   ejemplo, porque son viejas y su camino ya se lo comio el reciclado), el bucle no ancla nada,
  //   `limiteFin` se queda en `puntosTrack`... y entonces:
  //       desdeAhora = puntosTrack
  //       if (puntosTrack >= desdeAhora + 2)   ->   puntosTrack >= puntosTrack + 2   -> FALSO
  //   **La sesion de "ahora" se descartaba y la lista salia VACIA teniendo el track lleno.**
  //   Es exactamente lo que reporto el operador: «sale sin tracks grabados, pero el aparato tiene
  //   un track de hoy y otros de otros dias».
  //   ★★ LA REGLA, que es lo que hay que recordar: **"ahora" empieza despues de la ultima salida
  //     que SE HA PODIDO ANCLAR, no despues de la ultima que hay en el registro.** Si no se anclo
  //     ninguna, "ahora" es TODO el track, porque no hay nada mejor que ensenar y el operador
  //     tiene un camino que quiere seguir.
  const uint32_t desdeAhora = (gCuenta > 0) ? limiteFin : 0;

  if (puntosTrack >= desdeAhora + kMinimoPuntosAhora && gCuenta < TRACK_SESIONES_MAX) {
    // Se desplaza la lista una posicion para meterlo ARRIBA (es el mas reciente).
    if (gCuenta > 0) {
      memmove(&gLista[1], &gLista[0], sizeof(TrackSesion) * gCuenta);
      memmove(&gDesde[1], &gDesde[0], sizeof(uint32_t) * gCuenta);
      memmove(&gHasta[1], &gHasta[0], sizeof(uint32_t) * gCuenta);
    }
    TrackSesion *dst = &gLista[0];
    memset(dst, 0, sizeof(*dst));
    dst->valida = true;
    // La fecha y hora: la del track vivo si la tiene; si no, la de la primera salida anclada (es
    // lo mas parecido a la verdad que hay, y la pantalla sabe ensenar "sin fecha" con year == 0).
    uint16_t yy = 0; uint8_t mo = 0, dd = 0, hh = 0, mm = 0;
    if (tracksVivoCuando(&yy, &mo, &dd, &hh, &mm)) {
      dst->year = yy; dst->month = mo; dst->day = dd; dst->hour = hh; dst->minute = mm;
    }
    dst->puntos = puntosTrack - desdeAhora;
    dst->metros = metrosDelTrozo(desdeAhora, puntosTrack);
    gDesde[0] = desdeAhora;
    gHasta[0] = puntosTrack;
    gCuenta++;
  }

  return gCuenta;
}

uint32_t tracksSesionDesde(uint8_t idx) { return (idx < gCuenta) ? gDesde[idx] : 0; }
uint32_t tracksSesionHasta(uint8_t idx) { return (idx < gCuenta) ? gHasta[idx] : 0; }

#endif  // TRACKS_DISPONIBLE
