#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cot_relay.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "task_stats.h"

static const char *TAG = "cot_relay";

static esp_netif_t *s_netif_a = NULL;
static esp_netif_t *s_netif_b = NULL;

/* lwIP interface indices for the two netifs above, resolved once at start.
 * These are what IP_PKTINFO actually reports per datagram (see relay_task);
 * they're stable for the lifetime of a netif, so there's no reason to look
 * them up per packet. */
static int s_ifindex_a = -1;
static int s_ifindex_b = -1;

static int s_sock = -1;
static struct sockaddr_in s_group_dest;

/* send_via() does setsockopt(IP_MULTICAST_IF) + sendto() as a pair on one
 * shared socket; without this lock a concurrent caller (relay_task vs. a
 * future cot_relay_inject() self-beacon) could retarget the egress
 * interface between another sender's two calls. */
static SemaphoreHandle_t s_send_lock = NULL;

static esp_err_t get_netif_addr(esp_netif_t *netif, struct in_addr *out)
{
    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(netif, &ip_info);
    if (err != ESP_OK) {
        return err;
    }
    out->s_addr = ip_info.ip.addr;
    return ESP_OK;
}

static esp_err_t join_group(int sock, const struct in_addr *iface_addr, const struct in_addr *group_addr)
{
    struct ip_mreq mreq = { 0 };
    mreq.imr_interface = *iface_addr;
    mreq.imr_multiaddr = *group_addr;
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        ESP_LOGE(TAG, "IP_ADD_MEMBERSHIP failed: errno %d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void send_via(esp_netif_t *netif, const void *data, size_t len)
{
    struct in_addr iface_addr;
    if (get_netif_addr(netif, &iface_addr) != ESP_OK || iface_addr.s_addr == 0) {
        return; /* interface has no address yet - nothing to source from */
    }

    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    if (setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_IF, &iface_addr, sizeof(iface_addr)) < 0) {
        ESP_LOGW(TAG, "IP_MULTICAST_IF failed: errno %d", errno);
    } else if (sendto(s_sock, data, len, 0, (struct sockaddr *)&s_group_dest, sizeof(s_group_dest)) < 0) {
        ESP_LOGW(TAG, "sendto failed: errno %d", errno);
    }
    xSemaphoreGive(s_send_lock);
}

/* Second line of defence against a forwarding loop, behind IP_MULTICAST_LOOP
 * being disabled in cot_relay_start(): if a datagram we transmitted ever
 * comes back to us (loopback default changing, an upstream device reflecting
 * the group, or our own inject() echoing), its source address is one of our
 * own interface addresses and we must not forward it again. */
static bool is_own_address(const struct in_addr *addr)
{
    struct in_addr self;

    if (get_netif_addr(s_netif_a, &self) == ESP_OK && self.s_addr != 0 && self.s_addr == addr->s_addr) {
        return true;
    }
    if (get_netif_addr(s_netif_b, &self) == ESP_OK && self.s_addr != 0 && self.s_addr == addr->s_addr) {
        return true;
    }
    return false;
}

static void relay_task(void *arg)
{
    (void)arg;
    static uint8_t rx_buffer[1500];

    for (;;) {
        struct sockaddr_in source_addr;
        u8_t cmsg_buf[CMSG_SPACE(sizeof(struct in_pktinfo))];
        struct iovec iov = {
            .iov_base = rx_buffer,
            .iov_len = sizeof(rx_buffer),
        };
        struct msghdr msg = {
            .msg_name = &source_addr,
            .msg_namelen = sizeof(source_addr),
            .msg_iov = &iov,
            .msg_iovlen = 1,
            .msg_control = cmsg_buf,
            .msg_controllen = sizeof(cmsg_buf),
        };

        int len = recvmsg(s_sock, &msg, 0);
        if (len < 0) {
            ESP_LOGW(TAG, "recvmsg failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (len == 0) {
            continue;
        }

        /* Which interface did this arrive on? Use ipi_ifindex, NOT ipi_addr:
         * lwIP fills ipi_addr from the packet's *destination* address
         * (sockets.c: inet_addr_from_ip4addr(&pkti->ipi_addr,
         * ip_2_ip4(netbuf_destaddr(buf)))), which for this relay is always
         * the multicast group and therefore never matches an interface's own
         * unicast address. ipi_ifindex is the real arrival-interface signal
         * (pkti->ipi_ifindex = buf->p->if_idx). */
        int arrival_ifindex = -1;
        for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
                struct in_pktinfo *pktinfo = (struct in_pktinfo *)CMSG_DATA(cmsg);
                arrival_ifindex = pktinfo->ipi_ifindex;
                break;
            }
        }

        if (arrival_ifindex < 0) {
            ESP_LOGW(TAG, "datagram had no IP_PKTINFO, can't tell arrival interface - dropping");
            continue;
        }

        if (is_own_address(&source_addr.sin_addr)) {
            continue; /* our own transmission came back - never re-forward */
        }

        esp_netif_t *forward_to = NULL;
        if (arrival_ifindex == s_ifindex_a) {
            forward_to = s_netif_b;
        } else if (arrival_ifindex == s_ifindex_b) {
            forward_to = s_netif_a;
        }

        if (forward_to == NULL) {
            continue; /* arrived via neither known interface - drop rather than guess */
        }

        send_via(forward_to, rx_buffer, len);
    }
}

