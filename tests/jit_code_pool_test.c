#include "jit_code_pool.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { UNIT = 64, UNITS = 64, SLOTS = 64 };
static _Alignas(UNIT) unsigned char backing[UNIT * UNITS];
static uint16_t spans[UNITS];
static jit_code_pool pool;
static uint32_t rng = 0x937a41c5u;
static uint32_t random_word(void) { rng = rng * 1664525u + 1013904223u; return rng; }

int main(void)
{
    assert(!jit_code_pool_init(&pool, backing + 1, sizeof(backing), UNIT, spans, UNITS));
    assert(!jit_code_pool_init(&pool, backing, sizeof(backing), 3, spans, UNITS));
    assert(!jit_code_pool_init(&pool, backing, sizeof(backing), UNIT, spans, UNITS - 1));
    assert(jit_code_pool_init(&pool, backing, sizeof(backing), UNIT, spans, UNITS));
    assert(!jit_code_pool_alloc(&pool, 0));
    assert(!jit_code_pool_alloc(&pool, SIZE_MAX));
    void *whole = jit_code_pool_alloc(&pool, sizeof(backing));
    assert(whole == backing && pool.live_bytes == sizeof(backing));
    assert(!jit_code_pool_alloc(&pool, 1));
    assert(!jit_code_pool_free(&pool, backing + 1));
    assert(!jit_code_pool_free(&pool, backing + UNIT));
    assert(!jit_code_pool_free(&pool, backing + sizeof(backing)));
    assert(jit_code_pool_free(&pool, whole) == sizeof(backing));
    assert(!jit_code_pool_free(&pool, whole));

    void *ptr[SLOTS] = {0};
    size_t length[SLOTS] = {0};
    for (unsigned i = 0; i < UNITS; ++i) {
        ptr[i] = jit_code_pool_alloc(&pool, 1);
        assert(ptr[i]);
    }
    for (unsigned i = 0; i < UNITS; i += 2) {
        assert(jit_code_pool_free(&pool, ptr[i]) == UNIT); ptr[i] = NULL;
    }
    assert(!jit_code_pool_alloc(&pool, UNIT + 1)); /* separated holes */
    for (unsigned i = 1; i < UNITS; i += 2) {
        assert(jit_code_pool_free(&pool, ptr[i]) == UNIT); ptr[i] = NULL;
    }
    whole = jit_code_pool_alloc(&pool, sizeof(backing));
    assert(whole && jit_code_pool_free(&pool, whole) == sizeof(backing));

    for (unsigned step = 0; step < 50000; ++step) {
        unsigned slot = (random_word() >> 16) % SLOTS;
        if (ptr[slot]) {
            assert(jit_code_pool_free(&pool, ptr[slot]) == length[slot]);
            ptr[slot] = NULL; length[slot] = 0;
        } else {
            size_t n = 1 + ((random_word() >> 16) % (UNIT * 8));
            ptr[slot] = jit_code_pool_alloc(&pool, n);
            if (ptr[slot]) {
                length[slot] = (n + UNIT - 1) / UNIT * UNIT;
                assert((uintptr_t)ptr[slot] % UNIT == 0);
                assert(jit_code_pool_span(&pool, ptr[slot]) == length[slot]);
                memset(ptr[slot], slot + 1, length[slot]);
            }
        }
        unsigned char owners[UNITS] = {0};
        size_t live = 0;
        for (unsigned i = 0; i < SLOTS; ++i) {
            if (!ptr[i]) continue;
            unsigned char *p = ptr[i];
            size_t start = (size_t)(p - backing) / UNIT;
            assert(start + length[i] / UNIT <= UNITS);
            for (size_t j = 0; j < length[i] / UNIT; ++j) {
                assert(!owners[start + j]); owners[start + j] = 1;
            }
            for (size_t j = 0; j < length[i]; ++j) assert(p[j] == i + 1);
            live += length[i];
        }
        assert(pool.live_bytes == live);
    }
    for (unsigned i = 0; i < SLOTS; ++i) if (ptr[i]) assert(jit_code_pool_free(&pool, ptr[i]));
    assert(!pool.live_bytes);
    assert(jit_code_pool_alloc(&pool, sizeof(backing)) == backing);
    puts("JIT_CODE_POOL_PASS 50000 interleaved operations; data, alignment, bounds and reuse verified");
    return 0;
}
