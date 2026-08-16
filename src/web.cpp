/*
 * web.cpp - Konfigurationsportal + Status-API
 */

#include "web.h"
#include "config.h"
#include "bridge.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"

#define AP_PASSWORD   "bridgesetup"   /* mind. 8 Zeichen, sonst offen */

static httpd_handle_t s_httpd = NULL;
static bool           s_provisioning = false;

/* ===========================================================================
 * Access Point
 * ========================================================================= */

bool portal_start_ap(void) {
  /* Idempotent halten. esp_netif_create_default_wifi_*() quittiert einen
   * bereits vergebenen if_key nicht mit einem Fehlercode, sondern mit einem
   * assert - also einem Absturz. Das Portal ist aber der Rettungsanker eines
   * Geraets ohne seriellen Zugang; hier darf es unter keinen Umstaenden
   * knallen, egal ueber welchen Pfad wir hereinkommen.
   * (main.cpp startet nach einem gescheiterten Bridge-Versuch inzwischen neu,
   * damit dieser Fall gar nicht erst auftritt - diese Pruefung ist die
   * zweite Verteidigungslinie.) */
  if (!esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"))  esp_netif_create_default_wifi_ap();
  /* STA zusaetzlich, damit der Scan im Portal funktioniert. */
  if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t ie = esp_wifi_init(&cfg);
  if (ie != ESP_OK) {
    /* Bereits initialisiert ist hier kein Grund aufzugeben - weitermachen
     * und schauen, ob sich der AP trotzdem konfigurieren laesst. */
    printf("[PORT] esp_wifi_init: %s (vermutlich schon initialisiert)\n",
           esp_err_to_name(ie));
  }
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

  wifi_config_t ap = {};
  snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s-setup-%02X%02X",
           g_cfg.name[0] ? g_cfg.name : "bridge", mac[4], mac[5]);
  ap.ap.ssid_len       = strlen((char *)ap.ap.ssid);
  ap.ap.channel        = 1;
  ap.ap.max_connection = 2;
  ap.ap.authmode       = WIFI_AUTH_WPA2_PSK;
  strcpy((char *)ap.ap.password, AP_PASSWORD);

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
  ESP_ERROR_CHECK(esp_wifi_start());

  printf("\n[PORT] Setup-WLAN: '%s'  Passwort: '%s'\n",
                (char *)ap.ap.ssid, AP_PASSWORD);
  printf("[PORT] Danach http://192.168.4.1 aufrufen\n\n");
  return true;
}

/* ===========================================================================
 * Hilfsfunktionen
 * ========================================================================= */

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void url_decode(char *s) {
  char *w = s;
  for (char *r = s; *r; r++) {
    if (*r == '+') {
      *w++ = ' ';
    } else if (*r == '%' && hexval(r[1]) >= 0 && hexval(r[2]) >= 0) {
      *w++ = (char)((hexval(r[1]) << 4) | hexval(r[2]));
      r += 2;
    } else {
      *w++ = *r;
    }
  }
  *w = '\0';
}

/*
 * Sucht key im form-urlencoded Body. Erst auf '&' und '=' trennen, dann
 * dekodieren - andersherum wuerde ein '&' im Passwort das Parsing zerlegen.
 */
static bool form_get(const char *body, const char *key, char *out, size_t out_len) {
  const size_t klen = strlen(key);
  const char *p = body;

  while (p && *p) {
    const char *amp = strchr(p, '&');
    const char *eq  = strchr(p, '=');
    if (eq && (!amp || eq < amp)) {
      if ((size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
        const size_t vlen = amp ? (size_t)(amp - eq - 1) : strlen(eq + 1);
        const size_t n    = vlen < out_len - 1 ? vlen : out_len - 1;
        memcpy(out, eq + 1, n);
        out[n] = '\0';
        url_decode(out);
        return true;
      }
    }
    p = amp ? amp + 1 : NULL;
  }
  return false;
}

/* JSON-Strings escapen - Passwoerter geben wir zwar nie aus, SSIDs aber schon. */
static void json_escape(const char *in, char *out, size_t out_len) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 2 < out_len; i++) {
    if (in[i] == '"' || in[i] == '\\') out[o++] = '\\';
    out[o++] = in[i];
  }
  out[o] = '\0';
}

/* ===========================================================================
 * Seite
 * ========================================================================= */

