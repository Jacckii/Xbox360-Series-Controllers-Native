// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * platform.h on the Xbox 360 (kernel 17559), plus the few libc pieces the core needs.
 * newlib's malloc and stdio aren't usable in a system DLL without crt0, so allocation
 * goes to the kernel pool and formatting is done here.
 */
#include <stdarg.h>

#include <xecore/xboxkrnl.h>

#include "../core/mt76_defs.h"
#include "../core/platform.h"
#include "platform_360.h"

#define XA_USB_TIMEOUT_MS 1000
#define XA_CANCEL_TIMEOUT_MS 500
#define BARRIER() __asm__ __volatile__("lwsync" ::: "memory")

int xa_debug;

/* DMA buffers: own cache lines (128 bytes on Xenon), never shared with other data */
static u8 ctrl_bounce[0x100] __attribute__((aligned(128)));
static u8 out_bounce[0x4000] __attribute__((aligned(128)));

static u16 swap16(u16 v)
{
	return (u16)(v << 8 | v >> 8);
}

/* submit more queued transfers, chained off this completion (both run on Usbd's DPC) */
static void drain_calls(void);
static void out_fifo_on_complete(struct usbd_transfer *t);

long XaTransferDoneC(struct usbd_transfer *t, long status)
{
	struct xa_xfer *x = (struct xa_xfer *)t;

	x->status = status;
	x->length_after = t->length;
	BARRIER();
	x->done = 1;
	out_fifo_on_complete(t); /* if this was the steady-state OUT, send the next queued one */
	drain_calls();
	return 0;
}

/* ---- Usbd call queue --------------------------------------------------------------------
 * Usbd takes no locks, so every call runs on Usbd's processor at DPC level, as the
 * kernel's own drivers do. One producer (the driver thread), one consumer (the DPC).
 * AddDevice/RemoveDevice also run there, so the DPC sees a consistent device state.
 */

enum { CALL_QUEUE = 1, CALL_CANCEL, CALL_RESET, CALL_RUN };

struct usbd_call {
	u32 op;
	struct xa_usb *usb; /* NULL: a node that isn't the adapter */
	void *node;         /* CALL_RUN: the argument */
	struct xa_xfer *x;
	void (*fn)(void *); /* CALL_RUN */
};

#define CALL_RING 32 /* a power of two */

struct ke_dpc_api ke_dpc;
static struct kdpc360 call_dpc;
static struct usbd_call calls[CALL_RING];
static volatile u32 call_head, call_tail; /* head: the thread's, tail: the DPC's */

static void fail_xfer(struct xa_xfer *x, long status)
{
	x->status = status;
	BARRIER();
	x->done = 1;
}

/* Drains the submit ring. Runs only on Usbd's DPC: from our own call_dpc and from every
 * transfer completion (XaTransferDoneC / XaInDoneC), which are already on that DPC. That
 * completion-chaining is what keeps submits flowing when Aurora saturates Usbd's processor
 * and our standalone call_dpc would otherwise starve. A `busy` gate keeps the (reentrant)
 * completion calls from running the drain on top of itself. */
static volatile u32 drain_busy;

static void drain_calls(void)
{
	if (drain_busy)
		return;
	drain_busy = 1;
	BARRIER();
	while (call_tail != call_head) {
		struct usbd_call *c = &calls[call_tail % CALL_RING];
		bool stale = c->usb && (c->usb->gone || c->usb->node != c->node);

		BARRIER();
		switch (c->op) {
		case CALL_QUEUE:
			if (stale)
				fail_xfer(c->x, -XA_ENODEV);
			else
				usbd.queue_async_transfer(c->node, c->x);
			break;
		case CALL_CANCEL:
			if (!stale)
				usbd.cancel_async_transfer(c->x);
			break;
		case CALL_RESET:
			if (!stale)
				usbd.reset_device(c->node, 1);
			break;
		case CALL_RUN:
			c->fn(c->node);
			break;
		}
		BARRIER();
		call_tail++;
	}
	BARRIER();
	drain_busy = 0;
}

void XaCallDpcC(struct kdpc360 *dpc, void *context, void *arg1, void *arg2)
{
	(void)dpc, (void)context, (void)arg1, (void)arg2;
	drain_calls();
}

void xa_usbd_calls_init(void)
{
	ke_dpc.initialize(&call_dpc, (void *)XaCallDpc, 0);
	call_dpc.target_processor = ke_dpc.usbd_processor;
}

