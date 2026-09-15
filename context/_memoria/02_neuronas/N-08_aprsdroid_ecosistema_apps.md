# N-08 — Ecosistema APRS móvil: APRSdroid y funciones que usa la gente

Investigación web (agent, 2026-09-09). Fuentes: github.com/ge0rg/aprsdroid, aprsdroid.org, XRPi docs (ohiopacket.org), YAAC (ka2ddo.org), APRSISCE/32 (aprsisce.wikidot.com), aprs.org (spec 1.2, messaging, APRS101), meshtastic.org.

## APRSdroid (referencia UX, cliente)
- App Android "cliente" APRS (GPL-2.0, Scala+jni C AFSK, autor Georg Lukas DO1GL). NO es iGate/digi (estuvo "planned" y nunca se hizo). Solo cliente.
- Conexiones: (1) APRS-IS TCP bidireccional (default), UDP 8080/HTTP POST; (2) BT-TNC SPP KISS (con init string); (3) AFSK por audio; (4) radios Kenwood; (5) USB serial/TCP TNC (KISS/TNC2/monitor).
- Funciones: beacon manual/periódico/**SmartBeaconing**, posición manual, símbolo/overlay + comentario (QRG), mensajes con ACK y conversaciones, Hub view + Map view, filtros IS (neighbor radius, javAPRSfilter), log en vivo, API inter-app por Intents (Tasker), perfiles JSON export/import, notificaciones.
- NO tiene: telemetría, WX, objetos, gestión de iGates.
- Settings que configura el usuario (patrón a copiar): callsign+SSID (con guía de SSID), passcode APRS-IS (solo si internet), tipo conexión y servidor/filtros, fuente de posición e intervalos, símbolo/comentario, privacidad (ambigüedad).

## Gestión de iGates/digis HOY (hueco real en el ecosistema)
- NO existe app móvil establecida para gestionar digis/iGates. Gestión real: escritorio (YAAC: cliente+iGate+digi AX.25, telemetría, autenticación ARETF experimental; APRSISCE/32: iGate configurable, telemetría, WHO-IS, QRU; Direwolf; XRPi/XRouter con queries) o local (ficheros).
- Un iGate se "identifica": beacon/status IGATE + símbolo, PHG; responde queries si el firmware las implementa.

## Queries remotas vía mensajes APRS (fáciles de implementar, diferenciador)
Convención: mensajes terminados en `?` al nodo; dirigidas `?APRSx` sin `?` (UI-View/XRPi). Respuestas del nodo:
- `?APRS?` → beacon ID del puerto (posición+tipo).
- `?IGATE?` → estadísticas iGate (nº mensajes, nº usuarios locales).
- `?APRSD` → lista de estaciones oídas directamente.
- `?APRSM` → mensajes no entregados dirigidos al que pregunta.
- `?APRSP` → posición del nodo. `?APRSS` → versión software + aliases digi.
- `?PING?`/`?APRST` → traceroute de la petición. UI-View `~\xFE n` ping, `~\xFC n` DX heard.
- Email vía APRS: mensaje a EMAIL/EMAIL-2/SAMAIL/WLNK-1 con `SP` (primer word = dirección).
- Propuesta spec 1.2: QSY?/QSY! y FREQ-in-MSG (estación a estación).

## Recomendación de funciones para NUESTRA app (mix APRS + gestión)
A) Función APRS típica: beacon configurable (estático o SmartBeaconing con GPS), status IGATE/DIGI + frecuencia, mensaje TX test, last-heard por RF con RSSI, confirmación "estás en internet" (comprobar aprs.fi con datos del teléfono), perfiles JSON.
B) Config BLE del nodo (en vez de ficheros): callsign/SSID/passcode, servidor IS+filtros IS→RF, frecuencia LoRa/SF/BW/CR/TX, modo digi (aliases, WIDEn-n, beacon ID), posición estática, OLED/LED, sleep/eco.
C) Huecos (ventaja): asistente primer arranque post-UF2; consola CLI por BLE tipo NavaCLI (get/set/reset/status/firmware); monitor RX en vivo; telemetría gráfica (VBAT, RX/TX counters, BME/BMP) — APRSdroid no la envía (hueco); queries APRS en firmware (`?APRS?`, `?IGATE?`, `?APRSD`...); seguridad pairing BLE con PIN mostrado en OLED; Web-BLE futuro (iOS).
D) Patrón probado a copiar: Meshtastic app Android = config/admin/telemetría/**firmware update** por BLE en nRF52. El rol BLE para un nRF52 sin WiFi es exactamente ese: consola + diagnóstico + OTA/DFU + "proxy smart" (el teléfono hace de puente a internet).
