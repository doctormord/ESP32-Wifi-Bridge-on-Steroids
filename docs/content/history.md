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
