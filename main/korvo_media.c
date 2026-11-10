#include "korvo_media.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#if CONFIG_IDF_TARGET_ESP32S31 && CONFIG_ESP_AUDIO_CODEC_S31_USE_ASM
#error "Audio decoding runs on VM Core 0; disable ESP_AUDIO_CODEC_S31_USE_ASM (PIE requires Core 1)"
#endif

/* The public MMAPI hook requires a complete PCM buffer in the Java heap.
 * Keep room for game objects and bound work during both decoding passes. */
#define PCM_MAX_BYTES (NJ_MAX_HEAP / 2u)
#define FRAME_MAX_BYTES (64u * 1024u)

static esp_audio_simple_dec_type_t audio_type(const nj_u1 *in, nj_u4 size)
{
    if (size >= 9 && !memcmp(in, "#!AMR-WB\n", 9)) return ESP_AUDIO_SIMPLE_DEC_TYPE_AMRWB;
    if (size >= 6 && !memcmp(in, "#!AMR\n", 6)) return ESP_AUDIO_SIMPLE_DEC_TYPE_AMRNB;
    if (size >= 4 && !memcmp(in, "fLaC", 4)) return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    if (size >= 12 && !memcmp(in, "RIFF", 4) && !memcmp(in + 8, "WAVE", 4))
        return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    if (size >= 12 && !memcmp(in + 4, "ftyp", 4)) return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    if (size >= 10 && !memcmp(in, "ID3", 3)) return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    if (size >= 4 && in[0] == 0xff && (in[1] & 0xf6) == 0xf0)
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    if (size >= 4 && in[0] == 0xff && (in[1] & 0xe6) == 0xe2 &&
        (in[1] & 0x18) != 0x08 && (in[2] & 0x0c) != 0x0c && (in[2] & 0xf0) != 0xf0)
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

static nj_status audio_decode(void *ud, const nj_u1 *in, nj_u4 in_len,
    nj_u1 *out, nj_u4 out_cap, nj_u4 *out_len, nj_u4 *sample_rate, nj_u4 *channels)
{
    (void)ud;
    if (!out_len || !sample_rate || !channels) return NJ_ERR;
    *out_len = *sample_rate = *channels = 0;
    if (!in || !in_len) return NJ_ERR_FORMAT;
    esp_audio_simple_dec_type_t type = audio_type(in, in_len);
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) return NJ_ERR_UNSUPPORTED;
    esp_audio_simple_dec_cfg_t config = { .dec_type = type };
    esp_audio_simple_dec_handle_t decoder = NULL;
    esp_audio_err_t result = esp_audio_simple_dec_open(&config, &decoder);
    if (result != ESP_AUDIO_ERR_OK)
        return result == ESP_AUDIO_ERR_MEM_LACK ? NJ_ERR_NOMEM : NJ_ERR_UNSUPPORTED;
    uint32_t capacity = 4096;
    uint8_t *scratch = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    nj_status status = NJ_ERR_NOMEM;
    nj_u4 offset = 0, total = 0, rate = 0, chan = 0;
    unsigned no_progress = 0;
    if (!scratch) goto done;
    status = NJ_ERR_FORMAT;
    while (offset < in_len) {
        /* Feed bounded chunks; eos is set on the final chunk in both passes. */
        nj_u4 chunk = in_len - offset > 4096 ? 4096 : in_len - offset;
        esp_audio_simple_dec_raw_t raw = { .buffer = (uint8_t *)in + offset,
            .len = chunk, .eos = offset + chunk == in_len };
        while (raw.len) {
            raw.consumed = 0;
            esp_audio_simple_dec_out_t frame = { .buffer = scratch, .len = capacity };
            result = esp_audio_simple_dec_process(decoder, &raw, &frame);
            if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                if (frame.needed_size <= capacity || frame.needed_size > FRAME_MAX_BYTES) goto done;
                uint8_t *next = heap_caps_realloc(scratch, frame.needed_size,
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!next) { status = NJ_ERR_NOMEM; goto done; }
                scratch = next;
                capacity = frame.needed_size;
                continue; /* Per the SDK contract, retry the same input. */
            }
            if (result != ESP_AUDIO_ERR_OK || raw.consumed > raw.len || frame.decoded_size > capacity)
                goto done;
            if (frame.decoded_size) {
                esp_audio_simple_dec_info_t info = {0};
                if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK ||
                    info.bits_per_sample != 16 || (info.channel != 1 && info.channel != 2) ||
                    info.sample_rate < 4000 || info.sample_rate > 96000 ||
                    frame.decoded_size % (2u * info.channel)) goto done;
                if (total && (rate != info.sample_rate || chan != info.channel)) goto done;
                rate = info.sample_rate;
                chan = info.channel;
                if (frame.decoded_size > PCM_MAX_BYTES - total) { status = NJ_ERR_NOMEM; goto done; }
                if (out) {
                    if (total > out_cap || frame.decoded_size > out_cap - total) goto done;
                    memcpy(out + total, scratch, frame.decoded_size);
                }
                total += frame.decoded_size;
            }
            /* Decoders may emit buffered PCM without consuming new input.
             * Bound such iterations, and reject a parser that makes no progress. */
            if (!raw.consumed) {
                if (!frame.decoded_size || ++no_progress > 32) goto done;
            } else no_progress = 0;
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            offset += raw.consumed;
        }
        /* Lengthy clips must let the idle task service the watchdog. */
        vTaskDelay(1);
    }
    if (total) {
        *out_len = total;
        *sample_rate = rate;
        *channels = chan;
        status = NJ_OK;
    }
done:
    heap_caps_free(scratch);
    esp_audio_simple_dec_close(decoder);
    return status;
}

void korvo_media_port(nj_port *port)
{
    /* Port initialization is serialized on the launcher task. The SDK's
     * process-wide registry lives for the application lifetime; each decode
     * opens and closes its own instance, including the sizing pass. */
    static bool ready;
    if (!ready) {
        esp_audio_err_t frames = esp_audio_dec_register_default();
        esp_audio_err_t containers = esp_audio_simple_dec_register_default();
        ready = frames == ESP_AUDIO_ERR_OK && containers == ESP_AUDIO_ERR_OK;
        if (!ready) {
            esp_audio_simple_dec_unregister_default();
            esp_audio_dec_unregister_default();
            ESP_LOGE("j2me-media", "Audio decoder registration failed: %d/%d", frames, containers);
        }
    }
    port->install_codecs = korvo_image_install;
    korvo_inflate_port(port);
    if (ready) port->audio_decode = audio_decode;
}
