// ===========================================================================
//  tracks.h — GUIADO POR TRACKS y VUELTA A CASA (2026-09-22)
//
//  QUE ES: la capa de datos de dos cosas DISTINTAS que nunca se mezclan:
//
//    1) TRACK EN VIVO: lo graba el nodo solo, desde que coge posicion. Es TU viaje.
//       Sirve para "volver sobre mis pasos".
//    2) TRACKS CARGADOS: 5 ranuras con rutas que el operador carga desde el configurador
//       web (un GPX de WikiLoc). Son rutas de otros. Sirven para "guiarme".
//
//  ★★ EL MAPA DE MEMORIA, Y POR QUE CADA COSA ESTA DONDE ESTA ★★
//  (estas direcciones estan REPETIDAS en `extra_scripts/nrf52_uf2.py`: si se cambia una, hay
//   que cambiar la otra. El script comprueba que la aplicacion no invada el track en vivo.)
//
//    0x027000  aplicacion            (el b96 acababa en 0x81A58)
//    0x081A58  ... margen para que la aplicacion crezca .. 89 KB
//    0x098000  CABECERA del track en vivo   1 pagina ENTERA (4 KB) -- ver la nota de abajo
//    0x099000  TRACK EN VIVO   80 KB  (20 paginas)  = 6800 puntos, 68 km a 10 m/punto
//    0x0AD000  RANURA 1        20 KB  ( 5 paginas)  = 1698 puntos, 17 km
//    0x0B2000  RANURA 2        20 KB
//    0x0B7000  RANURA 3        20 KB
//    0x0BC000  RANURA 4        20 KB
//    0x0C1000  RANURA 5        20 KB
//    0x0C6000  (8 KB sin usar)
//    0x0C8000  registro de viaje 128 KB (32 paginas, SU anillo)  <-- SIGUE IGUAL
//    0x0E8000  config (2 paginas)
//    0x0ED000  sistema de ficheros interno (DENTRO del cargador: NO se puede mover)
//    0x0F4000  cargador
//    0x0FF000  ajustes del cargador
//
//  ★★ ESTOS NUMEROS SE QUEDARON VIEJOS UNA VEZ, Y ESO HIZO DAÑO (2026-10-05) ★★
//    Cuando el punto paso de 10 a 12 bytes (ver `TRACK_PUNTO_STRIDE`, mas abajo) la capacidad
//    cambio de 8160 a 6800 puntos y las direcciones se movieron 4 KB, pero **los comentarios no se
//    tocaron**. Y un revisor que SI leyó los comentarios publico en el README «unos 80 km» cuando
//    el aparato hace 68. La leccion: **al cambiar un numero en el codigo, se buscan sus copias en
//    los comentarios**. El valor bueno es SIEMPRE el de la macro, no el del comentario.
//
//  ★ POR QUE LOS TRACKS VAN *ANTES* DEL REGISTRO DE VIAJE Y NO DESPUES: despues del registro
//    esta la config (0xE8000) y, pegado, el sistema de ficheros interno del core (0xED000),
//    que vive DENTRO de la zona del cargador y no se puede mover. O sea que detras del registro
//    no hay sitio. Y tampoco se puede meter nada DENTRO del registro: es un ANILLO que se mueve
//    por todo su rango (0xC8000..0xE7FFF), asi que en un momento dado parece libre y no lo esta.
//
//  ★ Y POR QUE EL PRINCIPIO DE LOS TRACKS ESTA DONDE ESTA: primero se probo pegarlos a la
//    aplicacion (0x082000) y la guarda del generador de UF2 dijo que al margen de la app le
//    quedaban **1,4 KB**, o sea que el menu y la pantalla de navegacion habrian chocado con los
//    tracks del usuario. Bajarlos a 0x098000 deja 89 KB de margen para la aplicacion **sin
//    quitarle un byte al registro de viaje**. Eso si: las ranuras se quedan en **1698 puntos**
//    (eran 2040 con paso de 10 bytes por punto; al pasar a 12 bajaron), que para un track de
//    WikiLoc simplificado sigue dando de sobra: una ruta de 17 km muestreada cada 10 m son 1700
//    puntos, y el configurador la simplifica sola antes de mandarla.
//
//  ★★ EL PROBLEMA DEL TRACK EN VIVO, QUE HAY QUE ENTENDER ★★
//    El track en vivo tambien es un anillo (tiene que serlo: no se sabe cuanto va a durar la
//    ruta). Y un anillo que da la vuelta **PIERDE EL PRINCIPIO**. Para "volver sobre mis
//    pasos" el principio es justo el sitio al que hay que volver, asi que perderlo en silencio
//    seria el peor fallo posible: el nodo te llevaria a un punto que ya no es el inicio.
//    POR ESO EL ANILLO CUENTA LAS VUELTAS: cuando da la primera, se marca y quien navegue
//    AVISA en vez de mentir.
//    ★ LOS NUMEROS DE VERDAD, y ya van TRES correcciones (la ultima el 2026-10-05):
//      - Primero ponia "12288 puntos / ~13,6 h", de una version con 30 paginas. Mal.
//      - Luego "8160 puntos / 81,6 km", que era correcto con paso de 10 bytes por punto.
//      - Y **al pasar el punto a 12 bytes** (ver `TRACK_PUNTO_STRIDE`) la capacidad bajo a
//        **6800 puntos = 68 km**, y estos comentarios se quedaron con el numero viejo. Eso hizo
//        que un revisor publicara "80 km" en el README. De ahi la nota de arriba.
//      LOS DE AHORA (con paso 12): 4088/12 = 340 puntos por pagina
//        20 paginas x 340 puntos = 6800 puntos
//        a 1 punto cada 10 m  -> 68,0 km de ruta (unas 17 h andando a 4 km/h)
//        a 1 punto cada 60 s  -> 113 horas parado (4,7 dias)
//      Es decir: una ruta de un dia NO da la vuelta, pero un nodo olvidado encendido en casa
//      SI (4,7 dias parado), y ahi es donde importa el aviso.
// ===========================================================================

