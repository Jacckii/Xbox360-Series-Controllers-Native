// SPDX-License-Identifier: GPL-3.0-or-later
/* XInput injection: see xinput_360.h. */
#include <xecore/xboxkrnl.h>

#include "../core/platform.h"
#include "detour.h"
#include "xinput_360.h"

#define BARRIER() __asm__ __volatile__("lwsync" ::: "memory")

/* ordinals */
#define ORD_XInputdReadState 486          /* xboxkrnl.exe */
#define ORD_XamInputGetCapabilitiesEx 685 /* xam.xex */
#define ORD_XamInputSetState 402          /* xam.xex */

/* XamUserBindDeviceCallback is not exported; retail 17559 address, checked before use.
 * prologue: 7c8b2378 7ca42b78 54ca063f 41820010 */
#define ADDR_XamUserBind 0x816D9060
static const u32 XAM_BIND_SIG[2] = { 0x7c8b2378, 0x7ca42b78 };

/* The kernel's table of XAM input callbacks (kernel 17559): XAM registers them at boot.
 * +0x0c is the Guide button, +0x10 XamUserBindDeviceCallback. The console's own
 * wireless driver (WgcWirelessOnPacketIn) calls the Guide entry, through 0x8012BB88,
 * with (device context, 0, 0) when Guide is released after a short press, from Usbd's
 * DPC. So it only queues the work and is safe from any context, unlike
 * XamInputSendXenonButtonPress, which opens the guide synchronously and can block its caller. */
#define K_XAM_INPUT_CALLBACKS 0x80189ea0
#define GUIDE_LONG_PRESS_MS 2000 /* held longer: the console's own power-menu path, not ours */
#define GUIDE_DEBOUNCE_MS 700    /* the controller repeats its Guide message: one press, one call */

/* After a Guide press, XAM's follow-up work blocked our driver thread in a kernel wait,
 * even with the Guide call made from another thread, and that thread is the one that
 * feeds XAM the input: a deadlock. The likely link is that the driver thread bound the
 * pads. So every XAM call (bind, unbind, Guide) goes through a thread of its own, on hw
 * thread 4, away from the driver thread (2); the driver thread only queues requests. */
#define XAM_THREAD_FLAGS (0x2 | (1u << 4) << 24)
#define XAM_THREAD_STACK 0x4000
#define XAM_QUEUE 16 /* requests: driver thread -> XAM thread (single producer/consumer) */
enum { REQ_BIND = 1, REQ_UNBIND, REQ_GUIDE, REQ_NOTIFY };

/* On-screen notifications (XNotifyQueueUI, xam ord 656 in every XAM version hiddriver360 supports).
 * Its prototype is (type, user index, u64 areas, text, context): the u64 is the third
 * argument, so it's declared u32 here to keep text and context in r6/r7 as XAM expects
 * (a ppc32 u64 would take a register pair). Type 14 shows text with the review icon. */
#define ORD_XNotifyQueueUI 656
#define NOTIFY_TYPE 14
#define NOTIFY_USER_ANY 0xff
#define NOTIFY_AREAS 1
static const char *const notify_text[XA_NOTIFY_COUNT] = {
	[XA_NOTIFY_STARTING] = "Wireless adapter: starting",
	[XA_NOTIFY_READY] = "Wireless adapter: ready",
};
static u16 notify_wide[XA_NOTIFY_COUNT][40]; /* kept alive: the queue may hold the pointer */

/* our device contexts and controller ids, as hiddriver360 picks them */
#define CONTEXT_BASE 0x10000005
#define CONTROLLER_ID_BASE 0xa7553952

/* XINPUT_GAMEPAD (big-endian native, 12 bytes) */
struct xinput_gamepad {
	u16 buttons;
	u8 trigger_left, trigger_right;
	s16 stick_lx, stick_ly, stick_rx, stick_ry;
};

/* XINPUT button bits */
#define XI_DPAD_UP 0x0001
#define XI_DPAD_DOWN 0x0002
#define XI_DPAD_LEFT 0x0004
#define XI_DPAD_RIGHT 0x0008
#define XI_START 0x0010
#define XI_BACK 0x0020
#define XI_THUMB_L 0x0040
#define XI_THUMB_R 0x0080
#define XI_SHOULDER_L 0x0100
#define XI_SHOULDER_R 0x0200
#define XI_A 0x1000
#define XI_B 0x2000
#define XI_X 0x4000
#define XI_Y 0x8000

