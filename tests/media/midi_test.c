#include "korvo_midi.h"
#include "korvo_midi_file.h"
#include "korvo_soundfont.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern _Atomic int sdk_allocations;
extern _Atomic size_t sdk_allocation_limit;
extern _Atomic int sdk_refused_allocations;
extern esp_midi_err_t __wrap_esp_midi_synth_handle_note_off(void *synth, esp_midi_parse_event_t *event);
static unsigned sdk_note_off_calls;
esp_midi_err_t __real_esp_midi_synth_handle_note_off(void *synth, esp_midi_parse_event_t *event)
{
    assert(synth && event);
    ++sdk_note_off_calls;
    return ESP_MIDI_ERR_OK;
}
struct test_semaphore { sem_t value; };
SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    SemaphoreHandle_t sem = malloc(sizeof(*sem));
    assert(sem && !sem_init(&sem->value, 0, 0));
    return sem;
}
int xSemaphoreTake(SemaphoreHandle_t sem, unsigned ticks)
{
    (void)ticks;
    int result;
    do { result = sem_wait(&sem->value); } while (result && errno == EINTR);
    assert(!result);
    return pdPASS;
}
int xSemaphoreGive(SemaphoreHandle_t sem) { assert(!sem_post(&sem->value)); return pdPASS; }
void vSemaphoreDelete(SemaphoreHandle_t sem) { assert(!sem_destroy(&sem->value)); free(sem); }
void vTaskDelay(unsigned ticks)
{
    struct timespec time = { .tv_sec = ticks / 1000, .tv_nsec = (long)(ticks % 1000) * 1000000 };
    nanosleep(&time, NULL);
}
void vTaskDelete(TaskHandle_t task) { assert(!task); pthread_exit(NULL); }
typedef struct { void (*entry)(void *); void *arg; } task_context;
static bool task_create_fails;
static void *task_main(void *arg)
{
    task_context ctx = *(task_context *)arg;
    free(arg);
    ctx.entry(ctx.arg);
    return NULL;
}
int xTaskCreatePinnedToCore(void (*entry)(void *), const char *name, unsigned stack,
    void *arg, unsigned priority, TaskHandle_t *handle, int core)
{
    (void)name; (void)stack; (void)priority; (void)handle;
    assert(core == 1);
    if (task_create_fails) return 0;
    task_context *ctx = malloc(sizeof(*ctx));
    assert(ctx);
    *ctx = (task_context){ .entry = entry, .arg = arg };
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, task_main, ctx));
    assert(!pthread_detach(thread));
    return pdPASS;
}
int64_t esp_timer_get_time(void)
{
    struct timespec t;
    assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

typedef struct {
    esp_midi_sound_cfg_t cfg;
    esp_midi_lib_loader_handle_t library;
    esp_midi_lib_sample_desc_t sample;
    atomic_bool paused;
    unsigned block;
} fake_synth;
static int synth_handles;
static unsigned synth_blocks = 16;
static bool synth_open_fails, synth_parse_fails;
static atomic_uint synth_output_blocks;
static atomic_uint synth_output_block_limit = UINT32_MAX;
esp_midi_err_t esp_midi_sound_open(esp_midi_sound_cfg_t *cfg, esp_midi_sound_handle_t *handle)
{
    if (synth_open_fails) { *handle = NULL; return ESP_MIDI_ERR_MEM_LACK; }
    fake_synth *synth = calloc(1, sizeof(*synth));
    assert(synth && cfg->out_stream_info.bits == 16 && cfg->out_stream_info.channel == 1);
    assert(cfg->note_off_resp == ESP_MIDI_SYNTH_NOTE_OFF_RESP_RELEASE);
    synth->cfg = *cfg;
    *handle = synth;
    ++synth_handles;
    return ESP_MIDI_ERR_OK;
}
esp_midi_err_t esp_midi_sound_load_sf_lib(esp_midi_sound_handle_t handle)
{
    fake_synth *synth = handle;
    esp_midi_lib_cfg_t *cfg = &synth->cfg.sf_lib_cfg;
    esp_midi_err_t result = cfg->loader_cb(cfg->loader_cfg, &synth->library);
    if (result != ESP_MIDI_ERR_OK) return result;
    synth->sample = (esp_midi_lib_sample_desc_t){ .note = 60, .velocity = 100, .program_change = 40 };
    assert(cfg->set_bank_cb(synth->library, 0) == ESP_MIDI_ERR_OK);
    assert(cfg->set_preset_cb(synth->library, 40) == ESP_MIDI_ERR_OK);
    assert(cfg->note_on_cb(synth->library, &synth->sample) == ESP_MIDI_ERR_OK);
    return cfg->set_data_cb(synth->library, &synth->sample);
}
esp_midi_err_t esp_midi_sound_parse(esp_midi_sound_handle_t handle, esp_midi_packet_t *packet, esp_midi_sound_info_t *info)
{
    fake_synth *synth = handle;
    if (synth_parse_fails) return ESP_MIDI_ERR_HEADER_PARSE;
    if (packet->data.len < 14 || memcmp(packet->data.buffer, "MThd", 4)) return ESP_MIDI_ERR_HEADER_PARSE;
    /* The real SDK processes only track zero. Require the adapter to merge
     * format 1 instead of letting this mock conceal the compatibility bug. */
    assert(packet->data.buffer[8] == 0 && packet->data.buffer[9] == 0);
    assert(packet->data.buffer[10] == 0 && packet->data.buffer[11] == 1);
    synth->block = 0;
    packet->consumed = packet->data.len;
    info->track_num = 1;
    return ESP_MIDI_ERR_OK;
}
esp_midi_err_t esp_midi_sound_process(esp_midi_sound_handle_t handle)
{
    fake_synth *synth = handle;
    while (synth->block < synth_blocks && !atomic_load(&synth->paused)) {
        int16_t samples[256];
        bool compatible = synth->sample.samplerate == (int)synth->cfg.out_stream_info.samplerate &&
            synth->sample.bits == synth->cfg.out_stream_info.bits;
        const int16_t *pcm = (const int16_t *)synth->sample.data.buffer;
        for (int i = 0; i < 256; ++i) {
            unsigned frame = synth->block * 256u + i;
            samples[i] = compatible && frame < synth->sample.data.len / 2u ?
                (int16_t)(pcm[frame] * synth->sample.gain) : 0;
        }
        atomic_fetch_add(&synth_output_blocks, 1);
        while (synth_output_blocks >= synth_output_block_limit && !atomic_load(&synth->paused))
            vTaskDelay(1);
        if (atomic_load(&synth->paused)) return ESP_MIDI_ERR_OK;
        esp_midi_err_t result = synth->cfg.out_stream_cb((uint8_t *)samples, sizeof(samples), synth->cfg.out_stream_ctx);
        if (result != ESP_MIDI_ERR_OK) return result;
        ++synth->block;
    }
    return ESP_MIDI_ERR_OK;
}
esp_midi_err_t esp_midi_sound_pause(esp_midi_sound_handle_t handle)
{
    assert(handle);
    atomic_store(&((fake_synth *)handle)->paused, true);
    return ESP_MIDI_ERR_OK;
}
esp_midi_err_t esp_midi_sound_resume(esp_midi_sound_handle_t handle)
{
    assert(handle);
    atomic_store(&((fake_synth *)handle)->paused, false);
    return ESP_MIDI_ERR_OK;
}
esp_midi_err_t esp_midi_sound_close(esp_midi_sound_handle_t handle)
{
    fake_synth *synth = handle;
    if (!synth) return ESP_MIDI_ERR_OK;
    if (synth->library) {
        synth->cfg.sf_lib_cfg.free_data_cb(synth->library, &synth->sample);
        synth->cfg.sf_lib_cfg.delete_cb(synth->library);
    }
    --synth_handles;
    free(synth);
    return ESP_MIDI_ERR_OK;
}

/* SF2 parsing itself is an ESP binary. Exercise its adapter using a small
 * temporary sample area and the real bounded read/seek callbacks. */
typedef struct { esp_midi_sf_parse_cfg_t cfg; } fake_sf;
static int sf_rate = 22050;
static uint32_t sf_loopstart = 4, sf_loopend = 12, sf_sample_bytes = 32;
static bool sf_note_available = true;
esp_midi_err_t esp_midi_sf_parse_open(esp_midi_sf_parse_cfg_t *cfg, esp_midi_sf_parse_handle_t *handle)
{
    fake_sf *sf = malloc(sizeof(*sf));
    assert(sf);
    sf->cfg = *cfg;
    *handle = sf;
    return ESP_MIDI_ERR_OK;
}
esp_midi_err_t esp_midi_sf_parse_process(esp_midi_sf_parse_handle_t handle)
{
    fake_sf *sf = handle;
    uint8_t header[12];
    assert(sf->cfg.read(header, sizeof(header), sf->cfg.ctx) == ESP_MIDI_ERR_OK);
    assert(!memcmp(header, "RIFF", 4));
    assert(sf->cfg.seek(4, sf->cfg.ctx) == ESP_MIDI_ERR_OK);
    assert(sf->cfg.seek(UINT32_MAX, sf->cfg.ctx) != ESP_MIDI_ERR_OK);
    return ESP_MIDI_ERR_OK;
}
esp_midi_err_t esp_midi_sf_parse_note_on(esp_midi_sf_parse_handle_t handle, uint32_t key, uint32_t velocity, esp_midi_sf_sample_t **out)
{
    (void)handle; (void)key; (void)velocity;
    if (!sf_note_available) { *out = NULL; return ESP_MIDI_ERR_FAIL; }
    static esp_midi_sf_sample_t sample = { .loopstart = 4, .loopend = 12, .origpitch = 60,
        .samplerate = 22050, .sampletype = 1 };
    sample.samplerate = sf_rate;
    sample.loopstart = sf_loopstart;
    sample.loopend = sf_loopend;
    *out = &sample;
    return ESP_MIDI_ERR_OK;
}
esp_midi_err_t esp_midi_sf_parse_get_sdat(esp_midi_sf_parse_handle_t handle, esp_midi_sf_sdat_t *data)
{
    (void)handle;
    *data = (esp_midi_sf_sdat_t){ .smpl_pos = 12, .smpl_size = sf_sample_bytes };
    return ESP_MIDI_ERR_OK;
}
esp_midi_err_t esp_midi_sf_parse_set_bank(esp_midi_sf_parse_handle_t handle, int32_t bank)
{ (void)handle; (void)bank; return ESP_MIDI_ERR_OK; }
esp_midi_err_t esp_midi_sf_parse_set_program(esp_midi_sf_parse_handle_t handle, int32_t program)
{ (void)handle; (void)program; return ESP_MIDI_ERR_OK; }
esp_midi_err_t esp_midi_sf_parse_close(esp_midi_sf_parse_handle_t handle) { free(handle); return ESP_MIDI_ERR_OK; }

static atomic_uint pcm_frames;
static atomic_int pcm_first;
static atomic_int pcm_peak;
static atomic_uint pcm_stop_calls;
static atomic_uint pcm_frame_limit = UINT32_MAX;
static nj_s2 pcm_capture[16384];
static bool pcm_open_fails;
static int pcm_open(void *ud, int rate, int channels)
{
    (void)ud;
    assert(rate == 22050 && channels == 1);
    if (pcm_open_fails) return -1;
    atomic_store(&pcm_frames, 0);
    atomic_store(&pcm_peak, 0);
    atomic_store(&synth_output_blocks, 0);
    return 0;
}
static int pcm_write(void *ud, const nj_s2 *samples, int frames)
{
    (void)ud;
    atomic_store(&pcm_first, samples[0]);
    /* Partial writes force the adapter to retain and retry the remainder. */
    int accepted = frames > 100 ? 100 : frames;
    unsigned offset = atomic_load(&pcm_frames);
    unsigned limit = atomic_load(&pcm_frame_limit);
    if (offset >= limit) return 0;
    if ((unsigned)accepted > limit - offset) accepted = (int)(limit - offset);
    for (int i = 0; i < accepted; ++i) {
        if (offset + i < sizeof(pcm_capture) / sizeof(pcm_capture[0]))
            pcm_capture[offset + i] = samples[i];
        int magnitude = abs(samples[i]);
        if (magnitude > pcm_peak) atomic_store(&pcm_peak, magnitude);
    }
    atomic_fetch_add(&pcm_frames, accepted);
    vTaskDelay(1);
    return accepted;
}
static int pcm_free(void *ud)
{
    (void)ud;
    unsigned frames = atomic_load(&pcm_frames), limit = atomic_load(&pcm_frame_limit);
    return frames >= limit ? 0 : limit - frames < 256 ? (int)(limit - frames) : 256;
}
static void pcm_close(void *ud) { (void)ud; }
static void pcm_stop(void *ud) { (void)ud; ++pcm_stop_calls; }

static void test_soundfont(void)
{
    char path[] = "/tmp/korvo-sf2-test-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    uint8_t bytes[64];
    for (int i = 0; i < 64; ++i) bytes[i] = i;
    memcpy(bytes, "RIFF", 4);
    assert(write(fd, bytes, sizeof(bytes)) == sizeof(bytes));
    assert(!close(fd));
    esp_midi_lib_cfg_t cfg = korvo_soundfont_config();
    cfg.loader_cfg = path;
    esp_midi_lib_loader_handle_t library;
    assert(cfg.loader_cb(cfg.loader_cfg, &library) == ESP_MIDI_ERR_OK);
    esp_midi_lib_sample_desc_t sample = { .note = 60, .velocity = 100 };
    assert(cfg.note_on_cb(library, &sample) == ESP_MIDI_ERR_OK);
    assert(sample.sampletype == ESP_MIDI_SF_SAMPLE_TYPE_MONO);
    assert(cfg.set_data_cb(library, &sample) == ESP_MIDI_ERR_OK);
    assert(sample.data.len == 16 && sample.data.buffer[0] == 20);
    assert(cfg.free_data_cb(library, &sample) == ESP_MIDI_ERR_OK);
    sample.loopend = UINT32_MAX;
    assert(cfg.set_data_cb(library, &sample) != ESP_MIDI_ERR_OK);
    sf_rate = 44100;
    sample.note = 60;
    assert(cfg.note_on_cb(library, &sample) == ESP_MIDI_ERR_OK);
    assert(sample.samplerate == KORVO_MIDI_RATE && sample.data.len == 8);
    assert(sample.data.buffer[0] == 20 && sample.data.buffer[2] == 24);
    cfg.free_data_cb(library, &sample);
    sample.note = 72;
    assert(cfg.note_on_cb(library, &sample) == ESP_MIDI_ERR_OK);
    assert(sample.samplerate == KORVO_MIDI_RATE && sample.data.len == 4);
    assert(sample.data.buffer[0] == 20 && sample.data.buffer[2] == 28);
    cfg.free_data_cb(library, &sample);
    sf_rate = 22050;
    /* A fallback voice can leave one reusable buffer. Loading an SF2 sample
     * must discard it first instead of keeping optional PCM beside the file. */
    int base = sdk_allocations;
    sf_note_available = false;
    assert(cfg.note_on_cb(library, &sample) == ESP_MIDI_ERR_OK);
    assert(cfg.free_data_cb(library, &sample) == ESP_MIDI_ERR_OK);
    assert(sdk_allocations == base + 2); /* idle PCM and its metadata */
    sf_note_available = true;
    assert(cfg.note_on_cb(library, &sample) == ESP_MIDI_ERR_OK);
    assert(sdk_allocations == base + 3); /* SF2 PCM, metadata and one voice */
    assert(cfg.free_data_cb(library, &sample) == ESP_MIDI_ERR_OK);
    assert(sdk_allocations == base);
    cfg.delete_cb(library);
    assert(!unlink(path));
    assert(sdk_allocations == 0);
}

static void test_soundfont_windowed(void)
{
    /* Span multiple stdio buffers as well as the 512-frame sample window,
     * so the later truncation cannot be concealed by the last read buffer. */
    const size_t source_frames = 4u * BUFSIZ + 1u;
    int16_t *source = malloc(source_frames * sizeof(*source));
    assert(source);
    const int rates[] = { 11025, 22050, 44100 };
    const int notes[] = { 60, 67 };
    char path[] = "/tmp/korvo-sf2-window-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    sf_loopend = sf_loopstart + source_frames;
    sf_sample_bytes = sf_loopend * 2u;
    size_t file_bytes = 12u + sf_sample_bytes;
    uint8_t *file = calloc(1, file_bytes);
    assert(file);
    memcpy(file, "RIFF", 4);
    for (size_t i = 0; i < source_frames; ++i) {
        source[i] = (int16_t)((int32_t)((i * 997u) & 65535u) - 32768);
        size_t at = 12u + (sf_loopstart + i) * 2u;
        file[at] = (uint16_t)source[i] & 255u;
        file[at + 1] = (uint16_t)source[i] >> 8;
    }
    assert(write(fd, file, file_bytes) == (ssize_t)file_bytes);
    assert(!close(fd));
    free(file);
    esp_midi_lib_cfg_t cfg = korvo_soundfont_config();
    esp_midi_lib_loader_handle_t library;
    cfg.loader_cfg = path;
    assert(cfg.loader_cb(cfg.loader_cfg, &library) == ESP_MIDI_ERR_OK);
    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r)
    for (size_t n = 0; n < sizeof(notes) / sizeof(notes[0]); ++n) {
        sf_rate = rates[r];
        double step = (double)sf_rate / KORVO_MIDI_RATE * pow(2.0, (notes[n] - 60) / 12.0);
        size_t frames = (size_t)ceil(source_frames / step);
        esp_midi_lib_sample_desc_t sample = { .note = notes[n], .velocity = 100 };
        /* Downsampling must work when only the smaller output block fits.
         * The previous whole-source allocation fails this limit. */
        sdk_allocation_limit = frames * sizeof(int16_t);
        sdk_refused_allocations = 0;
        assert(cfg.note_on_cb(library, &sample) == ESP_MIDI_ERR_OK);
        assert(sdk_refused_allocations == 0);
        sdk_allocation_limit = 0;
        assert(sample.data.len == frames * sizeof(int16_t));
        const int16_t *actual = (const int16_t *)sample.data.buffer;
        for (size_t i = 0; i < frames; ++i) {
            double phase = i * step;
            size_t index = (size_t)phase;
            if (index >= source_frames) index = source_frames - 1;
            size_t next = index + 1 < source_frames ? index + 1 : index;
            int16_t expected = (int16_t)(source[index] + (source[next] - source[index]) * (phase - index));
            assert(actual[i] == expected);
        }
        assert(cfg.free_data_cb(library, &sample) == ESP_MIDI_ERR_OK);
    }
    /* A file truncated after loading must release the output on read failure. */
    assert(!truncate(path, 32));
    sf_rate = 44100;
    esp_midi_lib_sample_desc_t sample = { .note = 60, .velocity = 100 };
    int before = sdk_allocations;
    assert(cfg.note_on_cb(library, &sample) == ESP_MIDI_ERR_DATA_LACK);
    assert(sdk_allocations == before);
    assert(cfg.delete_cb(library) == ESP_MIDI_ERR_OK);
    assert(!unlink(path));
    sf_rate = 22050;
    sf_loopend = 12;
    sf_sample_bytes = 32;
    free(source);
    assert(sdk_allocations == 0);
}

