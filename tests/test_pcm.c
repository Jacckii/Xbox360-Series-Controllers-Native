// SPDX-License-Identifier: GPL-3.0-or-later
/* Checks pcm_float_to_s16 (integer-only) against the float computation over a sweep.
 *
 *   cc -O2 -Wall -o test_pcm tests/test_pcm.c && ./test_pcm
 */
#include <math.h>
#include <stdio.h>
#include "../xbox360/audio_pcm.h"

static u32 bits_of(float f)
{
	u32 b;
	memcpy(&b, &f, sizeof(b));
	return b;
}

/* the reference: scale by 2^15, truncate toward zero, clip */
static s16 reference(float f)
{
	float x = f * 32768.0f;

	if (isnan(f))
		return 0;
	if (x >= 32767.0f)
		return 32767;
	if (x <= -32767.0f)
		return -32767;
	return (s16)x;
}

int main(void)
{
	static const float fixed[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 1.5f, -7.0f,
				       1e-6f, -1e-6f, 0.999999f, 3.0517578e-5f, INFINITY,
				       -INFINITY, NAN };
	int fails = 0, n = 0;

	for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++, n++)
		if (pcm_float_to_s16(bits_of(fixed[i])) != reference(fixed[i])) {
			printf("FAIL %g: %d, want %d\n", fixed[i], pcm_float_to_s16(bits_of(fixed[i])),
			       reference(fixed[i]));
			fails++;
		}
	for (int i = -2000000; i <= 2000000; i++, n++) {
		float f = i / 1000000.0f;

		if (pcm_float_to_s16(bits_of(f)) != reference(f)) {
			if (fails++ < 10)
				printf("FAIL %.7f: %d, want %d\n", f, pcm_float_to_s16(bits_of(f)),
				       reference(f));
		}
	}
	printf("%s %d values, %d mismatches\n", fails ? "FAIL" : "ok  ", n, fails);

	/* downmix: front + 0.707 * (centre + surround), clipped */
	int dm = pcm_downmix(1000, 0, 0) == 1000 && pcm_downmix(0, 1000, 0) == 707 &&
		 pcm_downmix(0, 0, -1000) == -708 && pcm_downmix(30000, 30000, 30000) == 32767 &&
		 pcm_downmix(-30000, -30000, -30000) == -32767;
	printf("%s downmix\n", dm ? "ok  " : "FAIL");
	return fails != 0 || !dm;
}
