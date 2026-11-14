# esp_lvgl_port 2.7.2 returns a freed context on PPA registration failure.
# Keep the original rotation implementation and compile a corrected build
# copy. Pin the normalized upstream source so upgrades require review.
function(meoave_patch_ppa input output)
    file(READ "${input}" source)
    string(REPLACE "\r\n" "\n" source "${source}")
    string(SHA256 source_sha "${source}")
    if(NOT source_sha STREQUAL "bd2abab5e3b176c2da02f9ab190e54cc160961d268363d19499a3be7dafd8040")
        message(FATAL_ERROR "PPA guard: unreviewed esp_lvgl_port source ${source_sha}; review the error-path patch before upgrading")
    endif()
    string(REPLACE "    assert(ppa_ctx->buffer != NULL);"
        "    ESP_GOTO_ON_FALSE(ppa_ctx->buffer, ESP_ERR_NO_MEM, err, TAG, \"Not enough memory for PPA buffer allocation!\");"
        source "${source}")
    set(old_cleanup [=[    if (ret != ESP_OK) {
        if (ppa_ctx->buffer) {
            free(ppa_ctx->buffer);
        }
        if (ppa_ctx) {
            free(ppa_ctx);
        }
    }

    return ppa_ctx;]=])
    set(new_cleanup [=[    if (ret != ESP_OK) {
        if (ppa_ctx) {
            if (ppa_ctx->srm_handle) {
                ppa_unregister_client(ppa_ctx->srm_handle);
            }
            free(ppa_ctx->buffer);
            free(ppa_ctx);
        }
        return NULL;
    }

    return ppa_ctx;]=])
    string(FIND "${source}" "${old_cleanup}" cleanup_at)
    if(cleanup_at LESS 0)
        message(FATAL_ERROR "PPA guard: expected upstream cleanup missing")
    endif()
    string(REPLACE "${old_cleanup}" "${new_cleanup}" source "${source}")
    # Do not touch an identical output: preserve incremental build timestamps.
    if(EXISTS "${output}")
        file(READ "${output}" previous)
        if(previous STREQUAL source)
            return()
        endif()
    endif()
    file(WRITE "${output}" "${source}")
endfunction()

function(meoave_install_ppa_guard)
    if(NOT CONFIG_IDF_TARGET_ESP32P4 OR NOT TARGET lvgl_port_lib)
        return()
    endif()
    get_target_property(sources lvgl_port_lib SOURCES)
    get_target_property(port_dir lvgl_port_lib SOURCE_DIR)
    set(ppa_source "${port_dir}/src/common/ppa/lcd_ppa.c")
    set(patched "${CMAKE_BINARY_DIR}/meoave_patches/lcd_ppa.c")
    meoave_patch_ppa("${ppa_source}" "${patched}")
    set(found FALSE)
    set(replaced)
    foreach(source IN LISTS sources)
        get_filename_component(absolute "${source}" ABSOLUTE BASE_DIR "${port_dir}")
        if(absolute STREQUAL ppa_source)
            list(APPEND replaced "${patched}")
            set(found TRUE)
        else()
            list(APPEND replaced "${source}")
        endif()
    endforeach()
    if(NOT found)
        message(FATAL_ERROR "PPA guard: upstream compilation target changed")
    endif()
    set_property(TARGET lvgl_port_lib PROPERTY SOURCES "${replaced}")
    target_include_directories(lvgl_port_lib PRIVATE "${port_dir}/src/common/ppa")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ppa_source}")
    message(STATUS "PPA guard: hash-pinned allocation/cleanup correction (rotation unchanged)")
endfunction()
