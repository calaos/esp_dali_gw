cmake_minimum_required(VERSION 3.16)

# Version comes from git so that the banner, /api/info, the MQTT status topic and the OTA image all
# report the same string. A shallow clone or a tarball has no tags, hence the fallbacks.
execute_process(
    COMMAND git describe --tags --dirty --always
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT PROJECT_VER)
    set(PROJECT_VER "0.0.0-nogit")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_dali_gw)
