#include <inttypes.h>
#include <string.h>

#include "uplink_wifi.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "task_stats.h"

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
static TaskHandle_t s_reconnect_task = NULL;
static uint32_t s_backoff_ms = RECONNECT_BACKOFF_MIN_MS;

static const char *wifi_disconnect_reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_UNSPECIFIED: return "unspecified";
    case WIFI_REASON_AUTH_EXPIRE: return "auth expired";
    case WIFI_REASON_AUTH_LEAVE: return "auth leave";
    case WIFI_REASON_ASSOC_EXPIRE: return "assoc expired";
    case WIFI_REASON_ASSOC_TOOMANY: return "assoc too many";
    case WIFI_REASON_NOT_AUTHED: return "not authed";
    case WIFI_REASON_NOT_ASSOCED: return "not assoced";
    case WIFI_REASON_ASSOC_LEAVE: return "assoc leave";
    case WIFI_REASON_ASSOC_NOT_AUTHED: return "assoc not authed";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4-way handshake timeout (wrong PSK or PMF mismatch)";
    case WIFI_REASON_NO_AP_FOUND: return "AP not found";
    case WIFI_REASON_AUTH_FAIL: return "auth failed (wrong password)";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "handshake timeout";
    case WIFI_REASON_CONNECTION_FAIL: return "connection failed";
    default: return "other";
    }
}

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

static void wifi_reconnect_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_associated) {
            continue;
        }
        s_associating = true;
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
            s_associating = false;
        }
    }
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (s_reconnect_task != NULL) {
        xTaskNotifyGive(s_reconnect_task);
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
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *disconn = (const wifi_event_sta_disconnected_t *)event_data;
            s_associated = false;
            s_associating = false;
            set_has_ip(false);
            uint8_t reason = disconn ? disconn->reason : 0;
            ESP_LOGW(TAG, "Wi-Fi uplink disconnected (reason=%u: %s), retrying in %u ms",
                     (unsigned)reason, wifi_disconnect_reason_str(reason), (unsigned)s_backoff_ms);
            schedule_reconnect();
            break;
        }
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

    /* Morse Micro's mmhalow_init() (in downlink_halow_ap.c / uplink_halow.c)
     * unconditionally creates its netif using ESP_NETIF_DEFAULT_WIFI_STA(),
     * which claims if_key "WIFI_STA_DEF". In GW_ROLE_RELAY, both the HaLow
     * radio (downlink AP) and the native Wi-Fi radio (uplink STA) are active.
     * Calling esp_netif_create_default_wifi_sta() here would attempt to
     * register "WIFI_STA_DEF" a second time and trigger a duplicate-key panic
     * in esp_netif_new_api().
     *
     * We use esp_netif_create_wifi() with a custom if_key ("WIFI_STA_NATIVE")
     * and attach default station handlers explicitly, which is identical in
     * function to esp_netif_create_default_wifi_sta() without key collision. */
    esp_netif_inherent_config_t sta_netif_cfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    sta_netif_cfg.if_key = "WIFI_STA_NATIVE";
    sta_netif_cfg.if_desc = "sta_native";
    sta_netif_cfg.route_prio = 128;
    s_netif = esp_netif_create_wifi(WIFI_IF_STA, &sta_netif_cfg);
    if (s_netif == NULL) {
        ESP_LOGE(TAG, "failed to create native Wi-Fi STA netif");
        return ESP_FAIL;
    }
    esp_err_t h_err = esp_wifi_set_default_wifi_sta_handlers();
    if (h_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_default_wifi_sta_handlers: %s", esp_err_to_name(h_err));
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

    if (xTaskCreate(wifi_reconnect_task, "wifi_reconnect", GW_STACK_WIFI_RECONNECT, NULL, 5, &s_reconnect_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create wifi reconnect task");
        return ESP_ERR_NO_MEM;
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
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;
        wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
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
