#pragma once

#define LAUNCHER_VIEW_W 392
#define LAUNCHER_VIEW_H 432
#define LAUNCHER_VIEW_TOP 44
#define LAUNCHER_FPS_X 672
#define LAUNCHER_FPS_Y 6
#define LAUNCHER_FPS_W 116
#define LAUNCHER_FPS_H 32
#define LAUNCHER_MENU_X 12
#define LAUNCHER_MENU_Y 6
#define LAUNCHER_MENU_W 112
#define LAUNCHER_MENU_H 32
#define LAUNCHER_ROW_Y 90
#define LAUNCHER_ROW_STEP 48
#define LAUNCHER_ROW_H 42
#define LAUNCHER_NAV_Y 394
#define LAUNCHER_NAV_H 48

typedef struct { int x, y, w, h; } launcher_rect;

static inline launcher_rect launcher_game_rect(int width, int height)
{
    if (width <= 0 || height <= 0) return (launcher_rect){0};
    int scale_x = LAUNCHER_VIEW_W / width;
    int scale_y = LAUNCHER_VIEW_H / height;
    int scale = scale_x < scale_y ? scale_x : scale_y;
    /* The launcher's supported 64..320 dimensions always fit at 1x. */
    if (scale < 1) return (launcher_rect){0};
    int w = width * scale, h = height * scale;
    return (launcher_rect){(800 - w) / 2,
        LAUNCHER_VIEW_TOP + (LAUNCHER_VIEW_H - h) / 2, w, h};
}
