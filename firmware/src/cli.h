// cli.h — NavaCLI-style token commands, shared by USB and RF-remote (N-10).
// Line NOT starting with '{' on USB-CDC is token mode (text replies); the RF
// remote-control path uses the same engine (viaRemote=true).
// Destructive policy (operator-approved):
//   reboot / reset  -> allowed (remote too) but require "confirm"; both do a
//                      SOFT reboot, config intact ("reset" used to wipe: it no
//                      longer does, see factory_reset below)
//   factory_reset   -> factory defaults PRESERVING callsign+managers+remoteEnabled
//   wipe            -> USB only + confirm (full erase; node unreachable after)
// License: GPL-3.0

#pragma once

#include <Arduino.h>

#include "config.h"

// Execute a token command line. Returns the text reply (may be multi-line).
String cliExecute(DigiConfig &cfg, const char *line, bool viaRemote);

// Envoltorio pubico del motor de tipeado/validacion de valor por clave (typedSet es
// interno a cli.cpp). Lo usa el menu en pantalla de la tinta electronica: valida el
// valor con el MISMO criterio que `set <clave> <valor>` y lo deja en cfg. Devuelve
// true si entro bien. La persistencia a flash (storeSave) la hace quien llama.
bool cliTypedSet(DigiConfig &cfg, const String &key, const String &val, String &errOut);
