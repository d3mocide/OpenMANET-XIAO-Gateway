#include "ip_forward_nat.h"

#include "esp_log.h"

static const char *TAG = "ip_forward_nat";

esp_err_t ip_forward_nat_init(esp_netif_t *uplink_netif)
{
    if (uplink_netif == NULL) {
        ESP_LOGW(TAG, "no uplink netif yet, skipping NAPT enable");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_netif_napt_enable(uplink_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_napt_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "NAPT enabled on uplink interface");
    return ESP_OK;
}
