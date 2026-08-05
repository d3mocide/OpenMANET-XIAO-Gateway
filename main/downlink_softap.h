#pragma once

#include "esp_err.h"
#include "esp_netif.h"

#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up the local 2.4GHz SoftAP (esp_wifi, native radio) that phones/
 * tablets/ATAK devices associate to (DESIGN.md §4.2). Starts the AP and its
 * DHCP server; independent of the HaLow uplink so it can be bench-tested
 * on its own (DESIGN.md §8 step 3). */
esp_err_t downlink_softap_init(const gw_softap_config_t *cfg);

/* NULL until downlink_softap_init() has succeeded. */
esp_netif_t *downlink_softap_get_netif(void);

#ifdef __cplusplus
}
#endif
