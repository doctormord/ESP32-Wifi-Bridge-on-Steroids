# History

Append-only chronological log of what happened on this project. **Never delete or rewrite past entries** — if something turns out to be wrong or superseded, add a new entry that says so; don't edit history away. New entries go at the bottom.

---

## 2026-08-11

- **Build was failing** on `pio run -e wt32-eth01`: `fatal error: esp_crt_bundle.h: No such file or directory` while compiling `esp_rmaker_ota.c` (ESP RainMaker, pulled in transitively by the Arduino-ESP32 3.x core via the IDF Component Manager — the project never used RainMaker itself).
  - Root cause: `sdkconfig.wt32-eth01` (the cached, fully-resolved sdkconfig PlatformIO/ESP-IDF generates from `sdkconfig.defaults`) had drifted out of sync with `sdkconfig.defaults`. `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` was off in the cache but on in the defaults; ESP-IDF's kconfig merge keeps an explicit cached value even when the defaults file changes underneath it.
  - Also found `CONFIG_COMPILER_OPTIMIZATION_PERF` (−O2) cached instead of `CONFIG_COMPILER_OPTIMIZATION_SIZE` (−Os) from defaults, plus stale `BT_ENABLED`/`PM_ENABLE`/MQTT-transport flags.
  - Fix: deleted `sdkconfig.wt32-eth01` (backed up first) and let it regenerate cleanly from `sdkconfig.defaults`. Build then succeeded, but firmware was 104.4% of the 960 KB OTA partition (42 KB over) with the stale optimization setting; after the O2→Os fix that dropped to 95.3% (936,779 / 983,040 bytes).
- **Removed Arduino entirely, switched to pure ESP-IDF** (`framework = espidf` instead of `arduino, espidf`). This was proposed and implemented in a separate claude.ai conversation; the exported files (`platformio.ini`, both `CMakeLists.txt`, `compat.h`, `sdkconfig.defaults`, and all of `src/*`) were found in `~/Downloads/files.zip`, reviewed file-by-file against the previous Arduino-based sources (near-identical line counts, confirmed as a faithful port, not a rewrite), and installed.
  - `setup()`/`loop()` → `bridge_setup()`/`bridge_loop()` called from `app_main()`. `millis()`/`delay()` kept via a small `compat.h` shim over `esp_timer`/`vTaskDelay` rather than rewriting call sites.
  - This eliminates RainMaker/Insights entirely (no more `managed_components/` at all) — the certificate-bundle problem is now structurally gone rather than worked around.
  - Old Arduino-based project state backed up to `~/Downloads/esp32_wifi_bridge_pio_arduino_backup_20260811_002243/` before any files were overwritten.
  - Result: build succeeds at 91.2% flash (896,191 / 983,040 bytes), up from 46 KB free to ~87 KB free versus the last working Arduino build.
  - **Not yet tested on real hardware** — compile-only verification so far. See `backlog.md`.
- Ran `/init` and created `CLAUDE.md`, plus this `docs/content/` doc set (`backlog.md`, `handoff.md`, `handover.md`, `history.md`).
- **Optimization pass** (flash savings + throughput/latency), driven by symbol-level analysis of `firmware.elf` rather than guesswork. Flash went **91.2% → 80.7%** (896,191 → 793,155 bytes; ~103 KB freed, headroom now ~190 KB).
  - `CONFIG_LIBC_NEWLIB_NANO_FORMAT=y` — the full newlib printf/scanf carries float support the project never uses (no `%f`/`%g`/`%ll` anywhere). Measured effect: `_vfprintf_r` 11,429→698 B, `_svfprintf_r` 11,210→624 B, `__ssvfscanf_r` 8,729→784 B (~29 KB).
  - `CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n` — 802.1X/EAP is never used (PSK network). Kconfig's own help text quantifies this at ~60 KB.
  - `CONFIG_LWIP_IPV6=n` — management interface is static IPv4; bridged traffic bypasses lwIP entirely as raw frames, so the client's own IPv6 is unaffected.
  - `CONFIG_ESP_WIFI_NVS_ENABLED=n` — credentials live in our own NVS blob and `esp_wifi_set_storage(WIFI_STORAGE_RAM)` is called, so the IDF's separate Wi-Fi NVS store was dead code.
  - `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT=y` — asserts still abort (so OTA rollback triggers), just without embedded file/line strings.
  - **Flash clock 40 → 80 MHz** (`board_build.f_flash = 80000000L`). The CPU was already maxed at 240 MHz (the ESP32 only offers 80/160/240 — there is no supported overclock beyond that), so flash bandwidth was the only real speed lever left. Verified in the image header, *not* in sdkconfig: `CONFIG_ESPTOOLPY_FLASHFREQ` still reads `40m` because PlatformIO patches the frequency in at esptool time, and the image header is what the bootloader actually honors.
  - **Hot-path callbacks moved to IRAM** (`IRAM_ATTR` on `eth_rx_cb` / `wifi_rx_cb`; `is_for_mgmt` gets inlined into the latter). Verified via `nm`: both now sit at `0x4008xxxx`. Avoids a flash cache miss on every forwarded packet. IRAM still has 26 KB free.
  - **Wi-Fi buffers raised** 16/32/32 → 24/48/48 with `RX_BA_WIN` 16 → 24 (the Kconfig rule `RX_BA_WIN <= STATIC_RX` and `<= DYNAMIC_RX/2` allows exactly 24 at these values). Plenty of DRAM available (~148 KB free).
  - **Removed the hardcoded buffer sizes from `bridge.cpp`.** They were commented as "doppelt abgesichert" (belt-and-braces) but actually *overrode* the Kconfig values — every increase in `sdkconfig.defaults` had been silently ineffective. `WIFI_INIT_CONFIG_DEFAULT()` reads them from Kconfig; `sdkconfig.defaults` is now the single source of truth.
