#include "korvo_soundfont.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <stdint.h>

#define WAVE_SAMPLES 256
#define BUILTIN_FRAMES (KORVO_MIDI_RATE * 4u)
#define SAMPLE_BUDGET (2u * 1024u * 1024u)

typedef struct sample_buffer {
    struct sample_buffer *next;
    uint8_t *buffer;
    size_t bytes;
    unsigned references;
    uint16_t instrument, note;
    bool builtin;
} sample_buffer;

typedef struct sample_allocation {
    struct sample_allocation *next;
    sample_buffer *storage;
    uint16_t channel, note;
} sample_allocation;

typedef struct {
    esp_midi_sf_parse_handle_t parser;
    FILE *file;
    size_t file_size, sample_bytes;
    bool bank_valid, program_valid;
    sample_allocation *samples;
    sample_buffer *buffers, *idle_builtin;
    esp_midi_lib_loader_handle_t *owner_slot;
    uint32_t allocations, shared_hits, idle_hits, refills;
    size_t peak_sample_bytes;
    int16_t waves[8][WAVE_SAMPLES];
} soundfont;

/* Each synth runs on its own task. Keep its live samples available to the
 * note-off guard without exposing the SDK's private synth layout. */
static _Thread_local soundfont *playing_soundfont;

extern esp_midi_err_t __real_esp_midi_synth_handle_note_off(void *synth, esp_midi_parse_event_t *event);

esp_midi_err_t __wrap_esp_midi_synth_handle_note_off(void *synth, esp_midi_parse_event_t *event)
{
    /* esp_midi 1.1.0 traverses a circular voice pool without a bound when a
     * note-off has no live voice. This includes notes whose samples expired,
     * failed allocations, and duplicate note-offs. MIDI permits ignoring them.
     * Live voices still use the SDK's normal RELEASE handling. */
    if (playing_soundfont) {
        for (sample_allocation *sample = playing_soundfont->samples; sample; sample = sample->next)
            if (sample->channel == event->msg.note_msg.channel && sample->note == event->msg.note_msg.note)
                return __real_esp_midi_synth_handle_note_off(synth, event);
    }
    return ESP_MIDI_ERR_OK;
}

static esp_midi_err_t read_sf(uint8_t *buffer, uint32_t size, void *ud)
{
    soundfont *sf = ud;
    return fread(buffer, 1, size, sf->file) == size ? ESP_MIDI_ERR_OK : ESP_MIDI_ERR_DATA_LACK;
}

static esp_midi_err_t seek_sf(uint32_t size, void *ud)
{
    soundfont *sf = ud;
    long position = ftell(sf->file);
    if (position < 0 || (uint64_t)position + size > sf->file_size) return ESP_MIDI_ERR_DATA_LACK;
    return fseek(sf->file, size, SEEK_CUR) ? ESP_MIDI_ERR_FAIL : ESP_MIDI_ERR_OK;
}

static esp_midi_err_t load_sf(esp_midi_lib_loader_cfg_t config, esp_midi_lib_loader_handle_t *handle)
{
    *handle = NULL;
    soundfont *sf = heap_caps_calloc(1, sizeof(*sf), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sf) return ESP_MIDI_ERR_MEM_LACK;
    /* Compact built-in wavetable bank keeps MIDI usable without an SD card
     * or a separate sound partition. SF2 may supply real instrument samples. */
    uint32_t random = 0x12345678;
    sf->bank_valid = sf->program_valid = true;
    for (unsigned i = 0; i < WAVE_SAMPLES; ++i) {
        float phase = (float)i / WAVE_SAMPLES;
        float sine = sinf(phase * 6.283185307f);
        random = random * 1664525u + 1013904223u;
        sf->waves[0][i] = (int16_t)(sine * 12000);
        sf->waves[1][i] = (int16_t)((1 - 4 * fabsf(phase - 0.5f)) * 10000);
        sf->waves[2][i] = (int16_t)((phase * 2 - 1) * 8000);
        sf->waves[3][i] = i < WAVE_SAMPLES / 2 ? 7000 : -7000;
        sf->waves[4][i] = (int16_t)((sine + 0.4f * sinf(phase * 12.566370614f)) * 8000);
        sf->waves[5][i] = (int16_t)((sine + 0.3f * sinf(phase * 18.849555922f)) * 8500);
        sf->waves[6][i] = (int16_t)((sine + 0.2f * sinf(phase * 31.415926535f)) * 9000);
        sf->waves[7][i] = (int16_t)((int32_t)(random >> 16) - 32768) / 4;
    }
    const char *paths[] = {config, "/sdcard/j2me/soundfont.sf2", "/flash/j2me/soundfont.sf2"};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        if (!paths[i]) continue;
        sf->file = fopen(paths[i], "rb");
        if (!sf->file) continue;
        if (!fseek(sf->file, 0, SEEK_END)) {
            long size = ftell(sf->file);
            if (size > 0 && size <= 32 * 1024 * 1024) {
                sf->file_size = size;
                rewind(sf->file);
                esp_midi_sf_parse_cfg_t cfg = { .read = read_sf, .seek = seek_sf, .ctx = sf };
                if (esp_midi_sf_parse_open(&cfg, &sf->parser) == ESP_MIDI_ERR_OK &&
                    esp_midi_sf_parse_process(sf->parser) == ESP_MIDI_ERR_OK) {
                    ESP_LOGI("j2me-midi", "SoundFont: %s", paths[i]);
                    break;
                }
            }
        }
        if (sf->parser) esp_midi_sf_parse_close(sf->parser);
        sf->parser = NULL;
        fclose(sf->file);
        sf->file = NULL;
        ESP_LOGW("j2me-midi", "Cannot load %s; using compact instrument bank", paths[i]);
    }
    *handle = sf;
    return ESP_MIDI_ERR_OK;
}

