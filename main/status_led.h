#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Drives the XIAO's on-board user LED (board.h) as a link-state indicator.
 *
 * This is the only diagnostic available when the node is powered from a
 * battery in a pocket or on a mast: no serial cable, and no phone associated
 * to read the web UI from. It answers "how far did it get?" at a glance,
 * which during bring-up is most of the question.
 *
 * Patterns, in ascending order of good news:
 *
 *   fast triple-blink, repeating   radio failed to initialize (wiring/BCF/pins)
 *   slow blink (1 Hz)              radio up, searching / not associated
 *   double-blink                   associated, waiting for a DHCP lease
 *   solid on                       uplink up - associated and leased
 *
 * The pattern is derived from uplink_halow_get_link_state(), so it stays
 * correct without anything having to remember to update it.
 */
esp_err_t status_led_start(void);

/* Overrides the link-state pattern with a solid-on/steady-fast-blink
 * acknowledgement, used by the factory-reset hold so the operator gets
 * feedback before the reboot. Pass false to hand control back. */
void status_led_set_attention(bool on);

#ifdef __cplusplus
}
#endif
