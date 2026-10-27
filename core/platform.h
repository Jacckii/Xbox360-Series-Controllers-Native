/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * What the core needs from the host. Implemented with libusb on Linux and with the
 * kernel's Usbd calls on the Xbox 360.
 *
 * All core functions run on one driver thread. Received USB buffers are queued by the
 * platform and handed to xa_dongle_rx() from that thread, so the core may make the
 * synchronous calls below at any time.
 */
#pragma once

#include "compat.h"

struct xa_usb;

/* vendor control transfer; returns bytes transferred or a negative error */
int xa_usb_ctrl(struct xa_usb *usb, u8 request_type, u8 request, u16 value,
		u16 index, void *data, u16 len);

/* bulk OUT, waits for completion; returns 0 or a negative error */
int xa_usb_bulk_out(struct xa_usb *usb, u8 ep, const void *data, int len);

/* cryptographically secure random bytes; returns 0 or a negative error */
int xa_random(void *buf, int len);

void xa_sleep_ms(unsigned int ms);
u64 xa_now_ms(void);
void xa_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* 0 = quiet, 1 = frames and channel changes, 2 = every received message */
extern int xa_debug;

#define USB_DIR_OUT 0x00
#define USB_DIR_IN 0x80
#define USB_TYPE_VENDOR 0x40
