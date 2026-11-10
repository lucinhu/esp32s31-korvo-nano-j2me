#pragma once
#include <stdbool.h>
#include "nj2me/nj.h"
#include "launcher_layout.h"

typedef struct korvo_port_state {
    int width, height;
    uint16_t *pixels;
    unsigned presents, input_events;
    int64_t last_yield;
    uint64_t present_us, poll_us, sleep_us;
    uint32_t present_max_us, poll_calls;
    unsigned updated_frames, unchanged_frames;
    size_t hot_used, hot_peak;
    void *hot_pool, *hot_heap;
    unsigned stack_internal, stack_psram;
    size_t jit_code_live, jit_code_peak;
    unsigned jit_allocations, jit_commits, jit_refused;
    struct korvo_jit_arena *jit_arena;
    uint64_t jit_commit_us;
    uint32_t jit_code_hash;
    unsigned jit_map_failures;
    bool jit_mapped, jit_pool_failed;
    uint16_t *previous_pixels;
    uint16_t xmap[LAUNCHER_VIEW_W], ymap[LAUNCHER_VIEW_H];
    int view_x, view_y, view_w, view_h;
    bool previous_valid;
    bool probes_valid;
    uint16_t probes[9];
    const nj_port *midi_port;
    struct korvo_midi *midi_players, *midi_active;
} korvo_port_state;

void korvo_port_init(nj_port *port, korvo_port_state *state, int width, int height);
void korvo_present(korvo_port_state *state);
void korvo_port_update_view(korvo_port_state *state);
void korvo_port_report(korvo_port_state *state, int64_t elapsed_us);
void launcher_pump(void);
int launcher_event(nj_event *event);
int launcher_push(const nj_event *event);
void launcher_view(int width, int height, int *x, int *y, int *w, int *h);
void launcher_fps_present(bool frame_changed);
bool launcher_fps_bounds(int *x, int *y, int *w, int *h);