#pragma once

#include <stdint.h>
#include <stddef.h>

// ===========================================================================
//  ★★ ESTA FUNCION ES SOLO PARA LOS T-ECHO, Y NO ES UN CAPRICHO ★★
//
//  El mapa de memoria de arriba (la zona de tracks en 0x98000..0xC4FFF) esta calculado sobre
//  el reparto del T-Echo, y ADEMAS se comprobo antes de escribirlo: esas direcciones estaban
//  LIBRES en el b96 (la aplicacion acababa en 0x81C2B).
//
//  En las FAKETEC no se ha comprobado nada de eso: su aplicacion puede acabar en otro sitio y
//  escribir ahi seria pisar algo suyo sin saberlo. El operador lo dejo claro: **las Faketec se
//  quedan como estan** (siguen con su SoftDevice v6 y su mapa).
//
//  Por eso todo el modulo se compila SOLO para el T-Echo. En las Faketec no existe: ni el
//  codigo, ni las funciones, ni el espacio. Asi no hay forma de que se pisen por descuido.
// ===========================================================================
#if defined(FAKETEC_BOARD_TECHO) && !defined(TRACKS_FUERA)

#define TRACKS_DISPONIBLE 1

// ---------------------------------------------------------------- mapa de memoria
// ★ ESTAS DIRECCIONES ESTAN REPETIDAS EN `extra_scripts/nrf52_uf2.py`: si se cambia una, hay
//   que cambiar la otra. El script comprueba que la aplicacion no invada el track en vivo.
#define TRACK_VIVO_BASE    0x098000u                  // cabecera del vivo (1 pagina)
#define TRACK_SLOT_BASE    0x0AD000u
#define TRACK_SLOT_TAM     0x5000u                    // 20 KB por ranura (5 paginas)
#define TRACK_SLOTS        5u

