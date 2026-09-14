idf_component_register(
    SRCS "src/mqtt_iface.c"
    INCLUDE_DIRS "include"
    REQUIRES app_config
    PRIV_REQUIRES gw_api gw_events dali_bus espressif__mqtt)
