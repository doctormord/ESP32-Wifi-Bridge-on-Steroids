/*
 * bridge.cpp
 *
 * Data path of the Wi-Fi <-> Ethernet bridge plus the management IP stack.
 *
 * The trick is in wifi_rx_cb(): esp_wifi_internal_reg_rxcb() has only ONE
 * slot, and lwIP normally owns it. We take it over, but do the gatekeeping
 * ourselves and hand exactly those frames to lwIP that are addressed to the
 * management IP. Everything else goes straight to Ethernet, bypassing the IP
 * stack entirely.
 */

#include "bridge.h"
#include "driver/gpio.h"
#include "config.h"
#include "telemetry.h"
#include "crashlog.h"

#include <string.h>
#include <stdlib.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#if CONFIG_ETH_SPI_ETHERNET_W5500
#include "esp_eth_mac_spi.h"
#include "driver/spi_master.h"
#include "w5500_mac.h"
#endif
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs.h"
#include "cpustat.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* For the location detection via gateway ping (see gateway_reachable). */
#include "ping/ping_sock.h"

#if defined(__has_include)
#  if __has_include("esp_private/esp_wifi_internal.h")
#    include "esp_private/esp_wifi_internal.h"
#    define BRIDGE_HAVE_WIFI_INTERNAL_HDR 1
#  elif __has_include("esp_wifi_internal.h")
#    include "esp_wifi_internal.h"
#    define BRIDGE_HAVE_WIFI_INTERNAL_HDR 1
#  endif
#endif

#ifndef BRIDGE_HAVE_WIFI_INTERNAL_HDR
extern "C" {
typedef esp_err_t (*wifi_rxcb_t)(void *buffer, uint16_t len, void *eb);
esp_err_t esp_wifi_internal_reg_rxcb(wifi_interface_t ifx, wifi_rxcb_t fn);
esp_err_t esp_wifi_internal_tx(wifi_interface_t ifx, void *buffer, uint16_t len);
void      esp_wifi_internal_free_rx_buffer(void *buffer);
}
#endif

/* Declared in esp_private/wifi.h, which is not part of the public include
 * path on every IDF version. Redeclaring an identical prototype is harmless
 * where the header is visible. */
extern "C" esp_err_t esp_wifi_set_tx_done_cb(
    void (*cb)(uint8_t ifidx, uint8_t *data, uint16_t *data_len, bool ok));

/* eth_rx_cb() hands management frames to lwIP via esp_netif_receive() and
 * frees the buffer right afterwards. That is ONLY allowed as long as
 * wlanif_input() copies the payload instead of referencing it - which is
 * exactly what CONFIG_LWIP_L2_TO_L3_COPY enables. Without the option this
 * would be a use-after-free that shows up as sporadic crashes under load and
 * is nearly impossible to track down. So fail the build hard instead of
 * hoping. */
#ifndef CONFIG_LWIP_L2_TO_L3_COPY
#error "CONFIG_LWIP_L2_TO_L3_COPY must be set - see eth_rx_cb() in bridge.cpp"
#endif

#if CONFIG_ETH_USE_ESP32_EMAC
/* --- WT32-ETH01 pin assignment -------------------------------------------- */
#define ETH_PHY_ADDR        1
#define ETH_PHY_MDC_GPIO    23
#define ETH_PHY_MDIO_GPIO   18
#define ETH_PHY_PWR_GPIO    16
#elif CONFIG_ETH_SPI_ETHERNET_W5500
/* --- NiceMCU ESP32-C5_MINI_V1.0 + W5500 pin assignment -------------------- *
 * SCLK and MOSI are not on the SPI2 IO_MUX pins (IO6/IO7), so the SPI driver
 * routes all four bus signals through the GPIO matrix. On the ESP32-C5 that
 * is equivalent to IO_MUX up to 80 MHz (ESP-IDF SPI master documentation).
 * IO2 is a strapping pin; as MISO it is only driven by the W5500 while CS is
 * low, so it cannot disturb the boot strapping. IO3 is JTAG MTDI, not a
 * strapping pin, and is free to use as the W5500 reset output.
 * INT is on IO15: free, not a strapping pin and not a UART pin. IO12 (UART0
 * RX) is avoided as a precaution only - the W5500 keeps running and holds INT
 * low while the ESP32-C5 is in reset, and the ROM's download mode also
 * listens on UART0 RX. It is not what breaks esptool's automatic reset into
 * download mode over USB: that fails the same way with INT on IO15. */
#define W5500_SPI_HOST      SPI2_HOST
#define W5500_SCLK_GPIO     9
#define W5500_MOSI_GPIO     8
#define W5500_MISO_GPIO     2
#define W5500_CS_GPIO       10
#define W5500_INT_GPIO      15
#define W5500_RST_GPIO      3
/* SPI clock steps tried in order by bridge_eth_init(). 80 MHz is the W5500's
 * theoretical design maximum; its datasheet only guarantees 33.3 MHz. The
 * driver verifies the chip version register at init, so a clock that is
 * grossly too fast fails there and the next step is tried. Marginal bit
 * errors during operation are NOT caught by this (the SPI link has no
 * checksum) - if they show up as TCP retransmissions or corrupted UDP
 * payloads, pin the list to 40 MHz.
 * The steps are whole divisors of the C5's 160 MHz SPI clock source
 * (PLL_F160M): /2, /3, /4, /8. 53.3 MHz sits between the 80 MHz that the
 * GPIO-matrix routing above cannot read reliably and the safe 40 MHz. */
static const int W5500_SPI_HZ[] = { 160 * 1000 * 1000 / 2, 160 * 1000 * 1000 / 3,
                                    160 * 1000 * 1000 / 4, 160 * 1000 * 1000 / 8 };
#else
#error "No supported Ethernet driver configured (ESP32 EMAC or W5500)"
#endif

/*
 * TX retries before a frame is dropped (Ethernet -> Wi-Fi).
 *
 * NOTE: the right value depends on the TRAFFIC, not on the device.
 *
 *   TCP (speed test, file transfer)
 *       Dropping fast is better. TCP retransmits and throttles on its own;
 *       waiting only blocks Ethernet reception. Measured: retries 1 gave
 *       12.0/4.8 Mbit, retries 8 only 10.4/4.0.
 *
 *   UDP (camera, RTP)
 *       Exactly the opposite. A dropped packet is gone FOR GOOD and turns
 *       into a picture glitch. Measured on a 2 Mbit camera stream:
 *       retries  1 -> 13.29 % loss
 *       retries  8 ->  0.93 %
 *       retries 16 ->  0.10 %
 *
 * The default is 8 because that is what this device is for: bridging a
 * camera, not winning speed tests.
 *
 * Going higher is not free: every retry holds up the emac_rx task longer,
 * and whatever arrives in that time is lost by the Ethernet DMA without our
 * counters seeing it. Hence the smallest value that meets the target - which
 * is exactly what the autotune in the portal searches for.
 */
#define ETH_TX_RETRIES_DEF  8
/* Opposite direction: Ethernet practically always accepts, drop_down stayed
 * at 0 in every measurement. Nothing to gain here. */
#define WIFI_TX_RETRIES_DEF 1
/* Several attempts per SSID instead of just one, before bridge_wifi_start()
 * gives up and main.cpp reboots straight into the portal.
 *
 * Reason: a single attempt does not survive two perfectly normal, transient
 * situations. First, an old association of the CLONED MAC that the AP has
 * not expired yet (802.11 reason 204: the AP still holds the old session
 * while the same MAC is already logging in again) - a quick abort() reboot
 * from the watchdog escalation (see watchdog_tick()) is exactly the case
 * most likely to produce this. Second, a short radio disturbance that has
 * not cleared yet - especially right after a watchdog reboot, whose cause
 * was by definition a Wi-Fi problem that need not have resolved itself in
 * the second until the next boot.
 *
 * Without retries a single bad moment throws the bridge into the portal
 * (192.168.4.1, invisible to every other device). This is deliberately a
 * retry of the SAME SSID, not a longer timeout: the actual problem is "wait
 * briefly and try again", not "wait longer for the same single attempt".
 *
 * 20 s / 3 attempts is a reasonable choice rather than a measured value;
 * both are overridable in the portal (g_cfg, 0 = this default). The pause
 * between attempts is deliberately fixed. */
#define WIFI_CONNECT_TIMEOUT_MS_DEF 20000u
#define WIFI_CONNECT_RETRIES_DEF        3
#define WIFI_CONNECT_RETRY_DELAY_MS  2000u

/* Defaults for active roaming - defined up here so bridge_get_defaults()
 * further down already knows them. Full rationale at roam_tick(). */
#define ROAM_CHECK_S_DEF            300u   /* 5 min */
#define ROAM_RSSI_THRESHOLD_DEF     (-70)  /* only check below this */
#define ROAM_MARGIN_DB_DEF            6u   /* how much better a candidate must be */

/* The retry counters live in their own variables rather than being read from
 * g_cfg in the data path: eth_rx_cb/wifi_rx_cb run from IRAM and are called
 * per packet, so no detour through the large config struct.
 * volatile because the portal changes them from another task. */
static volatile uint8_t s_eth_tx_retries  = ETH_TX_RETRIES_DEF;
static volatile uint8_t s_wifi_tx_retries = WIFI_TX_RETRIES_DEF;

/* --- State ---------------------------------------------------------------- */
static esp_eth_handle_t s_eth        = NULL;
static esp_netif_t     *s_mgmt_netif = NULL;

static volatile bool s_eth_link      = false;
static volatile bool s_wifi_up       = false;
static volatile bool s_active        = false;
static volatile bool s_paused        = false;   /* during OTA */

static uint8_t       s_client_mac[6] = {0};
static volatile bool s_mac_known     = false;

/* Management IP as bytes - lets the data path use memcmp() directly, without
 * any byte-order juggling. */
static uint8_t       s_mgmt_ip[4]    = {0};

/* IP of the bridged client, snooped from the data path. The bridge works at
 * layer 2 and does not need this address - it is purely diagnostic. Without
 * it the portal would only show the MAC, and there would be no way to tell
 * whether the client has an address at all, let alone which one. */
static volatile uint8_t s_client_ip[4] = {0};

/* Host name of the client, snooped from its DHCP request (option 12).
 * Routers name devices from exactly this. If it stays empty, the client does
 * not send one - in which case a missing name in the router is not the
 * router's fault. */
static char s_client_name[32] = "";

static volatile uint32_t s_pkt_e2w = 0, s_pkt_w2e = 0;
static volatile uint32_t s_drop_e2w = 0, s_drop_w2e = 0;
static volatile uint64_t s_by_e2w = 0, s_by_w2e = 0;
static uint32_t s_kbps_e2w = 0, s_kbps_w2e = 0;

/* Outcome of every frame the Wi-Fi MAC finished, as reported by the TX-done
 * callback: delivered (ACK from the AP) or abandoned after the hardware
 * retries. Includes the management stack's frames, which are a tiny
 * fraction. This is the only view of losses ON THE AIR - drop_e2w only sees
 * frames that never got a TX buffer. */
static volatile uint32_t s_air_ok   = 0;
static volatile uint32_t s_air_fail = 0;

/* TX queue accounting. Depth = frames the bridge handed to the driver
 * (s_pkt_e2w) minus frames the MAC finished (s_air_done), i.e. frames
 * waiting for the air. Every completion adds that depth to s_q_sum; the mean
 * depth over a window is dSum/dDone, and by Little's law depth / rate is the
 * time a frame waits in the queue - the bufferbloat that turns a weak link
 * into seconds of ping.
 *
 * The callback reports the finished 802.11 frame with an encrypted payload,
 * so the management stack's own frames (a few per second, not counted in
 * s_pkt_e2w) cannot be filtered out. Instead the estimate resynchronizes:
 * whenever it would go negative, the wifi task moves s_q_base; whenever a
 * full second passes without submissions or completions, the queue is
 * empty by definition and bridge_tick() moves s_q_idle. Each base has a
 * single writer. Unsigned differences make the wrap-around harmless. */
static volatile uint32_t s_air_done = 0;
static volatile uint32_t s_q_sum    = 0;
static volatile uint32_t s_q_base   = 0;   /* written by the wifi task only  */
static volatile uint32_t s_q_idle   = 0;   /* written by bridge_tick() only  */
static uint32_t s_q_avg_x10  = 0;    /* mean depth over the last second, x10 */
static uint32_t s_q_delay_ms = 0;    /* matching queue delay                 */

/* TX queue cap in frames, 0 = off (g_cfg.txq_cap, or the value under test
 * during a cap calibration), and the frames it dropped. Those drops are also
 * counted in s_drop_e2w, so every loss figure includes them. */
static volatile uint8_t  s_txq_cap  = 0;
static volatile uint32_t s_drop_cap = 0;

/* Frames up to this size bypass the cap: TCP ACKs, ARP, ICMP, DNS. They cost
 * next to no airtime, and dropping a camera's ACKs or the gateway's ARP
 * would hurt far more than queueing them. */
#define TXQ_CAP_MIN_LEN 256u

/* Current queue depth estimate (may be negative before a resync). */
IRAM_ATTR static inline int32_t txq_depth(uint32_t done) {
  return (int32_t)(s_pkt_e2w - done - s_q_base - s_q_idle);
}

/* TX-done callback, runs in the Wi-Fi task for every finished frame - so it
 * only updates counters. */
IRAM_ATTR static void wifi_tx_done(uint8_t ifidx, uint8_t *data,
                                   uint16_t *data_len, bool ok) {
  (void)data; (void)data_len;
  if (ifidx != WIFI_IF_STA) return;
  if (ok) s_air_ok = s_air_ok + 1;
  else    s_air_fail = s_air_fail + 1;
  const uint32_t done = s_air_done + 1;
  s_air_done = done;
  const int32_t depth = txq_depth(done);
  if (depth < 0) s_q_base = s_q_base + (uint32_t)depth;   /* resync to 0 */
  else           s_q_sum  = s_q_sum + (uint32_t)depth;
}

/* For the watchdog (see below) and for diagnosis in the portal: how often the
 * STA lost its association since boot. The reassociation rate shows something
 * the drop counters alone do not - whether the radio channel tears down the
 * connection itself, not just loses individual frames. */
static volatile uint32_t s_wifi_disc_count = 0;

static char s_bssid_str[18] = "-";

/* ---------------------------------------------------------------------------
 * Disconnect log (see bridge.h)
 *
 * Ring buffer in RTC memory so it survives reboots. After power-on it holds
 * random data - the magic word plus a plausibility check of head/fill level
 * discards that. Written from the event task, read from the HTTP and main
 * tasks, hence the spinlock.
 * ------------------------------------------------------------------------ */
#define DISC_LOG_MAGIC 0x44495333u   /* "DIS3" - bump when the layout changes */
RTC_NOINIT_ATTR static uint32_t      s_dl_magic;
RTC_NOINIT_ATTR static uint8_t       s_dl_boot;
RTC_NOINIT_ATTR static uint8_t       s_dl_head;   /* next slot to write */
RTC_NOINIT_ATTR static uint8_t       s_dl_used;
RTC_NOINIT_ATTR static bridge_disc_t s_dl[DISC_LOG_N];
static bool s_dl_ready = false;      /* check/increment once per boot */
static portMUX_TYPE s_dl_mux = portMUX_INITIALIZER_UNLOCKED;

/* Frequency per reason, this boot only. 12 slots are practically always
 * enough - in practice only a handful of reasons ever occur. */
#define DISC_REASON_SLOTS 12
static bridge_disc_count_t s_dr[DISC_REASON_SLOTS];
static uint16_t s_dr_rest = 0;
static uint32_t s_kicks = 0, s_fails = 0;   /* external, since boot */

/* Set before one of our own esp_wifi_disconnect() calls, so the resulting
 * disconnect event does not count as a radio problem. Timestamped: if no
 * event follows our own disconnect (we were not connected), the marker must
 * not stick to a later, real drop. */
static volatile uint8_t  s_disc_src = DISC_SRC_EXTERN;
static volatile uint32_t s_disc_src_ms = 0;

/* Mark the next disconnect event (within 3 s) as caused by `src`. */
static void disc_mark_own(uint8_t src) {
  portENTER_CRITICAL(&s_dl_mux);
  s_disc_src_ms = millis();
  s_disc_src    = src;
  portEXIT_CRITICAL(&s_dl_mux);
}

/* Set by disc_on_restart(): the system is going down, so wifi_evt() must not
 * start another connect attempt while the driver is being stopped. */
static volatile bool s_restarting = false;

/* Shutdown handler for esp_restart(). Marks the restart and disconnects
 * itself, so the clean ASSOC_LEAVE is logged as a restart instead of an
 * external drop. Disconnecting here instead of relying on Wi-Fi's own
 * esp_wifi_stop handler makes the result independent of the handler order
 * (which depends on free slots in IDF's handler table) and of whether that
 * handler exists at all (it does not in the BRIDGE_MINIMAL build). Panics do
 * not run shutdown handlers, but they do not disconnect cleanly either. */
static void disc_on_restart(void) {
  s_restarting = true;
  if (!s_wifi_up) return;
  disc_mark_own(DISC_SRC_RESTART);
  esp_wifi_disconnect();
}

/* Validate the RTC ring buffer once per boot: reset it if it holds garbage
 * (power-on, layout change), otherwise increment the boot counter.
 * Only call while holding s_dl_mux. */
static void dl_ensure_locked(void) {
  if (s_dl_ready) return;
  s_dl_ready = true;
  if (s_dl_magic != DISC_LOG_MAGIC || s_dl_head >= DISC_LOG_N || s_dl_used > DISC_LOG_N) {
    memset(s_dl, 0, sizeof(s_dl));
    s_dl_head = 0; s_dl_used = 0; s_dl_boot = 0;
    s_dl_magic = DISC_LOG_MAGIC;
  } else {
    s_dl_boot++;
  }
}

/* Record one disconnect event in the ring buffer and update the per-reason
 * statistics.
 *
 * was_up: was the STA connected at that moment? The driver reports a failed
 * connect attempt with the same event as the end of an existing connection -
 * after a kick by the AP, a dozen such failed attempts often follow, which
 * would bury the actual cause in the statistics. Only external drops of an
 * existing connection count as "kicks". */
static void disc_log_add(const wifi_event_sta_disconnected_t *d, bool was_up) {
  const uint8_t reason = d ? d->reason : 0;
  const uint32_t epoch = crashlog_now();
  const uint32_t now_ms = millis();
  portENTER_CRITICAL(&s_dl_mux);
  /* Read and clear the marker under the same lock disc_mark_own() takes, so
   * a mark set concurrently (e.g. by the restarting task) is neither lost nor
   * attributed to the wrong event. Once a restart is under way, every
   * disconnect belongs to it - esp_wifi_stop can post a second one. */
  uint8_t src = DISC_SRC_EXTERN;
  if (s_restarting) src = DISC_SRC_RESTART;
  else if (s_disc_src != DISC_SRC_EXTERN && now_ms - s_disc_src_ms < 3000) src = s_disc_src;
  /* Before the bridge is active, every event belongs to the boot-time
   * connect sequence - e.g. NO_AP_FOUND for a configured network that is
   * not there. Not a radio problem of a running link. */
  else if (!s_active && !was_up) src = DISC_SRC_CONNECT;
  s_disc_src = DISC_SRC_EXTERN;

  dl_ensure_locked();
  bridge_disc_t *e = &s_dl[s_dl_head];
  memset(e, 0, sizeof(*e));
  e->t_s    = millis() / 1000;
  e->epoch  = epoch;
  e->down_s = 0xFFFF;
  e->reason = reason;
  e->rssi   = d ? d->rssi : 0;
  if (d) memcpy(e->bssid, d->bssid, 6);
  e->boot   = s_dl_boot;
  e->src    = src;
  e->was_up = was_up ? 1 : 0;
  s_dl_head = (uint8_t)((s_dl_head + 1) % DISC_LOG_N);
  if (s_dl_used < DISC_LOG_N) s_dl_used++;

  if (src == DISC_SRC_EXTERN && !was_up) s_fails++;
  if (src == DISC_SRC_EXTERN && was_up) {
    s_kicks++;
    size_t k = 0;
    while (k < DISC_REASON_SLOTS && s_dr[k].n && s_dr[k].reason != reason) k++;
    if (k == DISC_REASON_SLOTS) {
      if (s_dr_rest < 0xFFFF) s_dr_rest++;
    } else {
      s_dr[k].reason = reason;
      if (s_dr[k].n < 0xFFFF) s_dr[k].n++;
    }
  }
  portEXIT_CRITICAL(&s_dl_mux);
}

/* On reconnect, fill in the outage duration - for all still-open entries of
 * this boot, back to and including the drop that started the outage. That
 * way the triggering drop shows the TOTAL outage, not just the time after the
 * last failed attempt. */
static void disc_log_reconnected(void) {
  portENTER_CRITICAL(&s_dl_mux);
  dl_ensure_locked();
  const uint32_t now_s = millis() / 1000;
  for (size_t i = 0; i < s_dl_used; i++) {
    bridge_disc_t *e = &s_dl[(s_dl_head + DISC_LOG_N - 1 - i) % DISC_LOG_N];
    if (e->boot != s_dl_boot || e->down_s != 0xFFFF) break;
    const uint32_t d = now_s - e->t_s;
    e->down_s = d < 0xFFFE ? (uint16_t)d : 0xFFFE;
    if (e->was_up) break;
  }
  portEXIT_CRITICAL(&s_dl_mux);
}

