// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MT76 chip of the Xbox Wireless Adapter: firmware, radio, beacon, clients.
 * Ported from xone (transport/mt76.c), Copyright (C) 2021 Severin von Wnuck-Lipinski.
 * Linux sk_buffs are replaced by struct xa_msg; wire structures are written byte by
 * byte so the code also runs on big-endian CPUs.
 */
#include <stdlib.h>

#include "mt76.h"

#define XA_MT_POLL_RETRIES 50

#define XA_MT_RF_PATCH 0x0130
#define XA_MT_FW_LOAD_IVB 0x12
#define XA_MT_FW_ILM_OFFSET 0x080000
#define XA_MT_FW_DLM_OFFSET 0x110800
#define XA_MT_FW_CHUNK_SIZE 0x3800

/* wireless channel bands */
#define XA_MT_CH_2G_LOW 0x01
#define XA_MT_CH_2G_MID 0x02
#define XA_MT_CH_2G_HIGH 0x03
#define XA_MT_CH_5G_LOW 0x01
#define XA_MT_CH_5G_HIGH 0x02

#define XA_MT_WCID_KEY_LEN 16

/* commands specific to the adapter's firmware */
enum xa_mt_ms_command {
	XA_MT_SET_MAC_ADDRESS = 0x00,
	XA_MT_ADD_CLIENT = 0x01,
	XA_MT_REMOVE_CLIENT = 0x02,
	XA_MT_SET_IDLE_TIME = 0x05,
	XA_MT_SET_CHAN_CANDIDATES = 0x07,
};

enum xa_mt_wow_feature {
	XA_MT_WOW_ENABLE = 0x01,
	XA_MT_WOW_TRAFFIC = 0x03,
};

enum xa_mt_wow_traffic {
	XA_MT_WOW_TO_FIRMWARE = 0x00,
	XA_MT_WOW_TO_HOST = 0x01,
};

#define TXWI_RATE_OFDM FIELD_PREP(MT_RXWI_RATE_PHY, MT_PHY_TYPE_OFDM)
#define TXWI_FLAGS_ACK FIELD_PREP(MT_TXWI_FLAGS_MPDU_DENSITY, IEEE80211_HT_MPDU_DENSITY_4)

/* ---- message buffers ----------------------------------------------------- */

struct xa_msg *xa_msg_alloc(int len)
{
	/* headroom for headers, room for padding (up to 4) and the trailer (4) */
	int size = XA_MSG_HEADROOM + len + 8;
	struct xa_msg *msg = malloc(sizeof(*msg) + size);

	if (!msg)
		return NULL;

	msg->head = (u8 *)(msg + 1);
	msg->data = msg->head + XA_MSG_HEADROOM;
	msg->len = 0;
	msg->size = size;
	memset(msg->head, 0, size);
	return msg;
}

void xa_msg_free(struct xa_msg *msg)
{
	free(msg);
}

u8 *xa_msg_put(struct xa_msg *msg, int len)
{
	u8 *tail = msg->data + msg->len;

	msg->len += len;
	return tail;
}

u8 *xa_msg_push(struct xa_msg *msg, int len)
{
	msg->data -= len;
	msg->len += len;
	return msg->data;
}

static void put_u32(struct xa_msg *msg, u32 v)
{
	put_le32(xa_msg_put(msg, 4), v);
}

static void put_data(struct xa_msg *msg, const void *data, int len)
{
	memcpy(xa_msg_put(msg, len), data, len);
}

void xa_mt_put_txwi(u8 *p, u16 flags, u16 rate, u8 ack_ctl, u8 wcid, u16 len)
{
	memset(p, 0, XA_MT_TXWI_LEN);
	put_le16(p + 0, flags);
	put_le16(p + 2, rate);
	p[4] = ack_ctl;
	p[5] = wcid;
	put_le16(p + 6, len);
}

/* 24-byte 802.11 header with three addresses */
static void put_hdr_3addr(u8 *p, u16 fc, const u8 *a1, const u8 *a2, const u8 *a3)
{
	memset(p, 0, IEEE80211_HDR_3ADDR_LEN);
	put_le16(p, fc);
	memcpy(p + 4, a1, ETH_ALEN);
	memcpy(p + 10, a2, ETH_ALEN);
	memcpy(p + 16, a3, ETH_ALEN);
}

/* ---- registers ------------------------------------------------------------ */

static int read_register(struct xa_mt *mt, u32 addr, u32 *val)
{
	u8 req = MT_VEND_MULTI_READ;
	u8 buf[4];
	int ret;

	if (addr & MT_VEND_TYPE_CFG) {
		req = MT_VEND_READ_CFG;
		addr &= ~MT_VEND_TYPE_CFG;
	}

	ret = xa_usb_ctrl(mt->usb, USB_DIR_IN | USB_TYPE_VENDOR, req, addr >> 16,
			  addr, buf, sizeof(buf));
	if (ret != sizeof(buf)) {
		*val = 0;
		return ret < 0 ? ret : -XA_EIO;
	}

	*val = get_le32(buf);
	return 0;
}

static u32 xa_mt_read(struct xa_mt *mt, u32 addr)
{
	u32 val;

	if (read_register(mt, addr, &val))
		xa_log("mt76: read %08x failed\n", addr);
	return val;
}

int xa_mt_peek(struct xa_mt *mt, u32 addr, u32 *val)
{
	return read_register(mt, addr, val);
}

