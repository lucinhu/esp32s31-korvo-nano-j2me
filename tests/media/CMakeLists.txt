cmake_minimum_required(VERSION 3.22)
project(korvo_media_test C)

set(NJ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NJ_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(NJ_BUILD_LIBRETRO OFF CACHE BOOL "" FORCE)
set(NJ_BUILD_SDL2 OFF CACHE BOOL "" FORCE)
set(NJ_BUILD_EMBEDDED_PORTS OFF CACHE BOOL "" FORCE)
set(NJ_ENABLE_FONT_CJK 0 CACHE STRING "" FORCE)
set(NJ_ENABLE_FONT_CJK_AA 0 CACHE STRING "" FORCE)
set(NJ_SANITIZE ON CACHE BOOL "" FORCE)
set(NJ_ENABLE_MEDIA 1 CACHE STRING "" FORCE)
set(NJ_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../third_party/nano_j2me")
set(ESP_COMPONENTS "${CMAKE_CURRENT_LIST_DIR}/../../managed_components")
add_subdirectory("${NJ_ROOT}" core)
target_compile_definitions(nj2me PUBLIC NJ_MAX_HEAP=6291456)
find_package(PNG REQUIRED)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory("${ESP_COMPONENTS}/espressif__freetype/freetype" freetype)
find_package(Threads REQUIRED)
add_executable(korvo_media_test media_test.c ../../main/korvo_image.c ../../main/korvo_media.c
    ../../main/korvo_inflate.c ../../main/korvo_font.c
    midi_test.c ../../main/korvo_midi.c ../../main/korvo_midi_file.c ../../main/korvo_soundfont.c)
target_compile_definitions(korvo_media_test PRIVATE
    KORVO_TEST_FONT="${CMAKE_CURRENT_LIST_DIR}/../../assets/fonts/j2me_font.otf")
target_compile_definitions(korvo_media_test PRIVATE
    KORVO_TEST_JAR="${CMAKE_CURRENT_LIST_DIR}/../../assets/korvo_demo.jar")
target_include_directories(korvo_media_test PRIVATE
    ../../main stubs
    "${ESP_COMPONENTS}/espressif__esp_new_jpeg/include"
    "${ESP_COMPONENTS}/espressif__esp_midi/include"
    "${ESP_COMPONENTS}/espressif__esp_audio_codec/include"
    "${ESP_COMPONENTS}/espressif__esp_audio_codec/include/decoder"
    "${ESP_COMPONENTS}/espressif__esp_audio_codec/include/decoder/impl"
    "${ESP_COMPONENTS}/espressif__esp_audio_codec/include/simple_dec")
target_compile_options(korvo_media_test PRIVATE -Wall -Wextra -Werror
    -UNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(korvo_media_test PRIVATE -fsanitize=address,undefined)
target_link_libraries(korvo_media_test PRIVATE nj2me PNG::PNG freetype ZLIB::ZLIB Threads::Threads)
enable_testing()
add_test(NAME korvo_media COMMAND korvo_media_test)
add_executable(lcdui_memory_test "${NJ_ROOT}/tests/test_lcdui.c")
target_include_directories(lcdui_memory_test PRIVATE "${NJ_ROOT}/src/core")
target_link_libraries(lcdui_memory_test PRIVATE nj2me)
add_test(NAME lcdui_memory COMMAND lcdui_memory_test "${NJ_ROOT}/tests/fixtures")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../build-media/validation/short-pcm-bench.jar")
    add_executable(pcm_lifecycle_test pcm_lifecycle_test.c)
    target_compile_options(pcm_lifecycle_test PRIVATE -UNDEBUG)
    target_link_libraries(pcm_lifecycle_test PRIVATE nj2me)
    add_test(NAME short_pcm_lifecycle COMMAND pcm_lifecycle_test
        "${CMAKE_CURRENT_LIST_DIR}/../../build-media/validation/short-pcm-bench.jar")
endif()
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../build-media/validation/upstream-audiodemo.jar")
    add_executable(upstream_audio_test "${NJ_ROOT}/tests/test_audio.c")
    target_link_libraries(upstream_audio_test PRIVATE nj2me)
    add_test(NAME upstream_audio COMMAND upstream_audio_test
        "${CMAKE_CURRENT_LIST_DIR}/../../build-media/validation/upstream-audiodemo.jar")
endif()
