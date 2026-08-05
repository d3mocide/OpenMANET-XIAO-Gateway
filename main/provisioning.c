#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "provisioning.h"

#include "esp_console.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "provisioning";

#define GWCFG_NVS_NAMESPACE "gwcfg"
#define GWCFG_NVS_KEY       "config"

/* Points at the app's live in-RAM config so console commands can edit it
 * directly; set by provisioning_register_console_commands(). */
static gw_config_t *s_cfg = NULL;

void provisioning_get_defaults(gw_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

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
    }

    return ESP_OK;
}

esp_err_t provisioning_save(const gw_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(GWCFG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, GWCFG_NVS_KEY, cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void print_config(const gw_config_t *cfg)
{
    static const char *security_names[] = { "open", "owe", "sae" };

    printf("node_id       : %s\n", cfg->node_id);
    printf("uplink.ssid   : %s\n", cfg->uplink.ssid);
    printf("uplink.security: %s\n", security_names[cfg->uplink.security]);
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
    print_config(s_cfg);
    return 0;
}

static int cmd_gwcfg_set_node(int argc, char **argv)
{
    if (!s_cfg || argc != 2) {
        printf("usage: gwcfg-set-node <node_id>\n");
        return 1;
    }
    strlcpy(s_cfg->node_id, argv[1], sizeof(s_cfg->node_id));
    return 0;
}

/* HaLow (802.11ah) has no WPA2-PSK mode - only open/OWE/SAE, confirmed
 * against the real morsemicro/halow SDK's enum mmwlan_security_type. */
static gw_security_mode_t parse_security(const char *s)
{
    if (strcmp(s, "owe") == 0) {
        return GW_SECURITY_OWE;
    }
    if (strcmp(s, "sae") == 0) {
        return GW_SECURITY_SAE;
    }
    return GW_SECURITY_OPEN;
}

static int cmd_gwcfg_set_uplink(int argc, char **argv)
{
    if (!s_cfg || argc < 4) {
        printf("usage: gwcfg-set-uplink <ssid> <psk|-> <open|owe|sae>\n");
        return 1;
    }
    strlcpy(s_cfg->uplink.ssid, argv[1], sizeof(s_cfg->uplink.ssid));
    strlcpy(s_cfg->uplink.psk, strcmp(argv[2], "-") == 0 ? "" : argv[2], sizeof(s_cfg->uplink.psk));
    s_cfg->uplink.security = parse_security(argv[3]);
    printf("uplink config updated in RAM; run 'gwcfg-save' then reboot to apply\n");
    return 0;
}

static int cmd_gwcfg_set_softap(int argc, char **argv)
{
    if (!s_cfg || argc < 3) {
        printf("usage: gwcfg-set-softap <ssid> <psk|-> [channel]\n");
        return 1;
    }
    strlcpy(s_cfg->softap.ssid, argv[1], sizeof(s_cfg->softap.ssid));
    strlcpy(s_cfg->softap.psk, strcmp(argv[2], "-") == 0 ? "" : argv[2], sizeof(s_cfg->softap.psk));
    if (argc >= 4) {
        s_cfg->softap.channel = (uint8_t)atoi(argv[3]);
    }
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
    esp_err_t err = provisioning_save(s_cfg);
    printf("%s\n", err == ESP_OK ? "saved" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_gwcfg_reset(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!s_cfg) {
        return 1;
    }
    provisioning_get_defaults(s_cfg);
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
