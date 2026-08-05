#include <string.h>

#include "downlink_softap.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lwip/inet.h"

static const char *TAG = "downlink_softap";

static esp_netif_t *s_netif = NULL;

static esp_err_t apply_custom_subnet(esp_netif_t *netif, const gw_softap_config_t *cfg)
{
    esp_netif_ip_info_t ip_info = { 0 };
    ip_info.ip.addr = ipaddr_addr(cfg->ip);
    ip_info.gw.addr = ipaddr_addr(cfg->gateway);
    ip_info.netmask.addr = ipaddr_addr(cfg->netmask);

    /* DHCP server may not be running yet at this point (called before
     * esp_wifi_start()); tolerate "already stopped" rather than aborting. */
    esp_err_t err = esp_netif_dhcps_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "dhcps_stop: %s", esp_err_to_name(err));
    }

    err = esp_netif_set_ip_info(netif, &ip_info);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_dhcps_start(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGW(TAG, "dhcps_start: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t downlink_softap_init(const gw_softap_config_t *cfg)
{
    s_netif = esp_netif_create_default_wifi_ap();
    if (s_netif == NULL) {
        return ESP_FAIL;
    }

    if (cfg->use_custom_subnet) {
        esp_err_t err = apply_custom_subnet(s_netif, cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to apply custom subnet: %s", esp_err_to_name(err));
            return err;
        }
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t wifi_config = { 0 };
    size_t ssid_len = strlen(cfg->ssid);
    memcpy(wifi_config.ap.ssid, cfg->ssid, ssid_len);
    wifi_config.ap.ssid_len = ssid_len;
    wifi_config.ap.channel = cfg->channel;
    wifi_config.ap.max_connection = cfg->max_connections ? cfg->max_connections : 4;
    wifi_config.ap.pmf_cfg.required = false;

    if (strlen(cfg->psk) > 0) {
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        strlcpy((char *)wifi_config.ap.password, cfg->psk, sizeof(wifi_config.ap.password));
    } else {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), TAG, "set_config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");

    ESP_LOGI(TAG, "SoftAP '%s' started on channel %u (%s)", cfg->ssid, cfg->channel,
              strlen(cfg->psk) > 0 ? "WPA2-PSK" : "open");
    return ESP_OK;
}

esp_netif_t *downlink_softap_get_netif(void)
{
    return s_netif;
}
