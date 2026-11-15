if(CONFIG_APP_FLS2_OFFICIAL_BASELINE)
    set(product_sources)
    set(product_requires)
    if(CONFIG_APP_MEOAVE_UI)
        list(APPEND product_sources "meoave_ui.c" "meoave_icons.c" "meoave_model.c" "meoave_app.c"
             "meoave_wake.c"
             "meoave_espsr.c"
             "meoave_sr_alloc.c"
             "meoave_wake_dsp.c" "meoave_sr_pack.c"
             "meoave_sentence.c" "meoave_sentence_json.c" "meoave_voice.c"
             "meoave_voice_link.c"
             "bsp_wifi_probe.c" "bsp_c5_diag.c"
             "fonts/meo_font_16.c" "fonts/meo_font_20.c"
             "fonts/meo_font_28.c" "fonts/meo_font_44.c")
        list(APPEND product_requires esp_driver_sdmmc)
        # ESP-SR is intentionally opt-in at the component-manager layer.  The
        # normal offline/product build still compiles the adapter and reports
        # COMPONENT_MISSING rather than pretending that USB text injection is
        # acoustic wake detection.  Enable with MEOAVE_USE_ESP_SR=1 after the
        # official esp-sr component and a custom 猫猫 WakeNet asset are present.
        if(CONFIG_APP_MEOAVE_ESPSR AND DEFINED ENV{MEOAVE_USE_ESP_SR} AND
           "$ENV{MEOAVE_USE_ESP_SR}" STREQUAL "1")
            list(APPEND product_requires esp-sr)
        endif()
        if(CONFIG_APP_C5_SDIO_LOADER)
            list(APPEND product_sources "bsp_c5_loader.c")
            list(APPEND product_requires esp-serial-flasher)
        endif()
        if(CONFIG_APP_MEOAVE_WIFI_HOSTED)
            if(CONFIG_APP_C5_NATIVE_MAINTENANCE)
                list(APPEND product_sources "bsp_c5_maintenance.c" "../bringup/main/main_maintenance.c")
                list(APPEND product_requires mbedtls esp_driver_uart)
            else()
                list(APPEND product_sources "bsp_wifi_hosted.c")
            endif()
            list(APPEND product_requires esp_hosted esp_wifi esp_netif esp_event)
        endif()
    endif()
    idf_component_register(
        SRCS "fls2_official_source.c" "fls2_official_board.c" "fls2_touch_diag.c"
             "fls2_display_diag.c" "fls2_display_startup.c" "fls2_test_loop.c"
             "bsp_common.c" "bsp_power.c" "bsp_usb.c" "bsp_usb_tx.c" "bsp_audio.c"
             "audio_selftest.c" "meoave_pcm.c"
             ${product_sources}
        INCLUDE_DIRS "."
        REQUIRES esp_lcd esp_driver_i2c esp_driver_gpio esp_driver_i2s esp_adc esp_event
                 esp_hw_support esp_timer esp_app_format meo_link ${product_requires}
        # Dependencies are resolved before sdkconfig in IDF's early expansion.
        PRIV_REQUIRES esp_lcd_touch_cst820 esp_lcd_touch nvs_flash esp_event mbedtls esp_driver_uart
    )
    # ESP-IDF 的增量配置不会为新加入 components/ 的组件刷新 main 的
    # REQUIRES 传播，首轮构建会报 "meo_link.h: No such file" 却仍然把
    # meo_link 自己编译出来。显式补齐 include 与链接，重复声明无害。
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../components/meo_link/include")
    target_link_libraries(${COMPONENT_LIB} PRIVATE idf::meo_link)
    if(CONFIG_APP_MEOAVE_ESPSR AND DEFINED ENV{MEOAVE_USE_ESP_SR} AND
       "$ENV{MEOAVE_USE_ESP_SR}" STREQUAL "1")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MEOAVE_ESPSR_LINKED=1)
    else()
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MEOAVE_ESPSR_LINKED=0)
    endif()
    if(CONFIG_APP_C5_NATIVE_MAINTENANCE)
        idf_component_optional_requires(PRIVATE mbedtls esp_driver_uart esp_hosted)
    endif()
    if(CONFIG_APP_MEOAVE_WIFI_HOSTED)
        # IDF's early dependency scan runs before CONFIG_APP_* is available.
        # Link the real interfaces after configuration rather than imitating
        # them with manually injected native include directories.
        idf_component_optional_requires(PRIVATE esp_hosted esp_wifi esp_netif esp_event)
        # Keep the hosted adapter's public headers visible even when this
        # vendored component is not resolved through the normal dependency
        # graph during the IDF configure pass.
        target_include_directories(${COMPONENT_LIB} PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/../components/esp_hosted/host"
            "${CMAKE_CURRENT_LIST_DIR}/../components/esp_hosted/host/api/include"
            "$ENV{IDF_PATH}/components/esp_event/include"
            "$ENV{IDF_PATH}/components/esp_netif/include"
            # Do NOT add esp_wifi/include by hand. It shadows IDF's remote
            # injected headers: main then has a 92-byte wifi_ap_record_t while
            # Hosted copies 96 bytes, corrupting stack/scan-array boundaries.
            # REQUIRES esp_wifi propagates the correct remote include order.
        )
        if(CONFIG_APP_C5_NATIVE_MAINTENANCE)
            target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../bringup/main")
        endif()
    endif()
    if(CONFIG_APP_MEOAVE_UI)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    endif()
    if(CONFIG_APP_C5_SDIO_LOADER)
        target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_BINARY_DIR}")
        if(NOT EXISTS "${C5_PAYLOAD_FILE}")
            message(FATAL_ERROR "C5_PAYLOAD_FILE must name the built C5 merged image")
        endif()
        target_add_binary_data(${COMPONENT_LIB} "${C5_PAYLOAD_FILE}" BINARY RENAME_TO c5_payload)
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

# IDF expands this file once before sdkconfig is available. Register the
# common dependencies in that pass; never fall back to the retired demo.
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(
        INCLUDE_DIRS "."
        REQUIRES esp_lcd esp_driver_i2c esp_driver_gpio esp_driver_i2s esp_adc
                 esp_event esp_hw_support esp_timer esp_app_format meo_link
        PRIV_REQUIRES esp_lcd_touch_cst820 esp_lcd_touch nvs_flash mbedtls esp_driver_uart
    )
else()
    message(FATAL_ERROR
        "The legacy application entry has been removed. Use tools/meoave.py build --profile product.")
endif()
