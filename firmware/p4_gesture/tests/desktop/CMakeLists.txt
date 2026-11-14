cmake_minimum_required(VERSION 3.20)
project(meoave_visual_test C CXX ASM)
enable_testing()
set(CMAKE_C_STANDARD 11)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory(../../managed_components/lvgl__lvgl lvgl)
file(GLOB FONTS ../../main/fonts/meo_font_*.c)
add_executable(meoave_visual_test visual_test.c ../../main/meoave_ui.c ../../main/meoave_icons.c ../../main/meoave_model.c ${FONTS})
target_include_directories(meoave_visual_test PRIVATE ../../main)
target_link_libraries(meoave_visual_test PRIVATE lvgl::lvgl m)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(meoave_visual_test PRIVATE -Wall -Wextra -Werror)
endif()

add_executable(meoave_interaction_test interaction_test.c ../../main/meoave_icons.c ../../main/meoave_model.c ${FONTS})
target_include_directories(meoave_interaction_test PRIVATE ../../main)
target_link_libraries(meoave_interaction_test PRIVATE lvgl::lvgl m)
target_compile_options(meoave_interaction_test PRIVATE -Wall -Wextra -Werror)

add_executable(meoave_audio_preview_test ../test_audio_preview.c)
target_include_directories(meoave_audio_preview_test PRIVATE ../../main)
target_link_libraries(meoave_audio_preview_test PRIVATE m)
target_compile_options(meoave_audio_preview_test PRIVATE -Wall -Wextra -Werror)

add_executable(meoave_model_test ../test_meoave_model.c ../../main/meoave_model.c)
target_include_directories(meoave_model_test PRIVATE ../../main)
target_link_libraries(meoave_model_test PRIVATE m)
target_compile_options(meoave_model_test PRIVATE -Wall -Wextra -Werror)

add_executable(meoave_conversation_test ../test_conversation.c
               ../../main/meoave_conversation.c ../../main/meoave_model.c)
target_include_directories(meoave_conversation_test PRIVATE ../../main)
target_compile_options(meoave_conversation_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(meoave_conversation_test PRIVATE m)

set(CJSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/espressif__cjson/cJSON")
add_executable(meoave_sentence_test ../test_sentence.c ../../main/meoave_model.c
               ../../main/meoave_sentence.c ../../main/meoave_sentence_json.c
               ../../main/meoave_pcm.c "${CJSON_DIR}/cJSON.c")
target_include_directories(meoave_sentence_test PRIVATE ../../main "${CJSON_DIR}")
target_compile_options(meoave_sentence_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(meoave_sentence_test PRIVATE m)
add_test(NAME conversation.sentence COMMAND meoave_sentence_test)

add_executable(meoave_voice_worker_test ../test_voice_worker.c ../../main/meoave_model.c
               ../../main/meoave_sentence.c ../../main/meoave_pcm.c)
target_include_directories(meoave_voice_worker_test PRIVATE ../audio_adapter/stubs ../app_adapter/stubs ../../main)
target_compile_options(meoave_voice_worker_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(meoave_voice_worker_test PRIVATE m)
add_test(NAME conversation.voice_worker COMMAND meoave_voice_worker_test)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/artifacts/visual" "${CMAKE_CURRENT_BINARY_DIR}/artifacts/interaction")
add_test(NAME ui.visual COMMAND meoave_visual_test "${CMAKE_CURRENT_BINARY_DIR}/artifacts/visual")
add_test(NAME ui.interaction COMMAND meoave_interaction_test "${CMAKE_CURRENT_BINARY_DIR}/artifacts/interaction")
add_test(NAME audio.preview COMMAND meoave_audio_preview_test)
add_test(NAME model.state COMMAND meoave_model_test)
add_test(NAME conversation.state COMMAND meoave_conversation_test)

add_executable(meoave_wake_test ../test_wake.c ../../main/meoave_wake.c)
target_include_directories(meoave_wake_test PRIVATE ../app_adapter/stubs ../../main)
target_compile_options(meoave_wake_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME wake.keyword COMMAND meoave_wake_test)
set_tests_properties(ui.visual ui.interaction PROPERTIES TIMEOUT 120)

add_executable(meoave_audio_adapter_test ../audio_adapter/test_audio_adapter.c
               ../../main/meoave_model.c ../../main/meoave_conversation.c
               ../../main/meoave_wake.c ../../main/meoave_espsr.c)
target_include_directories(meoave_audio_adapter_test PRIVATE ../audio_adapter/stubs ../app_adapter/stubs ../../main)
target_compile_options(meoave_audio_adapter_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(meoave_audio_adapter_test PRIVATE m)
add_test(NAME audio.adapter COMMAND meoave_audio_adapter_test)

add_executable(meoave_espsr_adapter_test ../espsr_adapter/test_espsr_adapter.c
               ../../main/meoave_wake_dsp.c ../../main/meoave_sr_pack.c)
target_include_directories(meoave_espsr_adapter_test PRIVATE ../espsr_adapter/stubs
               ../audio_adapter/stubs ../app_adapter/stubs ../../main)
target_compile_options(meoave_espsr_adapter_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME wake.espsr_adapter COMMAND meoave_espsr_adapter_test)

add_executable(meoave_wake_dsp_test ../espsr_adapter/test_wake_dsp.c
               ../../main/meoave_wake_dsp.c ../../main/meoave_sr_pack.c)
target_include_directories(meoave_wake_dsp_test PRIVATE ../../main)
target_compile_options(meoave_wake_dsp_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(meoave_wake_dsp_test PRIVATE m)
add_test(NAME wake.dsp_and_model_pack COMMAND meoave_wake_dsp_test)

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_FOUND AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    add_test(NAME wake.simd_allocator COMMAND ${CMAKE_COMMAND} -E env
             "CC=${CMAKE_C_COMPILER}" ${Python3_EXECUTABLE}
             "${CMAKE_CURRENT_SOURCE_DIR}/../test_sr_alloc.py")
    set_tests_properties(wake.simd_allocator PROPERTIES TIMEOUT 30)
    add_test(NAME recovery.ppa_and_usb COMMAND ${CMAKE_COMMAND} -E env
             "CC=${CMAKE_C_COMPILER}" ${Python3_EXECUTABLE}
             "${CMAKE_CURRENT_SOURCE_DIR}/../test_recovery_guard.py")
    set_tests_properties(recovery.ppa_and_usb PROPERTIES TIMEOUT 60)
    add_test(NAME audio.concurrency COMMAND ${CMAKE_COMMAND} -E env
             "CC=${CMAKE_C_COMPILER}" ${Python3_EXECUTABLE}
             "${CMAKE_CURRENT_SOURCE_DIR}/../test_audio_concurrency.py")
    set_tests_properties(audio.concurrency PROPERTIES TIMEOUT 60)
endif()

# Fast page-by-page smoke test: builds a few frames for home -> settings ->
# speaker. Catches render-time aborts (bad arc angles, NULL labels) without
# flashing the board. Build with --target meoave_steady.
add_executable(meoave_steady EXCLUDE_FROM_ALL
               visual_test.c ../../main/meoave_ui.c ../../main/meoave_icons.c ../../main/meoave_model.c ${FONTS})
target_include_directories(meoave_steady PRIVATE ../../main)
target_compile_definitions(meoave_steady PRIVATE STEADY_ONLY)
target_link_libraries(meoave_steady PRIVATE lvgl::lvgl m)
