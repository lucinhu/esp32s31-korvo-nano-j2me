#pragma once
#include <stdbool.h>
#include "nj2me/nj_gfx.h"

void korvo_simd_enable(bool enabled);
bool korvo_simd_enabled(void);
void korvo_simd_reset(void);
void korvo_simd_stats(void);
void korvo_simd_test(void);
void korvo_simd_prime(void);
int korvo_simd_fill_rect(void *ud, const nj_surface *surface,
                         int x, int y, int w, int h, nj_u4 packed_color);
