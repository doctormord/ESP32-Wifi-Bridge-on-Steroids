/*
 * crashlog.cpp - Absturz-Historie mit Zeitstempel (siehe crashlog.h)
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

/* Ab hier gilt die Uhr als gestellt (2023-11). Vor dem ersten SNTP-Abgleich
 * nach dem Einschalten zaehlt sie ab 1970. */
#define EPOCH_GUELTIG 1700000000u

/* ---------------------------------------------------------------------------
 * Herzschlag im RTC-Speicher
 *
 * Jede Sekunde aus einem esp_timer heraus aktualisiert, NICHT aus
 * bridge_loop(): die Hauptschleife steht beim Start teils zig Sekunden
 * (MAC-Sniff, WLAN-Verbindungsaufbau), der Zeitstempel waere dann entsprechend
 * alt. Nach einem Absturz liest der naechste Boot hier ab, wann es passierte.
 * ------------------------------------------------------------------------ */
#define HB_MAGIC 0x48425431u   /* "HBT1" */
typedef struct {
  uint32_t magic;
  uint32_t epoch;
  uint32_t uptime_s;
  char     build[12];
} herzschlag_t;
RTC_NOINIT_ATTR static herzschlag_t s_hb;

static void hb_tick(void *arg) {
  (void)arg;
  s_hb.epoch    = crashlog_now();
  s_hb.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
}

/* ---------------------------------------------------------------------------
 * Historie im NVS - ein Blob, neueste zuerst. Eigener Namespace, damit sie
 * ein Werksreset der Konfiguration nicht mitnimmt.
 * ------------------------------------------------------------------------ */
#define LOG_NS    "crashlog"
#define LOG_KEY   "log"
#define LOG_MAGIC 0x43524C31u   /* "CRL1" - bei Layoutaenderung hochzaehlen */
typedef struct {
  uint32_t    magic;
  uint32_t    n;
  crash_rec_t r[CRASHLOG_N];
} log_blob_t;

