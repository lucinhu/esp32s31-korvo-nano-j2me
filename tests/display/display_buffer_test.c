/* Execute the production presenter with delayed hardware completion and check
 * scanout ownership, dirty-region preservation, batching and PPA fallback. */
#include "korvo_display.h"
#include "korvo_board.h"
#include "test_sdk.h"
#include "esp_async_color_convert.h"
#include "korvo_ppa.h"
#include <stdio.h>
#include <string.h>

_Alignas(64) static uint16_t buffers[2][LCD_W * LCD_H], expected[LCD_W * LCD_H];
static const uint16_t *scanout = buffers[0], *pending;
static esp_lcd_rgb_panel_event_callbacks_t callbacks;
static void *callback_context;
static int semaphores[2], semaphore_count;
static unsigned submissions, releases, copies, fallbacks, dma_copies;
static bool fail_ppa, fail_dma, fail_fill;
static async_color_convert_request_t copy_request;
static async_color_convert_isr_cb_t copy_callback;
static void *copy_context;

SemaphoreHandle_t xSemaphoreCreateBinary(void) { assert(semaphore_count < 2); return &semaphores[semaphore_count++]; }
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t *wake)
{ assert(sem == &semaphores[0] || sem == &semaphores[1]); *(int *)sem = 1; *wake = 0; return 1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, unsigned ticks)
{
    assert(sem == &semaphores[0] || sem == &semaphores[1]);
    if (ticks && sem == &semaphores[1]) {
        assert(copy_callback && !pending && copy_request.dst_buffer != scanout);
        for (unsigned row = 0; row < copy_request.copy_height; ++row)
            memcpy((uint16_t *)copy_request.dst_buffer + (copy_request.dst_y+row)*copy_request.dst_stride + copy_request.dst_x,
                   (const uint16_t *)copy_request.src_buffer + (copy_request.src_y+row)*copy_request.src_stride + copy_request.src_x,
                   copy_request.copy_width*2);
        copy_callback(NULL, NULL, copy_context); copy_callback = NULL;
        if (copy_request.src_buffer == scanout) ++dma_copies;
    } else if (ticks) {
        assert(pending && pending != scanout);
        /* The release event occurs only after hardware stops reading the old
         * front buffer. CPU/PPA writes before here would fail below. */
        scanout = pending; pending = NULL; ++releases;
        callbacks.on_frame_buf_complete(NULL, NULL, callback_context);
    }
    int available = *(int *)sem; *(int *)sem = 0; return available;
}
esp_err_t esp_lcd_rgb_panel_get_frame_buffer(esp_lcd_panel_handle_t p, unsigned n, void **a, void **b)
{ (void)p; assert(n == 2); *a = buffers[0]; *b = buffers[1]; return ESP_OK; }
esp_err_t esp_lcd_rgb_panel_register_event_callbacks(esp_lcd_panel_handle_t p,
    const esp_lcd_rgb_panel_event_callbacks_t *c, void *ctx)
{ (void)p; callbacks = *c; callback_context = ctx; return ESP_OK; }
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x, int y, int w, int h, const void *data)
{
    (void)p; assert(x == 0 && y == 0 && w == LCD_W && h == LCD_H);
    assert(!pending && data != scanout); pending = data; ++submissions; return ESP_OK;
}
esp_err_t ppa_register_client(const ppa_client_config_t *c, ppa_client_handle_t *client)
{ assert(c->max_pending_trans_num == 1); *client = &copies; return ESP_OK; }
esp_err_t ppa_do_fill(ppa_client_handle_t client, const ppa_fill_oper_config_t *c)
{
    assert(client == &copies && c->out.buffer != scanout);
    if (fail_fill) return 7;
    int bpp = c->out.fill_cm == PPA_FILL_COLOR_MODE_RGB565 ? 2 : 4;
    uint32_t color = c->fill_color_val;
    if (bpp == 2) color = ((color>>8)&0xf800)|((color>>5)&0x7e0)|((color>>3)&31);
    for (unsigned y = 0; y < c->fill_block_h; ++y) for (unsigned x = 0; x < c->fill_block_w; ++x)
        memcpy((uint8_t *)c->out.buffer + ((c->out.block_offset_y+y)*c->out.pic_w+c->out.block_offset_x+x)*bpp,
               &color, bpp);
    return ESP_OK;
}
esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t client, const ppa_srm_oper_config_t *c)
{
    assert(client == &copies && c->out.buffer != scanout);
    if (c->scale_x == 2 || (c->out.buffer != buffers[0] && c->out.buffer != buffers[1])) {
        int scale = (int)c->scale_x;
        for (int y=0; y<c->in.block_h*scale; ++y) for (int x=0; x<c->in.block_w*scale; ++x)
            ((uint16_t *)c->out.buffer)[(c->out.block_offset_y+y)*c->out.pic_w+c->out.block_offset_x+x] =
                ((const uint16_t *)c->in.buffer)[(c->in.block_offset_y+y/scale)*c->in.pic_w+c->in.block_offset_x+x/scale];
        return ESP_OK;
    }
    assert(c->in.buffer == scanout);
    assert(!pending && c->scale_x == 1 && c->scale_y == 1);
    assert(c->in.block_offset_x >= 0 && c->in.block_offset_y >= 0);
    assert(c->in.block_offset_x + c->in.block_w <= LCD_W);
    assert(c->in.block_offset_y + c->in.block_h <= LCD_H);
    if (fail_ppa) { ++fallbacks; return 7; }
    for (int row = 0; row < c->in.block_h; ++row) {
        size_t at = (c->in.block_offset_y + row) * LCD_W + c->in.block_offset_x;
        memcpy((uint16_t *)c->out.buffer + at, (uint16_t *)c->in.buffer + at, c->in.block_w * 2);
    }
    ++copies; return ESP_OK;
}
esp_err_t esp_async_color_convert_install_dma2d(const async_color_convert_config_t *c, async_color_convert_handle_t *h)
{ assert(c->backlog == 1); *h = &dma_copies; return ESP_OK; }
esp_err_t esp_async_color_convert(async_color_convert_handle_t h, const async_color_convert_request_t *r,
                                async_color_convert_isr_cb_t cb, void *ctx)
{
    assert(h == &dma_copies && !copy_callback && r->dst_buffer != scanout && !pending);
    assert(r->src_x+r->copy_width <= r->src_stride && r->src_y+r->copy_height <= r->src_height);
    assert(r->dst_x+r->copy_width <= r->dst_stride && r->dst_y+r->copy_height <= r->dst_height);
    if (fail_dma) return 7;
    copy_request = *r; copy_callback = cb; copy_context = ctx; return ESP_OK;
}