static unsigned append_track(uint8_t *file, unsigned offset, const uint8_t *events, unsigned length)
{
    memcpy(file + offset, "MTrk", 4);
    for (unsigned i = 0; i < 4; ++i) file[offset + 4 + i] = length >> (24 - i * 8);
    memcpy(file + offset + 8, events, length);
    return offset + 8 + length;
}

static void test_midi_merge(void)
{
    const uint8_t conductor[] = {0,255,81,3,7,161,32, 96,255,81,3,6,26,128, 48,255,47,0};
    const uint8_t melody[] = {0,192,5, 0,144,60,100, 48,62,90,
        48,128,60,0, 0,62,0, 0,255,47,0};
    const uint8_t harmony[] = {24,177,7,64, 0,193,8, 0,145,67,64,
        72,129,67,0, 48,255,47,0};
    const uint8_t expected[] = {0,255,81,3,7,161,32, 0,192,5, 0,144,60,100,
        24,177,7,64, 0,193,8, 0,145,67,64, 24,144,62,90,
        48,255,81,3,6,26,128, 0,128,60,0, 0,128,62,0, 0,129,67,0, 48,255,47,0};
    uint8_t file[512] = "MThd\0\0\0\6\0\1\0\3\0\140";
    unsigned size = append_track(file,14,conductor,sizeof(conductor));
    size = append_track(file,size,melody,sizeof(melody));
    size = append_track(file,size,harmony,sizeof(harmony));
    uint8_t *output = NULL;
    uint32_t length = 0;
    assert(korvo_midi_file_prepare(file,size,&output,&length));
    assert(length == 22 + sizeof(expected));
    assert(!memcmp(output,"MThd\0\0\0\6\0\0\0\1\0\140MTrk",18));
    assert(!memcmp(output + 22,expected,sizeof(expected)));
    uint8_t *again = NULL;
    uint32_t again_length = 0;
    assert(korvo_midi_file_prepare(output,length,&again,&again_length));
    assert(again_length == length && !memcmp(again,output,length));
    heap_caps_free(again);
    heap_caps_free(output);
    for (unsigned i = 0; i < size; ++i) {
        assert(!korvo_midi_file_prepare(file,i,&output,&length));
        assert(!output && !length);
    }
    /* Same-tick EOT in an empty conductor must not discard later notes. */
    const uint8_t end[] = {0,255,47,0};
    const uint8_t sysex[] = {0,240,3,126,127,9, 0,247,1,247,
        0,255,127,2,1,2, 0,209,32, 0,193,16, 0,255,47,0};
    file[11] = 2;
    size = append_track(file,14,end,sizeof(end));
    size = append_track(file,size,sysex,sizeof(sysex));
    assert(korvo_midi_file_prepare(file,size,&output,&length));
    assert(length == sizeof(sysex) + 22 && !memcmp(output+22,sysex,sizeof(sysex)));
    heap_caps_free(output);
    file[size - 4] = 128; file[size - 3] = 128; file[size - 2] = 128; file[size - 1] = 128;
    assert(!korvo_midi_file_prepare(file,size,&output,&length));
    file[11] = 32;
    size = 14;
    for (unsigned i=0;i<32;++i) size = append_track(file,size,end,sizeof(end));
    assert(korvo_midi_file_prepare(file,size,&output,&length));
    assert(length == 26 && !memcmp(output+22,end,sizeof(end)));
    heap_caps_free(output);
    file[11] = 33;
    assert(!korvo_midi_file_prepare(file,size,&output,&length));
    file[11] = 32; file[9] = 2;
    assert(!korvo_midi_file_prepare(file,size,&output,&length));
    file[9] = 1; file[12] = 128;
    assert(!korvo_midi_file_prepare(file,size,&output,&length));
    assert(sdk_allocations == 0);
    puts("SMF merge: tempo-only conductor, independent running status, channels, timing, EOT, SysEx/meta, truncation, 32 tracks, format-0 identity");
}

