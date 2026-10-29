/*
 * crashlog.cpp - Timestamped crash history (see crashlog.h)
 */
#include "crashlog.h"
#include "build_info.h"

#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

/* From this point on the clock counts as set (2023-11). Before the first SNTP
 * sync after power-on it counts from 1970. */
#define EPOCH_VALID 1700000000u

/* ---------------------------------------------------------------------------
 * Heartbeat in RTC memory
 *
 * Updated every second from an esp_timer, NOT from bridge_loop(): the main
 * loop is blocked for tens of seconds at boot (MAC sniff, Wi-Fi connect), so
 * the timestamp would be correspondingly stale. After a crash the next boot
 * reads here when it happened.
 * ------------------------------------------------------------------------ */
#define HB_MAGIC 0x48425431u   /* "HBT1" */
typedef struct {
  uint32_t magic;
  uint32_t epoch;
  uint32_t uptime_s;
  char     build[12];
} heartbeat_t;
RTC_NOINIT_ATTR static heartbeat_t s_hb;

/* esp_timer callback: refresh time and uptime in the heartbeat. */
static void hb_tick(void *arg) {
  (void)arg;
  s_hb.epoch    = crashlog_now();
  s_hb.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
}

/* ---------------------------------------------------------------------------
 * History in NVS - one blob, newest first. Own namespace, so a factory reset
 * of the configuration does not wipe it.
 * ------------------------------------------------------------------------ */
#define LOG_NS    "crashlog"
#define LOG_KEY   "log"
#define LOG_MAGIC 0x43524C31u   /* "CRL1" - bump when the layout changes */
typedef struct {
  uint32_t    magic;
  uint32_t    n;
  crash_rec_t r[CRASHLOG_N];
} log_blob_t;

static log_blob_t s_log;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* Load the history blob from NVS into s_log.
 * Returns false if the blob is missing (never written) or unusable; s_log is
 * then empty. */
static bool log_load(void) {
  memset(&s_log, 0, sizeof(s_log));
  nvs_handle_t h;
  if (nvs_open(LOG_NS, NVS_READONLY, &h) != ESP_OK) return false;
  size_t len = sizeof(s_log);
  esp_err_t e = nvs_get_blob(h, LOG_KEY, &s_log, &len);
  nvs_close(h);
  if (e != ESP_OK || len != sizeof(s_log) || s_log.magic != LOG_MAGIC || s_log.n > CRASHLOG_N) {
    memset(&s_log, 0, sizeof(s_log));
    return false;
  }
  return true;
}

