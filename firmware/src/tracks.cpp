// ===========================================================================
//  tracks.cpp — capa de datos de los tracks (2026-09-22). Ver `tracks.h` para el mapa de
//  memoria, el formato de pagina y POR QUE cada zona esta donde esta.
//
//  ★★ LO QUE HAY QUE SABER ANTES DE TOCAR ESTO ★★
//
//  1) EL NVMC DE ESTE CHIP ESCRIBE PALABRAS DE 32 BITS, y solo puede pasar un bit de 1 a 0.
//     Para volver un bit a 1 hay que BORRAR LA PAGINA ENTERA. Por eso:
//       - un punto se programa como 3 palabras (los 10 bytes utiles y 2 de relleno);
//       - la cabecera de una pagina se escribe AL PRINCIPIO con los puntos a 0, y se
//         ACTUALIZA AL FINAL con el numero de puntos (0xFFFF -> el valor, que es bajar bits);
//       - reutilizar una pagina exige borrarla, y borrar son ~85 ms: nunca se hace en el
//         camino de un punto, solo al cambiar de pagina o al reciclar.
//
//  2) LA ESCRITURA A FLASH PARA EL USB. Es la leccion que ya se pago una vez en este proyecto
//     (esta escrita en `flog.cpp`): una escritura a flash que espera al NVMC **congela el
//     puerto USB CDC** si pasa en el arranque. Aqui NADA se escribe al arrancar: `tracksInit()`
//     solo LEE. Y el track en vivo se escribe desde el bucle, con la radio y el USB ya vivos.
//
//  3) EL ANILLO DEL TRACK EN VIVO Y SU CONTADOR DE VUELTAS. Cuando el track en vivo llega al
//     final de su zona, NO se puede borrar todo (son 80 KB de puntos: 20 paginas) ni se puede
//     seguir escribiendo:
//     se da la vuelta y se empieza a sobreescribir por el principio, que es lo unico que cabe
//     en un anillo. Pero eso PIERDE EL INICIO del track, y el inicio es justo adonde hay que
//     volver en "volver a casa". Por eso se marca con `gVivoVueltas`, y quien navegue puede
//     AVISAR en vez de llevar al usuario a un punto que ya no es el principio.
//     ★ 6800 puntos (no 8160: al pasar el punto a 12 bytes bajo la capacidad, y este comentario
//     se quedo con el numero viejo -- ver la nota de `tracks.h`). A 1 punto cada 10 m son 68 km de
//     ruta, y a 1 punto cada 60 s parado son 113 horas (4,7 dias). Una ruta de un dia NO da la
//     vuelta; un nodo olvidado encendido en casa SI, y ahi es donde importa el aviso.
// ===========================================================================

#include "tracks.h"

// ★ TODO este modulo es solo para el T-Echo (ver la explicacion en `tracks.h`: el mapa de
//   memoria solo se ha comprobado ahi, y el operador decidio que las Faketec se queden como
//   estan). En las Faketec este fichero se compila a VACIO: ni codigo, ni funciones, ni
//   escrituras a una zona que no es suya.
// ★★ LA MISMA CONDICION QUE `tracks.h`, Y TIENE QUE SER LA MISMA (2026-09-22) ★★
//   El guardian de este fichero miraba solo la placa, y el de la cabecera mira ademas
//   `TRACKS_FUERA`. Con las dos condiciones distintas, el entorno de diagnosis
//   (`techo_plus_diag_sintracks`, que quita el modulo a proposito) intentaba compilar este
//   fichero SIN las constantes ni los tipos de la cabecera: una cascada de errores que no tenia
//   nada que ver con el fallo que se estaba buscando.
//   Si algun dia se anade otra condicion, tiene que entrar en LOS DOS sitios.
#if defined(FAKETEC_BOARD_TECHO) && !defined(TRACKS_FUERA)

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <nrf.h>

#include "gps.h"        // gpsGet() / gpsPowered() / gpsDistanceM() para la cadencia de grabacion
#include "flog.h"       // flogLine(): para APUNTAR cuando la grabacion se pausa por guiado

namespace {

// ------------------------------------------------------------------ acceso a la flash
// El NVMC. `READY` vale 1 cuando esta LISTO (el nombre del campo en el header de Nordic
// enganna: `NVMC_READY_READY_Busy = 0`). Esta espera es la que no puede pasar en el arranque.
//
// ★★★ ESTE BUCLE NO TENIA LIMITE, Y ESO CUELGA EL APARATO (2026-10-07) ★★★
//   Estaba puesto `while (!NRF_NVMC->READY) {}` a secas. Si el NVMC no levanta `READY` —por lo que
//   sea: una operacion que no termina, un acceso prohibido, el planificador de la SoftDevice— ese
//   bucle **no sale NUNCA**: el aparato se queda mudo, sin reiniciarse y sin decir nada.
//   ★ SE DESCUBRIO CON EL NODO ENCHUFADO AL ORDENADOR, hablandole por el puerto serie:
//       - `status`, `track_list`, `beacon`, `track_end` e incluso un comando inventado: contestan
//         **al instante**.
//       - `track_begin`: **ni una respuesta en 40 segundos**, y el nodo sigue vivo despues (contesta
//         a lo siguiente). O sea: **se queda dentro y no vuelve**.
//     Y `track_begin` es el unico comando que BORRA PAGINAS (`tracksRanuraEmpieza` borra las 5 de
//     la ranura). Todo apunta a que se queda esperando aqui.
//   ★ AHORA LA ESPERA TIENE TOPE. Si vence, se sale, se cuenta el fallo, y **el comando puede
//     contestar un error** en vez de dejar al operador mirando una pantalla que no responde.
//     Un fallo que se cuenta es un fallo que se puede arreglar; uno que cuelga, no.
volatile uint32_t gNvmcEsperas = 0;      // cuantas veces se ha esperado al NVMC
volatile uint32_t gNvmcVencidas = 0;     // cuantas veces se ha agotado el tope (0 = todo bien)

/**
 * Que fallo la ultima vez al preparar una ranura: 0 = nada, 1 = el borrado, 2 = la escritura.
 * Se apunta para poder decirlo por el puerto, en vez de contestar solo "no se pudo".
 */
uint8_t gUltimoFalloRanura = 0;
// ★★ OJO CON EL ESPACIO DE NOMBRES ANONIMO (2026-10-07) ★★
//   Estas tres funciones de consulta (mas abajo, al final del fichero) tienen que estar FUERA del
//   `namespace {` que empieza aqui: dentro, tienen enlace INTERNO y el enlazador no las encuentra
//   desde `protocol.cpp`. El sintoma es un error de enlace («undefined reference»), que es facil de
//   confundir con no haberlas escrito. Las variables si pueden quedarse dentro: las lee el mismo
//   fichero.

/**
 * Espera a que el NVMC este listo, CON TOPE.
 *
 * @return true si quedo listo, false si se agoto el tope.
 * ★ EL TOPE ES GENEROSO A PROPOSITO: un borrado de pagina tarda ~28 ms en este chip, asi que 200 ms
 *   es margen de sobra y sigue siendo un parpadeo para quien lo sufre. Lo que NO vale es esperar
 *   para siempre.
 */
inline bool nvmcEspera() {
  gNvmcEsperas++;
  const uint32_t t0 = millis();
  while (!NRF_NVMC->READY) {
    if ((uint32_t)(millis() - t0) > 200u) {
      gNvmcVencidas++;
      // Se deja el NVMC en modo lectura antes de salir: si se quedara en modo escritura, la
      // siguiente lectura de flash podria dar un fallo de bus, y eso si que tumba el aparato.
      NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
      return false;
    }
  }
  return true;
}

/**
 * Borra una pagina. @return false si el NVMC no respondio (ver `nvmcEspera`).
 * ★ DEVUELVE SI HA IDO BIEN: antes no devolvia nada, asi que quien la llamaba **no podia saber** si
 *   el borrado habia ocurrido. Y un borrado que no ocurre deja la escritura siguiente mezclada
 *   (la NVMC solo pasa bits de 1 a 0), o sea una cabecera corrupta y un fallo que aparece lejos.
 */
bool flashBorraPagina(uint32_t dir) {
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een << NVMC_CONFIG_WEN_Pos;
  if (!nvmcEspera()) return false;
  NRF_NVMC->ERASEPAGE = dir;
  const bool ok = nvmcEspera();
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
  nvmcEspera();
  return ok;
}

/** Escribe una palabra. @return false si el NVMC no respondio (ver `nvmcEspera`). */
bool flashEscribePalabra(uint32_t dir, uint32_t valor) {
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen << NVMC_CONFIG_WEN_Pos;
  if (!nvmcEspera()) return false;
  *(volatile uint32_t *)dir = valor;
  const bool ok = nvmcEspera();
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
  nvmcEspera();
  return ok;
}

// ------------------------------------------------------------------ lectura
// La flash mapeada se lee como memoria normal, pero por puntero volatile para que el compilador
// no se guarde copias ni reordene.
inline uint32_t flashLee32(uint32_t dir) { return *(volatile uint32_t *)dir; }
inline uint16_t flashLee16(uint32_t dir) { return *(volatile uint16_t *)dir; }

// ------------------------------------------------------------------ cabecera de pagina
// Ocupa 8 bytes, pero NO se puede escribir como una palabra de 32 bits entera desde el
// principio: el NVMC de este chip SOLO escribe palabras de 32 bits (no hay escritura de 16),
// asi que el reparto tiene que ser en palabras:
//
//     palabra 0 (+0)  magico
//     palabra 1 (+4)  [secuencia:16][puntos:16]
//
// Se escribe la palabra 1 DOS veces: al empezar la pagina con los puntos a 0xFFFF ("aun no se
// cuantos hay") y al cerrarla con el numero de verdad. La segunda vez solo BAJA bits, asi que
// no hace falta borrar. (La version anterior de este fichero escribia la secuencia en +4 y
// luego intentaba leerla de +4 otra vez: se pisaba a si misma. Por eso esta escrito aqui.)
inline uint32_t pagPalabra(uint16_t seq, uint16_t puntos) {
  return ((uint32_t)seq << 16) | puntos;
}

// Los lectores van ANTES que los escritores: `pagMarcaPuntos` necesita `pagSeq`.
inline uint16_t pagSeq(uint32_t base)    { return (uint16_t)(flashLee32(base + 4) >> 16); }
inline uint16_t pagPuntos(uint32_t base) { return (uint16_t)(flashLee32(base + 4) & 0xFFFFu); }
inline bool pagValida(uint32_t base)     { return flashLee32(base) == TRACK_MAGIC; }

void pagInicia(uint32_t base, uint16_t seq) {
  flashBorraPagina(base);
  flashEscribePalabra(base + 0, TRACK_MAGIC);
  flashEscribePalabra(base + 4, pagPalabra(seq, 0xFFFFu));   // puntos = "aun no se"
}

void pagMarcaPuntos(uint32_t base, uint16_t puntos) {
  const uint16_t seq = pagSeq(base);
  flashEscribePalabra(base + 4, pagPalabra(seq, puntos));    // solo baja bits
}

// Direccion de un punto dentro de una pagina.
inline uint32_t dirPunto(uint32_t basePag, uint16_t idx) {
  // ★ `TRACK_PUNTO_STRIDE` (12) Y NO `sizeof(TrackPunto)` (10): con paso 10, la mitad de los
  //   puntos caian en direcciones NO alineadas a 4 bytes, y la NVMC de este chip NO puede
  //   escribir ahi -- no da error, da un FALLO DE BUS y el aparato se queda colgado. Fue
  //   exactamente el fallo del b126 en el T-Echo Plus. Ver la explicacion larga en tracks.h.
  return basePag + TRACK_PAG_HDR + (uint32_t)idx * TRACK_PUNTO_STRIDE;
}

void escribePunto(uint32_t dir, const TrackPunto *p) {
  // 10 bytes utiles = 2 palabras y media, asi que se escriben 3 palabras: la tercera lleva los
  // 2 bytes de alt y 2 de relleno. El relleno NO importa: al leer solo se usan los 10 primeros.
  uint8_t buf[12];
  memcpy(buf, p, sizeof(TrackPunto));
  buf[10] = 0xFF;
  buf[11] = 0xFF;
  flashEscribePalabra(dir + 0, (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                               ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24));
  flashEscribePalabra(dir + 4, (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
                               ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24));
  flashEscribePalabra(dir + 8, (uint32_t)buf[8] | ((uint32_t)buf[9] << 8) |
                               ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24));
}

