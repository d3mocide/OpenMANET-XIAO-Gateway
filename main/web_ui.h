#pragma once

#include "esp_err.h"
#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the on-device HTTP config UI, served over the SoftAP (DESIGN.md
 * §5.6's "small onboard config portal" option). *cfg is edited in place and
 * persisted through the same provisioning_save() the gwcfg-* console
 * commands use - both are just different transports onto the same
 * gw_config_t. cfg must outlive the server (device lifetime). */
esp_err_t web_ui_start(gw_config_t *cfg);

#ifdef __cplusplus
}
#endif
