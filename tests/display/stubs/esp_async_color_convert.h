#pragma once
#include "test_sdk.h"
typedef void *async_color_convert_handle_t;
typedef struct { int unused; } async_color_convert_event_data_t;
typedef bool (*async_color_convert_isr_cb_t)(async_color_convert_handle_t, async_color_convert_event_data_t *, void *);
typedef struct { unsigned backlog, dma_burst_size; } async_color_convert_config_t;
typedef struct {
    const void *src_buffer; unsigned src_stride, src_height, src_x, src_y;
    void *dst_buffer; unsigned dst_stride, dst_height, dst_x, dst_y;
    unsigned copy_width, copy_height, src_color_format, dst_color_format;
} async_color_convert_request_t;
#define ESP_COLOR_FOURCC_RGB16 1
esp_err_t esp_async_color_convert_install_dma2d(const async_color_convert_config_t *, async_color_convert_handle_t *);
esp_err_t esp_async_color_convert(async_color_convert_handle_t, const async_color_convert_request_t *, async_color_convert_isr_cb_t, void *);
