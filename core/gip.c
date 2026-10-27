// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * GIP, the Xbox One / Series controller protocol: framing, chunking, the
 * announce/identify/power handshake, security handshake (v2, ECDH) and gamepad input.
 * Ported from xone (bus/protocol.c, auth/auth.c, auth/crypto.c, driver/gamepad.c),
 * Copyright (C) 2021-2023 Severin von Wnuck-Lipinski.
 *
 * xone runs the key exchange in work items after the received packet is hashed into
 * the transcript; here everything is synchronous, so received handshake data is hashed
 * before replying, except the client's Finished message, which is checked against the
 * transcript without itself.
 */
#include <stdlib.h>

#include "gip.h"
#include "../third_party/micro-ecc/uECC.h"

#define GIP_HDR_CLIENT_ID GENMASK(3, 0)
#define GIP_HDR_MIN_LENGTH 3

/* max length, even for wireless packets (except audio) */
#define GIP_PKT_MAX_LENGTH 58
/* reliable packet transmission coalesce count */
#define GIP_PKT_COALESCE_COUNT 5
#define GIP_CHUNK_BUF_MAX_LENGTH 0xffff

#define GIP_STATUS_CONNECTED BIT(7)
#define GIP_VKEY_LEFT_WIN 0x5b
#define GIP_LED_BRIGHTNESS_DEFAULT 20

enum gip_command {
	GIP_CMD_ACKNOWLEDGE = 0x01,
	GIP_CMD_ANNOUNCE = 0x02,
	GIP_CMD_STATUS = 0x03,
	GIP_CMD_IDENTIFY = 0x04,
	GIP_CMD_POWER = 0x05,
	GIP_CMD_AUTHENTICATE = 0x06,
	GIP_CMD_VIRTUAL_KEY = 0x07,
	GIP_CMD_RUMBLE = 0x09,
	GIP_CMD_LED = 0x0a,
	GIP_CMD_INPUT = 0x20,
};

enum gip_option {
	GIP_OPT_ACKNOWLEDGE = BIT(4),
	GIP_OPT_INTERNAL = BIT(5),
	GIP_OPT_CHUNK_START = BIT(6),
	GIP_OPT_CHUNK = BIT(7),
};

struct gip_header {
	u8 command;
	u8 options;
	u8 sequence;
	u32 packet_length;
	u32 chunk_offset;
};

/* ---- framing -------------------------------------------------------------------- */

static int encode_varint(u8 *buf, u32 val)
{
	int i;

	for (i = 0; i < 4; i++) {
		buf[i] = val;
		if (val > GENMASK(6, 0))
			buf[i] |= BIT(7);
		val >>= 7;
		if (!val)
			break;
	}
	return i + 1;
}

static int decode_varint(const u8 *data, int len, u32 *val)
{
	int i;

	for (i = 0; i < 4 && i < len; i++) {
		*val |= (u32)(data[i] & GENMASK(6, 0)) << (i * 7);
		if (!(data[i] & BIT(7)))
			break;
	}
	return i + 1;
}

static int actual_header_length(const struct gip_header *hdr)
{
	u32 pkt_len = hdr->packet_length, chunk_offset = hdr->chunk_offset;
	int len = GIP_HDR_MIN_LENGTH;

	do {
		len++;
		pkt_len >>= 7;
	} while (pkt_len);

	if (hdr->options & GIP_OPT_CHUNK) {
		while (chunk_offset) {
			len++;
			chunk_offset >>= 7;
		}
	}
	return len;
}

static int header_length(const struct gip_header *hdr)
{
	int len = actual_header_length(hdr);

	/* round up to an even length */
	return len + (len % 2);
}

static void encode_header(const struct gip_header *hdr, u8 *buf)
{
	int len = 0;

	buf[len++] = hdr->command;
	buf[len++] = hdr->options;
	buf[len++] = hdr->sequence;
	len += encode_varint(buf + len, hdr->packet_length);

	/* header length must be even */
	if (actual_header_length(hdr) % 2) {
		buf[len - 1] |= BIT(7);
		buf[len++] = 0;
	}

	if (hdr->options & GIP_OPT_CHUNK)
		encode_varint(buf + len, hdr->chunk_offset);
}

