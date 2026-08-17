#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "web_ui.h"

#include "cot_relay.h"
#include "downlink_halow_ap.h"
#include "log_buffer.h"
#include "provisioning.h"
#include "task_stats.h"
#include "uplink_halow.h"
#include "uplink_wifi.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "web_ui";

extern const char web_ui_html_start[] asm("_binary_web_ui_html_start");
extern const char web_ui_html_end[] asm("_binary_web_ui_html_end");

#define POST_BODY_MAX 1024

/* Long enough for the JSON response to be written to the socket and read by
 * the browser before the CPU resets underneath it. */
#define REBOOT_DELAY_US 500000

/* A HaLow scan sweeps every channel in the regulatory domain and dwells on
 * each, so it is measured in seconds, not milliseconds. This bound has to sit
 * below the socket timeouts set in web_ui_start() or the browser gives up
 * before the handler answers. */
#define WEB_UI_SCAN_TIMEOUT_MS 8000

/* Live in-RAM config, shared with the console (provisioning.c) and app_main.c.
 * All access goes through provisioning_config_lock(). */
static gw_config_t *s_cfg = NULL;

/* The SoftAP netif, used to decide whether a request came from a local
 * client or from the mesh - see request_is_local(). */
static esp_netif_t *s_softap_netif = NULL;

/* Extracts the peer's IPv4 address from a request's socket. esp_http_server
 * may be listening on an IPv6 socket depending on lwIP config, in which case
 * IPv4 clients appear as IPv4-mapped addresses. */
static bool peer_ipv4(httpd_req_t *req, uint32_t *out_addr)
{
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) {
        return false;
    }

    struct sockaddr_storage peer;
    socklen_t len = sizeof(peer);
    if (getpeername(sockfd, (struct sockaddr *)&peer, &len) < 0) {
        return false;
    }

    if (peer.ss_family == AF_INET) {
        *out_addr = ((struct sockaddr_in *)&peer)->sin_addr.s_addr;
        return true;
    }
#if LWIP_IPV6
    if (peer.ss_family == AF_INET6) {
        struct sockaddr_in6 *peer6 = (struct sockaddr_in6 *)&peer;
        if (IN6_IS_ADDR_V4MAPPED(&peer6->sin6_addr)) {
            memcpy(out_addr, &peer6->sin6_addr.s6_addr[12], sizeof(*out_addr));
            return true;
        }
    }
#endif
    return false;
}

/* httpd_start() binds every interface, so once the HaLow uplink is up these
 * endpoints would otherwise be reachable from the entire mesh - including
 * unauthenticated POST /api/config and POST /api/reboot. There's no auth on
 * this UI yet (design/ROADMAP.md item 1), so the SoftAP subnet *is* the
 * authorization boundary: enforce it explicitly rather than relying on the
 * mesh being friendly.
 *
 * Fails closed. If the SoftAP netif is missing or has no address, nothing is
 * on its subnet anyway, so there is no client this could lock out. */
static bool request_is_local(httpd_req_t *req)
{
    if (s_softap_netif == NULL) {
        return false;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_softap_netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return false;
    }

    uint32_t peer_addr;
    if (!peer_ipv4(req, &peer_addr)) {
        return false;
    }

    return (peer_addr & ip_info.netmask.addr) == (ip_info.ip.addr & ip_info.netmask.addr);
}

/* The request's Host header must be the SoftAP's own address. This is the
 * anti-DNS-rebinding half of the interim CSRF defence (see reject_if_remote
 * below): a rebinding attack resolves an attacker's hostname to this device's
 * address, so the browser's requests are same-origin from its own point of
 * view and arrive here with full local-peer credentials - but carrying
 * Host: attacker.example, which is the one part of the request the attacker
 * cannot forge away.
 *
 * Fails closed on a missing or oversized Host header (HTTP/1.1 requires one;
 * every browser and curl sends it). If mDNS or a captive-portal hostname is
 * ever added (ROADMAP item 4), this check must learn those names too or the
 * UI becomes unreachable through them. */