/* See bridge.h. */
void bridge_get_disc_totals(uint32_t *kicks, uint32_t *fails) {
  portENTER_CRITICAL(&s_dl_mux);
  if (kicks) *kicks = s_kicks;
  if (fails) *fails = s_fails;
  portEXIT_CRITICAL(&s_dl_mux);
}

/* See bridge.h. Walks the ring buffer backwards from the newest entry. */
size_t bridge_get_disc_log(bridge_disc_t *out, size_t max, uint8_t *boot_now) {
  portENTER_CRITICAL(&s_dl_mux);
  dl_ensure_locked();
  size_t n = s_dl_used < max ? s_dl_used : max;
  for (size_t i = 0; i < n; i++)
    out[i] = s_dl[(s_dl_head + DISC_LOG_N - 1 - i) % DISC_LOG_N];
  if (boot_now) *boot_now = s_dl_boot;
  portEXIT_CRITICAL(&s_dl_mux);
  return n;
}

/* See bridge.h. Copies the table under the lock, sorts the copy outside it. */
size_t bridge_get_disc_counts(bridge_disc_count_t *out, size_t max, uint16_t *rest) {
  bridge_disc_count_t tmp[DISC_REASON_SLOTS];
  portENTER_CRITICAL(&s_dl_mux);
  memcpy(tmp, s_dr, sizeof(tmp));
  if (rest) *rest = s_dr_rest;
  portEXIT_CRITICAL(&s_dl_mux);

  size_t n = 0;
  while (n < DISC_REASON_SLOTS && tmp[n].n) n++;
  /* Descending by frequency - insertion sort is plenty for 12 entries. */
  for (size_t i = 1; i < n; i++)
    for (size_t j = i; j > 0 && tmp[j].n > tmp[j - 1].n; j--) {
      bridge_disc_count_t t = tmp[j]; tmp[j] = tmp[j - 1]; tmp[j - 1] = t;
    }
  if (n > max) n = max;
  memcpy(out, tmp, n * sizeof(*out));
  return n;
}

/* Map a wifi_err_reason_t to a short, stable name for logs and the API. */
const char *bridge_wifi_reason_str(uint8_t r) {
  switch (r) {
    case WIFI_REASON_UNSPECIFIED:                   return "UNSPECIFIED";
    case WIFI_REASON_AUTH_EXPIRE:                   return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE:                    return "AUTH_LEAVE";
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:    return "INACTIVITY";
    case WIFI_REASON_ASSOC_TOOMANY:                 return "ASSOC_TOOMANY";
    case WIFI_REASON_CLASS2_FRAME_FROM_NONAUTH_STA: return "CLASS2_FRAME";
    case WIFI_REASON_CLASS3_FRAME_FROM_NONASSOC_STA:return "CLASS3_FRAME";
    case WIFI_REASON_ASSOC_LEAVE:                   return "ASSOC_LEAVE";
    case WIFI_REASON_ASSOC_NOT_AUTHED:              return "ASSOC_NOT_AUTHED";
    case WIFI_REASON_BSS_TRANSITION_DISASSOC:       return "BSS_TRANSITION";
    case WIFI_REASON_MIC_FAILURE:                   return "MIC_FAILURE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:        return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:      return "GROUP_KEY_TIMEOUT";
    case WIFI_REASON_802_1X_AUTH_FAILED:            return "802_1X_AUTH_FAILED";
    case WIFI_REASON_MISSING_ACKS:                  return "MISSING_ACKS";
    case WIFI_REASON_STA_LEAVING:                   return "STA_LEAVING";
    case WIFI_REASON_TIMEOUT:                       return "TIMEOUT";
    case WIFI_REASON_PEER_INITIATED:                return "PEER_INITIATED";
    case WIFI_REASON_AP_INITIATED:                  return "AP_INITIATED";
    case WIFI_REASON_BEACON_TIMEOUT:                return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:                   return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:                     return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:                    return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:             return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:               return "CONNECTION_FAIL";
    case WIFI_REASON_AP_TSF_RESET:                  return "AP_TSF_RESET";
    case WIFI_REASON_ROAMING:                       return "ROAMING";
    case WIFI_REASON_SA_QUERY_TIMEOUT:              return "SA_QUERY_TIMEOUT";
    default:                                        return "OTHER";
  }
}

/* TX power the PHY sets on its own after start.
 * Remembered on the first bridge_apply_live_tuning() so that resetting
 * tx_power to 0 can actively RESTORE it - otherwise the last value set would
 * stay in effect and "Default" in the portal would do nothing. */
static int8_t s_txp_default = 0;

/* What was actually passed to the last esp_wifi_init(). */
static uint8_t s_eff_static_rx = CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM;
static uint8_t s_eff_dyn_rx    = CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM;
static uint8_t s_eff_dyn_tx    = CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM;
static uint8_t s_eff_ba_win    = CONFIG_ESP_WIFI_RX_BA_WIN;
static uint16_t s_eff_wifi_connect_timeout_s = WIFI_CONNECT_TIMEOUT_MS_DEF / 1000;
static uint8_t  s_eff_wifi_connect_retries   = WIFI_CONNECT_RETRIES_DEF;

/* Format a MAC address as "aa:bb:cc:dd:ee:ff" (out needs 18 bytes). */
static void mac2str(const uint8_t *m, char *out) {
  sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* ===========================================================================
 * Data path
 * ========================================================================= */

/* Defined further down with the Wi-Fi -> Ethernet path, but needed here
 * already: both directions use the same gatekeeper check. */
IRAM_ATTR static inline bool is_for_mgmt(const uint8_t *f, uint16_t len);

/*
 * Ethernet -> Wi-Fi. Runs in the emac_rx task; 'buffer' is ours and must be
 * freed exactly once on every path.
 *
 * Besides forwarding, it snoops the client's IP address and DHCP host name
 * for diagnostics, and diverts frames addressed to the management IP to the
 * local lwIP stack.
 *
 * IMPORTANT: this path must block as briefly as possible. How often it
 * retries when the Wi-Fi TX buffer is full is set by s_eth_tx_retries - the
 * trade-off (UDP camera vs. TCP) is explained at ETH_TX_RETRIES_DEF. In
 * short: waiting stalls Ethernet reception, and whatever arrives meanwhile
 * is lost by the DMA in bursts.
 */
IRAM_ATTR static esp_err_t eth_rx_cb(esp_eth_handle_t h, uint8_t *buffer,
                                     uint32_t len, void *priv) {
  (void)h; (void)priv;

  /* Snoop the client IP. On this side the sender is always the attached
   * client, so the source address is its address. Costs two comparisons and
   * copying four bytes. 0.0.0.0 is ignored - that is what a DHCP request
   * looks like before the client has an address, and it would be misleading
   * to display. */
  if (len >= 34) {
    const uint16_t et = ((uint16_t)buffer[12] << 8) | buffer[13];
    const uint8_t *src = NULL;
    if      (et == 0x0800)               src = buffer + 26;  /* IPv4 source  */
    else if (et == 0x0806 && len >= 42)  src = buffer + 28;  /* ARP sender   */
    if (src && (src[0] | src[1] | src[2] | src[3])) {
      s_client_ip[0]=src[0]; s_client_ip[1]=src[1];
      s_client_ip[2]=src[2]; s_client_ip[3]=src[3];
    }
  }

  /* Snoop the DHCP host name - only until we have one, so the hot path is
   * unburdened afterwards. Strict length checks, because untrusted data is
   * being parsed here. */
  if (!s_client_name[0] && len >= 42 &&
      ((uint16_t)buffer[12] << 8 | buffer[13]) == 0x0800 && buffer[23] == 17) {
    const uint32_t ihl = (uint32_t)(buffer[14] & 0x0F) * 4;
    const uint32_t udp = 14 + ihl;
    if (ihl >= 20 && len >= udp + 8) {
      const uint16_t sp = (uint16_t)buffer[udp] << 8 | buffer[udp + 1];
      const uint16_t dp = (uint16_t)buffer[udp + 2] << 8 | buffer[udp + 3];
      if (sp == 68 && dp == 67) {                 /* client -> DHCP server */
        uint32_t o = udp + 8 + 240;               /* fixed BOOTP header + magic */
        while (o + 1 < len) {
          const uint8_t t = buffer[o];
          if (t == 255) break;                    /* end of options */
          if (t == 0)   { o++; continue; }        /* padding */
          const uint8_t l = buffer[o + 1];
          if (o + 2 + l > len) break;
          if (t == 12 && l) {                     /* option 12 = host name */
            const uint8_t n = l < sizeof(s_client_name) - 1
                              ? l : sizeof(s_client_name) - 1;
            memcpy(s_client_name, buffer + o + 2, n);
            s_client_name[n] = '\0';
            break;
          }
          o += 2 + l;
        }
      }
    }
  }

  /* Frames for the management IP belong to the local stack, not to Wi-Fi.
   * Without this branch the web UI is unreachable from the client side -
   * even the ARP request for the management IP would go out over Wi-Fi and
   * never be answered.
   *
   * On buffer ownership: calling esp_netif_receive() here is only allowed
   * because CONFIG_LWIP_L2_TO_L3_COPY is set. Then wlanif_input() copies the
   * payload into its own pbuf; without the option it would merely reference
   * our buffer (PBUF_REF), which we free one line later -> use-after-free in
   * the hot path. NULL as l2_buff is allowed, wifi_free() checks for it. */
#ifndef BRIDGE_MINIMAL
  if (len >= 14 && is_for_mgmt(buffer, (uint16_t)len)) {
    esp_netif_receive(s_mgmt_netif, buffer, len, NULL);
    free(buffer);
    return ESP_OK;
  }
#endif

  /* Check the pause flag only AFTER the management branch - same as in
   * wifi_rx_cb(). Checking it first would also drop management frames from
   * the Ethernet side during an OTA upload, so an update started from the
   * attached client would cut its own TCP connection (bridge_set_paused()
   * promises the opposite). */
  if (s_paused) { free(buffer); return ESP_OK; }   /* paused, not a drop */

  /* Queue cap: with more than s_txq_cap frames already waiting for the air,
   * drop a large frame NOW rather than letting it occupy a TX buffer. The
   * point is latency: every queued frame delays everything behind it -
   * pings, the management portal, the client's own ACK-clocked TCP - and a
   * camera's I-frame burst otherwise fills all TX buffers (tens of ms of
   * queue, ~1.7 KB of heap per frame). Deliberately no waiting here: a frame
   * held back in this task just moves the same backlog into the Ethernet
   * DMA ring. The retry loop below still handles a genuinely full buffer. */
  const uint8_t cap = s_txq_cap;
  if (cap && len > TXQ_CAP_MIN_LEN && s_active && s_wifi_up &&
      txq_depth(s_air_done) >= (int32_t)cap) {
    s_drop_cap = s_drop_cap + 1;
    s_drop_e2w = s_drop_e2w + 1;
    free(buffer);
    return ESP_OK;
  }

  esp_err_t err = ESP_FAIL;
  if (s_active && s_wifi_up) {
    const uint8_t retries = s_eth_tx_retries;
    for (int i = 0; i < retries; i++) {
      err = esp_wifi_internal_tx(WIFI_IF_STA, buffer, (uint16_t)len);
      /* Do not wait after the LAST attempt - the frame is lost anyway, and
       * any further pause only holds up the emac_rx task (see below). With
       * 8 retries that would cost an extra 1 ms per drop. */
      if (err != ESP_ERR_NO_MEM || i + 1 >= retries) break;

      /* For the first attempts just yield the CPU instead of sleeping.
       *
       * vTaskDelay(1) ALWAYS waits a full tick, even if the Wi-Fi TX buffer
       * frees up after microseconds. That blocks the emac_rx task, and the
       * Ethernet DMA only buffers a couple of milliseconds at 100 Mbit -
       * longer stalls are lost there silently, without our drop counters
       * seeing it.
       *
       * taskYIELD() hands over to the higher-priority Wi-Fi task that frees
       * the buffers, and returns as soon as it is done. Only if that did not
       * help several times is real waiting worthwhile - then the radio link
       * is genuinely saturated and pushing harder does not help. With a plain
       * vTaskDelay(1) a residual loss of about 1.5 % remained that neither
       * more buffers nor HT40 could reduce. */
      if (i < 3) taskYIELD();
      else       vTaskDelay(1);
    }
  }

  if (err == ESP_OK) { s_pkt_e2w = s_pkt_e2w + 1; s_by_e2w += len; }
  else               { s_drop_e2w = s_drop_e2w + 1; }

  free(buffer);
  return ESP_OK;
}

/*
 * Does this frame belong to the management stack?
 *
 *   - IPv4 with destination == management IP
 *   - ARP request/reply with target IP == management IP
 *
 * Deliberately NOT included: broadcast and multicast IPv4. Those go only to
 * the client. As a consequence mDNS/SSDP do not work for the management
 * interface - acceptable with a fixed IP, and it keeps the hot path free of
 * special cases. Callers must ensure len >= 14.
 */
IRAM_ATTR static inline bool is_for_mgmt(const uint8_t *f, uint16_t len) {
  if (!s_mgmt_netif) return false;

  const uint16_t ethertype = ((uint16_t)f[12] << 8) | f[13];

  if (ethertype == 0x0800) {                 /* IPv4  */
    if (len < 34) return false;
    return memcmp(f + 30, s_mgmt_ip, 4) == 0;   /* destination IP */
  }
  if (ethertype == 0x0806) {                 /* ARP   */
    if (len < 42) return false;
    return memcmp(f + 38, s_mgmt_ip, 4) == 0;   /* target protocol address */
  }
  return false;
}

/*
 * Wi-Fi -> Ethernet, or -> lwIP for management frames.
 *
 * Registered in place of lwIP's own RX callback (see wifi_evt()). 'eb' is the
 * driver's internal RX buffer and MUST be released exactly once on every
 * path. esp_netif_receive() takes ownership of it itself - so no separate
 * free on that path.
 */
IRAM_ATTR static esp_err_t wifi_rx_cb(void *buffer, uint16_t len, void *eb) {
  uint8_t *f = (uint8_t *)buffer;

#ifndef BRIDGE_MINIMAL
  if (len >= 14 && is_for_mgmt(f, len)) {
    return esp_netif_receive(s_mgmt_netif, buffer, len, eb);
  }
#endif

  if (s_paused) {
    if (eb) esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
  }

  esp_err_t err = ESP_FAIL;
  if (s_active && s_eth_link && s_eth) {
    const uint8_t retries = s_wifi_tx_retries;
    for (int i = 0; i < retries; i++) {
      err = esp_eth_transmit(s_eth, buffer, len);
      /* Do not sleep after the last attempt: this is the Wi-Fi task, and
       * with the default of 1 retry every drop would block it for a full
       * tick for no benefit at all. */
      if (err == ESP_OK || i + 1 >= retries) break;
      vTaskDelay(1);
    }
  }

  if (err == ESP_OK) { s_pkt_w2e = s_pkt_w2e + 1; s_by_w2e += len; }
  else               { s_drop_w2e = s_drop_w2e + 1; }

  if (eb) esp_wifi_internal_free_rx_buffer(eb);
  return ESP_OK;
}

/* Ethernet input callback used only during the sniff phase: remembers the
 * source MAC of the first unicast-sourced frame and discards every frame. */
static esp_err_t eth_sniff_cb(esp_eth_handle_t h, uint8_t *buffer,
                              uint32_t len, void *priv) {
  (void)h; (void)priv;
  if (!s_mac_known && len >= 12 && (buffer[6] & 0x01) == 0) {
    memcpy(s_client_mac, buffer + 6, 6);
    s_mac_known = true;
  }
  free(buffer);
  return ESP_OK;
}

/* ===========================================================================
 * Events
 * ========================================================================= */

/* Ethernet link up/down handler - tracks s_eth_link for the data path. */
static void eth_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg; (void)base; (void)data;
  if (id == ETHERNET_EVENT_CONNECTED) {
    s_eth_link = true;  printf("[ETH ] Link up\n");
  } else if (id == ETHERNET_EVENT_DISCONNECTED) {
    s_eth_link = false; printf("[ETH ] Link down\n");
  }
}

/* Wi-Fi STA connect/disconnect handler.
 *
 * On connect: records the BSSID, closes open disconnect-log entries and
 * (re)installs our RX callback. On disconnect: logs the event and, while the
 * bridge is active, reconnects immediately - this is the single place that
 * reconnects, which the watchdog and roaming rely on. */
/* Defined with the network slot logic further down (see sta_lock_ap()). */
static void sta_release_lock_on_disconnect(void);
static void last_ap_note(const uint8_t bssid[6], uint8_t channel);

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg; (void)base;

  if (id == WIFI_EVENT_STA_CONNECTED) {
    s_wifi_up = true;
    wifi_event_sta_connected_t *e = (wifi_event_sta_connected_t *)data;
    if (e) mac2str(e->bssid, s_bssid_str);
    if (e) last_ap_note(e->bssid, e->channel);
    printf("[WIFI] Connected (AP %s)\n", s_bssid_str);
    disc_log_reconnected();

    /* IMPORTANT: register only here. esp_netif already registered itself on
     * attach - this call deliberately overrides that so we get the
     * gatekeeper role. */
    esp_err_t e2 = esp_wifi_internal_reg_rxcb(WIFI_IF_STA, wifi_rx_cb);
    if (e2 != ESP_OK) {
      printf("[WIFI] reg_rxcb failed: %s\n", esp_err_to_name(e2));
    }

  } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
    const bool was_up = s_wifi_up;   /* remember before resetting */
    s_wifi_up = false;
    strcpy(s_bssid_str, "-");
    s_wifi_disc_count = s_wifi_disc_count + 1;
    wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
    printf("[WIFI] Disconnected (reason %d %s, %d dBm)\n", d ? d->reason : -1,
           bridge_wifi_reason_str(d ? d->reason : 0), d ? d->rssi : 0);
    disc_log_add(d, was_up);
    /* A BSSID binding (sta_lock_ap()) holds for one connection only: release
     * it on the first disconnect that is not our own roaming switch, so the
     * reconnect below may pick any AP of the SSID again. */
    sta_release_lock_on_disconnect();
    /* In operation, keep reconnecting - but not while esp_restart() is
     * stopping the driver, where a connect would race the shutdown. */
    if (s_active && !s_restarting) esp_wifi_connect();
  }
}

/* ===========================================================================
 * Ethernet
 * ========================================================================= */

/* Put the Ethernet receive filter into bridge mode: accept every frame on
 * the wire. Promiscuous mode alone is not enough with the W5500 - its IDF
 * driver also sets "block multicast" in MACRAW mode, which would cut the
 * bridged client off from mDNS, SSDP, ONVIF discovery and IPv4 multicast
 * streams. On the ESP32 EMAC, promiscuous mode already passes multicast and
 * the second ioctl is harmless. Called wherever the driver is (re)started,
 * because the filter state is not guaranteed to survive a stop/start. */
static void eth_set_bridge_filters(void) {
  bool on = true;
  esp_eth_ioctl(s_eth, ETH_CMD_S_PROMISCUOUS, &on);
  esp_err_t e = esp_eth_ioctl(s_eth, ETH_CMD_S_ALL_MULTICAST, &on);
  if (e != ESP_OK && e != ESP_ERR_NOT_SUPPORTED) {
    printf("[ETH ] all_multicast: %s\n", esp_err_to_name(e));
  }
}

/* SPI clock the W5500 was brought up with, see bridge_get_eth_spi_mhz(). */
static uint8_t s_eth_spi_mhz = 0;

/* See bridge.h. */
uint8_t bridge_get_eth_spi_mhz(void) { return s_eth_spi_mhz; }

#if CONFIG_ETH_SPI_ETHERNET_W5500
/* The W5500 MAC instance, for its driver counters. */
static esp_eth_mac_t *s_w5500_mac = NULL;
#endif

/* See bridge.h. */
void bridge_get_eth_drv_stats(uint32_t out[5]) {
  out[0] = out[1] = out[2] = out[3] = out[4] = 0;
#if CONFIG_ETH_SPI_ETHERNET_W5500
  if (!s_w5500_mac) return;
  w5500_mac_stats_t st;
  w5500_mac_get_stats(s_w5500_mac, &st);
  out[0] = st.tx_ring_full;
  out[1] = st.tx_timeout;
  out[2] = st.rx_bad_len;
  out[3] = st.rx_no_mem;
  out[4] = st.rx_stall;
#endif
}

#if CONFIG_ETH_USE_ESP32_EMAC
/* Power up the LAN8720 PHY and install the ESP32 EMAC driver (RMII with the
 * external 50 MHz clock on GPIO0, as wired on the WT32-ETH01). No netif is
 * attached: frames are handled raw by the input callbacks above. */