static int decode_header(struct gip_header *hdr, const u8 *data, int len)
{
	int hdr_len = 0;

	hdr->command = data[hdr_len++];
	hdr->options = data[hdr_len++];
	hdr->sequence = data[hdr_len++];
	hdr->packet_length = 0;
	hdr->chunk_offset = 0;

	hdr_len += decode_varint(data + hdr_len, len - hdr_len, &hdr->packet_length);
	if (hdr->options & GIP_OPT_CHUNK)
		hdr_len += decode_varint(data + hdr_len, len - hdr_len, &hdr->chunk_offset);
	return hdr_len;
}

/* ---- sending -------------------------------------------------------------------- */

static int send_simple(struct gip_client *gc, struct gip_header *hdr, const u8 *data)
{
	u8 buf[16 + GIP_PKT_MAX_LENGTH];
	int hdr_len = header_length(hdr);

	if (hdr_len + hdr->packet_length > sizeof(buf))
		return -XA_ENOSPC;

	/* sequence numbers are never zero */
	while (!hdr->sequence)
		hdr->sequence = gc->sequence++;

	encode_header(hdr, buf);
	if (data && hdr->packet_length)
		memcpy(buf + hdr_len, data, hdr->packet_length);

	return xa_dongle_send(gc->xc, buf, hdr_len + hdr->packet_length, XA_DONGLE_QUEUE_DATA);
}

static void free_chunk(struct gip_chunk_buf *buf)
{
	free(buf->data);
	memset(buf, 0, sizeof(*buf));
}

static int send_pkt(struct gip_client *gc, struct gip_header *hdr, const u8 *data)
{
	u32 total = hdr->packet_length;
	int err;

	if (total <= GIP_PKT_MAX_LENGTH)
		return send_simple(gc, hdr, data);

	/* the first chunk's offset is the total length; the rest follow its acknowledgment */
	hdr->options |= GIP_OPT_ACKNOWLEDGE | GIP_OPT_CHUNK_START | GIP_OPT_CHUNK;
	hdr->chunk_offset = total;
	hdr->packet_length = GIP_PKT_MAX_LENGTH;

	err = send_simple(gc, hdr, data);
	if (err)
		return err;

	free_chunk(&gc->out);
	gc->out.data = malloc(total);
	if (!gc->out.data)
		return -XA_ENOMEM;
	memcpy(gc->out.data, data, total);
	gc->out.command = hdr->command;
	gc->out.options = hdr->options & ~(GIP_OPT_ACKNOWLEDGE | GIP_OPT_CHUNK_START);
	gc->out.sequence = hdr->sequence;
	gc->out.length = total;
	return 0;
}

static int send_cmd(struct gip_client *gc, u8 command, u8 options, const void *data, u32 len)
{
	struct gip_header hdr = {
		.command = command,
		.options = options,
		.packet_length = len,
	};

	return send_pkt(gc, &hdr, data);
}

static int acknowledge(struct gip_client *gc, const struct gip_header *ack)
{
	u32 len = ack->chunk_offset + ack->packet_length;
	struct gip_header hdr = {
		.command = GIP_CMD_ACKNOWLEDGE,
		.options = GIP_OPT_INTERNAL,
		.sequence = ack->sequence,
		.packet_length = 9,
	};
	u8 pkt[9] = { 0 };

	pkt[1] = ack->command;
	pkt[2] = GIP_OPT_INTERNAL;
	put_le16(pkt + 3, len);
	if ((ack->options & GIP_OPT_CHUNK) && gc->in.data)
		put_le16(pkt + 7, gc->in.length - len);

	return send_pkt(gc, &hdr, pkt);
}

static int send_remaining_chunks(struct gip_client *gc, u32 offset)
{
	struct gip_header hdr = {
		.command = gc->out.command,
		.options = gc->out.options,
		.sequence = gc->out.sequence,
	};
	u32 left = gc->out.length - offset;
	int err;

	for (int n = GIP_PKT_COALESCE_COUNT; left && n; n--) {
		/* the last chunk asks for an acknowledgment */
		if (left <= GIP_PKT_MAX_LENGTH)
			hdr.options |= GIP_OPT_ACKNOWLEDGE;

		hdr.packet_length = MIN(left, (u32)GIP_PKT_MAX_LENGTH);
		hdr.chunk_offset = gc->out.length - left;
		err = send_simple(gc, &hdr, gc->out.data + hdr.chunk_offset);
		if (err)
			return err;
		left -= hdr.packet_length;
	}
	return 0;
}

