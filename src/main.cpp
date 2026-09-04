/*
 * ============================================================================
 *  ESP32 Wi-Fi <-> Ethernet Transparent Bridge
 *  mit Management-IP, Konfigurationsportal und MQTT-Telemetrie
 * ============================================================================
 *
 *  Zielhardware : WT32-ETH01 (ESP32-D0WD + LAN8720A)
 *  Toolchain    : PlatformIO, framework = arduino, espidf
 *
 *  ---------------------------------------------------------------------------
 *  BETRIEBSMODI
 *  ---------------------------------------------------------------------------
 *
 *  PROVISIONIERUNG  (keine SSID gespeichert, Reset-Pin gezogen,
 *                    oder Verbindungsaufbau gescheitert)
 *      -> Access Point "<name>-setup-XXXX"
 *      -> http://192.168.4.1 : Name, WLANs, Management-IP, MQTT einstellen
 *      -> Speichern loest Neustart aus
 *
 *  BRIDGE
 *      1. Ethernet starten, MAC des angeschlossenen Clients sniffen
 *      2. Promiscuous aktivieren
 *      3. WLAN-STA mit der GEKLONTEN MAC verbinden
 *      4. Management-Netif mit statischer IP anhaengen
 *      5. Datenpfad scharf schalten, Webserver + MQTT starten
 *
 *  ---------------------------------------------------------------------------
 *  WARUM ZWEI IPs AUF EINER MAC FUNKTIONIEREN
 *  ---------------------------------------------------------------------------
 *  Der WLAN-STA traegt die MAC des Clients (noetig, weil ein 802.11-STA im
 *  3-Address-Mode arbeitet und der AP nur an diese MAC ausliefert). Der
 *  Management-Stack haengt am selben Interface und damit an derselben MAC,
 *  hat aber eine eigene IP. ARP loest beide IPs auf dieselbe MAC auf - fuer
 *  Switches und APs voellig unauffaellig.
 *
 *  Die Management-IP MUSS statisch sein: ein DHCP-Server wuerde derselben
 *  MAC zweimal dieselbe Adresse zuweisen.
 * ============================================================================
 */

#include "compat.h"

#include "driver/gpio.h"
#include "esp_attr.h"

#include "config.h"
#include "bridge.h"
#include "web.h"
#include "telemetry.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "lwip/apps/netbiosns.h"

/* Pin gegen GND beim Booten -> Werksreset. Auf dem WT32-ETH01 sind die
 * meisten GPIOs von RMII belegt; IO14 ist frei und keine Strapping-Leitung. */
#define FACTORY_RESET_GPIO   14

/* Wie lange auf das erste Client-Frame gewartet wird, bevor wir aufgeben
 * und stattdessen das Portal anbieten. Ohne Client-MAC keine Bridge. */
#define MAC_SNIFF_TIMEOUT_MS 60000

static bool s_provisioning = false;
static bool s_ota_confirmed = false;

/*
 * Anforderung "starte ins Portal", die einen Software-Reset ueberlebt.
 *
 * RTC_NOINIT_ATTR heisst: bleibt bei esp_restart() erhalten, ist nach einem
 * echten Kaltstart aber undefiniert - deshalb das Magic-Wort. Genau dieses
 * Verhalten wollen wir: nach einem gescheiterten Bridge-Start soll der
 * naechste Start direkt ins Portal, ein Kaltstart es aber wieder regulaer
 * versuchen (z.B. weil der AP inzwischen wieder da ist).
 */
#define PORTAL_MAGIC 0x50525450u   /* "PRTP" */
RTC_NOINIT_ATTR static uint32_t s_portal_request;

