# Host unit tests for the pure-logic components. Plain CMake + Unity rather than the IDF linux
# target: gw_api deliberately depends on nothing but cJSON, and keeping the test build IDF-free is
# what proves that.
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
    "${REPO}/components/gw_api/src/gw_api.c"
    "${REPO}/components/gw_api/src/gw_api_info.c"
    "${UNITY_DIR}/unity.c"
    "${CJSON_DIR}/cJSON.c"
    shim/esp_err.c)

target_include_directories(esp_dali_gw_tests PRIVATE
    shim
    "${REPO}/components/gw_api/include"
    "${REPO}/components/app_config/include"
    "${UNITY_DIR}"
    "${CJSON_DIR}")

target_compile_options(esp_dali_gw_tests PRIVATE -Wall -Wextra -Werror)

add_test(NAME unit COMMAND esp_dali_gw_tests)