static int handle_acknowledge(struct gip_client *gc, const u8 *data, u32 len)
{
	struct gip_header hdr;
	u32 acked;

	if (len != 9 || !gc->out.data || data[1] != gc->out.command)
		return 0;

	/* the length comes from the device: never trust it to index the buffer */
	acked = get_le16(data + 3);
	if (acked < gc->out.length)
		return send_remaining_chunks(gc, acked);

	/* an empty chunk completes the transfer */
	hdr = (struct gip_header){
		.command = gc->out.command,
		.options = gc->out.options,
		.sequence = gc->out.sequence,
		.packet_length = 0,
		.chunk_offset = gc->out.length,
	};
	free_chunk(&gc->out);
	return send_simple(gc, &hdr, NULL);
}

/* ---- security handshake (auth v2) ------------------------------------------------- */

enum gip_auth_command {
	GIP_AUTH_CMD_HOST_HELLO = 0x01,
	GIP_AUTH_CMD_CLIENT_HELLO = 0x02,
	GIP_AUTH_CMD_CLIENT_CERTIFICATE = 0x03,
	GIP_AUTH_CMD_HOST_SECRET = 0x05,
	GIP_AUTH_CMD_HOST_FINISH = 0x07,
	GIP_AUTH_CMD_CLIENT_FINISH = 0x08,

	GIP_AUTH2_CMD_HOST_HELLO = 0x21,
	GIP_AUTH2_CMD_CLIENT_HELLO = 0x22,
	GIP_AUTH2_CMD_CLIENT_CERTIFICATE = 0x23,
	GIP_AUTH2_CMD_CLIENT_PUBKEY = 0x24,
	GIP_AUTH2_CMD_HOST_PUBKEY = 0x25,
	GIP_AUTH2_CMD_HOST_FINISH = 0x26,
	GIP_AUTH2_CMD_CLIENT_FINISH = 0x27,
};

#define GIP_AUTH_OPT_ACKNOWLEDGE BIT(0)
#define GIP_AUTH_OPT_REQUEST BIT(1)
#define GIP_AUTH_OPT_FROM_HOST BIT(6)

#define AUTH_HS_LEN 6      /* context, options, error, command, be16 length */
#define AUTH_DATA_HDR_LEN 4 /* command, version, be16 length */
#define AUTH_HDR_LEN (AUTH_HS_LEN + AUTH_DATA_HDR_LEN)
#define AUTH_TRAILER_LEN 8

/* sizes of the client messages that are requested */
#define AUTH2_CLIENT_HELLO_LEN (32 + 108 + 32)
#define AUTH2_CLIENT_CERT_LEN (4 + 136 + 32 + 20 + 576)
#define AUTH2_CLIENT_PUBKEY_LEN (64 + 64)
#define AUTH2_CLIENT_FINISH_LEN (32 + 32)
#define AUTH_CLIENT_HELLO_LEN (32 + 48)

#define AUTH_TRANSCRIPT_LEN 32
#define AUTH_SESSION_KEY_LEN 16

static void put_be16(u8 *p, u16 v)
{
	p[0] = v >> 8;
	p[1] = v;
}

/* pkt has room for the headers in front and the trailer behind */
static int auth_send(struct gip_client *gc, u8 cmd, u8 *pkt, u16 len)
{
	u16 data_len = len - AUTH_HS_LEN - AUTH_TRAILER_LEN;

	pkt[0] = 0x00; /* handshake context */
	pkt[1] = GIP_AUTH_OPT_ACKNOWLEDGE | GIP_AUTH_OPT_FROM_HOST;
	pkt[2] = 0x00;
	pkt[3] = cmd;
	put_be16(pkt + 4, data_len);
	pkt[6] = cmd;
	pkt[7] = cmd >= GIP_AUTH2_CMD_HOST_HELLO ? 0x02 : 0x01;
	put_be16(pkt + 8, data_len - AUTH_DATA_HDR_LEN);

	gc->auth.last_sent = cmd;
	sha256_update(&gc->auth.transcript, pkt + AUTH_HS_LEN, data_len);

	return send_cmd(gc, GIP_CMD_AUTHENTICATE, GIP_OPT_INTERNAL | GIP_OPT_ACKNOWLEDGE, pkt, len);
}

