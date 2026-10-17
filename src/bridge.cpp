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
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"

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

/* --- WT32-ETH01 pin assignment -------------------------------------------- */
#define ETH_PHY_ADDR        1
#define ETH_PHY_MDC_GPIO    23
#define ETH_PHY_MDIO_GPIO   18
#define ETH_PHY_PWR_GPIO    16

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
  s_disc_src_ms = millis();
  s_disc_src    = src;
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
  uint8_t src = DISC_SRC_EXTERN;
  if (s_disc_src != DISC_SRC_EXTERN && millis() - s_disc_src_ms < 3000) src = s_disc_src;
  s_disc_src = DISC_SRC_EXTERN;

  const uint8_t reason = d ? d->reason : 0;
  const uint32_t epoch = crashlog_now();
  portENTER_CRITICAL(&s_dl_mux);
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
static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg; (void)base;

  if (id == WIFI_EVENT_STA_CONNECTED) {
    s_wifi_up = true;
    wifi_event_sta_connected_t *e = (wifi_event_sta_connected_t *)data;
    if (e) mac2str(e->bssid, s_bssid_str);
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
    if (s_active) esp_wifi_connect();     /* in operation, keep reconnecting */
  }
}

/* ===========================================================================
 * Ethernet
 * ========================================================================= */

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

