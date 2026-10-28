/*
 * conlog.cpp - In-memory copy of the console output (see conlog.h)
 *
 * stdout is replaced by a funopen() stream whose write function copies the
 * data into a ring buffer and then passes it on to the original stdout, so
 * the serial/USB console keeps working exactly as before.
 *
 * The ring lives in .noinit RAM, which startup code leaves untouched: after
 * any reset except a power loss it still holds the end of the previous boot's
 * output. conlog_init() takes the last CONLOG_PREV_SIZE bytes of it before
 * the new boot starts writing, and conlog_persist_prev() stores them in NVS,
 * where they survive further reboots and power loss.
 */

#include "conlog.h"

#include <stdio.h>
#include <string.h>
#include <sys/reent.h>

#include "esp_attr.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

#define CONLOG_MAGIC  0x474f4c43u   /* "CLOG" */
#define CONLOG_NVS_NS  "conlog"
#define CONLOG_NVS_KEY "prev"

/* The ring plus a validity header. check = magic ^ head is updated with every
 * write, so leftover RAM from a different firmware layout is not mistaken for
 * a log. */
typedef struct {
  uint32_t magic;
  uint32_t head;      /* total bytes ever written; wraps after 4 GB */
  uint32_t check;
  char     buf[CONLOG_SIZE];
} conlog_ring_t;

__NOINIT_ATTR static conlog_ring_t s_ring;

static FILE        *s_orig = NULL;     /* the console stdout before conlog_init() */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Tail of the previous boot's log, taken by conlog_init() and handed to NVS
 * by conlog_persist_prev(). The first byte is the reset reason. */
static char     s_prev[1 + CONLOG_PREV_SIZE];
static uint32_t s_prev_len = 0;        /* bytes in s_prev including the reason */

/* Copy 'len' bytes into the ring at the current head. Caller holds s_lock.
 * Writes larger than the ring keep only their tail, which is all the ring
 * could hold anyway. */
static void ring_put(const char *data, uint32_t len) {
  if (len > CONLOG_SIZE) {
    data += len - CONLOG_SIZE;
    s_ring.head += len - CONLOG_SIZE;
    len = CONLOG_SIZE;
  }
  const uint32_t pos   = s_ring.head & (CONLOG_SIZE - 1);
  const uint32_t first = (len < CONLOG_SIZE - pos) ? len : CONLOG_SIZE - pos;
  memcpy(s_ring.buf + pos, data, first);
  memcpy(s_ring.buf, data + first, len - first);
  s_ring.head += len;
  s_ring.check = CONLOG_MAGIC ^ s_ring.head;
}

/* Copy n bytes ending at absolute position 'end' out of the ring (n must not
 * exceed CONLOG_SIZE or 'end'). */
static void ring_copy_back(uint32_t end, uint32_t n, char *out) {
  const uint32_t since = end - n;
  const uint32_t pos   = since & (CONLOG_SIZE - 1);
  const uint32_t first = (n < CONLOG_SIZE - pos) ? n : CONLOG_SIZE - pos;
  memcpy(out, s_ring.buf + pos, first);
  memcpy(out + first, s_ring.buf, n - first);
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

/* See conlog.h. First salvages the previous boot's tail from the .noinit
 * ring (not after a power-on or brownout reset - RAM content is undefined
 * then), then starts a fresh ring and swaps the global and the calling
 * task's stdout for a line-buffered funopen() stream. Does nothing if called
 * twice; if the stream cannot be created the console simply stays as it
 * was. */
void conlog_init(void) {
  if (s_orig) return;

  const esp_reset_reason_t rr = esp_reset_reason();
  if (rr != ESP_RST_POWERON && rr != ESP_RST_BROWNOUT && rr != ESP_RST_UNKNOWN &&
      s_ring.magic == CONLOG_MAGIC && s_ring.check == (CONLOG_MAGIC ^ s_ring.head)) {
    uint32_t n = s_ring.head;
    if (n > CONLOG_SIZE)      n = CONLOG_SIZE;
    if (n > CONLOG_PREV_SIZE) n = CONLOG_PREV_SIZE;
    s_prev[0] = (char)(uint8_t)rr;
    ring_copy_back(s_ring.head, n, s_prev + 1);
    s_prev_len = 1 + n;
  }
  s_ring.magic = CONLOG_MAGIC;
  s_ring.head  = 0;
  s_ring.check = CONLOG_MAGIC;

  FILE *f = funopen(NULL, NULL, conlog_write, NULL, NULL);
  if (!f) return;
  setvbuf(f, NULL, _IOLBF, 256);
  s_orig = _REENT_STDOUT(_GLOBAL_REENT);
  _REENT_STDOUT(_GLOBAL_REENT) = f;   /* inherited by every task created later */
  stdout = f;                         /* the calling (main) task */
}

/* See conlog.h. One NVS write per boot that followed a reset with a valid
 * ring - i.e. only after reboots, which are rare - so flash wear is no
 * concern. */
void conlog_persist_prev(void) {
  if (!s_prev_len) return;
  nvs_handle_t h;
  if (nvs_open(CONLOG_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  esp_err_t e = nvs_set_blob(h, CONLOG_NVS_KEY, s_prev, s_prev_len);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  if (e != ESP_OK) printf("[LOG ] Saving the previous boot's log failed: %s\n", esp_err_to_name(e));
  s_prev_len = 0;
}

/* See conlog.h. The read buffer is static rather than on the stack: the only
 * caller is the HTTP server, which handles one request at a time and already
 * has its own 2 KB reply buffer on its stack. */
size_t conlog_get_prev(char *out, size_t max, uint8_t *rst) {
  nvs_handle_t h;
  if (nvs_open(CONLOG_NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
  static char raw[1 + CONLOG_PREV_SIZE];
  size_t sz = sizeof(raw);
  const esp_err_t e = nvs_get_blob(h, CONLOG_NVS_KEY, raw, &sz);
  nvs_close(h);
  if (e != ESP_OK || sz < 1) return 0;
  *rst = (uint8_t)raw[0];
  size_t n = sz - 1;
  if (n > max) n = max;
  memcpy(out, raw + 1 + (sz - 1 - n), n);   /* keep the end, it is the interesting part */
  return n;
}

/* See conlog.h. Positions are absolute byte counts, so a caller that keeps
 * polling with the returned 'next' only ever receives new output. */
size_t conlog_read(uint32_t since, char *out, size_t max, uint32_t *next, bool *reset) {
  portENTER_CRITICAL(&s_lock);
  const uint32_t head   = s_ring.head;
  const uint32_t oldest = (head > CONLOG_SIZE) ? head - CONLOG_SIZE : 0;
  bool r = false;
  if (since < oldest || since > head) { since = oldest; r = true; }
  uint32_t n = head - since;
  if (n > max) n = (uint32_t)max;
  ring_copy_back(since + n, n, out);
  portEXIT_CRITICAL(&s_lock);
  *next  = since + n;
  *reset = r;
  return n;
}
