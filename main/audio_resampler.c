#include "audio_resampler.h"

#include <string.h>

void audio_resampler_reset(audio_resampler_t *resampler, uint32_t source_rate)
{
    memset(resampler, 0, sizeof(*resampler));
    resampler->source_rate = source_rate;
}

void audio_resampler_push(audio_resampler_t *resampler, const int16_t *sample)
{
    resampler->previous[0] = sample[0];
    resampler->previous[1] = sample[1];
    if (resampler->primed) resampler->phase -= 48000;
    resampler->primed = true;
}

bool audio_resampler_next(audio_resampler_t *resampler, const int16_t *sample, int16_t *output)
{
    if (!resampler->primed || resampler->phase >= 48000) return false;
    /* At an exact sample boundary interpolation returns the previous frame.
     * This covers every frame at 48 kHz and regular boundaries at other rates. */
    if (resampler->phase == 0) {
        output[0] = resampler->previous[0];
        output[1] = resampler->previous[1];
        resampler->phase += resampler->source_rate;
        return true;
    }
    for (unsigned channel = 0; channel < 2; ++channel) {
        int32_t delta = (int32_t)sample[channel] - resampler->previous[channel];
        /* |delta| <= 65535, phase < 48000: the product fits uint32_t.
         * Preserve signed division's truncation with cheaper RV32 arithmetic. */
        uint32_t magnitude = delta < 0 ? (uint32_t)-delta : (uint32_t)delta;
        int32_t step = magnitude * resampler->phase / 48000;
        output[channel] = resampler->previous[channel] + (delta < 0 ? -step : step);
    }
    resampler->phase += resampler->source_rate;
    return true;
}