static void test_builtin_signal(void)
{
    esp_midi_lib_cfg_t cfg = korvo_soundfont_config();
    esp_midi_lib_loader_handle_t library;
    assert(cfg.loader_cb(NULL, &library) == ESP_MIDI_ERR_OK);
    unsigned crossings[2] = {0};
    for (unsigned n = 0; n < 2; ++n) {
        esp_midi_lib_sample_desc_t sample = { .note = 69 + n * 12, .velocity = 100 };
        assert(cfg.note_on_cb(library, &sample) == ESP_MIDI_ERR_OK);
        assert(sample.samplerate == KORVO_MIDI_RATE && sample.bits == 16);
        assert(sample.data.len == KORVO_MIDI_RATE * 4u * 2u);
        const int16_t *pcm = (const int16_t *)sample.data.buffer;
        int peak = 0;
        for (unsigned i = 1; i < KORVO_MIDI_RATE / 10u; ++i) {
            if ((pcm[i] >= 0) != (pcm[i - 1] >= 0)) crossings[n]++;
            if (abs(pcm[i]) > peak) peak = abs(pcm[i]);
        }
        assert(peak > 10000 && crossings[n] > 80);
        cfg.free_data_cb(library, &sample);
    }
    assert(crossings[1] >= crossings[0] * 2u - 2u && crossings[1] <= crossings[0] * 2u + 2u);
    /* SDK close may omit free_data for voices interrupted during playback. */
    esp_midi_lib_sample_desc_t interrupted = { .note = 60, .velocity = 100 };
    assert(cfg.note_on_cb(library, &interrupted) == ESP_MIDI_ERR_OK);
    cfg.delete_cb(library);
    assert(sdk_allocations == 0);
}

