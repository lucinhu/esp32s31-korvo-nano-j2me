#include "launcher_ui.h"
#include "korvo_board.h"
#include <stdio.h>
#include <string.h>

#define SURFACE 0x10e5
#define CARD 0x1947
#define BORDER 0x2a29
#define INK 0x0863

const launcher_button launcher_buttons[] = {
    {"SOFT L", NJ_KEY_SOFT_LEFT, 12, 62, 176, 44},
    {"SOFT R", NJ_KEY_SOFT_RIGHT, 612, 62, 176, 44},
    {"UP", NJ_KEY_UP, 72, 156, 56, 56},
    {"LEFT", NJ_KEY_LEFT, 12, 216, 56, 56},
    {"FIRE", NJ_KEY_FIRE, 72, 216, 56, 56},
    {"RIGHT", NJ_KEY_RIGHT, 132, 216, 56, 56},
    {"DOWN", NJ_KEY_DOWN, 72, 276, 56, 56},
    {"A", NJ_KEY_GAME_A, 12, 358, 40, 44},
    {"B", NJ_KEY_GAME_B, 56, 358, 40, 44},
    {"C", NJ_KEY_GAME_C, 100, 358, 40, 44},
    {"D", NJ_KEY_GAME_D, 144, 358, 44, 44},
    {"1", NJ_KEY_1, 612, 128, 56, 56},
    {"2", NJ_KEY_2, 672, 128, 56, 56},
    {"3", NJ_KEY_3, 732, 128, 56, 56},
    {"4", NJ_KEY_4, 612, 190, 56, 56},
    {"5", NJ_KEY_5, 672, 190, 56, 56},
    {"6", NJ_KEY_6, 732, 190, 56, 56},
    {"7", NJ_KEY_7, 612, 252, 56, 56},
    {"8", NJ_KEY_8, 672, 252, 56, 56},
    {"9", NJ_KEY_9, 732, 252, 56, 56},
    {"*", NJ_KEY_STAR, 612, 314, 56, 56},
    {"0", NJ_KEY_0, 672, 314, 56, 56},
    {"#", NJ_KEY_POUND, 732, 314, 56, 56},
    {"CLEAR", NJ_KEY_CLEAR, 612, 386, 176, 44},
};
const unsigned launcher_button_count = sizeof launcher_buttons / sizeof launcher_buttons[0];

/* A small radius keeps the touch target rectangular, with quieter corners. */
static void rounded(int x, int y, int w, int h, uint16_t color)
{
    korvo_fill(x + 4, y, w - 8, h, color);
    korvo_fill(x, y + 4, w, h - 8, color);
    korvo_fill(x + 1, y + 2, w - 2, h - 4, color);
    korvo_fill(x + 2, y + 1, w - 4, h - 2, color);
}

static void text_center(const char *text, int x, int y, int w, int h, int scale, uint16_t color)
{
    korvo_text(x + (w - (int)strlen(text) * 6 * scale) / 2,
               y + (h - 7 * scale) / 2, text, scale, color);
}

static void control(const char *label, int x, int y, int w, int h, bool active)
{
    rounded(x, y, w, h, active ? UI_ACCENT : BORDER);
    if (!active) rounded(x + 1, y + 1, w - 2, h - 2, CARD);
    text_center(label, x, y, w, h, 2, active ? INK : UI_TEXT);
}

void launcher_ui_fps(bool enabled)
{
    control(enabled ? "FPS ON" : "FPS OFF", LAUNCHER_FPS_X, LAUNCHER_FPS_Y,
            LAUNCHER_FPS_W, LAUNCHER_FPS_H, enabled);
}

void launcher_ui_scale(int width, int height, bool pixel_perfect)
{
    char label[24];
    launcher_rect view = launcher_game_rect(width, height, pixel_perfect);
    if (pixel_perfect) snprintf(label, sizeof label, "PIXEL %dX", view.w / width);
    else snprintf(label, sizeof label, "MAX SIZE");
    control(label, LAUNCHER_SCALE_X, LAUNCHER_SCALE_Y,
            LAUNCHER_SCALE_W, LAUNCHER_SCALE_H, pixel_perfect);
}

void launcher_ui_key(unsigned index, bool pressed)
{
    if (index >= launcher_button_count) return;
    const launcher_button *b = &launcher_buttons[index];
    uint16_t fg = pressed ? INK : UI_TEXT;
    bool direction = b->key == NJ_KEY_UP || b->key == NJ_KEY_DOWN ||
        b->key == NJ_KEY_LEFT || b->key == NJ_KEY_RIGHT;
    if (!direction && b->key != NJ_KEY_FIRE) {
        control(b->label, b->x, b->y, b->w, b->h, pressed);
        return;
    }
    rounded(b->x, b->y, b->w, b->h, pressed ? UI_ACCENT : BORDER);
    if (!pressed) rounded(b->x + 1, b->y + 1, b->w - 2, b->h - 2, CARD);
    int cx = b->x + b->w / 2, cy = b->y + b->h / 2;
    if (b->key == NJ_KEY_FIRE) {
        rounded(cx - 10, cy - 10, 20, 20, pressed ? INK : UI_ACCENT);
        return;
    }
    for (int i = 0; i <= 20; ++i) {
        bool reverse = b->key == NJ_KEY_DOWN || b->key == NJ_KEY_RIGHT;
        int half = (reverse ? 20 - i : i) / 2;
        if (b->key == NJ_KEY_UP || b->key == NJ_KEY_DOWN)
            korvo_fill(cx - half, cy - 10 + i, half * 2 + 1, 1, fg);
        else korvo_fill(cx - 10 + i, cy - half, 1, half * 2 + 1, fg);
    }
}

