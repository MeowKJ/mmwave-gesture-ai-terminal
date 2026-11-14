# dl_lib_free uses a private allocation prefix despite its public signature.
# Pin the reviewed P4 archive before supplying compatible allocator wrappers.
function(meoave_install_sr_simd_guard)
    if(NOT CONFIG_IDF_TARGET_ESP32P4 OR NOT CONFIG_APP_MEOAVE_ESPSR OR
       NOT "$ENV{MEOAVE_USE_ESP_SR}" STREQUAL "1")
        return()
    endif()
    if(TARGET idf::espressif__esp-sr)
        idf_component_get_property(sr_dir espressif__esp-sr COMPONENT_DIR)
    elseif(TARGET idf::esp-sr)
        idf_component_get_property(sr_dir esp-sr COMPONENT_DIR)
    else()
        message(FATAL_ERROR "SR SIMD guard: ESP-SR component missing")
    endif()
    if(CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
        message(FATAL_ERROR "SR SIMD guard: only the reviewed P4 rev >= 3 archive is supported")
    endif()
    file(SHA256 "${sr_dir}/lib/esp32p4/libdl_lib.a" archive_sha)
    if(NOT archive_sha STREQUAL "07174a92fae60884998186a843e06e9955b78c2dc4d491fe2b6de2068f6eedd2")
        message(FATAL_ERROR "SR SIMD guard: unreviewed dl_lib ${archive_sha}; review allocation/free ABI before upgrading")
    endif()
    idf_component_get_property(main_lib main COMPONENT_LIB)
    target_link_libraries(${main_lib} INTERFACE
        "-Wl,--wrap=dl_lib_calloc" "-Wl,--wrap=dl_lib_calloc_psram"
        # The wrappers are otherwise unreferenced by main itself and can be
        # discarded from its static archive before --wrap resolves ESP-SR.
        "-Wl,-u,__wrap_dl_lib_calloc" "-Wl,-u,__wrap_dl_lib_calloc_psram")
    message(STATUS "SR SIMD guard: constrain dl_lib allocations to 128-bit-capable RAM")
endfunction()
