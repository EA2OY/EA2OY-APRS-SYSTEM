/* Guion de enlazado para nRF52840 con SoftDevice S140 **version 7**.
 *
 * POR QUE HACE FALTA: el framework de Adafruit solo trae el guion de la S140 v6
 * (`nrf52840_s140_v6.ld`, aplicacion en 0x26000). Las placas que llevan grabada la
 * S140 v7 (como el LilyGO T-Echo Plus de N0CALL, con S140 7.2.0) tienen el SoftDevice
 * un poco mas grande, asi que **la aplicacion empieza en 0x27000**.
 *
 * Si se grabara un binario enlazado para la v6, empezaria en 0x26000 y pisaria los
 * ultimos 4 KB del SoftDevice: o el bootloader lo rechaza, o se lo come y deja la
 * radio inservible. Por eso hay un entorno aparte para estas placas.
 *
 * DE DONDE SALEN LOS NUMEROS (no inventados):
 *   - 0x27000: lo usa el guion de Meshtastic para S140 v7
 *     (`src/platform/nrf52/nrf52840_s140_v7.ld`, comprobado el 2026-09-14) y tambien
 *     el guion de la v7 que SI trae el framework para el nRF52833.
 *   - 0xED000 como final de la zona de aplicacion y 0x20006000 de RAM: los mismos que
 *     el guion de la v6 del framework, para que la unica diferencia sea el inicio.
 */

SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)

MEMORY
{
  FLASH (rx)     : ORIGIN = 0x27000, LENGTH = 0xED000 - 0x27000

  /* La RAM que necesita el SoftDevice depende de su configuracion (tabla de atributos,
   * tamano de UUIDs, MTU, enlaces...). 0x20006000 es lo que reserva el framework para
   * la S140 en el nRF52840. */
  RAM (rwx) :  ORIGIN = 0x20006000, LENGTH = 0x20040000 - 0x20006000
}

SECTIONS
{
  . = ALIGN(4);
  .svc_data :
  {
    PROVIDE(__start_svc_data = .);
    KEEP(*(.svc_data))
    PROVIDE(__stop_svc_data = .);
  } > RAM

  .fs_data :
  {
    PROVIDE(__start_fs_data = .);
    KEEP(*(.fs_data))
    PROVIDE(__stop_fs_data = .);
  } > RAM
} INSERT AFTER .data;

INCLUDE "nrf52_common.ld"