static int auth_request(struct gip_client *gc, u8 cmd, u16 len)
{
	u8 req[AUTH_HS_LEN + AUTH_TRAILER_LEN] = { 0 };

	req[1] = GIP_AUTH_OPT_REQUEST | GIP_AUTH_OPT_FROM_HOST;
	req[3] = cmd;
	put_be16(req + 4, len + AUTH_DATA_HDR_LEN);
	return send_cmd(gc, GIP_CMD_AUTHENTICATE, GIP_OPT_INTERNAL | GIP_OPT_ACKNOWLEDGE,
			req, sizeof(req));
}

static int auth_send_hello(struct gip_client *gc, bool v2)
{
	/* random, then 8 (v1) or 4 (v2) unknown bytes */
	u8 pkt[AUTH_HDR_LEN + GIP_AUTH_RANDOM_LEN + 8 + AUTH_TRAILER_LEN] = { 0 };
	u16 len = AUTH_HDR_LEN + GIP_AUTH_RANDOM_LEN + (v2 ? 4 : 8) + AUTH_TRAILER_LEN;
	int err = xa_random(gc->auth.random_host, GIP_AUTH_RANDOM_LEN);

	if (err)
		return err;
	memcpy(pkt + AUTH_HDR_LEN, gc->auth.random_host, GIP_AUTH_RANDOM_LEN);
	return auth_send(gc, v2 ? GIP_AUTH2_CMD_HOST_HELLO : GIP_AUTH_CMD_HOST_HELLO, pkt, len);
}

static void auth_randoms(struct gip_client *gc, u8 out[2 * GIP_AUTH_RANDOM_LEN])
{
	memcpy(out, gc->auth.random_host, GIP_AUTH_RANDOM_LEN);
	memcpy(out + GIP_AUTH_RANDOM_LEN, gc->auth.random_client, GIP_AUTH_RANDOM_LEN);
}

static int auth_send_finish(struct gip_client *gc, u8 cmd)
{
	u8 pkt[AUTH_HDR_LEN + AUTH_TRANSCRIPT_LEN + AUTH_TRAILER_LEN] = { 0 };
	u8 transcript[SHA256_LEN];

	sha256_peek(&gc->auth.transcript, transcript);
	gip_prf("Host Finished", gc->auth.master_secret, GIP_AUTH_SECRET_LEN, transcript,
		sizeof(transcript), pkt + AUTH_HDR_LEN, AUTH_TRANSCRIPT_LEN);
	return auth_send(gc, cmd, pkt, sizeof(pkt));
}

static int auth_handle_ack(struct gip_client *gc)
{
	switch (gc->auth.last_sent) {
	case GIP_AUTH2_CMD_HOST_HELLO:
		return auth_request(gc, GIP_AUTH2_CMD_CLIENT_HELLO, AUTH2_CLIENT_HELLO_LEN);
	case GIP_AUTH2_CMD_HOST_PUBKEY:
		return auth_send_finish(gc, GIP_AUTH2_CMD_HOST_FINISH);
	case GIP_AUTH2_CMD_HOST_FINISH:
		return auth_request(gc, GIP_AUTH2_CMD_CLIENT_FINISH, AUTH2_CLIENT_FINISH_LEN);
	case GIP_AUTH_CMD_HOST_HELLO:
		return auth_request(gc, GIP_AUTH_CMD_CLIENT_HELLO, AUTH_CLIENT_HELLO_LEN);
	default:
		return -XA_EINVAL;
	}
}

static int rng(uint8_t *dest, unsigned size)
{
	return xa_random(dest, size) == 0;
}

/* ECDH P-256 with the client's key, then the master secret; answers with our key */
static int auth2_exchange(struct gip_client *gc, const u8 *client_pubkey)
{
	const struct uECC_Curve_t *curve = uECC_secp256r1();
	u8 pkt[AUTH_HDR_LEN + GIP_AUTH2_PUBKEY_LEN + AUTH_TRAILER_LEN] = { 0 };
	u8 priv[32], secret[32], secret_hash[SHA256_LEN], randoms[2 * GIP_AUTH_RANDOM_LEN];
	struct sha256 sha;

	uECC_set_rng(rng);
	if (!uECC_valid_public_key(client_pubkey, curve)) {
		xa_log("gip: client public key not on P-256\n");
		return -XA_EINVAL;
	}
	if (!uECC_make_key(pkt + AUTH_HDR_LEN, priv, curve) ||
	    !uECC_shared_secret(client_pubkey, priv, secret, curve))
		return -XA_EIO;

	sha256_init(&sha);
	sha256_update(&sha, secret, sizeof(secret));
	sha256_final(&sha, secret_hash);

	auth_randoms(gc, randoms);
	gip_prf("Master Secret", secret_hash, sizeof(secret_hash), randoms, sizeof(randoms),
		gc->auth.master_secret, GIP_AUTH_SECRET_LEN);

	memset(priv, 0, sizeof(priv));
	memset(secret, 0, sizeof(secret));
	return auth_send(gc, GIP_AUTH2_CMD_HOST_PUBKEY, pkt, sizeof(pkt));
}

