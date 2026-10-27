/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GIP, the Xbox One / Series controller protocol: framing, chunking, the
 * announce/identify/power handshake, security handshake and gamepad input.
 * Ported from xone (bus/protocol.c, auth/auth.c, driver/gamepad.c),
 * Copyright (C) 2021-2023 Severin von Wnuck-Lipinski.
 */
#pragma once

#include "dongle.h"
#include "sha256.h"

#define GIP_AUTH_RANDOM_LEN 32
#define GIP_AUTH_SECRET_LEN 48
#define GIP_AUTH2_PUBKEY_LEN 64

/* gamepad state in GIP's own units */
struct gip_gamepad_state {
	u16 buttons;  /* GIP_GP_BTN_* */
	bool guide;
	u16 trigger_left, trigger_right; /* 0..1023 */
	s16 stick_left_x, stick_left_y, stick_right_x, stick_right_y; /* up = positive */
};

enum gip_gamepad_button {
	GIP_GP_BTN_MENU = BIT(2),
	GIP_GP_BTN_VIEW = BIT(3),
	GIP_GP_BTN_A = BIT(4),
	GIP_GP_BTN_B = BIT(5),
	GIP_GP_BTN_X = BIT(6),
	GIP_GP_BTN_Y = BIT(7),
	GIP_GP_BTN_DPAD_U = BIT(8),
	GIP_GP_BTN_DPAD_D = BIT(9),
	GIP_GP_BTN_DPAD_L = BIT(10),
	GIP_GP_BTN_DPAD_R = BIT(11),
	GIP_GP_BTN_BUMPER_L = BIT(12),
	GIP_GP_BTN_BUMPER_R = BIT(13),
	GIP_GP_BTN_STICK_L = BIT(14),
	GIP_GP_BTN_STICK_R = BIT(15),
};

struct gip_client;

struct gip_ops {
	void (*ready)(struct gip_client *gc);   /* authenticated, input flows */
	void (*input)(struct gip_client *gc, const struct gip_gamepad_state *state);
	void (*gone)(struct gip_client *gc);    /* controller turned off */
};

struct gip_auth {
	struct sha256 transcript;
	u8 last_sent;
	bool done;
	u8 random_host[GIP_AUTH_RANDOM_LEN];
	u8 random_client[GIP_AUTH_RANDOM_LEN];
	u8 master_secret[GIP_AUTH_SECRET_LEN];
};

struct gip_chunk_buf {
	u8 command, options, sequence;
	u32 length;
	u8 *data;
};

struct gip_client {
	struct xa_client *xc;
	const struct gip_ops *ops;
	void *user;

	u8 sequence;
	u16 vendor, product, version;
	bool identified, gamepad;

	struct gip_chunk_buf in;  /* chunked packet being received */
	struct gip_chunk_buf out; /* chunked packet being sent */

	struct gip_auth auth;
	struct gip_gamepad_state state;

	/* rumble: wanted vs. last sent (left, right, left trigger, right trigger) */
	u8 rumble[4], rumble_sent[4];
	u64 rumble_last;
};

struct gip_client *gip_client_new(struct xa_client *xc, const struct gip_ops *ops, void *user);
void gip_client_free(struct gip_client *gc);
void gip_rx(struct gip_client *gc, const u8 *data, int len);

/* motor strengths 0..100: main left/right, impulse triggers left/right */
void gip_set_rumble(struct gip_client *gc, u8 left, u8 right, u8 trigger_left,
		    u8 trigger_right);

/* timers (rumble pacing); call every few milliseconds */
void gip_poll(struct gip_client *gc);