#define XI_DEVTYPE_GAMEPAD 1
#define XI_DEVSUBTYPE_GAMEPAD 1
/* ERROR_SUCCESS, ERROR_DEVICE_NOT_CONNECTED, ERROR_INVALID_PARAMETER: from xboxkrnl_types.h */

struct xpad {
	volatile u32 active;
	u32 context;
	u32 user_index;
	volatile u32 packet;
	struct xinput_gamepad state; /* written by the driver thread, read by the hook */
	u64 guide_down_ms;           /* driver thread: when Guide went down, 0 = up */
	u64 guide_sent_ms;           /* driver thread: when we last asked for the guide */
	volatile u32 rumble_pending; /* XamInputSetState -> driver thread */
	volatile u8 rumble_left, rumble_right;
};

static struct xpad g_xpad[4];

/* Debug switches, writable over xbdm (addresses: llvm-nm out/xa.exe): a per-hook enable bit
 * (1 = our behaviour, 0 = pass through) and call counters, to narrow a problem down to one
 * hook without rebuilding. The enable bits (read, caps, set) also decide which hooks
 * xa_xinput_init installs: build.sh installs all three by default (XA_HOOKS=read for input
 * only); without its defines, only the input hook. */
#ifndef XA_HOOK_CAPS
#define XA_HOOK_CAPS 0
#endif
#ifndef XA_HOOK_SET
#define XA_HOOK_SET 0
#endif
__attribute__((used)) volatile u32 xa_xi_on[3] = { 1, XA_HOOK_CAPS, XA_HOOK_SET };
__attribute__((used)) volatile u32 xa_xi_calls[3];                  /* read, caps, set */
__attribute__((used)) volatile u32 xa_xi_read_ctx[4];              /* distinct read contexts seen */
__attribute__((used)) volatile u32 xa_guide = 1;                   /* 0 = Guide does nothing */

/* resolved kernel/XAM functions */
static int (*pXamUserBind)(u32 controller_id, u32 context, u8 category, int disconnect,
			   u8 *user_index);
static void (*pGuideCallback)(u32 context, u32 zero1, u32 zero2);
static void (*pXNotifyQueueUI)(u32 type, u32 user, u32 areas, const u16 *text, void *context);
static volatile u32 xam_queue[XAM_QUEUE]; /* (request << 8) | slot */
static volatile u32 xam_head, xam_tail;
static volatile u32 xam_stop, xam_running;

/* originals, reached through the trampolines */
typedef long (*read_state_t)(u32 ctx, u32 *packet, struct xinput_gamepad *out, int *unk);
typedef u32 (*caps_t)(u32 unk, u32 user, u32 flags, void *caps);
typedef u32 (*set_state_t)(u32 user, u32 flags, void *state, u8 amp, u8 freq, u8 off);

static struct detour d_read, d_caps, d_set;

/* thunks (thunks.s) */
u32 XaXamThread(void *context);
long XaReadStateHook(u32 ctx, u32 *packet, struct xinput_gamepad *out, int *unk);
u32 XaCapsHook(u32 unk, u32 user, u32 flags, void *caps);
u32 XaSetStateHook(u32 user, u32 flags, void *state, u8 amp, u8 freq, u8 off);

#define HOOK __attribute__((used, noinline))

static struct xpad *pad_by_context(u32 ctx)
{
	for (int i = 0; i < 4; i++)
		if (g_xpad[i].active && g_xpad[i].context == ctx)
			return &g_xpad[i];
	return NULL;
}

static struct xpad *pad_by_user(u32 user)
{
	if ((user & 0xff) == 0xff)
		user = 0;
	for (int i = 0; i < 4; i++)
		if (g_xpad[i].active && g_xpad[i].user_index == user)
			return &g_xpad[i];
	return NULL;
}

/* ---- the hooks (C bodies behind the NV64 thunks) --------------------------------------- */