void leePunto(uint32_t dir, TrackPunto *p) {
  uint8_t buf[12];
  uint32_t w0 = flashLee32(dir + 0);
  uint32_t w1 = flashLee32(dir + 4);
  uint32_t w2 = flashLee32(dir + 8);
  buf[0] = w0; buf[1] = w0 >> 8; buf[2] = w0 >> 16; buf[3] = w0 >> 24;
  buf[4] = w1; buf[5] = w1 >> 8; buf[6] = w1 >> 16; buf[7] = w1 >> 24;
  buf[8] = w2; buf[9] = w2 >> 8; buf[10] = w2 >> 16; buf[11] = w2 >> 24;
  memcpy(p, buf, sizeof(TrackPunto));
}

// ===========================================================================
//  TRACK EN VIVO
// ===========================================================================
bool     gVivoOn = false;        // hay grabacion en marcha
uint32_t gVivoEscribe = 0;       // ★ indice REAL de escritura, SIEMPRE en [0, TRACK_VIVO_PUNTOS)
uint16_t gVivoSeq = 0;           // secuencia de la ultima pagina empezada
uint32_t gVivoVueltas = 0;       // ★ cuantas PAGINAS se han reciclado (0 = no se ha perdido nada)
uint32_t gVivoTotal = 0;         // puntos que se le pueden pedir a `tracksVivoLee` (0..TOTAL)
// ★★ `gVivoPrimero` SIGNIFICA DOS COSAS SEGUN EL MOMENTO, y hay que tenerlo claro:
//    - MIENTRAS NO SE HA DADO LA VUELTA: es SIEMPRE 0. Los puntos del anillo estan en orden y
//      `tracksVivoLee(0)` devuelve el primero que se grabo.
//    - DESPUES DE DAR LA VUELTA: es el indice del punto MAS ANTIGUO que queda vivo, porque el
//      principio del track ya se ha sobreescrito. Ahi `tracksVivoLee(0)` devuelve ese.
//    La formula `real = gVivoPrimero + idx` no vale para las dos: mientras no ha dado la vuelta,
//    el indice real del usuario es `idx` a secas (ver `tracksVivoLee`).
uint32_t gVivoPrimero = 0;

inline uint32_t vivoPagBase(uint16_t pag) { return TRACK_VIVO_BASE_PTS + (uint32_t)pag * TRACK_PAGINA; }

// ---------------------------------------------------------------------------------------
//  LA FECHA DE INICIO DEL TRACK EN VIVO
// ---------------------------------------------------------------------------------------
// Va en la cabecera de 64 bytes al principio de la zona (ver tracks.h). Formato:
//     +0  magico
//     +4  anio
//     +6  mes, +7 dia, +8 hora, +9 minuto
// Se escribe UNA vez, al empezar el track, y se lee al arrancar. No se toca despues.
struct VivoCab {
  uint32_t magic;
  uint16_t year;
  uint8_t  month, day, hour, minute;
  uint8_t  relleno[3];
};
static_assert(sizeof(VivoCab) <= TRACK_VIVO_CAB, "la cabecera del vivo no cabe en su hueco");

void vivoCabEscribe(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute) {
  // Se borra la pagina de la cabecera (la 0, que es donde vive) y se escribe. Es UNA vez por
  // track, no en cada punto: el coste (~28 ms) no se nota.
  flashBorraPagina(TRACK_VIVO_BASE);
  flashEscribePalabra(TRACK_VIVO_BASE + 0, TRACK_VIVO_MAGIC);
  flashEscribePalabra(TRACK_VIVO_BASE + 4, (uint32_t)year | ((uint32_t)month << 16) |
                                            ((uint32_t)day << 24));
  flashEscribePalabra(TRACK_VIVO_BASE + 8, (uint32_t)hour | ((uint32_t)minute << 8) |
                                            0xFFFF0000u);
}

bool vivoCabLee(uint16_t *year, uint8_t *month, uint8_t *day, uint8_t *hour, uint8_t *minute) {
  if (flashLee32(TRACK_VIVO_BASE) != TRACK_VIVO_MAGIC) return false;
  const uint32_t w1 = flashLee32(TRACK_VIVO_BASE + 4);
  const uint32_t w2 = flashLee32(TRACK_VIVO_BASE + 8);
  if (year)   *year   = (uint16_t)(w1 & 0xFFFF);
  if (month)  *month  = (uint8_t)((w1 >> 16) & 0xFF);
  if (day)    *day    = (uint8_t)((w1 >> 24) & 0xFF);
  if (hour)   *hour   = (uint8_t)(w2 & 0xFF);
  if (minute) *minute = (uint8_t)((w2 >> 8) & 0xFF);
  return true;
}