bool bridge_eth_init(void) {
  gpio_config_t pwr = {};
  pwr.pin_bit_mask = 1ULL << ETH_PHY_PWR_GPIO;
  pwr.mode         = GPIO_MODE_OUTPUT;
  gpio_config(&pwr);
  gpio_set_level((gpio_num_t)ETH_PHY_PWR_GPIO, 1);
  delay(200);   /* let the oscillator settle */

  eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
  mac_cfg.rx_task_stack_size = 4096;   /* our callback calls into the Wi-Fi stack */

  eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
  emac.smi_gpio.mdc_num  = ETH_PHY_MDC_GPIO;
  emac.smi_gpio.mdio_num = ETH_PHY_MDIO_GPIO;
#else
  emac.smi_mdc_gpio_num  = ETH_PHY_MDC_GPIO;
  emac.smi_mdio_gpio_num = ETH_PHY_MDIO_GPIO;
#endif
  emac.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
  emac.clock_config.rmii.clock_gpio = EMAC_CLK_IN_GPIO;

  esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac, &mac_cfg);
  if (!mac) { printf("[ETH ] MAC init failed\n"); return false; }

  eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
  phy_cfg.phy_addr       = ETH_PHY_ADDR;
  phy_cfg.reset_gpio_num = -1;

  esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_cfg);
  if (!phy) { printf("[ETH ] PHY init failed\n"); return false; }

  esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
  esp_err_t err = esp_eth_driver_install(&cfg, &s_eth);
  if (err != ESP_OK) {
    printf("[ETH ] driver_install: %s\n", esp_err_to_name(err));
    return false;
  }

  esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                      &eth_evt, NULL, NULL);
  return true;
}
#elif CONFIG_ETH_SPI_ETHERNET_W5500
/* Bring up the W5500 on SPI2 and install its MAC/PHY driver. No netif is
 * attached: frames are handled raw by the input callbacks above, exactly as
 * with the EMAC. The SPI clock is tried in the steps of W5500_SPI_HZ, from
 * fastest to slowest; the first one at which the chip identifies itself
 * correctly is kept. The W5500 interrupt line is serviced through the shared
 * GPIO ISR service, which is installed here if nobody did it yet. */
bool bridge_eth_init(void) {
  esp_err_t err = gpio_install_isr_service(0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {   /* already installed is fine */
    printf("[ETH ] gpio_install_isr_service: %s\n", esp_err_to_name(err));
    return false;
  }

  spi_bus_config_t bus = {};
  bus.sclk_io_num   = W5500_SCLK_GPIO;
  bus.mosi_io_num   = W5500_MOSI_GPIO;
  bus.miso_io_num   = W5500_MISO_GPIO;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = W5500_MAC_MAX_TRANSFER;   /* RX batches, see w5500_mac.h */
  err = spi_bus_initialize(W5500_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    printf("[ETH ] spi_bus_initialize: %s\n", esp_err_to_name(err));
    return false;
  }

  eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
  mac_cfg.rx_task_stack_size = 4096;   /* our callback calls into the Wi-Fi stack */

  eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
  phy_cfg.phy_addr       = -1;          /* the W5500's internal PHY */
  phy_cfg.reset_gpio_num = W5500_RST_GPIO;

  int hz_used = 0;
  for (size_t i = 0; i < sizeof(W5500_SPI_HZ) / sizeof(W5500_SPI_HZ[0]); i++) {
    spi_device_interface_config_t dev = {};
    dev.mode           = 0;
    dev.clock_speed_hz = W5500_SPI_HZ[i];
    dev.queue_size     = 20;
    dev.spics_io_num   = W5500_CS_GPIO;

    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(W5500_SPI_HOST, &dev);
    w5500.int_gpio_num = W5500_INT_GPIO;

    esp_eth_mac_t *mac = w5500_mac_new(&w5500, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
    if (!mac || !phy) {
      printf("[ETH ] Could not create W5500 MAC/PHY object\n");
      if (mac) mac->del(mac);
      if (phy) phy->del(phy);
      return false;
    }

    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&cfg, &s_eth);
    if (err == ESP_OK) { hz_used = W5500_SPI_HZ[i]; s_w5500_mac = mac; break; }

    printf("[ETH ] W5500 not responding at %d MHz (%s)\n",
           W5500_SPI_HZ[i] / 1000000, esp_err_to_name(err));
    /* deinit removes the INT handler in case mac->init succeeded and only the
     * PHY failed - otherwise it would point into freed memory after del(). */
    mac->deinit(mac);
    mac->del(mac);
    phy->del(phy);
    s_eth = NULL;
  }
  if (!s_eth) return false;
  s_eth_spi_mhz = (uint8_t)(hz_used / 1000000);
  printf("[ETH ] W5500 on SPI at %u MHz\n", (unsigned)s_eth_spi_mhz);

  /* The W5500 has no factory MAC. In promiscuous mode its own address plays
   * no role in the data path (frames are sent with the client's source MAC),
   * but the driver needs a valid one. */
  uint8_t own[6];
  if (esp_read_mac(own, ESP_MAC_ETH) != ESP_OK) {
    esp_read_mac(own, ESP_MAC_WIFI_STA);
    own[0] |= 0x02;   /* locally administered */
  }
  esp_eth_ioctl(s_eth, ETH_CMD_S_MAC_ADDR, own);

  esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                      &eth_evt, NULL, NULL);
  return true;
}
#endif

/* Start Ethernet with the sniff callback and poll until the client MAC is
 * known or the timeout expires. On success, detach the sniff callback and
 * switch the receive filter to bridge mode (eth_set_bridge_filters), so
 * frames for the client's MAC, which the MAC would otherwise filter, are
 * received. */
bool bridge_sniff_client_mac(uint32_t timeout_ms) {
  esp_eth_update_input_path(s_eth, eth_sniff_cb, NULL);
  if (esp_eth_start(s_eth) != ESP_OK) {
    printf("[ETH ] Start failed\n");
    return false;
  }

  printf("[ETH ] Waiting for first client frame...\n");
  const uint32_t t0 = millis();
  uint32_t last_log = t0;

  while (!s_mac_known) {
    if (timeout_ms && (millis() - t0) > timeout_ms) {
      printf("[ETH ] Timeout - no client detected\n");
      return false;
    }
    if (millis() - last_log > 5000) {
      last_log = millis();
      printf("[ETH ] ...nothing yet. Is a client connected?\n");
    }
    delay(20);
  }

  char s[18]; mac2str(s_client_mac, s);
  printf("[ETH ] Client MAC: %s\n", s);

  esp_eth_update_input_path(s_eth, NULL, NULL);
  eth_set_bridge_filters();
  printf("[ETH ] Promiscuous mode on\n");
  return true;
}

/* ===========================================================================
 * Wi-Fi + management netif
 * ========================================================================= */

/* Is an IPv4 field set at all? 0.0.0.0 consistently means "not set" - see the
 * convention in config.h. */
static inline bool ip_is_set(const uint8_t a[4]) {
  return (a[0] | a[1] | a[2] | a[3]) != 0;
}

/* ---------------------------------------------------------------------------
 * Location detection via gateway ping
 *
 * The obvious idea of tying the profile to the SSID slot does not work: the
 * same SSID can exist at several sites. Then slot 1 would always connect and
 * profile 2 would never be used.
 *
 * So we ask the network itself: apply a profile, ping its gateway, and if it
 * stays silent try the next profile. This needs no extra input - the gateway
 * is part of the configuration anyway - and survives router or mesh changes,
 * unlike a hard-coded BSSID.
 * ------------------------------------------------------------------------ */

static volatile bool s_ping_reply;

/* esp_ping success callback: any reply is enough. */
static void ping_ok_cb(esp_ping_handle_t hdl, void *args) {
  (void)hdl; (void)args;
  s_ping_reply = true;
}

/* Ping `gw` over the management netif and wait up to total_ms for a reply.
 * Blocks the caller. Returns true on the first reply. If no ping session can
 * be created, returns true as well - an internal error must not be mistaken
 * for "unreachable" (that would make the bridge switch profiles or the
 * watchdog escalate for no reason). */
static bool gateway_reachable(const uint8_t gw[4], uint32_t total_ms) {
  if (!ip_is_set(gw)) return false;

  s_ping_reply = false;

  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.count       = 4;
  cfg.interval_ms = 250;
  cfg.timeout_ms  = 400;
  cfg.data_size   = 8;      /* keep it small, we only want a sign of life */
  /* ip_addr_t is NOT always the same struct: with IPv6 disabled
   * (CONFIG_LWIP_IPV6=n, see sdkconfig.defaults) lwIP shrinks the type to
   * struct ip4_addr - then there is neither .type nor the u_addr union. Not
   * to be confused with esp_netif_dns_info_t further down, which is
   * esp_ip_addr_t and keeps the union regardless. */
#if LWIP_IPV6
  cfg.target_addr.type = IPADDR_TYPE_V4;
  memcpy(&cfg.target_addr.u_addr.ip4.addr, gw, 4);
#else
  memcpy(&cfg.target_addr.addr, gw, 4);
#endif

  esp_ping_callbacks_t cbs = {};
  cbs.on_ping_success = ping_ok_cb;

  esp_ping_handle_t h = NULL;
  if (esp_ping_new_session(&cfg, &cbs, &h) != ESP_OK) {
    printf("[WIFI] Ping session failed - keeping profile\n");
    return true;
  }
  esp_ping_start(h);

  const uint32_t t0 = millis();
  while (!s_ping_reply && (millis() - t0) < total_ms) delay(50);

  esp_ping_stop(h);
  esp_ping_delete_session(h);
  return s_ping_reply;
}

/* Access to the three network profiles without special cases at every call
 * site. Index 0 = profile 1 (always valid), 1 = profile 2, 2 = profile 3. */
static const uint8_t *profile_ip(size_t i)   { return i==2 ? g_cfg.mgmt_ip3   : i==1 ? g_cfg.mgmt_ip2   : g_cfg.mgmt_ip;   }
static const uint8_t *profile_mask(size_t i) { return i==2 ? g_cfg.mgmt_mask3 : i==1 ? g_cfg.mgmt_mask2 : g_cfg.mgmt_mask; }
static const uint8_t *profile_gw(size_t i)   { return i==2 ? g_cfg.mgmt_gw3   : i==1 ? g_cfg.mgmt_gw2   : g_cfg.mgmt_gw;   }

/* Profile 1 is always in use; profiles 2 and 3 only if they have an IP. */
static bool profile_in_use(size_t i) { return i == 0 || ip_is_set(profile_ip(i)); }

/*
 * Apply the IP settings of profile `idx` to the management netif (falls back
 * to profile 1 if that profile is not in use).
 *
 * This cannot be done once before connecting: which network the bridge ends
 * up in is only known afterwards (see detect_location()).
 *
 * Empty mask/gateway fields inherit from profile 1 - a half-filled profile
 * should leave the board reachable instead of configuring it into the void
 * with mask 0.0.0.0. The gateway is also used as DNS server.
 */
static void mgmt_apply_ip(size_t idx) {
  if (!profile_in_use(idx)) idx = 0;
  const uint8_t *ip4  = profile_ip(idx);
  const uint8_t *mask = profile_mask(idx);
  const uint8_t *gw   = profile_gw(idx);
  if (!ip_is_set(mask)) mask = g_cfg.mgmt_mask;
  if (!ip_is_set(gw))   gw   = g_cfg.mgmt_gw;

  /* s_mgmt_ip is the copy that is_for_mgmt() compares against in the data
   * path - it MUST be updated too, otherwise the netif uses the new address
   * while the gatekeeper still lets the old one through. */
  memcpy(s_mgmt_ip, ip4, 4);

  esp_netif_ip_info_t ip = {};
  memcpy(&ip.ip.addr,      ip4,  4);
  memcpy(&ip.netmask.addr, mask, 4);
  memcpy(&ip.gw.addr,      gw,   4);
  if (esp_netif_set_ip_info(s_mgmt_netif, &ip) != ESP_OK) {
    printf("[WIFI] Could not set static IP\n");
  }

  esp_netif_dns_info_t dns = {};
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  memcpy(&dns.ip.u_addr.ip4.addr, gw, 4);
  esp_netif_set_dns_info(s_mgmt_netif, ESP_NETIF_DNS_MAIN, &dns);

  printf("[WIFI] Profile %u: %u.%u.%u.%u / %u.%u.%u.%u  GW %u.%u.%u.%u\n",
         (unsigned)(idx + 1),
         ip4[0], ip4[1], ip4[2], ip4[3],
         mask[0], mask[1], mask[2], mask[3],
         gw[0], gw[1], gw[2], gw[3]);
}

#ifndef BRIDGE_MINIMAL
/* Determine which site we are at, after the Wi-Fi connect succeeded.
 *
 * Tries every profile in use, starting with `preferred` (the profile of the
 * SSID slot that connected, the most likely match): apply it, ping its
 * gateway, stop at the first one that answers. If none answers, fall back to
 * the preferred profile - a possibly wrong but predictable address is better
 * than whichever one happened to be set last. */
static void detect_location(size_t preferred) {
  size_t order[3]; size_t n = 0;
  if (profile_in_use(preferred)) order[n++] = preferred;
  for (size_t k = 0; k < 3; k++)
    if (k != preferred && profile_in_use(k)) order[n++] = k;
  if (!n) { mgmt_apply_ip(0); return; }

  /* Only one profile in use - nothing to decide, and we save the ping wait
   * at boot. */
  if (n == 1) { mgmt_apply_ip(order[0]); return; }

  for (size_t j = 0; j < n; j++) {
    mgmt_apply_ip(order[j]);
    if (gateway_reachable(profile_gw(order[j]), 2500)) {
      printf("[WIFI] Gateway of profile %u answers - location detected\n",
             (unsigned)(order[j] + 1));
      return;
    }
    printf("[WIFI] Profile %u does not answer\n", (unsigned)(order[j] + 1));
  }
  printf("[WIFI] No gateway answers - staying with profile %u\n",
         (unsigned)(order[0] + 1));
  mgmt_apply_ip(order[0]);
}
#endif

/* --- Configured networks ------------------------------------------------- */

/* Index (0..2) of the network slot the STA is configured for right now. */
static volatile uint8_t s_cur_net = 0;

/* STA config bound to one BSSID (see sta_lock_ap()); s_ap_lock_skip = the
 * next disconnect is our own and must not release the binding. */
static volatile bool s_ap_locked    = false;
static volatile bool s_ap_lock_skip = false;

static const char *net_ssid(size_t i) { return i==2 ? g_cfg.ssid3 : i==1 ? g_cfg.ssid2 : g_cfg.ssid1; }
static const char *net_pass(size_t i) { return i==2 ? g_cfg.pass3 : i==1 ? g_cfg.pass2 : g_cfg.pass1; }
static bool        net_in_use(size_t i) { return net_ssid(i)[0] != '\0'; }

/* Point the STA at network slot i (driver may be started; a running
 * association is dropped by IDF). Takes the STRONGEST AP of that SSID, not
 * the first one found: the default WIFI_FAST_SCAN connects to the first
 * match, which in a mesh with several nodes under one SSID is a lottery -
 * and the ESP32 does not switch on its own afterwards. ALL_CHANNEL_SCAN
 * costs a few seconds per connect. */
static void sta_set_net(size_t i) {
  wifi_config_t wc = {};
  strncpy((char *)wc.sta.ssid,     net_ssid(i), sizeof(wc.sta.ssid) - 1);
  strncpy((char *)wc.sta.password, net_pass(i), sizeof(wc.sta.password) - 1);
  wc.sta.pmf_cfg.capable = true;
  wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
  if (e != ESP_OK) printf("[WIFI] set_config('%s'): %s\n", net_ssid(i), esp_err_to_name(e));
  s_cur_net = (uint8_t)i;
  s_ap_locked = false;
}

/* --- AP selection by band ------------------------------------------------- */

/* Under "both bands, 5 GHz preferred", a 5 GHz AP wins as long as its signal
 * is at least this strong; below that, the better range of 2.4 GHz is worth
 * more than the higher 5 GHz rates. */
#define BAND_5G_MIN_RSSI  (-75)

/* Is this AP on 5 GHz? Channels above 14 only exist there. */
static inline bool ap_is_5g(const wifi_ap_record_t *r) { return r->primary > 14; }

/* Band setting in effect: always "both, strongest" on 2.4 GHz-only chips. */
static uint8_t band_setting(void) {
#if CONFIG_SOC_WIFI_SUPPORT_5G
  return g_cfg.wifi_band <= WIFI_BAND_MAX ? g_cfg.wifi_band : WIFI_BAND_PREFER_5G;
#else
  return WIFI_BAND_STRONGEST;
#endif
}

/* May the station use this AP under the band setting? The "only" settings are
 * also enforced by the driver's band mode; checking here keeps scan results
 * from the other band out of the selection anyway. */
static bool ap_allowed(const wifi_ap_record_t *r) {
  switch (band_setting()) {
    case WIFI_BAND_ONLY_5G: return ap_is_5g(r);
    case WIFI_BAND_ONLY_2G: return !ap_is_5g(r);
    default:                return true;
  }
}

/* Ranking of an allowed AP, higher is better. With 5 GHz preferred, every
 * usable 5 GHz AP (>= BAND_5G_MIN_RSSI) ranks above every 2.4 GHz AP, and
 * within a group the signal decides. */
static int ap_rank(const wifi_ap_record_t *r) {
  if (band_setting() == WIFI_BAND_PREFER_5G && ap_is_5g(r) && r->rssi >= BAND_5G_MIN_RSSI)
    return 1000 + r->rssi;
  return r->rssi;
}

/* Best allowed AP for 'ssid' among scan records, skipping 'skip_bssid' (the
 * current AP when roaming; NULL = none). Returns false if none qualifies. */
static bool pick_best_ap(const char *ssid, const wifi_ap_record_t *recs, uint16_t n,
                         const uint8_t *skip_bssid, wifi_ap_record_t *out) {
  bool found = false;
  for (uint16_t k = 0; k < n; k++) {
    if (strcmp((const char *)recs[k].ssid, ssid) != 0) continue;
    if (skip_bssid && memcmp(recs[k].bssid, skip_bssid, 6) == 0) continue;
    if (!ap_allowed(&recs[k])) continue;
    if (!found || ap_rank(&recs[k]) > ap_rank(out)) { *out = recs[k]; found = true; }
  }
  return found;
}

/* Point the STA at network slot i, bound to one specific AP (BSSID and
 * channel from a scan). The driver then associates right away instead of
 * scanning all channels of both bands again first, which saves several
 * seconds per connect. The binding only holds for the next connection: the
 * first disconnect afterwards (a failed attempt or a later drop) releases it
 * in wifi_evt(), so a vanished AP cannot keep the bridge offline.
 * skip_next_disc: the caller is about to disconnect on purpose (roaming) -
 * that one disconnect must not release the binding yet. */
static void sta_lock_ap(size_t i, const wifi_ap_record_t *ap, bool skip_next_disc) {
  wifi_config_t wc = {};
  strncpy((char *)wc.sta.ssid,     net_ssid(i), sizeof(wc.sta.ssid) - 1);
  strncpy((char *)wc.sta.password, net_pass(i), sizeof(wc.sta.password) - 1);
  wc.sta.pmf_cfg.capable = true;
  wc.sta.scan_method = WIFI_FAST_SCAN;
  wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  wc.sta.bssid_set   = true;
  memcpy(wc.sta.bssid, ap->bssid, 6);
  wc.sta.channel     = ap->primary;
  esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
  if (e != ESP_OK) {
    printf("[WIFI] set_config(locked '%s'): %s\n", net_ssid(i), esp_err_to_name(e));
    sta_set_net(i);
    return;
  }
  s_cur_net         = (uint8_t)i;
  s_ap_lock_skip    = skip_next_disc;
  s_ap_locked       = true;
}

/* Called by wifi_evt() on every disconnect: releases a BSSID binding unless
 * this disconnect is the deliberate one announced by sta_lock_ap(). */
static void sta_release_lock_on_disconnect(void) {
  if (!s_ap_locked) return;
  if (s_ap_lock_skip) { s_ap_lock_skip = false; return; }
  sta_set_net(s_cur_net);
}

/* --- Last AP (fast start) ---------------------------------------------------
 * The AP of the last successful connection, kept in NVS so it survives a
 * power loss too. At boot, bridge_wifi_start() first tries it directly - no
 * scan - and only scans if that fails. Written only when it changes (roaming,
 * a different network), so the flash sees a write per AP change, not per
 * boot. */
#define LAST_AP_NS    "wifi_last"
#define LAST_AP_KEY   "ap"
#define LAST_AP_MAGIC 0x4c415031u   /* "LAP1" */

typedef struct {
  uint32_t magic;
  uint8_t  slot;       /* network slot 0..2 */
  uint8_t  channel;
  uint8_t  bssid[6];
} last_ap_t;

static last_ap_t     s_last_ap = {};
static volatile bool s_last_ap_dirty = false;

/* Load the stored last AP; false if none or invalid. */
static bool last_ap_load(last_ap_t *out) {
  nvs_handle_t h;
  if (nvs_open(LAST_AP_NS, NVS_READONLY, &h) != ESP_OK) return false;
  size_t sz = sizeof(*out);
  const esp_err_t e = nvs_get_blob(h, LAST_AP_KEY, out, &sz);
  nvs_close(h);
  return e == ESP_OK && sz == sizeof(*out) && out->magic == LAST_AP_MAGIC && out->slot < 3;
}

/* Called from wifi_evt() on every connect: remember the AP in RAM only. The
 * NVS write happens later in last_ap_tick(), outside the event task. */
static void last_ap_note(const uint8_t bssid[6], uint8_t channel) {
  if (s_last_ap.magic == LAST_AP_MAGIC && s_last_ap.slot == s_cur_net &&
      s_last_ap.channel == channel && memcmp(s_last_ap.bssid, bssid, 6) == 0) return;
  s_last_ap.magic   = LAST_AP_MAGIC;
  s_last_ap.slot    = s_cur_net;
  s_last_ap.channel = channel;
  memcpy(s_last_ap.bssid, bssid, 6);
  s_last_ap_dirty = true;
}

/* Persist a changed last AP; called from bridge_tick(). */
static void last_ap_tick(void) {
  if (!s_last_ap_dirty) return;
  s_last_ap_dirty = false;
  nvs_handle_t h;
  if (nvs_open(LAST_AP_NS, NVS_READWRITE, &h) != ESP_OK) return;
  esp_err_t e = nvs_set_blob(h, LAST_AP_KEY, &s_last_ap, sizeof(s_last_ap));
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  if (e != ESP_OK) printf("[WIFI] Storing the last AP failed: %s\n", esp_err_to_name(e));
}