static bool host_is_self(httpd_req_t *req)
{
    if (s_softap_netif == NULL) {
        return false;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_softap_netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return false;
    }

    char self[16];
    snprintf(self, sizeof(self), IPSTR, IP2STR(&ip_info.ip));

    char host[32]; /* fits "255.255.255.255:65535"; anything longer isn't us */
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }

    size_t self_len = strlen(self);
    if (strncmp(host, self, self_len) != 0) {
        return false;
    }
    /* Exact match, or the same address with an explicit :port. */
    return host[self_len] == '\0' || host[self_len] == ':';
}

/* Guard for every handler. Returns true if the request should be refused,
 * having already sent the error response.
 *
 * Checks two independent things: the peer must be on the SoftAP subnet
 * (authorization boundary while there's no auth), and the Host header must
 * name this device (anti-DNS-rebinding, see host_is_self). Both are interim
 * hardening, not authentication - a hostile client on the SoftAP can still
 * do everything the UI can until ROADMAP item 1 lands. */
static bool reject_if_remote(httpd_req_t *req)
{
    if (request_is_local(req)) {
        if (host_is_self(req)) {
            return false;
        }
        ESP_LOGW(TAG, "refused %s with a foreign Host header", req->uri);
    } else {
        ESP_LOGW(TAG, "refused %s from outside the SoftAP subnet", req->uri);
    }
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "config is only reachable from the local Wi-Fi");
    return true;
}

/* Guard for state-changing POSTs, after reject_if_remote. Requiring a JSON
 * Content-Type makes a cross-origin fetch() a non-"simple" CORS request, so
 * the browser sends an OPTIONS preflight first - which nothing here answers
 * with Access-Control-Allow-* headers, so the POST itself is never sent.
 * Without this, any web page open on a SoftAP client can fire a text/plain
 * POST at these endpoints with no preflight at all (the handlers never read
 * the Content-Type, so the body would parse fine). Complements host_is_self:
 * that one stops rebinding (where the request *is* same-origin to the
 * browser), this one stops plain cross-origin forgery. */
static bool reject_if_not_json(httpd_req_t *req)
{
    char ctype[64];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype)) == ESP_OK &&
        strncasecmp(ctype, "application/json", 16) == 0) {
        return false;
    }
    ESP_LOGW(TAG, "refused %s without a JSON Content-Type", req->uri);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content-Type must be application/json");
    return true;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (reject_if_remote(req)) {
        return ESP_FAIL;
    }
    const size_t len = web_ui_html_end - web_ui_html_start;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, web_ui_html_start, len);
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    if (reject_if_remote(req)) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    provisioning_config_lock();
    cJSON_AddStringToObject(root, "node_id", s_cfg->node_id);
    cJSON_AddStringToObject(root, "role", provisioning_role_name(s_cfg->role));

    cJSON *uplink = cJSON_AddObjectToObject(root, "uplink");
    cJSON_AddStringToObject(uplink, "ssid", s_cfg->uplink.ssid);
    cJSON_AddStringToObject(uplink, "security", provisioning_security_name(s_cfg->uplink.security));
    /* Passphrases are deliberately never echoed back - see web_ui.html's
     * "leave blank to keep current" fields and copy_json_str() below. */
    cJSON_AddBoolToObject(uplink, "use_static_ip", s_cfg->uplink.use_static_ip);
    cJSON_AddStringToObject(uplink, "static_ip", s_cfg->uplink.static_ip);
    cJSON_AddStringToObject(uplink, "static_gateway", s_cfg->uplink.static_gateway);
    cJSON_AddStringToObject(uplink, "static_netmask", s_cfg->uplink.static_netmask);

    cJSON *softap = cJSON_AddObjectToObject(root, "softap");
    cJSON_AddStringToObject(softap, "ssid", s_cfg->softap.ssid);
    cJSON_AddNumberToObject(softap, "channel", s_cfg->softap.channel);

    cJSON *wifi_uplink = cJSON_AddObjectToObject(root, "wifi_uplink");
    cJSON_AddStringToObject(wifi_uplink, "ssid", s_cfg->wifi_uplink.ssid);

    cJSON *halow_ap = cJSON_AddObjectToObject(root, "halow_ap");
    cJSON_AddStringToObject(halow_ap, "ssid", s_cfg->halow_ap.ssid);
    cJSON_AddStringToObject(halow_ap, "security", provisioning_security_name(s_cfg->halow_ap.security));
    cJSON_AddNumberToObject(halow_ap, "op_class", s_cfg->halow_ap.op_class);
    cJSON_AddNumberToObject(halow_ap, "s1g_chan_num", s_cfg->halow_ap.s1g_chan_num);
    cJSON_AddNumberToObject(halow_ap, "max_stas", s_cfg->halow_ap.max_stas);
    cJSON_AddStringToObject(halow_ap, "ip", s_cfg->halow_ap.ip);
    cJSON_AddStringToObject(halow_ap, "netmask", s_cfg->halow_ap.netmask);

    cJSON *cot = cJSON_AddObjectToObject(root, "cot");
    cJSON_AddStringToObject(cot, "group", s_cfg->cot.group);
    cJSON_AddNumberToObject(cot, "port", s_cfg->cot.port);
    provisioning_config_unlock();

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    return err;
}

