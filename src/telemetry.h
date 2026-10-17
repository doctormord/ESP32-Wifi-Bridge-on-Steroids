/*
 * telemetry.h - MQTT telemetry
 */
#pragma once

#include "compat.h"

/* Start the MQTT client. Returns false if no broker is configured. */
bool telemetry_start(void);

/* Call periodically from the main loop; publishes the state every
 * telemetry_s seconds while connected. */
void telemetry_tick(void);

/* Publish a single event immediately instead of waiting for the next
 * telemetry_tick() - used by the watchdog (bridge.cpp) so that an escalation
 * step shows up in Home Assistant/MQTT with a timestamp, rather than only as
 * a silently increased counter in the next regular state message. No
 * discovery entry for it (it is a log event, not a sensor state) - silently
 * a no-op if no broker is connected.
 * action: short code, e.g. "eth_reset", "wifi_reconnect", "reboot".
 * reason: short code, e.g. "loss", "disconnects", "probe", "eth_stall",
 *         or "" if not applicable. */
void telemetry_note_watchdog_event(const char *action, const char *reason);