static void xa_mt_write(struct xa_mt *mt, u32 addr, u32 val);

void xa_mt_poke(struct xa_mt *mt, u32 addr, u32 val)
{
	xa_mt_write(mt, addr, val);
}

static void xa_mt_write(struct xa_mt *mt, u32 addr, u32 val)
{
	u8 req = MT_VEND_MULTI_WRITE;
	u8 buf[4];
	int ret;

	if (addr & MT_VEND_TYPE_CFG) {
		req = MT_VEND_WRITE_CFG;
		addr &= ~MT_VEND_TYPE_CFG;
	}

	put_le32(buf, val);
	ret = xa_usb_ctrl(mt->usb, USB_DIR_OUT | USB_TYPE_VENDOR, req, addr >> 16,
			  addr, buf, sizeof(buf));
	if (ret != sizeof(buf))
		xa_log("mt76: write %08x failed: %d\n", addr, ret);
}

static int load_ivb(struct xa_mt *mt)
{
	/* load interrupt vector block */
	int ret = xa_usb_ctrl(mt->usb, USB_DIR_OUT | USB_TYPE_VENDOR,
			      MT_VEND_DEV_MODE, XA_MT_FW_LOAD_IVB, 0, NULL, 0);
	return ret < 0 ? ret : 0;
}

static bool poll_register(struct xa_mt *mt, u32 offset, u32 mask, u32 val)
{
	for (int i = 0; i < XA_MT_POLL_RETRIES; i++) {
		if ((xa_mt_read(mt, offset) & mask) == val)
			return true;
		xa_sleep_ms(15);
	}

	return false;
}

static int read_efuse(struct xa_mt *mt, u16 addr, void *data, int len)
{
	u32 ctrl, offset, val;

	ctrl = xa_mt_read(mt, MT_EFUSE_CTRL);
	ctrl &= ~(MT_EFUSE_CTRL_AIN | MT_EFUSE_CTRL_MODE);
	ctrl |= MT_EFUSE_CTRL_KICK;
	ctrl |= FIELD_PREP(MT_EFUSE_CTRL_AIN, addr & ~0x0f);
	ctrl |= FIELD_PREP(MT_EFUSE_CTRL_MODE, MT_EE_READ);
	xa_mt_write(mt, MT_EFUSE_CTRL, ctrl);

	if (!poll_register(mt, MT_EFUSE_CTRL, MT_EFUSE_CTRL_KICK, 0))
		return -XA_ETIMEDOUT;

	for (int i = 0; i < len; i += 4) {
		/* block data offset (multiple of 32 bits) */
		offset = (addr & GENMASK(3, 2)) + i;
		val = xa_mt_read(mt, MT_EFUSE_DATA_BASE + offset);

		u8 bytes[4];
		put_le32(bytes, val);
		memcpy((u8 *)data + i, bytes, MIN(len - i, 4));
	}

	return 0;
}

/* ---- MCU messages ------------------------------------------------------------ */

static void prep_message(struct xa_msg *msg, u32 info)
{
	/* padding and trailer */
	int len = ROUND_UP(msg->len, 4);
	int pad = len - msg->len + MT_CMD_HDR_LEN;

	put_le32(xa_msg_push(msg, MT_CMD_HDR_LEN), info | FIELD_PREP(MT_MCU_MSG_LEN, len));
	memset(xa_msg_put(msg, pad), 0, pad);
}

void xa_mt_prep_command(struct xa_msg *msg, u32 cmd)
{
	prep_message(msg, MT_MCU_MSG_TYPE_CMD |
			  FIELD_PREP(MT_MCU_MSG_PORT, MT_CPU_TX_PORT) |
			  FIELD_PREP(MT_MCU_MSG_CMD_TYPE, cmd));
}

int xa_mt_send_command(struct xa_mt *mt, struct xa_msg *msg, u32 cmd)
{
	int err;

	xa_mt_prep_command(msg, cmd);
	err = xa_usb_bulk_out(mt->usb, XA_MT_EP_OUT, msg->data, msg->len);
	xa_msg_free(msg);
	return err;
}

static int send_wlan(struct xa_mt *mt, struct xa_msg *msg)
{
	u16 frame_len = msg->len;
	int err;

	/* wait for acknowledgment, ignore the wireless client identifier (WCID) */
	xa_mt_put_txwi(xa_msg_push(msg, XA_MT_TXWI_LEN), TXWI_FLAGS_ACK,
		       TXWI_RATE_OFDM, MT_TXWI_ACK_CTL_REQ, 0xff, frame_len);

	/* enhanced distributed channel access (EDCA), wireless information valid */
	prep_message(msg, FIELD_PREP(MT_TXD_INFO_DPORT, MT_WLAN_PORT) |
			  FIELD_PREP(MT_TXD_INFO_QSEL, MT_QSEL_EDCA) |
			  MT_TXD_INFO_WIV | MT_TXD_INFO_80211);

	err = xa_usb_bulk_out(mt->usb, XA_MT_EP_OUT, msg->data, msg->len);
	xa_msg_free(msg);
	return err;
}

static int send_u32s(struct xa_mt *mt, u32 cmd, int n, const u32 *vals)
{
	struct xa_msg *msg = xa_msg_alloc(4 * n);

	if (!msg)
		return -XA_ENOMEM;

	for (int i = 0; i < n; i++)
		put_u32(msg, vals[i]);

	return xa_mt_send_command(mt, msg, cmd);
}

