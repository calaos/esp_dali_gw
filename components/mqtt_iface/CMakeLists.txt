idf_component_register(
    SRCS "src/mqtt_iface.c"
    INCLUDE_DIRS "include"
    REQUIRES app_config
    PRIV_REQUIRES gw_api gw_events dali_bus net_wifi espressif__mqtt esp_app_format esp_timer
                  esp_hw_support)
