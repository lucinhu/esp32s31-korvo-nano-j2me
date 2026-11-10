#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "korvo_board.h"
#include "korvo_port.h"
#include "korvo_jit.h"
#include "korvo_test_games.h"
#include "korvo_font.h"
#include "launcher_ui.h"
#include "korvo_settings.h"
#include "esp_system.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define PATH_LEN 384
#define MAX_GAMES 64
#define PAGE_SIZE 6
#define BG UI_BG
#define FG UI_TEXT
#define ACCENT UI_ACCENT

static const char *TAG = "korvo-j2me";
static char games[MAX_GAMES][PATH_LEN];
static char game_titles[MAX_GAMES][192];
static unsigned game_count = 1, page;
static int screen_width = 240, screen_height = 320;
static bool running, pumping, touch_latched, quit_requested;
static int pending_game = -1, active_game;
static bool pixel_perfect;
static korvo_settings preferences;
static unsigned run_frames;
static bool benchmark;
static bool fps_enabled;
static int64_t fps_sample_time;
static unsigned fps_sample_frames, fps_tenths;
static char scripted_keys[768];
static QueueHandle_t events;
static uint32_t touch_keys;
static bool pointer_down;
static int pointer_x, pointer_y;
static int64_t last_touch, last_stats;
static korvo_port_state port_state;
static char command[1024];
static char storage_files[96];
extern const unsigned char korvo_font_start[] asm("_binary_korvo_font_start");
extern const unsigned char korvo_font_end[] asm("_binary_korvo_font_end");
static unsigned command_len;

static void save_preferences(void)
{
    preferences.width = screen_width; preferences.height = screen_height;
    preferences.volume = korvo_get_volume(); preferences.fps = fps_enabled;
    preferences.pixel_perfect = pixel_perfect;
    const char *jit = korvo_jit_mode_name();
    preferences.jit_mode = !strcmp(jit, "probe") ? 2 : !strcmp(jit, "on") ? 1 : 0;
    preferences.jit_internal = !strcmp(korvo_jit_memory_name(), "internal");
    esp_err_t rc = korvo_settings_save(&preferences);
    if (rc != ESP_OK) ESP_LOGW(TAG, "Settings save failed: %s", esp_err_to_name(rc));
}

static void report_preferences(void)
{
    printf("SETTINGS res=%dx%d volume=%d fps=%s scale=%s jit=%s memory=%s\n",
           screen_width, screen_height, korvo_get_volume(), fps_enabled ? "on" : "off",
           pixel_perfect ? "pixel" : "fit", korvo_jit_mode_name(), korvo_jit_memory_name());
}

static bool inside(int x, int y, int bx, int by, int w, int h)
{ return x >= bx && y >= by && x < bx + w && y < by + h; }

void launcher_view(int width, int height, int *x, int *y, int *w, int *h)
{
    launcher_rect view = launcher_game_rect(width, height, pixel_perfect);
    *x = view.x; *y = view.y; *w = view.w; *h = view.h;
}

static void draw_fps_button(void) { launcher_ui_fps(fps_enabled); }

bool launcher_fps_bounds(int *x, int *y, int *w, int *h)
{
    if (!fps_enabled || !running || !port_state.pixels) return false;
    int vx, vy, vw, vh;
    launcher_view(screen_width, screen_height, &vx, &vy, &vw, &vh);
    *x = vx + 4; *y = vy + 4; *w = 124; *h = 24;
    return true;
}

static void reset_fps_sample(void)
{
    fps_sample_time = esp_timer_get_time();
    fps_sample_frames = port_state.updated_frames;
    fps_tenths = 0;
}

void launcher_fps_present(bool frame_changed)
{
    int x, y, w, h;
    if (!launcher_fps_bounds(&x, &y, &w, &h)) return;
    int64_t now = esp_timer_get_time(), elapsed = now - fps_sample_time;
    bool refresh = elapsed >= 500000;
    if (refresh) {
        uint64_t rate = (uint64_t)(port_state.updated_frames - fps_sample_frames) * 10000000 / elapsed;
        fps_tenths = rate > 9999 ? 9999 : rate;
        fps_sample_time = now; fps_sample_frames = port_state.updated_frames;
    }
    if (!frame_changed && !refresh) return;
    char text[16];
    snprintf(text, sizeof(text), "FPS %u.%u", fps_tenths / 10, fps_tenths % 10);
    korvo_fill(x, y, w, h, BG);
    korvo_text(x + 4, y + 5, text, 2, 0xffe0);
    korvo_flush_rect(x, y, w, h);
}