static int auth_complete(struct gip_client *gc)
{
	static const u8 control_complete[2] = { 0x01, 0x00 }; /* control context, complete */
	u8 randoms[2 * GIP_AUTH_RANDOM_LEN], key[AUTH_SESSION_KEY_LEN];
	int err;

	auth_randoms(gc, randoms);
	gip_prf("EXPORTER DAWN data channel session key for controller", gc->auth.master_secret,
		GIP_AUTH_SECRET_LEN, randoms, sizeof(randoms), key, sizeof(key));

	err = send_cmd(gc, GIP_CMD_AUTHENTICATE, GIP_OPT_INTERNAL, control_complete,
		       sizeof(control_complete));
	if (!err)
		err = xa_dongle_set_key(gc->xc, key, sizeof(key));
	memset(key, 0, sizeof(key));
	if (err)
		return err;

	gc->auth.done = true;
	xa_log("gip: client %d authenticated\n", gc->xc->wcid);
	if (gc->ops && gc->ops->ready)
		gc->ops->ready(gc);
	return 0;
}

static int auth_rx(struct gip_client *gc, const u8 *data, u32 len)
{
	const u8 *body = data + AUTH_HDR_LEN;
	u32 body_len = len - AUTH_HDR_LEN;
	u8 cmd;

	if (len < AUTH_HS_LEN)
		return -XA_EINVAL;
	if (data[2]) {
		xa_log("gip: client %d reports handshake error %02x\n", gc->xc->wcid, data[2]);
		return -XA_EIO;
	}

	if (data[1] & GIP_AUTH_OPT_ACKNOWLEDGE) {
		if (data[3] == 0x01)
			return auth_handle_ack(gc);
		xa_log("gip: client %d handshake failed: %02x\n", gc->xc->wcid, data[3]);
		return -XA_EIO;
	}

	if (len < AUTH_HDR_LEN)
		return -XA_EINVAL;

	/* the client answers a v1 hello with a v2 one: restart the transcript */
	if (data[3] != data[6]) {
		sha256_init(&gc->auth.transcript);
		return auth_send_hello(gc, true);
	}

	cmd = data[6];
	if (cmd == GIP_AUTH2_CMD_CLIENT_FINISH || cmd == GIP_AUTH_CMD_CLIENT_FINISH) {
		u8 transcript[SHA256_LEN], finished[AUTH_TRANSCRIPT_LEN];

		if (body_len < AUTH2_CLIENT_FINISH_LEN)
			return -XA_EINVAL;
		sha256_peek(&gc->auth.transcript, transcript);
		gip_prf("Device Finished", gc->auth.master_secret, GIP_AUTH_SECRET_LEN, transcript,
			sizeof(transcript), finished, sizeof(finished));
		if (memcmp(body, finished, sizeof(finished))) {
			xa_log("gip: client %d transcript mismatch\n", gc->xc->wcid);
			return -XA_EIO;
		}
		sha256_update(&gc->auth.transcript, data + AUTH_HS_LEN, len - AUTH_HS_LEN);
		return auth_complete(gc);
	}

	sha256_update(&gc->auth.transcript, data + AUTH_HS_LEN, len - AUTH_HS_LEN);

	switch (cmd) {
	case GIP_AUTH2_CMD_CLIENT_HELLO:
		if (body_len < AUTH2_CLIENT_HELLO_LEN)
			return -XA_EINVAL;
		memcpy(gc->auth.random_client, body, GIP_AUTH_RANDOM_LEN);
		return auth_request(gc, GIP_AUTH2_CMD_CLIENT_CERTIFICATE, AUTH2_CLIENT_CERT_LEN);
	case GIP_AUTH2_CMD_CLIENT_CERTIFICATE:
		if (body_len < AUTH2_CLIENT_CERT_LEN)
			return -XA_EINVAL;
		return auth_request(gc, GIP_AUTH2_CMD_CLIENT_PUBKEY, AUTH2_CLIENT_PUBKEY_LEN);
	case GIP_AUTH2_CMD_CLIENT_PUBKEY:
		if (body_len < AUTH2_CLIENT_PUBKEY_LEN)
			return -XA_EINVAL;
		return auth2_exchange(gc, body);
	case GIP_AUTH_CMD_CLIENT_HELLO:
	case GIP_AUTH_CMD_CLIENT_CERTIFICATE:
		xa_log("gip: client %d uses the v1 (RSA) handshake, not supported yet\n",
		       gc->xc->wcid);
		return -XA_EINVAL;
	}
	return -XA_EINVAL;
}

