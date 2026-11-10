#include "jit_code_pool.h"
#include <string.h>

bool jit_code_pool_init(jit_code_pool *pool, void *base, size_t bytes,
                        size_t unit_bytes, uint16_t *spans, size_t span_count)
{
    if (!pool || !base || !spans || unit_bytes < 4 ||
        (unit_bytes & (unit_bytes - 1)) || (uintptr_t)base % unit_bytes ||
        !bytes || bytes % unit_bytes || bytes / unit_bytes >= UINT16_MAX ||
        span_count < bytes / unit_bytes) return false;
    *pool = (jit_code_pool){base, spans, bytes / unit_bytes, unit_bytes, 0};
    memset(spans, 0, pool->units * sizeof(*spans));
    return true;
}

void *jit_code_pool_alloc(jit_code_pool *pool, size_t bytes)
{
    if (!pool || !pool->base || !bytes || bytes > pool->units * pool->unit_bytes)
        return NULL;
    size_t need = 1 + (bytes - 1) / pool->unit_bytes;
    size_t start = 0, run = 0;
    for (size_t i = 0; i < pool->units; ++i) {
        if (pool->spans[i]) { run = 0; continue; }
        if (!run) start = i;
        if (++run < need) continue;
        pool->spans[start] = (uint16_t)need;
        for (size_t j = 1; j < need; ++j) pool->spans[start + j] = UINT16_MAX;
        pool->live_bytes += need * pool->unit_bytes;
        return pool->base + start * pool->unit_bytes;
    }
    return NULL;
}

size_t jit_code_pool_span(const jit_code_pool *pool, const void *ptr)
{
    if (!pool || !pool->base || !ptr || (uintptr_t)ptr < (uintptr_t)pool->base)
        return 0;
    uintptr_t off = (uintptr_t)ptr - (uintptr_t)pool->base;
    if (off % pool->unit_bytes || off / pool->unit_bytes >= pool->units) return 0;
    size_t index = off / pool->unit_bytes, n = pool->spans[index];
    if (!n || n == UINT16_MAX || n > pool->units - index) return 0;
    return n * pool->unit_bytes;
}

size_t jit_code_pool_free(jit_code_pool *pool, void *ptr)
{
    size_t bytes = jit_code_pool_span(pool, ptr);
    if (!bytes) return 0;
    size_t start = ((uintptr_t)ptr - (uintptr_t)pool->base) / pool->unit_bytes;
    memset(pool->spans + start, 0, bytes / pool->unit_bytes * sizeof(*pool->spans));
    pool->live_bytes -= bytes;
    return bytes;
}
