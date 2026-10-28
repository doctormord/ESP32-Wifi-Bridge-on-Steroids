/*
 * w5500_mac.c - W5500 MAC driver tuned for bridging (see w5500_mac.h)
 *
 * Derived from ESP-IDF's esp_eth_mac_w5500.c (Apache-2.0, Espressif Systems).
 * Chip setup, the PHY register access and the receive filter work as there;
 * the data path is rebuilt around three facts about the W5500 in MACRAW mode:
 *
 *   - Socket 0 owns the whole 16 KB of TX and RX memory, but a SEND transmits
 *     exactly one frame, and the next SEND may only be issued once the chip
 *     reports SEND_OK. Writing the next frame's bytes into TX memory behind
 *     the committed write pointer is allowed at any time, though - so that
 *     transfer (the expensive part, ~0.2 ms per full frame at 53 MHz) runs
 *     while the previous frame is still going out on the wire.
 *   - Received frames sit back to back in RX memory, each preceded by a
 *     2-byte length. One SPI read fetches several of them, and a single
 *     RX_RD update + RECV command releases them all.
 *   - Every SPI transaction costs a fixed overhead of several microseconds
 *     on top of the bytes, so the number of transactions per frame matters
 *     as much as the clock: the IDF driver needs ~10 per transmitted and ~9
 *     per received frame, this one ~4 and ~4 per batch.
 *
 * Threads: transmit() runs in the caller's task (the Wi-Fi task in the
 * bridge) and only copies into the TX ring. Everything that talks to the
 * chip's data path runs in the driver task. Control calls from other tasks
 * (start/stop via the link timer, PHY register reads, filter ioctls) are
 * serialized against it with op_lock and spi_lock.
 */
#include "sdkconfig.h"
#if CONFIG_ETH_SPI_ETHERNET_W5500

#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <sys/cdefs.h>
#include "w5500_mac.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "w5500_regs.h"

static const char *TAG = "w5500m";

/* TX ring: number of frames transmit() can queue ahead of the chip. A Wi-Fi
 * A-MPDU delivers frames in bursts; slots absorb a burst instead of dropping
 * it while the SPI catches up. Must be a power of two (free-running
 * head/tail indices). 16 x 1.5 KB = 24 KB of internal RAM. */
#define TX_SLOTS           16
#define TX_SLOT_SIZE       1536    /* >= ETH_MAX_PACKET_SIZE, multiple of 4 */

/* Bytes fetched from RX memory per SPI read: room for four full frames plus
 * the 2-byte headers. Larger reads save transactions, but delay the first
 * frame of a batch and cost RAM. Multiple of 4 (DMA alignment). Capped at
 * runtime to what the SPI bus was set up for (rx_chunk). */
#define RX_CHUNK           W5500_MAC_MAX_TRANSFER

/* Transfers above this size wait for the SPI interrupt instead of spinning,
 * so the CPU is free for the Wi-Fi stack while the DMA runs. Small register
 * accesses are faster polled: the interrupt path costs two context
 * switches. */
#define BIG_XFER           64

/* Frames moved to the chip per driver-task round before reception gets a
 * turn - keeps a long Wi-Fi burst from starving the Ethernet RX side. */
#define TX_BURST           4

/* SEND_OK must arrive within the wire time of a full frame plus margin. */
#define TX_TMO_100M_US     200
#define TX_TMO_10M_US      1500

/* Sn_CR clears itself once the chip has taken a command, and a new command
 * must not be written before that. The chip takes a command within a few of
 * its own clocks; only a command issued less than this long ago is checked
 * by reading Sn_CR back, which saves that read in the normal case. */
#define CMD_SETTLE_US      20

/* INTLEVEL: how long the W5500 waits before asserting INT for a new event
 * after the previous one was cleared, in units of 4 PLL clocks (26.7 ns).
 * IDF uses the maximum (~1.75 ms) to make missed edges unlikely, but that
 * delays every RX wake-up after an idle moment by up to that much - long
 * enough to slow TCP's ACK clock. ~7 us still gives the GPIO edge detector a
 * clean high phase, and the driver task re-checks the INT level before it
 * sleeps, so a missed edge cannot strand frames. */
#define INT_ASSERT_WAIT    0x00FF

#define SPI_LOCK_TIMEOUT_MS 50

/* Rounds (1 tick each) a frame may stay incomplete in RX memory before the
 * buffer is assumed corrupt and resynced. A frame is stored in well under a
 * millisecond, so this only triggers on a genuinely broken length. */
#define RX_STALL_MAX       50

typedef struct {
  uint8_t  data[TX_SLOT_SIZE];
  uint32_t len;
} tx_slot_t;