/* Adds "<name>": "a.b.c.d" for a netif's current address, or null if the
 * interface has no address yet (normal for the uplink before DHCP). */
static void add_netif_ip(cJSON *parent, const char *name, esp_netif_t *netif)
{
    esp_netif_ip_info_t ip_info;
    if (netif == NULL || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        cJSON_AddNullToObject(parent, name);
        return;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip_info.ip));
    cJSON_AddStringToObject(parent, name, buf);
}

/* Live device state, polled by the page. This is the bring-up diagnostic
 * surface: without it, "is the uplink actually up, and did the relay start?"
 * is only answerable over the serial console, which is exactly the thing you
 * don't have when the device is deployed somewhere awkward. Read-only - it
 * exposes no secrets (no passphrases), but is still gated to SoftAP clients
 * like every other handler. */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    if (reject_if_remote(req)) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    provisioning_config_lock();
    cJSON_AddStringToObject(root, "node_id", s_cfg->node_id);
    gw_node_role_t role = s_cfg->role;
    cJSON_AddStringToObject(root, "role", provisioning_role_name(role));
    cJSON *cot = cJSON_AddObjectToObject(root, "cot");
    cJSON_AddStringToObject(cot, "group", s_cfg->cot.group);
    cJSON_AddNumberToObject(cot, "port", s_cfg->cot.port);
    provisioning_config_unlock();

    cJSON_AddBoolToObject(cot, "running", cot_relay_is_running());

    cJSON *uplink = cJSON_AddObjectToObject(root, "uplink");
    /* "connected" tracks the DHCP lease, not raw 802.11 association - the
     * same distinction ip_event_handler() draws, and the one that actually
     * determines whether NAT and the relay could start. */
    cJSON_AddBoolToObject(uplink, "connected", uplink_halow_is_connected());

    /* "state" is the finer-grained version, and the one that makes this panel
     * a bring-up instrument rather than a health light: "searching" and
     * "associated, no lease" are the two milestones of step 3 in
     * design/HARDWARE.md failing respectively, with entirely different causes
     * (RF/country/credentials vs. DHCP on the Pi). A single boolean cannot
     * tell them apart. */
    uplink_link_state_t link_state = uplink_halow_get_link_state();
    cJSON_AddStringToObject(uplink, "state", uplink_halow_link_state_name(link_state));
    cJSON_AddBoolToObject(uplink, "associated", link_state >= UPLINK_LINK_ASSOCIATED);
    cJSON_AddBoolToObject(uplink, "radio_ready", uplink_halow_is_ready());
    /* Lets the page tell "you haven't set this up yet" apart from "setup is
     * done and something is wrong", which are the same shape of empty status
     * panel but need opposite advice from the operator. */
    cJSON_AddBoolToObject(uplink, "configured", link_state != UPLINK_LINK_UNCONFIGURED);

    /* Null rather than a sentinel number when unknown, so the page can render
     * "-" instead of a misleading -2147483648. */
    int32_t rssi = uplink_halow_get_rssi();
    if (rssi == INT32_MIN) {
        cJSON_AddNullToObject(uplink, "rssi");
    } else {
        cJSON_AddNumberToObject(uplink, "rssi", rssi);
    }

    add_netif_ip(uplink, "ip", uplink_halow_get_netif());

    cJSON *softap = cJSON_AddObjectToObject(root, "softap");
    add_netif_ip(softap, "ip", s_softap_netif);
    wifi_sta_list_t sta_list;
    cJSON_AddNumberToObject(softap, "clients",
                            esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK ? sta_list.num : -1);

    /* GW_ROLE_RELAY's status - reported unconditionally, same reasoning as
     * the rest of this endpoint always including every section: the getters
     * below are safe to call regardless of active role (uplink_wifi_init()/
     * downlink_halow_ap_init() simply never ran on a GW_ROLE_CLIENT node, so
     * they report their own "not configured"/not-ready idle state rather
     * than anything misleading), and it's simpler for the page to always
     * receive both shapes and render whichever the "role" field says is
     * live. */
    cJSON *wifi_uplink = cJSON_AddObjectToObject(root, "wifi_uplink");
    uplink_wifi_link_state_t wifi_state = uplink_wifi_get_link_state();
    cJSON_AddStringToObject(wifi_uplink, "state", uplink_wifi_link_state_name(wifi_state));
    cJSON_AddBoolToObject(wifi_uplink, "connected", uplink_wifi_is_connected());
    int8_t wifi_rssi = uplink_wifi_get_rssi();
    if (wifi_rssi == INT8_MIN) {
        cJSON_AddNullToObject(wifi_uplink, "rssi");
    } else {
        cJSON_AddNumberToObject(wifi_uplink, "rssi", wifi_rssi);
    }
    add_netif_ip(wifi_uplink, "ip", uplink_wifi_get_netif());

    cJSON *halow_ap = cJSON_AddObjectToObject(root, "halow_ap");
    cJSON_AddBoolToObject(halow_ap, "ready", downlink_halow_ap_is_ready());
    /* Best-effort only - mmhalow_wifi_start() returns void, so this reflects
     * "we called it", not confirmation the AP is actually on air. See
     * downlink_halow_ap.h. */
    cJSON_AddBoolToObject(halow_ap, "started", downlink_halow_ap_is_started());
    add_netif_ip(halow_ap, "ip", downlink_halow_ap_get_netif());

    cJSON *sys = cJSON_AddObjectToObject(root, "system");
    cJSON_AddNumberToObject(sys, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(sys, "heap_free", esp_get_free_heap_size());
    cJSON_AddNumberToObject(sys, "heap_min", esp_get_minimum_free_heap_size());
    /* Build-time regulatory domain. Worth surfacing because it cannot be
     * changed at runtime and a mismatch with the mesh Pi is the failure that
     * blocks association outright (design/PI_SIDE.md item 3). */
    cJSON_AddStringToObject(sys, "country", CONFIG_HALOW_COUNTRY_CODE);

    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON_AddStringToObject(sys, "version", desc ? desc->version : "unknown");

    /* Which OTA slot is running - meaningless today (always ota_0) but the
     * first thing you want to see once OTA updates exist. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    cJSON_AddStringToObject(sys, "partition", running ? running->label : "unknown");

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    return err;
}

/* Copies a JSON string field into out (bounded, NUL-terminated) if present
 * and non-empty; a missing/empty field means "keep the current value" and
 * out is left untouched. Sets *too_long and leaves out untouched if the
 * value doesn't fit - callers must check this and reject the request. */
static void copy_json_str(const cJSON *parent, const char *key, char *out, size_t out_size, bool *too_long)
{
    *too_long = false;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL || item->valuestring[0] == '\0') {
        return;
    }
    if (strlen(item->valuestring) >= out_size) {
        *too_long = true;
        return;
    }
    strlcpy(out, item->valuestring, out_size);
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    if (reject_if_remote(req) || reject_if_not_json(req)) {
        return ESP_FAIL;
    }

    int total_len = req->content_len;
    if (total_len <= 0 || total_len >= POST_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large or empty");
        return ESP_FAIL;
    }

    char buf[POST_BODY_MAX];
    int cur_len = 0;
    while (cur_len < total_len) {
        int received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "failed to read body");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    /* cJSON parses nested containers recursively, and ESP-IDF compiles it with
     * the upstream default CJSON_NESTING_LIMIT of 1000 (components/json builds
     * cJSON.c with no override; cJSON.c v1.7.x checks the limit only *after*
     * recursing). 1000 frames is far more than this task's stack, so a ~1 KB
     * body of "[[[[..." would panic before the limit is ever reached. A valid
     * config body contains at most 6 objects (uplink, softap, wifi_uplink,
     * halow_ap, cot, plus root) and no arrays, so cap total containers well
     * below that before recursion depth can matter. */
    int container_budget = 16;
    for (const char *p = buf; *p != '\0'; p++) {
        if ((*p == '{' || *p == '[') && --container_budget < 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too many nested JSON containers");
            return ESP_FAIL;
        }
    }

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    /* Validate into a scratch copy first so a rejected request never
     * partially mutates the live config. */
    provisioning_config_lock();
    gw_config_t work;
    memcpy(&work, s_cfg, sizeof(work));
    provisioning_config_unlock();

    bool too_long = false;

    copy_json_str(root, "node_id", work.node_id, sizeof(work.node_id), &too_long);

    const cJSON *role = cJSON_GetObjectItemCaseSensitive(root, "role");
    if (cJSON_IsString(role) && role->valuestring != NULL) {
        work.role = provisioning_parse_role(role->valuestring);
    }

    const cJSON *uplink = cJSON_GetObjectItemCaseSensitive(root, "uplink");
    if (!too_long && cJSON_IsObject(uplink)) {
        copy_json_str(uplink, "ssid", work.uplink.ssid, sizeof(work.uplink.ssid), &too_long);
        if (!too_long) {
            copy_json_str(uplink, "psk", work.uplink.psk, sizeof(work.uplink.psk), &too_long);
        }
        const cJSON *sec = cJSON_GetObjectItemCaseSensitive(uplink, "security");
        if (cJSON_IsString(sec) && sec->valuestring != NULL) {
            work.uplink.security = provisioning_parse_security(sec->valuestring);
        }
        const cJSON *use_static = cJSON_GetObjectItemCaseSensitive(uplink, "use_static_ip");
        if (cJSON_IsBool(use_static)) {
            work.uplink.use_static_ip = cJSON_IsTrue(use_static);
        }
        if (!too_long) {
            copy_json_str(uplink, "static_ip", work.uplink.static_ip, sizeof(work.uplink.static_ip), &too_long);
        }
        if (!too_long) {
            copy_json_str(uplink, "static_gateway", work.uplink.static_gateway,
                          sizeof(work.uplink.static_gateway), &too_long);
        }
        if (!too_long) {
            copy_json_str(uplink, "static_netmask", work.uplink.static_netmask,
                          sizeof(work.uplink.static_netmask), &too_long);
        }
    }

    const cJSON *softap = cJSON_GetObjectItemCaseSensitive(root, "softap");
    if (!too_long && cJSON_IsObject(softap)) {
        copy_json_str(softap, "ssid", work.softap.ssid, sizeof(work.softap.ssid), &too_long);
        if (!too_long) {
            copy_json_str(softap, "psk", work.softap.psk, sizeof(work.softap.psk), &too_long);
        }
        const cJSON *channel = cJSON_GetObjectItemCaseSensitive(softap, "channel");
        if (cJSON_IsNumber(channel)) {
            work.softap.channel = (uint8_t)channel->valueint;
        }
    }

    const cJSON *wifi_uplink = cJSON_GetObjectItemCaseSensitive(root, "wifi_uplink");
    if (!too_long && cJSON_IsObject(wifi_uplink)) {
        copy_json_str(wifi_uplink, "ssid", work.wifi_uplink.ssid, sizeof(work.wifi_uplink.ssid), &too_long);
        if (!too_long) {
            copy_json_str(wifi_uplink, "psk", work.wifi_uplink.psk, sizeof(work.wifi_uplink.psk), &too_long);
        }
    }

    const cJSON *halow_ap = cJSON_GetObjectItemCaseSensitive(root, "halow_ap");
    if (!too_long && cJSON_IsObject(halow_ap)) {
        copy_json_str(halow_ap, "ssid", work.halow_ap.ssid, sizeof(work.halow_ap.ssid), &too_long);
        if (!too_long) {
            copy_json_str(halow_ap, "psk", work.halow_ap.psk, sizeof(work.halow_ap.psk), &too_long);
        }
        const cJSON *ap_sec = cJSON_GetObjectItemCaseSensitive(halow_ap, "security");
        if (cJSON_IsString(ap_sec) && ap_sec->valuestring != NULL) {
            work.halow_ap.security = provisioning_parse_security(ap_sec->valuestring);
        }
        const cJSON *op_class = cJSON_GetObjectItemCaseSensitive(halow_ap, "op_class");
        if (cJSON_IsNumber(op_class)) {
            work.halow_ap.op_class = (int16_t)op_class->valueint;
        }
        const cJSON *chan_num = cJSON_GetObjectItemCaseSensitive(halow_ap, "s1g_chan_num");
        if (cJSON_IsNumber(chan_num)) {
            work.halow_ap.s1g_chan_num = (uint8_t)chan_num->valueint;
        }
        const cJSON *max_stas = cJSON_GetObjectItemCaseSensitive(halow_ap, "max_stas");
        if (cJSON_IsNumber(max_stas)) {
            work.halow_ap.max_stas = (uint8_t)max_stas->valueint;
        }
        if (!too_long) {
            copy_json_str(halow_ap, "ip", work.halow_ap.ip, sizeof(work.halow_ap.ip), &too_long);
        }
        if (!too_long) {
            copy_json_str(halow_ap, "netmask", work.halow_ap.netmask, sizeof(work.halow_ap.netmask), &too_long);
        }
    }

    const cJSON *cot = cJSON_GetObjectItemCaseSensitive(root, "cot");
    if (!too_long && cJSON_IsObject(cot)) {
        copy_json_str(cot, "group", work.cot.group, sizeof(work.cot.group), &too_long);
        const cJSON *port = cJSON_GetObjectItemCaseSensitive(cot, "port");
        if (cJSON_IsNumber(port)) {
            work.cot.port = (port->valueint >= 0 && port->valueint <= 65535)
                                ? (uint16_t)port->valueint
                                : 0; /* out of range - provisioning_validate() rejects below */
        }
    }

    cJSON_Delete(root);

    if (too_long) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "a field value is too long");
        return ESP_FAIL;
    }

    /* Semantic validation on top of the length checks above: this is what
     * stops a 4-character Wi-Fi passphrase or channel 99 from being saved
     * and taking the SoftAP - and with it this very UI - down at next boot. */
    char reason[96];
    if (provisioning_validate(&work, reason, sizeof(reason)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, reason);
        return ESP_FAIL;
    }

    /* Save and write-back under one lock hold so NVS and the live config can't
     * end up disagreeing if a console edit interleaves (the console already
     * holds this lock across its own NVS write in cmd_gwcfg_save, so blocking
     * briefly on flash I/O here is precedented). A console edit made while the
     * request was still being parsed is still last-writer-wins - inherent to
     * two unauthenticated writers, acceptable until auth adds sessions. */
    provisioning_config_lock();
    esp_err_t err = provisioning_save(&work);
    if (err == ESP_OK) {
        memcpy(s_cfg, &work, sizeof(*s_cfg));
    }
    provisioning_config_unlock();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "provisioning_save failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to save");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"saved\"}");
}

