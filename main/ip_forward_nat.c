#include "ip_forward_nat.h"

#include "esp_log.h"

static const char *TAG = "ip_forward_nat";

esp_err_t ip_forward_nat_init(esp_netif_t *downlink_netif, esp_netif_t *uplink_netif)
{
    if (downlink_netif == NULL || uplink_netif == NULL) {
        ESP_LOGW(TAG, "missing netif (downlink=%p uplink=%p), skipping NAPT enable",
                 downlink_netif, uplink_netif);
        return ESP_ERR_INVALID_ARG;
    }

    /* The uplink must be the default route netif so translated traffic
     * actually egresses toward the mesh - ESP-IDF's softap_sta example
     * README states the recipe as "NAPT enabled on the softAP interface and
     * the station interface set as the default interface". esp_netif would
     * normally elect the STA netif anyway on route_prio, but the HaLow netif
     * is created by a third-party component, so state it explicitly rather
     * than depend on that. */
    esp_err_t err = esp_netif_set_default_netif(uplink_netif);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "couldn't set uplink as default netif: %s", esp_err_to_name(err));
        /* Not fatal on its own - NAPT below is still worth enabling. */
    }

    /* NAPT goes on the *downlink* (SoftAP) netif, not the uplink. This is
     * counter-intuitive but is what esp_netif implements: esp_netif_lwip.c's
     * napt control sets `napt = 1` on exactly the netif handed to it (and
     * clears it on every other netif, so only one can have it at a time),
     * and both ESP-IDF's softap_sta example and its README enable it on the
     * AP interface. Passing the uplink here instead silently produces a
     * gateway that forwards SoftAP client packets out the HaLow radio with
     * untranslated 192.168.x source addresses. */
    err = esp_netif_napt_enable(downlink_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_napt_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "NAPT enabled on the SoftAP interface, uplink is default route");
    return ESP_OK;
}
