cmake_minimum_required(VERSION 3.20)
project(fls2_display_startup_host_tests C)

enable_testing()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Compile the real implementation as a separate translation unit. The test
# never includes its .c file, replaces its header, or resets its static state.
add_executable(display_startup_test
    test_display_startup.c
    ../../main/fls2_display_startup.c)
target_include_directories(display_startup_test PRIVATE stubs ../../main)
target_compile_options(display_startup_test PRIVATE -Wall -Wextra -Werror)

set(cases
    initial cold warm reserved_bits gpio_zero gpio_high
    invalid_negative invalid_int_min invalid_shift_width invalid_int_max
    invalid_output retry_invalid
    gpio_level_error gpio_config_error retry_gpio_level retry_gpio_config
    readback_host readback_phy readback_both
    retry_readback_host retry_readback_phy retry_readback_both
    retry_readback_then_gpio_error
    repeated_success repeated_invalid_gpio)
foreach(case IN LISTS cases)
    # Fresh process: only the C runtime initializes production static state.
    add_test(NAME display_startup.${case} COMMAND display_startup_test ${case})
    set_tests_properties(display_startup.${case} PROPERTIES TIMEOUT 10)
endforeach()
