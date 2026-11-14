# 不要写 set(COMPONENTS main)：本工程依赖 managed_components 里的
# esp_lcd_co5300 / esp_lcd_touch / esp_lvgl_port / lvgl / esp_tinyusb。
cmake_minimum_required(VERSION 3.16)

# Component Manager evaluates the manifest before the project components are
# configured.  Keep the ESP-SR dependency opt-in, but make the default
# explicit so a plain `cmake`/`idf.py` invocation cannot fail merely because
# the feature flag was not exported by the shell.  The product build helper
# overrides this to 1 when a real esp-sr/model asset is installed.
if(NOT DEFINED ENV{MEOAVE_USE_ESP_SR})
    set(ENV{MEOAVE_USE_ESP_SR} "0")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4_gesture)

# Compile a hash-pinned, error-path-only correction into the existing LVGL
# port target. Never edit Component Manager's downloaded sources in place.
include(cmake/PpaGuard.cmake)
meoave_install_ppa_guard()

include(cmake/SrSimdGuard.cmake)
meoave_install_sr_simd_guard()