static const char *basename_of(const char *path)
{ const char *p = strrchr(path, '/'); return p ? p + 1 : path; }

static void draw_game(int index)
{
    launcher_ui_game(screen_width, screen_height, fps_enabled, pixel_perfect);
    const char *title = index ? (game_titles[index][0] ? game_titles[index] : basename_of(games[index]))
                              : "HARDWARE DEMO";
    korvo_filename_clipped(148, 10, title, 350);
    korvo_flush();
}

static void draw_menu(void)
{
    launcher_ui_menu_begin(korvo_sd_ready() ? "SD CARD" : korvo_storage_root() ? "FLASH" : "NO STORAGE",
                           game_count, fps_enabled);
    for (unsigned i = 0; i < PAGE_SIZE && page * PAGE_SIZE + i < game_count; ++i) {
        unsigned n = page * PAGE_SIZE + i;
        const char *title = n ? (game_titles[n][0] ? game_titles[n] : basename_of(games[n]))
                             : "BUILT-IN HARDWARE DEMO";
        launcher_ui_menu_row(i, n, title);
    }
    launcher_ui_menu_end(page, (game_count + PAGE_SIZE - 1) / PAGE_SIZE, screen_width, screen_height, pixel_perfect);
    korvo_flush();
}

static void change_volume(int volume)
{
    korvo_volume(volume);
    save_preferences();
    if (running) {
        launcher_ui_volume(korvo_get_volume());
        korvo_flush_rect(12, 408, 176, 58);
    } else draw_menu();
}

static void set_fps(bool enabled)
{
    if (fps_enabled == enabled) return;
    fps_enabled = enabled;
    save_preferences();
    reset_fps_sample();
    if (running) {
        /* Restore the pixels covered by the overlay immediately on disable. */
        korvo_present(&port_state);
        draw_fps_button(); korvo_flush_rect(LAUNCHER_FPS_X, LAUNCHER_FPS_Y, LAUNCHER_FPS_W, LAUNCHER_FPS_H);
        launcher_fps_present(true);
    } else draw_menu();
    printf("FPS_STATE %s\n", fps_enabled ? "on" : "off"); fflush(stdout);
}

static void set_scale(bool pixel)
{
    pixel_perfect = pixel;
    save_preferences();
    if (running && port_state.pixels) {
        korvo_display_begin();
        draw_game(active_game);
        korvo_port_update_view(&port_state);
        korvo_present(&port_state);
        launcher_fps_present(true);
        korvo_display_end();
    } else draw_menu();
    int x, y, w, h;
    launcher_view(screen_width, screen_height, &x, &y, &w, &h);
    printf("SCALE_STATE %s view=%d,%d,%d,%d\n", pixel ? "pixel" : "fit", x,y,w,h);
}

static void scan_dir(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *entry;
    while (game_count < MAX_GAMES && (entry = readdir(dir))) {
        size_t n = strlen(entry->d_name);
        if (n < 5 || strcasecmp(entry->d_name + n - 4, ".jar")) continue;
        int len = snprintf(games[game_count], PATH_LEN, "%s/%s", path, entry->d_name);
        struct stat st;
        if (len > 0 && len < PATH_LEN && stat(games[game_count], &st) == 0 && S_ISREG(st.st_mode)) ++game_count;
    }
    closedir(dir);
}
static void scan_games(void)
{
    game_count = 1; page = 0;
    memset(game_titles, 0, sizeof(game_titles));
    if (korvo_sd_ready()) { scan_dir("/sdcard"); scan_dir("/sdcard/roms/j2me"); scan_dir("/sdcard/j2me"); }
    scan_dir("/flash/games"); scan_dir("/flash/j2me");
    size_t test_count;
    const korvo_test_game *test_games = korvo_test_games(&test_count);
    for (size_t i=0;i<test_count && game_count<MAX_GAMES;++i) {
        snprintf(games[game_count],PATH_LEN,"%s",test_games[i].path);
        snprintf(game_titles[game_count],sizeof(game_titles[game_count]),"%s",test_games[i].title);
        ++game_count;
    }
    FILE *titles = fopen("/flash/games/titles.tsv", "r");
    if (titles) {
        char line[320];
        while (fgets(line, sizeof(line), titles)) {
            char *title = strchr(line, '\t');
            if (!title) continue;
            *title++ = 0; title[strcspn(title, "\r\n")] = 0;
            for (unsigned i = 1; i < game_count; ++i)
                if (!strncmp(games[i], "/flash/games/", 13) && !strcmp(basename_of(games[i]), line))
                    snprintf(game_titles[i], sizeof(game_titles[i]), "%s", title);
        }
        fclose(titles);
    }
    ESP_LOGI(TAG, "GAMES count=%u (0 is built-in demo)", game_count);
}
static void list_games(void)
{
    printf("GAMES_BEGIN %u\n0 BUILT-IN DEMO\n", game_count);
    for (unsigned i = 1; i < game_count; ++i) printf("%u %s %s\n", i, games[i], game_titles[i]);
    puts("GAMES_END");
}

