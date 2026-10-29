/*
 * cpustat.cpp - CPU load per task, see cpustat.h
 */
#include "cpustat.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "compat.h"

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

#define CPUSTAT_MAX_TASKS 32     /* snapshot capacity; more tasks are ignored */

typedef struct {
  char     name[configMAX_TASK_NAME_LEN];
  uint16_t pct_x10;              /* share of the window in 0.1 % */
} cpu_task_t;

typedef struct {
  bool       valid;
  uint32_t   at_s;               /* uptime at the end of the window */
  uint16_t   idle_x10;           /* idle share in 0.1 % */
  uint32_t   kbps_e2w, kbps_w2e;
  uint8_t    n;
  cpu_task_t top[CPUSTAT_TOP];
} cpu_window_t;

static TaskStatus_t s_prev[CPUSTAT_MAX_TASKS];
static UBaseType_t  s_prev_n = 0;
static configRUN_TIME_COUNTER_TYPE s_prev_total = 0;
static TaskStatus_t s_cur[CPUSTAT_MAX_TASKS];

static cpu_window_t s_last = {};
static cpu_window_t s_peak = {};

/* Run-time counter of 'h' in the previous snapshot, or 'fallback' for a task
 * created during the window (counted from zero then). */
static uint32_t prev_runtime(TaskHandle_t h, uint32_t fallback) {
  for (UBaseType_t i = 0; i < s_prev_n; i++)
    if (s_prev[i].xHandle == h) return (uint32_t)s_prev[i].ulRunTimeCounter;
  return fallback;
}

/* See cpustat.h. Takes a snapshot of all tasks once per window, turns the
 * counter deltas into shares of the elapsed run time (single core: the
 * shares of all tasks add up to 100 %), keeps the CPUSTAT_TOP busiest tasks
 * and updates the peak window. Runs in the main-loop task; the snapshot
 * arrays are static so they cost no stack. */
void cpustat_tick(uint64_t bytes_e2w, uint64_t bytes_w2e) {
  static uint32_t last_ms = 0;
  static uint64_t last_e = 0, last_w = 0;
  const uint32_t now = millis();
  if (last_ms && now - last_ms < CPUSTAT_WINDOW_MS) return;

  configRUN_TIME_COUNTER_TYPE total = 0;
  const UBaseType_t n = uxTaskGetSystemState(s_cur, CPUSTAT_MAX_TASKS, &total);
  const uint32_t dt_ms = now - last_ms;
  const uint32_t dtotal = (uint32_t)(total - s_prev_total);

  if (last_ms && n && dtotal) {
    cpu_window_t w = {};
    w.valid = true;
    w.at_s = now / 1000;
    w.kbps_e2w = (uint32_t)(((bytes_e2w - last_e) * 8ULL) / (dt_ms ? dt_ms : 1));
    w.kbps_w2e = (uint32_t)(((bytes_w2e - last_w) * 8ULL) / (dt_ms ? dt_ms : 1));
    for (UBaseType_t i = 0; i < n; i++) {
      const uint32_t d = (uint32_t)s_cur[i].ulRunTimeCounter - prev_runtime(s_cur[i].xHandle, 0);
      const uint16_t pct = (uint16_t)(((uint64_t)d * 1000) / dtotal);
      if (strncmp(s_cur[i].pcTaskName, "IDLE", 4) == 0) { w.idle_x10 += pct; continue; }
      /* Insert into the top list, busiest first. */
      uint8_t pos = w.n;
      while (pos > 0 && w.top[pos - 1].pct_x10 < pct) pos--;
      if (pos >= CPUSTAT_TOP) continue;
      const uint8_t keep = w.n < CPUSTAT_TOP ? w.n : CPUSTAT_TOP - 1;
      memmove(&w.top[pos + 1], &w.top[pos], (keep - pos) * sizeof(cpu_task_t));
      strncpy(w.top[pos].name, s_cur[i].pcTaskName, sizeof(w.top[pos].name) - 1);
      w.top[pos].name[sizeof(w.top[pos].name) - 1] = '\0';
      w.top[pos].pct_x10 = pct;
      if (w.n < CPUSTAT_TOP) w.n++;
    }
    s_last = w;
    if (!s_peak.valid || w.idle_x10 < s_peak.idle_x10) s_peak = w;
  }

  memcpy(s_prev, s_cur, n * sizeof(TaskStatus_t));
  s_prev_n = n;
  s_prev_total = total;
  last_ms = now;
  last_e = bytes_e2w;
  last_w = bytes_w2e;
}

/* See cpustat.h. */
void cpustat_reset_peak(void) { s_peak.valid = false; }

/* Append one window as a JSON object. */
static size_t window_json(char *out, size_t max, const cpu_window_t *w) {
  if (!w->valid) return (size_t)snprintf(out, max, "null");
  size_t o = (size_t)snprintf(out, max,
      "{\"at\":%lu,\"idle\":%u.%u,\"kbps_up\":%lu,\"kbps_down\":%lu,\"tasks\":[",
      (unsigned long)w->at_s, w->idle_x10 / 10, w->idle_x10 % 10,
      (unsigned long)w->kbps_e2w, (unsigned long)w->kbps_w2e);
  for (uint8_t i = 0; i < w->n && o < max; i++) {
    o += (size_t)snprintf(out + o, max - o, "%s[\"%s\",%u.%u]", i ? "," : "",
                          w->top[i].name, w->top[i].pct_x10 / 10, w->top[i].pct_x10 % 10);
  }
  if (o < max) o += (size_t)snprintf(out + o, max - o, "]}");
  return o < max ? o : max - 1;
}

/* See cpustat.h. */
size_t cpustat_json(char *out, size_t max) {
  size_t o = (size_t)snprintf(out, max, "{\"enabled\":1,\"window_ms\":%u,\"last\":",
                              (unsigned)CPUSTAT_WINDOW_MS);
  if (o < max) o += window_json(out + o, max - o, &s_last);
  if (o < max) o += (size_t)snprintf(out + o, max - o, ",\"peak\":");
  if (o < max) o += window_json(out + o, max - o, &s_peak);
  if (o < max) o += (size_t)snprintf(out + o, max - o, "}");
  return o < max ? o : max - 1;
}

#else  /* !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS */

/* Stubs: run-time statistics are not compiled into FreeRTOS. */
void cpustat_tick(uint64_t bytes_e2w, uint64_t bytes_w2e) { (void)bytes_e2w; (void)bytes_w2e; }
void cpustat_reset_peak(void) {}
size_t cpustat_json(char *out, size_t max) {
  return (size_t)snprintf(out, max, "{\"enabled\":0}");
}

#endif
