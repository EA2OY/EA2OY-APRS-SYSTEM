// diag.h — real-time diagnostics stream over USB (runtime only, never
// persisted). When enabled, the node emits one JSON snapshot per second with
// everything it is doing (GPS, tracker, power, radio, sensors, UI) plus
// event lines for each received/transmitted frame. Optional raw NMEA echo.
// It never changes the node behaviour and it is automatically muted while the
// USB TNC bridge is active (the TNC stream must stay clean).
// License: GPL-3.0
#pragma once

#include <Arduino.h>

#include "config.h"

// Bind the live config (call once at boot).
void diagBindConfig(DigiConfig *cfg);

// Runtime toggles (OFF at every boot).
void diagSetActive(bool on);
bool diagActive();
void diagSetNmea(bool on);
bool diagNmea();

// Effective stream state: enabled by the user and not in TNC mode.
bool diagStreaming();

// Call from loop(): emits the 1 Hz snapshot when streaming.
void diagLoop();

// Event hooks (no-op unless streaming).
void diagRxFrame(const char *from, const char *info, float rssi, float snr);
void diagTxFrame(const char *frame, size_t len, int code);
void diagNote(const char *what);
