// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Function hooks: see detour.h. Trampolines live in xa_trampolines (thunks.s), inside
 * our own .text: code in data pages doesn't run on the 360.
 */
#include "../core/platform.h"
#include "detour.h"

#define TRAMPOLINES 8

extern u32 xa_trampolines[TRAMPOLINES * DETOUR_TRAMPOLINE_WORDS];
static int trampolines_used;

#define OP(insn) ((insn) >> 26)
#define OP_BC 16
#define OP_B 18
#define BIT_AA 2 /* absolute */
#define BIT_LK 1 /* link */
#define BO_ALWAYS 20

static u32 lis(int r, u32 v) { return 0x3c000000 | r << 21 | (v >> 16); }
static u32 ori(int r, u32 v) { return 0x60000000 | r << 21 | r << 16 | (v & 0xffff); }
static u32 mtctr(int r) { return 0x7c0903a6 | r << 21; }
static u32 bcctr(u32 bo, u32 bi, u32 lk) { return 0x4c000420 | bo << 21 | bi << 16 | lk; }

/* far branch through CTR, loaded via scratch register r. Nothing on the stack is
 * touched: a stack slot collides with the `bl __savegpr` save area in some prologues. */
static int far_branch(u32 *p, int r, u32 to, u32 bo, u32 bi, u32 lk)
{
	int n = 0;

	p[n++] = lis(r, to);
	p[n++] = ori(r, to);
	p[n++] = mtctr(r);
	p[n++] = bcctr(bo, bi, lk);
	return n;
}

/* whether insn may name GPR r. Fields are decoded per opcode, because a blind check
 * mistakes branch BO/BI, compare CR fields and shift/immediate bits for registers;
 * unknown forms count every field (conservative). */
static bool names_reg(u32 insn, int r)
{
	u32 d = insn >> 21 & 31, a = insn >> 16 & 31, b = insn >> 11 & 31, ur = (u32)r;

	switch (OP(insn)) {
	case 16: case 17: case 18: case 19: /* bc, sc, b, bclr/bcctr/CR ops: no GPRs */
		return false;
	case 10: case 11:                   /* cmpli, cmpi: crfD, rA, immediate */
		return a == ur;
	case 20: case 21:                   /* rlwimi, rlwinm: rS, rA, SH immediate */
		return d == ur || a == ur;
	case 31:
		switch (insn >> 1 & 0x3ff) {
		case 339: case 467: case 371:   /* mfspr, mtspr, mftb: rD/rS + SPR number */
		case 19: case 144: case 83:     /* mfcr, mtcrf, mfmsr */
			return d == ur;
		}
		return d == ur || a == ur || b == ur;
	case 30: case 23:                   /* MD forms, rlwnm: three registers */
		return d == ur || a == ur || b == ur;
	default:                            /* D/DS forms: rD/rS, rA, then an immediate */
		return d == ur || a == ur;
	}
}

/* The scratch register for the trampoline's branches: volatile, not an argument, and
 * not touched by any relocated instruction. XAM prologues begin `mflr r12;
 * bl __savegprlr_N`, and __savegprlr stores r12 as the saved LR, so a branch built in
 * r12 there would replace the return address. 0 = none free. */
static int pick_scratch(const u32 *insns, int count)
{
	static const int candidates[] = { 12, 11, 0 };

	for (int c = 0; c < 3; c++) {
		int r = candidates[c], used = 0;

		for (int i = 0; i < count; i++)
			used |= names_reg(insns[i], r);
		if (!used)
			return r;
	}
	return -1;
}

/* copies one instruction from `at` to `p`, rewriting PC-relative branches; the number
 * of words written, or 0 if it can't be moved */
static int relocate(u32 *p, const u32 *at, int scratch)
{
	u32 insn = *at, pc = (u32)at;
	s32 off;

	if ((OP(insn) == OP_B || OP(insn) == OP_BC) && (insn & BIT_AA)) {
		p[0] = insn;
		return 1;
	}
	if (OP(insn) == OP_B) {
		off = (s32)(insn << 6) >> 6 & ~3;
		return far_branch(p, scratch, pc + off, BO_ALWAYS, 0, insn & BIT_LK);
	}
	if (OP(insn) == OP_BC) {
		u32 bo = insn >> 21 & 31, bi = insn >> 16 & 31;

		if (!(bo & 4))
			return 0; /* decrements CTR: bcctr can't do that */
		off = (s32)(insn << 16) >> 16 & ~3;
		return far_branch(p, scratch, pc + off, bo, bi, insn & BIT_LK);
	}
	p[0] = insn;
	return 1;
}

/* makes written code visible to instruction fetch */
static void flush_code(const void *p, int bytes)
{
	for (int i = 0; i < bytes; i += 4) {
		const u8 *a = (const u8 *)p + i;

		__asm__ __volatile__("dcbst 0, %0" : : "r"(a) : "memory");
	}
	__asm__ __volatile__("sync" ::: "memory");
	for (int i = 0; i < bytes; i += 4) {
		const u8 *a = (const u8 *)p + i;

		__asm__ __volatile__("icbi 0, %0" : : "r"(a) : "memory");
	}
	__asm__ __volatile__("sync; isync" ::: "memory");
}

int detour_install(struct detour *d, const char *name, void *target, const void *hook)
{
	u32 *t = target, *tr, jump[4];
	int n = 0, scratch;

	if (trampolines_used >= TRAMPOLINES)
		return -XA_ENOSPC;
	tr = &xa_trampolines[trampolines_used * DETOUR_TRAMPOLINE_WORDS];

	xa_log("detour: %s at %08x: %08x %08x %08x %08x\n", name, (u32)t, t[0], t[1], t[2], t[3]);
	scratch = pick_scratch(t, 4);
	if (scratch < 0) {
		xa_log("detour: %s: no free scratch register\n", name);
		return -XA_EINVAL;
	}
	for (int i = 0; i < 4; i++) {
		int w = relocate(tr + n, t + i, scratch);

		if (!w) {
			xa_log("detour: %s: instruction %08x can't be relocated\n", name, t[i]);
			return -XA_EINVAL;
		}
		n += w;
	}
	n += far_branch(tr + n, scratch, (u32)(t + 4), BO_ALWAYS, 0, 0);
	flush_code(tr, n * 4);

	d->name = name;
	d->target = t;
	d->trampoline = tr;
	for (int i = 0; i < 4; i++)
		d->original[i] = t[i];
	trampolines_used++;

	/* the jump replaces the first 4 instructions (it loads CTR via r12, free at entry).
	 * Other hardware threads may be calling the function while we patch it: park new
	 * arrivals on a branch-to-self first, write the rest, then release them. */
	far_branch(jump, 12, (u32)hook, BO_ALWAYS, 0, 0);
	t[0] = 0x48000000; /* b . */
	flush_code(t, 4);
	for (int i = 1; i < 4; i++)
		t[i] = jump[i];
	flush_code(t, 16);
	t[0] = jump[0];
	flush_code(t, 4);
	xa_log("detour: %s hooked (trampoline %08x, %d words, scratch r%d)\n", name, (u32)tr, n,
	       scratch);
	return 0;
}

void detour_remove(struct detour *d)
{
	u32 *t = d->target;

	if (!t)
		return;
	t[0] = 0x48000000; /* b . while the rest is restored (see detour_install) */
	flush_code(t, 4);
	for (int i = 1; i < 4; i++)
		t[i] = d->original[i];
	flush_code(t, 16);
	t[0] = d->original[0];
	flush_code(t, 4);
	d->target = NULL;
}
