#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cot_relay.h"
#include "downlink_halow_ap.h"
#include "downlink_softap.h"
#include "factory_reset.h"
#include "gw_config.h"
#include "ip_forward_nat.h"
#include "log_buffer.h"
#include "provisioning.h"
#include "status_led.h"
#include "task_stats.h"
#include "uplink_halow.h"
#include "uplink_wifi.h"
#include "web_ui.h"

static const char *TAG = "app_main";

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    /* Names, not numbers: this line is read by whoever is holding the board,
     * often through the web UI's log panel rather than a terminal. Values from
     * esp_reset_reason_t (esp_system/include/esp_system.h, v5.5.1 L24-41). */
    switch (reason) {
    case ESP_RST_POWERON:     return "power-on";
    case ESP_RST_EXT:         return "external pin";
    case ESP_RST_SW:          return "software (esp_restart)";
    case ESP_RST_PANIC:       return "PANIC (exception)";
    case ESP_RST_INT_WDT:     return "interrupt watchdog";
    case ESP_RST_TASK_WDT:    return "task watchdog";
    case ESP_RST_WDT:         return "other watchdog";
    case ESP_RST_DEEPSLEEP:   return "deep sleep wake";
    case ESP_RST_BROWNOUT:    return "BROWNOUT (supply sagged)";
    case ESP_RST_SDIO:        return "SDIO";
    case ESP_RST_USB:         return "USB peripheral";
    case ESP_RST_JTAG:        return "JTAG";
    case ESP_RST_EFUSE:       return "efuse error";
    case ESP_RST_PWR_GLITCH:  return "power glitch";
    case ESP_RST_CPU_LOCKUP:  return "CPU lockup (double exception)";
    case ESP_RST_UNKNOWN:
    default:                  return "unknown";
    }
}

/* Says why the last boot ended, in the first lines of the log ring.
 *
 * This exists because the two failure modes seen on real hardware are
 * indistinguishable from outside: a brownout and a firmware panic both restart
 * the node, both wipe the RAM-held log ring, and both leave the LED cycling
 * through its power-on patterns. Worse, the device this runs on may be
 * powered from a wall adapter with no serial cable attached - which is exactly
 * when a reboot loop is hardest to diagnose - so the answer has to reach the
 * web UI's log panel, not just a terminal.
 *
 * esp_reset_reason() is authoritative for *this* boot. The core dump is not:
 * it is simply the most recent panic ever recorded in flash, so it can predate
 * this boot by any amount. A brownout leaves no core dump at all, and would
 * otherwise be reported under whatever stale panic was still sitting in the
 * partition. The two are logged separately, and labelled, for that reason. */
static void log_boot_diagnostics(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_POWERON) {
        ESP_LOGI(TAG, "reset reason: %s", reset_reason_name(reason));
    } else {
        /* Anything other than a clean power-on is worth a warning: on a node
         * that is meant to stay up, it means the last boot ended badly. */
        ESP_LOGW(TAG, "reset reason: %s", reset_reason_name(reason));
    }

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    /* Heap-allocated on espressif's own advice - the summary carries a full
     * register set and backtrace and has no business on this stack
     * (esp_core_dump.h L162-169 shows exactly this pattern). */
    esp_core_dump_summary_t *summary = malloc(sizeof(*summary));
    if (summary == NULL) {
        return;
    }
    if (esp_core_dump_get_summary(summary) == ESP_OK) {
        ESP_LOGW(TAG, "a core dump is stored in flash - most recent panic was in task '%s' "
                      "at PC 0x%08" PRIx32 " (may predate this boot)",
                 summary->exc_task, summary->exc_pc);
        for (uint32_t i = 0; i < summary->exc_bt_info.depth && i < 16; i++) {
            ESP_LOGW(TAG, "  backtrace[%u]: 0x%08" PRIx32, (unsigned)i, summary->exc_bt_info.bt[i]);
        }
        if (summary->exc_bt_info.corrupted) {
            ESP_LOGW(TAG, "  (backtrace is marked corrupted - treat it as a hint, not proof)");
        }
        ESP_LOGW(TAG, "  run `idf.py coredump-info` for a symbolized version");
    }
    free(summary);
#endif
}

/* Lives for the life of the device, not app_main()'s stack - the uplink
 * state callback fires from a different task after app_main() has
 * returned. */