static esp_midi_err_t delete_sf(esp_midi_lib_loader_handle_t handle)
{
    soundfont *sf = handle;
    if (sf) {
        printf("MIDI_CACHE allocations=%lu shared=%lu idle_hits=%lu refills=%lu peak_bytes=%lu\n",
            (unsigned long)sf->allocations, (unsigned long)sf->shared_hits,
            (unsigned long)sf->idle_hits, (unsigned long)sf->refills,
            (unsigned long)sf->peak_sample_bytes);
        if (playing_soundfont == sf) playing_soundfont = NULL;
        if (sf->owner_slot && *sf->owner_slot == sf) *sf->owner_slot = NULL;
        /* The SDK's close path frees its voice descriptors without invoking
         * free_data_cb for voices still playing. Keep independent ownership. */
        while (sf->samples) {
            sample_allocation *sample = sf->samples;
            sf->samples = sample->next;
            heap_caps_free(sample);
        }
        while (sf->buffers) {
            sample_buffer *storage = sf->buffers;
            sf->buffers = storage->next;
            heap_caps_free(storage->buffer);
            heap_caps_free(storage);
        }
        if (sf->parser) esp_midi_sf_parse_close(sf->parser);
        if (sf->file) fclose(sf->file);
        heap_caps_free(sf);
    }
    return ESP_MIDI_ERR_OK;
}

static esp_midi_err_t set_data(esp_midi_lib_loader_handle_t handle, esp_midi_lib_sample_desc_t *desc);

static void drop_buffer(soundfont *sf, sample_buffer *storage)
{
    sample_buffer **link = &sf->buffers;
    while (*link && *link != storage) link = &(*link)->next;
    if (*link) *link = storage->next;
    if (sf->idle_builtin == storage) sf->idle_builtin = NULL;
    sf->sample_bytes -= storage->bytes;
    heap_caps_free(storage->buffer);
    heap_caps_free(storage);
}

static void drop_idle(soundfont *sf)
{
    if (sf->idle_builtin) drop_buffer(sf, sf->idle_builtin);
}

void korvo_soundfont_trim(void)
{
    /* Called by the synthesis task before it signals completion. Each task
     * owns its loader, so trimming cannot race the caller closing the engine. */
    if (playing_soundfont) drop_idle(playing_soundfont);
}

void korvo_soundfont_bind(esp_midi_lib_loader_handle_t handle)
{
    playing_soundfont = handle;
}

static bool reference_sample(soundfont *sf, sample_buffer *storage, const esp_midi_lib_sample_desc_t *desc)
{
    sample_allocation *sample = heap_caps_malloc(sizeof(*sample), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sample) return false;
    *sample = (sample_allocation){ .next = sf->samples, .storage = storage,
        .channel = desc->ch_index, .note = desc->note };
    sf->samples = sample;
    ++storage->references;
    if (sf->idle_builtin == storage) sf->idle_builtin = NULL;
    return true;
}

