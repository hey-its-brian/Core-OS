idf_component_register(
    SRCS "net_wifi.c" "net_tools.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl
    PRIV_REQUIRES deck_hal lwip espressif__mdns esp_wifi esp_netif esp_event esp_timer esp_http_client esp-tls mbedtls json
)
