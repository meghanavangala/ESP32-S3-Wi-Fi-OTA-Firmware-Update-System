idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client esp_netif esp_event nvs_flash esp_wifi app_update
)