#define TRACK_PAGINA       0x1000u                    // 4 KB
#define TRACK_VIVO_PAGINAS 20u                        // 80 KB
#define TRACK_SLOT_PAGINAS 5u                         // 20 KB

// ------------------------------------------------------- formato de una pagina
// Cabecera de 8 bytes de cada pagina:
//    0..3  magico 'TRK1'
//    4..5  numero de secuencia (para saber cual es la ultima pagina escrita al arrancar)
//    6..7  puntos validos en esta pagina
// Los puntos van detras, consecutivos, 10 bytes cada uno. El hueco que sobra detras del
// ultimo punto esta BORRADO (0xFF) y se puede seguir escribiendo sin borrar la pagina.
//   -> 4096 - 8 = 4088 bytes utiles; con paso de 12 son 340 puntos (8 bytes de sobra al
//      final de cada pagina, que no se usan).
#define TRACK_MAGIC        0x31524B54u   // 'TRK1' en little-endian
#define TRACK_PAG_HDR      8u

// ★★★ EL PUNTO OCUPA 12 BYTES EN LA FLASH, NO 10 — Y NO ES UN DESPILFARRO (2026-09-22) ★★★
//   La NVMC de este chip SOLO escribe palabras de 32 bits, y **la direccion tiene que estar
//   alineada a 4 bytes**. Si no lo esta, no da un error: da un FALLO DE BUS y el aparato se
//   queda colgado.
//   Con puntos de 10 bytes, los que empiezan en posicion impar caen desalineados:
//       punto 0 -> +8   (alineado)      punto 1 -> +18  DESALINEADO
//       punto 2 -> +28  (alineado)      punto 3 -> +38  DESALINEADO
//   O sea LA MITAD de los puntos. Y eso fue exactamente lo que paso: el b126 no arrancaba en el
//   T-Echo Plus porque el primer punto que grababa provocaba un fallo de bus.
//   (Por eso el fichero de diagnosis "sin grabar" SI arrancaba: solo escribia la cabecera de
//   fecha, que son 3 palabras alineadas en 0x98000. Nunca llegaba a escribir un punto.)
//   ★ LOS 2 BYTES DE RELLENO NO SE USAN: al leer solo se miran los 10 primeros. El precio es que
//     caben menos puntos por pagina (340 en vez de 408, un 17% menos), y ese precio se paga a
//     gusto: la alternativa es un aparato que no arranca.
//   ★ Y NO SE PUEDE "ARREGLAR" DE OTRA FORMA: escribir 3 palabras en una direccion no alineada
//     no es que sea lento, es que el chip no lo permite.
#define TRACK_PUNTO_STRIDE 12u
static_assert(TRACK_PUNTO_STRIDE % 4u == 0u,
              "el paso del punto tiene que ser multiplo de 4: la NVMC solo escribe palabras "
              "alineadas y si no, el aparato se cuelga con un fallo de bus");
// Cuantos puntos caben en una pagina, con el paso alineado.
#define TRACK_PUNTOS_PAGINA ((TRACK_PAGINA - TRACK_PAG_HDR) / TRACK_PUNTO_STRIDE)   // 340

