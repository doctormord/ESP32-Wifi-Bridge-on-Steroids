/*
 * bridge.h - Ethernet/WiFi-Datenpfad und Management-Netif
 */
#pragma once

#include "compat.h"
#include "esp_eth.h"
#include "esp_netif.h"

struct BridgeStats {
  uint32_t pkt_eth2wifi;
  uint32_t pkt_wifi2eth;
  uint32_t drop_eth2wifi;
  uint32_t drop_wifi2eth;
  uint32_t kbps_eth2wifi;     /* gleitend, alle 1 s aktualisiert */
  uint32_t kbps_wifi2eth;
  uint32_t wifi_disc_count;   /* WIFI_EVENT_STA_DISCONNECTED seit Boot, fuer
                               * den Watchdog und zur Diagnose im Portal    */
  uint8_t  wd_probe;          /* Ergebnis der letzten Watchdog-Gateway-Sonde:
                               * 0=nicht noetig/aus, 1=ok, 2=fehlgeschlagen  */
  uint32_t wd_reconnects;     /* Watchdog-ausgeloeste WLAN-Reconnects seit Boot
                               * (Eskalationsstufe 1, siehe watchdog_tick())  */
  uint8_t  wd_last_reason;    /* Grund des letzten erzwungenen Watchdog-Neustarts
                               * (Eskalationsstufe 2): 0=keiner seit Kaltstart,
                               * 1=Verlustquote, 2=Reconnects, 3=Gateway-Sonde.
                               * Bleibt nach dem Neustart stehen (RTC-Speicher),
                               * damit Portal/MQTT zeigen koennen WARUM.       */
  int8_t   rssi;
  uint8_t  channel;
  bool     eth_link;
  bool     wifi_up;
  char     bssid[18];
  char     ssid[33];          /* SSID des tatsaechlich verbundenen Netzes -
                               * bei mehreren konfigurierten WLANs sonst
                               * nicht erkennbar, welches gerade traegt   */
  char     client_mac[18];
  char     client_ip[16];     /* im Datenpfad mitgelesen, "-" wenn unbekannt */
  char     client_name[32];   /* Hostname aus der DHCP-Anfrage (Option 12)  */
};

/* Ethernet initialisieren (RMII, ohne Netif). */
bool bridge_eth_init(void);

/* Ethernet starten und auf das erste Client-Frame warten.
 * timeout_ms = 0 -> unbegrenzt. Liefert false bei Timeout. */
bool bridge_sniff_client_mac(uint32_t timeout_ms);

/* WiFi im STA-Modus mit geklonter MAC hochfahren, inkl. Management-Netif
 * mit statischer IP. Probiert beide konfigurierten SSIDs durch. */
bool bridge_wifi_start(void);

/* Datenpfad scharf schalten (erst nach erfolgreichem Connect aufrufen). */
void bridge_activate(void);

/* Muss regelmaessig aus loop() gerufen werden - berechnet die Datenraten. */
void bridge_tick(void);

void bridge_get_stats(BridgeStats *out);

/* Einmalig auswertbar (loescht sich beim Lesen, wie main.cpp's PORTAL_MAGIC):
 * true, wenn der GERADE laufende Boot von einem erzwungenen Watchdog-Neustart
 * kommt. main.cpp ruft das ganz am Anfang von bridge_setup() auf, um so einen
 * Neustart NICHT als Fehlstart zu zaehlen - das Geraet ist ja sauber gebootet
 * und gelaufen, hat sich nur selbst neu gestartet. Ohne diese Ausnahme koennten
 * mehrere Watchdog-Neustarts waehrend einer laengeren WLAN-Stoerung den
 * Absturzschleifen-Schutz ausloesen, den sie gar nicht betreffen. */
bool bridge_consume_planned_restart(void);

esp_netif_t *bridge_mgmt_netif(void);
bool         bridge_is_active(void);

/* Die TATSAECHLICH aktive Management-IP. Kann von g_cfg.mgmt_ip abweichen,
 * seit es zwei Netzwerkprofile gibt - bei einer Verbindung ueber das zweite
 * WLAN gilt dessen IP-Satz. Wer die Adresse ausgibt, muss diese hier nehmen,
 * sonst nennt der Boot-Log eine Adresse, unter der nichts erreichbar ist. */
void bridge_get_mgmt_ip(uint8_t out[4]);

