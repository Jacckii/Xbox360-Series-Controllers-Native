// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * xa: the Xbox Wireless Adapter driver as an Xbox 360 system DLL (kernel 17559).
 *
 * A class driver linked into the kernel's runtime USB driver list claims the adapter
 * (045e:02fe). AddDevice opens its endpoints; a driver thread then runs the portable
 * core (core/): firmware upload (the file next to xa.xex), radio, pairing, controllers.
 * Controller state lands in g_xa.pads; g_xa and the log ring are read over xbdm.
 *
 * Kernel context rules (see docs/INTERNALS.md): every kernel -> plugin call goes through
 * an NV64 thunk (thunks.s); the driver thread is pinned to a hw thread; Usbd callbacks
 * run on Usbd's DPC and only set flags; the thread reaches Usbd only through
 * platform_360.c's call queue.
 */
#include <stdlib.h>

#include <xecore/xboxkrnl.h>

#include "../core/gip.h"
#include "../core/platform.h"
#include "platform_360.h"
#include "xinput_360.h"

#define BARRIER() __asm__ __volatile__("lwsync" ::: "memory")

#define ADAPTER_VID 0x045E
#define EP_IN_CMD (XA_MT_EP_IN_CMD | 0x80)
#define EP_IN_WLAN (XA_MT_EP_IN_WLAN | 0x80)
#define EP_MAX_PACKET 512

/* the slim (2017) and the original (2015) Xbox Wireless Adapter for Windows: the same driver,
 * each with its own firmware, as in xone */
static const struct {
	u16 pid;
	const char *firmware;
} adapters[] = {
	{ 0x02FE, "xone_dongle_02fe.bin" },
	{ 0x02E6, "xone_dongle_02e6.bin" },
};

/* the firmware is looked for next to xa.xex first, then in Apps\xadapter on the HDD and
 * each USB drive */
#define FIRMWARE_DIR "\\Apps\\xadapter\\"
#define FIRMWARE_PATH_MAX 260
#define FIRMWARE_MAX 0x20000
static const char *const firmware_roots[] = {
	"\\Device\\Harddisk0\\Partition1", "\\Device\\Mass0", "\\Device\\Mass1", "\\Device\\Mass2",
};
/* LDR_DATA_TABLE_ENTRY, as far as the module's full path (the module handle points to it) */
struct ldr_entry {
	u32 links[6];
	void *nt_headers;
	u8 *image_base;
	u32 image_size;
	u16 full_name_length; /* bytes */
	u16 full_name_max;
	const u16 *full_name;
};

/* ExCreateThread: system thread, pinned to hw thread 2 (affinity mask in the top byte) */
#define THREAD_FLAGS (0x2 | (1u << 2) << 24)
#define THREAD_STACK 0x10000
/* The driver thread's priority. Hw thread 2 is also Usbd's DPC processor and home to a
 * XAM worker-pool thread (priority 6) and the title's workers. At the default 5 the
 * driver thread starved whenever the guide opened (that XAM worker kept the processor
 * busy): no input, the controller dropped, and the guide waited on it. A system-process
 * thread may only go up to 9 (17559 clamps the base to 2..9), which is above that worker.
 * It still only runs in short bursts (2 ms sleeps, drain_rx capped). */
#define THREAD_PRIORITY 9
/* KTHREAD fields (kernel 17559, KeSetBasePriorityThread): current priority, and the
 * process base / lowest / highest base priority the increment is clamped to */
#define KTHREAD_PRIORITY 0x70
#define KTHREAD_PRIORITY_MIN 0xc8
#define KTHREAD_PRIORITY_BASE 0xc9
#define KTHREAD_PRIORITY_MAX 0xca
/* frames handed to the core per loop pass, so commands and the heartbeat still run
 * while a backlog drains */
#define DRAIN_MAX 64

/* ---- kernel 17559 ---------------------------------------------------------------------- */
#define K_DRIVER_LIST 0x801A8048    /* LIST_ENTRY head walked by UsbdSelectClassDriver */
#define K_USBD_TIMER_DPC 0x801721D0 /* Usbd's own DPC: byte 3 = target processor + 1 */
#define K_WGC_DRIVER 0x80172598     /* the console's wireless (FF/5D/81) driver entry */
/* kernel 17559 list of USB devices no driver claimed: 20 entries, {node, mask} stride 8
 * (from UsbdTitleDriverSetUnrecognizedPort). An
 * adapter plugged in before we loaded sits here, so we can re-enumerate it (no replug). */
#define K_UNRECOGNIZED 0x801A7FA0
#define K_UNRECOGNIZED_COUNT 20
#define ORD_UsbdGetDeviceDescriptor 759
/* UsbdTitleDriverResetAllUnrecognizedPorts (17559 export table: 0x800D9BB8, prologue
 * mflr r12; bl __savegprlr). It re-queries every unrecognized device so the linked class
 * drivers get offered it again. It queues transfers and timers on unlocked Usbd state, so
 * it runs on Usbd's DPC, never blocks, and finishes asynchronously: the caller's context
 * (~0xF40 bytes, kept alive) carries a KEVENT at +0x1c that is set when it's done. */
#define ORD_UsbdTitleDriverResetAllUnrecognizedPorts 890
#define RESCAN_CTX_SIZE 0x1000
#define RESCAN_EVENT 0x1c
/* The boot grab waits for the title switch after we load to finish (it resets unrecognized
 * ports itself): our loader handing over to Aurora, or, as a launch.ini boot plugin, the
 * boot launching the default title. The running title is XexExecutableModuleHandle (krnl
 * ord 403, a variable). */
