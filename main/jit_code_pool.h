#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* External allocation metadata keeps published code cache lines immutable.
 * A caller owns the backing memory, mapping, synchronization and lifetime.
 * Operations are confined to the VM thread. */
typedef struct {
    unsigned char *base;
    uint16_t *spans;
    size_t units, unit_bytes, live_bytes;
} jit_code_pool;

bool jit_code_pool_init(jit_code_pool *pool, void *base, size_t bytes,
                        size_t unit_bytes, uint16_t *spans, size_t span_count);
void *jit_code_pool_alloc(jit_code_pool *pool, size_t bytes);
size_t jit_code_pool_span(const jit_code_pool *pool, const void *ptr);
size_t jit_code_pool_free(jit_code_pool *pool, void *ptr);