/* Passive dwell time per channel for our scans. The IDF default of 360 ms
 * makes a dual-band scan slow, because most 5 GHz channels are DFS channels
 * that may only be scanned passively. APs beacon every ~102 ms, so 150 ms
 * still catches at least one beacon per channel. */
#define SCAN_PASSIVE_MS  150

/* One blocking scan of all channels (of the allowed bands); vis[i] = network
 * slot i was seen, best[i] = its best AP under the band setting (valid only
 * where have[i] is set - a slot can be visible only on a forbidden band).
 * Returns false if the scan itself failed (vis is then meaningless). Only
 * call while no connect attempt is running - IDF refuses to scan then. */
static bool scan_visible_nets(bool vis[3], wifi_ap_record_t best[3], bool have[3]) {
  wifi_scan_config_t sc = {};
  sc.scan_time.passive = SCAN_PASSIVE_MS;
  if (esp_wifi_scan_start(&sc, true) != ESP_OK) return false;
  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  if (n > 32) n = 32;
  wifi_ap_record_t *recs = n ? (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * n) : NULL;
  if (n && !recs) { esp_wifi_clear_ap_list(); return false; }
  if (n) esp_wifi_scan_get_ap_records(&n, recs);
  esp_wifi_clear_ap_list();    /* frees what did not fit into recs */
  for (uint16_t k = 0; k < n; k++)
    for (size_t i = 0; i < 3; i++)
      if (net_in_use(i) && strcmp((const char *)recs[k].ssid, net_ssid(i)) == 0) vis[i] = true;
  for (size_t i = 0; i < 3; i++)
    have[i] = net_in_use(i) && pick_best_ap(net_ssid(i), recs, n, NULL, &best[i]);
  free(recs);
  return true;
}

/* Bring up Wi-Fi in STA mode with the cloned client MAC (see bridge.h).
 *
 * Steps: create the management netif with a static IP, initialize the Wi-Fi
 * driver with the configured buffer sizes (with a staged fallback if init
 * fails), apply protocol/bandwidth settings, then try each configured SSID
 * with several attempts. On success the location detection selects the
 * matching IP profile. */
bool bridge_wifi_start(void) {
#ifdef BRIDGE_MINIMAL
  /* Measurement variant: NO management netif. Without s_mgmt_netif,
   * is_for_mgmt() returns false immediately and the data path forwards
   * everything unchecked - exactly like the Rust implementation this project
   * derives from. Used to verify whether the management stack costs
   * throughput. */
  printf("[WIFI] MINIMAL variant: no management netif\n");
#else
  /* Create the netif and switch it to static IMMEDIATELY. With DHCP the
   * cloned MAC would get the same address as the client - a collision. */
  s_mgmt_netif = esp_netif_create_default_wifi_sta();
  if (!s_mgmt_netif) {
    printf("[WIFI] Netif creation failed\n");
    return false;
  }
  esp_netif_dhcpc_stop(s_mgmt_netif);

  /* Apply profile 1 provisionally, so the netif never sits without a valid
   * address. After a successful connect the matching profile is applied. */
  mgmt_apply_ip(0);
#endif

  /* WIFI_INIT_CONFIG_DEFAULT() already takes static_rx_buf_num,
   * dynamic_rx_buf_num and dynamic_tx_buf_num from sdkconfig. Do not assign
   * fixed numbers here - that would override the Kconfig values and make
   * changes in sdkconfig.defaults silently ineffective. The buffer sizes
   * live in exactly one place. */
  wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
#if CONFIG_ESP_WIFI_AMPDU_TX_ENABLED
  wcfg.ampdu_tx_enable    = 1;
#endif
#if CONFIG_ESP_WIFI_AMPDU_RX_ENABLED
  wcfg.ampdu_rx_enable    = 1;
#endif
  /* tx_ba_win no longer exists as a field of wifi_init_config_t in this IDF
   * version - the TX block-ack window is negotiated with the AP at connect
   * time and cannot be preconfigured. rx_ba_win still exists.
   * In practice the client->Wi-Fi direction (Ethernet->Wi-Fi, the client's
   * upload path) depends on the AP's behavior rather than on our setting -
   * usually not a problem, but if throughput in exactly this direction is
   * weak, this is where to look. */
#ifdef CONFIG_ESP_WIFI_RX_BA_WIN
  wcfg.rx_ba_win          = CONFIG_ESP_WIFI_RX_BA_WIN;
#endif

  /* Portal overrides. 0 consistently means "don't touch", so the sdkconfig
   * value stays - see the convention in config.h. */
  if (g_cfg.static_rx_buf)  wcfg.static_rx_buf_num  = g_cfg.static_rx_buf;
  if (g_cfg.dynamic_rx_buf) wcfg.dynamic_rx_buf_num = g_cfg.dynamic_rx_buf;
  if (g_cfg.dynamic_tx_buf) wcfg.dynamic_tx_buf_num = g_cfg.dynamic_tx_buf;
  if (g_cfg.rx_ba_win)      wcfg.rx_ba_win          = g_cfg.rx_ba_win;

  /* The BA window is deliberately NOT clamped to min(static_rx,
   * dynamic_rx/2), the often-quoted Kconfig rule. With the default buffers
   * (16/32) such a clamp would cut rx_ba_win from 32 to 16 - precisely the
   * parameter that governs receive aggregation and thus download speed. IDF
   * does not enforce the rule; this combination works. Should esp_wifi_init()
   * fail on a window that is too large, the fallback below handles it - that
   * is the right place for it. */

  /* Staged fallback instead of all-or-nothing.
   *
   * Falling back straight to WIFI_INIT_CONFIG_DEFAULT() would not help: it
   * contains the SAME Kconfig values, including the same rx_ba_win. If the
   * call failed because of it, the fallback would fail too, and the
   * ESP_ERROR_CHECK after it would turn that into a crash loop.
   *
   * So first the likely culprit (the BA window) is defused before giving up
   * completely - and every stage says in the log that it kicked in. A silent
   * fallback would be particularly treacherous here, because it looks like a
   * setting that has no effect. */
  esp_err_t werr = esp_wifi_init(&wcfg);
  if (werr != ESP_OK) {
    printf("[WIFI] init with rx_ba_win=%d failed (%s) - reducing\n",
           wcfg.rx_ba_win, esp_err_to_name(werr));
    uint16_t cap = wcfg.static_rx_buf_num;
    if (wcfg.dynamic_rx_buf_num / 2 < cap) cap = wcfg.dynamic_rx_buf_num / 2;
    if (cap < 2) cap = 2;
    wcfg.rx_ba_win = cap;
    werr = esp_wifi_init(&wcfg);
    if (werr == ESP_OK) {
      printf("[WIFI] init ok with rx_ba_win=%u\n", (unsigned)cap);
    }
  }
  if (werr != ESP_OK) {
    printf("[WIFI] that failed too (%s) - using full defaults\n",
           esp_err_to_name(werr));
    wifi_init_config_t fb = WIFI_INIT_CONFIG_DEFAULT();
    fb.rx_ba_win = 6;          /* conservative, fits any buffer count */
    ESP_ERROR_CHECK(esp_wifi_init(&fb));
    wcfg = fb;
  }

  /* Remember for display what actually went into the driver - recorded only
   * after the stage that succeeded, so a fallback shows up in the portal as
   * a difference between saved and effective values. */
  s_eff_static_rx = wcfg.static_rx_buf_num;
  s_eff_dyn_rx    = wcfg.dynamic_rx_buf_num;
  s_eff_dyn_tx    = wcfg.dynamic_tx_buf_num;
  s_eff_ba_win    = wcfg.rx_ba_win;

  /* disc_on_restart() disconnects on its own, so it does not depend on
   * running before Wi-Fi's own esp_wifi_stop handler. */
  esp_err_t sh = esp_register_shutdown_handler(disc_on_restart);
  if (sh != ESP_OK) printf("[WIFI] shutdown handler: %s\n", esp_err_to_name(sh));

  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));   /* throughput over power */

  /* 802.11b can be disabled: the b rates are slow and, as basic rates, waste
   * airtime. Only sensible if no pure 802.11b device is left on the network -
   * otherwise the AP might not see us anymore. */
#if CONFIG_SOC_WIFI_SUPPORT_5G
  /* Dual-band chips (ESP32-C5): allow 2.4 and 5 GHz, so the scan can pick
   * the strongest AP on either band, with 802.11ax on both. In auto band
   * mode the single-band APIs esp_wifi_set_protocol/_bandwidth return
   * ESP_ERR_NOT_SUPPORTED, hence the plural variants. The 802.11b switch only
   * concerns 2.4 GHz; 5 GHz has no b rates. */
  /* "Only" settings are enforced by the driver, so neither its own scans nor
   * a reconnect can ever end up on the other band. */
  const uint8_t band = band_setting();
  esp_wifi_set_band_mode(band == WIFI_BAND_ONLY_5G ? WIFI_BAND_MODE_5G_ONLY :
                         band == WIFI_BAND_ONLY_2G ? WIFI_BAND_MODE_2G_ONLY :
                                                     WIFI_BAND_MODE_AUTO);
  wifi_protocols_t protos = {};
  protos.ghz_2g = g_cfg.no_11b
      ? (WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX)
      : (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX);
  protos.ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC |
                  WIFI_PROTOCOL_11AX;
  esp_err_t pr = esp_wifi_set_protocols(WIFI_IF_STA, &protos);
  if (pr != ESP_OK) {
    printf("[WIFI] set_protocols: %s\n", esp_err_to_name(pr));
  }
#else
  esp_wifi_set_protocol(WIFI_IF_STA,
      g_cfg.no_11b ? (WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N)
                   : (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
#endif

  /* HT40 doubles the channel width and thus the gross data rate, but only
   * takes effect if the AP actually offers 40 MHz on 2.4 GHz - many don't,
   * for coexistence reasons. Without AP support it silently stays at HT20,
   * which is not an error. */
#if CONFIG_SOC_WIFI_SUPPORT_5G
  /* The HT40 switch applies to both bands. On 5 GHz 40 MHz is the norm, as
   * there is no 2.4 GHz coexistence restriction. */
  wifi_bandwidths_t bws = {};
  bws.ghz_2g = g_cfg.ht40 ? WIFI_BW_HT40 : WIFI_BW_HT20;
  bws.ghz_5g = g_cfg.ht40 ? WIFI_BW_HT40 : WIFI_BW_HT20;
  esp_err_t bw = esp_wifi_set_bandwidths(WIFI_IF_STA, &bws);
#else
  esp_err_t bw = esp_wifi_set_bandwidth(
      WIFI_IF_STA, g_cfg.ht40 ? WIFI_BW_HT40 : WIFI_BW_HT20);
#endif
  if (bw != ESP_OK) {
    printf("[WIFI] set_bandwidth: %s\n", esp_err_to_name(bw));
  }

  /* Register after the esp_netif handlers - the order matters. */
  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                      &wifi_evt, NULL, NULL);

  /* Portal overrides, 0 = firmware default - same convention as the other
   * tuning values (config.h). Remembered for display, and they also set how
   * long the runtime failover waits (see failover_tick()). */
  const uint32_t connect_timeout_ms = g_cfg.wifi_connect_timeout_s
      ? (uint32_t)g_cfg.wifi_connect_timeout_s * 1000u : WIFI_CONNECT_TIMEOUT_MS_DEF;
  const int connect_retries = g_cfg.wifi_connect_retries
      ? g_cfg.wifi_connect_retries : WIFI_CONNECT_RETRIES_DEF;
  s_eff_wifi_connect_timeout_s = (uint16_t)(connect_timeout_ms / 1000);
  s_eff_wifi_connect_retries   = (uint8_t)connect_retries;

  size_t first = 3;
  for (size_t i = 0; i < 3 && first == 3; i++) if (net_in_use(i)) first = i;
  if (first == 3) return false;

  /* The MAC is the same for every network, so the driver is started once:
   * set_config, THEN set_mac, THEN start. Switching networks afterwards only
   * needs a new config (sta_set_net()). */
  sta_set_net(first);
  esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, s_client_mac);
  if (err != ESP_OK) {
    printf("[WIFI] set_mac: %s\n", esp_err_to_name(err));
    return false;
  }
  ESP_ERROR_CHECK(esp_wifi_start());

  /* Needs a started driver, like the TX power below. Purely diagnostic, so
   * a failure is only logged. */
  esp_err_t td = esp_wifi_set_tx_done_cb(wifi_tx_done);
  if (td != ESP_OK) printf("[WIFI] tx_done_cb: %s\n", esp_err_to_name(td));

  /* The driver scans on its own before every connect and reconnect, even
   * with BSSID and channel given, and by default dwells 360 ms on every
   * passive (DFS) channel - on a dual-band chip that alone is close to 10 s.
   * Use the same shorter passive dwell as our own scans. Needs a started
   * driver. */
  /* Filled by hand: WIFI_SCAN_PARAMS_DEFAULT_CONFIG() uses nested C
   * designators that C++ does not accept. Everything but the passive time
   * stays at the IDF default. */
  wifi_scan_default_params_t sp = {};
  sp.scan_time.active.min    = WIFI_ACTIVE_SCAN_MIN_DEFAULT_TIME;
  sp.scan_time.active.max    = WIFI_ACTIVE_SCAN_MAX_DEFAULT_TIME;
  sp.scan_time.passive       = SCAN_PASSIVE_MS;
  sp.home_chan_dwell_time    = WIFI_SCAN_HOME_CHANNEL_DWELL_DEFAULT_TIME;
  esp_err_t spe = esp_wifi_set_scan_parameters(&sp);
  if (spe != ESP_OK) printf("[WIFI] set_scan_parameters: %s\n", esp_err_to_name(spe));

  /* TX power only AFTER esp_wifi_start() - before that the API returns
   * ESP_ERR_WIFI_NOT_STARTED. */
  bridge_apply_live_tuning();

  /* Connect order: the configured networks that one scan actually sees, in
   * the configured priority, then the unseen ones (a hidden SSID does not
   * show up in a scan but may still connect). Without the scan, an absent
   * network 1 would cost its full timeout x attempts on every boot before
   * network 2 is even tried. If the scan fails, the plain priority order is
   * used. */
  /* While connecting, let the Wi-Fi driver report its state changes
   * (auth -> assoc -> run) with timestamps, so a slow connect can be traced
   * in the log. Back to the quiet default once connected or given up. */
  esp_log_level_set("wifi", ESP_LOG_INFO);

  /* Fast path: straight to the AP of the last successful connection, no
   * scan. Only if it is still allowed under the band setting; the attempt
   * ends early on the first disconnect (AP not found, rejected) instead of
   * waiting out the full timeout. */
  last_ap_t la;
  if (last_ap_load(&la) && net_in_use(la.slot)) {
    wifi_ap_record_t rec = {};
    memcpy(rec.bssid, la.bssid, 6);
    rec.primary = la.channel;
    if (ap_allowed(&rec)) {
      s_last_ap = la;   /* known, so an identical connect does not rewrite NVS */
      sta_lock_ap(la.slot, &rec, false);
      printf("[WIFI] Connecting to '%s' via last AP %02x:%02x:%02x:%02x:%02x:%02x, channel %u (no scan)...\n",
             net_ssid(la.slot), la.bssid[0], la.bssid[1], la.bssid[2], la.bssid[3],
             la.bssid[4], la.bssid[5], (unsigned)la.channel);
      const uint32_t disc0 = s_wifi_disc_count;
      esp_wifi_connect();
      const uint32_t t0 = millis();
      while (!s_wifi_up && s_wifi_disc_count == disc0 && (millis() - t0) < connect_timeout_ms)
        delay(100);
      if (s_wifi_up) {
        s_cur_net = la.slot;
        esp_log_level_set("wifi", ESP_LOG_WARN);
#ifndef BRIDGE_MINIMAL
        detect_location(la.slot);
#endif
        return true;
      }
      printf("[WIFI] Last AP not reachable - scanning\n");
      disc_mark_own(DISC_SRC_CONNECT);
      esp_wifi_disconnect();
      sta_set_net(la.slot);
    }
  }

  bool vis[3] = { false, false, false };
  bool have[3] = { false, false, false };
  wifi_ap_record_t best[3];
  const bool scanned = scan_visible_nets(vis, best, have);
  size_t order[3]; size_t n = 0;
  for (size_t i = 0; i < 3; i++) if (net_in_use(i) && (!scanned || vis[i])) order[n++] = i;
  for (size_t i = 0; i < 3; i++) if (net_in_use(i) && scanned && !vis[i]) order[n++] = i;

  for (size_t j = 0; j < n; j++) {
    const size_t i = order[j];
    /* The driver was started with the first slot in use, but the scan may
     * have put another one first - always point it at this one. */
    if (i != s_cur_net) sta_set_net(i);
    /* First attempt straight to the AP the scan picked; the binding is
     * released by the first failure (wifi_evt()), so further attempts fall
     * back to the driver's own full scan. */
    if (scanned && have[i]) {
      sta_lock_ap(i, &best[i], false);
      printf("[WIFI] Connecting to '%s' via %02x:%02x:%02x:%02x:%02x:%02x, channel %u (%s, %d dBm)...\n",
             net_ssid(i), best[i].bssid[0], best[i].bssid[1], best[i].bssid[2],
             best[i].bssid[3], best[i].bssid[4], best[i].bssid[5], (unsigned)best[i].primary,
             ap_is_5g(&best[i]) ? "5 GHz" : "2.4 GHz", (int)best[i].rssi);
    } else {
      printf("[WIFI] Connecting to '%s'%s...\n", net_ssid(i),
             scanned ? (vis[i] ? " (seen in scan, no AP on an allowed band)" : " (not seen in scan)") : "");
    }

    for (int attempt = 0; attempt < connect_retries; attempt++) {
      if (attempt > 0) {
        printf("[WIFI] '%s' attempt %d/%d...\n", net_ssid(i),
               attempt + 1, connect_retries);
        delay(WIFI_CONNECT_RETRY_DELAY_MS);
      }
      esp_wifi_connect();

      uint32_t t0 = millis();
      while (!s_wifi_up && (millis() - t0) < connect_timeout_ms) delay(100);
      if (s_wifi_up) break;
      disc_mark_own(DISC_SRC_CONNECT);
      esp_wifi_disconnect();
    }
    if (s_wifi_up) {
      s_cur_net = (uint8_t)i;
      esp_log_level_set("wifi", ESP_LOG_WARN);
      /* Connected - but at WHICH site? The SSID slot does not tell when
       * several sites share the same SSID. So try the profiles in use and
       * ping their gateways, starting with the profile of the connected slot
       * because it is the most likely match. */
#ifndef BRIDGE_MINIMAL
      detect_location(i);
#endif
      return true;
    }

    printf("[WIFI] '%s' failed after %d attempts\n", net_ssid(i), connect_retries);
    disc_mark_own(DISC_SRC_CONNECT);
    esp_wifi_disconnect();
  }
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_wifi_stop();
  return false;
}

/* Arm the data path: route Ethernet frames to eth_rx_cb and start
 * forwarding. */
void bridge_activate(void) {
  esp_eth_update_input_path(s_eth, eth_rx_cb, NULL);
  s_active = true;
  printf("[BRDG] Data path active\n");
}

bool bridge_is_active(void) { return s_active; }
esp_netif_t *bridge_mgmt_netif(void) { return s_mgmt_netif; }
void bridge_get_mgmt_ip(uint8_t out[4]) { memcpy(out, s_mgmt_ip, 4); }

/* Watchdog settings with the 0 convention resolved - defined up here because
 * both bridge_get_effective() and watchdog_tick() need them. Unknown bits in
 * wd_actions are ignored; an empty mask means "all actions". */
static uint8_t wd_actions_eff(void) {
  const uint8_t m = g_cfg.wd_actions & WD_ACT_ALL;
  return m ? m : WD_ACT_ALL;
}

static uint8_t wd_bad_windows_eff(void) {
  return g_cfg.wd_bad_windows ? g_cfg.wd_bad_windows : WD_BAD_WINDOWS_DEF;
}

/* Firmware defaults for every numeric tuning value (for the portal's
 * "Defaults" button). */
void bridge_get_defaults(bridge_tuning_t *o) {
  o->static_rx    = CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM;
  o->dyn_rx       = CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM;
  o->dyn_tx       = CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM;
  o->ba_win       = CONFIG_ESP_WIFI_RX_BA_WIN;
  o->eth_retries  = ETH_TX_RETRIES_DEF;
  o->wifi_retries = WIFI_TX_RETRIES_DEF;
  o->wifi_connect_timeout_s = WIFI_CONNECT_TIMEOUT_MS_DEF / 1000;
  o->wifi_connect_retries   = WIFI_CONNECT_RETRIES_DEF;
  o->ap_idle_reboot_s       = AP_IDLE_REBOOT_S_DEF;
  o->roam_check_s           = ROAM_CHECK_S_DEF;
  o->roam_rssi_threshold    = ROAM_RSSI_THRESHOLD_DEF;
  o->roam_margin_db         = ROAM_MARGIN_DB_DEF;
  o->wd_actions             = WD_ACT_ALL;
  o->wd_bad_windows         = WD_BAD_WINDOWS_DEF;
}

/* Values currently in effect: what went into the driver at init, the live
 * retry counters, and the config values with the 0 convention resolved. */
