# web/dist is gzipped and embedded at configure time. Re-running CMake is wired to the contents of
# web/dist so a `npm run build` with new hashed asset names is picked up without a manual reconfigure.
set(WEBUI_DIST "${CMAKE_CURRENT_LIST_DIR}/../../web/dist")
set(WEBUI_GZDIR "${CMAKE_BINARY_DIR}/webui_gz")
set(WEBUI_SOURCE "${CMAKE_BINARY_DIR}/webui_assets.c")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${WEBUI_DIST}" "${CMAKE_CURRENT_LIST_DIR}/gen_assets.py")

execute_process(
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/gen_assets.py"
            --dist "${WEBUI_DIST}" --outdir "${WEBUI_GZDIR}" --source "${WEBUI_SOURCE}"
    OUTPUT_VARIABLE WEBUI_GZ_LIST
    RESULT_VARIABLE WEBUI_RC
    ERROR_VARIABLE WEBUI_ERR)

if(NOT WEBUI_RC EQUAL 0)
    message(FATAL_ERROR "web UI assets could not be embedded:${WEBUI_ERR}")
endif()

string(STRIP "${WEBUI_GZ_LIST}" WEBUI_GZ_LIST)
string(REPLACE "\n" ";" WEBUI_EMBED "${WEBUI_GZ_LIST}")

idf_component_register(
    SRCS "src/webui.c" "${WEBUI_SOURCE}"
    INCLUDE_DIRS "include"
    EMBED_FILES ${WEBUI_EMBED})
