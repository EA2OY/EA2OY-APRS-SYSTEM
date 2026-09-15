# 01 — Project, Components & Versions

## Overview
Custom APRS-LoRa 433 MHz firmware for Faketec V1-V6 boards (nRF52840): Digipeater/iGate that forwards/monitors APRS over LoRa, with solar-brownout resilience and sensor support. Radio: Heltec HT-RA62 (SX1262) or Ebyte E22P-433M30S (SX1262). Future: tracker mode.

## Main components / branches
- **Boards**: Faketec V1..V6 (nRF52840) — GPIO/PMU mapping TBD per variant.
- **Radio**: HT-RA62 (Heltec, SX1262) / E22P-433M30S (Ebyte, SX1262) — both SX1262 family.
- **Power monitoring**: INA219 (I2C), INA3221 (3-ch I2C).
- **Weather sensors**: BMP280, AHT20, BME280, BME680 (I2C).
- **Resilience**: deep sleep until solar recovers (anti-brownout) — port from user's Meshtastic project.
- **UI**: I2C OLED display.
- **Position**: GPS (as in CA2RXU firmware).
- **Modes**: digipeater/iGate; future tracker.

## Current version
Pre-alpha (no code yet). Base/reference: CA2RXU `LoRa_APRS_iGate-main` (ESP32) at the working folder root — protocol reference only (no nRF52 variant inside).

## Allowed differences between components
- May vary: Faketec board variant (pins/PMU), radio module wiring (HT-RA62 vs E22P), enabled sensors.
- Forbidden to break: APRS-LoRa framing/params interoperable with CA2RXU ecosystem; 433 MHz; callsign rules; do not hardcode pinouts without verifying each Faketec Vx.
