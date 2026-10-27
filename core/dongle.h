/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Xbox Wireless Adapter: received-frame handling, wireless clients, pairing.
 * Ported from xone (transport/dongle.c), Copyright (C) 2021 Severin von Wnuck-Lipinski.
 */
#pragma once

#include "mt76.h"

#define XA_DONGLE_MAX_CLIENTS 16
#define XA_DONGLE_PAIRING_TIMEOUT_MS 60000
#define XA_DONGLE_PAIR_SCAN_INTERVAL_MS 2000

/* IN endpoints and the buffer size each needs */
#define XA_DONGLE_LEN_CMD_PKT 0x0654
#define XA_DONGLE_LEN_WLAN_PKT 0x8400

enum xa_dongle_queue {
	XA_DONGLE_QUEUE_DATA = 0x00,
	XA_DONGLE_QUEUE_AUDIO = 0x02,
};

struct xa_dongle;

struct xa_client {
	struct xa_dongle *dongle;
	u8 wcid;
	u8 address[ETH_ALEN];
	bool encryption_enabled;
	bool active; /* has sent data since it associated */
	void *priv;  /* owned by the protocol layer */
};

struct xa_dongle_ops {
	void (*client_added)(struct xa_client *client);
	void (*client_removed)(struct xa_client *client);
	void (*client_data)(struct xa_client *client, const u8 *data, int len);
};

struct xa_dongle {
	struct xa_mt mt;
	const struct xa_dongle_ops *ops;

	struct xa_client *clients[XA_DONGLE_MAX_CLIENTS];
	int client_count;

	bool pairing;
	bool no_scan; /* testing: stay on the starting channel while pairing */
	bool no_led;  /* testing: never send the LED command */
	u64 pairing_until;
	u64 last_wlan_rx;
	u64 next_scan;
	u8 scan_idx;

	/* received-frame sanity, to detect an adapter whose RX DMA delivers garbage */
	u32 rx_good, rx_bad;
	u64 next_rx_check;
	bool rx_corrupt;
};

/* longest valid 802.11 MPDU (2304 body + headers), with margin */
#define XA_MAX_MPDU_LEN 2400

/* after the firmware is running: radio init, then pairing mode so known controllers reconnect */
int xa_dongle_start(struct xa_dongle *dongle);

/* before letting go of the adapter: clients removed, pairing off, radio stopped */
void xa_dongle_stop(struct xa_dongle *dongle);

/* one received USB buffer, from either IN endpoint */
void xa_dongle_rx(struct xa_dongle *dongle, const u8 *data, int len);

/* timers (pairing timeout, pairing channel scan); call every few milliseconds */
void xa_dongle_poll(struct xa_dongle *dongle);

int xa_dongle_set_pairing(struct xa_dongle *dongle, bool enable);
int xa_dongle_send(struct xa_client *client, const u8 *data, int len,
		   enum xa_dongle_queue queue);
int xa_dongle_set_key(struct xa_client *client, const u8 *key, int len);
