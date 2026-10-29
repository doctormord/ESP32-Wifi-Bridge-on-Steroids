/*
 * w5500_mac.h - W5500 MAC driver tuned for bridging
 *
 * A replacement for ESP-IDF's esp_eth_mac_new_w5500() with the same
 * esp_eth_mac_t interface, so it plugs into esp_eth_driver_install() together
 * with IDF's own W5500 PHY driver. The IDF driver handles one frame at a time
 * and waits for every SEND to complete inside transmit(), which is what the
 * Wi-Fi RX path calls; this one is built for throughput instead:
 *
 *   - transmit() only copies the frame into a byte ring and returns. The
 *     driver task moves the frames to the chip, writing the next frame into
 *     the W5500's TX memory while the previous one is still on the wire.
 *   - Reception reads as many queued frames as fit into one SPI transfer.
 *   - Large transfers are interrupt-driven, so the CPU (single core on the
 *     ESP32-C5) is free for the Wi-Fi stack while the SPI DMA runs.
 *
 * Interrupt mode only: the W5500 INT line must be wired.
 */
#pragma once

#include "esp_eth_mac_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Largest RX batch the driver reads per round: the whole 16 KB socket RX
 * memory. Pass it as max_transfer_sz to spi_bus_initialize(): the bus
 * default (4092 bytes with DMA) is smaller. With a smaller bus limit the
 * driver still works, just with smaller batches. */
#define W5500_MAC_MAX_TRANSFER 8192

/* What to pass as max_transfer_sz instead where the SPI master supports
 * segmented transfers (SCT): the bus sizes its DMA descriptor pool from it
 * (one per 4092 bytes), and an RX chain in SCT mode needs a descriptor per
 * segment on top of the data - about 20 for a W5500_MAC_MAX_TRANSFER chain.
 * The descriptors cost 24 bytes each; transfers stay capped at
 * W5500_MAC_MAX_TRANSFER by the driver. */
#define W5500_MAC_BUS_MAX_TRANSFER (24 * 4092)

/* Create the MAC instance. Takes the same configuration as
 * esp_eth_mac_new_w5500(), except that int_gpio_num is mandatory
 * (poll_period_ms must be 0) and custom SPI drivers are not supported.
 * The SPI device may be half-duplex (SPI_DEVICE_HALFDUPLEX in
 * spi_devcfg->flags): the W5500 protocol never sends and receives data in
 * the same transaction, and only in half-duplex mode does the SPI driver
 * apply input_delay_ns as dummy clocks before a read - the way to read
 * reliably at clocks where MISO arrives a clock late.
 * Returns NULL on invalid arguments or out of memory. */
esp_eth_mac_t *w5500_mac_new(const eth_w5500_config_t *w5500_config,
                             const eth_mac_config_t *mac_config);

/* Driver counters for the status API, all since boot: first the frames the
 * driver had to give up on, then activity counters for finding where the
 * time per frame goes (compare two readings taken under load). The
 * microsecond totals wrap after ~71 minutes; use differences. */
typedef struct {
  uint32_t tx_ring_full;   /* transmit() found no room in the TX ring     */
  uint32_t tx_timeout;     /* a SEND did not complete in time             */
  uint32_t rx_bad_len;     /* corrupt frame header, RX buffer resynced    */
  uint32_t rx_no_mem;      /* no heap for a received frame                */
  uint32_t rx_stall;       /* RX round found only an incomplete frame (not
                            * a drop - the frame is taken on a later round) */
  uint32_t rx_frames;      /* frames delivered to esp_eth                 */
  uint32_t rx_batches;     /* RX rounds that delivered at least one frame */
  uint32_t tx_frames;      /* frames handed to the chip with SEND         */
  uint32_t wakes;          /* driver-task rounds                          */
  uint32_t spi_us;         /* time spent in SPI transactions              */
  uint32_t deliver_us;     /* time spent handing frames to esp_eth, i.e.
                            * in the bridge's Ethernet -> Wi-Fi callback  */
  uint32_t tx_waits;       /* transmit() found the ring full and waited for
                            * room (not a drop unless tx_ring_full counts) */
  uint32_t tx_done_skipped; /* SEND_OK taken as done from the wire time,
                             * without reading the chip                 */
} w5500_mac_stats_t;

/* Move queued TX frames to the chip now. Only has an effect when called
 * from within the driver's own receive path (the esp_eth input callback):
 * a callback that has to wait there - e.g. for a Wi-Fi TX buffer - calls it
 * meanwhile, so the TX ring keeps draining. Otherwise the Wi-Fi task, held
 * in transmit() waiting for ring room, could not free the very buffer the
 * callback waits for. Returns false when the driver is in the middle of a
 * pipelined RX read (it cannot touch the SPI bus) and a producer is already
 * waiting for ring room: the caller must then stop waiting for Wi-Fi,
 * because nothing drains the ring meanwhile. */
bool w5500_mac_pump_tx(esp_eth_mac_t *mac);

/* Copy the counters of 'mac' (created by w5500_mac_new()) into 'out'. */
void w5500_mac_get_stats(esp_eth_mac_t *mac, w5500_mac_stats_t *out);

/* Whether the RX chain of 'mac' runs in the SPI master's segmented transfer
 * mode (SCT) - see W5500_MAC_BUS_MAX_TRANSFER. */
bool w5500_mac_uses_sct(esp_eth_mac_t *mac);

#ifdef __cplusplus
}
#endif
