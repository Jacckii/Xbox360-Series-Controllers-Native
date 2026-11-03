/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Presents the paired controllers to the console as XInput gamepads (the XInputd route).
 *
 * A virtual controller is bound per pad with XamUserBindDeviceCallback, and three
 * functions are hooked so the console reads our state: XInputdReadState (kernel),
 * XamInputGetCapabilitiesEx and XamInputSetState (XAM, the last for rumble). This is
 * hiddriver360's route (EinTim23, GPL-3.0); it leaves the console's own wireless slots
 * alone, so the built-in 360 pads keep working.
 */
#pragma once

#include "../core/compat.h"
#include "../core/gip.h"

/* resolve the hooked functions and install the hooks; 0 or a negative error */
int xa_xinput_init(void);

/* remove the hooks (restore the original kernel/XAM code). Call only when the dashboard
 * is not polling them (e.g. not in Aurora), after a short quiesce. */
void xa_xinput_shutdown(void);

/* unbind all pads in XAM, then xa_xinput_shutdown(): for handing over to a new build */
void xa_xinput_release(void);

/* bind pad `slot` (0..3) as a virtual controller, on the XAM thread (asynchronous):
 * 0 = queued, -1 = not possible */
int xa_xinput_bind(int slot);

/* unbind pad `slot` (stops serving it at once; XAM is told on the XAM thread) */
void xa_xinput_unbind(int slot);

/* store a pad's current state, translated to XInput (called from the driver thread) */
void xa_xinput_update(int slot, const struct gip_gamepad_state *s);

/* a rumble asked for by the console since the last call; false = none.
 * left/right are 0..100 (gip_set_rumble's scale). */
bool xa_xinput_take_rumble(int slot, u8 *left, u8 *right);

/* a headset's microphone as a player's voice device in XAM (on the XAM thread, so
 * asynchronous): bind 0 = queued; voice_user is the bound player, -1 = none; voice_wanted
 * the player it belongs to (our first controller's) */
int xa_xinput_voice_bind(void);
void xa_xinput_voice_unbind(void);
int xa_xinput_voice_user(void);
int xa_xinput_voice_wanted(void);

/* on-screen notifications, shown by the XAM thread; "starting" is shown when it starts */
enum {
	XA_NOTIFY_STARTING, XA_NOTIFY_READY, XA_NOTIFY_NO_FIRMWARE, XA_NOTIFY_NO_FIRMWARE_02FE,
	XA_NOTIFY_NO_FIRMWARE_02E6, XA_NOTIFY_COUNT
};
void xa_xinput_notify(int msg);

/* log the hook counters when they change (throttled); called from the driver thread */
void xa_xinput_debug_log(void);