void bridge_get_effective(bridge_tuning_t *o) {
  o->static_rx    = s_eff_static_rx;
  o->dyn_rx       = s_eff_dyn_rx;
  o->dyn_tx       = s_eff_dyn_tx;
  o->ba_win       = s_eff_ba_win;
  o->eth_retries  = s_eth_tx_retries;
  o->wifi_retries = s_wifi_tx_retries;
  o->wifi_connect_timeout_s = s_eff_wifi_connect_timeout_s;
  o->roam_check_s        = g_cfg.roam_check_s        ? g_cfg.roam_check_s        : ROAM_CHECK_S_DEF;
  o->roam_rssi_threshold = g_cfg.roam_rssi_threshold  ? g_cfg.roam_rssi_threshold : ROAM_RSSI_THRESHOLD_DEF;
  o->roam_margin_db      = g_cfg.roam_margin_db       ? g_cfg.roam_margin_db      : ROAM_MARGIN_DB_DEF;
  o->wifi_connect_retries   = s_eff_wifi_connect_retries;
  o->ap_idle_reboot_s       = g_cfg.ap_idle_reboot_s ? g_cfg.ap_idle_reboot_s : AP_IDLE_REBOOT_S_DEF;
  o->wd_actions             = wd_actions_eff();
  o->wd_bad_windows         = wd_bad_windows_eff();
}

void bridge_get_client_mac(uint8_t out[6]) { memcpy(out, s_client_mac, 6); }

/* Use a stored MAC as the client MAC (fallback when the sniff timed out). */
void bridge_set_client_mac(const uint8_t mac[6]) {
  memcpy(s_client_mac, mac, 6);
  s_mac_known = true;
  char t[18]; mac2str(s_client_mac, t);
  printf("[ETH ] Client MAC from storage: %s\n", t);

  /* Do the same finishing steps bridge_sniff_client_mac() does after a hit -
   * on a timeout it returns before reaching them: detach the sniff callback
   * and enable promiscuous mode. Without this the bridge would run with the
   * sniff callback installed and without promiscuous mode, i.e. it would only
   * pass broadcasts and transport practically nothing. */
  esp_eth_update_input_path(s_eth, NULL, NULL);
  eth_set_bridge_filters();
  printf("[ETH ] Promiscuous mode on\n");
}

/* ===========================================================================
 * Runtime tuning
 * ========================================================================= */

/* See bridge.h. Also aborts a running autotune, so a manually saved value is
 * not overwritten by it. */
void bridge_apply_live_tuning(void) {
  /* Retry counters: 0 here does NOT mean "never send" but "default". A real
   * 0 would be a silent total failure of the data path - so the fallback
   * catches it instead of relying on the portal's input validation. */
  s_eth_tx_retries  = g_cfg.eth_tx_retries  ? g_cfg.eth_tx_retries  : ETH_TX_RETRIES_DEF;
  s_wifi_tx_retries = g_cfg.wifi_tx_retries ? g_cfg.wifi_tx_retries : WIFI_TX_RETRIES_DEF;

  /* A manually saved value beats the autotune: a measurement still running
   * would otherwise overwrite the value just entered, at its next candidate
   * or with its result. A finished result is acknowledged as well, so the
   * portal does not fill it in again. */
  bridge_autotune_reset();
  bridge_txp_tune_reset();
  bridge_cap_tune_reset();
  bridge_long_tune_reset();
  s_txq_cap = g_cfg.txq_cap;

  /* Remember the PHY's initial value on the first call. */
  if (!s_txp_default) {
    int8_t p = 0;
    if (esp_wifi_get_max_tx_power(&p) == ESP_OK && p > 0) s_txp_default = p;
  }

  if (!g_cfg.tx_power) {
    /* 0 means "default" - and that must actively restore it, not just do
     * nothing. */
    if (s_txp_default) {
      esp_wifi_set_max_tx_power(s_txp_default);
      printf("[WIFI] TX power back to default (%d.%02d dBm)\n",
             s_txp_default / 4, (s_txp_default % 4) * 25);
    }
  } else {
    esp_err_t e = esp_wifi_set_max_tx_power(g_cfg.tx_power);
    if (e != ESP_OK) {
      printf("[WIFI] set_max_tx_power(%d): %s\n",
             (int)g_cfg.tx_power, esp_err_to_name(e));
    } else {
      /* Print the value actually applied, not the requested one: the PHY
       * only knows 11 steps and rounds down. */
      int8_t actual = 0;
      if (esp_wifi_get_max_tx_power(&actual) == ESP_OK) {
        printf("[WIFI] TX power: %d.%02d dBm (requested %d.%02d)\n",
               actual / 4, (actual % 4) * 25,
               g_cfg.tx_power / 4, (g_cfg.tx_power % 4) * 25);
      }
    }
  }

  printf("[BRDG] Retries eth->wifi=%u wifi->eth=%u, TX queue cap %u\n",
         (unsigned)s_eth_tx_retries, (unsigned)s_wifi_tx_retries, (unsigned)s_txq_cap);
}

int16_t bridge_get_tx_power(void) {
  int8_t p = 0;
  if (esp_wifi_get_max_tx_power(&p) != ESP_OK) return -1;
  return p;
}

uint8_t bridge_get_bandwidth_mhz(void) {
  wifi_bandwidth_t bw;
#if CONFIG_SOC_WIFI_SUPPORT_5G
  /* In auto band mode only the per-band query exists; the current channel
   * decides which of the two values is in effect. */
  wifi_bandwidths_t bws;
  uint8_t ch = 0;
  wifi_second_chan_t sec;
  if (esp_wifi_get_bandwidths(WIFI_IF_STA, &bws) != ESP_OK) return 0;
  if (esp_wifi_get_channel(&ch, &sec) != ESP_OK) return 0;
  bw = (ch > 14) ? bws.ghz_5g : bws.ghz_2g;
#else
  if (esp_wifi_get_bandwidth(WIFI_IF_STA, &bw) != ESP_OK) return 0;
#endif
  return (bw == WIFI_BW_HT40) ? 40 : 20;
}

/* See bridge.h. */
void bridge_set_paused(bool paused) {
  /* Only suspend forwarding, do not stop the drivers. The management
   * connection (and thus the running upload) stays up, because delivery of
   * management frames to lwIP happens before the pause check in both
   * callbacks. */
  s_paused = paused;
  printf("[BRDG] Data path %s\n", paused ? "paused" : "active");
}

/* ===========================================================================
 * Autotune of the TX retries
 * ========================================================================= */

/*
 * Procedure:
 *
 *   1. Measure the CURRENT value - this also yields the reference packet rate
 *      of the stream.
 *   2. If it misses the target: go UP as long as every step brings clearly
 *      less loss.
 *   3. Go DOWN from the start value as long as the loss stays within the
 *      allowed bounds; stop at the first outlier - smaller only gets worse.
 *
 * Why not start from the bottom: at 1-2 retries a camera's TCP stream can
 * collapse entirely (a fraction of the normal packet rate, the bridge barely
 * reachable for minutes, heap running low) - the measurement would then fail
 * with "too little traffic" that it caused itself. Coming from above it never
 * reaches such a value in the first place, and the collapse guard aborts a
 * step after a few seconds instead of running it for the full 19 s.
 */
static const uint8_t AT_CANDIDATES[] = { 2, 3, 4, 6, 8, 10, 12, 14, 16, 20 };
#define AT_N            (sizeof(AT_CANDIDATES) / sizeof(AT_CANDIDATES[0]))
#define AT_SETTLE_MS     4000u    /* let the buffers drain                 */
#define AT_MEASURE_MS   15000u    /* then count for this long              */
#define AT_MIN_PACKETS    400u    /* below this the measurement is useless */
#define AT_TARGET_LOSS     10u    /* hundredths of a percent: 0.10 % - good enough */
#define AT_COLLAPSE_MS   6000u    /* from here on, check the step for a collapse */
#define AT_COLLAPSE_PCT    30u    /* < 30 % of the reference rate = collapse */
#define AT_LOSS_COLLAPSED 0xFFFEu /* marker in s_at_loss                   */

typedef enum { AT_PH_START = 0, AT_PH_UP, AT_PH_DOWN } at_phase_t;

static autotune_state_t s_at_state = AT_IDLE;
static uint8_t   s_at_idx, s_at_result;
static uint8_t   s_at_start_idx, s_at_steps;
static at_phase_t s_at_phase;
static uint32_t  s_at_t0, s_at_pkt0, s_at_drop0, s_at_pkt_step0;
static uint32_t  s_at_ref_pps;           /* packets/s in the start measurement */
static bool      s_at_measuring;
/* 0xFFFF = not measured - also before the first run, so /api/autotune does
 * not report a perfect 0 % for values that were never tried. */
static uint16_t  s_at_loss[AT_N] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
                                     0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
static_assert(AT_N == 10, "update the s_at_loss initializer");
static uint8_t   s_at_prev;              /* value before the run, for aborts */

/* Index of the candidate closest to `value` (ties go to the larger one). */
static uint8_t at_nearest_idx(uint8_t value) {
  uint8_t best = 0;
  for (uint8_t i = 1; i < AT_N; i++) {
    const int di = abs((int)AT_CANDIDATES[i] - (int)value);
    const int db = abs((int)AT_CANDIDATES[best] - (int)value);
    if (di < db || (di == db && AT_CANDIDATES[i] > AT_CANDIDATES[best])) best = i;
  }
  return best;
}

/* Begin measuring candidate `idx`: apply its retry value and restart the
 * settle/measure timer. */
static void at_start_step(uint8_t idx) {
  s_at_idx = idx;
  s_eth_tx_retries = AT_CANDIDATES[idx];
  s_at_measuring = false;
  s_at_t0 = millis();
  s_at_pkt_step0 = s_pkt_e2w;
}

/* See bridge.h. Starts at the candidate closest to the current value. */
bool bridge_autotune_start(void) {
  if (!s_active || s_at_state == AT_RUNNING || bridge_txp_tune_state() == AT_RUNNING ||
      bridge_cap_tune_state() == AT_RUNNING || bridge_long_tune_state() == AT_RUNNING)
    return false;
  s_at_prev      = s_eth_tx_retries;
  s_at_start_idx = at_nearest_idx(s_at_prev);
  s_at_phase     = AT_PH_START;
  s_at_steps     = 0;
  s_at_ref_pps   = 0;
  s_at_state     = AT_RUNNING;
  for (size_t i = 0; i < AT_N; i++) s_at_loss[i] = 0xFFFF;
  at_start_step(s_at_start_idx);
  printf("[AUTO] Start at %u retries (current %u), %u s per step\n",
         (unsigned)AT_CANDIDATES[s_at_start_idx], (unsigned)s_at_prev,
         (unsigned)((AT_SETTLE_MS + AT_MEASURE_MS) / 1000));
  return true;
}

uint8_t bridge_autotune_candidate(void) { return AT_CANDIDATES[s_at_idx]; }

void bridge_autotune_reset(void) {
  if (s_at_state == AT_RUNNING)
    printf("[AUTO] Aborted - value was saved manually\n");
  s_at_state = AT_IDLE;
}

autotune_state_t bridge_autotune_state(void)  { return s_at_state; }
uint8_t bridge_autotune_step(void)            { return s_at_steps; }
uint8_t bridge_autotune_count(void)           { return AT_N; }
uint8_t bridge_autotune_result(void)          { return s_at_result; }
void bridge_autotune_values(uint8_t *k, uint16_t *v) {
  for (size_t i = 0; i < AT_N; i++) { k[i] = AT_CANDIDATES[i]; v[i] = s_at_loss[i]; }
}

/* Lowest loss measured so far (excluding collapses), 0xFFFF = none. */
static uint16_t at_best_loss(void) {
  uint16_t b = 0xFFFF;
  for (size_t i = 0; i < AT_N; i++)
    if (s_at_loss[i] < AT_LOSS_COLLAPSED && s_at_loss[i] < b) b = s_at_loss[i];
  return b;
}

/* What still counts as "good enough": the target, or - if even the best value
 * misses it - slightly above the best one (measurement noise). Without this
 * slack a randomly marginally better high value would always win, although it
 * blocks Ethernet reception longer. */
static uint16_t at_allowed_loss(void) {
  const uint16_t b = at_best_loss();
  if (b == 0xFFFF) return AT_TARGET_LOSS;
  const uint32_t slack = b / 4 > 3 ? b / 4 : 3;
  const uint32_t e = (uint32_t)b + slack;
  return (uint16_t)(e > AT_TARGET_LOSS ? e : AT_TARGET_LOSS);
}

/* End the run: pick the smallest measured candidate within the allowed loss,
 * apply and persist it. If nothing usable was measured, restore the previous
 * value and report "too little traffic". */
static void at_finish(void) {
  const uint16_t allowed = at_allowed_loss();
  uint8_t chosen = 0;
  for (size_t i = 0; i < AT_N; i++)
    if (s_at_loss[i] <= allowed) { chosen = AT_CANDIDATES[i]; break; }
  if (!chosen) {                      /* nothing usable measured */
    s_eth_tx_retries = s_at_prev;
    s_at_state = AT_TOO_LITTLE_TRAFFIC;
    printf("[AUTO] No valid measurement - staying at %u\n", (unsigned)s_at_prev);
    return;
  }
  s_at_result = chosen;
  s_eth_tx_retries = chosen;
  g_cfg.eth_tx_retries = chosen;
  cfg_save();
  s_at_state = AT_DONE;
  printf("[AUTO] Done after %u measurements - chosen: %u (saved)\n",
         (unsigned)s_at_steps, (unsigned)chosen);
}

/* Decide the next step after a measurement (or a collapse) at index
 * s_at_idx: either start the next step or finish. Implements the
 * start -> up -> down phase logic described above. */
static void at_advance(bool collapsed) {
  const uint16_t v = s_at_loss[s_at_idx];
  const uint16_t allowed = at_allowed_loss();

  if (s_at_phase == AT_PH_START) {
    if (!collapsed && v > AT_TARGET_LOSS && s_at_idx + 1 < (int)AT_N) {
      s_at_phase = AT_PH_UP;
      at_start_step(s_at_idx + 1);
      return;
    }
    s_at_phase = AT_PH_DOWN;
  } else if (s_at_phase == AT_PH_UP) {
    /* Keep going up only while it gets noticeably better (>= 25 % and at
     * least 0.02 percentage points) and the target is still missed. */
    const uint16_t prev = s_at_loss[s_at_idx - 1];
    const bool better = !collapsed && prev < AT_LOSS_COLLAPSED &&
                        v + 2 <= prev && (uint32_t)v * 4 <= (uint32_t)prev * 3;
    if (better && v > AT_TARGET_LOSS && s_at_idx + 1 < (int)AT_N) {
      at_start_step(s_at_idx + 1);
      return;
    }
    /* Going down only makes sense if the start value was already within
     * bounds - otherwise smaller values only get worse. */
    if (s_at_loss[s_at_start_idx] > allowed || s_at_start_idx == 0) {
      at_finish();
      return;
    }
    s_eth_tx_retries = AT_CANDIDATES[s_at_start_idx];
    s_at_phase = AT_PH_DOWN;
    at_start_step(s_at_start_idx - 1);
    return;
  } else {  /* AT_PH_DOWN */
    if (collapsed || v > allowed) {
      at_finish();
      return;
    }
  }

  /* AT_PH_DOWN: continue downwards if there is room. When coming from the
   * start phase, this begins right below the start value. */
  if (s_at_idx == 0) { at_finish(); return; }
  if (s_at_phase == AT_PH_DOWN && s_at_loss[s_at_idx] <= allowed) {
    at_start_step(s_at_idx - 1);
    return;
  }
  at_finish();
}

/* Autotune state machine; called from bridge_tick(), i.e. several times per
 * second. Each step first settles (AT_SETTLE_MS), then counts packets and
 * drops for AT_MEASURE_MS, then hands over to at_advance(). */
static void autotune_tick(void) {
  if (s_at_state != AT_RUNNING) return;
  const uint32_t elapsed = millis() - s_at_t0;

  /* Collapse guard: from the start measurement on, the normal packet rate is
   * known. If it falls far below that during a step, the stream is
   * collapsing - go back to the safe start value right away, not only after
   * 19 s. */
  if (s_at_ref_pps && elapsed >= AT_COLLAPSE_MS) {
    const uint32_t pkt = s_pkt_e2w - s_at_pkt_step0;
    const uint32_t expected = (uint32_t)((uint64_t)s_at_ref_pps * elapsed / 1000);
    if ((uint64_t)pkt * 100 < (uint64_t)expected * AT_COLLAPSE_PCT) {
      printf("[AUTO] Retries %2u -> collapse (%u instead of ~%u packets) - aborting step\n",
             (unsigned)AT_CANDIDATES[s_at_idx], (unsigned)pkt, (unsigned)expected);
      s_at_loss[s_at_idx] = AT_LOSS_COLLAPSED;
      s_at_steps++;
      s_eth_tx_retries = AT_CANDIDATES[s_at_start_idx];
      at_advance(true);
      return;
    }
  }

  if (!s_at_measuring) {
    if (elapsed < AT_SETTLE_MS) return;
    s_at_pkt0 = s_pkt_e2w; s_at_drop0 = s_drop_e2w;
    s_at_measuring = true;
    return;
  }
  if (elapsed < AT_SETTLE_MS + AT_MEASURE_MS) return;

  const uint32_t dp = s_pkt_e2w  - s_at_pkt0;
  const uint32_t dd = s_drop_e2w - s_at_drop0;
  s_at_steps++;

  if (dp + dd < AT_MIN_PACKETS) {
    if (s_at_phase == AT_PH_START) {
      /* Hardly any traffic even at the start value: any number would be
       * random - better to abort honestly than to fake a result. */
      printf("[AUTO] Aborted: only %u packets in %u s - too little traffic\n",
             (unsigned)(dp + dd), (unsigned)(AT_MEASURE_MS / 1000));
      s_eth_tx_retries = s_at_prev;
      s_at_state = AT_TOO_LITTLE_TRAFFIC;
      return;
    }
    /* Later on: treat it like a collapse. */
    s_at_loss[s_at_idx] = AT_LOSS_COLLAPSED;
    s_eth_tx_retries = AT_CANDIDATES[s_at_start_idx];
    at_advance(true);
    return;
  }

  s_at_loss[s_at_idx] = (uint16_t)((10000ULL * dd) / (dp + dd));
  if (s_at_phase == AT_PH_START)
    s_at_ref_pps = (dp + dd) * 1000 / AT_MEASURE_MS;
  printf("[AUTO] Retries %2u -> %u.%02u %% loss (%u packets)\n",
         (unsigned)AT_CANDIDATES[s_at_idx],
         (unsigned)(s_at_loss[s_at_idx] / 100),
         (unsigned)(s_at_loss[s_at_idx] % 100), (unsigned)(dp + dd));
  at_advance(false);
}

/* ===========================================================================
 * Autotune of the TX power
 * ========================================================================= */

/*
 * The uplink (client -> AP) is the direction an IP camera depends on, and
 * the ESP32's own TX power is the only RF lever the bridge has for it. More
 * power is not automatically better: the PHY's error vector magnitude grows
 * at the top of its range, so high-rate frames can need more retries. So it
 * is measured, not assumed.
 *
 * Metric per step (settle 4 s, measure 15 s, live traffic):
 *   - loss: frames without a TX buffer plus frames the MAC abandoned on the
 *     air, relative to all frames;
 *   - queue delay: how long a bridged frame waited in the driver queue
 *     (Little's law over the TX-done accounting). A weak uplink shows up
 *     here long before it shows up as loss - lower rates and hardware
 *     retries make the queue drain slowly, which is also what drives ping
 *     times and heap use up.
 *
 * Order: the current value first (reference packet rate for the collapse
 * guard), then every higher step, then lower steps until one leaves the
 * tolerance. Steps the driver maps to the same actual power as the previous
 * one are not measured again - the chip may cap well below 20 dBm.
 *
 * The HIGHEST power within tolerance of the best measurement wins, i.e.
 * power is only reduced where more of it measurably hurts. A few minutes
 * of measuring cannot see the bad moments (interference bursts, a
 * neighbouring AP switching channel) in which link margin decides whether
 * the stream survives - so equal results are no reason to give it up.
 */
static const int8_t TP_CANDIDATES[] = { 52, 56, 60, 66, 72, 80 };  /* 13..20 dBm */
#define TP_N               (sizeof(TP_CANDIDATES) / sizeof(TP_CANDIDATES[0]))
#define TP_DELAY_SLACK_X10 10u   /* +1 ms absolute tolerance on the delay */

typedef enum { TP_PH_START = 0, TP_PH_UP, TP_PH_DOWN } tp_phase_t;

static autotune_state_t s_tp_state = AT_IDLE;
static tp_phase_t s_tp_phase;
static uint8_t   s_tp_idx, s_tp_start_idx, s_tp_steps;
static int8_t    s_tp_result;
static bool      s_tp_measuring;
static uint32_t  s_tp_t0, s_tp_pkt_step0, s_tp_ref_pps;
static uint32_t  s_tp_pkt0, s_tp_drop0, s_tp_fail0, s_tp_qs0, s_tp_dn0;
/* 1/100 %, see AT_LOSS_COLLAPSED; 0xFFFF = not measured (also before the
 * first run). */
static uint16_t  s_tp_loss[TP_N]  = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
static uint16_t  s_tp_delay[TP_N] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };  /* 0.1 ms */
static_assert(TP_N == 6, "update the s_tp_loss/s_tp_delay initializers");
static int8_t    s_tp_actual[TP_N];      /* what the driver made of it      */