#define ORD_XexExecutableModuleHandle 403
#define TITLE_WAIT_MS 60000  /* no switch seen by then: go ahead anyway */
#define TITLE_SETTLE_MS 8000 /* the new title unchanged this long */
#define RESCAN_WAIT_MS 10000
/* at boot the drives or XAM may not be ready the moment launch.xex loads us */
#define BOOT_RETRY_MS 30000

/* after the firmware starts, how long the 360 gets to notice the chip re-enumerating */
#define REENUMERATE_WAIT_MS 1000

/* thunks (thunks.s) */
void XaLinkDpc(struct kdpc360 *dpc, void *context, void *arg1, void *arg2);
u32 XaMatch(const u8 *ifdesc, const u8 *devdesc, struct usbd_class_driver *self);
void XaAddDevice(void *node);
void XaRemoveDevice(void *node);
long XaCloseDone(struct usbd_transfer *t, long status);
u32 XaThread(void *context);

#define BODY __attribute__((used, noinline))

struct usbd_api usbd;
static const u8 *(*pUsbdGetDeviceDescriptor)(void *node);
static void (*pResetAllUnrecognized)(void *context); /* ord 890, optional */
static HANDLE *pExecutableModule;                     /* ord 403, optional */
static u8 g_rescan_ctx[RESCAN_CTX_SIZE] __attribute__((aligned(16)));
static void (*pXexUnloadAndExit)(HANDLE module); /* XexUnloadImageAndExitThread, ord 418 */
static HANDLE g_self;                            /* our own module handle */
#define ORD_XexUnloadImageAndExitThread 418

/* ---- status block, read over xbdm (magic "XA36"); initialised, so .data isn't empty ---- */

enum xa_state {
	XA_STARTED = 1, XA_RESOLVE_FAILED, XA_CODE_MISMATCH, XA_WAITING, XA_ADDED,
	XA_FIRMWARE, XA_REENUMERATING, XA_RADIO, XA_RUNNING, XA_REMOVED, XA_FAILED,
	XA_NO_FIRMWARE, XA_RELEASED,
};

/* this build's module name (build.sh XA_SLOT: each build loads under its own name and
 * base, so a new build can replace a running one without a reboot) */
#ifndef XA_NAME
#define XA_NAME "xa.xex"
#endif

struct xa_pad {
	u32 connected;
	u32 wcid;
	u32 packets;
	u16 buttons; /* GIP_GP_BTN_* */
	u8 guide;
	u8 reserved;
	u16 trigger_left, trigger_right; /* 0..1023 */
	s16 stick_left_x, stick_left_y, stick_right_x, stick_right_y;
};

struct xa_status {
	u32 magic;
	u32 version;
	volatile u32 state;
	u32 node;
	u32 adds, removes, match_calls;
	s32 last_error;
	u32 firmware_size;
	u32 rx_cmd, rx_wlan, rx_errors;
	u32 log_addr;
	s32 open_status[4];
	volatile u32 cmd_reset_node; /* xbdm writes a device node: re-enumerate it */
	volatile u32 cmd_pairing;    /* xbdm writes 1: pairing on */
	u32 heartbeat;
	struct xa_pad pads[4];
	/* debug: with hold set, a USB removal stops before each step until xbdm writes
	 * cmd_step = 1 (xa.py step), so a freeze can be pinned to one Usbd call */
	volatile u32 hold;
	volatile u32 cmd_step;
	u32 no_led; /* testing: never send the LED command (xa.py led 0|1, read at adapter start) */
	/* debug: adapter register access over xbdm (xa.py reg). xbdm sets reg_addr (and
	 * reg_value to write), then reg_op = 1 read / 2 write; the thread clears reg_op
	 * when done, with reg_status 0 or a negative error */
	volatile u32 reg_op;
	volatile u32 reg_addr;
	volatile u32 reg_value;
	volatile s32 reg_status;
	volatile u32 cmd_unload; /* xbdm writes 1: tear down and XexUnloadImage ourselves */
	/* xbdm writes 1: hand the adapter over to the next build (xa.py reload). We unbind
	 * the pads, remove the hooks, stop the radio and unlink our driver, but keep the
	 * device: the next build resets it, Usbd calls our remove (on its DPC), and the
	 * adapter re-enumerates into the new driver. State becomes XA_RELEASED, node stays. */
	volatile u32 cmd_release;
};

__attribute__((used)) struct xa_status g_xa = {
	.magic = 0x58413336 /* XA36 */,
	.version = 1,
	.hold = 0, /* xa.py hold 1 turns it on */
	.no_led = 0,
};

static struct usbd_class_driver g_driver;
static struct kdpc360 g_link_dpc;
static struct xa_usb g_usb;
static struct xa_xfer g_close[4]; /* one per close step */
static u32 g_close_step;
static void *g_removing; /* the node whose removal is in progress */
static struct xa_dongle g_dongle;
static int g_adapter; /* index in adapters[] of the one being taken; set on Usbd's DPC */
static struct {
	u8 *data;
	u32 len;
} g_fw[ARRAY_SIZE(adapters)];

/*
 * IN buffers: each must fit one EHCI transfer descriptor (XA_USB_IN_MAX from a page
 * boundary). Usbd splits a larger IN transfer over several descriptors without an
 * alternate-next pointer, so after a short packet the controller carries on into the
 * next descriptor of the same buffer while Usbd has already completed the transfer.
 * The adapter's USB RX aggregation is off (MT_USB_DMA_CFG = 0): one frame per transfer.
 */
#define IN_WLAN_SIZE 0x4000
static u8 in_cmd_buf[XA_XFER_IN_COUNT][0x1000] __attribute__((aligned(0x1000)));
static u8 in_wlan_buf[XA_XFER_IN_COUNT][IN_WLAN_SIZE] __attribute__((aligned(0x1000)));

