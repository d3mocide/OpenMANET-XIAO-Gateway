#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "log_buffer.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* 6KB of the ~300KB free heap. Enough for a few hundred lines, which covers
 * boot plus a couple of reconnect cycles - the window that actually explains a
 * bring-up failure. */
#define LOG_RING_SIZE 6144

/* Bound on one formatted log line. Longer lines are truncated into the ring
 * (they still reach the console in full). */
#define LOG_LINE_MAX 256

static char s_ring[LOG_RING_SIZE];
static size_t s_head = 0;  /* next write position */
static bool s_wrapped = false;
static SemaphoreHandle_t s_lock = NULL;
static vprintf_like_t s_next = NULL;

static void ring_append(const char *data, size_t len)
{
    if (len >= LOG_RING_SIZE) {
        /* Keep only the tail - it's the most recent. */
        data += len - (LOG_RING_SIZE - 1);
        len = LOG_RING_SIZE - 1;
    }

    size_t first = LOG_RING_SIZE - s_head;
    if (first > len) {
        first = len;
    }
    memcpy(s_ring + s_head, data, first);
    if (len > first) {
        memcpy(s_ring, data + first, len - first);
        s_wrapped = true;
    }
    s_head = (s_head + len) % LOG_RING_SIZE;
}

static int log_vprintf(const char *fmt, va_list args)
{
    /* Format once for the console via the original handler. The va_list is
     * consumed by that call, so the copy for the ring has to be taken first. */
    va_list ring_args;
    va_copy(ring_args, args);

    int written = s_next ? s_next(fmt, args) : vprintf(fmt, args);

    char line[LOG_LINE_MAX];
    int n = vsnprintf(line, sizeof(line), fmt, ring_args);
    va_end(ring_args);

    if (n > 0) {
        size_t len = ((size_t)n < sizeof(line)) ? (size_t)n : sizeof(line) - 1;
        /* Never block a logging call on the lock: logging happens from every
         * task including time-sensitive ones, and dropping a line from the
         * in-RAM copy is far cheaper than stalling the caller. The console
         * output above has already happened either way.
         *
         * Nothing in here logs, which is what keeps this from recursing back
         * into itself through esp_log. */
        if (s_lock != NULL && xSemaphoreTake(s_lock, 0) == pdTRUE) {
            ring_append(line, len);
            xSemaphoreGive(s_lock);
        }
    }

    return written;
}

esp_err_t log_buffer_init(void)
{
    if (s_lock != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* esp_log_set_vprintf returns the previous handler; chaining to it rather
     * than to vprintf() directly keeps whatever else may be installed working
     * (and keeps the serial console fed). */
    s_next = esp_log_set_vprintf(log_vprintf);
    return ESP_OK;
}

size_t log_buffer_read(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }

    size_t available = s_wrapped ? LOG_RING_SIZE : s_head;
    size_t start = s_wrapped ? s_head : 0; /* oldest byte */

    /* Drop the oldest if the caller's buffer is smaller than what we hold. */
    if (available > out_size - 1) {
        size_t drop = available - (out_size - 1);
        start = (start + drop) % LOG_RING_SIZE;
        available -= drop;
    }

    size_t first = LOG_RING_SIZE - start;
    if (first > available) {
        first = available;
    }
    memcpy(out, s_ring + start, first);
    if (available > first) {
        memcpy(out + first, s_ring, available - first);
    }
    out[available] = '\0';

    xSemaphoreGive(s_lock);
    return available;
}

size_t log_buffer_capacity(void)
{
    return LOG_RING_SIZE;
}
