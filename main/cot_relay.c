#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cot_relay.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "cot_relay";

static esp_netif_t *s_netif_a = NULL;
static esp_netif_t *s_netif_b = NULL;
static int s_sock = -1;
static struct sockaddr_in s_group_dest;

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
    if (get_netif_addr(netif, &iface_addr) != ESP_OK) {
        return;
    }
    if (setsockopt(s_sock, IPPROTO_IP, IP_MULTICAST_IF, &iface_addr, sizeof(iface_addr)) < 0) {
        ESP_LOGW(TAG, "IP_MULTICAST_IF failed: errno %d", errno);
        return;
    }
    if (sendto(s_sock, data, len, 0, (struct sockaddr *)&s_group_dest, sizeof(s_group_dest)) < 0) {
        ESP_LOGW(TAG, "sendto failed: errno %d", errno);
    }
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

        struct in_addr dest_addr = { 0 };
        bool have_dest = false;
        for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
                struct in_pktinfo *pktinfo = (struct in_pktinfo *)CMSG_DATA(cmsg);
                dest_addr = pktinfo->ipi_addr;
                have_dest = true;
                break;
            }
        }

        if (!have_dest) {
            ESP_LOGW(TAG, "datagram had no IP_PKTINFO, can't tell arrival interface - dropping");
            continue;
        }

        struct in_addr addr_a, addr_b;
        esp_netif_t *forward_to = NULL;
        if (get_netif_addr(s_netif_a, &addr_a) == ESP_OK && addr_a.s_addr == dest_addr.s_addr) {
            forward_to = s_netif_b;
        } else if (get_netif_addr(s_netif_b, &addr_b) == ESP_OK && addr_b.s_addr == dest_addr.s_addr) {
            forward_to = s_netif_a;
        }

        if (forward_to == NULL) {
            continue; /* arrived via neither known interface - drop rather than guess */
        }

        send_via(forward_to, rx_buffer, len);
    }
}

esp_err_t cot_relay_start(esp_netif_t *netif_a, esp_netif_t *netif_b, const gw_cot_config_t *cfg)
{
    s_netif_a = netif_a;
    s_netif_b = netif_b;

    struct in_addr group_addr;
    if (inet_aton(cfg->group, &group_addr) == 0) {
        ESP_LOGE(TAG, "invalid multicast group '%s'", cfg->group);
        return ESP_ERR_INVALID_ARG;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
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
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    struct in_addr addr_a, addr_b;
    ESP_RETURN_ON_ERROR(get_netif_addr(netif_a, &addr_a), TAG, "netif_a has no IP yet");
    ESP_RETURN_ON_ERROR(get_netif_addr(netif_b, &addr_b), TAG, "netif_b has no IP yet");
    ESP_RETURN_ON_ERROR(join_group(s_sock, &addr_a, &group_addr), TAG, "join on netif_a");
    ESP_RETURN_ON_ERROR(join_group(s_sock, &addr_b, &group_addr), TAG, "join on netif_b");

    int pktinfo_enable = 1;
    if (setsockopt(s_sock, IPPROTO_IP, IP_PKTINFO, &pktinfo_enable, sizeof(pktinfo_enable)) < 0) {
        ESP_LOGE(TAG, "IP_PKTINFO failed: errno %d (is CONFIG_LWIP_NETBUF_RECVINFO enabled?)", errno);
        return ESP_FAIL;
    }

    memset(&s_group_dest, 0, sizeof(s_group_dest));
    s_group_dest.sin_family = AF_INET;
    s_group_dest.sin_addr = group_addr;
    s_group_dest.sin_port = htons(cfg->port);

    if (xTaskCreate(relay_task, "cot_relay", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "CoT relay joined %s:%u on both interfaces", cfg->group, cfg->port);
    return ESP_OK;
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
