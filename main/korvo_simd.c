#include "korvo_simd.h"
#include "korvo_parallel.h"
#include "korvo_ppa.h"
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool enabled = true;
static unsigned fills, blocks;
extern void korvo_simd_fill_blocks(uint32_t *destination, const uint32_t *value, unsigned count);

void korvo_simd_enable(bool value) { enabled = value; }
bool korvo_simd_enabled(void) { return enabled; }
void korvo_simd_reset(void) { fills = blocks = 0; }
void korvo_simd_stats(void)
{
    printf("SIMD enabled=%d core=1 fills=%u blocks128=%u\n", enabled, fills, blocks);
}

void korvo_simd_prime(void)
{
    /* This function is called by the worker on Core 1, before any VM starts.
     * Warm the task's lazy PIE state, and verify broadcast/postincrement. */
    _Alignas(16) uint32_t output[8] = {0};
    _Alignas(16) uint32_t value = 0x13579bdf;
    configASSERT(xPortGetCoreID() == 1);
    korvo_simd_fill_blocks(output, &value, 2);
    for (unsigned i = 0; i < 8; ++i) configASSERT(output[i] == value);
}

typedef struct {
    const nj_surface *surface;
    int x, y, w, h, bpp;
    _Alignas(16) uint32_t value;
} fill_job;

static void fill_band(void *context, nj_u4 part)
{
    fill_job *job = context;
    int first = part ? job->h/2 : 0;
    int end = part ? job->h : job->h/2;
    bool vector = enabled && xPortGetCoreID() == 1;
    for (int y = first; y < end; ++y) {
        uint8_t *destination = (uint8_t *)job->surface->pixels +
            (job->y+y)*job->surface->stride + job->x*job->bpp;
        unsigned bytes = job->w*job->bpp;
        while (((uintptr_t)destination & 15u) && bytes >= (unsigned)job->bpp) {
            memcpy(destination, &job->value, job->bpp);
            destination += job->bpp; bytes -= job->bpp;
        }
        unsigned count = bytes/16;
        if (count) {
            if (vector) {
                korvo_simd_fill_blocks((uint32_t *)destination, &job->value, count);
                blocks += count;
            } else {
                uint32_t *words = (uint32_t *)destination;
                for (unsigned i = 0; i < count*4; ++i) words[i] = job->value;
            }
            destination += count*16; bytes -= count*16;
        }
        while (bytes) {
            memcpy(destination, &job->value, job->bpp);
            destination += job->bpp; bytes -= job->bpp;
        }
    }
}

int korvo_simd_fill_rect(void *ud, const nj_surface *surface,
                         int x, int y, int w, int h, nj_u4 packed_color)
{
    (void)ud;
    if (korvo_ppa_fill(surface, x, y, w, h, packed_color)) return 1;
    int bpp = surface->format == NJ_PIXFMT_RGB565 ? 2 :
              surface->format == NJ_PIXFMT_ARGB8888 ? 4 : 0;
    if (!korvo_parallel_enabled() || !bpp || w <= 0 || h < 4 ||
        (size_t)w*h < 4096 || surface->bpp != bpp ||
        ((uintptr_t)surface->pixels % bpp) || surface->stride % bpp) return 0;
    uint32_t word = bpp == 2 ? (packed_color & 0xffffu)*0x10001u : packed_color;
    fill_job job = {surface, x, y, w, h, bpp, word};
    korvo_parallel_run(NULL, fill_band, &job, 2);
    ++fills;
    return 1;
}

void korvo_simd_test(void)
{
    bool ppa = korvo_ppa_enabled();
    korvo_ppa_enable(false);
    unsigned mismatches = 0, rounds = 0;
    for (int bpp = 2; bpp <= 4; bpp += 2) {
        int stride = 137*bpp+8;
        size_t size = stride*83+128;
        uint8_t *output = heap_caps_aligned_alloc(64, (size+63)&~(size_t)63, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        uint8_t *expected = heap_caps_malloc(size, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if (!output || !expected) {
            heap_caps_free(output); heap_caps_free(expected);
            korvo_ppa_enable(ppa);
            puts("SIMD_TEST allocation failed"); return;
        }
        memset(output, 0xa5, size); memset(expected, 0xa5, size);
        nj_surface surface = {.pixels = output+64+bpp, .width = 137, .height = 83,
            .stride = stride, .format = bpp == 2 ? NJ_PIXFMT_RGB565 : NJ_PIXFMT_ARGB8888, .bpp = bpp};
        for (unsigned n = 0; n < 64; ++n) {
            uint32_t value = n*1664525u+1013904223u;
            if (!korvo_simd_fill_rect(NULL, &surface, 3, 5, 129, 73, value)) {
                ++mismatches; break;
            }
            for (int y = 5; y < 78; ++y) for (int x = 3; x < 132; ++x)
                memcpy(expected+64+bpp+y*stride+x*bpp, &value, bpp);
            mismatches += memcmp(output, expected, size) != 0;
            ++rounds;
        }
        heap_caps_free(output); heap_caps_free(expected);
    }
    printf("SIMD_TEST rounds=%u mismatches=%u core=1 blocks128=%u\n", rounds, mismatches, blocks);
    korvo_ppa_enable(ppa);
}
