/* Execute the production presenter with delayed hardware completion and check
 * scanout ownership, dirty-region preservation, batching and PPA fallback. */
#include "korvo_display.h"
#include "korvo_board.h"
#include "test_sdk.h"
#include <stdio.h>
#include <string.h>

static uint16_t buffers[2][LCD_W * LCD_H], expected[LCD_W * LCD_H];
static const uint16_t *scanout = buffers[0], *pending;
static esp_lcd_rgb_panel_event_callbacks_t callbacks;
static void *callback_context;
static int semaphore;
static unsigned submissions, releases, copies, fallbacks;
static bool fail_ppa;

SemaphoreHandle_t xSemaphoreCreateBinary(void) { return &semaphore; }
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t *wake)
{ assert(sem == &semaphore); semaphore = 1; *wake = 0; return 1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, unsigned ticks)
{
    assert(sem == &semaphore);
    if (ticks) {
        assert(pending && pending != scanout);
        /* The release event occurs only after hardware stops reading the old
         * front buffer. CPU/PPA writes before here would fail below. */
        scanout = pending; pending = NULL; ++releases;
        callbacks.on_frame_buf_complete(NULL, NULL, callback_context);
    }
    int available = semaphore; semaphore = 0; return available;
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
esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t client, const ppa_srm_oper_config_t *c)
{
    assert(client == &copies && c->in.buffer == scanout && c->out.buffer != scanout);
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
        korvo_display_end(); coherent();
    }
    fail_ppa = false;
    korvo_display_test(90); coherent();
    assert(copies > 0 && fallbacks > 0 && submissions == copies + fallbacks);
    printf("DISPLAY_HOST_PASS frames=%u dma=%u fallback=%u\n", submissions, copies, fallbacks);
}
