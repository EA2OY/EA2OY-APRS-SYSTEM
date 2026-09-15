// button.h — Faketec button (P1.00, active low) state machine.
// Three gestures only (deliberately, to keep it predictable with one button):
//   short  = released before the long threshold (resolved after a short window,
//            so a double click is not mistaken for two shorts)
//   long   = fires WHILE still held when the threshold is reached (no need to
//            guess the timing or release at the right moment)
//   double = two shorts inside the window
// There is no "very long" gesture any more. Used by the UI/menu (Phase A/B).
// License: GPL-3.0

#pragma once

#include <Arduino.h>

enum ButtonEvent {
  BTN_NONE = 0,
  BTN_SHORT,
  BTN_LONG,
  BTN_DOUBLE,
};

void buttonInit();
ButtonEvent buttonPoll();  // call from loop()
