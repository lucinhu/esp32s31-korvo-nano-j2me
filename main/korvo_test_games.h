#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *path, *title;
    const uint8_t *start, *end;
} korvo_test_game;

const korvo_test_game *korvo_test_games(size_t *count);
const korvo_test_game *korvo_test_game_find(const char *path);
bool korvo_test_game_handle(const void *handle);
