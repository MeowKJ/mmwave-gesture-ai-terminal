if(CONFIG_APP_FLS2_OFFICIAL_BASELINE)
    set(product_sources)
    set(product_requires)
    if(CONFIG_APP_MEOAVE_UI)
        list(APPEND product_sources "meoave_ui.c" "meoave_model.c" "meoave_app.c"
             "bsp_wifi_probe.c" "bsp_c5_diag.c"
             "fonts/meo_font_16.c" "fonts/meo_font_20.c"
             "fonts/meo_font_28.c" "fonts/meo_font_44.c")
        list(APPEND product_requires esp_driver_sdmmc)
        if(CONFIG_APP_MEOAVE_WIFI_HOSTED)
            list(APPEND product_sources "bsp_wifi_hosted.c")
            list(APPEND product_requires esp_hosted esp_wifi esp_netif esp_event)
        endif()
    endif()
    idf_component_register(
        SRCS "fls2_official_source.c" "fls2_official_board.c" "fls2_touch_diag.c"
             "fls2_display_diag.c" "fls2_display_startup.c" "fls2_test_loop.c"
             "bsp_common.c" "bsp_power.c" "bsp_usb.c" "bsp_usb_tx.c" ${product_sources}
        INCLUDE_DIRS "."
        REQUIRES esp_lcd esp_driver_i2c esp_driver_gpio esp_adc esp_event
                 esp_hw_support esp_timer esp_app_format ${product_requires}
        # Dependencies are resolved before sdkconfig in IDF's early expansion.
        PRIV_REQUIRES esp_lcd_touch_cst820 esp_lcd_touch nvs_flash esp_event
    )
    if(CONFIG_APP_MEOAVE_WIFI_HOSTED)
        # Keep the hosted adapter's public headers visible even when this
        # vendored component is not resolved through the normal dependency
        # graph during the IDF configure pass.
        target_include_directories(${COMPONENT_LIB} PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/../components/esp_hosted/host"
            "${CMAKE_CURRENT_LIST_DIR}/../components/esp_hosted/host/api/include"
            "$ENV{IDF_PATH}/components/esp_event/include"
            "$ENV{IDF_PATH}/components/esp_netif/include"
            "$ENV{IDF_PATH}/components/esp_wifi/include"
        )
    endif()
    if(CONFIG_APP_MEOAVE_UI)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    endif()
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=esp_lcd_panel_io_rx_param"
        "-Wl,--wrap=esp_lcd_touch_new_i2c_cst820"
        "-Wl,--wrap=esp_lcd_new_panel_co5300"
        "-Wl,--wrap=esp_lcd_panel_draw_bitmap"
        "-Wl,--wrap=esp_panic_handler"
    )
    return()
endif()

idf_component_register(
    SRCS "main.c"
         "demo_ui.c"
         "bsp_common.c"
         "bsp_power.c"
         "bsp_display.c"
         "bsp_touch.c"
         "bsp_audio.c"
         "bsp_radar.c"
         "bsp_usb.c"
         "bsp_usb_tx.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_i2c esp_driver_gpio esp_driver_uart
             esp_driver_i2s esp_adc esp_hw_support esp_timer esp_app_format
    PRIV_REQUIRES esp_lcd_touch_cst820 esp_lcd_touch nvs_flash
)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