typedef struct {
  esp_eth_mac_t       parent;
  esp_eth_mediator_t *eth;
  spi_device_handle_t spi;
  SemaphoreHandle_t   spi_lock;    /* one SPI transaction at a time            */
  SemaphoreHandle_t   op_lock;     /* data path vs. start/stop                 */
  SemaphoreHandle_t   tx_lock;     /* producers of the TX ring                 */
  TaskHandle_t        task;
  int                 int_gpio;
  uint32_t            sw_reset_timeout_ms;
  uint8_t             addr[ETH_ADDR_LEN];
  volatile bool       running;     /* socket 0 open, data path live            */

  /* TX ring: producers fill slots[head], the driver task drains slots[tail]. */
  tx_slot_t          *slots;
  volatile uint32_t   tx_head;
  volatile uint32_t   tx_tail;
  uint16_t            tx_wr;       /* Sn_TX_WR as last committed               */
  bool                tx_pending;  /* a SEND is in flight, SEND_OK not yet seen */
  uint32_t            tx_tmo_us;
  int64_t             last_cmd_us;

  /* RX: rx_buf[0..rx_have) mirrors RX memory starting at rx_rd - a frame
   * that was cut off at the end of the previous read is kept here, so it is
   * not fetched twice. */
  uint8_t            *rx_buf;
  uint32_t            rx_chunk;    /* bytes per read: RX_CHUNK or the bus limit */
  uint32_t            rx_have;
  uint16_t            rx_rd;

  w5500_mac_stats_t   stats;
} w5500m_t;

/* --- SPI access ------------------------------------------------------------ */

/* One W5500 SPI frame: 16-bit offset (sent as the SPI command phase), 8-bit
 * control byte (block select, read/write, variable length - the SPI address
 * phase), then 'len' data bytes. Up to 4 bytes travel in the transaction
 * itself; longer buffers go by DMA. Reads into 'data' with len > 4 need a
 * 4-byte aligned buffer, or the SPI driver allocates a bounce buffer. */
