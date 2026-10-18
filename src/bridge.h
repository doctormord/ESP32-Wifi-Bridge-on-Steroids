/*
 * bridge.h - Ethernet/Wi-Fi data path and management netif.
 *
 * Public interface of bridge.cpp: bring-up of Ethernet and Wi-Fi, the raw
 * L2 forwarding path, runtime tuning, the retry autotune, the watchdog,
 * roaming, statistics and the Wi-Fi disconnect log.
 */
#pragma once

#include "compat.h"
#include "esp_eth.h"
#include "esp_netif.h"

/* Snapshot of counters and link state, filled by bridge_get_stats(). */
struct BridgeStats {
  uint32_t pkt_eth2wifi;
  uint32_t pkt_wifi2eth;
  uint32_t drop_eth2wifi;
  uint32_t drop_wifi2eth;
  uint32_t kbps_eth2wifi;     /* rolling, updated every 1 s */
  uint32_t kbps_wifi2eth;
  uint32_t wifi_disc_count;   /* WIFI_EVENT_STA_DISCONNECTED since boot, for
                               * the watchdog and for diagnosis in the portal */
  uint8_t  wd_probe;          /* result of the last watchdog gateway probe:
                               * 0=not needed/off, 1=ok, 2=failed             */
  uint32_t wd_eth_resets;     /* watchdog-triggered Ethernet resets since boot
                               * (escalation level "no client traffic")       */
  uint32_t wd_reconnects;     /* watchdog-triggered Wi-Fi reconnects since boot */
  uint8_t  wd_last_reason;    /* reason of the last forced watchdog reboot
                               * (highest escalation level): 0=none since
                               * power-on, 1=loss ratio, 2=reconnects,
                               * 3=gateway probe, 4=no client traffic.
                               * Survives the reboot (RTC memory) so the
                               * portal/MQTT can show WHY it happened.        */
  uint32_t roam_count;        /* active AP switches since boot (roam_tick())  */
  uint32_t wifi_kicks;        /* real drops (external, connection was up)
                               * since boot - excluding the failed reconnect
                               * attempts that follow                         */
  uint8_t  disc_last_reason;  /* reason of the last REAL drop in this boot,
                               * 0 = none (see bridge_get_disc_log())         */
  uint8_t  disc_last_src;     /* matching DISC_SRC_*                          */
  uint32_t disc_last_ago_s;   /* how long ago, in s                           */
  uint32_t air_ok;            /* frames the Wi-Fi MAC reported as delivered
                               * (acknowledged by the AP) since boot          */
  uint32_t air_fail;          /* frames the MAC gave up on after its own
                               * hardware retries - lost on the air, unlike
                               * drop_eth2wifi, which counts frames that never
                               * got a TX buffer                              */
  uint32_t txq_now;           /* bridged frames handed to the driver but not
                               * yet finished on the air, right now           */
  uint32_t txq_x10;           /* mean of that over the last second, x10       */
  uint32_t txq_delay_ms;      /* how long a frame waited in that queue on
                               * average over the last second (Little's law) */
  uint32_t drop_cap;          /* frames dropped by the TX queue cap since boot
                               * (a subset of drop_eth2wifi)                  */
  uint8_t  txq_cap;           /* cap in effect right now, 0 = off             */
  char     country[3];        /* regulatory country in effect ("DE", "01"...),
                               * adopted from the AP's beacon by default      */
  int8_t   country_max_txp;   /* its TX power cap as esp_wifi_get_country()
                               * reports it - an upper bound for tx_power     */
  int8_t   rssi;
  uint8_t  channel;
  bool     eth_link;
  bool     wifi_up;
  char     bssid[18];
  char     ssid[33];          /* SSID of the network actually connected to -
                               * with several configured networks it is
                               * otherwise impossible to tell which one is
                               * in use                                       */
  char     client_mac[18];
  char     client_ip[16];     /* snooped from the data path, "-" if unknown   */
  char     client_name[32];   /* host name from the DHCP request (option 12)  */
};

/* Initialize Ethernet (RMII MAC + LAN8720 PHY, no netif). */
bool bridge_eth_init(void);

/* Start Ethernet and wait for the first frame from the client to learn its
 * MAC. timeout_ms = 0 -> wait forever. Returns false on timeout. */
bool bridge_sniff_client_mac(uint32_t timeout_ms);

/* Bring up Wi-Fi in STA mode with the cloned client MAC, including the
 * management netif with its static IP. Tries all configured SSIDs in order.
 * Returns false if none of them could be connected. */
