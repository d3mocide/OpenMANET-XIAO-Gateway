#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "provisioning.h"

#include "cot_relay.h"
#include "downlink_halow_ap.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "task_stats.h"
#include "uplink_halow.h"
#include "uplink_wifi.h"

static const char *TAG = "provisioning";

#define GWCFG_NVS_NAMESPACE "gwcfg"
#define GWCFG_NVS_KEY       "config"

/* Generous compared to the web UI's budget: nothing is waiting on a socket
 * here, and a console scan is a deliberate, attended action. */
#define GWCFG_SCAN_TIMEOUT_MS 12000

/* Points at the app's live in-RAM config so console commands can edit it
 * directly; set by provisioning_register_console_commands(). */
static gw_config_t *s_cfg = NULL;

/* The live config is touched by three tasks - the console REPL, the httpd
 * task, and app_main at boot - so mutation is serialized. Created in
 * provisioning_init(), i.e. before any of those exist. */
static SemaphoreHandle_t s_cfg_lock = NULL;

void provisioning_config_lock(void)
{
    if (s_cfg_lock != NULL) {
        xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    }
}

void provisioning_config_unlock(void)
{
    if (s_cfg_lock != NULL) {
        xSemaphoreGive(s_cfg_lock);
    }
}

void provisioning_get_defaults(gw_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->magic = GW_CONFIG_MAGIC;
    cfg->version = GW_CONFIG_VERSION;

    strlcpy(cfg->node_id, "xiao-gw-01", sizeof(cfg->node_id));

    /* GW_ROLE_CLIENT: today's original design, and still what a factory-fresh
     * node ships as - a relay is a deliberate per-node choice
     * (gwcfg-set-role), not a default anyone should get by accident. */
    cfg->role = GW_ROLE_CLIENT;

    /* No uplink by default, and deliberately no placeholder SSID.
     *
     * This used to ship "openmanet-halow", which made a factory-fresh node
     * indistinguishable from a configured one whose AP is switched off: both
     * reported "searching" and blinked identically, while the node burned
     * 15-second association attempts against a name nobody had chosen. An
     * empty SSID is the "not configured yet" state (gw_uplink_is_configured())
     * and the firmware reports and acts on it - see uplink_halow_start(). */
    cfg->uplink.ssid[0] = '\0';
    cfg->uplink.psk[0] = '\0';
    cfg->uplink.security = GW_SECURITY_OPEN;
    cfg->uplink.use_static_ip = false;
    cfg->uplink.static_ip[0] = '\0';
    cfg->uplink.static_gateway[0] = '\0';
    cfg->uplink.static_netmask[0] = '\0';

    /* GW_ROLE_RELAY fields. Also unconfigured by default, same reasoning as
     * the HaLow uplink above - and harmless to leave populated with their
     * defaults on a GW_ROLE_CLIENT node, since bring_up_client_role() never
     * reads them. */
    cfg->wifi_uplink.ssid[0] = '\0';
    cfg->wifi_uplink.psk[0] = '\0';

    cfg->halow_ap.ssid[0] = '\0';
    cfg->halow_ap.psk[0] = '\0';
    cfg->halow_ap.security = GW_SECURITY_SAE;
    cfg->halow_ap.op_class = 0;
    cfg->halow_ap.s1g_chan_num = 0;
    cfg->halow_ap.max_stas = 0; /* => MMWLAN_DEFAULT_AP_MAX_STAS (4) */
    /* A different /24 than the client role's SoftAP subnet (172.16.50.0/24)
     * purely so the two are never confusable in logs/ARP tables if someone's
     * looking at both roles side by side - see gw_config.h. */
    strlcpy(cfg->halow_ap.ip, "172.16.60.1", sizeof(cfg->halow_ap.ip));
    strlcpy(cfg->halow_ap.netmask, "255.255.255.0", sizeof(cfg->halow_ap.netmask));

    /* Local client-facing SoftAP.
     *
     * 172.16.50.0/24, not 192.168.x: this subnet has to coexist with whatever
     * the phone or tablet was last attached to, and with the mesh subnet on
     * the far side of the NAT. 192.168.0/1/4/50.x are heavily used by home
     * routers, phone hotspots and other ESP32 SoftAPs (esp_netif's own default
     * is 192.168.4.1), and an overlap between a client's remembered network
     * and this one produces routing behaviour that is very hard to diagnose in
     * the field. 172.16.0.0/12 is the least-trafficked of the three RFC1918
     * blocks. Every XIAO node can safely use the same subnet - each one NATs
     * behind its own uplink address, so they never see each other's. */
    strlcpy(cfg->softap.ssid, "xiao-gateway", sizeof(cfg->softap.ssid));
    strlcpy(cfg->softap.psk, "openmanet", sizeof(cfg->softap.psk));
    cfg->softap.channel = 6;
    cfg->softap.max_connections = 8;
    cfg->softap.use_custom_subnet = true;
    strlcpy(cfg->softap.ip, "172.16.50.1", sizeof(cfg->softap.ip));
    strlcpy(cfg->softap.gateway, "172.16.50.1", sizeof(cfg->softap.gateway));
    strlcpy(cfg->softap.netmask, "255.255.255.0", sizeof(cfg->softap.netmask));

    /* ATAK CoT multicast group. */
    strlcpy(cfg->cot.group, "239.2.3.1", sizeof(cfg->cot.group));
    cfg->cot.port = 6969;
}

