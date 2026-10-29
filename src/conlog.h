/*
 * conlog.h - In-memory copy of the console output for the web portal
 *
 * The board runs without a serial connection once deployed, and on targets
 * with a native USB console the log is only visible while a host has the port
 * open. conlog keeps the most recent CONLOG_SIZE bytes of everything printed
 * to stdout (our own printf output and ESP_LOG lines) in a ring buffer, so
 * the portal can show it over the network (GET /api/log). The normal console
 * output is unchanged: every write is forwarded to the original stdout.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define CONLOG_SIZE      8192u   /* must be a power of two */
#define CONLOG_PREV_SIZE 2048u   /* tail of the previous boot kept in NVS */

/* Redirect stdout through the ring buffer. Call as the very first thing in
 * app_main(): tasks inherit stdout from the global reent structure when they
 * are created, so only tasks created after this call are covered. Before the
 * ring is restarted, the tail of the previous boot's output is taken from it
 * (the ring is in .noinit RAM and survives every reset but a power loss). */
void conlog_init(void);

/* Store the tail taken by conlog_init() in NVS, together with the reset
 * reason that ended that boot. Call once after nvs_flash_init(); does
 * nothing if there was no previous log (e.g. after power-on). */
void conlog_persist_prev(void);

/* Copy the stored previous-boot log into 'out' (at most 'max' bytes, the end
 * of the log if it is longer) and return its length; '*rst' receives the
 * esp_reset_reason_t that ended that boot. Returns 0 if nothing is stored. */
size_t conlog_get_prev(char *out, size_t max, uint8_t *rst);

/* Copy console output starting at byte position 'since' into 'out' (at most
 * 'max' bytes, not NUL-terminated) and return the number of bytes copied.
 * '*next' receives the position to pass as 'since' on the next call. If
 * 'since' is older than the buffer holds, or ahead of what was ever written
 * (the board rebooted since the caller's last read), reading restarts at the
 * oldest byte still available and '*reset' is set. */
size_t conlog_read(uint32_t since, char *out, size_t max, uint32_t *next, bool *reset);