static log_blob_t s_log;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* false = Blob fehlt (noch nie geschrieben) oder ist unbrauchbar. */
static bool log_laden(void) {
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

static void log_speichern(void) {
  s_log.magic = LOG_MAGIC;
  nvs_handle_t h;
  if (nvs_open(LOG_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_blob(h, LOG_KEY, &s_log, sizeof(s_log));
  nvs_commit(h);
  nvs_close(h);
}

static void log_voranstellen(const crash_rec_t *r) {
  memmove(&s_log.r[1], &s_log.r[0], sizeof(crash_rec_t) * (CRASHLOG_N - 1));
  s_log.r[0] = *r;
  if (s_log.n < CRASHLOG_N) s_log.n++;
}

/* Coredump-Kerndaten in den Eintrag uebernehmen. Liefert false, wenn kein
 * lesbares Abbild da ist. */
static bool dump_lesen(crash_rec_t *r) {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
  size_t addr = 0, size = 0;
  if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size < 8) return false;

  /* Die letzten 4 Byte des Abbilds sind seine CRC32 - als Fingerabdruck gut
   * genug, um "derselbe alte Dump wie beim letzten Mal" zu erkennen. */
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
    for (uint32_t i = 0; i < sum->exc_bt_info.depth && r->bt_n < 8; i++)
      r->bt[r->bt_n++] = sum->exc_bt_info.bt[i];
    if (sum->exc_bt_info.corrupted) r->flags |= CRASH_F_BT_CORRUPT;
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

void crashlog_boot(bool watchdog_neustart) {
  /* Herzschlag des vorigen Boots sichern, bevor der neue ihn ueberschreibt. */
  const herzschlag_t alt = s_hb;
  const bool alt_gueltig = (alt.magic == HB_MAGIC);

  s_hb.magic = HB_MAGIC;
  strlcpy(s_hb.build, GIT_REV, sizeof(s_hb.build));
  hb_tick(NULL);

  const esp_timer_create_args_t ta = { .callback = hb_tick, .arg = NULL,
                                       .dispatch_method = ESP_TIMER_TASK,
                                       .name = "crash_hb", .skip_unhandled_events = true };
  esp_timer_handle_t t;
  if (esp_timer_create(&ta, &t) == ESP_OK) esp_timer_start_periodic(t, 1000000);

  const bool vorhanden = log_laden();
  const esp_reset_reason_t rr = esp_reset_reason();

  const bool absturz = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
                       rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT ||
                       rr == ESP_RST_BROWNOUT;
  if (absturz) {
    crash_rec_t r = {};
    r.rst      = (uint8_t)rr;
    r.epoch    = alt_gueltig && alt.epoch >= EPOCH_GUELTIG ? alt.epoch : 0;
    r.uptime_s = alt_gueltig ? alt.uptime_s : 0xFFFFFFFFu;
    strlcpy(r.build, alt_gueltig ? alt.build : "?", sizeof(r.build));
    if (watchdog_neustart) r.flags |= CRASH_F_WATCHDOG;

    if (rr == ESP_RST_PANIC) {
      /* Ein Panic sollte ein frisches Abbild hinterlassen haben. Hat er nicht
       * (Schreiben gescheitert), liegt dort noch der Dump eines FRUEHEREN
       * Absturzes - dessen Daten duerfen nicht diesem hier zugeschrieben
       * werden. Erkennbar am unveraenderten Fingerabdruck. */
      crash_rec_t d = {};
      if (dump_lesen(&d)) {
        bool alt_dump = false;
        for (uint32_t i = 0; i < s_log.n; i++)
          if ((s_log.r[i].flags & CRASH_F_DUMP) && s_log.r[i].dump_fp == d.dump_fp) alt_dump = true;
        if (alt_dump) {
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
    }
    log_voranstellen(&r);
    log_speichern();
    printf("[CRSH] Unerwarteter Reset (Grund %d) protokolliert\n", (int)rr);
  } else if (!vorhanden) {
    /* Allererster Start mit Historie: ein schon vorhandener Coredump stammt
     * aus der Zeit davor - einmalig uebernehmen, Zeitpunkt unbekannt. */
    crash_rec_t r = {};
    if (dump_lesen(&r)) {
      r.rst      = 0xFF;
      r.uptime_s = 0xFFFFFFFFu;
      r.flags   |= CRASH_F_IMPORTED;
      strlcpy(r.build, "?", sizeof(r.build));
      log_voranstellen(&r);
      printf("[CRSH] Vorhandenen Coredump in die Historie uebernommen\n");
    }
    log_speichern();   /* auch leer - markiert "Import erledigt" */
  }
}

void crashlog_time_start(const uint8_t gw[4]) {
  static bool gestartet = false;
  static char gw_str[16];
  if (gestartet) return;
  gestartet = true;

  snprintf(gw_str, sizeof(gw_str), "%u.%u.%u.%u", gw[0], gw[1], gw[2], gw[3]);
  /* Erst das Gateway (die FritzBox ist von Haus aus Zeitserver, und es
   * braucht kein DNS), ersatzweise der oeffentliche Pool. */
  esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
                          ESP_SNTP_SERVER_LIST(gw_str, "pool.ntp.org"));
  c.wait_for_sync = false;
  if (esp_netif_sntp_init(&c) != ESP_OK) {
    printf("[TIME] SNTP-Start fehlgeschlagen\n");
    return;
  }
  printf("[TIME] SNTP gestartet (%s, pool.ntp.org)\n", gw_str);
}

uint32_t crashlog_now(void) {
  const time_t t = time(NULL);
  return t >= (time_t)EPOCH_GUELTIG ? (uint32_t)t : 0;
}

size_t crashlog_get(crash_rec_t *out, size_t max) {
  portENTER_CRITICAL(&s_mux);
  size_t n = s_log.n < max ? s_log.n : max;
  memcpy(out, s_log.r, n * sizeof(crash_rec_t));
  portEXIT_CRITICAL(&s_mux);
  return n;
}

void crashlog_clear(void) {
  portENTER_CRITICAL(&s_mux);
  s_log.n = 0;
  memset(s_log.r, 0, sizeof(s_log.r));
  portEXIT_CRITICAL(&s_mux);
  log_speichern();
}