static sample_buffer *own_sample(soundfont *sf, void *buffer, size_t bytes,
    const esp_midi_lib_sample_desc_t *desc, bool builtin, unsigned instrument)
{
    sample_buffer *storage = heap_caps_malloc(sizeof(*storage), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!storage) { heap_caps_free(buffer); return NULL; }
    *storage = (sample_buffer){ .next = sf->buffers, .buffer = buffer, .bytes = bytes,
        .builtin = builtin, .instrument = instrument, .note = desc->note };
    sf->buffers = storage;
    sf->sample_bytes += bytes;
    ++sf->allocations;
    if (sf->sample_bytes > sf->peak_sample_bytes) sf->peak_sample_bytes = sf->sample_bytes;
    if (!reference_sample(sf, storage, desc)) {
        drop_buffer(sf, storage);
        return NULL;
    }
    return storage;
}

static esp_midi_err_t note_on(esp_midi_lib_loader_handle_t handle, esp_midi_lib_sample_desc_t *desc)
{
    soundfont *sf = handle;
    playing_soundfont = sf;
    esp_midi_sf_sample_t *sample = NULL;
    if (sf->parser && sf->bank_valid && sf->program_valid &&
        esp_midi_sf_parse_note_on(sf->parser, desc->note, desc->velocity, &sample) == ESP_MIDI_ERR_OK && sample) {
        desc->loopstart = sample->loopstart;
        desc->loopend = sample->loopend;
        desc->origpitch = sample->origpitch;
        desc->pitchadj = sample->pitchadj;
        desc->samplerate = sample->samplerate;
        desc->sampletype = sample->sampletype == 2 ? ESP_MIDI_SF_SAMPLE_TYPE_RIGHT :
            sample->sampletype == 4 ? ESP_MIDI_SF_SAMPLE_TYPE_LEFT : ESP_MIDI_SF_SAMPLE_TYPE_MONO;
        desc->data = (esp_midi_data_t){0};
    } else {
        unsigned instrument = desc->ch_index == 9 ? 7 : (desc->program_change / 16) % 7;
        desc->loopstart = 0;
        desc->loopend = WAVE_SAMPLES;
        desc->origpitch = 60;
        desc->pitchadj = 0;
        /* set_data expands this cycle into pitched PCM at the mixer rate. */
        desc->samplerate = KORVO_MIDI_RATE;
        desc->sampletype = ESP_MIDI_SF_SAMPLE_TYPE_MONO;
        desc->data = (esp_midi_data_t){ .buffer = (uint8_t *)sf->waves[instrument], .len = sizeof(sf->waves[0]) };
    }
    desc->bits = 16;
    desc->gain = 0.6f * desc->velocity / 127.0f;
    /* The SDK checks sample rate BEFORE set_data_cb, and silently discards
     * notes that do not exactly match its output format. Prepare pitched PCM
     * here: this mixer neither resamples nor repeats a single wavetable cycle. */
    return set_data(handle, desc);
}

/* Keep only a small read window while pitching a file sample. Loading the
 * whole source alongside the destination can double the peak sample memory. */
static esp_midi_err_t read_pitched_sample(soundfont *sf, uint64_t position,
    size_t source_frames, double step, int16_t *buffer, size_t frames)
{
    uint8_t window[1024];
    size_t first = 0, loaded = 0;
    for (size_t i = 0; i < frames; ++i) {
        double phase = i * step;
        size_t index = (size_t)phase;
        if (index >= source_frames) index = source_frames - 1;
        size_t next = index + 1 < source_frames ? index + 1 : index;
        if (!loaded || index < first || next - first >= loaded) {
            first = index;
            loaded = source_frames - first;
            if (loaded > sizeof(window) / 2u) loaded = sizeof(window) / 2u;
            if (fseek(sf->file, (long)(position + first * 2u), SEEK_SET) ||
                fread(window, 2u, loaded, sf->file) != loaded)
                return ESP_MIDI_ERR_DATA_LACK;
        }
        size_t a = (index - first) * 2u, b = (next - first) * 2u;
        int16_t left = (int16_t)((uint16_t)window[a] | (uint16_t)window[a + 1] << 8);
        int16_t right = (int16_t)((uint16_t)window[b] | (uint16_t)window[b + 1] << 8);
        buffer[i] = (int16_t)(left + (right - left) * (phase - index));
    }
    return ESP_MIDI_ERR_OK;
}