int launcher_push(const nj_event *event) { return xQueueSend(events, event, 0) == pdTRUE; }
int launcher_event(nj_event *event)
{
    if (quit_requested) { quit_requested = false; *event = (nj_event){ .type = NJ_EV_QUIT }; return 1; }
    return xQueueReceive(events, event, 0) == pdTRUE;
}
static void key_event(int key, bool down)
{
    nj_event e = { .type = down ? NJ_EV_KEY_DOWN : NJ_EV_KEY_UP, .key = key };
    if (!launcher_push(&e)) ESP_LOGW(TAG, "Input queue full");
    if (running) for (unsigned i = 0; i < launcher_button_count; ++i) {
        const launcher_button *b = &launcher_buttons[i];
        if (b->key == key) {
            launcher_ui_key(i, down);
            korvo_flush_rect(b->x, b->y, b->w, b->h);
        }
    }
}
static void pointer_event(int type, int x, int y)
{ nj_event e = { .type = type, .x = x, .y = y }; launcher_push(&e); }

static void cycle_resolution(void)
{
    if (screen_width == 240) { screen_width = 176; screen_height = 208; }
    else if (screen_width == 176) { screen_width = 128; screen_height = 160; }
    else if (screen_width == 128) { screen_width = 320; screen_height = 240; }
    else { screen_width = 240; screen_height = 320; }
    save_preferences();
}

static void touch_poll(void)
{
    uint16_t xs[5], ys[5];
    int count = korvo_touch(xs, ys, 5);
    bool fresh = !touch_latched;
    touch_latched = count > 0;
    for (int n = 0; !benchmark && fresh && n < count; ++n)
        if (inside(xs[n], ys[n], LAUNCHER_FPS_X, LAUNCHER_FPS_Y, LAUNCHER_FPS_W, LAUNCHER_FPS_H)) { set_fps(!fps_enabled); break; }
        else if (inside(xs[n], ys[n], running ? LAUNCHER_SCALE_X : LAUNCHER_MENU_SCALE_X,
                        running ? LAUNCHER_SCALE_Y : LAUNCHER_MENU_SCALE_Y,
                        running ? LAUNCHER_SCALE_W : LAUNCHER_MENU_SCALE_W,
                        running ? LAUNCHER_SCALE_H : LAUNCHER_MENU_SCALE_H)) { set_scale(!pixel_perfect); break; }
    if (!running) {
        if (!fresh || !count) return;
        int x = xs[0], y = ys[0];
        for (unsigned i = 0; i < PAGE_SIZE; ++i) {
            unsigned index = page * PAGE_SIZE + i;
            if (index < game_count && inside(x, y, 20, LAUNCHER_ROW_Y + i * LAUNCHER_ROW_STEP, 760, LAUNCHER_ROW_H)) pending_game = index;
        }
        if (y >= LAUNCHER_NAV_Y && y < LAUNCHER_NAV_Y + LAUNCHER_NAV_H) {
            if (x >= 20 && x < 160 && page) --page;
            else if (x >= 172 && x < 312 && (page + 1) * PAGE_SIZE < game_count) ++page;
            else if (x >= 324 && x < 464) cycle_resolution();
            else if (x >= 476 && x < 616) scan_games();
            else if (x >= 628 && x < 780) pending_game = 0;
            draw_menu();
        }
        return;
    }
    /* Fixed-input benchmark runs must not pick up incidental touch events.
     * Keep MENU available so a running test can still be interrupted. */
    if (benchmark) {
        for (int n = 0; fresh && n < count; ++n)
            if (inside(xs[n], ys[n], LAUNCHER_MENU_X, LAUNCHER_MENU_Y, LAUNCHER_MENU_W, LAUNCHER_MENU_H)) quit_requested = true;
        return;
    }
    uint32_t keys = 0;
    bool have_pointer = false;
    int px = 0, py = 0, vx, vy, vw, vh;
    launcher_view(screen_width, screen_height, &vx, &vy, &vw, &vh);
    for (int n = 0; n < count; ++n) {
        int x = xs[n], y = ys[n];
        if (fresh && inside(x, y, LAUNCHER_MENU_X, LAUNCHER_MENU_Y, LAUNCHER_MENU_W, LAUNCHER_MENU_H)) quit_requested = true;
        if (fresh && inside(x, y, 12, 426, 84, 40)) change_volume(korvo_get_volume() - 10);
        if (fresh && inside(x, y, 104, 426, 84, 40)) change_volume(korvo_get_volume() + 10);
        bool is_key = false;
        for (unsigned i = 0; i < launcher_button_count; ++i) {
            const launcher_button *b = &launcher_buttons[i];
            if (inside(x, y, b->x, b->y, b->w, b->h)) { keys |= 1u << b->key; is_key = true; }
        }
        if (!is_key && !have_pointer && inside(x, y, vx, vy, vw, vh)) {
            have_pointer = true; px = (x - vx) * screen_width / vw; py = (y - vy) * screen_height / vh;
        }
    }
    uint32_t changed = keys ^ touch_keys;
    for (int key = 1; key < NJ_KEY_COUNT; ++key) if (changed & (1u << key)) key_event(key, (keys & (1u << key)) != 0);
    touch_keys = keys;
    if (have_pointer) {
        if (!pointer_down || px != pointer_x || py != pointer_y)
            pointer_event(pointer_down ? NJ_EV_POINTER_MOVE : NJ_EV_POINTER_DOWN, px, py);
        pointer_x = px; pointer_y = py;
    } else if (pointer_down) pointer_event(NJ_EV_POINTER_UP, pointer_x, pointer_y);
    pointer_down = have_pointer;
}

