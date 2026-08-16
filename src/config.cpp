#include "config.h"

#include <string.h>
#include <stdio.h>

#include "nvs.h"
#include "nvs_flash.h"

BridgeConfig g_cfg;

void cfg_defaults(BridgeConfig *c) {
  memset(c, 0, sizeof(*c));
  c->magic = CFG_MAGIC;
  strcpy(c->name, "wifibridge");

  /* Sinnvolle Startwerte - der Nutzer korrigiert sie im Portal ohnehin. */
  c->mgmt_ip[0] = 192; c->mgmt_ip[1] = 168; c->mgmt_ip[2] = 178; c->mgmt_ip[3] = 250;
  c->mgmt_mask[0] = 255; c->mgmt_mask[1] = 255; c->mgmt_mask[2] = 255; c->mgmt_mask[3] = 0;
  c->mgmt_gw[0] = 192; c->mgmt_gw[1] = 168; c->mgmt_gw[2] = 178; c->mgmt_gw[3] = 1;

  c->mqtt_port    = 1883;
  c->telemetry_s  = 10;
  c->configured   = false;

  /* Die Feinabstimmungsfelder bleiben bewusst auf 0. Das ist kein
   * vergessener Default, sondern die Konvention aus config.h: 0 heisst
   * "nimm den Wert aus sdkconfig bzw. den PHY-Default". Wer hier feste
   * Zahlen eintraegt, baut sich die zweite Quelle wieder ein, die in
   * bridge.cpp gerade erst entfernt wurde. */
}

void cfg_load(void) {
  cfg_defaults(&g_cfg);

  nvs_handle_t h;
  if (nvs_open(CFG_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
    printf("[CFG ] Kein NVS-Namespace - Defaults\n");
    return;
  }

  /* Erst die abgelegte Groesse erfragen. Nach einem OTA-Update kann die
   * Struktur gewachsen (oder bei einem Downgrade geschrumpft) sein - dann
   * uebernehmen wir den gemeinsamen Anfang und lassen den Rest auf Default.
   * Ohne das waere nach jedem Firmware-Update die Konfiguration weg, und
   * das Board haenge im AP-Modus statt an seinem Einbauort zu arbeiten. */
  size_t sz = 0;
  esp_err_t err = nvs_get_blob(h, CFG_NVS_KEY, NULL, &sz);
  if (err != ESP_OK || sz < sizeof(uint32_t)) {
    nvs_close(h);
    printf("[CFG ] Keine gespeicherte Konfiguration - Defaults\n");
    return;
  }

  uint8_t *raw = (uint8_t *)malloc(sz);
  if (!raw) { nvs_close(h); return; }

  err = nvs_get_blob(h, CFG_NVS_KEY, raw, &sz);
  nvs_close(h);

  if (err != ESP_OK) { free(raw); return; }

  uint32_t magic;
  memcpy(&magic, raw, sizeof(magic));
  if (magic != CFG_MAGIC) {
    free(raw);
    printf("[CFG ] Magic passt nicht - Defaults\n");
    return;
  }

  BridgeConfig tmp;
  cfg_defaults(&tmp);
  memcpy(&tmp, raw, sz < sizeof(tmp) ? sz : sizeof(tmp));
  free(raw);

  g_cfg = tmp;
  printf("[CFG ] Geladen (%u von %u Byte): name='%s' ssid1='%s'\n",
                (unsigned)sz, (unsigned)sizeof(tmp), g_cfg.name, g_cfg.ssid1);
}

bool cfg_save(void) {
  g_cfg.magic = CFG_MAGIC;

  nvs_handle_t h;
  if (nvs_open(CFG_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;

  esp_err_t err = nvs_set_blob(h, CFG_NVS_KEY, &g_cfg, sizeof(g_cfg));
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);

  printf("[CFG ] Speichern: %s\n", err == ESP_OK ? "ok" : "FEHLER");
  return err == ESP_OK;
}

bool cfg_factory_reset(void) {
  nvs_handle_t h;
  if (nvs_open(CFG_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  nvs_erase_all(h);
  esp_err_t err = nvs_commit(h);
  nvs_close(h);
  cfg_defaults(&g_cfg);
  return err == ESP_OK;
}

/* --------------------------------------------------------------------------
 * IPv4-Helfer
 * ------------------------------------------------------------------------ */

bool parse_ipv4(const char *s, uint8_t out[4]) {
  if (!s) return false;
  unsigned a, b, c, d;
  if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
  if (a > 255 || b > 255 || c > 255 || d > 255) return false;
  out[0] = (uint8_t)a; out[1] = (uint8_t)b;
  out[2] = (uint8_t)c; out[3] = (uint8_t)d;
  return true;
}

void format_ipv4(const uint8_t ip[4], char *out, size_t out_len) {
  snprintf(out, out_len, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}
