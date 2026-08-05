#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes the NVS flash partition and the live-config mutex. Call once at
 * boot, before load/save and before any task that touches the config runs. */
esp_err_t provisioning_init(void);

/* Serializes access to the live gw_config_t shared by app_main, the console
 * REPL, and the httpd task. Hold across any read-modify-write of it; the
 * lock is recursive-free, so don't nest. Safe to call before
 * provisioning_init() (no-ops until the mutex exists). */
void provisioning_config_lock(void);
void provisioning_config_unlock(void);

/* Rejects a config that esp_wifi/lwIP would refuse at bring-up, or that
 * would take down the SoftAP the device is managed over (e.g. a WPA2
 * passphrase outside 8-63 chars, an out-of-range channel, a non-multicast
 * CoT group). Returns ESP_OK if usable, otherwise ESP_ERR_INVALID_ARG with a
 * human-readable explanation written to errbuf. Called automatically by
 * provisioning_save() and provisioning_load(); call it directly when you
 * want the reason string to show the user. */
esp_err_t provisioning_validate(const gw_config_t *cfg, char *errbuf, size_t errbuf_len);

/* Checks a config against *current* runtime state, which provisioning_validate()
 * deliberately knows nothing about. Today that means one thing: rejecting a
 * local subnet that overlaps the mesh address the uplink currently holds,
 * which would make the gateway treat mesh addresses as on-link and silently
 * stop forwarding to them.
 *
 * uplink_ip_be is the uplink's address in network byte order, or 0 if there
 * is no lease yet (in which case this passes - there is nothing to compare
 * against). Separate from provisioning_validate() so the NVS load path, which
 * runs before any interface is up, doesn't have to reason about liveness. */
esp_err_t provisioning_check_runtime_conflict(const gw_config_t *cfg, uint32_t uplink_ip_be,
                                              char *errbuf, size_t errbuf_len);

/* Fills cfg with the built-in placeholder defaults (DESIGN.md §5.6). The
 * uplink SSID/security in particular are guesses, not confirmed values -
 * see design/pi_side_reference.md §6 - and must be provisioned for real
 * hardware before the HaLow uplink will associate. */
void provisioning_get_defaults(gw_config_t *cfg);

/* Loads config from NVS into cfg, falling back to provisioning_get_defaults()
 * if nothing has been saved yet or the stored blob is invalid. Always
 * returns ESP_OK; failures degrade to defaults rather than propagate, since
 * "no config yet" is a normal first-boot state. */
esp_err_t provisioning_load(gw_config_t *cfg);

/* Persists cfg to NVS, stamping it with the current magic/version. Validates
 * first and returns ESP_ERR_INVALID_ARG without writing if the config is
 * unusable, so a bad value can't be made permanent. */
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
