# 06 — Build & Distribution

## Build commands
- TBD. Working repo is ESP32 (PlatformIO, espressif32). For nRF52840 we must choose platform:
  candidates PlatformIO `nrf52`/Arduino, Zephyr, or Meshtastic toolchain (aligns with reuse of drivers). Research before building.

## Artifact distribution
- Target: **UF2** file that users drag-and-drop onto the Faketec bootloader. Plus a simple post-flash config method.
- CA2RXU distributes binaries via a Web Flasher; our model = UF2 + config.

## Parity verification
- One build per Faketec Vx (pins) and per radio (HT-RA62 vs E22P) — all share same APRS core; verify identical protocol behavior.

## Known errors
- Base repo (CA2RXU ESP32 iGate) has no nRF52 variant; ESP32-only modules (WiFi/Web/OTA/NTP) are not usable on nRF52840 — must be dropped or replaced (BLE). Not a bug, a constraint.
