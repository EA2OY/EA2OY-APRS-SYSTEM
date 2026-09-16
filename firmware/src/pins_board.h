// pins_board.h — punto UNICO donde se elige el fichero de pines de la placa.
//
// Cada placa declara en su propio fichero lo que el resto del firmware necesita
// saber de ella (pines de radio, bateria, GPS...):
//   - `pins_faketec.h`  -> Faketec / ProMicro nRF52840 (SX1262 HT-RA62 o E22P)
//   - `pins_techo.h`    -> LilyGO T-Echo y T-Echo Plus (SX1262)
//
// La placa se elige con un define de compilacion (`platformio.ini`), no con un
// `#ifdef` repartido por medio proyecto: asi, anadir una placa nueva es anadir un
// fichero y una linea, y no hay que tocar la logica.
//
// License: GPL-3.0

#pragma once

#if defined(FAKETEC_BOARD_TECHO)
#include "pins_techo.h"
#elif defined(FAKETEC_BOARD_FAKETEC)
#include "pins_faketec.h"
#else
#error "No hay placa elegida: falta -DFAKETEC_BOARD_FAKETEC o -DFAKETEC_BOARD_TECHO en platformio.ini"
#endif
