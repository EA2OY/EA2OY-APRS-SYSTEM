// pins_faketec.h — Faketec Vx (nRF52840) radio wiring for APRS-LoRa firmware
// Source: NavaTastic variant (N-01) — Faketec shares env nrf52_promicro_diy_tcxo.
// Radio: Heltec HT-RA62 (SX1262)  [P0.17 = RXEN]
//     or Ebyte E22P-433M30S       [P0.17 = RADIO_POWER_ENABLE]
// Core pins (SPI/I2C/LED/button/UART) live in variants/faketec/variant.h.
// License: GPL-3.0

#pragma once

#include <RadioLib.h>

// Valor para decir "aqui no hay pin" (mismo convenio que en pins_techo.h).
#ifndef NO_PIN
#define NO_PIN (-1)
#endif

// SX1262 (shared between HT-RA62 and E22P)
#define SX126X_CS    (32 + 13)  // P1.13
#define SX126X_BUSY  (0 + 29)   // P0.29
#define SX126X_DIO1  (0 + 10)   // P0.10
#define SX126X_RESET (0 + 9)    // P0.09
#define SX126X_DIO3_TCXO_VOLTAGE 1.8

// ---- Hardware propio de la Faketec que el resto del firmware necesita saber ----
// (En el T-Echo estos mismos datos estan en pins_techo.h con otros valores.)
#define PIN_BATTERY_ADC 31   // P0.31 = AIN7 (el T-Echo lo tiene en P0.04 = AIN2)
#define BATTERY_DIVIDER 2.0f // divisor 1/2 fisico (2x1M)
#define BATTERY_LPCOMP_AIN 7 // AIN7 = P0.31 (el T-Echo: AIN2)
#define PIN_GPS_EN (0 + 24)  // P0.24 MOSFET del GPS (el T-Echo no tiene: NO_PIN)

#ifdef FAKETEC_RADIO_E22P
// E22P: P0.17 alimenta el modulo (HIGH en arranque, LOW en sleep); sin RXEN
#define RADIO_POWER_ENABLE_PIN (0 + 17)
#define SX126X_RXEN RADIOLIB_NC
#define SX126X_TXEN RADIOLIB_NC
#else
// HT-RA62 (Faketec): P0.17 = RXEN. NO conmuta TX/RX: es el ENABLE del camino de
// recepcion (LNA). RadioLib lo pone HIGH en RX y LOW en TX/idle (Meshtastic:
// "set high prior to receive"). La conmutacion TX/RX del HT-RA62 es interna
// por DIO2 (setDio2AsRfSwitch). Referencia: NavaTastic SX126xInterface.cpp:157
// + RF95Interface.cpp:149.
#define SX126X_RXEN (0 + 17)
#define SX126X_TXEN RADIOLIB_NC
#endif
