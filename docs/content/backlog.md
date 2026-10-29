# Backlog

Open tasks and known issues, current priority first. Mutable — edit freely, remove items once done (the permanent record of *that they were done* belongs in `history.md`).

## Code review 2026-10-18 (of PR #13, restart tagging) — all fixed in branch `worktree-perf-review-fixes`

Remove this section once that branch is merged; the details are in `history.md`.

1. [x] `s_eff_*` buffer values were recorded *before* `esp_wifi_init()`, so after a fallback stage `/api/config` `eff` showed the requested values, not what the driver runs with — the portal never warned. Now recorded after the stage that succeeded.
2. [x] `wifi_evt()` called `esp_wifi_connect()` on the disconnect caused by the restart itself, racing the driver stop. Skipped while `s_restarting`.
3. [x] `disc_log_add()` read and cleared the trigger marker outside the spinlock — a concurrent `disc_mark_own()` could be lost or land on the wrong event. Now under `s_dl_mux`; during a restart every disconnect counts as restart.
4. [x] Restart tagging depended on the shutdown-handler slot order (and on `esp_wifi_stop` being registered at all, which it is not in `BRIDGE_MINIMAL`); the registration result was ignored. The handler now disconnects itself and the result is logged.
5. [x] The portal's "Kind" column showed own disconnects (restart, watchdog, roaming) as a red **Drop**. Only external drops are red now.
6. [x] Comment claimed a precondition that is false in `BRIDGE_MINIMAL` — obsolete with 4.

## Performance and autotune — every lever, measured or assessed (2026-10-18)

Measured on the live camera link (ch 11, RSSI −66 dBm, ~3.5 Mbit/s camera upload) with the new TX-done accounting:

- **Air loss is zero.** `air_fail` stayed 0 over hundreds of thousands of frames: the MAC delivers everything within its own hardware retries. All remaining loss is `drop_up` — frames that found no free TX buffer.
- **The TX queue is the camera's burst, not the radio.** Mean queue wait 20–55 ms; each I-frame arrives at 100 Mbit/s over the cable and fills all 32 TX buffers (heap 91 → 36 KB, ≈1.7 KB per waiting frame), then drains at the air rate. This is the 5–120 ms ping jitter seen through the bridge. It did not change between 11.75 and 14.75 dBm.
- **The chip caps at 14.75 dBm.** Every request from 15 to 20 dBm ends at driver value 59; 13/14 dBm end at 11.75/13.25. The IDF mapping table promises 15/16.5/18/20 dBm for those requests. Country is `01`, cap 20 dBm — not the limit. Most likely per-chip calibration or the built-in PHY init data.

