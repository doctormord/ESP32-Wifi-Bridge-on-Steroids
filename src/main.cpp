/*
 * ============================================================================
 *  ESP32 Wi-Fi <-> Ethernet Transparent Bridge
 *  with management IP, configuration portal and MQTT telemetry
 * ============================================================================
 *
 *  Target hardware : WT32-ETH01 (ESP32-D0WD + LAN8720A)
 *  Toolchain       : PlatformIO, framework = espidf
 *
 *  ---------------------------------------------------------------------------
 *  OPERATING MODES
 *  ---------------------------------------------------------------------------
 *
 *  PROVISIONING  (no SSID stored, reset pin pulled low,
 *                 or bringing up the bridge failed)
 *      -> access point "<name>-setup-XXXX"
 *      -> http://192.168.4.1 : set name, Wi-Fi networks, management IP, MQTT
 *      -> saving triggers a reboot
 *
 *  BRIDGE
 *      1. start Ethernet, sniff the MAC of the attached client
 *      2. enable promiscuous mode
 *      3. connect the Wi-Fi STA using the CLONED MAC
 *      4. attach the management netif with its static IP
 *      5. arm the data path, start the web server + MQTT
 *
 *  ---------------------------------------------------------------------------
 *  WHY TWO IPs ON ONE MAC WORK
 *  ---------------------------------------------------------------------------
 *  The Wi-Fi STA carries the client's MAC (required, because an 802.11 STA
 *  works in 3-address mode and the AP only delivers frames to that MAC). The
 *  management stack sits on the same interface and therefore the same MAC,
 *  but has its own IP. ARP resolves both IPs to the same MAC - completely
 *  unremarkable for switches and APs.
 *
 *  The management IP MUST be static: a DHCP server would hand the same MAC
 *  the same lease twice.
 * ============================================================================
 */

#include "compat.h"

#include "driver/gpio.h"
#include "esp_attr.h"

#include "config.h"
#include "bridge.h"
#include "web.h"
#include "telemetry.h"
#include "crashlog.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "lwip/apps/netbiosns.h"

/* Pull this pin to GND at boot for a factory reset. On the WT32-ETH01 most
 * GPIOs are taken by RMII; IO14 is free and not a strapping pin. */
#define FACTORY_RESET_GPIO   14

/* How long to wait for the first client frame before giving up and offering
 * the portal instead. Without a client MAC there is no bridge. */
#define MAC_SNIFF_TIMEOUT_MS 60000

static bool s_provisioning = false;
static bool s_ota_confirmed = false;

/*
 * "Boot into the portal" request that survives a software reset.
 *
 * RTC_NOINIT_ATTR means: preserved across esp_restart(), but undefined after
 * a real power-on - hence the magic word. That is exactly the behavior we
 * want: after a failed bridge start the next boot goes straight to the
 * portal, but a power cycle tries the bridge again normally (e.g. because the
 * AP is back by then).
 */
#define PORTAL_MAGIC 0x50525450u   /* "PRTP" */
RTC_NOINIT_ATTR static uint32_t s_portal_request;

/*
 * Boot-loop protection.
 *
 * The portal request above only covers a Wi-Fi bring-up that fails cleanly.
 * A crash DURING operation - e.g. because someone set the buffers so high
 * that the heap runs out - instead causes reboot, crash, reboot: an endless
 * loop that cannot be escaped without a serial cable.
 *
 * So we count boots that never reached "stable". After three such boots the
 * board enters the portal, where the offending setting can be corrected. The
 * counter lives in RTC memory: it survives a software reset but not pulling
 * the plug - a real power-on should start over without prejudice.
 */
#define BOOT_MAGIC   0x424f4f54u   /* "BOOT" */
#define BOOT_MAX_FAILED_BOOTS 3
#define BOOT_STABLE_MS        60000  /* uptime needed for a boot to count as stable */
RTC_NOINIT_ATTR static uint32_t s_boot_magic;
RTC_NOINIT_ATTR static uint32_t s_boot_failures;
static bool s_stable_reported = false;

/*
 * Confirm a freshly flashed OTA image so it is not rolled back.
 *
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE a newly flashed firmware boots
 * in state PENDING_VERIFY. If it never calls
 * esp_ota_mark_app_valid_cancel_rollback(), the bootloader reverts to the
 * previous version on the next boot.
 *
 * The criterion is deliberately NOT "has booted" but "is reachable over the
 * network" (the caller only invokes this after 30 s of uptime) - losing
 * network access is what hurts on a board installed without serial access.
 *
 * In portal mode it depends on why the portal is up. Without stored
 * credentials (fresh or factory-reset device) the portal IS the intended
 * state, so the image is confirmed. WITH stored credentials, the portal
 * means this firmware could not get the bridge onto the network that the
 * previous one served - so it is deliberately left unconfirmed: the portal
 * idle reboot then rolls back to the previous image instead of looping
 * between a failed connect and the portal with a broken firmware that
 * confirmed itself.
 */