static void post_fn(u32 op, struct xa_usb *usb, void *node, struct xa_xfer *x,
		    void (*fn)(void *))
{
	struct usbd_call *c;

	while (call_head - call_tail >= CALL_RING)
		xa_sleep_ms(1);
	c = &calls[call_head % CALL_RING];
	c->op = op;
	c->usb = usb;
	c->node = node;
	c->x = x;
	c->fn = fn;
	BARRIER();
	call_head++;
	/* already queued is fine: that run drains this entry too */
	ke_dpc.insert(&call_dpc, 0, 0);
}

static void post(u32 op, struct xa_usb *usb, void *node, struct xa_xfer *x)
{
	post_fn(op, usb, node, x, NULL);
}

void xa_usbd_run(void (*fn)(void *), void *arg)
{
	post_fn(CALL_RUN, NULL, arg, NULL, fn);
}

int xa_usb_queue(struct xa_usb *usb, struct xa_xfer *x, void *buffer, u32 length)
{
	void *node = usb->node;

	if (!node || usb->gone || usb->broken)
		return -XA_ENODEV;
	x->c.t.buffer = buffer;
	x->c.t.length = length;
	x->c.t.flags = 1;
	x->c.t.callback = (void *)XaTransferDone;
	x->c.t.saved_endpoint = x->c.t.endpoint;
	x->done = 0;
	BARRIER();
	post(CALL_QUEUE, usb, node, x);
	return 0;
}

int xa_usbd_reset(struct xa_usb *usb, void *node)
{
	if (usb && (usb->gone || usb->broken))
		return -XA_ENODEV;
	post(CALL_RESET, usb, node, NULL);
	return 0;
}

/* ---- bulk IN: receive ring -------------------------------------------------------------
 * Completions (Usbd's DPC) append frames, the driver thread consumes them. Each entry is
 * a header and the frame, padded to 4 bytes; positions are running byte counts.
 */

#define RX_RING_SIZE 0x20000 /* a power of two */
#define RX_FRAME_MAX 0x8400  /* the largest IN transfer (XA_DONGLE_LEN_WLAN_PKT) */
#define STATUS_CANCELLED 0xC0000120

struct rx_hdr {
	u32 len;
	u32 ep;
};

volatile u32 xa_rx_dropped, xa_rx_errors;
static struct xa_usb *rx_usb;
static u8 rx_ring[RX_RING_SIZE] __attribute__((aligned(128)));
static volatile u32 rx_write, rx_read; /* write: the DPC's, read: the thread's */
static u8 rx_frame[RX_FRAME_MAX] __attribute__((aligned(4)));

static void ring_put(u32 pos, const void *src, u32 n)
{
	u32 at = pos % RX_RING_SIZE, first = MIN(n, RX_RING_SIZE - at);

	memcpy(rx_ring + at, src, first);
	memcpy(rx_ring, (const u8 *)src + first, n - first);
}

static void ring_get(void *dst, u32 pos, u32 n)
{
	u32 at = pos % RX_RING_SIZE, first = MIN(n, RX_RING_SIZE - at);

	memcpy(dst, rx_ring + at, first);
	memcpy((u8 *)dst + first, rx_ring, n - first);
}

/* the frame's length from its FCE header: the LEN field plus the header and trailer */
static u32 frame_length(const struct xa_xfer *x)
{
	u32 len = FIELD_GET(MT_RX_FCE_INFO_LEN, get_le32(x->buffer)) + 2 * MT_CMD_HDR_LEN;

	return MIN(len, x->size);
}

static void prep_in(struct xa_xfer *x)
{
	x->c.t.buffer = x->buffer;
	x->c.t.length = x->size;
	x->c.t.flags = 1;
	x->c.t.callback = (void *)XaInDone;
	x->c.t.saved_endpoint = x->c.t.endpoint;
	x->done = 0;
}

long XaInDoneC(struct usbd_transfer *t, long status)
{
	struct xa_xfer *x = (struct xa_xfer *)t;
	struct xa_usb *usb = rx_usb;

	x->status = status;
	x->length_after = t->length;
	if (status >= 0) {
		u32 len = frame_length(x), need = sizeof(struct rx_hdr) + ROUND_UP(len, 4);
		struct rx_hdr h = { len, x->ep };

		if (RX_RING_SIZE - (rx_write - rx_read) < need) {
			xa_rx_dropped++;
		} else {
			ring_put(rx_write, &h, sizeof(h));
			ring_put(rx_write + sizeof(h), x->buffer, len);
			BARRIER();
			rx_write += need;
		}
	} else {
		xa_rx_errors++;
	}

	/* straight back to Usbd, as xone resubmits its URBs; not after a cancel or once the
	 * device is going away (Usbd completes everything with an error status then) */
	if ((u32)status != STATUS_CANCELLED && usb && usb->node && !usb->gone && !usb->broken) {
		prep_in(x);
		usbd.queue_async_transfer(usb->node, x);
	} else {
		BARRIER();
		x->done = 1;
	}
	drain_calls(); /* keep our submits flowing even if the standalone call_dpc starves */
	return 0;
}