static int parse_key(const char *name)
{
    if (strlen(name) == 1 && *name >= '0' && *name <= '9') return NJ_KEY_0 + *name - '0';
    const struct { const char *name; int key; } keys[] = {
        { "up", NJ_KEY_UP }, { "down", NJ_KEY_DOWN }, { "left", NJ_KEY_LEFT },
        { "right", NJ_KEY_RIGHT }, { "fire", NJ_KEY_FIRE }, { "softl", NJ_KEY_SOFT_LEFT },
        { "softr", NJ_KEY_SOFT_RIGHT }, { "star", NJ_KEY_STAR }, { "pound", NJ_KEY_POUND },
        { "clear", NJ_KEY_CLEAR }, { "a", NJ_KEY_GAME_A }, { "b", NJ_KEY_GAME_B },
        { "c", NJ_KEY_GAME_C }, { "d", NJ_KEY_GAME_D } };
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) if (!strcmp(name, keys[i].name)) return keys[i].key;
    return 0;
}

static void screenshot(bool game, bool full, int transfer_baud)
{
    /* Restore the requested rate, not the rounded divider readback. */
    const uint32_t console_baud = 115200;
    bool acknowledged = transfer_baud != console_baud;
    if (transfer_baud != console_baud) {
        printf("SHOT_READY %d ACK\n", transfer_baud); fflush(stdout);
        ESP_ERROR_CHECK(uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(1000)));
        ESP_ERROR_CHECK(uart_set_baudrate(UART_NUM_0, transfer_baud));
        char go[8];
        int got = uart_read_bytes(UART_NUM_0, go, sizeof(go), pdMS_TO_TICKS(3000));
        if (got != sizeof(go) || memcmp(go, "SHOT_GO\n", sizeof(go))) {
            ESP_ERROR_CHECK(uart_set_baudrate(UART_NUM_0, console_baud));
            puts("SHOT_ERROR handshake"); return;
        }
    }
    int w = game && port_state.pixels ? screen_width : full ? LCD_W : 200;
    int h = game && port_state.pixels ? screen_height : full ? LCD_H : 120;
    printf("SHOT_BEGIN %d %d\n", w, h);
    char row[LCD_W * 4 + 2];
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint16_t color = game && port_state.pixels ? port_state.pixels[y * w + x]
                : korvo_lcd()[(y * (full ? 1 : 4)) * LCD_W + x * (full ? 1 : 4)];
            snprintf(row + x * 4, 5, "%04x", color);
        }
        row[w * 4] = '\0';
        /* Keep asynchronous audio diagnostics out of a pixel row. Release
         * stdout before waiting for the host, so a missing ACK cannot stall
         * the audio task behind a console lock. */
        flockfile(stdout);
        if (acknowledged) {
            /* Bound each USB-UART burst as well as the total in-flight row.
             * A 3200-byte LCD row can overrun the bridge at 2 Mbaud. */
            for (int offset = 0; offset < w * 4; offset += 128) {
                int count = w * 4 - offset;
                if (count > 128) count = 128;
                fwrite(row + offset,1,count,stdout); fflush(stdout);
                vTaskDelay(1);
            }
            putchar('\n'); fflush(stdout);
        } else { puts(row); fflush(stdout); }
        funlockfile(stdout);
        if (acknowledged) {
            char ack[9];
            int got = uart_read_bytes(UART_NUM_0,ack,sizeof(ack),pdMS_TO_TICKS(3000));
            if (got != sizeof(ack) || memcmp(ack,"SHOT_ACK\n",sizeof(ack))) {
                ESP_ERROR_CHECK(uart_set_baudrate(UART_NUM_0,console_baud));
                puts("SHOT_ERROR row acknowledgement"); return;
            }
        }
        vTaskDelay(1);
    }
    puts("SHOT_END"); fflush(stdout);
    ESP_ERROR_CHECK(uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(1000)));
    ESP_ERROR_CHECK(uart_set_baudrate(UART_NUM_0, console_baud));
}