/* ---- client lifecycle ------------------------------------------------------------ */

static int start_gamepad(struct gip_client *gc)
{
	static const u8 power_on[1] = { 0x00 };
	static const u8 led_on[3] = { 0x00, 0x01, GIP_LED_BRIGHTNESS_DEFAULT };
	/* all motors off; also required by some gamepads before they send input */
	static const u8 rumble_stop[9] = { 0x00, 0x0f, 0, 0, 0, 0, 0xff, 0x00, 0xeb };
	int err;

	err = send_cmd(gc, GIP_CMD_POWER, GIP_OPT_INTERNAL, power_on, sizeof(power_on));
	if (!err)
		err = send_cmd(gc, GIP_CMD_LED, GIP_OPT_INTERNAL, led_on, sizeof(led_on));
	if (!err) {
		sha256_init(&gc->auth.transcript);
		err = auth_send_hello(gc, false);
	}
	if (!err)
		err = send_cmd(gc, GIP_CMD_RUMBLE, 0, rumble_stop, sizeof(rumble_stop));
	return err;
}

static int handle_announce(struct gip_client *gc, const u8 *data, u32 len)
{
	if (len != 28)
		return -XA_EINVAL;

	if (!gc->vendor && !gc->product) {
		gc->vendor = get_le16(data + 8);
		gc->product = get_le16(data + 10);
		gc->version = get_le16(data + 12) << 8 | get_le16(data + 14);
		xa_log("gip: client %d is %04x:%04x, firmware %u.%u.%u.%u\n", gc->xc->wcid,
		       gc->vendor, gc->product, get_le16(data + 12), get_le16(data + 14),
		       get_le16(data + 16), get_le16(data + 18));
	}

	if (gc->identified)
		return 0;
	return send_cmd(gc, GIP_CMD_IDENTIFY, GIP_OPT_INTERNAL, NULL, 0);
}

static int handle_identify(struct gip_client *gc, const u8 *data, u32 len)
{
	const u8 *base = data + 16; /* offsets are relative to after the unknown header */
	u32 blen, off;
	u8 count;

	if (len < 32 || gc->identified)
		return 0;

	blen = len - 16;
	off = get_le16(data + 26); /* classes */
	if (off + 1 > blen)
		return -XA_EINVAL;

	count = base[off++];
	for (u8 i = 0; i < count; i++) {
		u16 str_len;

		if (off + 2 > blen)
			return -XA_EINVAL;
		str_len = get_le16(base + off);
		off += 2;
		if (!str_len || off + str_len > blen)
			return -XA_EINVAL;

		xa_log("gip: client %d class %.*s\n", gc->xc->wcid, str_len, (const char *)base + off);
		if (str_len == 26 && !memcmp(base + off, "Windows.Xbox.Input.Gamepad", 26))
			gc->gamepad = true;
		off += str_len;
	}

	gc->identified = true;
	return gc->gamepad ? start_gamepad(gc) : 0;
}

/* forget the session; the controller announces itself again when it comes back */
static void reset_session(struct gip_client *gc)
{
	free_chunk(&gc->in);
	free_chunk(&gc->out);
	gc->identified = gc->gamepad = false;
	gc->vendor = gc->product = gc->version = 0;
	memset(&gc->auth, 0, sizeof(gc->auth));
	memset(&gc->state, 0, sizeof(gc->state));
	memset(gc->rumble, 0, sizeof(gc->rumble));
	memset(gc->rumble_sent, 0, sizeof(gc->rumble_sent));
}