static void test_unmatched_note_off(void)
{
    esp_midi_lib_cfg_t cfg = korvo_soundfont_config();
    esp_midi_lib_loader_handle_t library;
    assert(cfg.loader_cb(NULL, &library) == ESP_MIDI_ERR_OK);
    esp_midi_parse_event_t event = { .type = ESP_MIDI_EVENT_TYPE_NOTE_OFF,
        .msg.note_msg = { .channel = 4, .note = 60 } };
    unsigned before = sdk_note_off_calls;
    assert(__wrap_esp_midi_synth_handle_note_off(library, &event) == ESP_MIDI_ERR_OK);
    assert(sdk_note_off_calls == before);
    esp_midi_lib_sample_desc_t sample = { .ch_index = 4, .note = 60, .velocity = 100 };
    assert(cfg.note_on_cb(library, &sample) == ESP_MIDI_ERR_OK);
    event.msg.note_msg.channel = 3;
    __wrap_esp_midi_synth_handle_note_off(library, &event);
    assert(sdk_note_off_calls == before);
    event.msg.note_msg.channel = 4;
    __wrap_esp_midi_synth_handle_note_off(library, &event);
    assert(sdk_note_off_calls == before + 1);
    /* The SDK also frees a sample when its PCM runs out. Its later note-off
     * must never enter the library's unbounded circular-pool lookup. */
    assert(cfg.free_data_cb(library, &sample) == ESP_MIDI_ERR_OK);
    __wrap_esp_midi_synth_handle_note_off(library, &event);
    __wrap_esp_midi_synth_handle_note_off(library, &event);
    assert(sdk_note_off_calls == before + 1);
    cfg.delete_cb(library);
    assert(sdk_allocations == 0);
    puts("MIDI unmatched/expired note-offs ignored; live notes retain SDK release: PASS");
}

