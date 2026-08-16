# Handoff

Technical state for whoever (human or Claude) picks up the next coding session. This file reflects the *current* state only — overwrite it each time; it is not a log (that's `history.md`) and not project-level context (that's `handover.md`).

## State as of 2026-08-14

The firmware is **verified on real hardware and in continuous use**. Bridge data path, management IP, config portal, MQTT, OTA, two network profiles with automatic location detection — all exercised at two different sites.

Updates go over the network (`POST /api/update`); the serial cable is only needed if the board becomes unreachable. Build with `~/.pio-py313/bin/pio` — see `CLAUDE.md` for why plain `pio` fails.

**Throughput is settled — don't reopen it without new information.** 24.4/35.7 Mbit at a good location, statistically indistinguishable from the Rust original (29/31) that this project was derived from. The limit is the RF path and the ESP32's single antenna, not the code. `history.md` has the full comparison and the methodological lesson; `backlog.md` records which knobs were tried and why further tuning isn't worth it.

## What's next

- **Connect the actual camera.** Every test so far used a MacBook as the Ethernet client. The MAC is learned from *any* frame, not specifically DHCP, so a fixed-IP camera should work — but that case is untested.
- **Verify profile 1's address is permanently free.** Currently `192.168.178.11` for home; it must sit outside the FritzBox DHCP pool or the conflict will return. Profile 2 (lab, `192.168.8.245`) is proven.
- Optional, low value: management access from the Ethernet side — see `backlog.md`. Today the portal answers only from the Wi-Fi side.

## Hardware setup

- **External 5 V supply at the 5V pin — not USB, not a powerbank.** Both failure modes were traced on 2026-08-13 and both mimic firmware bugs; the details and their tell-tale symptoms are in the `wt32-eth01-serial-adapters` memory and in `history.md`.
- Serial adapter is a CH340 with no USB serial number, so its `/dev/cu.usbserial-*` name follows the USB socket — re-check it each session. A second adapter (CH343, `/dev/cu.usbmodemXXXXXXXX`) belongs to an unrelated board; **always pass `--upload-port` explicitly.**
- Serial flashing needs the **IO0 jumper to GND**, and the firmware only runs with it removed. Reset can be triggered from the host, so only the jumper needs hands.
- **Never open the serial port while an OTA is in flight** — opening it toggles DTR/RTS and resets the board. That aborted an OTA once, and the reconnect then failed the WPA2 handshake (reason 204) because the cloned MAC re-associated before the AP had released the old session.

## Safety net that has proven itself

A firmware OTA'd into the inactive slot stays `PENDING_VERIFY` until it confirms itself after 30 s of uptime. Anything that doesn't confirm — a crash, or a build that deliberately omits the confirmation — is undone by a single power cycle. This worked four times: twice deliberately (the management-free measurement build, the Rust original) and twice after real crashes. It makes flashing experimental images over the air safe even with no serial adapter attached.