static void ota_confirm_if_pending(void) {
  if (s_ota_confirmed) return;

  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(run, &st) != ESP_OK) return;
  if (st != ESP_OTA_IMG_PENDING_VERIFY) { s_ota_confirmed = true; return; }

  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    s_ota_confirmed = true;
    printf("[OTA ] New firmware confirmed, no rollback\n");
  }
}

/* Check whether the factory-reset pin is held low at boot.
 *
 * Configures the pin as input with pull-up and requires it to stay low for
 * two seconds, so a glitch or a briefly touching wire does not wipe the
 * configuration. */
static bool factory_reset_requested(void) {
  gpio_config_t io = {};
  io.pin_bit_mask = 1ULL << FACTORY_RESET_GPIO;
  io.mode         = GPIO_MODE_INPUT;
  io.pull_up_en   = GPIO_PULLUP_ENABLE;
  gpio_config(&io);
  delay(20);
  if (gpio_get_level((gpio_num_t)FACTORY_RESET_GPIO) != 0) return false;

  printf("[BOOT] Reset pin detected, hold for 2 s...\n");
  for (int i = 0; i < 20; i++) {
    delay(100);
    if (gpio_get_level((gpio_num_t)FACTORY_RESET_GPIO) != 0) return false;
  }
  return true;
}

/* Switch into provisioning mode: start the setup AP and the HTTP portal.
 * `reason` is only logged. Ethernet bridging is not attempted in this mode. */
static void enter_provisioning(const char *reason) {
  printf("[BOOT] Provisioning mode: %s\n", reason);
  s_provisioning = true;
  portal_start_ap();
  web_start(true);
}

/* One-time boot sequence (the ESP-IDF counterpart of Arduino's setup()).
 *
 * Initializes NVS, netif and the event loop, loads the configuration, then
 * decides between provisioning and bridge mode (see the file header for the
 * criteria) and brings the chosen mode up. */