static void relay_cleanup(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    if (s_send_lock != NULL) {
        vSemaphoreDelete(s_send_lock);
        s_send_lock = NULL;
    }
}

esp_err_t cot_relay_start(esp_netif_t *netif_a, esp_netif_t *netif_b, const gw_cot_config_t *cfg)
{
    if (netif_a == NULL || netif_b == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sock >= 0) {
        return ESP_ERR_INVALID_STATE; /* already running */
    }

    s_netif_a = netif_a;
    s_netif_b = netif_b;

    struct in_addr group_addr;
    if (inet_aton(cfg->group, &group_addr) == 0) {
        ESP_LOGE(TAG, "invalid multicast group '%s'", cfg->group);
        return ESP_ERR_INVALID_ARG;
    }

    /* Resolved once - this is what IP_PKTINFO reports per datagram. */
    s_ifindex_a = esp_netif_get_netif_impl_index(netif_a);
    s_ifindex_b = esp_netif_get_netif_impl_index(netif_b);
    if (s_ifindex_a < 0 || s_ifindex_b < 0 || s_ifindex_a == s_ifindex_b) {
        ESP_LOGE(TAG, "bad netif indices (a=%d b=%d) - can't distinguish arrival interface",
                 s_ifindex_a, s_ifindex_b);
        return ESP_FAIL;
    }

    s_send_lock = xSemaphoreCreateMutex();
    if (s_send_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        relay_cleanup();
        return ESP_FAIL;
    }

    int reuse = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr = { 0 };
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(cfg->port);
    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed: errno %d", errno);
        relay_cleanup();
        return ESP_FAIL;
    }

    struct in_addr addr_a, addr_b;
    esp_err_t err = ESP_OK;
    if ((err = get_netif_addr(netif_a, &addr_a)) != ESP_OK) {
        ESP_LOGE(TAG, "netif_a has no IP yet: %s", esp_err_to_name(err));
        goto fail;
    }
    if ((err = get_netif_addr(netif_b, &addr_b)) != ESP_OK) {
        ESP_LOGE(TAG, "netif_b has no IP yet: %s", esp_err_to_name(err));
        goto fail;
    }
    if ((err = join_group(s_sock, &addr_a, &group_addr)) != ESP_OK) {
        goto fail;
    }
    if ((err = join_group(s_sock, &addr_b, &group_addr)) != ESP_OK) {
        goto fail;
    }

    /* We are joined to this group on both interfaces we also transmit on, so
     * loopback of our own datagrams would be re-received and forwarded back
     * out the other interface - a self-sustaining loop. Turn it off
     * explicitly rather than relying on the stack's default. */
    u8_t loop = 0;
    if (setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
        ESP_LOGE(TAG, "IP_MULTICAST_LOOP off failed: errno %d", errno);
        err = ESP_FAIL;
        goto fail;
    }

    int pktinfo_enable = 1;
    if (setsockopt(s_sock, IPPROTO_IP, IP_PKTINFO, &pktinfo_enable, sizeof(pktinfo_enable)) < 0) {
        ESP_LOGE(TAG, "IP_PKTINFO failed: errno %d (is CONFIG_LWIP_NETBUF_RECVINFO enabled?)", errno);
        err = ESP_FAIL;
        goto fail;
    }

    memset(&s_group_dest, 0, sizeof(s_group_dest));
    s_group_dest.sin_family = AF_INET;
    s_group_dest.sin_addr = group_addr;
    s_group_dest.sin_port = htons(cfg->port);

    if (xTaskCreate(relay_task, "cot_relay", GW_STACK_COT_RELAY, NULL, 5, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    ESP_LOGI(TAG, "CoT relay joined %s:%u on both interfaces (ifindex %d and %d)",
             cfg->group, cfg->port, s_ifindex_a, s_ifindex_b);
    return ESP_OK;

fail:
    relay_cleanup();
    return err;
}

bool cot_relay_is_running(void)
{
    return s_sock >= 0;
}

esp_err_t cot_relay_inject(const void *data, size_t len)
{
    if (s_sock < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    send_via(s_netif_a, data, len);
    send_via(s_netif_b, data, len);
    return ESP_OK;
}
