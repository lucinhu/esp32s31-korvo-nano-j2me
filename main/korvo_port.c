#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "korvo_port.h"
#include "korvo_parallel.h"
#include "korvo_simd.h"
#include "korvo_ppa.h"
#include "korvo_jit.h"
#include "korvo_media.h"
#include "korvo_midi.h"
#include "korvo_test_games.h"
#include "korvo_board.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "multi_heap.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const unsigned char demo_start[] asm("_binary_korvo_demo_jar_start");
extern const unsigned char demo_end[] asm("_binary_korvo_demo_jar_end");

typedef struct { uint32_t bytes, internal, reserved[2]; } alloc_header;
_Static_assert(sizeof(alloc_header)==16,"VM allocation header alignment");

static nj_u4 millis(void *ud) { (void)ud; return esp_timer_get_time() / 1000; }
static void *alloc_mem(void *ud, nj_size n)
{
#if KORVO_INTERNAL_HOT_DATA
    korvo_port_state *s = ud;
    if (!n) n = NJ_OBJ_ALIGN;
    if (n > SIZE_MAX - sizeof(alloc_header) - 15) return NULL;
    size_t bytes = ((n + 15) & ~(size_t)15) + sizeof(alloc_header);
    alloc_header *p = NULL;
    size_t stack_bytes = (size_t)NJ_MAX_STACK_SLOTS * sizeof(nj_value);
    /* The core may prepend aligned host-allocation bookkeeping. Match a
     * bounded size class without depending on its private header layout. */
    bool stack = n >= stack_bytes && n <= stack_bytes + 4 * NJ_OBJ_ALIGN;
    /* Keep thousands of aligned metadata blocks out of the system heap.
     * Counting payload bytes alone does not bound allocator fragmentation. */
    if (s->hot_heap && n <= 49152)
        p = multi_heap_aligned_alloc(s->hot_heap, bytes, NJ_OBJ_ALIGN);
    bool internal = p != NULL;
    if (!p) p = heap_caps_aligned_alloc(NJ_OBJ_ALIGN,bytes,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        printf("VM_ALLOC_FAILED request=%lu psram_free=%u psram_largest=%u\n",
               (unsigned long)n,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        return NULL;
    }
    p->bytes = bytes; p->internal = internal;
    if (stack) { if (internal) ++s->stack_internal; else ++s->stack_psram; }
    if (internal) { s->hot_used += bytes; if (s->hot_used > s->hot_peak) s->hot_peak = s->hot_used; }
    return p + 1;
#else
    (void)ud;
    return heap_caps_aligned_alloc(NJ_OBJ_ALIGN, n ? n : NJ_OBJ_ALIGN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}
static void free_mem(void *ud, void *p)
{
#if KORVO_INTERNAL_HOT_DATA
    if (!p) return;
    alloc_header *header = (alloc_header *)p - 1;
    if (header->internal) {
        korvo_port_state *s = ud;
        s->hot_used -= header->bytes;
        multi_heap_free(s->hot_heap, header);
    } else heap_caps_free(header);
#else
    (void)ud; heap_caps_free(p);
#endif
}
static void idle(void *ud)
{
    korvo_port_state *s = ud;
    int64_t now = esp_timer_get_time();
    if (now - s->last_yield >= 10000) {
        launcher_pump();
        vTaskDelay(1);
        s->last_yield = now;
    }
}
static void sleep_ms(void *ud, nj_u4 ms)
{
    korvo_port_state *s = ud;
    int64_t start = esp_timer_get_time();
    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    do {
        launcher_pump();
        vTaskDelay(pdMS_TO_TICKS(ms > 10 ? 10 : ms) ?: 1);
    } while (esp_timer_get_time() < end);
    s->sleep_us += esp_timer_get_time() - start;
}
static void log_message(void *ud, int level, const char *msg)
{
    (void)ud;
    esp_log_write(level < 1 ? ESP_LOG_ERROR : level == 1 ? ESP_LOG_WARN : ESP_LOG_INFO,
                  "nano-j2me", "%s\n", msg);
}
static int now_datetime(void *ud, nj_datetime *out)
{
    (void)ud;
    time_t now = time(NULL);
    if (now < 1700000000) return 0;
    struct tm t;
    gmtime_r(&now, &t);
    *out = (nj_datetime){ .year = t.tm_year + 1900, .month = t.tm_mon, .day = t.tm_mday,
        .hour = t.tm_hour, .minute = t.tm_min, .second = t.tm_sec, .weekday = t.tm_wday };
    return 1;
}
void korvo_port_update_view(korvo_port_state *s)
{
    launcher_view(s->width,s->height,&s->view_x,&s->view_y,&s->view_w,&s->view_h);
    for (int i=0;i<s->view_w;++i) s->xmap[i] = i * s->width / s->view_w;
    for (int i=0;i<s->view_h;++i) s->ymap[i] = i * s->height / s->view_h;
    s->previous_valid = false;
    s->probes_valid = false;
}

static int display_init(void *ud, int *w, int *h, int *stride, void **px)
{
    korvo_port_state *s = ud;
    s->pixels = heap_caps_aligned_alloc(64,s->width * s->height * 2,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->pixels) return -1;
    memset(s->pixels, 0, s->width * s->height * 2);
#if KORVO_FAST_RENDER
    s->previous_pixels = heap_caps_malloc(s->width * s->height * 2,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->previous_pixels) { heap_caps_free(s->pixels); s->pixels = NULL; return -1; }
    korvo_port_update_view(s);
#endif
    *w = s->width; *h = s->height; *stride = s->width * 2; *px = s->pixels;
    return NJ_PIXFMT_RGB565;
}
void korvo_present(korvo_port_state *s)
{
    if (!s->pixels) return;
    int x, y, w, h;
    launcher_view(s->width, s->height, &x, &y, &w, &h);
    uint16_t *dst = korvo_lcd();
    for (int row = 0; row < h; ++row) {
        const uint16_t *src = s->pixels + (row * s->height / h) * s->width;
        for (int col = 0; col < w; ++col) dst[(row + y) * LCD_W + x + col] = src[col * s->width / w];
    }
    korvo_flush();
}

#if KORVO_FAST_RENDER
static void present_rect(korvo_port_state *s, int x, int y, int w, int h)
{
    if (!s->pixels) return;
    if (!s->probes_valid || w <= 0 || h <= 0) { x=0; y=0; w=s->width; h=s->height; }
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x+w > s->width ? s->width : x+w;
    int y1 = y+h > s->height ? s->height : y+h;
    if (x0 >= x1 || y0 >= y1) return;
    int first = y1, last = y0;
    bool full=x0==0 && y0==0 && x1==s->width && y1==s->height;
    bool changed=false;
    uint16_t probes[9];
    if (full) {
        for (int row=0;row<3;++row) for (int col=0;col<3;++col) {
            int at=row*3+col;
            probes[at]=s->pixels[(row*(s->height-1)/2)*s->width+col*(s->width-1)/2];
            if (s->probes_valid && probes[at]!=s->probes[at]) changed=true;
        }
    }
    /* A changed sample PROVES that a full frame needs updating, so avoid
     * copying a shadow on continuously changing frames. Samples never prove
     * equality: skipping always requires a complete byte comparison. */
    if (full && changed) {
        s->previous_valid=false;
        first=y0; last=y1;
    } else if (!s->previous_valid) {
        first=y0; last=y1;
        if (full) {
            memcpy(s->previous_pixels,s->pixels,s->width*s->height*2);
            s->previous_valid=true;
        }
    } else {
        for (int row=y0;row<y1;++row) {
            uint16_t *src = s->pixels + row*s->width+x0;
            uint16_t *prev = s->previous_pixels + row*s->width+x0;
            if (memcmp(src,prev,(x1-x0)*2)) {
                if (row < first) first=row;
                last=row+1;
                memcpy(prev,src,(x1-x0)*2);
            }
        }
    }
    if (full) { memcpy(s->probes,probes,sizeof(probes)); s->probes_valid=true; }
    if (first >= last) { ++s->unchanged_frames; return; }
    int dx0=(x0*s->view_w+s->width-1)/s->width;
    int dx1=(x1*s->view_w+s->width-1)/s->width;
    int dy0=(first*s->view_h+s->height-1)/s->height;
    int dy1=(last*s->view_h+s->height-1)/s->height;
    if (s->view_w == s->width && s->view_h == s->height &&
        korvo_display_blit(s->pixels, s->width, s->height, x0, first,
                          x1-x0, last-first, s->view_x+dx0, s->view_y+dy0)) {
        korvo_flush_rect(s->view_x+dx0,s->view_y+dy0,dx1-dx0,dy1-dy0);
        ++s->updated_frames;
        return;
    }
    if (s->view_w == 2*s->width && s->view_h == 2*s->height &&
        korvo_display_scale2(s->pixels, s->width, s->height, x0, first,
                            x1-x0, last-first, s->view_x+dx0, s->view_y+dy0)) {
        korvo_flush_rect(s->view_x+dx0,s->view_y+dy0,dx1-dx0,dy1-dy0);
        ++s->updated_frames;
        return;
    }
    int previous_row=-1;
    /* Stage each scaled source row in internal task RAM. Bulk copies avoid
     * slow individual halfword writes to the PSRAM LCD framebuffer. */
    uint16_t line[400];
    for (int row=dy0;row<dy1;++row) {
        uint16_t *dst=korvo_lcd()+(s->view_y+row)*LCD_W+s->view_x+dx0;
        int sy=s->ymap[row];
        if (sy!=previous_row) {
            const uint16_t *src=s->pixels+sy*s->width;
            for (int col=dx0;col<dx1;++col) line[col-dx0]=src[s->xmap[col]];
        }
        memcpy(dst,line,(dx1-dx0)*2);
        previous_row=sy;
    }
    korvo_flush_rect(s->view_x+dx0,s->view_y+dy0,dx1-dx0,dy1-dy0);
    ++s->updated_frames;
}
#endif
static void display_present(void *ud, int x, int y, int w, int h)
{
    korvo_port_state *s = ud;
    int64_t start = esp_timer_get_time();
    unsigned before = s->updated_frames;
    korvo_display_begin();
#if KORVO_FAST_RENDER
    present_rect(s,x,y,w,h);
#else
    (void)x; (void)y; (void)w; (void)h;
    korvo_present(s); ++s->updated_frames;
#endif
    launcher_fps_present(s->updated_frames != before);
    korvo_display_end();
    uint32_t elapsed = esp_timer_get_time() - start;
    s->present_us += elapsed;
    if (elapsed > s->present_max_us) s->present_max_us = elapsed;
    ++s->presents;
}
static int input_poll(void *ud, nj_event *ev)
{
    korvo_port_state *s = ud;
    int64_t start = esp_timer_get_time();
    launcher_pump();
    int rc = launcher_event(ev);
    if (rc) ++s->input_events;
    s->poll_us += esp_timer_get_time() - start;
    ++s->poll_calls;
    return rc;
}
static int input_push(void *ud, const nj_event *ev) { (void)ud; return launcher_push(ev); }

void korvo_port_report(korvo_port_state *s, int64_t elapsed_us)
{
    uint32_t hash = s->pixels ? esp_rom_crc32_le(0, (uint8_t *)s->pixels, s->width * s->height * 2) : 0;
    int x,y,w,h;
    launcher_view(s->width,s->height,&x,&y,&w,&h);
    uint32_t lcd_hash = 0, mismatches = 0;
    int ox=0, oy=0, ow=0, oh=0;
    bool overlay = launcher_fps_bounds(&ox,&oy,&ow,&oh);
    for (int row=0;row<h;++row) {
        uint16_t line[400];
        memcpy(line,korvo_lcd()+(y+row)*LCD_W+x,w*2);
        if (s->pixels) for (int col=0;col<w;++col) {
            uint16_t expected = s->pixels[(row*s->height/h)*s->width+col*s->width/w];
            /* CRC and scaler checks describe the game, excluding the HUD. */
            if (overlay && x+col>=ox && x+col<ox+ow && y+row>=oy && y+row<oy+oh) line[col]=expected;
            else mismatches += line[col] != expected;
        }
        lcd_hash = esp_rom_crc32_le(lcd_hash,(uint8_t *)line,w*2);
    }
    printf("PERF elapsed_us=%llu presents=%u present_us=%llu present_max_us=%u poll_us=%llu poll_calls=%u sleep_us=%llu native_crc=%08lx lcd_crc=%08lx updated=%u unchanged=%u hot_peak=%u hot_live=%u scale_mismatch=%u stack_internal=%u stack_psram=%u\n",
           (unsigned long long)elapsed_us, s->presents, (unsigned long long)s->present_us, (unsigned)s->present_max_us,
           (unsigned long long)s->poll_us, (unsigned)s->poll_calls, (unsigned long long)s->sleep_us,
           (unsigned long)hash, (unsigned long)lcd_hash,s->updated_frames,s->unchanged_frames,(unsigned)s->hot_peak,(unsigned)s->hot_used,(unsigned)mismatches,s->stack_internal,s->stack_psram);
    korvo_parallel_stats();
    korvo_simd_stats();
    korvo_ppa_stats();
    printf("JIT_MEMORY allocations=%u commits=%u refused=%u peak=%u live=%u\n",
           s->jit_allocations,s->jit_commits,s->jit_refused,(unsigned)s->jit_code_peak,(unsigned)s->jit_code_live);
}

static int file_open(void *ud, const char *name, const char *mode, nj_file *out)
{
    (void)ud;
    const korvo_test_game *game = korvo_test_game_find(name);
    if (game) {
        if (strcmp(mode,"rb")) return NJ_ERR;
        *out = (nj_file){ .handle=(void *)game->start, .size=game->end-game->start };
        return NJ_OK;
    }
    /* Use the file route for the embedded demo. At this upstream revision
     * nj_run's borrowed jar_data path still calls free() at teardown. */
    if (!strcmp(name, "@builtin") && !strcmp(mode, "rb")) {
        *out = (nj_file){ .handle = (void *)demo_start, .size = demo_end - demo_start };
        return NJ_OK;
    }
    FILE *fp = fopen(name, mode);
    if (!fp) return NJ_ERR;
    struct stat st;
    *out = (nj_file){ .handle = fp, .size = stat(name, &st) == 0 ? st.st_size : 0 };
    return NJ_OK;
}
static nj_s4 file_read(void *ud, nj_file *f, void *buf, nj_u4 n)
{
    (void)ud;
    if (f->handle == demo_start || korvo_test_game_handle(f->handle)) {
        if (n > f->size - f->pos) n = f->size - f->pos;
        memcpy(buf, (const uint8_t *)f->handle + f->pos, n); f->pos += n;
        return n;
    }
    size_t got = fread(buf, 1, n, f->handle); f->pos += got;
    return ferror(f->handle) ? -1 : (nj_s4)got;
}
static nj_s4 file_write(void *ud, nj_file *f, const void *buf, nj_u4 n)
{
    (void)ud;
    if (f->handle == demo_start || korvo_test_game_handle(f->handle)) return -1;
    size_t put = fwrite(buf, 1, n, f->handle); f->pos += put;
    if (f->pos > f->size) f->size = f->pos;
    return ferror(f->handle) ? -1 : (nj_s4)put;
}
static int file_seek(void *ud, nj_file *f, nj_u4 pos)
{
    (void)ud;
    if (f->handle == demo_start || korvo_test_game_handle(f->handle)) { if (pos > f->size) return NJ_ERR; f->pos = pos; return NJ_OK; }
    if (fseek(f->handle, pos, SEEK_SET)) return NJ_ERR;
    f->pos = pos; return NJ_OK;
}
static void file_close(void *ud, nj_file *f) { (void)ud; if (f->handle != demo_start && !korvo_test_game_handle(f->handle)) fclose(f->handle); f->handle = NULL; }
static int file_remove(void *ud, const char *name) { (void)ud; return remove(name) == 0 ? NJ_OK : NJ_ERR; }
static int file_exists(void *ud, const char *name) { (void)ud; struct stat st; return stat(name, &st) == 0; }
static int dir_open(void *ud, const char *path, void **out)
{ (void)ud; *out = opendir(path); return *out ? NJ_OK : NJ_ERR; }
static int dir_next(void *ud, void *handle, char *name, nj_size cap, int *is_dir)
{
    (void)ud;
    struct dirent *e;
    while ((e = readdir(handle))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(name, cap, "%s", e->d_name); *is_dir = e->d_type == DT_DIR;
        return 1;
    }
    return 0;
}
static void dir_close(void *ud, void *handle) { (void)ud; closedir(handle); }
static int pcm_open(void *ud, int rate, int channels) { (void)ud; return korvo_pcm_open(rate, channels); }
static int pcm_write(void *ud, const nj_s2 *samples, int frames) { (void)ud; return korvo_pcm_write(samples, frames); }
static int pcm_free(void *ud) { (void)ud; return korvo_pcm_free(); }
static void pcm_close(void *ud) { (void)ud; korvo_pcm_close(); }
static void audio_stop(void *ud) { (void)ud; korvo_audio_stop(); }
static int tone_play(void *ud, const nj_audio_tone *tones, int count) { (void)ud; return korvo_tones(tones, count); }
static const char *property(void *ud, const char *key)
{
    (void)ud;
    if (!strcmp(key, "microedition.platform")) return "Nokia6230/05.50";
    if (!strcmp(key, "microedition.locale")) return "zh-CN";
    if (!strcmp(key, "com.nokia.keyboard.type")) return "PhoneKeypad";
    return NULL;
}
static void shutdown_port(void *ud)
{
    korvo_port_state *s = ud;
    korvo_jit_shutdown(s);
    korvo_midi_shutdown(s);
    korvo_audio_stop();
    heap_caps_free(s->pixels); s->pixels = NULL;
    heap_caps_free(s->previous_pixels); s->previous_pixels = NULL;
    if (s->hot_pool) {
        printf("HOT_POOL live=%u intact=%d\n", (unsigned)s->hot_used,
               multi_heap_check(s->hot_heap, true));
        heap_caps_free(s->hot_pool);
        s->hot_pool = s->hot_heap = NULL;
    }
}
void korvo_port_init(nj_port *p, korvo_port_state *s, int width, int height)
{
    memset(s, 0, sizeof(*s)); s->width = width; s->height = height;
#if KORVO_INTERNAL_HOT_DATA
    /* A bounded private heap leaves a contiguous system reserve for file
     * locks, queues and audio tasks, even after a sprite-heavy loading phase. */
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t pool_bytes = free_internal >= 256 * 1024 ? 32 * 1024 : 0;
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (pool_bytes + 4096 > largest) pool_bytes = 0;
    if (pool_bytes) {
        s->hot_pool = heap_caps_malloc(pool_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s->hot_pool) s->hot_heap = multi_heap_register(s->hot_pool, pool_bytes);
        if (!s->hot_heap) { heap_caps_free(s->hot_pool); s->hot_pool = NULL; }
    }
#endif
    *p = (nj_port){ .name = "ESP32-S31-Korvo-1", .ud = s,
        .millis = millis, .alloc = alloc_mem, .free = free_mem, .idle = idle,
        .sleep_ms = sleep_ms, .log = log_message, .now = now_datetime,
        .display_init = display_init, .display_present = display_present,
        .input_poll = input_poll, .input_push = input_push,
        .file_open = file_open, .file_read = file_read, .file_write = file_write,
        .file_seek = file_seek, .file_close = file_close, .file_remove = file_remove,
        .file_exists = file_exists, .dir_open = dir_open, .dir_next = dir_next, .dir_close = dir_close,
        .tone_play = tone_play, .tone_stop = audio_stop,
        .pcm_open = pcm_open, .pcm_write = pcm_write, .pcm_close = pcm_close, .pcm_free = pcm_free,
        .device_property = property, .shutdown = shutdown_port,
        .width = width, .height = height };
    korvo_parallel_reset();
    korvo_simd_reset();
    korvo_ppa_reset();
    if (korvo_parallel_enabled()) p->parallel_for = korvo_parallel_run;
    p->gfx_fill_rect = korvo_simd_fill_rect;
    korvo_jit_port(p);
    korvo_media_port(p);
    korvo_midi_port(p, s);
}