static void bridge_setup(void) {
  /* No Serial.begin needed - the IDF console is already up at this point. */
  delay(300);
  printf("\n=== ESP32 Wi-Fi <-> Ethernet Bridge ===\n");

  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }

  ESP_ERROR_CHECK(esp_netif_init());
  esp_err_t e = esp_event_loop_create_default();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(e);

  cfg_load();

  /* Evaluate once and clear immediately - otherwise a single failed attempt
   * would leave the board stuck in the portal for good. */
  const bool portal_forced = (s_portal_request == PORTAL_MAGIC);
  s_portal_request = 0;

  /* Count failed boots. On the very first power-on RTC memory is undefined;
   * the missing magic word identifies that case. */
  bool too_many_failures = false;

  /* Exception: a boot caused by a deliberate watchdog restart (bridge.cpp,
   * highest escalation level) does NOT count as a failed boot - the device
   * booted and ran fine, it only restarted itself for Wi-Fi health reasons.
   * Without this exception, several watchdog restarts during a longer Wi-Fi
   * outage would trip the boot-loop protection, which is meant for something
   * else entirely. Must be evaluated BEFORE the power-on branch below,
   * otherwise the one-shot magic word would be cleared without being
   * consumed. */
  const bool planned_restart = bridge_consume_planned_restart();

  /* Log an unexpected reset of the previous boot (timestamp taken from the
   * RTC heartbeat) - in every mode, even if we go straight to the portal. */
  crashlog_boot(planned_restart);

  if (s_boot_magic != BOOT_MAGIC) {
    s_boot_magic = BOOT_MAGIC;
    s_boot_failures = 0;
  } else if (planned_restart) {
    /* s_boot_failures stays unchanged. */
  } else {
    s_boot_failures++;
    if (s_boot_failures >= BOOT_MAX_FAILED_BOOTS) {
      too_many_failures = true;
      s_boot_failures = 0;      /* one-shot, otherwise it would stick forever */
    }
  }

  if (factory_reset_requested()) {
    cfg_factory_reset();
    printf("[BOOT] Factory reset done\n");
    enter_provisioning("factory reset");
    return;
  }

  if (too_many_failures) {
    /* Rebooted several times without ever running stably. Trying again would
     * just continue the loop - go to the portal instead, where the cause can
     * be fixed. */
    printf("[BOOT] Several boots without stable operation\n");
    enter_provisioning("repeated reboots - check settings");
    return;
  }

  if (portal_forced) {
    enter_provisioning("previous bridge start failed");
    return;
  }

  if (!g_cfg.configured || g_cfg.ssid1[0] == '\0') {
    enter_provisioning("no Wi-Fi credentials stored");
    return;
  }

  /* --- Bridge mode ------------------------------------------------------- */
  if (!bridge_eth_init()) {
    printf("[BOOT] Ethernet init failed - rebooting in 5 s\n");
    delay(5000);
    esp_restart();
  }

  /* Determine the client MAC.
   *
   * Sniffing alone is not enough: it waits for a frame from the client, and
   * some clients (e.g. IP cameras) only send while THEY boot. If only the
   * bridge restarts (reboot, OTA), nothing arrives and the board would end up
   * in the portal, unreachable.
   *
   * Therefore a MAC learned once is stored in NVS and used as a fallback. The
   * sniff still runs so that a DEVICE CHANGE is noticed.
   *
   * About the 30 s window when a MAC is stored: it is only used up if the
   * client really stays silent - as soon as a frame arrives, boot continues
   * immediately, so normally it costs nothing. A shorter window would still
   * be wrong: when a different device is plugged in, its first frame must
   * fall into this window, otherwise the bridge keeps cloning the old MAC and
   * return traffic from the AP never arrives (the AP has a different MAC
   * associated). A booting camera can take longer than 10 s. */
  uint8_t stored_mac[6];
  memcpy(stored_mac, g_cfg.client_mac, 6);
  const bool have_mac = (stored_mac[0] | stored_mac[1] | stored_mac[2] |
                         stored_mac[3] | stored_mac[4] | stored_mac[5]) != 0;

  if (bridge_sniff_client_mac(have_mac ? 30000 : MAC_SNIFF_TIMEOUT_MS)) {
    uint8_t learned[6];
    bridge_get_client_mac(learned);
    if (memcmp(learned, g_cfg.client_mac, 6) != 0) {
      memcpy(g_cfg.client_mac, learned, 6);
      cfg_save();
      printf("[ETH ] New client MAC stored\n");
    }
  } else if (have_mac) {
    bridge_set_client_mac(stored_mac);
  } else {
    /* Never seen a client - the portal is the only option left. */
    enter_provisioning("no client detected on the Ethernet port");
    return;
  }

  if (!bridge_wifi_start()) {
    /* Do NOT switch to the portal directly. bridge_wifi_start() has already
     * initialized the Wi-Fi stack and created a STA netif; portal_start_ap()
     * would create both again, and esp_netif_create_default_wifi_sta()
     * reports a duplicate if_key not with an error code but with an assert -
     * a boot loop in which the portal never appears, exactly the state that
     * cannot be escaped without serial access.
     *
     * Rather than tearing down half the stack by hand (many failure modes,
     * itself untested), force a clean reboot straight into the portal. */
    printf("[BOOT] Wi-Fi failed - rebooting straight into the portal\n");
    s_portal_request = PORTAL_MAGIC;
    delay(200);            /* let the log output drain */
    esp_restart();
  }

  bridge_activate();

  /* Ask for the active address instead of printing g_cfg.mgmt_ip: when the
   * location detection picked another profile, that profile's IP applies. */
  uint8_t active_ip[4];
  bridge_get_mgmt_ip(active_ip);
  char ip[16];
  format_ipv4(active_ip, ip, sizeof(ip));
  printf("[BOOT] Management UI: http://%s\n", ip);

  /* Wall-clock time for crash and disconnect timestamps. Ask the netif for
   * the gateway - with several profiles it is the authoritative source. */
  esp_netif_ip_info_t ipi = {};
  if (esp_netif_get_ip_info(bridge_mgmt_netif(), &ipi) == ESP_OK && ipi.gw.addr) {
    uint8_t gw[4];
    memcpy(gw, &ipi.gw.addr, 4);
    crashlog_time_start(gw);
  }

#ifdef BRIDGE_MINIMAL
  /* Neither web server nor MQTT - that is the point of this build variant
   * (it exists to measure whether the management stack costs throughput).
   * Without management access there is also no way back via OTA. The
   * rollback mechanism provides it instead: an image freshly flashed via OTA
   * is PENDING_VERIFY, and because bridge_loop() NEVER calls
   * esp_ota_mark_app_valid_cancel_rollback() in this variant, the bootloader
   * falls back to the previous slot on the next boot. A power cycle is
   * enough to get the full firmware back - no cable, no jumper. */
  printf("\n*** MINIMAL VARIANT - bridge only, no management ***\n");
  printf("*** A reboot automatically restores the full firmware ***\n\n");
#else
  web_start(false);
  telemetry_start();
#endif
}