static void receive_jar(unsigned size, unsigned crc, const char *name, int transfer_baud)
{
    if (running || !korvo_storage_root() || !size || size > 8 * 1024 * 1024 || !*name ||
        strchr(name, '/') || strchr(name, '\\') || strstr(name, "..") || strlen(name) > 120 ||
        strlen(name) < 5 || strcasecmp(name + strlen(name) - 4, ".jar") ||
        (transfer_baud != 115200 && transfer_baud != 2000000)) {
        puts("PUT_ERROR invalid request or game running"); return;
    }
    char path[PATH_LEN], tmp[PATH_LEN];
    snprintf(path, sizeof(path), "%s/j2me/%s", korvo_storage_root(), name);
    snprintf(tmp, sizeof(tmp), "%s/j2me/%s.part", korvo_storage_root(), name);
    struct stat st;
    if (stat(path, &st) == 0) { puts("PUT_ERROR file exists"); return; }
    uint64_t total_bytes, free_bytes;
    if (korvo_storage_info(&total_bytes, &free_bytes) != ESP_OK) { puts("PUT_ERROR storage info"); return; }
    if (free_bytes < (uint64_t)size + 256 * 1024) {
        printf("PUT_ERROR insufficient space free=%llu reserve=262144\n", (unsigned long long)free_bytes); return;
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) { puts("PUT_ERROR open"); return; }
    const uint32_t console_baud = 115200;
    if (transfer_baud != console_baud) printf("PUT_READY %d\n", transfer_baud);
    else puts("PUT_READY");
    fflush(stdout);
    ESP_ERROR_CHECK(uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(1000)));
    ESP_ERROR_CHECK(uart_set_baudrate(UART_NUM_0, transfer_baud));
    unsigned got = 0, sum = 0;
    uint8_t buf[1024];
    while (got < size) {
        int want = size - got > sizeof(buf) ? sizeof(buf) : size - got;
        int n = uart_read_bytes(UART_NUM_0, buf, want, pdMS_TO_TICKS(3000));
        if (n <= 0 || fwrite(buf, 1, n, f) != n) break;
        sum = esp_rom_crc32_le(sum, buf, n); got += n;
        /* Flow control at each chunk, including the final partial chunk. */
        if (!(got % 1024) || got == size) { printf("PUT_ACK %u\n", got); fflush(stdout); }
        vTaskDelay(1);
    }
    fflush(f); fsync(fileno(f)); fclose(f);
    bool ok = got == size && sum == crc && rename(tmp, path) == 0;
    if (ok) printf("PUT_OK %u %08x %s\n", got, sum, path);
    else { remove(tmp); printf("PUT_ERROR size=%u crc=%08x\n", got, sum); }
    fflush(stdout);
    ESP_ERROR_CHECK(uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(1000)));
    ESP_ERROR_CHECK(uart_set_baudrate(UART_NUM_0, console_baud));
    if (!ok) return;
    scan_games(); draw_menu();
}