// Al arrancar: buscar la pagina escrita MAS RECIENTE y seguir desde ahi, sin borrar nada.
// El criterio es la secuencia: la mas alta es la ultima que se empezo a escribir. Si hay
// varias con la misma (no deberia), gana la que tenga mas puntos.
//
// ★★ ESTO SE LLAMA `vivoRescata` Y HACE LO QUE DICE: rescatar el track que ya habia (2026-10-06) ★★
//   Hoy (2026-10-06, DESPUES de darle una vuelta) se ha vuelto a dejar COMO ESTABA, y conviene
//   apuntar por que, porque me lie yo solo:
//
//   Se intento cambiar para que cada encendido empezara un track NUEVO, apoyandose en que es lo
//   que hacen los GPS de monte (Garmin: «new track segments on Power-off / Power-on cycle»).
//   ★ PERO ERA INNECESARIO, y el operador lo dijo tres veces: **las sesiones YA se graban.** El
//     aparato parte el registro por sesiones desde septiembre, y quien lo hace es el REGISTRO DE
//     VIAJE, no este anillo:
//       - en `index.html` (configurador): `let sesiones=[]; // tracks en que se parte el registro
//         (una por encendido)`, y las corta por `EVT boot` (`BOOT_RE`) y, como respaldo, por un
//         hueco de mas de 30 minutos sin lineas (`HUECO_SESION_S`).
//       - y las ensena en un desplegable, con su GPX y su KML.
//     O sea que el historial de sesiones **ya existia**; lo que faltaba era que EL APARATO lo
//     supiera leer y ensenar, no cambiar como se graba.
//   ★ LA LECCION: **antes de tocar como se GRABA, mira quien lo LEE.** Yo mire el anillo y no mire
//     el registro de viaje, que es donde estaba la respuesta desde el primer dia.
void vivoRescata() {
  int mejor = -1;
  for (uint16_t i = 0; i < TRACK_VIVO_PAGINAS; i++) {
    const uint32_t b = vivoPagBase(i);
    if (!pagValida(b)) continue;
    if (mejor < 0) { mejor = i; continue; }
    const uint16_t sNew = pagSeq(b), sOld = pagSeq(vivoPagBase((uint16_t)mejor));
    if (sNew > sOld || (sNew == sOld && pagPuntos(b) > pagPuntos(vivoPagBase((uint16_t)mejor)))) {
      mejor = i;
    }
  }
  if (mejor < 0) {
    // No hay nada guardado: el primer punto empezara la pagina 0 con secuencia 1.
    gVivoSeq = 1;
    return;
  }

  gVivoSeq = pagSeq(vivoPagBase((uint16_t)mejor));
  uint16_t dentro = pagPuntos(vivoPagBase((uint16_t)mejor));
  if (dentro > TRACK_PUNTOS_PAGINA) dentro = 0;   // cabecera rara: esa pagina se rehace
  gVivoEscribe = (uint32_t)mejor * TRACK_PUNTOS_PAGINA + dentro;
  if (gVivoEscribe >= TRACK_VIVO_PUNTOS) gVivoEscribe = 0;
  gVivoOn = (dentro > 0);

  // ★ CUANTOS PUNTOS SE PUEDEN LEER TRAS UN REINICIO: se cuentan los que estan DENTRO de las
  //   paginas anteriores a la que se esta escribiendo. Es una cuenta conservadora y honesta:
  //   puede que falten los de paginas ya recicladas, pero nunca se le ensena al usuario un
  //   punto que no existe.
  uint32_t total = dentro;
  for (uint16_t i = 0; i < TRACK_VIVO_PAGINAS; i++) {
    if (i == (uint16_t)mejor) continue;
    if (!pagValida(vivoPagBase(i))) continue;
    const uint16_t p = pagPuntos(vivoPagBase(i));
    if (p <= TRACK_PUNTOS_PAGINA) total += p;
  }
  if (total > TRACK_VIVO_PUNTOS) total = TRACK_VIVO_PUNTOS;
  gVivoTotal = total;
  // ★ Y OJO: TRAS UN REINICIO NO SE PUEDE SABER SI EL ANILLO HABIA DADO LA VUELTA, porque eso
  //   no esta guardado en ninguna parte. Se arranca dando por hecho que NO (gVivoPrimero = 0),
  //   que es lo que vale mientras el track quepa. Si el inicio se habia perdido antes del
  //   reinicio, no se puede saber hasta que se vuelva a perder. Queda escrito aqui para que
  //   nadie lo tome por una garantia que no es.
  gVivoPrimero = 0;
  gVivoVueltas = 0;
}

// ===========================================================================
//  RANURAS CARGADAS
// ===========================================================================
// Cada ranura es una zona de 20 KB con 5 paginas. La PRIMERA pagina lleva delante los datos de
// la ranura (fecha, numero de puntos, extremos), y los puntos empiezan detras. Las paginas
// siguientes son iguales que las del track en vivo.
//
//   ranura + 0   cabecera de ranura (TrackRanuraInfo en crudo + magico)
//   ranura + ... puntos, en paginas, como el track en vivo
struct SlotCab {
  uint32_t magic;
  uint16_t puntos;
  uint16_t year;
  uint8_t  month, day, hour, minute;
  uint8_t  relleno[2];
  int32_t  lat1e7Ini, lon1e7Ini;
  int32_t  lat1e7Fin, lon1e7Fin;
};
constexpr uint32_t SLOT_MAGIC = 0x31534C54u;   // 'TLS1'

inline uint32_t slotBase(uint8_t slot) { return TRACK_SLOT_BASE + (uint32_t)slot * TRACK_SLOT_TAM; }

// Los puntos de una ranura: la pagina 0 empieza DESPUES de la cabecera de ranura, asi que caben
// menos. ★ El numero vive ahora en `tracks.h` (`TRACK_SLOT_PUNTOS`), para que el que VALIDA y el
//   que se ANUNCIA al configurador sean EL MISMO. Antes eran dos: el protocolo anunciaba 2040 y
//   aqui se aplicaba 2037, asi que un GPX simplificado a 2040 era rechazado justo despues de que
//   el nodo dijera que cabia. Aqui solo se comprueba que las cuentas no se separen.
static_assert(sizeof(SlotCab) == TRACK_SLOT_CAB,
              "TRACK_SLOT_CAB (tracks.h) no coincide con sizeof(SlotCab): el tope de puntos por "
              "ranura que se anuncia al configurador seria mentira");
constexpr uint16_t SLOT_PAG0_HDR = (uint16_t)((sizeof(SlotCab) + 7u) & ~7u);   // alineado a 8

// ★★★ LA ASERCION QUE IMPIDE QUE ESTO VUELVA A PASAR (2026-09-22) ★★★
//   La NVMC solo escribe palabras de 32 bits y la direccion tiene que estar alineada a 4. Si el
//   paso del punto, la cabecera de pagina o la de ranura no fueran multiplos de 4, la mitad de
//   los puntos caerian en direcciones prohibidas y el aparato se colgaria con un fallo de bus
//   (fue el fallo del b126: paso 10, y 204 de cada 408 puntos desalineados).
//   ★ Esto NO es una comprobacion de estilo: es la diferencia entre arrancar y no arrancar. Si
//     alguien cambia el formato del punto y no respeta el multiplo de 4, NO COMPILA.
static_assert(TRACK_PUNTO_STRIDE % 4u == 0u, "el paso del punto tiene que ser multiplo de 4");
static_assert(TRACK_PAG_HDR % 4u == 0u, "la cabecera de pagina tiene que ser multiplo de 4");
static_assert(SLOT_PAG0_HDR % 4u == 0u, "la cabecera de la pagina 0 de la ranura, tambien");
static_assert(TRACK_SLOT_CAB % 4u == 0u, "la cabecera de ranura, tambien");
constexpr uint16_t SLOT_PAG0_PUNTOS = (uint16_t)TRACK_SLOT_PAG0_PUNTOS;
constexpr uint16_t SLOT_PUNTOS_REAL = (uint16_t)TRACK_SLOT_PUNTOS;   // un solo numero

inline uint32_t slotDirPunto(uint8_t slot, uint32_t idx) {
  // ★ Paso alineado a 4 (`TRACK_PUNTO_STRIDE`), no `sizeof(TrackPunto)`: ver la explicacion en
  //   `dirPunto`. Aqui el despiste costaba lo mismo: fallo de bus al escribir la mitad de los
  //   puntos de una ranura.
  if (idx < SLOT_PAG0_PUNTOS) {
    return slotBase(slot) + SLOT_PAG0_HDR + TRACK_PAG_HDR + idx * TRACK_PUNTO_STRIDE;
  }
  const uint32_t resto = idx - SLOT_PAG0_PUNTOS;
  const uint32_t pag = 1 + resto / TRACK_PUNTOS_PAGINA;
  const uint32_t dentro = resto % TRACK_PUNTOS_PAGINA;
  return slotBase(slot) + pag * TRACK_PAGINA + TRACK_PAG_HDR + dentro * TRACK_PUNTO_STRIDE;
}

}  // namespace

// ===========================================================================
//                                  API
// ===========================================================================

void tracksInit() {
  // ★ AQUI NO SE ESCRIBE NADA. Una escritura a flash que espera al NVMC en el arranque congela
  //   el USB CDC (leccion ya pagada, ver flog.cpp). Solo se lee.
  gVivoOn = false;
  gVivoEscribe = 0; gVivoSeq = 0;
  gVivoVueltas = 0; gVivoTotal = 0; gVivoPrimero = 0;
  vivoRescata();
}

// ---------------------------------------------------------------- track en vivo
bool     tracksVivoActivo()      { return gVivoOn; }
uint32_t tracksVivoPuntos()      { return gVivoTotal; }
bool     tracksVivoDioLaVuelta() { return gVivoVueltas > 0; }

