#include "korvo_midi.h"
#include "audio_timing.h"
#include "korvo_midi_file.h"
#include "korvo_soundfont.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define MIDI_RATE KORVO_MIDI_RATE

typedef struct korvo_midi {
    struct korvo_midi *next;
    korvo_port_state *owner;
    esp_midi_sound_handle_t engine;
    esp_midi_lib_loader_handle_t soundfont;
    SemaphoreHandle_t done;
    uint8_t *data;
    nj_u4 size;
    atomic_uint frames, volume;
    atomic_bool stop, finished;
    uint64_t skip_frames;
    uint64_t sample_abs;
    uint32_t sample_count, sample_nonzero, sample_peak;
    int64_t finished_at;
    bool started, loop, restart, preempted;
} korvo_midi;

static void midi_stop(void *ud, void *handle);
static int midi_start(void *ud, void *handle, int loop);

static esp_midi_err_t output_pcm(uint8_t *buffer, uint32_t bytes, void *ud)
{
    korvo_midi *midi = ud;
    const nj_port *port = midi->owner->midi_port;
    if (bytes % 2) return ESP_MIDI_ERR_FAIL;
    uint32_t frames = bytes / 2, offset = 0;
    if (midi->skip_frames) {
        offset = frames < midi->skip_frames ? frames : midi->skip_frames;
        midi->skip_frames -= offset;
    }
    int volume = atomic_load(&midi->volume);
    nj_s2 *samples = (nj_s2 *)buffer;
    uint32_t previous_count = midi->sample_count;
    if (volume != 100) for (uint32_t i = offset; i < frames; ++i) samples[i] = (int32_t)samples[i] * volume / 100;
    for (uint32_t i = offset; i < frames; ++i) {
        int32_t magnitude = samples[i] < 0 ? -(int32_t)samples[i] : samples[i];
        midi->sample_count++;
        midi->sample_nonzero += magnitude != 0;
        midi->sample_abs += magnitude;
        if ((uint32_t)magnitude > midi->sample_peak) midi->sample_peak = magnitude;
    }
    if (previous_count < MIDI_RATE / 5u && midi->sample_count >= MIDI_RATE / 5u)
        printf("MIDI_SIGNAL samples=%lu nonzero=%lu peak=%lu mean_abs=%llu volume=%u\n",
               (unsigned long)midi->sample_count, (unsigned long)midi->sample_nonzero,
               (unsigned long)midi->sample_peak,
               (unsigned long long)(midi->sample_abs / midi->sample_count), atomic_load(&midi->volume));
    while (offset < frames && !atomic_load(&midi->stop)) {
        int free_frames = port->pcm_free ? port->pcm_free(port->ud) : (int)(frames - offset);
        if (free_frames > (int)(frames - offset)) free_frames = frames - offset;
        if (free_frames > 0) {
            int written = port->pcm_write(port->ud, samples + offset, free_frames);
            if (written < 0 || written > free_frames) return ESP_MIDI_ERR_FAIL;
            offset += written;
            atomic_fetch_add(&midi->frames, written);
        }
        if (offset < frames) vTaskDelay(1);
    }
    /* Fast seeking also yields so that long silent sections feed the WDT. */
    if (midi->skip_frames) vTaskDelay(1);
    return atomic_load(&midi->stop) ? ESP_MIDI_ERR_FAIL : ESP_MIDI_ERR_OK;
}

static int parse_song(korvo_midi *midi)
{
    esp_midi_packet_t packet = { .data = { .buffer = midi->data, .len = midi->size } };
    esp_midi_sound_info_t info = {0};
    while (packet.data.len) {
        packet.consumed = 0;
        esp_midi_err_t status = esp_midi_sound_parse(midi->engine, &packet, &info);
        if (status == ESP_MIDI_ERR_OK) return 1;
        if (status != ESP_MIDI_ERR_CONTINUE || !packet.consumed || packet.consumed > packet.data.len) return 0;
        packet.data.buffer += packet.consumed;
        packet.data.len -= packet.consumed;
    }
    return 0;
}

/* Only the VM thread closes engines, after joining any synthesis task. */
static void close_engine(korvo_midi *midi)
{
    if (midi->engine) esp_midi_sound_close(midi->engine);
    midi->engine = NULL;
    midi->soundfont = NULL;
}

