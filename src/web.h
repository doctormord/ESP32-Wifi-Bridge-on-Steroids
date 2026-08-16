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
