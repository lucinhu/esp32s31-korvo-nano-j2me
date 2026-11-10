#include "korvo_display.h"
#include "korvo_board.h"
#include <stdio.h>
#include <string.h>
#include "driver/ppa.h"
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
static ppa_client_handle_t copy_client;
static unsigned front, batch;
static bool dirty;
static int x0, y0, x1, y1;
static unsigned submitted, completed, dma_copies, cpu_copies, timeouts;

uint16_t *korvo_lcd(void) { return draw; }

static bool IRAM_ATTR frame_complete(esp_lcd_panel_handle_t lcd,
                                    const esp_lcd_rgb_panel_event_data_t *event, void *context)
{
    (void)lcd; (void)event;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)context, &wake);
    return wake == pdTRUE;
}

static void present(void)
{
    if (!dirty || batch) return;
    uint16_t *ready = draw;
    (void)xSemaphoreTake(frame_done, 0);
    /* Driver-owned buffers queue a GDMA link switch without copying into
     * the live scanout buffer. The RGB driver writes back the CPU cache. */
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, ready));
    ++submitted;
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
    bool copied = false;
    if (copy_client) {
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
    dirty = false;
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

void korvo_display_stats(void)
{
    printf("DISPLAY buffers=2 rgb_dma=1 burst=64 submitted=%u completed=%u dma_copies=%u cpu_copies=%u timeouts=%u\n",
           submitted, completed, dma_copies, cpu_copies, timeouts);
}

void korvo_display_test(unsigned count)
{
    unsigned mismatches = 0;
    for (unsigned n = 0; n < count; ++n) {
        int x = (n * 37) % (LCD_W - 64), y = (n * 19) % (LCD_H - 32);
        korvo_fill(x, y, 64, 32, (uint16_t)(n * 1237));
        korvo_flush_rect(x, y, 64, 32);
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
    ESP_LOGI(TAG, "RGB GDMA burst=64, 2 PSRAM framebuffers, release-synchronized flips, PPA sync=%d", copy_client != NULL);
    return ESP_OK;
}
