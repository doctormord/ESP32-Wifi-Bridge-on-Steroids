/*
 * telemetry.h - MQTT-Telemetrie
 */
#pragma once

#include "compat.h"

/* Startet den MQTT-Client. Liefert false, wenn kein Broker konfiguriert ist. */
bool telemetry_start(void);

/* Regelmaessig aus loop() aufrufen. */
void telemetry_tick(void);

/* Einzelnes Ereignis sofort veroeffentlichen, statt auf den naechsten
 * telemetry_tick() zu warten - fuer den Watchdog (bridge.cpp), damit ein
 * Eskalationsschritt in Home Assistant/MQTT mit Zeitstempel auftaucht, nicht
 * erst als stiller Zaehlerstand beim naechsten regulaeren Status. Kein
 * Discovery-Eintrag dafuer (kein Sensor-State, sondern ein Log-Ereignis) -
 * still ein No-Op, wenn kein Broker verbunden ist.
 * action: kurzer Code, z.B. "eth_reset", "wifi_reconnect", "reboot".
 * reason: kurzer Code, z.B. "verlust", "disconnects", "sonde", "eth_stall",
 *         oder "" wenn nicht zutreffend. */
void telemetry_note_watchdog_event(const char *action, const char *reason);
