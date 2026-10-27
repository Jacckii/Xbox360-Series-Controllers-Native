/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Function hooks for kernel and XAM code (PowerPC). The first four instructions of a
 * function become a far jump to the hook; a trampoline holds them, relocated, and
 * jumps back. The encoding follows iMoD1998's Detours.h (as used by hiddriver360).
 *
 * The hook is entered with the caller's arguments and return address, so it must be
 * an NV64 thunk (thunks.s). It calls the original through the trampoline.
 */
#pragma once

#include "../core/compat.h"

/* 4 words for the hook; per instruction at most 6 words relocated, plus 6 to go back */
#define DETOUR_TRAMPOLINE_WORDS 30

struct detour {
	const char *name;
	u32 *target;
	u32 original[4];
	u32 *trampoline; /* call this to run the original */
};

/* hooks target -> hook; 0, or a negative error with nothing patched */
int detour_install(struct detour *d, const char *name, void *target, const void *hook);

/* puts the original instructions back */
void detour_remove(struct detour *d);
