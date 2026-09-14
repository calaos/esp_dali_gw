# espressif/cjson registers as "espressif__cjson" in IDF 6; the old core name "json" is gone.
idf_component_register(
    SRCS "src/gw_api.c" "src/gw_api_info.c"
    INCLUDE_DIRS "include"
    REQUIRES app_config espressif__cjson)
