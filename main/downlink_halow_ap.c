#include <string.h>

#include "downlink_halow_ap.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"

#include "mmhalow.h"

static const char *TAG = "downlink_halow_ap";

static gw_halow_ap_config_t s_cfg;
static esp_netif_t *s_netif = NULL;
static bool s_ready = false;
static bool s_started = false;

/* AP mode doesn't support the full STA security set - mmwlan.h says so
 * explicitly (mmwlan_ap_enable() docs, v2.11.2-esp32-2): "OWE security is
 * not currently supported for AP mode." provisioning_validate() is meant to
 * catch GW_SECURITY_OWE before it ever reaches here; this fallback exists so
 * a stored config from a build that validated differently doesn't silently
 * hand the driver an unsupported value. */
static enum mmwlan_security_type ap_security_from_gw(gw_security_mode_t sec)
{
    switch (sec) {
    case GW_SECURITY_SAE:
        return MMWLAN_SAE;
    case GW_SECURITY_OWE:
        ESP_LOGW(TAG, "OWE is not supported for HaLow AP mode - falling back to open");
        return MMWLAN_OPEN;
    case GW_SECURITY_OPEN:
    default:
        return MMWLAN_OPEN;
    }
}

esp_err_t downlink_halow_ap_init(const gw_halow_ap_config_t *cfg)
{
    memcpy(&s_cfg, cfg, sizeof(s_cfg));

    esp_err_t err = mmhalow_init(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmhalow_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Same if_key uplink_halow.c relies on - mmhalow_init() always creates
     * its netif with if_key "WIFI_STA_DEF" regardless of which mode (STA or
     * AP) the caller ultimately runs (confirmed by reading mmhalow.c). */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        ESP_LOGE(TAG, "mmhalow_init() didn't register the expected WIFI_STA_DEF netif");
        return ESP_FAIL;
    }
    s_netif = netif;

    /* That netif is DHCP-client shaped (see the long comment in
     * downlink_halow_ap.h) - stop the client explicitly before assigning a
     * static address, so it can't contest the address we're about to set. */
    esp_err_t dhcpc_err = esp_netif_dhcpc_stop(netif);
    if (dhcpc_err != ESP_OK && dhcpc_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "dhcpc_stop: %s", esp_err_to_name(dhcpc_err));
    }

    esp_netif_ip_info_t ip_info = { 0 };
    ip_info.ip.addr = ipaddr_addr(s_cfg.ip);
    ip_info.gw.addr = ipaddr_addr(s_cfg.ip); /* this radio's own address - no routing beyond it */
    ip_info.netmask.addr = ipaddr_addr(s_cfg.netmask);
    err = esp_netif_set_ip_info(netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to set static IP %s/%s: %s", s_cfg.ip, s_cfg.netmask, esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    /* Logged unconditionally, same reasoning as uplink_halow_init(): if this
     * prints, host<->MM6108 SPI works, which rules out wiring/pin/BCF issues
     * before an AP start attempt (which has its own, separate, alpha-API
     * failure modes) muddies the picture. */
    mmhalow_print_version_info();

    if (!gw_halow_ap_is_configured(cfg)) {
        ESP_LOGW(TAG, "no HaLow AP SSID configured - radio is up but no AP will be started");
        return ESP_OK;
    }

    mmhalow_wifi_config_t conf = {
        .ap = MMWLAN_AP_ARGS_INIT,
    };

    size_t ssid_len = strlen(s_cfg.ssid);
    if (ssid_len > sizeof(conf.ap.ssid)) {
        ESP_LOGE(TAG, "HaLow AP SSID is %u bytes, max %u", (unsigned)ssid_len, (unsigned)sizeof(conf.ap.ssid));
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(conf.ap.ssid, s_cfg.ssid, ssid_len);
    conf.ap.ssid_len = ssid_len;

    conf.ap.security_type = ap_security_from_gw(s_cfg.security);
    if (conf.ap.security_type == MMWLAN_SAE) {
        size_t psk_len = strlen(s_cfg.psk);
        if (psk_len > sizeof(conf.ap.passphrase) - 1) {
            ESP_LOGE(TAG, "HaLow AP passphrase is %u bytes, max %u", (unsigned)psk_len,
                     (unsigned)sizeof(conf.ap.passphrase) - 1);
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(conf.ap.passphrase, s_cfg.psk, psk_len);
        conf.ap.passphrase_len = psk_len;
    }

    /* pmf_mode already defaults to MMWLAN_PMF_REQUIRED via MMWLAN_AP_ARGS_INIT
     * - left as-is, matching Morse Micro's own "softap" reference example
     * (esp-halow v2.11.2-esp32-2, halow/examples/softap/main/app_main.c)
     * rather than second-guessing an alpha API's chosen default. */
    conf.ap.op_class = (uint16_t)s_cfg.op_class;
    conf.ap.s1g_chan_num = s_cfg.s1g_chan_num;
    conf.ap.max_stas = s_cfg.max_stas;

    err = mmhalow_set_config(WIFI_IF_AP, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmhalow_set_config(AP) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "starting HaLow AP '%s' (op_class %d, chan %u)...", s_cfg.ssid, s_cfg.op_class,
             s_cfg.s1g_chan_num);
    /* void return - see downlink_halow_ap_is_started()'s doc comment. */
    mmhalow_wifi_start();
    s_started = true;

    return ESP_OK;
}

bool downlink_halow_ap_is_ready(void)
{
    return s_ready;
}

esp_netif_t *downlink_halow_ap_get_netif(void)
{
    return s_netif;
}

bool downlink_halow_ap_is_started(void)
{
    return s_started;
}

size_t downlink_halow_ap_list_channels(halow_ap_channel_cb_t cb, void *ctx)
{
    const struct mmwlan_regulatory_db *db = get_regulatory_db();
    const struct mmwlan_s1g_channel_list *channel_list =
        mmwlan_lookup_regulatory_domain(db, CONFIG_HALOW_COUNTRY_CODE);
    if (channel_list == NULL) {
        return 0;
    }

    for (unsigned i = 0; i < channel_list->num_channels; i++) {
        const struct mmwlan_s1g_channel *chan = &channel_list->channels[i];
        halow_ap_channel_t out = {
            .op_class = chan->s1g_operating_class,
            .s1g_chan_num = chan->s1g_chan_num,
            .freq_hz = chan->centre_freq_hz,
            .bw_mhz = chan->bw_mhz,
        };
        if (cb != NULL) {
            cb(&out, ctx);
        }
    }
    return channel_list->num_channels;
}
