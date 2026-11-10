cmake_minimum_required(VERSION 3.22)
project(adapter_tests C)

enable_testing()
add_executable(jit_code_pool_test jit_code_pool_test.c ../main/jit_code_pool.c)
target_include_directories(jit_code_pool_test PRIVATE ../main)
target_compile_features(jit_code_pool_test PRIVATE c_std_11)
target_compile_options(jit_code_pool_test PRIVATE -Wall -Wextra -Werror -UNDEBUG
    -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(jit_code_pool_test PRIVATE -fsanitize=address,undefined)
add_test(NAME jit_code_pool COMMAND jit_code_pool_test)

add_executable(audio_resampler_test audio_resampler_test.c ../main/audio_resampler.c)
target_include_directories(audio_resampler_test PRIVATE ../main)
target_compile_features(audio_resampler_test PRIVATE c_std_11)
target_compile_options(audio_resampler_test PRIVATE -Wall -Wextra -Werror -UNDEBUG
    -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(audio_resampler_test PRIVATE -fsanitize=address,undefined)
add_test(NAME audio_resampler COMMAND audio_resampler_test)

add_executable(display_buffer_test display/display_buffer_test.c ../main/korvo_display.c ../main/korvo_ppa.c)
target_include_directories(display_buffer_test PRIVATE display/stubs ../main ../third_party/nano_j2me/include)
target_compile_features(display_buffer_test PRIVATE c_std_11)
target_compile_definitions(display_buffer_test PRIVATE KORVO_DISPLAY_PIPELINE=1)
target_compile_options(display_buffer_test PRIVATE -Wall -Wextra -Werror -UNDEBUG
    -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(display_buffer_test PRIVATE -fsanitize=address,undefined)
add_test(NAME display_buffers COMMAND display_buffer_test)

add_executable(display_buffer_sync_test display/display_buffer_test.c ../main/korvo_display.c ../main/korvo_ppa.c)
target_include_directories(display_buffer_sync_test PRIVATE display/stubs ../main ../third_party/nano_j2me/include)
target_compile_features(display_buffer_sync_test PRIVATE c_std_11)
target_compile_definitions(display_buffer_sync_test PRIVATE KORVO_DISPLAY_PIPELINE=0)
target_compile_options(display_buffer_sync_test PRIVATE -Wall -Wextra -Werror -UNDEBUG
    -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(display_buffer_sync_test PRIVATE -fsanitize=address,undefined)
add_test(NAME display_buffers_sync COMMAND display_buffer_sync_test)
