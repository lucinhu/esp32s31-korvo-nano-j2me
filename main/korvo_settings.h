#pragma once
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t version, width, height, volume;
    uint32_t fps, pixel_perfect, jit_mode, jit_internal;
} korvo_settings;

esp_err_t korvo_settings_load(korvo_settings *settings);
esp_err_t korvo_settings_save(const korvo_settings *settings);
