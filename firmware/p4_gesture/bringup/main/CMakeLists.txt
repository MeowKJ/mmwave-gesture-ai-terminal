if(CONFIG_ESP_HOSTED_ENABLED)
    if(CONFIG_MEOAVE_C5_MAINT_HOST)
        set(bringup_source "main_maintenance.c" "wifi_diagnostic.c")
    else()
        set(bringup_source "main_hosted.c")
    endif()
    idf_component_register(SRCS "${bringup_source}" INCLUDE_DIRS "."
        REQUIRES esp_driver_uart esp_driver_gpio esp_hosted esp_app_format esp_timer spi_flash mbedtls esp_wifi esp_netif esp_http_client)
    idf_component_optional_requires(PRIVATE esp_hosted)
    idf_component_optional_requires(PRIVATE mbedtls esp_driver_gpio)
    idf_component_optional_requires(PRIVATE esp_wifi esp_netif esp_http_client)
else()
    idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
    REQUIRES esp_driver_uart esp_driver_gpio esp_driver_sdmmc sdmmc
             esp_app_format esp_timer spi_flash)
endif()