/* Serves the in-RAM log ring as plain text, newest content last.
 *
 * Read-only and SoftAP-gated like everything else. Note the logs may contain
 * SSIDs and IP addresses but never passphrases - nothing in this firmware logs
 * one, and that must stay true. */
static esp_err_t log_get_handler(httpd_req_t *req)
{
    if (reject_if_remote(req)) {
        return ESP_FAIL;
    }

    size_t cap = log_buffer_capacity() + 1;
    char *buf = malloc(cap);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    size_t len = log_buffer_read(buf, cap);
    httpd_resp_set_type(req, "text/plain");
    esp_err_t err = httpd_resp_send(req, buf, len);
    free(buf);
    return err;
}

static void scan_result_cb(const uplink_scan_result_t *result, void *ctx)
{
    cJSON *array = (cJSON *)ctx;

    cJSON *item = cJSON_CreateObject();
    if (item == NULL) {
        return;
    }
    cJSON_AddStringToObject(item, "ssid", result->ssid);
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x", result->bssid[0],
             result->bssid[1], result->bssid[2], result->bssid[3], result->bssid[4],
             result->bssid[5]);
    cJSON_AddStringToObject(item, "bssid", bssid);
    cJSON_AddNumberToObject(item, "rssi", result->rssi);
    /* kHz as a whole number, not MHz as a fraction: cJSON prints
     * integer-valued numbers with %d and everything else with %g, and %g is
     * one `CONFIG_LIBC_NEWLIB_NANO_FORMAT=y` away from emitting malformed
     * JSON. The page divides for display. */
    cJSON_AddNumberToObject(item, "freq_khz", (double)(result->freq_hz / 1000u));
    cJSON_AddNumberToObject(item, "bw_mhz", result->bw_mhz);
    cJSON_AddItemToArray(array, item);
}

