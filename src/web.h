/*
 * web.h - Konfigurationsportal und Status-API
 */
#pragma once

#include "compat.h"

/* Access-Point fuer die Erstinbetriebnahme starten.
 * Laeuft als APSTA, damit im Portal nach Netzen gescannt werden kann. */
bool portal_start_ap(void);

/* HTTP-Server starten. Funktioniert in beiden Modi - im AP-Modus auf
 * 192.168.4.1, im Bridge-Modus auf der Management-IP. */
bool web_start(bool provisioning);

/* millis()-Zeitpunkt der letzten ECHTEN Portal-Nutzung (Seite geladen,
 * Konfiguration abgerufen/gespeichert, Scan gestartet) - bewusst NICHT bei
 * jedem /api/status-Aufruf aktualisiert, sonst wuerde ein offen gelassener
 * Browser-Tab (2s-Auto-Poll) den Portal-Idle-Neustart fuer immer verhindern.
 * Fuer main.cpp's Idle-Neustart im Provisionierungsmodus, siehe config.h:
 * ap_idle_reboot_s. 0, solange noch keine Anfrage einging. */
uint32_t web_last_activity_ms(void);