/*
 * Announce a NetBIOS name for the management interface.
 *
 * Why at all: many home routers take device names from the DHCP request
 * (option 12). The management interface has a STATIC IP by design and never
 * asks, so the router's device list shows "unknown". NetBIOS is the second
 * way such routers learn names (that is how Windows machines get named), and
 * lwIP's responder is already compiled into IDF - no component manager, no
 * new dependency.
 *
 * The name carries the last octet of the client IP, so the device list shows
 * WHOM this bridge is currently bridging - e.g. WIFIBRIDGE-109. Both
 * necessarily share one MAC, so the list shows only one entry; this way it
 * at least says what it is.
 *
 * NetBIOS allows 15 characters. The device name is shortened accordingly so
 * the suffix always fits - better a truncated name than a lost association.
 * Called from every loop iteration; it only touches the responder when the
 * resulting name changes.
 */
static void netbios_update_name(void) {
  static bool  init_done = false;
  static char  current[16] = "";

  if (s_provisioning || !bridge_is_active()) return;

  if (!init_done) { netbiosns_init(); init_done = true; }

  BridgeStats st;
  bridge_get_stats(&st);

  /* Extract the last octet of the client IP; "-" means not known yet. */
  const char *dot = strrchr(st.client_ip, '.');
  const char *suffix = (dot && dot[1]) ? dot + 1 : NULL;

  char name[16];
  size_t room = suffix ? 15 - 1 - strlen(suffix) : 15;   /* 1 for the hyphen */
  size_t o = 0;
  for (size_t i = 0; g_cfg.name[i] && o < room; i++) {
    const char c = g_cfg.name[i];
    if      (c >= 'a' && c <= 'z') name[o++] = c - 32;    /* NetBIOS prefers upper case */
    else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) name[o++] = c;
    else if (o) name[o++] = '-';                          /* replace everything else */
  }
  /* Strip trailing separators, otherwise a name like "WIFIBRIDGE_1" would
   * become the double-separated "WIFIBRIDGE--109". */
  while (o && name[o - 1] == '-') o--;
  if (!o) { name[o++] = 'B'; name[o++] = 'R'; name[o++] = 'I'; name[o++] = 'D'; name[o++] = 'G'; name[o++] = 'E'; }
  if (suffix) { name[o++] = '-'; for (const char *q = suffix; *q && o < 15; q++) name[o++] = *q; }
  name[o] = '\0';

  if (strcmp(name, current) != 0) {
    netbiosns_set_name(name);
    strcpy(current, name);
    printf("[NBNS] Name: %s\n", name);
  }
}

/* One iteration of the main loop (the counterpart of Arduino's loop()),
 * called every ~50 ms.
 *
 * Handles OTA confirmation and the stable-boot marker in both modes; in
 * bridge mode it drives the periodic bridge/watchdog/roaming logic, the
 * NetBIOS name and MQTT telemetry; in provisioning mode it implements the
 * portal idle reboot. */
static void bridge_loop(void) {
  /* Confirm only after 30 s. A crash shortly after boot should still lead to
   * a rollback - that is why this is not done right away in setup. */
#ifndef BRIDGE_MINIMAL
  /* Deliberately NOT confirmed in the minimal variant - see bridge_setup().
   * The missing confirmation IS the way back. */
  if (!s_ota_confirmed && millis() > 30000 && !(s_provisioning && g_cfg.configured))
    ota_confirm_if_pending();
#endif

  /* Running long enough -> reset the failed-boot counter. Only from here on
   * does a boot count as successful; a crash before this point keeps
   * counting. */
  if (!s_stable_reported && millis() > BOOT_STABLE_MS) {
    s_stable_reported = true;
    s_boot_failures = 0;
  }

  if (!s_provisioning) {
    bridge_tick();
    netbios_update_name();
#ifndef BRIDGE_MINIMAL
    telemetry_tick();
#endif
  } else if (g_cfg.configured) {
    /* Portal mode: after a while without active portal use, try an ordinary
     * reboot on our own - see config.h: ap_idle_reboot_s. Only with stored
     * credentials (g_cfg.configured), otherwise a fresh or factory-reset
     * device would reboot in circles without anything ever changing.
     * web_last_activity_ms() deliberately only counts real configuration
     * actions, not the 2 s status poll - otherwise a browser tab left open
     * would prevent the reboot forever. */
    const uint32_t threshold_ms =
        (g_cfg.ap_idle_reboot_s ? g_cfg.ap_idle_reboot_s : AP_IDLE_REBOOT_S_DEF) * 1000UL;
    if (millis() - web_last_activity_ms() >= threshold_ms) {
      printf("[BOOT] %lu s without portal use - trying a reboot with the stored credentials\n",
             (unsigned long)(threshold_ms / 1000));
      delay(200);
      esp_restart();
    }
  }
  delay(50);
}

/* ---------------------------------------------------------------------------
 * Entry point. Replaces Arduino's setup()/loop() - same structure, just
 * without the Arduino core behind it.
 * ------------------------------------------------------------------------ */
extern "C" void app_main(void) {
  bridge_setup();
  for (;;) {
    bridge_loop();
  }
}
