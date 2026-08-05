#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "web_ui.h"

#include "provisioning.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "web_ui";

extern const char web_ui_html_start[] asm("_binary_web_ui_html_start");
extern const char web_ui_html_end[] asm("_binary_web_ui_html_end");

#define POST_BODY_MAX 1024

/* Live in-RAM config, shared with the console (provisioning.c) and app_main.c. */
static gw_config_t *s_cfg = NULL;

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const size_t len = web_ui_html_end - web_ui_html_start;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, web_ui_html_start, len);
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "node_id", s_cfg->node_id);

    cJSON *uplink = cJSON_AddObjectToObject(root, "uplink");
    cJSON_AddStringToObject(uplink, "ssid", s_cfg->uplink.ssid);
    cJSON_AddStringToObject(uplink, "security", provisioning_security_name(s_cfg->uplink.security));
    /* Passphrases are deliberately never echoed back - see web_ui.html's
     * "leave blank to keep current" fields and copy_json_str() below. */

    cJSON *softap = cJSON_AddObjectToObject(root, "softap");
    cJSON_AddStringToObject(softap, "ssid", s_cfg->softap.ssid);
    cJSON_AddNumberToObject(softap, "channel", s_cfg->softap.channel);

    cJSON *cot = cJSON_AddObjectToObject(root, "cot");
    cJSON_AddStringToObject(cot, "group", s_cfg->cot.group);
    cJSON_AddNumberToObject(cot, "port", s_cfg->cot.port);

    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);

    free(out);
    cJSON_Delete(root);
    return ESP_OK;
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

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    /* Validate into a scratch copy first so a rejected request never
     * partially mutates the live config. */
    gw_config_t work;
    memcpy(&work, s_cfg, sizeof(work));
    bool too_long = false;

    copy_json_str(root, "node_id", work.node_id, sizeof(work.node_id), &too_long);

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

    const cJSON *cot = cJSON_GetObjectItemCaseSensitive(root, "cot");
    if (!too_long && cJSON_IsObject(cot)) {
        copy_json_str(cot, "group", work.cot.group, sizeof(work.cot.group), &too_long);
        const cJSON *port = cJSON_GetObjectItemCaseSensitive(cot, "port");
        if (cJSON_IsNumber(port)) {
            work.cot.port = (uint16_t)port->valueint;
        }
    }

    cJSON_Delete(root);

    if (too_long) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "a field value is too long");
        return ESP_FAIL;
    }

    esp_err_t err = provisioning_save(&work);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "provisioning_save failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to save");
        return ESP_FAIL;
    }

    memcpy(s_cfg, &work, sizeof(*s_cfg));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"saved\"}");
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"rebooting\"}");
    esp_restart();
    return ESP_OK; /* unreachable */
}

esp_err_t web_ui_start(gw_config_t *cfg)
{
    s_cfg = cfg;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_get_handler },
        { .uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler },
        { .uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler },
        { .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to register %s: %s", routes[i].uri, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "web UI started");
    return ESP_OK;
}