/* ---- class driver (Usbd DPC) ------------------------------------------------------------ */

BODY u32 XaMatchC(const u8 *ifdesc, const u8 *devdesc, struct usbd_class_driver *self)
{
	u16 vid = devdesc[8] | devdesc[9] << 8, pid = devdesc[10] | devdesc[11] << 8;
	unsigned int i;

	(void)ifdesc;
	(void)self;
	g_xa.match_calls++;
	if (vid != ADAPTER_VID)
		return 0;
	for (i = 0; i < ARRAY_SIZE(adapters) && adapters[i].pid != pid; i++)
		;
	if (i == ARRAY_SIZE(adapters))
		return 0;
	xa_log("usb: adapter %04x offered (%s)\n", pid, g_usb.node ? "busy, declined" : "taking it");
	if (g_usb.node)
		return 0;
	g_adapter = (int)i;
	BARRIER(); /* before AddDevice publishes the node the thread waits for */
	return 1;
}

/* open an endpoint into the first block of a group and share its handle with the rest */
static long open_group(void *node, struct xa_xfer *x, int count, int type, int address)
{
	long st = usbd.open_endpoint(node, type, address, EP_MAX_PACKET, 0, &x[0].c.t);

	for (int i = 1; st >= 0 && i < count; i++)
		x[i].c.t = x[0].c.t;
	return st;
}

BODY void XaAddDeviceC(void *node)
{
	struct xa_usb *u = &g_usb;

	g_xa.adds++;
	g_xa.node = (u32)node;
	xa_log("usb: adding device %08x\n", (u32)node);
	usbd.add_device_complete(node, 0);

	memset(&u->ctrl, 0, sizeof(u->ctrl));
	memset(&u->bulk_out, 0, sizeof(u->bulk_out));
	memset(u->in_cmd, 0, sizeof(u->in_cmd));
	memset(u->in_wlan, 0, sizeof(u->in_wlan));
	u->open_status[0] = usbd.open_default_endpoint(node, &u->ctrl.c);
	u->open_status[1] = open_group(node, &u->bulk_out, 1, USBD_TYPE_BULK, XA_MT_EP_OUT);
	u->open_status[2] = open_group(node, u->in_cmd, XA_XFER_IN_COUNT, USBD_TYPE_BULK, EP_IN_CMD);
	u->open_status[3] = open_group(node, u->in_wlan, XA_XFER_IN_COUNT, USBD_TYPE_BULK,
				       EP_IN_WLAN);
	for (int i = 0; i < 4; i++)
		g_xa.open_status[i] = u->open_status[i];
	xa_log("usb: endpoints opened: %08x %08x %08x %08x\n", (u32)u->open_status[0],
	       (u32)u->open_status[1], (u32)u->open_status[2], (u32)u->open_status[3]);

	u->gone = 0;
	u->broken = 0;
	BARRIER();
	u->node = node;
	g_xa.state = XA_ADDED;
}

/* closes the endpoints one by one, then the default endpoint, then reports the removal
 * done, as the kernel's own drivers do. A close is asynchronous (Usbd links the block
 * into the controller's close list) and its completion continues here through
 * XaCloseDone, sometimes from inside the close call itself. So each step has its own
 * block: Usbd may still be unlinking the previous one when its callback runs. */
static void close_next(void *node)
{
	struct xa_usb *u = &g_usb;

	for (;;) {
		u32 step = g_close_step++;
		struct xa_xfer *c;

		if (step < 3) {
			static const int order[3] = { 3, 2, 1 }; /* wlan, cmd, out */
			int which = order[step];
			struct xa_xfer *x = which == 3 ? &u->in_wlan[0] : which == 2 ? &u->in_cmd[0]
										   : &u->bulk_out;

			if (u->open_status[which] < 0)
				continue;
			c = &g_close[step];
			memset(c, 0, sizeof(*c));
			c->c.t.saved_endpoint = x->c.t.endpoint;
			c->c.t.callback = (void *)XaCloseDone;
			xa_log("usb: closing endpoint %d\n", which);
			usbd.queue_close_endpoint(node, c);
			return;
		}
		if (step == 3) {
			if (u->open_status[0] < 0)
				continue;
			c = &g_close[step];
			memset(c, 0, sizeof(*c));
			c->c.t.saved_endpoint = u->ctrl.c.t.endpoint;
			c->c.t.callback = (void *)XaCloseDone;
			xa_log("usb: closing the default endpoint\n");
			usbd.queue_close_default_endpoint(node, c);
			return;
		}
		g_xa.removes++;
		g_xa.state = XA_REMOVED;
		g_removing = NULL;
		xa_log("usb: device %08x removed\n", (u32)node);
		usbd.remove_device_complete(node);
		return;
	}
}

BODY long XaCloseDoneC(struct usbd_transfer *t, long status)
{
	xa_log("usb: close %d done (status %08x)\n", (int)((struct xa_xfer *)t - g_close),
	       (u32)status);
	if (g_xa.hold)
		xa_log("usb: holding before the next removal step\n");
	else
		close_next(g_removing);
	return 0;
}

/* xa.py step: the next held removal step, on Usbd's DPC */
static void step_removal(void *arg)
{
	(void)arg;
	if (g_removing)
		close_next(g_removing);
	else
		xa_log("usb: no removal in progress\n");
}

BODY void XaRemoveDeviceC(void *node)
{
	if (node != g_usb.node) {
		usbd.remove_device_complete(node);
		return;
	}
	xa_log("usb: device %08x is going away\n", (u32)node);
	g_usb.gone = 1;
	BARRIER();
	g_usb.node = 0;
	g_close_step = 0;
	g_removing = node;
	if (g_xa.hold)
		xa_log("usb: holding the removal (xa.py step runs each step)\n");
	else
		close_next(node);
}