static int select_function(struct xa_mt *mt, u32 func, u32 val)
{
	u32 v[] = { func, val };
	return send_u32s(mt, MT_CMD_FUN_SET_OP, 2, v);
}

static int load_cr(struct xa_mt *mt, u8 mode)
{
	struct xa_msg *msg = xa_msg_alloc(4);

	if (!msg)
		return -XA_ENOMEM;

	/* mode, temperature, channel, padding */
	u8 *p = xa_msg_put(msg, 4);
	p[0] = mode;
	return xa_mt_send_command(mt, msg, MT_CMD_LOAD_CR);
}

static int send_ms_command(struct xa_mt *mt, u32 cmd, const void *data, int len)
{
	struct xa_msg *msg = xa_msg_alloc(4 + len);

	if (!msg)
		return -XA_ENOMEM;

	put_u32(msg, cmd);
	put_data(msg, data, len);

	/* command for Microsoft's proprietary firmware */
	return xa_mt_send_command(mt, msg, MT_CMD_INIT_GAIN_OP);
}

static int write_burst(struct xa_mt *mt, u32 idx, const void *data, int len)
{
	struct xa_msg *msg = xa_msg_alloc(4 + len);

	if (!msg)
		return -XA_ENOMEM;

	/* register offset in memory */
	put_u32(msg, idx + MT_MCU_MEMMAP_WLAN);
	put_data(msg, data, len);
	return xa_mt_send_command(mt, msg, MT_CMD_BURST_WRITE);
}

int xa_mt_set_led_mode(struct xa_mt *mt, enum xa_mt_led_mode mode)
{
	u32 v = mode;
	return send_u32s(mt, MT_CMD_LED_MODE_OP, 1, &v);
}

static int set_power_mode(struct xa_mt *mt, u32 mode)
{
	return send_u32s(mt, MT_CMD_POWER_SAVING_OP, 1, &mode);
}

static int set_wow_enable(struct xa_mt *mt, bool enable)
{
	struct xa_msg *msg = xa_msg_alloc(6);

	if (!msg)
		return -XA_ENOMEM;

	put_u32(msg, XA_MT_WOW_ENABLE);
	u8 *p = xa_msg_put(msg, 2);
	p[0] = enable;
	p[1] = mt->channel->index;
	return xa_mt_send_command(mt, msg, MT_CMD_WOW_FEATURE);
}

static int set_wow_traffic(struct xa_mt *mt, u8 traffic)
{
	struct xa_msg *msg = xa_msg_alloc(5);

	if (!msg)
		return -XA_ENOMEM;

	put_u32(msg, XA_MT_WOW_TRAFFIC);
	*xa_msg_put(msg, 1) = traffic;
	return xa_mt_send_command(mt, msg, MT_CMD_WOW_FEATURE);
}

int xa_mt_switch_channel(struct xa_mt *mt, struct xa_mt_channel *chan)
{
	struct xa_msg *msg = xa_msg_alloc(20);

	if (!msg)
		return -XA_ENOMEM;

	/* channel, padding[3], TX/RX stream 1, padding[10], bandwidth, power, scan, ? */
	u8 *p = xa_msg_put(msg, 20);
	p[0] = chan->index;
	put_le16(p + 4, 0x0101);
	p[16] = chan->bandwidth;
	p[17] = chan->power;
	p[18] = chan->scan;
	return xa_mt_send_command(mt, msg, MT_CMD_SWITCH_CHANNEL_OP);
}

static int calibrate(struct xa_mt *mt, u32 calib, u32 val)
{
	u32 v[] = { calib, val };
	return send_u32s(mt, MT_CMD_CALIBRATION_OP, 2, v);
}

/* ---- firmware ------------------------------------------------------------------ */

static int send_firmware_part(struct xa_mt *mt, u32 offset, const u8 *data, u32 len)
{
	for (u32 pos = 0; pos < len; pos += XA_MT_FW_CHUNK_SIZE) {
		u32 chunk_len = MIN(len - pos, (u32)XA_MT_FW_CHUNK_SIZE);
		struct xa_msg *msg = xa_msg_alloc(chunk_len);
		int err;

		if (!msg)
			return -XA_ENOMEM;

		put_data(msg, data + pos, chunk_len);
		chunk_len = ROUND_UP(chunk_len, 4);

		xa_mt_write(mt, MT_FCE_DMA_ADDR | MT_VEND_TYPE_CFG, offset + pos);
		xa_mt_write(mt, MT_FCE_DMA_LEN | MT_VEND_TYPE_CFG, chunk_len << 16);

		err = xa_mt_send_command(mt, msg, 0);
		if (err)
			return err;

		if (!poll_register(mt, MT_FCE_DMA_LEN | MT_VEND_TYPE_CFG, 0xffffffff,
				   0xc0000000 | (chunk_len << 16)))
			return -XA_ETIMEDOUT;
	}

	return 0;
}