/* Runs a HaLow scan and returns what it found.
 *
 * This is the endpoint that answers "is the Pi's AP even there?" without a
 * serial cable or a Pi-side capture. Two things worth knowing about the
 * result, both surfaced in the page's help text:
 *
 *  - it only covers channels legal in this build's CONFIG_HALOW_COUNTRY_CODE,
 *    so an empty list is evidence about the region setting as much as about
 *    the AP;
 *  - scanning briefly takes the radio away from the uplink, so a connected
 *    node may drop and re-associate afterwards. That's acceptable for a
 *    deliberate operator action, and the reconnect loop handles it.
 */
static esp_err_t scan_post_handler(httpd_req_t *req)
{
    if (reject_if_remote(req) || reject_if_not_json(req)) {
        return ESP_FAIL;
    }

    /* esp_http_server's httpd_err_code_t has no 503 or 409 (the enum stops at
     * 431 and omits both), so these use 500 with a specific message rather
     * than a status code the browser could act on. The message is what the
     * page surfaces to the operator, and it's the part that matters here. */
    if (!uplink_halow_is_ready()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "the HaLow radio isn't initialized - nothing to scan with");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *array = root ? cJSON_AddArrayToObject(root, "aps") : NULL;
    if (array == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    esp_err_t scan_err = uplink_halow_scan(scan_result_cb, array, WEB_UI_SCAN_TIMEOUT_MS);
    if (scan_err == ESP_ERR_INVALID_STATE) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "a scan is already running");
        return ESP_FAIL;
    }

    /* A timeout still delivers whatever was found before the deadline, so it's
     * reported as a partial success rather than an error - partial scan
     * results are exactly as useful as complete ones here. */
    cJSON_AddBoolToObject(root, "complete", scan_err == ESP_OK);
    cJSON_AddStringToObject(root, "country", CONFIG_HALOW_COUNTRY_CODE);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    return err;
}