static gw_config_t s_cfg;
static bool s_datapath_up = false;
static TaskHandle_t s_datapath_task = NULL;

/* What on_uplink_state() pokes to wake datapath_task().
 *
 * A binary semaphore rather than the task notification this used to use, and
 * that choice is what lets datapath_task() exit when its work is done (see the
 * end of that function). A notification has to be addressed to a task handle,
 * so the event-loop task would have to read s_datapath_task and then call
 * xTaskNotifyGive() on it - and there is no way to make "read the handle" and
 * "notify through it" atomic against the task freeing its own TCB in between.
 * The window is small and the crash would be rare, unattended, and blamed on
 * something else.
 *
 * A semaphore has no such window: it is created before the task and never
 * deleted, so a give that lands after the task is gone is simply a give
 * nobody takes. Giving a binary semaphore that is already available returns
 * pdFALSE and does nothing else, so the repeat costs nothing either. */
static SemaphoreHandle_t s_datapath_wake = NULL;

/* Brings up NAT + the CoT relay the first time the uplink (whichever kind -
 * HaLow STA for GW_ROLE_CLIENT, native Wi-Fi STA for GW_ROLE_RELAY) gets an
 * IP. Shared by both roles - datapath_task() below picks the netif pair - so
 * the NAT-direction and error-handling logic exists exactly once.
 *
 * Runs on the datapath task, never on the event loop task; see datapath_task()
 * for why that distinction is load-bearing rather than stylistic.
 *
 * Known v1 limitation: if a later reconnect gets a *different* IP, NAT/CoT
 * relay aren't re-initialized against it - see design/ROADMAP.md for the
 * NAT-vs-route tradeoff this is part of.
 *
 * The "did this already" flag is set only once both pieces actually succeeded.
 * Setting it on entry (as this used to) meant a single transient failure - the
 * downlink netif momentarily without an address, say - permanently disabled
 * the datapath until someone power-cycled the node, because no later
 * reconnect would ever try again. */
static void bring_up_datapath(esp_netif_t *downlink_netif, esp_netif_t *uplink_netif,
                               const gw_cot_config_t *cot)
{
    if (s_datapath_up) {
        return;
    }

    /* NAPT goes on the downlink side, default route on the uplink, and the
     * uplink's DNS server is copied into the downlink's DHCP offers - see
     * ip_forward_nat.h for why that direction is not the intuitive one. For
     * GW_ROLE_RELAY the "downlink" is the HaLow AP rather than the SoftAP,
     * but the same reasoning applies - it's still the side leaf clients sit
     * behind. */
    esp_err_t nat_err = ip_forward_nat_init(downlink_netif, uplink_netif);
    if (nat_err != ESP_OK) {
        ESP_LOGE(TAG, "NAT init failed: %s", esp_err_to_name(nat_err));
    }

    esp_err_t cot_err = cot_relay_start(uplink_netif, downlink_netif, cot);
    if (cot_err == ESP_ERR_INVALID_STATE) {
        cot_err = ESP_OK; /* already running from an earlier attempt */
    } else if (cot_err != ESP_OK) {
        ESP_LOGE(TAG, "CoT relay start failed: %s", esp_err_to_name(cot_err));
    }

    if (nat_err == ESP_OK && cot_err == ESP_OK) {
        s_datapath_up = true;
    } else {
        ESP_LOGW(TAG, "datapath is incomplete - will retry on the next uplink reconnect");
    }
}