| Lever | Status | Reasoning |
|---|---|---|
| TX power autotune | **implemented** | Live, measures loss + queue delay per PHY step, keeps the highest power that is not measurably worse. Result here: 15 dBm (driver 14.75). |
| TX queue / air-loss metrics | **implemented** | `txq_ms`, `air_fail` in portal, API, MQTT/HA — the first direct bufferbloat indicator. Use it the next time ping goes bad. |
| Retry autotune | done earlier | 12 retries, confirmed by long-run loss (0.012 % vs 0.063 % at 8). |
| Queue cap (AQM-lite) | **implemented, measured: off** | `txq_cap` (live) drops frames > 256 bytes once N wait, plus a calibration. Cap 24 halved the queue wait (51 → 28 ms) but lost 0.30 % against 0.00 % without a cap; cap 12 lost ~1.8 % and the camera's TCP stream paused for seconds. The tail of each I-frame has to wait somewhere, and every frame dropped there costs a retransmission — for a video stream the 50 ms queue is the better trade. Stays available for links where latency of *other* traffic matters more (e.g. a client that is not a camera), and as a safety valve if the heap ever runs critically low. |
| Priority for small frames (ACKs, ping, ARP) ahead of the bulk | not possible here | Would give low latency without dropping, but `esp_wifi_internal_tx()` has no priority/AC argument — all bridged frames share one driver queue. |
| Raising the TX power cap above 14.75 dBm | open, low priority | Would need custom PHY init data (`CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION` + a `phy_init` partition → partition table change → serial flash, not OTA). Legally the EU limit is 20 dBm EIRP *including* antenna gain, so the headroom is ~3 dB at most. Only worth it if the uplink margin becomes the problem again (AP hears the bridge ~10 dB weaker than vice versa). |
| Wi-Fi buffer autotune (static/dynamic RX, dynamic TX, BA window) | rejected | Each value goes into `esp_wifi_init()` → a reboot per step, ~15 s stream outage each, and successive steps compare different RF moments. Earlier A/B: more TX buffers cost heap and halved the download. With `txq_ms` a *manual* A/B of `dynamic_tx_buf` 24 vs 32 is now measurable — but the queue cap above does the same job live. |
| Fixed PHY rate (`esp_wifi_internal_set_fix_rate`) | rejected | Disables rate adaptation; any fixed rate is wrong for part of the time. `air_fail` = 0 shows the rate control is not failing. |
| RX gain | not possible | No API on the ESP32. The physical levers are antenna orientation/placement (PCB antenna) or a board variant with an external antenna. |
| HT40 | rejected (again) | No gain measured before; on 2.4 GHz with three APs it only adds overlap. |
| Power save, 802.11b | already optimal | `WIFI_PS_NONE`, 11b off. |
| `wifi_tx_retries` (Wi-Fi→LAN) autotune | not worth it | That direction carries ~100 kbit/s for a camera, `drop_down` ≈ 0. |
| Pinning `emac_rx` to core 1 | unmeasured, low priority | At 3.5 Mbit/s the CPU is not the limit. Note `eth_rx_cb`'s `taskYIELD()` assumes the Wi-Fi task can run — keep that in mind if pinning. |
| Manual country (`esp_wifi_set_country`) | not useful | `01` already allows 20 dBm; a manual `DE` would not lift the chip's cap. |
| 5 GHz (ESP32-C5 + W5500, branch `esp32c5-w5500`) | **biggest structural lever** | Escapes the shared 2.4 GHz channel entirely — tonight's incident (overlapping DCS channels after a power loss) cannot happen there. |

## Next up

- **ESP32-C5 + W5500: the module's LDO shuts down under sustained Wi-Fi TX load (hardware).** Sustained Ethernet -> Wi-Fi traffic resets the board with POWERON after ~85 s at 16 Mbit/s and within seconds above 20 Mbit/s, sooner when already warm, with a ~30 s outage after a long run (see `history.md`). The LDO on the NiceMCU board feeds the C5 and the W5500 (~130 mA at a 100 Mbit link). Fix on the hardware side: feed the W5500 from its own 3.3 V regulator (common GND), or the whole board from a stronger 3.3 V supply; a large capacitor on 3V3 only helps against short dips, not against heat. The firmware now protects itself (temperature throttle + `e2w_max_mbit`, 12 Mbit/s measured safe). Not done yet: forcing the W5500 PHY to 10BASE-T full duplex via PHYCFGR would save ~53 mA (datasheet: 128 -> 75 mA) at the cost of a 10 Mbit/s Ethernet side - an option for low-rate cameras on unmodified hardware. Verify after the change with `ramp.py`-style stepped UDP load (rate-controlled sender on the Ethernet side, `/api/status` polled over Wi-Fi).
- **ESP32-C5: spontaneous power-on reset under load, followed by an unexplained OTA rollback.** With 2 x 8 Mbit/s camera streams running, the board reset with `esp_reset_reason` POWERON and RTC memory lost, nobody touched it - points at the supply (board powered from a laptop USB port, W5500 plus Wi-Fi TX peaks). After that reset it ran the previous image (`2d6240f`) instead of the just-flashed test build; a second flash of the same test build confirmed itself normally after 30 s. Whether the first one had confirmed before the reset is unknown (the previous-boot log is not kept across a power-on). Next time: check `/api/status` `rst` and the running `build`; try a separate 5 V supply.
- **ESP32-C5 throughput: the SPI path is the limit, not the radio.** Next to the AP (-17 dBm) bridged traffic was as fast as at -55 dBm; 80 MHz compensated SPI gave +15-19 % (22 Mbit/s with 8 camera streams). At 80 MHz the bus is busy 46 % of the time, so the remaining ceiling is partly elsewhere (possibly the camera as the only test source). Still open: a laptop speed test behind the bridge at 80 MHz (both directions), and heap headroom - `heap_min` reached 37 KB under load at 80 MHz.