// Cuando empezo el track en vivo. Lo lee el menu para poder ensenar fecha y hora, que es como
// el operador quiere elegir los tracks.
bool tracksVivoCuando(uint16_t *year, uint8_t *month, uint8_t *day,
                      uint8_t *hour, uint8_t *minute) {
  return vivoCabLee(year, month, day, hour, minute);
}

// ★ ¿Se esta grabando AHORA? Se CALCULA con las MISMAS condiciones que usa `tracksTick` para
//   decidir si escribe, en el mismo orden. No se guarda en una variable aparte: si se guardara,
//   habria dos sitios diciendo cosas distintas del mismo hecho y el dia que uno cambiara el otro
//   mentiria.
//   ★★ OJO CON LA PRIMERA VERSION (corregida el 2026-09-22) ★★
//     Estaba puesta como `!tracksGuiaActivo()`, o sea "no hay guiado" = "esta grabando". NO ES
//     LO MISMO: con el GPS apagado o sin fijacion, `tracksTick` sale ANTES de escribir nada, asi
//     que no se graba aunque no haya guiado. Lo cazo una revision independiente. Hoy no se
//     notaba porque el aviso solo salia en la pantalla de guiado, pero al llevarlo a la cabecera
//     (que sale en TODAS las pantallas) si se notaria: el nodo diria que graba con el GPS
//     apagado. Se replica la comprobacion de verdad.
bool tracksVivoGrabando() {
  if (tracksGuiaActivo()) return false;   // el guiado pausa la grabacion (punto 7)
  if (!gpsPowered()) return false;        // sin GPS no hay nada que grabar
  if (!gpsGet().fix) return false;        // sin fijacion no se guarda (ver tracksTick)
  return true;
}

void tracksVivoOlvida() {  // Empezar de cero SIN borrar la flash: se reescribe la cabecera de la pagina 0 y se sigue
  // desde ahi. No se borran las demas paginas porque van a ser sobreescritas y, mientras
  // tanto, no molestan (su cabecera seguira siendo valida pero con secuencia VIEJA, y el
  // rescate del arranque coge siempre la de secuencia mas alta).
  gVivoOn = true;
  gVivoEscribe = 0;
  gVivoSeq = (uint16_t)(gVivoSeq + 1);
  if (gVivoSeq == 0) gVivoSeq = 1;
  // ★ LA FECHA DE INICIO (2026-09-22): se apunta la del GPS si la tiene. Si el GPS aun no tiene
  //   hora (los primeros segundos tras encender, o si nunca la coge), se deja la que hubiera: es
  //   mejor una fecha vieja que una inventada, y el menu dice "sin hora" si no hay ninguna.
  {
    const GpsData &g = gpsGet();
    if (g.dateValid) {
      vivoCabEscribe(g.utcYear, g.utcMonth, g.utcDay, g.utcH, g.utcM);
    }
  }
  pagInicia(vivoPagBase(0), gVivoSeq);
  gVivoTotal = 0;
  gVivoPrimero = 0;
  gVivoVueltas = 0;
}

void tracksVivoAnade(double lat, double lon, float altM) {
  if (!gVivoOn) {
    // Primera posicion valida de la sesion: empieza el track.
    tracksVivoOlvida();
  }

  TrackPunto p;
  p.lat1e7 = (int32_t)llround(lat * 1e7);
  p.lon1e7 = (int32_t)llround(lon * 1e7);
  // La altitud va en metros y en 16 bits con signo (-32768..32767): el Everest y el Mar Muerto
  // entran de sobra, y si el GPS da un absurdo se recorta en vez de dar la vuelta al numero.
  long a = lroundf(altM);
  if (a >  32767) a =  32767;
  if (a < -32768) a = -32768;
  p.altM = (int16_t)a;

  // Escribir en la pagina actual. `gVivoEscribe` es el indice REAL dentro del anillo, de 0 a
  // TRACK_VIVO_PUNTOS-1, y SIEMPRE se mantiene dentro de ese rango (ver la nota de abajo).
  const uint16_t pag   = (uint16_t)(gVivoEscribe / TRACK_PUNTOS_PAGINA);
  const uint16_t dentro = (uint16_t)(gVivoEscribe % TRACK_PUNTOS_PAGINA);

  // ¿Hay que empezar pagina nueva? Cuando la actual ya tiene sus 340 puntos.
  if (dentro == 0) {
    // La pagina anterior se cierra con su cuenta (solo baja bits: no hace falta borrarla).
    if (gVivoEscribe > 0) {
      pagMarcaPuntos(vivoPagBase((uint16_t)((gVivoEscribe - 1) / TRACK_PUNTOS_PAGINA)),
                     TRACK_PUNTOS_PAGINA);
    }
    // ★ ¿Se esta RECICLANDO una pagina del principio del anillo, o es la primera vez que se
    //   llega a ella? Se sabe porque la pagina YA es valida: si lo es y no es la 0, es que se
    //   escribio antes, o sea que el anillo ha dado la vuelta y el inicio se va a perder.
    //
    //   ★ Y AQUI ESTA EL TRUCO DE LA SECUENCIA: al reciclar, la secuencia NO se puede seguir
    //     subiendo (la pagina 0 tendria el numero mas alto, y el rescate del arranque se
    //     pensaria que el orden empieza ahi, que es justo al reves). Lo que significa la
    //     secuencia es "quien es el MAS NUEVO", asi que al reciclar se vuelve a empezar: la
    //     secuencia nueva es la de la pagina anterior mas uno. Asi, dentro de una vuelta, las
    //     secuencias siempre suben en el orden de escritura.
    const bool recicla = pagValida(vivoPagBase(pag));
    if (recicla) {
      if (gVivoVueltas == 0) gVivoVueltas = 1;   // ★ primera vuelta: el inicio se pierde
      gVivoPrimero = (gVivoPrimero + TRACK_PUNTOS_PAGINA) % TRACK_VIVO_PUNTOS;
      // La secuencia se reinicia tomando la de la pagina ANTERIOR (la que se acaba de cerrar).
      const uint16_t pagAnt = (uint16_t)((pag + TRACK_VIVO_PAGINAS - 1) % TRACK_VIVO_PAGINAS);
      gVivoSeq = pagValida(vivoPagBase(pagAnt)) ? (uint16_t)(pagSeq(vivoPagBase(pagAnt)) + 1) : 1;
    } else {
      gVivoSeq = (uint16_t)(gVivoSeq + 1);
    }
    if (gVivoSeq == 0) gVivoSeq = 1;
    pagInicia(vivoPagBase(pag), gVivoSeq);
  }

  escribePunto(dirPunto(vivoPagBase(pag), dentro), &p);
  pagMarcaPuntos(vivoPagBase(pag), (uint16_t)(dentro + 1));

  // ★★ EL INDICE DE ESCRITURA DA LA VUELTA CON EL ANILLO, Y ESTO ES UN ARREGLO (2026-09-22) ★★
  //   La primera version hacia `gVivoEscribe++` sin tope. Con el tiempo se salia del rango, y
  //   como la lectura calcula la direccion con `real = primero + idx`, al cabo de unas cuantas
  //   vueltas **la lectura se iba a direcciones FUERA de la zona de tracks**: un track leido
  //   basura, o peor, leyendo la flash del registro de viaje. Se descubrio revisando la
  //   aritmetica, no en el monte. Ahora el indice vive SIEMPRE en [0, TRACK_VIVO_PUNTOS).
  gVivoEscribe = (gVivoEscribe + 1) % TRACK_VIVO_PUNTOS;

  if (gVivoTotal < TRACK_VIVO_PUNTOS) gVivoTotal++;
}

bool tracksVivoLee(uint32_t idx, TrackPunto *out) {
  if (!out || idx >= gVivoTotal) return false;
  // ★★ EL INDICE REAL NO ES SIEMPRE `primero + idx` ★★
  //   Mientras el anillo NO ha dado la vuelta, los puntos estan en orden desde el principio de
  //   la zona, asi que el indice real es `idx` a secas. Si se sumara `primero` (que en ese
  //   momento es 0) daria igual, pero en cuanto se recicla la primera pagina `primero` deja de
  //   ser 0 y entonces SI hay que sumarlo. Distinguirlo por `gVivoVueltas` evita depender de
  //   que `primero` valga 0 por casualidad.
  const uint32_t real = (gVivoVueltas > 0) ? ((gVivoPrimero + idx) % TRACK_VIVO_PUNTOS) : idx;
  const uint32_t pag = real / TRACK_PUNTOS_PAGINA;
  const uint32_t dentro = real % TRACK_PUNTOS_PAGINA;
  leePunto(dirPunto(vivoPagBase((uint16_t)pag), (uint16_t)dentro), out);
  return true;
}

