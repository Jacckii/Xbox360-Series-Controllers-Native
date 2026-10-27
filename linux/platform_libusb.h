/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <libusb-1.0/libusb.h>

struct xa_usb {
	libusb_device_handle *handle;
};
