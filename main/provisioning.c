#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "provisioning.h"

#include "esp_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "provisioning";

#define GWCFG_NVS_NAMESPACE "gwcfg"
#define GWCFG_NVS_KEY       "config"

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

    /* Placeholder uplink values - DESIGN.md §6.3 flags the real SSID/
     * security mode as unconfirmed against a live Pi. Must be set via
     * `gwcfg-set-uplink` (or reflashed defaults) before first deployment. */
    strlcpy(cfg->uplink.ssid, "openmanet-halow", sizeof(cfg->uplink.ssid));
    cfg->uplink.psk[0] = '\0';
    cfg->uplink.security = GW_SECURITY_OPEN;

    /* Local client-facing SoftAP (DESIGN.md §4.2), default subnet matches
     * the example in the design doc's network diagram. */
    strlcpy(cfg->softap.ssid, "xiao-gateway", sizeof(cfg->softap.ssid));
    strlcpy(cfg->softap.psk, "openmanet", sizeof(cfg->softap.psk));
    cfg->softap.channel = 6;
    cfg->softap.max_connections = 8;
    cfg->softap.use_custom_subnet = true;
    strlcpy(cfg->softap.ip, "192.168.50.1", sizeof(cfg->softap.ip));
    strlcpy(cfg->softap.gateway, "192.168.50.1", sizeof(cfg->softap.gateway));
    strlcpy(cfg->softap.netmask, "255.255.255.0", sizeof(cfg->softap.netmask));

    /* ATAK CoT multicast group (DESIGN.md §4.3/§5.4). */
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

    if (cfg->uplink.ssid[0] == '\0') {
        GW_REJECT("uplink SSID must not be empty");
    }

    /* HaLow SAE needs a passphrase; open/OWE must not carry one. (No length
     * floor asserted for SAE - unlike WPA2-PSK, SAE does not specify one.) */
    if (cfg->uplink.security == GW_SECURITY_SAE && cfg->uplink.psk[0] == '\0') {
        GW_REJECT("uplink security is SAE but no passphrase is set");
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

static void print_config(const gw_config_t *cfg)
{
    printf("node_id       : %s\n", cfg->node_id);
    printf("uplink.ssid   : %s\n", cfg->uplink.ssid);
    printf("uplink.security: %s\n", provisioning_security_name(cfg->uplink.security));
    printf("softap.ssid   : %s\n", cfg->softap.ssid);
    printf("softap.channel: %u\n", cfg->softap.channel);
    printf("softap.subnet : %s\n",
           cfg->softap.use_custom_subnet ? cfg->softap.ip : "192.168.4.1 (esp-netif default)");
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
    provisioning_config_lock();
    strlcpy(s_cfg->node_id, argv[1], sizeof(s_cfg->node_id));
    provisioning_config_unlock();
    return 0;
}

static int cmd_gwcfg_set_uplink(int argc, char **argv)
{
    if (!s_cfg || argc < 4) {
        printf("usage: gwcfg-set-uplink <ssid> <psk|-> <open|owe|sae>\n");
        return 1;
    }
    provisioning_config_lock();
    strlcpy(s_cfg->uplink.ssid, argv[1], sizeof(s_cfg->uplink.ssid));
    strlcpy(s_cfg->uplink.psk, strcmp(argv[2], "-") == 0 ? "" : argv[2], sizeof(s_cfg->uplink.psk));
    s_cfg->uplink.security = provisioning_parse_security(argv[3]);
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
        { .command = "gwcfg-save", .help = "Persist current config to NVS", .hint = NULL, .func = &cmd_gwcfg_save },
        { .command = "gwcfg-reset", .help = "Reset in-RAM config to built-in defaults", .hint = NULL, .func = &cmd_gwcfg_reset },
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