/*
 * Schutz gegen Absturzschleifen.
 *
 * Die Portal-Sicherung oben greift nur, wenn der WLAN-Aufbau ordentlich
 * scheitert. Ein Absturz IM BETRIEB - etwa weil jemand die Puffer so hoch
 * gestellt hat, dass der Heap ausgeht - fuehrt dagegen zu Neustart, Absturz,
 * Neustart: eine Endlosschleife, aus der man ohne serielles Kabel nicht
 * herauskommt.
 *
 * Deshalb zaehlen wir Starts, die nie "stabil" erreicht haben. Nach drei
 * solchen Starts geht das Board ins Portal, wo sich die verantwortliche
 * Einstellung korrigieren laesst. Der Zaehler liegt im RTC-Speicher: Er
 * ueberlebt einen Software-Reset, aber nicht das Ziehen des Steckers - ein
 * echter Kaltstart soll wieder unbefangen probieren.
 */
#define BOOT_MAGIC   0x424f4f54u   /* "BOOT" */
#define BOOT_MAX_FEHLSTARTS 3
#define BOOT_STABIL_MS      60000  /* so lange muss es laufen, um zu zaehlen */
RTC_NOINIT_ATTR static uint32_t s_boot_magic;
RTC_NOINIT_ATTR static uint32_t s_boot_fehlstarts;
static bool s_stabil_gemeldet = false;

/*
 * Rollback-Freigabe.
 *
 * Mit CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE startet eine frisch geflashte
 * Firmware im Zustand PENDING_VERIFY. Ruft sie nicht innerhalb ihrer ersten
 * Laufzeit esp_ota_mark_app_valid_cancel_rollback(), springt der Bootloader
 * beim naechsten Start zurueck auf die alte Version.
 *
 * Als Kriterium nehmen wir bewusst NICHT "hat gebootet", sondern
 * "ist ueber das Netz erreichbar" - genau das ist die Eigenschaft, deren
 * Verlust bei einem eingebauten Board wehtut. Das gilt in beiden Modi:
 * auch ein Board, das nur das Portal anbietet, ist reparierbar.
 */
static void ota_confirm_if_pending(void) {
  if (s_ota_confirmed) return;

  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(run, &st) != ESP_OK) return;
  if (st != ESP_OTA_IMG_PENDING_VERIFY) { s_ota_confirmed = true; return; }

  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    s_ota_confirmed = true;
    printf("[OTA ] Neue Firmware bestaetigt, kein Rollback\n");
  }
}

static bool factory_reset_requested(void) {
  gpio_config_t io = {};
  io.pin_bit_mask = 1ULL << FACTORY_RESET_GPIO;
  io.mode         = GPIO_MODE_INPUT;
  io.pull_up_en   = GPIO_PULLUP_ENABLE;
  gpio_config(&io);
  delay(20);
  if (gpio_get_level((gpio_num_t)FACTORY_RESET_GPIO) != 0) return false;

  /* Gegen Wackler: muss zwei Sekunden anliegen. */
  printf("[BOOT] Reset-Pin erkannt, halte 2 s...\n");
  for (int i = 0; i < 20; i++) {
    delay(100);
    if (gpio_get_level((gpio_num_t)FACTORY_RESET_GPIO) != 0) return false;
  }
  return true;
}

static void enter_provisioning(const char *reason) {
  printf("[BOOT] Provisionierungsmodus: %s\n", reason);
  s_provisioning = true;
  portal_start_ap();
  web_start(true);
}

