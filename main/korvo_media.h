#pragma once

#include "nj2me/nj_port.h"

/* Install platform providers through nano_j2me's public port hooks. */
void korvo_media_port(nj_port *port);

/* Image provider hook shared by the two platform adapter translation units. */
void korvo_image_install(void *ud, nj_vm *vm);
void korvo_inflate_port(nj_port *port);