// ★★ CABECERA DEL TRACK EN VIVO: CUANDO EMPEZO (2026-09-22) ★★
//   El operador pidio que los tracks se elijan POR FECHA Y HORA (no por nombre). Las ranuras
//   cargadas ya la llevan (la manda el configurador), pero el track EN VIVO no tenia ninguna:
//   el menu no podia decir cuando se grabo.
//   Va en las PRIMERAS 16 palabras de la zona del track en vivo, ANTES de la pagina 0, para que
//   sobreviva a un reinicio: si el nodo se reinicia a mitad de una ruta, la fecha de inicio es
//   la misma y no se pierde.
//   ★ Y POR ESO LA ZONA TIENE UN HUECO AL PRINCIPIO: los puntos empiezan 64 bytes mas alla de
//     `TRACK_VIVO_BASE`. Se prefiere perder esos 6 puntos a perder la fecha.
// ★★ LA CABECERA DEL VIVO OCUPA SU PROPIA PAGINA, Y ESO NO ES UN LUJO (2026-09-22) ★★
//   La primera version puso la cabecera de fecha en los primeros 64 bytes de la zona y los
//   puntos JUSTO DETRAS. Parecia razonable (se perdian 6 puntos) y estaba ROTO DE RAIZ:
//     - `flashBorraPagina()` usa ERASEPAGE, que borra la pagina de 4 KB que **CONTIENE** la
//       direccion, no "desde" ella. Con los puntos en 0x98040, las paginas ya no estaban
//       alineadas: cada cambio de pagina borraba los ultimos 64 bytes de la ANTERIOR, o sea sus
//       6 ultimos puntos, DESPUES de haberlos dado por validos. Se leian como 0xFF, que a 1e7
//       son (-1,-1) grados: puntos en el Golfo de Guinea. Pasaba ya a los 408 puntos (~4 km).
//     - La pagina 19 acababa 64 bytes DENTRO de la ranura 1, asi que al llegar ahi se borraba su
//       cabecera y la ranura se perdia.
//     - Y `tracksVivoOlvida()` escribia la fecha y tres lineas despues borraba esa misma pagina:
//       el track se quedaba SIN FECHA para siempre ("Vivo sin hora").
//   Lo cazo una revision independiente contando las paginas. La leccion: con ERASEPAGE, TODA
//   direccion que se borre tiene que estar ALINEADA A 4 KB. No es opcional.
#define TRACK_VIVO_CAB     0x1000u       // 1 pagina entera, SOLO para la cabecera del vivo
#define TRACK_VIVO_MAGIC   0x31564B54u   // 'TKV1'
#define TRACK_VIVO_BASE_PTS (TRACK_VIVO_BASE + TRACK_VIVO_CAB)

// Cuantos puntos caben DE VERDAD en cada zona. La cuenta sale de las constantes de arriba y no
// se escribe a mano en ningun otro sitio, para que no haya dos numeros que puedan discrepar.
#define TRACK_VIVO_PUNTOS  (TRACK_VIVO_PAGINAS * TRACK_PUNTOS_PAGINA)   // 6800

// ★★ EL TOPE DE UNA RANURA, CALCULADO AQUI Y NO DE MEMORIA (2026-09-22) ★★
//   Estaba puesto como `TRACK_SLOT_PAGINAS * TRACK_PUNTOS_PAGINA` = 2040, o sea como si las 5
//   paginas fueran iguales. NO LO SON: la pagina 0 lleva delante la cabecera de la ranura
//   (fecha, numero de puntos y las coordenadas de los extremos), asi que le caben menos puntos.
//   El numero que DE VERDAD se aplicaba vivia escondido en `tracks.cpp` (`SLOT_PUNTOS_REAL` =
//   2037), y el protocolo anunciaba 2040. Consecuencia, y es de las que hacen perder una tarde:
//   el configurador simplificaba un GPX a 2040 puntos porque el NODO le dijo que ese era el
//   tope, y el nodo rechazaba la subida con "no se pudo preparar la ranura". La culpa no era del
//   configurador: era del nodo por decir un numero que no cumplia.
//   AHORA HAY UN SOLO NUMERO, aqui, y lo usan los dos: el que valida y el que se anuncia.
#define TRACK_SLOT_CAB      32u     // sizeof(SlotCab): ver el static_assert de tracks.cpp
#define TRACK_SLOT_PAG0_PUNTOS \
  ((TRACK_PAGINA - TRACK_SLOT_CAB - TRACK_PAG_HDR) / TRACK_PUNTO_STRIDE)   // 338 (la 0 lleva cabecera)
