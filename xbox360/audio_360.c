// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Game audio for a headset: the console's own render mix, tapped as usb-audio-x360
 * (TimeoutTim) showed it can be. Kernel ordinal 800 registers a client {callback,
 * context} with the audio mixer and returns a handle; the mixer then calls the callback
 * once per 256-sample frame (48 kHz), and in it ordinal 802 fills a buffer with that
 * frame: 6 channels (5.1) of 256 floats, one channel after the other. HDMI/AV output is
 * not affected. There is no unregistering.
 *
 * The callback's context isn't documented, so it only downmixes to stereo in 16 bits and
 * appends that to a ring; the driver thread reads the ring. One writer, one reader, each
 * owning its index. (usb-audio-x360 keeps only front left/right; games put sounds in the
 * centre and surround channels too, which were then missing.)
 */
#include <xecore/xboxkrnl.h>

#include "../core/platform.h"
#include "audio_pcm.h"

#define BODY __attribute__((used, noinline))
#define BARRIER() __asm__ __volatile__("lwsync" ::: "memory")

#define ORD_MecRegisterClient 800  /* (client, &handle) */
#define ORD_MecCaptureRenderFrame 802 /* (handle, float[6][256]) */

#define FRAME 256
/* the mixer's 5.1 channels, in the usual order; LFE (3) is left out of the downmix */
enum { CH_FL, CH_FR, CH_FC, CH_LFE, CH_SL, CH_SR };
/* stereo frames, a power of two: 170 ms, room for gip.c's backlog of 16 packets (128 ms) */
#define RING 8192

struct mec_client {
	u32 callback;
	u32 context;
};

static long (*mec_register)(struct mec_client *client, void **handle);
static long (*mec_capture)(void *handle, u32 *samples);

static struct mec_client client;
static void *volatile mec_handle;
static u32 frame[6 * FRAME] __attribute__((aligned(128))); /* float bits, planar */
static s16 ring[RING][2] __attribute__((aligned(128)));
static volatile u32 ring_write, ring_read; /* write: callback; read: driver thread */
static volatile u32 callbacks, overruns, failures;

void XaMecCallback(void *context); /* thunks.s */

BODY void XaMecCallbackC(void *context)
{
	void *handle = mec_handle;
	u32 w = ring_write;

	(void)context;
	callbacks++;
	if (!handle || mec_capture(handle, frame) < 0) {
		failures++;
		return;
	}
	if (w - ring_read > RING - FRAME) {
		overruns++; /* the reader is behind: drop this frame */
		return;
	}
	for (u32 i = 0; i < FRAME; i++) {
		s16 *d = ring[(w + i) & (RING - 1)];

		d[0] = pcm_downmix(pcm_float_to_s16(frame[CH_FL * FRAME + i]),
				   pcm_float_to_s16(frame[CH_FC * FRAME + i]),
				   pcm_float_to_s16(frame[CH_SL * FRAME + i]));
		d[1] = pcm_downmix(pcm_float_to_s16(frame[CH_FR * FRAME + i]),
				   pcm_float_to_s16(frame[CH_FC * FRAME + i]),
				   pcm_float_to_s16(frame[CH_SR * FRAME + i]));
	}
	BARRIER(); /* the samples before the index that publishes them */
	ring_write = w + FRAME;
}

int xa_audio_start(void)
{
	HANDLE krnl;
	void *handle = 0;
	long st;

	if (mec_handle)
		return 0;
	if (XexGetModuleHandle("xboxkrnl.exe", &krnl) < 0 ||
	    XexGetProcedureAddress(krnl, ORD_MecRegisterClient, (void **)&mec_register) < 0 ||
	    XexGetProcedureAddress(krnl, ORD_MecCaptureRenderFrame, (void **)&mec_capture) < 0 ||
	    !mec_register || !mec_capture)
		return -XA_ENODEV;

	ring_read = ring_write = 0;
	client.callback = (u32)XaMecCallback;
	client.context = 0;
	BARRIER();
	st = mec_register(&client, &handle);
	if (st < 0 || !handle)
		return st < 0 ? (int)st : -XA_EIO;
	mec_handle = handle;
	return 0;
}

u32 xa_audio_read(s16 *lr, u32 frames)
{
	u32 r = ring_read, avail = ring_write - r;

	if (frames > avail)
		frames = avail;
	BARRIER(); /* the index before the samples it published */
	for (u32 i = 0; lr && i < frames; i++) {
		const s16 *s = ring[(r + i) & (RING - 1)];

		lr[2 * i] = s[0];
		lr[2 * i + 1] = s[1];
	}
	BARRIER(); /* done reading before the slots are handed back */
	ring_read = r + frames;
	return frames;
}

u32 xa_audio_available(void)
{
	return ring_write - ring_read;
}

void xa_audio_stats(u32 *n_callbacks, u32 *n_overruns, u32 *n_failures)
{
	*n_callbacks = callbacks;
	*n_overruns = overruns;
	*n_failures = failures;
}
