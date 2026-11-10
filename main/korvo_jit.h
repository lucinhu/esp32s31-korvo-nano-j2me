#pragma once
#include "korvo_port.h"

bool korvo_jit_enabled(void);
void korvo_jit_enable(bool enabled);
void korvo_jit_probe(void);
const char *korvo_jit_mode_name(void);
void korvo_jit_port(nj_port *port);
bool korvo_jit_memory_set(const char *name);
const char *korvo_jit_memory_name(void);
void korvo_jit_shutdown(korvo_port_state *state);