#define TRACK_SLOT_PUNTOS \
  (TRACK_SLOT_PAG0_PUNTOS + (TRACK_SLOT_PAGINAS - 1u) * TRACK_PUNTOS_PAGINA)   // 1698

// ---------------------------------------------------------------- punto de track
// 10 bytes: lat int32, lon int32, alt int16.
//   int32 a 1e7 -> unidad de 1,1 cm. De sobra: el propio GPS tiene 3-5 m de error.
//   Se guardan enteros y no float a proposito: mismo tamano util, comparable sin margen de
//   error, y sin depender de como represente la coma flotante el compilador.
//
// ★★ `packed` NO ES OPCIONAL, Y EL `static_assert` DE ABAJO ES EL QUE LO VIGILA ★★
//   Sin `packed`, el compilador mete 2 bytes de relleno al final para alinear la struct a 4 y
//   **ocupa 12 bytes, no 10**. Eso descolocaria TODOS los puntos de la flash (cada uno se
//   leeria del sitio equivocado) y no se notaria hasta tener datos grabados. Lo cazo la
//   asercion en la primera compilacion: por eso esta puesta y por eso no se quita.
//
//   Y para que `packed` no obligue luego a leer structs desalineadas (en este chip eso se
//   castiga), todo el acceso a la flash en `tracks.cpp` pasa por buffers de bytes, nunca por
//   un puntero a `TrackPunto` sobre la flash.
struct __attribute__((packed)) TrackPunto {
  int32_t lat1e7;
  int32_t lon1e7;
  int16_t altM;
};
static_assert(sizeof(TrackPunto) == 10, "TrackPunto tiene que ocupar 10 bytes");

// ★ LA MISMA MEMORIA VISTA COMO PALABRAS DE 32 BITS, que es como hay que programar la flash:
//   el NVMC de este chip escribe palabras, no bytes sueltos. Con 10 bytes por punto son dos
//   palabras y media, asi que los puntos se programan a palabras y el ultimo de cada pagina
//   cae justo en el borde (408 x 10 + 8 de cabecera = 4088, y sobran 8 bytes hasta 4096, que
//   se quedan sin tocar y siguen borrados).
union TrackPuntoW {
  TrackPunto p;
  uint32_t   w[3];      // 12 bytes de hueco para poder programar los 10 utiles sin salirse
};
static_assert(sizeof(TrackPunto) == 10, "TrackPunto tiene que ocupar 10 bytes");

// Los mismos 10 bytes vistos como palabra de 32 bits, que es como se programa la flash (el
// NVMC de este chip solo escribe palabras). El ultimo punto de cada pagina son 8 bytes: se
// programa como dos palabras y los 2 bytes que sobran de la pagina no se tocan.
static_assert(TRACK_PAGINA % 4 == 0, "la pagina tiene que ser multiplo de 4 para programar palabras");

// ---------------------------------------------------------------- estado del guiado
enum TrackModo : uint8_t {
  TRACK_MODO_NADA = 0,     // sin guiado
  TRACK_MODO_SLOT,         // guiando un track cargado (de una ranura)
  TRACK_MODO_VIVO,         // guiando el track en vivo (volver a casa)
};

// ------------------------------------------------------- informacion de una ranura
// Lo que se ensena en el menu (fecha y hora, SIN nombre, como pidio el operador) y lo que hace
// falta para navegar sin tener que recorrer el track entero.
struct TrackRanuraInfo {
  bool     valida;         // la ranura tiene un track cargado y terminado
  uint16_t puntos;
  uint16_t year;
  uint8_t  month, day, hour, minute;
  int32_t  lat1e7Ini, lon1e7Ini;
  int32_t  lat1e7Fin, lon1e7Fin;
};

// ------------------------------------------------------------------- API publica
void tracksInit();                       // al arrancar: lee cabeceras. NO borra nada.

