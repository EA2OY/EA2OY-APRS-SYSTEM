# 02 — Keys & Configuration

## Credentials / keys
- Callsign: **EA2OY** (operator) + SSID for iGate/digi.
- APRS-IS passcode: only relevant if an internet backhaul is chosen (BLE host/cellular). Never stored as code literal.

## Hardcode rule
- No callsigns/passcodes/keys as literals in code. Config lives in a user-editable file/flash area (TBD after research). Validation on load.

## Critical configuration
- LoRa frequency/params for APRS-LoRa 433 MHz (must match CA2RXU ecosystem settings to interoperate).
- Callsign + SSID, beacon/digi behavior, sensor enable flags, sleep/eco thresholds.
- MUST NOT be touched: APRS frame format and RF params (interoperability).

## Recent config fixes / decisions
- 2026-09-09: none yet (config method TBD: after-UF2 simple editor).
