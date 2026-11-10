idf_component_register(SRCS "main.c" INCLUDE_DIRS ".."
    REQUIRES esp_driver_sdio esp_driver_gpio esp_app_format esp_system esp_timer)
