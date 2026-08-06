#include <inttypes.h>
#include <string.h>

#include "uplink_halow.h"

#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "mmhalow.h"

static const char *TAG = "uplink_halow";

#define RECONNECT_BACKOFF_MIN_MS 1000
#define RECONNECT_BACKOFF_MAX_MS 60000
#define HALOW_CONNECT_TIMEOUT_MS 15000
#define LINK_POLL_INTERVAL_MS    2000

/* How long to wait for a DHCP lease after 802.11 association before
 * concluding the lease isn't coming. Association succeeding while DHCP never
 * completes is a realistic failure (no/misconfigured DHCP server on the Pi
 * side), and without a bound here the task would sit in its "waiting for
 * lease" poll loop forever, never retrying and never reporting anything. */
#define DHCP_LEASE_TIMEOUT_MS 30000

/* On the first timeout the DHCP client is restarted in place, which fixes
 * the transient cases cheaply. If a lease still doesn't arrive, fall all the
 * way back to re-associating. */
#define DHCP_RESTART_ATTEMPTS 2

static gw_uplink_config_t s_cfg;
static esp_netif_t *s_netif = NULL;
static bool s_ready = false;               /* uplink_halow_init() succeeded */
static volatile bool s_associated = false; /* 802.11 association only */
static volatile bool s_associating = false;
static volatile bool s_has_ip = false;     /* the signal callers actually want */
static uplink_halow_state_cb_t s_cb = NULL;
static void *s_cb_ctx = NULL;
static TaskHandle_t s_reconnect_task_handle = NULL;
static SemaphoreHandle_t s_connect_sem = NULL;

/* Serializes uplink_halow_scan(): the driver takes one scan request at a time,
 * and the callbacks below write into a single shared context.
 *
 * That context and its completion semaphore are deliberately static rather
 * than stack-allocated in uplink_halow_scan(). A scan that times out leaves
 * the driver still holding the `cb_arg` pointer we handed it, and a late
 * callback writing through a pointer to a returned stack frame - or to a
 * semaphore we deleted - is a use-after-free. Keeping the storage alive for
 * the life of the process makes a late callback harmless, and the generation
 * counter makes it a no-op rather than a result misattributed to whatever scan
 * is running by then. */
static SemaphoreHandle_t s_scan_lock = NULL;
static SemaphoreHandle_t s_scan_done = NULL;

static struct scan_ctx {
    uplink_scan_cb_t cb;
    void *ctx;
    uint32_t count;
    uint32_t generation;
} s_scan;

/* HaLow (802.11ah) has no WPA2-PSK - confirmed against mmwlan.h's
 * enum mmwlan_security_type (MMWLAN_OPEN / MMWLAN_OWE / MMWLAN_SAE only). */
static enum mmwlan_security_type halow_security_from_gw(gw_security_mode_t sec)
{
    switch (sec) {
    case GW_SECURITY_OWE:
        return MMWLAN_OWE;
    case GW_SECURITY_SAE:
        return MMWLAN_SAE;
    case GW_SECURITY_OPEN:
    default:
        return MMWLAN_OPEN;
    }
}

/* Registered once with mmhalow_connect() and called on every STA state
 * transition thereafter (association only - not IP). Drives the
 * connect-with-timeout wrapper below via s_connect_sem. */
static void mm_sta_state_cb(enum mmwlan_sta_state sta_state)
{
    switch (sta_state) {
    case MMWLAN_STA_CONNECTED:
        s_associated = true;
        s_associating = false;
        if (s_connect_sem != NULL) {
            xSemaphoreGive(s_connect_sem);
        }
        break;
    case MMWLAN_STA_CONNECTING:
        s_associating = true;
        break;
    case MMWLAN_STA_DISABLED:
    default:
        s_associated = false;
        s_associating = false;
        /* Signal the waiter here too. A connect attempt that fails fast
         * should retry after the backoff, not sit out the full
         * HALOW_CONNECT_TIMEOUT_MS first; halow_sta_connect() distinguishes
         * the two by re-checking s_associated after the take succeeds. */
        if (s_connect_sem != NULL) {
            xSemaphoreGive(s_connect_sem);
        }
        break;
    }
}