/* Why bring_up_datapath() runs on a task of its own instead of directly in the
 * uplink state callback.
 *
 * Both uplink modules raise that callback from inside an esp_event handler, so
 * it runs on the default event loop's task - "sys_evt". That task gets
 * ESP_TASKD_EVENT_STACK bytes of stack, which at v5.5.1 is
 * CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE (2304, the Kconfig default this
 * project doesn't override) + TASK_EXTRA_STACK_SIZE (512, because
 * CONFIG_NEWLIB_NANO_FORMAT is off) = 2816 bytes total, for everything the
 * handler chain does. Sources: esp_event/default_event_loop.c L98-104 names the
 * task and picks the stack macro, esp_system/include/esp_task.h L48-53 defines
 * it, esp_system/Kconfig L219-221 gives the 2304 default.
 *
 * bring_up_datapath() does not fit in what's left of that below the event
 * loop's own frames. It makes a run of esp_netif calls, then opens, binds and
 * configures the CoT relay's socket and spawns a task - and logs as it goes,
 * where each ESP_LOGx costs several hundred bytes of stack through the full
 * (non-nano) newlib vfprintf. On real hardware in GW_ROLE_RELAY that overflowed
 * and panicked the node into a reboot loop the instant the Wi-Fi uplink got its
 * DHCP lease:
 *
 *     I (5325) uplink_wifi: Wi-Fi uplink up (has IP)
 *     E (5325) ip_
 *     ***ERROR*** A stack overflow in task sys_evt has been detected.
 *
 * The truncated tag is the tell: the overflow was caught while ip_forward_nat.c
 * was still formatting that line, so the log ends mid-word.
 *
 * The rule this broke is that esp_event handlers must stay short - the same
 * reason uplink_wifi.c drives esp_wifi_connect() from its own "wifi_reconnect"
 * task rather than calling it from the WIFI_EVENT handler. So the callback now
 * does nothing but give a semaphore, and the work happens here on a stack
 * sized for it.
 *
 * Doing it this way also means a callback can no longer block the event loop:
 * bring_up_datapath() waits on esp_netif's lwIP IPC for every call it makes,
 * and stalling sys_evt stalls delivery of every other event in the system. */
static void datapath_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_datapath_wake, portMAX_DELAY);

        /* Snapshot under the lock for the same reason the role bring-up
         * functions do it: the console and web UI are live by now and can be
         * editing this struct concurrently. */
        provisioning_config_lock();
        gw_node_role_t role = s_cfg.role;
        gw_cot_config_t cot = s_cfg.cot;
        provisioning_config_unlock();

        if (role == GW_ROLE_RELAY) {
            /* Native Wi-Fi STA uplink (to the Pi's local AP), HaLow AP
             * downlink (to leaf XIAOs). */
            bring_up_datapath(downlink_halow_ap_get_netif(), uplink_wifi_get_netif(), &cot);
        } else {
            /* HaLow STA uplink, local SoftAP downlink. */
            bring_up_datapath(downlink_softap_get_netif(), uplink_halow_get_netif(), &cot);
        }

        /* Done means done: give the 4 KB back rather than parking it.
         *
         * Once s_datapath_up is set, every future pass through this loop is
         * bring_up_datapath() returning at its first line - NAT and the CoT
         * relay are both up and are deliberately not re-initialized against a
         * later uplink IP (that limitation is recorded in design/ROADMAP.md
         * under "Known limitations", and is a decision, not an oversight). So
         * the task would otherwise sit on portMAX_DELAY for the life of the
         * device holding a stack sized for the heaviest thing it ever does.
         *
         * 4 KB is worth reclaiming on a part that also runs two Wi-Fi stacks,
         * lwIP with NAT, and an HTTP server. The retry-on-failure path is
         * untouched: if either half failed, s_datapath_up is still false and
         * this task stays alive waiting for the next reconnect to try again -
         * which is exactly the bug the flag was moved here to fix.
         *
         * The handle is cleared first so nothing is left holding a pointer to
         * a TCB the idle task is about to free - on_uplink_state() no longer
         * reads it at all (that is what s_datapath_wake is for), and
         * datapath_task_start() is called exactly once per boot, from the role
         * bring-up path before this task can have finished. `gwcfg-tasks` and
         * /api/tasks will report "datapath" as absent from here on, which
         * task_stats.c already treats as information rather than an error. */
        if (s_datapath_up) {
            ESP_LOGI(TAG, "datapath is up - the bring-up task has finished and is exiting, "
                          "returning its %u-byte stack", (unsigned)GW_STACK_DATAPATH);
            s_datapath_task = NULL;
            vTaskDelete(NULL);
        }
    }
}

/* Runs on the event loop task for both roles - keep it to a wake-up and
 * nothing else. See datapath_task() above for what happens otherwise.
 *
 * A give on a task that is mid-bring-up leaves the semaphore available, so a
 * reconnect that races an in-flight attempt re-runs it rather than being
 * dropped; bring_up_datapath()'s own s_datapath_up guard makes the repeat a
 * no-op once the datapath is actually up, and after that the task has exited
 * and the give goes nowhere at all. */