bool bridge_wifi_start(void);

/* Arm the data path (call only after a successful connect). */
void bridge_activate(void);

/* Must be called periodically from the main loop - drives the autotune,
 * watchdog and roaming state machines and computes the data rates. */
void bridge_tick(void);

/* Fill *out with a consistent snapshot of counters and link state. */
void bridge_get_stats(BridgeStats *out);

/* One-shot (clears itself when read, like main.cpp's PORTAL_MAGIC): true if
 * the CURRENT boot was caused by a deliberate watchdog restart. main.cpp calls
 * this at the very start of bridge_setup() so that such a restart does NOT
 * count as a failed boot - the device booted and ran fine, it only restarted
 * itself. Without this exception, several watchdog restarts during a longer
 * Wi-Fi outage would trip the boot-loop protection. */
bool bridge_consume_planned_restart(void);

/* Restart the Ethernet driver (stop/start) WITHOUT rebooting the chip -
 * produces a real short link down/up for the attached client while Wi-Fi
 * stays completely untouched. Can be triggered manually from the portal
 * (/api/eth_reset) and is used by the watchdog when the client seems hung.
 * Returns false and does nothing if the Ethernet driver is not initialized
 * yet (e.g. in provisioning mode without any detected client). */
bool bridge_reset_eth(void);

/* The management netif (NULL before bridge_wifi_start() or in the minimal
 * build). */
esp_netif_t *bridge_mgmt_netif(void);
/* True once bridge_activate() has armed the data path. */
bool         bridge_is_active(void);

/* The management IP that is ACTUALLY active. Can differ from g_cfg.mgmt_ip
 * when the location detection selected profile 2 or 3. Anything that prints
 * or reports the address must use this one, otherwise it names an address
 * under which nothing is reachable. */
void bridge_get_mgmt_ip(uint8_t out[4]);

/* Numeric tuning values, as reported to the portal.
 *
 * In the configuration, 0 means "use the firmware default" - in the portal
 * that would be confusing, because a value BELOW the default could not be
 * entered without the zero becoming ambiguous. The API therefore reports the
 * values that are ACTUALLY in effect, plus the defaults for the "Defaults"
 * button. */
typedef struct {
  uint8_t  static_rx, dyn_rx, dyn_tx, ba_win, eth_retries, wifi_retries;
  uint16_t wifi_connect_timeout_s;
  uint8_t  wifi_connect_retries;
  uint16_t ap_idle_reboot_s;
  uint16_t roam_check_s;
  int8_t   roam_rssi_threshold;
  uint8_t  roam_margin_db;
  uint8_t  wd_actions;
  uint8_t  wd_bad_windows;
} bridge_tuning_t;

/* Allowed watchdog actions (BridgeConfig.wd_actions). The escalation order is
 * fixed (Ethernet -> Wi-Fi -> reboot); the mask only selects which levels may
 * be used at all. */
#define WD_ACT_ETH_RESET   0x01u
#define WD_ACT_WIFI_RECON  0x02u
#define WD_ACT_REBOOT      0x04u
#define WD_ACT_ALL         (WD_ACT_ETH_RESET | WD_ACT_WIFI_RECON | WD_ACT_REBOOT)
#define WD_BAD_WINDOWS_DEF 3u

/* Firmware default for ap_idle_reboot_s (config.h) - defined here rather than
 * in main.cpp because both main.cpp (timer logic) and web.cpp (display via
 * bridge_get_defaults()) need it. */
#define AP_IDLE_REBOOT_S_DEF 900u   /* 15 minutes */

void bridge_get_defaults(bridge_tuning_t *out);    /* firmware defaults      */
void bridge_get_effective(bridge_tuning_t *out);   /* what is running now    */

/* Read the learned client MAC, or set it from outside. Setting it is the
 * fallback when the sniff found nothing but a MAC is stored - see config.h.
 * Must happen BEFORE bridge_wifi_start(), because the MAC is cloned onto the
 * Wi-Fi interface there. */
void bridge_get_client_mac(uint8_t out[6]);
void bridge_set_client_mac(const uint8_t mac[6]);

/* Pause forwarding while flash is being written (OTA).
 * Writing to flash briefly disables the cache, which does not mix well with a
 * data path under full load. Management traffic keeps flowing, so the OTA
 * upload connection itself stays alive. */
void bridge_set_paused(bool paused);

