#include "korvo_ppa.h"
#include <stdio.h>
#include <string.h>
#include "driver/ppa.h"
#include "esp_heap_caps.h"

static ppa_client_handle_t client;
static bool enabled = true, qualified;
static unsigned fills, operations;
static uint64_t pixels;

void korvo_ppa_enable(bool value) { enabled = value; }
bool korvo_ppa_enabled(void) { return enabled && qualified; }
void korvo_ppa_reset(void) { fills = operations = 0; pixels = 0; }
void korvo_ppa_stats(void)
{
    printf("PPA_FILL enabled=%d available=%d fills=%u pixels=%llu ops=%u\n",
           korvo_ppa_enabled(), qualified, fills, (unsigned long long)pixels, operations);
}

static int fill_target(const nj_surface *surface, int x, int y, int w, int h,
                       nj_u4 packed_color, size_t *hardware_pixels)
{
    int bpp = surface->format == NJ_PIXFMT_RGB565 ? 2 :
              surface->format == NJ_PIXFMT_ARGB8888 ? 4 : 0;
    if (!client || !bpp || surface->bpp != bpp || surface->stride <= 0 ||
        surface->stride%bpp || surface->height <= 0 || ((uintptr_t)surface->pixels % bpp)) return 0;
    /* VM image data can follow an object header at a non-cache-aligned
     * address. Align the descriptor base down, skip its prefix, and stop
     * before the physical row boundary. DMA writes only inside the caller's
     * clipped rectangle. CPU tails cover its right edge and any last row
     * that cannot fit a complete cache-aligned descriptor buffer. The VM and
     * all other access to this borrowed surface remain stopped until return. */
    uintptr_t base = (uintptr_t)surface->pixels & ~(uintptr_t)63;
    unsigned prefix = (uintptr_t)surface->pixels-base;
    size_t capacity = ((size_t)surface->stride*surface->height+prefix) & ~(size_t)63;
    int picture_width = surface->stride/bpp;
    int picture_height = capacity/surface->stride;
    if (picture_height > surface->height) picture_height = surface->height;
    int hardware_width = picture_width-x-(int)prefix/bpp;
    int hardware_height = picture_height-y;
    if (hardware_width > w) hardware_width = w;
    if (hardware_height > h) hardware_height = h;
    if (hardware_width <= 0 || hardware_height <= 0) return 0;
    uint32_t color = packed_color;
    if (bpp == 2) {
        unsigned r = (color>>11)&31, g = (color>>5)&63, b = color&31;
        color = 0xff000000u | (((r<<3)|(r>>2))<<16) | (((g<<2)|(g>>4))<<8) | (b<<3)|(b>>2);
    }
    ppa_fill_oper_config_t request = {
        .out = {.buffer = (void *)base, .buffer_size = capacity,
                .pic_w = picture_width, .pic_h = picture_height,
                .block_offset_x = x+prefix/bpp, .block_offset_y = y,
                .fill_cm = bpp == 2 ? PPA_FILL_COLOR_MODE_RGB565 : PPA_FILL_COLOR_MODE_ARGB8888},
        .fill_block_w = hardware_width, .fill_block_h = hardware_height, .fill_color_val = color,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_fill(client, &request) != ESP_OK) return 0;
    ++operations;
    /* A logical row's right edge wraps into the next descriptor row's
     * prefix. Fill that narrow strip with a second GPU window when large
     * enough to offset another submission. Skip the first physical prefix
     * (the object header) and leave any final unmappable row to the CPU. */
    int tail_width = w-hardware_width;
    int tail_height = picture_height-y-1;
    if (tail_height > h) tail_height = h;
    int tail_done = 0;
    if (tail_width > 0 && tail_height > 0 &&
        (size_t)tail_width*tail_height*bpp >= 4096) {
        request.out.block_offset_x = 0;
        request.out.block_offset_y = y+1;
        request.fill_block_w = tail_width;
        request.fill_block_h = tail_height;
        if (ppa_do_fill(client, &request) == ESP_OK) {
            ++operations;
            tail_done = tail_height;
        }
    }
    for (int row = 0; row < h; ++row) {
        if (row < tail_done) continue;
        int first = row < hardware_height ? hardware_width : 0;
        uint8_t *destination = (uint8_t *)surface->pixels + (y+row)*surface->stride+(x+first)*bpp;
        for (int col = first; col < w; ++col, destination += bpp)
            memcpy(destination, &packed_color, bpp);
    }
    if (hardware_pixels) *hardware_pixels = (size_t)hardware_width*hardware_height +
                                            (size_t)tail_width*tail_done;
    return 1;
}

