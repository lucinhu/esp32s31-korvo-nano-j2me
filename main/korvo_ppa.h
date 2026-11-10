#pragma once
#include <stdbool.h>
#include "nj2me/nj_gfx.h"
void korvo_ppa_init(void);
void korvo_ppa_enable(bool enabled);
bool korvo_ppa_enabled(void);
void korvo_ppa_reset(void);
void korvo_ppa_stats(void);
int korvo_ppa_fill(const nj_surface *surface, int x, int y, int w, int h, nj_u4 packed_color);
