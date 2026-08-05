#pragma once

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Enables NAPT on the HaLow uplink netif so traffic sourced from the local
 * SoftAP clients gets translated on its way out (DESIGN.md §4.3, v1
 * NAT-for-general-traffic recommendation). IP forwarding between the two
 * netifs itself is handled automatically by lwIP once CONFIG_LWIP_IP_FORWARD
 * is enabled (sdkconfig.defaults) - this call only adds the NAT/NAPT layer
 * on top, and per esp_netif's implementation can only be enabled on a single
 * interface (the one facing the upstream network). */
esp_err_t ip_forward_nat_init(esp_netif_t *uplink_netif);

#ifdef __cplusplus
}
#endif