static void bridge_setup(void) {
  /* Kein Serial.begin noetig - die IDF-Konsole steht beim Start bereits. */
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

  /* Einmalig auswerten und sofort loeschen - sonst haengt das Board nach
   * einem einzigen Fehlversuch dauerhaft im Portal fest. */
  const bool portal_erzwungen = (s_portal_request == PORTAL_MAGIC);
  s_portal_request = 0;

  /* Fehlstarts zaehlen. Beim ersten Kaltstart ist der RTC-Speicher
   * undefiniert - daran erkennen wir ihn am Magic-Wort. */
  bool zu_viele_fehlstarts = false;

  /* Ausnahme: kam dieser Boot von einem erzwungenen Watchdog-Neustart
   * (bridge.cpp, Eskalationsstufe 2), zaehlt er NICHT als Fehlstart - das
   * Geraet ist sauber gebootet und gelaufen, hat sich nur selbst aus
   * WLAN-Gesundheitsgruenden neu gestartet. Ohne diese Ausnahme koennten
   * mehrere Watchdog-Neustarts waehrend einer laengeren WLAN-Stoerung genau
   * diesen Absturzschleifen-Schutz ausloesen, den sie gar nicht betreffen -
   * am 2026-09-04 vermutlich genau so passiert (kein Coredump vorgefunden,
   * also kein Absturz, aber die Bruecke landete trotzdem unerreichbar im
   * Portal). Muss VOR dem Kaltstart-Zweig ausgewertet werden, sonst wird das
   * einmalige Magic-Wort verschluckt, ohne je konsumiert worden zu sein. */
  const bool geplanter_neustart = bridge_consume_planned_restart();

  if (s_boot_magic != BOOT_MAGIC) {
    s_boot_magic = BOOT_MAGIC;
    s_boot_fehlstarts = 0;
  } else if (geplanter_neustart) {
    /* s_boot_fehlstarts bleibt unveraendert. */
  } else {
    s_boot_fehlstarts++;
    if (s_boot_fehlstarts >= BOOT_MAX_FEHLSTARTS) {
      zu_viele_fehlstarts = true;
      s_boot_fehlstarts = 0;      /* einmalig, sonst haengt es dauerhaft */
    }
  }

  if (factory_reset_requested()) {
    cfg_factory_reset();
    printf("[BOOT] Werksreset durchgefuehrt\n");
    enter_provisioning("Werksreset");
    return;
  }

  if (zu_viele_fehlstarts) {
    /* Mehrfach neu gestartet, ohne je stabil zu laufen. Weiterprobieren
     * hiesse, die Schleife fortzusetzen - also lieber ins Portal, wo die
     * Ursache korrigierbar ist. */
    printf("[BOOT] Mehrere Starts ohne stabilen Betrieb\n");
    enter_provisioning("wiederholte Neustarts - Einstellungen pruefen");
    return;
  }

  if (portal_erzwungen) {
    enter_provisioning("vorheriger Bridge-Start gescheitert");
    return;
  }

  if (!g_cfg.configured || g_cfg.ssid1[0] == '\0') {
    enter_provisioning("keine WLAN-Zugangsdaten gespeichert");
    return;
  }

  /* --- Bridge-Modus ------------------------------------------------------ */
  if (!bridge_eth_init()) {
    printf("[BOOT] Ethernet-Init fehlgeschlagen - Neustart in 5 s\n");
    delay(5000);
    esp_restart();
  }

  /* Client-MAC bestimmen.
   *
   * Der Sniff allein reicht nicht: Er wartet auf ein Frame des Clients, und
   * eine Kamera sendet nur, wenn SIE hochfaehrt. Startet die Bridge allein neu
   * (Reboot, OTA), kommt nichts mehr - am 2026-08-14 im Labor genau so erlebt,
   * das Geraet landete im Portal und war nicht mehr erreichbar.
   *
   * Deshalb: eine einmal gelernte MAC steht im NVS und dient als Rueckfall.
   * Der Sniff laeuft trotzdem, damit ein GERAETEWECHSEL bemerkt wird.
   *
   * Zum Fenster von 30 s bei gespeicherter MAC: Es wird nur ausgeschoepft,
   * wenn der Client WIRKLICH schweigt - sobald ein Frame kommt, geht es sofort
   * weiter. Im Normalfall kostet es also nichts. Kuerzer waere trotzdem
   * falsch: Beim Wechsel auf ein anderes Geraet muss dessen erstes Frame in
   * dieses Fenster fallen, sonst klont die Bridge die alte MAC weiter - und
   * dann kommt der Rueckweg vom AP nicht mehr an, weil dort eine andere MAC
   * assoziiert ist. Eine bootende Kamera braucht laenger als 10 s. */
  uint8_t gespeichert[6];
  memcpy(gespeichert, g_cfg.client_mac, 6);
  const bool haben_mac = (gespeichert[0] | gespeichert[1] | gespeichert[2] |
                          gespeichert[3] | gespeichert[4] | gespeichert[5]) != 0;

  if (bridge_sniff_client_mac(haben_mac ? 30000 : MAC_SNIFF_TIMEOUT_MS)) {
    uint8_t gelernt[6];
    bridge_get_client_mac(gelernt);
    if (memcmp(gelernt, g_cfg.client_mac, 6) != 0) {
      memcpy(g_cfg.client_mac, gelernt, 6);
      cfg_save();
      printf("[ETH ] Neue Client-MAC gespeichert\n");
    }
  } else if (haben_mac) {
    bridge_set_client_mac(gespeichert);
  } else {
    /* Noch nie einen Client gesehen - dann bleibt nur das Portal. */
    enter_provisioning("kein Client am Ethernet-Port erkannt");
    return;
  }

  if (!bridge_wifi_start()) {
    /* NICHT direkt ins Portal wechseln. bridge_wifi_start() hat den WiFi-
     * Stack bereits initialisiert und ein STA-Netif angelegt; portal_start_ap()
     * wuerde beides erneut anlegen, und esp_netif_create_default_wifi_sta()
     * meldet einen doppelten if_key nicht als Fehlercode, sondern per assert.
     * Auf echter Hardware ergab das am 2026-08-13 eine Boot-Schleife, in der
     * das Portal nie erschien - also genau der Zustand, aus dem man sich
     * ohne seriellen Zugang nicht mehr befreien kann.
     *
     * Statt den halben Stack von Hand zurueckzubauen (viele Fehlerquellen,
     * selbst wieder ungetestet) erzwingen wir einen sauberen Neustart. */
    printf("[BOOT] WLAN fehlgeschlagen - Neustart direkt ins Portal\n");
    s_portal_request = PORTAL_MAGIC;
    delay(200);            /* Logausgabe noch rausschreiben lassen */
    esp_restart();
  }

  bridge_activate();

  /* Die aktive Adresse erfragen, nicht g_cfg.mgmt_ip ausgeben: bei einer
   * Verbindung ueber das zweite WLAN gilt dessen IP-Satz. */
  uint8_t aktiv[4];
  bridge_get_mgmt_ip(aktiv);
  char ip[16];
  format_ipv4(aktiv, ip, sizeof(ip));
  printf("[BOOT] Management-Oberflaeche: http://%s\n", ip);

#ifdef BRIDGE_MINIMAL
  /* Weder Webserver noch MQTT - das ist der Sinn dieser Variante.
   * Ohne Management-Zugang gibt es auch keinen Weg zurueck per OTA. Den
   * liefert stattdessen der Rollback-Mechanismus: Ein frisch per OTA
   * eingespieltes Image steht auf PENDING_VERIFY, und weil bridge_loop()
   * hier NIE esp_ota_mark_app_valid_cancel_rollback() ruft, faellt der
   * Bootloader beim naechsten Start automatisch auf den vorherigen Slot
   * zurueck. Ein Stromausfall genuegt also, um die vollstaendige Firmware
   * wiederzubekommen - ohne Kabel, ohne Jumper. */
  printf("\n*** MINIMAL-VARIANTE - nur Bridge, kein Management ***\n");
  printf("*** Neustart stellt automatisch die vollstaendige Firmware her ***\n\n");
#else
  web_start(false);
  telemetry_start();
#endif
}

