# Host unit tests for the pure-logic components. Plain CMake + Unity rather than the IDF linux
# target: gw_api and the validation half of app_config deliberately depend on nothing but cJSON,
# and keeping the test build IDF-free is what proves that. shim/ supplies the handful of symbols
# the rest of app_config would otherwise pull in (esp_err, sdkconfig, the MAC-derived device id).
cmake_minimum_required(VERSION 3.16)
project(esp_dali_gw_tests C)

enable_testing()
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(REPO "${CMAKE_CURRENT_LIST_DIR}/../..")
set(UNITY_DIR "$ENV{IDF_PATH}/components/unity/unity/src")
set(CJSON_DIR "${REPO}/managed_components/espressif__cjson/cJSON")

foreach(dir "${UNITY_DIR}" "${CJSON_DIR}")
    if(NOT EXISTS "${dir}")
        message(FATAL_ERROR
            "${dir} is missing.\n"
            "Run tools/host-tests.sh, which fetches the managed components first.")
    endif()
endforeach()

add_executable(esp_dali_gw_tests
    test_main.c
    test_gw_api.c
    test_gw_api_config.c
    test_app_config.c
    "${REPO}/components/gw_api/src/gw_api.c"
    "${REPO}/components/gw_api/src/gw_api_info.c"
    "${REPO}/components/app_config/src/app_config_logic.c"
    "${UNITY_DIR}/unity.c"
    "${CJSON_DIR}/cJSON.c"
    shim/esp_err.c
    shim/host_stubs.c)

target_include_directories(esp_dali_gw_tests PRIVATE
    shim
    "${REPO}/components/gw_api/include"
    "${REPO}/components/app_config/include"
    "${REPO}/components/app_config/src"
    "${UNITY_DIR}"
    "${CJSON_DIR}")

target_compile_options(esp_dali_gw_tests PRIVATE -Wall -Wextra -Werror)

add_test(NAME unit COMMAND esp_dali_gw_tests)
