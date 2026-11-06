# 不要写 set(COMPONENTS main)：本工程依赖 managed_components 里的
# esp_lcd_co5300 / esp_lcd_touch / esp_lvgl_port / lvgl / esp_tinyusb。
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4_gesture)
