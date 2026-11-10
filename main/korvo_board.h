#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "nj2me/nj.h"

#define LCD_W 800
#define LCD_H 480
#define AUDIO_FRAMES 256

esp_err_t korvo_board_init(void);
bool korvo_sd_ready(void);
const char *korvo_storage_root(void);
esp_err_t korvo_storage_info(uint64_t *total_bytes, uint64_t *free_bytes);
uint16_t *korvo_lcd(void);
void korvo_flush(void);
void korvo_display_begin(void);
void korvo_display_end(void);
void korvo_display_stats(void);
void korvo_display_test(unsigned frames);
bool korvo_display_blit(const uint16_t *source, int width, int height,
                        int sx, int sy, int w, int h, int dx, int dy);
bool korvo_display_scale2(const uint16_t *source, int width, int height,
                          int sx, int sy, int w, int h, int dx, int dy);
void korvo_display_accel_test(void);
void korvo_flush_rect(int x, int y, int w, int h);
void korvo_fill(int x, int y, int w, int h, uint16_t color);
void korvo_text(int x, int y, const char *text, int scale, uint16_t color);
void korvo_filename(int x, int y, const char *text);
void korvo_filename_clipped(int x, int y, const char *text, int width);
int korvo_touch(uint16_t *x, uint16_t *y, int max);
int korvo_pcm_open(int rate, int channels);
int korvo_pcm_write(const int16_t *samples, int frames);
int korvo_pcm_free(void);
void korvo_pcm_close(void);
void korvo_audio_stop(void);
int korvo_tones(const nj_audio_tone *tones, int count);
void korvo_volume(int volume);
int korvo_get_volume(void);
void korvo_audio_stats(void);
void korvo_audio_reset_stats(void);
