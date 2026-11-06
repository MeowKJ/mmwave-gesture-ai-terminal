if(CONFIG_APP_FLS2_OFFICIAL_BASELINE)
    idf_component_register(
        SRCS "../build_fls2_source/main.c" "fls2_official_board.c" "fls2_touch_diag.c"
             "fls2_display_diag.c" "fls2_test_loop.c"
             "bsp_common.c" "bsp_power.c" "bsp_usb.c"
        INCLUDE_DIRS "."
        REQUIRES esp_lcd esp_driver_i2c esp_driver_gpio esp_adc
                 esp_hw_support esp_timer
        PRIV_REQUIRES esp_lcd_touch_cst820 esp_lcd_touch
    )
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=esp_lcd_panel_io_rx_param"
        "-Wl,--wrap=esp_lcd_touch_new_i2c_cst820"
        "-Wl,--wrap=esp_lcd_new_panel_co5300"
        "-Wl,--wrap=esp_lcd_panel_draw_bitmap"
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
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_i2c esp_driver_gpio esp_driver_uart
             esp_driver_i2s esp_adc esp_hw_support esp_timer
    PRIV_REQUIRES esp_lcd_touch_cst820 esp_lcd_touch
)
