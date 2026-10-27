// SPDX-License-Identifier: GPL-3.0-or-later
/* Reads a few MT76 registers of the Xbox Wireless Adapter, for comparing hosts. */
#include <stdio.h>
#include <libusb-1.0/libusb.h>

static int rd(libusb_device_handle *h, unsigned char req, unsigned addr, unsigned *val)
{
	unsigned char b[4];
	int n = libusb_control_transfer(h, 0xc0, req, addr >> 16, addr & 0xffff, b, 4, 1000);
	*val = b[0] | b[1] << 8 | b[2] << 16 | (unsigned)b[3] << 24;
	return n;
}

int main(void)
{
	libusb_device_handle *h;
	unsigned v;
	int n;

	libusb_init(NULL);
	h = libusb_open_device_with_vid_pid(NULL, 0x045e, 0x02fe);
	if (!h || libusb_claim_interface(h, 0)) {
		fprintf(stderr, "cannot open/claim the adapter\n");
		return 1;
	}
	n = rd(h, 0x07, 0x0000, &v);
	printf("ASIC_VERSION (req 07, 0x0000): n=%d value=%08x\n", n, v);
	n = rd(h, 0x47, 0x0230, &v);
	printf("FCE_DMA_ADDR (req 47, cfg 0x0230): n=%d value=%08x\n", n, v);
	libusb_release_interface(h, 0);
	libusb_close(h);
	return 0;
}
