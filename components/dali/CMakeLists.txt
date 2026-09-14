# Vendored copy — see VENDORED.md. IDF 6 only, so the pre-5.5 "driver" fallback is dropped.
set(REQ esp_driver_rmt esp_driver_gpio esp_timer)

# Part 101 (dali_system_components.c) is always compiled.
set(DALI_SRCS "src/dali_system_components.c" "src/dali_frame_decode.c")

if(CONFIG_DALI_PART102_ENABLED)
    list(APPEND DALI_SRCS "src/dali_control_gear.c")
endif()

if(CONFIG_DALI_PART209_ENABLED)
    list(APPEND DALI_SRCS "src/dali_color_control_dt8.c")
endif()

if(CONFIG_DALI_PART103_ENABLED)
    list(APPEND DALI_SRCS "src/dali_control_device.c")
endif()

if(CONFIG_DALI_PART303_304_ENABLED)
    list(APPEND DALI_SRCS "src/dali_device_sensors.c")
endif()

idf_component_register(SRCS ${DALI_SRCS}
                        INCLUDE_DIRS "include"
                        PRIV_INCLUDE_DIRS "src"
                        REQUIRES ${REQ}
                        PRIV_REQUIRES freertos)
