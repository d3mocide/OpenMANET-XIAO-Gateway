#include "task_stats.h"

#include "esp_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Console REPL default from ESP_CONSOLE_REPL_CONFIG_DEFAULT()
 * (console/esp_console.h L64-73 at v5.5.1, `.task_stack_size = 4096`).
 * provisioning_start_console() takes that macro's value unmodified. */
#define CONSOLE_REPL_STACK 4096

/* morselib's umac event loop asks mmosal_task_create() for 2152 32-bit *words*
 * (morselib/src/umac/core/umac_evtloop.c L110), and the ESP32 shim converts to
 * the bytes ESP-IDF's xTaskCreate() wants (`stack_size_u32 * 4`,
 * components/shims/mmosal_shim_freertos_esp32.c L216-221) - so this is 8608
 * bytes, not 2152. Worth listing even though the SDK owns it: this is the task
 * that invokes mm_sta_state_cb() and scan_rx_cb(), so web_ui.c's cJSON scan
 * collector is charged here rather than to httpd. */
#define HALOW_EVTLOOP_STACK (2152 * 4)

/* The tasks worth watching, and what each was created with.
 *
 * Ordered by how tight the budget is rather than alphabetically, so the row
 * that matters is the first one read. */
static const struct {
    const char *name;
    size_t total;
} s_watched[] = {
    /* First, because it has the smallest budget in the system and runs every
     * esp_event handler - including ones from esp_netif, esp_wifi and mmhalow
     * that this project doesn't own and can't shorten. It is also the task
     * that has already overflowed once (design/ROADMAP.md item 8), so it is
     * the number anyone opening this report came to see.
     *
     * ESP_TASKD_EVENT_STACK rather than a literal: it resolves to
     * CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE + TASK_EXTRA_STACK_SIZE
     * (esp_system/include/esp_task.h L48-53), so raising the Kconfig value
     * moves this report with it. */
    { "sys_evt",         ESP_TASKD_EVENT_STACK },
    { "status_led",      GW_STACK_STATUS_LED },
    { "esp_timer",       ESP_TASK_TIMER_STACK },
    { "tiT",             ESP_TASK_TCPIP_STACK },
    { "console_repl",    CONSOLE_REPL_STACK },
    { "datapath",        GW_STACK_DATAPATH },
    { "cot_relay",       GW_STACK_COT_RELAY },
    { "wifi_reconnect",  GW_STACK_WIFI_RECONNECT },
    { "halow_reconnect", GW_STACK_HALOW_RECONNECT },
    { "factory_reset",   GW_STACK_FACTORY_RESET },
    { "httpd",           GW_STACK_WEB_UI },
    { "evtloop",         HALOW_EVTLOOP_STACK },
};

size_t task_stats_each_stack(task_stack_cb_t cb, void *ctx)
{
    size_t found = 0;

    for (size_t i = 0; i < sizeof(s_watched) / sizeof(s_watched[0]); i++) {
        task_stack_info_t info = {
            .name = s_watched[i].name,
            .present = false,
            .stack_total = s_watched[i].total,
            .stack_free_min = 0,
        };

        /* Several of these are legitimately absent depending on role and
         * state: halow_reconnect only exists in GW_ROLE_CLIENT, wifi_reconnect
         * only in GW_ROLE_RELAY, cot_relay only once the datapath is up, and
         * datapath only when an uplink was configured at boot. NULL here is
         * information, not an error - report the row as absent. */
        TaskHandle_t handle = xTaskGetHandle(s_watched[i].name);
        if (handle != NULL) {
            /* Bytes, not words.
             *
             * Vanilla FreeRTOS documents uxTaskGetStackHighWaterMark() as
             * returning a count of StackType_t words, and its implementation
             * does divide by sizeof(StackType_t) (FreeRTOS-Kernel/tasks.c
             * L4807-4820, prvTaskCheckFreeStackSpace). But ESP-IDF's Xtensa
             * port defines portSTACK_TYPE - and therefore StackType_t - as
             * uint8_t (portable/xtensa/include/freertos/portmacro.h L88, L91),
             * so that divisor is 1 and the value comes back in bytes. Same
             * reason xTaskCreate() takes its stack depth in bytes here.
             *
             * Reading the upstream FreeRTOS docs instead and multiplying by 4
             * would report four times the headroom that actually exists, which
             * on a task already known to overflow is the worst possible
             * direction to be wrong in. */
            info.present = true;
            info.stack_free_min = (size_t)uxTaskGetStackHighWaterMark(handle);
            found++;
        }

        if (cb != NULL) {
            cb(&info, ctx);
        }
    }

    return found;
}