void korvo_fill(int x, int y, int w, int h, uint16_t color)
{
    uint16_t *draw = korvo_lcd();
    assert(draw && draw != scanout && draw != pending);
    for (int row = 0; row < h; ++row) for (int col = 0; col < w; ++col) {
        int dx = x + col, dy = y + row;
        if (dx >= 0 && dx < LCD_W && dy >= 0 && dy < LCD_H)
            draw[dy * LCD_W + dx] = expected[dy * LCD_W + dx] = color;
    }
}

static void coherent(void)
{
    assert(!pending && submissions == releases);
    assert(korvo_lcd() != scanout);
    assert(memcmp(buffers[0], expected, sizeof expected) == 0);
    assert(memcmp(buffers[1], expected, sizeof expected) == 0);
}

int main(void)
{
    assert(korvo_display_init(NULL) == ESP_OK);
    korvo_fill(0, 0, LCD_W, LCD_H, 0x1234); korvo_flush(); coherent();
    nj_surface surface = {.pixels = korvo_lcd(), .width = LCD_W, .height = LCD_H,
        .stride = LCD_W*2, .format = NJ_PIXFMT_RGB565, .bpp = 2};
    assert(korvo_ppa_fill(&surface, 7, 9, 79, 61, 0x1357));
    for (int y = 9; y < 70; ++y) for (int x = 7; x < 86; ++x) expected[y*LCD_W+x] = 0x1357;
    korvo_flush_rect(7, 9, 79, 61); coherent();
    surface.pixels = korvo_lcd();
    fail_fill = true;
    assert(!korvo_ppa_fill(&surface, 7, 9, 79, 61, 0x8fff)); coherent();
    fail_fill = false;
    uint16_t source[64*32];
    for (unsigned i = 0; i < 64*32; ++i) source[i] = (uint16_t)(i*37);
    assert(korvo_display_blit(source, 64, 32, 3, 5, 17, 13, 11, 9));
    for (int y = 0; y < 13; ++y) for (int x = 0; x < 17; ++x)
        expected[(y+9)*LCD_W+x+11] = source[(y+5)*64+x+3];
    korvo_flush_rect(11, 9, 17, 13); coherent();
    assert(korvo_display_scale2(source, 64, 32, 3, 5, 31, 17, 13, 11));
    for (int y = 0; y < 34; ++y) for (int x = 0; x < 62; ++x)
        expected[(y+11)*LCD_W+x+13] = source[(y/2+5)*64+x/2+3];
    korvo_flush_rect(13, 11, 62, 34); coherent();
    for (unsigned i = 0; i < 500; ++i) {
        unsigned before = submissions;
        korvo_display_begin(); korvo_display_begin();
        for (int n = 0; n < 3; ++n) {
            int x = (i * 37 + n * 113) % 850 - 25;
            int y = (i * 19 + n * 71) % 520 - 20;
            korvo_fill(x, y, 47, 29, (uint16_t)(i * 1237 + n));
            korvo_flush_rect(x, y, 47, 29);
        }
        assert(submissions == before);
        korvo_display_end(); assert(submissions == before);
        fail_ppa = i % 17 == 0;
        fail_dma = i % 11 == 0 || fail_ppa;
        korvo_display_end(); coherent();
    }
    fail_ppa = fail_dma = false;
    korvo_display_test(90); coherent();
    assert(copies > 0 && dma_copies > 0 && fallbacks > 0 && submissions == copies + dma_copies + fallbacks);
    printf("DISPLAY_HOST_PASS frames=%u ppa=%u dma2d=%u fallback=%u\n", submissions, copies, dma_copies, fallbacks);
}
