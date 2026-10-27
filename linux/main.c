// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * xadapter: drives the Xbox Wireless Adapter from userspace with libusb.
 * The adapter must not be bound to the xone kernel driver (unload xone_dongle).
 *
 *   xadapter /lib/firmware/xone_dongle_02fe.bin
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "../core/gip.h"
#include "platform_libusb.h"

#define VID 0x045e
#define PID 0x02fe
#define IN_TRANSFERS 4

/* received buffers, queued by libusb callbacks and processed by the main loop */
struct rx_buf {
	struct rx_buf *next;
	int len;
	u8 data[];
};

static struct rx_buf *rx_head, **rx_tail = &rx_head;
static volatile sig_atomic_t stop;
static struct xa_usb usb;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static void LIBUSB_CALL on_in_done(struct libusb_transfer *t)
{
	static int dumped;

	if (t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length > 0 &&
	    getenv("XA_RAWDUMP") && dumped < 4) {
		char path[64];
		FILE *f;

		snprintf(path, sizeof(path), "/tmp/xa_raw_%02x_%d.bin", t->endpoint, dumped++);
		f = fopen(path, "wb");
		if (f) {
			fwrite(t->buffer, 1, t->actual_length, f);
			fclose(f);
		}
	}

	if (t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length > 0) {
		struct rx_buf *b = malloc(sizeof(*b) + t->actual_length);

		if (b) {
			b->next = NULL;
			b->len = t->actual_length;
			memcpy(b->data, t->buffer, t->actual_length);
			*rx_tail = b;
			rx_tail = &b->next;
		}
	}

	if (t->status == LIBUSB_TRANSFER_NO_DEVICE || t->status == LIBUSB_TRANSFER_CANCELLED)
		return;
	if (libusb_submit_transfer(t))
		xa_log("usb: resubmit on %02x failed\n", t->endpoint);
}

static int start_in(u8 ep, int len)
{
	for (int i = 0; i < IN_TRANSFERS; i++) {
		struct libusb_transfer *t = libusb_alloc_transfer(0);
		u8 *buf = malloc(len);

		if (!t || !buf)
			return -XA_ENOMEM;
		libusb_fill_bulk_transfer(t, usb.handle, ep, buf, len, on_in_done, NULL, 0);
		if (libusb_submit_transfer(t))
			return -XA_EIO;
	}
	return 0;
}

static int open_adapter(int wait_ms)
{
	u64 until = xa_now_ms() + wait_ms;

	do {
		usb.handle = libusb_open_device_with_vid_pid(NULL, VID, PID);
		if (usb.handle) {
			libusb_set_auto_detach_kernel_driver(usb.handle, 1);
			if (!libusb_claim_interface(usb.handle, 0))
				return 0;
			xa_log("usb: claim failed (is xone_dongle still loaded?)\n");
			libusb_close(usb.handle);
			usb.handle = NULL;
			return -XA_EIO;
		}
		xa_sleep_ms(200);
	} while (xa_now_ms() < until);

	xa_log("usb: adapter %04x:%04x not found\n", VID, PID);
	return -XA_ENODEV;
}

static void close_adapter(void)
{
	if (!usb.handle)
		return;
	libusb_release_interface(usb.handle, 0);
	libusb_close(usb.handle);
	usb.handle = NULL;
}

