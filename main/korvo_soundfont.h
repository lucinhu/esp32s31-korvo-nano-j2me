#pragma once

/* esp_midi headers leave structure packing enabled; contain that setting. */
#pragma pack(push)
#include "esp_midi_sound.h"
#include "esp_midi_sf_parse.h"
#pragma pack(pop)

#define KORVO_MIDI_RATE 22050u

esp_midi_lib_cfg_t korvo_soundfont_config(void);
/* The owner slot must outlive the loader; deletion clears it. */
esp_midi_lib_cfg_t korvo_soundfont_config_for_owner(esp_midi_lib_loader_handle_t *owner);

/* Bind at each synthesis task's entry, including resume with existing voices. */
void korvo_soundfont_bind(esp_midi_lib_loader_handle_t handle);

/* Release only idle PCM from the current synthesis task's loader. */
void korvo_soundfont_trim(void);