HOOK long XaReadStateHookC(u32 ctx, u32 *packet, struct xinput_gamepad *out, int *unk)
{
	struct xpad *p = ctx >= CONTEXT_BASE ? pad_by_context(ctx) : NULL;

	xa_xi_calls[0]++;
	for (int i = 0; i < 4; i++) {
		if (xa_xi_read_ctx[i] == ctx)
			break;
		if (!xa_xi_read_ctx[i]) {
			xa_xi_read_ctx[i] = ctx;
			break;
		}
	}
	if (!p || !xa_xi_on[0])
		return ((read_state_t)d_read.trampoline)(ctx, packet, out, unk);
	if (!out)
		return ERROR_INVALID_PARAMETER;

	BARRIER();
	*out = p->state;
	if (packet)
		*packet = ++p->packet;
	if (unk)
		*unk = 0;
	/* no XAM calls here: XAM holds its input lock while it polls us (the Guide button is
	 * handled by the driver thread, see xa_xinput_update) */
	return 0;
}

HOOK u32 XaCapsHookC(u32 unk, u32 user, u32 flags, void *caps)
{
	u32 status = ((caps_t)d_caps.trampoline)(unk, user, flags, caps);
	struct xpad *p;
	u8 *c = caps;

	xa_xi_calls[1]++;
	if (status != ERROR_DEVICE_NOT_CONNECTED || !caps || !xa_xi_on[1])
		return status;
	p = pad_by_user(user);
	if (!p)
		return status;

	/* XINPUT_CAPABILITIES_EX: Type, SubType, Flags (u16), then XINPUT_GAMEPAD */
	memset(c, 0, 4 + sizeof(struct xinput_gamepad) + 4);
	c[0] = XI_DEVTYPE_GAMEPAD;
	c[1] = XI_DEVSUBTYPE_GAMEPAD;
	return ERROR_SUCCESS;
}

HOOK u32 XaSetStateHookC(u32 user, u32 flags, void *state, u8 amp, u8 freq, u8 off)
{
	u32 status = ((set_state_t)d_set.trampoline)(user, flags, state, amp, freq, off);
	struct xpad *p = pad_by_user(user);
	u8 *s = state;

	xa_xi_calls[2]++;
	if (!p || !xa_xi_on[2])
		return status;
	/* XINPUT_VIBRATION: wLeftMotorSpeed, wRightMotorSpeed (u16, 0..65535) */
	if (s) {
		p->rumble_left = (u8)(((u32)(s[0] << 8 | s[1]) * 100) / 65535);
		p->rumble_right = (u8)(((u32)(s[2] << 8 | s[3]) * 100) / 65535);
		BARRIER();
		p->rumble_pending = 1;
	}
	return ERROR_SUCCESS;
}

/* ---- interface ------------------------------------------------------------------------- */

int xa_xinput_init(void)
{
	HANDLE krnl, xam;
	void *read_state = 0, *caps = 0, *set_state = 0;
	u32 *bind = (u32 *)ADDR_XamUserBind;
	int err = 0;

	if (XexGetModuleHandle("xboxkrnl.exe", &krnl) < 0 ||
	    XexGetModuleHandle("xam.xex", &xam) < 0) {
		xa_log("xinput: no module handle\n");
		return -XA_ENODEV;
	}
	XexGetProcedureAddress(krnl, ORD_XInputdReadState, &read_state);
	XexGetProcedureAddress(xam, ORD_XamInputGetCapabilitiesEx, &caps);
	XexGetProcedureAddress(xam, ORD_XamInputSetState, &set_state);
	XexGetProcedureAddress(xam, ORD_XNotifyQueueUI, (void *)&pXNotifyQueueUI); /* optional */
	if (!read_state || !caps || !set_state) {
		xa_log("xinput: read=%08x caps=%08x set=%08x\n", (u32)read_state, (u32)caps,
		       (u32)set_state);
		return -XA_ENODEV;
	}
	if (bind[0] != XAM_BIND_SIG[0] || bind[1] != XAM_BIND_SIG[1]) {
		xa_log("xinput: XamUserBindDeviceCallback signature mismatch at %08x (%08x %08x)\n",
		       (u32)bind, bind[0], bind[1]);
		return -XA_EINVAL;
	}
	pXamUserBind = (void *)bind;

	/* the Guide entry, if the table is the one we know (its bind entry is ours) */
	{
		u32 *cb = (u32 *)K_XAM_INPUT_CALLBACKS;

		if (cb[4] == ADDR_XamUserBind && (cb[3] >> 24) == 0x81)
			pGuideCallback = (void *)cb[3];
		xa_log("xinput: guide callback %08x (bind entry %08x)\n", cb[3], cb[4]);
	}

	/* the XAM thread makes every XAM call from here on */
	{
		HANDLE thread;
		u32 tid;

		ExCreateThread(&thread, XAM_THREAD_STACK, &tid, 0, (void *)XaXamThread, 0,
			       XAM_THREAD_FLAGS);
	}

	if (xa_xi_on[0])
		err = detour_install(&d_read, "XInputdReadState", read_state, (void *)XaReadStateHook);
	if (!err && xa_xi_on[1])
		err = detour_install(&d_caps, "XamInputGetCapabilitiesEx", caps, (void *)XaCapsHook);
	if (!err && xa_xi_on[2])
		err = detour_install(&d_set, "XamInputSetState", set_state, (void *)XaSetStateHook);
	xa_log("xinput: installed read=%d caps=%d set=%d\n", xa_xi_on[0], xa_xi_on[1], xa_xi_on[2]);
	if (err) {
		/* leave no half-patched kernel behind */
		detour_remove(&d_set);
		detour_remove(&d_caps);
		detour_remove(&d_read);
		xa_log("xinput: hook install failed: %d\n", err);
		return err;
	}
	xa_log("xinput: hooks installed\n");
	return 0;
}

