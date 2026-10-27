// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Xbox Wireless Adapter: received-frame handling, wireless clients, pairing.
 * Ported from xone (transport/dongle.c), Copyright (C) 2021 Severin von Wnuck-Lipinski.
 *
 * xone defers client events from URB completion to a work queue; here everything
 * already runs on the driver thread, so events are handled directly.
 */
#include <stdio.h>
#include <stdlib.h>

#include "dongle.h"

static int set_led(struct xa_dongle *dongle, enum xa_mt_led_mode mode)
{
	if (dongle->no_led)
		return 0;
	return xa_mt_set_led_mode(&dongle->mt, mode);
}

int xa_dongle_set_pairing(struct xa_dongle *dongle, bool enable)
{
	int err;

	if (dongle->pairing == enable)
		return 0;

	err = xa_mt_set_pairing(&dongle->mt, enable);
	if (err)
		return err;

	err = set_led(dongle, enable ? XA_MT_LED_BLINK :
			      dongle->client_count ? XA_MT_LED_ON : XA_MT_LED_OFF);
	if (err)
		return err;

	xa_log("dongle: pairing %s\n", enable ? "on" : "off");
	dongle->pairing = enable;

	if (enable) {
		u64 now = xa_now_ms();

		dongle->pairing_until = now + XA_DONGLE_PAIRING_TIMEOUT_MS;
		dongle->last_wlan_rx = now;
		dongle->next_scan = now + XA_DONGLE_PAIR_SCAN_INTERVAL_MS;
		dongle->scan_idx = 0;
		for (int i = 0; i < XA_MT_NUM_CHANNELS; i++)
			if (dongle->mt.channel == &dongle->mt.channels[i])
				dongle->scan_idx = i;
	}

	return 0;
}

static void pairing_scan(struct xa_dongle *dongle, u64 now)
{
	struct xa_mt_channel *chan;

	dongle->next_scan = now + XA_DONGLE_PAIR_SCAN_INTERVAL_MS;

	/* keep the channel while a client is connected or recently heard from */
	if (dongle->client_count ||
	    now < dongle->last_wlan_rx + XA_DONGLE_PAIR_SCAN_INTERVAL_MS)
		return;

	dongle->scan_idx = (dongle->scan_idx + 1) % XA_MT_NUM_CHANNELS;
	chan = &dongle->mt.channels[dongle->scan_idx];
	if (!xa_mt_switch_channel(&dongle->mt, chan))
		dongle->mt.channel = chan;
	if (xa_debug)
		xa_log("dongle: pairing scan on channel %u\n", chan->index);
	dongle->last_wlan_rx = now;
}

void xa_dongle_poll(struct xa_dongle *dongle)
{
	u64 now = xa_now_ms();

	/* mostly malformed frames: the adapter's receive DMA is broken and needs a reset */
	if (now >= dongle->next_rx_check) {
		u32 total = dongle->rx_good + dongle->rx_bad;
		bool corrupt = total >= 50 && dongle->rx_bad * 2 > total;

		if (corrupt != dongle->rx_corrupt)
			xa_log(corrupt ? "dongle: received data is corrupt (%u of %u frames), the "
					 "adapter needs a power cycle\n" :
					 "dongle: received data looks sane again (%u of %u bad)\n",
			       dongle->rx_bad, total);
		dongle->rx_corrupt = corrupt;
		dongle->rx_good = dongle->rx_bad = 0;
		dongle->next_rx_check = now + 2000;
	}

	if (!dongle->pairing)
		return;

	if (now >= dongle->pairing_until) {
		xa_dongle_set_pairing(dongle, false);
		return;
	}

	if (now >= dongle->next_scan && !dongle->no_scan)
		pairing_scan(dongle, now);
}

static int remove_client(struct xa_dongle *dongle, u8 wcid);

void xa_dongle_stop(struct xa_dongle *dongle)
{
	for (int i = 0; i < XA_DONGLE_MAX_CLIENTS; i++)
		if (dongle->clients[i])
			remove_client(dongle, i + 1);
	xa_dongle_set_pairing(dongle, false);
	set_led(dongle, XA_MT_LED_OFF);
	xa_mt_stop_radio(&dongle->mt);
}