BODY void XaLinkDpcC(struct kdpc360 *dpc, void *context, void *arg1, void *arg2)
{
	struct usbd_class_driver *head = (struct usbd_class_driver *)K_DRIVER_LIST;

	(void)dpc, (void)context, (void)arg1, (void)arg2;
	g_driver.flink = head->flink;
	g_driver.blink = head;
	BARRIER(); /* the entry is complete before Usbd can reach it */
	head->flink->blink = &g_driver;
	head->flink = &g_driver;
	g_xa.state = XA_WAITING;
}

/* ---- controllers --------------------------------------------------------------------------- */

static struct xa_pad *pad_for(struct gip_client *gc)
{
	return gc->user;
}

static void pad_ready(struct gip_client *gc)
{
	for (int i = 0; i < 4; i++) {
		struct xa_pad *p = &g_xa.pads[i];

		if (p->connected)
			continue;
		memset(p, 0, sizeof(*p));
		p->wcid = gc->xc->wcid;
		p->connected = 1;
		gc->user = p;
		xa_log("pad %d: ready (client %d)\n", i, gc->xc->wcid);
		xa_xinput_bind(i);
		return;
	}
	xa_log("pad: client %d ready, but all 4 pads are taken\n", gc->xc->wcid);
}

static void pad_input(struct gip_client *gc, const struct gip_gamepad_state *s)
{
	struct xa_pad *p = pad_for(gc);
	bool changed;

	if (!p)
		return;
	changed = p->buttons != s->buttons || p->guide != s->guide;
	p->buttons = s->buttons;
	p->guide = s->guide;
	p->trigger_left = s->trigger_left;
	p->trigger_right = s->trigger_right;
	p->stick_left_x = s->stick_left_x;
	p->stick_left_y = s->stick_left_y;
	p->stick_right_x = s->stick_right_x;
	p->stick_right_y = s->stick_right_y;
	p->packets++;

	xa_xinput_update((int)(p - g_xa.pads), s);

	if (changed)
		xa_log("pad %d: buttons %04x%s\n", (int)(p - g_xa.pads), s->buttons,
		       s->guide ? " Guide" : "");
}

static void pad_gone(struct gip_client *gc)
{
	struct xa_pad *p = pad_for(gc);

	if (p) {
		xa_log("pad %d: off\n", (int)(p - g_xa.pads));
		xa_xinput_unbind((int)(p - g_xa.pads));
		p->connected = 0;
		gc->user = NULL;
	}
}

static const struct gip_ops pad_ops = {
	.ready = pad_ready,
	.input = pad_input,
	.gone = pad_gone,
};

static void client_added(struct xa_client *client)
{
	client->priv = gip_client_new(client, &pad_ops, NULL);
}

static void client_removed(struct xa_client *client)
{
	struct gip_client *gc = client->priv;

	if (gc && gc->user)
		pad_gone(gc);
	gip_client_free(gc);
	client->priv = NULL;
}

static void client_data(struct xa_client *client, const u8 *data, int len)
{
	if (client->priv)
		gip_rx(client->priv, data, len);
}

static const struct xa_dongle_ops dongle_ops = {
	.client_added = client_added,
	.client_removed = client_removed,
	.client_data = client_data,
};

/* ---- driver thread ------------------------------------------------------------------------- */

/* the firmware at path, or NULL (*status: why) */
static u8 *load_firmware_at(const char *path, u32 *len, NTSTATUS *status)
{
	ANSI_STRING name = { (u16)strlen(path), (u16)(strlen(path) + 1), (char *)path };
	OBJECT_ATTRIBUTES attrs = { 0, &name, OBJ_CASE_INSENSITIVE };
	IO_STATUS_BLOCK io;
	HANDLE file;
	NTSTATUS st;
	u8 *buf;
	u32 total = 0;

	/* GENERIC_READ | SYNCHRONIZE, FILE_SHARE_READ, FILE_OPEN,
	 * FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE */
	st = NtCreateFile(&file, 0x80100000, &attrs, &io, 0, 0x80, 1, 1, 0x60);
	*status = st;
	if ((s32)st < 0)
		return NULL;
	buf = malloc(FIRMWARE_MAX);
	while (buf && total < FIRMWARE_MAX) {
		int64_t offset = total;

		st = NtReadFile(file, 0, 0, 0, &io, buf + total, FIRMWARE_MAX - total, &offset);
		if ((s32)st < 0 || !io.Information)
			break;
		total += io.Information;
	}
	NtClose(file);
	*len = total;
	xa_log("xa: firmware %u bytes from %s\n", total, path);
	return buf;
}

/* appends s to the path at out (n: its length so far); the new length, or -1 if too long */
static int path_append(char *out, int n, const char *s)
{
	while (*s) {
		if (n >= FIRMWARE_PATH_MAX - 1)
			return -1;
		out[n++] = *s++;
	}
	out[n] = 0;
	return n;
}

/* the folder xa.xex was loaded from, with a trailing backslash, from our loader entry. Only a
 * \Device\ path inside an entry that really describes this image is trusted; 0 = unknown. */
static int module_dir(char *out)
{
	const struct ldr_entry *e = (const struct ldr_entry *)g_self;
	int n, slash = -1;

	if (!e || !e->full_name || (u8 *)&g_xa < e->image_base ||
	    (u8 *)&g_xa >= e->image_base + e->image_size)
		return 0;
	n = e->full_name_length / 2;
	if (n < 9 || n >= FIRMWARE_PATH_MAX)
		return 0;
	for (int i = 0; i < n; i++) {
		if (e->full_name[i] > 0x7e || e->full_name[i] < 0x20)
			return 0;
		out[i] = (char)e->full_name[i];
		if (out[i] == '\\')
			slash = i;
	}
	out[n] = 0;
	if (memcmp(out, "\\Device\\", 8) || slash < 0)
		return 0;
	out[slash + 1] = 0;
	return slash + 1;
}