int korvo_ppa_fill(const nj_surface *surface, int x, int y, int w, int h, nj_u4 packed_color)
{
    size_t accelerated = 0;
    if (!korvo_ppa_enabled() || w <= 0 || h <= 0 || (size_t)w*h < 4096 ||
        !fill_target(surface, x, y, w, h, packed_color, &accelerated)) return 0;
    ++fills; pixels += accelerated;
    return 1;
}

void korvo_ppa_init(void)
{
    ppa_client_config_t config = {.oper_type = PPA_OPERATION_FILL,
        .max_pending_trans_num = 1, .data_burst_length = PPA_DATA_BURST_LENGTH_128};
    if (ppa_register_client(&config, &client) != ESP_OK) return;
    unsigned mismatches = 0;
    for (int bpp = 2; bpp <= 4; bpp += 2) {
        int stride = 40*bpp;
        size_t size = (64+52+stride*31+127)&~(size_t)63;
        uint8_t *output = heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        uint8_t *reference = heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if (!output || !reference) { heap_caps_free(output); heap_caps_free(reference); ++mismatches; break; }
        nj_surface surface = {.pixels = output, .width = 40, .height = 31,
            .stride = stride, .bpp = bpp, .format = bpp == 2 ? NJ_PIXFMT_RGB565 : NJ_PIXFMT_ARGB8888};
        const uint32_t colors[] = {0u, 0xffffffffu, 0x70123456u, 0xffa55a13u};
        const unsigned offsets[] = {0, 4, 16, 48, 52};
        for (unsigned offset = 0; offset < sizeof(offsets)/sizeof(offsets[0]); ++offset) for (unsigned shape = 0; shape < 2; ++shape) {
            unsigned prefix = 64+offsets[offset];
            surface.pixels = output+prefix;
            int x = shape ? 0 : 3, y = shape ? 0 : 5;
            int w = shape ? 40 : 31, h = shape ? 31 : 17;
            for (unsigned n = 0; n < sizeof(colors)/sizeof(colors[0]); ++n) {
                memset(output, 0xa5, size); memset(reference, 0xa5, size);
                if (!fill_target(&surface, x, y, w, h, colors[n], NULL)) { ++mismatches; break; }
                for (int row = y; row < y+h; ++row) for (int col = x; col < x+w; ++col)
                    memcpy(reference+prefix+row*stride+col*bpp, &colors[n], bpp);
                mismatches += memcmp(output, reference, size) != 0;
            }
        }
        heap_caps_free(output); heap_caps_free(reference);
        /* Exercise the wrapped GPU strip and the CPU's final-row tail. */
        stride = 128*bpp;
        size = 64+48+stride*160+80;
        output = heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        reference = heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if (!output || !reference) { heap_caps_free(output); heap_caps_free(reference); ++mismatches; break; }
        surface = (nj_surface){.pixels = output+116, .width = 128, .height = 160,
            .stride = stride, .bpp = bpp, .format = bpp == 2 ? NJ_PIXFMT_RGB565 : NJ_PIXFMT_ARGB8888};
        for (unsigned n = 0; n < sizeof(colors)/sizeof(colors[0]); ++n) {
            memset(output, 0xa5, size); memset(reference, 0xa5, size);
            if (!fill_target(&surface, 5, 7, 123, 153, colors[n], NULL)) { ++mismatches; break; }
            for (int row = 7; row < 160; ++row) for (int col = 5; col < 128; ++col)
                memcpy(reference+116+row*stride+col*bpp, &colors[n], bpp);
            mismatches += memcmp(output, reference, size) != 0;
        }
        heap_caps_free(output); heap_caps_free(reference);
    }
    qualified = mismatches == 0;
    printf("PPA_FILL_TEST available=%d mismatches=%u\n", qualified, mismatches);
    korvo_ppa_reset();
}
