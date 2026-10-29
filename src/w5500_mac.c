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
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "w5500_regs.h"
#include "soc/soc_caps.h"

/* Segmented transfers (SCT): the SPI master runs several transactions from
 * one DMA list, reloading the peripheral's configuration between them
 * itself, with a single interrupt at the end. The RX chain uses it (see
 * rx_fetch()). ESP-IDF provides it through a private API on chips whose
 * soc_caps.h enables it - on the ESP32-C5 only with the backport applied by
 * scripts/idf_sct_backport.py. */
#if SOC_SPI_SCT_SUPPORTED
#define W5500_SCT 1
#include "esp_clk_tree.h"
#include "esp_private/spi_master_internal.h"
#include "hal/spi_hal.h"
#else
#define W5500_SCT 0
#endif

static const char *TAG = "w5500m";

/* TX ring: bytes transmit() can queue ahead of the chip. Frames are packed
 * (4-byte length header, data padded to 4 bytes), so 16 KB hold ~10
 * full-size frames or a few hundred TCP ACKs - fixed 1.5 KB slots would
 * waste most of their space on ACKs. A Wi-Fi A-MPDU burst can be larger;
 * that is handled by making transmit() wait (TX_FULL_WAIT_MS), not by ring
 * size: on the C5 free heap is the scarcer resource under load, and the
 * Wi-Fi driver fails allocations when it runs low. Must be a power of two
 * (free-running head/tail byte offsets). */
#define TX_RING_SIZE       16384
#define TX_RING_SKIP       0xFFFFFFFFu   /* header: rest of the ring unused, wrap */

/* How long transmit() waits for room when the ring is full before it drops
 * the frame. A download from Wi-Fi arrives in A-MPDU bursts larger than the
 * ring (32-64 full frames); dropping the overflow makes TCP back off hard,
 * while making the Wi-Fi task wait lets the lower-priority driver task run
 * and drain the ring - the surplus frames stay in the Wi-Fi driver's own RX
 * buffers meanwhile, and the sender is slowed by flow control instead of
 * loss. Only the wait for a hung chip is bounded by this. */
#define TX_FULL_WAIT_MS    20

/* Bytes fetched from RX memory per round at most: the whole socket RX
 * memory, so a backlog is taken in one round and the per-round work (task
 * wake-up, queueing the chain, the size read) is shared by up to ten full
 * frames. Multiple of 4 (DMA alignment). Capped at runtime to what the SPI bus was
 * set up for (rx_chunk). */
#define RX_CHUNK           W5500_MAC_MAX_TRANSFER

/* A round's read is split into pieces of this size, queued to the SPI
 * driver at once together with the release of each piece (see rx_fetch()):
 * the chip gets each piece's memory back right after it was read, and while
 * the DMA fetches the later pieces, the frames of the pieces already in are
 * handed to Wi-Fi, so the Wi-Fi task's work on them overlaps the transfer.
 * Two full frames per piece. Each piece costs three transactions (read,
 * RX_RD write, RECV), and every transaction ~10 us of SPI ISR work in which
 * the bus is idle - smaller pieces release earlier but drain slower.
 * Multiple of 4. */
#define RX_PIECE           3040
#define RX_PIECES_MAX      ((RX_CHUNK + RX_PIECE - 1) / RX_PIECE)

/* Transactions in one RX chain (see rx_fetch()): read, RX_RD write and
 * RECV per piece, plus the final size read. The SPI device queue must hold
 * them all. */
#define RX_CHAIN_MAX       (3 * RX_PIECES_MAX + 1)

/* rx_buf: a whole RX_CHUNK behind the kept head of a cut-off frame (at most
 * one maximum-size frame plus its header) and up to 3 bytes of alignment. */
#define RX_BUF_SIZE        (RX_CHUNK + 2 + ETH_MAX_PACKET_SIZE + 3)

/* Frames carried from one RX round into the next (see rx_service()): as
 * many minimum-size frames as a whole RX_CHUNK can hold. */
#define RX_CARRY_MAX       (RX_CHUNK / (2 + ETH_MIN_PACKET_SIZE - ETH_CRC_LEN) + 1)

/* How long frames may still be delivered while RX pieces hold the SPI bus
 * (see rx_may_deliver()); well below SPI_LOCK_TIMEOUT_MS. */
#define RX_INFLIGHT_DELIVER_US 1000

/* Largest RX chain while frames wait in the TX ring (see rx_service()). */
#define RX_SHARED_CHUNK    (2 * RX_PIECE)

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

/* An RX round that finds only an incomplete frame waits this long before
 * looking again - a full frame is stored in ~120 us at 100 Mbit/s. A
 * scheduler tick (10 ms at CONFIG_FREERTOS_HZ=100) would stall reception
 * far longer than the frame needs. */
#define RX_STALL_WAIT_US   30

/* Consecutive incomplete rounds before the buffer is assumed corrupt and
 * resynced: ~RX_STALL_MAX * (RX_STALL_WAIT_US + one register read), several
 * milliseconds - far more than any frame needs, so this only triggers on a
 * genuinely broken length. */
#define RX_STALL_MAX       200

/* SCT: per RX piece one segmented transfer of up to four segments (read,
 * RX_RD write, RECV, and on the last piece the size read). */
#define SCT_SEGS_MAX       4

/* SPI clocks with CS inactive between two segments of an SCT, on top of the
 * time the peripheral needs to load the next segment's configuration by
 * DMA. The W5500 only needs CS high for a few ns between frames. */
#define SCT_GAP_CLOCKS     0

