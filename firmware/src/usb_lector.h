// usb_lector.h — LECTURA DEL PUERTO USB: buzon de bytes + troceado en lineas.
//
// ★★ T-ECHO PROJECT BUTTER II (2026-09-16) ★★
//
// POR QUE EXISTE ESTE FICHERO:
// El parser del USB se alimentaba SOLO desde el bucle (protocol.cpp, feed()), y el bucle se
// pasa 1,5-3 s dentro del driver de la tinta esperando al panel. En ese rato el puerto no lo
// leia nadie, y el FIFO del CDC son 256 bytes (`CFG_TUD_CDC_RX_BUFSIZE`, ver
// libraries/Adafruit_TinyUSB_Arduino/src/arduino/ports/nrf/tusb_config_nrf.h). Consecuencia
// medida en el modelo del banco (tools/banco_usb): una linea larga —el `set` del
// configurador son ~1.4 KB— NO entra en una vuelta del bucle; hace falta ir vaciando el FIFO
// vuelta a vuelta, y cada vuelta puede levar otro repintado de 1,5-2,5 s por delante.
//
// LO QUE HACE, Y LO QUE NO (esto es lo importante):
//   * `bombea()`  -> LEE del puerto y ENCOLA los bytes en un anillo. **NO ejecuta NADA.**
//                    Es lo unico que llama el driver de la tinta desde sus esperas (el
//                    gancho de la pantalla, ver displaySetPumpBoton).
//   * `atiende()` -> SACA bytes del anillo y los mete por el UNICO sitio que los interpreta
//                    (`meteByte()`: la regla del 0xC0 del KISS y el troceado en lineas). Solo
//                    lo llama el BUCLE PRINCIPAL. La ejecucion de un comando nunca ocurre
//                    dentro del driver: no se puede pintar dentro de un pintado ni escribir
//                    la flash en medio de una transaccion con el panel.
//
// ES EL MISMO PATRON QUE EL DEL BOTON (Project Butter, 2026-09-15): buzon + cola + ejecucion
// en el bucle. Alli el buzon guarda FLANCOS con marca de tiempo; aqui, BYTES del puerto.
//
// UN SOLO ACUMULADOR, UN SOLO SITIO DONDE SE PARTEN LAS LINEAS:
//   * los bytes viven en `anillo_`,
//   * la linea a medias vive en `linea_`,
//   * y quien decide que un byte es del KISS, texto o fin de linea es SOLO `meteByte()`.
//   No hay un segundo parser en ninguna parte: el bucle y el driver comparten este.
//
// ES C++ PURO (ni Arduino ni String): el mismo fichero se compila en el firmware y en el
// banco de pruebas del ordenador (tools/banco_usb), asi que lo que se mide alli es ESTE
// codigo, no una copia reescrita para la prueba.
//
// License: GPL-3.0

#pragma once

#include <stddef.h>
#include <stdint.h>

// El puerto, visto por el lector: dos funciones y un contexto. En el firmware es el
// `Serial` del core (USB CDC) a traves de dos thunks de protocol.cpp; en el banco, el
// puerto simulado.
struct UsbPuerto {
  void *ctx;
  int (*disponible)(void *ctx);  // bytes que hay ahora mismo (0 = nada)
  int (*lee)(void *ctx);         // siguiente byte (0..255), o -1 si no hay
};

// Gancho de bytes del TNC: es `tncHandleUsbByte()`. Devuelve true cuando el byte pertenece a
// una trama KISS (entonces NO entra en la linea de texto). Es la regla del 0xC0: en cuanto el
// TNC esta en KISS, el binario se cuenta y no se interpreta.
typedef bool (*UsbByteFn)(void *ctx, uint8_t b);

// Gancho de linea completa: el despacho JSON/CLI de protocol.cpp. La cadena va terminada en
// cero y `n` es su longitud (sin el CR/LF).
typedef void (*UsbLineaFn)(void *ctx, const char *linea, size_t n);

