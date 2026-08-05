#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Keeps the most recent ESP_LOG output in RAM so it can be read back over
 * HTTP.
 *
 * The coredump partition covers crashes. This covers the other case, which
 * during bring-up is the more common one: the node is running, something is
 * wrong, and the logs that would say what are scrolling past a serial port
 * nobody is attached to - because the node is on a mast, in a pack, or simply
 * the other side of the field from the laptop.
 *
 * Installed as an esp_log_set_vprintf() hook, so it captures everything
 * already being logged without any call site changing. Output still goes to
 * the console as well; this is a tee, not a redirect.
 */
esp_err_t log_buffer_init(void);

/* Copies the buffered log, oldest first, into out as a NUL-terminated string.
 * Returns the number of bytes written excluding the terminator. If the buffer
 * holds more than out_size-1 bytes, the oldest are dropped - the recent lines
 * are the interesting ones. */
size_t log_buffer_read(char *out, size_t out_size);

/* Capacity of the ring, so callers can size their read buffer to match. */
size_t log_buffer_capacity(void);

#ifdef __cplusplus
}
#endif
