cmake_minimum_required(VERSION 3.20)
project(meoave_usb_tx_host_tests C)

enable_testing()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Both executables compile and link the production file as its own translation
# unit. Never include its .c file or expose/reset its static state in a test.
function(usb_tx_target target enabled)
    add_executable(${target} test_usb_tx.c ../../main/bsp_usb_tx.c)
    target_include_directories(${target} PRIVATE stubs ../../main)
    target_compile_definitions(${target} PRIVATE CONFIG_APP_USB_HID_ENABLE=${enabled})
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
endfunction()

usb_tx_target(usb_tx_test 1)
usb_tx_target(usb_tx_disabled_test 0)

set(cases
    success_byte_count byte_count_equals_error newline_only exact_capacity maximum_record explicit_length
    capacity_zero capacity_one_short capacity_consumed
    invalid_null invalid_zero invalid_oversize invalid_size_max
    invalid_missing_newline invalid_newline_outside_length invalid_trailing_space
    unmounted closed isr pending_boundary_preflight
    queue_zero short_write_repair short_write_one short_write_last_byte
    boundary_no_capacity_retry boundary_write_failure_retry
    boundary_only_capacity boundary_record_one_short
    boundary_then_zero_write boundary_then_short_write
    flush_not_finished flush_failure flush_no_capacity flush_zero_write
    reentrant_capacity reentrant_write reentrant_flush
    reentrant_zero_write reentrant_no_capacity reentrant_boundary_write
    reentrant_boundary_record reentrant_boundary_failure reentrant_keeps_owner_lock)
foreach(case IN LISTS cases)
    # One process per scenario: only the C runtime initializes production statics.
    add_test(NAME usb_tx.${case} COMMAND usb_tx_test ${case})
    set_tests_properties(usb_tx.${case} PROPERTIES TIMEOUT 10)
endforeach()
add_test(NAME usb_tx.disabled COMMAND usb_tx_disabled_test disabled)
set_tests_properties(usb_tx.disabled PROPERTIES TIMEOUT 10)
