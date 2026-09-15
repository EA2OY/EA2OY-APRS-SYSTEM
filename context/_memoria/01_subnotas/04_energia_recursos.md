# 04 — Power & Resources

## Power / resource management
- Core feature: **solar/brownout-safe deep sleep** — before voltage collapses (brownout), sleep deeply until solar panel recovers charge; avoid needing physical access to reboot (port approach from user's Meshtastic project).
- Power monitoring: INA219 (single), INA3221 (3-ch) for battery/solar/load currents+voltages.
- Eco/digi sleep modes reference: CA2RXU eco-digi concept.

## Energy configuration
- Thresholds: low-voltage entry, wake/recovery hysteresis, minimum charge. Per battery chemistry — configurable, NOT hardcoded per variant.
- Deep sleep must keep config + wake timers/RTC usable on nRF52840.

## Criticals
- Must NOT let brownout corrupt config or leave node bricked.
- Known bugs: none yet (port from Meshtastic will be reviewed for nRF52840 specifics).