static void set_has_ip(bool has_ip)
{
    if (s_has_ip == has_ip) {
        return;
    }
    s_has_ip = has_ip;
    ESP_LOGI(TAG, "HaLow uplink %s", has_ip ? "up (has IP)" : "down");
    if (s_cb) {
        s_cb(has_ip, s_cb_ctx);
    }
}

/* mmhalow_init() creates its netif via ESP_NETIF_DEFAULT_WIFI_STA(), which
 * sets .get_ip_event = IP_EVENT_STA_GOT_IP and DHCP-client flags just like
 * a normal esp_wifi STA netif (confirmed by reading mmhalow.c directly) -
 * so the standard IP_EVENT_STA_GOT_IP/LOST_IP pair is the real "ready"
 * signal, filtered to our netif since other netifs (the SoftAP) exist too. */
static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;

    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        if (event->esp_netif == s_netif) {
            set_has_ip(true);
        }
    } else if (id == IP_EVENT_STA_LOST_IP) {
        /* This project has exactly one DHCP-client netif (the HaLow STA) -
         * the SoftAP runs a DHCP *server*, not a client - so no netif
         * filter is needed here. */
        set_has_ip(false);
    }
}

static esp_err_t halow_sta_bringup(const gw_uplink_config_t *cfg, esp_netif_t **out_netif)
{
    esp_err_t err = mmhalow_init(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmhalow_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* mmhalow.c has no public "get netif" accessor - its netif is a
     * module-static pointer. It's created with if_key "WIFI_STA_DEF" (the
     * same default esp_wifi STA would use), which is safe to look up here
     * since this project's own SoftAP uses "WIFI_AP_DEF" instead. */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        ESP_LOGE(TAG, "mmhalow_init() didn't register the expected WIFI_STA_DEF netif");
        return ESP_FAIL;
    }
    *out_netif = netif;

    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    mmhalow_wifi_config_t conf = {
        .sta = MMWLAN_STA_ARGS_INIT,
    };

    /* Bounded by the destination field, not just by our own buffer: these
     * are two independently-sized structs (ours from gw_config.h, theirs
     * from mmwlan.h) and a copy sized only by strlen() would overflow if
     * theirs is ever the smaller of the two. */
    size_t ssid_len = strlen(cfg->ssid);
    if (ssid_len > sizeof(conf.sta.ssid)) {
        ESP_LOGE(TAG, "uplink SSID is %u bytes, max %u", (unsigned)ssid_len,
                 (unsigned)sizeof(conf.sta.ssid));
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(conf.sta.ssid, cfg->ssid, ssid_len);
    conf.sta.ssid_len = ssid_len;

    conf.sta.security_type = halow_security_from_gw(cfg->security);
    if (conf.sta.security_type == MMWLAN_SAE) {
        size_t psk_len = strlen(cfg->psk);
        if (psk_len > sizeof(conf.sta.passphrase)) {
            ESP_LOGE(TAG, "uplink passphrase is %u bytes, max %u", (unsigned)psk_len,
                     (unsigned)sizeof(conf.sta.passphrase));
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(conf.sta.passphrase, cfg->psk, psk_len);
        conf.sta.passphrase_len = psk_len;
    }

    return mmhalow_set_config(WIFI_IF_STA, &conf);
}

static esp_err_t halow_sta_connect(TickType_t timeout_ticks)
{
    if (s_connect_sem == NULL) {
        s_connect_sem = xSemaphoreCreateBinary();
        if (s_connect_sem == NULL) {
            ESP_LOGE(TAG, "couldn't create connect semaphore");
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_connect_sem, 0); /* drain any stale signal before retrying */

    /* Already associated: a previous attempt's association can complete during
     * the backoff sleep, after its timeout was declared. mmhalow_connect() ->
     * mmwlan_sta_enable() must not be re-issued in that state - mmwlan.h
     * (v2.11.2-esp32-2, above mmwlan_sta_enable): "If station mode is already
     * enabled when this function is invoked then it will disconnect from (if
     * already connected) and initiate connection" - i.e. it would tear down
     * the working link we just got. */
    if (s_associated) {
        return ESP_OK;
    }

    esp_err_t err = mmhalow_connect(mm_sta_state_cb);
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_connect_sem, timeout_ticks) != pdTRUE) {
        /* The wait timing out doesn't prove association failed - it can land
         * in the gap between the timeout expiring and this check. Declaring
         * failure here would loop back into mmhalow_connect() and tear the
         * fresh association down (see the mmwlan.h citation above), so trust
         * the state flag over the semaphore. If association reliably takes
         * longer than HALOW_CONNECT_TIMEOUT_MS the retry loop still converges:
         * each mmwlan_sta_enable() restarts the driver's internal scan cycle,
         * and whichever attempt completes during a backoff window is accepted
         * by the s_associated checks instead of being discarded. */
        return s_associated ? ESP_OK : ESP_ERR_TIMEOUT;
    }

    /* The callback signals both outcomes, so a successful take doesn't by
     * itself mean association - it means the state settled. */
    return s_associated ? ESP_OK : ESP_FAIL;
}