int xa_usb_start_in(struct xa_usb *usb, struct xa_xfer *x, u8 ep, u8 *buf, u32 size)
{
	if (!usb->node || usb->gone || usb->broken)
		return -XA_ENODEV;
	if (((u32)buf & 0xfff) + size > XA_USB_IN_MAX) {
		xa_log("usb: IN buffer %08x+%#x spans two transfer descriptors\n", (u32)buf, size);
		return -XA_EINVAL;
	}
	rx_usb = usb;
	x->buffer = buf;
	x->size = size;
	x->ep = ep;
	prep_in(x);
	BARRIER();
	post(CALL_QUEUE, usb, usb->node, x);
	return 0;
}

int xa_usb_rx_next(const u8 **data, u8 *ep)
{
	struct rx_hdr h;

	if (rx_read == rx_write)
		return 0;
	BARRIER();
	ring_get(&h, rx_read, sizeof(h));
	ring_get(rx_frame, rx_read + sizeof(h), h.len);
	BARRIER();
	rx_read += sizeof(h) + ROUND_UP(h.len, 4);
	*data = rx_frame;
	*ep = (u8)h.ep;
	return (int)h.len;
}

/* a transfer that never completed: cancel it and wait until Usbd has let go of it */
static int cancel(struct xa_usb *usb, struct xa_xfer *x)
{
	u64 until = xa_now_ms() + XA_CANCEL_TIMEOUT_MS;
	const u8 *buf = x->c.t.buffer;

	xa_log("usb: cancelling a %u-byte transfer (endpoint block %08x, header %08x)\n",
	       x->c.t.length, x->c.t.endpoint, buf && x->c.t.length >= 4 ? get_le32(buf) : 0);
	post(CALL_CANCEL, usb, usb->node, x);
	while (!x->done) {
		if (usb->gone)
			return -XA_ENODEV;
		if (xa_now_ms() > until) {
			/* Usbd still owns the block: no transfer or reset is safe from here on */
			usb->broken = 1;
			xa_log("usb: a timed-out transfer could not be cancelled\n");
			return -XA_ETIMEDOUT;
		}
		xa_sleep_ms(1);
	}
	BARRIER();
	xa_log("usb: timed-out transfer cancelled (status %08x)\n", (u32)x->status);
	return -XA_ETIMEDOUT;
}

/* sleeps until the transfer completes; 0, or a negative error */
static int wait_done(struct xa_usb *usb, struct xa_xfer *x)
{
	u64 until = xa_now_ms() + XA_USB_TIMEOUT_MS;

	while (!x->done) {
		if (usb->gone)
			return -XA_ENODEV;
		if (xa_now_ms() > until)
			return cancel(usb, x);
		xa_sleep_ms(1);
	}
	BARRIER();
	if (x->status < 0)
		return usb->gone ? -XA_ENODEV : -XA_EIO;
	return 0;
}

int xa_usb_ctrl(struct xa_usb *usb, u8 request_type, u8 request, u16 value,
		u16 index, void *data, u16 len)
{
	struct xa_xfer *x = &usb->ctrl;
	int err;

	if (len > sizeof(ctrl_bounce))
		return -XA_EINVAL;
	if (!(request_type & USB_DIR_IN) && len)
		memcpy(ctrl_bounce, data, len);
	x->c.request_type = request_type;
	x->c.request = request;
	x->c.value = swap16(value);
	x->c.index = swap16(index);
	x->c.length = swap16(len);

	/* no buffer without data: Usbd builds a data stage whenever the buffer pointer is
	 * set, even for 0 bytes, and the device never completes such a request */
	err = xa_usb_queue(usb, x, len ? ctrl_bounce : NULL, len);
	if (!err)
		err = wait_done(usb, x);
	if (err)
		return err;
	if ((request_type & USB_DIR_IN) && len)
		memcpy(data, ctrl_bounce, len);
	return len;
}

/* Steady-state OUT queue. The driver thread must never block on an OUT (Aurora delays the
 * completion and the whole RX pipeline stalls behind it), but OUT must also stay reliable
 * and ordered: dropping GIP association/auth packets breaks the controller session. So
 * the thread only appends a payload to this FIFO; a single transfer is kept in flight on
 * the one OUT endpoint, and each completion submits the next (chained on Usbd's DPC). One
 * in flight also keeps the kernel's unlocked qTD free-list from being over-used. Bring-up
 * (firmware/radio) stays synchronous (xa_out_async=0), where blocking is fine. */
