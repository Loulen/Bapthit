idf_component_register(
    SRCS "main.cpp" "wifi_sta.c" "score_queue.c" "ws_client.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event nvs_flash esp_netif esp_app_format app_update
             lwip esp_timer idf_component
    PRIV_REQUIRES mdns esp_websocket_client
)

# Dev build variant: set BAPTHIT_DEV=1 in the environment (e.g. via the
# Makefile target `build-dev`) to switch SSID to BAPTHIT-DEV.
if(DEFINED ENV{BAPTHIT_DEV})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BAPTHIT_DEV=1)
endif()
