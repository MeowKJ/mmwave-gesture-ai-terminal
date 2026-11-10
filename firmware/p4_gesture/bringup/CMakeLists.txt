cmake_minimum_required(VERSION 3.16)
set(COMPONENTS main)
if(MEOAVE_BRINGUP_HOSTED)
    set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/esp_hosted")
    # Early component discovery does not inherit arbitrary project variables.
    set(COMPONENTS main esp_hosted esp_wifi esp_netif esp_http_client)
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4_c5_bringup)
