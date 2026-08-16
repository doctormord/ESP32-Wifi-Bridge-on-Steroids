# ESP32 Wi-Fi Bridge on Steroids

A **transparent Layer-2 Wi-Fi ↔ Ethernet bridge** for the WT32-ETH01, with a web portal, MQTT telemetry, over-the-air updates and live performance tuning.

Plug an IP camera (or any Ethernet device) into the board, and it appears on your Wi-Fi network **as if it were cabled directly to your router** — same MAC, its own DHCP lease, directly reachable, discovery protocols intact. No NAT, no second subnet, no port forwarding.

Written in C++ against plain ESP-IDF. Derived from [owenthewizard/esp32-wifi-bridge](https://github.com/owenthewizard/esp32-wifi-bridge) (Rust), which contributed the core idea and the driver tuning this project started from.

---

## The magic, in one paragraph

An 802.11 station operates in *3-address mode*: the access point delivers frames only to the MAC address that associated with it. A normal bridge would need 4-address mode (WDS), which almost no consumer AP offers. This project sidesteps that entirely — it **sniffs the MAC address of the device on the Ethernet port and clones it onto the Wi-Fi interface**. From the network's point of view there is no bridge at all; there is just your camera, on Wi-Fi.

The second trick is that the bridge stays manageable while doing this. `esp_wifi_internal_reg_rxcb()` has exactly one callback slot, normally owned by lwIP. This firmware takes it over and acts as the doorman itself: frames addressed to a separate static management IP go to the local TCP/IP stack, everything else is forwarded raw between the two interfaces without ever touching lwIP. **One interface, one MAC, two IP addresses** — one for the bridged device, one for the bridge itself.

---

## Why not one of the existing solutions?

This is the question worth answering before you build anything.

### NAT-based ESP32 "repeaters" / `esp32_nat_router` and friends

By far the most common approach, and the reason people end up disappointed. The wired device lands **behind NAT on its own subnet**. Consequences:

- It is **not reachable from your LAN** unless you set up port forwarding, per port, per device.
- **mDNS / SSDP / ONVIF WS-Discovery do not cross the NAT boundary.** Your NVR will not find the camera. Frigate, Home Assistant, Synology Surveillance Station — all of them discover cameras by broadcast or multicast.
- The device gets a fake DHCP lease from the ESP32, not from your router. No DHCP reservations, no name in your router's device list, no consistent addressing.
- RTSP works only if you forward it manually and hardcode the address everywhere.

This project has **no NAT and no second subnet**. The camera gets a real lease from your real DHCP server and answers ARP on your real LAN.

### OpenWrt client mode + `relayd`

Works, and is a genuine bridge of sorts — but `relayd` is a proxy-ARP pseudo-bridge, not true L2 forwarding. It handles IPv4 well and everything else poorly (IPv6, non-IP protocols, some multicast). You also need a device running OpenWrt: more cost, more power draw, more to configure, and a second thing that can break.

### WDS / 4-address mode

The technically correct answer, and unavailable in practice. Consumer APs — FritzBox, most mesh systems, ISP routers — do not expose it, or only between units of the same brand.

### Commercial Wi-Fi client bridges (Vonets, TP-Link in client mode, …)

These do essentially the same MAC-cloning trick, and they work. What you don't get: any insight into what is happening. No packet counters, no loss rate, no RSSI history, no MQTT, no OTA, closed firmware, and no way to tune the radio when the link misbehaves. When throughput is bad, you are guessing.

### The Rust original this project came from

[owenthewizard/esp32-wifi-bridge](https://github.com/owenthewizard/esp32-wifi-bridge) implements the same core idea cleanly and compactly, and its Wi-Fi and Ethernet buffer configuration is the basis for this one. What it deliberately does not have — its own TODO lists *"Add HTTP provisioning webpage"* as the top open item — is everything around the data path: no configuration interface, no telemetry, no updates without a cable, no diagnostics. Credentials are compiled in via environment variables.

**If you want a minimal, elegant bridge and you are comfortable reflashing over USB to change an SSID, use the original.** It is less code and less surface area. This project exists for the case where the board ends up somewhere you cannot easily reach.

---

## Features

### Bridging

- **Transparent L2 forwarding** in both directions, raw frames, no IP stack in the path.
- **MAC cloning** with automatic sniffing of the attached device, plus persistence — a camera only talks when *it* boots, so the learned MAC is stored and reused if the bridge restarts on its own.
- **Promiscuous Ethernet mode**, so unicast traffic to the client is forwarded, not just broadcast.
- Hot-path callbacks in IRAM; no flash cache miss per forwarded packet.

### Management

- **Static management IP on the same interface**, selectively delivered to lwIP. The bridge is reachable while bridging.
- **Self-contained web portal** — status, configuration, tuning, firmware upload. Single embedded page, no external assets, no internet required.
- **Provisioning mode**: with no credentials stored, the board opens an access point and serves the same portal at `192.168.4.1`.
- **JSON API** (`/api/status`, `/api/config`, `/api/scan`, `/api/reboot`, `/api/update`, `/api/autotune`) — everything the portal does is scriptable.
- **NetBIOS name announcement**, so the bridge shows up by name instead of "unknown" in routers that read NBNS (FritzBox does; OpenWrt-based routers do not).

### Multi-site operation

- **Three network profiles**, each with its own SSID, password, IP, netmask and gateway.
- **Automatic location detection**: after associating, the bridge pings each configured profile's gateway and adopts the one that answers. This works even when both sites use the *same SSID* — a case where selecting by SSID alone cannot work.
- **Strongest-AP selection** (`WIFI_ALL_CHANNEL_SCAN` + `WIFI_CONNECT_AP_BY_SIGNAL`). In a mesh with several nodes sharing an SSID, the ESP-IDF default (`WIFI_FAST_SCAN`) picks whichever it finds first and never re-evaluates. Getting this wrong cost 20 dB and two thirds of the throughput in testing.

### Updates and recovery

- **OTA over HTTP** with automatic rollback. A firmware that does not confirm itself within 30 seconds of *network reachability* is undone by a single power cycle. This has been exercised repeatedly, including after real crashes.
- **Crash-loop protection**: three starts without 60 seconds of stable operation, and the board falls back to the portal instead of rebooting forever.
- **Factory reset** by holding GPIO14 low for two seconds at boot.
- **Core dumps** to a dedicated flash partition, for a board with no serial access once installed.

### Telemetry and diagnostics

- **MQTT** with Home Assistant autodiscovery: RSSI, channel, throughput both directions, packet and drop counters, uptime, free heap, Ethernet link state, and the bridged client's IP.
- **Client IP and hostname snooping** — the bridge reads the client's source address from IPv4/ARP frames and its hostname from DHCP option 12. Invaluable when a camera is "missing": you can see immediately whether it has an address, which one, and whether it announces a name at all.
- **Actual values, not requested ones**: TX power and channel width are read back from the driver, because the PHY quantises TX power to 11 discrete steps and HT40 is only ever a request.
- **Heap low-water mark, DMA-capable free space and largest free block**, so memory pressure is visible before it becomes a crash.

### Tuning

- **Live** (no reboot): TX power, and TX retry counts for both directions.
- **Reboot required**: Wi-Fi buffer counts, RX block-ack window, HT20/HT40, 802.11b rates.
- **Autotune**: sweeps the retry count, measures the loss rate under real traffic, and picks the *smallest* value that stays under 0.10 % — not the best one, because every retry blocks the Ethernet receive task a little longer. Aborts honestly if there is not enough traffic to measure.

---

## Hardware

- **WT32-ETH01** (ESP32-D0WD + LAN8720A PHY). No PSRAM, and none is needed — Wi-Fi DMA buffers must live in internal RAM anyway.
- **8 MB flash** on the board used for development. The partition table (`partitions_8mb_ota.csv`) gives two 3 MB OTA slots plus a 64 KB core dump partition. `partitions_2mb_ota.csv` is kept only for boards that really have 2 MB.
- **A proper 5 V supply at the 5V pin.** Not USB, and *not a power bank*. Two separate failure modes were traced to power during development, and both look exactly like firmware bugs:
  - Fed from a USB-serial adapter's 5 V pin, the rail collapses when Wi-Fi starts and takes the adapter down with it. Tell-tale: the *adapter* re-enumerates on USB — a crashing ESP32 cannot do that.
  - A power bank switches off because the ESP32 in download mode draws less than its idle threshold. Symptom: serial reads work right after a reset, then `esptool` fails with "No serial data received". Looks identical to a broken TX wire.

### Wiring for flashing

| USB-serial adapter | WT32-ETH01 |
|---|---|
| TX | RX0 (crossed) |
| RX | TX0 (crossed) |
| GND | GND |
| DTR / RTS | IO0 / EN (optional, for automatic reset) |

Use a **3.3 V logic** adapter. Pull **IO0 to GND** to enter download mode; the firmware only runs with it removed. Leave **IO14 unconnected** — it is the factory reset input.

---

## Building

This is a PlatformIO project using `framework = espidf` (no Arduino).

```bash
git clone https://github.com/doctormord/ESP32-Wifi-Bridge-on-Steroids.git
cd ESP32-Wifi-Bridge-on-Steroids
pio run -e wt32-eth01
pio run -e wt32-eth01 -t upload --upload-port /dev/ttyUSB0
```

Always pass `--upload-port` explicitly if you have more than one serial adapter attached.

### PlatformIO needs Python 3.10 – 3.13

The `platform-espressif32` package rejects Python ≥ 3.14 outright. If your system Python is newer (Homebrew's `platformio` pulls `python@3.14` and rebuilds its own venv on every run, so patching that venv does not stick), create a standalone environment:

```bash
python3.13 -m venv ~/.pio-py313
~/.pio-py313/bin/pip install "platformio==6.1.19" pyyaml
~/.pio-py313/bin/pio run -e wt32-eth01
```

`pyyaml` is not a PlatformIO dependency but the platform's `component_manager.py` imports it.

### Flash size is configured in two places

`board_build.flash_size` sets what ESP-IDF believes; `board_upload.flash_size` sets the size nibble esptool writes into the image header. Both must match your board. `esp_flash_init_default_chip()` **fails hard** when the detected chip is smaller than the header claims — a too-large header means the board never boots. A too-small one only wastes capacity.

---

## Onboarding

1. **Flash** the firmware, remove the IO0 jumper, power-cycle.
2. **Connect an Ethernet device** — the bridge waits for its first frame to learn the MAC. A laptop is ideal for the first run: it speaks DHCP and ARP on its own, so success is visible in both directions immediately.
3. With no credentials stored, the board opens **`<name>-setup-XXXX`** (password `bridgesetup`). Join it and open **`http://192.168.4.1`**.
4. Enter Wi-Fi credentials and a **static management IP**.
   - It must be **outside your DHCP pool**. The bridge and the client share one MAC, so a DHCP server that hands out that address will collide with the static one.
   - The **gateway field is your router**, not a second management address.
5. Save. The board reboots into bridge mode and is reachable at the address you set.

### A consequence worth understanding

Because the bridge and the client share a MAC address, your router shows **one device, not two**. Whatever name or DHCP reservation you attach to that MAC applies to both. Tell them apart by IP, not by MAC.

### The management IP is reachable from the Wi-Fi side only

Ingress from the Ethernet side works, but replies cannot get back: lwIP's egress always goes out over Wi-Fi, and the access point will not relay a frame back to the station it came from. Reach the portal from the LAN, not from a laptop sitting behind the bridge.

---

## Performance

Measured on the development board, over a mesh access point at −61 dBm, with an IP camera streaming:

| | |
|---|---|
| Peak throughput | **21.2 Mbit/s** |
| Loss at peak | **0.003 %** |
| Sustained camera stream (~10 Mbit/s) | **0.005 %** loss over an hour |
| Latency through the bridge | **4.9 ms** average, better than the host's own Wi-Fi (7.5 ms) |

The upstream project reports ~50 Mbit/s symmetrical. That figure could not be reproduced here with **either** implementation — in a direct A/B on the same board, at the same spot, at the same time, both landed around 60 Mbit/s *combined* (24/36 and 29/31 respectively). Radio environment dominates: the same firmware delivered 3.3 Mbit/s upstream on a weak mesh node and 35.7 Mbit/s on a good access point, without a single line changed.

**Placement beats tuning.** If throughput disappoints, check which access point you are actually associated with before touching anything else.

### Retry counts matter more than anything else

The single most important setting, and the one that is counter-intuitive:

| Retries | Loss (UDP camera stream) |
|---|---|
| 1 | 13.29 % |
| 4 | 0.426 % |
| 8 | 0.059 % |
| 16 | **0.000 %** |

For **TCP**, dropping fast is better — the protocol retransmits and backs off, and blocking the receive task only causes bursty loss in the Ethernet DMA, which TCP handles worse than isolated drops. For **UDP video**, every dropped packet is a permanent artefact. The default is 8; use the autotune to find the right value for your link.

Note that more is not free: each retry holds the `emac_rx` task a little longer, and what arrives during that time is lost in the DMA where no counter can see it.

### More Wi-Fi buffers make things worse

Raising `dynamic_tx_buf` from 32 to 64 dropped free heap from 111 kB to 9 kB and halved the download, because TX and RX buffers compete for the same memory. The defaults (16/32/32, BA window 32) come from the upstream project and are a good place to stay.

---

## MQTT

Publishes to `bridge/<mac-suffix>/state` as JSON, with an availability topic and Home Assistant autodiscovery under `homeassistant/sensor/…`. Sensors: RSSI, channel, throughput up/down, packets up/down, drops up/down, uptime, free heap, client IP, and Ethernet link as a connectivity binary sensor.

TLS is deliberately not supported — it would cost roughly 40 KB of flash for a device that only ever talks to a broker on the local network.

---

## Troubleshooting

**Board unreachable after a config change.** Power-cycle. A firmware that failed to confirm itself is rolled back automatically; a bridge that could not associate restarts into the portal; three unstable starts in a row also land in the portal.

**Bridge works, portal does not.** You are probably reaching it from the Ethernet side. Use the Wi-Fi side.

**Portal shows stale values after an update.** Fixed by `Cache-Control: no-store`, but a hard reload (`Ctrl/Cmd+Shift+R`) clears anything cached before that.

**Throughput poor, RSSI good.** Check the retry count first, then which AP you are on (`/api/scan` shows what the board sees from where it is mounted, which is not what your phone sees from where you are standing).

**`sdkconfig` changes have no effect.** `sdkconfig.<env>` is a generated cache; ESP-IDF only fills in options *missing* from it. Delete it and let it regenerate from `sdkconfig.defaults` rather than hand-editing.

**Board silent, adapter fine.** Power. See the hardware section.

---

## Project layout

```
src/
  main.cpp        entry point, boot modes, crash-loop protection, NetBIOS
  bridge.cpp      the data path, Wi-Fi/Ethernet setup, autotune
  web.cpp         portal page and JSON API
  config.cpp      NVS-backed configuration
  telemetry.cpp   MQTT and Home Assistant discovery
  compat.h        millis()/delay() over esp_timer/vTaskDelay
docs/content/     development log, backlog, handoff notes
```

`BridgeConfig` is stored as a single magic-prefixed NVS blob. `cfg_load()` accepts blobs shorter than the current struct, so new fields must be **appended only** — never inserted. Changing the size of an existing field requires a factory reset.

---

## Credits

This project stands on [owenthewizard/esp32-wifi-bridge](https://github.com/owenthewizard/esp32-wifi-bridge) by Owen Walpole, which established the approach and whose Wi-Fi and Ethernet buffer configuration this one still uses. Where the two were measured against each other on identical hardware, they performed the same — the differences here are in what surrounds the data path, not in the bridging itself.

Thanks also to [esp-rs](https://github.com/esp-rs/) and [Espressif](https://www.espressif.com/).

## License

GNU General Public License v3.0 or later, following the upstream project.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
