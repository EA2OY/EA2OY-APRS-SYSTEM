# N-01 — Faketec nRF52840: placa, pinout y mapeo real

## Qué se sabe de las placas "Faketec V1-V6"
- En NavaTastic **no existe variante "faketec_v1..v6"**: la placa Faketec comparte con el ProMicro DIY el env base `nrf52_promicro_diy_tcxo` (board `promicro-nrf52840`, HW model 63, `-DNRF52_PROMICRO_DIY`).
- La diferencia Faketec(HT-RA62 SX1262) vs ProMicro(E22P) se resuelve **por macro** dentro del mismo `variant.h`:
  - `-DNAVARICO_RADIO_SX1262` → Faketec (22 dBm, P0.17 = RXEN).
  - `-DNAVARICO_RADIO_E22P` → ProMicro E22P (12 dBm, P0.17 = RADIO_POWER_ENABLE).
- Ruta: `C:\NavaTastic Codigo completo\variants\nrf52840\diy\nrf52_promicro_diy_tcxo\variant.h`. El esquemático real es el PDF binario `Schematic_Pro-Micro_Pinouts_2025-12-04.pdf` de esa carpeta (pendiente de leer).
- Env Ejemplo: `navarrico_faketec_sx1262_r2ig_labaudit` (banco, 869.545) en `variants\nrf52840\navarrico.ini`.

## Pinout nRF52840 (variant.h, fragmentos exactos)
```
variant.h:117-119  PIN_SPI_MISO (0+2)  P0.02 | PIN_SPI_MOSI (32+15) P1.15 | PIN_SPI_SCK (32+11) P1.11
variant.h:124      LORA_CS (32+13)     P1.13
variant.h:135-137  LORA_DIO0 (0+29) P0.29 (=BUSY) | LORA_DIO1 (0+10) P0.10 (IRQ) | LORA_RESET (0+9) P0.09 (NRST)
variant.h:150-163  SX126X_CS (32+13), SX126X_DIO1 (0+10), SX126X_BUSY (0+29), SX126X_RESET (0+9),
                   SX126X_DIO2_AS_RF_SWITCH, y:
                     #ifdef NAVARICO_RADIO_E22P → SX126X_RXEN NC / TXEN NC
                     else                      → SX126X_RXEN (0+17) P0.17 (HT-RA62 Faketec)
variant.h:60-62    RADIO_POWER_ENABLE_PIN (0+17) P0.17  (solo E22P: HIGH arranque, LOW en deep sleep/storm)
variant.h:54       PIN_3V3_EN (0+13) P0.13  (periferia 3V3, HIGH en variant.cpp:36-37)
variant.h:87-88    PIN_WIRE_SDA (32+4) P1.04 | PIN_WIRE_SCL (0+11) P0.11   (I2C, 1 bus)
variant.h:100-103  GPS_TX_PIN (0+20) P0.20 | GPS_RX_PIN (0+22) P0.22 | PIN_GPS_EN (0+24) P0.24 | GPS_UBLOX
variant.h:65-82    BATERÍA ADC: BATTERY_PIN (0+31) P0.31, ADC_CHANNEL ADC1_GPIO4_CHANNEL,
                   VBAT_DIVIDER 0.6F (comentario desactualizado), VBAT_DIVIDER_COMP 2.0,
                   REAL_VBAT_MV_PER_LSB 1.46484375 mV, AREF_VOLTAGE 3.0, VBAT_AR_INTERNAL AR_INTERNAL_3_0
                   (divisor físico real: 2x1M = 0.5, factor 2.0)
variant.h:221-222  BATTERY_LPCOMP_INPUT NRF_LPCOMP_INPUT_7 | BATTERY_LPCOMP_THRESHOLD NRF_LPCOMP_REF_SUPPLY_9_16
variant.h:91-97    PIN_LED1 (0+15) P0.15 (rojo) | BUTTON_PIN (32+0) P1.00
variant.h:211-212  SX126X_DIO3_TCXO_VOLTAGE 1.8 + TCXO_OPTIONAL
variant.h:225-231  Curva OCV por radio: E22P clamp 3500 mV / SX1262 3400 mV
variant.h:236-238  #error si no hay macro de radio definida (USE_SX1262/LLCC68/RF95/SX1268/LR1121)
```

## Observaciones clave para nuestro firmware APRS
- El **OLED no tiene pines dedicados**: va por el bus I2C (P1.04/P0.11) con autodetección SSD1306/SH1106 en 0x3C/0x3D (ver N-05).
- `WIRE_INTERFACES_COUNT 1` → sensores y pantalla comparten `Wire` (sin Wire1 en estas placas).
- Radio: en el HT-RA62 la conmutación RF es interna vía DIO2 (`SX126X_DIO2_AS_RF_SWITCH`); en el módulo E22P es el pin de alimentación el que lo controla (ver N-02).
- GPS: hay pines UART dedicados P0.20/P0.22 + enable P0.24 (u-blox).
- Referencias del fork: Seed Solar Node (`seeed_solar_node\variant.h`: RXEN D5=P0.05) y T114 (`heltec_mesh_node_t114\variant.h`: otro reparto SPI) → NO copiar pines entre placas distintas: cada placa tiene su tabla.
- LEDs/GPIO: apagar LED antes de `sd_power_system_off` (GPIO enclavados ~10 mA) — ver N-03.
