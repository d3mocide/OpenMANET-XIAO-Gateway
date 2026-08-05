#pragma once

#include "esp_err.h"
#include "esp_netif.h"
#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the on-device HTTP config UI, served over the SoftAP (DESIGN.md
 * §5.6's "small onboard config portal" option). *cfg is edited in place and
 * persisted through the same provisioning_save() the gwcfg-* console
 * commands use - both are just different transports onto the same
 * gw_config_t. cfg must outlive the server (device lifetime).
 *
 * softap_netif is required, and not just for convenience: esp_http_server
 * binds all interfaces, so once the HaLow uplink is up these endpoints would
 * be reachable from the whole mesh. This UI has no authentication, so every
 * handler refuses requests whose peer address is outside softap_netif's
 * subnet - association with the local SoftAP is the authorization boundary.
 * Passing NULL is an error rather than a permissive default. */
esp_err_t web_ui_start(gw_config_t *cfg, esp_netif_t *softap_netif);

#ifdef __cplusplus
}
#endif
