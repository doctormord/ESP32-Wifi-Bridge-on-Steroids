/*
 * compat.h - Minimale Ersatzschicht fuer die paar Arduino-Helfer,
 *            die der Code benutzt.
 *
 * Bewusst KEINE Arduino-Emulation: nur millis() und delay(), weil die im
 * Code an vielen Stellen vorkommen und eine Umschreibung nur Fehlerquellen
 * ohne Nutzen erzeugen wuerde. Alles andere (Serial, pinMode, ESP.*) ist
 * direkt durch die IDF-Entsprechungen ersetzt.
 */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_system.h"

/* esp_timer laeuft in Mikrosekunden seit dem Start und ist 64 Bit breit.
 * Der Zuschnitt auf 32 Bit haelt das Verhalten identisch zu Arduinos
 * millis() - inklusive Ueberlauf nach rund 49 Tagen. Alle Vergleiche im
 * Code sind als Differenzen geschrieben und damit ueberlaufsicher. */
static inline uint32_t millis(void) {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static inline void delay(uint32_t ms) {
  vTaskDelay(pdMS_TO_TICKS(ms));
}