void xa_xinput_shutdown(void)
{
	/* stop our hook logic first, then let any in-flight hook call drain before we restore
	 * the original bytes (a thread caught mid-trampoline as it's rewritten would crash) */
	xa_xi_on[0] = xa_xi_on[1] = xa_xi_on[2] = 0;
	xam_stop = 1;
	BARRIER();
	for (int i = 0; i < 4; i++)
		g_xpad[i].active = 0;
	xa_sleep_ms(150); /* let any in-flight hook call return before rewriting the code */
	for (int i = 0; i < 50 && xam_running; i++)
		xa_sleep_ms(20);
	if (xam_running)
		xa_log("xinput: XAM thread still busy in XAM\n");
	detour_remove(&d_set);
	detour_remove(&d_caps);
	detour_remove(&d_read);
	xa_log("xinput: hooks removed\n");
}

/* driver thread: queue a request for the XAM thread; false if the queue is full */
static bool xam_request(u32 req, int slot)
{
	u32 head = xam_head;

	if (!xam_running || head - xam_tail >= XAM_QUEUE)
		return false;
	xam_queue[head % XAM_QUEUE] = req << 8 | (u32)slot;
	BARRIER();
	xam_head = head + 1;
	return true;
}

/* before handing over to the next build: unbind every pad in XAM, wait for the XAM
 * thread to do it, then remove the hooks */
void xa_xinput_release(void)
{
	for (int i = 0; i < 4; i++)
		xa_xinput_unbind(i);
	for (int i = 0; i < 100 && xam_running && xam_tail != xam_head; i++)
		xa_sleep_ms(10);
	xa_xinput_shutdown();
}

int xa_xinput_bind(int slot)
{
	if (!pXamUserBind || !xam_request(REQ_BIND, slot)) {
		xa_log("xinput: pad %d not bound (XAM thread not ready)\n", slot);
		return -1;
	}
	return 0;
}

void xa_xinput_unbind(int slot)
{
	struct xpad *p = &g_xpad[slot];

	if (!p->active)
		return;
	p->active = 0; /* the read hook stops serving it now; XAM hears it on the XAM thread */
	BARRIER();
	xam_request(REQ_UNBIND, slot);
}

/* XAM thread: the bodies of the requests */
static void do_bind(int slot)
{
	struct xpad *p = &g_xpad[slot];
	u8 user = 0xff;

	memset(p, 0, sizeof(*p));
	p->context = CONTEXT_BASE + slot;
	pXamUserBind(CONTROLLER_ID_BASE + slot, p->context, 0, 0, &user);
	p->user_index = user == 0xff ? 0 : user;
	BARRIER();
	p->active = 1;
	xa_log("xinput: pad %d bound as user %d\n", slot, p->user_index);
}

static void do_unbind(int slot)
{
	pXamUserBind(CONTROLLER_ID_BASE + slot, CONTEXT_BASE + slot, 0, 1 /* disconnect */, 0);
	xa_log("xinput: pad %d unbound\n", slot);
}

void xa_xinput_notify(int msg)
{
	if (msg >= 0 && msg < XA_NOTIFY_COUNT && !xam_request(REQ_NOTIFY, msg))
		xa_log("xinput: notification %d not queued\n", msg);
}

