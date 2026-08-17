idf_component_register(
    SRCS
        "app_main.c"
        "provisioning.c"
        "uplink_halow.c"
        "uplink_wifi.c"
        "downlink_softap.c"
        "downlink_halow_ap.c"
        "ip_forward_nat.c"
        "cot_relay.c"
        "web_ui.c"
        "status_led.c"
        "factory_reset.c"
        "log_buffer.c"
        "task_stats.c"
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
)

# web_ui.html is embedded rather than served from a filesystem, so unlike the C
# sources - whose comments the preprocessor discards before codegen, costing
# zero flash - every byte of this file occupies a byte of the 3 MB app
# partition. Measured: comments and indentation are ~24% of it.
#
# So strip the embedded copy at build time instead of thinning the source. The
# generated file lands in the build directory and is never committed;
# main/web_ui.html keeps its comments. minify_web_ui.py documents why it only
# removes line-initial comments (the file contains regex literals a naive
# stripper would mangle) and fails loudly rather than guessing.
#
# This replaces `EMBED_FILES "web_ui.html"`, which cannot take a generated
# file. target_add_binary_data() is what EMBED_FILES calls internally
# (esp-idf/tools/cmake/component.cmake L512, with type "BINARY" - no trailing
# null byte, which matters because web_ui.c takes the length as end - start).
# RENAME_TO pins the symbol to `_binary_web_ui_html_start`/`_end` regardless of
# the generated filename, so web_ui.c:26-27 needs no change
# (esp-idf/tools/cmake/scripts/data_file_embed_asm.cmake builds the symbol from
# VARIABLE_BASENAME).
idf_build_get_property(python PYTHON)

set(web_ui_src "${CMAKE_CURRENT_LIST_DIR}/web_ui.html")
set(web_ui_minifier "${CMAKE_CURRENT_LIST_DIR}/minify_web_ui.py")
set(web_ui_embedded "${CMAKE_CURRENT_BINARY_DIR}/web_ui.min.html")

add_custom_command(
    OUTPUT "${web_ui_embedded}"
    COMMAND "${python}" "${web_ui_minifier}" "${web_ui_src}" "${web_ui_embedded}"
    MAIN_DEPENDENCY "${web_ui_src}"
    DEPENDS "${web_ui_minifier}"
    COMMENT "Minifying web_ui.html for embedding"
    VERBATIM)

target_add_binary_data(${COMPONENT_LIB} "${web_ui_embedded}" "BINARY"
    RENAME_TO web_ui_html
    DEPENDS "${web_ui_embedded}")