// ---------------------------------------------------------------------------------------
//  LA GRABACION: cuando se guarda un punto (2026-09-22)
//
//  ★ LA POLITICA VIVE AQUI Y SOLO AQUI. El bucle llama a `tracksTick()` y se desentiende: no
//    le pasa la posicion ni decide nada. Asi la cadencia no se puede quedar repartida por el
//    bucle en dos sitios que se contradigan.
//
//  SE GUARDA UN PUNTO CUANDO:
//    - te has movido >= 10 m desde el ultimo guardado, O
//    - han pasado 60 s sin guardar (estas parado: queda constancia de que sigues ahi, y asi el
//      track no tiene un salto raro cuando reanudas la marcha).
//
//  ★ POR QUE 10 METROS: guardando por tiempo, un tramo lento se llena de puntos y uno rapido
//    queda vacio, o sea que el track representaria al reloj y no a la ruta. Con 10 m, los 8.160
//    puntos del anillo dan para 68 km: una ruta de montana de un dia entero son 15-30 km.
//
//  ★ Y NO SE GUARDA NADA SIN FIJACION: si el GPS no tiene fix, la latitud y la longitud son
//    basura (o el ultimo valor conocido), y meterlas en el track lo envenenaria para siempre
//    (el "volver a casa" te llevaria a un punto que nunca existio). Se exige `fix` a secas.
// ---------------------------------------------------------------------------------------
void tracksTick(uint32_t nowMs) {
  // El GPS se lee por su cuenta: la politica de grabacion vive aqui, no en el bucle.
  if (!gpsPowered()) return;
  const GpsData &g = gpsGet();
  if (!g.fix) return;

  // ★★ GUIANDO NO SE GRABA (punto 7 de la especificacion, 2026-09-22) ★★
  //   CON CUALQUIER GUIADO, no solo con los cargados. Motivo, y es el mismo en los dos casos:
  //   el guiado esta siguiendo un track FIJO (el de WikiLoc, o el TUYO hasta este momento en
  //   "volver a casa"), y si se siguiera grabando encima, el track que se esta siguiendo
  //   cambiaria mientras se sigue. En "volver a casa" es peor todavia: el track es TU CAMINO DE
  //   IDA, y grabando mientras vuelves le estarias anadiendo la vuelta, asi que "el principio"
  //   dejaria de ser el sitio al que quieres volver.
  //
  //   ★ Y NO SE PARA EN SILENCIO: se apunta en el registro de viaje UNA vez, al empezar y al
  //     acabar. Si el operador ve que su track no crece, tiene que poder averiguar por que sin
  //     adivinar. (El registro es el diario del nodo: para eso esta.)
  static bool grabando = true;
  const bool guiando = tracksGuiaActivo();
  if (guiando == grabando) {          // cambio de estado: se anota
    grabando = !guiando;
    flogLine("EVT track vivo %s por guiado", guiando ? "PAUSADO" : "reanudado");
  }
  if (guiando) return;                // guiando: no se graba

  // ★★ INTERRUPTOR DE DIAGNOSIS: `TRACKS_SIN_GRABAR` (2026-09-22) ★★
  //   PARA QUE: el operador reporta que el b126 NO ARRANCA en su T-Echo Plus, y el mismo
  //   firmware con el modulo de tracks FUERA si arranca. O sea que el fallo esta en este modulo.
  //   Lo unico de este modulo que ESCRIBE en la flash es la grabacion (lo demas solo lee), y el
  //   primer punto se escribe en cuanto hay fijacion GPS, o sea casi al arrancar. Asi que aqui
  //   se corta LO DE ESCRIBIR y se deja TODO LO DEMAS (el modulo, el menu, la pantalla).
  //   Si con esto arranca, el fallo esta en la escritura a flash (o en la aritmetica que la
  //   rodea). Si tampoco arranca, el fallo NO es de la grabacion.
  //   NO SE DEFINE EN LAS TANDAS NORMALES: hay que pedirlo por su nombre.
#ifdef TRACKS_SIN_GRABAR
  return;
#endif

  constexpr uint32_t kMinMetros = 10;      // ★ la cadencia: ver la explicacion de arriba
  constexpr uint32_t kMaxSinGuardarMs = 60000;

  static bool     hayUltimo = false;
  static double   ultLat = 0.0, ultLon = 0.0;
  static uint32_t ultMs = 0;

  // Primera posicion de la sesion: empieza el track. Si ya venia uno de antes (el nodo se
  // reinicio), NO se borra: se sigue grabando en el mismo. El track es del viaje, no del
  // arranque. `tracksVivoAnade` se encarga de crear la pagina si hacia falta.
  if (!hayUltimo) {
    tracksVivoAnade(g.lat, g.lon, g.altM);
    hayUltimo = true;
    ultLat = g.lat; ultLon = g.lon; ultMs = nowMs;
    return;
  }

  const bool porDistancia =
      gpsDistanceM(ultLat, ultLon, g.lat, g.lon) >= (float)kMinMetros;
  const bool porTiempo = (uint32_t)(nowMs - ultMs) >= kMaxSinGuardarMs;
  if (!porDistancia && !porTiempo) return;

  tracksVivoAnade(g.lat, g.lon, g.altM);
  ultLat = g.lat; ultLon = g.lon; ultMs = nowMs;
}

// ---------------------------------------------------------------- ranuras
void tracksRanuraInfo(uint8_t slot, TrackRanuraInfo *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (slot >= TRACK_SLOTS) return;
  const uint32_t b = slotBase(slot);
  if (flashLee32(b) != SLOT_MAGIC) return;      // vacia
  const SlotCab *c = (const SlotCab *)b;
  if (c->puntos == 0 || c->puntos > SLOT_PUNTOS_REAL) return;   // a medias o corrupta
  out->valida    = true;
  out->puntos    = c->puntos;
  out->year      = c->year;
  out->month     = c->month;
  out->day       = c->day;
  out->hour      = c->hour;
  out->minute    = c->minute;
  out->lat1e7Ini = c->lat1e7Ini;
  out->lon1e7Ini = c->lon1e7Ini;
  out->lat1e7Fin = c->lat1e7Fin;
  out->lon1e7Fin = c->lon1e7Fin;
}

bool tracksRanuraLee(uint8_t slot, uint32_t idx, TrackPunto *out) {
  if (!out || slot >= TRACK_SLOTS) return false;
  TrackRanuraInfo info;
  tracksRanuraInfo(slot, &info);
  if (!info.valida || idx >= info.puntos) return false;
  leePunto(slotDirPunto(slot, idx), out);
  return true;
}

bool tracksRanuraEmpieza(uint8_t slot, uint16_t puntos, const TrackRanuraInfo *meta) {
  if (slot >= TRACK_SLOTS || !meta) return false;
  if (puntos == 0 || puntos > SLOT_PUNTOS_REAL) return false;   // no cabe: se rechaza, NO se recorta
  // ★★ SE BORRAN LAS CINCO PAGINAS, NO SOLO LA CABECERA (2026-09-22) ★★
  //   Antes se borraba solo la pagina 0. Parecia suficiente (quitar el magico invalida la
  //   ranura) pero estaba mal por DOS motivos, y los dos dan un track corrupto o un fallo que
  //   parece misterioso:
  //     1) LA FLASH NO SE PUEDE ESCRIBIR DOS VECES. NVMC solo pasa bits de 1 a 0, nunca al
  //        reves. Si en una ranura ya hubo un track, sus puntos viejos NO estan borrados, y
  //        escribir encima los mezcla: salen puntos que no son ni los viejos ni los nuevos.
  //        Con una sola pagina borrada, la segunda subida a la misma ranura quedaba corrupta.
  //     2) UNA SUBIDA CORTA DEJA LAS PAGINAS SIGUIENTES COMO ESTABAN. Si un track ocupa solo la
  //        pagina 0, al subir despues uno largo las paginas 1..4 todavia tienen la basura
  //        anterior... y no se pueden escribir.
  //   Cuesta ~140 ms de flash (5 x 28 ms) UNA vez por subida, no por punto: no se nota, y a
  //   cambio la ranura empieza siempre LIMPIA, que es lo unico que hace fiable lo de despues.
  // ★★ Y SI EL BORRADO NO VA BIEN, SE DICE Y SE SALE (2026-10-07) ★★
  //   Antes se borraba y se seguia, pasara lo que pasara. Si el NVMC se queda colgado, la version
  //   anterior **no volvia nunca** de `track_begin` (sin respuesta, con el aparato vivo pero mudo).
  //   Ahora, si una pagina no se deja borrar, se para AQUI y el comando puede contestar un error.
  for (uint32_t k = 0; k < TRACK_SLOT_PAGINAS; k++) {
    if (!flashBorraPagina(slotBase(slot) + k * TRACK_PAGINA)) {
      gUltimoFalloRanura = 1;      // 1 = fallo el BORRADO
      return false;
    }
  }
  SlotCab cab;
  memset(&cab, 0xFF, sizeof(cab));
  cab.magic     = SLOT_MAGIC;
  cab.puntos    = puntos;
  cab.year      = meta->year;
  cab.month     = meta->month;
  cab.day       = meta->day;
  cab.hour      = meta->hour;
  cab.minute    = meta->minute;
  cab.lat1e7Ini = meta->lat1e7Ini;
  cab.lon1e7Ini = meta->lon1e7Ini;
  cab.lat1e7Fin = meta->lat1e7Fin;
  cab.lon1e7Fin = meta->lon1e7Fin;   // ★ era meta->lat1e7Fin (copiar-pegar): la longitud final
                                     //   se guardaba como si fuera latitud
  const uint32_t *w = (const uint32_t *)&cab;
  for (size_t i = 0; i < sizeof(cab) / 4; i++) {
    // ★ SI LA ESCRITURA NO VA BIEN, SE SABE Y SE DICE (2026-10-07): antes se escribia y se seguia,
    //   asi que una cabecera a medias se devolvia como "preparada" y el fallo aparecia luego, en el
    //   primer trozo, donde ya no se sabe por que.
    if (!flashEscribePalabra(slotBase(slot) + i * 4, w[i])) {
      gUltimoFalloRanura = 2;      // 2 = fallo la ESCRITURA de la cabecera
      return false;
    }
  }
  // ★★ LA CABECERA DE LA PAGINA 0 SE ESCRIBE A MANO, SIN BORRAR (2026-09-22) ★★
  //   Aqui estaba `pagInicia(slotBase(slot) + SLOT_PAG0_HDR, 1)`, y esa funcion EMPIEZA
  //   borrando la pagina: borraba la misma pagina de 4 KB que acabamos de borrar y donde
  //   acabamos de escribir el `SlotCab`. Resultado: el magico 'TLS1' quedaba en 0xFF, la ranura
  //   se leia como VACIA, y TODOS los `track_chunk` se rechazaban. O sea: **cargar un track
  //   desde el configurador no podia funcionar nunca**, y la lista del nodo ensenaria las cinco
  //   filas "Vacia" sin que nada dijera por que.
  //   Lo cazo una revision independiente. La pagina YA esta borrada (linea de arriba), asi que
  //   escribir la cabecera es solo escribir: borrar otra vez no aporta nada y destruye lo hecho.
  const bool ok1 = flashEscribePalabra(slotBase(slot) + SLOT_PAG0_HDR + 0, TRACK_MAGIC);
  const bool ok2 = flashEscribePalabra(slotBase(slot) + SLOT_PAG0_HDR + 4, pagPalabra(1, 0xFFFFu));
  if (!ok1 || !ok2) {
    gUltimoFalloRanura = 3;      // 3 = fallo la cabecera de PAGINA (el magico de la pagina)
    return false;
  }
  gUltimoFalloRanura = 0;
  return true;
}