static void handle_command(char *line)
{
    char op[24] = "", arg[128] = "";
    int a = 0, b = 0;
    sscanf(line, "%23s %127s", op, arg);
    if (!strcmp(op, "help")) {
        puts("COMMANDS list | storage | settings | reboot (menu) | scale pixel/fit | display | displaytest [frames] | demo [frames] | run INDEX [frames] | bench INDEX FRAMES [keys] | stop | res W H | fps [on/off] | jit [on/off/probe] | jit memory [mapped/internal] (menu only) | key NAME down/up | pointer down/move/up X Y | volume N | stats | shot [game/lcd/full] [baud] | put SIZE CRC32 NAME.jar [baud]");
    } else if (!strcmp(op, "list")) { if (!running) scan_games(); list_games(); }
    else if (!strcmp(op, "settings")) { report_preferences(); }
    else if (!strcmp(op, "reboot")) {
        if (running) { puts("ERROR stop current game first"); return; }
        esp_restart();
    }
    else if (!strcmp(op, "scale")) {
        if (!strcmp(arg, "pixel")) set_scale(true);
        else if (!strcmp(arg, "fit")) set_scale(false);
        else puts("ERROR scale pixel/fit");
    }
    else if (!strcmp(op, "display")) { korvo_display_stats(); }
    else if (!strcmp(op, "displaytest")) {
        if (running) { puts("ERROR stop current game first"); return; }
        unsigned frames = atoi(arg);
        if (!frames || frames > 300) frames = 90;
        korvo_display_test(frames);
        draw_menu();
    }
    else if (!strcmp(op, "storage")) {
        uint64_t total_bytes, free_bytes;
        if (korvo_storage_info(&total_bytes, &free_bytes) == ESP_OK)
            printf("STORAGE root=%s total=%llu free=%llu reserve=262144\n", korvo_storage_root(),
                   (unsigned long long)total_bytes, (unsigned long long)free_bytes);
        else puts("STORAGE_ERROR unavailable");
    }
    else if (!strcmp(op, "demo") || !strcmp(op, "run") || !strcmp(op, "bench")) {
        if (running) { puts("ERROR stop current game first"); return; }
        if (!strcmp(op, "demo")) { a = 0; b = atoi(arg); }
        else sscanf(line, "%*s %d %d", &a, &b);
        if (a < 0 || (unsigned)a >= game_count || b < 0) { puts("ERROR invalid game or frame count"); return; }
        benchmark = !strcmp(op, "bench");
        scripted_keys[0] = 0;
        if (benchmark) sscanf(line, "%*s %*d %*d %767s", scripted_keys);
        pending_game = a; run_frames = b;
    } else if (!strcmp(op, "stop")) { if (running) quit_requested = true; puts("STOP_REQUESTED"); }
    else if (!strcmp(op, "res")) {
        sscanf(line, "%*s %d %d", &a, &b);
        if (running || a < 64 || b < 64 || a > 320 || b > 320) { puts("ERROR res requires menu and 64..320 dimensions"); return; }
        screen_width = a; screen_height = b; save_preferences(); draw_menu(); printf("RES_OK %d %d\n", a, b);
    } else if (!strcmp(op, "volume")) { change_volume(atoi(arg)); printf("VOLUME %d\n", korvo_get_volume()); }
    else if (!strcmp(op, "fps")) {
        if (*arg && strcmp(arg, "on") && strcmp(arg, "off")) { puts("ERROR fps on/off"); return; }
        if (*arg) set_fps(!strcmp(arg, "on"));
        printf("FPS_STATE %s rate=%u.%u\n", fps_enabled ? "on" : "off", fps_tenths / 10, fps_tenths % 10);
    }
    else if (!strcmp(op, "jit")) {
        if (!strcmp(arg, "memory")) {
            char memory[24] = "";
            sscanf(line, "%*s %*s %23s", memory);
            if (running || (*memory && !korvo_jit_memory_set(memory))) {
                puts("ERROR jit memory internal/mapped requires menu"); return;
            }
            if (*memory) save_preferences();
            printf("JIT_MEMORY_STATE %s\n", korvo_jit_memory_name());
            return;
        }
        if (*arg && (running || (strcmp(arg, "on") && strcmp(arg, "off") && strcmp(arg,"probe")))) {
            puts("ERROR jit on/off/probe requires menu"); return;
        }
        if (*arg) {
            if (!strcmp(arg,"probe")) korvo_jit_probe(); else korvo_jit_enable(!strcmp(arg,"on"));
            save_preferences();
            draw_menu();
        }
        printf("JIT_STATE %s available=%d\n", korvo_jit_mode_name(), NJ_ENABLE_JIT);
    }
    else if (!strcmp(op, "key")) {
        char edge[16] = ""; sscanf(line, "%*s %127s %15s", arg, edge);
        a = parse_key(arg);
        if (!running || !a || (strcmp(edge, "down") && strcmp(edge, "up"))) { puts("ERROR key NAME down/up while running"); return; }
        key_event(a, !strcmp(edge, "down")); printf("KEY_OK %d %s\n", a, edge);
    } else if (!strcmp(op, "pointer")) {
        sscanf(line, "%*s %127s %d %d", arg, &a, &b);
        int type = !strcmp(arg, "down") ? NJ_EV_POINTER_DOWN : !strcmp(arg, "move") ? NJ_EV_POINTER_MOVE : !strcmp(arg, "up") ? NJ_EV_POINTER_UP : 0;
        if (running && type && a >= 0 && a < screen_width && b >= 0 && b < screen_height) {
            pointer_event(type, a, b); puts("POINTER_OK");
        } else puts("ERROR pointer");
    } else if (!strcmp(op, "stats")) {
        printf("STATUS running=%d res=%dx%d presents=%u input=%u psram=%u internal=%u\n", running,
               screen_width, screen_height, port_state.presents, port_state.input_events,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        korvo_audio_stats();
    } else if (!strcmp(op, "shot")) {
        int baud = 115200;
        sscanf(line, "%*s %*s %d", &baud);
        if (baud != 115200 && baud != 2000000) puts("SHOT_ERROR baud");
        else screenshot(!strcmp(arg, "game"), !strcmp(arg, "full"), baud);
    }
    else if (!strcmp(op, "put")) {
        unsigned size = 0, crc = 0; char name[128] = ""; int baud = 115200;
        if (sscanf(line, "%*s %u %x %127s %d", &size, &crc, name, &baud) >= 3) receive_jar(size, crc, name, baud);
        else puts("PUT_ERROR syntax");
    } else if (*op) puts("ERROR unknown command; use help");
    fflush(stdout);
}

void launcher_pump(void)
{
    if (pumping) return;
    pumping = true;
    /* Read one byte at a time so PUT_READY can delimit the binary upload. */
    uint8_t c;
    for (int n = 0; n < 512 && uart_read_bytes(UART_NUM_0, &c, 1, 0) == 1; ++n) {
        if (c == '\n' || c == '\r') {
            if (command_len) { command[command_len] = 0; handle_command(command); command_len = 0; }
        } else if (c >= 32 && c != 127 && command_len < sizeof(command) - 1) command[command_len++] = c;
    }
    int64_t now = esp_timer_get_time();
    if (now - last_touch >= 15000) { last_touch = now; touch_poll(); }
    if (fps_enabled) launcher_fps_present(false);
    if (running && now - last_stats >= 5000000) {
        ESP_LOGI(TAG, "RUN presents=%u input=%u PSRAM=%u KiB", port_state.presents, port_state.input_events,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        last_stats = now;
    }
    pumping = false;
}

static uint32_t game_id(const char *path)
{ return esp_rom_crc32_le(0, (const uint8_t *)path, strlen(path)); }

/* Only the fresh per-run bench directory created below is eligible. Normal
 * RMS and previous diagnostic runs are never traversed or deleted. */
static void cleanup_benchmark_rms(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        char file[PATH_LEN]; struct stat st;
        int len = snprintf(file,sizeof(file),"%s/%s",path,entry->d_name);
        if (len > 0 && len < sizeof(file) && stat(file,&st)==0 && S_ISREG(st.st_mode)) remove(file);
    }
    closedir(dir);
    printf("BENCH_CLEAN ok=%d\n",rmdir(path)==0);
}

static void run_game(int index)
{
    korvo_audio_stop();
    vTaskDelay(pdMS_TO_TICKS(20));
    korvo_audio_reset_stats();
    size_t before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    char rms[96];
    snprintf(rms, sizeof(rms), "%s/j2me/rms/%08lx", korvo_storage_root() ?: "/unavailable", (unsigned long)game_id(index ? games[index] : "builtin-demo"));
    nj_port port;
    korvo_port_init(&port, &port_state, screen_width, screen_height);
    port.rms_dir = rms;
    port.fs_dir = storage_files;
    nj_run_opts opts = { .heap_bytes = NJ_MAX_HEAP, .stats = 1, .where = 1,
                         .run_frames = run_frames, .device = nj_device_find("nokia6230") };
    if (benchmark) {
        snprintf(rms,sizeof(rms),"%s/j2me/bench/%016llx",korvo_storage_root() ?: "/unavailable",(unsigned long long)esp_timer_get_time());
        char parent[96];
        snprintf(parent,sizeof(parent),"%s/j2me/bench",korvo_storage_root() ?: "/unavailable"); mkdir(parent,0777);
        if (mkdir(rms,0777) != 0) {
            puts("BENCH_ERROR cannot create fresh RMS directory");
            if (port.shutdown) port.shutdown(port.ud);
            benchmark = false; draw_menu(); return;
        }
        opts.keys = scripted_keys[0] ? scripted_keys : NULL;
        printf("BENCH_KEYS %s\n",opts.keys ?: "(none)");
    }
    korvo_font *font = korvo_font_open(korvo_font_start, korvo_font_end - korvo_font_start);
    if (font) korvo_font_use(font, &opts);
    else ESP_LOGW(TAG, "FreeType font unavailable; using built-in faces");
    if (index) opts.jar_path = games[index];
    else opts.jar_path = "@builtin";
    run_frames = 0; quit_requested = false; touch_keys = 0; pointer_down = false;
    xQueueReset(events); running = true; last_stats = esp_timer_get_time();
    reset_fps_sample();
    active_game = index;
    draw_game(index);
    ESP_LOGI(TAG, "RUN_BEGIN index=%d %s res=%dx%d heap=%uKiB rms=%s", index,
             index ? games[index] : "builtin-demo", screen_width, screen_height, (unsigned)(opts.heap_bytes / 1024), rms);
    int64_t run_start = esp_timer_get_time();
    int rc = nj_run(&port, &opts);
    korvo_font_close(font);
    korvo_port_report(&port_state, esp_timer_get_time() - run_start);
    /* Current upstream leaves port lifetime with the caller. */
    if (port.shutdown) port.shutdown(port.ud);
    if (benchmark) cleanup_benchmark_rms(rms);
    running = false; benchmark = false; quit_requested = false; xQueueReset(events);
    korvo_audio_stop();
    size_t after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "RUN_END rc=%d presents=%u input=%u psram_before=%u psram_after=%u delta=%ld",
             rc, port_state.presents, port_state.input_events, (unsigned)before, (unsigned)after, (long)after - (long)before);
    korvo_audio_stats();
    draw_menu();
}

