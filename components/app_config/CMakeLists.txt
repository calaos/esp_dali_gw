idf_component_register(
    SRCS "src/app_config.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash esp_system esp_hw_support)