static int handle_status(struct gip_client *gc, const u8 *data, u32 len)
{
	if (len < 4)
		return -XA_EINVAL;

	if (!(data[0] & GIP_STATUS_CONNECTED) && gc->identified) {
		xa_log("gip: client %d turned off\n", gc->xc->wcid);
		reset_session(gc);
		if (gc->ops && gc->ops->gone)
			gc->ops->gone(gc);
	}
	return 0;
}

/* ---- rumble ----------------------------------------------------------------------- */

#define GIP_GP_RUMBLE_DELAY_MS 10
#define GIP_GP_RUMBLE_MAX 100

void gip_set_rumble(struct gip_client *gc, u8 left, u8 right, u8 trigger_left,
		    u8 trigger_right)
{
	gc->rumble[0] = MIN(left, GIP_GP_RUMBLE_MAX);
	gc->rumble[1] = MIN(right, GIP_GP_RUMBLE_MAX);
	gc->rumble[2] = MIN(trigger_left, GIP_GP_RUMBLE_MAX);
	gc->rumble[3] = MIN(trigger_right, GIP_GP_RUMBLE_MAX);
}

void gip_poll(struct gip_client *gc)
{
	u64 now = xa_now_ms();
	u8 pkt[9];

	if (!gc->auth.done || !memcmp(gc->rumble, gc->rumble_sent, sizeof(gc->rumble)))
		return;

	/* the controller firmware drops rumble packets sent back to back */
	if (now < gc->rumble_last + GIP_GP_RUMBLE_DELAY_MS)
		return;

	/* all four motors, the values, then duration, delay, repeat (as xone) */
	pkt[0] = 0x00;
	pkt[1] = 0x0f;
	pkt[2] = gc->rumble[2];
	pkt[3] = gc->rumble[3];
	pkt[4] = gc->rumble[0];
	pkt[5] = gc->rumble[1];
	pkt[6] = 0xff;
	pkt[7] = 0x00;
	pkt[8] = 0xeb;
	if (!send_cmd(gc, GIP_CMD_RUMBLE, 0, pkt, sizeof(pkt)))
		memcpy(gc->rumble_sent, gc->rumble, sizeof(gc->rumble));
	gc->rumble_last = now;
}

static void report(struct gip_client *gc)
{
	if (gc->ops && gc->ops->input)
		gc->ops->input(gc, &gc->state);
}

static int handle_virtual_key(struct gip_client *gc, const u8 *data, u32 len)
{
	if (len != 2 || data[1] != GIP_VKEY_LEFT_WIN)
		return -XA_EINVAL;
	gc->state.guide = data[0];
	report(gc);
	return 0;
}

static int handle_input(struct gip_client *gc, const u8 *data, u32 len)
{
	if (len < 14 || !gc->gamepad)
		return -XA_EINVAL;

	gc->state.buttons = get_le16(data);
	gc->state.trigger_left = get_le16(data + 2);
	gc->state.trigger_right = get_le16(data + 4);
	gc->state.stick_left_x = (s16)get_le16(data + 6);
	gc->state.stick_left_y = (s16)get_le16(data + 8);
	gc->state.stick_right_x = (s16)get_le16(data + 10);
	gc->state.stick_right_y = (s16)get_le16(data + 12);
	report(gc);
	return 0;
}

static int dispatch(struct gip_client *gc, const struct gip_header *hdr, const u8 *data, u32 len)
{
	if (hdr->options & GIP_OPT_INTERNAL) {
		switch (hdr->command) {
		case GIP_CMD_ACKNOWLEDGE:
			return handle_acknowledge(gc, data, len);
		case GIP_CMD_ANNOUNCE:
			return handle_announce(gc, data, len);
		case GIP_CMD_STATUS:
			return handle_status(gc, data, len);
		case GIP_CMD_IDENTIFY:
			return handle_identify(gc, data, len);
		case GIP_CMD_AUTHENTICATE:
			return auth_rx(gc, data, len);
		case GIP_CMD_VIRTUAL_KEY:
			return handle_virtual_key(gc, data, len);
		}
		return 0;
	}

	if (hdr->command == GIP_CMD_INPUT)
		return handle_input(gc, data, len);
	return 0;
}

/* ---- receiving -------------------------------------------------------------------- */

