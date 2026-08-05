#pragma once

#include "esp_err.h"

#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Watches the XIAO's BOOT button (board.h) and, on a sustained hold, restores
 * the built-in default config and reboots.
 *
 * Why this exists: every other way of recovering a mis-provisioned node needs
 * something the field doesn't have. The web UI needs the SoftAP to be up with
 * a passphrase you still know; the `gwcfg-*` console needs a USB cable and a
 * terminal. provisioning_validate() blocks the obvious ways to lock yourself
 * out, but not all of them - a valid-but-wrong SoftAP passphrase, saved and
 * rebooted, is unrecoverable without this.
 *
 * A physical button is also the right trust model for a reset: it requires
 * physical presence, which is exactly the property design/ROADMAP.md settles
 * on for credential recovery.
 *
 * The hold is deliberately long, and the LED acknowledges it partway through,
 * so an accidental press can't wipe a node's config and a deliberate one gives
 * feedback before it commits.
 *
 * cfg is the live in-RAM config, updated in place before the reboot so nothing
 * else can write a stale copy back over the defaults in between. */
esp_err_t factory_reset_start(gw_config_t *cfg);

#ifdef __cplusplus
}
#endif
