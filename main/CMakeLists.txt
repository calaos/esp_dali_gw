idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS ""
    REQUIRES app_config status_led net_wifi dali_bus mqtt_iface http_iface gw_events
    PRIV_REQUIRES nvs_flash esp_event esp_netif esp_app_format)