/*
 * NetBIOS-Namen bekanntgeben.
 *
 * Warum ueberhaupt: Die FritzBox holt Geraetenamen aus der DHCP-Anfrage
 * (Option 12). Unsere Management-Schnittstelle hat aber per Entwurf eine
 * STATISCHE IP und fragt deshalb nie - in der Geraeteliste steht darum
 * "unknown". NetBIOS ist der zweite Weg, ueber den die FritzBox Namen lernt
 * (so werden Windows-Rechner benannt), und lwIPs Responder ist in der IDF
 * bereits einkompiliert. Kein Component Manager, keine neue Abhaengigkeit.
 *
 * Der Name traegt das letzte Oktett der Client-IP, damit in der Geraeteliste
 * sichtbar wird, WEN diese Bridge gerade bruecken - z.B. WIFIBRIDGE-109.
 * Beide teilen sich zwangslaeufig eine MAC, die Liste zeigt also nur einen
 * Eintrag; so steht wenigstens dran, worum es sich handelt.
 *
 * NetBIOS erlaubt 15 Zeichen. Der Gearetename wird entsprechend gekuerzt,
 * damit das Suffix immer passt - lieber der Name knapp als die Zuordnung weg.
 */
static void netbios_namen_pflegen(void) {
  static bool  init_done = false;
  static char  gesetzt[16] = "";

  if (s_provisioning || !bridge_is_active()) return;

  if (!init_done) { netbiosns_init(); init_done = true; }

  BridgeStats st;
  bridge_get_stats(&st);

  /* Letztes Oktett der Client-IP herausziehen; "-" heisst noch unbekannt. */
  const char *punkt = strrchr(st.client_ip, '.');
  const char *suffix = (punkt && punkt[1]) ? punkt + 1 : NULL;

  char name[16];
  size_t platz = suffix ? 15 - 1 - strlen(suffix) : 15;   /* 1 fuer den Bindestrich */
  size_t o = 0;
  for (size_t i = 0; g_cfg.name[i] && o < platz; i++) {
    const char c = g_cfg.name[i];
    if      (c >= 'a' && c <= 'z') name[o++] = c - 32;    /* NetBIOS mag Grossbuchstaben */
    else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) name[o++] = c;
    else if (o) name[o++] = '-';                          /* alles andere ersetzen */
  }
  /* Trennzeichen am Ende abschneiden, sonst wird aus einem Namen wie
   * "WIFIBRIDGE_1" das doppelt getrennte "WIFIBRIDGE--109". */
  while (o && name[o - 1] == '-') o--;
  if (!o) { name[o++] = 'B'; name[o++] = 'R'; name[o++] = 'I'; name[o++] = 'D'; name[o++] = 'G'; name[o++] = 'E'; }
  if (suffix) { name[o++] = '-'; for (const char *q = suffix; *q && o < 15; q++) name[o++] = *q; }
  name[o] = '\0';

  if (strcmp(name, gesetzt) != 0) {
    netbiosns_set_name(name);
    strcpy(gesetzt, name);
    printf("[NBNS] Name: %s\n", name);
  }
}

