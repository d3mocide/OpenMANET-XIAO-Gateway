idf_component_register(
    SRCS
        "app_main.c"
        "provisioning.c"
        "uplink_halow.c"
        "downlink_softap.c"
        "ip_forward_nat.c"
        "cot_relay.c"
        "web_ui.c"
        "status_led.c"
        "factory_reset.c"
        "log_buffer.c"
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
        esp_app_format
        app_update
        esp_driver_gpio
        json
    PRIV_REQUIRES
        # app_main.c reports the stored core dump's crashing task and backtrace
        # at boot (esp_core_dump.h). PRIV_ rather than REQUIRES because no
        # header this component installs exposes the type - it is an
        # implementation detail of app_main.c.
        espcoredump
    EMBED_FILES "web_ui.html"
)