esp_err_t provisioning_init(void)
{
    if (s_cfg_lock == NULL) {
        s_cfg_lock = xSemaphoreCreateMutex();
        if (s_cfg_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t provisioning_load(gw_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(GWCFG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no stored config (%s), using defaults", esp_err_to_name(err));
        provisioning_get_defaults(cfg);
        return ESP_OK;
    }

    size_t len = sizeof(*cfg);
    err = nvs_get_blob(handle, GWCFG_NVS_KEY, cfg, &len);
    nvs_close(handle);

    if (err != ESP_OK || len != sizeof(*cfg)) {
        ESP_LOGW(TAG, "stored config missing/invalid (%s), using defaults", esp_err_to_name(err));
        provisioning_get_defaults(cfg);
        return ESP_OK;
    }

    /* Size alone doesn't prove compatibility - a field could change meaning
     * without changing the struct's size. Check the stamp explicitly. */
    if (cfg->magic != GW_CONFIG_MAGIC || cfg->version != GW_CONFIG_VERSION) {
        ESP_LOGW(TAG, "stored config is magic=0x%08" PRIx32 " v%" PRIu32 ", expected 0x%08" PRIx32
                      " v%" PRIu32 " - using defaults",
                 cfg->magic, cfg->version, (uint32_t)GW_CONFIG_MAGIC, (uint32_t)GW_CONFIG_VERSION);
        provisioning_get_defaults(cfg);
        return ESP_OK;
    }

    /* A blob written by an older build (or hand-edited NVS) could still hold
     * values this build considers unusable; catching that here beats letting
     * esp_wifi fail at AP start and taking the management path down. */
    char reason[96];
    if (provisioning_validate(cfg, reason, sizeof(reason)) != ESP_OK) {
        ESP_LOGW(TAG, "stored config failed validation (%s), using defaults", reason);
        provisioning_get_defaults(cfg);
    }

    return ESP_OK;
}

esp_err_t provisioning_save(const gw_config_t *cfg)
{
    char reason[96];
    esp_err_t err = provisioning_validate(cfg, reason, sizeof(reason));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "refusing to save invalid config: %s", reason);
        return err;
    }

    /* Written unconditionally so a blob saved by this build always carries
     * this build's stamp, even if the caller's struct came from elsewhere. */
    gw_config_t stamped;
    memcpy(&stamped, cfg, sizeof(stamped));
    stamped.magic = GW_CONFIG_MAGIC;
    stamped.version = GW_CONFIG_VERSION;

    nvs_handle_t handle;
    err = nvs_open(GWCFG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, GWCFG_NVS_KEY, &stamped, sizeof(stamped));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/* Rejects configs that would brick the device's own management path or that
 * esp_wifi/lwIP would refuse at bring-up. Shared by the console, the web UI,
 * and the NVS load path so all three agree on what "valid" means. On failure
 * writes a human-readable reason into errbuf (shown verbatim in the web UI's
 * error banner and on the console). */
esp_err_t provisioning_validate(const gw_config_t *cfg, char *errbuf, size_t errbuf_len)
{
#define GW_REJECT(...)                                       \
    do {                                                     \
        if (errbuf != NULL && errbuf_len > 0) {              \
            snprintf(errbuf, errbuf_len, __VA_ARGS__);       \
        }                                                    \
        return ESP_ERR_INVALID_ARG;                          \
    } while (0)

    if (cfg->node_id[0] == '\0') {
        GW_REJECT("node_id must not be empty");
    }

    /* An empty uplink SSID is valid: it is how "not configured yet" is
     * represented (gw_uplink_is_configured()), and rejecting it here would
     * make the built-in defaults themselves fail validation on first boot.
     * The remaining uplink checks only apply once one has actually been set. */
    if (gw_uplink_is_configured(&cfg->uplink)) {
        /* HaLow SAE needs a passphrase; open/OWE must not carry one. (No length
         * floor asserted for SAE - unlike WPA2-PSK, SAE does not specify one.) */
        if (cfg->uplink.security == GW_SECURITY_SAE && cfg->uplink.psk[0] == '\0') {
            GW_REJECT("uplink security is SAE but no passphrase is set");
        }
    }

    /* Only meaningful against a GW_ROLE_RELAY's HaLow AP - see the long
     * comment on gw_uplink_config_t.use_static_ip for why a real Pi doesn't
     * need this. Validated whenever it's turned on, regardless of the
     * node's current role, same reasoning as the rest of this function:
     * one validator, shared by console/web UI/NVS load, that doesn't need
     * to know which fields the active role actually reads. */
    if (cfg->uplink.use_static_ip) {
        struct in_addr tmp;
        if (inet_aton(cfg->uplink.static_ip, &tmp) == 0) {
            GW_REJECT("uplink static IP '%s' is not a valid address", cfg->uplink.static_ip);
        }
        if (inet_aton(cfg->uplink.static_gateway, &tmp) == 0) {
            GW_REJECT("uplink static gateway '%s' is not a valid address", cfg->uplink.static_gateway);
        }
        if (inet_aton(cfg->uplink.static_netmask, &tmp) == 0) {
            GW_REJECT("uplink static netmask '%s' is not a valid address", cfg->uplink.static_netmask);
        }
    }

    if (cfg->softap.ssid[0] == '\0') {
        GW_REJECT("local Wi-Fi SSID must not be empty");
    }

    /* Empty means an open AP, which is allowed; anything else must be a
     * legal WPA2 passphrase or esp_wifi refuses to start the AP - and the
     * AP is how this device is managed. */
    size_t ap_psk_len = strlen(cfg->softap.psk);
    if (ap_psk_len > 0 && (ap_psk_len < GW_WPA2_PSK_MIN_LEN || ap_psk_len > GW_WPA2_PSK_MAX_LEN)) {
        GW_REJECT("local Wi-Fi passphrase must be %d-%d characters (or empty for an open network)",
                  GW_WPA2_PSK_MIN_LEN, GW_WPA2_PSK_MAX_LEN);
    }

    if (cfg->softap.channel < GW_SOFTAP_CHANNEL_MIN || cfg->softap.channel > GW_SOFTAP_CHANNEL_MAX) {
        GW_REJECT("local Wi-Fi channel must be %d-%d", GW_SOFTAP_CHANNEL_MIN, GW_SOFTAP_CHANNEL_MAX);
    }

    if (cfg->softap.max_connections > 15) {
        GW_REJECT("max_connections must be 15 or fewer");
    }

    if (cfg->softap.use_custom_subnet) {
        struct in_addr tmp;
        if (inet_aton(cfg->softap.ip, &tmp) == 0) {
            GW_REJECT("local Wi-Fi IP '%s' is not a valid address", cfg->softap.ip);
        }
        if (inet_aton(cfg->softap.gateway, &tmp) == 0) {
            GW_REJECT("local Wi-Fi gateway '%s' is not a valid address", cfg->softap.gateway);
        }
        if (inet_aton(cfg->softap.netmask, &tmp) == 0) {
            GW_REJECT("local Wi-Fi netmask '%s' is not a valid address", cfg->softap.netmask);
        }
    }

    /* GW_ROLE_RELAY fields. Same "empty means not configured yet" pattern as
     * the HaLow uplink - validated only once an operator has actually set a
     * value, so the built-in defaults (both empty) pass validation too. */
    if (gw_wifi_uplink_is_configured(&cfg->wifi_uplink)) {
        size_t wifi_psk_len = strlen(cfg->wifi_uplink.psk);
        if (wifi_psk_len > 0 && (wifi_psk_len < GW_WPA2_PSK_MIN_LEN || wifi_psk_len > GW_WPA2_PSK_MAX_LEN)) {
            GW_REJECT("Wi-Fi uplink passphrase must be %d-%d characters (or empty for an open network)",
                      GW_WPA2_PSK_MIN_LEN, GW_WPA2_PSK_MAX_LEN);
        }
    }

    if (gw_halow_ap_is_configured(&cfg->halow_ap)) {
        /* "OWE security is not currently supported for AP mode" - mmwlan.h's
         * own mmwlan_ap_enable() docs (v2.11.2-esp32-2). Rejected here rather
         * than silently downgraded, so a bad choice is caught at the point
         * it's made instead of surfacing later as an AP that starts wrong. */
        if (cfg->halow_ap.security == GW_SECURITY_OWE) {
            GW_REJECT("HaLow AP security cannot be OWE (unsupported for AP mode) - use open or sae");
        }
        if (cfg->halow_ap.security == GW_SECURITY_SAE && cfg->halow_ap.psk[0] == '\0') {
            GW_REJECT("HaLow AP security is SAE but no passphrase is set");
        }
        /* Zero is MMWLAN_AP_ARGS_INIT's default, meaning "never actually
         * chosen" here - mmwlan_ap_enable() only auto-configures from 0/0
         * when a STA is concurrently active on the same radio, which a
         * relay's HaLow radio never is (AP mode only - see gw_config.h). An
         * operator must pick a real channel, e.g. via a channel list command
         * backed by downlink_halow_ap_list_channels(). */
        if (cfg->halow_ap.op_class == 0) {
            GW_REJECT("HaLow AP channel must be set explicitly (op_class 0 has no meaning without "
                      "a concurrently active STA)");
        }
        if (cfg->halow_ap.max_stas > 20) { /* MMWLAN_AP_MAX_STAS_LIMIT, mmwlan.h */
            GW_REJECT("HaLow AP max_stas must be 20 or fewer");
        }
        struct in_addr tmp;
        if (inet_aton(cfg->halow_ap.ip, &tmp) == 0) {
            GW_REJECT("HaLow AP IP '%s' is not a valid address", cfg->halow_ap.ip);
        }
        if (inet_aton(cfg->halow_ap.netmask, &tmp) == 0) {
            GW_REJECT("HaLow AP netmask '%s' is not a valid address", cfg->halow_ap.netmask);
        }
    }

    struct in_addr group;
    if (inet_aton(cfg->cot.group, &group) == 0) {
        GW_REJECT("CoT group '%s' is not a valid address", cfg->cot.group);
    }
    /* Must be in 224.0.0.0/4 - a unicast address here would make the relay
     * join a group it can never receive on. */
    if ((ntohl(group.s_addr) & 0xF0000000u) != 0xE0000000u) {
        GW_REJECT("CoT group '%s' is not a multicast address (224.0.0.0/4)", cfg->cot.group);
    }

    if (cfg->cot.port == 0) {
        GW_REJECT("CoT port must be 1-65535");
    }

    return ESP_OK;

#undef GW_REJECT
}

const char *provisioning_security_name(gw_security_mode_t sec)
{
    static const char *names[] = { "open", "owe", "sae" };
    if ((size_t)sec >= sizeof(names) / sizeof(names[0])) {
        return "open";
    }
    return names[sec];
}

/* HaLow (802.11ah) has no WPA2-PSK mode - only open/OWE/SAE, confirmed
 * against the real morsemicro/halow SDK's enum mmwlan_security_type. */
gw_security_mode_t provisioning_parse_security(const char *s)
{
    if (strcmp(s, "owe") == 0) {
        return GW_SECURITY_OWE;
    }
    if (strcmp(s, "sae") == 0) {
        return GW_SECURITY_SAE;
    }
    return GW_SECURITY_OPEN;
}

const char *provisioning_role_name(gw_node_role_t role)
{
    return role == GW_ROLE_RELAY ? "relay" : "client";
}

gw_node_role_t provisioning_parse_role(const char *s)
{
    return strcmp(s, "relay") == 0 ? GW_ROLE_RELAY : GW_ROLE_CLIENT;
}

static void print_config(const gw_config_t *cfg)
{
    printf("node_id       : %s\n", cfg->node_id);
    printf("role          : %s\n", provisioning_role_name(cfg->role));
    if (cfg->role == GW_ROLE_RELAY) {
        printf("wifi_uplink.ssid: %s\n", cfg->wifi_uplink.ssid);
        printf("halow_ap.ssid : %s\n", cfg->halow_ap.ssid);
        printf("halow_ap.security: %s\n", provisioning_security_name(cfg->halow_ap.security));
        printf("halow_ap.chan : op_class %d, s1g_chan_num %u\n", cfg->halow_ap.op_class,
               cfg->halow_ap.s1g_chan_num);
        printf("halow_ap.ip   : %s/%s\n", cfg->halow_ap.ip, cfg->halow_ap.netmask);
    } else {
        printf("uplink.ssid   : %s\n", cfg->uplink.ssid);
        printf("uplink.security: %s\n", provisioning_security_name(cfg->uplink.security));
        if (cfg->uplink.use_static_ip) {
            printf("uplink.static_ip: %s/%s via %s\n", cfg->uplink.static_ip, cfg->uplink.static_netmask,
                   cfg->uplink.static_gateway);
        }
        printf("softap.ssid   : %s\n", cfg->softap.ssid);
        printf("softap.channel: %u\n", cfg->softap.channel);
        printf("softap.subnet : %s\n",
               cfg->softap.use_custom_subnet ? cfg->softap.ip : "192.168.4.1 (esp-netif default)");
    }
    printf("cot           : %s:%u\n", cfg->cot.group, cfg->cot.port);
}

static int cmd_gwcfg_show(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!s_cfg) {
        return 1;
    }
    provisioning_config_lock();
    print_config(s_cfg);
    provisioning_config_unlock();
    return 0;
}

static int cmd_gwcfg_set_node(int argc, char **argv)
{
    if (!s_cfg || argc != 2) {
        printf("usage: gwcfg-set-node <node_id>\n");
        return 1;
    }

    /* Scratch-copy + validate, like every other setter: rejecting here, next
     * to the edit, beats a "not saved: ..." from gwcfg-save minutes later
     * with no hint about which command caused it. */
    provisioning_config_lock();
    gw_config_t work = *s_cfg;
    strlcpy(work.node_id, argv[1], sizeof(work.node_id));

    char reason[96];
    if (provisioning_validate(&work, reason, sizeof(reason)) != ESP_OK) {
        provisioning_config_unlock();
        printf("rejected: %s\n", reason);
        return 1;
    }

    *s_cfg = work;
    provisioning_config_unlock();
    return 0;
}

static int cmd_gwcfg_set_uplink(int argc, char **argv)
{
    if (!s_cfg || argc < 4) {
        printf("usage: gwcfg-set-uplink <ssid> <psk|-> <open|owe|sae>\n");
        return 1;
    }

    /* Validated against a scratch copy so a rejected value never lands in
     * the live config - same discipline as gwcfg-set-softap and the web
     * UI's POST handler. Catches e.g. SAE with no passphrase immediately
     * instead of at gwcfg-save time. */
    provisioning_config_lock();
    gw_config_t work = *s_cfg;
    strlcpy(work.uplink.ssid, argv[1], sizeof(work.uplink.ssid));
    strlcpy(work.uplink.psk, strcmp(argv[2], "-") == 0 ? "" : argv[2], sizeof(work.uplink.psk));
    work.uplink.security = provisioning_parse_security(argv[3]);

    char reason[96];
    if (provisioning_validate(&work, reason, sizeof(reason)) != ESP_OK) {
        provisioning_config_unlock();
        printf("rejected: %s\n", reason);
        return 1;
    }

    *s_cfg = work;
    provisioning_config_unlock();
    printf("uplink config updated in RAM; run 'gwcfg-save' then reboot to apply\n");
    return 0;
}

static int cmd_gwcfg_set_softap(int argc, char **argv)
{
    if (!s_cfg || argc < 3) {
        printf("usage: gwcfg-set-softap <ssid> <psk|-> [channel]\n");
        return 1;
    }

    /* Validated against a scratch copy so a rejected value never lands in
     * the live config - the same discipline the web UI's POST handler uses. */
    provisioning_config_lock();
    gw_config_t work = *s_cfg;
    strlcpy(work.softap.ssid, argv[1], sizeof(work.softap.ssid));
    strlcpy(work.softap.psk, strcmp(argv[2], "-") == 0 ? "" : argv[2], sizeof(work.softap.psk));
    if (argc >= 4) {
        work.softap.channel = (uint8_t)atoi(argv[3]);
    }

    char reason[96];
    if (provisioning_validate(&work, reason, sizeof(reason)) != ESP_OK) {
        provisioning_config_unlock();
        printf("rejected: %s\n", reason);
        return 1;
    }

    *s_cfg = work;
    provisioning_config_unlock();
    printf("softap config updated in RAM; run 'gwcfg-save' then reboot to apply\n");
    return 0;
}

static int cmd_gwcfg_set_role(int argc, char **argv)
{
    if (!s_cfg || argc != 2) {
        printf("usage: gwcfg-set-role <client|relay>\n");
        return 1;
    }

    provisioning_config_lock();
    gw_config_t work = *s_cfg;
    work.role = provisioning_parse_role(argv[1]);

    char reason[96];
    if (provisioning_validate(&work, reason, sizeof(reason)) != ESP_OK) {
        provisioning_config_unlock();
        printf("rejected: %s\n", reason);
        return 1;
    }

    *s_cfg = work;
    provisioning_config_unlock();
    printf("role set to '%s' in RAM; run 'gwcfg-save' then reboot to apply - it changes which "
           "radios/netifs come up entirely, not just a config value\n",
           provisioning_role_name(work.role));
    return 0;
}

/* Separate from gwcfg-set-uplink deliberately: this only matters when the
 * uplink is a GW_ROLE_RELAY's HaLow AP rather than a real Pi (see the long
 * comment on gw_uplink_config_t.use_static_ip), so it's an edge case worth
 * keeping out of the common command's argument list. */
static int cmd_gwcfg_set_uplink_static_ip(int argc, char **argv)
{
    if (!s_cfg || (argc != 2 && argc != 4)) {
        printf("usage: gwcfg-set-uplink-static-ip -              (use DHCP, the default)\n"
               "       gwcfg-set-uplink-static-ip <ip> <gateway> <netmask>\n");
        return 1;
    }

    provisioning_config_lock();
    gw_config_t work = *s_cfg;

    if (argc == 2 && strcmp(argv[1], "-") == 0) {
        work.uplink.use_static_ip = false;
        work.uplink.static_ip[0] = '\0';
        work.uplink.static_gateway[0] = '\0';
        work.uplink.static_netmask[0] = '\0';
    } else if (argc == 4) {
        work.uplink.use_static_ip = true;
        strlcpy(work.uplink.static_ip, argv[1], sizeof(work.uplink.static_ip));
        strlcpy(work.uplink.static_gateway, argv[2], sizeof(work.uplink.static_gateway));
        strlcpy(work.uplink.static_netmask, argv[3], sizeof(work.uplink.static_netmask));
    } else {
        provisioning_config_unlock();
        printf("usage: gwcfg-set-uplink-static-ip -              (use DHCP, the default)\n"
               "       gwcfg-set-uplink-static-ip <ip> <gateway> <netmask>\n");
        return 1;
    }

    char reason[96];
    if (provisioning_validate(&work, reason, sizeof(reason)) != ESP_OK) {
        provisioning_config_unlock();
        printf("rejected: %s\n", reason);
        return 1;
    }

    *s_cfg = work;
    provisioning_config_unlock();
    printf("uplink static-IP config updated in RAM; run 'gwcfg-save' then reboot to apply\n");
    return 0;
}

static int cmd_gwcfg_set_wifi_uplink(int argc, char **argv)
{
    if (!s_cfg || argc != 3) {
        printf("usage: gwcfg-set-wifi-uplink <ssid> <psk|->\n"
               "  (GW_ROLE_RELAY only - the native 2.4GHz uplink to the Pi's local AP)\n");
        return 1;
    }

    provisioning_config_lock();
    gw_config_t work = *s_cfg;
    strlcpy(work.wifi_uplink.ssid, argv[1], sizeof(work.wifi_uplink.ssid));
    strlcpy(work.wifi_uplink.psk, strcmp(argv[2], "-") == 0 ? "" : argv[2], sizeof(work.wifi_uplink.psk));

    char reason[96];
    if (provisioning_validate(&work, reason, sizeof(reason)) != ESP_OK) {
        provisioning_config_unlock();
        printf("rejected: %s\n", reason);
        return 1;
    }

    *s_cfg = work;
    provisioning_config_unlock();
    printf("Wi-Fi uplink config updated in RAM; run 'gwcfg-save' then reboot to apply\n");
    return 0;
}

static int cmd_gwcfg_set_halow_ap(int argc, char **argv)
{
    if (!s_cfg || argc != 6) {
        printf("usage: gwcfg-set-halow-ap <ssid> <psk|-> <open|sae> <op_class> <s1g_chan_num>\n"
               "  (GW_ROLE_RELAY only - the HaLow AP leaf XIAOs associate to)\n"
               "  run 'gwcfg-list-halow-channels' first to see legal (op_class, s1g_chan_num) pairs\n");
        return 1;
    }

    provisioning_config_lock();
    gw_config_t work = *s_cfg;
    strlcpy(work.halow_ap.ssid, argv[1], sizeof(work.halow_ap.ssid));
    strlcpy(work.halow_ap.psk, strcmp(argv[2], "-") == 0 ? "" : argv[2], sizeof(work.halow_ap.psk));
    /* AP mode doesn't support OWE (mmwlan.h) - reusing provisioning_parse_security()
     * here means "owe" is parseable but provisioning_validate() below rejects it,
     * same as any other bad value, rather than silently mapping it to open. */
    work.halow_ap.security = provisioning_parse_security(argv[3]);
    work.halow_ap.op_class = (int16_t)atoi(argv[4]);
    work.halow_ap.s1g_chan_num = (uint8_t)atoi(argv[5]);

    char reason[96];
    if (provisioning_validate(&work, reason, sizeof(reason)) != ESP_OK) {
        provisioning_config_unlock();
        printf("rejected: %s\n", reason);
        return 1;
    }

    *s_cfg = work;
    provisioning_config_unlock();
    printf("HaLow AP config updated in RAM; run 'gwcfg-save' then reboot to apply\n");
    return 0;
}

/* Bring-up commands. These don't touch config at all - they exist because the
 * serial console is the one interface guaranteed to work when the SoftAP
 * hasn't come up, and the questions they answer ("does the radio respond?",
 * "is the AP visible?", "how strong?") are the first three asked of a node
 * that isn't associating. */
static int cmd_gwcfg_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const esp_app_desc_t *desc = esp_app_get_description();
    printf("firmware ver  : %s\n", desc ? desc->version : "unknown");

    gw_node_role_t role = s_cfg ? s_cfg->role : GW_ROLE_CLIENT;
    printf("role          : %s\n", provisioning_role_name(role));

    esp_netif_t *uplink_netif;
    if (role == GW_ROLE_RELAY) {
        printf("wifi uplink   : %s\n", uplink_wifi_link_state_name(uplink_wifi_get_link_state()));
        int8_t rssi = uplink_wifi_get_rssi();
        if (rssi == INT8_MIN) {
            printf("wifi RSSI     : (not associated)\n");
        } else {
            printf("wifi RSSI     : %" PRId8 " dBm\n", rssi);
        }
        printf("halow ap      : %s\n", downlink_halow_ap_is_started() ? "started (best-effort - "
                                                                          "mmhalow_wifi_start() has no "
                                                                          "return code)"
                                                                       : "not started");
        uplink_netif = uplink_wifi_get_netif();
    } else {
        uplink_link_state_t state = uplink_halow_get_link_state();
        printf("uplink state  : %s\n", uplink_halow_link_state_name(state));

        int32_t rssi = uplink_halow_get_rssi();
        if (rssi == INT32_MIN) {
            printf("uplink RSSI   : (not associated)\n");
        } else {
            printf("uplink RSSI   : %" PRId32 " dBm\n", rssi);
        }
        uplink_netif = uplink_halow_get_netif();
    }

    esp_netif_ip_info_t ip_info;
    if (uplink_netif != NULL && esp_netif_get_ip_info(uplink_netif, &ip_info) == ESP_OK &&
        ip_info.ip.addr != 0) {
        printf("uplink IP     : " IPSTR "\n", IP2STR(&ip_info.ip));
        printf("uplink gw     : " IPSTR "\n", IP2STR(&ip_info.gw));
    } else {
        printf("uplink IP     : (no lease)\n");
    }

    printf("cot relay     : %s\n", cot_relay_is_running() ? "running" : "not started");
    printf("country code  : %s (build-time, not settable here)\n", CONFIG_HALOW_COUNTRY_CODE);
    printf("free heap     : %u bytes\n", (unsigned)esp_get_free_heap_size());
    return 0;
}

