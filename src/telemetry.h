/*
 * telemetry.h - MQTT-Telemetrie
 */
#pragma once

#include "compat.h"

/* Startet den MQTT-Client. Liefert false, wenn kein Broker konfiguriert ist. */
bool telemetry_start(void);

/* Regelmaessig aus loop() aufrufen. */
void telemetry_tick(void);