/* Apply a TX power value (0.25 dBm units; 0 = PHY default). */
static void tp_apply(int8_t p) {
  esp_wifi_set_max_tx_power(p ? p : s_txp_default);
}

static void tp_advance(bool collapsed);

/* Begin measuring candidate `idx`. If the driver maps it to the same actual
 * power as an already measured neighbour, there is nothing new to learn:
 * take over that result and move on right away. */
static void tp_start_step(uint8_t idx) {
  s_tp_idx = idx;
  tp_apply(TP_CANDIDATES[idx]);
  int8_t a = 0;
  s_tp_actual[idx] = esp_wifi_get_max_tx_power(&a) == ESP_OK ? a : 0;
  for (int n = (int)idx - 1; n <= (int)idx + 1; n += 2) {
    if (n < 0 || n >= (int)TP_N || s_tp_loss[n] == 0xFFFF) continue;
    if (s_tp_actual[n] != s_tp_actual[idx]) continue;
    s_tp_loss[idx]  = s_tp_loss[n];
    s_tp_delay[idx] = s_tp_delay[n];
    printf("[TXP ] %d -> driver %d, same as %d - not measured again\n",
           (int)TP_CANDIDATES[idx], (int)a, (int)TP_CANDIDATES[n]);
    tp_advance(s_tp_loss[idx] == AT_LOSS_COLLAPSED);
    return;
  }
  s_tp_measuring = false;
  s_tp_t0 = millis();
  s_tp_pkt_step0 = s_pkt_e2w;
}

/* See bridge.h. */
bool bridge_txp_tune_start(void) {
  if (!s_active || s_tp_state == AT_RUNNING || s_at_state == AT_RUNNING ||
      bridge_cap_tune_state() == AT_RUNNING || bridge_long_tune_state() == AT_RUNNING)
    return false;
  const int8_t cur = g_cfg.tx_power ? g_cfg.tx_power : s_txp_default;
  uint8_t best = 0;
  for (uint8_t i = 1; i < TP_N; i++)
    if (abs(TP_CANDIDATES[i] - cur) < abs(TP_CANDIDATES[best] - cur)) best = i;
  s_tp_start_idx = best;
  s_tp_phase = TP_PH_START;
  s_tp_steps = 0;
  s_tp_ref_pps = 0;
  s_tp_result = 0;
  for (size_t i = 0; i < TP_N; i++) { s_tp_loss[i] = 0xFFFF; s_tp_delay[i] = 0xFFFF; s_tp_actual[i] = 0; }
  s_tp_state = AT_RUNNING;
  tp_start_step(best);
  printf("[TXP ] Start at %d (configured %d), %u s per step\n",
         (int)TP_CANDIDATES[best], (int)g_cfg.tx_power,
         (unsigned)((AT_SETTLE_MS + AT_MEASURE_MS) / 1000));
  return true;
}

/* See bridge.h. Restores the configured power if a run is aborted. */
void bridge_txp_tune_reset(void) {
  if (s_tp_state == AT_RUNNING) {
    printf("[TXP ] Aborted - value was saved manually\n");
    tp_apply(g_cfg.tx_power);
  }
  s_tp_state = AT_IDLE;
}

autotune_state_t bridge_txp_tune_state(void) { return s_tp_state; }
uint8_t bridge_txp_tune_step(void)           { return s_tp_steps; }
int8_t  bridge_txp_tune_candidate(void)      { return TP_CANDIDATES[s_tp_idx]; }
int8_t  bridge_txp_tune_result(void)         { return s_tp_result; }
size_t  bridge_txp_tune_values(int8_t *k, int8_t *act, uint16_t *loss, uint16_t *delay_x10) {
  for (size_t i = 0; i < TP_N; i++) {
    k[i] = TP_CANDIDATES[i]; act[i] = s_tp_actual[i];
    loss[i] = s_tp_loss[i];  delay_x10[i] = s_tp_delay[i];
  }
  return TP_N;
}

/* Best loss and best delay over the valid measurements so far. */
static void tp_best(uint16_t *loss, uint16_t *delay) {
  *loss = 0xFFFF; *delay = 0xFFFF;
  for (size_t i = 0; i < TP_N; i++) {
    if (s_tp_loss[i] >= AT_LOSS_COLLAPSED) continue;
    if (s_tp_loss[i] < *loss)   *loss  = s_tp_loss[i];
    if (s_tp_delay[i] < *delay) *delay = s_tp_delay[i];
  }
}

/* Is candidate i within tolerance of the best measurement? Loss uses the
 * same slack rule as the retry autotune; the delay may be 25 % + 1 ms worse,
 * because bursty camera traffic makes it noisier than the loss. */
static bool tp_within(size_t i) {
  if (s_tp_loss[i] >= AT_LOSS_COLLAPSED) return false;
  uint16_t bl, bd;
  tp_best(&bl, &bd);
  const uint32_t slack = bl / 4 > 3 ? bl / 4 : 3;
  uint32_t allowed = (uint32_t)bl + slack;
  if (allowed < AT_TARGET_LOSS) allowed = AT_TARGET_LOSS;
  const uint32_t allowed_delay = (uint32_t)bd + bd / 4 + TP_DELAY_SLACK_X10;
  return s_tp_loss[i] <= allowed && s_tp_delay[i] <= allowed_delay;
}

/* Pick the highest power within tolerance, apply and save it. */
static void tp_finish(void) {
  for (size_t i = TP_N; i-- > 0;) {
    if (s_tp_loss[i] == 0xFFFF || !tp_within(i)) continue;
    /* Several requests can end up at the same driver power - save the
     * smallest of them, so the configuration does not claim a power the
     * chip never delivers. */
    while (i > 0 && s_tp_actual[i - 1] == s_tp_actual[i] && s_tp_loss[i - 1] != 0xFFFF) i--;
    s_tp_result = TP_CANDIDATES[i];
    g_cfg.tx_power = s_tp_result;
    cfg_save();
    tp_apply(s_tp_result);
    s_tp_state = AT_DONE;
    printf("[TXP ] Done after %u measurements - chosen: %d (saved)\n",
           (unsigned)s_tp_steps, (int)s_tp_result);
    return;
  }
  tp_apply(g_cfg.tx_power);
  s_tp_state = AT_TOO_LITTLE_TRAFFIC;
  printf("[TXP ] No valid measurement - staying at %d\n", (int)g_cfg.tx_power);
}

/* Next step after a measurement (or collapse) at s_tp_idx. */
static void tp_advance(bool collapsed) {
  if (s_tp_phase == TP_PH_START) s_tp_phase = TP_PH_UP;
  if (s_tp_phase == TP_PH_UP) {
    if (!collapsed && s_tp_idx + 1 < (int)TP_N) { tp_start_step(s_tp_idx + 1); return; }
    s_tp_phase = TP_PH_DOWN;
    if (s_tp_start_idx > 0 && tp_within(s_tp_start_idx)) { tp_start_step(s_tp_start_idx - 1); return; }
    tp_finish();
    return;
  }
  /* Down: lower power only gets worse once it leaves the tolerance. */
  if (!collapsed && s_tp_idx > 0 && tp_within(s_tp_idx)) { tp_start_step(s_tp_idx - 1); return; }
  tp_finish();
}

/* TX power autotune state machine, driven from bridge_tick() like
 * autotune_tick(), with the same settle/measure timing and collapse guard. */
static void txp_tune_tick(void) {
  if (s_tp_state != AT_RUNNING) return;
  const uint32_t elapsed = millis() - s_tp_t0;

  if (s_tp_ref_pps && elapsed >= AT_COLLAPSE_MS) {
    const uint32_t pkt = s_pkt_e2w - s_tp_pkt_step0;
    const uint32_t expected = (uint32_t)((uint64_t)s_tp_ref_pps * elapsed / 1000);
    if ((uint64_t)pkt * 100 < (uint64_t)expected * AT_COLLAPSE_PCT) {
      printf("[TXP ] %d -> collapse (%u instead of ~%u packets) - aborting step\n",
             (int)TP_CANDIDATES[s_tp_idx], (unsigned)pkt, (unsigned)expected);
      s_tp_loss[s_tp_idx] = AT_LOSS_COLLAPSED;
      s_tp_steps++;
      tp_apply(TP_CANDIDATES[s_tp_start_idx]);
      tp_advance(true);
      return;
    }
  }

  if (!s_tp_measuring) {
    if (elapsed < AT_SETTLE_MS) return;
    s_tp_pkt0 = s_pkt_e2w; s_tp_drop0 = s_drop_e2w; s_tp_fail0 = s_air_fail;
    s_tp_qs0 = s_q_sum;    s_tp_dn0 = s_air_done;
    s_tp_measuring = true;
    return;
  }
  if (elapsed < AT_SETTLE_MS + AT_MEASURE_MS) return;

  const uint32_t dp = s_pkt_e2w - s_tp_pkt0;
  const uint32_t dd = (s_drop_e2w - s_tp_drop0) + (s_air_fail - s_tp_fail0);
  const uint32_t dsum = s_q_sum - s_tp_qs0, ddone = s_air_done - s_tp_dn0;
  s_tp_steps++;

  if (dp + dd < AT_MIN_PACKETS || !ddone) {
    if (s_tp_phase == TP_PH_START) {
      printf("[TXP ] Aborted: only %u packets in %u s - too little traffic\n",
             (unsigned)(dp + dd), (unsigned)(AT_MEASURE_MS / 1000));
      tp_apply(g_cfg.tx_power);
      s_tp_state = AT_TOO_LITTLE_TRAFFIC;
      return;
    }
    s_tp_loss[s_tp_idx] = AT_LOSS_COLLAPSED;
    tp_apply(TP_CANDIDATES[s_tp_start_idx]);
    tp_advance(true);
    return;
  }

  s_tp_loss[s_tp_idx] = (uint16_t)((10000ULL * dd) / (dp + dd));
  const uint64_t d10 = (10ULL * dsum * AT_MEASURE_MS) / ((uint64_t)ddone * ddone);
  s_tp_delay[s_tp_idx] = (uint16_t)(d10 < 0xFFFE ? d10 : 0xFFFE);
  if (s_tp_phase == TP_PH_START) s_tp_ref_pps = (dp + dd) * 1000 / AT_MEASURE_MS;
  printf("[TXP ] %d (driver %d) -> %u.%02u %% loss, queue %u.%u ms (%u packets)\n",
         (int)TP_CANDIDATES[s_tp_idx], (int)s_tp_actual[s_tp_idx],
         (unsigned)(s_tp_loss[s_tp_idx] / 100), (unsigned)(s_tp_loss[s_tp_idx] % 100),
         (unsigned)(s_tp_delay[s_tp_idx] / 10), (unsigned)(s_tp_delay[s_tp_idx] % 10),
         (unsigned)(dp + dd));
  tp_advance(false);
}

/* ===========================================================================
 * Autotune of the TX queue cap
 * ========================================================================= */

/*
 * The cap trades loss for latency, so the question is how small it can get
 * before it costs anything measurable. Order: no cap first (the reference
 * for loss, throughput and packet rate), then ever smaller caps; the first
 * step that loses more than the tolerance allows (same slack rule as the
 * other autotunes, relative to the reference) or delivers less than 90 % of
 * the reference throughput ends the run. A camera's encoder produces a fixed
 * bitrate, so a TCP stream that backs off under the drops shows up as lost
 * throughput. The smallest cap that passed is saved - or "off" if none did.
 *
 * 15 s windows see the immediate effect on loss and rate, not how a
 * recorder copes with the retransmissions over hours - worth a look at the
 * long-run drop counter after applying a result.
 */
static const uint8_t QC_CANDIDATES[] = { 0, 24, 16, 12, 8 };   /* 0 = no cap */
#define QC_N          (sizeof(QC_CANDIDATES) / sizeof(QC_CANDIDATES[0]))
#define QC_RATE_PCT   90u    /* minimum throughput vs. the reference */
#define QC_NONE       0xFFu  /* "no result" marker for s_qc_result    */

static autotune_state_t s_qc_state = AT_IDLE;
static uint8_t   s_qc_idx, s_qc_steps, s_qc_result = QC_NONE;
static bool      s_qc_measuring;
static uint32_t  s_qc_t0, s_qc_pkt_step0, s_qc_ref_pps;
static uint32_t  s_qc_pkt0, s_qc_drop0, s_qc_fail0, s_qc_qs0, s_qc_dn0;
static uint64_t  s_qc_by0;
static uint16_t  s_qc_loss[QC_N]  = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };  /* 1/100 % */
static uint16_t  s_qc_delay[QC_N] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };  /* 0.1 ms  */
static uint16_t  s_qc_kbps[QC_N];
static_assert(QC_N == 5, "update the s_qc_* initializers");

/* Begin measuring candidate `idx`. */
static void qc_start_step(uint8_t idx) {
  s_qc_idx = idx;
  s_txq_cap = QC_CANDIDATES[idx];
  s_qc_measuring = false;
  s_qc_t0 = millis();
  s_qc_pkt_step0 = s_pkt_e2w;
}

/* See bridge.h. */
bool bridge_cap_tune_start(void) {
  if (!s_active || s_qc_state == AT_RUNNING || s_at_state == AT_RUNNING ||
      s_tp_state == AT_RUNNING || bridge_long_tune_state() == AT_RUNNING) return false;
  for (size_t i = 0; i < QC_N; i++) { s_qc_loss[i] = 0xFFFF; s_qc_delay[i] = 0xFFFF; s_qc_kbps[i] = 0; }
  s_qc_steps = 0;
  s_qc_ref_pps = 0;
  s_qc_result = QC_NONE;
  s_qc_state = AT_RUNNING;
  qc_start_step(0);
  printf("[QCAP] Start (configured cap %u), %u s per step\n",
         (unsigned)g_cfg.txq_cap, (unsigned)((AT_SETTLE_MS + AT_MEASURE_MS) / 1000));
  return true;
}

/* See bridge.h. Restores the configured cap if a run is aborted. */
void bridge_cap_tune_reset(void) {
  if (s_qc_state == AT_RUNNING) {
    printf("[QCAP] Aborted - value was saved manually\n");
    s_txq_cap = g_cfg.txq_cap;
  }
  s_qc_state = AT_IDLE;
}

autotune_state_t bridge_cap_tune_state(void) { return s_qc_state; }
uint8_t bridge_cap_tune_step(void)           { return s_qc_steps; }
uint8_t bridge_cap_tune_candidate(void)      { return QC_CANDIDATES[s_qc_idx]; }
uint8_t bridge_cap_tune_result(void)         { return s_qc_result; }
size_t  bridge_cap_tune_values(uint8_t *k, uint16_t *loss, uint16_t *delay_x10, uint16_t *kbps) {
  for (size_t i = 0; i < QC_N; i++) {
    k[i] = QC_CANDIDATES[i]; loss[i] = s_qc_loss[i];
    delay_x10[i] = s_qc_delay[i]; kbps[i] = s_qc_kbps[i];
  }
  return QC_N;
}

/* Does step i keep up with the reference (step 0)? */
static bool qc_passes(size_t i) {
  if (s_qc_loss[i] >= AT_LOSS_COLLAPSED) return false;
  const uint32_t ref = s_qc_loss[0];
  const uint32_t slack = ref / 4 > 3 ? ref / 4 : 3;
  uint32_t allowed = ref + slack;
  if (allowed < AT_TARGET_LOSS) allowed = AT_TARGET_LOSS;
  return s_qc_loss[i] <= allowed &&
         (uint32_t)s_qc_kbps[i] * 100 >= (uint32_t)s_qc_kbps[0] * QC_RATE_PCT;
}

/* End the run with the smallest passing cap (index `last_ok`), save it. */
static void qc_finish(uint8_t last_ok) {
  s_qc_result = QC_CANDIDATES[last_ok];
  s_txq_cap = s_qc_result;
  g_cfg.txq_cap = s_qc_result;
  cfg_save();
  s_qc_state = AT_DONE;
  printf("[QCAP] Done after %u measurements - chosen: %s%u (saved)\n",
         (unsigned)s_qc_steps, s_qc_result ? "cap " : "no cap ", (unsigned)s_qc_result);
}

/* TX queue cap autotune state machine, driven from bridge_tick() with the
 * same settle/measure timing and collapse guard as the other autotunes. */
static void cap_tune_tick(void) {
  if (s_qc_state != AT_RUNNING) return;
  const uint32_t elapsed = millis() - s_qc_t0;

  if (s_qc_ref_pps && elapsed >= AT_COLLAPSE_MS) {
    const uint32_t pkt = s_pkt_e2w - s_qc_pkt_step0;
    const uint32_t expected = (uint32_t)((uint64_t)s_qc_ref_pps * elapsed / 1000);
    if ((uint64_t)pkt * 100 < (uint64_t)expected * AT_COLLAPSE_PCT) {
      printf("[QCAP] Cap %u -> collapse (%u instead of ~%u packets) - stopping\n",
             (unsigned)QC_CANDIDATES[s_qc_idx], (unsigned)pkt, (unsigned)expected);
      s_qc_loss[s_qc_idx] = AT_LOSS_COLLAPSED;
      s_qc_steps++;
      qc_finish(s_qc_idx - 1);
      return;
    }
  }

  if (!s_qc_measuring) {
    if (elapsed < AT_SETTLE_MS) return;
    s_qc_pkt0 = s_pkt_e2w; s_qc_drop0 = s_drop_e2w; s_qc_fail0 = s_air_fail;
    s_qc_qs0 = s_q_sum;    s_qc_dn0 = s_air_done; s_qc_by0 = s_by_e2w;
    s_qc_measuring = true;
    return;
  }
  if (elapsed < AT_SETTLE_MS + AT_MEASURE_MS) return;

  const uint32_t dp = s_pkt_e2w - s_qc_pkt0;
  const uint32_t dd = (s_drop_e2w - s_qc_drop0) + (s_air_fail - s_qc_fail0);
  const uint32_t dsum = s_q_sum - s_qc_qs0, ddone = s_air_done - s_qc_dn0;
  const uint64_t dby = s_by_e2w - s_qc_by0;
  s_qc_steps++;

  if (dp + dd < AT_MIN_PACKETS || !ddone) {
    if (s_qc_idx == 0) {
      printf("[QCAP] Aborted: only %u packets in %u s - too little traffic\n",
             (unsigned)(dp + dd), (unsigned)(AT_MEASURE_MS / 1000));
      s_txq_cap = g_cfg.txq_cap;
      s_qc_state = AT_TOO_LITTLE_TRAFFIC;
      return;
    }
    s_qc_loss[s_qc_idx] = AT_LOSS_COLLAPSED;
    qc_finish(s_qc_idx - 1);
    return;
  }

  s_qc_loss[s_qc_idx] = (uint16_t)((10000ULL * dd) / (dp + dd));
  const uint64_t d10 = (10ULL * dsum * AT_MEASURE_MS) / ((uint64_t)ddone * ddone);
  s_qc_delay[s_qc_idx] = (uint16_t)(d10 < 0xFFFE ? d10 : 0xFFFE);
  const uint64_t kb = dby * 8 / AT_MEASURE_MS;
  s_qc_kbps[s_qc_idx] = (uint16_t)(kb < 0xFFFF ? kb : 0xFFFF);
  if (s_qc_idx == 0) s_qc_ref_pps = (dp + dd) * 1000 / AT_MEASURE_MS;
  printf("[QCAP] Cap %2u -> %u.%02u %% loss, queue %u.%u ms, %u kbit/s (%u packets)\n",
         (unsigned)QC_CANDIDATES[s_qc_idx],
         (unsigned)(s_qc_loss[s_qc_idx] / 100), (unsigned)(s_qc_loss[s_qc_idx] % 100),
         (unsigned)(s_qc_delay[s_qc_idx] / 10), (unsigned)(s_qc_delay[s_qc_idx] % 10),
         (unsigned)s_qc_kbps[s_qc_idx], (unsigned)(dp + dd));

  if (s_qc_idx > 0 && !qc_passes(s_qc_idx)) { qc_finish(s_qc_idx - 1); return; }
  if (s_qc_idx + 1 < (int)QC_N) { qc_start_step(s_qc_idx + 1); return; }
  qc_finish(s_qc_idx);
}

/* ===========================================================================
 * Long-run check of the TX retries
 * ========================================================================= */

/*
 * The retry autotune measures 15 s per value and picks the smallest one
 * below 0.10 % loss. It cannot separate values that both stay below that:
 * in practice 8 and 12 retries both pass in 15 s, while hours of operation
 * show one losing several times more than the other. Only a long
 * comparison sees that, so this runs one.
 *
 * Candidates: one step below the current value, the value itself and two
 * steps above - the short autotune tends to land too low, so the check
 * looks mainly upwards. They are rotated in 5-minute blocks (A B C D A B
 * ...), each block counted after a 10 s settle, so every candidate sees the
 * same mix of busy and quiet hours - a fair A/B instead of "A in the
 * afternoon, B at night".
 *
 * At the end the smallest value whose loss rate is at most 1.5x the best
 * one (+50 ppm for noise) wins - smaller values block the Ethernet receive
 * task less. A candidate needs LR_MIN_PACKETS to count at all; one that
 * loses more than 1 % is taken out of the rotation immediately, so a value
 * that is too small cannot hurt the stream for hours. RAM only: a reboot
 * ends the run without a result.
 */
#define LR_MAX_K        4
#define LR_BLOCK_MS     300000u   /* 5 min per block                     */
#define LR_SETTLE_MS     10000u   /* not counted at the start of a block */
#define LR_MIN_PACKETS   50000u   /* per candidate, for a valid result    */
#define LR_BAD_PPM       10000u   /* 1 % - leaves the rotation at once    */
#define LR_BAD_MIN_PKTS   5000u   /* ...judged after this many packets    */
#define LR_SLACK_PPM        50u

