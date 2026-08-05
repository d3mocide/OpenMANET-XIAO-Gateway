#pragma once

#include <stdbool.h>
#include <stdint.h>

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

/* Where the uplink actually is, as opposed to the binary "usable / not usable"
 * the state callback reports.
 *
 * The distinction matters during bring-up: association (step 1) and the DHCP
 * lease (step 2) are separate milestones with completely different failure
 * causes - a stuck ASSOCIATING points at country code,
 * security mode, SSID or RF; a stuck ASSOCIATED points at DHCP on the Pi. A
 * single "connected" flag collapses those two into one indistinguishable
 * "it doesn't work". */
typedef enum {
    UPLINK_LINK_RADIO_FAILED = 0, /* uplink_halow_init() failed - no radio at all */
    UPLINK_LINK_DOWN,             /* radio up, not associated */
    UPLINK_LINK_ASSOCIATING,      /* association in progress */
    UPLINK_LINK_ASSOCIATED,       /* 802.11 associated, still no DHCP lease */
    UPLINK_LINK_UP,               /* associated + leased: the datapath is usable */
} uplink_link_state_t;

/* One flattened scan hit. Deliberately not `struct mmwlan_scan_result`: that
 * one is full of pointers into the driver's receive buffer which are only
 * valid inside the callback, so it can't be stored or handed to the web UI. */
typedef struct {
    char ssid[GW_SSID_MAX_LEN + 1];
    uint8_t bssid[6];
    int16_t rssi;
    uint32_t freq_hz;
    uint8_t bw_mhz;
} uplink_scan_result_t;

/* Invoked once per discovered AP, from the driver's scan task. Keep it short.
 * `result` is owned by the caller and only valid for the duration of the call. */
typedef void (*uplink_scan_cb_t)(const uplink_scan_result_t *result, void *ctx);

/* One-time radio/netif bring-up. Safe to call even if the HaLow component
 * integration isn't wired up yet (the bring-up runbook expects
 * SoftAP/NAT/CoT relay to be exercisable before HaLow STA works) - on
 * failure this logs and returns an error, it does not abort. Check
 * uplink_halow_is_ready() before calling anything else here. */
esp_err_t uplink_halow_init(const gw_uplink_config_t *cfg);

/* True once uplink_halow_init() has completed successfully. Everything below
 * except uplink_halow_get_link_state() is meaningless when this is false. */
bool uplink_halow_is_ready(void);

/* Starts the association + reconnect/backoff task. Refuses with
 * ESP_ERR_INVALID_STATE if init failed - a reconnect loop against a radio that
 * never initialized just spins, and its error spam buries the one log line
 * that says what actually went wrong. */
esp_err_t uplink_halow_start(void);

/* NULL until uplink_halow_init() has successfully created the netif. */
esp_netif_t *uplink_halow_get_netif(void);

/* True only in UPLINK_LINK_UP - i.e. associated *and* holding a DHCP lease.
 * This is the signal NAT and the CoT relay gate on. */
bool uplink_halow_is_connected(void);

uplink_link_state_t uplink_halow_get_link_state(void);
const char *uplink_halow_link_state_name(uplink_link_state_t state);

/* Last known RSSI of the associated AP in dBm, or INT32_MIN when unknown
 * (not associated, or the radio hasn't heard from the AP yet). Thin wrapper
 * over mmwlan_get_rssi().
 *
 * The single most useful number during bring-up and antenna placement: it
 * separates "the link is configured wrong" from "the link is configured right
 * but too weak", which otherwise look identical. */
int32_t uplink_halow_get_rssi(void);

/* Runs one scan and invokes cb for each AP found, then returns once the scan
 * completes or timeout_ms elapses. Blocks the calling task.
 *
 * This answers the question nothing else in this firmware can: "is the Pi's
 * HaLow AP visible at all, on what channel, at what strength?" - which is
 * bring-up steps 1-2 in design/HARDWARE.md, and previously required a
 * Pi-side capture to answer.
 *
 * Note it scans the channel list derived from CONFIG_HALOW_COUNTRY_CODE. An AP
 * on a channel outside this build's regulatory domain will not be found, so an
 * empty result is itself evidence about the country-code question rather than
 * proof the AP is absent. Only one scan runs at a time; concurrent callers get
 * ESP_ERR_INVALID_STATE. */
esp_err_t uplink_halow_scan(uplink_scan_cb_t cb, void *ctx, uint32_t timeout_ms);

/* Logs the radio's BCF, firmware and morselib versions via the component's own
 * mmhalow_print_version_info(). Getting output here proves host<->MM6108 SPI
 * communication works, which is the cheapest way to tell a wiring/BCF problem
 * apart from a regulatory or credentials problem - the two present identically
 * as "it never associates". */
void uplink_halow_log_radio_info(void);

void uplink_halow_set_state_callback(uplink_halow_state_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
