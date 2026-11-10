#include "korvo_display.h"
#include "korvo_board.h"
#include "korvo_ppa.h"
#include <stdio.h>
#include <string.h>
#include "driver/ppa.h"
#include "esp_async_color_convert.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "korvo-display";
static uint16_t *frames[2], *draw;
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t frame_done;
static SemaphoreHandle_t copy_done;
static ppa_client_handle_t copy_client;
static async_color_convert_handle_t dma_client;
static unsigned front, batch;
static bool dirty;
static int x0, y0, x1, y1;
static unsigned submitted, completed, dma_copies, cpu_copies, timeouts;
static unsigned direct_copies, scaled_copies;
static unsigned ppa_scalings;
static uint64_t sync_us;
static bool exact_scale2;
static bool pending_frame;
static void finish_pending(void);

static bool IRAM_ATTR copy_complete(async_color_convert_handle_t client,
                                    async_color_convert_event_data_t *event, void *context)
{
    (void)client; (void)event;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)context, &wake);
    return wake == pdTRUE;
}

static bool copy_rectangle(uint16_t *destination, const uint16_t *source,
                           int source_width, int source_height, int sx, int sy,
                           int destination_width, int destination_height, int dx, int dy,
                           int width, int height)
{
    if (!dma_client) return false;
    async_color_convert_request_t request = {
        .src_buffer = source, .src_stride = source_width, .src_height = source_height,
        .src_x = sx, .src_y = sy,
        .dst_buffer = destination, .dst_stride = destination_width, .dst_height = destination_height,
        .dst_x = dx, .dst_y = dy, .copy_width = width, .copy_height = height,
        .src_color_format = ESP_COLOR_FOURCC_RGB16, .dst_color_format = ESP_COLOR_FOURCC_RGB16,
    };
    (void)xSemaphoreTake(copy_done, 0);
    if (esp_async_color_convert(dma_client, &request, copy_complete, copy_done) != ESP_OK)
        return false;
    /* An accepted request still owns the destination until completion. Never
     * fall back to CPU writes while DMA might be writing the same buffer. */
    if (xSemaphoreTake(copy_done, pdMS_TO_TICKS(250)) != pdTRUE) {
        ++timeouts;
        ESP_ERROR_CHECK(ESP_ERR_TIMEOUT);
    }
    return true;
}

uint16_t *korvo_lcd(void)
{
    finish_pending();
    return draw;
}

static bool IRAM_ATTR frame_complete(esp_lcd_panel_handle_t lcd,
                                    const esp_lcd_rgb_panel_event_data_t *event, void *context)
{
    (void)lcd; (void)event;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)context, &wake);
    return wake == pdTRUE;
}

/* A queued frame owns both scanout buffers until the release callback. Wait
 * when a caller next needs the drawing buffer, so Java computation can run
 * while the LCD switches frames. No borrowed VM surface survives present. */