static autotune_state_t s_lr_state = AT_IDLE;
static uint8_t   s_lr_k[LR_MAX_K], s_lr_n, s_lr_cur, s_lr_prev, s_lr_result;
static bool      s_lr_bad[LR_MAX_K], s_lr_measuring;
static uint32_t  s_lr_pkts[LR_MAX_K], s_lr_drops[LR_MAX_K];
static uint32_t  s_lr_t_start, s_lr_dur_ms, s_lr_block_t0, s_lr_pkt0, s_lr_drop0;

/* Loss of candidate i in parts per million (0xFFFFFFFF = no packets). */
static uint32_t lr_ppm(size_t i) {
  const uint64_t all = (uint64_t)s_lr_pkts[i] + s_lr_drops[i];
  return all ? (uint32_t)(1000000ULL * s_lr_drops[i] / all) : 0xFFFFFFFFu;
}

/* Start a block for candidate i. */
static void lr_start_block(uint8_t i) {
  s_lr_cur = i;
  s_eth_tx_retries = s_lr_k[i];
  s_lr_block_t0 = millis();
  s_lr_measuring = false;
}

/* Add the counts since the block's settle phase ended to candidate cur. */
static void lr_account(void) {
  if (!s_lr_measuring) return;
  s_lr_pkts[s_lr_cur]  += s_pkt_e2w - s_lr_pkt0;
  s_lr_drops[s_lr_cur] += (s_drop_e2w - s_lr_drop0);
  s_lr_measuring = false;
}

/* See bridge.h. */
bool bridge_long_tune_start(uint8_t hours) {
  if (!s_active || s_lr_state == AT_RUNNING || s_at_state == AT_RUNNING ||
      s_tp_state == AT_RUNNING || s_qc_state == AT_RUNNING) return false;
  if (hours < 1)  hours = 1;
  if (hours > 48) hours = 48;
  s_lr_prev = s_eth_tx_retries;
  const uint8_t c = at_nearest_idx(s_lr_prev);
  s_lr_n = 0;
  for (int i = (int)c - 1; i <= (int)c + 2 && s_lr_n < LR_MAX_K; i++) {
    if (i < 0 || i >= (int)AT_N) continue;
    s_lr_k[s_lr_n] = AT_CANDIDATES[i];
    s_lr_pkts[s_lr_n] = 0; s_lr_drops[s_lr_n] = 0; s_lr_bad[s_lr_n] = false;
    s_lr_n++;
  }
  s_lr_dur_ms  = (uint32_t)hours * 3600000u;
  s_lr_t_start = millis();
  s_lr_result  = 0;
  s_lr_state   = AT_RUNNING;
  lr_start_block(0);
  printf("[LONG] Retries: %u h, %u candidates around %u, 5 min blocks\n",
         (unsigned)hours, (unsigned)s_lr_n, (unsigned)s_lr_prev);
  return true;
}

/* See bridge.h. Restores the configured retries if a run is aborted. */
void bridge_long_tune_reset(void) {
  if (s_lr_state == AT_RUNNING) {
    printf("[LONG] Aborted\n");
    s_eth_tx_retries = g_cfg.eth_tx_retries ? g_cfg.eth_tx_retries : ETH_TX_RETRIES_DEF;
  }
  s_lr_state = AT_IDLE;
}

autotune_state_t bridge_long_tune_state(void) { return s_lr_state; }
uint8_t  bridge_long_tune_candidate(void)     { return s_lr_k[s_lr_cur]; }
uint8_t  bridge_long_tune_result(void)        { return s_lr_result; }
void     bridge_long_tune_progress(uint32_t *elapsed_min, uint32_t *total_min) {
  *elapsed_min = s_lr_state == AT_RUNNING ? (millis() - s_lr_t_start) / 60000u : 0;
  *total_min   = s_lr_dur_ms / 60000u;
}
size_t   bridge_long_tune_values(uint8_t *k, uint32_t *pkts, uint32_t *drops, bool *bad) {
  for (size_t i = 0; i < s_lr_n; i++) {
    k[i] = s_lr_k[i]; pkts[i] = s_lr_pkts[i]; drops[i] = s_lr_drops[i]; bad[i] = s_lr_bad[i];
  }
  return s_lr_n;
}

/* Choose and save the result once the duration is over. */
static void lr_finish(void) {
  uint32_t best = 0xFFFFFFFFu;
  for (size_t i = 0; i < s_lr_n; i++)
    if (!s_lr_bad[i] && s_lr_pkts[i] >= LR_MIN_PACKETS && lr_ppm(i) < best) best = lr_ppm(i);
  if (best == 0xFFFFFFFFu) {
    s_eth_tx_retries = s_lr_prev;
    s_lr_state = AT_TOO_LITTLE_TRAFFIC;
    printf("[LONG] Not enough traffic for a result - staying at %u\n", (unsigned)s_lr_prev);
    return;
  }
  const uint32_t allowed = best + best / 2 + LR_SLACK_PPM;
  for (size_t i = 0; i < s_lr_n; i++) {
    if (s_lr_bad[i] || s_lr_pkts[i] < LR_MIN_PACKETS || lr_ppm(i) > allowed) continue;
    s_lr_result = s_lr_k[i];
    break;
  }
  s_eth_tx_retries = s_lr_result;
  g_cfg.eth_tx_retries = s_lr_result;
  cfg_save();
  s_lr_state = AT_DONE;
  printf("[LONG] Done - chosen: %u retries (best %lu ppm, allowed %lu ppm, saved)\n",
         (unsigned)s_lr_result, (unsigned long)best, (unsigned long)allowed);
}

/* Long-run state machine, driven from bridge_tick(). */
static void long_tune_tick(void) {
  if (s_lr_state != AT_RUNNING) return;
  const uint32_t now = millis();

  if (!s_lr_measuring && now - s_lr_block_t0 >= LR_SETTLE_MS && now - s_lr_block_t0 < LR_BLOCK_MS) {
    s_lr_pkt0 = s_pkt_e2w; s_lr_drop0 = s_drop_e2w;
    s_lr_measuring = true;
  }

  /* A candidate far beyond any sensible loss leaves the rotation at once -
   * judged on the running block plus what it collected before. */
  if (s_lr_measuring) {
    const uint32_t p = s_lr_pkts[s_lr_cur] + (s_pkt_e2w - s_lr_pkt0);
    const uint32_t d = s_lr_drops[s_lr_cur] + (s_drop_e2w - s_lr_drop0);
    if (p + d >= LR_BAD_MIN_PKTS && 1000000ULL * d / (p + d) > LR_BAD_PPM) {
      lr_account();
      s_lr_bad[s_lr_cur] = true;
      printf("[LONG] %u retries lose > 1 %% - out of the rotation\n", (unsigned)s_lr_k[s_lr_cur]);
      s_lr_block_t0 = now - LR_BLOCK_MS;   /* switch below */
    }
  }

  if (now - s_lr_block_t0 < LR_BLOCK_MS) return;
  lr_account();
  if (now - s_lr_t_start >= s_lr_dur_ms) { lr_finish(); return; }

  uint8_t next = s_lr_cur;
  for (size_t step = 1; step <= s_lr_n; step++) {
    const uint8_t c = (uint8_t)((s_lr_cur + step) % s_lr_n);
    if (!s_lr_bad[c]) { next = c; break; }
  }
  if (s_lr_bad[next]) {         /* every candidate failed */
    s_eth_tx_retries = s_lr_prev;
    s_lr_state = AT_TOO_LITTLE_TRAFFIC;
    printf("[LONG] Every candidate loses > 1 %% - staying at %u\n", (unsigned)s_lr_prev);
    return;
  }
  lr_start_block(next);
}

/* ===========================================================================
 * "Calibrate all": the three autotunes in sequence
 * ========================================================================= */

/*
 * Runs the retry, TX power and queue cap calibrations one after another, so
 * each one measures with the results of the previous ones applied: the TX
 * power with the chosen retries, the cap with both. The order follows the
 * size of the effect - retries decide most of the loss, the cap is only
 * judged against what the others leave over.
 *
 * Optionally the long-run check of the retries follows, because the
 * 15-second retry step cannot separate values below 0.10 % loss.
 *
 * The chain stops when a stage ends without a result (too little traffic)
 * or is aborted by a manual save (its state drops back to idle), so it
 * never keeps measuring on a basis the user has just overridden.
 */
static all_stage_t s_all_stage = ALL_IDLE;
static uint8_t     s_all_long_h = 0;   /* 0 = no long-run stage */

/* See bridge.h. */
bool bridge_tune_all_start(uint8_t long_hours) {
  if (s_all_stage >= ALL_RETRIES && s_all_stage <= ALL_LONG) return false;
  if (!bridge_autotune_start()) return false;
  s_all_stage  = ALL_RETRIES;
  s_all_long_h = long_hours;
  printf("[ALL ] Calibrating retries -> TX power -> queue cap%s\n",
         long_hours ? " -> long-run retry check" : "");
  return true;
}

all_stage_t bridge_tune_all_stage(void) { return s_all_stage; }

/* See bridge.h. Re-applying the saved configuration aborts every running
 * calibration and restores the values they were varying. */
void bridge_tune_stop(void) {
  if (s_all_stage >= ALL_RETRIES && s_all_stage <= ALL_LONG) s_all_stage = ALL_STOPPED;
  bridge_apply_live_tuning();
}

/* Advance the chain once the running stage has finished. */
static void tune_all_tick(void) {
  if (s_all_stage < ALL_RETRIES || s_all_stage > ALL_LONG) return;
  const autotune_state_t st = s_all_stage == ALL_RETRIES ? s_at_state
                            : s_all_stage == ALL_TXP     ? s_tp_state
                            : s_all_stage == ALL_CAP     ? s_qc_state
                                                         : s_lr_state;
  if (st == AT_RUNNING) return;
  if (st != AT_DONE) {
    printf("[ALL ] Stopped at stage %u (%s)\n", (unsigned)s_all_stage,
           st == AT_TOO_LITTLE_TRAFFIC ? "too little traffic" : "aborted");
    s_all_stage = ALL_STOPPED;
    return;
  }
  if (s_all_stage == ALL_RETRIES && bridge_txp_tune_start()) { s_all_stage = ALL_TXP; return; }
  if (s_all_stage == ALL_TXP     && bridge_cap_tune_start()) { s_all_stage = ALL_CAP; return; }
  if (s_all_stage == ALL_CAP && s_all_long_h) {
    if (bridge_long_tune_start(s_all_long_h)) { s_all_stage = ALL_LONG; return; }
    s_all_stage = ALL_STOPPED;
    return;
  }
  if (s_all_stage == ALL_CAP || s_all_stage == ALL_LONG) {
    s_all_stage = ALL_DONE;
    printf("[ALL ] Done\n");
    return;
  }
  s_all_stage = ALL_STOPPED;     /* next stage refused to start */
}

/* ===========================================================================
 * Watchdog
 *
 * Detects a bridge that the driver still reports as connected (wifi_up =
 * true, normal RSSI) but that hardly delivers any packets anymore - a state
 * that can follow heavy RF interference and persist until a reboot. Neither
 * RSSI nor wifi_up reveal it; the signals that do are the loss ratio in the
 * LAN->Wi-Fi path under load and frequent reassociations. Both are evaluated
 * per time window, and several bad windows in a row trigger an escalation.
 *
 * When there is too little real traffic to judge (e.g. the camera is not
 * streaming), the bridge would otherwise be blind to exactly that state. So
 * in that case it actively probes - a ping to the Wi-Fi gateway via the
 * management netif (gateway_reachable(), otherwise only used at boot for the
 * location detection). Deliberately NOT the client itself: the bridge carries
 * the client's cloned MAC on the STA interface, so an ICMP from the
 * management stack to the client IP would resolve via ARP to our OWN MAC - a
 * round trip through ourselves that tests nothing. Reaching the gateway, by
 * contrast, goes through the same lwIP/netif/radio path the management HTTP
 * uses - a useful proxy, without touching the fragile raw-frame path in
 * eth_rx_cb/wifi_rx_cb.
 * ========================================================================= */

#define WD_WINDOW_MS           20000u  /* measurement window - long enough not
                                        * to react to a brief fluctuation     */
#define WD_MIN_PACKETS            200u  /* below this, the counters say nothing */
#define WD_LOSS_PERMILLE         150u  /* 15 % loss in the window = "bad"     */
#define WD_DISC_PER_WINDOW          4  /* this many reconnects in a window = "bad" */
#define WD_ETH_STALL_PACKETS        5  /* below this (LAN->Wi-Fi, raw, not only
                                        * dropped) with the link up, the client
                                        * counts as silent - see below.       */
/* How many bad windows in a row until the next escalation level:
 * g_cfg.wd_bad_windows, 0 = WD_BAD_WINDOWS_DEF (bridge.h). */
#define WD_PROBE_BUDGET_MS      1200u  /* upper limit for the gateway probe -
                                        * briefly blocks bridge_tick()        */

/*
 * Three-level escalation instead of an immediate esp_restart().
 *
 * A full reboot briefly takes down the Ethernet MAC/PHY - that takes the
 * attached camera offline for the duration of the boot, and depending on its
 * firmware it may need minutes to recover. More importantly: a reboot does
 * not fix a bad radio environment - if the interference is still there, the
 * new connect attempt after the reboot fails just like a simple reconnect.
 * A reboot is only the right response when the Wi-Fi driver itself is hung.
 *
 * Which escalation comes first depends on the SYMPTOM:
 *
 * - WD_REASON_ETH_STALL (link up, but practically no traffic from the client
 *   - see the detection below): first only the Ethernet driver is restarted
 *   (bridge_reset_eth()), Wi-Fi completely untouched. A pure Wi-Fi problem
 *   must NOT take this path - an Ethernet reset would not help and would
 *   disturb the client for no reason.
 * - Any other reason (loss ratio, reconnects, gateway probe - all Wi-Fi-side
 *   symptoms): disconnect Wi-Fi directly. wifi_evt() reconnects on
 *   WIFI_EVENT_STA_DISCONNECTED automatically while s_active is set - no
 *   extra esp_wifi_connect() needed.
 *
 * If the first step does not help (the symptom persists), the Wi-Fi
 * reconnect ALWAYS comes next (unless already tried). If that does not help
 * either: last resort, a full reboot - but via abort() instead of
 * esp_restart(), so a coredump is written: the next boot should be traceable
 * (the backtrace points exactly at this code, and the reason is additionally
 * kept in bridge_get_stats()->wd_last_reason, which survives the reboot in
 * RTC memory). Deliberately, no level counts as a failed boot - the Ethernet
 * reset and reconnect do not reboot at all, and even the final reboot is a
 * clean boot, not a failed one.
 */
typedef enum {
  WD_REASON_NONE        = 0,
  WD_REASON_LOSS        = 1,   /* loss ratio above the threshold */
  WD_REASON_DISCONNECTS = 2,   /* too many reconnects in the window */
  WD_REASON_PROBE       = 3,   /* gateway probe failed */
  WD_REASON_ETH_STALL   = 4,   /* link up, but no traffic from the client */
} wd_reason_t;

/* Short code for a watchdog reason, used in logs and MQTT events. */
static const char *wd_reason_str(uint8_t r) {
  switch (r) {
    case WD_REASON_LOSS:        return "loss";
    case WD_REASON_DISCONNECTS: return "disconnects";
    case WD_REASON_PROBE:       return "probe";
    case WD_REASON_ETH_STALL:   return "eth_stall";
    default:                    return "";
  }
}

/* Last action taken in the current escalation. The values are also the
 * escalation order - wd_next_action() looks for the next ALLOWED level above
 * the current one. */
typedef enum {
  WD_STAGE_NORMAL         = 0,
  WD_STAGE_ETH_RESET      = 1,   /* Ethernet reset already tried */
  WD_STAGE_WIFI_RECONNECT = 2,   /* Wi-Fi reconnect already tried */
  WD_STAGE_REBOOT         = 3,   /* only a target, never "reached" */
} wd_stage_t;

/* Next action for this symptom, starting from the last one taken. The
 * Ethernet reset is only an option for WD_REASON_ETH_STALL (it would not help
 * against a Wi-Fi symptom, see above). If no allowed levels are left above -
 * because the escalation was deliberately limited, e.g. "Ethernet reset
 * only" - it starts over from the bottom: the highest allowed action is
 * repeated rather than giving up.
 * WD_STAGE_NORMAL = nothing at all is allowed for this symptom. */
static uint8_t wd_next_action(uint8_t stage, uint8_t reason) {
  const uint8_t m = wd_actions_eff();
  for (int pass = 0; pass < 2; pass++) {
    for (uint8_t s = stage + 1; s <= WD_STAGE_REBOOT; s++) {
      if (s == WD_STAGE_ETH_RESET && (reason != WD_REASON_ETH_STALL || !(m & WD_ACT_ETH_RESET))) continue;
      if (s == WD_STAGE_WIFI_RECONNECT && !(m & WD_ACT_WIFI_RECON)) continue;
      if (s == WD_STAGE_REBOOT && !(m & WD_ACT_REBOOT)) continue;
      return s;
    }
    stage = WD_STAGE_NORMAL;
  }
  return WD_STAGE_NORMAL;
}

/* Failed-boot exception: one-shot, cleared when read - see
 * bridge_consume_planned_restart(). Same pattern as main.cpp's PORTAL_MAGIC. */
#define WD_RESTART_MAGIC 0x57445254u  /* "WDRT" */
RTC_NOINIT_ATTR static uint32_t s_wd_restart_magic;

/* Display reason: NOT one-shot, stays available for /api/status and MQTT
 * until the next watchdog reboot overwrites it. Separate magic word, because
 * RTC memory is undefined after a real power-on. */
#define WD_REASON_MAGIC  0x57445253u  /* "WDRS" */
RTC_NOINIT_ATTR static uint32_t s_wd_reason_magic;
RTC_NOINIT_ATTR static uint8_t  s_wd_reason;

/* Mark the upcoming reboot as a deliberate watchdog restart with `reason`. */
void bridge_mark_planned_restart(uint8_t reason) {
  s_wd_restart_magic = WD_RESTART_MAGIC;
  s_wd_reason_magic  = WD_REASON_MAGIC;
  s_wd_reason        = reason;
}

/* See bridge.h. */
bool bridge_consume_planned_restart(void) {
  const bool planned = (s_wd_restart_magic == WD_RESTART_MAGIC);
  s_wd_restart_magic = 0;
  return planned;
}

static uint32_t s_wd_t0 = 0, s_wd_pkt0 = 0, s_wd_drop0 = 0, s_wd_disc0 = 0;
static uint8_t  s_wd_bad = 0;
static uint8_t  s_wd_probe = 0;   /* 0=not needed/off, 1=ok, 2=failed -
                                   * for /api/status, see BridgeStats.wd_probe */
static uint8_t  s_wd_stage = WD_STAGE_NORMAL;
static uint32_t s_wd_reconnects = 0;   /* since boot, for /api/status/MQTT */
static uint32_t s_wd_eth_resets = 0;   /* since boot, for /api/status/MQTT */

/* Restart the Ethernet driver without rebooting the chip (see bridge.h). No
 * netif is involved (the data path is raw, see the file header), so none of
 * the pitfalls of creating a netif twice apply (see portal_start_ap() in
 * web.cpp). The input callback and promiscuous mode may not reliably survive
 * a stop/start, so both are set again, exactly as in bridge_activate(). */
bool bridge_reset_eth(void) {
  if (!s_eth) return false;   /* not initialized yet - e.g. provisioning */

  printf("[ETH ] Driver reset requested\n");
  esp_eth_stop(s_eth);
  delay(500);   /* the link partner should really see the down/up, not a
                 * blip so short that its PHY filters it out */
  esp_eth_start(s_eth);

  esp_eth_update_input_path(s_eth, eth_rx_cb, NULL);
  eth_set_bridge_filters();
  return true;
}

/* Read the gateway currently configured on the management netif - asked
 * directly from the netif, which is the authoritative source after
 * mgmt_apply_ip(), instead of re-deriving it from g_cfg and the profile
 * choice. Returns false if there is no netif or no gateway. */
static bool wd_get_gateway(uint8_t gw[4]) {
  if (!s_mgmt_netif) return false;
  esp_netif_ip_info_t ip = {};
  if (esp_netif_get_ip_info(s_mgmt_netif, &ip) != ESP_OK) return false;
  memcpy(gw, &ip.gw.addr, 4);
  return gw[0] || gw[1] || gw[2] || gw[3];
}

/* Watchdog state machine; called from bridge_tick().
 *
 * At the end of every WD_WINDOW_MS window it classifies the window as good or
 * bad (loss ratio with enough traffic; otherwise a gateway probe, plus the
 * client-silence check; reconnect churn always), counts consecutive bad
 * windows and, once wd_bad_windows is reached, takes the next allowed action
 * from wd_next_action(). A good window ends any running escalation. */
