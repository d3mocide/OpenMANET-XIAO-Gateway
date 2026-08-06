#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "provisioning.h"

#include "cot_relay.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "uplink_halow.h"

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

/* Bring-up commands. These don't touch config at all - they exist because the
 * serial console is the one interface guaranteed to work when the SoftAP
 * hasn't come up, and the questions they answer ("does the radio respond?",
 * "is the AP visible?", "how strong?") are the first three asked of a node
 * that isn't associating. */
static int cmd_gwcfg_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uplink_link_state_t state = uplink_halow_get_link_state();
    printf("uplink state  : %s\n", uplink_halow_link_state_name(state));

    int32_t rssi = uplink_halow_get_rssi();
    if (rssi == INT32_MIN) {
        printf("uplink RSSI   : (not associated)\n");
    } else {
        printf("uplink RSSI   : %" PRId32 " dBm\n", rssi);
    }

    esp_netif_t *uplink_netif = uplink_halow_get_netif();
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
        { .command = "gwcfg-status", .help = "Show live uplink/relay state, RSSI and IPs", .hint = NULL, .func = &cmd_gwcfg_status },
        { .command = "gwcfg-scan", .help = "Scan for HaLow APs on this build's channel list", .hint = NULL, .func = &cmd_gwcfg_scan },
        { .command = "gwcfg-radio", .help = "Print HaLow BCF/firmware versions (proves SPI works)", .hint = NULL, .func = &cmd_gwcfg_radio },
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
