// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * A headset's microphone for games, through the console's voice service. Titles link XHV,
 * which reaches XAM's voice service through three exports; as usb-audio-x360
 * (TimeoutTim) found, hooking them is enough:
 *
 *   XamVoiceHeadsetPresent (xam 0x30D)  a headset is there while the microphone runs
 *   XamVoiceSubmitPacket   (xam 0x30E)  direction 1 is a capture request: we fill it
 *   XamVoiceGetBatteryStatus (xam 0x310) full, for our bound player
 *
 * plus the microphone bound to player 1 in XAM's chat category (xinput_360.c), so XAM
 * routes that player's voice to us. A capture request is a block of words: [0] status
 * (0x103 pending, 0 done, failure code), [1] bytes delivered, [2] the buffer, [3] its
 * size in units of two 16 kHz samples (G.726 bytes, XHV's default codec), [5] the format.
 * We deliver 16-bit PCM: format 0 and the byte count of that. XHV keeps voice detection,
 * encoding and sending.
 *
 * Requests come from title threads and are only queued in slots there (atomically); the
 * driver thread fills them, oldest first, from the microphone ring, which only it
 * touches: the headset's 24 kHz samples, resampled to 16 kHz.
 */
#include <xecore/xboxkrnl.h>

#include "../core/platform.h"
#include "audio_pcm.h"
#include "detour.h"
#include "voice_360.h"
#include "xinput_360.h"

#define BARRIER() __asm__ __volatile__("lwsync" ::: "memory")
#define HOOK __attribute__((used, noinline))

#define ORD_XamVoiceHeadsetPresent 0x30D
#define ORD_XamVoiceSubmitPacket 0x30E
#define ORD_XamVoiceGetBatteryStatus 0x310

#define DIRECTION_CAPTURE 1
#define PACKET_PENDING 0x103
#define PACKET_FAILED 0xc0000001
#define BATTERY_FULL 3

#define SLOTS 64
#define MIC_RING 4096 /* 16 kHz samples, a power of two: 256 ms */
#define MIC_MAX_UNITS 512
#define MIC_LIVE_MS 200       /* the microphone counts as running this long after a packet */
#define WAIT_MS 100           /* a request waits this long for samples, then fails */
#define SESSION_IDLE_MS 1000  /* no request this long: a new session starts from now */
#define BIND_RETRY_MS 2000
#define UNBIND_AFTER_MS 2000  /* microphone silent this long: unbind */
#define STATS_MS 10000

enum { SLOT_FREE, SLOT_CLAIMED, SLOT_QUEUED };

struct slot {
	volatile u32 state;
	volatile u32 *words;
	u32 units, sequence;
	u64 queued_ms;
};

static struct slot slots[SLOTS];
static volatile u32 sequence;
static volatile u32 last_submit_ms; /* low 32 bits of xa_now_ms, from the hooks */
static volatile u32 mic_live;       /* driver thread: samples are coming */

/* driver thread only */
static s16 mic_ring[MIC_RING];
static u32 mic_write, mic_read;
static u64 mic_last_ms, bind_retry_ms, stats_ms;
static u32 n_requests, n_filled, n_failed, n_dropped;

/* diagnostics, from the hooks (title threads; a lost count doesn't matter) */
static volatile u32 n_present, n_present_yes, n_battery;
static volatile u32 n_submit[3]; /* direction 0, 1 (capture), other */
static volatile u32 n_native;    /* capture passed on: a native headset, we unbound */
static volatile u32 n_not_live, n_bad_args, n_full;
static volatile u32 last_units;

static struct detour d_present, d_submit, d_battery;
typedef u32 (*present_t)(void *handle);
typedef long (*submit_t)(void *handle, u32 direction, void *packet);
typedef long (*battery_t)(u32 user, u32 *status);

/* thunks (thunks.s) */
u32 XaVoicePresentHook(void *handle);
long XaVoiceSubmitHook(void *handle, u32 direction, void *packet);
long XaVoiceBatteryHook(u32 user, u32 *status);

static u32 cas(volatile u32 *p, u32 old, u32 new)
{
	u32 prev;

	__asm__ __volatile__("lwsync\n"
			     "1: lwarx %0, 0, %1\n"
			     "   cmpw %0, %2\n"
			     "   bne- 2f\n"
			     "   stwcx. %3, 0, %1\n"
			     "   bne- 1b\n"
			     "2: isync"
			     : "=&r"(prev)
			     : "r"(p), "r"(old), "r"(new)
			     : "cc", "memory");
	return prev;
}

static u32 atomic_inc(volatile u32 *p)
{
	u32 v;

	do
		v = *p;
	while (cas(p, v, v + 1) != v);
	return v + 1;
}

/* ---- the hooks (title threads) --------------------------------------------------------- */

HOOK u32 XaVoicePresentHookC(void *handle)
{
	n_present++;
	if (((present_t)d_present.trampoline)(handle) || mic_live) {
		n_present_yes++;
		return 1;
	}
	return 0;
}

static bool queue_request(volatile u32 *words)
{
	u32 units;

	if (!mic_live) {
		n_not_live++;
		return false;
	}
	if (!words || !words[2]) {
		n_bad_args++;
		return false;
	}
	units = words[3];
	last_units = units;
	if (!units || units > MIC_MAX_UNITS) {
		n_bad_args++;
		return false;
	}

	last_submit_ms = (u32)xa_now_ms();
	for (int i = 0; i < SLOTS; i++) {
		struct slot *s = &slots[i];

		if (cas(&s->state, SLOT_FREE, SLOT_CLAIMED) != SLOT_FREE)
			continue;
		s->words = words;
		s->units = units;
		s->sequence = atomic_inc(&sequence);
		s->queued_ms = xa_now_ms();
		words[0] = PACKET_PENDING;
		BARRIER();
		s->state = SLOT_QUEUED;
		return true;
	}
	n_full++;
	return false;
}

HOOK long XaVoiceSubmitHookC(void *handle, u32 direction, void *packet)
{
	n_submit[MIN(direction, 2u)]++;
	/* our player's capture requests are ours; before we are bound, only when no native
	 * headset is there, so the console's own voice route keeps working */
	if (direction == DIRECTION_CAPTURE) {
		if (xa_xinput_voice_user() < 0 && ((present_t)d_present.trampoline)(handle))
			n_native++;
		else if (queue_request(packet))
			return 0;
	}
	return ((submit_t)d_submit.trampoline)(handle, direction, packet);
}

HOOK long XaVoiceBatteryHookC(u32 user, u32 *status)
{
	n_battery++;
	if (status && mic_live && (int)user == xa_xinput_voice_user()) {
		*status = BATTERY_FULL;
		return 0;
	}
	return ((battery_t)d_battery.trampoline)(user, status);
}

/* ---- interface (driver thread) --------------------------------------------------------- */

int xa_voice_init(void)
{
	HANDLE xam;
	void *present = 0, *submit = 0, *battery = 0;
	int err;

	if (XexGetModuleHandle("xam.xex", &xam) < 0)
		return -XA_ENODEV;
	XexGetProcedureAddress(xam, ORD_XamVoiceHeadsetPresent, &present);
	XexGetProcedureAddress(xam, ORD_XamVoiceSubmitPacket, &submit);
	XexGetProcedureAddress(xam, ORD_XamVoiceGetBatteryStatus, &battery);
	if (!present || !submit || !battery) {
		xa_log("voice: present=%08x submit=%08x battery=%08x\n", (u32)present, (u32)submit,
		       (u32)battery);
		return -XA_ENODEV;
	}
	err = detour_install(&d_present, "XamVoiceHeadsetPresent", present,
			     (void *)XaVoicePresentHook);
	if (!err)
		err = detour_install(&d_submit, "XamVoiceSubmitPacket", submit,
				     (void *)XaVoiceSubmitHook);
	if (!err)
		err = detour_install(&d_battery, "XamVoiceGetBatteryStatus", battery,
				     (void *)XaVoiceBatteryHook);
	if (err) {
		detour_remove(&d_battery);
		detour_remove(&d_submit);
		detour_remove(&d_present);
		xa_log("voice: hook install failed: %d\n", err);
		return err;
	}
	xa_log("voice: hooks installed\n");
	return 0;
}

void xa_voice_shutdown(void)
{
	mic_live = 0;
	BARRIER();
	xa_sleep_ms(150); /* let in-flight hook calls return before rewriting the code */
	detour_remove(&d_battery);
	detour_remove(&d_submit);
	detour_remove(&d_present);
}

int xa_voice_live(void)
{
	return mic_live;
}

void xa_voice_mic(const u8 *pcm_le, u32 bytes, u32 khz)
{
	s16 in[256], out[256];
	u32 n = MIN(bytes / 2, (u32)ARRAY_SIZE(in)), m;

	for (u32 i = 0; i < n; i++)
		in[i] = (s16)get_le16(pcm_le + 2 * i);
	if (khz == 24) {
		m = pcm_24k_to_16k(in, n, out);
	} else if (khz == 16) {
		memcpy(out, in, n * sizeof(*in));
		m = n;
	} else {
		return; /* other rates: no microphone */
	}

	if (mic_write - mic_read + m > MIC_RING) {
		n_dropped += m; /* titles read too slowly: the oldest samples make room */
		mic_read = mic_write - (MIC_RING - m);
	}
	for (u32 i = 0; i < m; i++)
		mic_ring[(mic_write + i) & (MIC_RING - 1)] = out[i];
	mic_write += m;
	mic_last_ms = xa_now_ms();
	if (!mic_live) {
		mic_live = 1;
		xa_log("voice: microphone running (%u kHz)\n", khz);
	}
}

/* fill the queued requests, oldest first */
static void complete_requests(u64 now)
{
	for (;;) {
		struct slot *oldest = NULL;

		for (int i = 0; i < SLOTS; i++) {
			struct slot *s = &slots[i];

			if (s->state == SLOT_CLAIMED)
				return; /* being queued: don't overtake it */
			if (s->state == SLOT_QUEUED &&
			    (!oldest || (s32)(s->sequence - oldest->sequence) < 0))
				oldest = s;
		}
		if (!oldest)
			return;
		BARRIER();

		volatile u32 *w = oldest->words;
		u32 samples = oldest->units * 2;
		bool ready = mic_live && mic_write - mic_read >= samples;

		if (!ready && mic_live && now - oldest->queued_ms < WAIT_MS)
			return;
		n_requests++;
		if (ready) {
			s16 *dst = (s16 *)w[2];

			for (u32 i = 0; i < samples; i++)
				dst[i] = mic_ring[(mic_read + i) & (MIC_RING - 1)];
			mic_read += samples;
			w[3] = samples * 2; /* bytes of 16-bit PCM */
			w[5] = 0;           /* PCM, not G.726 */
			w[1] = w[3];
			BARRIER();
			w[0] = 0;
			n_filled++;
		} else {
			w[1] = 0;
			w[5] = 0;
			BARRIER();
			w[0] = PACKET_FAILED;
			n_failed++;
		}
		oldest->words = NULL;
		BARRIER();
		oldest->state = SLOT_FREE;
	}
}

void xa_voice_tick(void)
{
	u64 now = xa_now_ms();
	int user = xa_xinput_voice_user();

	if (mic_live && now - mic_last_ms > MIC_LIVE_MS) {
		mic_live = 0;
		xa_log("voice: microphone stopped\n");
	}

	/* a new capture session (no request for a while) starts with fresh samples */
	if ((u32)now - last_submit_ms > SESSION_IDLE_MS)
		mic_read = mic_write;
	complete_requests(now);

	if (mic_live && user < 0 && now >= bind_retry_ms) {
		bind_retry_ms = now + BIND_RETRY_MS;
		xa_xinput_voice_bind();
	} else if (user >= 0 && now >= bind_retry_ms &&
		   (!mic_live ? now - mic_last_ms > UNBIND_AFTER_MS
			      : user != xa_xinput_voice_wanted())) {
		/* silent, or its controller is another player now (bound again when due); the
		 * XAM thread unbinds later, so ask once per retry period */
		bind_retry_ms = now + BIND_RETRY_MS;
		xa_xinput_voice_unbind();
	}
	if (mic_live && now >= stats_ms) {
		if (stats_ms) {
			xa_log("voice: %u requests (%u filled, %u failed), %u samples dropped, "
			       "user %d\n", n_requests, n_filled, n_failed, n_dropped, user);
			xa_log("voice calls: present %u (%u yes), submit %u/%u/%u, battery %u; "
			       "passed on %u, not live %u, bad %u (units %u), full %u\n",
			       n_present, n_present_yes, n_submit[0], n_submit[1], n_submit[2],
			       n_battery, n_native, n_not_live, n_bad_args, last_units, n_full);
		}
		stats_ms = now + STATS_MS;
	}
}