static void finish_pending(void)
{
    if (!pending_frame) return;
    uint16_t *ready = draw;
    if (xSemaphoreTake(frame_done, pdMS_TO_TICKS(250)) != pdTRUE) {
        ++timeouts;
        /* Reusing a queued/scanning buffer would tear: fail instead. */
        ESP_LOGE(TAG, "RGB framebuffer release timed out; refusing unsafe reuse");
        ESP_ERROR_CHECK(ESP_ERR_TIMEOUT);
    }
    ++completed;
    unsigned released = front;
    front ^= 1u;
    draw = frames[released];
    int w = x1 - x0, h = y1 - y0;
    int64_t sync_start = esp_timer_get_time();
    bool copied = copy_rectangle(draw, ready, LCD_W, LCD_H, x0, y0,
                                  LCD_W, LCD_H, x0, y0, w, h);
    if (copied) ++direct_copies;
    if (!copied && copy_client) {
        ppa_srm_oper_config_t copy = {
            .in = {.buffer = ready, .pic_w = LCD_W, .pic_h = LCD_H,
                   .block_w = w, .block_h = h,
                   .block_offset_x = x0, .block_offset_y = y0,
                   .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
            .out = {.buffer = draw, .buffer_size = LCD_W * LCD_H * 2,
                    .pic_w = LCD_W, .pic_h = LCD_H,
                    .block_offset_x = x0, .block_offset_y = y0,
                    .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
            .scale_x = 1.0f, .scale_y = 1.0f,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        copied = ppa_do_scale_rotate_mirror(copy_client, &copy) == ESP_OK;
    }
    if (copied) ++dma_copies;
    else {
        ++cpu_copies;
        for (int row = y0; row < y1; ++row)
            memcpy(draw + row * LCD_W + x0, ready + row * LCD_W + x0, w * 2);
    }
    pending_frame = false;
    sync_us += esp_timer_get_time() - sync_start;
}

static void present(void)
{
    if (!dirty || batch) return;
    configASSERT(!pending_frame);
    (void)xSemaphoreTake(frame_done, 0);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, draw));
    ++submitted;
    pending_frame = true;
    dirty = false;
#if !KORVO_DISPLAY_PIPELINE
    finish_pending();
#endif
}

void korvo_display_begin(void) { ++batch; }
void korvo_display_end(void)
{
    configASSERT(batch);
    if (--batch == 0) present();
}

void korvo_flush_rect(int x, int y, int w, int h)
{
    int left = x < 0 ? 0 : x, top = y < 0 ? 0 : y;
    int right = x + w > LCD_W ? LCD_W : x + w;
    int bottom = y + h > LCD_H ? LCD_H : y + h;
    if (w <= 0 || h <= 0 || left >= right || top >= bottom) return;
    finish_pending();
    if (!dirty) { x0 = left; y0 = top; x1 = right; y1 = bottom; }
    else {
        if (left < x0) x0 = left;
        if (top < y0) y0 = top;
        if (right > x1) x1 = right;
        if (bottom > y1) y1 = bottom;
    }
    dirty = true;
    present();
}

void korvo_flush(void) { korvo_flush_rect(0, 0, LCD_W, LCD_H); }

bool korvo_display_blit(const uint16_t *source, int width, int height,
                        int sx, int sy, int w, int h, int dx, int dy)
{
    bool copied = copy_rectangle(korvo_lcd(), source, width, height, sx, sy,
                                  LCD_W, LCD_H, dx, dy, w, h);
    if (copied) ++scaled_copies;
    return copied;
}

static bool srm_rectangle(uint16_t *destination, int destination_width, int destination_height,
                              const uint16_t *source, int width, int height,
                              int sx, int sy, int w, int h, int dx, int dy, int factor)
{
    if (!copy_client) return false;
    ppa_srm_oper_config_t scale = {
        .in = {.buffer = source, .pic_w = width, .pic_h = height,
               .block_w = w, .block_h = h, .block_offset_x = sx, .block_offset_y = sy,
               .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = destination, .buffer_size = destination_width * destination_height * 2,
                .pic_w = destination_width, .pic_h = destination_height,
                .block_offset_x = dx, .block_offset_y = dy, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0, .scale_x = factor, .scale_y = factor,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(copy_client, &scale) == ESP_OK;
}

bool korvo_display_scale2(const uint16_t *source, int width, int height,
                          int sx, int sy, int w, int h, int dx, int dy)
{
    if (!exact_scale2 || !srm_rectangle(korvo_lcd(), LCD_W, LCD_H, source, width, height,
                                        sx, sy, w, h, dx, dy, 2)) return false;
    ++ppa_scalings;
    return true;
}

/* Qualify the real peripheral before enabling pixel-perfect scaling. The
 * output must match nearest-neighbour pixels, including odd clipped blocks
 * and untouched border pixels; an unsupported implementation stays on CPU. */
void korvo_display_accel_test(void)
{
    const int width = 176, height = 208, out_width = 384, out_height = 432;
    uint16_t *source = heap_caps_aligned_alloc(64, width*height*2, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    uint16_t *output = heap_caps_aligned_alloc(64, out_width*out_height*2, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    unsigned mismatches = 0;
    bool ok = source && output;
    int64_t start = esp_timer_get_time();
    if (ok) {
        for (int i = 0; i < width*height; ++i) source[i] = (uint16_t)(i*1237u + 0x1357u);
        for (int test = 0; test < 3; ++test) {
            int sx = test == 1 ? 3 : 0, sy = test == 1 ? 5 : 0;
            int w = test == 0 ? width : test == 1 ? 31 : 64;
            int h = test == 0 ? height : test == 1 ? 17 : 32;
            int dx = 7, dy = 3;
            for (int i = 0; i < out_width*out_height; ++i) output[i] = 0xa55a;
            if (!srm_rectangle(output, out_width, out_height, source, width, height,
                                sx, sy, w, h, dx, dy, 2)) { ok = false; break; }
            for (int y = 0; y < out_height; ++y) for (int x = 0; x < out_width; ++x) {
                uint16_t expected = 0xa55a;
                if (x >= dx && x < dx+2*w && y >= dy && y < dy+2*h)
                    expected = source[(sy+(y-dy)/2)*width + sx+(x-dx)/2];
                mismatches += output[y*out_width+x] != expected;
            }
        }
    }
    exact_scale2 = ok && mismatches == 0;
    printf("ACCEL_TEST ppa_scale2=%d mismatches=%u elapsed_us=%llu dma2d=%d\n",
           exact_scale2, mismatches, (unsigned long long)(esp_timer_get_time()-start), dma_client != NULL);
    if (source && output) {
        uint64_t elapsed[3];
        unsigned errors = 0;
        for (int method = 0; method < 3; ++method) {
            for (int i = 0; i < out_width*out_height; ++i) output[i] = 0xa55a;
            start = esp_timer_get_time();
            for (int repeat = 0; repeat < 20; ++repeat) {
                __asm__ volatile("" ::: "memory");
                if (method == 0) {
                    for (int row = 0; row < height; ++row)
                        memcpy(output + row*out_width, source + row*width, width*2);
                } else if (method == 1) {
                    errors += !srm_rectangle(output, out_width, out_height, source, width, height,
                                             0, 0, width, height, 0, 0, 1);
                } else {
                    errors += !copy_rectangle(output, source, width, height, 0, 0,
                                               out_width, out_height, 0, 0, width, height);
                }
            }
            elapsed[method] = esp_timer_get_time()-start;
            for (int y = 0; y < out_height; ++y) for (int x = 0; x < out_width; ++x)
                errors += output[y*out_width+x] != (y < height && x < width ? source[y*width+x] : 0xa55a);
        }
        printf("ACCEL_COPY pixels=%d rounds=20 cpu_us=%llu ppa_us=%llu dma2d_us=%llu errors=%u\n",
               width*height, (unsigned long long)elapsed[0], (unsigned long long)elapsed[1],
               (unsigned long long)elapsed[2], errors);
    }
    heap_caps_free(source); heap_caps_free(output);
}

void korvo_display_stats(void)
{
    finish_pending();
    printf("DISPLAY buffers=2 rgb_dma=1 burst=64 submitted=%u completed=%u dma_copies=%u cpu_copies=%u timeouts=%u dma2d_copies=%u game_blits=%u ppa_scalings=%u sync_us=%llu\n",
           submitted, completed, dma_copies, cpu_copies, timeouts, direct_copies, scaled_copies,
           ppa_scalings, (unsigned long long)sync_us);
}

void korvo_display_test(unsigned count)
{
    unsigned mismatches = 0;
    for (unsigned n = 0; n < count; ++n) {
        int x = (n * 37) % (LCD_W - 64), y = (n * 19) % (LCD_H - 32);
        korvo_fill(x, y, 64, 32, (uint16_t)(n * 1237));
        korvo_flush_rect(x, y, 64, 32);
        finish_pending();
        /* Check the whole frame: a partial PPA copy must preserve every prior
         * rectangle as well as untouched UI/background pixels. */
        mismatches += memcmp(frames[0], frames[1], LCD_W * LCD_H * 2) != 0;
    }
    printf("DISPLAY_TEST frames=%u mismatches=%u\n", count, mismatches);
    korvo_display_stats();
}

esp_err_t korvo_display_init(esp_lcd_panel_handle_t lcd)
{
    panel = lcd;
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(panel, 2,
        (void **)&frames[0], (void **)&frames[1]), TAG, "framebuffers");
    front = 0;
    draw = frames[1];
    frame_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(frame_done, ESP_ERR_NO_MEM, TAG, "frame semaphore");
    esp_lcd_rgb_panel_event_callbacks_t callbacks = {.on_frame_buf_complete = frame_complete};
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_register_event_callbacks(panel, &callbacks, frame_done), TAG, "callbacks");
    ppa_client_config_t config = {.oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1, .data_burst_length = PPA_DATA_BURST_LENGTH_128};
    if (ppa_register_client(&config, &copy_client) != ESP_OK)
        ESP_LOGW(TAG, "PPA unavailable; using CPU framebuffer sync");
    copy_done = xSemaphoreCreateBinary();
    async_color_convert_config_t dma_config = {.backlog = 1, .dma_burst_size = 128};
    if (!copy_done || esp_async_color_convert_install_dma2d(&dma_config, &dma_client) != ESP_OK)
        ESP_LOGW(TAG, "DMA2D unavailable; using PPA framebuffer sync");
    korvo_display_accel_test();
    korvo_ppa_init();
    ESP_LOGI(TAG, "RGB GDMA burst=64, 2 PSRAM framebuffers, release-synchronized flips, PPA sync=%d", copy_client != NULL);
    return ESP_OK;
}
