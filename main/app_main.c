#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

#include "esp_core_dump.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"

#include "cot_relay.h"
#include "downlink_softap.h"
#include "factory_reset.h"
#include "gw_config.h"
#include "ip_forward_nat.h"
#include "log_buffer.h"
#include "provisioning.h"
#include "status_led.h"
#include "uplink_halow.h"
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

/* Brings up NAT + the CoT relay the first time the HaLow uplink gets an IP.
 *
 * Known v1 limitation: if a later reconnect gets a *different* IP, NAT/CoT
 * relay aren't re-initialized against it - see design/ROADMAP.md for the
 * NAT-vs-route tradeoff this is part of.
 *
 * The "did this already" flag is set only once both pieces actually succeeded.
 * Setting it on entry (as this used to) meant a single transient failure - the
 * SoftAP netif momentarily without an address, say - permanently disabled the
 * datapath until someone power-cycled the node, because no later reconnect
 * would ever try again. */
static void on_uplink_state(bool connected, void *ctx)
{
    gw_config_t *cfg = (gw_config_t *)ctx;

    if (!connected || s_datapath_up) {
        return;
    }

    esp_netif_t *uplink_netif = uplink_halow_get_netif();
    esp_netif_t *softap_netif = downlink_softap_get_netif();

    /* NAPT goes on the SoftAP side, default route on the uplink, and the
     * uplink's DNS server is copied into the SoftAP's DHCP offers - see
     * ip_forward_nat.h for why that direction is not the intuitive one. */
    esp_err_t nat_err = ip_forward_nat_init(softap_netif, uplink_netif);
    if (nat_err != ESP_OK) {
        ESP_LOGE(TAG, "NAT init failed: %s", esp_err_to_name(nat_err));
    }

    provisioning_config_lock();
    gw_cot_config_t cot = cfg->cot;
    provisioning_config_unlock();

    esp_err_t cot_err = cot_relay_start(uplink_netif, softap_netif, &cot);
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

    /* Started before the radio: until uplink_halow_init() runs, the link state
     * reads as RADIO_FAILED, so the LED shows "not up yet" from the first
     * moment there's power rather than staying dark through bring-up. */
    err = status_led_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "status LED start failed: %s", esp_err_to_name(err));
    }

    /* The config-recovery escape hatch. Started early and independently of
     * everything below so it still works when the SoftAP or the radio doesn't
     * - which is exactly when it's needed. */
    err = factory_reset_start(&s_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "factory reset watcher failed to start: %s", esp_err_to_name(err));
    }

    /* SoftAP first: it doesn't depend on the uplink and should be usable
     * standalone - it's the natural first hardware test. */
    err = downlink_softap_init(&s_cfg.softap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP bring-up failed: %s", esp_err_to_name(err));
    }

    provisioning_register_console_commands(&s_cfg);
    err = provisioning_start_console();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "console start failed: %s", esp_err_to_name(err));
    }

    /* Same config, second transport: reachable at the SoftAP's IP once
     * connected to it. The SoftAP netif is passed so the
     * server can refuse requests arriving from the mesh over the uplink -
     * these endpoints are unauthenticated. */
    err = web_ui_start(&s_cfg, downlink_softap_get_netif());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "web UI start failed: %s", esp_err_to_name(err));
    }

    /* The console and web UI are already accepting edits by now, so take a
     * consistent snapshot rather than letting the radio read a half-updated
     * struct. */
    provisioning_config_lock();
    gw_uplink_config_t uplink_cfg = s_cfg.uplink;
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
                 s_cfg.softap.ssid, s_cfg.softap.use_custom_subnet ? s_cfg.softap.ip : "192.168.4.1");
    } else {
        uplink_halow_set_state_callback(on_uplink_state, &s_cfg);
        err = uplink_halow_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to start HaLow reconnect task: %s", esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "xiao-halow-gateway up: node_id=%s", s_cfg.node_id);
}
