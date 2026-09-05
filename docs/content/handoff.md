# Handoff

Technical state for whoever (human or Claude) picks up the next coding session. This file reflects the *current* state only — overwrite it each time; it is not a log (that's `history.md`) and not project-level context (that's `handover.md`).

## State as of 2026-09-05

The firmware is **verified on real hardware and in continuous use** bridging a live IP camera. Bridge data path, management IP, config portal, MQTT, OTA, two network profiles with automatic location detection, a three-stage self-healing watchdog, and a crash-forensics endpoint — all exercised on the deployed board, not just compile-tested.

Updates go over the network (`POST /api/update`, or `scripts/ota_flash.sh <ip> [admin-key]` which also archives the matching `.elf` — see below); the serial cable is only needed if the board becomes unreachable. Build with `~/.pio-py313/bin/pio` — see `CLAUDE.md` for why plain `pio` fails.

**Throughput is settled — don't reopen it without new information.** See `backlog.md` for what was tried; `history.md` (2026-08-14/15) has the full comparison against the Rust original and the methodological lesson.

## Two resilience mechanisms exist, and they're now coordinated

- **`wd_enable`** (`bridge.cpp`, portal-toggleable, default **off**): a three-stage escalation, branching by symptom. "Ethernet link up but the camera's gone silent" (checked only once a gateway probe confirms WiFi itself is fine, to avoid double-counting a WiFi problem as a camera one) → `bridge_reset_eth()` first (driver stop/start, no chip reboot, WLAN untouched). Any WiFi-side symptom (drop ratio, reconnect churn, failed gateway probe) → `esp_wifi_disconnect()` directly (the existing `WIFI_EVENT_STA_DISCONNECTED` handler reconnects on its own). Whichever ran first, if the symptom persists the other lightweight remedy is tried next; only if *that* doesn't help does it reboot — via `abort()`, not `esp_restart()`, so a coredump is produced. 20 s windows (was 10 s), 3 bad windows before the first escalation.
- **The pre-existing boot-loop guard** (`main.cpp`, always on, not configurable): after 3 boots that never reach 60 s of stable operation, the firmware gives up and drops into the provisioning AP (`192.168.4.1`) instead of retrying bridge mode.

