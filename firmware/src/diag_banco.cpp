// diag_banco.cpp — LAS DOS PREGUNTAS DE DIAGNOSTICO, CONTESTADAS "NO" (2026-09-21)
//
// ===========================================================================
//  POR QUE EXISTE ESTE FICHERO
//
//  El driver de la tinta pregunta dos cosas antes de escribir trazas por el puerto serie:
//        diagTrazaTaller()    -> ¿salimos con las trazas de taller?
//        diagTrazaArranque()  -> ¿podemos escribir las lineas del arranque?
//  Esas dos funciones viven en `src/diag.cpp`, que a su vez tira de medio proyecto (APRS, GPS,
//  radio, sensores, TNC, rastreador, protocolo...). El banco de medida de la pastilla tactil
//  (`src/hello_pad.cpp`, entorno `techo_plus_pad`) NO compila nada de eso: es un firmware de
//  un solo proposito.
//
//  Como el driver necesita esas dos respuestas para ENLAZAR, aqui se dan de la forma mas
//  barata posible: las dos dicen que NO, o sea sin trazas. Es justo lo que quiere un banco de
//  medida, que ya escribe lo suyo por el puerto en cada vuelta.
//
//  ★ ESTE FICHERO NO ENTRA EN NINGUN OTRO ENTORNO: solo lo compila `techo_plus_pad`, porque su
//    `build_src_filter` lo pide por su nombre. Si entrara en los demas, habria DOS definiciones
//    de estas funciones y el enlazador se quejaria (con razon).
// ===========================================================================

#include <Arduino.h>

bool diagTrazaTaller() { return false; }
bool diagTrazaArranque() { return false; }