- **Client swapped while the bridge booted without an Ethernet link: the new client gets no DHCP lease.** Seen on the ESP32-C5 target: booted with no link, then the IP camera plugged in; the camera never got a lease and fell back to its own default address (192.168.1.58); an Ethernet restart did not help. Suspected (not yet verified): with no frame to sniff, the bridge keeps the client MAC stored from the previous client (at the time a laptop used for speed tests, `client_mac` in `/api/status` still showed it), so Wi-Fi is associated with the old MAC and the AP drops the camera's frames with its own source MAC. An Ethernet restart does not re-clone - the STA MAC is only set at Wi-Fi start. Check `client_mac` against the camera's MAC next time; a fix would re-clone (Wi-Fi reconnect with the new MAC) whenever the first frames after a link-up come from a different unicast source.

- **Find the root cause of the 2026-08-31 triple-crash.** Three boot failures within 60 s tripped the boot-loop guard and dropped the board into the provisioning AP, invisible on the LAN. A stale coredump was found that day but was from an old build and couldn't be decoded, and it's never recurred in exactly that shape since (the two 2026-09-04/09-05 outages that looked similar both turned out to be different, now-fixed mechanisms — see `history.md`). `/api/coredump` plus `scripts/ota_flash.sh`'s `.elf` archiving are in place specifically so the *next* occurrence is diagnosable — check it before reflashing or power-cycling.
- **Whether a plain WiFi reconnect actually resolves the original jammer-style "hung driver" state was never isolated-tested.** The 2026-08-16 incident was fixed with a full reboot; the watchdog (2026-09-04) tries a lighter `esp_wifi_disconnect()`/reconnect first specifically for this case, on the reasoning that a reboot only ever helped because the RF environment had already recovered by then, not because rebooting itself fixes anything WiFi-related. Still unverified against a *real* recurrence — next time this pattern shows up (`wifi_up:true`, near-zero throughput, no coredump), watch whether the reconnect rung alone clears it or it always needs the full reboot rung.
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
- Free heap dips to ~36 KB for a moment on every camera I-frame: that is the full TX queue (32 frames × ≈1.7 KB), visible as `txq` in `/api/status`. Normal, but close to the threshold below. `txq_cap` would bound it, at the cost measured above.
- Free heap runs around 93 KB in operation. `/api/status` reports it; if it ever approaches ~40 KB the Wi-Fi driver starts failing allocations, which shows up as a collapsing download rather than an obvious error.
- Management interface answers **only from the Wi-Fi side**. From a client on the Ethernet port the ingress works but replies can't get back — lwIP's egress always goes out Wi-Fi. Fixing it needs a custom netif driver that routes by destination MAC; judged not worth touching the working data path for a diagnostic convenience.
- `dependencies.lock` at the project root is a leftover from the Arduino/IDF-Component-Manager era. Presumably unused now (no `idf_component.yml` anywhere), but not confirmed — check before deleting.
- `partitions_2mb_ota.csv` is the obsolete table from when the flash was believed to be 2 MB. The live one is `partitions_8mb_ota.csv`.
- No automated test suite — verification is build success plus on-device behavior. If that ever changes, the off-device-testable parts are `is_for_mgmt()` and the config blob parsing; everything else genuinely needs hardware.