static esp_midi_err_t set_data(esp_midi_lib_loader_handle_t handle, esp_midi_lib_sample_desc_t *desc)
{
    soundfont *sf = handle;
    uintptr_t ptr = (uintptr_t)desc->data.buffer, waves = (uintptr_t)sf->waves;
    bool builtin = ptr >= waves && ptr < waves + sizeof(sf->waves);
    if (ptr && !builtin) return ESP_MIDI_ERR_OK;
    if (builtin) {
        size_t bytes = BUILTIN_FRAMES * sizeof(int16_t);
        unsigned instrument = (ptr - waves) / sizeof(sf->waves[0]);
        sample_buffer *storage = sf->buffers;
        while (storage && (!storage->builtin || storage->instrument != instrument || storage->note != desc->note))
            storage = storage->next;
        bool generate = !storage;
        if (storage) {
            /* The mixer reads PCM without modifying it; gain and playback
             * position remain in each voice's separate descriptor. */
            bool idle = storage->references == 0;
            if (!reference_sample(sf, storage, desc)) return ESP_MIDI_ERR_MEM_LACK;
            if (idle) ++sf->idle_hits;
            else ++sf->shared_hits;
        } else if (sf->idle_builtin) {
            /* Keep just one idle allocation. A different pitch overwrites it
             * only after its last voice has released the previous content. */
            storage = sf->idle_builtin;
            if (!reference_sample(sf, storage, desc)) return ESP_MIDI_ERR_MEM_LACK;
            storage->instrument = instrument;
            storage->note = desc->note;
            ++sf->refills;
        } else {
            if (bytes > SAMPLE_BUDGET - sf->sample_bytes) return ESP_MIDI_ERR_MEM_LACK;
            void *buffer = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!buffer) return ESP_MIDI_ERR_MEM_LACK;
            storage = own_sample(sf, buffer, bytes, desc, true, instrument);
            if (!storage) return ESP_MIDI_ERR_MEM_LACK;
        }
        int16_t *buffer = (int16_t *)storage->buffer;
        const int16_t *wave = (const int16_t *)desc->data.buffer;
        if (generate) {
            float step = 440.0f * powf(2.0f, ((int)desc->note - 69) / 12.0f) * WAVE_SAMPLES / KORVO_MIDI_RATE;
            float phase = 0;
            for (unsigned i = 0; i < BUILTIN_FRAMES; ++i) {
                unsigned index = (unsigned)phase;
                float fraction = phase - index;
                float value = wave[index] + (wave[(index + 1) % WAVE_SAMPLES] - wave[index]) * fraction;
                /* Five millisecond attack; the SDK handles note-off. Fade the
                 * finite fallback sample's final 100 ms instead of clicking. */
                if (i < KORVO_MIDI_RATE / 200u) value *= (float)i / (KORVO_MIDI_RATE / 200u);
                if (BUILTIN_FRAMES - i < KORVO_MIDI_RATE / 10u)
                    value *= (float)(BUILTIN_FRAMES - i) / (KORVO_MIDI_RATE / 10u);
                buffer[i] = (int16_t)value;
                phase += step;
                while (phase >= WAVE_SAMPLES) phase -= WAVE_SAMPLES;
            }
        }
        desc->data = (esp_midi_data_t){ .buffer = (uint8_t *)buffer, .len = bytes };
        desc->samplerate = KORVO_MIDI_RATE;
        desc->origpitch = desc->note;
        desc->pitchadj = 0;
        desc->loopstart = 0;
        desc->loopend = BUILTIN_FRAMES;
        return ESP_MIDI_ERR_OK;
    }
    if (!sf->parser || desc->loopend <= desc->loopstart || desc->samplerate <= 0) return ESP_MIDI_ERR_FAIL;
    uint64_t source_bytes = (uint64_t)(desc->loopend - desc->loopstart) * 2u;
    if (source_bytes > 512u * 1024u) return ESP_MIDI_ERR_MEM_LACK;
    double step = (double)desc->samplerate / KORVO_MIDI_RATE *
        pow(2.0, ((int)desc->note - desc->origpitch + desc->pitchadj / 100.0) / 12.0);
    double length = ceil((source_bytes / 2u) / step);
    if (!isfinite(length) || length < 1 || length > 256u * 1024u) return ESP_MIDI_ERR_MEM_LACK;
    size_t frames = (size_t)length, bytes = frames * 2u;
    esp_midi_sf_sdat_t data = {0};
    if (esp_midi_sf_parse_get_sdat(sf->parser, &data) != ESP_MIDI_ERR_OK) return ESP_MIDI_ERR_FAIL;
    uint64_t position = (uint64_t)data.smpl_pos + (uint64_t)desc->loopstart * 2u;
    if (position + source_bytes > sf->file_size || (uint64_t)desc->loopend * 2u > data.smpl_size)
        return ESP_MIDI_ERR_DATA_LACK;
    /* Optional cached PCM must not prevent loading a real instrument. */
    drop_idle(sf);
    if (bytes > SAMPLE_BUDGET - sf->sample_bytes) return ESP_MIDI_ERR_MEM_LACK;
    int16_t *buffer = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) return ESP_MIDI_ERR_MEM_LACK;
    esp_midi_err_t result;
    if (step == 1.0) {
        result = !fseek(sf->file, (long)position, SEEK_SET) &&
            fread(buffer, 1, bytes, sf->file) == bytes ? ESP_MIDI_ERR_OK : ESP_MIDI_ERR_DATA_LACK;
    } else {
        result = read_pitched_sample(sf, position, (size_t)(source_bytes / 2u), step, buffer, frames);
    }
    if (result != ESP_MIDI_ERR_OK) {
        heap_caps_free(buffer);
        return result;
    }
    if (!own_sample(sf, buffer, bytes, desc, false, 0)) return ESP_MIDI_ERR_MEM_LACK;
    desc->data = (esp_midi_data_t){ .buffer = (uint8_t *)buffer, .len = bytes };
    desc->samplerate = KORVO_MIDI_RATE;
    desc->origpitch = desc->note;
    desc->pitchadj = 0;
    desc->loopstart = 0;
    desc->loopend = bytes / 2u;
    return ESP_MIDI_ERR_OK;
}

