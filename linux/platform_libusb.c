// SPDX-License-Identifier: GPL-3.0-or-later
/* platform.h on Linux, with libusb */
#include <stdarg.h>
#include <stdio.h>
#include <sys/random.h>
#include <time.h>

#include <libusb-1.0/libusb.h>

#include "../core/platform.h"
#include "platform_libusb.h"

#define XA_USB_TIMEOUT_MS 1000

int xa_debug;

static int map_error(int ret)
{
	if (ret >= 0)
		return ret;
	if (ret == LIBUSB_ERROR_NO_DEVICE)
		return -XA_ENODEV;
	if (ret == LIBUSB_ERROR_TIMEOUT)
		return -XA_ETIMEDOUT;
	return -XA_EIO;
}

int xa_usb_ctrl(struct xa_usb *usb, u8 request_type, u8 request, u16 value,
		u16 index, void *data, u16 len)
{
	return map_error(libusb_control_transfer(usb->handle, request_type, request, value,
						 index, data, len, XA_USB_TIMEOUT_MS));
}

int xa_usb_bulk_out(struct xa_usb *usb, u8 ep, const void *data, int len)
{
	int done = 0;
	int ret = libusb_bulk_transfer(usb->handle, ep, (unsigned char *)data, len, &done,
				       XA_USB_TIMEOUT_MS);

	if (ret)
		return map_error(ret);
	return done == len ? 0 : -XA_EIO;
}

int xa_random(void *buf, int len)
{
	return getrandom(buf, len, 0) == len ? 0 : -XA_EIO;
}

void xa_sleep_ms(unsigned int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };
	nanosleep(&ts, NULL);
}

u64 xa_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void xa_log(const char *fmt, ...)
{
	static u64 start;
	va_list ap;

	if (!start)
		start = xa_now_ms();
	fprintf(stderr, "[%7.3f] ", (xa_now_ms() - start) / 1000.0);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