static void do_notify(int msg)
{
	u16 *w = notify_wide[msg];
	const char *s = notify_text[msg];
	int i;

	if (!pXNotifyQueueUI)
		return;
	for (i = 0; s[i] && i < (int)ARRAY_SIZE(notify_wide[0]) - 1; i++)
		w[i] = (u8)s[i];
	w[i] = 0;
	pXNotifyQueueUI(NOTIFY_TYPE, NOTIFY_USER_ANY, NOTIFY_AREAS, w, 0);
	xa_log("xinput: notified \"%s\"\n", s);
}

/* the XAM thread: runs the driver thread's requests in order */
HOOK u32 XaXamThreadC(void *context)
{
	(void)context;
	xam_running = 1;
	do_notify(XA_NOTIFY_STARTING); /* at boot this is while the other plugins still load */
	while (!xam_stop) {
		while (xam_tail != xam_head) {
			u32 r = xam_queue[xam_tail % XAM_QUEUE];
			int slot = r & 3;

			BARRIER();
			xam_tail++;
			if (r >> 8 == REQ_BIND)
				do_bind(slot);
			else if (r >> 8 == REQ_UNBIND)
				do_unbind(slot);
			else if (r >> 8 == REQ_GUIDE && pGuideCallback)
				pGuideCallback(CONTEXT_BASE + slot, 0, 0);
			else if (r >> 8 == REQ_NOTIFY)
				do_notify((int)(r & 0xff));
		}
		xa_sleep_ms(10);
	}
	xam_running = 0;
	BARRIER();
	ExTerminateThread(0);
	return 0;
}

void xa_xinput_update(int slot, const struct gip_gamepad_state *s)
{
	struct xpad *p = &g_xpad[slot];
	struct xinput_gamepad g;
	u16 b = s->buttons;
	u16 x = 0;

	if (!p->active)
		return;
	if (b & GIP_GP_BTN_A) x |= XI_A;
	if (b & GIP_GP_BTN_B) x |= XI_B;
	if (b & GIP_GP_BTN_X) x |= XI_X;
	if (b & GIP_GP_BTN_Y) x |= XI_Y;
	if (b & GIP_GP_BTN_MENU) x |= XI_START;
	if (b & GIP_GP_BTN_VIEW) x |= XI_BACK;
	if (b & GIP_GP_BTN_DPAD_U) x |= XI_DPAD_UP;
	if (b & GIP_GP_BTN_DPAD_D) x |= XI_DPAD_DOWN;
	if (b & GIP_GP_BTN_DPAD_L) x |= XI_DPAD_LEFT;
	if (b & GIP_GP_BTN_DPAD_R) x |= XI_DPAD_RIGHT;
	if (b & GIP_GP_BTN_BUMPER_L) x |= XI_SHOULDER_L;
	if (b & GIP_GP_BTN_BUMPER_R) x |= XI_SHOULDER_R;
	if (b & GIP_GP_BTN_STICK_L) x |= XI_THUMB_L;
	if (b & GIP_GP_BTN_STICK_R) x |= XI_THUMB_R;

	g.buttons = x;
	g.trigger_left = (u8)((u32)s->trigger_left * 255 / 1023);
	g.trigger_right = (u8)((u32)s->trigger_right * 255 / 1023);
	g.stick_lx = s->stick_left_x;
	g.stick_ly = s->stick_left_y;
	g.stick_rx = s->stick_right_x;
	g.stick_ry = s->stick_right_y;

	p->state = g;
	BARRIER();

	/* Guide: on release after a short press, like the console's own wireless driver */
	if (s->guide) {
		if (!p->guide_down_ms)
			p->guide_down_ms = xa_now_ms();
	} else if (p->guide_down_ms) {
		u64 held = xa_now_ms() - p->guide_down_ms;

		p->guide_down_ms = 0;
		if (held < GUIDE_LONG_PRESS_MS && xa_guide && pGuideCallback &&
		    xa_now_ms() - p->guide_sent_ms >= GUIDE_DEBOUNCE_MS) {
			p->guide_sent_ms = xa_now_ms();
			xa_log("xinput: pad %d guide\n", slot);
			xam_request(REQ_GUIDE, slot);
		}
	}
}

bool xa_xinput_take_rumble(int slot, u8 *left, u8 *right)
{
	struct xpad *p = &g_xpad[slot];

	if (!p->rumble_pending)
		return false;
	p->rumble_pending = 0;
	BARRIER();
	*left = p->rumble_left;
	*right = p->rumble_right;
	return true;
}
