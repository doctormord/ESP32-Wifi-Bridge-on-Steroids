# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An ESP32 firmware that turns a WT32-ETH01 board (ESP32-D0WD + LAN8720A PHY) into a transparent Wi-Fi↔Ethernet L2 bridge with MAC cloning: a device plugged into the Ethernet port (e.g. an IP camera) gets bridged onto a Wi-Fi network as if it were directly connected, while a separate static management IP on the same interface serves a config portal, a status API, and MQTT telemetry.

Pure ESP-IDF via PlatformIO (`framework = espidf`) — no Arduino. The project used to be a dual `arduino, espidf` build; Arduino was removed deliberately because it transitively pulled in the ESP RainMaker/Insights components through the IDF Component Manager (unused, cost flash, caused recurring `esp_crt_bundle.h` / certificate build failures). `compat.h` provides just `millis()`/`delay()` over `esp_timer`/`vTaskDelay` so the original Arduino-style code didn't need a full rewrite.

## Commands

**Use `~/.pio-py313/bin/pio`, not plain `pio`.** See "Toolchain" below — plain `pio` aborts with a Python version error on this machine.

```bash
~/.pio-py313/bin/pio run -e wt32-eth01                 # build
~/.pio-py313/bin/pio run -e wt32-eth01 -t upload       # flash
~/.pio-py313/bin/pio run -e wt32-eth01 -t monitor      # serial monitor (115200, esp32_exception_decoder)
~/.pio-py313/bin/pio run -t clean                      # clean build artifacts
```

Always pass the port explicitly (`--upload-port /dev/cu.usbserial-XXX`): a second USB-serial adapter for an unrelated board is often plugged in, and auto-detect could flash the wrong device.

### Toolchain: PlatformIO must run on Python ≤ 3.13

The pioarduino `platform-espressif32` rejects Python ≥ 3.14 outright in its `platform.py`. Homebrew's `pio` runs on `python@3.14` **and recreates `~/.platformio/penv` with `uv` whenever it runs**, so fixing that venv in place does not stick. The working setup is a standalone venv at `~/.pio-py313` (Python 3.13) containing `platformio==6.1.19` **and `pyyaml`** — PyYAML is not a PlatformIO dependency, but the platform's `component_manager.py` imports it. It shares the `~/.platformio` core dir, so toolchains aren't duplicated.

Moving `~/.platformio/platforms/espressif32` out of the way does *not* work as a dodge: PlatformIO re-downloads it from the URL in `platformio.ini`, and the current release carries the same check.

There is only one environment (`wt32-eth01`); no separate test suite (this is embedded firmware — verification is build success + on-device serial log behavior).

### Flash budget

The board has **8 MB** of flash — measured at the chip on 2026-08-13 (`esptool flash-id` → Zbit `0x5E`/`0x4017`, capacity byte `0x17` = 8 MiB, cross-checked with an aliasing read at `0x401000`). Earlier project notes claiming 2 MB were wrong, and `partitions_2mb_ota.csv` is the obsolete leftover from that belief; the live layout is `partitions_8mb_ota.csv` (2 × 3 MB OTA slots + a 64 KB coredump partition). Current usage is ~26% of a slot, so flash pressure is no longer a real constraint.

Note the asymmetry if you ever change the size again: `esp_flash_init_default_chip()` **fails hard** when the detected chip is *smaller* than the image header claims, but only *warns* (and wastes capacity) when it's larger. Too-big headers brick the boot; too-small ones just cost space. After a build, check the `Flash:` percentage in the `pio run` output. If a change pushes it over 100%, the fix is almost never "trim comments" — check `CONFIG_COMPILER_OPTIMIZATION_SIZE` is still `y` (not `PERF`) and that nothing new is `REQUIRES`d in `src/CMakeLists.txt` that isn't actually needed.

Don't guess at what's consuming flash — measure it. `xtensa-esp-elf-gcc-nm --print-size --size-sort --radix=d .pio/build/wt32-eth01/firmware.elf | tail -30` lists the biggest symbols and has repeatedly pointed at whole subsystems that were linked but unused. `docs/content/backlog.md` records which further savings were evaluated and rejected, and why — read it before re-attempting one.

### Flash size is configured in two independent places

`board_build.flash_size` sets `CONFIG_ESPTOOLPY_FLASHSIZE` (what ESP-IDF believes); `board_upload.flash_size` sets the size nibble in the image header that esptool writes (what the hardware acts on). Both must say `2MB`. If only the first is set, the header inherits 4 MB from the `esp32dev` board manifest, and `esp_flash_init_default_chip()` fails with `ESP_ERR_FLASH_SIZE_NOT_MATCH` ("Probe failed") because detected size < header size — that's a hard error, so the board never boots. Verify with the first 4 bytes of `firmware.bin`/`bootloader.bin` rather than trusting sdkconfig.

The same split applies to flash *frequency*: `CONFIG_ESPTOOLPY_FLASHFREQ` may still read `40m` in the generated sdkconfig while the actual header says 80 MHz, because PlatformIO patches it in at esptool time. The header wins — check there.

### sdkconfig drift

`sdkconfig.wt32-eth01` is a generated, fully-resolved cache of `sdkconfig.defaults` + component Kconfig defaults. PlatformIO/ESP-IDF only fills in options *missing* from it — an explicit value already present (e.g. from a previous build with different settings) silently wins over a later change to `sdkconfig.defaults`. If build behavior doesn't match what `sdkconfig.defaults` says it should (wrong flash size, unexpectedly large binary, a Kconfig-gated header suddenly missing), suspect drift first: delete `sdkconfig.wt32-eth01` and let it regenerate from `sdkconfig.defaults` on the next `pio run`, rather than hand-editing the cached file.