static void on_uplink_state(bool connected, void *ctx)
{
    (void)ctx;
    if (!connected || s_datapath_wake == NULL) {
        return;
    }
    xSemaphoreGive(s_datapath_wake);
}

/* Started only on the paths that actually register the callback, so a node
 * with no uplink configured (or whose radio failed to init) doesn't hold 4 KB
 * of stack for a task that can never be woken. */
static esp_err_t datapath_task_start(void)
{
    if (s_datapath_task != NULL) {
        return ESP_OK;
    }
    /* Before the task, not after: the task's first act is to wait on this,
     * and on_uplink_state() drops a wake-up on the floor if it doesn't exist
     * yet. */
    if (s_datapath_wake == NULL) {
        s_datapath_wake = xSemaphoreCreateBinary();
        if (s_datapath_wake == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* 4096 matches this project's other long-lived worker tasks
     * (wifi_reconnect, halow_reconnect, cot_relay) and is well clear of what
     * bring_up_datapath() needs. Unlike them, this one is not long-lived - it
     * exits once the datapath is up (see datapath_task()). */
    if (xTaskCreate(datapath_task, "datapath", GW_STACK_DATAPATH, NULL, 5, &s_datapath_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Today's original design: local 2.4GHz SoftAP for phones/ATAK devices,
 * HaLow STA uplink to whatever AP is configured. Unchanged behaviour from
 * before GW_ROLE existed - only pulled into its own function so app_main()
 * can pick between this and bring_up_relay_role(). */
static void bring_up_client_role(gw_config_t *cfg)
{
    /* SoftAP first: it doesn't depend on the uplink and should be usable
     * standalone - it's the natural first hardware test. */
    esp_err_t err = downlink_softap_init(&cfg->softap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP bring-up failed: %s", esp_err_to_name(err));
    }

    /* Same config, second transport: reachable at the SoftAP's IP once
     * connected to it. The SoftAP netif is passed so the server can refuse
     * requests arriving from the mesh over the uplink - these endpoints are
     * unauthenticated. */
    err = web_ui_start(cfg, downlink_softap_get_netif());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "web UI start failed: %s", esp_err_to_name(err));
    }

    /* The console and web UI are already accepting edits by now, so take a
     * consistent snapshot rather than letting the radio read a half-updated
     * struct. */
    provisioning_config_lock();
    gw_uplink_config_t uplink_cfg = cfg->uplink;
    provisioning_config_unlock();

    err = uplink_halow_init(&uplink_cfg);
    if (err != ESP_OK) {
        /* Deliberately *not* followed by uplink_halow_start(). A reconnect
         * loop against a radio that never initialized just spins, and its
         * error log every second buries the line above that says why. The
         * node stays useful in this state: SoftAP, web UI (whose status panel
         * reports "radio failed") and console all still work, which is what
         * you want when the cause is a wiring or BCF mismatch you're about to
         * go and fix. */
        ESP_LOGE(TAG, "HaLow uplink init failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "the radio is unavailable - check the CONFIG_MM_* pin/BCF config against "
                      "the board (design/HARDWARE.md); SoftAP and config UI remain up");
    } else if (!gw_uplink_is_configured(&uplink_cfg)) {
        /* Not an error, and deliberately not followed by uplink_halow_start().
         * A factory-fresh node has nothing to associate with, so the reconnect
         * loop would only generate failures that look like a fault and keep
         * the radio busy while the operator is trying to scan with it. The
         * link state reports "not configured" and the LED gets its own
         * pattern, so this is visible rather than silent. */
        ESP_LOGW(TAG, "no HaLow uplink configured yet - join '%s' and open http://%s/ to scan "
                      "for your gateway's AP, then save and reboot",
                 cfg->softap.ssid, cfg->softap.use_custom_subnet ? cfg->softap.ip : "192.168.4.1");
    } else {
        /* Before the callback is registered, not after: the uplink can report
         * an IP as soon as it's started, and on_uplink_state() silently drops
         * the notification if the task doesn't exist yet. */
        err = datapath_task_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to start the datapath task: %s", esp_err_to_name(err));
        }
        uplink_halow_set_state_callback(on_uplink_state, cfg);
        err = uplink_halow_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to start HaLow reconnect task: %s", esp_err_to_name(err));
        }
    }
}

/* GW_ROLE_RELAY: HaLow AP downlink (so client-role XIAOs have something to
 * associate to - design/PI_SIDE.md item 0), native Wi-Fi STA uplink to the
 * Pi's own local AP. See design/ROADMAP.md item 8 for the full reasoning,
 * including why this hop uses static IPs instead of DHCP. */
static void bring_up_relay_role(gw_config_t *cfg)
{
    /* HaLow AP first, same "usable standalone" reasoning as the SoftAP in
     * the client role - a relay with no uplink yet should still be joinable
     * by leaf XIAOs for bench testing between them. */
    esp_err_t err = downlink_halow_ap_init(&cfg->halow_ap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HaLow AP bring-up failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "the radio is unavailable - check the CONFIG_MM_* pin/BCF config against "
                      "the board (design/HARDWARE.md)");
    }

    /* Reachable from a leaf XIAO already associated to this relay's HaLow AP
     * (or a laptop bench-testing that link directly) - same subnet-restricted
     * trust model the client role uses, just against the HaLow AP netif
     * instead of the SoftAP one, since a relay runs no SoftAP of its own. */
    err = web_ui_start(cfg, downlink_halow_ap_get_netif());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "web UI start failed: %s", esp_err_to_name(err));
    }

    provisioning_config_lock();
    gw_wifi_uplink_config_t wifi_cfg = cfg->wifi_uplink;
    provisioning_config_unlock();

    err = uplink_wifi_init(&wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi uplink init failed: %s", esp_err_to_name(err));
    } else if (!gw_wifi_uplink_is_configured(&wifi_cfg)) {
        ESP_LOGW(TAG, "no Wi-Fi uplink configured yet - associate to this relay's HaLow AP and "
                      "open http://%s/ to set the Pi's local AP as the uplink, then save and reboot",
                 cfg->halow_ap.ip);
    } else {
        /* Same ordering requirement as the client role above. */
        err = datapath_task_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to start the datapath task: %s", esp_err_to_name(err));
        }
        uplink_wifi_set_state_callback(on_uplink_state, cfg);
        err = uplink_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to start Wi-Fi uplink: %s", esp_err_to_name(err));
        }
    }
}

