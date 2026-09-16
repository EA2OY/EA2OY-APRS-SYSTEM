/*
  Faketec V1-V6 (nRF52840) variant — APRS_NAVARRICA_FAKETEC_433
  Pinout source: NavaTastic fork variant "nrf52_promicro_diy_tcxo" (N-01);
  Faketec and ProMicro DIY share the same board family and pin table.
  License: GPL-3.0 (project) / LGPL-2.1 boilerplate from Adafruit core lineage.
*/

#ifndef _VARIANT_FAKETEC_
#define _VARIANT_FAKETEC_

/** Master clock frequency */
#define VARIANT_MCK (64000000ul)

// Board has no 32 kHz crystal: use the RC oscillator for the LF clock
#define USE_LFRC

/*----------------------------------------------------------------------------
 *        Headers
 *----------------------------------------------------------------------------*/

#include "WVariant.h"

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/*
  Pin table (Arduino pin number == Nordic GPIO number, identity map, see
  variant.cpp). Radio wiring lives in src/pins_faketec.h (single source).

  | GPIO  | Function        | GPIO  | Function       |
  |-------|-----------------|-------|----------------|
  | P0.02 | SPI MISO        | P1.11 | SPI SCK        |
  | P1.13 | SX1262 CS       | P1.15 | SPI MOSI       |
  | P0.29 | SX1262 BUSY     | P0.10 | SX1262 DIO1    |
  | P0.09 | SX1262 RESET    | P0.17 | RXEN / E22P PW |
  | P1.04 | I2C SDA         | P0.11 | I2C SCL        |
  | P0.20 | GPS TX          | P0.22 | GPS RX         |
  | P0.24 | GPS EN          | P0.31 | Battery ADC    |
  | P0.15 | LED (red)       | P1.00 | Button         |
  | P0.13 | 3V3 rail enable |       |                |
*/

// Number of pins defined in PinDescription array
#define PINS_COUNT (48)
#define NUM_DIGITAL_PINS (48)
#define NUM_ANALOG_INPUTS (1)
#define NUM_ANALOG_OUTPUTS (0)

// Pin 13 enables the 3.3V periphery rail (radio module, sensors, OLED).
// Kept HIGH by initVariant(); power-off candidates for deep sleep (N-03).
#define PIN_3V3_EN (0 + 13) // P0.13

// Analog pins (battery)
#define BATTERY_PIN (0 + 31) // P0.31 Battery ADC
#define ADC_RESOLUTION 14
#define AREF_VOLTAGE 3.0

// WIRE (I2C) — one bus shared by OLED + sensors
#define WIRE_INTERFACES_COUNT 1

#define PIN_WIRE_SDA (32 + 4) // P1.04
#define PIN_WIRE_SCL (0 + 11) // P0.11

// LED (red)
#define PIN_LED1 (0 + 15) // P0.15
#define LED_BUILTIN PIN_LED1
#define LED_BLUE PIN_LED1
#define LED_STATE_ON 1 // State when LED is lit

// Button
#define BUTTON_PIN (32 + 0) // P1.00

// UART interfaces
#define PIN_SERIAL1_RX (0 + 22) // P0.22 - GPS RX (data from the GNSS)
#define PIN_SERIAL1_TX (0 + 20) // P0.20 - GPS TX (data from the MCU)

#define PIN_SERIAL2_RX (0 + 6) // P0.06
#define PIN_SERIAL2_TX (0 + 8) // P0.08

// GPS power MOSFET (active high, NavaTastic GPS_EN_ACTIVE=1)
#define PIN_GPS_EN (0 + 24) // P0.24

// SPI interfaces (radio bus)
#define SPI_INTERFACES_COUNT 1

#define PIN_SPI_MISO (0 + 2)   // P0.02
#define PIN_SPI_MOSI (32 + 15) // P1.15
#define PIN_SPI_SCK  (32 + 11) // P1.11

#ifdef __cplusplus
}
#endif

#endif // _VARIANT_FAKETEC_
