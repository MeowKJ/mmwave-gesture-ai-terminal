cmake_minimum_required(VERSION 3.20)
project(meoave_wifi_probe_host_tests C)
enable_testing()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# The production file and its real header remain outside our write scope.
# A standalone object makes the separate translation unit easy to audit.
add_library(wifi_probe_production OBJECT ../../main/bsp_wifi_probe.c)
target_include_directories(wifi_probe_production PRIVATE stubs ../../main)
target_compile_options(wifi_probe_production PRIVATE -Wall -Wextra -Werror)
add_executable(wifi_probe_test test_wifi_probe.c $<TARGET_OBJECTS:wifi_probe_production>)
target_include_directories(wifi_probe_test PRIVATE stubs ../../main)
target_compile_options(wifi_probe_test PRIVATE -Wall -Wextra -Werror)

set(cases
    startup success token_min token_max token_numeric whitespace
    not_ready_r4 seven_funcs_r4 one_voltage_r4 slow_driver usb_failure eager_worker
    task_failure
    create_busy create_nomem create_invalidarg create_timeout create_generic
    create_hidden_nomem create_hidden_timeout
    add_nomem add_invalidstate add_invalidarg add_generic add_delete_failure add_gpio_failure
    trans_timeout trans_crc trans_invalidarg trans_generic trans_precedence trans_timeout_precedence
    cmd_timeout cmd_crc cmd_invalidarg cmd_generic
    r4_zero r4_ones r4_no_funcs r4_no_voltage r4_ready_only r4_funcs_only
    cleanup_remove cleanup_delete cleanup_gpio14 cleanup_gpio18 cleanup_gpio19
    cleanup_timeout_remove cleanup_cmd_delete cleanup_r4_gpio cleanup_first_error)
foreach(point T C A X R D 1 2 3)
    list(APPEND cases hook_${point})
endforeach()
foreach(input null empty bare tab_prefix leading_space wrong_prefix lower_prefix
              missing_command missing_token blank_token long_token huge_token
              underscore hyphen punctuation equals slash dot nonascii control extra
              extra_line split_token lower_command unknown long_command
              status_missing status_long status_extra status_bad)
    list(APPEND cases input_${input})
endforeach()
foreach(case IN LISTS cases)
    # Fresh process = C runtime initialises production statics. Never reset them.
    add_test(NAME wifi_probe.${case} COMMAND wifi_probe_test ${case})
    set_tests_properties(wifi_probe.${case} PROPERTIES TIMEOUT 10)
endforeach()
