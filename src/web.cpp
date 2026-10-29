/*
 * web.cpp - Configuration portal + status API
 *
 * Serves one self-contained HTML page (PAGE[]) and a small JSON API:
 *   GET  /                  the portal page
 *   GET  /api/status        live counters and link state (polled every 2 s)
 *   GET  /api/disconnects   Wi-Fi disconnect statistics and log
 *   GET  /api/config        current configuration (never passwords)
 *   POST /api/config        save configuration (form-urlencoded)
 *   POST /api/reboot        reboot
 *   POST /api/eth_reset     restart the Ethernet driver
 *   POST /api/autotune      start the retry autotune
 *   GET  /api/scan          Wi-Fi scan
 *   POST /api/update        raw OTA upload            (admin key)
 *   GET/DELETE /api/coredump  coredump summary/erase  (admin key)
 *   GET/DELETE /api/crashes   crash history/erase     (admin key)
 *   GET  /api/log?since=N   console output since byte N (admin key)
 *   GET  /api/log?prev=1    end of the previous boot's output  (admin key)
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
#include "esp_core_dump.h"
#include "build_info.h"
#include "crashlog.h"
#include "conlog.h"
#include "cpustat.h"

#define AP_PASSWORD   "bridgesetup"   /* at least 8 characters, otherwise open */

static httpd_handle_t s_httpd = NULL;
static bool           s_provisioning = false;
static uint32_t       s_last_activity_ms = 0;

uint32_t web_last_activity_ms(void) { return s_last_activity_ms; }

/* ===========================================================================
 * Access point
 * ========================================================================= */

/* Start the setup access point "<name>-setup-XXXX" (WPA2, AP_PASSWORD) in
 * APSTA mode, so the portal can also scan for networks. See web.h. */