## Architecture

### Entry point and lifecycle

`src/main.cpp`'s `app_main()` calls `bridge_setup()` once then loops `bridge_loop()` forever — the direct ESP-IDF replacement for Arduino's `setup()`/`loop()`. Two mutually exclusive modes, decided at boot:

- **Provisioning mode**: no Wi-Fi credentials stored, or the factory-reset GPIO (IO14, pulled low for 2s) was held at boot, or no Ethernet client was seen within `MAC_SNIFF_TIMEOUT_MS`. Starts an AP (`web.cpp: portal_start_ap()`) plus the HTTP portal, and does nothing else — Ethernet bridging is not attempted.
- **Bridge mode**: Ethernet client MAC is sniffed, Wi-Fi STA connects using that *cloned* MAC, then the data path is activated and the HTTP server + MQTT telemetry start on the management IP.

OTA rollback confirmation (`ota_confirm_if_pending()`) is deliberately gated on "reachable over the network for 30s", not "booted" — the board has no serial access once deployed, so a boot that succeeds but leaves networking broken must still roll back.

### The bridge data path (`bridge.cpp`)

This is the part that requires the most care when touched. `esp_wifi_internal_reg_rxcb()` has exactly one callback slot per interface, and lwIP normally owns it. `bridge_wifi_start()` re-registers it with `wifi_rx_cb()`, taking over frame delivery entirely:

- **Ethernet → Wi-Fi** (`eth_rx_cb`): every frame goes straight to `esp_wifi_internal_tx()` on `WIFI_IF_STA`. No IP stack involved.
- **Wi-Fi → Ethernet or management stack** (`wifi_rx_cb`): `is_for_mgmt()` inspects the raw frame (IPv4 dest or ARP target == the configured management IP) and routes matching frames to `esp_netif_receive()` on the management netif; everything else goes straight to `esp_eth_transmit()`. Broadcast/multicast IPv4 is intentionally *not* treated as management traffic — it only goes to the bridged client.

Both directions free their buffers in every code path exactly once — that ownership contract is the main thing to preserve when editing this file. `bridge_set_paused()` stops forwarding (used during OTA flash writes, when the cache is briefly unavailable) without tearing down the drivers, so the management connection driving the OTA upload itself stays alive.

The Wi-Fi STA and the management netif share one physical interface and MAC (the cloned client MAC) but have different IPs; ARP resolves both to the same MAC, which is what makes this transparent to switches/APs. The management IP **must** stay static — DHCP would hand the same MAC two different leases.

### Config (`config.cpp` / `config.h`)

`BridgeConfig` is stored as a single NVS blob (magic-prefixed). `cfg_load()` accepts blobs shorter than `sizeof(BridgeConfig)` (from older firmware) and leaves new fields at their defaults — this only works because new fields are **appended only**, never inserted in the middle (see the comment in `config.h`). Preserve that ordering discipline when adding config fields.

The performance-tuning fields at the end of the struct follow a deliberate convention: **`0` means "don't override — use the sdkconfig / PHY default"**, so `sdkconfig.defaults` stays the single source of truth for baseline values and the portal only overrides what someone explicitly set. Don't put real numbers in `cfg_defaults()` for those fields; that would recreate the duplicate-source-of-truth bug that made sdkconfig buffer changes silently ineffective.

### Runtime tuning split

Tuning settings divide into two groups, and the portal labels them as such:

- **Live** (`bridge_apply_live_tuning()`, no reboot): TX power and the two TX retry counters. TX power must be applied *after* `esp_wifi_start()` — earlier calls return `ESP_ERR_WIFI_NOT_STARTED`, which is why the call sits inside the connect loop rather than with the other Wi-Fi setup.
- **Reboot required**: Wi-Fi buffer counts and RX BA window (they go into `wifi_init_config_t` at `esp_wifi_init()`), plus HT40 and the 802.11b toggle (must be set before connecting).

TX power is quantized by the PHY to 11 discrete steps (2/5/7/8.5/11/13/14/15/16.5/18/20 dBm) — the portal offers exactly those as a dropdown, and `/api/status` reports the driver's *actual* value (`txp`, in 0.25 dBm units) rather than the requested one.

### Web portal (`web.cpp`)

Single self-contained HTML/CSS/JS page (`PAGE[]`) served at `/`, plus a small JSON API (`/api/status`, `/api/config` GET/POST, `/api/scan`, `/api/reboot`, `/api/update`). Config POST uses hand-rolled form-urlencoded parsing (`form_get`) — empty password fields mean "leave unchanged", not "clear". `/api/update` streams a raw `.bin` directly into the inactive OTA partition (`esp_ota_*`), gated by an optional `X-Admin-Key` header checked against `admin_pass`; there's no TLS (would cost ~40 KB of flash budget the project doesn't have).

### Telemetry (`telemetry.cpp`)

Optional MQTT client (only starts if `mqtt_host` is configured), publishes state to `bridge/<mac-suffix>/state` and does Home Assistant MQTT autodiscovery (`homeassistant/sensor/.../config`) on (re)connect.

### Partition layout

`partitions_2mb_ota.csv` is hand-cut for the WT32-ETH01's 2 MB flash (not the stock 4MB+ tables): 24 KB NVS, 8 KB otadata, two 960 KB OTA app slots. App partitions must start on 64 KB boundaries, hence the gap after otadata.