static esp_err_t w_xfer(w5500m_t *m, uint32_t address, bool write, void *data, uint32_t len) {
  spi_transaction_t t = {0};
  t.cmd    = (uint16_t)(address >> W5500_ADDR_OFFSET);
  t.addr   = (address & 0xFFFF) |
             ((write ? W5500_ACCESS_MODE_WRITE : W5500_ACCESS_MODE_READ) << W5500_RWB_OFFSET) |
             W5500_SPI_OP_MODE_VDM;
  t.length = 8 * len;
  if (len <= 4) {
    if (write) { t.flags = SPI_TRANS_USE_TXDATA; memcpy(t.tx_data, data, len); }
    else       { t.flags = SPI_TRANS_USE_RXDATA; }
  } else if (write) {
    t.tx_buffer = data;
  } else {
    t.rx_buffer = data;
  }

  if (xSemaphoreTake(m->spi_lock, pdMS_TO_TICKS(SPI_LOCK_TIMEOUT_MS)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  const esp_err_t e = len > BIG_XFER ? spi_device_transmit(m->spi, &t)
                                     : spi_device_polling_transmit(m->spi, &t);
  xSemaphoreGive(m->spi_lock);

  if (e != ESP_OK) ESP_LOGE(TAG, "SPI transfer failed: %s", esp_err_to_name(e));
  else if (!write && len <= 4) memcpy(data, t.rx_data, len);
  return e;
}

/* Read 'len' bytes of registers or buffer memory at 'address'. */
static inline esp_err_t w_read(w5500m_t *m, uint32_t address, void *data, uint32_t len) {
  return w_xfer(m, address, false, data, len);
}

/* Write 'len' bytes of registers or buffer memory at 'address'. */
static inline esp_err_t w_write(w5500m_t *m, uint32_t address, const void *data, uint32_t len) {
  return w_xfer(m, address, true, (void *)data, len);
}

/* Write a 16-bit register (the W5500 is big-endian). */
static esp_err_t w_write16(w5500m_t *m, uint32_t address, uint16_t v) {
  const uint8_t be[2] = { (uint8_t)(v >> 8), (uint8_t)v };
  return w_write(m, address, be, 2);
}

/* Poll Sn_CR until the chip has taken the last command, for at most
 * 'timeout_ms'. */
static esp_err_t w_cmd_wait(w5500m_t *m, uint32_t timeout_ms) {
  const int64_t t0 = esp_timer_get_time();
  uint8_t cr;
  do {
    ESP_RETURN_ON_ERROR(w_read(m, W5500_REG_SOCK_CR(0), &cr, 1), TAG, "read SCR failed");
    if (!cr) return ESP_OK;
    if (timeout_ms > 1) vTaskDelay(1);
  } while (esp_timer_get_time() - t0 < (int64_t)timeout_ms * 1000);
  return ESP_ERR_TIMEOUT;
}

/* Issue a socket command on the data path (SEND, RECV) without waiting for
 * it to be taken - see CMD_SETTLE_US. 'bytes' starts with the command and may
 * continue into the following registers (Sn_IR follows Sn_CR), so a SEND can
 * clear the previous SEND_OK in the same transaction. */
static esp_err_t w_cmd_fast(w5500m_t *m, const uint8_t *bytes, uint32_t len) {
  if (esp_timer_get_time() - m->last_cmd_us < CMD_SETTLE_US) {
    ESP_RETURN_ON_ERROR(w_cmd_wait(m, 1), TAG, "previous command not taken");
  }
  const esp_err_t e = w_write(m, W5500_REG_SOCK_CR(0), bytes, len);
  m->last_cmd_us = esp_timer_get_time();
  return e;
}

/* Issue a control command (OPEN, CLOSE) and wait until it was taken. */
static esp_err_t w_cmd(w5500m_t *m, uint8_t cmd) {
  ESP_RETURN_ON_ERROR(w_write(m, W5500_REG_SOCK_CR(0), &cmd, 1), TAG, "write SCR failed");
  m->last_cmd_us = esp_timer_get_time();
  return w_cmd_wait(m, 100);
}

/* --- Transmit ---------------------------------------------------------------- */

/* Wait for SEND_OK of the frame in flight. On timeout the frame is written
 * off (the link is probably gone) so the next one is not blocked forever. */
static bool tx_wait_send_done(w5500m_t *m) {
  const int64_t t0 = esp_timer_get_time();
  uint8_t ir = 0;
  do {
    if (w_read(m, W5500_REG_SOCK_IR(0), &ir, 1) != ESP_OK) break;
    if (ir & W5500_SIR_SEND) return true;
  } while (esp_timer_get_time() - t0 < (int64_t)m->tx_tmo_us);
  m->tx_pending = false;
  m->stats.tx_timeout++;
  return false;
}

/* Move up to TX_BURST queued frames to the chip. Per frame: write the bytes
 * behind the committed write pointer (overlaps the previous frame's wire
 * time), wait for that previous SEND_OK (usually already there by then),
 * commit Sn_TX_WR and issue SEND together with clearing SEND_OK. A frame that
 * fails anywhere is dropped - there is no retransmission at this layer. */
static void tx_service(w5500m_t *m) {
  for (int n = 0; n < TX_BURST && m->tx_tail != m->tx_head; n++) {
    tx_slot_t *s = &m->slots[m->tx_tail % TX_SLOTS];
    const uint16_t len = (uint16_t)s->len;

    bool ok = w_write(m, W5500_MEM_SOCK_TX(0, (uint32_t)m->tx_wr), s->data, len) == ESP_OK;
    if (ok && m->tx_pending) ok = tx_wait_send_done(m);
    if (ok) {
      const uint16_t wr = (uint16_t)(m->tx_wr + len);
      static const uint8_t send[2] = { W5500_SCR_SEND, W5500_SIR_SEND };
      ok = w_write16(m, W5500_REG_SOCK_TX_WR(0), wr) == ESP_OK &&
           w_cmd_fast(m, send, sizeof(send)) == ESP_OK;
      if (ok) { m->tx_wr = wr; m->tx_pending = true; }
    }
    __sync_synchronize();          /* slot contents consumed before release */
    m->tx_tail = m->tx_tail + 1;
  }
}

/* esp_eth_mac_t::transmit - queue a frame for the driver task. Never touches
 * the SPI bus, so the caller (the Wi-Fi RX path in the bridge) is held up
 * only for one memcpy. Returns ESP_ERR_NO_MEM if all slots are taken. */
static esp_err_t w5500m_transmit(esp_eth_mac_t *mac, uint8_t *buf, uint32_t length) {
  w5500m_t *m = __containerof(mac, w5500m_t, parent);
  ESP_RETURN_ON_FALSE(length <= ETH_MAX_PACKET_SIZE, ESP_ERR_INVALID_ARG, TAG,
                      "frame too big (%" PRIu32 ")", length);
  if (!m->running) return ESP_ERR_INVALID_STATE;

  xSemaphoreTake(m->tx_lock, portMAX_DELAY);
  if (m->tx_head - m->tx_tail >= TX_SLOTS) {
    m->stats.tx_ring_full++;
    xSemaphoreGive(m->tx_lock);
    return ESP_ERR_NO_MEM;
  }
  tx_slot_t *s = &m->slots[m->tx_head % TX_SLOTS];
  memcpy(s->data, buf, length);
  s->len = length;
  __sync_synchronize();            /* slot contents visible before publishing */
  m->tx_head = m->tx_head + 1;
  xSemaphoreGive(m->tx_lock);

  xTaskNotifyGive(m->task);
  return ESP_OK;
}

/* --- Receive ----------------------------------------------------------------- */

/* Hand one received frame to esp_eth (-> the bridge's input callback), which
 * takes ownership of the heap copy. */
static void rx_deliver(w5500m_t *m, const uint8_t *frame, uint32_t len) {
  uint8_t *buf = malloc(len);
  if (!buf) { m->stats.rx_no_mem++; return; }
  memcpy(buf, frame, len);
  m->eth->stack_input(m->eth, buf, len);
}

/* Discard everything in RX memory - used when a length header is implausible
 * or a frame never completes, because from a corrupt header on there is no
 * way to find the next frame. */
static void rx_resync(w5500m_t *m, uint16_t rd, uint32_t rsr) {
  static const uint8_t recv = W5500_SCR_RECV;
  m->stats.rx_bad_len++;
  m->rx_have = 0;
  m->rx_rd = (uint16_t)(rd + rsr);
  if (w_write16(m, W5500_REG_SOCK_RX_RD(0), m->rx_rd) == ESP_OK) w_cmd_fast(m, &recv, 1);
  ESP_LOGW(TAG, "corrupt RX frame header, dropped %" PRIu32 " bytes", rsr);
}

typedef enum { RX_IDLE, RX_PROGRESS, RX_STALLED } rx_result_t;

/* One reception round: read Sn_RX_RSR and Sn_RX_RD together, fetch up to
 * RX_CHUNK bytes (minus what is still buffered), deliver every complete
 * frame, then release them all with one RX_RD write and one RECV. A frame
 * cut off by the end of the read stays in rx_buf for the next round.
 *
 * Returns RX_PROGRESS if frames were delivered (more may be waiting),
 * RX_STALLED if data is waiting but no complete frame could be taken yet
 * (the received-size register lags a frame still being stored, or was read
 * mid-update), RX_IDLE if RX memory is empty. */
static rx_result_t rx_service(w5500m_t *m) {
  uint8_t r[4];
  if (w_read(m, W5500_REG_SOCK_RX_RSR(0), r, sizeof(r)) != ESP_OK) return RX_IDLE;
  const uint32_t rsr = ((uint32_t)r[0] << 8) | r[1];
  const uint16_t rd  = (uint16_t)(((uint16_t)r[2] << 8) | r[3]);
  if (rd != m->rx_rd || m->rx_have > rsr) m->rx_have = 0;   /* buffer no longer matches */
  m->rx_rd = rd;
  if (rsr == 0) return RX_IDLE;

  const uint32_t want = rsr < m->rx_chunk ? rsr : m->rx_chunk;
  if (want > m->rx_have) {
    /* Re-read up to 3 already buffered bytes so the DMA target stays
     * 4-byte aligned; they read back identical. */
    const uint32_t from = m->rx_have & ~3u;
    if (w_read(m, W5500_MEM_SOCK_RX(0, (uint32_t)(uint16_t)(rd + from)), m->rx_buf + from,
               want - from) != ESP_OK) {
      m->rx_have = 0;
      return RX_IDLE;
    }
    m->rx_have = want;
  }

  uint32_t off = 0;
  while (off + 2 <= m->rx_have) {
    const uint32_t flen = ((uint32_t)m->rx_buf[off] << 8) | m->rx_buf[off + 1];  /* incl. header */
    if (flen < 2 + ETH_MIN_PACKET_SIZE - ETH_CRC_LEN || flen > 2 + ETH_MAX_PACKET_SIZE) {
      rx_resync(m, rd, rsr);
      return RX_PROGRESS;
    }
    if (off + flen > m->rx_have) break;
    rx_deliver(m, m->rx_buf + off + 2, flen - 2);
    off += flen;
  }
  if (off == 0) return RX_STALLED;

  static const uint8_t recv = W5500_SCR_RECV;
  m->rx_rd = (uint16_t)(rd + off);
  if (w_write16(m, W5500_REG_SOCK_RX_RD(0), m->rx_rd) == ESP_OK) w_cmd_fast(m, &recv, 1);
  m->rx_have -= off;
  memmove(m->rx_buf, m->rx_buf + off, m->rx_have);
  return RX_PROGRESS;
}

/* --- Driver task --------------------------------------------------------------- */

/* INT falling edge: wake the driver task. */
IRAM_ATTR static void w5500m_isr(void *arg) {
  w5500m_t *m = (w5500m_t *)arg;
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(m->task, &woken);
  if (woken) portYIELD_FROM_ISR();
}

/* The driver task: sleeps until there is something to do - INT low (frames
 * received), frames in the TX ring, or reception still has a backlog - then
 * alternates TX bursts and RX rounds until both sides are idle. It never
 * sleeps while INT is low, so an edge missed by the GPIO interrupt only costs
 * the wake-up, never strands frames. As a last fallback, a full second
 * without any wake-up also checks RX memory, whatever the INT level says. */
static void w5500m_task(void *arg) {
  w5500m_t *m = (w5500m_t *)arg;
  bool rx_more = false;
  uint32_t idle_low = 0, stalls = 0;
  for (;;) {
    const bool tx_waiting = m->tx_tail != m->tx_head;
    if (!m->running ||
        (!rx_more && !tx_waiting && gpio_get_level(m->int_gpio) != 0)) {
      if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) rx_more = true;
    }

    rx_result_t rx = RX_IDLE;
    bool int_low = false;
    xSemaphoreTake(m->op_lock, portMAX_DELAY);
    if (m->running) {
      if (gpio_get_level(m->int_gpio) == 0) {
        /* Clear before reading the received size: a frame stored after this
         * point raises INT again. */
        static const uint8_t clr = W5500_SIR_RECV;
        w_write(m, W5500_REG_SOCK_IR(0), &clr, 1);
        rx_more = true;
        int_low = true;
      }
      tx_service(m);
      if (rx_more) {
        rx = rx_service(m);
        rx_more = rx != RX_IDLE;
        stalls = rx == RX_STALLED ? stalls + 1 : 0;
        if (stalls > RX_STALL_MAX) {
          uint8_t r[4];
          if (w_read(m, W5500_REG_SOCK_RX_RSR(0), r, sizeof(r)) == ESP_OK) {
            rx_resync(m, (uint16_t)((r[2] << 8) | r[3]), ((uint32_t)r[0] << 8) | r[1]);
          }
          stalls = 0;
        }
      }
    } else {
      m->tx_tail = m->tx_head;     /* frames queued while stopped are dropped */
      rx_more = false;
    }
    xSemaphoreGive(m->op_lock);

    /* Data announced but not complete yet, or INT stuck low with nothing to
     * read: give the chip (and lower-priority tasks) a tick instead of
     * spinning. */
    idle_low = (int_low && rx == RX_IDLE) ? idle_low + 1 : 0;
    if (rx == RX_STALLED || idle_low > 8) vTaskDelay(1);
  }
}

/* --- Chip setup ------------------------------------------------------------------ */

/* Software reset via MR.RST, which clears itself when the chip is done. */
static esp_err_t w5500m_reset(w5500m_t *m) {
  uint8_t mr = W5500_MR_RST;
  ESP_RETURN_ON_ERROR(w_write(m, W5500_REG_MR, &mr, 1), TAG, "write MR failed");
  for (uint32_t to = 0; to < m->sw_reset_timeout_ms / 10; to++) {
    ESP_RETURN_ON_ERROR(w_read(m, W5500_REG_MR, &mr, 1), TAG, "read MR failed");
    if (!(mr & W5500_MR_RST)) return ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  ESP_LOGE(TAG, "reset timeout");
  return ESP_ERR_TIMEOUT;
}

/* The W5500 has no chip ID; check the version register instead. It can read
 * 0 right after a reset, so it is polled. A wrong value is also what an SPI
 * clock too fast for the wiring produces - the bridge's clock step-down
 * relies on this check failing then. */
static esp_err_t w5500m_verify_id(w5500m_t *m) {
  uint8_t version = 0;
  for (uint32_t to = 0; to < m->sw_reset_timeout_ms / 10; to++) {
    ESP_RETURN_ON_ERROR(w_read(m, W5500_REG_VERSIONR, &version, 1), TAG, "read VERSIONR failed");
    if (version == W5500_CHIP_VERSION) return ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  ESP_LOGE(TAG, "W5500 version mismatched, expected 0x%02x, got 0x%02x",
           W5500_CHIP_VERSION, version);
  return ESP_ERR_INVALID_VERSION;
}

/* Register setup after reset: all 16 KB TX/RX memory to socket 0, MACRAW with
 * MAC filter and multicast block (the bridge clears both via ioctls), RECV
 * as the only interrupt source, short INT re-assert time. */
static esp_err_t w5500m_setup_default(w5500m_t *m) {
  uint8_t v = 16;
  ESP_RETURN_ON_ERROR(w_write(m, W5500_REG_SOCK_RXBUF_SIZE(0), &v, 1), TAG, "rx buffer size");
  ESP_RETURN_ON_ERROR(w_write(m, W5500_REG_SOCK_TXBUF_SIZE(0), &v, 1), TAG, "tx buffer size");
  v = 0;
  for (int i = 1; i < 8; i++) {
    ESP_RETURN_ON_ERROR(w_write(m, W5500_REG_SOCK_RXBUF_SIZE(i), &v, 1), TAG, "rx buffer size");
    ESP_RETURN_ON_ERROR(w_write(m, W5500_REG_SOCK_TXBUF_SIZE(i), &v, 1), TAG, "tx buffer size");
  }
  v = W5500_MR_PB;
  ESP_RETURN_ON_ERROR(w_write(m, W5500_REG_MR, &v, 1), TAG, "write MR failed");
  v = 0;
  ESP_RETURN_ON_ERROR(w_write(m, W5500_REG_SIMR, &v, 1), TAG, "write SIMR failed");
  v = W5500_SMR_MAC_RAW | W5500_SMR_MAC_FILTER | W5500_SMR_MAC_BLOCK_MCAST;
  ESP_RETURN_ON_ERROR(w_write(m, W5500_REG_SOCK_MR(0), &v, 1), TAG, "write SMR failed");
  v = W5500_SIR_RECV;
  ESP_RETURN_ON_ERROR(w_write(m, W5500_REG_SOCK_IMR(0), &v, 1), TAG, "write SOCK0 IMR failed");
  ESP_RETURN_ON_ERROR(w_write16(m, W5500_REG_INTLEVEL, INT_ASSERT_WAIT), TAG, "write INTLEVEL failed");
  return ESP_OK;
}

/* --- esp_eth_mac_t methods ----------------------------------------------------------- */

/* Open socket 0 in MACRAW mode and start the data path from the chip's
 * current pointers. Called by esp_eth on link up. */
static esp_err_t w5500m_start(esp_eth_mac_t *mac) {
  w5500m_t *m = __containerof(mac, w5500m_t, parent);
  esp_err_t ret = ESP_OK;
  uint8_t wr[2];
  uint8_t simr = W5500_SIMR_SOCK0;
  xSemaphoreTake(m->op_lock, portMAX_DELAY);
  ESP_GOTO_ON_ERROR(w_cmd(m, W5500_SCR_OPEN), out, TAG, "issue OPEN command failed");
  ESP_GOTO_ON_ERROR(w_read(m, W5500_REG_SOCK_TX_WR(0), wr, 2), out, TAG, "read TX WR failed");
  m->tx_wr      = (uint16_t)((wr[0] << 8) | wr[1]);
  m->tx_pending = false;
  m->rx_have    = 0;
  m->tx_tail    = m->tx_head;
  ESP_GOTO_ON_ERROR(w_write(m, W5500_REG_SIMR, &simr, 1), out, TAG, "write SIMR failed");
  m->running = true;
out:
  xSemaphoreGive(m->op_lock);
  if (ret == ESP_OK) xTaskNotifyGive(m->task);
  return ret;
}

/* Stop the data path and close socket 0. Called by esp_eth on link down and
 * on deinit. Queued TX frames are dropped. */
static esp_err_t w5500m_stop(esp_eth_mac_t *mac) {
  w5500m_t *m = __containerof(mac, w5500m_t, parent);
  esp_err_t ret = ESP_OK;
  uint8_t simr = 0;
  xSemaphoreTake(m->op_lock, portMAX_DELAY);
  m->running = false;
  ESP_GOTO_ON_ERROR(w_write(m, W5500_REG_SIMR, &simr, 1), out, TAG, "write SIMR failed");
  ESP_GOTO_ON_ERROR(w_cmd(m, W5500_SCR_CLOSE), out, TAG, "issue CLOSE command failed");
out:
  m->tx_tail = m->tx_head;
  xSemaphoreGive(m->op_lock);
  return ret;
}

/* Store the esp_eth mediator (the callbacks into the generic driver). */
static esp_err_t w5500m_set_mediator(esp_eth_mac_t *mac, esp_eth_mediator_t *eth) {
  ESP_RETURN_ON_FALSE(eth, ESP_ERR_INVALID_ARG, TAG, "can't set mac's mediator to null");
  __containerof(mac, w5500m_t, parent)->eth = eth;
  return ESP_OK;
}

/* PHY register write - the W5500 has exactly one, PHYCFGR, among its common
 * registers. Used by IDF's W5500 PHY driver. */
static esp_err_t w5500m_write_phy_reg(esp_eth_mac_t *mac, uint32_t phy_addr, uint32_t phy_reg, uint32_t reg_value) {
  w5500m_t *m = __containerof(mac, w5500m_t, parent);
  ESP_RETURN_ON_FALSE(phy_reg == W5500_REG_PHYCFGR, ESP_FAIL, TAG, "wrong PHY register");
  const uint8_t v = (uint8_t)reg_value;
  return w_write(m, W5500_REG_PHYCFGR, &v, 1);
}

/* PHY register read, see w5500m_write_phy_reg(). */
static esp_err_t w5500m_read_phy_reg(esp_eth_mac_t *mac, uint32_t phy_addr, uint32_t phy_reg, uint32_t *reg_value) {
  w5500m_t *m = __containerof(mac, w5500m_t, parent);
  ESP_RETURN_ON_FALSE(reg_value, ESP_ERR_INVALID_ARG, TAG, "can't set reg_value to null");
  ESP_RETURN_ON_FALSE(phy_reg == W5500_REG_PHYCFGR, ESP_FAIL, TAG, "wrong PHY register");
  uint8_t v = 0;
  ESP_RETURN_ON_ERROR(w_read(m, W5500_REG_PHYCFGR, &v, 1), TAG, "read PHY register failed");
  *reg_value = v;
  return ESP_OK;
}

/* Set the chip's own MAC address. */
static esp_err_t w5500m_set_addr(esp_eth_mac_t *mac, uint8_t *addr) {
  ESP_RETURN_ON_FALSE(addr, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
  w5500m_t *m = __containerof(mac, w5500m_t, parent);
  memcpy(m->addr, addr, ETH_ADDR_LEN);
  return w_write(m, W5500_REG_MAC, m->addr, ETH_ADDR_LEN);
}

/* Return the MAC address last set. */
static esp_err_t w5500m_get_addr(esp_eth_mac_t *mac, uint8_t *addr) {
  ESP_RETURN_ON_FALSE(addr, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
  memcpy(addr, __containerof(mac, w5500m_t, parent)->addr, ETH_ADDR_LEN);
  return ESP_OK;
}

/* Read-modify-write one bit of Sn_MR (filter and multicast-block flags). */
static esp_err_t w5500m_smr_bit(w5500m_t *m, uint8_t bit, bool set) {
  uint8_t smr = 0;
  ESP_RETURN_ON_ERROR(w_read(m, W5500_REG_SOCK_MR(0), &smr, 1), TAG, "read SMR failed");
  smr = set ? (smr | bit) : (smr & ~bit);
  return w_write(m, W5500_REG_SOCK_MR(0), &smr, 1);
}

/* The W5500 has no multicast filter table, only "block IPv4 multicast"; the
 * bridge passes all multicast (set_all_multicast), so per-address filters
 * are not supported. */
static esp_err_t w5500m_add_mac_filter(esp_eth_mac_t *mac, uint8_t *addr) {
  return ESP_ERR_NOT_SUPPORTED;
}

/* See w5500m_add_mac_filter(). */
static esp_err_t w5500m_rm_mac_filter(esp_eth_mac_t *mac, uint8_t *addr) {
  return ESP_ERR_NOT_SUPPORTED;
}

/* Promiscuous mode = MAC filter off. */
static esp_err_t w5500m_set_promiscuous(esp_eth_mac_t *mac, bool enable) {
  return w5500m_smr_bit(__containerof(mac, w5500m_t, parent), W5500_SMR_MAC_FILTER, !enable);
}

/* All multicast = multicast block off. IPv6 multicast always passes. */
static esp_err_t w5500m_set_all_multicast(esp_eth_mac_t *mac, bool enable) {
  return w5500m_smr_bit(__containerof(mac, w5500m_t, parent), W5500_SMR_MAC_BLOCK_MCAST, !enable);
}

/* Link state from the PHY driver: open or close socket 0. */
static esp_err_t w5500m_set_link(esp_eth_mac_t *mac, eth_link_t link) {
  switch (link) {
  case ETH_LINK_UP:   return mac->start(mac);
  case ETH_LINK_DOWN: return mac->stop(mac);
  default:            return ESP_ERR_INVALID_ARG;
  }
}

/* Link speed from the PHY driver - only sets the SEND_OK timeout. */
static esp_err_t w5500m_set_speed(esp_eth_mac_t *mac, eth_speed_t speed) {
  w5500m_t *m = __containerof(mac, w5500m_t, parent);
  switch (speed) {
  case ETH_SPEED_10M:  m->tx_tmo_us = TX_TMO_10M_US;  return ESP_OK;
  case ETH_SPEED_100M: m->tx_tmo_us = TX_TMO_100M_US; return ESP_OK;
  default:             return ESP_ERR_INVALID_ARG;
  }
}

/* Duplex is handled by the PHY alone; accept both. */
static esp_err_t w5500m_set_duplex(esp_eth_mac_t *mac, eth_duplex_t duplex) {
  return (duplex == ETH_DUPLEX_HALF || duplex == ETH_DUPLEX_FULL) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

/* The W5500 supports neither flow control nor PAUSE frames. */
static esp_err_t w5500m_enable_flow_ctrl(esp_eth_mac_t *mac, bool enable) {
  return ESP_ERR_NOT_SUPPORTED;
}

/* See w5500m_enable_flow_ctrl(). */
static esp_err_t w5500m_set_peer_pause_ability(esp_eth_mac_t *mac, uint32_t ability) {
  return ESP_ERR_NOT_SUPPORTED;
}

/* esp_eth_mac_t::receive is part of the interface but unused: received
 * frames are pushed to esp_eth by the driver task (rx_deliver()). */
static esp_err_t w5500m_receive(esp_eth_mac_t *mac, uint8_t *buf, uint32_t *length) {
  *length = 0;
  return ESP_ERR_NOT_SUPPORTED;
}

/* Install the INT handler, reset and identify the chip, set up registers.
 * On failure everything done here is undone, so the bridge can retry at a
 * lower SPI clock with a fresh instance. */
static esp_err_t w5500m_init(esp_eth_mac_t *mac) {
  w5500m_t *m = __containerof(mac, w5500m_t, parent);
  esp_eth_mediator_t *eth = m->eth;
  esp_err_t ret = ESP_OK;

  gpio_config_t io = {0};
  io.pin_bit_mask = 1ULL << m->int_gpio;
  io.mode         = GPIO_MODE_INPUT;
  io.pull_up_en   = GPIO_PULLUP_ENABLE;
  io.intr_type    = GPIO_INTR_NEGEDGE;   /* INT is active low */
  gpio_config(&io);
  gpio_isr_handler_add(m->int_gpio, w5500m_isr, m);

  ESP_GOTO_ON_ERROR(eth->on_state_changed(eth, ETH_STATE_LLINIT, NULL), err, TAG, "lowlevel init failed");
  ESP_GOTO_ON_ERROR(w5500m_reset(m), err, TAG, "reset w5500 failed");
  ESP_GOTO_ON_ERROR(w5500m_verify_id(m), err, TAG, "verify chip ID failed");
  ESP_GOTO_ON_ERROR(w5500m_setup_default(m), err, TAG, "w5500 default setup failed");
  return ESP_OK;
err:
  gpio_isr_handler_remove(m->int_gpio);
  gpio_reset_pin(m->int_gpio);
  eth->on_state_changed(eth, ETH_STATE_DEINIT, NULL);
  return ret;
}

/* Stop, remove the INT handler, tell esp_eth. */
static esp_err_t w5500m_deinit(esp_eth_mac_t *mac) {
  w5500m_t *m = __containerof(mac, w5500m_t, parent);
  mac->stop(mac);
  gpio_isr_handler_remove(m->int_gpio);
  gpio_reset_pin(m->int_gpio);
  m->eth->on_state_changed(m->eth, ETH_STATE_DEINIT, NULL);
  return ESP_OK;
}

/* Free the instance. The bus device and task go with it. */
static esp_err_t w5500m_del(esp_eth_mac_t *mac) {
  w5500m_t *m = __containerof(mac, w5500m_t, parent);
  if (m->task) vTaskDelete(m->task);
  if (m->spi) spi_bus_remove_device(m->spi);
  if (m->spi_lock) vSemaphoreDelete(m->spi_lock);
  if (m->op_lock) vSemaphoreDelete(m->op_lock);
  if (m->tx_lock) vSemaphoreDelete(m->tx_lock);
  heap_caps_free(m->slots);
  heap_caps_free(m->rx_buf);
  free(m);
  return ESP_OK;
}

/* See w5500_mac.h. */
void w5500_mac_get_stats(esp_eth_mac_t *mac, w5500_mac_stats_t *out) {
  *out = __containerof(mac, w5500m_t, parent)->stats;
}

/* See w5500_mac.h. */
esp_eth_mac_t *w5500_mac_new(const eth_w5500_config_t *w5500_config,
                             const eth_mac_config_t *mac_config) {
  ESP_RETURN_ON_FALSE(w5500_config && mac_config && w5500_config->spi_devcfg &&
                      w5500_config->int_gpio_num >= 0 && w5500_config->poll_period_ms == 0,
                      NULL, TAG, "invalid argument (INT pin required, no poll mode)");
  w5500m_t *m = calloc(1, sizeof(w5500m_t));
  ESP_RETURN_ON_FALSE(m, NULL, TAG, "no mem for MAC instance");

  m->sw_reset_timeout_ms = mac_config->sw_reset_timeout_ms;
  m->int_gpio  = w5500_config->int_gpio_num;
  m->tx_tmo_us = TX_TMO_100M_US;

  m->parent.set_mediator           = w5500m_set_mediator;
  m->parent.init                   = w5500m_init;
  m->parent.deinit                 = w5500m_deinit;
  m->parent.start                  = w5500m_start;
  m->parent.stop                   = w5500m_stop;
  m->parent.del                    = w5500m_del;
  m->parent.write_phy_reg          = w5500m_write_phy_reg;
  m->parent.read_phy_reg           = w5500m_read_phy_reg;
  m->parent.set_addr               = w5500m_set_addr;
  m->parent.get_addr               = w5500m_get_addr;
  m->parent.add_mac_filter         = w5500m_add_mac_filter;
  m->parent.rm_mac_filter          = w5500m_rm_mac_filter;
  m->parent.set_speed              = w5500m_set_speed;
  m->parent.set_duplex             = w5500m_set_duplex;
  m->parent.set_link               = w5500m_set_link;
  m->parent.set_promiscuous        = w5500m_set_promiscuous;
  m->parent.set_all_multicast      = w5500m_set_all_multicast;
  m->parent.set_peer_pause_ability = w5500m_set_peer_pause_ability;
  m->parent.enable_flow_ctrl       = w5500m_enable_flow_ctrl;
  m->parent.transmit               = w5500m_transmit;
  m->parent.receive                = w5500m_receive;

  m->slots    = heap_caps_malloc(TX_SLOTS * sizeof(tx_slot_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  m->rx_buf   = heap_caps_aligned_alloc(4, RX_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  m->spi_lock = xSemaphoreCreateMutex();
  m->op_lock  = xSemaphoreCreateMutex();
  m->tx_lock  = xSemaphoreCreateMutex();
  if (!m->slots || !m->rx_buf || !m->spi_lock || !m->op_lock || !m->tx_lock) {
    ESP_LOGE(TAG, "no mem for buffers");
    goto err;
  }

  /* W5500 SPI frame: the 16-bit offset goes out as the command phase and the
   * control byte as the address phase. */
  spi_device_interface_config_t dev = *w5500_config->spi_devcfg;
  dev.command_bits = 16;
  dev.address_bits = 8;
  if (spi_bus_add_device(w5500_config->spi_host_id, &dev, &m->spi) != ESP_OK) {
    ESP_LOGE(TAG, "adding device to SPI host #%d failed", w5500_config->spi_host_id + 1);
    m->spi = NULL;
    goto err;
  }
  size_t bus_max = 0;
  m->rx_chunk = RX_CHUNK;
  if (spi_bus_get_max_transaction_len(w5500_config->spi_host_id, &bus_max) == ESP_OK &&
      bus_max < m->rx_chunk) {
    m->rx_chunk = bus_max & ~3u;
    ESP_LOGW(TAG, "SPI bus allows only %u bytes per transfer, RX batches limited",
             (unsigned)bus_max);
  }

  const BaseType_t core = (mac_config->flags & ETH_MAC_FLAG_PIN_TO_CORE)
                          ? (BaseType_t)esp_cpu_get_core_id() : tskNO_AFFINITY;
  if (xTaskCreatePinnedToCore(w5500m_task, "w5500_tsk", mac_config->rx_task_stack_size, m,
                              mac_config->rx_task_prio, &m->task, core) != pdPASS) {
    ESP_LOGE(TAG, "create w5500 task failed");
    m->task = NULL;
    goto err;
  }
  return &m->parent;

err:
  w5500m_del(&m->parent);
  return NULL;
}

#endif /* CONFIG_ETH_SPI_ETHERNET_W5500 */