bool portal_start_ap(void) {
  /* Keep this idempotent. esp_netif_create_default_wifi_*() does not answer
   * an if_key that is already taken with an error code but with an assert -
   * i.e. a crash. The portal is the lifeline of a device without serial
   * access; it must not blow up here under any circumstances, whichever path
   * led here. (main.cpp reboots after a failed bridge attempt so this case
   * does not arise in the first place - this check is the second line of
   * defense.) */
  if (!esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"))  esp_netif_create_default_wifi_ap();
  /* STA as well, so the scan in the portal works. */
  if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t ie = esp_wifi_init(&cfg);
  if (ie != ESP_OK) {
    /* Already initialized is no reason to give up here - carry on and see
     * whether the AP can be configured anyway. */
    printf("[PORT] esp_wifi_init: %s (probably already initialized)\n",
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

  printf("\n[PORT] Setup Wi-Fi: '%s'  password: '%s'\n",
                (char *)ap.ap.ssid, AP_PASSWORD);
  printf("[PORT] Then open http://192.168.4.1\n\n");
  return true;
}

/* ===========================================================================
 * Helpers
 * ========================================================================= */

/* Value of one hex digit, -1 if `c` is not a hex digit. */
static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Decode a form-urlencoded value in place ('+' -> space, %XX -> byte).
 * Malformed % sequences are copied literally. */
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
 * Look up `key` in a form-urlencoded body and copy its decoded value into
 * `out` (truncated to out_len - 1). Returns false if the key is absent.
 * Split on '&' and '=' first, then decode - the other way round, an '&' in a
 * password would break the parsing.
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

/* Escape a string for use inside a JSON string literal (" and \ only).
 * Passwords are never output, but SSIDs and names are. The output can be up
 * to twice as long as the input. */
static void json_escape(const char *in, char *out, size_t out_len) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 2 < out_len; i++) {
    if (in[i] == '"' || in[i] == '\\') out[o++] = '\\';
    out[o++] = in[i];
  }
  out[o] = '\0';
}

/* ===========================================================================
 * Page
 * ========================================================================= */

static const char PAGE[] = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WiFi-Ethernet Bridge</title>
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
/* Feedback for ALL buttons: brighter on hover, clearly darker and slightly
   pressed in while held down. */
button{transition:background .1s,transform .08s,filter .1s}
button:hover:not(:disabled){filter:brightness(1.12)}
button:active:not(:disabled){background:#2a78d0;transform:scale(.97);filter:none}
button.sec:active:not(:disabled){background:#4a5367}
.bar{height:6px;background:#232833;border-radius:3px;overflow:hidden;margin-top:6px}
.bar i{display:block;height:100%;background:var(--acc)}
.ok{color:var(--ok)}.bad{color:var(--bad)}.warn{color:#f0b429}
#msg,#tmsg,#omsg{margin-top:12px;font-size:13px}
.tag{display:inline-block;font-size:11px;text-transform:uppercase;letter-spacing:.05em;
     padding:3px 8px;border-radius:4px;margin:18px 0 2px}
.tag.live{background:rgba(62,207,142,.15);color:var(--ok)}
.tag.reboot{background:rgba(255,183,77,.15);color:#ffb74d}
.hint{text-transform:none;letter-spacing:0;color:#6b7383;font-size:11px;margin-left:6px}
/* Reboot buttons: turn red while pressed, trigger only on release. */
button.hold{transition:background .12s,color .12s,transform .12s;touch-action:manipulation;user-select:none}
/* Same specificity as button.sec:active:not(:disabled) and declared later -
   so red also wins on grey reboot buttons. */
button.hold.armed,button.hold.armed:active{background:var(--bad);color:#fff;transform:scale(.97)}
button:disabled{opacity:.6;cursor:default}
/* Notice about unsaved changes, bottom right. */
#dirty{position:fixed;right:16px;bottom:16px;z-index:10;max-width:calc(100% - 32px);width:340px;
  background:#2a1b1f;border:1px solid rgba(255,107,107,.55);border-radius:10px;padding:12px 14px;
  box-shadow:0 8px 24px rgba(0,0,0,.45);font-size:13px}
#dirty[hidden]{display:none}
#dirty b{color:var(--bad)}
#dirty .row button{margin-top:10px;padding:8px 12px}
#dirtymsg{margin-top:6px;color:var(--mut);font-size:12px}
/* While the box is shown, leave room at the end of the page - otherwise it
   permanently covers the last fields (especially on phones). */
body:has(#dirty:not([hidden])) .wrap{padding-bottom:150px}
</style></head><body><div class="wrap">
<h1 id="title">WiFi-Ethernet Bridge</h1>
<div class="sub" id="mode"></div>

<div class="card" id="statuscard">
  <h2>Status</h2>
  <div class="grid">
    <div class="m"><div class="k">RSSI</div><div class="v" id="rssi">-</div>
      <div class="bar"><i id="rssibar" style="width:0"></i></div></div>
    <div class="m"><div class="k">Channel</div><div class="v" id="ch">-</div></div>
    <div class="m"><div class="k">LAN &rarr; Wi-Fi</div><div class="v" id="up">-</div></div>
    <div class="m"><div class="k">Wi-Fi &rarr; LAN</div><div class="v" id="down">-</div></div>
    <div class="m"><div class="k">Dropped</div><div class="v" id="drops">-</div></div>
    <div class="m"><div class="k">Uptime</div><div class="v" id="uptime">-</div></div>
    <div class="m"><div class="k">TX power</div><div class="v" id="txp">-</div></div>
    <div class="m"><div class="k">Channel width</div><div class="v" id="bw">-</div></div>
    <div class="m"><div class="k">TX queue</div><div class="v" id="txq">-</div></div>
    <div class="m"><div class="k">Lost on air</div><div class="v" id="airf">-</div></div>
    <div class="m" id="spim" style="display:none"><div class="k">Ethernet SPI</div><div class="v" id="spi">-</div></div>
    <div class="m" id="tempm" style="display:none"><div class="k">Chip temperature</div><div class="v" id="temp">-</div></div>
    <div class="m"><div class="k">LAN&rarr;Wi-Fi limit</div><div class="v" id="thr">-</div></div>
  </div>
  <div class="sub" style="margin:12px 0 0" id="detail"></div>
  <div id="cdbox" style="display:none;margin-top:10px;padding:10px;background:rgba(255,107,107,.08);
       border-radius:8px;border:1px solid rgba(255,107,107,.3)">
    <b class="bad">Crash dump present</b> <span class="hint">last unexpected reboot</span>
    <div id="cdtext" class="sub" style="margin:4px 0 8px"></div>
    <button class="sec" onclick="cdErase()" style="margin-top:0">Delete dump</button>
  </div>
  <div id="crbox" style="display:none;margin-top:10px">
    <b>Crash history</b> <span class="hint" id="crhint"></span>
    <div style="overflow-x:auto"><table id="crtab" class="sub" style="border-collapse:collapse;width:100%;font-size:12px;margin-top:4px"></table></div>
    <button class="sec" onclick="crErase()" style="margin-top:6px">Delete history</button>
  </div>
  <details id="dlbox" style="margin-top:10px" ontoggle="if(this.open)loadDisc()">
    <summary class="sub" style="cursor:pointer">Wi-Fi drops: reasons and history</summary>
    <div id="dlsum" class="sub" style="margin:6px 0"></div>
    <div style="overflow-x:auto"><table id="dltab" class="sub" style="border-collapse:collapse;width:100%;font-size:12px"></table></div>
    <button class="sec" onclick="loadDisc()" style="margin-top:6px">Refresh</button>
  </details>
  <details id="lgbox" style="margin-top:10px" ontoggle="logToggle(this.open)">
    <summary class="sub" style="cursor:pointer">Console log</summary>
    <pre id="lgpre" style="background:#0d1015;border-radius:8px;padding:8px;margin:6px 0 0;
         max-height:360px;overflow:auto;font-size:11px;line-height:1.35;white-space:pre-wrap;
         word-break:break-all"></pre>
    <span class="hint" id="lghint"></span>
    <button class="sec" onclick="$('lgpre').textContent=''" style="margin-top:6px">Clear view</button>
    <button class="sec" onclick="logPrev()" style="margin-top:6px">Previous boot</button>
    <pre id="lgprev" style="display:none;background:#0d1015;border-radius:8px;padding:8px;margin:6px 0 0;
         max-height:360px;overflow:auto;font-size:11px;line-height:1.35;white-space:pre-wrap;
         word-break:break-all"></pre>
  </details>
</div>

<div class="card">
  <h2>Configuration</h2>
  <label>Device name</label><input id="name">

  <div class="tag live" style="background:rgba(77,163,255,.15);color:var(--acc)">Network 1</div>
  <label>Wi-Fi <a href="#" onclick="scan();return false" style="color:var(--acc)">(scan)</a></label>
  <div class="row"><input id="ssid1" list="nets" placeholder="SSID">
    <input id="pass1" type="password" placeholder="Password"></div>
  <datalist id="nets"></datalist>
  <label>Management IP (static) <span class="hint">gateway = your router, not a second access path</span></label>
  <div class="row"><input id="ip" placeholder="IP"><input id="mask" placeholder="Netmask"><input id="gw" placeholder="Gateway"></div>

  <div class="tag live" style="background:rgba(77,163,255,.15);color:var(--acc)">Network 2 (optional)</div>
  <label>Wi-Fi</label>
  <div class="row"><input id="ssid2" list="nets" placeholder="SSID">
    <input id="pass2" type="password" placeholder="Password"></div>
  <label>Management IP <span class="hint">empty = use the same settings as network 1</span></label>
  <div class="row"><input id="ip2" placeholder="IP"><input id="mask2" placeholder="Netmask"><input id="gw2" placeholder="Gateway"></div>

  <div class="tag live" style="background:rgba(77,163,255,.15);color:var(--acc)">Network 3 (optional)</div>
  <label>Wi-Fi</label>
  <div class="row"><input id="ssid3" list="nets" placeholder="SSID">
    <input id="pass3" type="password" placeholder="Password"></div>
  <label>Management IP <span class="hint">empty = use the same settings as network 1</span></label>
  <div class="row"><input id="ip3" placeholder="IP"><input id="mask3" placeholder="Netmask"><input id="gw3" placeholder="Gateway"></div>
  <div class="sub" style="margin:6px 0 0">
    For several sites: the bridge tries the configured networks and applies
    the IP settings of the network it actually ended up in (detected by
    pinging each gateway). The same configuration then works at every site.
  </div>

  <label>MQTT broker (empty = off)</label>
  <div class="row"><input id="mqtt_host" placeholder="Host"><input id="mqtt_port" placeholder="1883" style="max-width:90px"></div>
  <div class="row"><input id="mqtt_user" placeholder="User"><input id="mqtt_pass" type="password" placeholder="Password"></div>

  <label>Telemetry interval (s)</label><input id="telemetry_s">
  <label>Admin key (protects firmware update and crash data, empty = open)</label>
  <input id="admin_pass" type="password" placeholder="leave empty = unchanged">

  <button class="hold" data-act="saverb">Save &amp; reboot</button>
  <button class="sec hold" data-act="reboot">Reboot only</button>
  <div id="msg"></div>
</div>

<div class="card">
  <h2>Tuning</h2>
  <div class="sub" style="margin:0 0 10px">
    The fields show the values <b>currently in effect</b>. The
    <i>Defaults</i> button below resets them to the firmware defaults.
    Only change them with a measurement at hand &ndash; the counters above
    refresh every 2&nbsp;s.
  </div>

  <div class="tag live">&#9679; takes effect IMMEDIATELY &ndash; no reboot</div>
  <label>TX power
    <span class="hint">the PHY only knows these steps and rounds down</span></label>
  <select id="tx_power">
    <option value="0">Default (maximum, 20 dBm)</option>
    <option value="80">20 dBm - full</option>
    <option value="72">18 dBm</option>
    <option value="66">16.5 dBm</option>
    <option value="60">15 dBm</option>
    <option value="56">14 dBm</option>
    <option value="52">13 dBm</option>
    <option value="44">11 dBm</option>
    <option value="34">8.5 dBm</option>
    <option value="28">7 dBm</option>
    <option value="20">5 dBm</option>
    <option value="8">2 dBm - minimum</option>
  </select>
  <div class="sub" style="margin:6px 0 0">
    More is not automatically better: the power amplifier becomes less
    linear at the top end. At short range, <i>turning it down</i> can
    increase throughput. It has no effect on reception.
  </div>

  <div class="row">
    <div><label>Retries LAN&rarr;Wi-Fi</label><input id="eth_tx_retries"></div>
    <div><label>Retries Wi-Fi&rarr;LAN</label><input id="wifi_tx_retries"></div>
  </div>
  <div class="sub" style="margin:6px 0 0">
    <b>The most important value.</b> It sets how often a frame is retried
    into Wi-Fi before it is dropped.
    <table style="width:100%;margin-top:8px;border-collapse:collapse;font-size:12px">
    <tr><td style="padding:2px 8px 2px 0"><b>1&ndash;2</b></td>
        <td>Only for TCP throughput tests. Drops fast, TCP recovers on its own.</td></tr>
    <tr><td style="padding:2px 8px 2px 0"><b>8</b></td>
        <td>Default. A good compromise for camera streams.</td></tr>
    <tr><td style="padding:2px 8px 2px 0"><b>12&ndash;20</b></td>
        <td>Weak signal or high bitrate. In testing, 16 reduced the loss
            from 13&nbsp;% to 0.05&nbsp;%.</td></tr>
    </table>
    With <b>UDP video every dropped packet becomes a picture glitch</b> &ndash;
    waiting is almost always better than dropping here. Values that are too
    high, however, block Ethernet reception, and then the loss happens
    invisibly in the DMA. So: as small as possible, as large as necessary.
    <br>The opposite direction rarely needs more than 1 &ndash; Ethernet
    practically always accepts.
  </div>

  <label>Temperature throttle LAN&rarr;Wi-Fi</label>
  <select id="thr_off">
    <option value="0">on (default)</option>
    <option value="1">off</option>
  </select>
  <label>Throttle start temperature
    <span class="hint">&deg;C, 0 = default (79)</span></label>
  <input id="thr_temp_c">
  <label>Max rate LAN&rarr;Wi-Fi
    <span class="hint">Mbit/s, 0 = no limit</span></label>
  <input id="e2w_max_mbit">
  <div class="sub" style="margin:6px 0 0">
    The temperature throttle lowers the LAN&rarr;Wi-Fi rate step by step while
    the chip runs hot (from the start temperature, 79&nbsp;&deg;C by default)
    and lifts it again once it is 3&nbsp;&deg;C cooler; tile <i>LAN&rarr;Wi-Fi limit</i> shows what is in
    force, the console log every change. The fixed maximum is for hardware
    whose power supply cannot carry sustained Wi-Fi transmission: an
    ESP32-C5 module feeding a W5500 from its own regulator resets above
    roughly 12&ndash;16&nbsp;Mbit/s, and the chip temperature rises too late to
    catch that. With a separate 3.3&nbsp;V supply the chip settles around
    81&nbsp;&deg;C at full load - raise the start temperature to about
    90&nbsp;&deg;C and remove the maximum. All three act within seconds, no
    reboot.
  </div>

  <label>TX queue cap LAN&rarr;Wi-Fi
    <span class="hint">frames, 0 = off</span></label>
  <input id="txq_cap">
  <div class="sub" style="margin:6px 0 0">
    How many frames may wait for the air at most. Without a cap a camera's
    I-frame fills every Wi-Fi TX buffer, and everything behind it &ndash;
    pings, this portal, the camera's own TCP &ndash; waits tens of
    milliseconds (tile <i>TX queue</i>). With a cap, larger frames beyond it
    are dropped at once instead; small ones (ACKs, ARP, ping) always pass.
    Lower latency and less heap use, paid for with some loss in bursts
    &ndash; use the calibration below to find out whether that is free on
    your link. Dropped frames are counted under <i>Dropped</i> as well.
  </div>

  <div style="margin-top:14px;padding:12px;background:#232833;border-radius:8px">
    <b style="font-size:13px">Calibrate all</b>
    <div class="sub" style="margin:4px 0 8px">
      Runs the three calibrations below one after another &ndash; retries,
      then TX power, then queue cap &ndash; each measuring with the results
      of the previous ones applied. About 5&nbsp;minutes; every result is
      saved as it is found and shown in its own section below.
      <br>All calibrations measure the <b>LAN&rarr;Wi-Fi</b> direction under
      <b>the traffic that is flowing right now</b>, so run them while the
      client does what it will normally do: a camera streaming to its
      recorder, a PC during its real workload. A synthetic load (e.g.
      <code>iperf3</code>) tunes for bulk transfer, not for a game or a call.
      Without traffic the run stops with &ldquo;too little traffic&rdquo;.
      <br>The 15-second retry step cannot tell apart values that both stay
      below 0.10&nbsp;% loss &ndash; on a real camera link 8 and 12 retries
      both passed, while hours of operation showed 8 losing five times more.
      So by default the run ends with the <b>long-run check</b> of the
      retries (see below), which settles exactly that.
    </div>
    <label>Then long-run check of the retries</label>
    <select id="allh">
      <option value="6" selected>6 hours (recommended)</option>
      <option value="12">12 hours</option>
      <option value="24">24 hours</option>
      <option value="2">2 hours (quick, less certain)</option>
      <option value="0">skip</option>
    </select>
    <div class="row" style="margin-top:8px">
      <button onclick="autotune('all',$('allh').value)" style="margin-top:0">Calibrate all</button>
      <button class="sec" onclick="autotune('stop')" style="margin-top:0">Stop running calibration</button>
    </div>
    <div id="allmsg" style="margin-top:10px;font-size:13px"></div>
  </div>

  <div style="margin-top:14px;padding:12px;background:#232833;border-radius:8px">
    <b style="font-size:13px">Automatic calibration</b>
    <div class="sub" style="margin:4px 0 8px">
      Measures the loss rate for about 15&nbsp;s per step, starting at the
      current value: if that misses the target it tries higher values, then
      smaller ones while the loss stays low (candidates 2&ndash;20). The
      <b>smallest</b> value that stays below 0.10&nbsp;% is chosen and saved
      permanently. A step that makes the stream collapse is aborted after a
      few seconds.
      <br><b>Traffic must be flowing</b> &ndash; so let the camera stream.
      Usually takes 3&ndash;6 steps of about 19&nbsp;s; the picture may be
      disturbed now and then while lower values are being tested.
    </div>
    <button onclick="autotune()" style="margin-top:0">Start calibration</button>
    <div id="atmsg" style="margin-top:10px;font-size:13px"></div>
    <table id="attab" style="margin-top:6px;font-size:12px;border-collapse:collapse"></table>

    <b style="font-size:13px;display:block;margin-top:14px">Long-run check</b>
    <div class="sub" style="margin:4px 0 8px">
      Compares one step below the current value, the value itself and two
      steps above under real operation: they take turns in 5-minute blocks,
      so each sees the same mix of busy and quiet hours. At the end the
      <b>smallest</b> value whose loss is at most 1.5&times; the best one is
      saved. A value losing more than 1&nbsp;% is dropped from the rotation
      at once. Use the bridge normally meanwhile; a reboot ends the check
      without a result. Run it after the short calibration &ndash; that one
      cannot separate values below 0.10&nbsp;% loss.
    </div>
    <div class="row">
      <select id="lrh">
        <option value="6" selected>6 hours (recommended)</option>
        <option value="12">12 hours</option>
        <option value="24">24 hours</option>
        <option value="2">2 hours (quick, less certain)</option>
      </select>
      <button onclick="autotune('long',$('lrh').value)" style="margin-top:0">Start long-run check</button>
    </div>
    <div id="lrmsg" style="margin-top:10px;font-size:13px"></div>
    <table id="lrtab" style="margin-top:6px;font-size:12px;border-collapse:collapse"></table>
  </div>

  <div style="margin-top:14px;padding:12px;background:#232833;border-radius:8px">
    <b style="font-size:13px">TX power calibration</b>
    <div class="sub" style="margin:4px 0 8px">
      Measures the PHY's power steps from 13 to 20&nbsp;dBm against the live
      stream, about 19&nbsp;s each: the loss and how long frames wait in the
      Wi-Fi TX queue (a weak uplink makes the queue drain slowly long before
      frames get lost - that is what drives ping times up). Starts at the
      current value, tries every higher step, then lower ones while they keep
      up. The <b>highest</b> power that is as good as the best one is chosen
      and saved: power is only reduced where more of it measurably hurts,
      because link margin is what carries the stream through interference a
      short measurement never sees. Steps the chip cannot deliver (it may cap
      below 20&nbsp;dBm, see the Driver column) are measured once.
      <b>Traffic must be flowing.</b> Takes about 2&ndash;3 minutes.
    </div>
    <button onclick="autotune('txp')" style="margin-top:0">Start TX power calibration</button>
    <div id="ptmsg" style="margin-top:10px;font-size:13px"></div>
    <table id="pttab" style="margin-top:6px;font-size:12px;border-collapse:collapse"></table>
  </div>

  <div style="margin-top:14px;padding:12px;background:#232833;border-radius:8px">
    <b style="font-size:13px">TX queue cap calibration</b>
    <div class="sub" style="margin:4px 0 8px">
      Measures without a cap first (the reference), then with caps of 24, 16,
      12 and 8 frames, about 19&nbsp;s each, and stops at the first one that
      loses noticeably more than the reference or delivers less than 90&nbsp;%
      of its throughput. The <b>smallest</b> cap that passed is saved
      &ndash; or <i>off</i> if none did. The picture may stutter during the
      smaller caps. <b>Traffic must be flowing.</b> Takes 1&ndash;2 minutes.
      After applying a cap, keep an eye on <i>Dropped</i> over a few hours.
    </div>
    <button onclick="autotune('cap')" style="margin-top:0">Start queue cap calibration</button>
    <div id="qcmsg" style="margin-top:10px;font-size:13px"></div>
    <table id="qctab" style="margin-top:6px;font-size:12px;border-collapse:collapse"></table>
  </div>

  <div style="margin-top:14px;padding:12px;background:#232833;border-radius:8px">
    <b style="font-size:13px">Watchdog</b>
    <div class="sub" style="margin:4px 0 8px">
      Detects when the bridge stops delivering properly despite being
      "connected", and escalates in up to three levels depending on the
      symptom: <b>Ethernet link up, but no traffic from the camera</b>
      &rarr; only the Ethernet driver is restarted (Wi-Fi stays untouched);
      <b>Wi-Fi-side problems</b> (high loss rate, frequent reassociations,
      failed gateway probe) &rarr; disconnect and reconnect Wi-Fi
      (Ethernet/camera stay untouched); <b>if none of that helps</b> &rarr;
      full reboot with a deliberate coredump, so the reason can be traced
      afterwards. None of this counts as a failed boot for the boot-loop
      protection.
    </div>
    <select id="wd_enable">
      <option value="0">Off (default)</option>
      <option value="1">On</option>
    </select>
    <label>Allowed actions
      <span class="hint">order is always Ethernet &rarr; Wi-Fi &rarr; reboot</span></label>
    <select id="wd_actions">
      <option value="7">Ethernet reset &rarr; Wi-Fi reconnect &rarr; reboot (default)</option>
      <option value="3">Ethernet reset &rarr; Wi-Fi reconnect, never reboot</option>
      <option value="1">Ethernet reset only (Wi-Fi is never touched)</option>
      <option value="5">Ethernet reset &rarr; reboot, no Wi-Fi reconnect</option>
      <option value="2">Wi-Fi reconnect only</option>
      <option value="6">Wi-Fi reconnect &rarr; reboot, no Ethernet reset</option>
      <option value="4">Reboot only</option>
    </select>
    <div class="sub" style="margin:6px 0 0">
      Disabled levels are skipped. Once the highest allowed level is reached
      and the problem persists, it is repeated (e.g. with "Ethernet reset
      only", another Ethernet reset). An Ethernet reset only happens for
      "link up, but no traffic from the camera" &ndash; Wi-Fi symptoms
      without an allowed Wi-Fi reconnect/reboot are only logged.
    </div>
    <label>Bad measurement windows (20&nbsp;s each) before the next level</label>
    <input id="wd_bad_windows">
    <div class="sub" style="margin:10px 0 4px">
      Trigger manually, independent of the watchdog state above (e.g. when
      the camera is hung right now and you don't want to wait for the next
      automatic cycle):
    </div>
    <button class="sec" onclick="ethReset()" style="margin-top:0">Restart Ethernet</button>
    <div id="ethmsg" style="margin-top:8px;font-size:13px"></div>
  </div>

  <div style="margin-top:14px;padding:12px;background:#232833;border-radius:8px">
    <b style="font-size:13px">Portal idle reboot</b>
    <div class="sub" style="margin:4px 0 8px">
      While the bridge sits in portal mode (setup Wi-Fi, 192.168.4.1) it is
      invisible on the normal network. Without active portal use (page
      loaded, saved or scanned &ndash; the automatic
      2&#8209;second status poll deliberately does NOT count) it tries a
      normal start with the stored credentials after this time. Only applies
      if a Wi-Fi network is configured at all &ndash; otherwise a fresh or
      factory-reset device would reboot in circles for nothing.
    </div>
    <label>Time until reboot attempt (s)</label>
    <input id="ap_idle_reboot_s">
  </div>

  <div style="margin-top:14px;padding:12px;background:#232833;border-radius:8px">
    <b style="font-size:13px">Active roaming</b>
    <div class="sub" style="margin:4px 0 8px">
      The strongest visible access point is chosen only ONCE, at connect
      time &ndash; afterwards the bridge stays on that AP, even if a much
      better one appears later (e.g. another mesh node). With a weak signal
      it therefore regularly checks whether an AP with the same SSID and a
      sufficiently better signal is available, and switches to it if so.
      With a good signal it does not scan at all, to avoid wasting airtime.
    </div>
    <select id="roam_enable">
      <option value="0">Off (default)</option>
      <option value="1">On - switch to the stronger AP automatically when the signal is weak</option>
    </select>
    <div class="row" style="margin-top:10px">
      <div><label>Check interval (s)</label>
        <input id="roam_check_s"></div>
      <div><label>Threshold (dBm, negative)</label>
        <input id="roam_rssi_threshold"></div>
      <div><label>Minimum advantage (dB)</label>
        <input id="roam_margin_db"></div>
    </div>
  </div>

  <div class="tag reboot">&#9679; takes effect only after a REBOOT</div>
  <div class="row">
    <div><label>Static RX buffers</label>
      <input id="static_rx_buf"></div>
    <div><label>Dynamic RX buffers</label>
      <input id="dynamic_rx_buf"></div>
  </div>
  <div class="row">
    <div><label>Dynamic TX buffers</label>
      <input id="dynamic_tx_buf"></div>
    <div><label>RX block-ack window</label>
      <input id="rx_ba_win"></div>
  </div>
  <div id="bufeff" class="sub" style="margin:6px 0 0;color:#e0b050"></div>
  <div class="sub" style="margin:6px 0 0">
    <b>More is worse here.</b> TX and RX buffers share the same memory:
    raising the TX buffers to 64 dropped the free heap from 111 to
    9&nbsp;kB and halved the throughput.
    The heap is shown in the status above &ndash; if it falls below about
    30&nbsp;kB, it is too much.
  </div>

  <div class="row">
    <div><label>Wi-Fi connect attempt: timeout (s)</label>
      <input id="wifi_connect_timeout_s"></div>
    <div><label>Wi-Fi connect attempt: retries</label>
      <input id="wifi_connect_retries"></div>
  </div>
  <div class="sub" style="margin:6px 0 0">
    How long and how often an SSID is tried at boot before the next
    configured SSID is tried, or before giving up and switching to the
    portal. Several attempts cover the case where the router still briefly
    holds an old Wi-Fi session of the (cloned) MAC after a quick reboot and
    the first attempt fails because of it.
  </div>

  <label>Channel width</label>
  <select id="ht40">
    <option value="0">HT20 - 20 MHz (default)</option>
    <option value="1">HT40 - 40 MHz, if the AP supports it</option>
  </select>
  <label>802.11b rates</label>
  <select id="no_11b">
    <option value="0">keep enabled (default)</option>
    <option value="1">disable - saves airtime</option>
  </select>
  <div id="bandrow" style="display:none">
    <label>Wi-Fi band</label>
    <select id="wifi_band">
      <option value="0">Both, prefer 5 GHz (default)</option>
      <option value="1">5 GHz only</option>
      <option value="2">2.4 GHz only</option>
      <option value="3">Both, strongest signal</option>
    </select>
    <div class="sub" style="margin:6px 0 0">
      With "prefer 5 GHz" a 5 GHz AP is used whenever its signal is at least
      -75&nbsp;dBm, and a bridge that had to fall back to 2.4&nbsp;GHz moves
      back to 5&nbsp;GHz on its own once a usable AP is in range (checked at
      the roaming interval, even with roaming off). "Only" settings are
      enforced by the radio itself and also apply to every reconnect.
    </div>
  </div>
  <div class="sub" style="margin:6px 0 0">
    HT40 doubles the gross rate but occupies twice the bandwidth and picks
    up correspondingly more interference. In crowded 2.4&nbsp;GHz
    environments HT20 is often just as fast and disturbs the neighbors less
    &ndash; in testing the difference was not measurable. The width actually
    negotiated is shown in the status above.
  </div>

  <button onclick="save(false)">Save</button>
  <button class="sec" onclick="defaults()">Defaults</button>
  <button class="sec hold" data-act="reboot">Reboot</button>
  <div id="tmsg"></div>
</div>

<div class="card">
  <h2>Firmware update</h2>
  <div class="sub" style="margin:0 0 10px">
    Upload the file <code>.pio/build/wt32-eth01/firmware.bin</code>.
    The bridge pauses forwarding meanwhile and reboots afterwards.
    If the new firmware does not boot cleanly, the bootloader automatically
    falls back to the previous version.
  </div>
  <input type="file" id="fw" accept=".bin">
  <label>Admin key</label><input id="okey" type="password">
  <div class="bar" style="margin-top:12px"><i id="pbar" style="width:0"></i></div>
  <button onclick="upload()">Upload &amp; flash</button>
  <div id="omsg"></div>
</div>
</div>
<div id="dirty" hidden role="status">
  <b>Unsaved changes</b> <span id="dirtyn" class="hint"></span>
  <div class="row" id="dirtybtns">
    <button onclick="dirtySave()">Save</button>
    <button class="sec" onclick="dirtyCancel()">Discard</button>
  </div>
  <div id="dirtymsg"></div>
</div><script>
const $=i=>document.getElementById(i);
/* In provisioning mode the STA data path is not running yet, so even the TX
   power does not take effect immediately - the message after saving must
   say so, otherwise one waits for an effect that never comes. */
let PROV=false;
/* Last seen calibration state: the result is only filled into the field on
   the TRANSITION to "done". Doing it on every 2 s poll while the board
   reports "done" would make a manually edited value jump back before it
   is saved. */
let ATPREV=-1, PTPREV=-1, PTRES=0, QCPREV=-1, QCRES=-1, LRPREV=-1, LRRES=0, LRPOLL=0;
function fmt(k){return k>1000?(k/1000).toFixed(1)+' Mbit/s':k+' kbit/s'}
function dur(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);
  return d?d+'d '+h+'h':(h?h+'h '+m+'m':m+'m')}
function sdur(s){return s<60?s+'s':dur(s)}
/* Unix seconds -> browser local time; 0 means "clock was not set". */
function ts(e){return e?new Date(e*1000).toLocaleString():''}
const TD=(t,h)=>'<'+(h?'th':'td')+' style="text-align:left;padding:2px 6px;border-bottom:1px solid rgba(128,128,128,.25);vertical-align:top">'+t+'</'+(h?'th':'td')+'>';
/* esp_reset_reason_t -> plain text. 255 = imported from before the history existed. */
const RST={4:'Crash (panic)',5:'Interrupt watchdog',6:'Task watchdog',7:'Hardware watchdog',9:'Brownout (voltage)',14:'Power glitch',15:'CPU lockup',255:'Crash (imported)'};
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
/* Load the crash history (admin key from the firmware field, if set). */
async function checkCrashes(){
  try{
    const r=await fetch('/api/crashes',{headers:cdHeaders()});
    if(!r.ok){$('crbox').style.display='none';return}
    const j=await r.json();
    if(!j.recs.length){$('crbox').style.display='none';return}
    $('crbox').style.display='block';
    $('crhint').textContent=j.recs.length+(j.recs.length===1?' entry':' entries')+', newest first'+(j.now?'':' - bridge clock not set yet');
    $('crtab').innerHTML='<tr>'+['When','Kind','Task / reason','Build'].map(t=>TD(t,1)).join('')+'</tr>'+
      j.recs.map(c=>{
        const when=c.e?ts(c.e):(c.f&4?'unknown (before history existed)':'unknown (clock not set)');
        let kind=(c.f&2)?'Watchdog reboot (intended)':(RST[c.rst]||('Reset '+c.rst));
        const det=(c.f&1)?'<b>'+esc(c.task)+'</b> @ 0x'+c.pc+(c.reason?'<br>'+esc(c.reason):'')+
            (c.bt.length?'<br><span class="hint">BT: '+c.bt.map(a=>'0x'+a).join(' ')+(c.f&8?' (incomplete)':'')+'</span>':'')
          :(c.f&16?'<span class="hint">no new dump written</span>':'<span class="hint">no dump (reset without coredump)</span>');
        return '<tr>'+TD(when+(c.up>=0?'<br><span class="hint">after '+sdur(c.up)+' uptime</span>':''))+
          TD(kind)+TD(det)+TD(esc(c.build))+'</tr>';
      }).join('');
  }catch(e){}
}
async function crErase(){
  if(!confirm('Delete crash history?'))return;
  await fetch('/api/crashes',{method:'DELETE',headers:cdHeaders()});
  checkCrashes();
}
/* Who disconnected - anything except 0 is the firmware itself, not a radio problem. */
const DSRC=['external','connecting','watchdog','roaming','restart','failover'];
/* Load and render the Wi-Fi disconnect statistics and log. */
async function loadDisc(){
  try{
    const j=await(await fetch('/api/disconnects')).json();
    /* Show real drops (existing connection lost) and the failed reconnect
       attempts that follow separately - otherwise the follow-up failures
       hide the actual cause. */
    $('dlsum').innerHTML='Since this boot (external only): <b>'+j.kicks+'</b> real drops'+
      (j.by_reason.length?' - '+j.by_reason.map(x=>'<b>'+x.rs+'</b> ('+x.r+') '+x.n+'&times;').join(', ')+
        (j.rest?', other '+j.rest+'&times;':''):'')+
      '<br>plus <b>'+j.fails+'</b> failed reconnect attempts';
    const td=TD, empty=e=>e.bssid==='00:00:00:00:00:00';
    $('dltab').innerHTML=j.log.length
      ? '<tr>'+['When','Kind','Reason','Trigger','RSSI','AP','Down'].map(t=>td(t,1)).join('')+'</tr>'+
        j.log.map(e=>'<tr'+(e.up?'':' style="opacity:.6"')+'>'+
          td((e.e?ts(e.e)+'<br><span class="hint">':'')+
             (e.prev?(e.prev===1?'previous boot':e.prev+' boots ago')+', at '+sdur(e.t):sdur(e.ago)+' ago')+
             (e.e?'</span>':''))+
          td(e.up?(e.src?'Disconnect':'<b class="bad">Drop</b>'):'Failed attempt')+
          td('<b>'+e.rs+'</b> ('+e.r+')')+
          td(e.src?'<span class="hint">'+(DSRC[e.src]||e.src)+'</span>':'external')+
          td(empty(e)?'-':e.rssi+' dBm')+td(empty(e)?'-':e.bssid)+
          td(e.down<0?'-':sdur(e.down))+'</tr>').join('')
      : '';
  }catch(e){$('dlsum').textContent='Could not load /api/disconnects'}
}
/* Status poll (every 2 s): refresh the tiles and the detail line. */
async function tick(){
  try{const s=await(await fetch('/api/status')).json();
    PROV=s.prov;
    $('mode').textContent=s.prov?'Initial setup - bridge not active yet':
      (s.wifi?'Bridge active':'Wi-Fi disconnected');
    $('rssi').textContent=s.wifi?s.rssi+' dBm':'-';
    /* -30 dBm = excellent, -90 = dead */
    $('rssibar').style.width=Math.max(0,Math.min(100,(s.rssi+95)*100/65))+'%';
    $('ch').textContent=s.wifi?s.ch+(s.ch>14?' (5 GHz)':' (2.4 GHz)'):'-';
    $('up').textContent=fmt(s.kbps_up); $('down').textContent=fmt(s.kbps_down);
    $('drops').textContent=s.drop_up+' / '+s.drop_down;
    $('drops').className='v '+((s.drop_up+s.drop_down)?'bad':'ok');
    $('uptime').textContent=dur(s.uptime);
    /* txp comes in 0.25 dBm steps and is the driver's actual value, not the
       configured target. */
    $('txp').textContent=(s.wifi&&s.txp>=0)?(s.txp/4).toFixed(2).replace(/\.?0+$/,'')+' dBm':'-';
    /* Actual value from the driver: shows whether HT40 was really negotiated. */
    $('bw').textContent=(s.wifi&&s.bw)?s.bw+' MHz':'-';
    /* Only SPI Ethernet (W5500) has a bus clock to show; the EMAC reports 0. */
    if(s.spi){$('spim').style.display='';$('spi').textContent=s.spi+' MHz'}
    if(s.temp>-32768){$('tempm').style.display='';
      $('temp').textContent=(s.temp/10).toFixed(1)+' \u00b0C (max '+(s.temp_max/10).toFixed(1)+')'}
    $('thr').textContent=(s.thr?(s.thr/1000).toFixed(1)+' Mbit/s':'none')+
      (s.drop_thr?' \u00b7 '+s.drop_thr+' dropped':'');
    /* Calibration progress. at: 0=idle 1=running 2=done 3=too little traffic */
    /* The number of steps is not known in advance (up first, then down,
       stop at the first outlier) - so no remaining time, only the progress. */
    if(s.at===1) $('atmsg').textContent='Calibration running - measurement '+(s.at_s+1)+
                 ', testing '+s.at_k+' retries (about 19 s each, usually 3-6 measurements)';
    else if(s.at===2){ $('atmsg').textContent='Done - chosen: '+s.at_r+' retries, saved.';
                       if(ATPREV===1){ $('eth_tx_retries').value=s.at_r;
                         /* The board saved it itself - so it is not an
                            unsaved change. */
                         if(SNAP){SNAP.eth_tx_retries=String(s.at_r);dirtyCheck()} } }
    else if(s.at===3) $('atmsg').textContent='Aborted: too little traffic during the measurement. '+
                      'Let the camera stream and start again.';
    /* The tables only change when a measurement finishes - fill them on the
       first poll, refresh them while a run is active and once on the
       transition out of it. */
    /* A long-run check only changes its table every few minutes - poll it
       every 15th status tick (30 s), not every 2 s for hours. */
    const lrTick=s.lr===1&&(++LRPOLL%15===0);
    if(ATPREV===-1||s.at===1||s.pt===1||s.qc===1||lrTick||(ATPREV===1&&s.at!==1)||
       (PTPREV===1&&s.pt!==1)||(QCPREV===1&&s.qc!==1)||(LRPREV===1&&s.lr!==1)) atTables();
    ATPREV=s.at;
    const dbm=v=>(v/4).toFixed(2).replace(/\.?0+$/,'')+' dBm';
    if(s.pt===1) $('ptmsg').textContent='TX power calibration running - measurement '+(s.pt_s+1)+
                 ', testing '+dbm(s.pt_k)+' (about 19 s each)';
    else if(s.pt===2){ PTRES=s.pt_r; $('ptmsg').textContent='Done - chosen: '+dbm(s.pt_r)+', saved.';
                       if(PTPREV===1){ $('tx_power').value=s.pt_r;
                         if(SNAP){SNAP.tx_power=String(s.pt_r);dirtyCheck()} } }
    else if(s.pt===3) $('ptmsg').textContent='Aborted: too little traffic during the measurement. '+
                      'Let the camera stream and start again.';
    PTPREV=s.pt;
    if(s.qc===1) $('qcmsg').textContent='Queue cap calibration running - measurement '+(s.qc_s+1)+
                 ', testing '+(s.qc_k?'cap '+s.qc_k:'without cap')+' (about 19 s each)';
    else if(s.qc===2){ QCRES=s.qc_r; $('qcmsg').textContent='Done - chosen: '+
                         (s.qc_r?'cap '+s.qc_r+' frames':'no cap (every cap cost too much)')+', saved.';
                       if(QCPREV===1){ $('txq_cap').value=s.qc_r;
                         if(SNAP){SNAP.txq_cap=String(s.qc_r);dirtyCheck()} } }
    else if(s.qc===3) $('qcmsg').textContent='Aborted: too little traffic during the measurement. '+
                      'Let the camera stream and start again.';
    QCPREV=s.qc;
    /* Long-run check: progress, result. */
    const hm=m=>Math.floor(m/60)+' h '+String(m%60).padStart(2,'0')+' min';
    if(s.lr===1) $('lrmsg').textContent='Long-run check running - '+hm(s.lr_el)+' of '+hm(s.lr_tot)+
                 ', now testing '+s.lr_k+' retries (blocks of 5 min)';
    else if(s.lr===2){ LRRES=s.lr_r; $('lrmsg').textContent='Done - chosen: '+s.lr_r+' retries, saved.';
                       if(LRPREV===1){ $('eth_tx_retries').value=s.lr_r;
                         if(SNAP){SNAP.eth_tx_retries=String(s.lr_r);dirtyCheck()} } }
    else if(s.lr===3) $('lrmsg').textContent='No result: not enough traffic, or every value lost more '+
                      'than 1 %. The previous value is active again.';
    LRPREV=s.lr;
    /* "Calibrate all": 1-4 = running stage, 5 done, 6 stopped. */
    if(s.all>=1&&s.all<=4) $('allmsg').textContent='Running - '+
                 ['retries','TX power','queue cap','long-run check of the retries'][s.all-1]+
                 ' (details in its section below)';
    else if(s.all===5) $('allmsg').textContent='Done - all results saved (see below).';
    else if(s.all===6) $('allmsg').textContent='Stopped - a step had too little traffic or was '+
                 'stopped / overridden by a manual save. Results found up to then are kept.';
    /* Mean frames waiting in the driver queue and the resulting wait. A few
       ms is normal; tens to hundreds of ms mean the uplink cannot keep up. */
    $('txq').textContent=(s.wifi&&s.txq_ms!==undefined)?s.txq_ms+' ms':'-';
    $('txq').className='v '+(s.txq_ms>=50?'bad':s.txq_ms>=15?'warn':'ok');
    $('txq').title=(s.txq_x10/10).toFixed(1)+' frames on average, '+s.txq+' now';
    $('airf').textContent=s.air_fail!==undefined?s.air_fail:'-';
    $('airf').className='v '+(s.air_fail?'bad':'ok');
    $('airf').title=s.air_ok+' frames delivered';
    $('detail').innerHTML='Client: <b>'+s.client_mac+'</b> &middot; IP: <b>'+(s.client_ip||'-')+'</b>'+
      ' &middot; Name: <b>'+(s.client_name||'-')+'</b><br>Connected to: <b>'+
      (s.ssid||'-')+'</b> ('+s.bssid+')'+
      /* Which network slot, flagged when it is not the first one - e.g.
         after a failover - and how often the bridge switched. */
      (s.net?' &middot; network <b class="'+(s.net>1?'warn':'')+'">'+s.net+'</b>':'')+
      (s.fo?' <span class="hint">('+s.fo+' switch'+(s.fo>1?'es':'')+')</span>':'')+
      ' &middot; Ethernet: <span class="'+(s.eth?'ok':'bad')+'">'+(s.eth?'connected':'no link')+
      '</span> &middot; Wi-Fi drops: <b>'+(s.wifi_kicks!==undefined?s.wifi_kicks:s.wifi_disc)+'</b>'+
      (s.wifi_kicks!==undefined&&s.wifi_disc>s.wifi_kicks?' <span class="hint">(+'+(s.wifi_disc-s.wifi_kicks)+' failed attempts/own disconnects)</span>':'')+
      (s.disc_r ? ' (last <b>'+s.disc_rs+'</b> '+sdur(s.disc_ago)+' ago'+
        (s.disc_src?', '+(DSRC[s.disc_src]||s.disc_src):'')+')' : '')+
      (s.wd_probe ? ' &middot; Watchdog probe: <span class="'+(s.wd_probe===1?'ok':'bad')+'">'+
        (s.wd_probe===1?'gateway reached':'gateway not reached')+'</span>' : '')+
      (s.wd_reconnects ? ' &middot; Watchdog Wi-Fi reconnects: <b>'+s.wd_reconnects+'</b>' : '')+
      (s.wd_eth_resets ? ' &middot; Watchdog Ethernet resets: <b>'+s.wd_eth_resets+'</b>' : '')+
      (s.roam_count ? ' &middot; AP switches (roaming): <b>'+s.roam_count+'</b>' : '')+
      (s.wd_last_reason ? ' &middot; last watchdog reboot: <span class="bad">'+
        (['','loss ratio','reconnects','gateway probe','no camera traffic'][s.wd_last_reason]||'?')+'</span>' : '')+
      ' &middot; Heap: <b>'+Math.round(s.heap/1024)+' kB</b>'+
      (s.heap_min!==undefined
        ? ' (minimum '+Math.round(s.heap_min/1024)+' kB, DMA '+
          Math.round(s.heap_dma/1024)+' kB, largest block '+
          Math.round(s.heap_blk/1024)+' kB)'
        : '')+
      (s.build ? ' &middot; Build: <b>'+s.build+'</b>' : '');
  }catch(e){}
}
/* One field name per entry, in exactly this order when saving too.
   Password fields are deliberately NOT part of load() - they never come
   back from the board; an empty field means "leave unchanged" on save. */
const CFG=['name','ssid1','ssid2','ssid3','ip','mask','gw','ip2','mask2','gw2','ip3','mask3','gw3',
           'mqtt_host','mqtt_port','mqtt_user','telemetry_s',
           'tx_power','eth_tx_retries','wifi_tx_retries',
           'static_rx_buf','dynamic_rx_buf','dynamic_tx_buf','rx_ba_win',
           'ht40','no_11b','wd_enable','wd_actions','wd_bad_windows',
           'wifi_connect_timeout_s','wifi_connect_retries','ap_idle_reboot_s',
           'roam_enable','roam_check_s','roam_rssi_threshold','roam_margin_db','txq_cap',
           'wifi_band','thr_off','e2w_max_mbit','thr_temp_c'];
/* Fill the form from /api/config and take the unsaved-changes snapshot. */
async function load(){
  const c=await(await fetch('/api/config')).json();
  DEF=c.def||null;
  $('bandrow').style.display=c.band5g?'':'none';
  for(const k of CFG) if(c[k]!==undefined)$(k).value=c[k];
  snapshot();
  /* Buffers: the form shows the SAVED value (otherwise a change made before
     the reboot would be lost on the next save). If the running value
     differs, it is shown as a note below. */
  const e=c.eff||{},d=['static_rx_buf','dynamic_rx_buf','dynamic_tx_buf','rx_ba_win']
    .filter(k=>e[k]!==undefined&&e[k]!=c[k]);
  $('bufeff').textContent=d.length?'Still in effect: '+d.map(k=>$(k).previousElementSibling
    .textContent+' '+e[k]).join(', ')+' - the saved value applies after the reboot.':'';
}
async function scan(){
  $('msg').textContent='Scanning...';
  const r=await(await fetch('/api/scan')).json();
  /* Via DOM instead of innerHTML: SSIDs are untrusted data, a neighbor AP
     named "<img src=x onerror=...>" would otherwise run script in the portal. */
  const d=$('nets');d.textContent='';
  r.forEach(n=>{const o=document.createElement('option');o.value=n.ssid;
    o.textContent=n.ssid+' ('+n.rssi+' dBm)';d.appendChild(o)});
  $('msg').textContent=r.length+' networks found';
}
/* rb=true -> save and reboot (Wi-Fi credentials, IP, buffers...).
   rb=false -> save only; TX power and retries take effect immediately, the
   board stays online and the counters above show right away whether the
   change helped. That is what the tuning panel is for.
   tgtOverride: element for status messages (default: the card's own). */
async function save(rb,tgtOverride){
  const body=CFG.concat(PW).map(k=>k+'='+encodeURIComponent($(k).value)).join('&');
  const tgt=tgtOverride||(rb?$('msg'):$('tmsg'));
  tgt.textContent='Saving...';
  let r;
  try{r=await fetch('/api/config',{method:'POST',body:body,
    headers:{'Content-Type':'application/x-www-form-urlencoded'}});}catch(e){}
  if(!r||!r.ok){tgt.textContent='Error while saving';return false}
  /* Saved = new baseline. Clear the password fields: empty means
     "unchanged", a password left in the field would otherwise count as a
     change and be sent again on every save. */
  for(const k of PW)$(k).value='';
  snapshot();
  if(rb){
    tgt.textContent='Saved.';
    setTimeout(()=>reboot(tgt),600);
  }else if(PROV){
    tgt.textContent='Saved. In provisioning mode none of this takes effect yet - '
      +'the values are applied at the first bridge start.';
  }else{
    tgt.textContent='Saved. TX power, retries and watchdog are active immediately - '
      +'buffers, channel width and 802.11b only after a reboot.';
  }
  return true;
}
let DEF=null;
/* Fill the numeric fields with the firmware defaults. Nothing is saved until
   Save is clicked - so the values can be reviewed first. */
function defaults(){
  if(!DEF){$('tmsg').textContent='Defaults not loaded yet';return}
  for(const k in DEF) if($(k)) $(k).value=DEF[k];
  $('tx_power').value='0';
  $('txq_cap').value='0';
  $('thr_off').value='0';
  $('e2w_max_mbit').value='0';
  $('thr_temp_c').value='0';
  $('tmsg').textContent='Defaults filled in - save to apply.';
  dirtyCheck();   /* set by script - does not fire an input event */
}
/* Start the retry calibration, with kind 'txp' / 'cap' the TX power or
   queue cap one, 'long' the long-run retry check, 'all' the chain (h =
   hours of the long-run check, 0 = none); 'stop' aborts whatever runs. */
async function autotune(kind,h){
  const m=$({txp:'ptmsg',cap:'qcmsg',all:'allmsg',stop:'allmsg',long:'lrmsg'}[kind]||'atmsg');
  m.textContent=kind==='stop'?'Stopping...':'Starting...';
  try{
    const j=await(await fetch('/api/autotune'+(kind?'?kind='+kind+(h!==undefined?'&h='+h:''):''),
                              {method:'POST'})).json();
    m.textContent=kind==='stop'?'Stopped - the saved values are active again.'
                 :j.ok?'Calibration running - please keep traffic flowing.'
                      :('Not started: '+(j.err||'unknown'));
  }catch(e){m.textContent='Error while starting'}
}
/* Fill both calibration tables from /api/autotune: one row per measured
   candidate, the chosen one in bold. Called while a run is active and once
   when it ends. */
async function atTables(){
  try{
    const j=await(await fetch('/api/autotune')).json(), td=TD;
    const pct=v=>v<0?'<b class="bad">collapse</b>':(v/100).toFixed(2)+' %';
    const dbm=v=>(v/4).toFixed(2).replace(/\.?0+$/,'')+' dBm';
    $('attab').innerHTML=j.retries.length?'<tr>'+td('Retries',1)+td('Loss',1)+'</tr>'+
      j.retries.map(e=>'<tr>'+td(e.k)+td(pct(e.loss))+'</tr>').join(''):'';
    $('pttab').innerHTML=j.txp.length?'<tr>'+['Power','Driver','Loss','Queue delay'].map(t=>td(t,1)).join('')+'</tr>'+
      j.txp.map(e=>'<tr'+(e.k===PTRES?' style="font-weight:bold"':'')+'>'+td(dbm(e.k))+td(dbm(e.act))+
        td(pct(e.loss))+td(e.delay<0?'-':(e.delay/10).toFixed(1)+' ms')+'</tr>').join(''):'';
    $('qctab').innerHTML=j.cap&&j.cap.length?'<tr>'+['Cap','Loss','Queue delay','Throughput'].map(t=>td(t,1)).join('')+'</tr>'+
      j.cap.map(e=>'<tr'+(e.k===QCRES?' style="font-weight:bold"':'')+'>'+td(e.k?e.k+' frames':'off')+
        td(pct(e.loss))+td(e.delay<0?'-':(e.delay/10).toFixed(1)+' ms')+
        td(e.kbps<0?'-':(e.kbps/1000).toFixed(2)+' Mbit/s')+'</tr>').join(''):'';
    /* Loss in ppm-accurate percent: long runs separate values far below
       0.01 %, so three decimals. */
    $('lrtab').innerHTML=j.long&&j.long.length?'<tr>'+['Retries','Packets','Lost','Loss'].map(t=>td(t,1)).join('')+'</tr>'+
      j.long.map(e=>{const a=e.pkts+e.drops;return '<tr'+(e.k===LRRES?' style="font-weight:bold"':'')+'>'+
        td(e.k)+td(e.pkts.toLocaleString())+td(e.drops.toLocaleString())+
        td(e.bad?'<b class="bad">&gt; 1 % - dropped</b>':a?(100*e.drops/a).toFixed(3)+' %':'-')+'</tr>'}).join(''):'';
  }catch(e){}
}
/* Trigger a reboot and reload the page as soon as the board answers again -
   otherwise one faces a dead page without knowing when it comes back. Only
   start asking after 8 s: before that the old instance might still answer. */
async function reboot(tgt){
  tgt=tgt||$('msg');
  document.querySelectorAll('button.hold').forEach(b=>b.disabled=true);
  try{await fetch('/api/reboot',{method:'POST'})}catch(e){}
  REBOOTING=true;
  const t0=Date.now();
  tgt.textContent='Rebooting - the page reloads as soon as the bridge answers again...';
  const poll=async()=>{
    const ctl=new AbortController();const to=setTimeout(()=>ctl.abort(),3000);
    try{const r=await fetch('/api/status',{cache:'no-store',signal:ctl.signal});
        if(r.ok){location.reload();return}}catch(e){}
    clearTimeout(to);
    tgt.textContent='Rebooting... ('+Math.round((Date.now()-t0)/1000)+' s)';
    setTimeout(poll,2000);
  };
  setTimeout(poll,8000);
}
let REBOOTING=false;
/* Button that only triggers on RELEASE: pressing turns it red
   ("Release = reboot"), releasing over the button triggers, dragging away
   or scrolling cancels. An accidental tap while scrolling therefore reboots
   nothing. Via keyboard (no pointer) a confirmation dialog is used instead. */
function holdBtn(b,act){
  const txt=b.textContent;let armed=false;
  const reset=()=>{armed=false;b.classList.remove('armed');b.textContent=txt};
  b.addEventListener('pointerdown',e=>{
    if(e.button||b.disabled)return;
    armed=true;b.classList.add('armed');b.textContent='Release = reboot';
  });
  b.addEventListener('pointerup',e=>{
    if(!armed)return;
    /* With touch the pointer stays "captured" by the button - only
       elementFromPoint tells whether the finger is still over it. */
    const over=b.contains(document.elementFromPoint(e.clientX,e.clientY));
    reset();
    if(over)act();
  });
  b.addEventListener('pointercancel',reset);
  b.addEventListener('pointerleave',e=>{if(e.pointerType==='mouse')reset()});
  b.addEventListener('click',e=>{
    if(e.detail===0&&!b.disabled&&confirm(txt+' - are you sure?'))act();
  });
}
/* ---------------------------------------------------------------------------
   Unsaved changes. Baseline = the form after load() or after the last
   successful save. Password fields: empty = unchanged.
   ------------------------------------------------------------------------- */
const PW=['pass1','pass2','pass3','mqtt_pass','admin_pass'];
/* These fields take effect without a reboot (checked in the firmware: they
   are read fresh from g_cfg on every use or applied by
   bridge_apply_live_tuning()). Everything else only after a reboot. */
const LIVE=['tx_power','eth_tx_retries','wifi_tx_retries','wd_enable','wd_actions','wd_bad_windows',
            'roam_enable','roam_check_s','roam_rssi_threshold','roam_margin_db',
            'ap_idle_reboot_s','telemetry_s','admin_pass','txq_cap','thr_off','e2w_max_mbit','thr_temp_c'];
let SNAP=null;
function snapshot(){SNAP={};for(const k of CFG)SNAP[k]=$(k).value;dirtyCheck()}
/* Names of all fields that differ from the baseline. */
function changedFields(){
  if(!SNAP)return[];
  return CFG.filter(k=>$(k).value!==SNAP[k]).concat(PW.filter(k=>$(k).value!==''));
}
/* Show or hide the unsaved-changes box. */
function dirtyCheck(){
  const g=changedFields();
  if(!g.length){
    /* After saving, the box stays as long as it still has something to say
       (reboot required) - otherwise it disappears. */
    if(!DIRTY_NEEDS_RB)$('dirty').hidden=true;
    return;
  }
  DIRTY_NEEDS_RB=false;
  const rb=g.filter(k=>!LIVE.includes(k)).length;
  $('dirtyn').textContent=g.length+(g.length===1?' field':' fields')+
    (rb?', '+rb+' of them only after a reboot':'');
  $('dirtymsg').textContent='';
  $('dirtybtns').innerHTML='<button onclick="dirtySave()">Save</button>'+
    '<button class="sec" onclick="dirtyCancel()">Discard</button>';
  $('dirty').querySelector('b').textContent='Unsaved changes';
  $('dirty').hidden=false;
}
let DIRTY_NEEDS_RB=false;
/* Save from the unsaved-changes box; offer a reboot if some of the saved
   fields need one. */
async function dirtySave(){
  const rb=changedFields().filter(k=>!LIVE.includes(k)).length;
  if(!await save(false,$('dirtymsg')))return;
  if(rb&&!PROV){
    /* Saved, but part of it only takes effect after a reboot - offer that
       right here, again as a release-to-trigger button. */
    DIRTY_NEEDS_RB=true;
    $('dirty').querySelector('b').textContent='Saved - reboot required';
    $('dirtyn').textContent='';
    $('dirtymsg').textContent=rb+(rb===1?' change takes':' changes take')+' effect only after a reboot.';
    $('dirtybtns').innerHTML='<button class="hold" data-act="reboot">Reboot now</button>'+
      '<button class="sec" onclick="DIRTY_NEEDS_RB=false;$(\'dirty\').hidden=true">Later</button>';
    holdBtn($('dirtybtns').firstChild,()=>reboot($('dirtymsg')));
    /* save() saw "no changes" against the new baseline and hid the box
       before DIRTY_NEEDS_RB was set - show it again. */
    $('dirty').hidden=false;
  }else{
    $('dirtymsg').textContent='Saved.';
    setTimeout(()=>{if(!changedFields().length)$('dirty').hidden=true},1500);
  }
}
/* Restore the form to the baseline. */
function dirtyCancel(){
  if(!SNAP)return;
  for(const k of CFG)$(k).value=SNAP[k];
  for(const k of PW)$(k).value='';
  dirtyCheck();
}
/* Check inputs anywhere on the page (delegated), plus a warning when leaving
   the page with unsaved changes. */
document.addEventListener('input',dirtyCheck);
document.addEventListener('change',dirtyCheck);
window.addEventListener('beforeunload',e=>{
  if(!REBOOTING&&changedFields().length){e.preventDefault();e.returnValue=''}
});
/* iOS Safari only shows :active (press feedback) if some touchstart
   listener exists - an empty one is enough. */
document.addEventListener('touchstart',()=>{},{passive:true});
document.querySelectorAll('button.hold').forEach(b=>
  holdBtn(b,b.dataset.act==='saverb'?()=>save(true):()=>reboot()));
async function ethReset(){
  $('ethmsg').textContent='Resetting Ethernet...';
  try{
    const j=await(await fetch('/api/eth_reset',{method:'POST'})).json();
    $('ethmsg').textContent=j.ok?'Done - a short link drop for the camera is normal.'
                               :'Not possible: Ethernet driver not active yet.';
  }catch(e){$('ethmsg').textContent='Error while triggering'}
}
/* Checked once on load, not every 2 s - a coredump only changes after a
   crash, constant polling would be pure load. */
function cdHeaders(){const h={};if($('okey')&&$('okey').value)h['X-Admin-Key']=$('okey').value;return h}
async function checkCoredump(){
  try{
    const r=await fetch('/api/coredump',{headers:cdHeaders()});
    if(!r.ok){$('cdbox').style.display='none';return}
    const j=await r.json();
    if(!j.present){$('cdbox').style.display='none';return}
    $('cdbox').style.display='block';
    $('cdtext').textContent=j.readable===false
      ? 'Present, but not readable ('+(j.err||'?')+')'
      : 'Task: '+j.task+' @ '+j.pc+(j.reason?' - '+j.reason:'')+
        (j.bt&&j.bt.length?' | Backtrace: '+j.bt.join(' '):'')+
        (j.corrupted?' (dump corrupted)':'');
  }catch(e){}
}
async function cdErase(){
  await fetch('/api/coredump',{method:'DELETE',headers:cdHeaders()});
  $('cdbox').style.display='none';
}
/* Upload the selected .bin to /api/update with progress display. */
function upload(){
  const f=$('fw').files[0];
  if(!f){$('omsg').textContent='No file selected';return}
  const x=new XMLHttpRequest();
  x.open('POST','/api/update');
  x.setRequestHeader('Content-Type','application/octet-stream');
  if($('okey').value)x.setRequestHeader('X-Admin-Key',$('okey').value);
  x.upload.onprogress=e=>{
    const p=e.total?Math.round(e.loaded*100/e.total):0;
    $('pbar').style.width=p+'%';$('omsg').textContent='Uploading... '+p+'%';
  };
  x.onload=()=>{
    $('omsg').textContent = x.status==200
      ? 'Flashed. The board reboots - reload the page in about 20 s.'
      : 'Error: '+x.responseText;
  };
  x.onerror=()=>{$('omsg').textContent='Connection lost'};
  $('omsg').textContent='Starting upload...';
  x.send(f);
}
/* Console log: while the box is open, fetch only the output that is new
   since the last poll (lgSince = byte position returned by the board) once
   a second. Closed, it costs nothing. The view is capped so a long session
   does not grow the page without bound. */
/* Reset reasons for the previous-boot log header (esp_reset_reason_t). */
const RSTN={1:'power-on',2:'reset pin',3:'software restart',4:'panic',5:'interrupt watchdog',
  6:'task watchdog',7:'hardware watchdog',8:'deep sleep',9:'brownout',11:'USB',12:'JTAG',
  14:'power glitch',15:'CPU lockup'};
/* Show the end of the previous boot's log (stored in NVS at boot), headed
   by the reset reason that ended it. */
async function logPrev(){
  const p=$('lgprev');p.style.display='block';
  try{
    const r=await fetch('/api/log?prev=1',{headers:cdHeaders(),cache:'no-store'});
    if(!r.ok){p.textContent=r.status==401?'Admin key required (enter it under Firmware update)':'Error '+r.status;return}
    const t=await r.text(),n=+(r.headers.get('X-Log-Rst')||0);
    p.textContent=t?'--- previous boot, ended by: '+(RSTN[n]||('reset '+n))+' ---\n'+t
                   :'No previous boot log stored (none since the last power-on).';
    p.scrollTop=p.scrollHeight;
  }catch(e){p.textContent='Board not reachable'}
}
let lgSince=0,lgTimer=null,lgBusy=false;
function logToggle(open){
  clearInterval(lgTimer);lgTimer=null;
  if(open){logPoll();lgTimer=setInterval(logPoll,1000)}
}
/* One request at a time: the 1 s timer and the immediate follow-up after a
   full reply must not both read from the same position. */
async function logPoll(){
  if(lgBusy)return;
  lgBusy=true;let more=false;
  try{
    const r=await fetch('/api/log?since='+lgSince,{headers:cdHeaders(),cache:'no-store'});
    if(!r.ok){$('lghint').textContent=r.status==401?'Admin key required (enter it under Firmware update)':'Error '+r.status;return}
    const t=await r.text(),p=$('lgpre');
    const atEnd=p.scrollTop+p.clientHeight>=p.scrollHeight-4;
    if(r.headers.get('X-Log-Reset')==='1'&&p.textContent)
      p.textContent+='\n--- board rebooted or older lines dropped ---\n';
    p.textContent+=t;
    if(p.textContent.length>65536)p.textContent=p.textContent.slice(-49152);
    lgSince=+(r.headers.get('X-Log-Next')||0);
    if(atEnd)p.scrollTop=p.scrollHeight;
    $('lghint').textContent='';
    more=t.length>=2048;   /* full reply: more is waiting */
  }catch(e){$('lghint').textContent='Board not reachable'}
  finally{lgBusy=false}
  if(more&&lgTimer)logPoll();
}
load();tick();checkCoredump();checkCrashes();setInterval(tick,2000);
</script></body></html>)HTML";

/* ===========================================================================
 * Handlers
 * ========================================================================= */

/* GET / - the portal page. Counts as portal activity. */
static esp_err_t h_root(httpd_req_t *r) {
  s_last_activity_ms = millis();
  httpd_resp_set_type(r, "text/html; charset=utf-8");
  /* Without cache control, browsers cache HTML heuristically - after a
   * firmware update one would see the old page with missing or wrong fields,
   * and a normal reload would not help. */
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  return httpd_resp_send(r, PAGE, HTTPD_RESP_USE_STRLEN);
}

/* GET /api/status - live counters, link state, autotune progress, heap and
 * build info as one JSON object. Deliberately does NOT count as portal
 * activity (the page polls it every 2 s). */
static esp_err_t h_status(httpd_req_t *r) {
  BridgeStats st;
  bridge_get_stats(&st);
  uint32_t lr_el, lr_tot;
  bridge_long_tune_progress(&lr_el, &lr_tot);
  uint32_t ethd[BRIDGE_ETH_DRV_STATS];
  bridge_get_eth_drv_stats(ethd);

  char buf[1550];
  snprintf(buf, sizeof(buf),
    "{\"prov\":%d,\"wifi\":%d,\"eth\":%d,\"rssi\":%d,\"ch\":%u,"
    "\"kbps_up\":%lu,\"kbps_down\":%lu,\"pkt_up\":%lu,\"pkt_down\":%lu,"
    "\"drop_up\":%lu,\"drop_down\":%lu,\"wifi_disc\":%lu,\"wifi_kicks\":%lu,"
    "\"disc_r\":%u,\"disc_rs\":\"%s\",\"disc_src\":%u,\"disc_ago\":%lu,\"wd_probe\":%u,"
    "\"wd_reconnects\":%lu,\"wd_eth_resets\":%lu,\"wd_last_reason\":%u,"
    "\"roam_count\":%lu,\"uptime\":%lu,"
    "\"client_mac\":\"%s\",\"client_ip\":\"%s\",\"client_name\":\"%s\","
    "\"ssid\":\"%s\",\"bssid\":\"%s\",\"net\":%u,\"fo\":%lu,"
    "\"at\":%d,\"at_s\":%u,\"at_n\":%u,\"at_r\":%u,\"at_k\":%u,"
    "\"heap\":%lu,\"heap_min\":%lu,\"heap_dma\":%lu,\"heap_blk\":%lu,"
    "\"txp\":%d,\"bw\":%u,\"air_ok\":%lu,\"air_fail\":%lu,\"cc\":\"%s\",\"cc_max\":%d,"
    "\"txq\":%lu,\"txq_x10\":%lu,\"txq_ms\":%lu,"
    "\"pt\":%d,\"pt_s\":%u,\"pt_k\":%d,\"pt_r\":%d,"
    "\"drop_cap\":%lu,\"txq_cap\":%u,\"qc\":%d,\"qc_s\":%u,\"qc_k\":%u,\"qc_r\":%d,\"all\":%d,"
    "\"lr\":%d,\"lr_k\":%u,\"lr_el\":%lu,\"lr_tot\":%lu,\"lr_r\":%u,"
    "\"spi\":%u,\"temp\":%d,\"temp_max\":%d,\"thr\":%lu,\"drop_thr\":%lu,\"w5\":[%lu,%lu,%lu,%lu,%lu],\"w5x\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu],\"rst\":%u,\"rtc\":%d,\"time\":%lu,\"build\":\"" GIT_REV "\"}",
    s_provisioning ? 1 : 0, st.wifi_up ? 1 : 0, st.eth_link ? 1 : 0,
    (int)st.rssi, (unsigned)st.channel,
    (unsigned long)st.kbps_eth2wifi, (unsigned long)st.kbps_wifi2eth,
    (unsigned long)st.pkt_eth2wifi,  (unsigned long)st.pkt_wifi2eth,
    (unsigned long)st.drop_eth2wifi, (unsigned long)st.drop_wifi2eth,
    (unsigned long)st.wifi_disc_count, (unsigned long)st.wifi_kicks,
    /* Last drop in this boot - disc_r 0 = none. The full history (also
     * across reboots) is available at /api/disconnects. */
    (unsigned)st.disc_last_reason,
    st.disc_last_reason ? bridge_wifi_reason_str(st.disc_last_reason) : "",
    (unsigned)st.disc_last_src, (unsigned long)st.disc_last_ago_s,
    (unsigned)st.wd_probe,
    (unsigned long)st.wd_reconnects, (unsigned long)st.wd_eth_resets,
    (unsigned)st.wd_last_reason, (unsigned long)st.roam_count,
    (unsigned long)(millis() / 1000), st.client_mac, st.client_ip, st.client_name, st.ssid, st.bssid,
    /* Configured network slot (1-3) and network switches since boot. */
    (unsigned)(bridge_get_net() + 1), (unsigned long)bridge_get_failovers(),
    (int)bridge_autotune_state(), bridge_autotune_step(),
    bridge_autotune_count(), bridge_autotune_result(), bridge_autotune_candidate(),
    (unsigned long)esp_get_free_heap_size(),
    /* Minimum since boot - reveals dips that fall between two polls and
     * would otherwise stay invisible. */
    (unsigned long)esp_get_minimum_free_heap_size(),
    /* For Wi-Fi buffers only DMA-capable internal memory counts. */
    (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
    /* Largest contiguous block: if it is far below the total, the heap is
     * fragmented and large allocations fail although enough would be free
     * on paper. */
    (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
    /* The actual value reported by the driver, not the configured target -
     * the PHY rounds down to 11 fixed steps. */
    (int)bridge_get_tx_power(),
    /* Likewise the channel width actually negotiated: the ht40 switch only
     * says what we offer. */
    (unsigned)bridge_get_bandwidth_mhz(),
    /* Delivered/abandoned frames as the Wi-Fi MAC reports them, and the
     * regulatory cap the TX power has to stay under. */
    (unsigned long)st.air_ok, (unsigned long)st.air_fail,
    st.country, (int)st.country_max_txp,
    /* Driver TX queue: frames waiting now, mean over the last second (x10)
     * and the resulting wait per frame - the bufferbloat indicator. */
    (unsigned long)st.txq_now, (unsigned long)st.txq_x10, (unsigned long)st.txq_delay_ms,
    /* TX power autotune, same state codes as "at". */
    (int)bridge_txp_tune_state(), bridge_txp_tune_step(),
    (int)bridge_txp_tune_candidate(), (int)bridge_txp_tune_result(),
    /* TX queue cap: its drops, the cap in effect, and its autotune (qc_r
     * -1 = no result yet, 0 = off). */
    (unsigned long)st.drop_cap, (unsigned)st.txq_cap,
    (int)bridge_cap_tune_state(), bridge_cap_tune_step(), bridge_cap_tune_candidate(),
    bridge_cap_tune_result() == 0xFF ? -1 : (int)bridge_cap_tune_result(),
    /* "Calibrate all": 0 idle, 1-4 running stage, 5 done, 6 stopped. */
    (int)bridge_tune_all_stage(),
    /* Long-run retry check: state, retries in the running block, minutes
     * elapsed / total, chosen retries. */
    (int)bridge_long_tune_state(), bridge_long_tune_candidate(),
    (unsigned long)lr_el, (unsigned long)lr_tot, bridge_long_tune_result(),
    /* W5500 SPI clock in MHz, 0 = internal EMAC. */
    (unsigned)bridge_get_eth_spi_mhz(),
    /* SoC temperature now and its maximum since boot, 0.1 degC (-32768 = no
     * sensor), see cpustat.h. */
    (int)cpustat_temp_x10(), (int)cpustat_temp_max_x10(),
    /* Thermal throttle: Ethernet -> Wi-Fi limit in kbit/s (0 = none) and
     * its drops (see bridge_thermal_limit_kbps()). */
    (unsigned long)bridge_thermal_limit_kbps(), (unsigned long)bridge_thermal_drops(),
    /* Ethernet driver: drops (TX ring full, SEND timeout, RX resync, RX no
     * heap) and incomplete RX rounds (see bridge_get_eth_drv_stats()). */
    (unsigned long)ethd[0], (unsigned long)ethd[1], (unsigned long)ethd[2], (unsigned long)ethd[3],
    (unsigned long)ethd[4],
    /* ... and its activity: RX frames, RX batches, TX frames, task rounds,
     * ms in SPI, ms delivering to Wi-Fi, waits for TX ring room. */
    (unsigned long)ethd[5], (unsigned long)ethd[6], (unsigned long)ethd[7],
    (unsigned long)ethd[8], (unsigned long)ethd[9], (unsigned long)ethd[10],
    (unsigned long)ethd[11],
    /* Why this boot happened (esp_reset_reason_t) and whether RTC memory
     * survived it - see crashlog.h. */
    (unsigned)crashlog_reset_reason(), crashlog_rtc_retained() ? 1 : 0,
    /* Time UTC, 0 = not set via SNTP yet. */
    (unsigned long)crashlog_now());

  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  return httpd_resp_send(r, buf, HTTPD_RESP_USE_STRLEN);
}

/* GET /api/disconnects - disconnect log (see bridge.h): frequency per reason
 * since this boot plus the last DISC_LOG_N drops, also from earlier boots.
 * "prev" = how many boots back (0 = current); "ago" only exists for the
 * current boot, because the uptime of earlier boots is unknown.
 * "down" = seconds until reconnected, -1 = not (yet) reconnected.
 * "up" = 1: existing connection dropped (real drop), 0: only a failed
 * connect attempt. "by_reason" counts real drops only, "kicks"/"fails" are
 * the totals since this boot (external only). */
static esp_err_t h_disconnects(httpd_req_t *r) {
  /* Heap instead of stack: 48 entries plus the response buffer would blow
   * the 6 KB stack of the HTTP task. */
  bridge_disc_t *log = (bridge_disc_t *)malloc(sizeof(bridge_disc_t) * DISC_LOG_N);
  const size_t cap = 10240;
  char *buf = (char *)malloc(cap);
  if (!log || !buf) {
    free(log); free(buf);
    httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    return ESP_FAIL;
  }
  uint8_t boot = 0;
  const size_t n = bridge_get_disc_log(log, DISC_LOG_N, &boot);
  bridge_disc_count_t cnt[12];
  uint16_t rest = 0;
  const size_t nc = bridge_get_disc_counts(cnt, 12, &rest);
  uint32_t kicks = 0, fails = 0;
  bridge_get_disc_totals(&kicks, &fails);

  const uint32_t now_s = millis() / 1000;
  size_t o = snprintf(buf, cap, "{\"kicks\":%lu,\"fails\":%lu,\"rest\":%u,\"by_reason\":[",
                      (unsigned long)kicks, (unsigned long)fails, (unsigned)rest);
  for (size_t i = 0; i < nc && o < cap; i++) {
    o += snprintf(buf + o, cap - o, "%s{\"r\":%u,\"rs\":\"%s\",\"n\":%u}", i ? "," : "",
                  (unsigned)cnt[i].reason, bridge_wifi_reason_str(cnt[i].reason),
                  (unsigned)cnt[i].n);
  }
  if (o < cap) o += snprintf(buf + o, cap - o, "],\"log\":[");
  for (size_t i = 0; i < n && o < cap; i++) {
    const bridge_disc_t *e = &log[i];
    const unsigned prev = (uint8_t)(boot - e->boot);
    o += snprintf(buf + o, cap - o,
      "%s{\"prev\":%u,\"t\":%lu,\"e\":%lu,\"ago\":%ld,\"r\":%u,\"rs\":\"%s\",\"src\":%u,\"up\":%u,"
      "\"rssi\":%d,\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"down\":%ld}",
      i ? "," : "", prev, (unsigned long)e->t_s, (unsigned long)e->epoch,
      prev ? -1L : (long)(now_s - e->t_s),
      (unsigned)e->reason, bridge_wifi_reason_str(e->reason), (unsigned)e->src,
      (unsigned)e->was_up,
      (int)e->rssi, e->bssid[0], e->bssid[1], e->bssid[2], e->bssid[3], e->bssid[4], e->bssid[5],
      e->down_s >= 0xFFFE ? -1L : (long)e->down_s);
  }
  if (o < cap) o += snprintf(buf + o, cap - o, "]}");
  if (o >= cap) o = cap - 1;   /* truncated - not expected with 48+12 entries (~200 B each) */
  free(log);

  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  esp_err_t err = httpd_resp_send(r, buf, o);
  free(buf);
  return err;
}

/* GET /api/config - the configuration as JSON for the form, plus the
 * firmware defaults ("def") and the buffer values currently running ("eff").
 * Passwords are never returned. Counts as portal activity. */
static esp_err_t h_config_get(httpd_req_t *r) {
  s_last_activity_ms = millis();
  char ip[16], mask[16], gw[16];
  format_ipv4(g_cfg.mgmt_ip,   ip,   sizeof(ip));
  format_ipv4(g_cfg.mgmt_mask, mask, sizeof(mask));
  format_ipv4(g_cfg.mgmt_gw,   gw,   sizeof(gw));

  /* Profiles 2 and 3: return unset fields as an empty string instead of
   * "0.0.0.0" - the form should show nothing there, otherwise it looks like a
   * set (and broken) address. */
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

  /* mu must hold the ESCAPED user name: json_escape() prefixes every " and \
   * with a backslash, so in the worst case the length doubles. With 64
   * usable characters that is 128 plus the NUL byte - a smaller buffer would
   * silently truncate the name, and only for long values, i.e. exactly when
   * nobody tests it. */
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

  /* Passwords are deliberately never returned.
   * Buffer size: name, three SSIDs, nine IP addresses from three profiles,
   * host, escaped user name and the tuning numbers add up to roughly
   * 1750 bytes in the worst case. snprintf truncates cleanly, but a truncated
   * JSON is unparsable - the form would simply stay empty. Grow this buffer
   * when adding fields. */
  char buf[2048];
  snprintf(buf, sizeof(buf),
    "{\"name\":\"%s\",\"ssid1\":\"%s\",\"ssid2\":\"%s\",\"ssid3\":\"%s\","
    "\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\","
    "\"ip2\":\"%s\",\"mask2\":\"%s\",\"gw2\":\"%s\","
    "\"ip3\":\"%s\",\"mask3\":\"%s\",\"gw3\":\"%s\","
    "\"mqtt_host\":\"%s\",\"mqtt_port\":%u,\"mqtt_user\":\"%s\","
    "\"telemetry_s\":%u,"
    "\"tx_power\":%d,\"eth_tx_retries\":%u,\"wifi_tx_retries\":%u,"
    "\"static_rx_buf\":%u,\"dynamic_rx_buf\":%u,\"dynamic_tx_buf\":%u,"
    "\"rx_ba_win\":%u,\"ht40\":%u,\"no_11b\":%u,\"wd_enable\":%u,"
    "\"wd_actions\":%u,\"wd_bad_windows\":%u,"
    "\"wifi_connect_timeout_s\":%u,\"wifi_connect_retries\":%u,"
    "\"ap_idle_reboot_s\":%u,"
    "\"roam_enable\":%u,\"roam_check_s\":%u,\"roam_rssi_threshold\":%d,"
    "\"roam_margin_db\":%u,\"txq_cap\":%u,\"wifi_band\":%u,\"band5g\":%d,\"thr_off\":%u,\"e2w_max_mbit\":%u,\"thr_temp_c\":%u,"
    "\"def\":{\"eth_tx_retries\":%u,\"wifi_tx_retries\":%u,"
    "\"static_rx_buf\":%u,\"dynamic_rx_buf\":%u,\"dynamic_tx_buf\":%u,"
    "\"rx_ba_win\":%u,\"wifi_connect_timeout_s\":%u,"
    "\"wifi_connect_retries\":%u,\"ap_idle_reboot_s\":%u,"
    "\"roam_check_s\":%u,\"roam_rssi_threshold\":%d,\"roam_margin_db\":%u,"
    "\"wd_actions\":%u,\"wd_bad_windows\":%u},"
    "\"eff\":{\"static_rx_buf\":%u,\"dynamic_rx_buf\":%u,\"dynamic_tx_buf\":%u,"
    "\"rx_ba_win\":%u}}",
    n, s1, s2, s3, ip, mask, gw, ip2, mask2, gw2, ip3, mask3, gw3,
    mh, g_cfg.mqtt_port, mu, g_cfg.telemetry_s,
    /* Return the EFFECTIVE values, not the stored 0 - the form should show
     * what is running. On save the number is then stored explicitly; the
     * "Defaults" button resets it.
     * Exception buffers/BA window: those only take effect after a reboot. If
     * a value is already stored there, THAT one must go into the form -
     * otherwise the field would jump back to the running value after a page
     * reload, and the next save (of any setting) would silently overwrite
     * the change. The running values are additionally reported under
     * "eff". */
    (int)g_cfg.tx_power, eff.eth_retries, eff.wifi_retries,
    g_cfg.static_rx_buf  ? g_cfg.static_rx_buf  : eff.static_rx,
    g_cfg.dynamic_rx_buf ? g_cfg.dynamic_rx_buf : eff.dyn_rx,
    g_cfg.dynamic_tx_buf ? g_cfg.dynamic_tx_buf : eff.dyn_tx,
    g_cfg.rx_ba_win      ? g_cfg.rx_ba_win      : eff.ba_win,
    g_cfg.ht40, g_cfg.no_11b, g_cfg.wd_enable,
    eff.wd_actions, eff.wd_bad_windows,
    eff.wifi_connect_timeout_s, eff.wifi_connect_retries, eff.ap_idle_reboot_s,
    g_cfg.roam_enable, eff.roam_check_s, (int)eff.roam_rssi_threshold, eff.roam_margin_db,
    g_cfg.txq_cap, g_cfg.wifi_band,
#if CONFIG_SOC_WIFI_SUPPORT_5G
    1,
#else
    0,
#endif
    g_cfg.thr_off, g_cfg.e2w_max_mbit, g_cfg.thr_temp_c,
    def.eth_retries, def.wifi_retries,
    def.static_rx, def.dyn_rx, def.dyn_tx, def.ba_win,
    def.wifi_connect_timeout_s, def.wifi_connect_retries, def.ap_idle_reboot_s,
    def.roam_check_s, (int)def.roam_rssi_threshold, def.roam_margin_db,
    def.wd_actions, def.wd_bad_windows,
    eff.static_rx, eff.dyn_rx, eff.dyn_tx, eff.ba_win);

  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  return httpd_resp_send(r, buf, HTTPD_RESP_USE_STRLEN);
}

/* POST /api/config - parse the form-urlencoded body, validate and clamp
 * every field, save to NVS and apply the live tuning values. Fields missing
 * from the body stay unchanged; empty password fields mean "unchanged".
 * Unauthenticated (see CLAUDE.md). Counts as portal activity. */
static esp_err_t h_config_post(httpd_req_t *r) {
  s_last_activity_ms = millis();
  /* More generous than needed, but not unlimited: the form always sends ALL
   * fields, and in the worst case passwords and SSIDs are fully
   * percent-encoded (three characters per byte). */
  if (r->content_len > 2040) {
    httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "too large");
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

  /* form_get() truncates the RAW, still percent-encoded value and only
   * decodes afterwards. encodeURIComponent() turns every special character
   * into three characters, a non-ASCII letter into six - so a smaller buffer
   * would silently store a truncated 64-character password with a few
   * special characters. 3 x 64 + NUL covers the worst case. */
  char v[200];
  if (form_get(body, "name",  v, sizeof(v))) strncpy(g_cfg.name,  v, sizeof(g_cfg.name)  - 1);
  if (form_get(body, "ssid1", v, sizeof(v))) strncpy(g_cfg.ssid1, v, sizeof(g_cfg.ssid1) - 1);
  if (form_get(body, "ssid2", v, sizeof(v))) strncpy(g_cfg.ssid2, v, sizeof(g_cfg.ssid2) - 1);
  if (form_get(body, "ssid3", v, sizeof(v))) strncpy(g_cfg.ssid3, v, sizeof(g_cfg.ssid3) - 1);

  /* An empty password field means "unchanged" - otherwise every save from
   * the status screen would wipe the credentials. */
  if (form_get(body, "pass1", v, sizeof(v)) && v[0]) strncpy(g_cfg.pass1, v, sizeof(g_cfg.pass1) - 1);
  if (form_get(body, "pass2", v, sizeof(v)) && v[0]) strncpy(g_cfg.pass2, v, sizeof(g_cfg.pass2) - 1);
  if (form_get(body, "pass3", v, sizeof(v)) && v[0]) strncpy(g_cfg.pass3, v, sizeof(g_cfg.pass3) - 1);

  if (form_get(body, "ip",   v, sizeof(v))) parse_ipv4(v, g_cfg.mgmt_ip);
  if (form_get(body, "mask", v, sizeof(v))) parse_ipv4(v, g_cfg.mgmt_mask);
  if (form_get(body, "gw",   v, sizeof(v))) parse_ipv4(v, g_cfg.mgmt_gw);

  /* Profiles 2 and 3. Here an empty field deliberately clears the value (to
   * 0.0.0.0), so a profile entered once can be removed again - unlike the
   * password fields, where empty means "unchanged".
   * parse_ipv4() leaves the value untouched on garbage, hence zero it first. */
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

  /* --- Tuning ------------------------------------------------------------
   * Throughout, 0 = "default from sdkconfig / PHY", so 0 is an allowed value
   * everywhere here, not an input error. The upper limits are hard, so a
   * typo in the portal cannot make the board unusable - for a device without
   * serial access that is not a luxury. */
  if (form_get(body, "tx_power", v, sizeof(v))) {
    int t = atoi(v);
    /* Valid per esp_wifi.h: 8..84 (2..20 dBm). Anything in between is
     * rounded down by the PHY to the next of its 11 steps. */
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
    /* IDF limits for static_rx_buf_num are 2..32. */
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
  if (form_get(body, "wifi_band", v, sizeof(v))) {
    int t = atoi(v);
    g_cfg.wifi_band = (uint8_t)((t < 0 || t > (int)WIFI_BAND_MAX) ? WIFI_BAND_PREFER_5G : t);
  }
  /* Throttle LAN -> Wi-Fi (see config.h); read by thermal_tick(), no
   * reboot needed. */
  if (form_get(body, "thr_off", v, sizeof(v))) g_cfg.thr_off = (uint8_t)(atoi(v) ? 1 : 0);
  if (form_get(body, "e2w_max_mbit", v, sizeof(v))) {
    int t = atoi(v);
    g_cfg.e2w_max_mbit = (uint8_t)(t < 0 ? 0 : t > 200 ? 200 : t);
  }
  if (form_get(body, "thr_temp_c", v, sizeof(v))) {
    int t = atoi(v);
    g_cfg.thr_temp_c = (uint8_t)(t <= 0 ? 0 : t < 40 ? 40 : t > 110 ? 110 : t);
  }

  /* Take effect at the next connect (bridge_wifi_start()), not immediately -
   * unlike tx_power/retries they do not affect an existing connection. Upper
   * limits deliberately generous, but not unlimited: a typo should not push
   * the board to minutes per attempt. */
  if (form_get(body, "wifi_connect_timeout_s", v, sizeof(v))) {
    int t = atoi(v);
    if (t != 0 && t < 3) t = 3;
    if (t > 60)          t = 60;
    g_cfg.wifi_connect_timeout_s = (uint16_t)t;
  }
  if (form_get(body, "wifi_connect_retries", v, sizeof(v))) {
    int t = atoi(v);
    if (t < 0)  t = 0;
    if (t > 10) t = 10;
    g_cfg.wifi_connect_retries = (uint8_t)t;
  }

  /* Takes effect immediately - watchdog_tick() reads g_cfg.wd_enable
   * directly, no reboot needed, so "off" also applies right away. */
  if (form_get(body, "wd_enable", v, sizeof(v))) g_cfg.wd_enable = (uint8_t)(atoi(v) ? 1 : 0);
  /* Also immediate. Unknown bits are discarded; 0 stays the default marker
   * ("all levels", see config.h). */
  if (form_get(body, "wd_actions", v, sizeof(v)))
    g_cfg.wd_actions = (uint8_t)(atoi(v) & WD_ACT_ALL);
  if (form_get(body, "wd_bad_windows", v, sizeof(v))) {
    int t = atoi(v);
    if (t < 0)  t = 0;
    if (t > 30) t = 30;   /* 30 x 20 s = 10 min - beyond that it's no longer a watchdog */
    g_cfg.wd_bad_windows = (uint8_t)t;
  }
  /* Also immediate. 0 = off; below 4 frames the cap would drop inside
   * every ordinary burst, above 64 it can never trigger with any buffer
   * count the portal allows. */
  if (form_get(body, "txq_cap", v, sizeof(v))) {
    int t = atoi(v);
    if (t < 0)            t = 0;
    if (t != 0 && t < 4)  t = 4;
    if (t > 64)           t = 64;
    g_cfg.txq_cap = (uint8_t)t;
  }

  /* Lower limit 60 s, so a typo (e.g. "5" instead of "500") cannot send the
   * device into a fast reboot loop in portal mode. */
  if (form_get(body, "ap_idle_reboot_s", v, sizeof(v))) {
    int t = atoi(v);
    if (t != 0 && t < 60) t = 60;
    if (t > 65535)        t = 65535;   /* uint16_t - overflow otherwise */
    g_cfg.ap_idle_reboot_s = (uint16_t)(t < 0 ? 0 : t);
  }

  /* Takes effect immediately - roam_tick() reads g_cfg.roam_enable directly,
   * no reboot needed. Like the watchdog: an explicit portal switch rather
   * than always on, see config.h. */
  if (form_get(body, "roam_enable", v, sizeof(v))) g_cfg.roam_enable = (uint8_t)(atoi(v) ? 1 : 0);
  if (form_get(body, "roam_check_s", v, sizeof(v))) {
    int t = atoi(v);
    /* Lower limit 30 s - checking more often hardly helps (a scan itself
     * takes noticeable time) and only costs airtime. */
    if (t != 0 && t < 30) t = 30;
    if (t > 65535)        t = 65535;   /* uint16_t - overflow otherwise */
    g_cfg.roam_check_s = (uint16_t)(t < 0 ? 0 : t);
  }
  if (form_get(body, "roam_rssi_threshold", v, sizeof(v))) {
    int t = atoi(v);
    /* 0 stays untouched as the default marker (see config.h) - real RSSI
     * values are always negative, so it is unambiguous. */
    if (t != 0) {
      if (t > -1)   t = -1;
      if (t < -100) t = -100;
    }
    g_cfg.roam_rssi_threshold = (int8_t)t;
  }
  if (form_get(body, "roam_margin_db", v, sizeof(v))) {
    int t = atoi(v);
    if (t < 0)  t = 0;
    if (t > 40) t = 40;
    g_cfg.roam_margin_db = (uint8_t)t;
  }

  g_cfg.configured = (g_cfg.ssid1[0] != '\0');

  if (!cfg_save()) {
    httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS");
    return ESP_FAIL;
  }

  /* Apply TX power and retry counters right away, so they can be measured
   * without rebooting every time. In provisioning mode the STA data path is
   * not running yet - the call would be pointless then. */
  if (!s_provisioning) bridge_apply_live_tuning();

  httpd_resp_set_type(r, "application/json");
  return httpd_resp_send(r, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* POST /api/autotune - start the retry autotune, with ?kind=txp the TX power
 * autotune, with ?kind=cap the TX queue cap autotune, with ?kind=long&h=N
 * the N-hour long-run retry check (default 6), with ?kind=all all three in
 * sequence (&h=N appends the long-run check). ?kind=stop aborts whatever
 * runs and restores the saved values (see bridge.h). */
static esp_err_t h_autotune(httpd_req_t *r) {
  char q[48] = "", kind[8] = "", hs[4] = "";
  if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) {
    httpd_query_key_value(q, "kind", kind, sizeof(kind));
    httpd_query_key_value(q, "h", hs, sizeof(hs));
  }
  int h = atoi(hs);
  if (h < 0)  h = 0;
  if (h > 48) h = 48;
  bool ok = true;
  if (strcmp(kind, "stop") == 0) bridge_tune_stop();
  else ok = strcmp(kind, "txp")  == 0 ? bridge_txp_tune_start()
          : strcmp(kind, "cap")  == 0 ? bridge_cap_tune_start()
          : strcmp(kind, "long") == 0 ? bridge_long_tune_start((uint8_t)(h ? h : 6))
          : strcmp(kind, "all")  == 0 ? bridge_tune_all_start((uint8_t)h)
                                      : bridge_autotune_start();
  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  return httpd_resp_send(r, ok ? "{\"ok\":true}"
                              : "{\"ok\":false,\"err\":\"already running or bridge inactive\"}",
                         HTTPD_RESP_USE_STRLEN);
}

/* GET /api/autotune - per-candidate measurements of both autotunes, so the
 * portal can show WHY a value was chosen. Unmeasured candidates are omitted;
 * a collapsed step is reported as loss -1. */
static esp_err_t h_autotune_get(httpd_req_t *r) {
  uint8_t  rk[16]; uint16_t rl[16];
  int8_t   pk[8], pa[8]; uint16_t pl[8], pd[8];
  const size_t rn = bridge_autotune_count();
  bridge_autotune_values(rk, rl);
  const size_t pn = bridge_txp_tune_values(pk, pa, pl, pd);
  uint8_t  ck[8]; uint16_t cl[8], cd[8], cr[8];
  const size_t cn = bridge_cap_tune_values(ck, cl, cd, cr);
  uint8_t  lk[4]; uint32_t lp[4], ld[4]; bool lb[4];
  const size_t ln = bridge_long_tune_values(lk, lp, ld, lb);

  char buf[1600];
  int n = snprintf(buf, sizeof(buf), "{\"retries\":[");
  bool first = true;
  for (size_t i = 0; i < rn && n < (int)sizeof(buf) - 64; i++) {
    if (rl[i] == 0xFFFF) continue;
    n += snprintf(buf + n, sizeof(buf) - n, "%s{\"k\":%u,\"loss\":%d}", first ? "" : ",",
                  (unsigned)rk[i], rl[i] == 0xFFFE ? -1 : (int)rl[i]);
    first = false;
  }
  n += snprintf(buf + n, sizeof(buf) - n, "],\"txp\":[");
  first = true;
  for (size_t i = 0; i < pn && n < (int)sizeof(buf) - 80; i++) {
    if (pl[i] == 0xFFFF) continue;
    n += snprintf(buf + n, sizeof(buf) - n,
                  "%s{\"k\":%d,\"act\":%d,\"loss\":%d,\"delay\":%d}", first ? "" : ",",
                  (int)pk[i], (int)pa[i], pl[i] == 0xFFFE ? -1 : (int)pl[i],
                  pl[i] == 0xFFFE ? -1 : (int)pd[i]);
    first = false;
  }
  n += snprintf(buf + n, sizeof(buf) - n, "],\"cap\":[");
  first = true;
  for (size_t i = 0; i < cn && n < (int)sizeof(buf) - 80; i++) {
    if (cl[i] == 0xFFFF) continue;
    const bool col = cl[i] == 0xFFFE;
    n += snprintf(buf + n, sizeof(buf) - n,
                  "%s{\"k\":%u,\"loss\":%d,\"delay\":%d,\"kbps\":%d}", first ? "" : ",",
                  (unsigned)ck[i], col ? -1 : (int)cl[i], col ? -1 : (int)cd[i],
                  col ? -1 : (int)cr[i]);
    first = false;
  }
  n += snprintf(buf + n, sizeof(buf) - n, "],\"long\":[");
  for (size_t i = 0; i < ln && n < (int)sizeof(buf) - 80; i++)
    n += snprintf(buf + n, sizeof(buf) - n, "%s{\"k\":%u,\"pkts\":%lu,\"drops\":%lu,\"bad\":%d}",
                  i ? "," : "", (unsigned)lk[i], (unsigned long)lp[i],
                  (unsigned long)ld[i], lb[i] ? 1 : 0);
  snprintf(buf + n, sizeof(buf) - n, "]}");
  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  return httpd_resp_send(r, buf, HTTPD_RESP_USE_STRLEN);
}

/* POST /api/reboot - answer first, then reboot after a short delay so the
 * response still reaches the browser. */
static esp_err_t h_reboot(httpd_req_t *r) {
  httpd_resp_send(r, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
  delay(400);
  bridge_restart_requested();
  return ESP_OK;
}

/* POST /api/eth_reset - restart the Ethernet driver (see bridge.h). */
static esp_err_t h_eth_reset(httpd_req_t *r) {
  const bool ok = bridge_reset_eth();
  httpd_resp_set_type(r, "application/json");
  return httpd_resp_send(r, ok ? "{\"ok\":true}" : "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
}

/* GET /api/scan - blocking Wi-Fi scan, returns up to 20 networks as
 * [{"ssid":..,"rssi":..}]. Hidden networks are skipped. Counts as portal
 * activity. */
static esp_err_t h_scan(httpd_req_t *r) {
  s_last_activity_ms = millis();
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
  bool first = true;
  for (uint16_t i = 0; i < n; i++) {
    char esc[80], item[128];
    json_escape((const char *)recs[i].ssid, esc, sizeof(esc));
    if (!esc[0]) continue;
    /* Tie the comma to the entry actually emitted, not to i - otherwise
     * "[,{...}]" results as soon as the first hit is skipped, and the portal
     * can no longer parse the list. */
    snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d}",
             first ? "" : ",", esc, (int)recs[i].rssi);
    first = false;
    httpd_resp_sendstr_chunk(r, item);
  }
  httpd_resp_sendstr_chunk(r, "]");
  httpd_resp_sendstr_chunk(r, NULL);

  free(recs);
  return ESP_OK;
}


/* ===========================================================================
 * Firmware update (OTA)
 * ---------------------------------------------------------------------------
 * Raw POST of the .bin file, streamed directly into the inactive app
 * partition. No HTTPS (it would cost about 40 kB of flash for TLS). Hence the
 * header key as minimal protection, and the recommendation to only trigger
 * updates from the local network.
 * ========================================================================= */

/* Minimal protection, shared by firmware update, coredump and crash history.
 * Over plain HTTP this is no real protection against someone sniffing the
 * traffic - but it prevents accidental flashing/reading and access from
 * devices that do not know the key. No admin_pass set = open. */
static bool admin_key_ok(httpd_req_t *r) {
  if (!g_cfg.admin_pass[0]) return true;
  char key[48] = "";
  return httpd_req_get_hdr_value_str(r, "X-Admin-Key", key, sizeof(key)) == ESP_OK &&
         strcmp(key, g_cfg.admin_pass) == 0;
}

/* POST /api/update - stream the request body into the next OTA partition,
 * validate it and make it the boot partition, then reboot. The data path is
 * paused for the duration; every error path un-pauses it again. The new
 * image boots as PENDING_VERIFY and is rolled back unless it confirms itself
 * (see main.cpp: ota_confirm_if_pending()). */
static esp_err_t h_update(httpd_req_t *r) {
  if (!admin_key_ok(r)) {
    httpd_resp_send_err(r, HTTPD_401_UNAUTHORIZED, "wrong key");
    return ESP_FAIL;
  }

  const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
  if (!target) {
    httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "no OTA partition - wrong partition table?");
    return ESP_FAIL;
  }

  printf("[OTA ] Target: %s (%u kB), expecting %u kB\n",
                target->label, (unsigned)(target->size / 1024),
                (unsigned)(r->content_len / 1024));

  if (r->content_len > target->size) {
    httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "image does not fit");
    return ESP_FAIL;
  }

  /* Pause the data path: while writing flash the cache is briefly disabled,
   * which a data path under load tolerates badly. The camera is offline for
   * the duration of the update - that is the right compromise. */
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
    httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    return ESP_FAIL;
  }

  int remaining = r->content_len;
  bool header_checked = false;

  while (remaining > 0) {
    int n = httpd_req_recv(r, buf, remaining < 4096 ? remaining : 4096);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) {
      free(buf); esp_ota_abort(ota); bridge_set_paused(false);
      printf("[OTA ] Transfer aborted\n");
      httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "aborted");
      return ESP_FAIL;
    }

    /* Check the magic byte before overwriting a valid partition. Prevents an
     * accidentally uploaded image/PDF from making the board unusable. */
    if (!header_checked) {
      if (n < 1 || (uint8_t)buf[0] != ESP_IMAGE_HEADER_MAGIC) {
        free(buf); esp_ota_abort(ota); bridge_set_paused(false);
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "not an ESP32 firmware");
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
        err == ESP_ERR_OTA_VALIDATE_FAILED ? "invalid image" : esp_err_to_name(err));
    return ESP_FAIL;
  }

  err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK) {
    bridge_set_paused(false);
    httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    return ESP_FAIL;
  }

  printf("[OTA ] Success - rebooting\n");
  httpd_resp_set_type(r, "application/json");
  httpd_resp_send(r, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
  delay(600);
  bridge_restart_requested();
  return ESP_OK;
}

/* ===========================================================================
 * Crash dump (coredump)
 * ---------------------------------------------------------------------------
 * The board runs without serial access - without a coredump a crash is
 * exactly one piece of information ("it rebooted"). The coredump partition
 * (see partitions_8mb_ota.csv) captures the panic reason, the affected task
 * and the backtrace; this endpoint makes them available over HTTP instead of
 * only via cable.
 * ========================================================================= */

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF

/* GET /api/coredump - summary of the stored coredump as JSON
 * ({"present":false} if there is none). */
static esp_err_t h_coredump_get(httpd_req_t *r) {
  if (!admin_key_ok(r)) {
    httpd_resp_send_err(r, HTTPD_401_UNAUTHORIZED, "wrong key");
    return ESP_FAIL;
  }

  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");

  /* Do not use esp_core_dump_image_check() for the presence check - after
   * erasing it reports a different error than "not present".
   * esp_core_dump_image_get() is not clean either: its header comment
   * promises ESP_ERR_NOT_FOUND after esp_core_dump_image_erase(), but the
   * actual implementation (core_dump_flash.c) writes 0xFFFFFFFF as the size
   * field on erase and returns ESP_ERR_INVALID_SIZE for it - ESP_ERR_NOT_FOUND
   * only applies there if the partition itself is missing (verified on
   * hardware). Both count as "no dump" here. */
  size_t cd_addr = 0, cd_size = 0;
  esp_err_t ge = esp_core_dump_image_get(&cd_addr, &cd_size);
  if (ge == ESP_ERR_NOT_FOUND || ge == ESP_ERR_INVALID_SIZE) {
    return httpd_resp_send(r, "{\"present\":false}", HTTPD_RESP_USE_STRLEN);
  }

  esp_core_dump_summary_t sum;
  esp_err_t se = esp_core_dump_get_summary(&sum);
  if (se != ESP_OK) {
    /* Present (the check above was not NOT_FOUND) but not readable - e.g.
     * corrupted. Report it anyway instead of pretending there is nothing. */
    char buf[112];
    snprintf(buf, sizeof(buf), "{\"present\":true,\"readable\":false,\"err\":\"%s\"}",
             esp_err_to_name(se));
    return httpd_resp_send(r, buf, HTTPD_RESP_USE_STRLEN);
  }

  char reason[200] = "";
  esp_core_dump_get_panic_reason(reason, sizeof(reason));
  char reason_esc[410];
  json_escape(reason, reason_esc, sizeof(reason_esc));

  char task_esc[40];
  json_escape(sum.exc_task, task_esc, sizeof(task_esc));

  char bt[400] = "";
  char extra[80] = "";
#if CONFIG_IDF_TARGET_ARCH_XTENSA
  /* Backtrace as hex addresses - resolvable to file+line with the matching
   * firmware.elf via xtensa-esp32-elf-addr2line. */
  size_t o = 0;
  for (uint32_t i = 0; i < sum.exc_bt_info.depth && i < 16 && o + 13 < sizeof(bt); i++) {
    o += snprintf(bt + o, sizeof(bt) - o, "%s\"0x%08lx\"", i ? "," : "",
                  (unsigned long)sum.exc_bt_info.bt[i]);
  }
  const bool corrupted = sum.exc_bt_info.corrupted;
#else
  /* RISC-V: ESP-IDF builds no backtrace on the device, only registers and a
   * raw stack dump (the full backtrace comes from espcoredump.py on the host).
   * The return address ra is the one call-site entry available without the
   * ELF; mcause/mtval say what happened (e.g. mcause 5 = load access fault,
   * mtval = the faulting address). The JSON shape stays the same. */
  snprintf(bt, sizeof(bt), "\"0x%08lx\"", (unsigned long)sum.ex_info.ra);
  snprintf(extra, sizeof(extra), ",\"mcause\":\"0x%08lx\",\"mtval\":\"0x%08lx\"",
           (unsigned long)sum.ex_info.mcause, (unsigned long)sum.ex_info.mtval);
  const bool corrupted = false;
#endif

  char buf[1280];
  snprintf(buf, sizeof(buf),
    "{\"present\":true,\"readable\":true,\"corrupted\":%s,"
    "\"task\":\"%s\",\"pc\":\"0x%08lx\",\"reason\":\"%s\",\"bt\":[%s]%s}",
    corrupted ? "true" : "false",
    task_esc, (unsigned long)sum.exc_pc, reason_esc, bt, extra);

  return httpd_resp_send(r, buf, HTTPD_RESP_USE_STRLEN);
}

/* DELETE /api/coredump - erase the stored coredump. */
static esp_err_t h_coredump_delete(httpd_req_t *r) {
  if (!admin_key_ok(r)) {
    httpd_resp_send_err(r, HTTPD_401_UNAUTHORIZED, "wrong key");
    return ESP_FAIL;
  }
  esp_core_dump_image_erase();
  httpd_resp_set_type(r, "application/json");
  return httpd_resp_send(r, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

#endif /* CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF */

/* ===========================================================================
 * Crash history (crashlog.h)
 * ---------------------------------------------------------------------------
 * The coredump above is always only the LAST one and carries no time. This
 * lists the last CRASHLOG_N unexpected resets with their time - including
 * those without a coredump (brownout, hardware watchdog). Protected like
 * /api/coredump.
 * "e" = crash time UTC (0 = unknown), "up" = uptime until then (-1 =
 * unknown), "rst" = esp_reset_reason_t (255 = imported from before the
 * history existed), "f" = CRASH_F_* flags.
 * ========================================================================= */

/* GET /api/crashes - the crash history as JSON, newest first. */
static esp_err_t h_crashes_get(httpd_req_t *r) {
  if (!admin_key_ok(r)) {
    httpd_resp_send_err(r, HTTPD_401_UNAUTHORIZED, "wrong key");
    return ESP_FAIL;
  }
  crash_rec_t *recs = (crash_rec_t *)malloc(sizeof(crash_rec_t) * CRASHLOG_N);
  const size_t cap = 4608;
  char *buf = (char *)malloc(cap);
  if (!recs || !buf) {
    free(recs); free(buf);
    httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    return ESP_FAIL;
  }
  const size_t n = crashlog_get(recs, CRASHLOG_N);

  size_t o = snprintf(buf, cap, "{\"now\":%lu,\"recs\":[", (unsigned long)crashlog_now());
  for (size_t i = 0; i < n && o < cap; i++) {
    const crash_rec_t *c = &recs[i];
    char task[40], reason[200], build[28];
    json_escape(c->task, task, sizeof(task));
    json_escape(c->reason, reason, sizeof(reason));
    json_escape(c->build, build, sizeof(build));
    char bt[8 * 13 + 1] = "";
    size_t b = 0;
    for (uint8_t k = 0; k < c->bt_n && k < 8; k++)
      b += snprintf(bt + b, sizeof(bt) - b, "%s\"%08lx\"", k ? "," : "", (unsigned long)c->bt[k]);
    o += snprintf(buf + o, cap - o,
      "%s{\"e\":%lu,\"up\":%ld,\"rst\":%u,\"f\":%u,\"build\":\"%s\",\"task\":\"%s\","
      "\"pc\":\"%08lx\",\"reason\":\"%s\",\"bt\":[%s]}",
      i ? "," : "", (unsigned long)c->epoch,
      c->uptime_s == 0xFFFFFFFFu ? -1L : (long)c->uptime_s,
      (unsigned)c->rst, (unsigned)c->flags, build, task,
      (unsigned long)c->pc, reason, bt);
  }
  if (o < cap) o += snprintf(buf + o, cap - o, "]}");
  if (o >= cap) o = cap - 1;
  free(recs);

  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  esp_err_t err = httpd_resp_send(r, buf, o);
  free(buf);
  return err;
}

/* DELETE /api/crashes - clear the crash history. */
static esp_err_t h_crashes_delete(httpd_req_t *r) {
  if (!admin_key_ok(r)) {
    httpd_resp_send_err(r, HTTPD_401_UNAUTHORIZED, "wrong key");
    return ESP_FAIL;
  }
  crashlog_clear();
  httpd_resp_set_type(r, "application/json");
  return httpd_resp_send(r, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* ===========================================================================
 * Start
 * ========================================================================= */

/* GET /api/log?since=N - console output from byte position N on, as plain
 * text (see conlog.h). X-Log-Next carries the position for the next poll;
 * X-Log-Reset: 1 means the requested position was no longer (or not yet)
 * available - the board rebooted or the ring wrapped - and the reply starts
 * at the oldest byte held. A reply carries at most LOG_REPLY_MAX bytes; the
 * portal asks again right away when it got a full one. That keeps the ring
 * lock short and the buffer on the httpd stack small. Protected like the
 * other diagnostics: the log shows network details. */
#define LOG_REPLY_MAX 2048
static esp_err_t h_log(httpd_req_t *r) {
  if (!admin_key_ok(r)) {
    httpd_resp_send_err(r, HTTPD_401_UNAUTHORIZED, "wrong key");
    return ESP_FAIL;
  }
  uint32_t since = 0;
  bool prev = false;
  char q[32], v[12];
  if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) {
    if (httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK)
      since = (uint32_t)strtoul(v, NULL, 10);
    if (httpd_query_key_value(q, "prev", v, sizeof(v)) == ESP_OK)
      prev = atoi(v) != 0;
  }

  char buf[LOG_REPLY_MAX];
  if (prev) {
    /* The end of the previous boot's log from NVS (see conlog.h), with the
     * reset reason that ended that boot in X-Log-Rst; empty if none. */
    uint8_t rst = 0;
    const size_t pn = conlog_get_prev(buf, sizeof(buf), &rst);
    char rst_s[4];
    snprintf(rst_s, sizeof(rst_s), "%u", (unsigned)rst);
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Log-Rst", rst_s);
    return httpd_resp_send(r, buf, pn);
  }
  uint32_t next = since;
  bool reset = false;
  const size_t n = conlog_read(since, buf, sizeof(buf), &next, &reset);

  char next_s[12];
  snprintf(next_s, sizeof(next_s), "%lu", (unsigned long)next);
  httpd_resp_set_type(r, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  httpd_resp_set_hdr(r, "X-Log-Next", next_s);
  httpd_resp_set_hdr(r, "X-Log-Reset", reset ? "1" : "0");
  return httpd_resp_send(r, buf, n);
}

/* GET /api/cpu - CPU load per task over the last and the busiest window
 * (see cpustat.h); ?reset=1 forgets the busiest one first, so a new load
 * test starts clean. Admin key, like the other diagnostics. */
static esp_err_t h_cpu(httpd_req_t *r) {
  if (!admin_key_ok(r)) {
    httpd_resp_send_err(r, HTTPD_401_UNAUTHORIZED, "wrong key");
    return ESP_FAIL;
  }
  char q[16], v[4];
  if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK &&
      httpd_query_key_value(q, "reset", v, sizeof(v)) == ESP_OK && atoi(v)) {
    cpustat_reset_peak();
  }
  char buf[900];
  const size_t n = cpustat_json(buf, sizeof(buf));
  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  return httpd_resp_send(r, buf, n);
}

/* Largest test transfer GET/POST /api/speed accepts, in MiB. */
#define SPEED_MAX_MB 200

/* GET /api/speed?mb=N - send N MiB (default 20) of zeros from the management
 * stack. Together with POST it measures what the bridge's own Wi-Fi link
 * carries at its location, without the Ethernet side involved: if this is
 * no faster than traffic through the bridge, the radio link is the limit,
 * not the Ethernet driver. Admin key; occupies the HTTP server while it
 * runs. */
static esp_err_t h_speed_get(httpd_req_t *r) {
  if (!admin_key_ok(r)) {
    httpd_resp_send_err(r, HTTPD_401_UNAUTHORIZED, "wrong key");
    return ESP_FAIL;
  }
  uint32_t mb = 20;
  char q[16], v[8];
  if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK &&
      httpd_query_key_value(q, "mb", v, sizeof(v)) == ESP_OK) {
    mb = (uint32_t)strtoul(v, NULL, 10);
  }
  if (mb < 1) mb = 1;
  if (mb > SPEED_MAX_MB) mb = SPEED_MAX_MB;

  static const uint8_t zeros[4096] = {};
  httpd_resp_set_type(r, "application/octet-stream");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  const uint32_t chunks = mb * (1024 * 1024 / sizeof(zeros));
  for (uint32_t i = 0; i < chunks; i++) {
    if (httpd_resp_send_chunk(r, (const char *)zeros, sizeof(zeros)) != ESP_OK) return ESP_FAIL;
  }
  return httpd_resp_send_chunk(r, NULL, 0);
}

/* POST /api/speed - read and discard the request body, reply with its size
 * and the time it took: the upload half of the Wi-Fi-only measurement (see
 * h_speed_get()). Admin key. */
static esp_err_t h_speed_post(httpd_req_t *r) {
  if (!admin_key_ok(r)) {
    httpd_resp_send_err(r, HTTPD_401_UNAUTHORIZED, "wrong key");
    return ESP_FAIL;
  }
  if (r->content_len > (size_t)SPEED_MAX_MB * 1024 * 1024) {
    httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "too large");
    return ESP_FAIL;
  }
  static char sink[4096];
  const int64_t t0 = esp_timer_get_time();
  size_t left = r->content_len, got = 0;
  while (left > 0) {
    const int n = httpd_req_recv(r, sink, left < sizeof(sink) ? left : sizeof(sink));
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) return ESP_FAIL;
    left -= (size_t)n;
    got += (size_t)n;
  }
  const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
  char out[96];
  snprintf(out, sizeof(out), "{\"bytes\":%lu,\"ms\":%lu,\"kbps\":%lu}",
           (unsigned long)got, (unsigned long)ms,
           (unsigned long)(ms ? (uint64_t)got * 8 / ms : 0));
  httpd_resp_set_type(r, "application/json");
  return httpd_resp_sendstr(r, out);
}

#ifdef BRIDGE_PANIC_TEST
/* POST /api/panic_test - deliberately crash, to verify on real hardware that
 * a panic writes a coredump and that RTC and .noinit RAM (crash heartbeat,
 * previous-boot log) survive the reset. Only exists in builds made with
 * -DBRIDGE_PANIC_TEST, never in the regular firmware, and additionally
 * requires the admin key. */
static esp_err_t h_panic_test(httpd_req_t *r) {
  if (!admin_key_ok(r)) {
    httpd_resp_send_err(r, HTTPD_401_UNAUTHORIZED, "wrong key");
    return ESP_FAIL;
  }
  httpd_resp_sendstr(r, "{\"ok\":true}");
  printf("[TEST] Deliberate panic requested via /api/panic_test\n");
  fflush(stdout);
  delay(200);   /* let the reply and the log line go out */
  abort();
  return ESP_OK;
}
#endif

/* Start the HTTP server and register all routes (see web.h). */
bool web_start(bool provisioning) {
  s_provisioning = provisioning;

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size       = 6144;
  cfg.max_uri_handlers = 24;    /* currently 21 routes (uris[] below) - if this
                                 * is too small, registration fails silently */
  cfg.recv_wait_timeout = 20;   /* the OTA upload needs patience */
  cfg.lru_purge_enable = true;

  /* Derive the socket count from the lwIP budget instead of using the IDF
   * default (7). In addition to max_open_sockets the httpd uses 3 sockets
   * internally; if the sum exceeds CONFIG_LWIP_MAX_SOCKETS it does NOT start
   * at all. The 2 reserved sockets are for MQTT and DNS.
   * Computed here on purpose rather than as a fixed number - otherwise it
   * silently breaks again the next time someone lowers LWIP_MAX_SOCKETS. */
  int free_sockets = CONFIG_LWIP_MAX_SOCKETS - 3 /* httpd internal */ - 2 /* MQTT, DNS */;
  if (free_sockets < 2) free_sockets = 2;   /* lower limit: otherwise the portal is useless */
  cfg.max_open_sockets = free_sockets;

  esp_err_t err = httpd_start(&s_httpd, &cfg);
  if (err != ESP_OK) {
    /* Print the error code too - "start failed" alone does not tell the
     * cause, which otherwise only appears in the IDF log line above. */
    printf("[WEB ] Start failed: %s (max_open_sockets=%d, LWIP_MAX_SOCKETS=%d)\n",
           esp_err_to_name(err), cfg.max_open_sockets, CONFIG_LWIP_MAX_SOCKETS);
    return false;
  }

  const httpd_uri_t uris[] = {
    { "/",            HTTP_GET,  h_root,        NULL },
    { "/api/status",  HTTP_GET,  h_status,      NULL },
    { "/api/disconnects",HTTP_GET,h_disconnects,NULL },
    { "/api/config",  HTTP_GET,  h_config_get,  NULL },
    { "/api/config",  HTTP_POST, h_config_post, NULL },
    { "/api/reboot",  HTTP_POST, h_reboot,      NULL },
    { "/api/eth_reset",HTTP_POST,h_eth_reset,   NULL },
    { "/api/autotune",HTTP_POST, h_autotune,    NULL },
    { "/api/autotune",HTTP_GET,  h_autotune_get,NULL },
    { "/api/scan",    HTTP_GET,  h_scan,        NULL },
    { "/api/update",  HTTP_POST, h_update,      NULL },
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    { "/api/coredump",HTTP_GET,    h_coredump_get,    NULL },
    { "/api/coredump",HTTP_DELETE, h_coredump_delete, NULL },
#endif
    { "/api/crashes", HTTP_GET,    h_crashes_get,     NULL },
    { "/api/crashes", HTTP_DELETE, h_crashes_delete,  NULL },
    { "/api/log",     HTTP_GET,    h_log,             NULL },
    { "/api/cpu",     HTTP_GET,    h_cpu,             NULL },
    { "/api/speed",   HTTP_GET,    h_speed_get,       NULL },
    { "/api/speed",   HTTP_POST,   h_speed_post,      NULL },
#ifdef BRIDGE_PANIC_TEST
    { "/api/panic_test", HTTP_POST, h_panic_test,     NULL },
#endif
  };
  for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
    httpd_register_uri_handler(s_httpd, &uris[i]);
  }

  printf("[WEB ] HTTP server running\n");
  return true;
}