int xa_dongle_start(struct xa_dongle *dongle)
{
	int err = 0;

	for (int i = 0; i < 3; i++) {
		err = xa_mt_init_radio(&dongle->mt);
		if (err != -XA_ETIMEDOUT)
			break;
		xa_log("dongle: radio init timed out, retrying\n");
		xa_sleep_ms(500);
	}
	if (err)
		return err;

	/* the radio comes up with pairing off, which keeps paired controllers out */
	return xa_dongle_set_pairing(dongle, true);
}

/* ---- clients ---------------------------------------------------------------------- */

static int add_client(struct xa_dongle *dongle, const u8 *addr)
{
	struct xa_client *client;
	int i, err;

	/* a controller that already has a slot asks again */
	for (i = 0; i < XA_DONGLE_MAX_CLIENTS; i++) {
		client = dongle->clients[i];
		if (!client || memcmp(client->address, addr, ETH_ALEN))
			continue;

		/* silent since: it never got our association response */
		if (!client->active) {
			if (xa_debug)
				xa_log("dongle: client %d asks again, resending the response\n",
				       client->wcid);
			return xa_mt_send_assoc_response(&dongle->mt, addr);
		}

		/* it had a session and restarted: start over */
		xa_log("dongle: client %d reconnecting\n", client->wcid);
		remove_client(dongle, client->wcid);
		break;
	}

	for (i = 0; i < XA_DONGLE_MAX_CLIENTS; i++)
		if (!dongle->clients[i])
			break;
	if (i == XA_DONGLE_MAX_CLIENTS)
		return -XA_ENOSPC;

	client = calloc(1, sizeof(*client));
	if (!client)
		return -XA_ENOMEM;
	client->dongle = dongle;
	client->wcid = i + 1;
	memcpy(client->address, addr, ETH_ALEN);

	err = xa_mt_associate_client(&dongle->mt, client->wcid, addr);
	if (!err && !dongle->pairing)
		err = set_led(dongle, XA_MT_LED_ON);
	if (err) {
		free(client);
		return err;
	}

	xa_log("dongle: client %d associated (%02x:%02x:%02x:%02x:%02x:%02x)\n",
	       client->wcid, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
	dongle->clients[i] = client;
	dongle->client_count++;
	if (dongle->ops && dongle->ops->client_added)
		dongle->ops->client_added(client);
	return 0;
}

static int remove_client(struct xa_dongle *dongle, u8 wcid)
{
	struct xa_client *client;
	int err;

	if (!wcid || wcid > XA_DONGLE_MAX_CLIENTS || !dongle->clients[wcid - 1])
		return 0;

	client = dongle->clients[wcid - 1];
	xa_log("dongle: client %d gone\n", wcid);
	if (dongle->ops && dongle->ops->client_removed)
		dongle->ops->client_removed(client);
	dongle->clients[wcid - 1] = NULL;
	free(client);

	err = xa_mt_remove_client(&dongle->mt, wcid);
	if (--dongle->client_count == 0 && !dongle->pairing)
		set_led(dongle, XA_MT_LED_OFF);
	return err;
}

static int pair_client(struct xa_dongle *dongle, const u8 *addr)
{
	int err;

	xa_log("dongle: pairing request from %02x:%02x:%02x:%02x:%02x:%02x\n", addr[0],
	       addr[1], addr[2], addr[3], addr[4], addr[5]);
	err = xa_mt_pair_client(&dongle->mt, addr);
	if (err)
		return err;

	return xa_dongle_set_pairing(dongle, false);
}

static int enable_client_encryption(struct xa_dongle *dongle, u8 wcid)
{
	static const u8 data[] = { 0x00, 0x00 };
	struct xa_client *client;
	int err;

	if (!wcid || wcid > XA_DONGLE_MAX_CLIENTS || !dongle->clients[wcid - 1])
		return -XA_EINVAL;

	client = dongle->clients[wcid - 1];
	err = xa_mt_send_client_command(&dongle->mt, wcid, client->address,
					XA_MT_CLIENT_ENABLE_ENCRYPTION, data, sizeof(data));
	if (err)
		return err;

	xa_log("dongle: client %d encryption on\n", wcid);
	client->encryption_enabled = true;
	return 0;
}

int xa_dongle_set_key(struct xa_client *client, const u8 *key, int len)
{
	return xa_mt_set_client_key(&client->dongle->mt, client->wcid, key, len);
}

/* ---- transmit ------------------------------------------------------------------- */

int xa_dongle_send(struct xa_client *client, const u8 *data, int len,
		   enum xa_dongle_queue queue)
{
	struct xa_dongle *dongle = client->dongle;
	struct xa_msg *msg = xa_msg_alloc(len);
	u16 fc;
	u8 *p;
	int err;

	if (!msg)
		return -XA_ENOMEM;

	memcpy(xa_msg_put(msg, len), data, len);

	/* 2 bytes of padding after the QoS header */
	memset(xa_msg_push(msg, 2), 0, 2);

	/* QoS data from the AP (DS), encrypted once the client has a key */
	fc = IEEE80211_FTYPE_DATA | IEEE80211_STYPE_QOS_DATA | IEEE80211_FCTL_FROMDS;
	if (client->encryption_enabled)
		fc |= IEEE80211_FCTL_PROTECTED;
	p = xa_msg_push(msg, IEEE80211_QOS_HDR_LEN);
	memset(p, 0, IEEE80211_QOS_HDR_LEN);
	put_le16(p, fc);
	put_le16(p + 2, 144); /* duration: time to transmit (us) */
	memcpy(p + 4, client->address, ETH_ALEN);
	memcpy(p + 10, dongle->mt.address, ETH_ALEN);
	memcpy(p + 16, dongle->mt.address, ETH_ALEN);

	/* wait for acknowledgment */
	xa_mt_put_txwi(xa_msg_push(msg, XA_MT_TXWI_LEN),
		       FIELD_PREP(MT_TXWI_FLAGS_MPDU_DENSITY, IEEE80211_HT_MPDU_DENSITY_4),
		       FIELD_PREP(MT_RXWI_RATE_PHY, MT_PHY_TYPE_OFDM), MT_TXWI_ACK_CTL_REQ,
		       client->wcid - 1, IEEE80211_QOS_HDR_LEN + len);

	p = xa_msg_push(msg, 8);
	memset(p, 0, 8);
	p[2] = queue;
	p[3] = client->wcid - 1;

	err = xa_mt_send_command(&dongle->mt, msg, 0);
	return err;
}

/* ---- receive -------------------------------------------------------------------- */

static void process_frame(struct xa_dongle *dongle, const u8 *frame, int len,
			  unsigned int hdr_len, u8 wcid)
{
	u16 fc;
	const u8 *body = frame + hdr_len;
	int body_len = len - hdr_len;

	/* ignore invalid frames */
	if (len < (int)hdr_len || hdr_len < IEEE80211_HDR_3ADDR_LEN)
		return;

	fc = get_le16(frame);
	if (xa_debug) {
		u16 type = fc & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE);
		bool from_client = false;

		for (int i = 0; i < XA_DONGLE_MAX_CLIENTS; i++)
			if (dongle->clients[i] &&
			    !memcmp(dongle->clients[i]->address, frame + 10, ETH_ALEN))
				from_client = true;

		/* level 1: controllers and the management frames that matter; 2: everything */
		if (xa_debug > 1 || from_client || type == IEEE80211_STYPE_ASSOC_REQ ||
		    type == IEEE80211_STYPE_DISASSOC ||
		    type == (IEEE80211_FTYPE_MGMT | XA_MT_WLAN_RESERVED))
			if (type != (IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_BEACON))
				xa_log("rx: frame fc %04x wcid %d from %02x:%02x:%02x:%02x:%02x:%02x, "
				       "%d bytes%s\n", fc, wcid, frame[10], frame[11], frame[12],
				       frame[13], frame[14], frame[15], len,
				       fc & IEEE80211_FCTL_PROTECTED ? " (protected)" : "");
	}

	switch (fc & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) {
	case IEEE80211_FTYPE_DATA | IEEE80211_STYPE_QOS_DATA:
		if (wcid && wcid <= XA_DONGLE_MAX_CLIENTS && dongle->clients[wcid - 1]) {
			dongle->clients[wcid - 1]->active = true;
			if (dongle->pairing)
				dongle->last_wlan_rx = xa_now_ms();
			if (dongle->ops && dongle->ops->client_data)
				dongle->ops->client_data(dongle->clients[wcid - 1], body, body_len);
		}
		break;
	case IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ASSOC_REQ:
		/* the channel scan can produce associations from invalid addresses; and
		 * never take a client while the received data is known to be corrupt */
		if (!ether_addr_is_valid(frame + 10) || dongle->rx_corrupt)
			break;
		if (dongle->pairing)
			dongle->last_wlan_rx = xa_now_ms();
		add_client(dongle, frame + 10);
		break;
	case IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_DISASSOC:
		remove_client(dongle, wcid);
		break;
	case IEEE80211_FTYPE_MGMT | XA_MT_WLAN_RESERVED:
		if (body_len < 2 || body[0] != XA_MT_WLAN_RESERVED)
			break;
		if (body[1] == XA_MT_CLIENT_PAIR_REQ) {
			if (dongle->pairing)
				dongle->last_wlan_rx = xa_now_ms();
			pair_client(dongle, frame + 10);
		} else if (body[1] == XA_MT_CLIENT_ENABLE_ENCRYPTION) {
			enable_client_encryption(dongle, wcid);
		}
		break;
	}
}

