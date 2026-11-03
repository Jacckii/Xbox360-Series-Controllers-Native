/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A headset's microphone for games, through XAM's voice service: see voice_360.c. */
#pragma once

#include "../core/compat.h"

/* hook XAM's voice exports; 0 or a negative error (the microphone then stays unused) */
int xa_voice_init(void);

/* remove the hooks, after a short quiesce */
void xa_voice_shutdown(void);

/* driver thread, every pass: fill capture requests, bind/unbind the microphone */
void xa_voice_tick(void);

/* a headset microphone is sending samples */
int xa_voice_live(void);
