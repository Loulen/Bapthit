idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event nvs_flash esp_netif esp_http_server esp_http_client esp_app_format app_update lwip idf_component
    EMBED_TXTFILES "index.html"
)