static void bridge_loop(void) {
  /* Erst nach 30 s bestaetigen. Ein Absturz kurz nach dem Start soll noch
   * zum Rollback fuehren - deshalb nicht sofort in setup() freigeben. */
#ifndef BRIDGE_MINIMAL
  /* In der Minimal-Variante bewusst NICHT bestaetigen - siehe bridge_setup().
   * Das Ausbleiben dieser Bestaetigung IST der Rueckweg. */
  if (!s_ota_confirmed && millis() > 30000) ota_confirm_if_pending();
#endif

  /* Laeuft lange genug -> Fehlstartzaehler zuruecksetzen. Erst ab hier gilt
   * ein Start als geglueckt; ein Absturz davor zaehlt weiter hoch. */
  if (!s_stabil_gemeldet && millis() > BOOT_STABIL_MS) {
    s_stabil_gemeldet = true;
    s_boot_fehlstarts = 0;
  }

  if (!s_provisioning) {
    bridge_tick();
    netbios_namen_pflegen();
#ifndef BRIDGE_MINIMAL
    telemetry_tick();
#endif
  }
  delay(50);
}

/* ---------------------------------------------------------------------------
 * Einstiegspunkt. Ersetzt Arduinos setup()/loop() - inhaltlich identisch,
 * nur ohne den Arduino-Core dahinter.
 * ------------------------------------------------------------------------ */
extern "C" void app_main(void) {
  bridge_setup();
  for (;;) {
    bridge_loop();
  }
}
