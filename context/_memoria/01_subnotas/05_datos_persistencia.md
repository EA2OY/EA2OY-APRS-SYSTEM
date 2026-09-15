# 05 — Data & Persistence

## Data strategy
- Config (callsign, RF params, sensor flags, thresholds) persisted in flash so it survives sleep/reboot.
- Last-heard / packet buffers: RAM or low-write flash; dedupe to avoid repeating packets (port from CA2RXU station/heard handling).

## Mechanisms
- Selective writing: only persist config on change; buffers in RAM with periodic/none flash writes (flash wear on nRF52840).
- Atomic config writes with validation to avoid brick/corruption.

## Persistence protection
- Avoid writing telemetry/logs to flash frequently; guard critical settings.
- Protect config region from brownout-write corruption.

## Recent errors / fixes
- None yet (no code).
