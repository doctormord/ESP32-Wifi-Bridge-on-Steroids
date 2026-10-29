# Draining a W5500 in MACRAW mode under line-rate bursts

Technical note on the receive path of `src/w5500_mac.c`, written for people
maintaining W5500 drivers elsewhere (ESP-IDF / esp-eth-drivers, Zephyr, the
Linux `w5100` driver, RP2040 ports). It covers what limits reception when the
W5500 is used as a plain MAC (socket 0 in MACRAW mode), what was measured, the
technique that helped, and what did not.

## Abstract

In MACRAW mode the W5500 has at most 16 KB of RX memory for socket 0 — 10.8
full-size frames. An Ethernet sender at 100 Mbit/s fills that in 1.3 ms. Whether
a burst survives therefore depends on how fast the host empties the memory
*while the burst is still arriving*, not on its average throughput. The usual
driver loop (read the received size, read a batch, then write `Sn_RX_RD` and
issue `RECV`) frees memory only once per round, at the end. Measured on an
ESP32-C5 at 80 MHz SPI, that loop lost frames from bursts of ~12 frames on.

Releasing each part of a batch right after it is read, with the release
transactions queued behind the data transactions in one DMA-driven chain, lets
bursts of 15 full frames through without loss. The same change raised sustained
UDP throughput from 43 to 56–59 Mbit/s. The release does not have to follow
frame boundaries: `Sn_RX_RD` can point into the middle of a frame.

## Setup

- ESP32-C5 (single-core RISC-V, 240 MHz), ESP-IDF 5.5.1, W5500 on SPI2 through
  the GPIO matrix at 80 MHz. Reads are half-duplex with one dummy clock, which
  compensates for MISO arriving late at that clock.
- The W5500 runs as a transparent L2 bridge port: socket 0 in MACRAW mode with
  all 16 KB of RX and TX memory, every received frame forwarded to Wi-Fi.
- The sender is a laptop's USB Ethernet adapter at 100 Mbit/s full duplex,
  sending rate-controlled UDP or bursts of N back-to-back 1514-byte frames.