bool tracksRanuraEscribe(uint8_t slot, uint32_t idx, const TrackPunto *p) {
  if (slot >= TRACK_SLOTS || !p) return false;
  TrackRanuraInfo info;
  tracksRanuraInfo(slot, &info);
  if (!info.valida || idx >= info.puntos) return false;
  escribePunto(slotDirPunto(slot, idx), p);
  return true;
}

bool tracksRanuraTermina(uint8_t slot) {
  if (slot >= TRACK_SLOTS) return false;
  // La cabecera de ranura YA dice cuantos puntos son (se escribio al empezar), asi que
  // "terminar" es solo confirmar que la ranura es buena. Se deja como funcion para poder
  // anadir comprobaciones (por ejemplo, que todos los puntos se escribieron) sin cambiar la
  // interfaz del protocolo.
  TrackRanuraInfo info;
  tracksRanuraInfo(slot, &info);
  return info.valida;
}

void tracksRanuraBorra(uint8_t slot) {
  if (slot >= TRACK_SLOTS) return;
  flashBorraPagina(slotBase(slot));   // basta con quitar el magico
}

// ===========================================================================
//  MOTOR DE NAVEGACION (2026-09-22). Ver la explicacion larga en `tracks.h`.
//
//  COMO SE HACE LA CUENTA, que es el corazon de todo esto:
//
//  1) SE PASA A METROS. Grados no sirven para medir distancias: un grado de longitud mide 111 km
//     en el Ecuador y 78 km en Espana. Asi que la posicion de referencia se lleva a un plano en
//     metros (x = esta, y = norte) y ahi SI valen las cuentas de toda la vida.
//       x = (lon - lon0) * 111320 * cos(lat0)
//       y = (lat - lat0) * 110540
//     (110540 es la constante de latitud; 111320 la de longitud en el ecuador, corregida por el
//     coseno de la latitud. Para distancias de una ruta es de sobra.)
//
//  2) PARA CADA TRAMO de la ruta se busca el punto mas cercano a ti, por proyeccion:
//       t = ((P-A)·(B-A)) / |B-A|²      (cuanto del tramo A->B hay que recorrer)
//     - Si 0 <= t <= 1, el punto cae DENTRO del tramo y la desviacion es la distancia
//       perpendicular (la de verdad, la que se ve en el mapa).
//     - Si t < 0 o t > 1, el pie cae fuera y lo que vale es la distancia a la punta.
//     Se guarda el tramo que da la distancia MINIMA: ese es el sitio de la ruta donde estas.
//
//  3) LO QUE FALTA no es la distancia al final en linea recta: es la LONGITUD DE RUTA que queda.
//     Se suma lo que resta del tramo actual mas todos los tramos siguientes. Eso es lo que dice
//     un GPS, y es lo que de verdad te queda por andar.
//
//  ★ TIENE QUE SER BARATO: no se copia el track a RAM (el vivo son 80 KB y no caben), se lee de
//    la flash. Son ~16.000 lecturas por calculo y el calculo va cada 5 s.
// ===========================================================================
namespace {

TrackFuente gFuente = TRK_FUENTE_NADA;
int        gSlot = -1;
bool       gAlReves = false;
uint32_t   gPuntos = 0;

/**
 * ★ DESDE QUE PUNTO DEL TRACK VIVO SE GUIA (2026-10-06). Casi siempre 0.
 *
 * Lo pone `tracksGuiaEmpiezaTrozo`, que es lo que usa "volver a casa" cuando el operador elige UNA
 * salida de la lista. El track vivo es una sola tirada con las salidas de varios dias pegadas, y
 * sin esto te guiaria por la ruta entera (los paseos de anteayer incluidos).
 */
uint32_t   gDesde = 0;
TrackGuia  gGuia{};
uint32_t   gUltCalculoMs = 0;

// ===========================================================================
//  ★★ EL ANCLA: POR DONDE IBA, PARA NO SALTAR DE TRAMO (2026-10-06) ★★
//
//  EL FALLO QUE ARREGLA, contado por el operador y luego confirmado en el codigo:
//    El motor buscaba el tramo GEOMETRICAMENTE MAS CERCANO entre TODOS los de la ruta, sin
//    acordarse de por donde ibas. En una ruta que **se cruza consigo misma** —una horquilla de
//    montana, una ida y vuelta por el mismo camino, un mirador al que te acercas dos veces— tu
//    posicion puede estar casi a la misma distancia del tramo 10 y del tramo 90. El motor elegia
//    uno de los dos **al azar de la geometria**, y el resultado era una barbaridad: darte por
//    avanzado medio track, o mandarte hacia atras.
//
//  ★ Y OJO, QUE ESTO NO ES LO MISMO QUE "HABERSE SALTADO UN PUNTO": el motor NO exige pasar por
//    los puntos (proyecta sobre la LINEA), y eso esta bien y no se toca. Lo que faltaba era
//    **memoria de por donde ibas**.
//
//  COMO FUNCIONA: se guarda POR DONDE IBAS (en metros de recorrido desde el principio de la ruta,
//  ver `gRecorridoAncla`), y la busqueda se limita a los tramos que caen a menos de
//  `kGuiaAnclaMetros` de ahi. Dentro de esa ventana SI se elige el mas cercano (que es lo correcto:
//  asi no hace falta pasar exactamente por los puntos), pero **no se puede pegar un salto al otro
//  lado de la ruta**.
//
//  ★ LA VENTANA ES DE LAS DOS DIRECCIONES A PROPOSITO: si te das la vuelta y vuelves por donde
//    viniste, tienes que poder retroceder. Lo que no se puede es TELEPORTARTE.
// ===========================================================================
bool     gAnclaValida = false; // false = todavia no hay ancla (primer calculo tras empezar a guiar)

/**
 * DONDE ESTABAS, medido en METROS DE RECORRIDO desde el principio de la ruta.
 *
 * ★ Esta es la unidad buena (ver `kGuiaAnclaMetros`): el numero de tramo no vale, porque un tramo
 *   mide 10 m en el track en vivo y puede medir 1 km en una ruta simplificada.
 */
double gRecorridoAncla = 0.0;

/** Donde caes TU, en metros de recorrido. Se recalcula en cada busqueda y se usa para el ancla. */
double gRecorridoMejor = 0.0;
constexpr double kMetrosPorGradoLat = 110540.0;
constexpr double kMetrosPorGradoLon = 111320.0;

/**
 * Cuantos METROS de recorrido a cada lado del ancla se miran al buscar por donde vas.
 *
 * ★★ AQUI ESTA LA LECCION DEL DIA (2026-10-06) ★★
 *   El primer intento midio la ventana en NUMERO DE TRAMOS (24 tramos), y estaba MAL por un motivo
 *   que solo se ve montando el caso: **un tramo no mide lo mismo en un track que en otro**. Con
 *   puntos cada 10 m (el track en vivo), 24 tramos son 240 m; pero en una ruta con puntos cada
 *   1 km, 24 tramos son 24 km... y entonces la ventana **no protege de nada**: deja entrar el otro
 *   lado de la ruta. La prueba del motor lo cazo (con 3 tramos, el tramo «de la vuelta» estaba a
 *   distancia 2 y entraba en la ventana).
 *   **La ventana tiene que estar en METROS, que es lo que se quiere decir cuando se dice "cerca".**
 *
 * ★ POR QUE 250 m: es del orden de la anchura de un camino con su margen, y bastante menos que la
 *   separacion tipica entre dos trozos distintos de una misma ruta. Ni tan corto que una curva
 *   cerrada se salga, ni tan largo que deje entrar el otro lado.
 *
 * ★ Y NO HACE FALTA RECORRER LA RUTA DOS VECES: la distancia se va acumulando mientras se busca,
 *   asi que el mismo bucle sirve para medir y para elegir.
 */
constexpr double kGuiaAnclaMetros = 250.0;

// Lee un punto del track ACTIVO (sin aplicar el sentido).
bool leeCrudo(uint32_t idx, TrackPunto *out) {
  if (idx >= gPuntos) return false;
  // ★ AQUI ESTA EL TROZO (2026-10-06): `gDesde` desplaza la lectura dentro del track vivo, para
  //   poder guiar por UNA salida y no por la tirada entera. Con `gDesde = 0` (lo normal) esto es
  //   exactamente lo de antes.
  return (gFuente == TRK_FUENTE_VIVO) ? tracksVivoLee(idx + gDesde, out)
                                      : tracksRanuraLee((uint8_t)gSlot, idx, out);
}

// Pasa un punto de la ruta a metros, relativo a (lat0, lon0).
inline void aMetros(const TrackPunto &p, double lat0, double lon0, double cosLat0,
                    double *x, double *y) {
  *x = ((double)p.lon1e7 / 1e7 - lon0) * kMetrosPorGradoLon * cosLat0;
  *y = ((double)p.lat1e7 / 1e7 - lat0) * kMetrosPorGradoLat;
}

}  // namespace