static int send_firmware(struct xa_mt *mt, const u8 *fw, u32 fw_len)
{
	u32 ilm_len, dlm_len;
	int err;

	if (fw_len < XA_MT_FW_HDR_LEN)
		return -XA_EINVAL;

	ilm_len = get_le32(fw + 0);
	dlm_len = get_le32(fw + 4);
	if (fw_len != XA_MT_FW_HDR_LEN + ilm_len + dlm_len)
		return -XA_EINVAL;

	xa_log("mt76: firmware build %.16s\n", (const char *)fw + 16);

	/* configure DMA, enable FCE and packet DMA */
	xa_mt_write(mt, MT_USB_U3DMA_CFG | MT_VEND_TYPE_CFG,
		    MT_USB_DMA_CFG_TX_BULK_EN | MT_USB_DMA_CFG_RX_BULK_EN);
	xa_mt_write(mt, MT_FCE_PSE_CTRL, 0x01);
	xa_mt_write(mt, MT_TX_CPU_FROM_FCE_BASE_PTR, 0x00400230);
	xa_mt_write(mt, MT_TX_CPU_FROM_FCE_MAX_COUNT, 0x01);
	xa_mt_write(mt, MT_TX_CPU_FROM_FCE_CPU_DESC_IDX, 0x01);
	xa_mt_write(mt, MT_FCE_PDMA_GLOBAL_CONF, 0x44);
	xa_mt_write(mt, MT_FCE_SKIP_FS, 0x03);

	/* instruction local memory, then data local memory */
	err = send_firmware_part(mt, XA_MT_FW_ILM_OFFSET, fw + XA_MT_FW_HDR_LEN, ilm_len);
	if (err)
		return err;

	return send_firmware_part(mt, XA_MT_FW_DLM_OFFSET,
				  fw + XA_MT_FW_HDR_LEN + ilm_len, dlm_len);
}

static void apply_rf_patch(struct xa_mt *mt)
{
	/* power-on RF patch: without it the radio never transmits beacons */
	u32 val = xa_mt_read(mt, XA_MT_RF_PATCH | MT_VEND_TYPE_CFG);
	xa_mt_write(mt, XA_MT_RF_PATCH | MT_VEND_TYPE_CFG, val & ~BIT(19));
}

int xa_mt_load_firmware(struct xa_mt *mt, const u8 *fw, u32 fw_len)
{
	u32 val;
	int err;

	err = read_register(mt, MT_FCE_DMA_ADDR | MT_VEND_TYPE_CFG, &val);
	if (err)
		return err;

	if (val) {
		/* firmware is already running: restart it */
		xa_log("mt76: firmware present, resetting it\n");
		apply_rf_patch(mt);
		err = load_ivb(mt);
		if (err)
			return err;
		if (!poll_register(mt, MT_FCE_DMA_ADDR | MT_VEND_TYPE_CFG, 0x80000000, 0x80000000))
			return -XA_ETIMEDOUT;

		/* the MCU needs time to finish starting before it takes commands */
		xa_sleep_ms(500);
		return 0;
	}

	xa_log("mt76: uploading firmware (%u bytes)\n", fw_len);
	err = send_firmware(mt, fw, fw_len);
	if (err)
		return err;

	xa_mt_write(mt, MT_FCE_DMA_ADDR | MT_VEND_TYPE_CFG, 0);
	apply_rf_patch(mt);

	err = load_ivb(mt);
	/* a cold chip may drop off USB before it acknowledges the request */
	if (err == -XA_ETIMEDOUT || err == -XA_ENODEV) {
		xa_log("mt76: no answer to the firmware start: the chip is restarting\n");
		return XA_MT_REENUMERATE;
	}
	if (err)
		return err;

	/* the chip normally drops off USB here and comes back with the firmware running */
	xa_sleep_ms(500);
	err = read_register(mt, MT_FCE_DMA_ADDR | MT_VEND_TYPE_CFG, &val);
	if (err)
		return XA_MT_REENUMERATE;

	return poll_register(mt, MT_FCE_DMA_ADDR | MT_VEND_TYPE_CFG, 0x01, 0x01) ?
		       0 : -XA_ETIMEDOUT;
}

/* ---- radio --------------------------------------------------------------------- */

static const struct xa_mt_channel xa_mt_channels[XA_MT_NUM_CHANNELS] = {
	{ 0x01, XA_MT_CH_2G_LOW, MT_PHY_BW_20, 0, true, 0 },
	{ 0x06, XA_MT_CH_2G_MID, MT_PHY_BW_20, 0, true, 0 },
	{ 0x0b, XA_MT_CH_2G_HIGH, MT_PHY_BW_20, 0, true, 0 },
	{ 0x24, XA_MT_CH_5G_LOW, MT_PHY_BW_40, MT_CH_5G_UNII_1, true, 0 },
	{ 0x28, XA_MT_CH_5G_LOW, MT_PHY_BW_40, MT_CH_5G_UNII_1, false, 0 },
	{ 0x2c, XA_MT_CH_5G_HIGH, MT_PHY_BW_40, MT_CH_5G_UNII_1, true, 0 },
	{ 0x30, XA_MT_CH_5G_HIGH, MT_PHY_BW_40, MT_CH_5G_UNII_1, false, 0 },
	{ 0x95, XA_MT_CH_5G_LOW, MT_PHY_BW_80, MT_CH_5G_UNII_3, true, 0 },
	{ 0x99, XA_MT_CH_5G_LOW, MT_PHY_BW_80, MT_CH_5G_UNII_3, false, 0 },
	{ 0x9d, XA_MT_CH_5G_HIGH, MT_PHY_BW_80, MT_CH_5G_UNII_3, true, 0 },
	{ 0xa1, XA_MT_CH_5G_HIGH, MT_PHY_BW_80, MT_CH_5G_UNII_3, false, 0 },
	{ 0xa5, XA_MT_CH_5G_HIGH, MT_PHY_BW_80, MT_CH_5G_UNII_3, false, 0 },
};

