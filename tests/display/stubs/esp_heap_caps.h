#pragma once
#include "test_sdk.h"
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static inline void *heap_caps_aligned_alloc(size_t a, size_t n, unsigned caps)
{ (void)caps; return aligned_alloc(a, (n+a-1)&~(a-1)); }
static inline void heap_caps_free(void *p) { free(p); }