/* the firmware named name from the first place that has it; NULL if none does. The places
 * tried are logged the first time only, as the boot retries call this every second. */
static u8 *load_firmware(const char *name, u32 *len, bool *logged_once)
{
	bool logged = *logged_once;
	char path[FIRMWARE_PATH_MAX];
	NTSTATUS st;
	u8 *fw;
	int n;

	n = module_dir(path);
	if (n > 0 && path_append(path, n, name) > 0) {
		if ((fw = load_firmware_at(path, len, &st)))
			return fw;
		if (!logged)
			xa_log("xa: no firmware at %s (%08x)\n", path, (u32)st);
	} else if (!logged) {
		xa_log("xa: own path unknown, trying the default folders\n");
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(firmware_roots); i++) {
		n = path_append(path, 0, firmware_roots[i]);
		if (n < 0 || (n = path_append(path, n, FIRMWARE_DIR)) < 0 ||
		    path_append(path, n, name) < 0)
			continue;
		if ((fw = load_firmware_at(path, len, &st)))
			return fw;
		g_xa.last_error = st;
		if (!logged)
			xa_log("xa: no firmware at %s (%08x)\n", path, (u32)st);
	}
	*logged_once = true;
	return NULL;
}

/* loads every adapter's firmware not loaded yet; the number of adapters that have one */
static int load_all_firmware(void)
{
	static bool logged[ARRAY_SIZE(adapters)];
	int have = 0;

	for (unsigned int i = 0; i < ARRAY_SIZE(adapters); i++) {
		if (!g_fw[i].data) {
			u32 len = 0;
			u8 *fw = load_firmware(adapters[i].firmware, &len, &logged[i]);

			if (fw && len >= 1024) {
				g_fw[i].data = fw;
				g_fw[i].len = len;
			}
		}
		have += g_fw[i].data != NULL;
	}
	return have;
}

/* xbdm commands, checked wherever the thread waits */
static void wait_ms(unsigned int ms);

/* unlink our class driver from the Usbd list; runs on Usbd's DPC (xa_usbd_run) */
static void unlink_driver(void *arg)
{
	struct usbd_class_driver *d = &g_driver;

	(void)arg;
	((struct usbd_class_driver *)d->blink)->flink = d->flink;
	((struct usbd_class_driver *)d->flink)->blink = d->blink;
	BARRIER();
}

/* Tear down and unload ourselves so a new build can be loaded without a reboot. Runs on
 * the driver thread; XexUnloadImageAndExitThread frees the image and exits this thread
 * atomically. Order matters: stop touching the device, remove the hooks (quiesced),
 * unlink the class driver, release the adapter, then unload. */
static void do_unload(void)
{
	xa_log("xa: unloading\n");
	xa_usb_out_async(0);
	g_usb.gone = 1; /* stop IN re-queue; in-flight transfers finish with done */
	BARRIER();
	xa_xinput_shutdown();        /* removes the kernel/XAM hooks after a quiesce */
	xa_usbd_run(unlink_driver, NULL);
	wait_ms(50);
	if (g_usb.node)              /* re-enumerate the adapter so it isn't left claimed by us */
		xa_usbd_reset(NULL, g_usb.node);
	wait_ms(300);                /* let the reset cancel/complete our transfers */
	if (!g_self)
		XexGetModuleHandle(XA_NAME, &g_self);
	if (pXexUnloadAndExit && g_self) {
		xa_log("xa: bye\n");
		pXexUnloadAndExit(g_self); /* frees us and exits this thread; never returns */
	}
	xa_log("xa: cannot self-unload (handle %08x, fn %08x)\n", (u32)g_self, (u32)pXexUnloadAndExit);
	for (;;)
		wait_ms(1000);
}

static volatile int g_release; /* cmd_release seen: the main loop hands over */

/* hand the adapter to the next build (cmd_release): see struct xa_status. Runs on the
 * driver thread at the top of its loop, after the radio is stopped; never returns. */
static void do_release(void)
{
	xa_log("xa: releasing the adapter (node %08x) to the next build\n", (u32)g_usb.node);
	xa_xinput_release();         /* pads unbound in XAM, then the hooks removed */
	xa_usbd_run(unlink_driver, NULL);
	wait_ms(50);
	g_usb.gone = 1;              /* stop IN re-queue; the next build's reset removes us */
	BARRIER();
	g_xa.state = XA_RELEASED;
	xa_log("xa: released\n");
	for (;;)
		xa_sleep_ms(1000);
}

