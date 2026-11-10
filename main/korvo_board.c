#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "korvo_board.h"
#include "korvo_display.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "audio_resampler.h"
#include "audio_timing.h"
#include "driver/gpio.h"
#include "esp_board_manager.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "dev_audio_codec.h"
#include "dev_display_lcd.h"
#include "dev_lcd_touch.h"
#include "ui_font.h"

static const char *TAG = "korvo-board";
static esp_lcd_touch_handle_t touch;
static esp_codec_dev_handle_t codec;
static bool sd_ready;
static bool flash_ready;
static wl_handle_t flash_wl = WL_INVALID_HANDLE;
static QueueHandle_t audio_queue;
static _Atomic unsigned audio_epoch;
static _Atomic int volume = 70;
static _Atomic bool pcm_active;
static _Atomic unsigned audio_underruns, audio_silence_frames;
static int applied_volume = -1;
static int pcm_rate = 48000, pcm_channels = 2;
static _Atomic unsigned audio_writes, audio_frames, audio_errors, tone_count;
static _Atomic unsigned audio_offered, audio_accepted, audio_short, audio_io_us, audio_volume_calls;

typedef struct {
    unsigned epoch;
    int rate, frames;
    nj_audio_tone tone;
    int16_t samples[AUDIO_FRAMES * 2];
} audio_packet;

bool korvo_sd_ready(void) { return sd_ready; }
const char *korvo_storage_root(void) { return sd_ready ? "/sdcard" : flash_ready ? "/flash" : NULL; }
esp_err_t korvo_storage_info(uint64_t *total_bytes, uint64_t *free_bytes)
{
    const char *root = korvo_storage_root();
    return root ? esp_vfs_fat_info(root, total_bytes, free_bytes) : ESP_ERR_INVALID_STATE;
}