static void launcher_task(void *arg)
{
    (void)arg;
    esp_err_t settings_rc = korvo_settings_load(&preferences);
    if (settings_rc != ESP_OK) ESP_LOGW(TAG, "Settings unavailable: %s", esp_err_to_name(settings_rc));
    screen_width = preferences.width; screen_height = preferences.height;
    fps_enabled = preferences.fps; pixel_perfect = preferences.pixel_perfect;
    korvo_volume(preferences.volume);
    if (preferences.jit_mode == 2) korvo_jit_probe();
    else korvo_jit_enable(preferences.jit_mode == 1);
    (void)korvo_jit_memory_set(preferences.jit_internal ? "internal" : "mapped");
    ESP_ERROR_CHECK(korvo_board_init());
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 4096, 0, 0, NULL, 0));
    events = xQueueCreate(128, sizeof(nj_event));
    configASSERT(events);
    if (korvo_storage_root()) {
        char path[96];
        snprintf(path, sizeof(path), "%s/j2me", korvo_storage_root()); mkdir(path, 0777);
        snprintf(path, sizeof(path), "%s/j2me/rms", korvo_storage_root()); mkdir(path, 0777);
        snprintf(storage_files, sizeof(storage_files), "%s/j2me/files", korvo_storage_root()); mkdir(storage_files, 0777);
    }
    scan_games(); draw_menu();
    ESP_LOGI(TAG, "READY nano_j2me %s console=115200 flash=2000000; type help", NJ_VERSION_STRING);
    list_games();
    report_preferences();
    while (true) {
        launcher_pump();
        if (pending_game >= 0) { int index = pending_game; pending_game = -1; run_game(index); }
        vTaskDelay(pdMS_TO_TICKS(10) ?: 1);
    }
}

static void allocation_failed(size_t bytes, uint32_t caps, const char *function)
{
    /* This callback may run under a libc lock: do not allocate or use stdio. */
    esp_rom_printf("ALLOC_FAILED bytes=%u caps=%x fn=%s internal=%u largest=%u psram=%u psram_largest=%u\n",
                   (unsigned)bytes, (unsigned)caps, function,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

void app_main(void)
{
    heap_caps_register_failed_alloc_callback(allocation_failed);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    esp_log_level_set("nano-j2me", ESP_LOG_INFO);
    if (xTaskCreatePinnedToCore(launcher_task, "j2me", 32 * 1024, NULL, 4, NULL, 0) != pdPASS)
        ESP_LOGE(TAG, "Cannot create J2ME task");
}
