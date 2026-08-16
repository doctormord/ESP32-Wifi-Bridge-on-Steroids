/*
 * config.h - Persistente Konfiguration (NVS)
 */
#pragma once

#include "compat.h"
#include <stdint.h>

#define CFG_MAGIC       0x42524447u   /* "BRDG" */
#define CFG_NVS_NS      "bridge"
#define CFG_NVS_KEY     "cfg"

struct BridgeConfig {
  uint32_t magic;

  char     name[32];          /* Geraetename, taucht in MQTT/HA auf         */

  char     ssid1[33];
  char     pass1[65];
  char     ssid2[33];
  char     pass2[65];

  uint8_t  mgmt_ip[4];        /* Management-IP - MUSS statisch sein         */
  uint8_t  mgmt_mask[4];
  uint8_t  mgmt_gw[4];

  char     mqtt_host[64];     /* leer = MQTT aus, 63 Zeichen nutzbar        */
  uint16_t mqtt_port;
  /* 65 statt 32/64: gefordert sind Werte von 64 Byte, und das abschliessende
   * Nullbyte will auch untergebracht sein - mit [64] waeren nur 63 Zeichen
   * nutzbar gewesen. Cloud-Broker vergeben gern Token dieser Laenge.
   * ACHTUNG: Diese Aenderung hat Feldoffsets MITTEN in der Struktur
   * verschoben und damit den Anhaenge-Kontrakt gebrochen - alte NVS-Blobs
   * waeren ab hier falsch interpretiert worden. Zulaessig war das nur, weil
   * das NVS am 2026-08-13 ohnehin geloescht wurde. Wer spaeter Feldgroessen
   * aendern will: nicht ohne Werksreset, sonst liest cfg_load() Muell. */
  char     mqtt_user[65];
  char     mqtt_pass[65];

  uint16_t telemetry_s;       /* Sendeintervall in Sekunden                 */
  bool     configured;        /* false -> Provisionierungsmodus             */

  /* --- Ab hier nur ANHAENGEN, nie dazwischenschieben. ---------------------
   * cfg_load() akzeptiert auch kuerzere Blobs aus aelteren Firmwares und
   * laesst neue Felder auf ihren Defaults. Das funktioniert nur, solange
   * neue Felder hinten angehaengt werden - sonst verschiebt sich alles.   */
  char     admin_pass[33];    /* schuetzt den OTA-Endpunkt, leer = offen    */

  /* --- Feinabstimmung (angehaengt 2026-08-11) -----------------------------
   * Durchgaengige Konvention: 0 heisst "nicht anfassen, nimm den Wert aus
   * sdkconfig bzw. den PHY-Default". Damit bleibt sdkconfig.defaults die
   * einzige Quelle fuer die Standardwerte und das Portal ueberschreibt nur
   * das, was jemand bewusst eingestellt hat. (Genau die doppelte Quelle war
   * vorher der Grund, warum Erhoehungen in sdkconfig wirkungslos blieben.) */

  /* SOFORT wirksam, ohne Neustart: */
  int8_t   tx_power;          /* esp_wifi_set_max_tx_power, 0.25-dBm-Schritte
                               * gueltig 8..84, wird intern auf 11 Stufen
                               * gerundet. 0 = PHY-Default (Maximum).       */
  uint8_t  eth_tx_retries;    /* Ethernet->WiFi, Wiederholungen bei NO_MEM  */
  uint8_t  wifi_tx_retries;   /* WiFi->Ethernet, Wiederholungen             */

  /* Brauchen einen NEUSTART (gehen in wifi_init_config_t bzw. muessen vor
   * dem Verbindungsaufbau stehen): */
  uint8_t  static_rx_buf;     /* wifi_init_config_t.static_rx_buf_num       */
  uint8_t  dynamic_rx_buf;    /* .dynamic_rx_buf_num                        */
  uint8_t  dynamic_tx_buf;    /* .dynamic_tx_buf_num - Kamera-Upload-Pfad   */
  uint8_t  rx_ba_win;         /* .rx_ba_win, muss <= static_rx_buf und
                               * <= dynamic_rx_buf/2 sein                   */
  uint8_t  ht40;              /* 1 = 40-MHz-Kanal erlauben (nur wirksam,
                               * wenn der AP das auch anbietet)             */
  uint8_t  no_11b;            /* 1 = 802.11b-Raten abschalten               */

  /* --- Zweites Netzwerkprofil (angehaengt 2026-08-13) ---------------------
   * Die Firmware kannte zwei WLANs, aber nur einen IP-Satz - beim Wechsel
   * zwischen zwei Standorten (Labor / Zuhause) musste man deshalb jedes Mal
   * umkonfigurieren. Diese Felder gehoeren zu ssid2/pass2 und werden nur
   * angewandt, wenn die Verbindung tatsaechlich ueber ssid2 zustande kam.
   *
   * mgmt_ip2 == 0.0.0.0 heisst "nicht gesetzt": dann gilt auch fuer das
   * zweite WLAN der erste IP-Satz. Damit bleiben bestehende Konfigurationen
   * unveraendert gueltig, auch die aus aelteren Firmwares mit kuerzerem
   * NVS-Blob. Maske und Gateway fallen einzeln auf den ersten Satz zurueck,
   * falls sie leer bleiben - ein halb ausgefuelltes Profil soll das Board
   * nicht unerreichbar machen. */
  uint8_t  mgmt_ip2[4];
  uint8_t  mgmt_mask2[4];
  uint8_t  mgmt_gw2[4];

  /* --- Gelernte Client-MAC (angehaengt 2026-08-14) ------------------------
   * Die Bridge kann nur starten, wenn sie die MAC des angeschlossenen Geraets
   * kennt - und die lernt sie aus dem ersten Frame. Ein MacBook plaudert
   * sofort los, eine Kamera sendet nur beim EIGENEN Hochfahren. Startete die
   * Bridge allein neu (z.B. nach einem OTA), kam nichts mehr, der Sniff lief
   * in den Timeout und das Geraet landete im Portal - unerreichbar.
   *
   * Deshalb wird eine einmal gelernte MAC hier gesichert und beim naechsten
   * Start als Rueckfall benutzt. 00:00:00:00:00:00 = noch keine gelernt. */
  uint8_t  client_mac[6];

  /* --- Drittes Netzwerkprofil (angehaengt 2026-08-15) ---------------------
   * Gleiche Konvention wie beim zweiten: leere Felder fallen einzeln auf
   * Profil 1 zurueck, eine leere IP heisst "Profil nicht belegt". */
  char     ssid3[33];
  char     pass3[65];
  uint8_t  mgmt_ip3[4];
  uint8_t  mgmt_mask3[4];
  uint8_t  mgmt_gw3[4];
};

extern BridgeConfig g_cfg;

void cfg_load(void);          /* laedt aus NVS, sonst Defaults              */
bool cfg_save(void);
void cfg_defaults(BridgeConfig *c);
bool cfg_factory_reset(void);

/* Hilfsfunktionen */
bool parse_ipv4(const char *s, uint8_t out[4]);
void format_ipv4(const uint8_t ip[4], char *out, size_t out_len);
