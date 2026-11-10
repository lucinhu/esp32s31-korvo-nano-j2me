#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t phase;
    uint32_t source_rate;
    int16_t previous[2];
    bool primed;
} audio_resampler_t;

void audio_resampler_reset(audio_resampler_t *resampler, uint32_t source_rate);
void audio_resampler_push(audio_resampler_t *resampler, const int16_t *sample);
bool audio_resampler_next(audio_resampler_t *resampler, const int16_t *sample, int16_t *output);
