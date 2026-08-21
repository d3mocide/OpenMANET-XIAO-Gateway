#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GW_ROLE_RELAY's uplink: the ESP32-S3's own native 2.4GHz radio, in plain
 * STA mode, joining a Pi's already-existing local Wi-Fi AP directly (every
 * OpenMANET mesh point exposes one - "br-ahwlan" in their own docs - so
 * this needs no Pi-side change at all). See design/ROADMAP.md item 8.
 *
 * Deliberately mirrors uplink_halow.h's shape (init/start/netif/connected/
 * link_state/rssi/callback) so app_main.c and web_ui.c can report on
 * whichever uplink is actually active with the same pattern, even though the
 * two are separate implementations against separate SDKs (esp_wifi here,
 * morsemicro/halow there) and are never active on the same node at once -
 * role selection in gw_config_t.role is exactly one or the other. */

/* Same contract as uplink_halow_state_cb_t: raised from the default esp_event
 * loop task ("sys_evt", 2816 bytes of stack in this build), so it must be short
 * and non-blocking. See that typedef in uplink_halow.h and datapath_task() in
 * app_main.c. */
typedef void (*uplink_wifi_state_cb_t)(bool connected, void *ctx);

/* Same shape as uplink_link_state_t in uplink_halow.h (RADIO_FAILED has no
 * equivalent here - the native Wi-Fi radio doesn't have a discrete init step
 * that can fail the way an external SPI radio can, so UNCONFIGURED is this
 * enum's first real state). Kept as its own type rather than sharing
 * uplink_halow.h's: the two uplinks are never active on the same node, and
 * duplicating a 5-value enum costs far less than coupling this file to
 * uplink_halow.h's header. */
typedef enum {
    WIFI_UPLINK_UNCONFIGURED = 0, /* no SSID has ever been provisioned */
    WIFI_UPLINK_DOWN,             /* configured, not connected */
    WIFI_UPLINK_CONNECTING,       /* association/DHCP in progress */
    WIFI_UPLINK_ASSOCIATED,       /* 802.11 associated, DHCP still outstanding.
                                   * This uplink is DHCP-only by design -
                                   * gw_wifi_uplink_config_t carries no
                                   * static-IP fields, unlike the HaLow
                                   * uplink's gw_uplink_config_t, because this
                                   * hop joins a Pi's ordinary AP and that AP
                                   * runs a DHCP server. See ROADMAP.md
                                   * "Known limitations". */
    WIFI_UPLINK_UP,               /* associated and has a usable IP */
} uplink_wifi_link_state_t;

/* One-time radio/netif bring-up: esp_wifi_init() + WIFI_MODE_STA +
 * esp_wifi_set_config(WIFI_IF_STA, ...) + esp_wifi_start(). Does not connect
 * yet. Safe to call even with an unconfigured cfg (no SSID) - mirrors
 * uplink_halow_init()'s reasoning: bring-up should not depend on provisioning
 * having happened first. */
esp_err_t uplink_wifi_init(const gw_wifi_uplink_config_t *cfg);

bool uplink_wifi_is_ready(void);

/* Starts the connect + reconnect/backoff logic. Refuses with
 * ESP_ERR_INVALID_STATE if init failed or no SSID is configured - same
 * reasoning as uplink_halow_start(): a reconnect loop against nothing just
 * generates log noise and holds the radio open for no reason. */
esp_err_t uplink_wifi_start(void);

/* NULL until uplink_wifi_init() has successfully created the netif. */
esp_netif_t *uplink_wifi_get_netif(void);

/* True only once the netif holds a usable IP (DHCP lease, or - if this
 * project ever needs a static-IP option on this hop too - a manually applied
 * address). This is the signal ip_forward_nat_init()/cot_relay_start() gate
 * on, same as uplink_halow_is_connected(). */
bool uplink_wifi_is_connected(void);

uplink_wifi_link_state_t uplink_wifi_get_link_state(void);
const char *uplink_wifi_link_state_name(uplink_wifi_link_state_t state);

/* Last known RSSI of the associated AP in dBm, or INT8_MIN when unknown
 * (not associated). Thin wrapper over esp_wifi_sta_get_ap_info(). */
int8_t uplink_wifi_get_rssi(void);

void uplink_wifi_set_state_callback(uplink_wifi_state_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