static int set_channel_candidates(struct xa_mt *mt)
{
	u8 buf[4 * (3 + XA_MT_NUM_CHANNELS)];
	u8 best = mt->channel->index;
	int len = 0;

	put_le32(buf + len, 1), len += 4;
	put_le32(buf + len, best), len += 4;
	put_le32(buf + len, XA_MT_NUM_CHANNELS - 1), len += 4;
	for (int i = 0; i < XA_MT_NUM_CHANNELS; i++)
		if (mt->channels[i].index != best)
			put_le32(buf + len, mt->channels[i].index), len += 4;

	return send_ms_command(mt, XA_MT_SET_CHAN_CANDIDATES, buf, len);
}

static int get_channel_power(struct xa_mt *mt, struct xa_mt_channel *chan)
{
	u16 addr;
	u8 idx, target, offset, entry[8];
	int err;

	if (chan->bandwidth == MT_PHY_BW_20) {
		addr = MT_EE_TX_POWER_0_START_2G;
		idx = 4;
	} else {
		/* each group has its own power table */
		addr = MT_EE_TX_POWER_0_START_5G + chan->group * MT_TX_POWER_GROUP_SIZE_5G;
		idx = 5;
	}

	err = read_efuse(mt, addr, entry, sizeof(entry));
	if (err)
		return err;

	target = entry[idx];
	offset = entry[idx + chan->band];

	/* increase or decrease power by offset (in 0.5 dB steps) */
	if (offset & BIT(7))
		chan->power = (offset & BIT(6)) ? target + (offset & GENMASK(5, 0)) :
						  target - (offset & GENMASK(5, 0));
	else
		chan->power = target;

	return 0;
}

static int init_channels(struct xa_mt *mt)
{
	int err, pow = 0;

	/* promiscuous mode while evaluating */
	xa_mt_write(mt, MT_RX_FILTR_CFG, 0x014f13);

	mt->channel = NULL;
	memcpy(mt->channels, xa_mt_channels, sizeof(xa_mt_channels));
	for (int i = 0; i < XA_MT_NUM_CHANNELS; i++) {
		struct xa_mt_channel *chan = &mt->channels[i];

		err = get_channel_power(mt, chan);
		if (err)
			return err;
		err = xa_mt_switch_channel(mt, chan);
		if (err)
			return err;

		/* the highest-power channel seen first */
		if (chan->power > pow) {
			mt->channel = chan;
			pow = chan->power;
		}
	}
	if (!mt->channel)
		mt->channel = &mt->channels[XA_MT_NUM_CHANNELS - 1];

	xa_mt_write(mt, MT_RX_FILTR_CFG, 0x017f17);
	xa_log("mt76: channel %u\n", mt->channel->index);

	mt->channel->scan = true;
	err = xa_mt_switch_channel(mt, mt->channel);
	if (err)
		return err;
	err = set_power_mode(mt, MT_RADIO_OFF);
	if (err)
		return err;
	xa_sleep_ms(50);
	err = set_power_mode(mt, MT_RADIO_ON);
	if (err)
		return err;

	mt->channel->scan = false;
	err = xa_mt_switch_channel(mt, mt->channel);
	if (err)
		return err;

	return set_channel_candidates(mt);
}

static int set_idle_time(struct xa_mt *mt)
{
	u8 time[4];

	/* keeps idle clients from disconnecting */
	put_le32(time, 64);
	return send_ms_command(mt, XA_MT_SET_IDLE_TIME, time, sizeof(time));
}

static int init_address(struct xa_mt *mt)
{
	int err = read_efuse(mt, MT_EE_MAC_ADDR, mt->address, ETH_ALEN);

	if (err)
		return err;

	/* clients only connect to 62:45:bx:xx:xx:xx */
	if (mt->address[0] != 0x62) {
		mt->address[0] = 0x62;
		mt->address[1] = 0x45;
		mt->address[2] = 0xbd;
	}
	xa_log("mt76: address %02x:%02x:%02x:%02x:%02x:%02x\n", mt->address[0],
	       mt->address[1], mt->address[2], mt->address[3], mt->address[4],
	       mt->address[5]);

	err = write_burst(mt, MT_MAC_ADDR_DW0, mt->address, ETH_ALEN);
	if (err)
		return err;
	err = write_burst(mt, MT_MAC_BSSID_DW0, mt->address, ETH_ALEN);
	if (err)
		return err;

	return send_ms_command(mt, XA_MT_SET_MAC_ADDRESS, mt->address, ETH_ALEN);
}

static int calibrate_crystal(struct xa_mt *mt)
{
	u8 trim[4];
	u16 val;
	s8 offset;
	u32 ctrl;
	int err;

	err = read_efuse(mt, MT_EE_XTAL_TRIM_2, trim, sizeof(trim));
	if (err)
		return err;

	val = (trim[3] << 8) | trim[2];
	offset = val & GENMASK(6, 0);
	if ((val & 0xff) == 0xff)
		offset = 0;
	else if (val & BIT(7))
		offset = -offset;

	val >>= 8;
	if (!val || val == 0xff) {
		err = read_efuse(mt, MT_EE_XTAL_TRIM_1, trim, sizeof(trim));
		if (err)
			return err;

		val = ((trim[3] << 8) | trim[2]) & 0xff;
		if (!val || val == 0xff)
			val = 0x14;
	}

	val = (val & GENMASK(6, 0)) + offset;
	ctrl = xa_mt_read(mt, MT_XO_CTRL5 | MT_VEND_TYPE_CFG);
	xa_mt_write(mt, MT_XO_CTRL5 | MT_VEND_TYPE_CFG, (ctrl & ~MT_XO_CTRL5_C2_VAL) | (val << 8));
	xa_mt_write(mt, MT_XO_CTRL6 | MT_VEND_TYPE_CFG, MT_XO_CTRL6_C2_CTRL);
	xa_mt_write(mt, MT_CMB_CTRL, 0x0091a7ff);
	return 0;
}

