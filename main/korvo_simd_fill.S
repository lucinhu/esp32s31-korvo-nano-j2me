#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S31
    .text
    .align 2
    .global korvo_simd_fill_blocks
    .type korvo_simd_fill_blocks, @function
/* Core 1 only: a0=16-byte-aligned destination, a1=32-bit value address,
 * a2=nonzero number of 128-bit blocks. FreeRTOS owns PIE context switching. */
korvo_simd_fill_blocks:
    esp.vldbc.32.ip q0, a1, 0
1:
    esp.vst.128.ip q0, a0, 16
    addi a2, a2, -1
    bnez a2, 1b
    ret
    .size korvo_simd_fill_blocks, .-korvo_simd_fill_blocks
#endif