- **Loss accounting:** frames on the wire (the sender's interface counter) minus
  frames the driver received. **The W5500 itself does not count dropped frames in
  MACRAW mode.** Without this comparison, overflow losses are invisible on the
  device.

## Where the time goes

One full frame plus its 2-byte length header is 1516 bytes. At 80 MHz SPI that
is 152 µs of pure transfer; at 100 Mbit/s a new frame arrives every 121 µs. Even
with zero overhead, the host drains at most ~80 % of line rate, so during a
line-rate burst the RX memory always fills. What can be influenced is how fast
it fills and when the memory comes back.

This trace of one 30-frame burst comes from the classic loop (one read per
round, release at the end). The RSR column is RX memory occupied at the start of
each round:

| Round | Frames read | Round time | Transfer time | Overhead | RSR at start |
|---|---|---|---|---|---|
| 1 | 1 | 370 µs | 152 µs | 218 µs | 1516 |
| 2 | 3 | 673 µs | 455 µs | 218 µs | 4548 |
| 3 onwards | 5 | 935 µs | 758 µs | ~180 µs | 7580 |

During a 935 µs round, 7.7 new frames arrive, while the 5 frames being read
still occupy their memory until the release at the end of the round. 5 + 7.7
frames do not fit into 10.8, so about two frames are lost per round.

The overhead consists of:
- the polled register accesses: interrupt clear, RSR read, RX_RD write, RECV;
- task wake-up;
- the delivery of the last frames before the next read can start.

## Technique

### 1. One chain per round, release per piece

A round's read is split into pieces of about two frames (3040 bytes). For each
piece, three transactions are queued:

```
read   RX memory [rd + pos, rd + pos + n)   -> host buffer
write  Sn_RX_RD = rd + pos + n
write  Sn_CR = RECV        (on the last piece also Sn_IR = RECV, i.e. clear)
...
read   Sn_RX_RSR + Sn_RX_RD (4 bytes)      -> size for the next round
```

All transactions of a round are queued to the SPI driver at once. The SPI
interrupt starts each one as the previous ends, so the bus never waits for the
task. Each piece's memory is returned to the chip about one piece-time after it
was read, instead of at the end of the round.

The size read at the end of the chain gives the next round its length without
a separate polled access.

Clearing `Sn_IR.RECV` together with the last RECV, before that size read, keeps
the usual rule: a frame stored after the size was read raises INT again. If the
task loop clears the interrupt on its own later, it must discard the size from
the chain and read it again.

### 2. Byte-granular release

The chain releases exactly the bytes it read, even when the last piece ends in
the middle of a frame. The host keeps the head of that frame and appends the
rest in the next round.

The W5500 treats socket RX memory as a byte ring (`Sn_RX_RSR = Sn_RX_WR −
Sn_RX_RD`); the 2-byte length headers in MACRAW mode are only a convention
between chip and driver. Evidence: zero length-header resyncs over several
hundred thousand frames in this test series, and TCP checksums verified on the
receiving side.

The point of byte-granular release is that the release transaction can be
queued before the data has been parsed. Without it, the host would have to
parse each piece before it knows how far to release.

### 3. Overlapping delivery with the transfer

While later pieces are still being transferred, the frames of the pieces
already received are handed to the stack. Here that is the Wi-Fi driver, which
on a single core costs ~30 µs per frame in the caller plus ~45 µs in the Wi-Fi
task.

The frames of the last piece are copied out and delivered during the next
round's transfer. If there is no next round, they are delivered right away.

## Results

All figures on the same board and link:

| | Classic loop | Chain, per-piece release |
|---|---|---|
| Sustained UDP, 70 Mbit/s offered | 42.9 Mbit/s | **55.7–59.1 Mbit/s** |
| Bursts of 15 full frames | ~1 lost per burst | **0** |
| Bursts of 20 full frames | 4.0 lost | 0.9–1.4 lost |
| Bursts of 40 full frames | 14.4 lost | 5.5–7.8 lost |
| Bursts of 60 full frames | 24.8 lost | 14–16 lost |
| Download direction (host → W5500 TX) | 32.8 Mbit/s | 32.6–32.9 Mbit/s |

From the marginal loss of long bursts, the drain rate during a line-rate burst
is ~66–68 % of 100 Mbit/s, against the ~80 % that 80 MHz SPI allows
theoretically. Removing most of the per-transaction interrupt work (SCT, see
below) did not change it, so the gap is not in the SPI driver.

## What limits it now, and what did not help

- **TCP from a sender with TSO/GSO is still burst-limited.** A 64 KB segment
  burst is 44 frames at line rate. Even at the theoretical drain rate, 16 KB of
  RX memory absorbs only about 50 frames, and in practice ~20. Measured: 5–7 %
  of such a sender's frames are lost before the driver, and TCP throughput stays
  at 34–40 Mbit/s with or without the chain.
- **PAUSE frames (802.3x) have no effect.** The driver can send them through
  MACRAW, but the W5500's PHY does not advertise PAUSE in auto-negotiation and
  its advertisement register is not reachable. A partner that honours PAUSE only
  when it was negotiated — the tested USB adapter — ignores them.
- **Half-duplex backpressure was not reached.** The idea is to force 100 Mbit
  half duplex and occupy the wire with own frames when RX memory runs full. In
  ESP-IDF the speed/duplex ioctls require the driver to be stopped, and the mode
  set before `esp_eth_start()` did not survive the PHY's initialisation. The
  link stayed full duplex. Not pursued further: on a half-duplex link both
  directions also share one medium.
- **SCT mode does not help.** SCT is the SPI peripheral's segmented
  transfer: several transactions run from one DMA list, with one interrupt at
  the end. ESP-IDF 5.5.1 does not enable it on the C5; with a backport (the
  C6's code, the same hardware) the chain ran as one SCT per piece. SPI time
  per frame fell from ~154 to ~145 µs, but in an interleaved A/B test
  throughput was the same (57.0 vs 56.3 Mbit/s, run-to-run spread ±3) and
  burst loss unchanged. The per-transaction cost was already hidden behind
  the delivery work on the CPU, and burst loss is set by RX memory size and
  the raw SPI rate. Leaving SCT mode also exposed an ESP-IDF bug: the master
  driver's ISR trigger is cleared, and the next interrupt-driven transaction
  never starts.
- **Raising the driver task above the Wi-Fi task** gained only noise-level
  improvements (bursts of 40: 6.1 instead of 7.2 lost). It needs care: a busy
  wait in the driver task then starves Wi-Fi.
- **SPI mode 0 or 3 makes no difference:** both sample on the rising edge.
  Stronger pin drive does not help the MISO timing either, because MISO is
  driven by the W5500.

## Constraints when applying this elsewhere

- While transactions are queued on the device, no polled transaction may be
  issued (ESP-IDF rule), and no other user of the device may pick up results.
  Hold the bus lock for the chain, and make every path that could touch the SPI
  bus check for a chain in flight. Here that is the TX pump called from inside
  the receive callback.
- Delivering frames during the chain means calling into the stack with the bus
  lock held. If the stack can wait for something only the TX side can free, that
  is a deadlock. In a bridge, the Wi-Fi task can be waiting for TX ring room. The
  driver therefore delivers during the chain only while its TX ring is empty and
  no producer is waiting, and it bounds the time.
- Commands written by the chain go to `Sn_CR` without the read-back the polled
  path does. Keep the settle time after the last command (e.g. a SEND) before
  queuing the chain.
- The SPI device queue must hold a whole chain: 3 transactions per piece plus 1.
- Heap: faster reception moves frames into the next stage sooner. Under heavy
  bidirectional load the minimum free heap fell from ~33 KB to 6–14 KB here.
  Budget for it, or cap the chain (8 KB here).

## When this is worth porting

- **Worth it:** the W5500 is used as a MAC (MACRAW: bridges, lwIP-on-host, Linux
  `w5100` in MACRAW mode), the host has DMA and an SPI driver with a transaction
  queue, and the traffic is bursty: cameras sending I-frames, TCP senders with
  TSO, anything at line rate for more than ~10 frames.
- **Not worth it:** the W5500 runs in its own TCP/IP offload mode (the chip
  handles windows and retransmission itself), or the traffic is paced well below
  the SPI rate. The classic loop is simpler and as good there.
- **The minimum** that captures most of the burst gain is the per-piece release
  inside a queued chain; overlapped delivery and carrying the last piece's
  frames add ~5 %. A driver without a transaction queue gets part of it by
  writing `Sn_RX_RD` + RECV after each piece, polled, at the cost of those
  transactions' CPU time.