static void service_commands(void)
{
	u32 node = g_xa.cmd_reset_node;

	if (g_xa.cmd_release) {
		g_xa.cmd_release = 0;
		g_release = 1;
	}

	if (g_xa.cmd_unload) {
		g_xa.cmd_unload = 0;
		do_unload();
		return;
	}

	if (node) {
		g_xa.cmd_reset_node = 0;
		xa_log("xa: re-enumerating device %08x\n", node);
		if ((void *)node == g_usb.node)
			xa_usbd_reset(&g_usb, (void *)node);
		else
			xa_usbd_reset(NULL, (void *)node);
	}
	if (g_xa.reg_op) {
		u32 op = g_xa.reg_op, addr = g_xa.reg_addr, val = g_xa.reg_value;

		if (!g_usb.node || g_usb.gone) {
			g_xa.reg_status = -XA_ENODEV;
		} else if (op == 1) {
			g_xa.reg_status = xa_mt_peek(&g_dongle.mt, addr, &val);
			g_xa.reg_value = val;
			xa_log("reg: %08x = %08x (%d)\n", addr, val, g_xa.reg_status);
		} else {
			xa_mt_poke(&g_dongle.mt, addr, val);
			g_xa.reg_status = 0;
			xa_log("reg: %08x <- %08x\n", addr, val);
		}
		BARRIER();
		g_xa.reg_op = 0;
	}
	if (g_xa.cmd_step) {
		g_xa.cmd_step = 0;
		xa_usbd_run(step_removal, NULL);
	}
	if (g_xa.cmd_pairing) {
		g_xa.cmd_pairing = 0;
		if (g_xa.state == XA_RUNNING)
			xa_dongle_set_pairing(&g_dongle, true);
	}
	g_xa.heartbeat++;
}

static void wait_ms(unsigned int ms)
{
	for (unsigned int i = 0; i < ms; i += 10) {
		service_commands();
		xa_sleep_ms(10);
	}
}

static void start_in(struct xa_xfer *x, u8 ep, u8 *buf, u32 size)
{
	if (xa_usb_start_in(&g_usb, x, ep, buf, size))
		g_xa.rx_errors++;
}

/* hands the frames the IN completions queued to the core (platform_360.c keeps the
 * transfers themselves flowing) */
static void drain_rx(void)
{
	static u32 logged;
	const u8 *data;
	u8 ep;
	int len, n = 0;

	while (n++ < DRAIN_MAX && (len = xa_usb_rx_next(&data, &ep)) > 0) {
		if (logged < 6) {
			logged++;
			xa_log("usb: in %02x, %d bytes\n", ep, len);
		}
		if (ep == EP_IN_CMD)
			g_xa.rx_cmd++;
		else
			g_xa.rx_wlan++;
		xa_dongle_rx(&g_dongle, data, len);
	}
	g_xa.rx_errors = xa_rx_errors + xa_rx_dropped;
}

static void run_adapter(const u8 *fw, u32 fw_len)
{
	int err;

	memset(&g_dongle, 0, sizeof(g_dongle));
	g_dongle.ops = &dongle_ops;
	g_dongle.mt.usb = &g_usb;
	g_dongle.no_led = g_xa.no_led;
	if (g_dongle.no_led)
		xa_log("xa: test mode: the LED command is skipped\n");

	for (int i = 0; i < 4; i++) {
		if (g_usb.open_status[i] < 0) {
			xa_log("xa: endpoint %d did not open: %08x\n", i, (u32)g_usb.open_status[i]);
			g_xa.state = XA_FAILED;
			return;
		}
	}

	g_xa.state = XA_FIRMWARE;
	err = xa_mt_load_firmware(&g_dongle.mt, fw, fw_len);
	if (err == XA_MT_REENUMERATE) {
		/* the chip drops off the bus and comes back as a new device; the 360 doesn't
		 * always notice, and then Usbd keeps talking to the old address */
		xa_log("xa: adapter re-enumerating after firmware upload\n");
		g_xa.state = XA_REENUMERATING;
		for (unsigned int ms = 0; ms < REENUMERATE_WAIT_MS && !g_usb.gone; ms += 10)
			wait_ms(10);
		if (!g_usb.gone) {
			xa_log("xa: no removal seen: asking Usbd to re-enumerate it\n");
			if (xa_usbd_reset(&g_usb, g_usb.node))
				xa_log("xa: cannot re-enumerate (a transfer is stuck): re-plug the adapter\n");
		}
		return;
	}
	if (err) {
		xa_log("xa: firmware load failed: %d\n", err);
		g_xa.last_error = err;
		g_xa.state = XA_FAILED;
		return;
	}
	xa_log("xa: firmware running\n");

	for (int i = 0; i < XA_XFER_IN_COUNT; i++) {
		start_in(&g_usb.in_cmd[i], EP_IN_CMD, in_cmd_buf[i], sizeof(in_cmd_buf[i]));
		start_in(&g_usb.in_wlan[i], EP_IN_WLAN, in_wlan_buf[i], sizeof(in_wlan_buf[i]));
	}

	g_xa.state = XA_RADIO;
	err = xa_dongle_start(&g_dongle);
	if (err) {
		xa_log("xa: radio init failed: %d\n", err);
		g_xa.last_error = err;
		g_xa.state = XA_FAILED;
		return;
	}
	g_xa.state = XA_RUNNING;
	xa_log("xa: radio up, waiting for controllers\n");
	xa_xinput_notify(XA_NOTIFY_READY);
	xa_usb_out_async(1); /* steady state: never block the loop on an OUT */

	while (!g_usb.gone && !g_usb.broken && !g_release) {
		drain_rx();
		xa_dongle_poll(&g_dongle);
		for (int i = 0; i < XA_DONGLE_MAX_CLIENTS; i++) {
			struct gip_client *gc = g_dongle.clients[i] ? g_dongle.clients[i]->priv : NULL;
			struct xa_pad *p;
			u8 left, right;

			if (!gc)
				continue;
			/* a rumble the console asked for, forwarded to the controller */
			p = gc->user;
			if (p && xa_xinput_take_rumble((int)(p - g_xa.pads), &left, &right))
				gip_set_rumble(gc, left, right, left, right);
			gip_poll(gc);
		}
		service_commands();
		xa_sleep_ms(2);
	}
	xa_usb_out_async(0); /* back to synchronous for any teardown / next bring-up */
	if (g_usb.broken)
		xa_log("xa: a USB transfer timed out; waiting for the adapter to be re-plugged\n");
}