static int cmd_gwcfg_radio(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!uplink_halow_is_ready()) {
        printf("radio not initialized - check CONFIG_MM_* pins/BCF (design/HARDWARE.md)\n");
        return 1;
    }
    uplink_halow_log_radio_info();
    return 0;
}

static void task_stack_print_cb(const task_stack_info_t *info, void *ctx)
{
    unsigned *rows = (unsigned *)ctx;
    (*rows)++;

    if (!info->present) {
        printf("%-17s %8s  %8s  %6s  %s\n", info->name, "-", "-", "-", "not running");
        return;
    }

    /* Integer percentage, and of *used* rather than free, so the number grows
     * as the situation worsens - the same reason the frequency column below
     * avoids %f: this table is read during bring-up, when a misread costs
     * hardware time. Rounded up, so a task that has touched any of its stack
     * at all never reports 0% used. */
    size_t used = info->stack_total - info->stack_free_min;
    unsigned pct = info->stack_total ? (unsigned)((used * 100 + info->stack_total - 1) / info->stack_total) : 0;

    /* Flagged, not just printed. A bare column of numbers requires the reader
     * to already know what "352 bytes free" means for a task they didn't
     * create; these thresholds say it outright. */
    const char *note = "";
    if (info->stack_free_min < 256) {
        note = "  <-- CRITICAL, will overflow";
    } else if (info->stack_free_min < 512) {
        note = "  <-- tight, raise it";
    }

    printf("%-17s %8u  %8u  %5u%%  %s\n", info->name, (unsigned)info->stack_total,
           (unsigned)info->stack_free_min, pct, note);
}