void app_main(void)
{
    /* First, so the ring captures boot logs too - the ones that say whether
     * the radio came up are the ones you most want to read back later. */
    esp_err_t err = log_buffer_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "log buffer init failed: %s", esp_err_to_name(err));
    }

    /* Immediately after the ring exists, so "why did we restart?" is the first
     * thing anyone reads back over /api/log - and so it survives being asked
     * from a phone with no serial cable in sight. */
    log_boot_diagnostics();

    ESP_ERROR_CHECK(provisioning_init());
    provisioning_load(&s_cfg);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Started before the radio: until uplink_halow_init()/uplink_wifi_init()
     * runs, the link state reads as "not up yet", so the LED shows that from
     * the first moment there's power rather than staying dark through
     * bring-up. */
    err = status_led_start(s_cfg.role);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "status LED start failed: %s", esp_err_to_name(err));
    }

    /* The config-recovery escape hatch. Started early and independently of
     * everything below so it still works when the SoftAP/HaLow AP or the
     * radio doesn't - which is exactly when it's needed. */
    err = factory_reset_start(&s_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "factory reset watcher failed to start: %s", esp_err_to_name(err));
    }

    provisioning_register_console_commands(&s_cfg);
    err = provisioning_start_console();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "console start failed: %s", esp_err_to_name(err));
    }

    /* Console is up regardless of role by this point - gwcfg-* always works
     * over USB, which matters most for GW_ROLE_RELAY: it has no SoftAP, so
     * before its Wi-Fi uplink and HaLow AP are both configured the console
     * (or a temporary direct HaLow association to reach the web UI) is the
     * only way in. */
    if (s_cfg.role == GW_ROLE_RELAY) {
        bring_up_relay_role(&s_cfg);
    } else {
        bring_up_client_role(&s_cfg);
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG, "xiao-halow-gateway up: version=%s node_id=%s role=%s",
             desc ? desc->version : "unknown",
             s_cfg.node_id,
             s_cfg.role == GW_ROLE_RELAY ? "relay" : "client");
}