/* an adapter plugged in before we loaded was enumerated with no driver and parked in the
 * kernel's unrecognized list; have Usbd offer it to the class drivers again so our (now
 * linked) driver claims it. Saves a replug after each boot. A plain UsbdResetDevice of
 * the node doesn't do it: Usbd only re-offers unrecognized devices through
 * UsbdTitleDriverResetAllUnrecognizedPorts. */
__attribute__((used)) volatile u32 xa_boot_grab = 1; /* 0 = never grab (xbdm-writable) */

static void rescan_on_usbd_dpc(void *context) /* xa_usbd_run: on Usbd's DPC */
{
	pResetAllUnrecognized(context);
}

/* The first parked device, if any. They can't be told apart here: UsbdGetDeviceDescriptor
 * ignores the node and returns the descriptor of the device its USB controller enumerated
 * last (only right inside AddDevice). So the rescan runs for any parked device; the ones
 * that aren't ours stay unrecognized. */
static void *find_unrecognized_device(void)
{
	for (int i = 0; i < K_UNRECOGNIZED_COUNT; i++) {
		void *node = *(void *volatile *)(K_UNRECOGNIZED + i * 8);

		if (node)
			return node;
	}
	return NULL;
}

/* until the running title has changed since we loaded and then stayed the same for
 * TITLE_SETTLE_MS (or TITLE_WAIT_MS passed), or the adapter turned up by itself */
static void wait_for_title_switch(void)
{
	HANDLE title = pExecutableModule ? *pExecutableModule : 0;
	unsigned int ms, stable = 0;
	bool switched = false;

	for (ms = 0; ms < TITLE_WAIT_MS && !g_usb.node; ms += 100) {
		HANDLE now = pExecutableModule ? *pExecutableModule : 0;

		if (now != title) {
			title = now;
			switched = true;
			stable = 0;
		} else if ((stable += 100) >= TITLE_SETTLE_MS && (switched || !pExecutableModule)) {
			break;
		}
		wait_ms(100);
	}
	xa_log("xa: title %s after %u ms\n", switched ? "switched and settled" : "unchanged", ms);
}

static void grab_present_adapter(void)
{
	volatile s32 *done = (volatile s32 *)(g_rescan_ctx + RESCAN_EVENT + 4); /* SignalState */
	u32 matches = g_xa.match_calls;
	void *node = find_unrecognized_device();
	unsigned int ms;

	if (!node) {
		xa_log("xa: no device waiting in the unrecognized list\n");
		return;
	}
	if (!pResetAllUnrecognized) {
		xa_log("xa: adapter present (node %08x), but no rescan routine: re-plug it\n",
		       (u32)node);
		return;
	}
	memset(g_rescan_ctx, 0, sizeof(g_rescan_ctx));
	KeInitializeEvent((KEVENT *)(g_rescan_ctx + RESCAN_EVENT), 0 /* notification */, 0);
	BARRIER();
	xa_log("xa: unrecognized device present (node %08x): asking Usbd to offer it again\n",
	       (u32)node);
	xa_usbd_run(rescan_on_usbd_dpc, g_rescan_ctx);
	for (ms = 0; ms < RESCAN_WAIT_MS && !g_usb.node; ms += 10)
		wait_ms(10);
	xa_log("xa: rescan %s after %u ms, match calls +%u, adapter %s\n",
	       *done ? "done" : "still running", ms, g_xa.match_calls - matches,
	       g_usb.node ? "claimed" : "not offered: re-plug it");
}

/* the calling thread's KTHREAD: PCR (r13) + 0x100 */
static u8 *current_kthread(void)
{
	u32 pcr;

	__asm__ volatile("mr %0, 13" : "=r"(pcr));
	return *(u8 **)(pcr + 0x100);
}

static void raise_priority(void)
{
	u8 *kt = current_kthread();

	KeSetBasePriorityThread(kt, (u32)(THREAD_PRIORITY - kt[KTHREAD_PRIORITY_BASE]));
	xa_log("xa: driver thread priority %u (process base %u, allowed %u..%u)\n",
	       kt[KTHREAD_PRIORITY], kt[KTHREAD_PRIORITY_BASE], kt[KTHREAD_PRIORITY_MIN],
	       kt[KTHREAD_PRIORITY_MAX]);
}

BODY u32 XaThreadC(void *context)
{
	(void)context;
	xa_log("xa: driver thread started\n");
	raise_priority();
	/* as a boot plugin we can be up before XAM or the drives: retry those */
	for (unsigned int ms = 0; xa_xinput_init() == -XA_ENODEV && ms < BOOT_RETRY_MS; ms += 1000)
		wait_ms(1000);
	for (unsigned int ms = 0; !load_all_firmware() && ms < BOOT_RETRY_MS; ms += 1000)
		wait_ms(1000);
	if (!load_all_firmware()) {
		g_xa.state = XA_NO_FIRMWARE;
		for (;;)
			wait_ms(1000);
	}

	/* the class driver is linked (start()'s DPC); if the adapter is already plugged in,
	 * grab it without a replug, once the title switch that follows our load is over (the
	 * switch resets unrecognized USB ports itself, and a rescan overlapping it can hang the
	 * console) */
	if (xa_boot_grab) {
		wait_for_title_switch();
		if (!g_usb.node)
			grab_present_adapter();
	}

	for (;;) {
		void *node;
		int a;

		while (!g_usb.node || g_usb.gone) {
			if (g_release)
				do_release();
			wait_ms(10);
		}
		node = g_usb.node;
		a = g_adapter;
		xa_log("xa: adapter %08x (%04x)\n", (u32)node, adapters[a].pid);
		if (!g_fw[a].data)
			load_all_firmware(); /* it may have been copied since boot */
		g_xa.firmware_size = g_fw[a].len;
		if (g_fw[a].data) {
			run_adapter(g_fw[a].data, g_fw[a].len);
		} else {
			xa_log("xa: no %s for this adapter\n", adapters[a].firmware);
			g_xa.state = XA_NO_FIRMWARE;
		}

		/* gone, or unusable until it is re-plugged: drop clients, wait for removal
		 * (not after the firmware upload: the chip is resetting itself). Compare
		 * nodes: after a re-enumeration the new device may already be added. */
		if (g_xa.state != XA_REENUMERATING && g_xa.state != XA_NO_FIRMWARE)
			xa_dongle_stop(&g_dongle);
		if (g_release)
			do_release();
		while (g_usb.node == node && !g_usb.gone)
			wait_ms(10);
		if (g_xa.state != XA_REENUMERATING)
			xa_log("xa: adapter removed\n");
	}
}

