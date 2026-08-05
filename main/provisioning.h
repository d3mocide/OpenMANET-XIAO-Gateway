#pragma once

#include "esp_err.h"
#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes the NVS flash partition. Call once at boot before load/save. */
esp_err_t provisioning_init(void);

/* Fills cfg with the built-in placeholder defaults (DESIGN.md §5.6). The
 * uplink SSID/security in particular are guesses, not confirmed values -
 * see docs/pi_side_reference.md §6 - and must be provisioned for real
 * hardware before the HaLow uplink will associate. */
void provisioning_get_defaults(gw_config_t *cfg);

/* Loads config from NVS into cfg, falling back to provisioning_get_defaults()
 * if nothing has been saved yet or the stored blob is invalid. Always
 * returns ESP_OK; failures degrade to defaults rather than propagate, since
 * "no config yet" is a normal first-boot state. */
esp_err_t provisioning_load(gw_config_t *cfg);

/* Persists cfg to NVS. */
esp_err_t provisioning_save(const gw_config_t *cfg);

/* Shared uplink-security (de)serialization, used by both the console
 * (gwcfg-set-uplink) and the web UI so the two never drift apart. */
gw_security_mode_t provisioning_parse_security(const char *s);
const char *provisioning_security_name(gw_security_mode_t sec);

/* Registers `gwcfg-*` esp_console commands that read/mutate *cfg in place
 * and can persist it via provisioning_save(). cfg must outlive the console. */
esp_err_t provisioning_register_console_commands(gw_config_t *cfg);

/* Brings up a UART REPL console (stdin/stdout) running the registered
 * commands. Blocks nothing - starts a background task. */
esp_err_t provisioning_start_console(void);

#ifdef __cplusplus
}
#endif