/* Apply the immediately effective tuning values from g_cfg (TX power and the
 * two retry counters). Called after saving in the portal so changes can be
 * measured without a reboot.
 * The remaining tuning fields (buffers, BA window, HT40, 11b) only take effect
 * on the next boot - they go into wifi_init_config_t or must be set before
 * connecting. */
void bridge_apply_live_tuning(void);

/* ---------------------------------------------------------------------------
 * Autotune for the TX retries
 *
 * Measures the loss rate first at the current value of eth_tx_retries, then -
 * if that misses the target - goes up, and afterwards down as long as the loss
 * stays within bounds (details and rationale in bridge.cpp). If a step makes
 * the stream collapse, it is aborted after a few seconds.
 * The SMALLEST value that meets the loss target is chosen - not the one with
 * the best result. Reason: every retry blocks the Ethernet receive task a
 * little longer, and whatever arrives in that time is lost invisibly in the
 * DMA. More patience is not free.
 *
 * Needs RUNNING TRAFFIC, otherwise there is nothing to measure. Runs in the
 * background via bridge_tick(), so it blocks neither HTTP nor the data path.
 * ------------------------------------------------------------------------ */
typedef enum { AT_IDLE = 0, AT_RUNNING, AT_DONE, AT_TOO_LITTLE_TRAFFIC } autotune_state_t;

/* Start a run. Returns false if the bridge is not active or a run is already
 * in progress. */
bool             bridge_autotune_start(void);
/* Abort a running measurement or acknowledge a finished result - called when
 * a value is saved manually, so the entered value wins. */
void             bridge_autotune_reset(void);
autotune_state_t bridge_autotune_state(void);
uint8_t          bridge_autotune_step(void);       /* completed measurements */
uint8_t          bridge_autotune_count(void);      /* total candidates (upper bound) */
uint8_t          bridge_autotune_candidate(void);  /* retry value under test */
uint8_t          bridge_autotune_result(void);     /* chosen value, 0 = none */
/* Loss per candidate in hundredths of a percent, 0xFFFF = not measured */
void             bridge_autotune_values(uint8_t *candidates, uint16_t *loss);

/* ---------------------------------------------------------------------------
 * TX power autotune
 *
 * Measures the PHY's TX power steps from 13 to 20 dBm against live traffic:
 * loss (no TX buffer + abandoned on the air) and the mean time a frame waits
 * in the driver's TX queue. Starts at the current value, tries every higher
 * step, then lower ones until one falls out of tolerance, and saves the
 * LOWEST power that performs as well as the best one within tolerance.
 * Same timing, collapse guard and traffic requirement as the retry
 * autotune; the two never run at the same time. Uses the AT_* states.
 * ------------------------------------------------------------------------ */
bool             bridge_txp_tune_start(void);
/* Abort a run (restoring the configured power) or acknowledge a result. */
void             bridge_txp_tune_reset(void);
autotune_state_t bridge_txp_tune_state(void);
uint8_t          bridge_txp_tune_step(void);       /* completed measurements */
int8_t           bridge_txp_tune_candidate(void);  /* value under test, 0.25 dBm */
int8_t           bridge_txp_tune_result(void);     /* chosen value, 0 = none */
/* Per candidate: requested and driver-reported power (0.25 dBm), loss in
 * 1/100 % (0xFFFF = not measured, 0xFFFE = collapse) and queue delay in
 * 0.1 ms. Arrays need room for 8 entries; returns the number filled. */
size_t           bridge_txp_tune_values(int8_t *candidates, int8_t *actual,
                                        uint16_t *loss, uint16_t *delay_x10);

/* ---------------------------------------------------------------------------
 * TX queue cap autotune
 *
 * Measures without a cap first (reference), then caps 24, 16, 12, 8 frames,
 * and stops at the first step that loses noticeably more than the reference
 * or delivers less than 90 % of its throughput. Saves the smallest cap that
 * passed, or 0 (off) if none did. Same timing, collapse guard and traffic
 * requirement as the other autotunes; never runs alongside them.
 * ------------------------------------------------------------------------ */
bool             bridge_cap_tune_start(void);
/* Abort a run (restoring the configured cap) or acknowledge a result. */
void             bridge_cap_tune_reset(void);
autotune_state_t bridge_cap_tune_state(void);
uint8_t          bridge_cap_tune_step(void);       /* completed measurements */
uint8_t          bridge_cap_tune_candidate(void);  /* cap under test, 0 = off */
uint8_t          bridge_cap_tune_result(void);     /* chosen cap, 0 = off,
                                                    * 0xFF = no result yet   */
