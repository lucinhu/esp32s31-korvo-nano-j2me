idf_component_register(
    SRCS "main.c" "launcher_ui.c" "korvo_board.c" "korvo_display.c" "korvo_port.c" "korvo_jit.c" "jit_code_pool.c" "korvo_test_games.c" "audio_resampler.c"
         "korvo_image.c" "korvo_media.c" "korvo_inflate.c" "korvo_font.c"
         "korvo_midi.c" "korvo_midi_file.c" "korvo_soundfont.c"
    INCLUDE_DIRS "."
    REQUIRES nano_j2me esp_board_manager esp_timer heap esp_lcd esp_mm
             esp_driver_gpio esp_driver_uart esp_driver_ppa esp_ringbuf
             fatfs wear_levelling
             espressif__esp_lcd_touch espressif__esp_codec_dev
    PRIV_REQUIRES espressif__esp_new_jpeg espressif__libpng espressif__esp_audio_codec
                  espressif__esp_midi espressif__freetype espressif__zlib
    EMBED_FILES "../assets/korvo_demo.jar"
)
if(NOT CMAKE_SCRIPT_MODE_FILE)
    # Guard esp_midi 1.1.0's unbounded search for unmatched note-offs.
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_midi_synth_handle_note_off")
    set(KORVO_FONT_FILE "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/j2me_font.otf" CACHE FILEPATH "Embedded FreeType TTF/OTF font")
    target_add_binary_data(${COMPONENT_LIB} "${KORVO_FONT_FILE}" BINARY RENAME_TO "korvo_font")
    set(KORVO_TEST_GAME_DIR "" CACHE PATH "Optional directory of read-only test JARs; no FAT partition writes")
    set(KORVO_DIAGNOSTIC_JAR "" CACHE FILEPATH "Optional diagnostic MIDlet embedded in the application")
    set(KORVO_DIAGNOSTIC_DIR "" CACHE PATH "Optional RGB/PCM/MIDI diagnostic JAR directory")
    if(KORVO_DIAGNOSTIC_DIR)
        foreach(test rgb pcm midi)
            target_add_binary_data(${COMPONENT_LIB} "${KORVO_DIAGNOSTIC_DIR}/${test}.jar" BINARY RENAME_TO "korvo_diag_${test}")
        endforeach()
        target_compile_definitions(${COMPONENT_LIB} PRIVATE KORVO_DIAGNOSTIC_SET=1)
    endif()
    if(KORVO_DIAGNOSTIC_JAR)
        target_add_binary_data(${COMPONENT_LIB} "${KORVO_DIAGNOSTIC_JAR}" BINARY RENAME_TO "korvo_diagnostic")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE KORVO_DIAGNOSTIC=1)
    endif()
    if(KORVO_TEST_GAME_DIR)
        foreach(game dmc gof moto kernels)
            if(NOT EXISTS "${KORVO_TEST_GAME_DIR}/${game}.jar")
                message(FATAL_ERROR "Missing test JAR ${KORVO_TEST_GAME_DIR}/${game}.jar")
            endif()
            target_add_binary_data(${COMPONENT_LIB} "${KORVO_TEST_GAME_DIR}/${game}.jar" BINARY RENAME_TO "korvo_test_${game}")
        endforeach()
        target_compile_definitions(${COMPONENT_LIB} PRIVATE KORVO_TEST_GAMES=1)
    endif()
    option(KORVO_FAST_RENDER "Exact nearest-neighbour lookup scaling and unchanged-frame detection" ON)
    option(KORVO_INTERNAL_HOT_DATA "Use a bounded internal RAM budget for VM host data" ON)
    option(KORVO_AUDIO_IMPROVED "Complete PCM writes and smooth buffered output" ON)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        KORVO_FAST_RENDER=$<BOOL:${KORVO_FAST_RENDER}>
        KORVO_INTERNAL_HOT_DATA=$<BOOL:${KORVO_INTERNAL_HOT_DATA}>
        KORVO_AUDIO_IMPROVED=$<BOOL:${KORVO_AUDIO_IMPROVED}>)
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
    if(KORVO_JIT)
        # Observe/configure each VM through public APIs without patching the runner.
        target_link_options(${COMPONENT_LIB} INTERFACE
            "-Wl,--wrap=nj_vm_create" "-Wl,--wrap=nj_vm_destroy")
    endif()
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "../managed_components/espressif__esp_board_manager/devices/dev_display_lcd"
        "../managed_components/espressif__esp_board_manager/devices/dev_lcd_touch"
        "../managed_components/espressif__esp_board_manager/devices/dev_audio_codec"
    )
endif()

# Flash games separately with `idf.py j2me-flash`; normal app updates preserve saves.
fatfs_create_spiflash_image(j2me "${CMAKE_CURRENT_LIST_DIR}/../flash_data")
