// usb_lector.cpp — buzon de bytes del puerto USB + troceado en lineas.
// Ver usb_lector.h (el porque y el contrato completo). License: GPL-3.0

#include "usb_lector.h"

void UsbLector::init(UsbByteFn aTnc, UsbLineaFn aLinea, void *ctx) {
  aTnc_ = aTnc;
  aLinea_ = aLinea;
  ctx_ = ctx;
  cab_ = cola_ = n_ = 0;
  nLinea_ = 0;
  linea_[0] = '\0';
  enPantalla_ = false;
  // Los contadores tambien empiezan de cero (el nodo enciende con ellos a 0, y el banco de
  // pruebas llama a init() entre escenario y escenario para que cada numero sea del suyo).
  leidos_ = 0;
  bombeadosEspera_ = 0;
  lineas_ = 0;
  tirados_ = 0;
  ejecutadasEnBombeo_ = 0;
  maxPend_ = 0;
}

/* ★★ LEE Y ENCOLA (lo llama el driver de la tinta desde sus esperas) ★★
   Lo unico que hace es sacar bytes del puerto y dejarlos en el anillo, EN ORDEN. Ni
   interpreta, ni parte lineas, ni ejecuta: eso es `atiende()`, y `atiende()` solo lo llama
   el bucle. Es lo que hace que leer dentro del repintado no pueda provocar reentrada.

   SI EL ANILLO SE LLENA, SE DEJA DE LEER: los bytes se quedan en el FIFO del CDC (256 bytes)
   y el USB los retiene (NAK). No se pierde ni uno; simplemente esperan a que el bucle vacie
   el anillo. Preferimos eso a tirar bytes del operador. */
size_t UsbLector::bombea(const UsbPuerto &p, bool enEsperaDePantalla) {
  if (p.disponible == nullptr || p.lee == nullptr) return 0;

  size_t leidosAhora = 0;
  while (n_ < kCap && p.disponible(p.ctx) > 0) {
    const int b = p.lee(p.ctx);
    if (b < 0) break;  // se acabo lo que habia: no se espera aqui
    anillo_[cola_] = (uint8_t)b;
    cola_ = (cola_ + 1) % kCap;
    n_++;
    leidos_++;
    leidosAhora++;
    if (enEsperaDePantalla) bombeadosEspera_++;
    if (n_ > maxPend_) maxPend_ = n_;
  }
  return leidosAhora;
}

/* ★★ EJECUTA LO ENCOLADO (solo desde el bucle principal) ★★
   Saca los bytes del anillo en el mismo orden en que llegaron y se los da a `meteByte()`,
   que es el unico sitio que los interpreta. Aqui es donde una linea completa puede acabar
   ejecutando un comando: por eso esto NO se puede llamar desde el driver de la pantalla. */
size_t UsbLector::atiende() {
  // Red de seguridad, no un camino normal: si la marca del gancho de la pantalla sigue
  // puesta, es que se esta ejecutando DENTRO del repintado. Tiene que quedarse en 0
  // SIEMPRE (sale en `status.usb.dentro` y en el comando `usb`).
  if (enPantalla_) ejecutadasEnBombeo_++;

  size_t atendidos = 0;
  while (n_ > 0) {
    const uint8_t b = anillo_[cab_];
    cab_ = (cab_ + 1) % kCap;
    n_--;
    meteByte(b);
    atendidos++;
  }
  return atendidos;
}

/* EL UNICO SITIO QUE INTERPRETA BYTES. Orden EXACTO del feed() de antes (protocol.cpp):
     1) el byte se le ofrece al TNC (KISS) ANTES que nada: si lo coge, la linea de texto a
        medias muere con la trama binaria (`nLinea_ = 0`, igual que el `lineBuf_ = ""` de
        antes). ESA es la regla del 0xC0, y sigue en el mismo sitio y en el mismo orden.
     2) el CR se ignora (tolerancia CRLF),
     3) el LF cierra la linea y la entrega al despacho,
     4) lo demas se acumula, con el mismo tope de 4096 caracteres por linea. */
void UsbLector::meteByte(uint8_t b) {
  if (aTnc_ != nullptr && aTnc_(ctx_, b)) {
    nLinea_ = 0;  // una linea a medias muere con la trama binaria
    return;
  }

  const char c = (char)b;
  if (c == '\r') return;
  if (c == '\n') {
    if (nLinea_ > 0) {
      linea_[nLinea_] = '\0';
      lineas_++;
      if (aLinea_ != nullptr) aLinea_(ctx_, linea_, nLinea_);
    }
    nLinea_ = 0;
    return;
  }
  if (nLinea_ < kMaxLinea) {
    linea_[nLinea_++] = c;
  } else {
    tirados_++;  // sobra de una linea larguisima: se tira, como antes
  }
}
