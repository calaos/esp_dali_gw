idf_component_register(
    SRCS "src/dali_bus.c" "src/dali_ops.c" "src/dali_names.c" "src/dali_commission.c" "src/dali_configure.c"
    INCLUDE_DIRS "include"
    REQUIRES gw_api gw_events
    PRIV_REQUIRES app_config espressif__dali esp_timer freertos)
