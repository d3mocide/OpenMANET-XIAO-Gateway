idf_component_register(
    SRCS
        "app_main.c"
        "provisioning.c"
        "uplink_halow.c"
        "downlink_softap.c"
        "ip_forward_nat.c"
        "cot_relay.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        console
        lwip
)
