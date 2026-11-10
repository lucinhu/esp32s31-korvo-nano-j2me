#pragma once
#include "launcher_layout.h"
#include <stdbool.h>
#include <stdint.h>

#define UI_BG 0x0863
#define UI_TEXT 0xe77e
#define UI_MUTED 0x8d36
#define UI_ACCENT 0x4eb8

typedef struct { const char *label; int key, x, y, w, h; } launcher_button;
extern const launcher_button launcher_buttons[];
extern const unsigned launcher_button_count;

void launcher_ui_game(int width, int height, bool fps, bool pixel_perfect);
void launcher_ui_key(unsigned index, bool pressed);
void launcher_ui_fps(bool enabled);
void launcher_ui_scale(int width, int height, bool pixel_perfect);
void launcher_ui_volume(int volume);
void launcher_ui_menu_begin(const char *storage, unsigned count, bool fps);
void launcher_ui_menu_row(unsigned row, unsigned index, const char *title);
void launcher_ui_menu_end(unsigned page, unsigned pages, int width, int height, bool pixel_perfect);