// ---- track en vivo
bool     tracksVivoActivo();             // ¿hay grabacion en vivo en marcha?
uint32_t tracksVivoPuntos();             // puntos validos AHORA (de los que quedan)
bool     tracksVivoDioLaVuelta();        // ★ true = el inicio del track YA SE PERDIO
void     tracksVivoAnade(double lat, double lon, float altM);
bool     tracksVivoLee(uint32_t idx, TrackPunto *out);   // idx 0 = el mas antiguo que queda
void     tracksVivoOlvida();             // empezar un track nuevo (al coger la primera posicion)

// ★ CUANDO EMPEZO el track en vivo (2026-09-22). El operador pidio elegir los tracks por FECHA Y
//   HORA, y el vivo no tenia ninguna. Devuelve false si no hay fecha (GPS sin hora todavia).
//   Sobrevive a un reinicio: va en la cabecera de la zona, no en RAM.
bool     tracksVivoCuando(uint16_t *year, uint8_t *month, uint8_t *day,
                          uint8_t *hour, uint8_t *minute);

// ★ ¿SE ESTA GRABANDO AHORA MISMO? (2026-09-22). Falso mientras hay un guiado en marcha, porque
//   el guiado pausa la grabacion (punto 7 de la especificacion). La pantalla lo necesita para
//   AVISAR: si el usuario acaba la ruta y se olvida de "Finalizar guiado", se queda sin grabar
//   el resto de la caminata, y hasta ahora eso solo se apuntaba en el registro de viaje (que se
//   lee por USB). Se PREGUNTA al motor en vez de guardar una copia en la pantalla: dos verdades
//   del mismo hecho acaban discrepando.
bool     tracksVivoGrabando();

// ★ LA GRABACION, CON SU CADENCIA, EN UN SOLO SITIO (2026-09-22).
//   La llama el bucle principal cada vez que pasa (cada 2 s). Ella sola decide si toca guardar
//   un punto, y para eso mira la posicion: NO se le pasa la posicion desde fuera a proposito,
//   para que la politica de grabacion viva en un unico sitio y no repartida por el bucle.
//
//   LA CADENCIA ES POR DISTANCIA, NO POR TIEMPO: se guarda un punto cuando te has movido
//   **10 metros** desde el ultimo guardado (o cada 60 s si estas parado, para que quede
//   constancia de que sigues ahi). Por que asi:
//     - guardando por TIEMPO, un tramo lento (subiendo) se llena de puntos y uno rapido
//       (bajando) queda vacio; el track no representa la ruta, representa el reloj;
//     - con 10 m, los 6.800 puntos del anillo dan para **68 km de ruta**, y una ruta de
//       montana de un dia entero son 15-30 km: sobra mas del doble.
//   Es la misma idea que usa cualquier GPS para grabar tracks.
void     tracksTick(uint32_t nowMs);

// ---- ranuras cargadas (las escribe el protocolo USB, no el menu)
void tracksRanuraInfo(uint8_t slot, TrackRanuraInfo *out);
bool tracksRanuraLee(uint8_t slot, uint32_t idx, TrackPunto *out);
bool tracksRanuraEmpieza(uint8_t slot, uint16_t puntos, const TrackRanuraInfo *meta);
bool tracksRanuraEscribe(uint8_t slot, uint32_t idx, const TrackPunto *p);
bool tracksRanuraTermina(uint8_t slot);
void tracksRanuraBorra(uint8_t slot);

