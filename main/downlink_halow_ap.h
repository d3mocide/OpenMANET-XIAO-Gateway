#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GW_ROLE_RELAY's downlink: the HaLow radio in AP mode, so client-role XIAOs
 * have something to associate to when the Pi itself can't run a HaLow AP -
 * see design/PI_SIDE.md item 0 and design/ROADMAP.md item 8.
 *
 * Real, not a stub - esp-halow's CONFIG_HALOW_AP_MODE builds genuine
 * wpa_supplicant/hostapd AP code (hostapd.c, beacon.c, ap_mlme.c) against the
 * MM6108, confirmed by reading the component's own CMakeLists.txt. But it's
 * also marked "ALPHA NOTICE: under development; breaking changes may be
 * introduced" in mmwlan.h's own mmwlan_ap_args/mmwlan_ap_enable() docs
 * (v2.11.2-esp32-2), and untested on real S3+MM6108 hardware by this project
 * or, as far as this repo's research found, by Morse Micro's own examples -
 * their "softap" example is the only one that exercises it, and it has no
 * counterpart proving it against a real client. Budget bring-up time for
 * this like any other untested path (design/HARDWARE.md's runbook pattern).
 *
 * No DHCP server. mmhalow_init() creates its netif as
 * ESP_NETIF_DEFAULT_WIFI_STA() unconditionally, regardless of whether the
 * caller wants STA or AP mode (confirmed by reading mmhalow.c directly) -
 * that shape carries ESP_NETIF_DHCP_CLIENT, not ESP_NETIF_DHCP_SERVER, and
 * esp_netif only allocates the actual DHCP server object
 * (esp_netif->dhcps, via dhcps_new()) when ESP_NETIF_DHCP_SERVER was set at
 * esp_netif_new() time (esp_netif_lwip.c) - which never happens here, and
 * there is no public API to retrofit it afterward. So this AP gets one fixed
 * static address (gw_halow_ap_config_t.ip) instead, and every leaf XIAO that
 * associates to it needs its own static IP configured on its own uplink
 * (gw_uplink_config_t.use_static_ip) rather than DHCP. See
 * design/ROADMAP.md item 8 for the reasoning and the alternative (a
 * hand-rolled DHCP server) this deliberately avoided for a first cut. */

/* One-time radio/netif bring-up: mmhalow_init() + mmhalow_set_config(WIFI_IF_AP, ...)
 * + a static IP on the resulting netif + mmhalow_wifi_start(). Like
 * uplink_halow_init(), safe to call with an unconfigured cfg (no SSID) - the
 * radio still comes up, just without starting the AP, so gwcfg-radio and a
 * future gwcfg-list-halow-channels still work standalone. */
esp_err_t downlink_halow_ap_init(const gw_halow_ap_config_t *cfg);

bool downlink_halow_ap_is_ready(void);

/* NULL until downlink_halow_ap_init() has succeeded. */
esp_netif_t *downlink_halow_ap_get_netif(void);

/* Best-effort only: mmhalow_wifi_start() returns void (confirmed in
 * mmhalow.h - "void mmhalow_wifi_start()"), so there is no error code to
 * report here. True once this code has called it; not proof the AP actually
 * came up on air. */
bool downlink_halow_ap_is_started(void);

/* One HaLow channel from the regulatory domain table CONFIG_HALOW_COUNTRY_CODE
 * already loaded (mmwlan_lookup_regulatory_domain() against get_regulatory_db()
 * - the same table mmhalow_init() points mmwlan_set_channel_list() at, so
 * anything listed here is legal to transmit on). Exists because
 * mmwlan_ap_args wants an (op_class, s1g_chan_num) pair that has no obvious
 * mapping to the MHz figures an operator actually has - use this to let them
 * pick a channel by frequency instead of guessing raw op-class numbers. */
typedef struct {
    int16_t op_class;
    uint8_t s1g_chan_num;
    uint32_t freq_hz;
    uint8_t bw_mhz;
} halow_ap_channel_t;

typedef void (*halow_ap_channel_cb_t)(const halow_ap_channel_t *chan, void *ctx);

/* Invokes cb once per channel in this build's regulatory domain. Synchronous,
 * no radio activity involved (pure table walk) - safe to call before
 * downlink_halow_ap_init(), unlike uplink_halow_scan() which needs the radio
 * up first. Returns the number of channels found. */
size_t downlink_halow_ap_list_channels(halow_ap_channel_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