void korvo_filename_clipped(int x, int y, const char *text, int width)
{
    uint16_t *lcd_fb = korvo_lcd();
    nj_u2 chars[128];
    int n = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p && n < 128) {
        unsigned ch = *p++;
        if (ch >= 0xc2 && ch <= 0xdf && p[0] >= 0x80 && p[0] <= 0xbf) {
            ch = ((ch & 31) << 6) | (*p++ & 63);
        } else if (ch >= 0xe0 && ch <= 0xef && p[0] && p[1] &&
                   (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80) {
            ch = ((ch & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2;
        } else if (ch >= 128) ch = 0x25a1;
        chars[n++] = ch;
    }
    nj_fontset fonts = {0};
    nj_fontset_init(&fonts);
    nj_surface surface = { .pixels = lcd_fb, .width = LCD_W, .height = LCD_H,
                           .stride = LCD_W * 2, .format = NJ_PIXFMT_RGB565, .bpp = 2 };
    nj_gfx gfx;
    nj_gfx_init(&gfx, &surface);
    nj_gfx_set_clip(&gfx, x, y, width, 30);
    nj_gfx_set_font(&gfx, nj_fontset_get(&fonts, NJ_FONT_SIZE_LARGE, 0));
    nj_gfx_set_color(&gfx, 0xffefefef);
    nj_gfx_draw_text(&gfx, chars, n, x, y, NJ_ANCHOR_TOP | NJ_ANCHOR_LEFT);
}

void korvo_filename(int x, int y, const char *text)
{
    korvo_filename_clipped(x, y, text, 740);
}

void korvo_fill(int x, int y, int w, int h, uint16_t color)
{
    uint16_t *lcd_fb = korvo_lcd();
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > LCD_W ? LCD_W : x + w;
    int y1 = y + h > LCD_H ? LCD_H : y + h;
    for (int row = y0; row < y1; ++row)
        for (int col = x0; col < x1; ++col) lcd_fb[row * LCD_W + col] = color;
}

void korvo_text(int x, int y, const char *text, int scale, uint16_t color)
{
    for (; *text && x < LCD_W; ++text, x += 6 * scale) {
        const uint8_t *rows = glyph_rows(*text);
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (rows[row] & (1 << (4 - col)))
                    korvo_fill(x + col * scale, y + row * scale, scale, scale, color);
    }
}

int korvo_touch(uint16_t *x, uint16_t *y, int max)
{
    esp_lcd_touch_point_data_t points[5];
    uint8_t count = 0;
    if (!touch || esp_lcd_touch_read_data(touch) != ESP_OK) return 0;
    if (max > 5) max = 5;
    if (esp_lcd_touch_get_data(touch, points, &count, max) != ESP_OK) return 0;
    for (int i = 0; i < count; ++i) { x[i] = points[i].x; y[i] = points[i].y; }
    return count;
}

static void write_audio(int16_t *samples, int frames)
{
    int64_t start = esp_timer_get_time();
    int vol = atomic_load(&volume);
#if KORVO_AUDIO_IMPROVED
    if (vol != applied_volume) {
        if (esp_codec_dev_set_out_vol(codec, vol) == ESP_CODEC_DEV_OK) applied_volume = vol;
        atomic_fetch_add(&audio_volume_calls, 1);
    }
#else
    esp_codec_dev_set_out_vol(codec, vol);
    atomic_fetch_add(&audio_volume_calls, 1);
#endif
    int rc = esp_codec_dev_write(codec, samples, frames * 4);
    if (rc == ESP_CODEC_DEV_OK) {
        atomic_fetch_add(&audio_writes, 1);
        atomic_fetch_add(&audio_frames, frames);
    } else atomic_fetch_add(&audio_errors, 1);
    atomic_fetch_add(&audio_io_us, (unsigned)(esp_timer_get_time() - start));
}

#if KORVO_AUDIO_IMPROVED
static const int16_t sine_wave[256] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602,
    6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285,
    32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571,
    30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683,
    27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868,
    18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279,
    12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179,
    6393, 5602, 4808, 4011, 3212, 2410, 1608, 804,
    0, -804, -1608, -2410, -3212, -4011, -4808, -5602,
    -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
    -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
    -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
    -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
    -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179,
    -6393, -5602, -4808, -4011, -3212, -2410, -1608, -804
};

static void audio_task(void *arg)
{
    (void)arg;
    audio_packet packet;
    int16_t out[512 * 2];
    audio_resampler_t resampler;
    audio_resampler_reset(&resampler,48000);
    unsigned epoch=atomic_load(&audio_epoch), silent_chunks=3;
    int used=0;
    bool fed=false, starving=false;
    while (true) {
        unsigned current=atomic_load(&audio_epoch);
        if (epoch != current) {
            bool dirty=used || fed || silent_chunks<3;
            epoch=current; used=0; fed=false; starving=false; silent_chunks=dirty ? 0 : 3;
            audio_resampler_reset(&resampler,48000);
        }
        TickType_t wait=(used || (atomic_load(&pcm_active) && fed) || silent_chunks<3) ? 0 : pdMS_TO_TICKS(30);
        if (xQueueReceive(audio_queue,&packet,wait) != pdTRUE) {
            bool active=atomic_load(&pcm_active);
            if (used || (active && fed) || silent_chunks<3) {
                if (active && fed && !starving) { atomic_fetch_add(&audio_underruns,1); starving=true; }
                int missing=512-used;
                /* Ramp the final held sample down rather than replaying stale DMA data. */
                int16_t left=used ? out[(used-1)*2] : 0, right=used ? out[(used-1)*2+1] : 0;
                for (int i=0;i<missing;++i) {
                    int gain=i<240 ? 240-i : 0;
                    out[(used+i)*2]=left*gain/240;
                    out[(used+i)*2+1]=right*gain/240;
                }
                atomic_fetch_add(&audio_silence_frames,missing);
                write_audio(out,512); used=0; ++silent_chunks;
                if (!active && silent_chunks>=3) fed=false;
            }
            continue;
        }
        if (packet.epoch != atomic_load(&audio_epoch)) continue;
        if (packet.epoch != epoch) { epoch=packet.epoch; used=0; fed=false; audio_resampler_reset(&resampler,48000); }
        silent_chunks=0; starving=false;
        if (packet.frames == 0) {
            unsigned total=(unsigned)packet.tone.duration_ms*48, remaining=total, phase=0;
            unsigned step=(uint64_t)packet.tone.freq_hz*4294967296ULL/48000;
            used=0;
            while (remaining && epoch==atomic_load(&audio_epoch)) {
                unsigned n=remaining>512 ? 512 : remaining;
                for (unsigned i=0;i<n;++i) {
                    unsigned at=total-remaining+i;
                    unsigned envelope=at<240 ? at : 240;
                    if (remaining-i<envelope) envelope=remaining-i;
                    int v=packet.tone.freq_hz ? (sine_wave[phase>>24]*3500/32768)*(int)envelope/240 : 0;
                    phase+=step; out[i*2]=out[i*2+1]=v;
                }
                write_audio(out,n); remaining-=n;
            }
            fed=false;
            continue;
        }
        if (resampler.source_rate != packet.rate) { used=0; audio_resampler_reset(&resampler,packet.rate); }
        fed=true;
        for (int i=0;i<packet.frames && epoch==atomic_load(&audio_epoch);++i) {
            const int16_t *sample=packet.samples+i*2;
            while (audio_resampler_next(&resampler,sample,out+used*2)) {
                if (++used==512) {
                    if (epoch==atomic_load(&audio_epoch)) write_audio(out,used);
                    used=0;
                }
            }
            audio_resampler_push(&resampler,sample);
        }
    }
}
#else
static void audio_task(void *arg)
{
    (void)arg;
    audio_packet packet;
    int16_t out[512 * 2];
    audio_resampler_t resampler;
    audio_resampler_reset(&resampler, 48000);
    unsigned epoch = 0;
    while (true) {
        if (xQueueReceive(audio_queue, &packet, portMAX_DELAY) != pdTRUE) continue;
        if (packet.epoch != atomic_load(&audio_epoch)) continue;
        if (packet.frames == 0) {
            unsigned remaining = (unsigned)packet.tone.duration_ms * 48;
            unsigned phase = 0;
            while (remaining && packet.epoch == atomic_load(&audio_epoch)) {
                unsigned n = remaining > 512 ? 512 : remaining;
                for (unsigned i = 0; i < n; ++i) {
                    /* A quiet square wave, with rests preserved. */
                    phase = (phase + packet.tone.freq_hz) % 48000;
                    int16_t v = packet.tone.freq_hz ? (phase < 24000 ? 3500 : -3500) : 0;
                    out[i * 2] = out[i * 2 + 1] = v;
                }
                write_audio(out, n);
                remaining -= n;
            }
            continue;
        }
        if (epoch != packet.epoch || resampler.source_rate != packet.rate) {
            epoch = packet.epoch;
            audio_resampler_reset(&resampler, packet.rate);
        }
        int used = 0;
        for (int i = 0; i < packet.frames; ++i) {
            const int16_t *sample = &packet.samples[i * 2];
            while (audio_resampler_next(&resampler, sample, out + used * 2)) {
                if (++used == 512) { write_audio(out, used); used = 0; }
            }
            audio_resampler_push(&resampler, sample);
        }
        if (used) write_audio(out, used);
    }
}

#endif

void korvo_audio_stop(void)
{
    atomic_store(&pcm_active,false);
    atomic_fetch_add(&audio_epoch, 1);
    if (audio_queue) xQueueReset(audio_queue);
}

int korvo_pcm_open(int rate, int channels)
{
    if (!codec || rate < 4000 || rate > 96000 || (channels != 1 && channels != 2)) return -1;
    korvo_audio_stop();
    pcm_rate = rate; pcm_channels = channels;
    atomic_store(&pcm_active,true);
    ESP_LOGI(TAG, "PCM %d Hz %d channel(s) -> 48000 Hz stereo", rate, channels);
    return 0;
}

int korvo_pcm_write(const int16_t *samples, int frames)
{
    if (!codec || !samples || frames <= 0) return 0;
#if KORVO_AUDIO_IMPROVED
    /* Player.start() submits its first block without consulting pcm_free().
     * In particular, the MIDI stream has already advanced by 4096 frames.
     * Accept that initial burst into the physical queue; pcm_free() applies
     * the lower latency watermark to subsequent replenishment. WAV may still
     * short-write its whole-file first submission, which upstream retries. */
    int room=audio_queue ? uxQueueSpacesAvailable(audio_queue)*AUDIO_FRAMES : 0;
    int count=frames<room ? frames : room;
    int accepted=0;
    audio_packet packet={ .epoch=atomic_load(&audio_epoch), .rate=pcm_rate };
    while (accepted<count) {
        packet.frames=count-accepted>AUDIO_FRAMES ? AUDIO_FRAMES : count-accepted;
        if (pcm_channels==2) memcpy(packet.samples,samples+accepted*2,packet.frames*4);
        else for (int i=0;i<packet.frames;++i) packet.samples[i*2]=packet.samples[i*2+1]=samples[accepted+i];
        if (xQueueSend(audio_queue,&packet,0) != pdTRUE) break;
        accepted+=packet.frames;
    }
    atomic_fetch_add(&audio_offered,frames); atomic_fetch_add(&audio_accepted,accepted);
    if (accepted<frames) atomic_fetch_add(&audio_short,1);
    return accepted;
#else
    audio_packet packet = { .epoch = atomic_load(&audio_epoch), .rate = pcm_rate };
    packet.frames = frames > AUDIO_FRAMES ? AUDIO_FRAMES : frames;
    for (int i = 0; i < packet.frames; ++i) {
        packet.samples[i * 2] = samples[i * pcm_channels];
        packet.samples[i * 2 + 1] = samples[i * pcm_channels + pcm_channels - 1];
    }
    int accepted = xQueueSend(audio_queue, &packet, 0) == pdTRUE ? packet.frames : 0;
    atomic_fetch_add(&audio_offered, frames);
    atomic_fetch_add(&audio_accepted, accepted);
    if (accepted < frames) atomic_fetch_add(&audio_short, 1);
    return accepted;
#endif
}

int korvo_pcm_free(void)
{
#if KORVO_AUDIO_IMPROVED
    if (!audio_queue || !atomic_load(&pcm_active)) return 0;
    int packets=(pcm_rate*AUDIO_QUEUE_MS+1000*AUDIO_FRAMES-1)/(1000*AUDIO_FRAMES);
    if (packets>32) packets=32;
    int waiting=uxQueueMessagesWaiting(audio_queue);
    return waiting<packets ? (packets-waiting)*AUDIO_FRAMES : 0;
#else
    return audio_queue ? uxQueueSpacesAvailable(audio_queue) * AUDIO_FRAMES : 0;
#endif
}

void korvo_pcm_close(void)
{
#if KORVO_AUDIO_IMPROVED
    /* A completed Player has already queued its final PCM. Let it drain. */
    atomic_store(&pcm_active,false);
#else
    korvo_audio_stop();
#endif
}

int korvo_tones(const nj_audio_tone *tones, int count)
{
    if (!codec || !tones || count <= 0 || count > 32) return -1;
    korvo_audio_stop();
    for (int i = 0; i < count; ++i) {
        audio_packet packet = { .epoch = atomic_load(&audio_epoch), .tone = tones[i] };
        if (xQueueSend(audio_queue, &packet, 0) != pdTRUE) { korvo_audio_stop(); return -1; }
    }
    atomic_fetch_add(&tone_count, count);
    return 0;
}

void korvo_volume(int vol) { atomic_store(&volume, vol < 0 ? 0 : vol > 100 ? 100 : vol); }
int korvo_get_volume(void) { return atomic_load(&volume); }
void korvo_audio_stats(void)
{
    ESP_LOGI(TAG, "AUDIO writes=%u frames=%u errors=%u tones=%u volume=%d",
             atomic_load(&audio_writes), atomic_load(&audio_frames),
             atomic_load(&audio_errors), atomic_load(&tone_count), korvo_get_volume());
    ESP_LOGI(TAG, "AUDIO_DETAIL offered=%u accepted=%u short=%u io_us=%u volume_calls=%u queued=%u underruns=%u silence_frames=%u",
             atomic_load(&audio_offered),atomic_load(&audio_accepted),atomic_load(&audio_short),
             atomic_load(&audio_io_us),atomic_load(&audio_volume_calls),
             audio_queue ? (unsigned)uxQueueMessagesWaiting(audio_queue) : 0,
             atomic_load(&audio_underruns),atomic_load(&audio_silence_frames));
}

void korvo_audio_reset_stats(void)
{
    atomic_store(&audio_writes,0); atomic_store(&audio_frames,0); atomic_store(&audio_errors,0);
    atomic_store(&tone_count,0); atomic_store(&audio_offered,0); atomic_store(&audio_accepted,0);
    atomic_store(&audio_short,0); atomic_store(&audio_io_us,0); atomic_store(&audio_volume_calls,0);
    atomic_store(&audio_underruns,0); atomic_store(&audio_silence_frames,0);
}

esp_err_t korvo_board_init(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_RETURN_ON_ERROR(esp_board_manager_init(), TAG, "board init");
    dev_display_lcd_handles_t *lcd = NULL;
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle("display_lcd", (void **)&lcd), TAG, "LCD");
    ESP_RETURN_ON_ERROR(korvo_display_init(lcd->panel_handle), TAG, "LCD presentation");
    dev_lcd_touch_handles_t *tp = NULL;
    if (esp_board_manager_get_device_handle("lcd_touch", (void **)&tp) == ESP_OK && tp) touch = tp->touch_handle;
    gpio_set_level(GPIO_NUM_39, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    sd_ready = esp_board_manager_init_device_by_name("fs_sdcard") == ESP_OK;
    esp_vfs_fat_mount_config_t flash_cfg = { .format_if_mount_failed = false,
        .max_files = 8, .allocation_unit_size = 4096 };
    esp_err_t flash_rc = esp_vfs_fat_spiflash_mount_rw_wl("/flash", "j2me", &flash_cfg, &flash_wl);
    flash_ready = flash_rc == ESP_OK;
    ESP_LOGI(TAG, "Flash FAT storage: %s (%s)", flash_ready ? "/flash" : "unavailable", esp_err_to_name(flash_rc));
    ESP_LOGI(TAG, "LCD=800x480 touch=%s SD=%s", touch ? "GT1151" : "unavailable", sd_ready ? "mounted" : "unavailable");
    dev_audio_codec_handles_t *audio = NULL;
    if (esp_board_manager_get_device_handle("audio_dac", (void **)&audio) == ESP_OK && audio) {
        esp_codec_dev_sample_info_t info = { .bits_per_sample = 16, .channel = 2,
                                            .sample_rate = 48000, .mclk_multiple = 256 };
        if (esp_codec_dev_open(audio->codec_dev, &info) == ESP_CODEC_DEV_OK) {
            codec = audio->codec_dev;
            esp_codec_dev_set_out_vol(codec, korvo_get_volume());
            esp_codec_dev_set_out_mute(codec, false);
            gpio_set_direction(GPIO_NUM_7, GPIO_MODE_OUTPUT);
            gpio_set_level(GPIO_NUM_7, 1);
            audio_queue = xQueueCreate(32, sizeof(audio_packet));
            if (!audio_queue || xTaskCreatePinnedToCore(audio_task, "j2me-audio", 6144, NULL, 5, NULL, 1) != pdPASS)
                return ESP_ERR_NO_MEM;
            ESP_LOGI(TAG, "ES8389 audio ready: 48000 Hz stereo, PA GPIO7");
        }
    }
    return ESP_OK;
}
