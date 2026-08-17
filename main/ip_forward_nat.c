#include "ip_forward_nat.h"

#include "esp_log.h"

static const char *TAG = "ip_forward_nat";

/* Value of `OFFER_DNS` from lwIP's `enum dhcps_offer_option`
 * (components/lwip/include/apps/dhcpserver/dhcpserver.h in ESP-IDF v5.5.1).
 * That header is a private lwIP-app header, so ESP-IDF's own softap_sta
 * example re-declares the constant locally rather than including it
 * (examples/wifi/softap_sta/main/softap_sta.c defines DHCPS_OFFER_DNS 0x02);
 * we follow the same convention. */
#define GW_DHCPS_OFFER_DNS 0x02

/* Hands SoftAP clients a DNS server in their DHCP lease.
 *
 * This is not optional garnish - without it the SoftAP's DHCP server offers no
 * DNS option at all, because lwIP's dhcpserver initializes `dhcps->dhcps_dns`
 * to 0x00 (dhcpserver.c:172 at v5.5.1) and only emits the option when it's
 * been explicitly enabled (dhcpserver.c:466, `if (dhcps_dns_enabled(...))`).
 * Clients would get an address and a default route but no resolver, so every
 * hostname lookup fails while raw IP still works - which during bring-up looks
 * exactly like "NAT is broken". Android additionally flags such a network as
 * having no internet access and may fall back to cellular.
 *
 * The address handed out is whatever the *uplink* learned over DHCP from the
 * Pi, which is reachable from clients through the NAPT we enable below.
 *
 * Mirrors softap_set_dns_addr() in ESP-IDF's softap_sta example, which is the
 * reference implementation of this whole three-step recipe (DNS, default
 * route, NAPT); this firmware previously did steps two and three only. */
static esp_err_t propagate_dns(esp_netif_t *downlink_netif, esp_netif_t *uplink_netif)
{
    esp_netif_dns_info_t dns;
    esp_err_t err = esp_netif_get_dns_info(uplink_netif, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "couldn't read uplink DNS server: %s", esp_err_to_name(err));
        return err;
    }

    /* The Pi's DHCP server isn't obliged to send a DNS option. Offering
     * 0.0.0.0 to clients is worse than offering nothing - some stacks accept
     * it and then black-hole every lookup - so say so loudly and leave the
     * DHCP server's DNS option disabled instead. */
    if (dns.ip.type != ESP_IPADDR_TYPE_V4 || dns.ip.u_addr.ip4.addr == 0) {
        ESP_LOGW(TAG, "uplink DHCP lease carried no DNS server - local clients will have no "
                      "name resolution (check the Pi's DHCP config)");
        return ESP_ERR_NOT_FOUND;
    }

    /* The DHCP server has to be stopped to change its options; clients already
     * holding a lease keep their old (DNS-less) one until they renew, so a
     * phone that joined before the uplink came up needs to rejoin. */
    err = esp_netif_dhcps_stop(downlink_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "dhcps_stop: %s", esp_err_to_name(err));
    }

    uint8_t offer_dns = GW_DHCPS_OFFER_DNS;
    err = esp_netif_dhcps_option(downlink_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                 &offer_dns, sizeof(offer_dns));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "couldn't enable the DHCP DNS option: %s", esp_err_to_name(err));
    } else {
        err = esp_netif_set_dns_info(downlink_netif, ESP_NETIF_DNS_MAIN, &dns);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "couldn't set the offered DNS server: %s", esp_err_to_name(err));
        }
    }

    /* Restart the server regardless - leaving it stopped would strand every
     * future client without an address at all, which is strictly worse than
     * the missing-DNS problem this function exists to fix. */
    esp_err_t start_err = esp_netif_dhcps_start(downlink_netif);
    if (start_err != ESP_OK && start_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGE(TAG, "dhcps_start after DNS change: %s", esp_err_to_name(start_err));
        return start_err;
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SoftAP clients will be offered DNS " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
    }
    return err;
}

esp_err_t ip_forward_nat_init(esp_netif_t *downlink_netif, esp_netif_t *uplink_netif)
{
    if (downlink_netif == NULL || uplink_netif == NULL) {
        ESP_LOGW(TAG, "missing netif (downlink=%p uplink=%p), skipping NAPT enable",
                 downlink_netif, uplink_netif);
        return ESP_ERR_INVALID_ARG;
    }

    /* Step 1 of the recipe, and only meaningful when the downlink actually runs
     * a DHCP server to put the option in.
     *
     * In GW_ROLE_CLIENT it does: the SoftAP netif is created DHCP-server
     * shaped. In GW_ROLE_RELAY the downlink is the HaLow netif, which
     * mmhalow_init() creates from ESP_NETIF_DEFAULT_WIFI_STA() - DHCP *client*
     * shaped - and leaf nodes on that hop are statically addressed by design
     * (design/ROADMAP.md item 8). esp_netif allocates its `dhcps` handle only
     * for ESP_NETIF_DHCP_SERVER netifs (esp_netif_lwip.c L836-838 at v5.5.1),
     * so every dhcps call against the HaLow netif can only fail:
     * esp_netif_dhcps_option() short-circuits to ESP_ERR_ESP_NETIF_IF_NOT_READY
     * on the NULL handle (L2582), and esp_netif_dhcps_start() reaches
     * dhcps_start(NULL, ...), which returns ERR_ARG (dhcpserver.c L1362) and
     * costs an ESP_LOGE "DHCP server cannot be started" from inside esp_netif
     * (L1699-1700).
     *
     * Neither failure breaks anything - but they print as two errors on every
     * relay boot, describing a DHCP server that was never supposed to exist,
     * which is exactly the noise that buries a real fault. So check the netif's
     * flags first rather than calling and discarding the failure.
     *
     * Deliberately not fatal when it does run: a gateway with working NAT and
     * no DNS is still useful (ATAK over the CoT relay doesn't resolve names),
     * and propagate_dns()'s own warning tells the operator what's missing. */
    if (esp_netif_get_flags(downlink_netif) & ESP_NETIF_DHCP_SERVER) {
        (void)propagate_dns(downlink_netif, uplink_netif);
    } else {
        ESP_LOGI(TAG, "downlink runs no DHCP server - skipping DNS propagation (nodes on this "
                      "hop are statically addressed)");
    }

    /* Step 2. The uplink must be the default route netif so translated traffic
     * actually egresses toward the mesh - ESP-IDF's softap_sta example
     * README states the recipe as "NAPT enabled on the softAP interface and
     * the station interface set as the default interface". esp_netif would
     * normally elect the STA netif anyway on route_prio, but the HaLow netif
     * is created by a third-party component, so state it explicitly rather
     * than depend on that. */
    esp_err_t err = esp_netif_set_default_netif(uplink_netif);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "couldn't set uplink as default netif: %s", esp_err_to_name(err));
        /* Not fatal on its own - NAPT below is still worth enabling. */
    }

    /* Step 3. NAPT goes on the *downlink* (SoftAP) netif, not the uplink. This
     * is counter-intuitive but is what esp_netif implements: esp_netif_lwip.c's
     * napt control sets `napt = 1` on exactly the netif handed to it (and
     * clears it on every other netif, so only one can have it at a time),
     * and both ESP-IDF's softap_sta example and its README enable it on the
     * AP interface. Passing the uplink here instead silently produces a
     * gateway that forwards SoftAP client packets out the HaLow radio with
     * untranslated 172.16.x source addresses. */
    err = esp_netif_napt_enable(downlink_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_napt_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "NAPT enabled on the SoftAP interface, uplink is default route");
    return ESP_OK;
}
