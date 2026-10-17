/*
 * telemetry.cpp - MQTT connection including Home Assistant autodiscovery
 *
 * Only runs once the management netif has an IP. The MQTT client uses the
 * same lwIP stack that the selective delivery in bridge.cpp feeds with
 * packets.
 *
 * Topics (<node> = last three bytes of the STA MAC, hex):
 *   bridge/<node>/state         periodic JSON state
 *   bridge/<node>/availability  "online" / "offline" (last will)
 *   bridge/<node>/watchdog      discrete watchdog events
 *   homeassistant/...           retained discovery entries
 */

#include "telemetry.h"
#include "config.h"
#include "bridge.h"

#include <string.h>

#include "mqtt_client.h"
#include "esp_wifi.h"

static esp_mqtt_client_handle_t s_mqtt = NULL;
static bool  s_connected  = false;
static bool  s_announced  = false;
static char  s_node[16]   = "bridge";
static char  s_base[48];
static char  s_avail[64];
static char  s_state[64];

/* MQTT event handler: tracks the connection state, publishes "online" on
 * connect and schedules a fresh discovery announcement after a reconnect. */
static void mqtt_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg; (void)base;
  esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;

  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      s_connected = true;
      printf("[MQTT] Connected\n");
      esp_mqtt_client_publish(s_mqtt, s_avail, "online", 0, 1, 1);
      break;
    case MQTT_EVENT_DISCONNECTED:
      s_connected = false;
      s_announced = false;      /* announce again after reconnecting */
      printf("[MQTT] Disconnected\n");
      break;
    default:
      (void)e;
      break;
  }
}

/* Publish one Home Assistant sensor discovery entry (retained). `key` is the
 * field in the state JSON; unit/devclass/stateclass/icon are optional (NULL).
 * Kept short so everything fits into one buffer. */
static void announce_one(const char *key, const char *name, const char *unit,
                         const char *devclass, const char *stateclass,
                         const char *icon) {
  char topic[160];
  snprintf(topic, sizeof(topic),
           "homeassistant/sensor/bridge_%s/%s/config", s_node, key);

  char extra[160] = "";
  size_t o = 0;
  if (unit)       o += snprintf(extra + o, sizeof(extra) - o, ",\"unit_of_measurement\":\"%s\"", unit);
  if (devclass)   o += snprintf(extra + o, sizeof(extra) - o, ",\"device_class\":\"%s\"", devclass);
  if (stateclass) o += snprintf(extra + o, sizeof(extra) - o, ",\"state_class\":\"%s\"", stateclass);
  if (icon)       o += snprintf(extra + o, sizeof(extra) - o, ",\"icon\":\"%s\"", icon);

  char payload[640];
  snprintf(payload, sizeof(payload),
    "{\"name\":\"%s\",\"uniq_id\":\"bridge_%s_%s\","
    "\"stat_t\":\"%s\",\"avty_t\":\"%s\","
    "\"val_tpl\":\"{{ value_json.%s }}\"%s,"
    "\"dev\":{\"ids\":[\"bridge_%s\"],\"name\":\"%s\","
    "\"mf\":\"DIY\",\"mdl\":\"WT32-ETH01 L2 Bridge\"}}",
    name, s_node, key, s_state, s_avail, key, extra, s_node, g_cfg.name);

  esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 1);
}

/* Publish all discovery entries (sensors plus the Ethernet link binary
 * sensor). Called on the first telemetry tick after each (re)connect. */
static void announce_all(void) {
  announce_one("rssi",     "Signal strength",     "dBm", "signal_strength", "measurement", NULL);
  announce_one("ch",       "Wi-Fi channel",       NULL,  NULL, NULL, "mdi:wifi");
  announce_one("kbps_up",  "Throughput LAN to Wi-Fi", "kbit/s", "data_rate", "measurement", NULL);
  announce_one("kbps_down","Throughput Wi-Fi to LAN", "kbit/s", "data_rate", "measurement", NULL);
  announce_one("drop_up",  "Dropped LAN to Wi-Fi", "pkt", NULL, "total_increasing", "mdi:alert");
  announce_one("drop_down","Dropped Wi-Fi to LAN", "pkt", NULL, "total_increasing", "mdi:alert");
  announce_one("uptime",   "Uptime",              "s",   "duration", "total_increasing", NULL);
  announce_one("heap",     "Free heap",           "B",   NULL, "measurement", "mdi:memory");
  /* IP of the bridged device. For a camera without its own display this is
   * often the only way to find out under which address it is reachable - the
   * bridge sees it in the data path anyway. */
  announce_one("client_ip","Client IP",           NULL,  NULL, NULL, "mdi:ip-network");
  /* Increase with every watchdog escalation (Ethernet or Wi-Fi level) - the
   * trend is the signal, not the absolute value. The forced reboot (highest
   * level) is NOT visible here separately - telemetry_note_watchdog_event()
   * and the coredump cover that. */
  announce_one("wd_reconnects","Watchdog Wi-Fi reconnects",NULL, NULL, "total_increasing", "mdi:wifi-refresh");
  announce_one("wd_eth_resets","Watchdog Ethernet resets",NULL, NULL, "total_increasing", "mdi:lan-connect");
  announce_one("roam_count","Active AP switches",NULL, NULL, "total_increasing", "mdi:wifi-arrow-left-right");
  /* All Wi-Fi disconnects since boot and the reason of the last one - the
   * history with RSSI/AP/duration is available at /api/disconnects. */
  announce_one("wifi_disc","Wi-Fi disconnects total",NULL, NULL, "total_increasing", "mdi:wifi-off");
  /* Only real drops of an existing connection, without the failed attempts
   * that follow - this is the meaningful curve. */
  announce_one("wifi_kicks","Wi-Fi drops",NULL, NULL, "total_increasing", "mdi:wifi-alert");
  announce_one("disc_reason","Last drop reason",NULL, NULL, NULL, "mdi:wifi-alert");

  char topic[160], payload[512];
  snprintf(topic, sizeof(topic),
           "homeassistant/binary_sensor/bridge_%s/eth/config", s_node);
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Ethernet link\",\"uniq_id\":\"bridge_%s_eth\","
    "\"stat_t\":\"%s\",\"avty_t\":\"%s\","
    "\"val_tpl\":\"{{ 'ON' if value_json.eth == 1 else 'OFF' }}\","
    "\"dev_cla\":\"connectivity\","
    "\"dev\":{\"ids\":[\"bridge_%s\"],\"name\":\"%s\"}}",
    s_node, s_state, s_avail, s_node, g_cfg.name);
  esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 1);

  s_announced = true;
  printf("[MQTT] Discovery sent\n");
}

