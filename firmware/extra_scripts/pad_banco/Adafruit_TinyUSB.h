// Adafruit_TinyUSB.h — SUSTITUTO MINIMO, SOLO PARA EL BANCO DE MEDIDA (2026-09-21)
//
// ===========================================================================
//  POR QUE EXISTE ESTE FICHERO
//
//  La libreria `Wire` del nucleo (`Wire_nRF52.cpp`, linea 32) hace:
//        #include <Adafruit_TinyUSB.h>   // for Serial
//  o sea que la incluye UNICAMENTE para tener declarado `Serial`. En un proyecto normal eso
//  no molesta porque el nucleo ya trae la libreria entera (y `Adafruit_USBD_CDC Serial`).
//
//  Pero el banco de medida de la pastilla tactil (`src/hello_pad.cpp`, entorno
//  `techo_plus_pad`) NO compila el nucleo entero: es un firmware de un solo proposito. Ahi la
//  libreria de TinyUSB no esta, y al intentar traerla por el gestor de librerias arrastra
//  SdFat y compañia... que es exactamente lo que este banco existe para NO tener.
//
//  Este fichero resuelve el problema de la forma mas simple: declara `Serial` y nada mas. El
//  compilador queda contento, y en el enlazado se usa el `Serial` de verdad del nucleo. Es la
//  MISMA idea que el sustituto vacio de `ble_kiss.h` que ya usa este proyecto.
//
//  ★ NO AFECTA A NINGUN OTRO ENTORNO: esta carpeta **no** esta en el camino de busqueda de
//    includes de los demas entornos. Solo entra cuando se añade a mano con `-I`, y eso solo
//    lo hace el entorno `techo_plus_pad`. Si algun dia se añadiera a otro, ese entorno
//    compilaria con un `Serial` de mentira: NO HACERLO.
// ===========================================================================

#pragma once

#include <Arduino.h>

// `Serial` ya existe en el nucleo (es un `Adafruit_USBD_CDC`). Aqui solo hace falta que el
// nombre este declarado para que `Wire_nRF52.cpp` compile. Si por lo que sea no lo estuviera,
// estas dos lineas lo dejan inservible en vez de romper la compilacion.
extern HardwareSerial Serial;