typedef struct {
  esp_eth_mac_t       parent;
  esp_eth_mediator_t *eth;
  spi_device_handle_t spi;
  SemaphoreHandle_t   spi_lock;    /* one SPI transaction at a time            */
  SemaphoreHandle_t   op_lock;     /* data path vs. start/stop                 */
  SemaphoreHandle_t   tx_lock;     /* producers of the TX ring                 */
  SemaphoreHandle_t   tx_space;    /* given when the driver task frees ring room */
  TaskHandle_t        task;
  int                 int_gpio;
  uint32_t            sw_reset_timeout_ms;
  uint8_t             addr[ETH_ADDR_LEN];
  volatile bool       running;     /* socket 0 open, data path live            */

  /* TX ring: producers append at tx_head, the driver task consumes at
   * tx_tail (free-running byte offsets, see TX_RING_SIZE). An entry never
   * wraps: one that does not fit before the end is preceded by a
   * TX_RING_SKIP header and starts at offset 0. */
  uint8_t            *tx_ring;
  volatile uint32_t   tx_head;
  volatile uint32_t   tx_tail;
  uint16_t            tx_wr;       /* Sn_TX_WR as last committed               */
  bool                tx_pending;  /* a SEND is in flight, SEND_OK not yet seen */
  uint32_t            tx_tmo_us;
  int64_t             tx_sent_us;      /* when the frame in flight got its SEND */
  uint32_t            tx_sent_len;     /* its length                            */
  uint32_t            tx_ns_per_byte;  /* wire time per byte at the link speed  */
  bool                full_duplex;     /* link duplex, from the PHY driver      */
  int64_t             last_cmd_us;

  /* RX: the chip's RX memory is released up to rx_rd. The head of a frame
   * cut off at the end of the last fetch - already released on the chip -
   * is kept in rx_buf[rx_base, rx_base + rx_have) until the rest arrives.
   * rx_rsr is the received size read at the end of the last fetch, -1 if
   * it has to be read again. */
  uint8_t            *rx_buf;
  uint32_t            rx_chunk;    /* bytes per read: RX_CHUNK or the bus limit */
  uint32_t            rx_base;
  uint32_t            rx_have;
  uint16_t            rx_rd;
  int32_t             rx_rsr;
  volatile bool       rx_inflight; /* queued RX pieces pending, see rx_fetch() */
  spi_transaction_t   rx_t[RX_CHAIN_MAX];
  uint8_t            *carry[RX_CARRY_MAX];      /* frames for the next round */
  uint16_t            carry_len[RX_CARRY_MAX];
  uint16_t            carry_n, carry_done;      /* carry[carry_done..carry_n) */
  volatile uint32_t   tx_blocked;  /* producers waiting in transmit() for room */

  bool                half_duplex; /* SPI device in half-duplex mode         */
#if W5500_SCT
  /* SCT mode for the RX chain (see rx_fetch()): usable at all, the dummy
   * clocks a read needs (the SPI driver adds them itself only outside SCT),
   * the segments, and the DMA source/target buffers of the small register
   * accesses - SCT takes buffers as given, so they must be in RAM and stay
   * valid until the chain is done. */
  bool                sct;
  uint8_t             sct_dummy;
  spi_multi_transaction_t sct_t[RX_PIECES_MAX][SCT_SEGS_MAX];
  uint8_t             sct_rd[RX_PIECES_MAX][2];
  uint8_t             sct_recv[2];
  uint8_t             sct_rsr[4] __attribute__((aligned(4)));
#endif
  w5500_mac_stats_t   stats;
} w5500m_t;

/* --- SPI access ------------------------------------------------------------ */

/* One W5500 SPI frame: 16-bit offset (sent as the SPI command phase), 8-bit
 * control byte (block select, read/write, variable length - the SPI address
 * phase), then 'len' data bytes. Up to 4 bytes travel in the transaction
 * itself; longer buffers go by DMA. Reads into 'data' with len > 4 need a
 * 4-byte aligned buffer, or the SPI driver allocates a bounce buffer. */
static void w_setup(const w5500m_t *m, spi_transaction_t *t, uint32_t address, bool write,
                    void *data, uint32_t len) {
  memset(t, 0, sizeof(*t));
  t->cmd    = (uint16_t)(address >> W5500_ADDR_OFFSET);
  t->addr   = (address & 0xFFFF) |
              ((write ? W5500_ACCESS_MODE_WRITE : W5500_ACCESS_MODE_READ) << W5500_RWB_OFFSET) |
              W5500_SPI_OP_MODE_VDM;
  t->length = 8 * len;
  if (len <= 4) {
    if (write) { t->flags = SPI_TRANS_USE_TXDATA; memcpy(t->tx_data, data, len); }
    else       { t->flags = SPI_TRANS_USE_RXDATA; }
  } else if (write) {
    t->tx_buffer = data;
  } else {
    t->rx_buffer = data;
  }
  if (!write && m->half_duplex) {
    /* Half-duplex: a read has no MOSI data phase, only the MISO one - this
     * is what lets the SPI driver put its compensation dummy clocks in
     * front of it (see SPI_DEVICE_HALFDUPLEX in w5500_mac.h). */
    t->length   = 0;
    t->rxlength = 8 * len;
  }
}

/* Run one W5500 SPI frame (see w_setup()) under spi_lock. Transfers above
 * BIG_XFER wait for the SPI interrupt, smaller ones are polled. */
