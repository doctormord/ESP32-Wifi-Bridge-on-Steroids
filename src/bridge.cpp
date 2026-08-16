/*
 * bridge.cpp
 *
 * Datenpfad der Wi-Fi <-> Ethernet-Bridge plus Management-IP-Stack.
 *
 * Der Kniff steckt in wifi_rx_cb(): esp_wifi_internal_reg_rxcb() hat nur
 * EINEN Slot, und den belegt normalerweise lwIP. Wir nehmen ihn uns, machen
 * aber die Tuersteher-Arbeit selbst und reichen genau die Frames an lwIP
 * weiter, die an die Management-IP adressiert sind. Alles andere geht direkt
 * und ohne Umweg ueber den IP-Stack aufs Ethernet.
 */

#include "bridge.h"
#include "driver/gpio.h"
#include "config.h"

#include <string.h>

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

/* Fuer die Standorterkennung per Gateway-Ping (siehe gateway_erreichbar). */
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

/* eth_rx_cb() reicht Management-Frames per esp_netif_receive() an lwIP weiter
 * und gibt den Puffer danach sofort frei. Das ist NUR zulaessig, solange
 * wlanif_input() die Nutzdaten kopiert statt sie zu referenzieren - genau das
 * schaltet CONFIG_LWIP_L2_TO_L3_COPY. Ohne die Option waere es ein
 * Use-after-free, der sich als sporadische Abstuerze unter Last zeigen wuerde
 * und kaum auffindbar waere. Deshalb hier hart abbrechen statt hoffen. */
#ifndef CONFIG_LWIP_L2_TO_L3_COPY
#error "CONFIG_LWIP_L2_TO_L3_COPY muss gesetzt sein - siehe eth_rx_cb() in bridge.cpp"
#endif

/* --- WT32-ETH01 Pinbelegung ---------------------------------------------- */
#define ETH_PHY_ADDR        1
#define ETH_PHY_MDC_GPIO    23
#define ETH_PHY_MDIO_GPIO   18
#define ETH_PHY_PWR_GPIO    16

/*
 * Sendewiederholungen, bevor ein Frame verworfen wird.
 *
 * ACHTUNG: Der richtige Wert haengt vom VERKEHR ab, nicht vom Geraet.
 *
 *   TCP (Speedtest, Dateiuebertragung)
 *       Schnelles Verwerfen ist besser. TCP sendet selbst nach und drosselt;
 *       Warten blockiert nur den Ethernet-Empfang. Gemessen 2026-08-13:
 *       Retries 1 lieferte 12,0/4,8 Mbit, Retries 8 nur 10,4/4,0.
 *
 *   UDP (Kamera, RTP)
 *       Genau umgekehrt. Ein verworfenes Paket ist ENDGUELTIG weg und wird
 *       zum Bildfehler. Gemessen 2026-08-15 an einer Kamera mit 2 Mbit:
 *       Retries  1 -> 13,29 % Verlust
 *       Retries  8 ->  0,93 %
 *       Retries 16 ->  0,10 %
 *
 * Der Standard steht auf 8, weil das der Einsatzzweck dieses Geraets ist:
 * eine Kamera bruecken, nicht Speedtests gewinnen. Ich hatte hier zunaechst
 * 1 stehen und das ausfuehrlich mit Speedtest-Zahlen begruendet - das war
 * eine Optimierung auf den Benchmark statt auf die Anwendung.
 *
 * Nach oben ist es nicht gratis: Jede Wiederholung haelt den emac_rx-Task
 * laenger auf, und was in dieser Zeit hereinkommt, verliert der Ethernet-DMA,
 * ohne dass unsere Zaehler es sehen. Deshalb der kleinste Wert, der das Ziel
 * erreicht - genau danach sucht auch das Autotune im Portal.
 */
#define ETH_TX_RETRIES_DEF  8
/* Gegenrichtung: Ethernet nimmt praktisch immer an, drop_down ist in allen
 * Messungen 0 geblieben. Hier gibt es nichts zu gewinnen. */
#define WIFI_TX_RETRIES_DEF 1
#define WIFI_CONNECT_TIMEOUT_MS 12000

/* Die Retry-Zaehler liegen als eigene Variablen und nicht als g_cfg-Zugriff
 * im Datenpfad: eth_rx_cb/wifi_rx_cb laufen im IRAM und werden pro Paket
 * aufgerufen, da soll kein Umweg ueber die grosse Config-Struktur hinein.
 * volatile, weil das Portal sie aus einem anderen Task heraus aendert. */
static volatile uint8_t s_eth_tx_retries  = ETH_TX_RETRIES_DEF;
static volatile uint8_t s_wifi_tx_retries = WIFI_TX_RETRIES_DEF;

/* --- Zustand -------------------------------------------------------------- */
static esp_eth_handle_t s_eth        = NULL;
static esp_netif_t     *s_mgmt_netif = NULL;

static volatile bool s_eth_link      = false;
static volatile bool s_wifi_up       = false;
static volatile bool s_active        = false;
static volatile bool s_paused        = false;   /* waehrend OTA */

static uint8_t       s_client_mac[6] = {0};
static volatile bool s_mac_known     = false;

/* Management-IP als Bytes - so kann im Datenpfad direkt memcmp() laufen,
 * ohne Byte-Order-Gefummel. */
static uint8_t       s_mgmt_ip[4]    = {0};

/* IP des gebridgten Clients, im Datenpfad mitgelesen. Die Bridge arbeitet auf
 * Layer 2 und braucht diese Adresse nicht - sie ist reine Diagnose. Ohne sie
 * sieht man im Portal nur die MAC und weiss nicht, ob der Client ueberhaupt
 * eine Adresse hat, geschweige denn welche. Genau das war am 2026-08-14 beim
 * Anschluss der Kamera die offene Frage. */
static volatile uint8_t s_client_ip[4] = {0};

/* Hostname des Clients, aus seiner DHCP-Anfrage mitgelesen (Option 12).
 * Router benennen Geraete genau daraus. Steht hier nichts, sendet der Client
 * auch keinen - dann liegt es nicht am Router. Genau diese Frage war am
 * 2026-08-14 offen: die Kamera heisst laut Konfiguration "PB4", taucht im
 * AR-300 aber als "unknown" auf. */
static char s_client_name[32] = "";

static volatile uint32_t s_pkt_e2w = 0, s_pkt_w2e = 0;
static volatile uint32_t s_drop_e2w = 0, s_drop_w2e = 0;
static volatile uint64_t s_by_e2w = 0, s_by_w2e = 0;
static uint32_t s_kbps_e2w = 0, s_kbps_w2e = 0;

/* Fuer den Watchdog (siehe unten) und zur Diagnose im Portal: wie oft die
 * STA seit dem Boot die Assoziation verloren hat. Reine Reassoziierungs-
 * Haeufigkeit sagt etwas, das die Drop-Zaehler allein nicht zeigen - naemlich
 * ob der Funkkanal die Verbindung selbst wegreisst, nicht nur einzelne
 * Frames verliert. */
