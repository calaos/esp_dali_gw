idf_component_register(
    SRCS "src/status_led.c"
    INCLUDE_DIRS "include"
    REQUIRES gw_events
    PRIV_REQUIRES espressif__led_strip esp_timer esp_driver_rmt)
