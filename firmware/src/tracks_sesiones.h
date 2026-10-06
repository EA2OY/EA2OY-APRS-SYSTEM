// tracks_sesiones.h — LAS SALIDAS: partir el registro de viaje en tracks para "volver a casa"
//
// QUE ES UNA SALIDA: cada vez que el operador sale de casa. Se reconocen porque el registro de
// viaje se corta cuando pasa mucho tiempo sin apuntar nada (una parada larga = se acabo la salida).
//
// ★★ POR QUE ESTO NO LEE LAS POSICIONES DEL REGISTRO ★★
//   El registro solo apunta una posicion CUANDO EL NODO MANDA UNA BALIZA, y la baliza de fabrica
//   sale **cada 30 minutos** parado. O sea que un paseo de tres horas deja **6 puntos**: con eso no
//   se puede dibujar un camino ni guiar a nadie.
//   Pero el registro SI dice dos cosas que no estan en ningun otro sitio: **cuando empezo y cuando
//   acabo cada salida**. Y el track de la flash tiene el camino FINO (un punto cada 10 m) pero **no
//   tiene horas** (un punto son 10 bytes: latitud, longitud y altura).
//   Asi que esto hace de puente: saca del registro las DOS PUNTAS de cada salida (la primera
//   baliza y la ultima) y **busca esas dos posiciones dentro del track fino**. El trozo de track
//   que queda entre ellas es la salida, y por ahi se guia.
//
// ★ LAS DOS PUNTAS SON APROXIMADAS: la primera baliza no cae exactamente en el primer punto del
//   track (el track empieza antes, en cuanto hay fijacion). Pero el error es de metros, no de
//   kilometros, y para "volver a casa" eso no cambia nada.
//
// ★ CUANDO UNA SALIDA YA NO ESTA: el track es un anillo de 6.800 puntos (unos 68 km). Cuando se
//   llena, empieza a borrar por el principio, asi que las salidas mas viejas **se quedan sin
//   camino**. Esas NO salen en la lista: una lista que ofrece algo que ya no existe es mentir.
//
// License: GPL-3.0

#pragma once

#include <Arduino.h>

/** Cuantas salidas se guardan en la lista. Mas no caben de una vez en la pantalla de 200x200. */
#define TRACK_SESIONES_MAX 10

/**
 * Un hueco de este tamaño (o mas) sin apuntar nada = se acabo la salida.
 *
 * ★ SON 5 HORAS, Y ES DECISION DEL OPERADOR (2026-10-06). El configurador web usaba 30 minutos y
 *   eso partia un paseo en dos cada vez que te sentabas a comer, con un efecto malo de verdad:
 *   **"volver a casa" te dejaba en el sitio de la parada**, no en casa, porque el track nuevo
 *   empieza donde paraste. Con 5 horas un dia de monte entero cabe en una sola salida.
 *   ★ Este numero tiene que ser EL MISMO en el configurador web (`web/index.html`,
 *     `HUECO_SESION_S`): si no, el aparato y la web ensenarian listas distintas.
 */
#define TRACK_SESION_HUECO_S (5u * 3600u)

/** Fecha y hora en que empezo una salida (de su PRIMERA baliza buena). */
struct TrackSesion {
  bool     valida;      // true = tiene puntos dentro del track y se puede guiar por ella
  uint16_t year;
  uint8_t  month, day, hour, minute;
  uint32_t puntos;      // cuantos puntos del track tiene el trozo de esta salida
  uint32_t metros;      // cuantos metros de camino son (sumando los tramos)
};

/**
 * Rehace la lista de salidas leyendo el registro de viaje.
 *
 * Se llama al abrir la pantalla de tracks (no en cada repintado: recorrer el registro cuesta).
 * Deja la lista ordenada DE LA MAS RECIENTE A LA MAS VIEJA, que es como se elige.
 *
 * @return cuantas salidas han quedado en la lista (0 si no hay ninguna).
 */
uint8_t tracksSesionesRehace();

/** Cuantas salidas hay en la lista (el resultado de la ultima llamada a [tracksSesionesRehace]). */
uint8_t tracksSesionesCuenta();

/**
 * Cuantas salidas se sacaron DEL REGISTRO DE VIAJE en la ultima llamada (2026-10-06).
 *
 * PARA QUE: para poder decir en pantalla POR QUE la lista sale vacia. Si esto es 0, el modulo no ha
 * encontrado ni una baliza con hora y posicion en el registro; si es > 0 pero la lista esta vacia,
 * es que sus salidas ya no tienen camino en el anillo (se lo comio el reciclado) o que sus puntas
 * no cuadran con el track.
 *
 * ★ Esto no es un adorno: el operador reporto «sale sin tracks grabados, pero el aparato tiene un
 *   track de hoy y otros de otros dias», y sin este numero **no hay forma de saber cual de los dos
 *   casos es** en un aparato al que no se le puede conectar un depurador.
 */
uint8_t tracksSesionesDelRegistro();

/** Lee una salida de la lista. `idx` 0 = la mas reciente. False si no existe. */
bool tracksSesionesLee(uint8_t idx, TrackSesion *out);

/**
 * EL TROZO DEL TRACK que le toca a una salida: desde que punto hasta cual (sin incluir el de
 * arriba). Es lo que hay que guiar.
 *
 * ★ POR QUE HACE FALTA: el track vivo es UNA tirada de puntos de todas las salidas juntas, y el
 *   motor de guiado necesita saber por donde empieza y acaba ESTA salida. Sin esto, "volver a
 *   casa" te guiaria por la ruta entera (los paseos de anteayer incluidos).
 */
uint32_t tracksSesionDesde(uint8_t idx);
uint32_t tracksSesionHasta(uint8_t idx);
