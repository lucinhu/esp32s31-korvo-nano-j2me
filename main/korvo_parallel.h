#pragma once
#include <stdbool.h>
#include "nj2me/nj.h"

void korvo_parallel_init(void);
void korvo_parallel_enable(bool enabled);
bool korvo_parallel_enabled(void);
void korvo_parallel_reset(void);
void korvo_parallel_stats(void);
void korvo_parallel_test(unsigned rounds);
void korvo_parallel_run(void *ud, void (*work)(void *, nj_u4), void *context, nj_u4 count);
