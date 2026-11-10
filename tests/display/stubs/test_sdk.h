#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_TIMEOUT 2
#define IRAM_ATTR
#define ESP_ERROR_CHECK(x) assert((x)==ESP_OK)
#define ESP_RETURN_ON_ERROR(x,...) do { int test_rc=(x); if(test_rc) return test_rc; } while(0)
#define ESP_RETURN_ON_FALSE(x,rc,...) do { if(!(x)) return (rc); } while(0)
#define ESP_LOGI(tag,...) ((void)(tag))
#define ESP_LOGW(tag,...) ((void)(tag))
#define ESP_LOGE(tag,...) ((void)(tag))
#define configASSERT(x) assert(x)
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(x) (x)
typedef int BaseType_t;
typedef void *SemaphoreHandle_t;
typedef void *esp_lcd_panel_handle_t;
typedef void *ppa_client_handle_t;
typedef struct { int unused; } esp_lcd_rgb_panel_event_data_t;
typedef struct { bool (*on_frame_buf_complete)(esp_lcd_panel_handle_t,const esp_lcd_rgb_panel_event_data_t *,void *); } esp_lcd_rgb_panel_event_callbacks_t;
typedef struct { int oper_type,max_pending_trans_num,data_burst_length; } ppa_client_config_t;
typedef struct { void *buffer; size_t buffer_size; int pic_w,pic_h,block_w,block_h,block_offset_x,block_offset_y,srm_cm; } test_ppa_block;
typedef struct { test_ppa_block in,out; int rotation_angle; float scale_x,scale_y; int mode; } ppa_srm_oper_config_t;
#define PPA_SRM_COLOR_MODE_RGB565 0
#define PPA_SRM_ROTATION_ANGLE_0 0
#define PPA_TRANS_MODE_BLOCKING 0
#define PPA_OPERATION_SRM 0
#define PPA_DATA_BURST_LENGTH_128 128
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t,unsigned);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t,BaseType_t *);
esp_err_t esp_lcd_rgb_panel_get_frame_buffer(esp_lcd_panel_handle_t,unsigned,void **,void **);
esp_err_t esp_lcd_rgb_panel_register_event_callbacks(esp_lcd_panel_handle_t,const esp_lcd_rgb_panel_event_callbacks_t *,void *);
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t,int,int,int,int,const void *);
esp_err_t ppa_register_client(const ppa_client_config_t *,ppa_client_handle_t *);
esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t,const ppa_srm_oper_config_t *);
