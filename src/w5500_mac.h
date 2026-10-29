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

/* Largest single SPI transfer the driver issues (a batch of received
 * frames). Pass it as max_transfer_sz to spi_bus_initialize(): the bus
 * default (4092 bytes with DMA) is smaller. With a smaller bus limit the
 * driver still works, just with smaller batches. */
#define W5500_MAC_MAX_TRANSFER 6144

/* Create the MAC instance. Takes the same configuration as
 * esp_eth_mac_new_w5500(), except that int_gpio_num is mandatory
 * (poll_period_ms must be 0) and custom SPI drivers are not supported.
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
} w5500_mac_stats_t;

/* Copy the counters of 'mac' (created by w5500_mac_new()) into 'out'. */
void w5500_mac_get_stats(esp_eth_mac_t *mac, w5500_mac_stats_t *out);

#ifdef __cplusplus
}
#endif
