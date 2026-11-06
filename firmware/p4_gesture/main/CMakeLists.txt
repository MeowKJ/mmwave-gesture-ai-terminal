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