static void channel_collect_cb(const halow_ap_channel_t *chan, void *ctx)
{
    cJSON *array = (cJSON *)ctx;
    cJSON *item = cJSON_CreateObject();
    if (item == NULL) {
        return;
    }
    cJSON_AddNumberToObject(item, "op_class", chan->op_class);
    cJSON_AddNumberToObject(item, "chan_num", chan->s1g_chan_num);
    cJSON_AddNumberToObject(item, "freq_hz", chan->freq_hz);
    cJSON_AddNumberToObject(item, "bw_mhz", chan->bw_mhz);
    cJSON_AddItemToArray(array, item);
}

/* Returns the list of legal (op_class, s1g_chan_num) channels for this build's
 * regulatory domain (US 902-928MHz) so the web UI can populate channel dropdowns. */
static esp_err_t channels_get_handler(httpd_req_t *req)
{
    if (reject_if_remote(req)) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *array = root ? cJSON_AddArrayToObject(root, "channels") : NULL;
    if (array == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    cJSON_AddStringToObject(root, "country", CONFIG_HALOW_COUNTRY_CODE);
    downlink_halow_ap_list_channels(channel_collect_cb, array);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    return err;
}

static void task_stack_collect_cb(const task_stack_info_t *info, void *ctx)
{
    cJSON *array = (cJSON *)ctx;

    cJSON *item = cJSON_CreateObject();
    if (item == NULL) {
        return;
    }
    cJSON_AddStringToObject(item, "name", info->name);
    cJSON_AddBoolToObject(item, "present", info->present);
    cJSON_AddNumberToObject(item, "stack", (double)info->stack_total);
    /* Absent tasks report free = 0, which the page must not draw as "0 bytes
     * left" - hence the explicit `present` flag above rather than leaving the
     * client to infer it from a sentinel value. */
    cJSON_AddNumberToObject(item, "free", (double)(info->present ? info->stack_free_min : 0));
    cJSON_AddItemToArray(array, item);
}

/* Worst-case stack headroom per task, the same data as the console's
 * gwcfg-tasks.
 *
 * Deliberately its own endpoint rather than another field on /api/status: the
 * page polls status on a timer, and this costs an xTaskGetHandle() name walk
 * plus a stack scan per row (see task_stats.h). It's a diagnostic you open,
 * not a gauge that ticks.
 *
 * Reachable without a serial cable on purpose. The overflow this exists to
 * predict took down a node that had no console attached, which is the normal
 * case for anything deployed - see design/ROADMAP.md item 8. */
static esp_err_t tasks_get_handler(httpd_req_t *req)
{
    if (reject_if_remote(req)) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *array = root ? cJSON_AddArrayToObject(root, "tasks") : NULL;
    if (array == NULL) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    task_stats_each_stack(task_stack_collect_cb, array);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    free(out);
    return err;
}

static void reboot_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "rebooting on web UI request");
    esp_restart();
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    if (reject_if_remote(req) || reject_if_not_json(req)) {
        return ESP_FAIL;
    }

    /* Restarting inline would reset the CPU before the response drains out
     * of the socket, so the browser sees a connection reset instead of the
     * acknowledgement. Hand off to a one-shot timer and return normally. */
    const esp_timer_create_args_t args = {
        .callback = reboot_timer_cb,
        .name = "web_ui_reboot",
    };
    esp_timer_handle_t timer;
    esp_err_t err = esp_timer_create(&args, &timer);
    if (err == ESP_OK) {
        err = esp_timer_start_once(timer, REBOOT_DELAY_US);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "couldn't schedule reboot: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to schedule reboot");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"rebooting\"}");
}