static uint32_t sample_hash(const esp_midi_lib_sample_desc_t *sample)
{
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < sample->data.len; ++i)
        hash = (hash ^ sample->data.buffer[i]) * 16777619u;
    return hash;
}

static void test_builtin_shared_samples(void)
{
    esp_midi_lib_cfg_t cfg = korvo_soundfont_config();
    esp_midi_lib_loader_handle_t library;
    assert(cfg.loader_cb(NULL, &library) == ESP_MIDI_ERR_OK);
    esp_midi_lib_sample_desc_t a = { .ch_index = 4, .note = 60, .velocity = 100, .program_change = 32 };
    esp_midi_lib_sample_desc_t b = { .ch_index = 7, .note = 60, .velocity = 40, .program_change = 33 };
    assert(cfg.note_on_cb(library, &a) == ESP_MIDI_ERR_OK);
    uint8_t *shared = a.data.buffer;
    uint32_t original = sample_hash(&a);
    int before = sdk_allocations;
    /* The second voice may allocate metadata, but never another PCM block. */
    sdk_allocation_limit = 128;
    sdk_refused_allocations = 0;
    assert(cfg.note_on_cb(library, &b) == ESP_MIDI_ERR_OK);
    assert(sdk_refused_allocations == 0 && sdk_allocations == before + 1);
    sdk_allocation_limit = 0;
    assert(b.data.buffer == shared && a.gain != b.gain);
    assert(a.data.len == 4u * KORVO_MIDI_RATE * sizeof(int16_t));
    assert(sample_hash(&b) == original);

    /* Freeing by pointer alone would remove b's record and misroute note-off,
     * because its newer voice has the same PCM address as a. */
    assert(cfg.free_data_cb(library, &a) == ESP_MIDI_ERR_OK);
    unsigned calls = sdk_note_off_calls;
    esp_midi_parse_event_t event = { .type = ESP_MIDI_EVENT_TYPE_NOTE_OFF,
        .msg.note_msg = { .channel = 4, .note = 60 } };
    __wrap_esp_midi_synth_handle_note_off(library, &event);
    assert(sdk_note_off_calls == calls);
    event.msg.note_msg.channel = 7;
    __wrap_esp_midi_synth_handle_note_off(library, &event);
    assert(sdk_note_off_calls == calls + 1 && sample_hash(&b) == original);

    /* A distinct instrument needs distinct content while b remains active. */
    esp_midi_lib_sample_desc_t c = { .ch_index = 3, .note = 60, .velocity = 100, .program_change = 80 };
    assert(cfg.note_on_cb(library, &c) == ESP_MIDI_ERR_OK);
    assert(c.data.buffer != shared && sample_hash(&c) != original);
    uint8_t *idle = c.data.buffer;
    assert(cfg.free_data_cb(library, &c) == ESP_MIDI_ERR_OK);
    c.note = 65;
    sdk_allocation_limit = 128;
    sdk_refused_allocations = 0;
    assert(cfg.note_on_cb(library, &c) == ESP_MIDI_ERR_OK);
    sdk_allocation_limit = 0;
    assert(sdk_refused_allocations == 0 && c.data.buffer == idle);
    assert(sample_hash(&b) == original); /* no active buffer was overwritten */
    assert(cfg.free_data_cb(library, &c) == ESP_MIDI_ERR_OK);
    int with_idle = sdk_allocations;
    assert(cfg.free_data_cb(library, &b) == ESP_MIDI_ERR_OK);
    assert(sdk_allocations == with_idle - 3); /* voice plus the older idle pair */

    /* Repeating the same idle note preserves its samples without allocation. */
    sdk_allocation_limit = 128;
    sdk_refused_allocations = 0;
    assert(cfg.note_on_cb(library, &b) == ESP_MIDI_ERR_OK);
    sdk_allocation_limit = 0;
    assert(sdk_refused_allocations == 0 && b.data.buffer == shared);
    assert(sample_hash(&b) == original);

    /* Failed metadata allocation must not consume a reference or a cache hit. */
    before = sdk_allocations;
    sdk_allocation_limit = 1;
    assert(cfg.note_on_cb(library, &a) == ESP_MIDI_ERR_MEM_LACK);
    sdk_allocation_limit = 0;
    assert(sdk_allocations == before && sample_hash(&b) == original);
    assert(cfg.free_data_cb(library, &a) == ESP_MIDI_ERR_OK);
    assert(cfg.note_on_cb(library, &a) == ESP_MIDI_ERR_OK);
    assert(a.data.buffer == shared);
    c.note = 66;
    assert(cfg.note_on_cb(library, &c) == ESP_MIDI_ERR_OK);
    assert(cfg.free_data_cb(library, &c) == ESP_MIDI_ERR_OK);
    before = sdk_allocations;
    korvo_soundfont_trim();
    assert(sdk_allocations == before - 2);
    assert(sample_hash(&a) == original && sample_hash(&b) == original);
    korvo_soundfont_trim();
    assert(sdk_allocations == before - 2);
    /* Close may omit free_data for several voices sharing one allocation. */
    assert(cfg.delete_cb(library) == ESP_MIDI_ERR_OK);
    korvo_soundfont_trim();
    assert(sdk_allocations == 0);

    assert(cfg.loader_cb(NULL, &library) == ESP_MIDI_ERR_OK);
    esp_midi_lib_sample_desc_t voices[12] = {0};
    for (unsigned i = 0; i < 12; ++i) {
        voices[i].note = 48 + i;
        voices[i].velocity = 100;
        esp_midi_err_t result = cfg.note_on_cb(library, &voices[i]);
        assert(result == (i < 11 ? ESP_MIDI_ERR_OK : ESP_MIDI_ERR_MEM_LACK));
    }
    /* Once a voice ends its slot can serve a new pitch at the same budget. */
    assert(cfg.free_data_cb(library, &voices[3]) == ESP_MIDI_ERR_OK);
    sdk_allocation_limit = 128;
    assert(cfg.note_on_cb(library, &voices[11]) == ESP_MIDI_ERR_OK);
    sdk_allocation_limit = 0;
    assert(cfg.delete_cb(library) == ESP_MIDI_ERR_OK);
    assert(sdk_allocations == 0);
    puts("MIDI shared PCM: independent voices, one idle slot, sample integrity, budget, failure and shutdown: PASS");
}