static int open_engine(korvo_midi *midi)
{
    esp_midi_sound_cfg_t config = { .max_out_stream_frame = 1024,
        /* REDUCE scales by the note-off release velocity; the usual velocity
         * zero leaves a voice unchanged. RELEASE honors ordinary note-offs. */
        .note_off_resp = ESP_MIDI_SYNTH_NOTE_OFF_RESP_RELEASE,
        .out_stream_info = { .samplerate = MIDI_RATE, .bits = 16, .channel = 1 },
        .out_stream_cb = output_pcm, .out_stream_ctx = midi,
        .sf_lib_cfg = korvo_soundfont_config_for_owner(&midi->soundfont) };
    if (esp_midi_sound_open(&config, &midi->engine) == ESP_MIDI_ERR_OK &&
        esp_midi_sound_load_sf_lib(midi->engine) == ESP_MIDI_ERR_OK && parse_song(midi)) return 1;
    close_engine(midi);
    return 0;
}

static void play_task(void *ud)
{
    korvo_midi *midi = ud;
    korvo_soundfont_bind(midi->soundfont);
    do {
        esp_midi_err_t status = esp_midi_sound_process(midi->engine);
        if (atomic_load(&midi->stop)) break;
        if (status != ESP_MIDI_ERR_OK) {
            ESP_LOGW("j2me-midi", "MIDI synthesis failed: %d", status);
            break;
        }
        if (!midi->loop) break;
        if (!parse_song(midi) || atomic_load(&midi->stop)) break;
        atomic_store(&midi->frames, 0);
        esp_midi_sound_resume(midi->engine);
    } while (!atomic_load(&midi->stop));
    printf("MIDI_PCM samples=%lu nonzero=%lu peak=%lu mean_abs=%llu volume=%u\n",
             (unsigned long)midi->sample_count, (unsigned long)midi->sample_nonzero,
             (unsigned long)midi->sample_peak,
             (unsigned long long)(midi->sample_count ? midi->sample_abs / midi->sample_count : 0),
             atomic_load(&midi->volume));
    if (midi->sample_count && !midi->sample_peak)
        ESP_LOGW("j2me-midi", "MIDI produced only silence; check notes and sample format");
    /* Keep reuse within a song, but do not retain idle PCM in completed Players. */
    korvo_soundfont_trim();
    korvo_soundfont_bind(NULL);
    if (!atomic_load(&midi->stop)) {
        const nj_port *port = midi->owner->midi_port;
        port->pcm_close(port->ud);
        midi->restart = true;
        midi->finished_at = esp_timer_get_time();
        atomic_store(&midi->finished, true);
    }
    xSemaphoreGive(midi->done);
    vTaskDelete(NULL);
}

static void midi_close(void *ud, void *handle)
{
    korvo_port_state *state = ud;
    korvo_midi *midi = handle;
    if (!midi) return;
    midi_stop(ud, midi);
    korvo_midi **link = &state->midi_players;
    while (*link && *link != midi) link = &(*link)->next;
    if (*link) *link = midi->next;
    close_engine(midi);
    if (midi->done) vSemaphoreDelete(midi->done);
    heap_caps_free(midi->data);
    heap_caps_free(midi);
}

static void *midi_open(void *ud, const void *data, nj_u4 size, nj_u4 *duration)
{
    korvo_port_state *state = ud;
    if (duration) *duration = 0; /* SDK has no duration query; MMAPI reports unknown. */
    korvo_midi *midi = heap_caps_calloc(1, sizeof(*midi), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!midi) return NULL;
    midi->owner = state;
    if (!korvo_midi_file_prepare(data, size, &midi->data, &midi->size)) goto failed;
    midi->done = xSemaphoreCreateBinary();
    atomic_init(&midi->frames, 0);
    atomic_init(&midi->volume, 100);
    atomic_init(&midi->stop, false);
    atomic_init(&midi->finished, false);
    if (!midi->data || !midi->done) goto failed;
    if (midi->size != size || ((const uint8_t *)data)[9] != 0)
        printf("MIDI_FILE input=%lu tracks=%u merged=%lu format=0\n", (unsigned long)size,
               ((const uint8_t *)data)[11], (unsigned long)midi->size);
    if (!open_engine(midi)) goto failed;
    midi->next = state->midi_players;
    state->midi_players = midi;
    return midi;
failed:
    midi_close(ud, midi);
    return NULL; /* The core's stream synthesizer remains available. */
}