class UsbLector {
 public:
  // Anillo de bytes. 4096 = el tope de una linea (kMaxLinea): asi, durante un repintado
  // entero, cabe holgadamente el `set` mas largo que puede mandar el configurador.
  static constexpr size_t kCap = 4096;

  // Tope de una linea de texto. Es el `kMaxLine` que vivia en protocol.h, con su historia:
  // ★ 1024 -> 4096 (2026-09-16): el configurador manda la configuracion ENTERA en una sola
  //   linea JSON, y con los cuatro perfiles de uso se pasa de 1024 caracteres. Al pasarse,
  //   los caracteres sobrantes se tiraban en silencio y el nodo contestaba "bad json" al
  //   intentar leer un JSON cortado. Medido: 55 campos, ~1320 caracteres. El margen de 4096
  //   cubre cualquier configuracion con holgura.
  static constexpr size_t kMaxLinea = 4096;

  // Engancha el despacho del protocolo (se llama una vez, al construir el protocolo).
  void init(UsbByteFn aTnc, UsbLineaFn aLinea, void *ctx);

  // ★ LEE Y ENCOLA. Nunca ejecuta. `enEsperaDePantalla` solo marca el CONTADOR de
  //   diagnostico (bytes leidos dentro de las esperas del driver de la tinta); no cambia
  //   nada de lo que se hace.
  size_t bombea(const UsbPuerto &p, bool enEsperaDePantalla = false);

  // ★ EJECUTA lo encolado (solo desde el bucle principal). Devuelve los bytes atendidos.
  size_t atiende();

  /* ★★ RED DE SEGURIDAD DE LA REENTRADA (no es un camino normal) ★★
     El gancho de la pantalla pone la marca mientras el driver esta dentro de sus esperas y
     el BUCLE la quita en cuanto `displayRefresh()` ha vuelto (ver main.cpp). Si alguien
     ejecuta un comando con la marca puesta, es que esta ejecutando DENTRO del pintado:
     `ejecutadasEnBombeo()` sube y se ve en `status.usb.dentro` / comando `usb`.
     Tiene que ser 0 SIEMPRE. */
  void marcaEnPantalla(bool dentro) { enPantalla_ = dentro; }

  // --- Contadores de diagnostico (los publica `status.usb`) --------------------
  uint32_t leidos() const { return leidos_; }              // bytes sacados del puerto
  uint32_t bombeadosEnEspera() const { return bombeadosEspera_; }  // ... dentro de un repintado
  uint32_t lineas() const { return lineas_; }              // lineas entregadas al despacho
  uint32_t tirados() const { return tirados_; }            // sobra de una linea > kMaxLinea
  size_t pendientes() const { return n_; }                 // bytes esperando en el anillo
  size_t maxPendientes() const { return maxPend_; }        // maximo historico (cuanto se encolo)
  uint32_t ejecutadasEnBombeo() const { return ejecutadasEnBombeo_; }  // red de seguridad: 0

 private:
  void meteByte(uint8_t b);   // EL UNICO sitio que interpreta bytes

  uint8_t anillo_[kCap];
  size_t cab_ = 0;   // de donde se saca
  size_t cola_ = 0;  // donde se mete
  size_t n_ = 0;     // bytes dentro

  char linea_[kMaxLinea + 1];
  size_t nLinea_ = 0;

  UsbByteFn aTnc_ = nullptr;
  UsbLineaFn aLinea_ = nullptr;
  void *ctx_ = nullptr;

  bool enPantalla_ = false;        // marca del gancho de la pantalla (ver arriba)
  uint32_t leidos_ = 0;
  uint32_t bombeadosEspera_ = 0;
  uint32_t lineas_ = 0;
  uint32_t tirados_ = 0;
  uint32_t ejecutadasEnBombeo_ = 0;
  size_t maxPend_ = 0;
};
