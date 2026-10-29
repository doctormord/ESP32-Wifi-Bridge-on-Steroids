/*
 * cpustat.h - CPU load per task, for finding the throughput bottleneck
 *
 * Every CPUSTAT_WINDOW_MS the FreeRTOS run-time counters of all tasks are
 * compared with the previous snapshot, giving each task's share of the CPU
 * over that window, next to the bridge throughput in both directions. The
 * window with the least idle time since the last reset is kept as "peak",
 * so a load test can be read back afterwards - also when the machine that
 * ran the test sat behind the bridge, where the management IP is not
 * reachable. Needs CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS; without it the
 * functions compile to stubs and GET /api/cpu reports "enabled":0.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define CPUSTAT_WINDOW_MS 5000
#define CPUSTAT_TOP       8      /* tasks listed per window, busiest first */

/* Call from the main loop as often as convenient; samples once per window.
 * 'bytes_e2w'/'bytes_w2e' are the bridge's running byte totals per
 * direction (Ethernet -> Wi-Fi, Wi-Fi -> Ethernet). */
void cpustat_tick(uint64_t bytes_e2w, uint64_t bytes_w2e);

/* Forget the peak window (GET /api/cpu?reset=1). */
void cpustat_reset_peak(void);

/* Write the last and the peak window as JSON into 'out'; returns the length. */
size_t cpustat_json(char *out, size_t max);

/* Die temperature of the SoC's internal sensor, sampled by cpustat_tick()
 * once per window, in 0.1 degC; CPUSTAT_NO_TEMP if the chip has no sensor
 * or it could not be started. The sensor measures the chip, not the voltage
 * regulator next to it - on a small module both heat the same board, so the
 * trend is what tells. */
#define CPUSTAT_NO_TEMP (-32768)
int16_t cpustat_temp_x10(void);

/* Highest value of cpustat_temp_x10() since boot. */
int16_t cpustat_temp_max_x10(void);