/* Reports how close each task has come to overflowing its stack.
 *
 * Exists because this firmware has already lost a node to a stack overflow
 * that nothing visible predicted (design/ROADMAP.md item 8): the failure is
 * silent right up to the panic, and the margin is not inferable from reading
 * the code. This is the instrument that makes it a number instead of a guess.
 *
 * Worth running after the node has been up a while and through a reconnect or
 * two, not just at boot - the mark is a high-water measurement, so it only
 * reflects paths that have actually executed. A task that has never yet taken
 * its deepest branch will flatter itself here. */
static int cmd_gwcfg_tasks(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    unsigned rows = 0;
    printf("%-17s %8s  %8s  %6s\n", "TASK", "STACK", "FREE", "USED");
    size_t found = task_stats_each_stack(task_stack_print_cb, &rows);
    printf("%u of %u watched tasks running. FREE is the least this task has "
           "ever had spare, in bytes.\n",
           (unsigned)found, rows);
    return 0;
}

static void scan_print_cb(const uplink_scan_result_t *result, void *ctx)
{
    unsigned *n = (unsigned *)ctx;
    (*n)++;
    /* Frequency is formatted with integer arithmetic rather than %f on a
     * double. CONFIG_LIBC_NEWLIB_NANO_FORMAT is off in this build so %f would
     * work today, but it is exactly the kind of knob someone reaches for to
     * shrink a binary later - and the failure mode is a scan table that prints
     * garbage frequencies during bring-up, which is when it's least welcome. */
    unsigned mhz = (unsigned)(result->freq_hz / 1000000u);
    unsigned khz = (unsigned)((result->freq_hz % 1000000u) / 1000u);
    printf("%-32s %02x:%02x:%02x:%02x:%02x:%02x  %4d dBm  %4u.%03u MHz  %2u MHz\n",
           result->ssid[0] ? result->ssid : "(hidden)", result->bssid[0], result->bssid[1],
           result->bssid[2], result->bssid[3], result->bssid[4], result->bssid[5], result->rssi,
           mhz, khz, result->bw_mhz);
}

