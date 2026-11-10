#pragma once
#include <stdint.h>

/* Validate SMF 0/1 and return an owned PSRAM copy. Format 1 is merged into
 * format 0 because esp_midi 1.1.0's high-level player reads only track zero.
 * Release the result with heap_caps_free(). Failure leaves outputs empty. */
int korvo_midi_file_prepare(const uint8_t *data, uint32_t size,
                           uint8_t **output, uint32_t *output_size);
