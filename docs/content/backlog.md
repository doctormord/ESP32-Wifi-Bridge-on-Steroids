# Backlog

Open tasks and known issues, current priority first. Mutable — edit freely, remove items once done (the permanent record of *that they were done* belongs in `history.md`).

## Next up

- **The boot-loop guard still has no automatic way back to bridge mode.** Once it drops into the provisioning AP (`192.168.4.1`), the board sits there until someone visits the portal or power-cycles it (RTC-persisted fail counter survives a soft reset, not a cold boot). Watchdog-triggered restarts no longer feed this counter (fixed 2026-09-04), which removes the main way the watchdog itself could cause this — but a genuine early-boot problem (e.g. `bridge_wifi_start()` failing outright) still trips it with no way back. For a board with no serial access this is effectively "stuck until physically retrieved" unless someone happens to check the network. A bounded auto-retry (e.g. attempt bridge mode again after N minutes, same portal-toggle requirement as `wd_enable` — no compile-time-only switches) would close this, but needs deciding: retrying blindly risks masking a real hardware fault instead of surfacing it.
- **Find the root cause of the 2026-08-31 triple-crash.** Three boot failures within 60 s tripped the boot-loop guard and dropped the board into the provisioning AP, invisible on the LAN. A stale coredump was found that day but was from an old build and couldn't be decoded. `/api/coredump` plus `scripts/ota_flash.sh`'s `.elf` archiving are in place specifically so the *next* occurrence is diagnosable — check it before reflashing or power-cycling. (A second, 2026-09-04 outage of a similar shape turned out to be a *different* mechanism — the watchdog itself tripping the guard, now fixed — so this one may or may not recur.) See `history.md` (2026-08-31, 2026-09-04).
- **Whether a plain WiFi reconnect actually resolves the original jammer-style "hung driver" state was never isolated-tested.** The 2026-08-16 incident was fixed with a full reboot; the two-stage watchdog (2026-09-04) tries a lighter `esp_wifi_disconnect()`/reconnect first specifically for this case, on the reasoning that a reboot only ever helped because the RF environment had already recovered by then, not because rebooting itself fixes anything WiFi-related. Still unverified against a *real* recurrence — next time this pattern shows up (`wifi_up:true`, near-zero throughput, no coredump), watch whether stage 1 alone clears it or it always needs the full stage-2 reboot.
- **`/api/coredump` doesn't expose `app_elf_sha256`.** Matching a coredump to the right `elf_archive/` entry currently relies on `/api/status`'s `build` field, which only stays correct if no OTA update happened between the crash and the check (a runtime crash reboots into the *same* app partition, so this holds in the common case but not always). The coredump summary struct already carries the crashing app's own SHA — exposing it would make the match exact regardless of what's been flashed since.
- **Check that profile 1's address stays free.** Home is `192.168.178.11`; it must sit outside the FritzBox DHCP pool. An address collision already happened once and cost a factory reset. Profile 2 (lab, `192.168.8.245`) is proven in the field.

## Throughput — closed, don't reopen without new information

Settled on 2026-08-14 by A/B against the Rust original this project derives from: **24.4/35.7 Mbit, level with the original's 29/31**. The limit is the RF path and the ESP32's single antenna versus a MIMO client, not the code.

Already tried and measured — see `history.md` before repeating any of it:

- Retry loop in the TX path: **removed**, it hurt rather than helped (blocking the RX task moves losses into the Ethernet DMA, in bursts, which TCP handles worse).
- Buffer sizes: **more is worse.** Raising `dynamic_tx_buf` to 64 dropped free heap from 111 KB to 9 KB and halved the download — TX and RX buffers compete for the same memory.
- HT40: genuinely negotiated (the `bw` field in `/api/status` proves it), makes no measurable difference here.
- Management stack (lwIP + HTTP + MQTT): costs essentially nothing. A build with all of it removed measured the same.
- `-O2`, Ethernet DMA buffers, the removed BA-window clamp, the staged `esp_wifi_init` fallback: kept, each justified structurally rather than by a single measurement.

**The effective lever is placement.** Over a mesh repeater the upload collapsed to a tenth (3.3 instead of 35.7 Mbit) with identical firmware. Line of sight to the router, or a cabled access point near the camera, beats anything tunable in software.

## Known issues / things to keep an eye on

- Flash is at ~28% of a 3 MB slot — no pressure, but check the `Flash:` line after nontrivial changes anyway.
- Free heap runs around 93 KB in operation. `/api/status` reports it; if it ever approaches ~40 KB the Wi-Fi driver starts failing allocations, which shows up as a collapsing download rather than an obvious error.
- Management interface answers **only from the Wi-Fi side**. From a client on the Ethernet port the ingress works but replies can't get back — lwIP's egress always goes out Wi-Fi. Fixing it needs a custom netif driver that routes by destination MAC; judged not worth touching the working data path for a diagnostic convenience.
- `dependencies.lock` at the project root is a leftover from the Arduino/IDF-Component-Manager era. Presumably unused now (no `idf_component.yml` anywhere), but not confirmed — check before deleting.
- `partitions_2mb_ota.csv` is the obsolete table from when the flash was believed to be 2 MB. The live one is `partitions_8mb_ota.csv`.
- No automated test suite — verification is build success plus on-device behavior. If that ever changes, the off-device-testable parts are `is_for_mgmt()` and the config blob parsing; everything else genuinely needs hardware.