- **Found and fixed a latent boot-blocking bug while verifying the flash header** (predates the optimization pass, would have hit on first hardware bring-up): the image header declared **4 MB** flash on a 2 MB board. `board_build.flash_size` only feeds `CONFIG_ESPTOOLPY_FLASHSIZE`; the header nibble comes from `board_upload.flash_size`, which was inheriting 4 MB from the `esp32dev` board manifest. `esp_flash_init_default_chip()` treats "detected < header" as a hard error (`ESP_ERR_FLASH_SIZE_NOT_MATCH`, "Probe failed"), not a warning — so the board would not have come up. Fixed by adding `board_upload.flash_size = 2MB` (+ `board_upload.maximum_size`); header now reads DIO / 80 MHz / 2 MB.
- **Added a tuning panel to the web portal** so the performance knobs can be experimented with without reflashing. Flash 80.7% → 81.4% (+7.3 KB, almost all of it the added HTML/CSS/JS in `.rodata`).
  - Live, no reboot: TX power, `eth_tx_retries`, `wifi_tx_retries`. Applied via the new `bridge_apply_live_tuning()`, which the config POST handler calls after a successful save.
  - Reboot required: `static_rx_buf` / `dynamic_rx_buf` / `dynamic_tx_buf` / `rx_ba_win` (they feed `wifi_init_config_t`), HT40, and an 802.11b-off toggle.
  - TX power is exposed as a dropdown of the 11 real PHY steps rather than a free number field, because `esp_wifi_set_max_tx_power()` silently rounds down to those (mapping table is in `esp_wifi.h`). `/api/status` now also reports the driver's actual value (`txp`) so the quantization is visible.
  - `save()` no longer always reboots — it takes a flag; the Konfiguration card still saves-and-reboots, the tuning card saves in place so you can watch the throughput/drop counters react.
  - The BA-window rule (`rx_ba_win <= static_rx` and `<= dynamic_rx/2`) is enforced in `bridge_wifi_start()` rather than trusting portal validation, because an out-of-range value makes `esp_wifi_init()` fail and the fallback path would silently discard *all* buffer overrides.
  - Raised the config POST body limit 1400 → 2040 bytes: the form always submits every field, and fully percent-encoded passwords/SSIDs plus nine new fields could have exceeded the old bound.
  - Verified statically (no hardware): build succeeds, embedded HTML tag-balances, every `$('id')` and `CFG` entry in the JS resolves to a real element, and the JS parses cleanly under JavaScriptCore.
- **Tried and reverted:** `CONFIG_MBEDTLS_TLS_DISABLED=y`. ~30 KB of mbedTLS handshake/X.509 code is still linked despite nothing in the project using TLS, but disabling the TLS protocol layer fails at link time — `esp-tls` compiles `esp_tls_mbedtls.c` unconditionally and hard-references `mbedtls_ssl_*`, and it reaches the build through `tcp_transport` → `esp-mqtt`. Documented in `sdkconfig.defaults` as a do-not-retry with the reason.

## 2026-08-13

