/*
 * config.h - Persistent configuration, stored as a single blob in NVS.
 *
 * The whole BridgeConfig struct is written to NVS as one binary blob,
 * prefixed with a magic word. See cfg_load() for how blobs written by older
 * firmware versions (with fewer fields) are handled.
 */
#pragma once

#include "compat.h"
#include <stdint.h>

#define CFG_MAGIC       0x42524447u   /* "BRDG" */
#define CFG_NVS_NS      "bridge"
#define CFG_NVS_KEY     "cfg"

struct BridgeConfig {
  uint32_t magic;

  char     name[32];          /* device name, used for MQTT/Home Assistant,
                               * the setup AP SSID and the NetBIOS name     */

  char     ssid1[33];
  char     pass1[65];
  char     ssid2[33];
  char     pass2[65];

  uint8_t  mgmt_ip[4];        /* management IP - MUST be static             */
  uint8_t  mgmt_mask[4];
  uint8_t  mgmt_gw[4];

  char     mqtt_host[64];     /* empty = MQTT disabled, 63 usable chars     */
  uint16_t mqtt_port;
  /* 65 bytes so that 64-character values (common for cloud broker tokens)
   * fit together with the terminating NUL byte.
   * WARNING: changing the size of an existing field shifts the offsets of
   * every field after it and breaks the append-only contract below - old
   * NVS blobs would then be misinterpreted. Never resize a field without
   * requiring a factory reset. */
  char     mqtt_user[65];
  char     mqtt_pass[65];

  uint16_t telemetry_s;       /* MQTT publish interval in seconds           */
  bool     configured;        /* false -> boot into provisioning mode       */

  /* --- From here on only APPEND, never insert in between. ----------------
   * cfg_load() also accepts shorter blobs written by older firmware and
   * leaves the missing (newer) fields at their defaults. That only works as
   * long as new fields are appended at the end - inserting a field shifts
   * everything behind it. */
  char     admin_pass[33];    /* protects OTA and coredump endpoints,
                               * empty = unprotected                        */

  /* --- Performance tuning -------------------------------------------------
   * Convention for all tuning fields: 0 means "don't override, use the
   * sdkconfig value or the PHY default". This keeps sdkconfig.defaults the
   * single source of truth for baseline values; the portal only overrides
   * what someone deliberately set. Having a second set of defaults here
   * would make changes to sdkconfig.defaults silently ineffective. */

  /* Applied IMMEDIATELY, no reboot needed: */
  int8_t   tx_power;          /* esp_wifi_set_max_tx_power, 0.25 dBm steps,
                               * valid 8..84, rounded down by the PHY to one
                               * of 11 steps. 0 = PHY default (maximum).    */
  uint8_t  eth_tx_retries;    /* Ethernet->Wi-Fi, retries on NO_MEM         */
  uint8_t  wifi_tx_retries;   /* Wi-Fi->Ethernet, retries                   */

  /* Require a REBOOT (they go into wifi_init_config_t or must be set before
   * connecting): */
  uint8_t  static_rx_buf;     /* wifi_init_config_t.static_rx_buf_num       */
  uint8_t  dynamic_rx_buf;    /* .dynamic_rx_buf_num                        */
  uint8_t  dynamic_tx_buf;    /* .dynamic_tx_buf_num - client upload path   */
  uint8_t  rx_ba_win;         /* .rx_ba_win, commonly recommended to stay
                               * <= static_rx_buf and <= dynamic_rx_buf/2   */
  uint8_t  ht40;              /* 1 = allow 40 MHz channel (only effective
                               * if the AP offers it as well)               */
  uint8_t  no_11b;            /* 1 = disable 802.11b rates                  */

  /* --- Network profile 2 --------------------------------------------------
   * IP settings belonging to ssid2/pass2, so the same configuration works at
   * two sites with different subnets. They are only applied when the
   * location detection (see bridge.cpp: detect_location()) selects this
   * profile.
   *
   * mgmt_ip2 == 0.0.0.0 means "not set": profile 1's IP settings are used.
   * Mask and gateway individually fall back to profile 1 if left empty, so a
   * half-filled profile never leaves the board unreachable. */
  uint8_t  mgmt_ip2[4];
  uint8_t  mgmt_mask2[4];
  uint8_t  mgmt_gw2[4];

  /* --- Learned client MAC -------------------------------------------------
   * The bridge can only start once it knows the MAC of the device on the
   * Ethernet port, and it learns it from the first frame that device sends.
   * Some devices (e.g. IP cameras) only send when THEY boot, so after a
   * reboot of the bridge alone nothing would arrive and the sniff would time
   * out. The learned MAC is therefore stored here and used as a fallback on
   * the next boot. 00:00:00:00:00:00 = none learned yet. */
  uint8_t  client_mac[6];