static int cmd_gwcfg_scan(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!uplink_halow_is_ready()) {
        printf("radio not initialized - nothing to scan with\n");
        return 1;
    }

    printf("scanning (regulatory domain %s)...\n", CONFIG_HALOW_COUNTRY_CODE);
    printf("%-32s %-17s  %8s  %11s  %s\n", "SSID", "BSSID", "RSSI", "FREQ", "BW");

    unsigned found = 0;
    esp_err_t err = uplink_halow_scan(scan_print_cb, &found, GWCFG_SCAN_TIMEOUT_MS);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        printf("scan failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("%u AP(s) found%s\n", found, err == ESP_ERR_TIMEOUT ? " (scan timed out, partial)" : "");
    if (found == 0) {
        /* Said explicitly because the obvious conclusion ("the AP is off") is
         * only one of two, and the other one is a build-time setting that
         * can't be fixed from this console - or from a different build, since
         * this hardware is 902-928 MHz only (design/HARDWARE.md "Regulatory
         * domain"). The fix is on the Pi, so point there. */
        printf("note: only channels legal in '%s' were scanned. An AP on a channel outside\n"
               "      this regulatory domain is invisible here - this board is 902-928 MHz\n"
               "      only, so the Pi's HaLow radio has to be on '%s' too.\n",
               CONFIG_HALOW_COUNTRY_CODE, CONFIG_HALOW_COUNTRY_CODE);
    }
    return 0;
}

