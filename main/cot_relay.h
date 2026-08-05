#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_netif.h"

#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the ATAK CoT multicast relay (DESIGN.md §4.3/§5.4): joins the
 * configured group/port on both netif_a and netif_b via a single socket,
 * and bidirectionally forwards datagrams between them. Uses IP_PKTINFO to
 * determine which interface a datagram actually arrived on rather than
 * running two independently-bound sockets, which would risk both sockets
 * seeing the same inbound packet and forwarding it back onto its own
 * origin interface. Both netifs must already have a valid IP (call this
 * once the HaLow uplink is connected, not at cold boot). */
esp_err_t cot_relay_start(esp_netif_t *netif_a, esp_netif_t *netif_b, const gw_cot_config_t *cfg);

/* Sends data to the CoT group on both interfaces as if it originated
 * locally - the generic send primitive DESIGN.md §5.5 calls for, so a
 * future self-beacon (GPS/battery/status) can reuse this path instead of
 * needing separate code. Not yet called anywhere; no self-beacon exists
 * yet. */
esp_err_t cot_relay_inject(const void *data, size_t len);

#ifdef __cplusplus
}
#endif