  /* --- Network profile 3 --------------------------------------------------
   * Same convention as profile 2: empty fields individually fall back to
   * profile 1, an empty IP means "profile not in use". */
  char     ssid3[33];
  char     pass3[65];
  uint8_t  mgmt_ip3[4];
  uint8_t  mgmt_mask3[4];
  uint8_t  mgmt_gw3[4];

  /* --- Watchdog -----------------------------------------------------------
   * Detects a bridge that is nominally connected (wifi_up = true, normal
   * RSSI) but has stopped delivering traffic, and escalates through
   * Ethernet reset, Wi-Fi reconnect and reboot (see bridge.cpp:
   * watchdog_tick()). Off by default: an automatic recovery action can be
   * unwelcome during an OTA upload or a deliberate diagnosis session, so it
   * is an explicit portal switch rather than always on. */
  uint8_t  wd_enable;          /* 1 = watchdog active, 0 = off (default)    */

  /* --- Wi-Fi connect behavior ---------------------------------------------
   * Both follow the 0 = firmware default convention (see bridge.cpp). A
   * single short attempt is not enough after a quick reboot, when the AP
   * may still hold the old session of the cloned MAC. */
  uint16_t wifi_connect_timeout_s;  /* timeout per attempt, 0 = 20 s        */
  uint8_t  wifi_connect_retries;    /* attempts per SSID, 0 = 3             */

  /* --- Portal idle reboot -------------------------------------------------
   * How long the bridge waits in provisioning mode for an active portal
   * session before it tries an ordinary reboot on its own, using the stored
   * credentials (the same path as any other boot). "Active" means: page
   * loaded, configuration saved or a scan started - NOT the page's automatic
   * 2 s status poll, otherwise a browser tab left open would prevent the
   * reboot forever. Only applies when g_cfg.configured is set (see
   * main.cpp); otherwise a fresh or factory-reset device would reboot
   * pointlessly every few minutes. 0 = firmware default (900 s = 15 min). */
  uint16_t ap_idle_reboot_s;

  /* --- Active roaming -----------------------------------------------------
   * WIFI_ALL_CHANNEL_SCAN + WIFI_CONNECT_AP_BY_SIGNAL (see bridge.cpp) pick
   * the strongest AP only ONCE, at connect time. ESP-IDF does not roam on its
   * own, so the station stays on that AP even if a much better one (e.g.
   * another mesh node) becomes available later. The watchdog does not help
   * reliably here: a weak link can stay below its loss threshold although a
   * far better AP is in range. See bridge.cpp: roam_tick().
   *
   * roam_enable is a switch like wd_enable; the three numeric values follow
   * the 0 = firmware default convention, see bridge.cpp for the defaults. */
  uint8_t  roam_enable;          /* 1 = active, 0 = off (default)           */
  uint16_t roam_check_s;         /* check interval, 0 = default             */
  int8_t   roam_rssi_threshold;  /* only check below this RSSI (0 dBm never
                                   * occurs in practice, so it is safe as the
                                   * "default" marker), 0 = default          */
  uint8_t  roam_margin_db;       /* minimum advantage of a candidate in dB to
                                   * justify switching, 0 = default          */

  /* --- Watchdog escalation levels -----------------------------------------
   * Allows limiting how far the watchdog escalates, e.g. only ever restart
   * the Ethernet driver and never touch Wi-Fi. wd_actions is a bit mask of
   * the ALLOWED actions (WD_ACT_* in bridge.h); disallowed levels are
   * skipped. 0 = default (all three), matching the 0 convention of the other
   * tuning fields. */
  uint8_t  wd_actions;           /* bit mask of WD_ACT_*, 0 = all           */
  uint8_t  wd_bad_windows;       /* consecutive bad measurement windows
                                   * before the next action, 0 = default (3) */

  /* --- TX queue cap (live) ------------------------------------------------
   * Maximum number of LAN->Wi-Fi frames allowed to wait in the driver's TX
   * queue; a larger frame arriving beyond that is dropped at once instead of
   * occupying one of the dynamic TX buffers. Bounds the queue delay (and the
   * heap the queue eats) at the cost of some loss in bursts - see
   * bridge.cpp: eth_rx_cb(). 0 = off, i.e. the queue is only limited by the
   * TX buffer count. */
  uint8_t  txq_cap;
};

extern BridgeConfig g_cfg;

/* Load the configuration from NVS into g_cfg; falls back to defaults if
 * nothing (valid) is stored. */
void cfg_load(void);
/* Write g_cfg to NVS. Returns false if NVS could not be opened or written. */
bool cfg_save(void);
/* Fill *c with factory defaults. */
void cfg_defaults(BridgeConfig *c);
/* Erase the stored configuration and reset g_cfg to defaults. */
bool cfg_factory_reset(void);

/* Parse a dotted-quad IPv4 string ("192.168.1.10") into four bytes.
 * Returns false (and leaves `out` untouched) on malformed input. */
bool parse_ipv4(const char *s, uint8_t out[4]);
/* Format four IPv4 bytes as a dotted-quad string. */
void format_ipv4(const uint8_t ip[4], char *out, size_t out_len);