static esp_midi_err_t free_data(esp_midi_lib_loader_handle_t handle, esp_midi_lib_sample_desc_t *desc)
{
    soundfont *sf = handle;
    sample_allocation **link = &sf->samples;
    while (*link && ((*link)->storage->buffer != desc->data.buffer ||
                    (*link)->channel != desc->ch_index || (*link)->note != desc->note))
        link = &(*link)->next;
    if (*link) {
        sample_allocation *sample = *link;
        sample_buffer *storage = sample->storage;
        *link = sample->next;
        heap_caps_free(sample);
        if (--storage->references == 0) {
            if (storage->builtin) {
                drop_idle(sf);
                sf->idle_builtin = storage;
            } else {
                drop_buffer(sf, storage);
            }
        }
    }
    desc->data = (esp_midi_data_t){0};
    return ESP_MIDI_ERR_OK;
}

static esp_midi_err_t set_bank(esp_midi_lib_loader_handle_t handle, int32_t bank)
{
    soundfont *sf = handle;
    if (sf->parser) sf->bank_valid = esp_midi_sf_parse_set_bank(sf->parser, bank) == ESP_MIDI_ERR_OK;
    return ESP_MIDI_ERR_OK;
}

static esp_midi_err_t set_program(esp_midi_lib_loader_handle_t handle, int32_t program)
{
    soundfont *sf = handle;
    if (sf->parser) sf->program_valid = esp_midi_sf_parse_set_program(sf->parser, program) == ESP_MIDI_ERR_OK;
    return ESP_MIDI_ERR_OK;
}

esp_midi_lib_cfg_t korvo_soundfont_config(void)
{
    return (esp_midi_lib_cfg_t){ .loader_cb = load_sf, .delete_cb = delete_sf,
        .note_on_cb = note_on, .set_data_cb = set_data, .free_data_cb = free_data,
        .set_bank_cb = set_bank, .set_preset_cb = set_program };
}

static esp_midi_err_t load_owned_sf(esp_midi_lib_loader_cfg_t config, esp_midi_lib_loader_handle_t *handle)
{
    esp_midi_lib_loader_handle_t *owner = config;
    if (owner) *owner = NULL;
    esp_midi_err_t result = load_sf(NULL, handle);
    if (result == ESP_MIDI_ERR_OK && owner) {
        soundfont *sf = *handle;
        sf->owner_slot = owner;
        *owner = sf;
    }
    return result;
}

esp_midi_lib_cfg_t korvo_soundfont_config_for_owner(esp_midi_lib_loader_handle_t *owner)
{
    esp_midi_lib_cfg_t config = korvo_soundfont_config();
    if (owner) *owner = NULL;
    config.loader_cfg = owner;
    config.loader_cb = load_owned_sf;
    return config;
}