/* See telemetry.h. Derives the node id and topics from the STA MAC (= the
 * cloned client MAC), configures the last will on the availability topic and
 * starts the client asynchronously. */
bool telemetry_start(void) {
  if (g_cfg.mqtt_host[0] == '\0') {
    printf("[MQTT] No broker configured - skipped\n");
    return false;
  }

  uint8_t mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  snprintf(s_node, sizeof(s_node), "%02x%02x%02x", mac[3], mac[4], mac[5]);
  snprintf(s_base,  sizeof(s_base),  "bridge/%s", s_node);
  snprintf(s_state, sizeof(s_state), "%s/state", s_base);
  snprintf(s_avail, sizeof(s_avail), "%s/availability", s_base);

  char uri[96];
  snprintf(uri, sizeof(uri), "mqtt://%s:%u", g_cfg.mqtt_host, g_cfg.mqtt_port);

  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri              = uri;
  cfg.credentials.client_id           = s_node;
  cfg.session.last_will.topic         = s_avail;
  cfg.session.last_will.msg           = "offline";
  cfg.session.last_will.qos           = 1;
  cfg.session.last_will.retain        = 1;
  cfg.session.keepalive               = 30;
  if (g_cfg.mqtt_user[0]) {
    cfg.credentials.username                 = g_cfg.mqtt_user;
    cfg.credentials.authentication.password  = g_cfg.mqtt_pass;
  }

  s_mqtt = esp_mqtt_client_init(&cfg);
  if (!s_mqtt) { printf("[MQTT] Init failed\n"); return false; }

  esp_mqtt_client_register_event(s_mqtt, MQTT_EVENT_ANY, mqtt_evt, NULL);
  esp_mqtt_client_start(s_mqtt);
  printf("[MQTT] Connecting to %s\n", uri);
  return true;
}

/* See telemetry.h. Sends the discovery entries first if needed, then the
 * state JSON (QoS 0, not retained). */
void telemetry_tick(void) {
  if (!s_mqtt || !s_connected) return;

  static uint32_t last = 0;
  const uint32_t interval = (uint32_t)g_cfg.telemetry_s * 1000UL;
  if (millis() - last < interval) return;
  last = millis();

  if (!s_announced) announce_all();

  BridgeStats st;
  bridge_get_stats(&st);

  char payload[600];
  snprintf(payload, sizeof(payload),
    "{\"rssi\":%d,\"ch\":%u,\"kbps_up\":%lu,\"kbps_down\":%lu,"
    "\"pkt_up\":%lu,\"pkt_down\":%lu,\"drop_up\":%lu,\"drop_down\":%lu,"
    "\"wifi_disc\":%lu,\"wifi_kicks\":%lu,\"disc_reason\":\"%s\","
    "\"wd_reconnects\":%lu,\"wd_eth_resets\":%lu,\"roam_count\":%lu,"
    "\"eth\":%d,\"uptime\":%lu,\"heap\":%lu,\"bssid\":\"%s\","
    "\"client_ip\":\"%s\"}",
    (int)st.rssi, (unsigned)st.channel,
    (unsigned long)st.kbps_eth2wifi, (unsigned long)st.kbps_wifi2eth,
    (unsigned long)st.pkt_eth2wifi,  (unsigned long)st.pkt_wifi2eth,
    (unsigned long)st.drop_eth2wifi, (unsigned long)st.drop_wifi2eth,
    (unsigned long)st.wifi_disc_count, (unsigned long)st.wifi_kicks,
    st.disc_last_reason ? bridge_wifi_reason_str(st.disc_last_reason) : "-",
    (unsigned long)st.wd_reconnects, (unsigned long)st.wd_eth_resets, (unsigned long)st.roam_count,
    st.eth_link ? 1 : 0, (unsigned long)(millis() / 1000),
    (unsigned long)esp_get_free_heap_size(), st.bssid, st.client_ip);

  esp_mqtt_client_publish(s_mqtt, s_state, payload, 0, 0, 0);
}

/* See telemetry.h. */
void telemetry_note_watchdog_event(const char *action, const char *reason) {
  if (!s_mqtt || !s_connected) return;

  char topic[64];
  snprintf(topic, sizeof(topic), "%s/watchdog", s_base);

  char payload[128];
  snprintf(payload, sizeof(payload), "{\"action\":\"%s\",\"reason\":\"%s\",\"uptime\":%lu}",
           action, reason ? reason : "", (unsigned long)(millis() / 1000));

  /* QoS 1, not retained - an event, not a persistent state. Called
   * synchronously from the watchdog escalation (also right before a forced
   * reboot) - esp_mqtt_client_publish() itself returns immediately, so the
   * caller has to wait briefly for it to go out, see watchdog_tick() in
   * bridge.cpp. */
  esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 0);
}