/* ---- start ---------------------------------------------------------------------------------- */

static int resolve(HANDLE krnl, u32 ordinal, void *out)
{
	void *p = 0;

	XexGetProcedureAddress(krnl, ordinal, &p);
	if (!p) {
		g_xa.state = XA_RESOLVE_FAILED;
		g_xa.last_error = ordinal;
		return 0;
	}
	*(void **)out = p;
	return 1;
}

static int check_code(u32 addr, u32 expected)
{
	if (*(volatile u32 *)addr == expected)
		return 1;
	g_xa.state = XA_CODE_MISMATCH;
	g_xa.last_error = addr;
	return 0;
}

static void start(void)
{
	HANDLE krnl, thread;
	u32 tid;

	g_xa.state = XA_STARTED;
	g_xa.log_addr = (u32)&xa_log_ring;
	if (XexGetModuleHandle("xboxkrnl.exe", &krnl) < 0)
		return;
	XexGetModuleHandle(XA_NAME, &g_self); /* for self-unload */
	XexGetProcedureAddress(krnl, ORD_XexUnloadImageAndExitThread, (void **)&pXexUnloadAndExit);
	if (!resolve(krnl, ORD_KeInitializeDpc, &ke_dpc.initialize) ||
	    !resolve(krnl, ORD_KeInsertQueueDpc, &ke_dpc.insert) ||
	    !resolve(krnl, ORD_UsbdResetDevice, &usbd.reset_device) ||
	    !resolve(krnl, ORD_UsbdCancelAsyncTransfer, &usbd.cancel_async_transfer) ||
	    !resolve(krnl, ORD_UsbdAddDeviceComplete, &usbd.add_device_complete) ||
	    !resolve(krnl, ORD_UsbdOpenDefaultEndpoint, &usbd.open_default_endpoint) ||
	    !resolve(krnl, ORD_UsbdOpenEndpoint, &usbd.open_endpoint) ||
	    !resolve(krnl, ORD_UsbdQueueAsyncTransfer, &usbd.queue_async_transfer) ||
	    !resolve(krnl, ORD_UsbdQueueCloseDefaultEndpoint, &usbd.queue_close_default_endpoint) ||
	    !resolve(krnl, ORD_UsbdQueueCloseEndpoint, &usbd.queue_close_endpoint) ||
	    !resolve(krnl, ORD_UsbdRemoveDeviceComplete, &usbd.remove_device_complete) ||
	    !resolve(krnl, ORD_UsbdGetDeviceDescriptor, &pUsbdGetDeviceDescriptor))
		return;
	/* optional (boot grab): only the 17559 routine we analysed */
	{
		void *p = 0;

		XexGetProcedureAddress(krnl, ORD_UsbdTitleDriverResetAllUnrecognizedPorts, &p);
		if ((u32)p == 0x800D9BB8 && ((u32 *)p)[0] == 0x7D8802A6 && ((u32 *)p)[1] == 0x4803300D)
			pResetAllUnrecognized = p;
		p = 0;
		XexGetProcedureAddress(krnl, ORD_XexExecutableModuleHandle, &p);
		pExecutableModule = p;
	}

	/* the class-driver list, the wireless driver entry and Usbd's DPC as 17559 has them */
	if (!check_code(0x800D6184, 0x3D60801B) || !check_code(0x800D618C, 0x3BCB8048) ||
	    !check_code(K_WGC_DRIVER + 0x0C, 0x800F3400) ||
	    *(volatile u16 *)K_USBD_TIMER_DPC != 0x0013)
		return;

	ke_dpc.usbd_processor = *(volatile u8 *)(K_USBD_TIMER_DPC + 3);
	xa_usbd_calls_init();

	xa_log("xa: starting\n");
	ExCreateThread(&thread, THREAD_STACK, &tid, 0, (void *)XaThread, 0, THREAD_FLAGS);

	/* flags as the console's own wireless driver has them (interface-level driver) */
	g_driver.flags = *(volatile u32 *)(K_WGC_DRIVER + 0x08);
	g_driver.add_device = XaAddDevice;
	g_driver.remove_device = XaRemoveDevice;
	g_driver.match = XaMatch;

	/* link on Usbd's processor, serialised with Usbd itself */
	ke_dpc.initialize(&g_link_dpc, (void *)XaLinkDpc, 0);
	g_link_dpc.target_processor = ke_dpc.usbd_processor;
	ke_dpc.insert(&g_link_dpc, 0, 0);
}

BODY int DllMainC(unsigned int handle, unsigned int reason, unsigned int reserved)
{
	(void)handle, (void)reserved;
	if (reason == 1 /* DLL_PROCESS_ATTACH */)
		start();
	return 1;
}