typedef struct {
    esp_midi_lib_cfg_t config;
    esp_midi_lib_loader_handle_t owner;
    esp_midi_lib_sample_desc_t *sample;
} resume_loader_context;

static void *resume_loader_task(void *argument)
{
    resume_loader_context *context = argument;
    /* No note_on runs in this new thread: these are an engine's old voices. */
    korvo_soundfont_bind(context->owner);
    esp_midi_parse_event_t event = { .type = ESP_MIDI_EVENT_TYPE_NOTE_OFF,
        .msg.note_msg = { .channel = context->sample->ch_index, .note = context->sample->note } };
    unsigned calls = sdk_note_off_calls;
    __wrap_esp_midi_synth_handle_note_off(context->owner, &event);
    assert(sdk_note_off_calls == calls + 1);
    assert(context->config.free_data_cb(context->owner, context->sample) == ESP_MIDI_ERR_OK);
    int before = sdk_allocations;
    korvo_soundfont_trim();
    assert(sdk_allocations == before - 2);
    korvo_soundfont_trim();
    assert(sdk_allocations == before - 2);
    korvo_soundfont_bind(NULL);
    return NULL;
}

static void test_soundfont_task_binding(void)
{
    esp_midi_lib_loader_handle_t owner = NULL, library = NULL;
    esp_midi_lib_cfg_t cfg = korvo_soundfont_config_for_owner(&owner);
    assert(cfg.loader_cb(cfg.loader_cfg, &library) == ESP_MIDI_ERR_OK);
    assert(owner == library && library);
    for (unsigned i = 0; i < 2; ++i) {
        esp_midi_lib_sample_desc_t sample = { .ch_index = 2, .note = 60, .velocity = 100 };
        assert(cfg.note_on_cb(library, &sample) == ESP_MIDI_ERR_OK);
        resume_loader_context context = { .config = cfg, .owner = owner, .sample = &sample };
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, resume_loader_task, &context));
        assert(!pthread_join(thread, NULL));
        assert(sdk_allocations == 1); /* no idle PCM survives task completion */
    }
    assert(cfg.delete_cb(library) == ESP_MIDI_ERR_OK);
    assert(!owner && sdk_allocations == 0);
    cfg = korvo_soundfont_config_for_owner(&owner);
    sdk_allocation_limit = 1;
    assert(cfg.loader_cb(cfg.loader_cfg, &library) == ESP_MIDI_ERR_MEM_LACK);
    sdk_allocation_limit = 0;
    assert(!library && !owner && sdk_allocations == 0);
    puts("MIDI resumed task: existing voice note-off, idle trim, owner reset and load failure: PASS");
}