/* Die gelernte Client-MAC auslesen bzw. von aussen vorgeben. Vorgeben ist der
 * Rueckfall, wenn der Sniff nichts gefunden hat, aber eine MAC gespeichert
 * ist - siehe config.h. Muss VOR bridge_wifi_start() geschehen, weil die MAC
 * dort auf den WLAN-Adapter geklont wird. */
/* Die sechs Zahlenwerte der Feinabstimmung.
 *
 * In der Konfiguration heisst 0 "nimm den Firmware-Wert" - im Portal ist das
 * aber unverstaendlich, weil man dann keinen Wert UNTER dem Standard eintragen
 * koennte, ohne dass die Null widerspruechlich wird. Deshalb liefert die API
 * die TATSAECHLICH wirksamen Werte, und daneben die Standardwerte fuer den
 * "Standardwerte"-Knopf. */
typedef struct {
  uint8_t static_rx, dyn_rx, dyn_tx, ba_win, eth_retries, wifi_retries;
} bridge_tuning_t;

void bridge_get_defaults(bridge_tuning_t *out);    /* Firmware-Vorgaben     */
void bridge_get_effective(bridge_tuning_t *out);   /* was gerade laeuft     */

void bridge_get_client_mac(uint8_t out[6]);
void bridge_set_client_mac(const uint8_t mac[6]);

/* Datenpfad waehrend eines Flash-Schreibvorgangs (OTA) stilllegen.
 * Beim Schreiben ins Flash wird der Cache kurzzeitig abgeschaltet; das
 * vertraegt sich schlecht mit einem Datenpfad unter Volllast. */
void bridge_set_paused(bool paused);

/* Die sofort wirksamen Feinabstimmungswerte aus g_cfg uebernehmen
 * (TX-Power und die beiden Retry-Zaehler). Wird nach dem Speichern im
 * Portal gerufen, damit man ohne Neustart messen kann.
 * Die uebrigen Tuning-Felder (Puffer, BA-Fenster, HT40, 11b) greifen erst
 * beim naechsten Start - sie stecken in wifi_init_config_t bzw. muessen vor
 * dem Verbindungsaufbau gesetzt sein. */
void bridge_apply_live_tuning(void);

/* ---------------------------------------------------------------------------
 * Autotune fuer die Sendewiederholungen
 *
 * Probiert mehrere Werte fuer eth_tx_retries durch und misst zu jedem die
 * Verlustrate. Gewaehlt wird der KLEINSTE Wert, der das Verlustziel einhaelt -
 * nicht der mit dem besten Ergebnis. Grund: Jede Wiederholung blockiert den
 * Ethernet-Empfangstask ein Stueck laenger, und was in dieser Zeit hereinkommt,
 * verliert der DMA unsichtbar. Mehr Geduld ist also nicht kostenlos.
 *
 * Braucht LAUFENDEN VERKEHR, sonst misst es nichts. Laeuft im Hintergrund
 * ueber bridge_tick(), blockiert also weder HTTP noch Datenpfad.
 * ------------------------------------------------------------------------ */
typedef enum { AT_IDLE = 0, AT_LAEUFT, AT_FERTIG, AT_ZU_WENIG_VERKEHR } autotune_state_t;

bool             bridge_autotune_start(void);
autotune_state_t bridge_autotune_state(void);
uint8_t          bridge_autotune_schritt(void);   /* aktueller Kandidat 0..n */
uint8_t          bridge_autotune_anzahl(void);
uint8_t          bridge_autotune_ergebnis(void);  /* gewaehlter Wert, 0 = keiner */
/* Verlust je Kandidat in Hundertstelprozent, 0xFFFF = nicht gemessen */
void             bridge_autotune_werte(uint8_t *kandidaten, uint16_t *verlust);

/* Tatsaechlich wirksame TX-Power in 0.25-dBm-Einheiten, direkt vom Treiber
 * erfragt. Weicht in aller Regel vom eingestellten Wert ab, weil die PHY nur
 * 11 diskrete Stufen kennt und abrundet. <0 = nicht ermittelbar. */
int16_t bridge_get_tx_power(void);

/* Tatsaechlich ausgehandelte Kanalbreite in MHz (20 oder 40), direkt vom
 * Treiber. Der ht40-Schalter in der Konfiguration sagt nur, was wir ANBIETEN -
 * ob der Accesspoint mitmacht, steht auf einem anderen Blatt. Ohne diese
 * Anzeige laesst sich nicht unterscheiden, ob HT40 nichts brachte oder gar
 * nicht zustande kam. 0 = nicht ermittelbar. */
uint8_t bridge_get_bandwidth_mhz(void);
