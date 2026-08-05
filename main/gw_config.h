#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_SSID_MAX_LEN     32
#define GW_PSK_MAX_LEN      64
#define GW_NODE_ID_MAX_LEN  32
#define GW_IP4_STR_MAX_LEN  16 /* "255.255.255.255" + NUL */

/* Security modes the HaLow uplink STA can be configured for. Must match
 * whatever the associated Pi's HaLow AP is running (DESIGN.md §4.1/§6.3) -
 * unknown at scaffold time, so this is provisioned, not hardcoded.
 *
 * NOTE: unlike legacy 2.4GHz Wi-Fi, HaLow (802.11ah) has no WPA2-PSK mode -
 * confirmed against the real morsemicro/halow SDK (enum mmwlan_security_type
 * in mmwlan.h only defines MMWLAN_OPEN / MMWLAN_OWE / MMWLAN_SAE). The
 * design doc's original "open / WPA2-PSK / WPA3-SAE" (§4.1) was wrong on
 * this point; corrected here and in docs/pi_side_reference.md. */
typedef enum {
    GW_SECURITY_OPEN = 0,
    GW_SECURITY_OWE,
    GW_SECURITY_SAE,
} gw_security_mode_t;

typedef struct {
    char ssid[GW_SSID_MAX_LEN + 1];
    char psk[GW_PSK_MAX_LEN + 1]; /* only used when security == GW_SECURITY_SAE */
    gw_security_mode_t security;
    /* No per-connection channel selection in the real API - the HaLow STA
     * scans within the regulatory channel list derived from
     * CONFIG_HALOW_COUNTRY_CODE, which is a *build-time* Kconfig value
     * (sdkconfig.defaults), not something this NVS-provisioned struct can
     * override at runtime. See docs/pi_side_reference.md open item 3. */
} gw_uplink_config_t;

typedef struct {
    char ssid[GW_SSID_MAX_LEN + 1];
    char psk[GW_PSK_MAX_LEN + 1]; /* empty => open AP */
    uint8_t channel;
    uint8_t max_connections;

    /* If false, use esp_netif's default SoftAP subnet (192.168.4.1/24).
     * If true, apply ip/gateway/netmask below instead. */
    bool use_custom_subnet;
    char ip[GW_IP4_STR_MAX_LEN];
    char gateway[GW_IP4_STR_MAX_LEN];
    char netmask[GW_IP4_STR_MAX_LEN];
} gw_softap_config_t;

typedef struct {
    char group[GW_IP4_STR_MAX_LEN]; /* multicast group, e.g. "239.2.3.1" */
    uint16_t port;                  /* e.g. 6969 */
} gw_cot_config_t;

typedef struct {
    char node_id[GW_NODE_ID_MAX_LEN + 1];
    gw_uplink_config_t uplink;
    gw_softap_config_t softap;
    gw_cot_config_t cot;
} gw_config_t;

#ifdef __cplusplus
}
#endif