static volatile uint32_t s_wifi_disc_count = 0;

static char s_bssid_str[18] = "-";

/* Sendeleistung, die die PHY nach dem Start von sich aus einstellt.
 * Wird beim ersten bridge_apply_live_tuning() gemerkt, damit ein
 * zurueckgesetztes tx_power (=0) sie WIEDERHERSTELLEN kann. Ohne das blieb
 * der zuletzt gesetzte Wert stehen, und "Standard" im Portal war wirkungslos -
 * am 2026-08-15 beim Durchmessen der Sendeleistung aufgefallen. */
static int8_t s_txp_default = 0;

/* Was beim letzten esp_wifi_init() wirklich gesetzt wurde. */
static uint8_t s_eff_static_rx = CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM;
static uint8_t s_eff_dyn_rx    = CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM;
static uint8_t s_eff_dyn_tx    = CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM;
static uint8_t s_eff_ba_win    = CONFIG_ESP_WIFI_RX_BA_WIN;

static void mac2str(const uint8_t *m, char *out) {
  sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* ===========================================================================
 * Datenpfad
 * ========================================================================= */

/* Definition steht weiter unten beim Wi-Fi->Ethernet-Pfad, wird aber schon
 * hier gebraucht: beide Richtungen benutzen dieselbe Tuersteher-Pruefung. */
IRAM_ATTR static inline bool is_for_mgmt(const uint8_t *f, uint16_t len);

/*
 * Ethernet -> Wi-Fi. Laeuft im emac_rx-Task, 'buffer' gehoert uns.
 *
 * WICHTIG: Dieser Pfad darf NICHT blockieren. Standardmaessig gibt es genau
 * einen Sendeversuch; klappt der nicht, ist das Frame weg. Das wirkt falsch,
 * ist aber messbar richtig - die Begruendung steht bei ETH_TX_RETRIES_DEF.
 * Kurz: Warten haelt den Ethernet-Empfang an, und was in der Zeit hereinkommt,
 * verliert der DMA in Bursts. Das schadet TCP mehr, als der eingesparte
 * Einzelverlust nuetzt.
 */
IRAM_ATTR static esp_err_t eth_rx_cb(esp_eth_handle_t h, uint8_t *buffer,
                                     uint32_t len, void *priv) {
  (void)h; (void)priv;

  if (s_paused) { free(buffer); return ESP_OK; }   /* stillgelegt, kein Drop */

  /* Client-IP mitlesen. Hier ist der Absender immer der angeschlossene Client,
   * die Quelladresse also seine. Kostet zwei Vergleiche und vier Byte kopieren.
   * 0.0.0.0 wird verworfen - so sieht eine DHCP-Anfrage aus, bevor der Client
   * eine Adresse hat, und die waere als Anzeige irrefuehrend. */
  if (len >= 34) {
    const uint16_t et = ((uint16_t)buffer[12] << 8) | buffer[13];
    const uint8_t *src = NULL;
    if      (et == 0x0800)               src = buffer + 26;  /* IPv4-Quelle   */
    else if (et == 0x0806 && len >= 42)  src = buffer + 28;  /* ARP-Absender  */
    if (src && (src[0] | src[1] | src[2] | src[3])) {
      s_client_ip[0]=src[0]; s_client_ip[1]=src[1];
      s_client_ip[2]=src[2]; s_client_ip[3]=src[3];
    }
  }

  /* DHCP-Hostnamen mitlesen - nur solange wir noch keinen haben, damit der
   * heisse Pfad danach unbelastet bleibt. Strenge Laengenpruefungen, weil hier
   * ungeprueft Fremddaten geparst werden. */
  if (!s_client_name[0] && len >= 42 &&
      ((uint16_t)buffer[12] << 8 | buffer[13]) == 0x0800 && buffer[23] == 17) {
    const uint32_t ihl = (uint32_t)(buffer[14] & 0x0F) * 4;
    const uint32_t udp = 14 + ihl;
    if (ihl >= 20 && len >= udp + 8) {
      const uint16_t sp = (uint16_t)buffer[udp] << 8 | buffer[udp + 1];
      const uint16_t dp = (uint16_t)buffer[udp + 2] << 8 | buffer[udp + 3];
      if (sp == 68 && dp == 67) {                 /* Client -> DHCP-Server */
        uint32_t o = udp + 8 + 240;               /* fester BOOTP-Kopf + Magic */
        while (o + 1 < len) {
          const uint8_t t = buffer[o];
          if (t == 255) break;                    /* Ende der Optionen */
          if (t == 0)   { o++; continue; }        /* Fuellbyte */
          const uint8_t l = buffer[o + 1];
          if (o + 2 + l > len) break;
          if (t == 12 && l) {                     /* Option 12 = Hostname */
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

  /* Frames an die Management-IP gehoeren dem lokalen Stack, nicht ins WLAN.
   * Ohne diese Abzweigung ist die Weboberflaeche von der Client-Seite aus
   * unerreichbar - schon der ARP-Request nach der Management-IP wandert
   * hinaus ins WLAN und wird nie beantwortet. (Auf echter Hardware am
   * 2026-08-13 genau so beobachtet: Bridge lief, Oberflaeche vom
   * angeschlossenen Laptop aus tot.)
   *
   * Zur Sicherheit der Pufferuebergabe: esp_netif_receive() ist hier nur
   * deshalb erlaubt, weil CONFIG_LWIP_L2_TO_L3_COPY gesetzt ist. Dann
   * kopiert wlanif_input() die Nutzdaten in einen eigenen pbuf; ohne die
   * Option wuerde es unseren Puffer bloss referenzieren (PBUF_REF), den wir
   * eine Zeile spaeter freigeben -> Use-after-free im heissen Pfad.
   * NULL als l2_buff ist zulaessig, wifi_free() prueft darauf. */
#ifndef BRIDGE_MINIMAL
  if (len >= 14 && is_for_mgmt(buffer, (uint16_t)len)) {
    esp_netif_receive(s_mgmt_netif, buffer, len, NULL);
    free(buffer);
    return ESP_OK;
  }
#endif

  esp_err_t err = ESP_FAIL;
  if (s_active && s_wifi_up) {
    const uint8_t retries = s_eth_tx_retries;
    for (int i = 0; i < retries; i++) {
      err = esp_wifi_internal_tx(WIFI_IF_STA, buffer, (uint16_t)len);
      if (err == ESP_OK || err != ESP_ERR_NO_MEM) break;

      /* Die ersten Versuche nur die CPU abgeben statt schlafen.
       *
       * vTaskDelay(1) wartet bei 1000 Hz Tick IMMER eine volle Millisekunde,
       * auch wenn der WLAN-Sendepuffer schon nach Mikrosekunden wieder frei
       * ist. Das blockiert den emac_rx-Task, und der Ethernet-DMA puffert nur
       * rund 2,4 ms (20 Puffer x 1522 Byte bei 100 Mbit) - laengere Blockaden
       * verliert man dort still, ohne dass unsere Drop-Zaehler es sehen.
       *
       * taskYIELD() gibt an den hoeher priorisierten WLAN-Task ab, der die
       * Puffer freiraeumt, und kehrt zurueck sobald der fertig ist. Erst
       * wenn das mehrfach nichts gebracht hat, ist echtes Warten sinnvoll -
       * dann ist die Funkstrecke wirklich dicht und Nachdruecken hilft nicht.
       * Am 2026-08-13 gemessen: mit reinem vTaskDelay(1) blieben 1,5 %
       * Verlust uebrig, die sich weder durch mehr Puffer noch durch HT40
       * druecken liessen. */
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
 * Gehoert dieses Frame dem Management-Stack?
 *
 *   - IPv4 mit Ziel == Management-IP
 *   - ARP-Request/Reply mit Target-IP == Management-IP
 *
 * Bewusst NICHT enthalten: Broadcast- und Multicast-IPv4. Die gehen
 * ausschliesslich zum Client durch. Dadurch faellt mDNS/SSDP fuer die
 * Management-Schnittstelle weg - bei fester IP verschmerzbar, und es haelt
 * den heissen Pfad frei von Sonderfaellen.
 */
IRAM_ATTR static inline bool is_for_mgmt(const uint8_t *f, uint16_t len) {
  if (!s_mgmt_netif) return false;

  const uint16_t ethertype = ((uint16_t)f[12] << 8) | f[13];

  if (ethertype == 0x0800) {                 /* IPv4  */
    if (len < 34) return false;
    return memcmp(f + 30, s_mgmt_ip, 4) == 0;   /* Ziel-IP  */
  }
  if (ethertype == 0x0806) {                 /* ARP   */
    if (len < 42) return false;
    return memcmp(f + 38, s_mgmt_ip, 4) == 0;   /* Target Protocol Address */
  }
  return false;
}

/*
 * Wi-Fi -> Ethernet bzw. -> lwIP.
 *
 * 'eb' ist der interne RX-Buffer und MUSS in jedem Pfad genau einmal
 * freigegeben werden. esp_netif_receive() uebernimmt ihn selbst - dort also
 * kein eigenes free().
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
      if (err == ESP_OK) break;
      vTaskDelay(1);
    }
  }

  if (err == ESP_OK) { s_pkt_w2e = s_pkt_w2e + 1; s_by_w2e += len; }
  else               { s_drop_w2e = s_drop_w2e + 1; }

  if (eb) esp_wifi_internal_free_rx_buffer(eb);
  return ESP_OK;
}

/* Nur waehrend der Sniff-Phase aktiv. */
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

static void eth_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg; (void)base; (void)data;
  if (id == ETHERNET_EVENT_CONNECTED) {
    s_eth_link = true;  printf("[ETH ] Link up\n");
  } else if (id == ETHERNET_EVENT_DISCONNECTED) {
    s_eth_link = false; printf("[ETH ] Link down\n");
  }
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg; (void)base;

  if (id == WIFI_EVENT_STA_CONNECTED) {
    s_wifi_up = true;
    wifi_event_sta_connected_t *e = (wifi_event_sta_connected_t *)data;
    if (e) mac2str(e->bssid, s_bssid_str);
    printf("[WIFI] Verbunden (AP %s)\n", s_bssid_str);

    /* WICHTIG: erst hier registrieren. esp_netif hat sich beim Attach
     * bereits eingetragen - dieser Aufruf ueberschreibt das gezielt, damit
     * wir die Tuersteher-Rolle bekommen. */
    esp_err_t e2 = esp_wifi_internal_reg_rxcb(WIFI_IF_STA, wifi_rx_cb);
    if (e2 != ESP_OK) {
      printf("[WIFI] reg_rxcb fehlgeschlagen: %s\n", esp_err_to_name(e2));
    }

  } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
    s_wifi_up = false;
    strcpy(s_bssid_str, "-");
    s_wifi_disc_count = s_wifi_disc_count + 1;
    wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
    printf("[WIFI] Getrennt (reason %d)\n", d ? d->reason : -1);
    if (s_active) esp_wifi_connect();     /* im Betrieb dauerhaft nachfassen */
  }
}

/* ===========================================================================
 * Ethernet
 * ========================================================================= */

bool bridge_eth_init(void) {
  gpio_config_t pwr = {};
  pwr.pin_bit_mask = 1ULL << ETH_PHY_PWR_GPIO;
  pwr.mode         = GPIO_MODE_OUTPUT;
  gpio_config(&pwr);
  gpio_set_level((gpio_num_t)ETH_PHY_PWR_GPIO, 1);
  delay(200);   /* Oszillator einschwingen lassen */

  eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
  mac_cfg.rx_task_stack_size = 4096;   /* unser Callback ruft in den WiFi-Stack */

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
  if (!mac) { printf("[ETH ] MAC-Init fehlgeschlagen\n"); return false; }

  eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
  phy_cfg.phy_addr       = ETH_PHY_ADDR;
  phy_cfg.reset_gpio_num = -1;

  esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_cfg);
  if (!phy) { printf("[ETH ] PHY-Init fehlgeschlagen\n"); return false; }

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

bool bridge_sniff_client_mac(uint32_t timeout_ms) {
  esp_eth_update_input_path(s_eth, eth_sniff_cb, NULL);
  if (esp_eth_start(s_eth) != ESP_OK) {
    printf("[ETH ] Start fehlgeschlagen\n");
    return false;
  }

  printf("[ETH ] Warte auf erstes Client-Frame...\n");
  const uint32_t t0 = millis();
  uint32_t last_log = t0;

  while (!s_mac_known) {
    if (timeout_ms && (millis() - t0) > timeout_ms) {
      printf("[ETH ] Timeout - kein Client erkannt\n");
      return false;
    }
    if (millis() - last_log > 5000) {
      last_log = millis();
      printf("[ETH ] ...noch nichts. Client angeschlossen?\n");
    }
    delay(20);
  }

  char s[18]; mac2str(s_client_mac, s);
  printf("[ETH ] Client-MAC: %s\n", s);

  esp_eth_update_input_path(s_eth, NULL, NULL);
  bool promisc = true;
  esp_eth_ioctl(s_eth, ETH_CMD_S_PROMISCUOUS, &promisc);
  printf("[ETH ] Promiscuous aktiv\n");
  return true;
}

/* ===========================================================================
 * Wi-Fi + Management-Netif
 * ========================================================================= */

/* Ist ein IPv4-Feld ueberhaupt belegt? 0.0.0.0 gilt durchgaengig als
 * "nicht gesetzt" - siehe Konvention in config.h. */
static inline bool ip_gesetzt(const uint8_t a[4]) {
  return (a[0] | a[1] | a[2] | a[3]) != 0;
}

/* ---------------------------------------------------------------------------
 * Standorterkennung per Gateway-Ping
 *
 * Die naheliegende Idee, das Profil am SSID-Slot festzumachen, traegt nicht:
 * an beiden Standorten kann dasselbe WLAN stehen (hier tatsaechlich der Fall,
 * beide heissen gleich). Dann verbindet immer Slot 1 und Profil 2 kaeme nie
 * zum Zug.
 *
 * Also fragen wir das Netz selbst: Profil setzen, dessen Gateway anpingen,
 * und bei Schweigen das andere Profil probieren. Das braucht keine
 * zusaetzliche Eingabe - das Gateway steht ohnehin in der Konfiguration -
 * und ueberlebt Router- oder Mesh-Wechsel, im Gegensatz zu einer fest
 * eingetragenen BSSID.
 * ------------------------------------------------------------------------ */

static volatile bool s_ping_antwort;

static void ping_ok_cb(esp_ping_handle_t hdl, void *args) {
  (void)hdl; (void)args;
  s_ping_antwort = true;
}

static bool gateway_erreichbar(const uint8_t gw[4], uint32_t gesamt_ms) {
  if (!ip_gesetzt(gw)) return false;

  s_ping_antwort = false;

  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.count       = 4;
  cfg.interval_ms = 250;
  cfg.timeout_ms  = 400;
  cfg.data_size   = 8;      /* klein halten, wir wollen nur ein Lebenszeichen */
  /* ip_addr_t ist NICHT immer dieselbe Struktur: mit abgeschaltetem IPv6
   * (CONFIG_LWIP_IPV6=n, siehe sdkconfig.defaults) schrumpft lwIP den Typ auf
   * struct ip4_addr zusammen - dann gibt es weder .type noch die u_addr-Union.
   * Nicht verwechseln mit esp_netif_dns_info_t weiter unten, das ist
   * esp_ip_addr_t und behaelt die Union unabhaengig davon. */
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
    /* Kein Ping moeglich -> nicht faelschlich "nicht erreichbar" melden,
     * sonst wechselt die Bridge wegen eines internen Fehlers das Profil. */
    printf("[WIFI] Ping-Session fehlgeschlagen - Profil bleibt\n");
    return true;
  }
  esp_ping_start(h);

  const uint32_t t0 = millis();
  while (!s_ping_antwort && (millis() - t0) < gesamt_ms) delay(50);

  esp_ping_stop(h);
  esp_ping_delete_session(h);
  return s_ping_antwort;
}

/*
 * Den IP-Satz des tatsaechlich verbundenen Netzes auf das Management-Netif
 * legen. idx 0 = erstes WLAN, idx 1 = zweites.
 *
 * Warum das nicht einmalig vor dem Verbindungsaufbau passiert: welches der
 * beiden WLANs erreichbar ist, weiss man erst hinterher. Genau daran ist der
 * Wechsel zwischen zwei Standorten bisher gescheitert - es gab nur einen
 * IP-Satz fuer beide.
 *
 * Faellt ein Feld des zweiten Profils leer aus, gilt dafuer der Wert des
 * ersten. Ein halb ausgefuelltes Profil soll das Board erreichbar lassen,
 * statt es mit Maske 0.0.0.0 ins Nichts zu konfigurieren.
 */
/* Zugriff auf die drei Profile ohne Sonderfaelle an jeder Verwendungsstelle. */
static const uint8_t *profil_ip(size_t i)   { return i==2 ? g_cfg.mgmt_ip3   : i==1 ? g_cfg.mgmt_ip2   : g_cfg.mgmt_ip;   }
static const uint8_t *profil_mask(size_t i) { return i==2 ? g_cfg.mgmt_mask3 : i==1 ? g_cfg.mgmt_mask2 : g_cfg.mgmt_mask; }
static const uint8_t *profil_gw(size_t i)   { return i==2 ? g_cfg.mgmt_gw3   : i==1 ? g_cfg.mgmt_gw2   : g_cfg.mgmt_gw;   }

/* Profil 1 gilt immer; 2 und 3 nur, wenn sie eine IP haben. */
static bool profil_belegt(size_t i) { return i == 0 || ip_gesetzt(profil_ip(i)); }

static void mgmt_apply_ip(size_t idx) {
  if (!profil_belegt(idx)) idx = 0;
  const uint8_t *ip4  = profil_ip(idx);
  const uint8_t *mask = profil_mask(idx);
  const uint8_t *gw   = profil_gw(idx);
  /* Einzelne leere Felder erben von Profil 1 - ein halb ausgefuelltes Profil
   * soll das Board erreichbar lassen, nicht mit Maske 0.0.0.0 ins Nichts. */
  if (!ip_gesetzt(mask)) mask = g_cfg.mgmt_mask;
  if (!ip_gesetzt(gw))   gw   = g_cfg.mgmt_gw;

  /* s_mgmt_ip ist die Kopie, gegen die is_for_mgmt() im Datenpfad
   * vergleicht - die MUSS mitgezogen werden, sonst zeigt das Netif auf die
   * neue Adresse, waehrend der Tuersteher noch die alte durchlaesst. */
  memcpy(s_mgmt_ip, ip4, 4);

  esp_netif_ip_info_t ip = {};
  memcpy(&ip.ip.addr,      ip4,  4);
  memcpy(&ip.netmask.addr, mask, 4);
  memcpy(&ip.gw.addr,      gw,   4);
  if (esp_netif_set_ip_info(s_mgmt_netif, &ip) != ESP_OK) {
    printf("[WIFI] Statische IP konnte nicht gesetzt werden\n");
  }

  esp_netif_dns_info_t dns = {};
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  memcpy(&dns.ip.u_addr.ip4.addr, gw, 4);
  esp_netif_set_dns_info(s_mgmt_netif, ESP_NETIF_DNS_MAIN, &dns);

  printf("[WIFI] Profil %u: %u.%u.%u.%u / %u.%u.%u.%u  GW %u.%u.%u.%u\n",
         (unsigned)(idx + 1),
         ip4[0], ip4[1], ip4[2], ip4[3],
         mask[0], mask[1], mask[2], mask[3],
         gw[0], gw[1], gw[2], gw[3]);
}

#ifndef BRIDGE_MINIMAL
static void standort_bestimmen(size_t bevorzugt) {
  size_t reihe[3]; size_t n = 0;
  if (profil_belegt(bevorzugt)) reihe[n++] = bevorzugt;
  for (size_t k = 0; k < 3; k++)
    if (k != bevorzugt && profil_belegt(k)) reihe[n++] = k;
  if (!n) { mgmt_apply_ip(0); return; }

  /* Nur ein belegtes Profil - dann gibt es nichts zu entscheiden, und wir
   * sparen uns die Ping-Wartezeit beim Start. */
  if (n == 1) { mgmt_apply_ip(reihe[0]); return; }

  for (size_t j = 0; j < n; j++) {
    mgmt_apply_ip(reihe[j]);
    if (gateway_erreichbar(profil_gw(reihe[j]), 2500)) {
      printf("[WIFI] Gateway von Profil %u antwortet - Standort erkannt\n",
             (unsigned)(reihe[j] + 1));
      return;
    }
    printf("[WIFI] Profil %u antwortet nicht\n", (unsigned)(reihe[j] + 1));
  }
  /* Keins antwortet: auf das bevorzugte zurueck. Eine moeglicherweise
   * falsche, aber vorhersagbare Adresse ist besser als die zufaellig
   * zuletzt gesetzte. */
  printf("[WIFI] Kein Gateway antwortet - bleibe bei Profil %u\n",
         (unsigned)(reihe[0] + 1));
  mgmt_apply_ip(reihe[0]);
}
#endif

bool bridge_wifi_start(void) {
#ifdef BRIDGE_MINIMAL
  /* Messvariante: KEIN Management-Netif. Ohne s_mgmt_netif liefert
   * is_for_mgmt() sofort false, der Datenpfad reicht also alles ungeprueft
   * durch - genau wie das Rust-Original. Dient dem Nachweis, ob der
   * Management-Stack den Durchsatz kostet. */
  printf("[WIFI] MINIMAL-Variante: kein Management-Netif\n");
#else
  /* Netif anlegen und SOFORT auf statisch umstellen. Mit DHCP wuerde die
   * geklonte MAC dieselbe Adresse wie der Client bekommen - Kollision. */
  s_mgmt_netif = esp_netif_create_default_wifi_sta();
  if (!s_mgmt_netif) {
    printf("[WIFI] Netif-Erstellung fehlgeschlagen\n");
    return false;
  }
  esp_netif_dhcpc_stop(s_mgmt_netif);

  /* Vorlaeufig den ersten Satz setzen, damit das Netif nie ohne gueltige
   * Adresse dasteht. Nach erfolgreichem Verbindungsaufbau wird der Satz des
   * tatsaechlich verbundenen Netzes angewandt. */
  mgmt_apply_ip(0);
#endif

  /* WIFI_INIT_CONFIG_DEFAULT() uebernimmt static_rx_buf_num,
   * dynamic_rx_buf_num und dynamic_tx_buf_num bereits aus sdkconfig. Frueher
   * standen hier zusaetzlich feste Zahlen (16/32/32) - gemeint als doppelte
   * Absicherung, tatsaechlich haben sie die Kconfig-Werte ueberschrieben und
   * jede Erhoehung in sdkconfig.defaults wirkungslos gemacht. Deshalb raus:
   * die Puffergroessen stehen jetzt nur noch an einer Stelle. */
  wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
#if CONFIG_ESP_WIFI_AMPDU_TX_ENABLED
  wcfg.ampdu_tx_enable    = 1;
#endif
#if CONFIG_ESP_WIFI_AMPDU_RX_ENABLED
  wcfg.ampdu_rx_enable    = 1;
#endif
  /* tx_ba_win existiert in dieser IDF-/arduino-esp32-Version nicht mehr als
   * Feld in wifi_init_config_t - das TX-Block-Ack-Fenster wird hier beim
   * Verbindungsaufbau automatisch mit dem AP ausgehandelt und ist nicht
   * mehr separat vorkonfigurierbar. rx_ba_win existiert weiterhin.
   * Praktisch bedeutet das: die Kamera->WLAN-Richtung (Ethernet->WiFi, der
   * eigentliche Upload-Pfad der Kamera) haengt jetzt am AP-Verhalten statt
   * an unserer Vorgabe - meist kein Problem, aber falls der Durchsatz in
   * genau dieser Richtung schwaechelt, ist das der Punkt, an dem man
   * ansetzen muesste (z.B. ueber idf.py menuconfig mit reinem espidf-Build). */
#ifdef CONFIG_ESP_WIFI_RX_BA_WIN
  wcfg.rx_ba_win          = CONFIG_ESP_WIFI_RX_BA_WIN;
#endif

  /* Portal-Overrides. 0 heisst durchgaengig "nicht anfassen", damit der
   * sdkconfig-Wert stehen bleibt - siehe Konvention in config.h. */
  if (g_cfg.static_rx_buf)  wcfg.static_rx_buf_num  = g_cfg.static_rx_buf;
  if (g_cfg.dynamic_rx_buf) wcfg.dynamic_rx_buf_num = g_cfg.dynamic_rx_buf;
  if (g_cfg.dynamic_tx_buf) wcfg.dynamic_tx_buf_num = g_cfg.dynamic_tx_buf;
  if (g_cfg.rx_ba_win)      wcfg.rx_ba_win          = g_cfg.rx_ba_win;

  /* KEIN Herunterregeln des BA-Fensters mehr.
   *
   * Hier stand eine Schutzbegrenzung auf min(static_rx, dynamic_rx/2), nach
   * der oft zitierten Kconfig-Regel. Sie war gut gemeint und schaedlich: Bei
   * den Puffern des Rust-Originals (16/32) druckte sie dessen rx_ba_win von
   * 32 auf 16 herunter - also genau den Parameter, der die Empfangs-
   * Aggregation und damit den Download bestimmt. Das Original faehrt diese
   * Kombination seit jeher, die IDF erzwingt die Regel also nicht.
   *
   * Am 2026-08-13 im Log aufgefallen ("rx_ba_win 32 -> 16"), nachdem die
   * Meldung mehrere Messreihen lang unbemerkt mitlief und sie alle verfaelscht
   * hat. Sollte esp_wifi_init() an einem zu grossen Fenster scheitern, faengt
   * das der Fallback darunter ab - das ist der richtige Ort dafuer. */

  /* Gestufter Rueckfall statt Alles-oder-nichts.
   *
   * Vorher wurde bei einem Fehlschlag direkt auf WIFI_INIT_CONFIG_DEFAULT()
   * zurueckgegriffen - das enthaelt aber DIESELBEN Kconfig-Werte, also auch
   * dasselbe rx_ba_win. Scheiterte der Aufruf daran, scheiterte auch der
   * Rueckfall, und das ESP_ERROR_CHECK dahinter machte daraus einen Absturz.
   * Am 2026-08-13 genau so passiert: das Board kam in eine Absturzschleife und
   * wurde vom Bootloader zurueckgerollt.
   *
   * Jetzt wird zuerst der wahrscheinliche Uebeltaeter entschaerft (das
   * BA-Fenster), bevor komplett aufgegeben wird - und jede Stufe sagt im Log,
   * dass sie gegriffen hat. Ein stiller Rueckfall waere hier besonders
   * heimtueckisch, weil er wie eine wirkungslose Einstellung aussieht. */
  /* Fuer die Anzeige merken, was tatsaechlich in den Treiber ging. */
  s_eff_static_rx = wcfg.static_rx_buf_num;
  s_eff_dyn_rx    = wcfg.dynamic_rx_buf_num;
  s_eff_dyn_tx    = wcfg.dynamic_tx_buf_num;
  s_eff_ba_win    = wcfg.rx_ba_win;

  esp_err_t werr = esp_wifi_init(&wcfg);
  if (werr != ESP_OK) {
    printf("[WIFI] init mit rx_ba_win=%d fehlgeschlagen (%s) - reduziere\n",
           wcfg.rx_ba_win, esp_err_to_name(werr));
    uint16_t cap = wcfg.static_rx_buf_num;
    if (wcfg.dynamic_rx_buf_num / 2 < cap) cap = wcfg.dynamic_rx_buf_num / 2;
    if (cap < 2) cap = 2;
    wcfg.rx_ba_win = cap;
    werr = esp_wifi_init(&wcfg);
    if (werr == ESP_OK) {
      printf("[WIFI] init ok mit rx_ba_win=%u\n", (unsigned)cap);
    }
  }
  if (werr != ESP_OK) {
    printf("[WIFI] auch das schlug fehl (%s) - komplette Defaults\n",
           esp_err_to_name(werr));
    wifi_init_config_t fb = WIFI_INIT_CONFIG_DEFAULT();
    fb.rx_ba_win = 6;          /* konservativ, passt zu jeder Pufferzahl */
    ESP_ERROR_CHECK(esp_wifi_init(&fb));
  }

  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));   /* Durchsatz vor Strom */

  /* 802.11b abschaltbar: die b-Raten sind langsam und belegen als
   * Basic-Rate unnoetig Sendezeit. Nur sinnvoll, wenn im Netz kein reines
   * b-Geraet mehr haengt - sonst sieht der AP uns evtl. gar nicht mehr. */
  esp_wifi_set_protocol(WIFI_IF_STA,
      g_cfg.no_11b ? (WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N)
                   : (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));

  /* HT40 verdoppelt die Kanalbreite und damit die Brutto-Datenrate, greift
   * aber nur, wenn der AP auf 2,4 GHz tatsaechlich 40 MHz anbietet - viele
   * tun das aus Koexistenzgruenden nicht. Ohne AP-Unterstuetzung bleibt es
   * folgenlos bei HT20, das ist kein Fehlerfall. */
  esp_err_t bw = esp_wifi_set_bandwidth(
      WIFI_IF_STA, g_cfg.ht40 ? WIFI_BW_HT40 : WIFI_BW_HT20);
  if (bw != ESP_OK) {
    printf("[WIFI] set_bandwidth: %s\n", esp_err_to_name(bw));
  }

  /* Nach den esp_netif-Handlern registrieren - Reihenfolge ist relevant. */
  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                      &wifi_evt, NULL, NULL);

  struct { const char *ssid; const char *pass; } nets[] = {
    { g_cfg.ssid1, g_cfg.pass1 },
    { g_cfg.ssid2, g_cfg.pass2 },
    { g_cfg.ssid3, g_cfg.pass3 },
  };

  for (size_t i = 0; i < 3; i++) {
    if (nets[i].ssid[0] == '\0') continue;
    printf("[WIFI] Verbinde mit '%s'...\n", nets[i].ssid);

    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid,     nets[i].ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, nets[i].pass, sizeof(wc.sta.password) - 1);
    wc.sta.pmf_cfg.capable = true;

    /* Den STAERKSTEN Accesspoint dieser SSID nehmen, nicht den erstbesten.
     *
     * Voreingestellt ist WIFI_FAST_SCAN: die IDF verbindet mit dem ersten
     * passenden AP, den sie findet, und bricht die Suche dann ab. In einem
     * Mesh mit mehreren Knoten unter derselben SSID ist das ein Gluecksspiel -
     * und ein ESP32 wechselt danach nicht mehr von selbst.
     *
     * Am 2026-08-15 am Einbauort gemessen: die Bridge hing auf einem Knoten
     * mit -82 dBm, waehrend derselbe SSID-Name auf einem anderen mit -63 dBm
     * verfuegbar war. Folge waren 16 % Paketverlust und statt 8 Mbit nur rund
     * 1 - die Kamera kam nicht durch.
     *
     * ALL_CHANNEL_SCAN kostet beim Verbinden ein paar Sekunden mehr, weil
     * wirklich alle Kanaele abgesucht werden. Das ist einmal pro Start. */
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    /* Reihenfolge: set_config, DANN set_mac, DANN start. */
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, s_client_mac);
    if (err != ESP_OK) {
      printf("[WIFI] set_mac: %s\n", esp_err_to_name(err));
      return false;
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    /* TX-Power erst NACH esp_wifi_start() - vorher liefert die API
     * ESP_ERR_WIFI_NOT_STARTED. Deshalb steht der Aufruf in der Schleife und
     * nicht weiter oben bei den uebrigen Einstellungen. */
    bridge_apply_live_tuning();

    esp_wifi_connect();

    uint32_t t0 = millis();
    while (!s_wifi_up && (millis() - t0) < WIFI_CONNECT_TIMEOUT_MS) delay(100);
    if (s_wifi_up) {
      /* Verbunden - aber an WELCHEM Standort? Der SSID-Slot verraet es nicht,
       * wenn mehrere Orte dasselbe WLAN heissen. Also die belegten Profile
       * der Reihe nach durchprobieren und ihr Gateway anpingen; begonnen wird
       * mit dem Profil des verbundenen Slots, weil das am wahrscheinlichsten
       * passt. */
#ifndef BRIDGE_MINIMAL
      standort_bestimmen(i);
#endif
      return true;
    }

    printf("[WIFI] '%s' fehlgeschlagen\n", nets[i].ssid);
    esp_wifi_disconnect();
    esp_wifi_stop();
  }
  return false;
}

void bridge_activate(void) {
  esp_eth_update_input_path(s_eth, eth_rx_cb, NULL);
  s_active = true;
  printf("[BRDG] Datenpfad aktiv\n");
}

bool bridge_is_active(void) { return s_active; }
esp_netif_t *bridge_mgmt_netif(void) { return s_mgmt_netif; }
void bridge_get_mgmt_ip(uint8_t out[4]) { memcpy(out, s_mgmt_ip, 4); }

void bridge_get_defaults(bridge_tuning_t *o) {
  o->static_rx    = CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM;
  o->dyn_rx       = CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM;
  o->dyn_tx       = CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM;
  o->ba_win       = CONFIG_ESP_WIFI_RX_BA_WIN;
  o->eth_retries  = ETH_TX_RETRIES_DEF;
  o->wifi_retries = WIFI_TX_RETRIES_DEF;
}

void bridge_get_effective(bridge_tuning_t *o) {
  o->static_rx    = s_eff_static_rx;
  o->dyn_rx       = s_eff_dyn_rx;
  o->dyn_tx       = s_eff_dyn_tx;
  o->ba_win       = s_eff_ba_win;
  o->eth_retries  = s_eth_tx_retries;
  o->wifi_retries = s_wifi_tx_retries;
}

void bridge_get_client_mac(uint8_t out[6]) { memcpy(out, s_client_mac, 6); }

void bridge_set_client_mac(const uint8_t mac[6]) {
  memcpy(s_client_mac, mac, 6);
  s_mac_known = true;
  char t[18]; mac2str(s_client_mac, t);
  printf("[ETH ] Client-MAC aus dem Speicher: %s\n", t);

  /* Denselben Abschluss nachholen, den bridge_sniff_client_mac() nach einem
   * Treffer macht - bei einem Timeout kehrt die dort naemlich vorher zurueck:
   * Sniff-Callback abhaengen und Promiscuous einschalten. Ohne das laefe die
   * Bridge mit installiertem Sniff-Callback und ohne Promiscuous, wuerde also
   * nur Broadcasts durchreichen und praktisch nichts transportieren. */
  esp_eth_update_input_path(s_eth, NULL, NULL);
  bool promisc = true;
  esp_eth_ioctl(s_eth, ETH_CMD_S_PROMISCUOUS, &promisc);
  printf("[ETH ] Promiscuous aktiv\n");
}

/* ===========================================================================
 * Feinabstimmung zur Laufzeit
 * ========================================================================= */

void bridge_apply_live_tuning(void) {
  /* Retry-Zaehler: 0 bedeutet hier NICHT "nie senden", sondern "Default".
   * Ein echtes 0 waere ein stiller Totalausfall des Datenpfads - deshalb
   * faengt der Fallback das ab, statt sich auf die Portal-Pruefung zu
   * verlassen. */
  s_eth_tx_retries  = g_cfg.eth_tx_retries  ? g_cfg.eth_tx_retries  : ETH_TX_RETRIES_DEF;
  s_wifi_tx_retries = g_cfg.wifi_tx_retries ? g_cfg.wifi_tx_retries : WIFI_TX_RETRIES_DEF;

  /* Beim ersten Aufruf den Ausgangswert sichern. */
  if (!s_txp_default) {
    int8_t p = 0;
    if (esp_wifi_get_max_tx_power(&p) == ESP_OK && p > 0) s_txp_default = p;
  }

  if (!g_cfg.tx_power) {
    /* 0 heisst "Standard" - und das muss auch aktiv zurueckstellen, nicht
     * bloss nichts tun. */
    if (s_txp_default) {
      esp_wifi_set_max_tx_power(s_txp_default);
      printf("[WIFI] TX-Power auf Standard zurueck (%d.%02d dBm)\n",
             s_txp_default / 4, (s_txp_default % 4) * 25);
    }
  } else {
    esp_err_t e = esp_wifi_set_max_tx_power(g_cfg.tx_power);
    if (e != ESP_OK) {
      printf("[WIFI] set_max_tx_power(%d): %s\n",
             (int)g_cfg.tx_power, esp_err_to_name(e));
    } else {
      /* Den tatsaechlich uebernommenen Wert ausgeben, nicht den gewuenschten:
       * die PHY kennt nur 11 Stufen und rundet ab. */
      int8_t actual = 0;
      if (esp_wifi_get_max_tx_power(&actual) == ESP_OK) {
        printf("[WIFI] TX-Power: %d.%02d dBm (angefordert %d.%02d)\n",
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

void bridge_set_paused(bool paused) {
  /* Nur das Weiterleiten aussetzen, nicht die Treiber stoppen. Die
   * Management-Verbindung (und damit der laufende Upload) bleibt bestehen,
   * weil die Selektivzustellung an lwIP unabhaengig von s_active ist. */
  s_paused = paused;
  printf("[BRDG] Datenpfad %s\n", paused ? "pausiert" : "aktiv");
}

/* ===========================================================================
 * Autotune der Sendewiederholungen
 * ========================================================================= */

static const uint8_t AT_KANDIDATEN[] = { 1, 2, 4, 6, 8, 12, 16, 20 };
#define AT_N            (sizeof(AT_KANDIDATEN) / sizeof(AT_KANDIDATEN[0]))
#define AT_EINSCHWINGEN 4000u    /* Puffer leerlaufen lassen              */
#define AT_MESSDAUER   15000u    /* danach so lange zaehlen               */
#define AT_MIN_PAKETE    400u    /* darunter ist die Messung wertlos      */
#define AT_ZIEL_PROMILLE  10u    /* 0,10 % - erreicht? dann reicht es     */

static autotune_state_t s_at_state = AT_IDLE;
static uint8_t   s_at_idx, s_at_ergebnis;
static uint32_t  s_at_t0, s_at_pkt0, s_at_drop0;
static bool      s_at_messend;
static uint16_t  s_at_verlust[AT_N];
static uint8_t   s_at_alt;               /* Wert vor dem Lauf, fuer Abbruch */

bool bridge_autotune_start(void) {
  if (!s_active || s_at_state == AT_LAEUFT) return false;
  s_at_alt   = s_eth_tx_retries;
  s_at_idx   = 0;
  s_at_state = AT_LAEUFT;
  s_at_messend = false;
  s_at_t0    = millis();
  for (size_t i = 0; i < AT_N; i++) s_at_verlust[i] = 0xFFFF;
  s_eth_tx_retries = AT_KANDIDATEN[0];
  printf("[AUTO] Start - %u Kandidaten, je %u s\n",
         (unsigned)AT_N, (unsigned)((AT_EINSCHWINGEN + AT_MESSDAUER) / 1000));
  return true;
}

autotune_state_t bridge_autotune_state(void)  { return s_at_state; }
uint8_t bridge_autotune_schritt(void)         { return s_at_idx; }
uint8_t bridge_autotune_anzahl(void)          { return AT_N; }
uint8_t bridge_autotune_ergebnis(void)        { return s_at_ergebnis; }
void bridge_autotune_werte(uint8_t *k, uint16_t *v) {
  for (size_t i = 0; i < AT_N; i++) { k[i] = AT_KANDIDATEN[i]; v[i] = s_at_verlust[i]; }
}

/* Wird aus bridge_tick() gerufen, also etwa im Sekundentakt. */
static void autotune_tick(void) {
  if (s_at_state != AT_LAEUFT) return;
  const uint32_t seit = millis() - s_at_t0;

  if (!s_at_messend) {
    if (seit < AT_EINSCHWINGEN) return;
    s_at_pkt0 = s_pkt_e2w; s_at_drop0 = s_drop_e2w;
    s_at_messend = true;
    return;
  }
  if (seit < AT_EINSCHWINGEN + AT_MESSDAUER) return;

  const uint32_t dp = s_pkt_e2w  - s_at_pkt0;
  const uint32_t dd = s_drop_e2w - s_at_drop0;

  if (dp + dd < AT_MIN_PAKETE) {
    /* Ohne nennenswerten Verkehr ist jede Zahl Zufall - lieber ehrlich
     * abbrechen als ein Ergebnis vortaeuschen. */
    printf("[AUTO] Abbruch: nur %u Pakete in %u s - zu wenig Verkehr\n",
           (unsigned)(dp + dd), (unsigned)(AT_MESSDAUER / 1000));
    s_eth_tx_retries = s_at_alt;
    s_at_state = AT_ZU_WENIG_VERKEHR;
    return;
  }

  s_at_verlust[s_at_idx] = (uint16_t)((10000ULL * dd) / (dp + dd));
  printf("[AUTO] Retries %2u -> %u.%02u %% Verlust\n",
         (unsigned)AT_KANDIDATEN[s_at_idx],
         (unsigned)(s_at_verlust[s_at_idx] / 100),
         (unsigned)(s_at_verlust[s_at_idx] % 100));

  if (++s_at_idx < AT_N) {
    s_eth_tx_retries = AT_KANDIDATEN[s_at_idx];
    s_at_messend = false;
    s_at_t0 = millis();
    return;
  }

  /* Auswertung: kleinster Kandidat, der das Ziel erreicht. Verfehlen alle
   * das Ziel, nehmen wir den mit dem geringsten Verlust. */
  uint8_t  gewaehlt = 0;
  uint16_t bestes   = 0xFFFF;
  for (size_t i = 0; i < AT_N; i++) {
    if (s_at_verlust[i] <= AT_ZIEL_PROMILLE) { gewaehlt = AT_KANDIDATEN[i]; break; }
    if (s_at_verlust[i] < bestes) { bestes = s_at_verlust[i]; gewaehlt = AT_KANDIDATEN[i]; }
  }
  s_at_ergebnis = gewaehlt;
  s_eth_tx_retries = gewaehlt;
  g_cfg.eth_tx_retries = gewaehlt;
  cfg_save();
  s_at_state = AT_FERTIG;
  printf("[AUTO] Fertig - gewaehlt: %u (gespeichert)\n", (unsigned)gewaehlt);
}

/* ===========================================================================
 * Watchdog
 *
 * Ausgangspunkt war ein RF-Stoertest (2,4-GHz-Jammer, ca. 1 Minute aktiv):
 * die STA blieb laut Treiber verbunden (wifi_up=true, RSSI unauffaellig),
 * aber die Bruecke stellte kaum noch Pakete zu - Stunden spaeter noch, bis
 * ein manueller Reboot den Zustand geloest hat. Weder RSSI noch wifi_up
 * haetten das erkannt; die einzigen Groessen, die den Einbruch zeigten,
 * waren die Verlustrate im LAN->WLAN-Pfad unter Last und (waere es dazu
 * gekommen) gehaeufte Reassoziierungen. Beides wird hier pro Zeitfenster
 * bewertet, mehrere schlechte Fenster hintereinander loesen einen Neustart
 * aus. Bewusst NICHT erkannt wird ein Stillstand ganz ohne Verkehr (zu wenig
 * Pakete fuer eine Aussage) - das waeren aktive Sondierungen wert, die diese
 * Erweiterung (noch) nicht macht.
 * ========================================================================= */

#define WD_WINDOW_MS           10000u  /* Messfenster                        */
#define WD_MIN_PAKETE             200u  /* darunter keine Aussage moeglich    */
#define WD_VERLUST_PROMILLE      150u  /* 15% Verlust im Fenster = "schlecht" */
#define WD_DISC_JE_FENSTER          4  /* so viele Reconnects im Fenster = "schlecht" */
#define WD_SCHLECHTE_FENSTER        3  /* so oft hintereinander -> Neustart  */

static uint32_t s_wd_t0 = 0, s_wd_pkt0 = 0, s_wd_drop0 = 0, s_wd_disc0 = 0;
static uint8_t  s_wd_bad = 0;

static void watchdog_tick(void) {
  /* Waehrend OTA (s_paused) oder ausserhalb des Bridge-Betriebs keine
   * Bewertung - ein Neustart mitten im Firmware-Flash waere fatal, und ohne
   * aktiven Datenpfad sind die Zaehler ohnehin bedeutungslos. */
  if (!g_cfg.wd_enable || !s_active || s_paused) {
    s_wd_bad = 0; s_wd_t0 = 0;
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

  bool schlecht = false;
  if (dp + dd >= WD_MIN_PAKETE) {
    const uint32_t promille = (1000ULL * dd) / (dp + dd);
    if (promille >= WD_VERLUST_PROMILLE) schlecht = true;
  }
  if (ddisc >= WD_DISC_JE_FENSTER) schlecht = true;

  s_wd_bad = schlecht ? (uint8_t)(s_wd_bad + 1) : 0;
  printf("[WD  ] Fenster: %u Pakete, %u Verlust, %u Reconnects -> %s (%u/%u)\n",
         (unsigned)(dp + dd), (unsigned)dd, (unsigned)ddisc,
         schlecht ? "schlecht" : "ok", (unsigned)s_wd_bad, (unsigned)WD_SCHLECHTE_FENSTER);

  if (s_wd_bad >= WD_SCHLECHTE_FENSTER) {
    printf("[WD  ] %u schlechte Fenster in Folge - Neustart\n", (unsigned)s_wd_bad);
    esp_restart();
  }
}

/* ===========================================================================
 * Statistik
 * ========================================================================= */

void bridge_tick(void) {
  autotune_tick();
  watchdog_tick();

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

void bridge_get_stats(BridgeStats *o) {
  memset(o, 0, sizeof(*o));
  o->pkt_eth2wifi  = s_pkt_e2w;
  o->pkt_wifi2eth  = s_pkt_w2e;
  o->drop_eth2wifi = s_drop_e2w;
  o->drop_wifi2eth = s_drop_w2e;
  o->kbps_eth2wifi = s_kbps_e2w;
  o->kbps_wifi2eth = s_kbps_w2e;
  o->wifi_disc_count = s_wifi_disc_count;
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
    /* Bei mehreren konfigurierten WLANs war bisher nirgends ablesbar, ueber
     * welches die Bridge tatsaechlich haengt - die BSSID allein sagt das
     * niemandem. */
    strncpy(o->ssid, (const char *)ap.ssid, sizeof(o->ssid) - 1);
    o->ssid[sizeof(o->ssid) - 1] = '\0';
  }
}