static void wait_pcm_frames(unsigned frames)
{
    for (unsigned i = 0; i < 2000 && pcm_frames < frames; ++i) vTaskDelay(1);
    assert(pcm_frames == frames);
}

static void wait_midi_finished(nj_port *port, void *midi)
{
    for (unsigned i = 0; i < 2000 && !port->midi_finished(port->ud, midi); ++i) vTaskDelay(1);
    assert(port->midi_finished(port->ud, midi) == 1);
}

static void test_stopped_player_memory(void)
{
    const uint8_t file[] = {'M','T','h','d',0,0,0,6,0,0,0,1,0,96,'M','T','r','k',0,0,0,4,0,0xff,0x2f,0};
    korvo_port_state state = {0};
    nj_port port = { .ud = &state, .pcm_open = pcm_open, .pcm_write = pcm_write,
        .pcm_free = pcm_free, .pcm_close = pcm_close, .tone_stop = pcm_stop };
    nj_s2 reference[4096];
    nj_u4 duration;
    korvo_midi_port(&port, &state);
    synth_blocks = 16;
    void *midi = port.midi_open(&state, file, sizeof(file), &duration);
    assert(midi && synth_handles == 1);
    unsigned stops = pcm_stop_calls;
    assert(port.midi_start(&state, midi, 0) == 0);
    wait_midi_finished(&port, midi);
    assert(pcm_frames == 4096 && synth_handles == 0);
    assert(pcm_stop_calls == stops && !state.midi_active);
    assert(sdk_allocations == 2); /* persistent Player and copied MIDI only */
    memcpy(reference, pcm_capture, sizeof(reference));

    /* An EOF restart rewinds even though the old engine is already closed. */
    pcm_frame_limit = 300;
    assert(port.midi_start(&state, midi, 0) == 0);
    wait_pcm_frames(300);
    port.midi_stop(&state, midi);
    assert(pcm_stop_calls == stops + 1);
    assert(synth_handles == 0 && sdk_allocations == 2);
    assert(port.midi_time(&state, midi) == 300u * 1000u / KORVO_MIDI_RATE);
    assert(memcmp(pcm_capture, reference, 300 * sizeof(nj_s2)) == 0);

    /* Resume after 300 accepted frames, not a rounded millisecond or the
     * SDK's 512-frame frontier. Interrupt that resume during another write. */
    pcm_frame_limit = 111;
    assert(port.midi_start(&state, midi, 0) == 0);
    wait_pcm_frames(111);
    port.midi_stop(&state, midi);
    assert(synth_handles == 0 && sdk_allocations == 2);
    assert(port.midi_time(&state, midi) == 411u * 1000u / KORVO_MIDI_RATE);
    assert(memcmp(pcm_capture, reference + 300, 111 * sizeof(nj_s2)) == 0);

    /* All unsuccessful restart paths release partially rebuilt state and
     * preserve the next frame for a later successful attempt. */
    synth_open_fails = true;
    assert(port.midi_start(&state, midi, 0) < 0);
    synth_open_fails = false;
    synth_parse_fails = true;
    assert(port.midi_start(&state, midi, 0) < 0);
    synth_parse_fails = false;
    pcm_open_fails = true;
    assert(port.midi_start(&state, midi, 0) < 0);
    pcm_open_fails = false;
    task_create_fails = true;
    assert(port.midi_start(&state, midi, 0) < 0);
    task_create_fails = false;
    assert(synth_handles == 0 && sdk_allocations == 2 && !state.midi_active);
    assert(port.midi_time(&state, midi) == 411u * 1000u / KORVO_MIDI_RATE);
    pcm_frame_limit = UINT32_MAX;
    stops = pcm_stop_calls;
    assert(port.midi_start(&state, midi, 0) == 0);
    wait_midi_finished(&port, midi);
    assert(pcm_stop_calls == stops);
    assert(pcm_frames == 4096 - 411 && synth_handles == 0 && sdk_allocations == 2);
    assert(memcmp(pcm_capture, reference + 411, (4096 - 411) * sizeof(nj_s2)) == 0);

    /* Seeking a stopped Player does not retain a synth. If fast-forward is
     * itself stopped, the next start still seeks to the requested position. */
    synth_blocks = 128;
    port.midi_seek(&state, midi, 1000);
    assert(synth_handles == 0 && sdk_allocations == 2);
    assert(port.midi_time(&state, midi) == 1000 && !port.midi_finished(&state, midi));
    synth_output_block_limit = 3;
    assert(port.midi_start(&state, midi, 0) == 0);
    for (unsigned i = 0; i < 1000 && synth_output_blocks < 3; ++i) vTaskDelay(1);
    port.midi_stop(&state, midi);
    synth_output_block_limit = UINT32_MAX;
    assert(synth_output_blocks >= 3 && pcm_frames == 0);
    assert(synth_handles == 0 && sdk_allocations == 2 && port.midi_time(&state, midi) == 1000);
    assert(port.midi_start(&state, midi, 0) == 0);
    wait_midi_finished(&port, midi);
    assert(pcm_frames == 128u * 256u - KORVO_MIDI_RATE);
    assert(port.midi_time(&state, midi) == 128u * 256u * 1000u / KORVO_MIDI_RATE);
    port.midi_close(&state, midi);
    assert(synth_handles == 0 && sdk_allocations == 0);
    synth_blocks = 16;

    /* Retain several stopped handles like a game's music/effect bank. Each
     * still owns MIDI bytes and its public time, but no soundfont or voices. */
    void *players[4];
    for (unsigned i = 0; i < 4; ++i) {
        players[i] = port.midi_open(&state, file, sizeof(file), &duration);
        assert(players[i]);
        pcm_frame_limit = 123;
        assert(port.midi_start(&state, players[i], 0) == 0);
        wait_pcm_frames(123);
        port.midi_stop(&state, players[i]);
        assert(synth_handles == 0 && sdk_allocations == (int)(i + 1) * 2);
        assert(port.midi_time(&state, players[i]) == 123u * 1000u / KORVO_MIDI_RATE);
    }
    /* Device preemption still reports the displaced Player as stopped and
     * releases it before the new Player's synth needs sample memory. */
    pcm_frame_limit = 50;
    assert(port.midi_start(&state, players[0], 0) == 0);
    wait_pcm_frames(50);
    assert(port.midi_start(&state, players[1], 0) == 0);
    wait_pcm_frames(50);
    assert(port.midi_finished(&state, players[0]) == -1 && synth_handles == 1);
    port.midi_stop(&state, players[1]);
    assert(synth_handles == 0 && sdk_allocations == 8);
    pcm_frame_limit = UINT32_MAX;
    korvo_midi_shutdown(&state);
    assert(!state.midi_players && !state.midi_active);
    assert(synth_handles == 0 && sdk_allocations == 0);
    puts("MIDI stopped Players: sample-exact resume, interrupted seek, failure cleanup, preemption and retained-handle memory: PASS");
}