/* Write s_log to NVS. Errors are ignored - the history is best effort. */
static void log_save(void) {
  s_log.magic = LOG_MAGIC;
  nvs_handle_t h;
  if (nvs_open(LOG_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_blob(h, LOG_KEY, &s_log, sizeof(s_log));
  nvs_commit(h);
  nvs_close(h);
}

/* Insert a record at the front, dropping the oldest one if the log is full. */
static void log_prepend(const crash_rec_t *r) {
  memmove(&s_log.r[1], &s_log.r[0], sizeof(crash_rec_t) * (CRASHLOG_N - 1));
  s_log.r[0] = *r;
  if (s_log.n < CRASHLOG_N) s_log.n++;
}

/* Copy the key data of the coredump into the record (task, PC, backtrace,
 * panic reason, ELF SHA, fingerprint). Returns false if there is no readable
 * image. */
static bool dump_read(crash_rec_t *r) {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
  size_t addr = 0, size = 0;
  if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size < 8) return false;

  /* The last 4 bytes of the image are its CRC32 - good enough as a
   * fingerprint to recognize "the same old dump as last time". */
  uint32_t fp = 0;
  if (esp_flash_read(NULL, &fp, addr + size - 4, 4) != ESP_OK) return false;
  r->dump_fp = fp;

  esp_core_dump_summary_t *sum = (esp_core_dump_summary_t *)calloc(1, sizeof(*sum));
  if (!sum) return false;
  bool ok = esp_core_dump_get_summary(sum) == ESP_OK;
  if (ok) {
    strlcpy(r->task, sum->exc_task, sizeof(r->task));
    r->pc   = sum->exc_pc;
    r->bt_n = 0;
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    for (uint32_t i = 0; i < sum->exc_bt_info.depth && r->bt_n < 8; i++)
      r->bt[r->bt_n++] = sum->exc_bt_info.bt[i];
    if (sum->exc_bt_info.corrupted) r->flags |= CRASH_F_BT_CORRUPT;
#else
    /* RISC-V has no on-device backtrace (see h_coredump_get() in web.cpp);
     * the return address is the best single call-site hint. */
    r->bt[r->bt_n++] = sum->ex_info.ra;
#endif
    strlcpy(r->elf_sha, (const char *)sum->app_elf_sha256, sizeof(r->elf_sha));
    esp_core_dump_get_panic_reason(r->reason, sizeof(r->reason));
    r->flags |= CRASH_F_DUMP;
  }
  free(sum);
  return ok;
#else
  (void)r;
  return false;
#endif
}

/* See crashlog.h.
 *
 * Saves the previous boot's heartbeat before the new one overwrites it,
 * starts the 1 s heartbeat timer, loads the history, and - if the reset
 * reason indicates a crash - prepends a record built from the old heartbeat
 * and (for panics) the coredump. On the very first boot with this feature an
 * existing coredump is imported once, with unknown time. */
/* Set by crashlog_boot(), read by the status API. */
static uint8_t s_boot_rst = 0;
static bool    s_rtc_retained = false;

/* See crashlog.h. */
uint8_t crashlog_reset_reason(void) { return s_boot_rst; }
bool    crashlog_rtc_retained(void) { return s_rtc_retained; }

/* See crashlog.h. */
void crashlog_boot(bool watchdog_restart) {
  /* Save the previous boot's heartbeat before the new boot overwrites it. */
  const heartbeat_t prev = s_hb;
  const bool prev_valid = (prev.magic == HB_MAGIC);

  s_hb.magic = HB_MAGIC;
  strlcpy(s_hb.build, GIT_REV, sizeof(s_hb.build));
  hb_tick(NULL);

  const esp_timer_create_args_t ta = { .callback = hb_tick, .arg = NULL,
                                       .dispatch_method = ESP_TIMER_TASK,
                                       .name = "crash_hb", .skip_unhandled_events = true };
  esp_timer_handle_t t;
  if (esp_timer_create(&ta, &t) == ESP_OK) esp_timer_start_periodic(t, 1000000);

  const bool exists = log_load();
  const esp_reset_reason_t rr = esp_reset_reason();
  s_boot_rst = (uint8_t)rr;
  s_rtc_retained = prev_valid;
  printf("[BOOT] Reset reason %d, RTC data from previous boot %s\n",
         (int)rr, prev_valid ? "retained" : "not retained");

  /* Every reset that is a fault, not a deliberate or external action. A CPU
   * lockup (RISC-V double exception) and a power glitch are as much a crash
   * as a panic or a brownout, even though no coredump is written for them. */
  const bool crashed = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
                       rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT ||
                       rr == ESP_RST_BROWNOUT || rr == ESP_RST_CPU_LOCKUP ||
                       rr == ESP_RST_PWR_GLITCH;
  if (crashed) {
    crash_rec_t r = {};
    r.rst      = (uint8_t)rr;
    r.epoch    = prev_valid && prev.epoch >= EPOCH_VALID ? prev.epoch : 0;
    r.uptime_s = prev_valid ? prev.uptime_s : 0xFFFFFFFFu;
    strlcpy(r.build, prev_valid ? prev.build : "?", sizeof(r.build));
    if (watchdog_restart) r.flags |= CRASH_F_WATCHDOG;

    if (rr == ESP_RST_PANIC) {
      /* A panic should have left a fresh image. If it did not (writing
       * failed), the dump of an EARLIER crash is still there - its data must
       * not be attributed to this one. Recognizable by the unchanged
       * fingerprint. */
      crash_rec_t d = {};
      if (dump_read(&d)) {
        bool stale_dump = false;
        for (uint32_t i = 0; i < s_log.n; i++)
          if ((s_log.r[i].flags & CRASH_F_DUMP) && s_log.r[i].dump_fp == d.dump_fp) stale_dump = true;
        if (stale_dump) {
          r.flags |= CRASH_F_STALE_DUMP;
        } else {
          memcpy(r.task, d.task, sizeof(r.task));
          r.pc = d.pc; r.bt_n = d.bt_n; memcpy(r.bt, d.bt, sizeof(r.bt));
          r.dump_fp = d.dump_fp;
          memcpy(r.reason, d.reason, sizeof(r.reason));
          memcpy(r.elf_sha, d.elf_sha, sizeof(r.elf_sha));
          r.flags |= d.flags;
        }
      } else {
        r.flags |= CRASH_F_STALE_DUMP;
      }
      /* No new dump and no heartbeat of the previous boot: that boot did not
       * get as far as the first heartbeat (~1 s), and the panic left nothing
       * behind. That is what a supply that bounces while being plugged in
       * produces - the chip starts, faults in the voltage dip and restarts
       * itself, so this boot reports a panic instead of a power-on. */
      if ((r.flags & CRASH_F_STALE_DUMP) && !prev_valid) r.flags |= CRASH_F_SUPPLY;
    }
    /* Repeated supply glitches of the same build would push every real
     * crash out of the short history: count them in the newest entry. */
    crash_rec_t *top = s_log.n ? &s_log.r[0] : NULL;
    if ((r.flags & CRASH_F_SUPPLY) && top && (top->flags & CRASH_F_SUPPLY) &&
        top->rst == r.rst && strcmp(top->build, r.build) == 0) {
      if (top->repeat < 255) top->repeat++;
      if (r.epoch) top->epoch = r.epoch;
    } else {
      log_prepend(&r);
    }
    log_save();
    printf("[CRSH] Unexpected reset (reason %d%s) recorded\n", (int)rr,
           (r.flags & CRASH_F_SUPPLY) ? ", probably a supply glitch at power-on" : "");
  } else if (!exists) {
    /* Very first boot with the history: an existing coredump predates it -
     * import it once, time unknown. */
    crash_rec_t r = {};
    if (dump_read(&r)) {
      r.rst      = 0xFF;
      r.uptime_s = 0xFFFFFFFFu;
      r.flags   |= CRASH_F_IMPORTED;
      strlcpy(r.build, "?", sizeof(r.build));
      log_prepend(&r);
      printf("[CRSH] Existing coredump imported into the history\n");
    }
    log_save();   /* even when empty - marks "import done" */
  }
}

/* See crashlog.h. Only the first call has an effect. */
void crashlog_time_start(const uint8_t gw[4]) {
  static bool started = false;
  static char gw_str[16];
  if (started) return;
  started = true;

  snprintf(gw_str, sizeof(gw_str), "%u.%u.%u.%u", gw[0], gw[1], gw[2], gw[3]);
  /* The gateway first (many home routers act as a time server, and it needs
   * no DNS), the public pool as a fallback. */
  esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
                          ESP_SNTP_SERVER_LIST(gw_str, "pool.ntp.org"));
  c.wait_for_sync = false;
  if (esp_netif_sntp_init(&c) != ESP_OK) {
    printf("[TIME] SNTP start failed\n");
    return;
  }
  printf("[TIME] SNTP started (%s, pool.ntp.org)\n", gw_str);
}

/* See crashlog.h. Times before EPOCH_VALID mean "not synced yet". */
uint32_t crashlog_now(void) {
  const time_t t = time(NULL);
  return t >= (time_t)EPOCH_VALID ? (uint32_t)t : 0;
}

/* See crashlog.h. */
size_t crashlog_get(crash_rec_t *out, size_t max) {
  portENTER_CRITICAL(&s_mux);
  size_t n = s_log.n < max ? s_log.n : max;
  memcpy(out, s_log.r, n * sizeof(crash_rec_t));
  portEXIT_CRITICAL(&s_mux);
  return n;
}

/* See crashlog.h. */
void crashlog_clear(void) {
  portENTER_CRITICAL(&s_mux);
  s_log.n = 0;
  memset(s_log.r, 0, sizeof(s_log.r));
  portEXIT_CRITICAL(&s_mux);
  log_save();
}
