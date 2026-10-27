/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * platform.h on the Xbox 360: Usbd transfers, kernel pool, the kernel clock and RNG.
 *
 * Usbd completes transfers on its DPC; the core runs on the driver thread. The thread
 * never calls Usbd itself: it posts requests to a call queue that a DPC on Usbd's
 * processor executes. A synchronous call posts a transfer and sleeps until its
 * completion flag is set.
 */
#pragma once

#include "../core/compat.h"
#include "usbd.h"

/* a transfer block plus what its completion reports; the block must stay first */
struct xa_xfer {
	struct usbd_control_transfer c;
	volatile int done;
	volatile long status;
	volatile u32 length_after; /* the block's length field after completion */
	u8 *buffer;
	u32 size;
	u8 ep;     /* IN transfers: the endpoint, for the receive ring */
};

#define XA_XFER_IN_COUNT 2 /* outstanding transfers per IN endpoint */

struct xa_usb {
	void *volatile node;  /* the adapter's device node, 0 = none */
	volatile int gone;    /* set when the kernel removes the device */
	volatile int broken;  /* a transfer timed out: its block may still be queued */

	struct xa_xfer ctrl;      /* default endpoint */
	struct xa_xfer bulk_out;  /* XA_MT_EP_OUT */
	struct xa_xfer in_cmd[XA_XFER_IN_COUNT];
	struct xa_xfer in_wlan[XA_XFER_IN_COUNT];
	long open_status[4];      /* default, out, cmd, wlan */
};

/* kernel entry points, through their NV64 thunks */
long XaTransferDone(struct usbd_transfer *t, long status);
long XaInDone(struct usbd_transfer *t, long status);
void XaCallDpc(struct kdpc360 *dpc, void *context, void *arg1, void *arg2);

/* sets up the call queue's DPC; needs ke_dpc and usbd filled in */
void xa_usbd_calls_init(void);

/* queue a transfer whose block is filled in; 0 or a negative error. Completion (or a
 * failure to queue) is reported through x->done and x->status. */
int xa_usb_queue(struct xa_usb *usb, struct xa_xfer *x, void *buffer, u32 length);

/* run fn(arg) on Usbd's DPC, serialised with Usbd and the class-driver callbacks */
void xa_usbd_run(void (*fn)(void *), void *arg);

/* steady-state OUT mode: on = xa_usb_bulk_out submits without blocking the driver thread
 * (fire-and-forget). Off during firmware/radio bring-up, where OUT is synchronous. */
void xa_usb_out_async(int on);

/*
 * Bulk IN, kept flowing as xone's URBs are: each completion (on Usbd's DPC) copies the
 * received frame into a ring and re-queues the transfer at once, so the adapter can
 * always deliver, even while the thread is inside a synchronous call. The thread
 * drains the ring with xa_usb_rx_next().
 */
int xa_usb_start_in(struct xa_usb *usb, struct xa_xfer *x, u8 ep, u8 *buf, u32 size);

/* the most one EHCI transfer descriptor covers, counted from the buffer's page start.
 * An IN transfer must fit in one: Usbd sets no alternate-next pointer, so after a short
 * packet the controller would carry on into the next descriptor of the same transfer. */
#define XA_USB_IN_MAX 0x5000

/* the next received frame: its length (0 = none), data and endpoint. The data stays
 * valid until the next call. */
int xa_usb_rx_next(const u8 **data, u8 *ep);

/* frames dropped because the ring was full; IN completions with an error status */
extern volatile u32 xa_rx_dropped, xa_rx_errors;

/* re-enumerate a device: the adapter (usb, or NULL for any other node). Only safe
 * with none of its transfers outstanding. 0 or a negative error */
int xa_usbd_reset(struct xa_usb *usb, void *node);

/* the log, readable over xbdm: magic "XLOG", ring size, bytes written so far, then text */
struct xa_log_ring {
	u32 magic;
	u32 size;
	volatile u32 written;
	char text[0x8000];
};
extern struct xa_log_ring xa_log_ring;

int xa_vsnprintf(char *buf, int size, const char *fmt, __builtin_va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...);
