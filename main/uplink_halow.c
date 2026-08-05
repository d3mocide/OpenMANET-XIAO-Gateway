#include <string.h>

#include "uplink_halow.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "uplink_halow";

#define RECONNECT_BACKOFF_MIN_MS 1000
#define RECONNECT_BACKOFF_MAX_MS 60000
#define HALOW_CONNECT_TIMEOUT_MS 15000
#define LINK_POLL_INTERVAL_MS    2000

static gw_uplink_config_t s_cfg;
static esp_netif_t *s_netif = NULL;
static volatile bool s_connected = false;
static uplink_halow_state_cb_t s_cb = NULL;
static void *s_cb_ctx = NULL;
static TaskHandle_t s_reconnect_task_handle = NULL;

/* -----------------------------------------------------------------------
 * Morse Micro HaLow component integration point.
 *
 * UNVERIFIED: this environment had no ESP-IDF toolchain available, so the
 * exact morsemicro/halow API surface (function/struct names, whether
 * "connect" is blocking or event-driven, etc.) has not been checked
 * against real component headers - see main/idf_component.yml. These three
 * functions are the *only* place that integration needs to happen; they
 * are deliberately left as clean-compiling stubs (returning
 * ESP_ERR_NOT_SUPPORTED / false) rather than guessed calls against a
 * header that may not match, so the rest of the gateway (SoftAP, NAT, CoT
 * relay) can be built and bench-tested without HaLow hardware.
 *
 * Expected shape once wired up, per the component's published docs
 * (https://components.espressif.com/components/morsemicro/halow),
 * to be confirmed against managed_components/morsemicro__halow/include
 * after `idf.py build` fetches it:
 *
 *   #include "mmhalow.h"
 *   ESP_ERROR_CHECK(mmhalow_init(WIFI_INIT_CONFIG_DEFAULT()));
 *   mmhalow_sta_config_t sta_cfg = {
 *       .ssid = cfg->ssid,
 *       .password = cfg->psk,
 *       .security_type = halow_security_from_gw(cfg->security),
 *       .channel = cfg->channel,
 *   };
 *   ESP_ERROR_CHECK(mmhalow_set_config(&sta_cfg));
 *   esp_netif_t *netif = mmhalow_netif_create_default_sta(); // name unconfirmed
 *   mmhalow_connect(NULL);
 * ---------------------------------------------------------------------*/

static esp_err_t halow_sta_bringup(const gw_uplink_config_t *cfg, esp_netif_t **out_netif)
{
    (void)cfg;
    ESP_LOGW(TAG, "halow_sta_bringup() is a stub - HaLow radio/netif integration not wired up yet");
    *out_netif = NULL;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t halow_sta_connect(TickType_t timeout_ticks)
{
    (void)timeout_ticks;
    return ESP_ERR_NOT_SUPPORTED;
}

static bool halow_sta_link_up(void)
{
    return false;
}

/* ---------------------------------------------------------------------*/

static void set_connected(bool connected)
{
    if (s_connected == connected) {
        return;
    }
    s_connected = connected;
    ESP_LOGI(TAG, "HaLow uplink %s", connected ? "up" : "down");
    if (s_cb) {
        s_cb(connected, s_cb_ctx);
    }
}

static void reconnect_task(void *arg)
{
    (void)arg;
    uint32_t backoff_ms = RECONNECT_BACKOFF_MIN_MS;

    for (;;) {
        esp_err_t err = halow_sta_connect(pdMS_TO_TICKS(HALOW_CONNECT_TIMEOUT_MS));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HaLow STA connect failed (%s), retrying in %u ms",
                     esp_err_to_name(err), (unsigned)backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = (backoff_ms * 2 > RECONNECT_BACKOFF_MAX_MS) ? RECONNECT_BACKOFF_MAX_MS : backoff_ms * 2;
            continue;
        }

        backoff_ms = RECONNECT_BACKOFF_MIN_MS;
        set_connected(true);

        while (halow_sta_link_up()) {
            vTaskDelay(pdMS_TO_TICKS(LINK_POLL_INTERVAL_MS));
        }

        ESP_LOGW(TAG, "HaLow uplink dropped, will reassociate");
        set_connected(false);
    }
}

esp_err_t uplink_halow_init(const gw_uplink_config_t *cfg)
{
    memcpy(&s_cfg, cfg, sizeof(s_cfg));
    return halow_sta_bringup(&s_cfg, &s_netif);
}

esp_err_t uplink_halow_start(void)
{
    if (s_reconnect_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ok = xTaskCreate(reconnect_task, "halow_reconnect", 4096, NULL, 5, &s_reconnect_task_handle);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_netif_t *uplink_halow_get_netif(void)
{
    return s_netif;
}

bool uplink_halow_is_connected(void)
{
    return s_connected;
}

void uplink_halow_set_state_callback(uplink_halow_state_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
}
