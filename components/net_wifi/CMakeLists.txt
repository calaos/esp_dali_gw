idf_component_register(
    SRCS "src/net_wifi.c" "src/dns_hijack.c" "src/time_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES gw_events
    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer lwip nvs_flash app_config espressif__mdns)