static bool halow_sta_link_up(void)
{
    return s_associated;
}

/* Waits up to timeout_ms for the DHCP lease, giving up early if the link
 * drops underneath us. Returns true if the uplink has an IP. */
static bool wait_for_dhcp_lease(uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;

    while (waited_ms < timeout_ms) {
        if (s_has_ip) {
            return true;
        }
        if (!halow_sta_link_up()) {
            return false; /* association lost - outer loop handles it */
        }
        vTaskDelay(pdMS_TO_TICKS(LINK_POLL_INTERVAL_MS));
        waited_ms += LINK_POLL_INTERVAL_MS;
    }

    return s_has_ip;
}

static void reconnect_task(void *arg)
{
    (void)arg;
    uint32_t backoff_ms = RECONNECT_BACKOFF_MIN_MS;

    for (;;) {
        esp_err_t err = halow_sta_connect(pdMS_TO_TICKS(HALOW_CONNECT_TIMEOUT_MS));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HaLow STA connect failed (%s), retrying in %u ms",
                     esp_err_to_name(err), (unsigned)backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = (backoff_ms * 2 > RECONNECT_BACKOFF_MAX_MS) ? RECONNECT_BACKOFF_MAX_MS : backoff_ms * 2;
            continue;
        }

        backoff_ms = RECONNECT_BACKOFF_MIN_MS;
        ESP_LOGI(TAG, "HaLow STA associated (RSSI %" PRId32 " dBm), waiting for DHCP lease...",
                 uplink_halow_get_rssi());

        /* Association alone isn't usable - NAT and the CoT relay both need a
         * real address (see ip_event_handler's comment). Bound the wait so a
         * silent DHCP failure can't strand the task here indefinitely. */
        bool leased = false;
        for (int attempt = 0; attempt < DHCP_RESTART_ATTEMPTS; attempt++) {
            if (wait_for_dhcp_lease(DHCP_LEASE_TIMEOUT_MS)) {
                leased = true;
                break;
            }
            if (!halow_sta_link_up()) {
                break; /* dropped while waiting - re-associate below */
            }
            if (attempt + 1 < DHCP_RESTART_ATTEMPTS) {
                ESP_LOGW(TAG, "associated but no DHCP lease after %u ms, restarting DHCP client",
                         (unsigned)DHCP_LEASE_TIMEOUT_MS);
                esp_netif_dhcpc_stop(s_netif);
                esp_err_t dhcp_err = esp_netif_dhcpc_start(s_netif);
                if (dhcp_err != ESP_OK) {
                    ESP_LOGW(TAG, "dhcpc restart failed: %s", esp_err_to_name(dhcp_err));
                }
            }
        }

        if (!leased && halow_sta_link_up()) {
            /* Still associated but unusable. Drop the association properly
             * before retrying: mmhalow_disconnect() (-> mmwlan_sta_disable())
             * is a real API in the component's public header, so the earlier
             * approach of re-calling mmhalow_connect() on top of a live
             * association is no longer necessary. */
            ESP_LOGW(TAG, "no DHCP lease on this association, disconnecting and re-associating");
            set_has_ip(false);
            esp_err_t dis_err = mmhalow_disconnect();
            if (dis_err != ESP_OK) {
                ESP_LOGW(TAG, "mmhalow_disconnect failed: %s", esp_err_to_name(dis_err));
            }
            s_associated = false;
            continue;
        }

        /* Leased and healthy - hold here until the link actually drops. */
        while (halow_sta_link_up()) {
            vTaskDelay(pdMS_TO_TICKS(LINK_POLL_INTERVAL_MS));
        }

        ESP_LOGW(TAG, "HaLow uplink dropped, will reassociate");
        set_has_ip(false);
    }
}