/* Start Ethernet with the sniff callback and poll until the client MAC is
 * known or the timeout expires. On success, detach the sniff callback and
 * enable promiscuous mode, so frames for the client's MAC (which the EMAC
 * would otherwise filter) are received. */
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
  bool promisc = true;
  esp_eth_ioctl(s_eth, ETH_CMD_S_PROMISCUOUS, &promisc);
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
  /* Remember for display what actually went into the driver. */
  s_eff_static_rx = wcfg.static_rx_buf_num;
  s_eff_dyn_rx    = wcfg.dynamic_rx_buf_num;
  s_eff_dyn_tx    = wcfg.dynamic_tx_buf_num;
  s_eff_ba_win    = wcfg.rx_ba_win;

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
  }

  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));   /* throughput over power */

  /* 802.11b can be disabled: the b rates are slow and, as basic rates, waste
   * airtime. Only sensible if no pure 802.11b device is left on the network -
   * otherwise the AP might not see us anymore. */
  esp_wifi_set_protocol(WIFI_IF_STA,
      g_cfg.no_11b ? (WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N)
                   : (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));

  /* HT40 doubles the channel width and thus the gross data rate, but only
   * takes effect if the AP actually offers 40 MHz on 2.4 GHz - many don't,
   * for coexistence reasons. Without AP support it silently stays at HT20,
   * which is not an error. */
  esp_err_t bw = esp_wifi_set_bandwidth(
      WIFI_IF_STA, g_cfg.ht40 ? WIFI_BW_HT40 : WIFI_BW_HT20);
  if (bw != ESP_OK) {
    printf("[WIFI] set_bandwidth: %s\n", esp_err_to_name(bw));
  }

  /* Register after the esp_netif handlers - the order matters. */
  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                      &wifi_evt, NULL, NULL);

  struct { const char *ssid; const char *pass; } nets[] = {
    { g_cfg.ssid1, g_cfg.pass1 },
    { g_cfg.ssid2, g_cfg.pass2 },
    { g_cfg.ssid3, g_cfg.pass3 },
  };

  /* Portal overrides, 0 = firmware default - same convention as the other
   * tuning values (config.h). Remembered for display. */
  const uint32_t connect_timeout_ms = g_cfg.wifi_connect_timeout_s
      ? (uint32_t)g_cfg.wifi_connect_timeout_s * 1000u : WIFI_CONNECT_TIMEOUT_MS_DEF;
  const int connect_retries = g_cfg.wifi_connect_retries
      ? g_cfg.wifi_connect_retries : WIFI_CONNECT_RETRIES_DEF;
  s_eff_wifi_connect_timeout_s = (uint16_t)(connect_timeout_ms / 1000);
  s_eff_wifi_connect_retries   = (uint8_t)connect_retries;

  for (size_t i = 0; i < 3; i++) {
    if (nets[i].ssid[0] == '\0') continue;
    printf("[WIFI] Connecting to '%s'...\n", nets[i].ssid);

    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid,     nets[i].ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, nets[i].pass, sizeof(wc.sta.password) - 1);
    wc.sta.pmf_cfg.capable = true;

    /* Take the STRONGEST access point of this SSID, not the first one found.
     *
     * The default is WIFI_FAST_SCAN: IDF connects to the first matching AP it
     * finds and stops searching. In a mesh with several nodes under the same
     * SSID that is a lottery - and an ESP32 does not switch on its own
     * afterwards. A weak node (e.g. -82 dBm instead of -63 dBm on another)
     * means heavy packet loss and a fraction of the throughput.
     *
     * ALL_CHANNEL_SCAN costs a few extra seconds at connect time because all
     * channels are really scanned. That happens once per boot. */
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    /* Order: set_config, THEN set_mac, THEN start. */
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, s_client_mac);
    if (err != ESP_OK) {
      printf("[WIFI] set_mac: %s\n", esp_err_to_name(err));
      return false;
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    /* TX power only AFTER esp_wifi_start() - before that the API returns
     * ESP_ERR_WIFI_NOT_STARTED. That is why the call sits inside the loop and
     * not further up with the other settings. */
    bridge_apply_live_tuning();

    for (int attempt = 0; attempt < connect_retries; attempt++) {
      if (attempt > 0) {
        printf("[WIFI] '%s' attempt %d/%d...\n", nets[i].ssid,
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
      /* Connected - but at WHICH site? The SSID slot does not tell when
       * several sites share the same SSID. So try the profiles in use and
       * ping their gateways, starting with the profile of the connected slot
       * because it is the most likely match. */
#ifndef BRIDGE_MINIMAL
      detect_location(i);
#endif
      return true;
    }

    printf("[WIFI] '%s' failed after %d attempts\n",
           nets[i].ssid, connect_retries);
    disc_mark_own(DISC_SRC_CONNECT);
    esp_wifi_disconnect();
    esp_wifi_stop();
  }
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
  bool promisc = true;
  esp_eth_ioctl(s_eth, ETH_CMD_S_PROMISCUOUS, &promisc);
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

  printf("[BRDG] Retries eth->wifi=%u wifi->eth=%u\n",
         (unsigned)s_eth_tx_retries, (unsigned)s_wifi_tx_retries);
}

int16_t bridge_get_tx_power(void) {
  int8_t p = 0;
  if (esp_wifi_get_max_tx_power(&p) != ESP_OK) return -1;
  return p;
}

uint8_t bridge_get_bandwidth_mhz(void) {
  wifi_bandwidth_t bw;
  if (esp_wifi_get_bandwidth(WIFI_IF_STA, &bw) != ESP_OK) return 0;
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
static uint16_t  s_at_loss[AT_N];
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
  if (!s_active || s_at_state == AT_RUNNING) return false;
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
  bool promisc = true;
  esp_eth_ioctl(s_eth, ETH_CMD_S_PROMISCUOUS, &promisc);
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

/* Best RSSI among the OTHER APs with the given SSID - the currently connected
 * one (cur_bssid) does not count as a candidate. Otherwise a mere measurement
 * difference between the averaged RSSI from esp_wifi_sta_get_ap_info() and
 * the single value from the scan could "beat" the margin, and the bridge
 * would disconnect only to reconnect to the same AP. Blocking scan.
 * Returns false if the scan failed or no other AP was visible. */
static bool roam_scan_best(const uint8_t *ssid, const uint8_t *cur_bssid,
                           int8_t *out_best) {
  wifi_scan_config_t sc = {};
  sc.ssid        = (uint8_t *)ssid;
  sc.show_hidden = false;
  if (esp_wifi_scan_start(&sc, true) != ESP_OK) return false;

  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  if (n == 0) return false;
  if (n > 20) n = 20;

  wifi_ap_record_t *recs = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * n);
  if (!recs) return false;
  esp_wifi_scan_get_ap_records(&n, recs);

  bool found = false;
  int8_t best = -127;
  for (uint16_t i = 0; i < n; i++) {
    if (memcmp(recs[i].bssid, cur_bssid, 6) == 0) continue;
    if (recs[i].rssi > best) best = recs[i].rssi;
    found = true;
  }
  free(recs);

  *out_best = best;
  return found;
}

/* Roaming check; called from bridge_tick(). Every roam_check_s seconds: if
 * the current RSSI is below the threshold, scan for other APs of the same
 * SSID and disconnect if one is better by at least the margin - the
 * reconnect in wifi_evt() then picks the strongest AP. */
static void roam_tick(void) {
  if (!g_cfg.roam_enable || !s_active || s_paused) { s_roam_t0 = 0; return; }

  const uint32_t interval_ms =
      (g_cfg.roam_check_s ? g_cfg.roam_check_s : ROAM_CHECK_S_DEF) * 1000UL;
  const uint32_t now = millis();
  if (s_roam_t0 == 0) { s_roam_t0 = now; return; }
  if (now - s_roam_t0 < interval_ms) return;
  s_roam_t0 = now;

  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;   /* not connected right now */

  const int8_t threshold = g_cfg.roam_rssi_threshold
      ? g_cfg.roam_rssi_threshold : ROAM_RSSI_THRESHOLD_DEF;
  if (ap.rssi >= threshold) return;   /* good enough already - don't even scan */

  int8_t best;
  if (!roam_scan_best(ap.ssid, ap.bssid, &best)) return;

  const uint8_t margin = g_cfg.roam_margin_db ? g_cfg.roam_margin_db : ROAM_MARGIN_DB_DEF;
  if ((int)best - (int)ap.rssi < (int)margin) return;   /* no sufficient candidate */

  printf("[ROAM] Current %d dBm, found candidate with %d dBm - switching AP\n",
         (int)ap.rssi, (int)best);
  s_roam_count++;
  /* wifi_evt()'s WIFI_EVENT_STA_DISCONNECTED handler reconnects automatically
   * (s_active is always set here) and, via WIFI_ALL_CHANNEL_SCAN +
   * WIFI_CONNECT_AP_BY_SIGNAL, again picks the strongest visible AP - no
   * extra esp_wifi_connect() needed. */
  disc_mark_own(DISC_SRC_ROAM);
  esp_wifi_disconnect();
}

/* ===========================================================================
 * Statistics
 * ========================================================================= */

/* Periodic work, called from the main loop: run the autotune, watchdog and
 * roaming state machines, and once per second recompute the throughput in
 * kbit/s from the byte counters. */
void bridge_tick(void) {
  autotune_tick();
  watchdog_tick();
  roam_tick();

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