static int calibrate_radio(struct xa_mt *mt)
{
	int err;

	/* automatic gain control (AGC) */
	xa_mt_write(mt, MT_BBP(AGC, 8), 0x18365efa);
	xa_mt_write(mt, MT_BBP(AGC, 9), 0x18365efa);

	/* reset required for reliable WLAN associations */
	xa_mt_write(mt, MT_MAC_SYS_CTRL, 0);
	xa_mt_write(mt, MT_RF_BYPASS_0, 0);
	xa_mt_write(mt, MT_RF_SETTING_0, 0);

	err = calibrate(mt, MT_MCU_CAL_TEMP_SENSOR, 0);
	if (err)
		return err;
	err = calibrate(mt, MT_MCU_CAL_RXDCOC, 1);
	if (err)
		return err;
	err = calibrate(mt, MT_MCU_CAL_RC, 0);
	if (err)
		return err;

	xa_mt_write(mt, MT_MAC_SYS_CTRL, MT_MAC_SYS_CTRL_ENABLE_RX | MT_MAC_SYS_CTRL_ENABLE_TX);
	return 0;
}

static void init_registers(struct xa_mt *mt)
{
	static const u32 regs[][2] = {
		{ MT_MAC_SYS_CTRL, MT_MAC_SYS_CTRL_RESET_BBP | MT_MAC_SYS_CTRL_RESET_CSR },
		{ MT_USB_DMA_CFG, 0 },
		{ MT_MAC_SYS_CTRL, 0 },
		{ MT_PWR_PIN_CFG, 0 },
		{ MT_LDO_CTRL_1, 0x6b006464 },
		{ MT_WPDMA_GLO_CFG, 0x70 },
		{ MT_WMM_AIFSN, 0x2273 },
		{ MT_WMM_CWMIN, 0x2344 },
		{ MT_WMM_CWMAX, 0x34aa },
		{ MT_FCE_DMA_ADDR, 0x041200 },
		{ MT_TSO_CTRL, 0 },
		{ MT_PBF_SYS_CTRL, 0x080c00 },
		{ MT_PBF_TX_MAX_PCNT, 0x1fbf1f1f },
		{ MT_FCE_PSE_CTRL, 0x01 },
		{ MT_MAC_SYS_CTRL, MT_MAC_SYS_CTRL_ENABLE_RX | MT_MAC_SYS_CTRL_ENABLE_TX },
		{ MT_AUTO_RSP_CFG, 0x13 },
		{ MT_MAX_LEN_CFG, 0x3e3fff },
		{ MT_AMPDU_MAX_LEN_20M1S, 0xfffc9855 },
		{ MT_AMPDU_MAX_LEN_20M2S, 0xff },
		{ MT_BKOFF_SLOT_CFG, 0x0109 },
		{ MT_PWR_PIN_CFG, 0 },
		{ MT_EDCA_CFG_AC(0), 0x064320 },
		{ MT_EDCA_CFG_AC(1), 0x0a4700 },
		{ MT_EDCA_CFG_AC(2), 0x043238 },
		{ MT_EDCA_CFG_AC(3), 0x03212f },
		{ MT_TX_PIN_CFG, 0x150f0f },
		{ MT_TX_SW_CFG0, 0x101001 },
		{ MT_TX_SW_CFG1, 0x010000 },
		{ MT_TXOP_CTRL_CFG, 0x10583f },
		{ MT_TX_TIMEOUT_CFG, 0x0a0f90 },
		{ MT_TX_RETRY_CFG, 0x47d01f0f },
		{ MT_CCK_PROT_CFG, 0x03f40003 },
		{ MT_OFDM_PROT_CFG, 0x03f40003 },
		{ MT_MM20_PROT_CFG, 0x01742004 },
		{ MT_GF20_PROT_CFG, 0x01742004 },
		{ MT_GF40_PROT_CFG, 0x03f42084 },
		{ MT_EXP_ACK_TIME, 0x2c00dc },
		{ MT_TX_ALC_CFG_2, 0x22160a00 },
		{ MT_TX_ALC_CFG_3, 0x22160a76 },
		{ MT_TX_ALC_CFG_0, 0x3f3f1818 },
		{ MT_TX_ALC_CFG_4, 0x0606 },
		{ MT_PIFS_TX_CFG, 0x060fff },
		{ MT_RX_FILTR_CFG, 0x017f17 },
		{ MT_LEGACY_BASIC_RATE, 0x017f },
		{ MT_HT_BASIC_RATE, 0x8003 },
		{ MT_PN_PAD_MODE, 0x02 },
		{ MT_TXOP_HLDR_ET, 0x02 },
		{ MT_TX_PROT_CFG6, 0xe3f42004 },
		{ MT_TX_PROT_CFG7, 0xe3f42084 },
		{ MT_TX_PROT_CFG8, 0xe3f42104 },
		{ MT_DACCLK_EN_DLY_CFG, 0 },
		{ MT_RF_PA_MODE_ADJ0, 0xee000000 },
		{ MT_RF_PA_MODE_ADJ1, 0xee000000 },
		{ MT_TX0_RF_GAIN_CORR, 0x0f3c3c3c },
		{ MT_TX1_RF_GAIN_CORR, 0x0f3c3c3c },
		{ MT_PBF_CFG, 0x1efebcf5 },
		{ MT_PAUSE_ENABLE_CONTROL1, 0x0a },
		{ MT_RF_BYPASS_0, 0x7f000000 },
		{ MT_RF_SETTING_0, 0x1a800000 },
		{ MT_XIFS_TIME_CFG, 0x33a40e0a },
		{ MT_FCE_L2_STUFF, 0x03ff0223 },
		{ MT_TX_RTS_CFG, 0 },
		{ MT_BEACON_TIME_CFG, 0x0640 },
		{ MT_EXT_CCA_CFG, 0xf0e4 },
		{ MT_CH_TIME_CFG, 0x015f },
	};

	for (size_t i = 0; i < ARRAY_SIZE(regs); i++)
		xa_mt_write(mt, regs[i][0], regs[i][1]);
}

