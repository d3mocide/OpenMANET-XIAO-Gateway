#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif.h"

#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called whenever the HaLow uplink transitions between having a DHCP-leased
 * IP and not (IP_EVENT_STA_GOT_IP/LOST_IP on this netif - deliberately not
 * just 802.11 association, since NAT/CoT relay need a real IP to be useful).
 * Invoked from the default esp_event loop task, not an ISR. */
typedef void (*uplink_halow_state_cb_t)(bool connected, void *ctx);

/* One-time radio/netif bring-up. Safe to call even if the HaLow component
 * integration isn't wired up yet (DESIGN.md build order §8 step 3 expects
 * SoftAP/NAT/CoT relay to be exercisable before HaLow STA works) - on
 * failure this logs and returns an error, it does not abort. */
esp_err_t uplink_halow_init(const gw_uplink_config_t *cfg);

/* Starts the association + reconnect/backoff task. */
esp_err_t uplink_halow_start(void);

/* NULL until uplink_halow_init() has successfully created the netif. */
esp_netif_t *uplink_halow_get_netif(void);

bool uplink_halow_is_connected(void);

void uplink_halow_set_state_callback(uplink_halow_state_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
