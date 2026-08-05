#pragma once

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sets up address translation so SoftAP clients reach the mesh through the
 * HaLow uplink (DESIGN.md §4.3, v1 NAT-for-general-traffic recommendation).
 *
 * NAPT is enabled on the *downlink* (SoftAP) netif and the *uplink* netif is
 * made the default route. That direction is deliberate and matches ESP-IDF's
 * own softap_sta NAT-router example: esp_netif's NAPT control sets the napt
 * flag on precisely the netif it's given (clearing it on all others, so only
 * one interface can have it), and the AP-side interface is the one that must
 * carry it. Enabling it on the uplink instead yields a gateway that forwards
 * client traffic with untranslated private source addresses.
 *
 * Plain IP forwarding between the two netifs is handled by lwIP itself once
 * CONFIG_LWIP_IP_FORWARD is set (sdkconfig.defaults); this call only adds the
 * NAT/NAPT layer plus default-route selection on top. Call once the uplink
 * holds a DHCP lease - esp_netif refuses to enable NAPT on a netif that
 * isn't up. */
esp_err_t ip_forward_nat_init(esp_netif_t *downlink_netif, esp_netif_t *uplink_netif);

#ifdef __cplusplus
}
#endif