int xa_mt_init_radio(struct xa_mt *mt)
{
	u8 id[4];
	int err;

	if (!read_efuse(mt, MT_EE_CHIP_ID, id, sizeof(id)))
		xa_log("mt76: chip id %04x\n", (id[1] << 8) | id[2]);

	err = select_function(mt, MT_Q_SELECT, 1);
	if (err)
		return err;
	err = set_power_mode(mt, MT_RADIO_ON);
	if (err)
		return err;
	err = load_cr(mt, MT_RF_BBP_CR);
	if (err)
		return err;

	init_registers(mt);

	err = calibrate_crystal(mt);
	if (err)
		return err;
	err = init_address(mt);
	if (err)
		return err;
	err = set_idle_time(mt);
	if (err)
		return err;
	err = calibrate_radio(mt);
	if (err)
		return err;
	err = init_channels(mt);
	if (err)
		return err;

	/* mandatory delay after a channel change */
	xa_sleep_ms(1000);

	/* normal traffic routing (a previous wake-on-wireless setup may persist) */
	err = set_wow_traffic(mt, XA_MT_WOW_TO_FIRMWARE);
	if (err)
		return err;
	err = set_wow_enable(mt, false);
	if (err)
		return err;

	return xa_mt_set_pairing(mt, false);
}

void xa_mt_stop_radio(struct xa_mt *mt)
{
	/* stop RX/TX (as xone's suspend does) so no DMA is left half-done */
	xa_mt_write(mt, MT_BEACON_TIME_CFG, 0);
	xa_mt_write(mt, MT_MAC_SYS_CTRL, 0);
}

/* ---- beacon and pairing -------------------------------------------------------- */

