#include <inttypes.h>
#include <string.h>

#include "uplink_wifi.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static const char *TAG = "uplink_wifi";

#define RECONNECT_BACKOFF_MIN_MS 1000
#define RECONNECT_BACKOFF_MAX_MS 60000

static gw_wifi_uplink_config_t s_cfg;
static esp_netif_t *s_netif = NULL;
static bool s_ready = false;
static bool s_configured = false;
static volatile bool s_associated = false;
static volatile bool s_associating = false;
static volatile bool s_has_ip = false;
static uplink_wifi_state_cb_t s_cb = NULL;
static void *s_cb_ctx = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;
static uint32_t s_backoff_ms = RECONNECT_BACKOFF_MIN_MS;

static void set_has_ip(bool has_ip)
{
    if (s_has_ip == has_ip) {
        return;
    }
    s_has_ip = has_ip;
    ESP_LOGI(TAG, "Wi-Fi uplink %s", has_ip ? "up (has IP)" : "down");
    if (s_cb) {
        s_cb(has_ip, s_cb_ctx);
    }
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    /* esp_wifi_connect() while already connecting/connected is harmless (it
     * just re-issues the request), but skip it once associated - identical
     * reasoning to halow_sta_connect()'s citation in uplink_halow.c: tearing
     * down a working association to "retry" it would be self-defeating. */
    if (s_associated) {
        return;
    }
    s_associating = true;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        s_associating = false;
    }
}

static void schedule_reconnect(void)
{
    if (s_reconnect_timer == NULL) {
        return;
    }
    esp_timer_stop(s_reconnect_timer); /* no-op if not running */
    esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_ms * 1000ULL);
    s_backoff_ms = (s_backoff_ms * 2 > RECONNECT_BACKOFF_MAX_MS) ? RECONNECT_BACKOFF_MAX_MS : s_backoff_ms * 2;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            s_backoff_ms = RECONNECT_BACKOFF_MIN_MS;
            s_associating = true;
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            s_associated = true;
            s_associating = false;
            s_backoff_ms = RECONNECT_BACKOFF_MIN_MS;
            ESP_LOGI(TAG, "Wi-Fi uplink associated (RSSI %" PRId8 " dBm), waiting for DHCP lease...",
                     uplink_wifi_get_rssi());
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_associated = false;
            s_associating = false;
            set_has_ip(false);
            ESP_LOGW(TAG, "Wi-Fi uplink disconnected, retrying in %u ms", (unsigned)s_backoff_ms);
            schedule_reconnect();
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
            if (event->esp_netif == s_netif) {
                set_has_ip(true);
            }
        } else if (id == IP_EVENT_STA_LOST_IP) {
            set_has_ip(false);
        }
    }
}

esp_err_t uplink_wifi_init(const gw_wifi_uplink_config_t *cfg)
{
    memcpy(&s_cfg, cfg, sizeof(s_cfg));
    s_configured = gw_wifi_uplink_is_configured(&s_cfg);

    /* Owns esp_wifi's global init + mode entirely, unlike downlink_softap.c -
     * a GW_ROLE_RELAY node never runs downlink_softap_init(), so there is no
     * second caller of esp_wifi_init()/esp_wifi_set_mode() to conflict with.
     * Plain STA mode, not APSTA: a relay has no local phone-facing SoftAP of
     * its own (see gw_config.h's GW_ROLE_RELAY comment), so there is nothing
     * on this radio for an AP role to share, and no channel-locking concern
     * that WIFI_MODE_APSTA would otherwise raise. */
    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name = "wifi_uplink_reconnect",
    };
    err = esp_timer_create(&timer_args, &s_reconnect_timer);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    if (s_configured) {
        wifi_config_t wifi_config = { 0 };
        strlcpy((char *)wifi_config.sta.ssid, s_cfg.ssid, sizeof(wifi_config.sta.ssid));
        strlcpy((char *)wifi_config.sta.password, s_cfg.psk, sizeof(wifi_config.sta.password));
        /* WIFI_AUTH_WPA2_PSK as a floor, not an exact match: esp_wifi accepts
         * whatever the AP actually negotiates at or above threshold.authmode,
         * same latitude downlink_softap.c leaves itself on the AP side. An
         * empty password here would be rejected by esp_wifi at connect time
         * if the AP isn't open - validated ahead of that in
         * provisioning_validate() instead, so the failure is reported where
         * the operator is looking, not buried in a wifi_event_handler log. */
        wifi_config.sta.threshold.authmode = strlen(s_cfg.psk) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    s_ready = true;
    return ESP_OK;
}

bool uplink_wifi_is_ready(void)
{
    return s_ready;
}

esp_err_t uplink_wifi_start(void)
{
    if (!s_ready) {
        ESP_LOGE(TAG, "not starting - Wi-Fi uplink radio never initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_configured) {
        ESP_LOGW(TAG, "not starting - no Wi-Fi uplink SSID is configured");
        return ESP_ERR_INVALID_STATE;
    }
    /* esp_wifi_start() (already called in uplink_wifi_init()) fires
     * WIFI_EVENT_STA_START, whose handler calls esp_wifi_connect() - the
     * connect attempt is already in flight by the time this returns. Nothing
     * further to start here; this function exists so app_main.c's call
     * pattern matches uplink_halow_start()'s (init succeeds regardless of
     * configuration, start() is the point that's refused when unconfigured). */
    return ESP_OK;
}

esp_netif_t *uplink_wifi_get_netif(void)
{
    return s_netif;
}

bool uplink_wifi_is_connected(void)
{
    return s_has_ip;
}

uplink_wifi_link_state_t uplink_wifi_get_link_state(void)
{
    if (!s_ready || !s_configured) {
        return WIFI_UPLINK_UNCONFIGURED;
    }
    if (s_has_ip) {
        return WIFI_UPLINK_UP;
    }
    if (s_associated) {
        return WIFI_UPLINK_ASSOCIATED;
    }
    if (s_associating) {
        return WIFI_UPLINK_CONNECTING;
    }
    return WIFI_UPLINK_DOWN;
}

const char *uplink_wifi_link_state_name(uplink_wifi_link_state_t state)
{
    switch (state) {
    case WIFI_UPLINK_UNCONFIGURED: return "not configured";
    case WIFI_UPLINK_DOWN:         return "searching";
    case WIFI_UPLINK_CONNECTING:   return "connecting";
    case WIFI_UPLINK_ASSOCIATED:   return "associated, no lease";
    case WIFI_UPLINK_UP:           return "up";
    default:                       return "unknown";
    }
}

int8_t uplink_wifi_get_rssi(void)
{
    if (!s_associated) {
        return INT8_MIN;
    }
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return INT8_MIN;
    }
    return ap_info.rssi;
}

void uplink_wifi_set_state_callback(uplink_wifi_state_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
}