esp_err_t uplink_halow_init(const gw_uplink_config_t *cfg)
{
    memcpy(&s_cfg, cfg, sizeof(s_cfg));
    esp_err_t err = halow_sta_bringup(&s_cfg, &s_netif);
    s_ready = (err == ESP_OK);
    if (s_ready) {
        s_scan_lock = xSemaphoreCreateMutex();
        s_scan_done = xSemaphoreCreateBinary();
        if (s_scan_lock == NULL || s_scan_done == NULL) {
            ESP_LOGW(TAG, "couldn't create scan primitives - scanning will be unavailable");
        }
        /* Logged unconditionally at bring-up: if this prints, host<->MM6108
         * SPI works, which rules out the single most likely first-flash
         * failure before any association attempt muddies the picture. */
        uplink_halow_log_radio_info();
    }
    return err;
}

bool uplink_halow_is_ready(void)
{
    return s_ready;
}

esp_err_t uplink_halow_start(void)
{
    if (!s_ready) {
        /* Without this the reconnect task would spin on mmhalow_connect()
         * against an uninitialized radio forever, and its once-per-second
         * error log would bury the single line from uplink_halow_init() that
         * says what actually went wrong. */
        ESP_LOGE(TAG, "not starting the reconnect task - the radio never initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_reconnect_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ok = xTaskCreate(reconnect_task, "halow_reconnect", 4096, NULL, 5, &s_reconnect_task_handle);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_netif_t *uplink_halow_get_netif(void)
{
    return s_netif;
}

bool uplink_halow_is_connected(void)
{
    return s_has_ip;
}

uplink_link_state_t uplink_halow_get_link_state(void)
{
    if (!s_ready) {
        return UPLINK_LINK_RADIO_FAILED;
    }
    if (s_has_ip) {
        return UPLINK_LINK_UP;
    }
    if (s_associated) {
        return UPLINK_LINK_ASSOCIATED;
    }
    if (s_associating) {
        return UPLINK_LINK_ASSOCIATING;
    }
    return UPLINK_LINK_DOWN;
}

const char *uplink_halow_link_state_name(uplink_link_state_t state)
{
    switch (state) {
    case UPLINK_LINK_RADIO_FAILED:
        return "radio failed";
    case UPLINK_LINK_DOWN:
        return "searching";
    case UPLINK_LINK_ASSOCIATING:
        return "associating";
    case UPLINK_LINK_ASSOCIATED:
        return "associated, no lease";
    case UPLINK_LINK_UP:
        return "up";
    default:
        return "unknown";
    }
}

int32_t uplink_halow_get_rssi(void)
{
    /* mmwlan_get_rssi() documents INT32_MIN as its error return, which is also
     * what we want to mean "not associated", so an unassociated radio and a
     * failed read are reported identically and callers only need one check. */
    if (!s_ready || !s_associated) {
        return INT32_MIN;
    }
    return mmwlan_get_rssi();
}

void uplink_halow_log_radio_info(void)
{
    if (!s_ready) {
        ESP_LOGW(TAG, "radio not initialized - no version info available");
        return;
    }
    /* The component's own printer: BCF API/build version, board description,
     * firmware and morselib versions, all via ESP_LOGI. */
    mmhalow_print_version_info();
}

/* `arg` is the generation this callback was registered for, passed by value
 * through the driver's void* so a late callback from a timed-out scan can be
 * recognised and dropped instead of feeding results to the wrong caller. */
static void scan_rx_cb(const struct mmwlan_scan_result *result, void *arg)
{
    struct scan_ctx *sc = &s_scan;
    if (result == NULL || (uint32_t)(uintptr_t)arg != sc->generation) {
        return;
    }

    uplink_scan_result_t out = { 0 };

    /* result->ssid points into the driver's receive buffer and is not
     * NUL-terminated; copy it out bounded by both lengths. */
    size_t ssid_len = result->ssid_len;
    if (ssid_len > sizeof(out.ssid) - 1) {
        ssid_len = sizeof(out.ssid) - 1;
    }
    if (result->ssid != NULL && ssid_len > 0) {
        memcpy(out.ssid, result->ssid, ssid_len);
    }
    out.ssid[ssid_len] = '\0';

    if (result->bssid != NULL) {
        memcpy(out.bssid, result->bssid, sizeof(out.bssid));
    }
    out.rssi = result->rssi;
    out.freq_hz = result->channel_freq_hz;
    out.bw_mhz = result->bw_mhz;

    sc->count++;
    if (sc->cb != NULL) {
        sc->cb(&out, sc->ctx);
    }
}

static void scan_complete_cb(enum mmwlan_scan_state state, void *arg)
{
    (void)state;
    if ((uint32_t)(uintptr_t)arg != s_scan.generation) {
        return; /* completion for a scan that already timed out */
    }
    if (s_scan_done != NULL) {
        xSemaphoreGive(s_scan_done);
    }
}

esp_err_t uplink_halow_scan(uplink_scan_cb_t cb, void *ctx, uint32_t timeout_ms)
{
    if (!s_ready || s_scan_lock == NULL || s_scan_done == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_scan_lock, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE; /* another scan is already running */
    }

    s_scan.generation++;
    s_scan.cb = cb;
    s_scan.ctx = ctx;
    s_scan.count = 0;
    uint32_t generation = s_scan.generation;

    /* Drain any completion left over from a previous scan that timed out, so
     * this one doesn't return instantly on a stale signal. */
    xSemaphoreTake(s_scan_done, 0);

    struct mmhalow_scan_args args = {
        .rx_cb = scan_rx_cb,
        .complete_cb = scan_complete_cb,
        .cb_arg = (void *)(uintptr_t)generation,
    };

    esp_err_t err = mmhalow_scan(&args);
    if (err == ESP_OK) {
        if (xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
            /* Results received so far have already been delivered to cb; the
             * scan just never reported completion within the budget. */
            ESP_LOGW(TAG, "scan didn't complete within %u ms", (unsigned)timeout_ms);
            err = ESP_ERR_TIMEOUT;
        } else {
            ESP_LOGI(TAG, "scan complete: %u AP(s) found", (unsigned)s_scan.count);
        }
    } else {
        ESP_LOGW(TAG, "mmhalow_scan failed: %s", esp_err_to_name(err));
    }

    /* Bumping the generation before releasing the lock retires this scan's
     * callbacks even if the driver delivers more of them later. */
    s_scan.generation++;
    s_scan.cb = NULL;
    xSemaphoreGive(s_scan_lock);
    return err;
}

void uplink_halow_set_state_callback(uplink_halow_state_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
}