**They used to not compose, and that caused two real incidents before both gaps closed.** A 2026-08-31 total outage was the boot-loop guard, not a watchdog failure — the watchdog can only act while the bridge is running, and that outage was a boot-time problem. A 2026-09-04 incident turned out to be the watchdog *causing* the boot-loop guard to trip: each `esp_restart()` it issued during a prolonged WiFi problem counted as a failed boot attempt toward the same 3-strikes counter. Fixed by exempting watchdog-triggered restarts from that counter (`bridge_mark_planned_restart()`/`bridge_consume_planned_restart()`, RTC-persisted one-shot flag, same pattern as the existing `PORTAL_MAGIC`). A third, 2026-09-05 incident showed a *different* bypass of the same counter: `bridge_wifi_start()` gave each SSID only one connect attempt, and a single failure forces the board into the portal directly (`main.cpp`'s `s_portal_request`/`PORTAL_MAGIC`), never even consulting the 3-strikes counter. Fixed by retrying the same SSID (`WIFI_CONNECT_RETRIES_DEF`=3, `WIFI_CONNECT_TIMEOUT_MS_DEF`=20s, both portal-overridable as `wifi_connect_retries`/`wifi_connect_timeout_s`) before giving up on it.

**And now there's a way back even when the board does end up in the portal.** `ap_idle_reboot_s` (default 900s/15min, portal-configurable): while in the provisioning AP, if no real portal activity has occurred for that long, the board attempts one ordinary reboot with whatever WiFi credentials are already stored. Gated on `g_cfg.configured` so a factory-reset or never-configured board doesn't reboot-loop pointlessly. "Real activity" is tracked by `web_last_activity_ms()` (`web.cpp`) across `h_root`/`h_config_get`/`h_config_post`/`h_scan` — deliberately *not* updated by the page's own 2s status auto-poll, or a browser tab left open would block the auto-recovery forever.

If a future outage looks like any of these again, check `/api/status`'s `prov` field and whether the board is reachable on `192.168.4.1` — and if it's stuck there, it should now recover on its own within `ap_idle_reboot_s` as long as credentials are stored.

## Watchdog visibility

- `/api/status` and the periodic MQTT state payload carry `wd_reconnects`, `wd_eth_resets` (both plain runtime counters, reset on boot) and `wd_last_reason` (1=drop ratio, 2=reconnects, 3=gateway probe, 4=camera silent; RTC-persisted, so it survives the eventual reboot and stays visible until the next watchdog event overwrites it).
- `telemetry_note_watchdog_event()` additionally publishes a discrete MQTT message to `bridge/<node>/watchdog` (action + reason + uptime) at the moment each escalation step fires, rather than only being discoverable as a counter at the next periodic refresh — Home Assistant's own message-received timestamp is the "when", since the board has no real-time clock.
- `bridge_reset_eth()` is also reachable manually via `POST /api/eth_reset` + a portal button, independent of watchdog state.

## Crash forensics: what exists, what's still missing

- `GET /api/coredump` (admin-key gated like OTA): panic reason, crashing task, PC, backtrace, as JSON. `DELETE /api/coredump` erases it.
- `/api/status` reports `"build"`: the git short-rev the running firmware was built from (`+"-dirty"` if built from an uncommitted tree). Generated by `scripts/gen_build_info.py` as a PlatformIO pre-build step, written to `src/build_info.h` (gitignored, regenerated every build).
- `scripts/ota_flash.sh <ip> [admin-key]` builds, copies `firmware.elf` into `elf_archive/<rev>_<utc-timestamp>.elf` (gitignored), then uploads. **Use this instead of a bare `curl` OTA** — the whole point is that a future coredump needs the exact matching `.elf`, which plain `pio run` + manual OTA doesn't preserve (PlatformIO overwrites `.pio/build/.../firmware.elf` on every build).
- Matching a coredump to an archived `.elf` currently relies on `/api/status`'s `build` field being read *before* any subsequent OTA update happens — a runtime crash reboots into the same OTA partition, so this holds in the common case, but isn't foolproof. See `backlog.md` for the more robust fix (exposing `app_elf_sha256`).
- **Still open:** the actual root cause of the 2026-08-31 triple-crash that triggered the boot-loop guard. A stale coredump was found and erased that day but couldn't be decoded (wrong build). Next occurrence: check `/api/coredump` before reflashing or otherwise touching the board.

## What's next

See `backlog.md` — the boot-loop auto-recovery gap is closed now; it leads with the 2026-08-31 coredump root-cause hunt and verifying whether the watchdog's lighter reconnect rung alone actually clears a hung-driver state, both more pressing than the old throughput/tuning items.

## Hardware setup

- **External 5 V supply at the 5V pin — not USB, not a powerbank.** Both failure modes were traced on 2026-08-13 and both mimic firmware bugs; the details and their tell-tale symptoms are in the `wt32-eth01-serial-adapters` memory and in `history.md`.
- Serial adapter is a CH340 with no USB serial number, so its `/dev/cu.usbserial-*` name follows the USB socket — re-check it each session. A second adapter (CH343, `/dev/cu.usbmodemXXXXXXXX`) belongs to an unrelated board; **always pass `--upload-port` explicitly.**
- Serial flashing needs the **IO0 jumper to GND**, and the firmware only runs with it removed. Reset can be triggered from the host, so only the jumper needs hands.
- **Never open the serial port while an OTA is in flight** — opening it toggles DTR/RTS and resets the board. That aborted an OTA once, and the reconnect then failed the WPA2 handshake (reason 204) because the cloned MAC re-associated before the AP had released the old session.

## Safety net that has proven itself

A firmware OTA'd into the inactive slot stays `PENDING_VERIFY` until it confirms itself after 30 s of *network-reachable* uptime, not just "booted" (see `ota_confirm_if_pending()`). Anything that doesn't confirm — a crash, or a build that deliberately omits the confirmation — is undone by a single power cycle. Proven repeatedly, most recently across three back-to-back OTA cycles on 2026-08-31 while building the coredump endpoint.
