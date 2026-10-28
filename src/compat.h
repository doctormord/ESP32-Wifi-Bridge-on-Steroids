/*
 * compat.h - Minimal replacement for the few Arduino helpers the code uses.
 *
 * Deliberately NOT an Arduino emulation layer: only millis() and delay() are
 * provided, because they are used all over the code base and rewriting every
 * call site would add risk without any benefit. Everything else Arduino used
 * to provide (Serial, pinMode, ESP.*) has been replaced directly by the
 * corresponding ESP-IDF APIs.
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

/* Milliseconds since boot.
 *
 * esp_timer counts microseconds since boot in 64 bits. Truncating to 32 bits
 * keeps the behavior identical to Arduino's millis(), including the wrap-around
 * after roughly 49 days. All comparisons in the code are written as
 * differences (millis() - t0 < x), which makes them wrap-around safe. */
static inline uint32_t millis(void) {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* Block the calling FreeRTOS task for at least `ms` milliseconds.
 *
 * Yields the CPU to other tasks while waiting. Resolution is one FreeRTOS
 * tick (10 ms at the default 100 Hz), so short delays are rounded to ticks. */
static inline void delay(uint32_t ms) {
  vTaskDelay(pdMS_TO_TICKS(ms));
}