#define OUT_FIFO 32 /* a power of two */
#define OUT_BUF 512 /* steady-state packets (GIP acks, rumble, beacons) are small */
static u8 out_fifo_buf[OUT_FIFO][OUT_BUF] __attribute__((aligned(128)));
static u16 out_fifo_len[OUT_FIFO];
static volatile u32 out_head, out_tail; /* head: driver thread; tail: Usbd DPC */
static volatile u32 out_inflight;       /* DPC-owned: a transfer is on the endpoint */
static struct xa_usb *out_usb;
volatile int xa_out_async;

/* submit the FIFO's tail entry on the one OUT endpoint; call only on Usbd's DPC */
static void out_submit_tail(void)
{
	struct xa_xfer *x;
	u32 i;

	if (out_inflight || out_head == out_tail)
		return;
	if (!out_usb || !out_usb->node || out_usb->gone || out_usb->broken)
		return;
	i = out_tail % OUT_FIFO;
	x = &out_usb->bulk_out;
	x->c.t.buffer = out_fifo_buf[i];
	x->c.t.length = out_fifo_len[i];
	x->c.t.flags = 1;
	x->c.t.callback = (void *)XaTransferDone;
	x->c.t.saved_endpoint = x->c.t.endpoint;
	x->done = 0;
	out_inflight = 1;
	BARRIER();
	usbd.queue_async_transfer(out_usb->node, x);
}

/* the OUT just completed: drop it from the FIFO and send the next (on Usbd's DPC) */
static void out_fifo_on_complete(struct usbd_transfer *t)
{
	if (!xa_out_async || !out_usb || t != (struct usbd_transfer *)&out_usb->bulk_out)
		return;
	out_tail++;
	out_inflight = 0;
	BARRIER();
	out_submit_tail();
}

static void out_kick(void *arg) /* CALL_RUN on the DPC: start the FIFO if idle */
{
	(void)arg;
	out_submit_tail();
}

void xa_usb_out_async(int on)
{
	out_head = out_tail = out_inflight = 0;
	BARRIER();
	xa_out_async = on;
}

int xa_usb_bulk_out(struct xa_usb *usb, u8 ep, const void *data, int len)
{
	struct xa_xfer *x;
	int err;

	(void)ep; /* the adapter has one OUT endpoint in use, opened at AddDevice */

	if (xa_out_async) {
		u32 i;

		if (len > OUT_BUF)
			return -XA_EINVAL;
		if (out_head - out_tail >= OUT_FIFO)
			return 0; /* FIFO full (32 deep): only happens under extreme overload */
		i = out_head % OUT_FIFO;
		memcpy(out_fifo_buf[i], data, len);
		out_fifo_len[i] = (u16)len;
		out_usb = usb;
		BARRIER();
		out_head++;
		xa_usbd_run(out_kick, NULL); /* kick the FIFO on the DPC (never blocks the thread) */
		return 0;
	}

	x = &usb->bulk_out;
	if (len > (int)sizeof(out_bounce))
		return -XA_EINVAL;
	memcpy(out_bounce, data, len);
	err = xa_usb_queue(usb, x, out_bounce, len);
	if (!err)
		err = wait_done(usb, x);
	return err;
}

int xa_random(void *buf, int len)
{
	XeCryptRandom(buf, len);
	return 0;
}

void xa_sleep_ms(unsigned int ms)
{
	int64_t interval = -(int64_t)ms * 10000; /* relative, 100 ns units */

	KeDelayExecutionThread(0, 0, &interval);
}

u64 xa_now_ms(void)
{
	int64_t t;

	KeQuerySystemTime(&t);
	return (u64)t / 10000;
}

/* ---- log ring ------------------------------------------------------------------------ */

struct xa_log_ring xa_log_ring = { 0x584C4F47 /* XLOG */, sizeof(xa_log_ring.text), 0, "" };

static void log_put(const char *s, int n)
{
	u32 w = xa_log_ring.written;

	for (int i = 0; i < n; i++)
		xa_log_ring.text[(w + i) % sizeof(xa_log_ring.text)] = s[i];
	BARRIER();
	xa_log_ring.written = w + n;
}

void xa_log(const char *fmt, ...)
{
	static u64 start;
	char line[256];
	u64 t = xa_now_ms();
	va_list ap;
	int n;

	if (!start)
		start = t;
	t -= start;
	n = snprintf(line, sizeof(line), "[%4u.%03u] ", (u32)(t / 1000), (u32)(t % 1000));
	va_start(ap, fmt);
	n += xa_vsnprintf(line + n, sizeof(line) - n, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1)
		n = sizeof(line) - 1;
	log_put(line, n);
}