static void channel_print_cb(const halow_ap_channel_t *chan, void *ctx)
{
    unsigned *n = (unsigned *)ctx;
    (*n)++;
    unsigned mhz = (unsigned)(chan->freq_hz / 1000000u);
    unsigned khz = (unsigned)((chan->freq_hz % 1000000u) / 1000u);
    printf("op_class %-4d  s1g_chan_num %-4u  %4u.%03u MHz  %2u MHz\n", chan->op_class,
           chan->s1g_chan_num, mhz, khz, chan->bw_mhz);
}

/* GW_ROLE_RELAY only: mmwlan_ap_args wants an (op_class, s1g_chan_num) pair,
 * not a frequency - this exists so gwcfg-set-halow-ap's arguments can be
 * chosen from a real table instead of guessed. Pure table walk against
 * CONFIG_HALOW_COUNTRY_CODE's already-loaded regulatory domain, no radio
 * activity - works even before downlink_halow_ap_init() has run. */
static int cmd_gwcfg_list_halow_channels(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("HaLow channels legal in regulatory domain '%s':\n", CONFIG_HALOW_COUNTRY_CODE);
    unsigned found = 0;
    downlink_halow_ap_list_channels(channel_print_cb, &found);
    printf("%u channel(s)\n", found);
    return 0;
}

