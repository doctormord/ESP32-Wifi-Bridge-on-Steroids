# Handover

Project-level context for someone (or something) coming to this project fresh — scope, intent, and open decisions. This is not a status log (`history.md`) and not a per-session technical snapshot (`handoff.md`); update it when the *shape* of the project changes, not on every session.

## What this project is

Firmware for a WT32-ETH01 board (ESP32-D0WD + LAN8720A Ethernet PHY, 2 MB flash) that acts as a transparent Wi-Fi↔Ethernet bridge: whatever is plugged into the Ethernet port gets bridged onto Wi-Fi by cloning its MAC address onto the Wi-Fi STA interface, so it appears on the wireless network as if directly connected. A separate static management IP on the same interface hosts a config portal and status API, plus optional MQTT telemetry with Home Assistant autodiscovery.

The comments in the code frame the primary use case as bridging something Ethernet-only (an IP camera is mentioned specifically, e.g. in the OTA-pause rationale in `web.cpp`) onto a Wi-Fi-only network segment. As of 2026-08-31 that's not hypothetical — a real camera has been bridged in continuous use for weeks, and the two most recent pieces of work (an optional WiFi watchdog, a crash-forensics endpoint) both exist because of failure modes that only showed up under that real, unattended deployment.

`sdkconfig.defaults` notes it was "abgeleitet aus dem Rust-Original" (derived from a Rust original) — there is or was a Rust implementation of this same bridge that the current sdkconfig tuning traces back to. Not otherwise referenced in this repo; worth knowing if a second implementation surfaces elsewhere.

## Hardware constraints shaping the design

- **8 MB flash** (measured at the chip on 2026-08-13 — an earlier project note wrongly assumed 2 MB; `partitions_2mb_ota.csv` is the obsolete leftover, the live table is `partitions_8mb_ota.csv`). Flash pressure was real under the old 2 MB assumption and shaped a lot of early decisions (documented in `history.md`); it no longer is — current usage is under 30% of a 3 MB OTA slot.
- **No serial access once deployed** (board is presumably installed somewhere enclosed) — this is why OTA has automatic rollback gated on network reachability rather than just "did it boot", why factory reset is a held-GPIO rather than a serial command, and why two whole subsystems exist purely to make unattended failures diagnosable/recoverable without a cable: an opt-in watchdog for a bridge stuck in a degraded-but-connected state (`bridge.cpp`, `wd_enable`), and a coredump-retrieval endpoint (`/api/coredump`) for the harder case of an actual crash. See `handoff.md` for their current state and known gaps.
- Dual 3 MB OTA app partitions plus a 64 KB coredump partition for safe updates and crash retrieval.

## Toolchain choice

PlatformIO with the `pioarduino` fork of `platform-espressif32`, pinned to a specific release (`55.03.34`) rather than tracking `stable` — chosen deliberately (per the original `platformio.ini` comments) because that exact version was known-good with this project structure elsewhere (the comment cited the ESPuino project as precedent). If bumping the platform version, treat it as a deliberate, tested decision, not a routine dependency update.

## Open / historical decisions worth knowing about

- **Arduino was removed** (2026-08-11) in favor of pure ESP-IDF. See `history.md` for the why (RainMaker/Insights getting pulled in unavoidably via Arduino-ESP32 3.x's component manifest, recurring certificate build failures, flash cost). If Arduino APIs are ever needed again, that trade-off should be revisited deliberately, not reintroduced piecemeal.
- The management stack (HTTP portal + MQTT) is deliberately minimal (`CONFIG_LWIP_MAX_SOCKETS=8`, small TCP buffers) — it's sized for a config UI and a telemetry connection, not for the bridged traffic itself, which never touches lwIP at all in the active bridge path.
- **Two independent self-protection mechanisms exist and are easy to conflate.** The portal-toggleable `wd_enable` watchdog (`bridge.cpp`) only runs once the bridge is fully active; a separate, always-on, non-configurable boot-loop guard (`main.cpp`, three failed boots within 60 s) drops the board into the provisioning AP before the watchdog ever gets a chance to run. A 2026-08-31 "the watchdog isn't working" report turned out to be the second mechanism, not a bug in the first — see `history.md` and `handoff.md`.