static u8 *read_file(const char *path, u32 *len)
{
	FILE *f = fopen(path, "rb");
	u8 *buf;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(n);
	if (buf && fread(buf, 1, n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	*len = n;
	return buf;
}

/* ---- controllers: print what they do ---------------------------------------------- */

static void pad_ready(struct gip_client *gc)
{
	xa_log("pad %d: ready, press buttons\n", gc->xc->wcid);
}

static void pad_input(struct gip_client *gc, const struct gip_gamepad_state *s)
{
	/* rumble test: triggers drive their impulse motors, A/B the left/right main motors */
	gip_set_rumble(gc, s->buttons & GIP_GP_BTN_A ? 60 : 0, s->buttons & GIP_GP_BTN_B ? 60 : 0,
		       s->trigger_left * 100 / 1023, s->trigger_right * 100 / 1023);

	static const struct { u16 bit; const char *name; } names[] = {
		{ GIP_GP_BTN_A, "A" }, { GIP_GP_BTN_B, "B" }, { GIP_GP_BTN_X, "X" },
		{ GIP_GP_BTN_Y, "Y" }, { GIP_GP_BTN_BUMPER_L, "LB" }, { GIP_GP_BTN_BUMPER_R, "RB" },
		{ GIP_GP_BTN_VIEW, "View" }, { GIP_GP_BTN_MENU, "Menu" },
		{ GIP_GP_BTN_STICK_L, "LS" }, { GIP_GP_BTN_STICK_R, "RS" },
		{ GIP_GP_BTN_DPAD_U, "Up" }, { GIP_GP_BTN_DPAD_D, "Down" },
		{ GIP_GP_BTN_DPAD_L, "Left" }, { GIP_GP_BTN_DPAD_R, "Right" },
	};
	/* last printed state per pad, to print only real changes */
	static struct gip_gamepad_state last[XA_DONGLE_MAX_CLIENTS + 1];
	struct gip_gamepad_state *l = &last[gc->xc->wcid];
	char line[160] = "";

#define MOVED(a, b, t) (abs((int)(a) - (int)(b)) > (t))
	if (s->buttons == l->buttons && s->guide == l->guide &&
	    !MOVED(s->trigger_left, l->trigger_left, 100) &&
	    !MOVED(s->trigger_right, l->trigger_right, 100) &&
	    !MOVED(s->stick_left_x, l->stick_left_x, 6000) &&
	    !MOVED(s->stick_left_y, l->stick_left_y, 6000) &&
	    !MOVED(s->stick_right_x, l->stick_right_x, 6000) &&
	    !MOVED(s->stick_right_y, l->stick_right_y, 6000))
		return;
	*l = *s;

	for (size_t i = 0; i < ARRAY_SIZE(names); i++)
		if (s->buttons & names[i].bit)
			snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s ", names[i].name);
	if (s->guide)
		strcat(line, "Guide ");
	if (!*line)
		strcat(line, "(no buttons) ");
	xa_log("pad %d: %s LT %4u RT %4u  L %6d,%6d  R %6d,%6d\n", gc->xc->wcid, line,
	       s->trigger_left, s->trigger_right, s->stick_left_x, s->stick_left_y,
	       s->stick_right_x, s->stick_right_y);
}

static void pad_gone(struct gip_client *gc)
{
	xa_log("pad %d: off\n", gc->xc->wcid);
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
	gip_client_free(client->priv);
	client->priv = NULL;
}

static void client_data(struct xa_client *client, const u8 *data, int len)
{
	if (client->priv)
		gip_rx(client->priv, data, len);
}

static const struct xa_dongle_ops ops = {
	.client_added = client_added,
	.client_removed = client_removed,
	.client_data = client_data,
};

int main(int argc, char **argv)
{
	static struct xa_dongle dongle;
	u32 fw_len;
	u8 *fw;
	int err;

	if (argc < 2) {
		fprintf(stderr, "usage: %s FIRMWARE\n", argv[0]);
		return 2;
	}
	fw = read_file(argv[1], &fw_len);
	if (!fw) {
		fprintf(stderr, "cannot read %s: %s\n", argv[1], strerror(errno));
		return 2;
	}

	if (getenv("XA_DEBUG"))
		xa_debug = atoi(getenv("XA_DEBUG"));

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	if (libusb_init(NULL))
		return 1;

	dongle.ops = &ops;
	dongle.mt.usb = &usb;
	dongle.no_scan = getenv("XA_NOSCAN") != NULL;
	dongle.no_led = getenv("XA_NOLED") != NULL;

	/* a cold firmware upload makes the chip re-enumerate: reopen and go again */
	for (int attempt = 0;; attempt++) {
		err = open_adapter(attempt ? 5000 : 0);
		if (err)
			return 1;
		err = xa_mt_load_firmware(&dongle.mt, fw, fw_len);
		if (err != XA_MT_REENUMERATE)
			break;
		xa_log("usb: adapter re-enumerating after firmware upload\n");
		close_adapter();
		xa_sleep_ms(1000);
		if (attempt == 2) {
			err = -XA_ETIMEDOUT;
			break;
		}
	}
	if (err) {
		xa_log("firmware load failed: %d\n", err);
		return 1;
	}
	xa_log("firmware running\n");

	if (start_in(XA_MT_EP_IN_CMD | 0x80, XA_DONGLE_LEN_CMD_PKT) ||
	    start_in(XA_MT_EP_IN_WLAN | 0x80, XA_DONGLE_LEN_WLAN_PKT)) {
		xa_log("usb: starting IN transfers failed\n");
		return 1;
	}

	err = xa_dongle_start(&dongle);
	if (err) {
		xa_log("radio init failed: %d\n", err);
		return 1;
	}
	xa_log("radio up, waiting for controllers (Ctrl+C to stop)\n");

	while (!stop) {
		struct timeval tv = { 0, 10000 };

		libusb_handle_events_timeout(NULL, &tv);
		while (rx_head) {
			struct rx_buf *b = rx_head;

			rx_head = b->next;
			if (!rx_head)
				rx_tail = &rx_head;
			xa_dongle_rx(&dongle, b->data, b->len);
			free(b);
		}
		xa_dongle_poll(&dongle);
		for (int i = 0; i < XA_DONGLE_MAX_CLIENTS; i++)
			if (dongle.clients[i] && dongle.clients[i]->priv)
				gip_poll(dongle.clients[i]->priv);
	}

	xa_log("stopping\n");
	xa_dongle_stop(&dongle);
	close_adapter();
	libusb_exit(NULL);
	return 0;
}