static int cmd_gwcfg_save(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!s_cfg) {
        return 1;
    }

    provisioning_config_lock();
    char reason[96];
    esp_err_t err = provisioning_validate(s_cfg, reason, sizeof(reason));
    if (err == ESP_OK) {
        err = provisioning_save(s_cfg);
    }
    provisioning_config_unlock();

    if (err == ESP_ERR_INVALID_ARG) {
        printf("not saved: %s\n", reason);
    } else {
        printf("%s\n", err == ESP_OK ? "saved" : esp_err_to_name(err));
    }
    return err == ESP_OK ? 0 : 1;
}

static int cmd_gwcfg_reset(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!s_cfg) {
        return 1;
    }
    provisioning_config_lock();
    provisioning_get_defaults(s_cfg);
    provisioning_config_unlock();
    printf("reset to defaults in RAM; run 'gwcfg-save' then reboot to apply\n");
    return 0;
}

esp_err_t provisioning_register_console_commands(gw_config_t *cfg)
{
    s_cfg = cfg;

    const esp_console_cmd_t cmds[] = {
        { .command = "gwcfg-show", .help = "Show current gateway config", .hint = NULL, .func = &cmd_gwcfg_show },
        { .command = "gwcfg-set-node", .help = "Set node id: gwcfg-set-node <id>", .hint = NULL, .func = &cmd_gwcfg_set_node },
        { .command = "gwcfg-set-uplink", .help = "Set HaLow uplink STA config", .hint = NULL, .func = &cmd_gwcfg_set_uplink },
        { .command = "gwcfg-set-softap", .help = "Set local SoftAP config", .hint = NULL, .func = &cmd_gwcfg_set_softap },
        { .command = "gwcfg-set-role", .help = "Set node role: gwcfg-set-role <client|relay>", .hint = NULL, .func = &cmd_gwcfg_set_role },
        { .command = "gwcfg-set-uplink-static-ip", .help = "Set/clear a static IP on the uplink (relay leaves only)", .hint = NULL, .func = &cmd_gwcfg_set_uplink_static_ip },
        { .command = "gwcfg-set-wifi-uplink", .help = "Set the relay role's native Wi-Fi uplink to the Pi", .hint = NULL, .func = &cmd_gwcfg_set_wifi_uplink },
        { .command = "gwcfg-set-halow-ap", .help = "Set the relay role's HaLow AP downlink", .hint = NULL, .func = &cmd_gwcfg_set_halow_ap },
        { .command = "gwcfg-save", .help = "Persist current config to NVS", .hint = NULL, .func = &cmd_gwcfg_save },
        { .command = "gwcfg-reset", .help = "Reset in-RAM config to built-in defaults", .hint = NULL, .func = &cmd_gwcfg_reset },
        { .command = "gwcfg-status", .help = "Show live uplink/relay state, RSSI and IPs", .hint = NULL, .func = &cmd_gwcfg_status },
        { .command = "gwcfg-scan", .help = "Scan for HaLow APs on this build's channel list", .hint = NULL, .func = &cmd_gwcfg_scan },
        { .command = "gwcfg-list-halow-channels", .help = "List legal (op_class, s1g_chan_num) pairs for gwcfg-set-halow-ap", .hint = NULL, .func = &cmd_gwcfg_list_halow_channels },
        { .command = "gwcfg-radio", .help = "Print HaLow BCF/firmware versions (proves SPI works)", .hint = NULL, .func = &cmd_gwcfg_radio },
        { .command = "gwcfg-tasks", .help = "Show worst-case stack headroom per task", .hint = NULL, .func = &cmd_gwcfg_tasks },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t err = esp_console_cmd_register(&cmds[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t provisioning_start_console(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "xiao-gw>";

    esp_err_t err;
    /* Which peripheral esp_console attaches to must match sdkconfig's
     * ESP_CONSOLE_* choice (sdkconfig.defaults sets USB_SERIAL_JTAG for the
     * XIAO S3's native USB port) - mirrors ESP-IDF's own console example. */
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl);
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl);
#else
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    err = esp_console_new_repl_uart(&hw_config, &repl_config, &repl);
#endif
    if (err != ESP_OK) {
        return err;
    }

    return esp_console_start_repl(repl);
}
