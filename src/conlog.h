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

#define CONLOG_SIZE 8192u   /* must be a power of two */

/* Redirect stdout through the ring buffer. Call as the very first thing in
 * app_main(): tasks inherit stdout from the global reent structure when they
 * are created, so only tasks created after this call are covered. */
void conlog_init(void);

/* Copy console output starting at byte position 'since' into 'out' (at most
 * 'max' bytes, not NUL-terminated) and return the number of bytes copied.
 * '*next' receives the position to pass as 'since' on the next call. If
 * 'since' is older than the buffer holds, or ahead of what was ever written
 * (the board rebooted since the caller's last read), reading restarts at the
 * oldest byte still available and '*reset' is set. */
size_t conlog_read(uint32_t since, char *out, size_t max, uint32_t *next, bool *reset);