static void midi_stop(void *ud, void *handle)
{
    korvo_port_state *state = ud;
    korvo_midi *midi = handle;
    if (midi->started) {
        atomic_store(&midi->stop, true);
        if (midi->engine) esp_midi_sound_pause(midi->engine);
        xSemaphoreTake(midi->done, portMAX_DELAY);
        midi->started = false;
        /* The SDK advances a full output block before calling output_pcm.
         * Retain only accepted frames, including a pending seek's position,
         * so reopening also restores a stop in the middle of a partial write. */
        midi->skip_frames = atomic_load(&midi->frames);
    }
    /* Games often retain stopped Players. Their live voices must not retain
     * megabytes of PCM until Player.close(); keep only MIDI bytes and time. */
    close_engine(midi);
    if (state->midi_active == midi) {
        if (state->midi_port->tone_stop) state->midi_port->tone_stop(state->midi_port->ud);
        state->midi_active = NULL;
    }
}

static int midi_start(void *ud, void *handle, int loop)
{
    korvo_port_state *state = ud;
    korvo_midi *midi = handle;
    if (state->midi_active) {
        korvo_midi *previous = state->midi_active;
        bool displaced = previous != midi && !atomic_load(&previous->finished);
        midi_stop(ud, previous);
        if (displaced) {
            /* The single output has moved to another Player. Let MMAPI stop
             * the displaced one, rather than leaving it STARTED forever. */
            previous->preempted = true;
            previous->restart = true;
        }
    }
    else if (midi->started) midi_stop(ud, midi);
    if (midi->restart) {
        close_engine(midi);
        atomic_store(&midi->frames, 0);
        midi->skip_frames = 0;
    }
    if (!midi->engine && !open_engine(midi)) return -1;
    midi->restart = false;
    if (state->midi_port->pcm_open(state->midi_port->ud, MIDI_RATE, 1) != 0) {
        close_engine(midi);
        return -1;
    }
    midi->loop = loop != 0;
    midi->sample_abs = 0;
    midi->sample_count = midi->sample_nonzero = midi->sample_peak = 0;
    atomic_store(&midi->stop, false);
    atomic_store(&midi->finished, false);
    midi->preempted = false;
    esp_midi_sound_resume(midi->engine);
    state->midi_active = midi;
    midi->started = true;
    if (xTaskCreatePinnedToCore(play_task, "j2me-midi", 16384, midi, 4, NULL, 1) != pdPASS) {
        midi->started = false;
        state->midi_active = NULL;
        state->midi_port->pcm_close(state->midi_port->ud);
        close_engine(midi);
        return -1;
    }
    return 0;
}

static void midi_seek(void *ud, void *handle, nj_u4 ms)
{
    korvo_midi *midi = handle;
    bool playing = midi->started && !atomic_load(&midi->finished);
    midi_stop(ud, handle);
    /* The SDK has no indexed seek. Rebuild and discard PCM until the target. */
    midi->skip_frames = (uint64_t)ms * MIDI_RATE / 1000u;
    atomic_store(&midi->frames, midi->skip_frames > UINT32_MAX ? UINT32_MAX : midi->skip_frames);
    atomic_store(&midi->finished, false);
    midi->restart = false;
    if (playing) midi_start(ud, handle, midi->loop);
}

static nj_u4 midi_time(void *ud, void *handle)
{
    (void)ud;
    korvo_midi *midi = handle;
    return (uint64_t)atomic_load(&midi->frames) * 1000u / MIDI_RATE;
}

static void midi_volume(void *ud, void *handle, int volume)
{
    (void)ud;
    korvo_midi *midi = handle;
    atomic_store(&midi->volume, volume < 0 ? 0 : volume > 100 ? 100 : volume);
}

static int midi_finished(void *ud, void *handle)
{
    korvo_port_state *state = ud;
    korvo_midi *midi = handle;
    /* Let the buffered tail drain before the next loop or end-of-media. */
    if (midi->preempted) return -1;
    if (!atomic_load(&midi->finished) ||
        esp_timer_get_time() - midi->finished_at < AUDIO_DRAIN_MS * 1000) return 0;
    /* The task publishes finished after its last engine access, then signals
     * done. Join before freeing it, without touching the output device: a
     * sampled Player may already have claimed PCM during this drain wait. */
    if (midi->started) {
        xSemaphoreTake(midi->done, portMAX_DELAY);
        midi->started = false;
    }
    close_engine(midi);
    if (state->midi_active == midi) state->midi_active = NULL;
    return 1;
}

void korvo_midi_port(nj_port *port, korvo_port_state *state)
{
    state->midi_port = port;
    port->midi_open = midi_open;
    port->midi_close = midi_close;
    port->midi_start = midi_start;
    port->midi_stop = midi_stop;
    port->midi_seek = midi_seek;
    port->midi_time = midi_time;
    port->midi_volume = midi_volume;
    port->midi_finished = midi_finished;
}

void korvo_midi_shutdown(korvo_port_state *state)
{
    while (state->midi_players) midi_close(state, state->midi_players);
}
