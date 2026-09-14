idf_component_register(
    SRCS "src/http_iface.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server
    PRIV_REQUIRES gw_api gw_events app_config net_wifi webui dali_bus
                  esp_app_format esp_timer esp_hw_support app_update)
