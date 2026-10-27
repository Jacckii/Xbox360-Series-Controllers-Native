/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MT76 chip of the Xbox Wireless Adapter. Ported from xone (transport/mt76.h),
 * Copyright (C) 2021 Severin von Wnuck-Lipinski.
 */
#pragma once

#include "mt76_defs.h"
#include "platform.h"

#define XA_MT_EP_IN_CMD 0x05
#define XA_MT_EP_IN_WLAN 0x04
#define XA_MT_EP_OUT 0x04

#define XA_MT_NUM_CHANNELS 12

/* 802.11 frame subtype: reserved */
#define XA_MT_WLAN_RESERVED 0x70

#define XA_MT_TXWI_LEN 20
#define XA_MT_RXWI_LEN 32
#define XA_MT_FW_HDR_LEN 32

enum xa_mt_led_mode {
	XA_MT_LED_BLINK = 0x00,
	XA_MT_LED_ON = 0x01,
	XA_MT_LED_OFF = 0x02,
};

enum xa_mt_event {
	XA_MT_EVT_BUTTON = 0x04,
	XA_MT_EVT_CHANNELS = 0x0a,
	XA_MT_EVT_PACKET_RX = 0x0c,
	XA_MT_EVT_COREDUMP = 0x0d,
	XA_MT_EVT_CLIENT_LOST = 0x0e,
};

enum xa_mt_client_command {
	XA_MT_CLIENT_PAIR_REQ = 0x01,
	XA_MT_CLIENT_PAIR_RESP = 0x02,
	XA_MT_CLIENT_CHANGE_CHAN_REQ = 0x03,
	XA_MT_CLIENT_CHANGE_CHAN_RESP = 0x04,
	XA_MT_CLIENT_STATISTICS_REQ = 0x05,
	XA_MT_CLIENT_STATISTICS_RESP = 0x06,
	XA_MT_CLIENT_SCAN_CHAN_REQ = 0x07,
	XA_MT_CLIENT_SCAN_CHAN_RESP = 0x08,
	XA_MT_CLIENT_ENABLE_ENCRYPTION = 0x10,
};

struct xa_mt_channel {
	u8 index;
	u8 band;
	u8 bandwidth; /* enum mt76_phy_bandwidth */
	u8 group;     /* enum mt76_cal_channel_group */
	bool scan;
	u8 power;
};

struct xa_mt {
	struct xa_usb *usb;
	u8 address[ETH_ALEN];
	struct xa_mt_channel channels[XA_MT_NUM_CHANNELS];
	struct xa_mt_channel *channel;
};

/* message buffer with headroom for the headers pushed in front of a payload */
struct xa_msg {
	u8 *head;
	u8 *data;
	int len;
	int size;
};

#define XA_MSG_HEADROOM 64

struct xa_msg *xa_msg_alloc(int len);
void xa_msg_free(struct xa_msg *msg);
u8 *xa_msg_put(struct xa_msg *msg, int len);
u8 *xa_msg_push(struct xa_msg *msg, int len);
void xa_mt_prep_command(struct xa_msg *msg, u32 cmd);
void xa_mt_put_txwi(u8 *p, u16 flags, u16 rate, u8 ack_ctl, u8 wcid, u16 len);

/* load_firmware returns this when a cold upload made the chip re-enumerate:
 * reopen the device and call it again (it then takes the warm path) */
#define XA_MT_REENUMERATE 1

int xa_mt_load_firmware(struct xa_mt *mt, const u8 *fw, u32 fw_len);
int xa_mt_init_radio(struct xa_mt *mt);
void xa_mt_stop_radio(struct xa_mt *mt);
int xa_mt_set_led_mode(struct xa_mt *mt, enum xa_mt_led_mode mode);

/* debugging: raw register access (MT_VEND_TYPE_CFG in addr selects the CFG space) */
int xa_mt_peek(struct xa_mt *mt, u32 addr, u32 *val);
void xa_mt_poke(struct xa_mt *mt, u32 addr, u32 val);
int xa_mt_switch_channel(struct xa_mt *mt, struct xa_mt_channel *chan);
int xa_mt_set_pairing(struct xa_mt *mt, bool enable);
int xa_mt_send_command(struct xa_mt *mt, struct xa_msg *msg, u32 cmd);

int xa_mt_pair_client(struct xa_mt *mt, const u8 *addr);
int xa_mt_associate_client(struct xa_mt *mt, u8 wcid, const u8 *addr);
int xa_mt_send_assoc_response(struct xa_mt *mt, const u8 *addr);
int xa_mt_send_client_command(struct xa_mt *mt, u8 wcid, const u8 *addr,
			      enum xa_mt_client_command cmd, const u8 *data,
			      int len);
int xa_mt_set_client_key(struct xa_mt *mt, u8 wcid, const u8 *key, int len);
int xa_mt_remove_client(struct xa_mt *mt, u8 wcid);
