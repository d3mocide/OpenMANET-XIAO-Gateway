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

/* WPA2-PSK passphrase bounds, fixed by the 802.11i spec and enforced by
 * esp_wifi at AP start - a SoftAP configured outside this range fails to
 * come up, which on this device would take the primary management path
 * (the SoftAP itself) down with it. Validated before save, not at boot. */
#define GW_WPA2_PSK_MIN_LEN 8
#define GW_WPA2_PSK_MAX_LEN 63

/* 2.4GHz SoftAP channel range. 14 is excluded deliberately: it's Japan-only
 * and 802.11b-only, and esp_wifi rejects it under most country settings. */
#define GW_SOFTAP_CHANNEL_MIN 1
#define GW_SOFTAP_CHANNEL_MAX 13

/* Stamped into every persisted blob so a layout change is detected rather
 * than silently reinterpreted. Bump GW_CONFIG_VERSION whenever the shape or
 * meaning of anything below changes; a mismatch makes provisioning_load()
 * fall back to defaults and say so, instead of loading garbage into a
 * deployed unit. */
#define GW_CONFIG_MAGIC   0x4F4D4757u /* "OMGW" */
/* v2: default SoftAP subnet moved from 192.168.50.0/24 to 172.16.50.0/24.
 * The struct layout is unchanged, so the bump is not strictly required to read
 * a v1 blob - it is deliberate, so that any unit already provisioned on a
 * bench picks up the new default instead of silently staying on a subnet that
 * collides with the phone-hotspot and home-router space. A v1 blob is
 * discarded with a log line and the unit falls back to defaults; reprovision
 * it via the web UI or `gwcfg-*`. */
#define GW_CONFIG_VERSION 2u

/* Security modes the HaLow uplink STA can be configured for. Must match
 * whatever the associated Pi's HaLow AP is running (design/PI_SIDE.md) -
 * unknown at scaffold time, so this is provisioned, not hardcoded.
 *
 * NOTE: unlike legacy 2.4GHz Wi-Fi, HaLow (802.11ah) has no WPA2-PSK mode -
 * confirmed against the real morsemicro/halow SDK (enum mmwlan_security_type
 * in mmwlan.h only defines MMWLAN_OPEN / MMWLAN_OWE / MMWLAN_SAE). The
 * design doc's original "open / WPA2-PSK / WPA3-SAE" (§4.1) was wrong on
 * this point; corrected here and in design/PI_SIDE.md. */
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
     * override at runtime. See design/PI_SIDE.md "Still to verify" item 3. */
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
    /* Must stay first and must not change type - provisioning_load() reads
     * these before trusting anything after them. */
    uint32_t magic;   /* GW_CONFIG_MAGIC */
    uint32_t version; /* GW_CONFIG_VERSION */

    char node_id[GW_NODE_ID_MAX_LEN + 1];
    gw_uplink_config_t uplink;
    gw_softap_config_t softap;
    gw_cot_config_t cot;
} gw_config_t;

#ifdef __cplusplus
}
#endif
