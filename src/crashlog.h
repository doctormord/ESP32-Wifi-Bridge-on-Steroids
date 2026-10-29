/*
 * crashlog.h - Timestamped crash history
 *
 * The coredump partition holds exactly ONE image, and without a clock nobody
 * knows WHEN it was written. Therefore:
 *  - Wall-clock time via SNTP (gateway first, pool.ntp.org as fallback). The
 *    system time survives soft reboots and crashes (RTC timer), only a power
 *    loss resets it.
 *  - A heartbeat in RTC memory (time + uptime + build, every second) survives
 *    the crash and tells the next boot when it happened.
 *  - On the first boot after an unexpected reset (panic, interrupt/RTC
 *    watchdog, brownout) a compact record is stored in NVS - including the
 *    key data from the coredump, if one was written. The last CRASHLOG_N
 *    records are kept.
 */
#pragma once

#include "compat.h"

#define CRASHLOG_N 8

#define CRASH_F_DUMP      0x01u   /* task/pc/bt/reason from the coredump are valid */
#define CRASH_F_WATCHDOG  0x02u   /* our own watchdog reboot (abort), not a fault */
#define CRASH_F_IMPORTED  0x04u   /* written before the history existed */
#define CRASH_F_BT_CORRUPT 0x08u  /* backtrace incomplete according to the coredump */
#define CRASH_F_STALE_DUMP 0x10u  /* panic, but no new image was written */
#define CRASH_F_SUPPLY    0x20u   /* panic without a new dump and without the RTC
                                   * heartbeat: most likely the supply bouncing
                                   * at power-on, not a firmware fault */

typedef struct {
  uint32_t epoch;        /* crash time UTC, 0 = unknown (clock not set) */
  uint32_t uptime_s;     /* uptime of the crashed boot, 0xFFFFFFFF = unknown */
  uint8_t  rst;          /* esp_reset_reason_t, 0xFF = unknown (import) */
  uint8_t  flags;        /* CRASH_F_* */
  uint8_t  bt_n;
  uint8_t  repeat;       /* further identical CRASH_F_SUPPLY resets merged into
                          * this entry (see crashlog_boot()) */
  char     build[12];    /* GIT_REV of the crashed image */
  char     task[16];
  uint32_t pc;
  uint32_t bt[8];
  uint32_t dump_fp;      /* fingerprint (CRC) of the coredump - detects stale dumps */
  char     reason[96];
  char     elf_sha[12];  /* prefix of the ELF SHA256 from the coredump */
} crash_rec_t;

/* Call once early in bridge_setup(), after nvs_flash_init().
 * watchdog_restart = result of bridge_consume_planned_restart(). Records a
 * crash of the previous boot if there was one, and starts the heartbeat. */
void crashlog_boot(bool watchdog_restart);

/* Reset reason of the current boot (esp_reset_reason_t), and whether the
 * previous boot's RTC heartbeat was found intact. The latter shows whether
 * RTC_NOINIT memory survived the reset - all RTC-based diagnostics (this
 * heartbeat, the Wi-Fi disconnect log, the planned-restart marker) depend on
 * it. Both are valid after crashlog_boot(). */
uint8_t crashlog_reset_reason(void);
bool    crashlog_rtc_retained(void);

/* Start SNTP once the management netif is up (bridge mode). */
void crashlog_time_start(const uint8_t gw[4]);

/* Current time (UTC, Unix seconds), 0 = not set yet. */
uint32_t crashlog_now(void);

/* Copy up to `max` records into `out`, newest first. Returns the count. */
size_t crashlog_get(crash_rec_t *out, size_t max);

/* Erase the crash history (RAM and NVS). */
void crashlog_clear(void);