bool tracksGuiaEmpieza(TrackFuente fuente, int slot, bool alReves) {
  if (fuente == TRK_FUENTE_NADA) return false;
  uint32_t n = 0;
  if (fuente == TRK_FUENTE_VIVO) {
    n = tracksVivoPuntos();
  } else {
    if (slot < 0 || slot >= (int)TRACK_SLOTS) return false;
    TrackRanuraInfo inf;
    tracksRanuraInfo((uint8_t)slot, &inf);
    if (!inf.valida) return false;
    n = inf.puntos;
  }
  // Con menos de dos puntos no hay linea que seguir: se rechaza en vez de guiar a un punto.
  if (n < 2) return false;

  gFuente  = fuente;
  gSlot    = slot;
  gAlReves = alReves;
  gPuntos  = n;
  gDesde   = 0;               // el track entero: del primer punto al ultimo
  gUltCalculoMs = 0;          // fuerza un calculo en el primer tick
  // ★ EL ANCLA SE OLVIDA AL EMPEZAR (2026-10-06): si no, se arrastraria el tramo de la ruta
  //   ANTERIOR y el primer calculo de esta se haria con la ventana puesta en un sitio que no
  //   tiene nada que ver con ella. Al invalidarla, el primer tick vuelve a mirar toda la ruta,
  //   que es lo correcto para engancharse.
  gAnclaValida = false;
  gGuia = TrackGuia{};
  gGuia.activo = true;
  return true;
}

bool tracksGuiaEmpiezaTrozo(uint32_t desde, uint32_t hasta, bool alReves) {
  const uint32_t total = tracksVivoPuntos();
  if (hasta > total) hasta = total;
  if (desde >= hasta) return false;
  const uint32_t n = hasta - desde;
  if (n < 2) return false;    // con un punto no hay camino que seguir

  gFuente  = TRK_FUENTE_VIVO;
  gSlot    = -1;
  gAlReves = alReves;
  gPuntos  = n;
  gDesde   = desde;           // ★ y aqui esta todo: se guia SOLO por este trozo
  gUltCalculoMs = 0;
  gAnclaValida = false;
  gGuia = TrackGuia{};
  gGuia.activo = true;
  return true;
}

void tracksGuiaTermina() {
  gFuente = TRK_FUENTE_NADA;
  gSlot = -1;
  gPuntos = 0;
  gDesde = 0;                 // y el trozo se olvida: el proximo guiado empieza limpio
  gAnclaValida = false;       // el ancla no sobrevive a un guiado (ver arriba)
  gGuia = TrackGuia{};
}

bool     tracksGuiaActivo() { return gFuente != TRK_FUENTE_NADA; }
void     tracksGuiaEstado(TrackGuia *out) { if (out) *out = gGuia; }
uint32_t tracksGuiaPuntos() { return gPuntos; }

// ¿Merece la pena dibujar el track entero? ★ Con un track de cientos de kilometros, meterlo en
// 200 px deja la linea como un garabato: no informa y ademas cuesta. El limite se pone en 200 km
// de largo TOTAL, que se calcula sumando los tramos... y eso seria caro de hacer en cada
// refresco. Asi que se estima barato: distancia en linea recta del primer punto al ultimo mas un
// 30% (una ruta casi nunca es mas corta que su recta, y con el 30% se cubre una ruta normal).
bool tracksGuiaDibujable() {
  if (gFuente == TRK_FUENTE_NADA || gPuntos < 2) return false;
  TrackPunto p0, pn;
  if (!tracksGuiaLee(0, &p0) || !tracksGuiaLee(gPuntos - 1, &pn)) return false;
  const double recta = gpsDistanceM((double)p0.lat1e7 / 1e7, (double)p0.lon1e7 / 1e7,
                                    (double)pn.lat1e7 / 1e7, (double)pn.lon1e7 / 1e7);
  return recta <= 200000.0;
}

bool tracksGuiaLee(uint32_t idx, TrackPunto *out) {
  if (!out || idx >= gPuntos) return false;
  // ★ EL SENTIDO SE APLICA AQUI, en un solo sitio: quien pinte no tiene que saber si va al
  //   reves. idx 0 es siempre "el principio de MI viaje", sea el principio del track o su final.
  const uint32_t real = gAlReves ? (gPuntos - 1 - idx) : idx;
  return leeCrudo(real, out);
}

