#pragma once

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sets up address translation so SoftAP clients reach the mesh through the
 * HaLow uplink. See design/ROADMAP.md "Settled decisions" for why v1 NATs
 * general traffic and relays CoT multicast separately.
 *
 * Three things happen here, and all three are required for a client behind the
 * SoftAP to have working connectivity:
 *
 *   1. the DNS server the uplink learned over DHCP is copied into the SoftAP's
 *      DHCP server so local clients are offered a resolver (lwIP's DHCP server
 *      offers none by default - see propagate_dns() in the .c for the exact
 *      upstream citation);
 *   2. the uplink is made the default route;
 *   3. NAPT is enabled on the downlink.
 *
 * Steps 2 and 3 alone produce a gateway where raw IP works and every hostname
 * lookup fails - which presents as "NAT is broken".
 *
 * Step 1 is skipped when the downlink netif runs no DHCP server, which is the
 * case in GW_ROLE_RELAY (the HaLow netif is DHCP-client shaped and leaf nodes
 * are statically addressed). That is not the degraded case described above -
 * there is simply no DHCP offer to attach a DNS option to. Steps 2 and 3 are
 * unconditional.
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
 * holds a DHCP lease *and* the downlink netif is up - esp_netif refuses to
 * enable NAPT on a netif that isn't (esp-idf esp_netif_lwip.c L2695/2701-2706
 * at v5.5.1: ip_napt_enable_netif() "fails only if netif is down"). The
 * uplink getting its lease doesn't imply the downlink is up too - in
 * GW_ROLE_RELAY they're two independent radios and the uplink can win that
 * race - so app_main.c's bring_up_datapath() waits on the downlink netif
 * before ever calling this. */
esp_err_t ip_forward_nat_init(esp_netif_t *downlink_netif, esp_netif_t *uplink_netif);

#ifdef __cplusplus
}
#endif