static esp_err_t w_xfer(w5500m_t *m, uint32_t address, bool write, void *data, uint32_t len) {
  spi_transaction_t t;
  w_setup(m, &t, address, write, data, len);

  if (xSemaphoreTake(m->spi_lock, pdMS_TO_TICKS(SPI_LOCK_TIMEOUT_MS)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  const int64_t t0 = esp_timer_get_time();
  const esp_err_t e = len > BIG_XFER ? spi_device_transmit(m->spi, &t)
                                     : spi_device_polling_transmit(m->spi, &t);
  m->stats.spi_us += (uint32_t)(esp_timer_get_time() - t0);
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

/* Margin on top of the wire time of the frame in flight after which its
 * SEND is taken as complete without reading Sn_IR (see
 * tx_wait_send_done()): covers the chip fetching the frame from TX memory
 * and the inter-frame gap. */
#define TX_DONE_MARGIN_US  15

/* Wait for SEND_OK of the frame in flight. On a full-duplex link a frame
 * leaves at a fixed rate, so once its wire time (plus TX_DONE_MARGIN_US) has
 * passed since the SEND it is out, and the Sn_IR read - one SPI transaction
 * per frame - is skipped; the SEND_OK bit is still cleared together with the
 * next SEND. Half duplex (collisions, back-off) always reads. On timeout the
 * frame is written off (the link is probably gone) so the next one is not
 * blocked forever. */
static bool tx_wait_send_done(w5500m_t *m) {
  if (m->full_duplex &&
      esp_timer_get_time() - m->tx_sent_us >
          (int64_t)((m->tx_sent_len + 24) * m->tx_ns_per_byte / 1000) + TX_DONE_MARGIN_US) {
    m->stats.tx_done_skipped++;
    return true;
  }
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
    const uint32_t pos = m->tx_tail % TX_RING_SIZE;
    const uint32_t hdr = *(const uint32_t *)(m->tx_ring + pos);
    if (hdr == TX_RING_SKIP) {         /* wrap marker: next entry is at 0 */
      m->tx_tail = m->tx_tail + (TX_RING_SIZE - pos);
      n--;
      continue;
    }
    const uint16_t len = (uint16_t)hdr;
    const uint8_t *data = m->tx_ring + pos + 4;

    bool ok = w_write(m, W5500_MEM_SOCK_TX(0, (uint32_t)m->tx_wr), data, len) == ESP_OK;
    if (ok && m->tx_pending) ok = tx_wait_send_done(m);
    if (ok) {
      const uint16_t wr = (uint16_t)(m->tx_wr + len);
      static const uint8_t send[2] = { W5500_SCR_SEND, W5500_SIR_SEND };
      ok = w_write16(m, W5500_REG_SOCK_TX_WR(0), wr) == ESP_OK &&
           w_cmd_fast(m, send, sizeof(send)) == ESP_OK;
      if (ok) {
        m->tx_wr = wr;
        m->tx_pending = true;
        m->tx_sent_us = esp_timer_get_time();
        m->tx_sent_len = len;
        m->stats.tx_frames++;
      }
    }
    __sync_synchronize();          /* entry consumed before its space is released */
    m->tx_tail = m->tx_tail + 4 + ((len + 3u) & ~3u);
    /* Wake a producer waiting for room only once half the ring is free (or
     * it is empty): it has the higher priority, so waking it per freed
     * frame would switch tasks for every single frame. */
    if (m->tx_tail == m->tx_head ||
        TX_RING_SIZE - (m->tx_head - m->tx_tail) >= TX_RING_SIZE / 2) {
      xSemaphoreGive(m->tx_space);
    }
  }
}

/* esp_eth_mac_t::transmit - queue a frame for the driver task. Never touches
 * the SPI bus, so the caller (the Wi-Fi RX path in the bridge) is normally
 * held up only for one memcpy. If the ring is full it waits for room, up to
 * TX_FULL_WAIT_MS (see there), and returns ESP_ERR_NO_MEM only after that. */
static esp_err_t w5500m_transmit(esp_eth_mac_t *mac, uint8_t *buf, uint32_t length) {
  w5500m_t *m = __containerof(mac, w5500m_t, parent);
  ESP_RETURN_ON_FALSE(length <= ETH_MAX_PACKET_SIZE, ESP_ERR_INVALID_ARG, TAG,
                      "frame too big (%" PRIu32 ")", length);
  if (!m->running) return ESP_ERR_INVALID_STATE;

  const uint32_t need = 4 + ((length + 3u) & ~3u);
  const TickType_t t0 = xTaskGetTickCount();
  const TickType_t wait = pdMS_TO_TICKS(TX_FULL_WAIT_MS) ? pdMS_TO_TICKS(TX_FULL_WAIT_MS) : 1;
  uint32_t head, pos, skip;
  for (;;) {
    xSemaphoreTake(m->tx_lock, portMAX_DELAY);
    head = m->tx_head;
    pos = head % TX_RING_SIZE;
    const uint32_t tail_room = TX_RING_SIZE - pos;        /* bytes before the end */
    skip = tail_room < need ? tail_room : 0;              /* wrap if it does not fit */
    if (TX_RING_SIZE - (head - m->tx_tail) >= skip + need) break;
    xSemaphoreGive(m->tx_lock);

    /* Full: let the driver task drain, then look again. */
    const TickType_t spent = xTaskGetTickCount() - t0;
    bool room = false;
    if (m->running && spent < wait) {
      __atomic_add_fetch(&m->tx_blocked, 1, __ATOMIC_SEQ_CST);
      room = xSemaphoreTake(m->tx_space, wait - spent) == pdTRUE;
      __atomic_sub_fetch(&m->tx_blocked, 1, __ATOMIC_SEQ_CST);
    }
    if (!room) {
      m->stats.tx_ring_full++;
      return ESP_ERR_NO_MEM;
    }
    m->stats.tx_waits++;
  }
  if (skip) {
    *(uint32_t *)(m->tx_ring + pos) = TX_RING_SKIP;
    head += skip;
  }
  uint8_t *e = m->tx_ring + head % TX_RING_SIZE;
  *(uint32_t *)e = length;
  memcpy(e + 4, buf, length);
  __sync_synchronize();            /* entry visible before publishing it */
  m->tx_head = head + need;
  xSemaphoreGive(m->tx_lock);

  xTaskNotifyGive(m->task);
  return ESP_OK;
}

/* --- Receive ----------------------------------------------------------------- */

/* Copy one received frame out of rx_buf into its own heap buffer - esp_eth
 * takes ownership of it. NULL (counted as rx_no_mem) without heap. */
static uint8_t *rx_copy(w5500m_t *m, const uint8_t *frame, uint32_t len) {
  uint8_t *buf = malloc(len);
  if (!buf) { m->stats.rx_no_mem++; return NULL; }
  memcpy(buf, frame, len);
  return buf;
}

/* Hand one frame copied by rx_copy() to esp_eth (-> the bridge's input
 * callback, which sends it to Wi-Fi and frees it). */
static void rx_push(w5500m_t *m, uint8_t *buf, uint32_t len) {
  const int64_t t0 = esp_timer_get_time();
  m->eth->stack_input(m->eth, buf, len);
  m->stats.deliver_us += (uint32_t)(esp_timer_get_time() - t0);
  m->stats.rx_frames++;
}

/* Whether a frame may be delivered while RX pieces are still queued on the
 * SPI bus (rx_inflight, spi_lock held). Delivering then overlaps the Wi-Fi
 * work with the DMA, but nothing drains the TX ring meanwhile: once a
 * producer waits in transmit() for ring room - the Wi-Fi task, which this
 * delivery may in turn wait for - delivery waits until the pieces are in.
 * The time bound keeps spi_lock short for other users of the bus (PHY
 * register reads) when Wi-Fi makes deliveries wait. Carried frames go
 * first, so frames stay in order. */
static bool rx_may_deliver(const w5500m_t *m, int64_t t0) {
  return m->tx_tail == m->tx_head && !m->tx_blocked &&
         esp_timer_get_time() - t0 < RX_INFLIGHT_DELIVER_US;
}

/* Deliver the carried frames (see rx_service()), oldest first. With
 * 'inflight' only as long as rx_may_deliver() allows; the rest stays. */
static void rx_flush(w5500m_t *m, bool inflight, int64_t t0) {
  while (m->carry_done < m->carry_n) {
    if (inflight && !rx_may_deliver(m, t0)) return;
    const uint16_t i = m->carry_done++;
    rx_push(m, m->carry[i], m->carry_len[i]);
  }
  m->carry_n = m->carry_done = 0;
}

/* Drop the carried frames - the data path stopped. */
static void rx_carry_drop(w5500m_t *m) {
  while (m->carry_done < m->carry_n) free(m->carry[m->carry_done++]);
  m->carry_n = m->carry_done = 0;
}

/* Discard everything in RX memory - used when a length header is implausible
 * or a frame never completes, because from a corrupt header on there is no
 * way to find the next frame. Frames already carried are complete and are
 * delivered first. */
static void rx_resync(w5500m_t *m) {
  static const uint8_t recv = W5500_SCR_RECV;
  uint8_t r[4];
  rx_flush(m, false, 0);
  m->stats.rx_bad_len++;
  m->rx_have = 0;
  m->rx_rsr = -1;
  if (w_read(m, W5500_REG_SOCK_RX_RSR(0), r, sizeof(r)) != ESP_OK) return;
  const uint32_t rsr = ((uint32_t)r[0] << 8) | r[1];
  m->rx_rd = (uint16_t)((((uint16_t)r[2] << 8) | r[3]) + rsr);
  if (w_write16(m, W5500_REG_SOCK_RX_RD(0), m->rx_rd) == ESP_OK) w_cmd_fast(m, &recv, 1);
  ESP_LOGW(TAG, "corrupt RX frame header, dropped %" PRIu32 " bytes", rsr);
}

typedef enum { RX_IDLE, RX_PROGRESS, RX_STALLED } rx_result_t;

/* What rx_take() does with a complete frame: deliver it now, deliver it now
 * if rx_may_deliver() allows (stop otherwise), or copy it to the carry list
 * for delivery during the next round's DMA. */
typedef enum { TAKE_NOW, TAKE_INFLIGHT, TAKE_CARRY } rx_take_t;

/* Take every complete frame in rx_buf[*off..avail) and advance *off past
 * them (a frame that fails rx_copy() is dropped but still consumed).
 * Returns false on an implausible length header (the caller must resync),
 * true otherwise - including when the last frame is still cut off at
 * 'avail' or TAKE_INFLIGHT had to stop. */
static bool rx_take(w5500m_t *m, uint32_t *off, uint32_t avail, rx_take_t how, int64_t t0) {
  while (*off + 2 <= avail) {
    const uint32_t flen = ((uint32_t)m->rx_buf[*off] << 8) | m->rx_buf[*off + 1];  /* incl. header */
    if (flen < 2 + ETH_MIN_PACKET_SIZE - ETH_CRC_LEN || flen > 2 + ETH_MAX_PACKET_SIZE) return false;
    if (*off + flen > avail) break;
    if (how == TAKE_INFLIGHT && (m->carry_done < m->carry_n || !rx_may_deliver(m, t0))) break;
    const uint32_t len = flen - 2;
    uint8_t *buf = rx_copy(m, m->rx_buf + *off + 2, len);
    if (buf && how == TAKE_CARRY && m->carry_n < RX_CARRY_MAX) {
      m->carry[m->carry_n] = buf;
      m->carry_len[m->carry_n++] = (uint16_t)len;
    } else if (buf) {
      rx_push(m, buf, len);
    }
    *off += flen;
  }
  return true;
}

/* Queue one transaction of the RX chain (see rx_fetch()). */
static esp_err_t rx_queue(w5500m_t *m, uint32_t *queued, uint32_t address, bool write,
                          void *data, uint32_t len) {
  spi_transaction_t *t = &m->rx_t[*queued];
  w_setup(m, t, address, write, data, len);
  const esp_err_t e = spi_device_queue_trans(m->spi, t, portMAX_DELAY);
  if (e == ESP_OK) (*queued)++;
  return e;
}

#if W5500_SCT
/* Fill one SCT segment with a W5500 SPI frame - the same frame as
 * w_setup(), but always with a DMA buffer, and with the read compensation
 * dummy clocks set explicitly (SCT does not apply the device's timing). */
static void sct_seg(const w5500m_t *m, spi_multi_transaction_t *s, uint32_t address, bool write,
                    void *data, uint32_t len) {
  memset(s, 0, sizeof(*s));
  s->base.cmd  = (uint16_t)(address >> W5500_ADDR_OFFSET);
  s->base.addr = (address & 0xFFFF) |
                 ((write ? W5500_ACCESS_MODE_WRITE : W5500_ACCESS_MODE_READ) << W5500_RWB_OFFSET) |
                 W5500_SPI_OP_MODE_VDM;
  s->sct_gap_len = SCT_GAP_CLOCKS;
  if (write) {
    s->base.length    = 8 * len;
    s->base.tx_buffer = data;
  } else {
    s->base.rxlength  = 8 * len;
    s->base.rx_buffer = data;
    s->seg_trans_flags = SPI_MULTI_TRANS_DUMMY_LEN_UPDATED;
    s->dummy_bits      = m->sct_dummy;
  }
}

/* Queue the RX chain of rx_fetch() as one SCT per piece: read, RX_RD write,
 * RECV - and on the last piece also the size read into sct_rsr. Each SCT
 * costs one SPI interrupt instead of one per transaction, and the pieces
 * still complete one by one, so their frames can be delivered while the
 * later pieces transfer. The first segment's user field holds the end of
 * the piece in rx_buf. Counts the queued SCTs in *queued; *size_read tells
 * whether the last one includes the size read. */
static esp_err_t rx_queue_sct(w5500m_t *m, uint16_t rd, uint32_t dst, uint32_t want,
                              uint32_t *queued, bool *size_read) {
  m->sct_recv[0] = W5500_SCR_RECV;
  m->sct_recv[1] = W5500_SIR_RECV;
  esp_err_t e = ESP_OK;
  for (uint32_t pos = 0; pos < want && e == ESP_OK; pos += RX_PIECE) {
    const uint32_t k = *queued;
    const uint32_t n = want - pos < RX_PIECE ? want - pos : RX_PIECE;
    const bool last = pos + n >= want;
    const uint16_t nrd = (uint16_t)(rd + pos + n);
    spi_multi_transaction_t *s = m->sct_t[k];
    m->sct_rd[k][0] = (uint8_t)(nrd >> 8);
    m->sct_rd[k][1] = (uint8_t)nrd;
    sct_seg(m, &s[0], W5500_MEM_SOCK_RX(0, (uint32_t)(uint16_t)(rd + pos)), false,
            m->rx_buf + dst + pos, n);
    s[0].base.user = (void *)(uintptr_t)(dst + pos + n);
    sct_seg(m, &s[1], W5500_REG_SOCK_RX_RD(0), true, m->sct_rd[k], 2);
    sct_seg(m, &s[2], W5500_REG_SOCK_CR(0), true, m->sct_recv, last ? 2 : 1);
    uint32_t segs = 3;
    if (last) sct_seg(m, &s[segs++], W5500_REG_SOCK_RX_RSR(0), false, m->sct_rsr, 4);
    e = spi_device_queue_multi_trans(m->spi, s, segs, portMAX_DELAY);
    if (e == ESP_OK) {
      (*queued)++;
      *size_read = last;
    }
  }
  return e;
}

/* Check once whether the RX chain can run in SCT mode, and work out the
 * dummy clocks a read needs: the same number the SPI driver puts in front
 * of a half-duplex read for the device's input_delay_ns. SCT requires a
 * half-duplex device, and a descriptor pool large enough for a whole chain
 * (W5500_MAC_BUS_MAX_TRANSFER). */
static bool sct_probe(w5500m_t *m, const spi_device_interface_config_t *dev, size_t bus_max) {
  const uint32_t descs_needed = 5 * RX_PIECES_MAX + 1;   /* TX side: conf + data per segment */
  if (!m->half_duplex || bus_max / LLDESC_MAX_NUM_PER_DESC < descs_needed) return false;
  int khz = 0;
  uint32_t src_hz = 0;
  const soc_module_clk_t src = dev->clock_source ? (soc_module_clk_t)dev->clock_source
                                                 : (soc_module_clk_t)SPI_CLK_SRC_DEFAULT;
  if (spi_device_get_actual_freq(m->spi, &khz) != ESP_OK ||
      esp_clk_tree_src_get_freq_hz(src, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &src_hz) != ESP_OK ||
      khz <= 0) {
    return false;
  }
  int dummy = 0, miso_delay = 0;
  spi_hal_cal_timing((int)src_hz, khz * 1000, true, dev->input_delay_ns, &dummy, &miso_delay);
  if (dummy < 0 || dummy > 4) return false;
  if (spi_bus_multi_trans_mode_enable(m->spi, true) != ESP_OK) return false;
  spi_bus_multi_trans_mode_enable(m->spi, false);
  m->sct_dummy = (uint8_t)dummy;
  ESP_LOGI(TAG, "RX chain in SCT mode (%d dummy clock(s) per read)", dummy);
  return true;
}
#endif

/* The RX chain of rx_fetch() as single queued transactions: per piece a
 * read, an RX_RD write and a RECV, then the size read into r. Delivers
 * frames while the chain runs (see rx_fetch()). */
static esp_err_t rx_chain_trans(w5500m_t *m, uint16_t rd, uint32_t dst, uint32_t want,
                                uint32_t *off, bool *bad, int64_t t0, uint8_t r[4],
                                bool *size_read) {
  esp_err_t e = ESP_OK;
  uint32_t queued = 0, pieces = 0;
  for (uint32_t pos = 0; pos < want && e == ESP_OK; pos += RX_PIECE) {
    const uint32_t n = want - pos < RX_PIECE ? want - pos : RX_PIECE;
    const bool last = pos + n >= want;
    const uint16_t nrd = (uint16_t)(rd + pos + n);
    const uint8_t be[2] = { (uint8_t)(nrd >> 8), (uint8_t)nrd };
    const uint8_t recv[2] = { W5500_SCR_RECV, W5500_SIR_RECV };
    e = rx_queue(m, &queued, W5500_MEM_SOCK_RX(0, (uint32_t)(uint16_t)(rd + pos)), false,
                 m->rx_buf + dst + pos, n);
    if (e == ESP_OK) {
      m->rx_t[queued - 1].user = (void *)(uintptr_t)(dst + pos + n);
      pieces++;
      e = rx_queue(m, &queued, W5500_REG_SOCK_RX_RD(0), true, (void *)be, 2);
    }
    if (e == ESP_OK) e = rx_queue(m, &queued, W5500_REG_SOCK_CR(0), true, (void *)recv, last ? 2 : 1);
  }
  *size_read = e == ESP_OK &&
               rx_queue(m, &queued, W5500_REG_SOCK_RX_RSR(0), false, r, 4) == ESP_OK;
  m->rx_inflight = queued > 0;
  if (queued) rx_flush(m, true, t0);
  uint32_t piece = 0;
  for (uint32_t i = 0; i < queued; i++) {
    spi_transaction_t *done;
    const esp_err_t res = spi_device_get_trans_result(m->spi, &done, portMAX_DELAY);
    if (res != ESP_OK) { e = res; continue; }
    if (done->user == NULL) {                   /* register write, or the size read */
      if (*size_read && i + 1 == queued) memcpy(r, done->rx_data, 4);
      continue;
    }
    const uint32_t pend = (uint32_t)(uintptr_t)done->user;
    if (done->flags & SPI_TRANS_USE_RXDATA) {   /* <= 4 bytes came in rx_data */
      const uint32_t n = (done->rxlength ? done->rxlength : done->length) / 8;
      memcpy(m->rx_buf + pend - n, done->rx_data, n);
    }
    if (++piece < pieces && e == ESP_OK && !*bad) {
      *bad = !rx_take(m, off, pend, TAKE_INFLIGHT, t0);
    }
  }
  return e;
}

#if W5500_SCT
/* The same chain as rx_chain_trans(), as one SCT per piece (see
 * rx_queue_sct()). The host is switched into SCT mode only for the chain:
 * in SCT mode it can run nothing else, and every other access of this
 * driver is a polled or single transaction. Switching costs a few register
 * writes, no bus time. */
static esp_err_t rx_chain_sct(w5500m_t *m, uint16_t rd, uint32_t dst, uint32_t want,
                              uint32_t *off, bool *bad, int64_t t0, uint8_t r[4],
                              bool *size_read) {
  esp_err_t e = spi_bus_multi_trans_mode_enable(m->spi, true);
  if (e != ESP_OK) return e;
  uint32_t queued = 0;
  e = rx_queue_sct(m, rd, dst, want, &queued, size_read);
  *size_read = *size_read && e == ESP_OK;
  m->rx_inflight = queued > 0;
  if (queued) rx_flush(m, true, t0);
  for (uint32_t i = 0; i < queued; i++) {
    spi_multi_transaction_t *done;
    const esp_err_t res = spi_device_get_multi_trans_result(m->spi, &done, portMAX_DELAY);
    if (res != ESP_OK) { e = res; continue; }
    if (i + 1 < queued && e == ESP_OK && !*bad) {
      *bad = !rx_take(m, off, (uint32_t)(uintptr_t)done->base.user, TAKE_INFLIGHT, t0);
    }
  }
  if (*size_read) memcpy(r, m->sct_rsr, 4);
  spi_bus_multi_trans_mode_enable(m->spi, false);
  return e;
}
#endif

/* Fetch 'want' bytes of RX memory from 'rd' into rx_buf at 'dst', release
 * them on the chip as they come in, and read the received size for the
 * next round - all as one chain of queued SPI transfers that the SPI ISR
 * runs back to back, without the task in between:
 *
 *   per RX_PIECE: read the piece, write Sn_RX_RD past it, RECV
 *   (the last RECV also clears Sn_IR.RECV), then read Sn_RX_RSR + Sn_RX_RD.
 *
 * With SCT (rx_chain_sct()) each piece is one segmented transfer with one
 * interrupt, otherwise (rx_chain_trans()) each of these is a transaction of
 * its own with an interrupt each.
 *
 * Releasing each piece right after it is read gives the chip its memory
 * back while a burst is still arriving, instead of after the whole round.
 * The release is byte-granular: a frame cut off at the end of 'want' is
 * released too and its head kept in rx_buf (the chip's RX memory is a plain
 * byte ring; the frame structure is only the driver's). Clearing the
 * interrupt before the final size read keeps the rule that no frame stored
 * after the read can go unnoticed.
 *
 * While the chain runs, the frames carried from the previous round are
 * delivered, and after each finished piece but the last its frames too
 * (rx_take(TAKE_INFLIGHT), advancing *off) - so the Wi-Fi task's work on
 * them overlaps the transfer. spi_lock is held while transfers are
 * queued, and rx_inflight tells w5500_mac_pump_tx() that the bus is busy
 * (the SPI driver allows no polled transaction while queued ones are
 * pending, and a second user would pick up the wrong results). Carried
 * frames that could not go during the chain are delivered after the lock
 * is released. *bad is set on an implausible header; delivery stops there,
 * but the whole chain is still collected. On success m->rx_rd is past the
 * fetched bytes and m->rx_rsr holds the size read at the end (or -1). */
static esp_err_t rx_fetch(w5500m_t *m, uint16_t rd, uint32_t dst, uint32_t want,
                          uint32_t *off, bool *bad) {
  if (xSemaphoreTake(m->spi_lock, pdMS_TO_TICKS(SPI_LOCK_TIMEOUT_MS)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  /* The chain's RECVs go to Sn_CR without the read-back of w_cmd_fast():
   * keep CMD_SETTLE_US after the last command (a SEND from tx_service())
   * so a RECV cannot overwrite a command the chip has not taken yet. */
  const int64_t since = esp_timer_get_time() - m->last_cmd_us;
  if (since < CMD_SETTLE_US) esp_rom_delay_us((uint32_t)(CMD_SETTLE_US - since));
  const int64_t t0 = esp_timer_get_time();
  const uint32_t d0 = m->stats.deliver_us;
  uint8_t r[4] = {0};
  bool size_read = false;
#if W5500_SCT
  const esp_err_t e = m->sct ? rx_chain_sct(m, rd, dst, want, off, bad, t0, r, &size_read)
                             : rx_chain_trans(m, rd, dst, want, off, bad, t0, r, &size_read);
#else
  const esp_err_t e = rx_chain_trans(m, rd, dst, want, off, bad, t0, r, &size_read);
#endif
  m->rx_inflight = false;
  m->last_cmd_us = esp_timer_get_time();
  m->stats.spi_us += (uint32_t)(esp_timer_get_time() - t0) - (m->stats.deliver_us - d0);
  xSemaphoreGive(m->spi_lock);
  rx_flush(m, false, 0);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "SPI RX transfer failed: %s", esp_err_to_name(e));
    return e;
  }
  m->rx_rd = (uint16_t)(rd + want);
  const uint16_t rd_now = (uint16_t)(((uint16_t)r[2] << 8) | r[3]);
  m->rx_rsr = size_read && rd_now == m->rx_rd ? (int32_t)(((uint32_t)r[0] << 8) | r[1]) : -1;
  return ESP_OK;
}

/* One reception round. The received size comes from the end of the
 * previous round's chain (m->rx_rsr), or - after an interrupt clear by the
 * task loop, or when unknown - from reading Sn_RX_RSR + Sn_RX_RD. Up to
 * RX_CHUNK bytes are then fetched and released with rx_fetch(), appended to
 * the head of a cut-off frame kept from the previous round
 * (rx_buf[rx_base, rx_base + rx_have)), and every complete frame is taken.
 *
 * The frames not delivered during the fetch are carried into the next
 * round when the read was a backlog (more than one piece - the next round
 * will have DMA to overlap them with), otherwise delivered right away. Any
 * round that does not fetch delivers the carried frames first, so they
 * never wait longer than one round.
 *
 * Returns RX_PROGRESS if bytes were fetched (more may be waiting),
 * RX_STALLED if nothing new is there but a frame is still cut off (the rest
 * is still being stored), RX_IDLE if RX memory is empty. */
static rx_result_t rx_service(w5500m_t *m) {
  uint32_t rsr;
  uint16_t rd;
  if (m->rx_rsr >= 0) {
    rsr = (uint32_t)m->rx_rsr;
    rd = m->rx_rd;
  } else {
    uint8_t r[4];
    if (w_read(m, W5500_REG_SOCK_RX_RSR(0), r, sizeof(r)) != ESP_OK) {
      rx_flush(m, false, 0);
      return RX_IDLE;
    }
    rsr = ((uint32_t)r[0] << 8) | r[1];
    rd  = (uint16_t)(((uint16_t)r[2] << 8) | r[3]);
    if (rd != m->rx_rd) m->rx_have = 0;    /* someone else moved it: kept head is stale */
    m->rx_rd = rd;
  }
  m->rx_rsr = -1;
  if (rsr == 0) {
    rx_flush(m, false, 0);
    return m->rx_have ? RX_STALLED : RX_IDLE;
  }

  /* Move the kept head so that new data lands 4-byte aligned (DMA). */
  const uint32_t base = (4u - (m->rx_have & 3u)) & 3u;
  if (base != m->rx_base) memmove(m->rx_buf + base, m->rx_buf + m->rx_base, m->rx_have);
  m->rx_base = base;
  const uint32_t dst = base + m->rx_have;
  /* With Wi-Fi -> Ethernet frames waiting, keep the chain short: the TX
   * ring is not served while it runs, and a Wi-Fi task held in transmit()
   * keeps the download frames in its own RX buffers - on the heap. */
  const uint32_t limit = m->tx_tail != m->tx_head ? RX_SHARED_CHUNK : m->rx_chunk;
  const uint32_t want = rsr < limit ? rsr : limit;
  const bool backlog = want > RX_PIECE;
  uint32_t off = base;
  bool bad = false;
  if (rx_fetch(m, rd, dst, want, &off, &bad) != ESP_OK) {
    /* State on the chip unknown: start over from what it reports. */
    m->rx_have = 0;
    m->rx_rsr = -1;
    return off > base ? RX_PROGRESS : RX_IDLE;
  }
  m->stats.rx_batches++;
  if (!bad) bad = !rx_take(m, &off, dst + want, backlog ? TAKE_CARRY : TAKE_NOW, 0);
  if (bad) {
    rx_resync(m);
    return RX_PROGRESS;
  }
  m->rx_base = off;
  m->rx_have = dst + want - off;
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
    m->stats.wakes++;
    xSemaphoreTake(m->op_lock, portMAX_DELAY);
    if (m->running) {
      if (gpio_get_level(m->int_gpio) == 0) {
        /* Clear before reading the received size: a frame stored after this
         * point raises INT again. Not needed while the size from the last
         * RX chain still promises data - the next chain clears it before
         * its own size read. */
        if (m->rx_rsr <= 0) {
          static const uint8_t clr = W5500_SIR_RECV;
          w_write(m, W5500_REG_SOCK_IR(0), &clr, 1);
          m->rx_rsr = -1;
        }
        rx_more = true;
        int_low = true;
      }
      tx_service(m);
      if (rx_more) {
        rx = rx_service(m);
        rx_more = rx != RX_IDLE;
        stalls = rx == RX_STALLED ? stalls + 1 : 0;
        if (stalls > RX_STALL_MAX) {
          rx_resync(m);
          stalls = 0;
        }
      }
    } else {
      m->tx_tail = m->tx_head;     /* frames queued while stopped are dropped */
      rx_carry_drop(m);
      rx_more = false;
    }
    xSemaphoreGive(m->op_lock);

    /* Data announced but not complete yet: the rest of the frame arrives
     * within microseconds, so wait briefly and look again. INT stuck low
     * with nothing to read: give the chip (and lower-priority tasks) a tick
     * instead of spinning. */
    if (rx == RX_STALLED) {
      m->stats.rx_stall++;
      esp_rom_delay_us(RX_STALL_WAIT_US);
    }
    idle_low = (int_low && rx == RX_IDLE) ? idle_low + 1 : 0;
    if (idle_low > 8) vTaskDelay(1);
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
  m->rx_rsr     = -1;
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
  case ETH_SPEED_10M:  m->tx_tmo_us = TX_TMO_10M_US;  m->tx_ns_per_byte = 800; return ESP_OK;
  case ETH_SPEED_100M: m->tx_tmo_us = TX_TMO_100M_US; m->tx_ns_per_byte = 80;  return ESP_OK;
  default:             return ESP_ERR_INVALID_ARG;
  }
}

/* Duplex is handled by the PHY; it is only remembered for the SEND_OK
 * shortcut in tx_wait_send_done(). */
static esp_err_t w5500m_set_duplex(esp_eth_mac_t *mac, eth_duplex_t duplex) {
  w5500m_t *m = __containerof(mac, w5500m_t, parent);
  if (duplex != ETH_DUPLEX_HALF && duplex != ETH_DUPLEX_FULL) return ESP_ERR_INVALID_ARG;
  m->full_duplex = duplex == ETH_DUPLEX_FULL;
  return ESP_OK;
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
  if (m->tx_space) vSemaphoreDelete(m->tx_space);
  heap_caps_free(m->tx_ring);
  heap_caps_free(m->rx_buf);
  free(m);
  return ESP_OK;
}

/* See w5500_mac.h. Runs tx_service() directly: the caller is the driver task
 * itself (inside the esp_eth input callback), which already holds op_lock
 * and owns the TX side, so no locking is needed - and from any other task
 * it does nothing. */
bool w5500_mac_pump_tx(esp_eth_mac_t *mac) {
  w5500m_t *m = __containerof(mac, w5500m_t, parent);
  if (xTaskGetCurrentTaskHandle() != m->task || !m->running) return true;
  if (m->rx_inflight) return !m->tx_blocked;
  if (m->tx_tail != m->tx_head) tx_service(m);
  return true;
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
  m->tx_ns_per_byte = 800;   /* conservative until the PHY reports the speed */

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

  m->tx_ring  = heap_caps_aligned_alloc(4, TX_RING_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  m->rx_buf   = heap_caps_aligned_alloc(4, RX_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  m->spi_lock = xSemaphoreCreateMutex();
  m->op_lock  = xSemaphoreCreateMutex();
  m->tx_lock  = xSemaphoreCreateMutex();
  m->tx_space = xSemaphoreCreateBinary();
  if (!m->tx_ring || !m->rx_buf || !m->spi_lock || !m->op_lock || !m->tx_lock || !m->tx_space) {
    ESP_LOGE(TAG, "no mem for buffers");
    goto err;
  }

  /* W5500 SPI frame: the 16-bit offset goes out as the command phase and the
   * control byte as the address phase. */
  spi_device_interface_config_t dev = *w5500_config->spi_devcfg;
  dev.command_bits = 16;
  dev.address_bits = 8;
  if (dev.queue_size < RX_CHAIN_MAX) dev.queue_size = RX_CHAIN_MAX;
  m->half_duplex = (dev.flags & SPI_DEVICE_HALFDUPLEX) != 0;
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
#if W5500_SCT
  m->sct = sct_probe(m, &dev, bus_max);
#endif

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
