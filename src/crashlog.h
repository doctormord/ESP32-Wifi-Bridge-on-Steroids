/*
 * crashlog.h - Absturz-Historie mit Zeitstempel
 *
 * Die Coredump-Partition fasst genau EIN Abbild, und ohne Uhr wusste niemand,
 * WANN es entstand - das liess sich bisher nur ueber die Laufzeit des
 * Folge-Boots zurueckrechnen, und auch das nur bis zum naechsten Neustart.
 *
 * Deshalb:
 *  - Uhrzeit per SNTP (Gateway, ersatzweise pool.ntp.org). Die Systemzeit
 *    ueberlebt Soft-Neustarts und Abstuerze (RTC-Timer), nur das Abschalten
 *    der Versorgung nicht.
 *  - Ein Herzschlag im RTC-Speicher (jede Sekunde Uhrzeit + Laufzeit + Build)
 *    ueberlebt den Absturz und sagt dem naechsten Boot, wann es passierte.
 *  - Beim ersten Boot nach einem unerwarteten Reset (Panic, Interrupt-/RTC-
 *    Watchdog, Brownout) wird ein kompakter Eintrag im NVS abgelegt -
 *    inklusive der Kerndaten aus dem Coredump, falls einer entstanden ist.
 *    Die letzten CRASHLOG_N bleiben erhalten.
 */
#pragma once

#include "compat.h"

#define CRASHLOG_N 8

#define CRASH_F_DUMP      0x01u   /* task/pc/bt/reason aus dem Coredump gueltig */
#define CRASH_F_WATCHDOG  0x02u   /* eigener Watchdog-Neustart (abort), kein Fehler */
#define CRASH_F_IMPORTED  0x04u   /* vor Einfuehrung der Historie entstanden */
#define CRASH_F_BT_CORRUPT 0x08u  /* Backtrace laut Coredump unvollstaendig */
#define CRASH_F_STALE_DUMP 0x10u  /* Panic, aber kein neues Abbild geschrieben */

typedef struct {
  uint32_t epoch;        /* Absturzzeit UTC, 0 = unbekannt (Uhr nicht gestellt) */
  uint32_t uptime_s;     /* Laufzeit des abgestuerzten Boots, 0xFFFFFFFF = unbekannt */
  uint8_t  rst;          /* esp_reset_reason_t, 0xFF = unbekannt (Import) */
  uint8_t  flags;        /* CRASH_F_* */
  uint8_t  bt_n;
  uint8_t  _pad;
  char     build[12];    /* GIT_REV des abgestuerzten Images */
  char     task[16];
  uint32_t pc;
  uint32_t bt[8];
  uint32_t dump_fp;      /* Fingerabdruck (CRC) des Coredumps - erkennt Altbestand */
  char     reason[96];
  char     elf_sha[12];  /* Anfang des ELF-SHA256 aus dem Coredump */
} crash_rec_t;

/* Einmal frueh in bridge_setup(), nach nvs_flash_init(). watchdog_neustart =
 * Ergebnis von bridge_consume_planned_restart(). Startet auch den Herzschlag. */
void crashlog_boot(bool watchdog_neustart);

/* SNTP starten, sobald das Management-Netif steht (Bridge-Modus). */
void crashlog_time_start(const uint8_t gw[4]);

/* Aktuelle Uhrzeit (UTC, Unix-Sekunden), 0 = noch nicht gestellt. */
uint32_t crashlog_now(void);

/* Eintraege, neueste zuerst. */
size_t crashlog_get(crash_rec_t *out, size_t max);

void crashlog_clear(void);
