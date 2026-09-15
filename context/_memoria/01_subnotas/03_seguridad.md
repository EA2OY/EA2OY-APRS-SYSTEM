# 03 — Security & Access

## Security model
APRS is an open amateur-radio service. No encryption (license/legal). "Security" here = operator control and etiquette: only the licensed operator transmits under EA2OY.

## Protection mechanisms
- Tx gating: transmit only when explicitly enabled (ham-only feature, as in CA2RXU).
- Callsign validation + blacklist for digipeated/forwarded packets (port from CA2RXU logic).
- Config protected from accidental corruption (checksum/atomic write) — later.

## Access rules
- Config changes: local operator only (serial/BLE/Web config after UF2). No remote unauthorized control.
- Beacon/telemetry content must not leak credentials.

## Recent security errors / fixes
- None yet (no code).
