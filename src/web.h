/*
 * web.h - Configuration portal and status API
 */
#pragma once

#include "compat.h"

/* Start the access point for initial setup.
 * Runs as APSTA so the portal can scan for networks. */
bool portal_start_ap(void);

/* Start the HTTP server. Works in both modes - in AP mode on 192.168.4.1, in
 * bridge mode on the management IP. `provisioning` only changes what the
 * status API reports and whether saved tuning is applied live. */
bool web_start(bool provisioning);

/* millis() timestamp of the last REAL portal use (page loaded, configuration
 * fetched/saved, scan started) - deliberately NOT updated on every
 * /api/status call, otherwise a browser tab left open (2 s auto poll) would
 * prevent the portal idle reboot forever. Used by main.cpp's idle reboot in
 * provisioning mode, see config.h: ap_idle_reboot_s. 0 until the first
 * request. */
uint32_t web_last_activity_ms(void);