static int write_beacon(struct xa_mt *mt, bool pair)
{
	/* information element with Microsoft's OUI (00:50:f2), incl. the channel */
	u8 ie[] = {
		0x00, 0x00, 0xdd, 0x10, 0x00, 0x50, 0xf2, 0x11, 0x01, 0x10,
		pair, 0xa5, 0x30, 0x99, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	static const u8 broadcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	/* 3-address header + beacon body (timestamp, interval, capabilities) */
	int mgmt_len = IEEE80211_HDR_3ADDR_LEN + 12;
	u8 buf[XA_MT_TXWI_LEN + IEEE80211_HDR_3ADDR_LEN + 12 + sizeof(ie)];
	u8 *p = buf;

	ie[14] = mt->channel ? mt->channel->index : 1;

	/* beacon timestamp generated by hardware, hardware sequence control */
	xa_mt_put_txwi(p, MT_TXWI_FLAGS_TS, TXWI_RATE_OFDM, MT_TXWI_ACK_CTL_NSEQ, 0,
		       mgmt_len + sizeof(ie));
	p += XA_MT_TXWI_LEN;

	put_hdr_3addr(p, IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_BEACON, broadcast,
		      mt->address, mt->address);
	p += IEEE80211_HDR_3ADDR_LEN;

	memset(p, 0, 8);            /* timestamp */
	put_le16(p + 8, 100);       /* beacon interval (100 TU) */
	put_le16(p + 10, 0xc631);   /* capabilities as the original driver sends */
	p += 12;

	memcpy(p, ie, sizeof(ie));
	return write_burst(mt, MT_BEACON_BASE, buf, sizeof(buf));
}

int xa_mt_set_pairing(struct xa_mt *mt, bool enable)
{
	int err;

	xa_mt_write(mt, MT_RX_FILTR_CFG, enable ? 0x014f13 : 0x017f17);

	err = write_beacon(mt, enable);
	if (err)
		return err;

	/* TSF and TBTT timers on, AP mode, beacon transmission on */
	xa_mt_write(mt, MT_BEACON_TIME_CFG,
		    MT_BEACON_TIME_CFG_BEACON_TX | MT_BEACON_TIME_CFG_TBTT_EN |
		    MT_BEACON_TIME_CFG_SYNC_MODE | MT_BEACON_TIME_CFG_TIMER_EN |
		    FIELD_PREP(MT_BEACON_TIME_CFG_INTVAL, 0x0640));
	return 0;
}

/* ---- clients ------------------------------------------------------------------- */

int xa_mt_pair_client(struct xa_mt *mt, const u8 *addr)
{
	static const u8 data[] = { 0x00, 0x45, 0x55, 0x01, 0x0f, 0x8f, 0xff, 0x87, 0x1f };
	struct xa_msg *msg = xa_msg_alloc(IEEE80211_HDR_3ADDR_LEN + 2 + sizeof(data));

	if (!msg)
		return -XA_ENOMEM;

	put_hdr_3addr(xa_msg_put(msg, IEEE80211_HDR_3ADDR_LEN),
		      IEEE80211_FTYPE_MGMT | XA_MT_WLAN_RESERVED, addr, mt->address,
		      mt->address);
	*xa_msg_put(msg, 1) = XA_MT_WLAN_RESERVED;
	*xa_msg_put(msg, 1) = XA_MT_CLIENT_PAIR_RESP;
	put_data(msg, data, sizeof(data));
	return send_wlan(mt, msg);
}

int xa_mt_send_assoc_response(struct xa_mt *mt, const u8 *addr)
{
	struct xa_msg *msg = xa_msg_alloc(IEEE80211_HDR_3ADDR_LEN + 6 + 8);
	u8 *p;

	if (!msg)
		return -XA_ENOMEM;

	put_hdr_3addr(xa_msg_put(msg, IEEE80211_HDR_3ADDR_LEN),
		      IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ASSOC_RESP, addr,
		      mt->address, mt->address);
	/* capabilities 0, original status code and association ID */
	p = xa_msg_put(msg, 6);
	put_le16(p, 0);
	put_le16(p + 2, 0x0110);
	put_le16(p + 4, 0x0f00);
	memset(xa_msg_put(msg, 8), 0, 8);
	return send_wlan(mt, msg);
}

int xa_mt_associate_client(struct xa_mt *mt, u8 wcid, const u8 *addr)
{
	u8 data[] = { wcid - 1, 0x00, 0x00, 0x00, 0x40, 0x1f, 0x00, 0x00 };
	int err;

	err = write_burst(mt, MT_WCID_ADDR(wcid), addr, ETH_ALEN);
	if (!err)
		err = send_ms_command(mt, XA_MT_ADD_CLIENT, data, sizeof(data));
	if (err)
		return err;

	return xa_mt_send_assoc_response(mt, addr);
}

int xa_mt_send_client_command(struct xa_mt *mt, u8 wcid, const u8 *addr,
			      enum xa_mt_client_command cmd, const u8 *data, int len)
{
	const u8 info[] = { 0x00, 0x00, 0x00, wcid - 1, 0x00, 0x00, 0x00, 0x00 };
	struct xa_msg *msg = xa_msg_alloc(sizeof(info) + XA_MT_TXWI_LEN +
					  IEEE80211_HDR_3ADDR_LEN + 2 + len);

	if (!msg)
		return -XA_ENOMEM;

	put_data(msg, info, sizeof(info));
	xa_mt_put_txwi(xa_msg_put(msg, XA_MT_TXWI_LEN), TXWI_FLAGS_ACK, TXWI_RATE_OFDM,
		       MT_TXWI_ACK_CTL_REQ, wcid - 1, IEEE80211_HDR_3ADDR_LEN + 2 + len);
	put_hdr_3addr(xa_msg_put(msg, IEEE80211_HDR_3ADDR_LEN),
		      IEEE80211_FTYPE_MGMT | XA_MT_WLAN_RESERVED, addr, mt->address,
		      mt->address);
	*xa_msg_put(msg, 1) = XA_MT_WLAN_RESERVED;
	*xa_msg_put(msg, 1) = cmd;
	if (data)
		put_data(msg, data, len);

	return xa_mt_send_command(mt, msg, 0);
}

int xa_mt_set_client_key(struct xa_mt *mt, u8 wcid, const u8 *key, int len)
{
	static const u8 iv[] = { 0x01, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00 };
	u8 attr[4];
	int err;

	if (len != XA_MT_WCID_KEY_LEN)
		return -XA_EINVAL;

	put_le32(attr, FIELD_PREP(MT_WCID_ATTR_PKEY_MODE, MT_CIPHER_AES_CCMP) |
			       MT_WCID_ATTR_PAIRWISE);

	err = write_burst(mt, MT_WCID_KEY(wcid), key, len);
	if (!err)
		err = write_burst(mt, MT_WCID_IV(wcid), iv, sizeof(iv));
	if (!err)
		err = write_burst(mt, MT_WCID_ATTR(wcid), attr, sizeof(attr));
	return err;
}

int xa_mt_remove_client(struct xa_mt *mt, u8 wcid)
{
	u8 data[] = { wcid - 1, 0x00, 0x00, 0x00 };
	u8 zero[XA_MT_WCID_KEY_LEN] = { 0 };
	int err;

	err = send_ms_command(mt, XA_MT_REMOVE_CLIENT, data, sizeof(data));
	if (!err)
		err = write_burst(mt, MT_WCID_ADDR(wcid), zero, ETH_ALEN);
	if (!err)
		err = write_burst(mt, MT_WCID_IV(wcid), zero, 8);
	if (!err)
		err = write_burst(mt, MT_WCID_ATTR(wcid), zero, 4);
	if (!err)
		err = write_burst(mt, MT_WCID_KEY(wcid), zero, XA_MT_WCID_KEY_LEN);
	return err;
}