/* Per candidate: cap, loss in 1/100 % (0xFFFF = not measured, 0xFFFE =
 * collapse), queue delay in 0.1 ms and throughput in kbit/s. Arrays need
 * room for 8 entries; returns the number filled. */
size_t           bridge_cap_tune_values(uint8_t *candidates, uint16_t *loss,
                                        uint16_t *delay_x10, uint16_t *kbps);

/* TX power actually in effect, in 0.25 dBm units, queried from the driver.
 * Usually differs from the configured value because the PHY only knows 11
 * discrete steps and rounds down. <0 = could not be determined. */
int16_t bridge_get_tx_power(void);

/* Channel width actually negotiated, in MHz (20 or 40), from the driver. The
 * ht40 switch in the configuration only says what we OFFER - whether the AP
 * goes along with it is another matter. Without this value it is impossible
 * to tell whether HT40 brought nothing or was never established.
 * 0 = could not be determined. */
uint8_t bridge_get_bandwidth_mhz(void);

/* ===========================================================================
 * Disconnect log
 * ---------------------------------------------------------------------------
 * A plain disconnect counter does not say WHY the connection dropped, and the
 * serial log is not accessible on a deployed board. Every
 * WIFI_EVENT_STA_DISCONNECTED is therefore recorded with reason, RSSI and AP
 * in a small ring buffer in RTC memory: it survives soft reboots, watchdog
 * reboots and crashes, only not a power loss. That keeps visible what was
 * going on BEFORE a reboot.
 * ========================================================================= */

/* A single kick by the AP is often followed by a dozen failed reconnect
 * attempts; the buffer must be large enough that the trigger of such a burst
 * is still in it. 48 x 24 bytes of RTC memory. */
#define DISC_LOG_N 48

/* Who triggered the disconnect. Everything except EXTERN is a deliberate
 * disconnect by the firmware itself, not a radio problem. */
#define DISC_SRC_EXTERN   0u   /* AP or radio link */
#define DISC_SRC_CONNECT  1u   /* connect attempt at boot aborted */
#define DISC_SRC_WATCHDOG 2u   /* watchdog level "reconnect Wi-Fi" */
#define DISC_SRC_ROAM     3u   /* active AP switch */
#define DISC_SRC_RESTART  4u   /* clean disconnect during esp_restart() */

typedef struct {
  uint32_t t_s;        /* uptime in s at the drop (of the boot in 'boot') */
  uint32_t epoch;      /* UTC time at the drop, 0 = clock not set */
  uint16_t down_s;     /* seconds until reconnected, 0xFFFF = still down */
  uint8_t  reason;     /* wifi_err_reason_t */
  int8_t   rssi;       /* RSSI at the drop, from the driver */
  uint8_t  bssid[6];
  uint8_t  boot;       /* boot counter (mod 256) - see bridge_get_disc_log() */
  uint8_t  src;        /* DISC_SRC_* */
  uint8_t  was_up;     /* 1 = existing connection dropped (real drop),
                        * 0 = connect attempt failed (follow-up failure) */
  uint8_t  _pad[3];
} bridge_disc_t;

typedef struct {
  uint8_t  reason;
  uint16_t n;
} bridge_disc_count_t;

/* Copy up to `max` log entries into `out`, newest first. *boot_now receives
 * the counter of the current boot; entries with a different 'boot' come from
 * earlier boots, and their t_s then refers to that boot's uptime. Returns the
 * number of entries copied. */
size_t bridge_get_disc_log(bridge_disc_t *out, size_t max, uint8_t *boot_now);

/* Real drops (external, existing connection) per reason since THIS boot,
 * sorted by frequency, descending. *rest = drops whose reason no longer fit
 * into the table. Failed reconnect attempts do NOT count here - they are a
 * consequence, not a cause; see bridge_get_disc_totals(). */
size_t bridge_get_disc_counts(bridge_disc_count_t *out, size_t max, uint16_t *rest);

/* Totals since this boot, externally triggered only: real drops of an
 * existing connection, and failed connect attempts. */
void bridge_get_disc_totals(uint32_t *kicks, uint32_t *fails);

/* Short name of a wifi_err_reason_t, e.g. "BEACON_TIMEOUT". */
const char *bridge_wifi_reason_str(uint8_t reason);