esp_err_t web_ui_start(gw_config_t *cfg, esp_netif_t *softap_netif)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (softap_netif == NULL) {
        /* Without it there's no subnet to authorize against, and every
         * request would be refused - starting the server would be
         * misleading. */
        ESP_LOGE(TAG, "no SoftAP netif - refusing to start an unreachable web UI");
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg = cfg;
    s_softap_netif = softap_netif;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    /* The scan handler blocks for up to WEB_UI_SCAN_TIMEOUT_MS; the default
     * 5s socket timeouts would abort the connection before it can answer. */
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    /* Default is 8 and there are 8 routes below - raised so adding one doesn't
     * fail registration at runtime instead of at compile time. */
    config.max_uri_handlers = 12;
    /* The status and scan handlers build and print whole cJSON trees on this
     * stack, on top of the default 4KB. (Scan *results* are delivered from the
     * driver's own task - see uplink_halow.h - but assembling and serializing
     * the response happens here.) */
    config.stack_size = GW_STACK_WEB_UI;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_get_handler },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler },
        { .uri = "/api/channels", .method = HTTP_GET, .handler = channels_get_handler },
        { .uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler },
        { .uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler },
        { .uri = "/api/log", .method = HTTP_GET, .handler = log_get_handler },
        { .uri = "/api/tasks", .method = HTTP_GET, .handler = tasks_get_handler },
        { .uri = "/api/scan", .method = HTTP_POST, .handler = scan_post_handler },
        { .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to register %s: %s", routes[i].uri, esp_err_to_name(err));
            httpd_stop(server);
            return err;
        }
    }

    ESP_LOGI(TAG, "web UI started (SoftAP clients only)");
    return ESP_OK;
}