/* ---- formatting: %[-0][width][.prec][l|ll|h|hh]{d,i,u,x,X,s,c,p,%} ------------------------ */

struct out {
	char *buf;
	int size, len;
};

static void put(struct out *o, char c)
{
	if (o->len < o->size - 1)
		o->buf[o->len] = c;
	o->len++;
}

/* right-aligned in `width` (left alignment is padded by the caller) */
static void put_num(struct out *o, unsigned long long v, int base, bool upper, bool neg,
		    int width, bool zero, bool left)
{
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char tmp[24];
	int n = 0;

	do {
		tmp[n++] = digits[v % base];
		v /= base;
	} while (v);
	if (neg)
		width--;
	if (neg && zero)
		put(o, '-');
	for (int pad = width - n; !left && pad > 0; pad--)
		put(o, zero ? '0' : ' ');
	if (neg && !zero)
		put(o, '-');
	while (n)
		put(o, tmp[--n]);
}

int xa_vsnprintf(char *buf, int size, const char *fmt, va_list ap)
{
	struct out o = { buf, size, 0 };

	for (; *fmt; fmt++) {
		bool zero = false, left = false;
		int width = 0, prec = -1, longs = 0, start;

		if (*fmt != '%') {
			put(&o, *fmt);
			continue;
		}
		fmt++;
		for (;; fmt++) {
			if (*fmt == '0')
				zero = true;
			else if (*fmt == '-')
				left = true;
			else if (*fmt != ' ' && *fmt != '+' && *fmt != '#')
				break;
		}
		if (*fmt == '*') {
			width = va_arg(ap, int);
			if (width < 0) {
				left = true;
				width = -width;
			}
			fmt++;
		}
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + *fmt++ - '0';
		if (*fmt == '.') {
			prec = 0;
			fmt++;
			if (*fmt == '*') {
				prec = va_arg(ap, int);
				fmt++;
			} else {
				for (; *fmt >= '0' && *fmt <= '9'; fmt++)
					prec = prec * 10 + *fmt - '0';
			}
		}
		for (; *fmt == 'l' || *fmt == 'h' || *fmt == 'z'; fmt++)
			if (*fmt == 'l')
				longs++;

		start = o.len;
		switch (*fmt) {
		case 'd':
		case 'i': {
			long long v = longs > 1 ? va_arg(ap, long long) : longs ? va_arg(ap, long)
								 : va_arg(ap, int);
			put_num(&o, v < 0 ? -(unsigned long long)v : (unsigned long long)v, 10,
				false, v < 0, width, zero && !left, left);
			break;
		}
		case 'u':
		case 'x':
		case 'X': {
			unsigned long long v = longs > 1 ? va_arg(ap, unsigned long long)
					       : longs  ? va_arg(ap, unsigned long)
							: va_arg(ap, unsigned int);
			put_num(&o, v, *fmt == 'u' ? 10 : 16, *fmt == 'X', false, width,
				zero && !left, left);
			break;
		}
		case 'p':
			put(&o, '0');
			put(&o, 'x');
			put_num(&o, (unsigned long)va_arg(ap, void *), 16, false, false, 8, true,
				false);
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			int n = 0;

			if (!s)
				s = "(null)";
			while (s[n] && (prec < 0 || n < prec))
				n++;
			for (int pad = width - n; !left && pad > 0; pad--)
				put(&o, ' ');
			for (int i = 0; i < n; i++)
				put(&o, s[i]);
			break;
		}
		case 'c':
			put(&o, (char)va_arg(ap, int));
			break;
		case '%':
			put(&o, '%');
			break;
		default:
			put(&o, '%');
			if (*fmt)
				put(&o, *fmt);
			else
				fmt--;
			break;
		}
		/* left alignment: pad after the field */
		for (int pad = width - (o.len - start); left && pad > 0; pad--)
			put(&o, ' ');
	}
	if (size > 0)
		o.buf[o.len < size ? o.len : size - 1] = 0;
	return o.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = xa_vsnprintf(buf, (int)size, fmt, ap);
	va_end(ap);
	return n;
}

/* ---- memory: kernel pool -------------------------------------------------------------- */

void *malloc(size_t size)
{
	return ExAllocatePool(size ? size : 1);
}

void *calloc(size_t n, size_t size)
{
	void *p = malloc(n * size);

	if (p)
		memset(p, 0, n * size);
	return p;
}

void free(void *p)
{
	if (p)
		ExFreePool(p);
}