static int process_chunked(struct gip_client *gc, struct gip_header *hdr, const u8 *data)
{
	u32 len;
	int err;

	if (!gc->in.data)
		/* older gamepads occasionally send spurious completions */
		return hdr->packet_length ? -XA_EINVAL : 0;
	if (hdr->command != gc->in.command)
		return -XA_EINVAL;

	len = hdr->chunk_offset + hdr->packet_length;
	if (gc->in.length < len)
		return -XA_EINVAL;

	if (hdr->packet_length) {
		/* acknowledge the last chunk even when not asked to */
		if ((hdr->options & GIP_OPT_ACKNOWLEDGE) || gc->in.length == len) {
			err = acknowledge(gc, hdr);
			if (err)
				return err;
		}
		memcpy(gc->in.data + hdr->chunk_offset, data, hdr->packet_length);
		return 0;
	}

	/* an empty chunk completes the transfer */
	struct gip_header full = *hdr;
	u8 *buf = gc->in.data;
	u32 buf_len = gc->in.length;

	full.options = gc->in.options;
	gc->in.data = NULL;
	free_chunk(&gc->in);
	err = dispatch(gc, &full, buf, buf_len);
	free(buf);
	return err;
}

static int process_pkt(struct gip_client *gc, struct gip_header *hdr, const u8 *data)
{
	int err;

	if (hdr->options & GIP_OPT_CHUNK_START) {
		/* the first chunk's offset is the total length */
		if (hdr->chunk_offset > GIP_CHUNK_BUF_MAX_LENGTH)
			return -XA_EINVAL;
		free_chunk(&gc->in);
		gc->in.data = calloc(1, hdr->chunk_offset ? hdr->chunk_offset : 1);
		if (!gc->in.data)
			return -XA_ENOMEM;
		gc->in.command = hdr->command;
		gc->in.options = hdr->options & ~(GIP_OPT_ACKNOWLEDGE | GIP_OPT_CHUNK_START);
		gc->in.length = hdr->chunk_offset;
		hdr->chunk_offset = 0;
	}

	if (hdr->options & GIP_OPT_CHUNK)
		return process_chunked(gc, hdr, data);

	if (hdr->options & GIP_OPT_ACKNOWLEDGE) {
		err = acknowledge(gc, hdr);
		if (err)
			return err;
	}
	return dispatch(gc, hdr, data, hdr->packet_length);
}

void gip_rx(struct gip_client *gc, const u8 *data, int len)
{
	while (len > GIP_HDR_MIN_LENGTH) {
		struct gip_header hdr;
		int hdr_len = decode_header(&hdr, data, len);
		int err;

		if (len < hdr_len + (int)hdr.packet_length) {
			if (xa_debug)
				xa_log("gip: client %d truncated packet (cmd %02x, %u of %d bytes)\n",
				       gc->xc->wcid, hdr.command, hdr.packet_length, len - hdr_len);
			return;
		}

		/* input arrives ~125 times a second: only level 2 shows it */
		if (xa_debug > (hdr.command == GIP_CMD_INPUT ? 1 : 0))
			xa_log("gip: client %d rx cmd %02x opt %02x seq %02x len %u off %u\n",
			       gc->xc->wcid, hdr.command, hdr.options, hdr.sequence,
			       hdr.packet_length, hdr.chunk_offset);

		/* client 0 is the controller itself; others are attachments (headset, chatpad) */
		if (!(hdr.options & GIP_HDR_CLIENT_ID)) {
			err = process_pkt(gc, &hdr, data + hdr_len);
			if (err && xa_debug)
				xa_log("gip: client %d command %02x failed: %d\n", gc->xc->wcid,
				       hdr.command, err);
		}

		data += hdr_len + hdr.packet_length;
		len -= hdr_len + hdr.packet_length;
	}
}

struct gip_client *gip_client_new(struct xa_client *xc, const struct gip_ops *ops, void *user)
{
	struct gip_client *gc = calloc(1, sizeof(*gc));

	if (!gc)
		return NULL;
	gc->xc = xc;
	gc->ops = ops;
	gc->user = user;
	return gc;
}

void gip_client_free(struct gip_client *gc)
{
	if (!gc)
		return;
	free_chunk(&gc->in);
	free_chunk(&gc->out);
	memset(&gc->auth, 0, sizeof(gc->auth));
	free(gc);
}