static void watchdog_tick(void) {
  /* No evaluation during OTA (s_paused) or outside bridge operation - a
   * reboot in the middle of a firmware flash would be fatal, and without an
   * active data path the counters are meaningless anyway. */
  if (!g_cfg.wd_enable || !s_active || s_paused) {
    s_wd_bad = 0; s_wd_t0 = 0; s_wd_probe = 0; s_wd_stage = 0;
    return;
  }

  const uint32_t now = millis();
  if (s_wd_t0 == 0) {
    s_wd_t0 = now; s_wd_pkt0 = s_pkt_e2w; s_wd_drop0 = s_drop_e2w;
    s_wd_disc0 = s_wifi_disc_count;
    return;
  }
  if (now - s_wd_t0 < WD_WINDOW_MS) return;

  const uint32_t dp    = s_pkt_e2w - s_wd_pkt0;
  const uint32_t dd    = s_drop_e2w - s_wd_drop0;
  const uint32_t ddisc = s_wifi_disc_count - s_wd_disc0;
  s_wd_t0 = now; s_wd_pkt0 = s_pkt_e2w; s_wd_drop0 = s_drop_e2w;
  s_wd_disc0 = s_wifi_disc_count;

  bool bad = false;
  uint8_t reason = WD_REASON_NONE;

  if (dp + dd >= WD_MIN_PACKETS) {
    /* Enough real traffic - the loss ratio decides, no probe needed (saves
     * airtime during a running stream). */
    s_wd_probe = 0;
    const uint32_t permille = (1000ULL * dd) / (dp + dd);
    if (permille >= WD_LOSS_PERMILLE) { bad = true; reason = WD_REASON_LOSS; }
  } else {
    uint8_t gw[4];
    if (!wd_get_gateway(gw)) {
      s_wd_probe = 0;   /* no gateway known - nothing can be concluded */
    } else if (gateway_reachable(gw, WD_PROBE_BUDGET_MS)) {
      s_wd_probe = 1;
      /* Wi-Fi demonstrably fine - but that does not answer whether the
       * client itself still delivers anything. The intended client (a camera)
       * streams continuously, so real silence with the link up is abnormal
       * and a symptom of its own, not a Wi-Fi problem - hence a separate
       * reason. */
      if (s_eth_link && (dp + dd) < WD_ETH_STALL_PACKETS) {
        bad = true; reason = WD_REASON_ETH_STALL;
      }
    } else {
      s_wd_probe = 2;
      bad = true; reason = WD_REASON_PROBE;
    }
  }
  /* Reconnects take precedence - if several apply, frequent reassociation is
   * the more telling symptom. */
  if (ddisc >= WD_DISC_PER_WINDOW) { bad = true; reason = WD_REASON_DISCONNECTS; }

  const uint8_t bad_max = wd_bad_windows_eff();
  s_wd_bad = bad ? (uint8_t)(s_wd_bad + 1) : 0;
  printf("[WD  ] Stage %u, window: %u packets, %u lost, %u reconnects, probe=%u, reason=%s -> %s (%u/%u)\n",
         (unsigned)s_wd_stage, (unsigned)(dp + dd), (unsigned)dd, (unsigned)ddisc,
         (unsigned)s_wd_probe, wd_reason_str(reason), bad ? "bad" : "ok",
         (unsigned)s_wd_bad, (unsigned)bad_max);

  if (!bad) {
    /* Recovered - any running escalation is finished. */
    s_wd_stage = WD_STAGE_NORMAL;
    return;
  }
  if (s_wd_bad < bad_max) return;

  /* The next window starts fresh after every escalation step - the action
   * itself takes a few seconds, which should not be held against the window
   * that is currently running. */
  s_wd_bad = 0;
  s_wd_t0  = 0;

  const uint8_t next = wd_next_action(s_wd_stage, reason);

  if (next == WD_STAGE_NORMAL) {
    /* No action is enabled in the portal for this symptom (e.g. a Wi-Fi
     * problem with "Ethernet reset only") - just log it. */
    printf("[WD  ] Reason %s - no action enabled (mask 0x%02x), observing only\n",
           wd_reason_str(reason), (unsigned)wd_actions_eff());
    return;
  }

  if (next == WD_STAGE_ETH_RESET) {
    /* Client-side symptom -> reset only Ethernet, Wi-Fi stays untouched. */
    printf("[WD  ] No traffic from the client despite link - resetting Ethernet\n");
    bridge_reset_eth();
    s_wd_eth_resets++;
    telemetry_note_watchdog_event("eth_reset", wd_reason_str(reason));
    s_wd_stage = WD_STAGE_ETH_RESET;
    return;
  }

  if (next == WD_STAGE_WIFI_RECONNECT) {
    /* Either a Wi-Fi-side symptom (straight here), or an Ethernet reset in
     * the previous round did not help - either way, disconnect Wi-Fi now.
     * wifi_evt() reconnects automatically on WIFI_EVENT_STA_DISCONNECTED
     * (s_active is always set here); an extra esp_wifi_connect() would be
     * redundant. */
    printf("[WD  ] Disconnecting Wi-Fi to reconnect\n");
    disc_mark_own(DISC_SRC_WATCHDOG);
    esp_wifi_disconnect();
    s_wd_reconnects++;
    telemetry_note_watchdog_event("wifi_reconnect", wd_reason_str(reason));
    s_wd_stage = WD_STAGE_WIFI_RECONNECT;
    return;
  }

  /* The enabled lower levels did not help (or are disabled) - highest
   * escalation level. abort() instead of esp_restart(), so a coredump is
   * written: the next boot should be traceable, not just "it rebooted". */
  printf("[WD  ] No improvement - forcing reboot with coredump (reason %s)\n",
         wd_reason_str(reason));
  telemetry_note_watchdog_event("reboot", wd_reason_str(reason));
  bridge_mark_planned_restart(reason);
  delay(400);   /* let the log line and the MQTT publish go out */
  abort();
}

/* ===========================================================================
 * Active roaming
 * ---------------------------------------------------------------------------
 * WIFI_ALL_CHANNEL_SCAN + WIFI_CONNECT_AP_BY_SIGNAL (see bridge_wifi_start())
 * pick the strongest AP only ONCE, at connect time. Once connected, the STA
 * stays on that AP even if a much better one shows up later - ESP-IDF does
 * not roam on its own, and the watchdog does not help reliably here: a weak
 * but "stable" link can stay below its loss threshold although a far better
 * AP (e.g. another mesh node) would be available.
 *
 * Deliberately conservative: only check when the CURRENT signal is already
 * weak (no constant speculative scanning - that costs airtime and briefly
 * disturbs the data path), and only switch when a candidate has a clear lead
 * (a margin against ping-pong between two similarly good APs).
 * ========================================================================= */

static uint32_t s_roam_t0    = 0;
static uint32_t s_roam_count = 0;

/* Best candidate among the OTHER APs with the given SSID under the band
 * setting (pick_best_ap()) - the currently connected one (cur_bssid) does not
 * count. Otherwise a mere measurement difference between the averaged RSSI
 * from esp_wifi_sta_get_ap_info() and the single value from the scan could
 * "beat" the margin, and the bridge would disconnect only to reconnect to
 * the same AP. Blocking scan. Returns false if the scan failed or no other
 * allowed AP was visible. */
static bool roam_scan_best(const uint8_t *ssid, const uint8_t *cur_bssid,
                           wifi_ap_record_t *out) {
  wifi_scan_config_t sc = {};
  sc.ssid        = (uint8_t *)ssid;
  sc.show_hidden = false;
  sc.scan_time.passive = SCAN_PASSIVE_MS;
  if (esp_wifi_scan_start(&sc, true) != ESP_OK) return false;

  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  if (n == 0) return false;
  if (n > 20) n = 20;

  wifi_ap_record_t *recs = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * n);
  if (!recs) { esp_wifi_clear_ap_list(); return false; }
  esp_wifi_scan_get_ap_records(&n, recs);
  esp_wifi_clear_ap_list();

  const bool found = pick_best_ap((const char *)ssid, recs, n, cur_bssid, out);
  free(recs);
  return found;
}

/* Roaming check; called from bridge_tick(). Every roam_check_s seconds it
 * may scan for other APs of the same SSID and switch to one of them:
 *  - roaming (roam_enable): when the current RSSI is below the threshold and
 *    a candidate is better by at least the margin;
 *  - band return (5 GHz preferred, regardless of roam_enable): when the
 *    bridge sits on 2.4 GHz and a usable 5 GHz AP (BAND_5G_MIN_RSSI) is in
 *    range - e.g. because 5 GHz was not available when it connected.
 * The switch binds the STA to the chosen AP (sta_lock_ap()) and disconnects;
 * wifi_evt()'s reconnect then goes straight to that AP. */
static void roam_tick(void) {
  const bool band_return = band_setting() == WIFI_BAND_PREFER_5G;
  if ((!g_cfg.roam_enable && !band_return) || !s_active || s_paused) { s_roam_t0 = 0; return; }

  const uint32_t interval_ms =
      (g_cfg.roam_check_s ? g_cfg.roam_check_s : ROAM_CHECK_S_DEF) * 1000UL;
  const uint32_t now = millis();
  if (s_roam_t0 == 0) { s_roam_t0 = now; return; }
  if (now - s_roam_t0 < interval_ms) return;
  s_roam_t0 = now;

  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;   /* not connected right now */

  const bool want_5g = band_return && !ap_is_5g(&ap);
  const int8_t threshold = g_cfg.roam_rssi_threshold
      ? g_cfg.roam_rssi_threshold : ROAM_RSSI_THRESHOLD_DEF;
  const bool weak = g_cfg.roam_enable && ap.rssi < threshold;
  if (!want_5g && !weak) return;   /* nothing to look for - don't even scan */

  wifi_ap_record_t cand;
  if (!roam_scan_best(ap.ssid, ap.bssid, &cand)) return;

  const uint8_t margin = g_cfg.roam_margin_db ? g_cfg.roam_margin_db : ROAM_MARGIN_DB_DEF;
  if (want_5g && ap_is_5g(&cand) && cand.rssi >= BAND_5G_MIN_RSSI) {
    printf("[ROAM] On 2.4 GHz (%d dBm), 5 GHz AP with %d dBm on channel %u - switching\n",
           (int)ap.rssi, (int)cand.rssi, (unsigned)cand.primary);
  } else if (weak && (int)cand.rssi - (int)ap.rssi >= (int)margin) {
    printf("[ROAM] Current %d dBm, found candidate with %d dBm - switching AP\n",
           (int)ap.rssi, (int)cand.rssi);
  } else {
    return;   /* no sufficient candidate */
  }
  s_roam_count++;
  sta_lock_ap(s_cur_net, &cand, true);
  disc_mark_own(DISC_SRC_ROAM);
  esp_wifi_disconnect();
}

/* ===========================================================================
 * Network failover / failback
 * ---------------------------------------------------------------------------
 * The boot sequence tries the configured networks in turn, but once running,
 * wifi_evt() only ever reconnects to the SSID it is configured for. If that
 * network disappears (AP off, SSID disabled), the bridge would retry it
 * forever while network 2 is right there.
 *
 * Failover: after the same time a boot spends on one network (connect
 * timeout x attempts) without a connection, switch to the next configured
 * network, round-robin. wifi_evt()'s reconnect loop then simply continues
 * with the new SSID.
 *
 * Failback: while on a lower-priority network, check every few minutes with
 * a targeted scan whether a higher-priority one is visible again (>= -80 dBm)
 * and switch back - otherwise the bridge would stay on the fallback until
 * the next reboot. A failback that does not connect ends in a failover back
 * to the working network and doubles the check interval (up to 1 h), so a
 * network that is visible but broken does not cause an outage every 5 min.
 *
 * After every switch the location detection runs again, so the management
 * IP follows the profile of the network actually joined.
 * ========================================================================= */

#define FAILBACK_MS_MIN    300000u   /* 5 min  */
#define FAILBACK_MS_MAX   3600000u   /* 1 h    */
#define FAILBACK_MIN_RSSI     (-80)

static uint32_t s_fo_down_t0  = 0;      /* when the link went down, 0 = up   */
static bool     s_fo_locate   = false;  /* re-run location detection once up */
static uint32_t s_fo_count    = 0;      /* network switches since boot       */
static uint32_t s_fb_t0       = 0;
static uint32_t s_fb_interval = FAILBACK_MS_MIN;
static bool     s_fb_trying   = false;  /* a failback switch is under way    */
static uint8_t  s_fb_target   = 0;

/* Switch the STA to network slot i. */
static void fo_switch(size_t i, const char *why) {
  printf("[FAIL] %s - switching from '%s' to '%s'\n", why, net_ssid(s_cur_net), net_ssid(i));
  s_fo_count++;
  disc_mark_own(DISC_SRC_FAILOVER);
  esp_wifi_disconnect();
  sta_set_net(i);
  s_fo_locate = true;
  esp_wifi_connect();
}

/* Is network slot i visible with at least FAILBACK_MIN_RSSI? Blocking,
 * targeted scan (only that SSID), so it costs about as much airtime as a
 * roaming check. */
static bool net_visible(size_t i) {
  wifi_scan_config_t sc = {};
  sc.ssid = (uint8_t *)net_ssid(i);
  if (esp_wifi_scan_start(&sc, true) != ESP_OK) return false;
  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  if (n > 8) n = 8;
  wifi_ap_record_t recs[8];
  if (n) esp_wifi_scan_get_ap_records(&n, recs);
  esp_wifi_clear_ap_list();
  for (uint16_t k = 0; k < n; k++) if (recs[k].rssi >= FAILBACK_MIN_RSSI) return true;
  return false;
}

/* Failover/failback state machine; called from bridge_tick() (main task, so
 * the blocking scan and the gateway pings of detect_location() never run in
 * the event handler). */
static void failover_tick(void) {
  if (!s_active || s_paused || s_restarting) return;
  const uint32_t now = millis();

  if (!s_wifi_up) {
    if (!s_fo_down_t0) { s_fo_down_t0 = now ? now : 1; return; }
    const uint32_t wait = (uint32_t)s_eff_wifi_connect_timeout_s * 1000u * s_eff_wifi_connect_retries;
    if (now - s_fo_down_t0 < wait) return;
    s_fo_down_t0 = now;                       /* next network gets the same time */
    size_t next = s_cur_net;
    for (size_t k = 1; k <= 3; k++) {
      const size_t c = (s_cur_net + k) % 3;
      if (net_in_use(c)) { next = c; break; }
    }
    if (next == s_cur_net) return;            /* only one network configured */
    if (s_fb_trying) {                        /* the failback did not work out */
      s_fb_trying = false;
      s_fb_interval = s_fb_interval * 2 > FAILBACK_MS_MAX ? FAILBACK_MS_MAX : s_fb_interval * 2;
      printf("[FAIL] Failback to '%s' failed - next check in %lu min\n",
             net_ssid(s_fb_target), (unsigned long)(s_fb_interval / 60000));
    }
    fo_switch(next, "No connection");
    return;
  }

  s_fo_down_t0 = 0;
  if (s_fo_locate) {
    s_fo_locate = false;
#ifndef BRIDGE_MINIMAL
    detect_location(s_cur_net);
#endif
  }
  if (s_fb_trying && s_cur_net == s_fb_target) {   /* failback succeeded */
    s_fb_trying = false;
    s_fb_interval = FAILBACK_MS_MIN;
  }

  /* Failback: only while on a lower-priority network. */
  size_t pref = 0;
  while (pref < 3 && !net_in_use(pref)) pref++;
  if (pref >= 3 || s_cur_net <= pref) { s_fb_t0 = 0; return; }
  if (!s_fb_t0) { s_fb_t0 = now; return; }
  if (now - s_fb_t0 < s_fb_interval) return;
  s_fb_t0 = now;
  for (size_t i = pref; i < s_cur_net; i++) {
    if (!net_in_use(i) || !net_visible(i)) continue;
    s_fb_trying = true;
    s_fb_target = (uint8_t)i;
    fo_switch(i, "Higher-priority network visible again");
    return;
  }
}

/* See bridge.h. */
uint8_t  bridge_get_net(void)       { return s_cur_net; }
uint32_t bridge_get_failovers(void) { return s_fo_count; }

/* ===========================================================================
 * Statistics
 * ========================================================================= */

/* Periodic work, called from the main loop: run the autotune, watchdog and
 * roaming state machines, and once per second recompute the throughput in
 * kbit/s from the byte counters. */
void bridge_tick(void) {
  autotune_tick();
  txp_tune_tick();
  cap_tune_tick();
  long_tune_tick();
  tune_all_tick();
  watchdog_tick();
  roam_tick();
  last_ap_tick();
  failover_tick();
  cpustat_tick(s_by_e2w, s_by_w2e);

  static uint32_t last = 0;
  static uint64_t le = 0, lw = 0;

  const uint32_t now = millis();
  if (now - last < 1000) return;
  const uint32_t dt = now - last ? now - last : 1;
  last = now;

  const uint64_t e = s_by_e2w, w = s_by_w2e;
  s_kbps_e2w = (uint32_t)(((e - le) * 8ULL) / dt);
  s_kbps_w2e = (uint32_t)(((w - lw) * 8ULL) / dt);
  le = e; lw = w;

  /* TX queue over the last second: mean depth L = dSum / dDone, rate
   * lambda = dDone / dt, wait W = L / lambda = dSum * dt / dDone^2. */
  static uint32_t lq = 0, ld = 0, lp = 0;
  const uint32_t qs = s_q_sum, dn = s_air_done, pk = s_pkt_e2w;
  const uint32_t dsum = qs - lq, ddone = dn - ld;
  /* A whole second without a single submission or completion: nothing can
   * be waiting, so whatever depth is left is accounting drift (e.g. frames
   * the driver flushed on a disconnect without reporting them). */
  if (!ddone && pk == lp) {
    const int32_t depth = txq_depth(dn);
    if (depth > 0) s_q_idle = s_q_idle + (uint32_t)depth;
  }
  lq = qs; ld = dn; lp = pk;
  if (ddone) {
    s_q_avg_x10  = (uint32_t)((10ULL * dsum) / ddone);
    s_q_delay_ms = (uint32_t)(((uint64_t)dsum * dt) / ((uint64_t)ddone * ddone));
  } else {
    s_q_avg_x10 = 0; s_q_delay_ms = 0;
  }
}

/* See bridge.h. */
void bridge_get_stats(BridgeStats *o) {
  memset(o, 0, sizeof(*o));
  o->pkt_eth2wifi  = s_pkt_e2w;
  o->pkt_wifi2eth  = s_pkt_w2e;
  o->drop_eth2wifi = s_drop_e2w;
  o->drop_wifi2eth = s_drop_w2e;
  o->kbps_eth2wifi = s_kbps_e2w;
  o->kbps_wifi2eth = s_kbps_w2e;
  o->wifi_disc_count = s_wifi_disc_count;
  o->wd_probe      = s_wd_probe;
  o->wd_reconnects = s_wd_reconnects;
  o->wd_eth_resets = s_wd_eth_resets;
  o->wd_last_reason = (s_wd_reason_magic == WD_REASON_MAGIC) ? s_wd_reason : 0;
  o->roam_count     = s_roam_count;

  /* Most recent REAL drop of this boot - not the most recent entry, which
   * after a kick is usually just the last failed attempt (e.g. NO_AP_FOUND)
   * and hides the cause. Search the buffer directly instead of copying 48
   * entries onto the stack. */
  portENTER_CRITICAL(&s_dl_mux);
  dl_ensure_locked();
  for (size_t i = 0; i < s_dl_used; i++) {
    const bridge_disc_t *e = &s_dl[(s_dl_head + DISC_LOG_N - 1 - i) % DISC_LOG_N];
    if (e->boot != s_dl_boot) break;
    if (!e->was_up) continue;
    o->disc_last_reason = e->reason;
    o->disc_last_src    = e->src;
    o->disc_last_ago_s  = millis() / 1000 - e->t_s;
    break;
  }
  o->wifi_kicks = s_kicks;
  portEXIT_CRITICAL(&s_dl_mux);

  o->air_ok   = s_air_ok;
  o->air_fail = s_air_fail;
  o->txq_x10      = s_q_avg_x10;
  o->txq_delay_ms = s_q_delay_ms;
  const int32_t depth = txq_depth(s_air_done);
  o->txq_now = depth > 0 ? (uint32_t)depth : 0;
  o->drop_cap = s_drop_cap;
  o->txq_cap  = s_txq_cap;
  wifi_country_t cc;
  if (esp_wifi_get_country(&cc) == ESP_OK) {
    o->country[0] = cc.cc[0] >= ' ' ? cc.cc[0] : '?';
    o->country[1] = cc.cc[1] >= ' ' ? cc.cc[1] : '?';
    o->country[2] = '\0';
    o->country_max_txp = cc.max_tx_power;
  }

  o->eth_link      = s_eth_link;
  o->wifi_up       = s_wifi_up;
  strcpy(o->bssid, s_bssid_str);

  if (s_mac_known) mac2str(s_client_mac, o->client_mac);
  else             strcpy(o->client_mac, "-");

  strncpy(o->client_name, s_client_name[0] ? s_client_name : "-",
          sizeof(o->client_name) - 1);
  o->client_name[sizeof(o->client_name) - 1] = '\0';

  if (s_client_ip[0] | s_client_ip[1] | s_client_ip[2] | s_client_ip[3])
    sprintf(o->client_ip, "%u.%u.%u.%u",
            s_client_ip[0], s_client_ip[1], s_client_ip[2], s_client_ip[3]);
  else
    strcpy(o->client_ip, "-");

  wifi_ap_record_t ap;
  o->ssid[0] = '\0';
  if (s_wifi_up && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    o->rssi    = ap.rssi;
    o->channel = ap.primary;
    /* With several configured networks, the BSSID alone does not tell
     * anyone which network the bridge is actually on. */
    strncpy(o->ssid, (const char *)ap.ssid, sizeof(o->ssid) - 1);
    o->ssid[sizeof(o->ssid) - 1] = '\0';
  }
}