static const char PAGE[] = R"HTML(<!DOCTYPE html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WiFi-Ethernet-Bridge</title>
<style>
:root{--bg:#12151a;--fg:#e6e9ef;--mut:#8b94a5;--acc:#4da3ff;--ok:#3ecf8e;--bad:#ff6b6b;--card:#1b1f27}
*{box-sizing:border-box}
body{margin:0;font:15px/1.5 system-ui,sans-serif;background:var(--bg);color:var(--fg)}
.wrap{max-width:720px;margin:0 auto;padding:20px}
h1{font-size:20px;margin:0 0 4px}
.sub{color:var(--mut);font-size:13px;margin-bottom:20px}
.card{background:var(--card);border-radius:10px;padding:16px;margin-bottom:16px}
.card h2{font-size:14px;text-transform:uppercase;letter-spacing:.06em;color:var(--mut);margin:0 0 12px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:12px}
.m{background:#232833;border-radius:8px;padding:10px}
.m .k{font-size:11px;color:var(--mut);text-transform:uppercase}
.m .v{font-size:20px;font-weight:600;margin-top:2px}
label{display:block;font-size:12px;color:var(--mut);margin:10px 0 4px}
input,select{width:100%;padding:9px 10px;border-radius:7px;border:1px solid #2f3542;background:#0e1116;color:var(--fg);font-size:14px}
.row{display:flex;gap:10px}.row>*{flex:1}
button{margin-top:16px;padding:11px 18px;border:0;border-radius:7px;background:var(--acc);color:#06101c;font-weight:600;font-size:14px;cursor:pointer}
button.sec{background:#2f3542;color:var(--fg)}
.bar{height:6px;background:#232833;border-radius:3px;overflow:hidden;margin-top:6px}
.bar i{display:block;height:100%;background:var(--acc)}
.ok{color:var(--ok)}.bad{color:var(--bad)}
#msg,#tmsg,#omsg{margin-top:12px;font-size:13px}
.tag{display:inline-block;font-size:11px;text-transform:uppercase;letter-spacing:.05em;
     padding:3px 8px;border-radius:4px;margin:18px 0 2px}
.tag.live{background:rgba(62,207,142,.15);color:var(--ok)}
.tag.reboot{background:rgba(255,183,77,.15);color:#ffb74d}
.hint{text-transform:none;letter-spacing:0;color:#6b7383;font-size:11px;margin-left:6px}
</style></head><body><div class="wrap">
<h1 id="title">WiFi-Ethernet-Bridge</h1>
<div class="sub" id="mode"></div>

<div class="card" id="statuscard">
  <h2>Status</h2>
  <div class="grid">
    <div class="m"><div class="k">RSSI</div><div class="v" id="rssi">-</div>
      <div class="bar"><i id="rssibar" style="width:0"></i></div></div>
    <div class="m"><div class="k">Kanal</div><div class="v" id="ch">-</div></div>
    <div class="m"><div class="k">LAN &rarr; WLAN</div><div class="v" id="up">-</div></div>
    <div class="m"><div class="k">WLAN &rarr; LAN</div><div class="v" id="down">-</div></div>
    <div class="m"><div class="k">Verworfen</div><div class="v" id="drops">-</div></div>
    <div class="m"><div class="k">Laufzeit</div><div class="v" id="uptime">-</div></div>
    <div class="m"><div class="k">Sendeleistung</div><div class="v" id="txp">-</div></div>
    <div class="m"><div class="k">Kanalbreite</div><div class="v" id="bw">-</div></div>
  </div>
  <div class="sub" style="margin:12px 0 0" id="detail"></div>
</div>

<div class="card">
  <h2>Konfiguration</h2>
  <label>Geraetename</label><input id="name">

  <div class="tag live" style="background:rgba(77,163,255,.15);color:var(--acc)">Netzwerk 1</div>
  <label>WLAN <a href="#" onclick="scan();return false" style="color:var(--acc)">(scannen)</a></label>
  <div class="row"><input id="ssid1" list="nets" placeholder="SSID">
    <input id="pass1" type="password" placeholder="Passwort"></div>
  <datalist id="nets"></datalist>
  <label>Management-IP (statisch) <span class="hint">Gateway = dein Router, kein zweiter Zugang</span></label>
  <div class="row"><input id="ip" placeholder="IP"><input id="mask" placeholder="Maske"><input id="gw" placeholder="Gateway"></div>

  <div class="tag live" style="background:rgba(77,163,255,.15);color:var(--acc)">Netzwerk 2 (optional)</div>
  <label>WLAN</label>
  <div class="row"><input id="ssid2" list="nets" placeholder="SSID">
    <input id="pass2" type="password" placeholder="Passwort"></div>
  <label>Management-IP <span class="hint">leer = denselben Satz wie Netzwerk 1 benutzen</span></label>
  <div class="row"><input id="ip2" placeholder="IP"><input id="mask2" placeholder="Maske"><input id="gw2" placeholder="Gateway"></div>

  <div class="tag live" style="background:rgba(77,163,255,.15);color:var(--acc)">Netzwerk 3 (optional)</div>
  <label>WLAN</label>
  <div class="row"><input id="ssid3" list="nets" placeholder="SSID">
    <input id="pass3" type="password" placeholder="Passwort"></div>
  <label>Management-IP <span class="hint">leer = denselben Satz wie Netzwerk 1 benutzen</span></label>
  <div class="row"><input id="ip3" placeholder="IP"><input id="mask3" placeholder="Maske"><input id="gw3" placeholder="Gateway"></div>
  <div class="sub" style="margin:6px 0 0">
    Fuer zwei Standorte: die Bridge probiert beide WLANs durch und uebernimmt
    den IP-Satz des Netzes, in dem sie tatsaechlich gelandet ist. Damit
    funktioniert derselbe Konfigurationsstand an beiden Orten.
  </div>

  <label>MQTT-Broker (leer = aus)</label>
  <div class="row"><input id="mqtt_host" placeholder="Host"><input id="mqtt_port" placeholder="1883" style="max-width:90px"></div>
  <div class="row"><input id="mqtt_user" placeholder="Benutzer"><input id="mqtt_pass" type="password" placeholder="Passwort"></div>

  <label>Telemetrie-Intervall (s)</label><input id="telemetry_s">
  <label>Admin-Schluessel (schuetzt Firmware-Update, leer = offen)</label>
  <input id="admin_pass" type="password" placeholder="unveraendert lassen = leer">

  <button onclick="save(true)">Speichern &amp; neu starten</button>
  <button class="sec" onclick="reboot()">Nur neu starten</button>
  <div id="msg"></div>
</div>

<div class="card">
  <h2>Feinabstimmung</h2>
  <div class="sub" style="margin:0 0 10px">
    In den Feldern stehen die <b>aktuell wirksamen Werte</b>. Der Knopf
    <i>Standardwerte</i> unten setzt sie auf die Firmware-Vorgaben zurueck.
    Aendern nur mit einem Messwert in der Hand &ndash; die Zaehler oben
    aktualisieren sich alle 2&nbsp;s.
  </div>

  <div class="tag live">&#9679; wirkt SOFORT &ndash; kein Neustart</div>
  <label>Sendeleistung
    <span class="hint">die PHY kennt nur diese Stufen und rundet ab</span></label>
  <select id="tx_power">
    <option value="0">Standard (Maximum, 20 dBm)</option>
    <option value="80">20 dBm - voll</option>
    <option value="72">18 dBm</option>
    <option value="66">16.5 dBm</option>
    <option value="60">15 dBm</option>
    <option value="56">14 dBm</option>
    <option value="52">13 dBm</option>
    <option value="44">11 dBm</option>
    <option value="34">8.5 dBm</option>
    <option value="28">7 dBm</option>
    <option value="20">5 dBm</option>
    <option value="8">2 dBm - Minimum</option>
  </select>
  <div class="sub" style="margin:6px 0 0">
    Mehr ist nicht automatisch besser: die Endstufe wird am oberen Ende
    unlinearer. Auf kurzer Distanz kann <i>Runterdrehen</i> den Durchsatz
    erhoehen. Auf den Empfang hat es keinen Einfluss.
  </div>

  <div class="row">
    <div><label>Retries LAN&rarr;WLAN</label><input id="eth_tx_retries"></div>
    <div><label>Retries WLAN&rarr;LAN</label><input id="wifi_tx_retries"></div>
  </div>
  <div class="sub" style="margin:6px 0 0">
    <b>Der wichtigste Wert.</b> Er bestimmt, wie oft ein Frame ins WLAN
    nachgereicht wird, bevor es verworfen wird.
    <table style="width:100%;margin-top:8px;border-collapse:collapse;font-size:12px">
    <tr><td style="padding:2px 8px 2px 0"><b>1&ndash;2</b></td>
        <td>Nur fuer TCP-Durchsatztests. Schnelles Verwerfen, TCP regelt selbst nach.</td></tr>
    <tr><td style="padding:2px 8px 2px 0"><b>8</b></td>
        <td>Standard. Guter Kompromiss fuer Kamerastreams.</td></tr>
    <tr><td style="padding:2px 8px 2px 0"><b>12&ndash;20</b></td>
        <td>Schwaches Signal oder hohe Bitrate. Bei uns senkte 16 den Verlust
            von 13&nbsp;% auf 0,05&nbsp;%.</td></tr>
    </table>
    Bei <b>UDP-Video zaehlt jedes verworfene Paket als Bildfehler</b> &ndash; hier
    ist Warten fast immer besser als Verwerfen. Zu hohe Werte blockieren
    allerdings den Ethernet-Empfang, und dann geht der Verlust unsichtbar im
    DMA verloren. Deshalb: so klein wie moeglich, so gross wie noetig.
    <br>Die Gegenrichtung braucht selten mehr als 1 &ndash; Ethernet nimmt
    praktisch immer an.
  </div>

  <div style="margin-top:14px;padding:12px;background:#232833;border-radius:8px">
    <b style="font-size:13px">Automatisch einmessen</b>
    <div class="sub" style="margin:4px 0 8px">
      Probiert 1, 2, 4, 6, 8, 12, 16 und 20 durch und misst jeweils 15&nbsp;s
      die Verlustrate. Gewaehlt wird der <b>kleinste</b> Wert, der unter
      0,10&nbsp;% bleibt, und dauerhaft gespeichert.
      <br><b>Es muss dabei Verkehr fliessen</b> &ndash; also die Kamera streamen
      lassen. Dauert etwa 2,5&nbsp;Minuten, waehrenddessen ist das Bild
      zeitweise gestoert (die niedrigen Werte werden ja bewusst getestet).
    </div>
    <button onclick="autotune()" style="margin-top:0">Einmessen starten</button>
    <div id="atmsg" style="margin-top:10px;font-size:13px"></div>
  </div>

  <div class="tag reboot">&#9679; erst nach NEUSTART wirksam</div>
  <div class="row">
    <div><label>Statische RX-Puffer</label>
      <input id="static_rx_buf"></div>
    <div><label>Dynamische RX-Puffer</label>
      <input id="dynamic_rx_buf"></div>
  </div>
  <div class="row">
    <div><label>Dynamische TX-Puffer</label>
      <input id="dynamic_tx_buf"></div>
    <div><label>RX-Block-Ack-Fenster</label>
      <input id="rx_ba_win"></div>
  </div>
  <div class="sub" style="margin:6px 0 0">
    <b>Mehr ist hier schlechter.</b> Sende- und Empfangspuffer teilen sich
    denselben Speicher: eine Erhoehung der TX-Puffer auf 64 liess den freien
    Heap von 111 auf 9&nbsp;kB fallen und halbierte den Durchsatz.
    Der Heap steht oben im Status &ndash; faellt er unter etwa 30&nbsp;kB, ist es
    zu viel.
  </div>

  <label>Kanalbreite</label>
  <select id="ht40">
    <option value="0">HT20 - 20 MHz (Standard)</option>
    <option value="1">HT40 - 40 MHz, falls der AP mitmacht</option>
  </select>
  <label>802.11b-Raten</label>
  <select id="no_11b">
    <option value="0">aktiv lassen (Standard)</option>
    <option value="1">abschalten - spart Sendezeit</option>
  </select>
  <div class="sub" style="margin:6px 0 0">
    HT40 verdoppelt die Bruttorate, belegt aber die doppelte Frequenzbreite
    und faengt entsprechend mehr Stoerungen ein. In dicht belegten
    2,4-GHz-Umgebungen ist HT20 oft gleich schnell und stoert die Nachbarn
    weniger &ndash; bei uns war der Unterschied nicht messbar. Die tatsaechlich
    ausgehandelte Breite steht oben im Status.
  </div>

  <button onclick="save(false)">Speichern</button>
  <button class="sec" onclick="defaults()">Standardwerte</button>
  <button class="sec" onclick="reboot()">Neu starten</button>
  <div id="tmsg"></div>
</div>

<div class="card">
  <h2>Firmware-Update</h2>
  <div class="sub" style="margin:0 0 10px">
    Datei <code>.pio/build/wt32-eth01/firmware.bin</code> hochladen.
    Die Bridge steht waehrenddessen still und startet danach neu.
    Bootet die neue Firmware nicht sauber, faellt der Bootloader
    automatisch auf die vorherige Version zurueck.
  </div>
  <input type="file" id="fw" accept=".bin">
  <label>Admin-Schluessel</label><input id="okey" type="password">
  <div class="bar" style="margin-top:12px"><i id="pbar" style="width:0"></i></div>
  <button onclick="upload()">Hochladen &amp; flashen</button>
  <div id="omsg"></div>
</div>
</div><script>
const $=i=>document.getElementById(i);
/* Im Provisionierungsmodus laeuft der STA-Datenpfad noch nicht, dort greift
   auch die Sendeleistung nicht sofort - die Rueckmeldung nach dem Speichern
   muss das sagen, sonst wartet man auf eine Wirkung, die es nicht gibt. */
let PROV=false;
function fmt(k){return k>1000?(k/1000).toFixed(1)+' Mbit/s':k+' kbit/s'}
function dur(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);
  return d?d+'d '+h+'h':(h?h+'h '+m+'m':m+'m')}
async function tick(){
  try{const s=await(await fetch('/api/status')).json();
    PROV=s.prov;
    $('mode').textContent=s.prov?'Erstinbetriebnahme - noch keine Bridge aktiv':
      (s.wifi?'Bridge aktiv':'WLAN getrennt');
    $('rssi').textContent=s.wifi?s.rssi+' dBm':'-';
    /* -30 dBm = super, -90 = tot */
    $('rssibar').style.width=Math.max(0,Math.min(100,(s.rssi+95)*100/65))+'%';
    $('ch').textContent=s.wifi?s.ch:'-';
    $('up').textContent=fmt(s.kbps_up); $('down').textContent=fmt(s.kbps_down);
    $('drops').textContent=s.drop_up+' / '+s.drop_down;
    $('drops').className='v '+((s.drop_up+s.drop_down)?'bad':'ok');
    $('uptime').textContent=dur(s.uptime);
    /* txp kommt in 0.25-dBm-Schritten und ist der Ist-Wert des Treibers,
       nicht der eingestellte Soll-Wert. */
    $('txp').textContent=(s.wifi&&s.txp>=0)?(s.txp/4).toFixed(2).replace(/\.?0+$/,'')+' dBm':'-';
    /* Ist-Wert vom Treiber: zeigt, ob HT40 wirklich ausgehandelt wurde. */
    $('bw').textContent=(s.wifi&&s.bw)?s.bw+' MHz':'-';
    /* Fortschritt der Einmessung. at: 0=untaetig 1=laeuft 2=fertig 3=zu wenig Verkehr */
    if(s.at===1) $('atmsg').textContent='Einmessung laeuft - Schritt '+(s.at_s+1)+' von '+s.at_n+
                 ' (etwa '+Math.max(0,Math.round((s.at_n-s.at_s)*19/60*10)/10)+' min uebrig)';
    else if(s.at===2){ $('atmsg').textContent='Fertig - gewaehlt: '+s.at_r+' Retries, gespeichert.';
                       if($('eth_tx_retries').value!=s.at_r) $('eth_tx_retries').value=s.at_r; }
    else if(s.at===3) $('atmsg').textContent='Abgebrochen: zu wenig Verkehr waehrend der Messung. '+
                      'Kamera streamen lassen und erneut starten.';
    $('detail').innerHTML='Client: <b>'+s.client_mac+'</b> &middot; IP: <b>'+(s.client_ip||'-')+'</b>'+
      ' &middot; Name: <b>'+(s.client_name||'-')+'</b><br>Verbunden mit: <b>'+
      (s.ssid||'-')+'</b> ('+s.bssid+')'+
      ' &middot; Ethernet: <span class="'+(s.eth?'ok':'bad')+'">'+(s.eth?'verbunden':'kein Link')+
      '</span> &middot; Heap: <b>'+Math.round(s.heap/1024)+' kB</b>'+
      (s.heap_min!==undefined
        ? ' (Tiefststand '+Math.round(s.heap_min/1024)+' kB, DMA '+
          Math.round(s.heap_dma/1024)+' kB, groesster Block '+
          Math.round(s.heap_blk/1024)+' kB)'
        : '');
  }catch(e){}
}
/* Ein Feldname pro Eintrag, genau in dieser Reihenfolge auch beim Speichern.
   Passwortfelder stehen bewusst NICHT in load() - sie kommen nie vom Board
   zurueck, ein leeres Feld heisst beim Speichern "unveraendert lassen". */
const CFG=['name','ssid1','ssid2','ssid3','ip','mask','gw','ip2','mask2','gw2','ip3','mask3','gw3',
           'mqtt_host','mqtt_port','mqtt_user','telemetry_s',
           'tx_power','eth_tx_retries','wifi_tx_retries',
           'static_rx_buf','dynamic_rx_buf','dynamic_tx_buf','rx_ba_win',
           'ht40','no_11b'];
async function load(){
  const c=await(await fetch('/api/config')).json();
  DEF=c.def||null;
  for(const k of CFG) if(c[k]!==undefined)$(k).value=c[k];
}
async function scan(){
  $('msg').textContent='Suche Netze...';
  const r=await(await fetch('/api/scan')).json();
  $('nets').innerHTML=r.map(n=>'<option value="'+n.ssid+'">'+n.ssid+' ('+n.rssi+' dBm)</option>').join('');
  $('msg').textContent=r.length+' Netze gefunden';
}
/* rb=true -> speichern und neu starten (WLAN-Zugangsdaten, IP, Puffer...).
   rb=false -> nur speichern; TX-Power und Retries greifen dann sofort, das
   Board bleibt online und man kann direkt oben am Zaehler ablesen, ob die
   Aenderung etwas gebracht hat. Genau dafuer ist das Tuning-Panel da. */
async function save(rb){
  const f=CFG.concat(['pass1','pass2','pass3','mqtt_pass','admin_pass']);
  const body=f.map(k=>k+'='+encodeURIComponent($(k).value)).join('&');
  const tgt=rb?$('msg'):$('tmsg');
  tgt.textContent='Speichere...';
  const r=await fetch('/api/config',{method:'POST',body:body,
    headers:{'Content-Type':'application/x-www-form-urlencoded'}});
  if(!r.ok){tgt.textContent='Fehler beim Speichern';return}
  if(rb){
    tgt.textContent='Gespeichert. Neustart laeuft...';
    setTimeout(()=>fetch('/api/reboot',{method:'POST'}),600);
  }else if(PROV){
    tgt.textContent='Gespeichert. Im Provisionierungsmodus greift noch nichts davon - '
      +'die Werte werden beim ersten Bridge-Start uebernommen.';
  }else{
    tgt.textContent='Gespeichert. Sendeleistung und Retries sind sofort aktiv - '
      +'Puffer, Kanalbreite und 802.11b erst nach einem Neustart.';
  }
}
let DEF=null;
/* Fuellt die Zahlenfelder mit den Firmware-Vorgaben. Gespeichert wird erst
   beim Klick auf Speichern - so kann man es sich vorher ansehen. */
function defaults(){
  if(!DEF){$('tmsg').textContent='Standardwerte noch nicht geladen';return}
  for(const k in DEF) if($(k)) $(k).value=DEF[k];
  $('tx_power').value='0';
  $('tmsg').textContent='Standardwerte eingetragen - zum Uebernehmen speichern.';
}
async function autotune(){
  $('atmsg').textContent='Starte...';
  try{
    const j=await(await fetch('/api/autotune',{method:'POST'})).json();
    $('atmsg').textContent=j.ok?'Einmessung laeuft - bitte Verkehr laufen lassen.'
                               :('Nicht gestartet: '+(j.err||'unbekannt'));
  }catch(e){$('atmsg').textContent='Fehler beim Starten'}
}
async function reboot(){await fetch('/api/reboot',{method:'POST'});$('msg').textContent='Neustart...';}
function upload(){
  const f=$('fw').files[0];
  if(!f){$('omsg').textContent='Keine Datei gewaehlt';return}
  const x=new XMLHttpRequest();
  x.open('POST','/api/update');
  x.setRequestHeader('Content-Type','application/octet-stream');
  if($('okey').value)x.setRequestHeader('X-Admin-Key',$('okey').value);
  x.upload.onprogress=e=>{
    const p=e.total?Math.round(e.loaded*100/e.total):0;
    $('pbar').style.width=p+'%';$('omsg').textContent='Uebertrage... '+p+'%';
  };
  x.onload=()=>{
    $('omsg').textContent = x.status==200
      ? 'Geflasht. Board startet neu - Seite in ca. 20 s neu laden.'
      : 'Fehler: '+x.responseText;
  };
  x.onerror=()=>{$('omsg').textContent='Verbindung abgebrochen'};
  $('omsg').textContent='Starte Upload...';
  x.send(f);
}
load();tick();setInterval(tick,2000);
</script></body></html>)HTML";

/* ===========================================================================
 * Handler
 * ========================================================================= */

static esp_err_t h_root(httpd_req_t *r) {
  httpd_resp_set_type(r, "text/html; charset=utf-8");
  /* Ohne Cache-Steuerung speichern Browser HTML heuristisch zwischen - nach
   * einem Firmware-Update sieht man dann die alte Seite, samt fehlender oder
   * falscher Felder, und ein normaler Reload hilft nicht. Am 2026-08-15 genau
   * so aufgetreten: ein per API gesetzter Wert war im Portal unsichtbar,
   * obwohl er in /api/config korrekt stand. */
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  return httpd_resp_send(r, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_status(httpd_req_t *r) {
  BridgeStats st;
  bridge_get_stats(&st);

  char buf[576];
  snprintf(buf, sizeof(buf),
    "{\"prov\":%d,\"wifi\":%d,\"eth\":%d,\"rssi\":%d,\"ch\":%u,"
    "\"kbps_up\":%lu,\"kbps_down\":%lu,\"pkt_up\":%lu,\"pkt_down\":%lu,"
    "\"drop_up\":%lu,\"drop_down\":%lu,\"uptime\":%lu,"
    "\"client_mac\":\"%s\",\"client_ip\":\"%s\",\"client_name\":\"%s\","
    "\"ssid\":\"%s\",\"bssid\":\"%s\","
    "\"at\":%d,\"at_s\":%u,\"at_n\":%u,\"at_r\":%u,"
    "\"heap\":%lu,\"heap_min\":%lu,\"heap_dma\":%lu,\"heap_blk\":%lu,"
    "\"txp\":%d,\"bw\":%u}",
    s_provisioning ? 1 : 0, st.wifi_up ? 1 : 0, st.eth_link ? 1 : 0,
    (int)st.rssi, (unsigned)st.channel,
    (unsigned long)st.kbps_eth2wifi, (unsigned long)st.kbps_wifi2eth,
    (unsigned long)st.pkt_eth2wifi,  (unsigned long)st.pkt_wifi2eth,
    (unsigned long)st.drop_eth2wifi, (unsigned long)st.drop_wifi2eth,
    (unsigned long)(millis() / 1000), st.client_mac, st.client_ip, st.client_name, st.ssid, st.bssid,
    (int)bridge_autotune_state(), bridge_autotune_schritt(),
    bridge_autotune_anzahl(), bridge_autotune_ergebnis(),
    (unsigned long)esp_get_free_heap_size(),
    /* Tiefststand seit dem Start - zeigt Einbrueche, die zwischen zwei
     * Abfragen liegen und sonst unsichtbar bleiben. */
    (unsigned long)esp_get_minimum_free_heap_size(),
    /* Fuer WLAN-Puffer zaehlt nur DMA-faehiger interner Speicher. */
    (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
    /* Groesster zusammenhaengender Block: faellt er weit unter die Summe,
     * ist der Heap fragmentiert und grosse Anforderungen scheitern, obwohl
     * rechnerisch genug frei waere. */
    (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
    /* Der vom Treiber gemeldete Ist-Wert, nicht der eingestellte Soll-Wert -
     * die PHY rundet auf 11 feste Stufen ab. */
    (int)bridge_get_tx_power(),
    /* Ebenso die tatsaechlich ausgehandelte Kanalbreite: der ht40-Schalter
     * sagt nur, was wir anbieten. */
    (unsigned)bridge_get_bandwidth_mhz());

  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  return httpd_resp_send(r, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_config_get(httpd_req_t *r) {
  char ip[16], mask[16], gw[16];
  format_ipv4(g_cfg.mgmt_ip,   ip,   sizeof(ip));
  format_ipv4(g_cfg.mgmt_mask, mask, sizeof(mask));
  format_ipv4(g_cfg.mgmt_gw,   gw,   sizeof(gw));

  /* Zweites Profil: unbelegte Felder als leeren String ausliefern statt als
   * "0.0.0.0" - im Formular soll dort nichts stehen, sonst sieht es aus wie
   * eine gesetzte (und kaputte) Adresse. */
  char ip2[16] = "", mask2[16] = "", gw2[16] = "";
  char ip3[16] = "", mask3[16] = "", gw3[16] = "";
  if (g_cfg.mgmt_ip2[0] || g_cfg.mgmt_ip2[1] || g_cfg.mgmt_ip2[2] || g_cfg.mgmt_ip2[3])
    format_ipv4(g_cfg.mgmt_ip2, ip2, sizeof(ip2));
  if (g_cfg.mgmt_mask2[0] || g_cfg.mgmt_mask2[1] || g_cfg.mgmt_mask2[2] || g_cfg.mgmt_mask2[3])
    format_ipv4(g_cfg.mgmt_mask2, mask2, sizeof(mask2));
  if (g_cfg.mgmt_gw2[0] || g_cfg.mgmt_gw2[1] || g_cfg.mgmt_gw2[2] || g_cfg.mgmt_gw2[3])
    format_ipv4(g_cfg.mgmt_gw2, gw2, sizeof(gw2));
  if (g_cfg.mgmt_ip3[0] || g_cfg.mgmt_ip3[1] || g_cfg.mgmt_ip3[2] || g_cfg.mgmt_ip3[3])
    format_ipv4(g_cfg.mgmt_ip3, ip3, sizeof(ip3));
  if (g_cfg.mgmt_mask3[0] || g_cfg.mgmt_mask3[1] || g_cfg.mgmt_mask3[2] || g_cfg.mgmt_mask3[3])
    format_ipv4(g_cfg.mgmt_mask3, mask3, sizeof(mask3));
  if (g_cfg.mgmt_gw3[0] || g_cfg.mgmt_gw3[1] || g_cfg.mgmt_gw3[2] || g_cfg.mgmt_gw3[3])
    format_ipv4(g_cfg.mgmt_gw3, gw3, sizeof(gw3));

  /* mu muss den ESKAPIERTEN Benutzernamen fassen: json_escape() stellt jedem
   * " und \ ein Backslash voran, im unguenstigsten Fall verdoppelt sich die
   * Laenge also. Bei 64 nutzbaren Zeichen sind das 128 plus Nullbyte - mit
   * den alten 64 waere der Name still abgeschnitten worden, und zwar nur bei
   * langen Werten, also genau dann, wenn es niemand ausprobiert. */
  char n[64], s1[80], s2[80], s3[80], mh[128], mu[136];
  json_escape(g_cfg.name,      n,  sizeof(n));
  json_escape(g_cfg.ssid1,     s1, sizeof(s1));
  json_escape(g_cfg.ssid2,     s2, sizeof(s2));
  json_escape(g_cfg.ssid3,     s3, sizeof(s3));
  json_escape(g_cfg.mqtt_host, mh, sizeof(mh));
  json_escape(g_cfg.mqtt_user, mu, sizeof(mu));

  bridge_tuning_t eff, def;
  bridge_get_effective(&eff);
  bridge_get_defaults(&def);

  /* Passwoerter werden bewusst nie zurueckgeliefert.
   * Puffergroesse mitgewachsen: Name, zwei SSIDs, sechs IP-Adressen aus zwei
   * Profilen, Host, eskapierter Benutzername und die Tuning-Zahlen kommen
   * zusammen auf gut 900 Byte im ungeguenstigsten Fall. snprintf schneidet
   * zwar sauber ab, aber ein abgeschnittenes JSON ist unparsbar - dann
   * bliebe das Formular schlicht leer. */
  char buf[1800];
  snprintf(buf, sizeof(buf),
    "{\"name\":\"%s\",\"ssid1\":\"%s\",\"ssid2\":\"%s\",\"ssid3\":\"%s\","
    "\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\","
    "\"ip2\":\"%s\",\"mask2\":\"%s\",\"gw2\":\"%s\","
    "\"ip3\":\"%s\",\"mask3\":\"%s\",\"gw3\":\"%s\","
    "\"mqtt_host\":\"%s\",\"mqtt_port\":%u,\"mqtt_user\":\"%s\","
    "\"telemetry_s\":%u,"
    "\"tx_power\":%d,\"eth_tx_retries\":%u,\"wifi_tx_retries\":%u,"
    "\"static_rx_buf\":%u,\"dynamic_rx_buf\":%u,\"dynamic_tx_buf\":%u,"
    "\"rx_ba_win\":%u,\"ht40\":%u,\"no_11b\":%u,"
    "\"def\":{\"eth_tx_retries\":%u,\"wifi_tx_retries\":%u,"
    "\"static_rx_buf\":%u,\"dynamic_rx_buf\":%u,\"dynamic_tx_buf\":%u,"
    "\"rx_ba_win\":%u}}",
    n, s1, s2, s3, ip, mask, gw, ip2, mask2, gw2, ip3, mask3, gw3,
    mh, g_cfg.mqtt_port, mu, g_cfg.telemetry_s,
    /* Die WIRKSAMEN Werte ausliefern, nicht die gespeicherte 0 - im Formular
     * soll stehen, was laeuft. Beim Speichern wird die Zahl dann explizit
     * uebernommen; der "Standardwerte"-Knopf setzt sie zurueck. */
    (int)g_cfg.tx_power, eff.eth_retries, eff.wifi_retries,
    eff.static_rx, eff.dyn_rx, eff.dyn_tx,
    eff.ba_win, g_cfg.ht40, g_cfg.no_11b,
    def.eth_retries, def.wifi_retries,
    def.static_rx, def.dyn_rx, def.dyn_tx, def.ba_win);

  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  return httpd_resp_send(r, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_config_post(httpd_req_t *r) {
  /* Grosszuegiger als noetig, aber nicht beliebig: das Formular schickt immer
   * ALLE Felder mit, und im unguenstigsten Fall sind Passwoerter und SSIDs
   * komplett prozentkodiert (drei Zeichen pro Byte). Mit den neun Tuning-
   * Feldern kam die alte Grenze von 1400 in genau diesem Fall ins Wanken. */
  if (r->content_len > 2040) {
    httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "zu gross");
    return ESP_FAIL;
  }

  char body[2048];
  int got = 0;
  while (got < (int)r->content_len) {
    int n = httpd_req_recv(r, body + got, r->content_len - got);
    if (n <= 0) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "recv"); return ESP_FAIL; }
    got += n;
  }
  body[got] = '\0';

  char v[80];
  if (form_get(body, "name",  v, sizeof(v))) strncpy(g_cfg.name,  v, sizeof(g_cfg.name)  - 1);
  if (form_get(body, "ssid1", v, sizeof(v))) strncpy(g_cfg.ssid1, v, sizeof(g_cfg.ssid1) - 1);
  if (form_get(body, "ssid2", v, sizeof(v))) strncpy(g_cfg.ssid2, v, sizeof(g_cfg.ssid2) - 1);
  if (form_get(body, "ssid3", v, sizeof(v))) strncpy(g_cfg.ssid3, v, sizeof(g_cfg.ssid3) - 1);

  /* Leeres Passwortfeld heisst "unveraendert" - sonst wuerde jedes Speichern
   * vom Statusbildschirm aus die Zugangsdaten loeschen. */
  if (form_get(body, "pass1", v, sizeof(v)) && v[0]) strncpy(g_cfg.pass1, v, sizeof(g_cfg.pass1) - 1);
  if (form_get(body, "pass2", v, sizeof(v)) && v[0]) strncpy(g_cfg.pass2, v, sizeof(g_cfg.pass2) - 1);
  if (form_get(body, "pass3", v, sizeof(v)) && v[0]) strncpy(g_cfg.pass3, v, sizeof(g_cfg.pass3) - 1);

  if (form_get(body, "ip",   v, sizeof(v))) parse_ipv4(v, g_cfg.mgmt_ip);
  if (form_get(body, "mask", v, sizeof(v))) parse_ipv4(v, g_cfg.mgmt_mask);
  if (form_get(body, "gw",   v, sizeof(v))) parse_ipv4(v, g_cfg.mgmt_gw);

  /* Zweites Profil. Ein leeres Feld loescht hier bewusst (auf 0.0.0.0),
   * damit man ein einmal eingetragenes Profil auch wieder loswird - anders
   * als bei den Passwortfeldern, wo leer "unveraendert" bedeutet.
   * parse_ipv4() laesst den Wert bei Unsinn stehen, deshalb vorher nullen. */
  if (form_get(body, "ip2", v, sizeof(v))) {
    memset(g_cfg.mgmt_ip2, 0, 4);
    if (v[0]) parse_ipv4(v, g_cfg.mgmt_ip2);
  }
  if (form_get(body, "mask2", v, sizeof(v))) {
    memset(g_cfg.mgmt_mask2, 0, 4);
    if (v[0]) parse_ipv4(v, g_cfg.mgmt_mask2);
  }
  if (form_get(body, "gw2", v, sizeof(v))) {
    memset(g_cfg.mgmt_gw2, 0, 4);
    if (v[0]) parse_ipv4(v, g_cfg.mgmt_gw2);
  }
  if (form_get(body, "ip3", v, sizeof(v)))   { memset(g_cfg.mgmt_ip3, 0, 4);   if (v[0]) parse_ipv4(v, g_cfg.mgmt_ip3); }
  if (form_get(body, "mask3", v, sizeof(v))) { memset(g_cfg.mgmt_mask3, 0, 4); if (v[0]) parse_ipv4(v, g_cfg.mgmt_mask3); }
  if (form_get(body, "gw3", v, sizeof(v)))   { memset(g_cfg.mgmt_gw3, 0, 4);   if (v[0]) parse_ipv4(v, g_cfg.mgmt_gw3); }

  if (form_get(body, "mqtt_host", v, sizeof(v))) strncpy(g_cfg.mqtt_host, v, sizeof(g_cfg.mqtt_host) - 1);
  if (form_get(body, "mqtt_port", v, sizeof(v))) g_cfg.mqtt_port = (uint16_t)atoi(v);
  if (form_get(body, "mqtt_user", v, sizeof(v))) strncpy(g_cfg.mqtt_user, v, sizeof(g_cfg.mqtt_user) - 1);
  if (form_get(body, "mqtt_pass", v, sizeof(v)) && v[0]) strncpy(g_cfg.mqtt_pass, v, sizeof(g_cfg.mqtt_pass) - 1);

  if (form_get(body, "admin_pass", v, sizeof(v)) && v[0])
    strncpy(g_cfg.admin_pass, v, sizeof(g_cfg.admin_pass) - 1);

  if (form_get(body, "telemetry_s", v, sizeof(v))) {
    int t = atoi(v);
    g_cfg.telemetry_s = (uint16_t)(t < 1 ? 1 : (t > 3600 ? 3600 : t));
  }

  /* --- Feinabstimmung ----------------------------------------------------
   * Durchgaengig gilt 0 = "Default aus sdkconfig / PHY", deshalb ist 0 hier
   * ueberall ein erlaubter Wert und keine Fehleingabe. Die Obergrenzen sind
   * hart, damit ein Vertipper im Portal das Board nicht unbrauchbar macht -
   * bei einem Geraet ohne seriellen Zugang ist das kein Luxus. */
  if (form_get(body, "tx_power", v, sizeof(v))) {
    int t = atoi(v);
    /* Gueltig laut esp_wifi.h: 8..84 (2..20 dBm). Alles dazwischen wird von
     * der PHY auf die naechstniedrigere der 11 Stufen gerundet. */
    if (t != 0 && t < 8)  t = 8;
    if (t > 84)           t = 84;
    g_cfg.tx_power = (int8_t)t;
  }
  if (form_get(body, "eth_tx_retries", v, sizeof(v))) {
    int t = atoi(v);
    g_cfg.eth_tx_retries = (uint8_t)(t < 0 ? 0 : (t > 20 ? 20 : t));
  }
  if (form_get(body, "wifi_tx_retries", v, sizeof(v))) {
    int t = atoi(v);
    g_cfg.wifi_tx_retries = (uint8_t)(t < 0 ? 0 : (t > 20 ? 20 : t));
  }
  if (form_get(body, "static_rx_buf", v, sizeof(v))) {
    int t = atoi(v);
    /* IDF-Grenzen fuer static_rx_buf_num sind 2..32. */
    if (t != 0 && t < 2) t = 2;
    if (t > 32)          t = 32;
    g_cfg.static_rx_buf = (uint8_t)t;
  }
  if (form_get(body, "dynamic_rx_buf", v, sizeof(v))) {
    int t = atoi(v);
    if (t > 128) t = 128;
    g_cfg.dynamic_rx_buf = (uint8_t)(t < 0 ? 0 : t);
  }
  if (form_get(body, "dynamic_tx_buf", v, sizeof(v))) {
    int t = atoi(v);
    if (t > 128) t = 128;
    g_cfg.dynamic_tx_buf = (uint8_t)(t < 0 ? 0 : t);
  }
  if (form_get(body, "rx_ba_win", v, sizeof(v))) {
    int t = atoi(v);
    if (t > 32) t = 32;
    g_cfg.rx_ba_win = (uint8_t)(t < 0 ? 0 : t);
  }
  if (form_get(body, "ht40",   v, sizeof(v))) g_cfg.ht40   = (uint8_t)(atoi(v) ? 1 : 0);
  if (form_get(body, "no_11b", v, sizeof(v))) g_cfg.no_11b = (uint8_t)(atoi(v) ? 1 : 0);

  g_cfg.configured = (g_cfg.ssid1[0] != '\0');

  if (!cfg_save()) {
    httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS");
    return ESP_FAIL;
  }

  /* TX-Power und Retry-Zaehler sofort uebernehmen, damit man im Prueffeld
   * messen kann, ohne jedes Mal neu zu starten. Im Provisionierungsmodus
   * laeuft der STA-Datenpfad noch nicht - dann waere der Aufruf sinnlos. */
  if (!s_provisioning) bridge_apply_live_tuning();

  httpd_resp_set_type(r, "application/json");
  return httpd_resp_send(r, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_autotune(httpd_req_t *r) {
  const bool ok = bridge_autotune_start();
  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  return httpd_resp_send(r, ok ? "{\"ok\":true}"
                              : "{\"ok\":false,\"err\":\"laeuft bereits oder Bridge inaktiv\"}",
                         HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_reboot(httpd_req_t *r) {
  httpd_resp_send(r, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
  delay(400);
  esp_restart();
  return ESP_OK;
}

static esp_err_t h_scan(httpd_req_t *r) {
  wifi_scan_config_t sc = {};
  sc.show_hidden = false;

  if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, "[]", HTTPD_RESP_USE_STRLEN);
  }

  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  if (n > 20) n = 20;

  wifi_ap_record_t *recs = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * n);
  if (!recs) {
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, "[]", HTTPD_RESP_USE_STRLEN);
  }
  esp_wifi_scan_get_ap_records(&n, recs);

  httpd_resp_set_type(r, "application/json");
  httpd_resp_sendstr_chunk(r, "[");
  for (uint16_t i = 0; i < n; i++) {
    char esc[80], item[128];
    json_escape((const char *)recs[i].ssid, esc, sizeof(esc));
    if (!esc[0]) continue;
    snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d}",
             i ? "," : "", esc, (int)recs[i].rssi);
    httpd_resp_sendstr_chunk(r, item);
  }
  httpd_resp_sendstr_chunk(r, "]");
  httpd_resp_sendstr_chunk(r, NULL);

  free(recs);
  return ESP_OK;
}


/* ===========================================================================
 * Firmware-Update (OTA)
 * ---------------------------------------------------------------------------
 * Rohes POST des .bin-Files, direkt in die inaktive App-Partition gestreamt.
 * Kein HTTPS - das wuerde ~40 kB fuer TLS kosten, die wir hier nicht haben.
 * Deshalb der Header-Schluessel als Minimalschutz und die Empfehlung, das
 * Update nur aus dem eigenen Netz anzustossen.
 * ========================================================================= */

static esp_err_t h_update(httpd_req_t *r) {
  /* Minimalschutz. Ueber Klartext-HTTP ist das kein echter Schutz gegen
   * jemanden, der mitliest - aber es verhindert versehentliches Flashen
   * und Zugriffe von Geraeten, die den Schluessel nicht kennen. */
  if (g_cfg.admin_pass[0]) {
    char key[48] = "";
    if (httpd_req_get_hdr_value_str(r, "X-Admin-Key", key, sizeof(key)) != ESP_OK ||
        strcmp(key, g_cfg.admin_pass) != 0) {
      httpd_resp_send_err(r, HTTPD_401_UNAUTHORIZED, "falscher Schluessel");
      return ESP_FAIL;
    }
  }

  const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
  if (!target) {
    httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "keine OTA-Partition - falsche Partitionstabelle?");
    return ESP_FAIL;
  }

  printf("[OTA ] Ziel: %s (%u kB), erwartet %u kB\n",
                target->label, (unsigned)(target->size / 1024),
                (unsigned)(r->content_len / 1024));

  if (r->content_len > target->size) {
    httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Image passt nicht");
    return ESP_FAIL;
  }

  /* Datenpfad stilllegen: beim Flash-Schreiben faellt der Cache kurz aus,
   * und ein Datenpfad unter Last vertraegt das schlecht. Die Kamera ist fuer
   * die Dauer des Updates offline - das ist der richtige Kompromiss. */
  bridge_set_paused(true);

  esp_ota_handle_t ota = 0;
  esp_err_t err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &ota);
  if (err != ESP_OK) {
    bridge_set_paused(false);
    httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    return ESP_FAIL;
  }

  char *buf = (char *)malloc(4096);
  if (!buf) {
    esp_ota_abort(ota);
    bridge_set_paused(false);
    httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "kein Speicher");
    return ESP_FAIL;
  }

  int remaining = r->content_len;
  bool header_checked = false;

  while (remaining > 0) {
    int n = httpd_req_recv(r, buf, remaining < 4096 ? remaining : 4096);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) {
      free(buf); esp_ota_abort(ota); bridge_set_paused(false);
      printf("[OTA ] Uebertragung abgebrochen\n");
      httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Abbruch");
      return ESP_FAIL;
    }

    /* Magic-Byte pruefen, bevor wir eine gueltige Partition ueberschreiben.
     * Verhindert, dass ein versehentlich hochgeladenes Bild/PDF das Board
     * unbrauchbar macht. */
    if (!header_checked) {
      if (n < 1 || (uint8_t)buf[0] != ESP_IMAGE_HEADER_MAGIC) {
        free(buf); esp_ota_abort(ota); bridge_set_paused(false);
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "keine ESP32-Firmware");
        return ESP_FAIL;
      }
      header_checked = true;
    }

    err = esp_ota_write(ota, buf, n);
    if (err != ESP_OK) {
      free(buf); esp_ota_abort(ota); bridge_set_paused(false);
      httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
      return ESP_FAIL;
    }
    remaining -= n;
  }
  free(buf);

  err = esp_ota_end(ota);
  if (err != ESP_OK) {
    bridge_set_paused(false);
    printf("[OTA ] end: %s\n", esp_err_to_name(err));
    httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
        err == ESP_ERR_OTA_VALIDATE_FAILED ? "Image ungueltig" : esp_err_to_name(err));
    return ESP_FAIL;
  }

  err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK) {
    bridge_set_paused(false);
    httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    return ESP_FAIL;
  }

  printf("[OTA ] Erfolgreich - Neustart\n");
  httpd_resp_set_type(r, "application/json");
  httpd_resp_send(r, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
  delay(600);
  esp_restart();
  return ESP_OK;
}

/* ===========================================================================
 * Start
 * ========================================================================= */

bool web_start(bool provisioning) {
  s_provisioning = provisioning;

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size       = 6144;
  cfg.max_uri_handlers = 12;
  cfg.recv_wait_timeout = 20;   /* OTA-Upload braucht Geduld */
  cfg.lru_purge_enable = true;

  /* Sockets aus dem lwIP-Budget ableiten statt den IDF-Default (7) zu nehmen.
   * Der httpd belegt zusaetzlich zu max_open_sockets noch 3 Sockets intern;
   * ist die Summe groesser als CONFIG_LWIP_MAX_SOCKETS, startet er GAR NICHT.
   * Genau daran ist der erste Hardware-Boot gescheitert.
   * Die 2 reservierten Sockets sind fuer MQTT und DNS.
   * Bewusst hier gerechnet und nicht als feste Zahl: sonst kippt die Sache
   * beim naechsten Mal wieder still um, wenn jemand LWIP_MAX_SOCKETS
   * verkleinert. */
  int frei = CONFIG_LWIP_MAX_SOCKETS - 3 /* httpd-intern */ - 2 /* MQTT, DNS */;
  if (frei < 2) frei = 2;       /* Untergrenze: sonst ist das Portal nutzlos */
  cfg.max_open_sockets = frei;

  esp_err_t err = httpd_start(&s_httpd, &cfg);
  if (err != ESP_OK) {
    /* Fehlercode mit ausgeben - "Start fehlgeschlagen" allein hat beim
     * ersten Mal nicht gereicht, die eigentliche Ursache stand nur in der
     * IDF-Logzeile darueber. */
    printf("[WEB ] Start fehlgeschlagen: %s (max_open_sockets=%d, LWIP_MAX_SOCKETS=%d)\n",
           esp_err_to_name(err), cfg.max_open_sockets, CONFIG_LWIP_MAX_SOCKETS);
    return false;
  }

  const httpd_uri_t uris[] = {
    { "/",            HTTP_GET,  h_root,        NULL },
    { "/api/status",  HTTP_GET,  h_status,      NULL },
    { "/api/config",  HTTP_GET,  h_config_get,  NULL },
    { "/api/config",  HTTP_POST, h_config_post, NULL },
    { "/api/reboot",  HTTP_POST, h_reboot,      NULL },
    { "/api/autotune",HTTP_POST, h_autotune,    NULL },
    { "/api/scan",    HTTP_GET,  h_scan,        NULL },
    { "/api/update",  HTTP_POST, h_update,      NULL },
  };
  for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
    httpd_register_uri_handler(s_httpd, &uris[i]);
  }

  printf("[WEB ] HTTP-Server laeuft\n");
  return true;
}