- **Serial connection to the WT32-ETH01 established.** Chip identifies as ESP32-D0WD-V3 rev 3.1, MAC `d4:e9:f4:bc:7c:40`, 40 MHz crystal. Download mode is reached automatically (DTR → EN with IO0 jumpered to GND), so flashing needs no manual intervention.
- **The board has 8 MB of flash, not 2 MB.** The project's founding assumption was wrong. Measured two independent ways: `esptool flash-id` reports Zbit `0x5E` / device `0x4017` (capacity byte `0x17` = 8 MiB), and an aliasing test confirmed `0x001000` and `0x401000` hold different data, which a 4 MB part could not do. Leftover image headers found in flash (2 MB at `0x1000`, 4 MB at `0x401000`) explain where the confusion came from.
  - **This invalidates the "boot-blocking bug" reported on 2026-08-11.** That entry claimed a 4 MB header on a 2 MB board would prevent boot. On the real hardware (8 MB) a 4 MB header takes the *warning* path — `esp_flash_init_default_chip()` only errors when detected size is **smaller** than the header. The board would have booted, just capped at 4 MB. The subsequent "fix" to 2 MB was not harmful but capped it at a quarter of the chip.
  - New layout in `partitions_8mb_ota.csv`: nvs 24 K, otadata 8 K, **app0/app1 3 MB each**, coredump 64 K, ~1.8 MB left unallocated. Chosen deliberately generous because a partition table cannot be changed over OTA — revising it later means bringing the board back to a cable.
  - **Coredump re-enabled** (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`) now that there's room. The original reasoning for disabling it ("can't read it out without serial access anyway") was backwards: precisely *because* the deployed board has no serial access, a crash would otherwise yield no information beyond "it rebooted".
  - Result: flash usage dropped from 81.4% of a 960 KB slot to **25.9% of a 3 MB slot** (813,639 bytes). Header verified as DIO / 80 MHz / 8 MB.
- **Toolchain broke mid-session and was worked around.** PlatformIO auto-updated 6.1.18 → 6.1.19 between builds; the pioarduino platform hard-rejects Python ≥ 3.14, and Homebrew's `pio` runs on `python@3.14`. Two dead ends were tried and rejected before finding the fix:
  - Rebuilding `~/.platformio/penv` on Python 3.13 — silently reverted within seconds, because Homebrew's PlatformIO recreates that venv with `uv` on 3.14. This also explains why an earlier switch to 3.13 (in the claude.ai session) didn't hold.
  - Moving `~/.platformio/platforms/espressif32` aside — PlatformIO immediately re-downloaded it from the URL, and the current pioarduino release carries the same check. (The older `@src-…` copy that lacked it was simply out of date.)
  - Working fix: a standalone venv `~/.pio-py313` (Python 3.13.15) with `platformio==6.1.19` plus `pyyaml` (needed by the platform's `component_manager.py`, not a PlatformIO dependency). Builds now run as `~/.pio-py313/bin/pio`. Documented in `CLAUDE.md`.
- Still **not flashed to hardware** — the 8 MB build exists but has not been written to the board yet.
- **First hardware bring-up.** Flashed over serial (CH340 at 115200 — 921600 from the old config was pure wishful thinking; even an `esptool read-flash` at 460800 aborted).
  - **Power problem found first:** with the board fed from the USB adapter's 5 V pin, the CH340 itself re-enumerated on USB roughly once per second (registry ID changed on every sample). Diagnostic that settled it: a crashing ESP32 cannot drag the USB adapter down — the adapter hangs off the Mac, not the ESP32 — so a flapping *adapter* means the supply rail is collapsing, not the firmware. It only appeared once the firmware started the Wi-Fi AP (flashing itself works because Wi-Fi is off). Resolved with an external 5 V supply, GND shared, adapter 5 V line disconnected.
  - **Boot log confirmed the whole flash/partition rework**: `SPI Speed: 80MHz`, `SPI Mode: DIO`, `SPI Flash Size: 8MB`, partition table exactly as designed, provisioning mode entered as predicted, AP `wifibridge-setup-7C41`.
  - **Real bug caught that only hardware would have shown:** `httpd: Config option max_open_sockets is too large (max allowed 5, 3 sockets used by HTTP server internally)` → `[WEB ] Start fehlgeschlagen`. `HTTPD_DEFAULT_CONFIG()` asks for 7 sockets plus 3 internal, while `CONFIG_LWIP_MAX_SOCKETS=8` (deliberately small) allowed 5. The board would have advertised a setup AP that nothing could connect to — unconfigurable in its as-shipped state, and a physical retrieval if it had already been installed.
    - Fixed by raising `CONFIG_LWIP_MAX_SOCKETS` 8 → 12 **and** deriving `cfg.max_open_sockets` from it in `web_start()` (`LWIP_MAX_SOCKETS - 3 internal - 2 reserved for MQTT/DNS`) instead of accepting the IDF default. A hardcoded number would have drifted apart again the next time someone tuned the socket budget.
    - Also made the failure message print the actual `esp_err_t` and both socket numbers — the original bare "Start fehlgeschlagen" gave no clue; the real cause was only in the IDF log line above it.
  - Second flash verified the fix: `[WEB ] HTTP-Server laeuft`.
  - Toolchain note for future sessions: `esptool` is not in `~/.pio-py313`; use `~/.platformio/penv/bin/python -m esptool` for direct chip access (the Python 3.14 restriction applies to the platform, not to esptool).
- **Bridge mode verified end-to-end on hardware.** Ethernet link, MAC sniffing (`aa:bb:cc:dd:ee:01` from a laptop), MAC cloning, Wi-Fi association, data path activation, management netif and HTTP server all work simultaneously — the "both work" case in the diagnostic matrix, which confirms the `esp_wifi_internal_reg_rxcb` takeover *and* the selective delivery in `is_for_mgmt()`. Portal answers in ~94 ms from another host on the LAN; `drop_up`/`drop_down` both 0; free heap steady at ~121 KB (the raised Wi-Fi buffers did not starve it); `txp` reads 78 quarter-dBm = 19.5 dBm, the PHY default, since `tx_power` is left at 0.
- **Second hardware-only bug, and the more dangerous one: a boot loop with no way out.** After a failed Wi-Fi association the fallback called `enter_provisioning()` → `portal_start_ap()`, which ran `esp_netif_create_default_wifi_sta()` a second time — `bridge_wifi_start()` had already created that netif. A duplicate `if_key` is not reported as an error code but trips an **assert**, so the board crashed, rebooted, retried Wi-Fi, crashed again. The portal never came up, meaning a deployed board would have been unrecoverable without physical access.
  - Fixed in `main.cpp` by *not* unwinding the half-initialised stack by hand (many failure modes, itself untested) but forcing a clean restart into the portal, flagged through an `RTC_NOINIT_ATTR` word that survives `esp_restart()` but not a cold boot — so a power cycle still retries the bridge normally.
  - `portal_start_ap()` additionally made idempotent (create netifs only if the if_key is free, tolerate an already-initialised Wi-Fi stack) as a second line of defence. The portal is the rescue path for a device with no serial access; it must never assert, whatever route reaches it.
- **Config error found in the user's provisioning input** (not a firmware bug): gateway `192.168.178.9` with IP `192.168.8.245/24` — a different subnet, so no usable default route and, since DNS is pointed at the gateway, no name resolution either. Harmless until MQTT points somewhere off-subnet.

## 2026-08-14 — Durchsatz: Vergleich gegen das Rust-Original

**Ergebnis: die 50 Mbit des Originals sind an diesem Standort nicht reproduzierbar.** Das Rust-Original, gebaut und per OTA auf dasselbe Board gespielt, am selben Ort, am selben Repeater, mit demselben Speedtest gemessen: **8,6 / 1,5 Mbit**. Unsere Firmware lag im selben Zeitraum bei 7,8–11 / 3,3–3,8 — im Upload also deutlich besser als das Original.

Damit ist die Firmware als Ursache der niedrigen Werte ausgeschlossen. Der Engpass ist die Umgebung: Verbindung über einen Mesh-Repeater (BSSID `ba:bb:a3:e4:a8:bd`, Locally-Administered-Bit gesetzt) statt über den Router, 2,4 GHz, RSSI zwischen −49 und −63 dBm schwankend. Ein MacBook direkt daneben schaffte 63/18 — aber mit MIMO-Radio gegen die eine Antenne des ESP32.

### Methodische Lehre

Die Messreihe des Tages schwankte zwischen 5,6 und 12 Mbit im Download **ohne erkennbaren Zusammenhang zu den jeweils geänderten Parametern**. Die Streuung der Messmethode (Speedtest gegen einen Internet-Server, über Router und Repeater) war größer als der gesuchte Effekt. Mehrere Zwischenschlüsse des Tages ("Yield hilft", "mehr Puffer helfen") beruhten auf Einzelmessungen und sind nicht belegt.

### Was trotzdem bleibt, weil strukturell begründet

- **Ein Sendeversuch statt Warteschleife** (`ETH_TX_RETRIES_DEF 1`). Die Retry-Schleife blockierte den `emac_rx`-Task; die Pakete gingen dann im Ethernet-DMA verloren, in Bursts statt einzeln, was TCP mehr schadet. Das Original macht es genauso: `let _ = wifi.send(...)`, Ergebnis verworfen.
- **Ethernet-DMA 30/30** statt der Absenkung auf 20.
- **Gestufter Rückfall in `bridge_wifi_start()`**: Vorher führte ein fehlgeschlagenes `esp_wifi_init()` auf `WIFI_INIT_CONFIG_DEFAULT()` — mit denselben Kconfig-Werten, also demselben Fehler, und dahinter ein `ESP_ERROR_CHECK`. Eine Sackgasse, die das Board am 2026-08-13 in eine Rollback-Schleife schickte.
- **`-O2` statt `-Os`**: Der Flash-Zwang (960-KB-Slot) existiert seit der 8-MB-Entdeckung nicht mehr.
- **Kein Clamp des BA-Fensters mehr.** Die Schutzbegrenzung auf `min(static_rx, dynamic_rx/2)` regelte den Wert stillschweigend von 32 auf 16 herunter und verfälschte damit *alle* Messreihen des Tages, inklusive der als "exakt wie das Original" bezeichneten.

### Nebenbefunde

- Der OTA-Rollback hat dreimal zuverlässig gegriffen: geplant bei der Minimal-Messvariante und beim Rust-Original, ungeplant nach dem Absturz der BA-Fenster-Version. Eine unbestätigte Firmware plus ein Stromausfall ist ein vollwertiger Rückweg ohne Kabel.
- **Serielle Schnittstelle nicht während eines OTA öffnen**: schon das Öffnen löst über DTR/RTS einen Reset aus. Am 2026-08-13 brach dadurch ein OTA ab (`HTTP 000`), und der anschließende Reconnect scheiterte am WPA2-Handshake (Reason 204) — die geklonte MAC meldete sich zu schnell erneut an, während der AP die alte Assoziation noch hielt.
- Der Rust-Build braucht ein Python **ausserhalb** eines venv; `~/.platformio/penv/bin` steht im PATH und lässt `idf_tools.py` abbrechen ("called from a virtual environment").

### Offen

Christian will denselben Vergleich im Labor wiederholen — dort hing die Bridge an einem echten Accesspoint (BSSID `aa:bb:cc:dd:ee:02`) statt an einem Mesh-Repeater. Erst dieser zweite Datenpunkt zeigt, ob der Standort die Erklärung ist.

### 2026-08-14, Labor — der Vergleich, der die Frage beendet

Derselbe Test am zweiten Standort: echter Accesspoint (BSSID `aa:bb:cc:dd:ee:02`) statt Mesh-Repeater, RSSI −27 dBm, Bridge 0,3 m vom Router.

| | Down / Up | Summe |
|---|---|---|
| MacBook direkt (1 m) | 84 / 92 | 176 |
| **Unsere Firmware** | 24,4 / **35,7** | 60,1 |
| **Rust-Original** | **29** / 31 | 60,0 |

**Beide Firmwares sind gleichauf** — in der Summe identisch, in den Einzelrichtungen innerhalb der bekannten Streuung. Und **auch das Original erreicht die 50 Mbit nicht**, unter besten denkbaren Bedingungen. Die kolportierte Zahl kommt der SUMME beider Richtungen (~60) nahe; vermutlich stammte sie daher.

**Der Standort war der ganze Unterschied.** Dieselbe Firmware, die zuhause 3,3 Mbit Upload schaffte, macht hier 35,7 — Faktor 10, ohne eine einzige Codeänderung.

Damit sind rückwirkend entwertet:
- Die Annahme, unsere Firmware sei langsamer als das Original. Sie ist es nicht, und war es nie.
- Die Annahme, der Management-Stack koste nennenswert Durchsatz. Wir liefern dieselbe Leistung wie das Original **plus** Weboberfläche, MQTT, OTA, zwei Standortprofile und Standorterkennung.
- Die Annahme, die Drop-Rate sei die Ursache. Im Labor liegt sie bei 4,13 % — und der Upload ist trotzdem die schnellere Richtung.

**Methodische Lehre:** Der Standort haette als Variable ausgeschlossen gehoert, BEVOR ueber zwanzig Konfigurationsaenderungen getestet wurden. Die Streuung der Messmethode war groesser als jeder Effekt, den die Aenderungen bewirken konnten. Die Aufloesung kam durch zwei Vorschlaege von Christian (Minimalvariante bauen; im Labor gegentesten), nicht durch die Parametersuche.

**Betriebsempfehlung:** Fuer den spaeteren Kameraeinsatz ist die Platzierung der entscheidende Faktor, nicht die Firmware. Sichtverbindung zum Router statt Weg ueber einen Repeater ist mehr wert als jede Einstellung. Wo das baulich nicht geht, ist ein per Kabel angebundener Accesspoint am Kameraort die wirksamere Massnahme.

**Erstmals im Echteinsatz bestaetigt:** Die Standorterkennung per Gateway-Ping hat beim Ortswechsel automatisch von Profil 1 (zuhause) auf Profil 2 (Labor, 192.168.8.245) umgeschaltet.

## 2026-08-15 — Kamera im Einsatz, und zwei teure Fehlannahmen korrigiert

### Der grosse Hebel: Auswahl des Accesspoints

Die Bridge hing in einem Mesh auf dem **schwaechsten** Knoten (−82 dBm), waehrend derselbe SSID-Name auf einem anderen mit −63 dBm verfuegbar war. Ursache war die IDF-Voreinstellung `WIFI_FAST_SCAN`: sie verbindet mit dem *erstbesten* passenden AP und bricht die Suche ab; ein ESP32 wechselt danach nicht mehr von selbst.

Behoben mit `WIFI_ALL_CHANNEL_SCAN` + `WIFI_CONNECT_AP_BY_SIGNAL`. Wirkung: **20 dB besseres Signal, dreifacher Durchsatz.**

**Das Rust-Original hatte das nie falsch** — `ClientConfiguration::default()` setzt in embedded-svc `ScanMethod::CompleteScan(ScanSortMethod::Signal)`. Beim A/B-Test im Labor fiel es nicht auf, weil dort nur ein sinnvoller AP in Reichweite war. Ich hatte `..Default::default()` beim Lesen des Rust-Codes als "nichts Besonderes" abgehakt, statt nachzusehen, was der Default einstellt.

### Der zweite Hebel: Retries fuer UDP statt TCP

`ETH_TX_RETRIES_DEF` stand auf 1, begruendet mit Speedtest-Messungen. Fuer TCP ist das richtig (das Protokoll sendet nach und drosselt selbst), fuer einen **UDP-Videostrom ist es grundfalsch** — jedes verworfene Paket ist endgueltig weg.

Gemessen an der laufenden Kamera:

| Retries | Verlust |
|---|---|
| 1 | 13,29 % |
| 4 | 0,426 % |
| 6 | 0,064 % |
| 8 | 0,059 % |
| 16 | **0,000 %** |

Standard jetzt 8, im Betrieb 16. Ich hatte fuer einen Benchmark optimiert statt fuer den Einsatzzweck.

### Kapazitaet: meine 3,2 Mbit waren um Faktor sechs zu niedrig

Gemessen wurden **21,2 Mbit bei 0,003 % Verlust**. Die 3,2 Mbit stammten aus der Zeit am schwachen Knoten und wurden danach nie neu erhoben — ich habe sie trotzdem als Budget weiterverwendet und darauf eine ganze Empfehlungskette aufgebaut (Aufloesung senken, Bildrate senken, H.265+ als notwendig, 2 Mbit als Ziel). **Alles davon war gegenstandslos.**

Endkonfiguration der Kamera: 4256×1888 @15fps VBR 6 Mbit H.265 GOP30 + 1200×536 @12fps 1 Mbit H.265 GOP12. Ergebnis: ~8,8 Mbit im Mittel, **0,005 % Verlust**, ueber eine Stunde stabil.

### Sendeleistung

Von 14,5 dBm (Ausgangswert; die frueher genannten 19,5 galten nur unter der alten Firmware) auf **9,75 dBm** gesenkt, ohne messbare Verschlechterung: 0,0032 % Verlust. Spart Waerme in der Endstufe.

Dabei fiel ein Fehler auf: `tx_power = 0` hiess "nicht anfassen" und stellte den Ausgangswert deshalb **nicht wieder her** — der zuletzt gesetzte Wert blieb stehen. Behoben: der Startwert wird gemerkt und aktiv zurueckgesetzt.

### Weitere Aenderungen

- **Client-IP und -Hostname werden im Datenpfad mitgelesen** (IPv4/ARP-Quelle, DHCP-Option 12) und in Portal wie MQTT angezeigt. Klaerte binnen Minuten, dass die Kamera `PB4` gar nicht sendet — der Name ist rein geraeteintern, kein Router kann ihn anzeigen.
- **Gelernte Client-MAC im NVS.** Eine Kamera sendet nur beim EIGENEN Start; nach einem Bridge-Neustart lief der Sniff in den Timeout und das Geraet landete unerreichbar im Portal. Jetzt dient die gespeicherte MAC als Rueckfall.
- **Absturzschleifen-Schutz**: drei Starts ohne 60 s stabilen Betrieb -> Portal. Die bisherige Sicherung griff nur bei gescheitertem WLAN-Aufbau, nicht bei einem Absturz im Betrieb.
- **Drittes Netzwerkprofil**, Standorterkennung darauf verallgemeinert.
- **Autotune** fuer die Retries, ueber das Portal startbar. Sucht den *kleinsten* Wert unter 0,10 % Verlust und bricht ehrlich ab, wenn zu wenig Verkehr fliesst.
- **`Cache-Control: no-store`** auf Seite und API. Ohne das lieferten Browser nach einem Firmware-Update die alte Seite aus — ein per API gesetzter Wert war im Portal unsichtbar.
- **Portal zeigt die wirksamen Werte** statt der gespeicherten 0, dazu ein "Standardwerte"-Knopf. Die 0-heisst-Default-Konvention ist im Code sinnvoll, in der Oberflaeche war sie unbedienbar: man konnte keinen Wert *unter* dem Standard eintragen, ohne dass die Null widerspruechlich wurde.
- Verbundene SSID, Heap-Tiefststand, DMA-Heap und groesster freier Block im Status.

### Kein PSRAM

Bestaetigt: keine SPIRAM-Option aktiv, und GPIO16 ist bei diesem Board die PHY-Stromversorgung — genau der Pin, den PSRAM belegen wuerde. Waere ohnehin nutzlos, WLAN-DMA-Puffer muessen in internem RAM liegen.

## 2026-08-16 — Ins Git ueberfuehrt, WiFi-Watchdog nach Jammer-Test

Das Projekt existierte bis hierhin nur als lokaler Verzeichnisstand; die obigen Eintraege (08-11 bis 08-15) sind rekonstruiert, nicht aus Commits. `b846bea`/`9870e7a` sind der erste Git-Stand ueberhaupt und buendeln alles bisher Beschriebene.

### Kamera im Dauerbetrieb, dann ein 1-Minuten-Jammertest

Zum ersten Mal beobachtet: ein 2,4-GHz-Stoersender, absichtlich nur 1 Minute aktiv, legte den Stream fuer **ueber 5 Stunden** lahm. `/api/status` zeigte die ganze Zeit `wifi_up=true` und einen unauffaelligen RSSI — die Bruecke hielt sich fuer verbunden, stellte aber praktisch keine Pakete mehr zu. Ein manueller `/api/reboot` behob es sofort. Ein zweites, komplett unabhaengiges Kamerasystem an einem anderen Router im selben Gebaeude zeigte im selben Zeitraum dasselbe Symptom — das schliesst die Firmware als alleinige Ursache aus, ohne sie freizusprechen: der Treiber haengt offenbar in einem degradierten Zustand fest, aus dem er sich ohne Neustart nicht selbst loest, und das ist unabhaengig von der konkreten Hardware reproduzierbar.

### Watchdog: explizit portal-schaltbar, nicht als Kompilierschalter

Anforderung von Christian: die Reaktion muss sich **im Portal aktivieren lassen**, nicht als Config-Datei- oder Compiler-Flag — ein Geraet ohne seriellen Zugang darf nicht von einer Einstellung abhaengen, die nur beim naechsten Flash aenderbar ist.

Umgesetzt als neues `wd_enable`-Feld in `BridgeConfig` (angehaengt, per Konvention default 0/aus), ein Portal-Dropdown dafuer, und `watchdog_tick()` in `bridge.cpp` (`b2a0f47`): ein 10-Sekunden-Fenster beobachtet Paketverlust (`LAN->WLAN`-Drop-Quote) und WLAN-Reconnects; drei schlechte Fenster in Folge -> `esp_restart()`. Live umschaltbar, kein Neustart noetig (`h_config_post` ruft `g_cfg.wd_enable` direkt ab).

**Bekannte Luecke beim ersten Entwurf:** das Fenster braucht Mindestverkehr (`WD_MIN_PAKETE`), um eine Verlustquote ueberhaupt berechnen zu koennen — bei nahezu keinem Verkehr (der Jammer-Fall, wenn die Kamera selbst kaum sendet) bleibt der Watchdog blind. Bewusst nicht "geloest" durch einen naiven Timeout auf Nullverkehr, weil das bei einer wirklich ruhigen Szene faelschlich ausloesen wuerde.

### Aktive Gateway-Sonde schliesst die Luecke (`adf396a`)

Wiederverwendet: `gateway_erreichbar()`, bereits vorhanden fuer den einmaligen Standort-Ping beim Boot (`standort_bestimmen()`). Springt ein, wenn im Fenster zu wenig Verkehr floss, um die Verlustquote zu bewerten: pingt das Gateway der Management-IP (`esp_netif_get_ip_info()` -> `gw`), Budget 1200 ms. Erfolglos -> zaehlt als schlechtes Fenster. Ergebnis als `wd_probe` in `BridgeStats` und `/api/status` (`0`=nicht noetig/aus, `1`=ok, `2`=fehlgeschlagen), im Portal als Text neben den Reconnects sichtbar.

Damit deckt der Watchdog beide Faelle ab: hohe Last mit hohem Verlust (passiv erkannt) und nahezu keine Last bei haengendem Treiber (aktiv erfragt).

PR `#1` (dieser Zweig) erst am **2026-08-20** gemerged — vier Tage Verzoegerung zwischen Implementierung und Merge, ohne dass hier festgehalten ist warum.

## 2026-08-31 — Kompletter Ausfall trotz Watchdog, Ursache gefunden, Coredump-Forensik gebaut

### Symptom: Watchdog "greift nicht"

Christian meldete Faelle, in denen die Bruecke komplett unerreichbar wurde — sowohl die Management-IP (`.11`) als auch die gebrueckte Kamera (`.88`) gleichzeitig weg — und **nicht von selbst zurueckkam**, obwohl der Watchdog aus dem 08-16-Eintrag aktiviert war. Einziges verfuegbares Beweismittel: ein Home-Assistant-Graph des freien Heaps und des Durchsatzes, mit mehreren "Nicht verfuegbar"-Luecken ueber zwei Tage.

### Ursache: ein laengst vorhandener, aber unbeachteter Absturzschleifen-Schutz

In `main.cpp` (aus der 08-15-Zeit, nie in `history.md` als eigener Mechanismus vermerkt): nach **drei Boots ohne 60 s stabilen Betrieb** (`BOOT_MAX_FEHLSTARTS=3`, Zaehler im RTC-Speicher) gibt die Firmware bewusst auf und geht in den Provisionierungs-/AP-Modus (`192.168.4.1`) statt weiter Bruecke zu spielen.

Das erklaert das Symptom vollstaendig:
- Der AP-Modus liegt auf einem komplett anderen Subnetz — deshalb verschwinden `.11` und `.88` gleichzeitig, und Home Assistant (das `/api/status` auf `.11` abfragt) sieht ab da nur noch Luecken.
- Es gibt **keinen automatischen Weg zurueck** aus dem Portal-Modus — nur manuelles Eingreifen oder ein echter Kaltstart (Stromunterbrechung, der den RTC-Zaehler loescht) helfen.
- Der WiFi-Watchdog aus dem 08-16-Eintrag kann hier grundsaetzlich nicht greifen: `watchdog_tick()` ist bewusst an `s_active` (aktiver Bruecken-Modus) gebunden, und genau der wird in diesem Fehlerfall nie erreicht bzw. nie 60 s gehalten.

Die eigentliche Ursache — *warum* die Bruecke ueberhaupt dreimal hintereinander innerhalb von 60 s abstuerzt — blieb an diesem Tag ungeklaert. Alle Hardware-Sicherheitsnetze (Task-Watchdog 5 s, Interrupt-Watchdog 300 ms, Brownout-Detector, Panic-Handler mit Reboot) sind korrekt scharf; das Problem ist nicht, dass die Bruecke nicht neu startet, sondern dass niemand sieht, *woran* sie abstuerzt.

### `/api/coredump`: Absturzforensik ohne Kabel

Die Coredump-Partition existierte bereits seit dem 08-13-Eintrag (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`), wurde aber nie ausgelesen. Neuer Endpoint (GET/DELETE `/api/coredump`, admin-key-geschuetzt wie das Firmware-Update) liest `esp_core_dump_get_summary()` und `esp_core_dump_get_panic_reason()` aus: abstuerzender Task, PC, Backtrace, Panic-Grund als JSON. Portal zeigt automatisch eine Warnbox, wenn ein Abbild vorliegt.

Direkt beim ersten Test auf echter Hardware lag tatsaechlich ein altes Abbild im Flash (Task `main`, `assert failed`) — aber die Backtrace-Adressen loesten sich gegen das aktuelle `firmware.elf` zu voellig unzusammenhaengenden Funktionen auf (`rtc_clk_bbpll_configure`, `ieee80211_is_tx_allowed`, `sha_hal_read_digest`, ...). Klassisches Zeichen fuer ein Adress/Build-Mismatch: das Abbild stammte von einer aelteren Firmware-Version, deren `.elf` laengst durch neuere Builds ueberschrieben war. Abbild geloescht, um kuenftige echte Abstuerze nicht mit diesem Altfund zu vermischen.

**Zwei ESP-IDF-Ueberraschungen beim Bau der "leer"-Erkennung**, beide auf echter Hardware verifiziert und nicht aus der Doku ersichtlich:
- `esp_core_dump_image_check()` liefert nach einem Loeschen NICHT `ESP_ERR_NOT_FOUND`.
- `esp_core_dump_image_get()` liefert nach einem Loeschen `ESP_ERR_INVALID_SIZE`, nicht das im Header dokumentierte `ESP_ERR_NOT_FOUND` — der geloeschte Header liest als `0xFFFFFFFF` ("blank"), und `esp_core_dump_partition_and_size_get()` (siehe `core_dump_flash.c`) behandelt das als ungueltige Groesse, nicht als "keine Partition". Erst als beide Fehlercodes als "kein Abbild da" behandelt wurden, meldete der Endpoint nach dem Loeschen korrekt `present:false`.

### Build-Archivierung, damit ein kuenftiger Coredump ueberhaupt aufloesbar bleibt

Der Blindflug beim alten Abbild war strukturell: PlatformIO ueberschreibt `firmware.elf` bei jedem Build, und der Worktree des Watchdog-Builds war laengst geloescht. Behoben mit zwei kleinen Ergaenzungen statt einem Kompilierschalter-Trick in `platformio.ini` (dessen Shell-Escaping als zu fehleranfaellig verworfen wurde):

- `scripts/gen_build_info.py`, als PlatformIO-`extra_scripts`-Pre-Build-Hook: schreibt `src/build_info.h` mit dem aktuellen Git-Kurz-Hash (plus `-dirty`-Suffix bei unversionierten Aenderungen). Der Wert landet als `"build"` in `/api/status`.
- `scripts/ota_flash.sh`: baut, kopiert `firmware.elf` nach `elf_archive/<git-rev>_<utc-timestamp>.elf`, laedt dann per OTA hoch.

Die Zuordnung eines kuenftigen Coredumps zum richtigen archivierten `.elf` stuetzt sich darauf, dass ein Laufzeitabsturz dieselbe OTA-Partition erneut bootet (kein Image-Wechsel) — der `build`-Wert aus `/api/status` zur Abfragezeit stimmt also nur mit dem abgestuerzten Build ueberein, solange dazwischen kein neues OTA-Update kam. Fuer den ueblichen Fall (Absturz, Watchdog/Hardware-Reboot, jemand schaut kurz danach nach) reicht das; nicht abgedeckt ist der seltenere Fall "Coredump liegt schon laenger, inzwischen wurde erneut geflasht" — dafuer muesste zusaetzlich `app_elf_sha256` aus der Coredump-Summary ausgewertet werden (aktuell nicht in der API exponiert, siehe `backlog.md`).

Auf Hardware verifiziert: `build:"3765974-dirty"` in `/api/status` passte exakt zum Dateinamen `elf_archive/3765974-dirty_20260831T205316Z.elf`. Drei OTA-Zyklen insgesamt (Endpoint, Erase-Fix, Build-Archivierung), Bridge und Kamera nach jedem Zyklus gesund verifiziert. PR `#2` selbstaendig gepusht (kein `gh` auf dieser Maschine, kein Merge auf `main` durch die Session selbst — Merge von Christian ueber die GitHub-UI).

**Offen geblieben:** die eigentliche Ursache des Dreifach-Absturzes. Naechstes Vorkommnis sollte ueber `/api/coredump` ausgewertet werden, BEVOR irgendjemand erneut flasht oder das Abbild loescht — sonst wiederholt sich genau der Blindflug von diesem Tag.

## 2026-09-04 — Watchdog trat sich selbst ins Portal, dreistufige Eskalation gebaut

### Symptom: wieder ein Totalausfall, diesmal ohne Absturz

Christian fand die Bruecke erneut komplett unerreichbar — Portal zeigte "Erstinbetriebnahme", **Laufzeit 1 Tag 8 Stunden im Provisionierungsmodus**, niemandem war es aufgefallen. `/api/coredump` meldete `present:false` — **kein Absturz**, die vorigen Neustarts waren also sauber (`esp_restart()`), nicht durch einen Crash erzwungen.

### Ursache: der Watchdog hat sich selbst ins Portal manoevriert

Rekonstruiert im Gespraech: anhaltend schlechte WLAN-Bedingungen liessen den (seit dem 08-16-Eintrag aktivierten) Watchdog wiederholt `esp_restart()` ausloesen. Jeder dieser Neustarts zaehlte als Boot-Versuch fuer den Absturzschleifen-Schutz (`main.cpp`, `BOOT_MAX_FEHLSTARTS=3`, siehe 08-15) — der aber komplett unabhaengig vom Watchdog entstanden war und nie mit ihm abgestimmt wurde. Scheiterte der WLAN-Aufbau nach einem Watchdog-Neustart erneut, bevor die 60-Sekunden-Stabilitaetsschwelle erreicht war (rechnerisch moeglich: der Watchdog konnte schon nach ~30-40 s zuschlagen), zaehlte das als Fehlstart. Drei solcher Zyklen, und der komplett unabhaengige Absturzschleifen-Schutz warf die Bruecke ins isolierte Portal-WLAN — wo der Watchdog per Definition nicht mehr laufen kann (`s_active` ist dort nie wahr). Der Watchdog hat also getan, wofuer er da ist, wurde dafuer aber vom Nachbarmechanismus bestraft.

### Warum ein Reboot keine schlechte Funkumgebung repariert

Christian wies zurecht darauf hin: wenn ein reiner Reconnect nicht hilft, warum sollte ein voller Neustart helfen — der aendert an der Umgebung ja nichts. Richtig, und das war die entscheidende Praezisierung: der urspruengliche Jammer-Vorfall (08-16) wurde nicht durch einen Reboot "repariert", weil der Reboot die Umgebung verbesserte — die Umgebung war zum Zeitpunkt des (Stunden spaeteren, manuellen) Reboots laengst wieder gut, nur der WLAN-Treiber im ESP32 hing noch in einem degradierten Zustand fest. Ein Reboot hilft also nur gegen einen haengenden Treiber, nicht gegen anhaltend schlechten Empfang — und genau das laesst sich mit einem reinen `esp_wifi_disconnect()` pruefen, ohne das Ethernet ueberhaupt anzufassen.

### Fix: zweistufige Eskalation, Watchdog-Neustarts zaehlen nicht mehr als Fehlstart

- `watchdog_tick()` eskaliert jetzt in zwei Schritten statt direkt `esp_restart()` zu rufen: **Stufe 1** trennt nur das WLAN (`esp_wifi_disconnect()`) — der bereits vorhandene `WIFI_EVENT_STA_DISCONNECTED`-Handler verbindet automatisch neu, solange `s_active` gilt, ein zusaetzliches `esp_wifi_connect()` waere doppelt. Ethernet/Kamera bleiben unberuehrt. **Stufe 2** (nur wenn Stufe 1 nach einem weiteren Beobachtungsfenster nicht half): voller Neustart — aber ueber `abort()` statt `esp_restart()`, damit ein Coredump entsteht und der naechste Boot nachvollziehbar ist (Wunsch von Christian: "wenn das gemacht werden muss, waere es sinnvoll, einen Coredump zu provozieren").
- Beide Stufen zaehlen jetzt **nicht** mehr als Fehlstart: `bridge_mark_planned_restart()`/`bridge_consume_planned_restart()`, ein RTC-persistiertes Einmal-Flag nach demselben Muster wie `main.cpp`s bestehendes `PORTAL_MAGIC`. Ein Watchdog-Neustart bedeutet ja "sauber gebootet und gelaufen, hat sich nur selbst neu gestartet" — fundamental etwas anderes als "kam nie hoch".
- Neu in `BridgeStats`/`/api/status`/MQTT: `wd_reconnects` (Zaehler seit Boot) und `wd_last_reason` (1=Verlustquote, 2=Reconnects, 3=Gateway-Sonde; RTC-persistiert, ueberlebt also den Neustart und bleibt im Portal sichtbar, bis der naechste Watchdog-Vorfall ihn ueberschreibt).
- Messfenster von 10 s auf 20 s entspannt (Christian: "10s ist zu kurz gegriffen").

Auf Hardware verifiziert: OTA erfolgreich, `wd_reconnects`/`wd_last_reason` korrekt vorhanden und auf 0, Kamera weiter erreichbar.

### Nachtrag: Kamera kam nach einem Bridge-Neustart nicht zurueck — Ursachensuche

Waehrend der Diagnose des 1d8h-Vorfalls musste die Bruecke manuell neu gestartet werden; danach blieb die Kamera fuer ~3 Minuten unerreichbar (kam am Ende von selbst zurueck, ohne eigenen Stromreset). Frage von Christian: passiert das auch bei einem reinen WLAN-Ausfall ohne Bridge-Neustart, und liegt es an Kamera, Router oder Bridge-Mechanismus?

Architektonische Antwort: **nein, ein reiner WLAN-Ausfall sollte die Kamera nicht betreffen** — der Ethernet-Treiber (`bridge_eth_init()`/`eth_rx_cb`) ist komplett unabhaengig vom WLAN-Zustand, ein `WIFI_EVENT_STA_DISCONNECTED` fasst ihn nie an. Ein **Bridge-Neustart** dagegen reisst den Ethernet-MAC/PHY tatsaechlich kurz runter — ein echter Link-Flap auf der Kamera-Seite, den ihre (vermutlich HiSilicon-basierte) Firmware offenbar nur traege wegsteckt.

Folgefrage von Christian: laesst sich das nicht eleganter loesen, z.B. per DHCP-Neuverhandlung oder ARP? Antwort: **nein** — DHCP kennt keinen Mechanismus, mit dem ein Dritter (nicht der DHCP-Server) einen Client zur Neuverhandlung zwingen kann; `DHCPFORCERENEW` (RFC 3203) muesste vom Server kommen und wird von Consumer-Routern praktisch nie unterstuetzt. Gratuitous ARP aktualisiert nur fremde ARP-Caches, bringt aber einen haengenden Kamera-Netzwerkstack nicht zum Laufen. Was tatsaechlich geht: **nur den Ethernet-Treiber zuruecksetzen, ohne den Chip neu zu booten** (`esp_eth_stop()`/`esp_eth_start()`) — erzeugt fuer die Kamera einen echten, aber kurzen Link-Down/Up, ohne WLAN ueberhaupt anzufassen. Vermutlich genau das (als Nebeneffekt eines vollen Reboots), was der Kamera nach dem 08-16-Jammervorfall schon einmal geholfen hat.

### Dritte Eskalationsstufe: Ethernet-Reset bei stiller Kamera

- Neuer, eigenstaendiger Erkennungspfad in `watchdog_tick()`: Ethernet-Link vorhanden, aber praktisch kein Verkehr von der Kamera (`WD_ETH_STALL_PAKETE`, < 5 Pakete im Fenster) — geprueft NUR, nachdem die Gateway-Sonde WLAN bereits als funktionierend bestaetigt hat, damit ein WLAN-Problem nicht faelschlich als Kamera-Problem gezaehlt wird. Bei dieser Installation (Dauerstream, siehe 08-15) ist echte Stille abnormal, kein Fehlalarm-Risiko durch legitime Pausen.
- Eskalation jetzt symptomabhaengig: `WD_REASON_ETH_STALL` -> zuerst `bridge_reset_eth()` (Stop/Start, kein Netif involviert, also keines der Risiken, die ein doppeltes WLAN-Netif-Anlegen hat). Jeder andere Grund -> direkt WLAN-Reconnect wie zuvor, ein Ethernet-Reset waere dort wirkungslos und wuerde die Kamera grundlos stoeren. Hilft die jeweils erste Massnahme nicht, folgt die andere leichte Massnahme, erst danach der volle Neustart.
- `bridge_reset_eth()` zusaetzlich manuell ueber einen neuen Button "Ethernet neu starten" im Portal (`POST /api/eth_reset`) ausloesbar, unabhaengig vom Watchdog-Zustand — fuer den Fall, dass jemand nicht auf den naechsten automatischen Zyklus warten will (Wunsch von Christian: "1. beides" — automatisch UND manuell).
- Neu: `telemetry_note_watchdog_event()` veroeffentlicht jede Eskalationsstufe SOFORT als MQTT-Nachricht auf `bridge/<node>/watchdog` (Aktion, Grund, Laufzeit) statt erst beim naechsten regulaeren `telemetry_tick()` als stiller Zaehlerstand sichtbar zu werden — Home Assistant timestempelt den Nachrichtenempfang selbst, ein eigener Zeitstempel im Geraet (das keine Echtzeituhr hat) war dafuer nicht noetig. `wd_eth_resets` zusaetzlich als laufender Zaehler in Status/MQTT/HA-Sensor.

Auf Hardware verifiziert: OTA erfolgreich, `wd_eth_resets` vorhanden und 0 nach Boot. Manueller Ethernet-Reset live ausgeloest: Link erholte sich, Kamera-Traffic lief sofort weiter, kein Bridge-Neustart. Heap sackte kurzzeitig um ~40 kB ab (88,6 -> 41,6 kB) und erholte sich innerhalb von 10 s vollstaendig auf 95 kB — einmalig beobachtet, kein Leck.

**Offen geblieben:** ob ein reiner WLAN-Reconnect (Stufe 1/2) den urspruenglichen Jammer-Zustand (haengender Treiber trotz wiederhergestellter Umgebung) tatsaechlich loest, wurde nie isoliert getestet — beim naechsten echten Vorfall dieser Art zeigt sich das live. Ebenso weiterhin offen: die tieferliegende Ursache dafuer, dass die WLAN-Bedingungen ueberhaupt so lange anhaltend schlecht waren, dass der Watchdog in eine Neustart-Spirale geriet.

## 2026-09-05 — Zweite Luecke im Absturzschleifen-Schutz gefunden und geschlossen

### Wieder unerreichbar, diesmal per Coredump aufgeklaert

Trotz des 09-04-Fixes fand Christian die Bruecke erneut im Portal-Modus (`prov:1`, `192.168.4.1`), diesmal mit einem tatsaechlich vorhandenen Coredump. Entschluesselt (Quellcode zum exakt laufenden Commit neu gebaut, `addr2line` gegen das frische `firmware.elf` - kein archiviertes `.elf` noetig, da main-Checkout und Build identisch waren): der Stacktrace fuehrte glasklar durch `panic_abort -> esp_system_abort -> abort -> bridge_tick() (bridge.cpp:1346, watchdog_tick() inline) -> app_main -> main_task`. Kein mysterioeser Absturz - exakt der eigene, absichtliche `abort()` aus der letzten Watchdog-Eskalationsstufe, wie vorgesehen ausgeloest. `wd_last_reason:3` (Gateway-Sonde fehlgeschlagen) bestaetigte: WLAN war zum Eskalationszeitpunkt wirklich unerreichbar, kein reines Verlustproblem.

### Die Luecke: ein einziger fehlgeschlagener Reconnect-Versuch reicht

Der 09-04-Fix hatte Watchdog-Neustarts korrekt vom 3-Fehlstart-Zaehler ausgenommen. Trotzdem landete die Bruecke wieder im Portal - ueber einen KOMPLETT ANDEREN, bis dahin unbeachteten Pfad: `bridge_wifi_start()` gab jeder konfigurierten SSID nur EINEN Verbindungsversuch (12 s Timeout, keine Wiederholung). Scheiterte der, setzte `main.cpp` sofort `s_portal_request = PORTAL_MAGIC` und startete direkt ins Portal um - unabhaengig vom Fehlstart-Zaehler, der ueberhaupt nicht befragt wird. Mit nur einem konfigurierten Netzwerkprofil (`BMI_2G`) reichte also ein einziger schlechter Moment fuer den Rueckfall ins unerreichbare Portal.

Christian fragte zurecht nach, warum ein Reboot ueberhaupt einen neuen WLAN-Verbindungsversuch retten sollte, wenn ein reiner Reconnect es nicht tut - berechtigter Einwand, siehe dazu den 09-04-Eintrag oben (ein Reboot repariert keine schlechte Funkumgebung). Die eigentliche Erklaerung liegt woanders: zwei ganz gewoehnliche, voruebergehende Ursachen fuer einen einzelnen fehlgeschlagenen Versuch direkt nach einem (schnellen `abort()`-)Neustart:

1. **Alte Assoziation der GEKLONTEN MAC beim Router** - bereits am 2026-08-14 in diesem Projekt als 802.11-Reason-204 dokumentiert: der AP haelt die alte Sitzung noch, waehrend sich dieselbe MAC schon wieder anmeldet.
2. **Die Funkstoerung, die den Watchdog erst zum Eskalieren brachte, war eine Sekunde spaeter noch nicht zwingend weg.**

### Fix: Wiederholung statt laengerem Timeout, jetzt Portal-einstellbar

`bridge_wifi_start()` probiert dieselbe SSID jetzt mehrfach (Standard 3×, 2 s Pause dazwischen) mit je 20 s Timeout (Standard vorher 12 s/1 Versuch, nie gemessen, nur geraten), bevor das naechste Profil bzw. das Portal drankommt. Auf Nachfrage von Christian, warum ausgerechnet 12s/1 Versuch "empirisch" sein sollten (waren sie nie) - beide Werte sind jetzt zusaetzlich per Portal ueberschreibbar (`wifi_connect_timeout_s`, `wifi_connect_retries`, 0 = Firmware-Standard, gleiche Konvention wie die uebrige Feinabstimmung), damit sich ein falscher Standardwert ohne Neuflashen korrigieren laesst.

### Portal-Idle-Neustart: die Grundsatzluecke aus dem Backlog geschlossen

Zusaetzlich, auf Vorschlag von Christian: `ap_idle_reboot_s` (Standard 900 s = 15 min, ebenfalls Portal-einstellbar). Solange die Bruecke im Portal-Modus ohne ECHTE Portal-Nutzung haengt (Seite geladen, Konfiguration gespeichert oder gescannt - der automatische 2-Sekunden-Status-Poll zaehlt bewusst NICHT, sonst wuerde ein offen gelassener Browser-Tab den Neustart fuer immer verhindern, siehe `web_last_activity_ms()` in `web.cpp`), versucht sie nach dieser Zeit von selbst einen normalen Neustart mit den vorhandenen Zugangsdaten. Gate auf `g_cfg.configured`, damit sich ein frisches oder werksrueckgesetztes Geraet nicht sinnlos im Kreis neu startet, wo es nichts zu wiederholen gibt. Das schliesst die seit 2026-08-31 im Backlog stehende Luecke ("boot-loop guard has no automatic way back").

### Portal-Texte bereinigt

Auf Hinweis von Christian: die neue WLAN-Verbindungsversuch-Beschreibung im Portal erzaehlte die Debug-Geschichte nach ("War frueher ein einzelner 12-Sekunden-Versuch... reichte nicht immer"), statt schlicht das aktuelle Verhalten zu beschreiben. Portal-Text ist fuer Endnutzer, nicht fuer die Entwicklungshistorie - die gehoert in Code-Kommentare und hierher, nicht in die Oberflaeche. Korrigiert, ohne die Historie in den Code-Kommentaren oder hier anzufassen.

Auf Hardware verifiziert ueber vier OTA-Zyklen (Retry-Fix, Portal-Parameter + AP-Idle-Reboot, Text-Cleanup): Bridge und Kamera nach jedem Zyklus gesund, neue Felder (`wifi_connect_timeout_s`, `wifi_connect_retries`, `ap_idle_reboot_s`) korrekt in `/api/config` sichtbar (effektiv und im `def`-Block).