void launcher_ui_volume(int volume)
{
    control("VOL-", 12, 426, 84, 40, false);
    control("VOL+", 104, 426, 84, 40, false);
    /* Volume is legible between the controls and the direction pad. */
    char label[24]; snprintf(label, sizeof label, "VOLUME %d", volume);
    korvo_fill(12, 408, 176, 14, SURFACE);
    text_center(label, 12, 408, 176, 14, 1, UI_MUTED);
}

void launcher_ui_game(int width, int height, bool fps, bool pixel_perfect)
{
    korvo_fill(0, 0, LCD_W, LCD_H, UI_BG);
    korvo_fill(0, 0, LCD_W, 44, SURFACE);
    korvo_fill(0, 44, 200, LCD_H - 44, SURFACE);
    korvo_fill(600, 44, 200, LCD_H - 44, SURFACE);
    control("MENU", LAUNCHER_MENU_X, LAUNCHER_MENU_Y, LAUNCHER_MENU_W, LAUNCHER_MENU_H, false);
    launcher_ui_fps(fps);
    launcher_ui_scale(width, height, pixel_perfect);
    for (unsigned i = 0; i < launcher_button_count; ++i) launcher_ui_key(i, false);
    launcher_ui_volume(korvo_get_volume());
    launcher_rect view = launcher_game_rect(width, height, pixel_perfect);
    if (view.w && view.h) {
        rounded(view.x - 2, view.y - 2, view.w + 4, view.h + 4, BORDER);
        korvo_fill(view.x, view.y, view.w, view.h, 0);
    }
}

void launcher_ui_menu_begin(const char *storage, unsigned count, bool fps)
{
    korvo_fill(0, 0, LCD_W, LCD_H, UI_BG);
    korvo_fill(0, 0, LCD_W, 76, SURFACE);
    korvo_fill(20, 18, 4, 28, UI_ACCENT);
    korvo_text(36, 16, "NANO J2ME", 3, UI_TEXT);
    char status[80]; snprintf(status, sizeof status, "%u GAMES  /  %s", count, storage);
    korvo_text(36, 52, status, 2, UI_MUTED);
    launcher_ui_fps(fps);
}

void launcher_ui_menu_row(unsigned row, unsigned index, const char *title)
{
    int y = LAUNCHER_ROW_Y + (int)row * LAUNCHER_ROW_STEP;
    rounded(20, y, 760, LAUNCHER_ROW_H, CARD);
    char number[8]; snprintf(number, sizeof number, "%02u", index);
    text_center(number, 28, y, 44, LAUNCHER_ROW_H, 2, UI_ACCENT);
    korvo_filename_clipped(84, y + 10, title, 632);
    /* Play triangle, independent of the launcher's bitmap font. */
    for (int col = 0; col < 9; ++col)
        korvo_fill(746 + col, y + 12 + col, 1, 19 - col * 2, UI_MUTED);
}

void launcher_ui_menu_end(unsigned page, unsigned pages, int width, int height, bool pixel_perfect)
{
    control("PREV", 20, LAUNCHER_NAV_Y, 140, LAUNCHER_NAV_H, false);
    control("NEXT", 172, LAUNCHER_NAV_Y, 140, LAUNCHER_NAV_H, false);
    control("RES", 324, LAUNCHER_NAV_Y, 140, LAUNCHER_NAV_H, false);
    control("RESCAN", 476, LAUNCHER_NAV_Y, 140, LAUNCHER_NAV_H, false);
    control("DEMO", 628, LAUNCHER_NAV_Y, 152, LAUNCHER_NAV_H, true);
    char text[80]; snprintf(text, sizeof text, "PAGE %u/%u", page + 1, pages);
    korvo_text(20, 458, text, 2, UI_MUTED);
    snprintf(text, sizeof text, "%d X %d", width, height);
    korvo_text(350, 458, text, 2, UI_MUTED);
    control(pixel_perfect ? "PIXEL ON" : "MAX SIZE", LAUNCHER_MENU_SCALE_X, LAUNCHER_MENU_SCALE_Y,
            LAUNCHER_MENU_SCALE_W, LAUNCHER_MENU_SCALE_H, pixel_perfect);
}
