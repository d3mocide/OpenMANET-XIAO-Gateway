#include <string.h>

#include "uplink_halow.h"

#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "mmhalow.h"

static const char *TAG = "uplink_halow";

#define RECONNECT_BACKOFF_MIN_MS 1000
#define RECONNECT_BACKOFF_MAX_MS 60000
#define HALOW_CONNECT_TIMEOUT_MS 15000
#define LINK_POLL_INTERVAL_MS    2000

static gw_uplink_config_t s_cfg;
static esp_netif_t *s_netif = NULL;
static volatile bool s_associated = false; /* 802.11 association only */
static volatile bool s_has_ip = false;     /* the signal callers actually want */
static uplink_halow_state_cb_t s_cb = NULL;
static void *s_cb_ctx = NULL;
static TaskHandle_t s_reconnect_task_handle = NULL;
static SemaphoreHandle_t s_connect_sem = NULL;

/* HaLow (802.11ah) has no WPA2-PSK - confirmed against mmwlan.h's
 * enum mmwlan_security_type (MMWLAN_OPEN / MMWLAN_OWE / MMWLAN_SAE only). */
static enum mmwlan_security_type halow_security_from_gw(gw_security_mode_t sec)
{
    switch (sec) {
    case GW_SECURITY_OWE:
        return MMWLAN_OWE;
    case GW_SECURITY_SAE:
        return MMWLAN_SAE;
    case GW_SECURITY_OPEN:
    default:
        return MMWLAN_OPEN;
    }
}

/* Registered once with mmhalow_connect() and called on every STA state
 * transition thereafter (association only - not IP). Drives the
 * connect-with-timeout wrapper below via s_connect_sem. */
static void mm_sta_state_cb(enum mmwlan_sta_state sta_state)
{
    switch (sta_state) {
    case MMWLAN_STA_CONNECTED:
        s_associated = true;
        if (s_connect_sem != NULL) {
            xSemaphoreGive(s_connect_sem);
        }
        break;
    case MMWLAN_STA_CONNECTING:
        break;
    case MMWLAN_STA_DISABLED:
    default:
        s_associated = false;
        break;
    }
}

static void set_has_ip(bool has_ip)
{
    if (s_has_ip == has_ip) {
        return;
    }
    s_has_ip = has_ip;
    ESP_LOGI(TAG, "HaLow uplink %s", has_ip ? "up (has IP)" : "down");
    if (s_cb) {
        s_cb(has_ip, s_cb_ctx);
    }
}

/* mmhalow_init() creates its netif via ESP_NETIF_DEFAULT_WIFI_STA(), which
 * sets .get_ip_event = IP_EVENT_STA_GOT_IP and DHCP-client flags just like
 * a normal esp_wifi STA netif (confirmed by reading mmhalow.c directly) -
 * so the standard IP_EVENT_STA_GOT_IP/LOST_IP pair is the real "ready"
 * signal, filtered to our netif since other netifs (the SoftAP) exist too. */
static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;

    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        if (event->esp_netif == s_netif) {
            set_has_ip(true);
        }
    } else if (id == IP_EVENT_STA_LOST_IP) {
        /* This project has exactly one DHCP-client netif (the HaLow STA) -
         * the SoftAP runs a DHCP *server*, not a client - so no netif
         * filter is needed here. */
        set_has_ip(false);
    }
}

static esp_err_t halow_sta_bringup(const gw_uplink_config_t *cfg, esp_netif_t **out_netif)
{
    esp_err_t err = mmhalow_init(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmhalow_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* mmhalow.c has no public "get netif" accessor - its netif is a
     * module-static pointer. It's created with if_key "WIFI_STA_DEF" (the
     * same default esp_wifi STA would use), which is safe to look up here
     * since this project's own SoftAP uses "WIFI_AP_DEF" instead. */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        ESP_LOGE(TAG, "mmhalow_init() didn't register the expected WIFI_STA_DEF netif");
        return ESP_FAIL;
    }
    *out_netif = netif;

    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    mmhalow_wifi_config_t conf = {
        .sta = MMWLAN_STA_ARGS_INIT,
    };
    memcpy(conf.sta.ssid, cfg->ssid, strlen(cfg->ssid));
    conf.sta.ssid_len = strlen(cfg->ssid);
    conf.sta.security_type = halow_security_from_gw(cfg->security);
    if (conf.sta.security_type == MMWLAN_SAE) {
        memcpy(conf.sta.passphrase, cfg->psk, strlen(cfg->psk));
        conf.sta.passphrase_len = strlen(cfg->psk);
    }

    return mmhalow_set_config(WIFI_IF_STA, &conf);
}

static esp_err_t halow_sta_connect(TickType_t timeout_ticks)
{
    if (s_connect_sem == NULL) {
        s_connect_sem = xSemaphoreCreateBinary();
    }
    xSemaphoreTake(s_connect_sem, 0); /* drain any stale signal before retrying */

    esp_err_t err = mmhalow_connect(mm_sta_state_cb);
    if (err != ESP_OK) {
        return err;
    }

    return xSemaphoreTake(s_connect_sem, timeout_ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static bool halow_sta_link_up(void)
{
    return s_associated;
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
        ESP_LOGI(TAG, "HaLow STA associated, waiting for DHCP lease...");

        while (halow_sta_link_up()) {
            vTaskDelay(pdMS_TO_TICKS(LINK_POLL_INTERVAL_MS));
        }

        ESP_LOGW(TAG, "HaLow uplink dropped, will reassociate");
        set_has_ip(false);
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
    return s_has_ip;
}

void uplink_halow_set_state_callback(uplink_halow_state_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
}