void test_midi(void)
{
    test_soundfont();
    test_soundfont_windowed();
    test_midi_merge();
    test_builtin_signal();
    test_unmatched_note_off();
    test_builtin_shared_samples();
    test_soundfont_task_binding();
    test_stopped_player_memory();
    korvo_port_state state = {0};
    nj_port port = { .ud = &state, .pcm_open = pcm_open, .pcm_write = pcm_write,
        .pcm_free = pcm_free, .pcm_close = pcm_close, .tone_stop = pcm_stop };
    korvo_midi_port(&port, &state);
    const uint8_t file[] = {'M','T','h','d',0,0,0,6,0,0,0,1,0,96,'M','T','r','k',0,0,0,4,0,0xff,0x2f,0};
    nj_u4 duration = 99;
    for (size_t length = 0; length < sizeof(file); ++length)
        assert(!port.midi_open(&state, file, length, &duration));
    uint8_t malformed[sizeof(file)];
    memcpy(malformed, file, sizeof(file));
    memset(malformed + 4, 255, 4);
    assert(!port.midi_open(&state, malformed, sizeof(malformed), &duration));
    void *midi = port.midi_open(&state, file, sizeof(file), &duration);
    assert(midi && duration == 0 && state.midi_players);
    port.midi_volume(&state, midi, 20);
    assert(!port.midi_start(&state, midi, 0));
    for (int i = 0; i < 1000 && !port.midi_finished(&state, midi); ++i) vTaskDelay(1);
    assert(port.midi_finished(&state, midi));
    assert(pcm_frames == synth_blocks * 256 && pcm_peak > 500 && pcm_peak < 1000);
    assert(port.midi_time(&state, midi) > 100);
    port.midi_seek(&state, midi, 0);
    assert(port.midi_time(&state, midi) == 0);
    assert(!port.midi_start(&state, midi, 1));
    vTaskDelay(12);
    port.midi_stop(&state, midi);
    assert(!state.midi_active);
    unsigned stopped = pcm_frames;
    vTaskDelay(5);
    assert(pcm_frames == stopped);
    port.midi_seek(&state, midi, 100);
    assert(port.midi_time(&state, midi) >= 99);
    assert(!port.midi_start(&state, midi, 0));
    void *other = port.midi_open(&state, file, sizeof(file), &duration);
    assert(other && !port.midi_start(&state, other, 0));
    assert(state.midi_active == other);
    korvo_midi_shutdown(&state);
    assert(!state.midi_players && !state.midi_active);
    vTaskDelay(5);
    assert(synth_handles == 0 && sdk_allocations == 0);
    puts("MIDI/SF2 adapters: default instruments, bounded sample reads, partial PCM writes, volume, finish, stop/seek/loop, multiple handles, shutdown");
}