// ===========================================================================
//  ★★★ MOTOR DE NAVEGACION: seguir la LINEA del track (2026-09-22) ★★★
//
//  QUE HACE: dado un track y tu posicion, calcula las tres cosas que se ensenan:
//    - la DESVIACION: a que distancia estas de la linea del track (perpendicular),
//    - la DIRECCION: hacia donde tienes que ir para seguir la ruta,
//    - lo que FALTA: metros de ruta que quedan hasta el final.
//
//  ★★ ES NAVEGACION EN RUTA, NO "EN LINEA RECTA AL DESTINO" ★★
//    La diferencia importa: en linea recta, si la ruta da una vuelta alrededor de un barranco,
//    la flecha te manda CRUZANDOLO. Aqui se busca el punto de la LINEA mas cercano a ti y se
//    sigue la linea desde ahi, que es lo que hace cualquier GPS de track.
//
//  ★ Y NO GASTA MEMORIA: no se copia el track a RAM (el vivo son 80 KB y no caben). Cada punto
//    se LEE DE LA FLASH cuando hace falta. Son unas 16.000 lecturas por calculo y el calculo se
//    hace cada 5 s: nada para este chip.
// ===========================================================================

// De donde sale el track que se esta siguiendo.
enum TrackFuente : uint8_t {
  TRK_FUENTE_NADA = 0,
  TRK_FUENTE_VIVO,      // el track grabado por el nodo
  TRK_FUENTE_RANURA,    // una de las 5 ranuras cargadas
};

// Lo que el motor ha calculado. Todo en unidades de calle: metros y grados.
struct TrackGuia {
  bool   activo;          // hay guiado en marcha
  bool   alReves;         // se va en sentido contrario (para "hacia atras" y "volver a casa")
  bool   sinTrack;        // el track elegido no tiene puntos suficientes
  // ★ DE DONDE SALE EL TRACK QUE SE ESTA GUIANDO (2026-09-22). La pantalla lo necesita para un
  //   aviso concreto: si vas HACIA ATRAS con el track EN VIVO, el destino es "el principio" del
  //   track, y si el anillo ya dio la vuelta ese principio no es donde empezaste. Sin este dato,
  //   la pantalla no puede distinguir ese caso y callaria.
  TrackFuente fuente;
  float  desviacionM;     // distancia a la LINEA del track (perpendicular)
  float  faltanM;         // metros de RUTA que quedan hasta el final de la ruta
  float  rumboRutaDeg;    // hacia donde va la ruta en el punto mas cercano (0..360)
  uint32_t idxCerca;      // indice del punto de la ruta mas cercano a ti
};

// Empieza a guiar. `slot` se ignora si la fuente es TRK_FUENTE_VIVO.
bool tracksGuiaEmpieza(TrackFuente fuente, int slot, bool alReves);
void tracksGuiaTermina();
bool tracksGuiaActivo();
void tracksGuiaEstado(TrackGuia *out);
// Cuantos puntos tiene el track que se esta guiando (para pintar y para saber si llegaste).
uint32_t tracksGuiaPuntos();
// Lee un punto del track que se esta guiando, ya con el sentido aplicado: idx 0 = el principio
// DEL VIAJE QUE ESTAS HACIENDO (o sea, el final del track si vas al reves). Asi la pantalla no
// tiene que saber nada del sentido.
bool tracksGuiaLee(uint32_t idx, TrackPunto *out);
// Recalcula con la posicion actual. La llama el bucle.
void tracksGuiaTick(double lat, double lon);
// ¿Se puede dibujar la LINEA del track? Falso si el track es tan largo que dibujarlo entero no
// aporta nada (mas de ~200 km): en ese caso se ensena solo la brujula y los datos.
bool tracksGuiaDibujable();

// ---------------------------------------------------------------------------------------
//  Constantes del motor. Estan aqui arriba y con nombre porque son LAS REGLAS que pidio el
//  operador, y no conviene que queden enterradas en medio de una cuenta.
// ---------------------------------------------------------------------------------------
constexpr float kGuiaCercaM       = 25.0f;   // a menos de esto del final: "LLEGADA"
constexpr float kGuiaMuyLejosM    = 500.0f;  // mas de esto de la linea: "MUY LEJOS"
constexpr float kGuiaPasoM        = 25.0f;   // cuanto se mira por delante para la flecha
constexpr float kGuiaAndandoKmh   = 1.0f;    // por debajo de esto, el rumbo del GPS no vale

#endif  // FAKETEC_BOARD_TECHO
