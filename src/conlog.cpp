/*
 * conlog.cpp - In-memory copy of the console output (see conlog.h)
 *
 * stdout is replaced by a funopen() stream whose write function copies the
 * data into a ring buffer and then passes it on to the original stdout, so
 * the serial/USB console keeps working exactly as before.
 */

#include "conlog.h"

#include <stdio.h>
#include <string.h>
#include <sys/reent.h>

#include "freertos/FreeRTOS.h"

static char         s_buf[CONLOG_SIZE];
static uint32_t     s_head = 0;        /* total bytes ever written; wraps after 4 GB */
static FILE        *s_orig = NULL;     /* the console stdout before conlog_init() */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Copy 'len' bytes into the ring at the current head. Caller holds s_lock.
 * Writes larger than the ring keep only their tail, which is all the ring
 * could hold anyway. */
static void ring_put(const char *data, uint32_t len) {
  if (len > CONLOG_SIZE) {
    data += len - CONLOG_SIZE;
    s_head += len - CONLOG_SIZE;
    len = CONLOG_SIZE;
  }
  const uint32_t pos   = s_head & (CONLOG_SIZE - 1);
  const uint32_t first = (len < CONLOG_SIZE - pos) ? len : CONLOG_SIZE - pos;
  memcpy(s_buf + pos, data, first);
  memcpy(s_buf, data + first, len - first);
  s_head += len;
}

/* funopen() write callback, called by newlib when the stream's line buffer is
 * flushed - always from task context (printf is not used in ISRs). The
 * spinlock only covers the memcpy into the ring, never the forwarding to the
 * console, which may block. */
static int conlog_write(void *cookie, const char *data, int len) {
  (void)cookie;
  if (len <= 0) return 0;
  portENTER_CRITICAL(&s_lock);
  ring_put(data, (uint32_t)len);
  portEXIT_CRITICAL(&s_lock);
  if (s_orig) {
    fwrite(data, 1, (size_t)len, s_orig);
    fflush(s_orig);
  }
  return len;
}

/* See conlog.h. Swaps the global and the calling task's stdout for a line-
 * buffered funopen() stream; does nothing if called twice or if the stream
 * cannot be created (the console then simply stays as it was). */
void conlog_init(void) {
  if (s_orig) return;
  FILE *f = funopen(NULL, NULL, conlog_write, NULL, NULL);
  if (!f) return;
  setvbuf(f, NULL, _IOLBF, 256);
  s_orig = _REENT_STDOUT(_GLOBAL_REENT);
  _REENT_STDOUT(_GLOBAL_REENT) = f;   /* inherited by every task created later */
  stdout = f;                         /* the calling (main) task */
}

/* See conlog.h. Positions are absolute byte counts, so a caller that keeps
 * polling with the returned 'next' only ever receives new output. */
size_t conlog_read(uint32_t since, char *out, size_t max, uint32_t *next, bool *reset) {
  portENTER_CRITICAL(&s_lock);
  const uint32_t head   = s_head;
  const uint32_t oldest = (head > CONLOG_SIZE) ? head - CONLOG_SIZE : 0;
  bool r = false;
  if (since < oldest || since > head) { since = oldest; r = true; }
  uint32_t n = head - since;
  if (n > max) n = (uint32_t)max;
  const uint32_t pos   = since & (CONLOG_SIZE - 1);
  const uint32_t first = (n < CONLOG_SIZE - pos) ? n : CONLOG_SIZE - pos;
  memcpy(out, s_buf + pos, first);
  memcpy(out + first, s_buf, n - first);
  portEXIT_CRITICAL(&s_lock);
  *next  = since + n;
  *reset = r;
  return n;
}
