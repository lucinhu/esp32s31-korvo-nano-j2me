# Platform integration stays here; the core submodule follows main.
set(NJ_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../third_party/nano_j2me")
if(NOT EXISTS "${NJ_ROOT}/include/nj2me/nj.h")
    message(FATAL_ERROR "Run git submodule update --init --recursive first")
endif()
idf_component_register(
    SRCS
        "${NJ_ROOT}/src/core/nj_heap.c"
        "${NJ_ROOT}/src/core/nj_string.c"
        "${NJ_ROOT}/src/core/nj_class.c"
        "${NJ_ROOT}/src/core/nj_native.c"
        "${NJ_ROOT}/src/core/nj_vm.c"
        "${NJ_ROOT}/src/core/nj_thread.c"
        "${NJ_ROOT}/src/core/nj_interp.c"
        "${NJ_ROOT}/src/core/nj_engine.c"
        "${NJ_ROOT}/src/core/nj_avcodec.c"
        "${NJ_ROOT}/src/core/nj_jit.c"
        "${NJ_ROOT}/src/core/nj_jit_rv32.c"
        "${NJ_ROOT}/src/core/nj_jit_arm64.c"
        "${NJ_ROOT}/src/core/nj_jit_x86_64.c"
        "${NJ_ROOT}/src/core/nj_app.c"
        "${NJ_ROOT}/src/core/nj_builtins.c"
        "${NJ_ROOT}/src/core/nj_device.c"
        "${NJ_ROOT}/src/core/nj_detect.c"
        "${NJ_ROOT}/src/core/nj_save.c"
        "${NJ_ROOT}/src/core/nj_lib_object.c"
        "${NJ_ROOT}/src/core/nj_lib_string.c"
        "${NJ_ROOT}/src/core/nj_lib_system.c"
        "${NJ_ROOT}/src/core/nj_lib_math.c"
        "${NJ_ROOT}/src/core/nj_lib_thread.c"
        "${NJ_ROOT}/src/core/nj_lib_boxing.c"
        "${NJ_ROOT}/src/midp/nj_lib_util.c"
        "${NJ_ROOT}/src/midp/nj_lib_io.c"
        "${NJ_ROOT}/src/midp/nj_lib_http.c"
        "${NJ_ROOT}/src/midp/nj_lib_pim.c"
        "${NJ_ROOT}/src/midp/nj_lib_samsung.c"
        "${NJ_ROOT}/src/midp/nj_lib_socket.c"
        "${NJ_ROOT}/src/midp/nj_lib_datagram.c"
        "${NJ_ROOT}/src/midp/nj_lib_sensor.c"
        "${NJ_ROOT}/src/midp/nj_lib_bt.c"
        "${NJ_ROOT}/src/midp/nj_lib_lcdui.c"
        "${NJ_ROOT}/src/midp/nj_lib_game.c"
        "${NJ_ROOT}/src/midp/nj_lib_rms.c"
        "${NJ_ROOT}/src/midp/nj_lib_midlet.c"
        "${NJ_ROOT}/src/midp/nj_lib_media.c"
        "${NJ_ROOT}/src/midp/nj_midi.c"
        "${NJ_ROOT}/src/midp/nj_mp3.c"
        "${NJ_ROOT}/src/midp/nj_mp4.c"
        "${NJ_ROOT}/src/midp/nj_m4v.c"
        "${NJ_ROOT}/src/midp/nj_h263.c"
        "${NJ_ROOT}/src/midp/nj_amr.c"
        "${NJ_ROOT}/src/midp/nj_amr_mp4.c"
        "${NJ_ROOT}/src/midp/nj_lib_content.c"
        "${NJ_ROOT}/src/midp/nj_lib_sax.c"
        "${NJ_ROOT}/src/midp/nj_lib_nokia.c"
        "${NJ_ROOT}/src/midp/nj_lib_fc.c"
        "${NJ_ROOT}/src/midp/nj_lib_wireless.c"
        "${NJ_ROOT}/src/midp/nj_lib_m3g.c"
        "${NJ_ROOT}/src/midp/nj_lib_mascot.c"
        "${NJ_ROOT}/src/midp/nj_mascot_model.c"
        "${NJ_ROOT}/src/midp/nj_mascot_anim.c"
        "${NJ_ROOT}/src/midp/nj_mascot_raster.c"
        "${NJ_ROOT}/src/gfx/nj_gfx.c"
        "${NJ_ROOT}/src/gfx/nj_text.c"
        "${NJ_ROOT}/src/gfx/nj_font.c"
        "${NJ_ROOT}/src/gfx/nj_font_ttf.c"
        "${NJ_ROOT}/src/gfx/nj_ttf.c"
        "${NJ_ROOT}/src/gfx/nj_png.c"
        "${NJ_ROOT}/src/gfx/nj_jpeg.c"
        "${NJ_ROOT}/src/gfx/nj_codec.c"
        "${NJ_ROOT}/src/gfx/nj_gif.c"
        "${NJ_ROOT}/src/zip/nj_inflate.c"
        "${NJ_ROOT}/src/zip/nj_zip.c"
        "${NJ_ROOT}/src/runner/nj_run.c"
    INCLUDE_DIRS "${NJ_ROOT}/include"
    PRIV_INCLUDE_DIRS "${NJ_ROOT}/src/core"
    REQUIRES vfs fatfs
    LDFRAGMENTS "linker.lf"
)
# Same public ABI in frontend and runtime. Large allocations use PSRAM hooks.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    NJ_MAX_HEAP=10485760 NJ_MAX_STACK_SLOTS=4096 NJ_MAX_FRAMES=192
    NJ_SCREEN_WIDTH=240 NJ_SCREEN_HEIGHT=320
    NJ_PIXFMT=NJ_PIXFMT_RGB565 NJ_ENABLE_MEDIA=1 NJ_ENABLE_JPEG=1
    NJ_MP4_SEPARATE
)
option(KORVO_THREADED_DISPATCH "Use upstream's computed-goto interpreter" OFF)
option(KORVO_SUPERINSTR "Use upstream fused opcode handlers (benchmark on RV32)" OFF)
option(KORVO_JIT "Build upstream RV32 JIT with board executable-memory hooks" ON)
set(KORVO_JIT_CACHE_ENTRIES 1024 CACHE STRING "Maximum compiled blocks per VM")
set(KORVO_JIT_CACHE_WAYS 4 CACHE STRING "Associativity of the compiled-block cache")
set(KORVO_JIT_METHOD_ENTRIES 1024 CACHE STRING "Concurrent JIT method maps")
set(KORVO_JIT_METHOD_ARENA_BYTES 262144 CACHE STRING "Byte budget for JIT hot-site maps")
set(KORVO_JIT_MIN_BLOCK_INSNS 8 CACHE STRING "Minimum useful bytecodes in a compiled block")
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    NJ_THREADED_DISPATCH=$<BOOL:${KORVO_THREADED_DISPATCH}>
    NJ_ENABLE_SUPERINSTR=$<BOOL:${KORVO_SUPERINSTR}>
    NJ_ENABLE_JIT=$<BOOL:${KORVO_JIT}>
    NJ_JIT_CACHE_ENTRIES=${KORVO_JIT_CACHE_ENTRIES}
    NJ_JIT_CACHE_WAYS=${KORVO_JIT_CACHE_WAYS}
    NJ_JIT_METHOD_ENTRIES=${KORVO_JIT_METHOD_ENTRIES}
    NJ_JIT_METHOD_ARENA_BYTES=${KORVO_JIT_METHOD_ARENA_BYTES}
    NJ_JIT_MIN_BLOCK_INSNS=${KORVO_JIT_MIN_BLOCK_INSNS})
# Jump tables are safe here: the VM never runs in interrupt context.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fjump-tables
    -ftree-switch-conversion -Wno-error -Werror=format)
target_link_libraries(${COMPONENT_LIB} PUBLIC m)
