idf_component_register(
    SRCS "src/dali_bus.c"
    INCLUDE_DIRS "include"
    REQUIRES gw_api gw_events
    PRIV_REQUIRES app_config espressif__dali esp_timer freertos)
