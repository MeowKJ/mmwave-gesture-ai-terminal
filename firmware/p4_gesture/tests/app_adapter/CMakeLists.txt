cmake_minimum_required(VERSION 3.20)
project(meoave_app_adapter_test C)

enable_testing()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Compile the actual adapter and model as separate translation units.
add_executable(meoave_app_adapter_test
    test_app_adapter.c
    fake_dependencies.c
    ../../main/meoave_app.c
    ../../main/meoave_model.c)
target_include_directories(meoave_app_adapter_test PRIVATE stubs ../../main)
target_compile_options(meoave_app_adapter_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(meoave_app_adapter_test PRIVATE m)

set(cases
    before_ready defaults legacy_missing_app_list missing_namespace
    validation_low validation_high validation_min validation_max
    read_errors boolean_normalization debounce_last_value
    init_no_free_pages init_new_version init_generic_error
    open_failure set_brightness_failure set_bpm_failure
    set_reduced_failure set_app_list_failure commit_failure
    notice_retry_success notice_retry_failure
    update_during_commit_success update_during_commit_failure
    update_during_set update_during_lock update_while_notice_waits
    update_at_deadline status_retry status_before_ready
    status_during_notice_retry status_reentrant brightness_failure
    read_open_error_feedback read_value_error_feedback)
foreach(key brightness guide_bpm reduced app_list)
    list(APPEND cases read_${key}_io_error read_${key}_type_mismatch
        missing_${key}_migration)
endforeach()
foreach(case IN LISTS cases)
    add_test(NAME app_adapter.${case} COMMAND meoave_app_adapter_test ${case})
endforeach()

# Separate processes share only fake committed storage, never adapter statics.
set(image "${CMAKE_CURRENT_BINARY_DIR}/roundtrip_nvs.bin")
add_test(NAME app_adapter.roundtrip_write
    COMMAND meoave_app_adapter_test roundtrip_write "${image}")
add_test(NAME app_adapter.roundtrip_load
    COMMAND meoave_app_adapter_test roundtrip_load "${image}")
add_test(NAME app_adapter.roundtrip_read_failure
    COMMAND meoave_app_adapter_test roundtrip_read_failure "${image}")
set_tests_properties(app_adapter.roundtrip_write PROPERTIES
    FIXTURES_SETUP saved_settings RESOURCE_LOCK roundtrip_nvs)
set_tests_properties(app_adapter.roundtrip_load PROPERTIES
    FIXTURES_REQUIRED read_failure_checked RESOURCE_LOCK roundtrip_nvs)
set_tests_properties(app_adapter.roundtrip_read_failure PROPERTIES
    FIXTURES_REQUIRED saved_settings FIXTURES_SETUP read_failure_checked
    RESOURCE_LOCK roundtrip_nvs)
set_tests_properties(app_adapter.roundtrip_write app_adapter.roundtrip_load
    app_adapter.roundtrip_read_failure
    PROPERTIES TIMEOUT 10)
foreach(case IN LISTS cases)
    set_tests_properties(app_adapter.${case} PROPERTIES TIMEOUT 10)
endforeach()
