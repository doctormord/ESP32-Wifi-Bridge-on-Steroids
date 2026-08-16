# Handover

Project-level context for someone (or something) coming to this project fresh — scope, intent, and open decisions. This is not a status log (`history.md`) and not a per-session technical snapshot (`handoff.md`); update it when the *shape* of the project changes, not on every session.

## What this project is

Firmware for a WT32-ETH01 board (ESP32-D0WD + LAN8720A Ethernet PHY, 2 MB flash) that acts as a transparent Wi-Fi↔Ethernet bridge: whatever is plugged into the Ethernet port gets bridged onto Wi-Fi by cloning its MAC address onto the Wi-Fi STA interface, so it appears on the wireless network as if directly connected. A separate static management IP on the same interface hosts a config portal and status API, plus optional MQTT telemetry with Home Assistant autodiscovery.

The comments in the code frame the primary use case as bridging something Ethernet-only (an IP camera is mentioned specifically, e.g. in the OTA-pause rationale in `web.cpp`) onto a Wi-Fi-only network segment.

`sdkconfig.defaults` notes it was "abgeleitet aus dem Rust-Original" (derived from a Rust original) — there is or was a Rust implementation of this same bridge that the current sdkconfig tuning traces back to. Not otherwise referenced in this repo; worth knowing if a second implementation surfaces elsewhere.

## Hardware constraints shaping the design

- **2 MB flash**, not the 4 MB+ most ESP32 dev-board tooling assumes — hence the hand-cut `partitions_2mb_ota.csv` and the flash-size build flag override in `platformio.ini`.
- **No serial access once deployed** (board is presumably installed somewhere enclosed) — this is why OTA has automatic rollback gated on network reachability rather than just "did it boot", and why factory reset is a held-GPIO rather than a serial command.
- Dual 960 KB OTA app partitions for safe updates, which is the source of most of the flash-budget pressure documented in `CLAUDE.md` and `backlog.md`.

## Toolchain choice

PlatformIO with the `pioarduino` fork of `platform-espressif32`, pinned to a specific release (`55.03.34`) rather than tracking `stable` — chosen deliberately (per the original `platformio.ini` comments) because that exact version was known-good with this project structure elsewhere (the comment cited the ESPuino project as precedent). If bumping the platform version, treat it as a deliberate, tested decision, not a routine dependency update.

## Open / historical decisions worth knowing about

- **Arduino was removed** (2026-08-11) in favor of pure ESP-IDF. See `history.md` for the why (RainMaker/Insights getting pulled in unavoidably via Arduino-ESP32 3.x's component manifest, recurring certificate build failures, flash cost). If Arduino APIs are ever needed again, that trade-off should be revisited deliberately, not reintroduced piecemeal.
- The management stack (HTTP portal + MQTT) is deliberately minimal (`CONFIG_LWIP_MAX_SOCKETS=8`, small TCP buffers) — it's sized for a config UI and a telemetry connection, not for the bridged traffic itself, which never touches lwIP at all in the active bridge path.
