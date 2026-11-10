#pragma once
#include "nj2me/nj.h"

typedef struct korvo_font korvo_font;
/* Font bytes are borrowed and must outlive the returned object. */
korvo_font *korvo_font_open(const void *data, nj_u4 size);
void korvo_font_use(korvo_font *font, nj_run_opts *opts);
void korvo_font_close(korvo_font *font);
