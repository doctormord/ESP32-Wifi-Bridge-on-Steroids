/*
 * telemetry.cpp - MQTT-Anbindung inkl. Home-Assistant-Autodiscovery
 *
 * Laeuft erst, wenn das Management-Netif eine IP hat. Der MQTT-Client nutzt
 * denselben lwIP-Stack, den wir ueber die Selektivzustellung in bridge.cpp
 * mit Paketen versorgen.
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

static void mqtt_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg; (void)base;
  esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;

  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      s_connected = true;
      printf("[MQTT] Verbunden\n");
      esp_mqtt_client_publish(s_mqtt, s_avail, "online", 0, 1, 1);
      break;
    case MQTT_EVENT_DISCONNECTED:
      s_connected = false;
      s_announced = false;      /* nach Reconnect neu ankuendigen */
      printf("[MQTT] Getrennt\n");
      break;
    default:
      (void)e;
      break;
  }
}

/* Ein Discovery-Eintrag. Kurz gehalten, damit alles in einen Puffer passt. */
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

static void announce_all(void) {
  announce_one("rssi",     "Signalstaerke",       "dBm", "signal_strength", "measurement", NULL);
  announce_one("ch",       "WLAN-Kanal",          NULL,  NULL, NULL, "mdi:wifi");
  announce_one("kbps_up",  "Durchsatz LAN-WLAN",  "kbit/s", "data_rate", "measurement", NULL);
  announce_one("kbps_down","Durchsatz WLAN-LAN",  "kbit/s", "data_rate", "measurement", NULL);
  announce_one("drop_up",  "Verworfen LAN-WLAN",  "pkt", NULL, "total_increasing", "mdi:alert");
  announce_one("drop_down","Verworfen WLAN-LAN",  "pkt", NULL, "total_increasing", "mdi:alert");
  announce_one("uptime",   "Laufzeit",            "s",   "duration", "total_increasing", NULL);
  announce_one("heap",     "Freier Heap",         "B",   NULL, "measurement", "mdi:memory");
  /* Die IP des gebridgten Geraets. Fuer eine Kamera ohne eigenes Display ist
   * das oft die einzige Moeglichkeit herauszufinden, unter welcher Adresse
   * sie zu erreichen ist - die Bridge sieht sie im Datenpfad ohnehin. */
  announce_one("client_ip","IP des Clients",      NULL,  NULL, NULL, "mdi:ip-network");
  /* Steigen bei jeder Watchdog-Eskalation (Ethernet- bzw. WLAN-Stufe) - allein
   * die Tendenz ist das Signal, nicht der Absolutwert. Der erzwungene
   * Neustart (letzte Stufe) ist hier NICHT gesondert sichtbar - dafuer gibt
   * es telemetry_note_watchdog_event() und den Coredump. */
  announce_one("wd_reconnects","Watchdog-WLAN-Reconnects",NULL, NULL, "total_increasing", "mdi:wifi-refresh");
  announce_one("wd_eth_resets","Watchdog-Ethernet-Resets",NULL, NULL, "total_increasing", "mdi:lan-connect");

  char topic[160], payload[512];
  snprintf(topic, sizeof(topic),
           "homeassistant/binary_sensor/bridge_%s/eth/config", s_node);
  snprintf(payload, sizeof(payload),
    "{\"name\":\"Ethernet-Link\",\"uniq_id\":\"bridge_%s_eth\","
    "\"stat_t\":\"%s\",\"avty_t\":\"%s\","
    "\"val_tpl\":\"{{ 'ON' if value_json.eth == 1 else 'OFF' }}\","
    "\"dev_cla\":\"connectivity\","
    "\"dev\":{\"ids\":[\"bridge_%s\"],\"name\":\"%s\"}}",
    s_node, s_state, s_avail, s_node, g_cfg.name);
  esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 1);

  s_announced = true;
  printf("[MQTT] Discovery gesendet\n");
}

bool telemetry_start(void) {
  if (g_cfg.mqtt_host[0] == '\0') {
    printf("[MQTT] Kein Broker konfiguriert - uebersprungen\n");
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
  if (!s_mqtt) { printf("[MQTT] Init fehlgeschlagen\n"); return false; }

  esp_mqtt_client_register_event(s_mqtt, MQTT_EVENT_ANY, mqtt_evt, NULL);
  esp_mqtt_client_start(s_mqtt);
  printf("[MQTT] Verbinde zu %s\n", uri);
  return true;
}

void telemetry_tick(void) {
  if (!s_mqtt || !s_connected) return;

  static uint32_t last = 0;
  const uint32_t interval = (uint32_t)g_cfg.telemetry_s * 1000UL;
  if (millis() - last < interval) return;
  last = millis();

  if (!s_announced) announce_all();

  BridgeStats st;
  bridge_get_stats(&st);

  char payload[500];
  snprintf(payload, sizeof(payload),
    "{\"rssi\":%d,\"ch\":%u,\"kbps_up\":%lu,\"kbps_down\":%lu,"
    "\"pkt_up\":%lu,\"pkt_down\":%lu,\"drop_up\":%lu,\"drop_down\":%lu,"
    "\"wd_reconnects\":%lu,\"wd_eth_resets\":%lu,"
    "\"eth\":%d,\"uptime\":%lu,\"heap\":%lu,\"bssid\":\"%s\","
    "\"client_ip\":\"%s\"}",
    (int)st.rssi, (unsigned)st.channel,
    (unsigned long)st.kbps_eth2wifi, (unsigned long)st.kbps_wifi2eth,
    (unsigned long)st.pkt_eth2wifi,  (unsigned long)st.pkt_wifi2eth,
    (unsigned long)st.drop_eth2wifi, (unsigned long)st.drop_wifi2eth,
    (unsigned long)st.wd_reconnects, (unsigned long)st.wd_eth_resets,
    st.eth_link ? 1 : 0, (unsigned long)(millis() / 1000),
    (unsigned long)esp_get_free_heap_size(), st.bssid, st.client_ip);

  esp_mqtt_client_publish(s_mqtt, s_state, payload, 0, 0, 0);
}

void telemetry_note_watchdog_event(const char *action, const char *reason) {
  if (!s_mqtt || !s_connected) return;

  char topic[64];
  snprintf(topic, sizeof(topic), "%s/watchdog", s_base);

  char payload[128];
  snprintf(payload, sizeof(payload), "{\"action\":\"%s\",\"reason\":\"%s\",\"uptime\":%lu}",
           action, reason ? reason : "", (unsigned long)(millis() / 1000));

  /* qos 1, nicht retained - ein Ereignis, kein Dauerzustand. Wird bewusst
   * synchron/blockierend aus der Watchdog-Eskalation heraus aufgerufen (auch
   * kurz vor einem erzwungenen Neustart) - esp_mqtt_client_publish() selbst
   * kehrt sofort zurueck, den Versand muss der Aufrufer mit einer kurzen
   * Pause abwarten, siehe watchdog_tick() in bridge.cpp. */
  esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 0);
}
