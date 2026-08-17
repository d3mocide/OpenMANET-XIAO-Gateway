#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stack sizes, in bytes, for the tasks this firmware creates.
 *
 * Named here rather than written as literals at each xTaskCreate() call so the
 * headroom report below can compute a percentage against the size actually in
 * force, instead of against a second copy of the number that drifts away from
 * it. Change a size here and both the task and its report move together.
 *
 * ESP-IDF's xTaskCreate() takes the stack depth in *bytes*, unlike vanilla
 * FreeRTOS which takes words - its port defines StackType_t as uint8_t
 * (freertos/FreeRTOS-Kernel/portable/xtensa/include/freertos/portmacro.h L88,
 * L91 at v5.5.1). These are byte counts. */
#define GW_STACK_DATAPATH        4096
#define GW_STACK_COT_RELAY       4096
#define GW_STACK_WIFI_RECONNECT  4096
#define GW_STACK_HALOW_RECONNECT 4096
#define GW_STACK_FACTORY_RESET   4096
#define GW_STACK_WEB_UI          6144

/* Deliberately the smallest in the firmware: status_led_task() reads an enum
 * and toggles a GPIO. It has no room for a log call, and shouldn't grow one -
 * see design/ROADMAP.md "Stack budgets". */
#define GW_STACK_STATUS_LED      2048

/* One task's stack headroom, as reported by task_stats_each_stack(). */
typedef struct {
    const char *name;      /* FreeRTOS task name that was queried */
    bool present;          /* false = not running right now; the fields below are then meaningless */
    size_t stack_total;    /* bytes the task was created with */
    size_t stack_free_min; /* bytes still free at the task's deepest point so far */
} task_stack_info_t;

typedef void (*task_stack_cb_t)(const task_stack_info_t *info, void *ctx);

/* Reports the worst-case stack headroom of every task this firmware cares
 * about - its own, the ESP-IDF ones whose budget it depends on, and the HaLow
 * SDK's event loop. Invokes `cb` once per task in the table, including tasks
 * that aren't currently running (with .present = false), so the caller can
 * show "not running" rather than silently omitting a row. Returns how many
 * were actually found.
 *
 * This exists because a stack overflow is invisible until it isn't: the node
 * ran for weeks looking healthy, then panicked into a reboot loop the moment a
 * handler got two frames deeper (design/ROADMAP.md item 8). The high-water
 * mark is the only way to see how much margin is really left rather than
 * guessing at it, and it has to be readable from the web UI as well as the
 * console, because the failure it predicts happens on nodes with no cable
 * attached.
 *
 * Call on demand, not in a loop: it resolves each name with xTaskGetHandle(),
 * which walks every task list in the scheduler, and measures each mark by
 * scanning the task's stack for the fill pattern. Neither is O(1). */
size_t task_stats_each_stack(task_stack_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
