/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Float PCM to 16-bit without floating point: the plugin is built with -msoft-float. */
#pragma once

#include "../core/compat.h"

/* an IEEE 754 single (its bits) in -1.0..1.0 to a 16-bit sample, clipped; NaN = 0 */
static inline s16 pcm_float_to_s16(u32 bits)
{
	u32 exp = bits >> 23 & 0xff, mag;

	if (exp == 0xff && (bits & 0x7fffff))
		return 0; /* NaN */
	if (exp >= 127) /* |x| >= 1.0 (and infinities) */
		mag = 32767;
	else if (exp < 127 - 15) /* |x| < 2^-15: below one step */
		mag = 0;
	else /* 1.mantissa * 2^(exp - 127) * 2^15, truncated */
		mag = (0x800000 | (bits & 0x7fffff)) >> (127 + 23 - 15 - exp);
	if (mag > 32767)
		mag = 32767;
	return (s16)((bits >> 31) ? -(s32)mag : (s32)mag);
}

/* one side of a 5.1 to stereo downmix: front + 0.707 centre + 0.707 surround (ITU), clipped */
static inline s16 pcm_downmix(s16 front, s16 centre, s16 surround)
{
	s32 v = front + ((centre * 181) >> 8) + ((surround * 181) >> 8);

	if (v > 32767)
		v = 32767;
	if (v < -32767)
		v = -32767;
	return (s16)v;
}