static void process_wlan(struct xa_dongle *dongle, const u8 *data, int len)
{
	/* static: too big for a kernel thread's stack; all calls are on the driver thread */
	static u8 frame[XA_DONGLE_LEN_WLAN_PKT];
	unsigned int hdr_len;
	u32 rxinfo, ctl, mpdu_len;

	if (len < XA_MT_RXWI_LEN)
		return;

	rxinfo = get_le32(data);
	ctl = get_le32(data + 4);
	data += XA_MT_RXWI_LEN;
	len -= XA_MT_RXWI_LEN;
	if (len < 2 || len > (int)sizeof(frame))
		return;

	hdr_len = ieee80211_hdrlen(get_le16(data));

	/* 2 bytes of padding after the 802.11 header */
	if (rxinfo & MT_RXINFO_L2PAD) {
		if (len < (int)hdr_len + 2)
			return;
		memcpy(frame, data, hdr_len);
		memcpy(frame + hdr_len, data + hdr_len + 2, len - hdr_len - 2);
		len -= 2;
	} else {
		memcpy(frame, data, len);
	}

	/* drop frames where RXWI claims more than was delivered (own beacon loopback) */
	mpdu_len = FIELD_GET(MT_RXWI_CTL_MPDU_LEN, ctl);
	if ((int)mpdu_len > len)
		return;

	/* no real 802.11 frame is this long or shorter than its header */
	if (mpdu_len > XA_MAX_MPDU_LEN || mpdu_len < hdr_len) {
		dongle->rx_bad++;
		return;
	}
	dongle->rx_good++;

	process_frame(dongle, frame, mpdu_len, hdr_len, FIELD_GET(MT_RXWI_CTL_WCID, ctl));
}