void tracksGuiaTick(double lat, double lon) {
  if (gFuente == TRK_FUENTE_NADA || gPuntos < 2) return;

  const double lat0 = lat, lon0 = lon;
  const double cosLat0 = cos(lat0 * M_PI / 180.0);

  // ★★ UNA SOLA PASADA A LA FLASH (2026-09-22) ★★
  //   La primera version recorria el track tres veces (una para buscar el tramo mas cercano,
  //   otra para lo que falta, otra para el punto de mira): el triple de lecturas de flash y mas
  //   codigo. Aqui se guarda TODO lo que hace falta del tramo ganador mientras se busca, que es
  //   el punto de proyeccion y el indice. Asi el track se lee una vez.
  double mejorDist = 1e12;
  uint32_t mejorSeg = 0;          // tramo (idx -> idx+1, ya orientados) mas cercano
  double mejorT = 0.0;            // por donde del tramo cae tu pie (0..1)
  double mejorSegLen = 0.0;       // longitud de ese tramo

  TrackPunto a, b;
  if (!tracksGuiaLee(0, &a)) return;
  double ax, ay, bx, by;
  aMetros(a, lat0, lon0, cosLat0, &ax, &ay);

  // ★★ LA VENTANA DEL ANCLA, EN METROS DE RECORRIDO (2026-10-06) ★★
  //   `recorrido` es lo que llevas andado DE RUTA desde el principio, y `recAncla` donde estabas
  //   cuando te vio la ultima vez. Solo se mira el tramo si su recorrido cae dentro de la ventana.
  //   Ver la explicacion larga donde se declara `kGuiaAnclaMetros`.
  double recorrido = 0.0;

  for (uint32_t i = 0; i + 1 < gPuntos; i++) {
    if (!tracksGuiaLee(i + 1, &b)) break;
    aMetros(b, lat0, lon0, cosLat0, &bx, &by);

    const double vx = bx - ax, vy = by - ay;
    const double len2 = vx * vx + vy * vy;
    const double len = sqrt(len2);

    const double recTramo = recorrido + len * 0.5;   // el centro del tramo, en recorrido
    const bool enVentana = !gAnclaValida ||
        (recTramo >= gRecorridoAncla - kGuiaAnclaMetros &&
         recTramo <= gRecorridoAncla + kGuiaAnclaMetros);

    double t = 0.0;
    double d = 1e12;
    if (enVentana) {
      if (len2 > 1e-9) {
        t = (-ax * vx - ay * vy) / len2;   // tu posicion es (0,0): el vector A->P es -A
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
      }
      const double px = ax + t * vx, py = ay + t * vy;
      d = sqrt(px * px + py * py);
    }
    if (d < mejorDist) {
      mejorDist = d; mejorSeg = i; mejorT = t;
      mejorSegLen = len;              // en metros, porque ax..by ya estan en metros
      gRecorridoMejor = recorrido + len * t;   // donde caes tu, en recorrido
    }
    ax = bx; ay = by;
    recorrido += len;
  }

  // ★ Y EL ANCLA SE MUEVE A DONDE TE HA VISTO. A partir de aqui, la proxima busqueda se hace
  //   alrededor de ese punto, no de toda la ruta.
  //
  //   ★ RED DE SEGURIDAD: si no se ha encontrado NADA dentro de la ventana, se repite con la ruta
  //     entera. Sin esto, `mejorDist` seguiria valiendo 1e12 y `mejorSeg` se quedaria en 0: el
  //     motor diria que estas en el PRINCIPIO de la ruta, que es una mentira gorda.
  if (mejorDist > 1e11) {
    mejorDist = 1e12; mejorSeg = 0; mejorT = 0.0; mejorSegLen = 0.0;
    // `a` ya esta leido arriba y no se ha tocado: solo hay que rehacer sus metros.
    aMetros(a, lat0, lon0, cosLat0, &ax, &ay);
    double rec = 0.0;
    for (uint32_t i = 0; i + 1 < gPuntos; i++) {
      if (!tracksGuiaLee(i + 1, &b)) break;
      aMetros(b, lat0, lon0, cosLat0, &bx, &by);
      const double vx = bx - ax, vy = by - ay;
      const double len2 = vx * vx + vy * vy;
      const double len = sqrt(len2);
      double t = 0.0;
      if (len2 > 1e-9) {
        t = (-ax * vx - ay * vy) / len2;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
      }
      const double px = ax + t * vx, py = ay + t * vy;
      const double d = sqrt(px * px + py * py);
      if (d < mejorDist) {
        mejorDist = d; mejorSeg = i; mejorT = t;
        mejorSegLen = len;
        gRecorridoMejor = rec + len * t;
      }
      ax = bx; ay = by;
      rec += len;
    }
  }

  // ★ EL ANCLA SE MUEVE A DONDE CAES TU, en metros de recorrido (no en numero de tramo: ver
  //   `kGuiaAnclaMetros`). Si el mejor tramo no se ha podido calcular (ruta de un solo punto), se
  //   deja el ancla donde estaba, que es lo menos malo.
  if (mejorDist <= 1e11) gRecorridoAncla = gRecorridoMejor;
  gAnclaValida = true;

  // ---- lo que FALTA: lo que resta del tramo actual mas los tramos que quedan por delante ----
  // Se recorre SOLO desde el tramo ganador, no desde el principio.
  double falta = mejorSegLen * (1.0 - mejorT);
  TrackPunto q0, q1;
  if (tracksGuiaLee(mejorSeg + 1, &q0)) {
    for (uint32_t i = mejorSeg + 1; i + 1 < gPuntos; i++) {
      if (!tracksGuiaLee(i + 1, &q1)) break;
      falta += gpsDistanceM((double)q0.lat1e7 / 1e7, (double)q0.lon1e7 / 1e7,
                            (double)q1.lat1e7 / 1e7, (double)q1.lon1e7 / 1e7);
      q0 = q1;
    }
  }

  // ---- el PUNTO DE MIRA: 25 m de RUTA por delante ----
  // ★ Se apunta a un punto POR DELANTE y no al mas cercano a proposito: apuntando al mas
  //   cercano, la flecha vibra al ir encima de la linea y ademas te manda hacia atras. Es lo
  //   mismo que hace un GPS de track.
  //
  // ★★ AQUI HABIA UN FALLO (2026-09-22), y lo cazo la prueba del motor ★★
  //   La primera version empezaba a contar con `acum = lo que queda del tramo actual`, y el
  //   bucle solo avanzaba `mientras acum < 25`. O sea que con un tramo LARGO (un camino recto de
  //   1 km, que es lo normal) `acum` ya valia cientos de metros, el bucle no daba ni una vuelta,
  //   y **la mira se quedaba en la PUNTA del tramo**: la flecha apuntaba al final del tramo en
  //   vez de al trozo que viene. Con vertices juntos no se notaba (por eso la prueba con la L no
  //   lo vio); con un tramo recto largo, si.
  //   AHORA: se mide la distancia DESDE TU PIE, empezando en 0, y se avanza hasta juntar los
  //   25 m. Y si el pie esta dentro del tramo pero a menos de 25 m de su punta, se AVANZA HACIA
  //   LA PUNTA con el mismo avance que llevabas, que es lo que hace un GPS (la flecha sigue la
  //   linea, no salta al vertice).
  TrackPunto mira = a;
  {
    double acum = 0.0;                       // distancia recorrida DESDE EL PIE
    double enTramo = mejorSegLen * (1.0 - mejorT);   // lo que queda del tramo para llegar a su punta
    bool resuelto = false;

    if (enTramo >= kGuiaPasoM) {
      const double avance = kGuiaPasoM / (mejorSegLen > 1e-6 ? mejorSegLen : 1.0);
      TrackPunto p0, p1;
      if (tracksGuiaLee(mejorSeg, &p0) && tracksGuiaLee(mejorSeg + 1, &p1)) {
        const double apart = mejorT + (1.0 - mejorT) * avance;   // por donde cae el punto de mira
        mira.lat1e7 = (int32_t)llround(((double)p0.lat1e7 + apart * ((double)p1.lat1e7 - p0.lat1e7)));
        mira.lon1e7 = (int32_t)llround(((double)p0.lon1e7 + apart * ((double)p1.lon1e7 - p0.lon1e7)));
        mira.altM   = p1.altM;
        resuelto = true;
      }
    }
    if (!resuelto) {
      acum = enTramo;
      TrackPunto r0, r1;
      if (tracksGuiaLee(mejorSeg + 1, &mira)) {
        for (uint32_t i = mejorSeg + 1; i + 1 < gPuntos && acum < kGuiaPasoM; i++) {
          if (!tracksGuiaLee(i, &r0) || !tracksGuiaLee(i + 1, &r1)) break;
          acum += gpsDistanceM((double)r0.lat1e7 / 1e7, (double)r0.lon1e7 / 1e7,
                               (double)r1.lat1e7 / 1e7, (double)r1.lon1e7 / 1e7);
          mira = r1;
        }
      }
    }
  }

  gGuia.activo      = true;
  gGuia.alReves     = gAlReves;
  gGuia.sinTrack    = false;
  // ★ De donde sale el track (2026-09-22): la pantalla lo necesita para avisar de que el
  //   principio del anillo se perdio, y eso solo importa yendo HACIA ATRAS con el VIVO. Si este
  //   campo no se rellenara, la pantalla leería basura y el aviso saldria cuando no toca.
  gGuia.fuente      = gFuente;
  gGuia.desviacionM = (float)mejorDist;
  gGuia.faltanM     = (float)falta;
  gGuia.idxCerca    = mejorSeg;
  // El rumbo se mide desde TU posicion REAL hasta el punto de mira, no en el plano: asi sale en
  // grados de brujula de verdad, que es lo que se compara con el rumbo del GPS.
  gGuia.rumboRutaDeg = gpsBearingDeg(lat, lon,
                                     (double)mira.lat1e7 / 1e7, (double)mira.lon1e7 / 1e7);
}

// ===========================================================================
//  CONSULTAS DE DIAGNOSTICO (2026-10-07)
//
//  ★ VAN AQUI, FUERA DEL `namespace {` DE ARRIBA, Y ESO ES LO IMPORTANTE: dentro tendrian enlace
//    interno y `protocol.cpp` no las encontraria (error de enlace, «undefined reference»). Es un
//    fallo facil de cometer y de confundir con no haber escrito la funcion.
// ===========================================================================

/** Cuantas veces la espera al NVMC se ha pasado del tope. Si no es 0, hay que mirarlo. */
uint32_t tracksNvmcVencidas() { return gNvmcVencidas; }

/** Cuantas veces se ha esperado al NVMC en total (para tener una referencia de cuanto se escribe). */
uint32_t tracksNvmcEsperas() { return gNvmcEsperas; }

/** En que paso fallo la ultima preparacion de ranura, en palabras (para el mensaje de error). */
const char *tracksRanuraFalloPalabra() {
  switch (gUltimoFalloRanura) {
    case 1:  return "borrando las paginas";
    case 2:  return "escribiendo la cabecera de la ranura";
    case 3:  return "escribiendo la cabecera de la pagina";
    default: return "en un sitio que no se apunto";
  }
}

#endif  // FAKETEC_BOARD_TECHO
