idf_component_register(
    SRCS
        "app_main.c"
        "provisioning.c"
        "uplink_halow.c"
        "downlink_softap.c"
        "ip_forward_nat.c"
        "cot_relay.c"
        "web_ui.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        console
        lwip
        esp_http_server
        esp_timer
        json
    EMBED_FILES "web_ui.html"
)