void xa_dongle_rx(struct xa_dongle *dongle, const u8 *data, int len)
{
	u32 info, port;

	if (xa_debug > 2) {
		char hex[3 * 48 + 1] = "";

		for (int i = 0; i < len && i < 48; i++)
			snprintf(hex + 3 * i, 4, "%02x ", data[i]);
		xa_log("rx: %d bytes: %s\n", len, hex);
	}

	/* command header + trailer */
	if (len < MT_CMD_HDR_LEN * 2)
		return;

	info = get_le32(data);
	port = FIELD_GET(MT_RX_FCE_INFO_D_PORT, info);

	/* command responses */
	if (FIELD_GET(MT_RX_FCE_INFO_CMD_SEQ, info) == 0x01)
		return;

	data += MT_CMD_HDR_LEN;
	len -= MT_CMD_HDR_LEN * 2;

	if (xa_debug > 1)
		xa_log("rx: port %u event %u, %d bytes\n", port,
		       FIELD_GET(MT_RX_FCE_INFO_EVT_TYPE, info), len);

	if (port == MT_WLAN_PORT) {
		process_wlan(dongle, data, len);
		return;
	}
	if (port != MT_CPU_RX_PORT)
		return;

	switch (FIELD_GET(MT_RX_FCE_INFO_EVT_TYPE, info)) {
	case XA_MT_EVT_BUTTON:
		xa_log("dongle: pair button\n");
		xa_dongle_set_pairing(dongle, true);
		break;
	case XA_MT_EVT_PACKET_RX:
		process_wlan(dongle, data, len);
		break;
	case XA_MT_EVT_CLIENT_LOST:
		if (len >= 1)
			remove_client(dongle, data[0]);
		break;
	}
}
