#include <stdbool.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "cot_relay.h"
#include "downlink_softap.h"
#include "gw_config.h"
#include "ip_forward_nat.h"
#include "provisioning.h"
#include "uplink_halow.h"
#include "web_ui.h"

static const char *TAG = "app_main";

/* Lives for the life of the device, not app_main()'s stack - the uplink
 * state callback fires from a different task after app_main() has
 * returned. */
static gw_config_t s_cfg;
static bool s_uplink_ever_connected = false;

/* Brings up NAT + the CoT relay the first time the HaLow uplink gets an IP.
 * Known v1 limitation: if a later reconnect gets a *different* IP, NAT/CoT
 * relay aren't re-initialized against it - see DESIGN.md §4.3 for the
 * NAT-vs-route tradeoff this is part of. */
static void on_uplink_state(bool connected, void *ctx)
{
    gw_config_t *cfg = (gw_config_t *)ctx;

    if (!connected || s_uplink_ever_connected) {
        return;
    }
    s_uplink_ever_connected = true;

    esp_netif_t *uplink_netif = uplink_halow_get_netif();
    esp_netif_t *softap_netif = downlink_softap_get_netif();

    esp_err_t err = ip_forward_nat_init(uplink_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NAT init failed: %s", esp_err_to_name(err));
    }

    err = cot_relay_start(uplink_netif, softap_netif, &cfg->cot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CoT relay start failed: %s", esp_err_to_name(err));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(provisioning_init());
    provisioning_load(&s_cfg);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* SoftAP first: it doesn't depend on the uplink and should be usable
     * standalone (DESIGN.md §8 build order, step 3). */
    esp_err_t err = downlink_softap_init(&s_cfg.softap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP bring-up failed: %s", esp_err_to_name(err));
    }

    provisioning_register_console_commands(&s_cfg);
    err = provisioning_start_console();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "console start failed: %s", esp_err_to_name(err));
    }

    /* Same config, second transport: reachable at the SoftAP's IP once
     * connected to it (DESIGN.md §5.6). */
    err = web_ui_start(&s_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "web UI start failed: %s", esp_err_to_name(err));
    }

    err = uplink_halow_init(&s_cfg.uplink);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HaLow uplink init failed: %s", esp_err_to_name(err));
    }
    uplink_halow_set_state_callback(on_uplink_state, &s_cfg);
    err = uplink_halow_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start HaLow reconnect task: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "xiao-halow-gateway up: node_id=%s", s_cfg.node_id);
}
