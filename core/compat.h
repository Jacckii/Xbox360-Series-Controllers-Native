/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The few Linux kernel idioms the ported xone code relies on, in portable C.
 * The core runs on little-endian (Pi) and big-endian (Xbox 360) CPUs, so every
 * on-the-wire little-endian field goes through the get_le / put_le helpers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;

/* documentation only: fields that are little-endian on the wire */
typedef uint16_t __le16;
typedef uint32_t __le32;
#ifndef __packed
#define __packed __attribute__((packed))
#endif

#define BIT(n) (1u << (n))
#define GENMASK(h, l) (((~0u) << (l)) & (~0u >> (31 - (h))))
#define FIELD_PREP(mask, val) (((u32)(val) << __builtin_ctz(mask)) & (mask))
#define FIELD_GET(mask, reg) (((u32)(reg) & (mask)) >> __builtin_ctz(mask))

#define ETH_ALEN 6
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ROUND_UP(x, n) (((x) + (n) - 1) / (n) * (n))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

/* errors (negative, Linux values so logs read the same) */
#define XA_EIO 5
#define XA_ENOMEM 12
#define XA_EINVAL 22
#define XA_ENOSPC 28
#define XA_ENODEV 19
#define XA_ETIMEDOUT 110

static inline u16 get_le16(const void *p)
{
	const u8 *b = p;
	return (u16)(b[0] | b[1] << 8);
}

static inline u32 get_le32(const void *p)
{
	const u8 *b = p;
	return (u32)b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24;
}

static inline void put_le16(void *p, u16 v)
{
	u8 *b = p;
	b[0] = v;
	b[1] = v >> 8;
}

static inline void put_le32(void *p, u32 v)
{
	u8 *b = p;
	b[0] = v;
	b[1] = v >> 8;
	b[2] = v >> 16;
	b[3] = v >> 24;
}

static inline bool ether_addr_is_valid(const u8 *a)
{
	static const u8 zero[ETH_ALEN];
	return !(a[0] & 1) && memcmp(a, zero, ETH_ALEN);
}

/* ---- 802.11 ------------------------------------------------------------- */
#define IEEE80211_FCTL_FTYPE 0x000c
#define IEEE80211_FCTL_STYPE 0x00f0
#define IEEE80211_FCTL_TODS 0x0100
#define IEEE80211_FCTL_FROMDS 0x0200
#define IEEE80211_FCTL_PROTECTED 0x4000
#define IEEE80211_FTYPE_MGMT 0x0000
#define IEEE80211_FTYPE_CTL 0x0004
#define IEEE80211_FTYPE_DATA 0x0008
#define IEEE80211_STYPE_ASSOC_REQ 0x0000
#define IEEE80211_STYPE_ASSOC_RESP 0x0010
#define IEEE80211_STYPE_BEACON 0x0080
#define IEEE80211_STYPE_DISASSOC 0x00a0
#define IEEE80211_STYPE_QOS_DATA 0x0080
#define IEEE80211_HT_MPDU_DENSITY_4 5

#define IEEE80211_HDR_3ADDR_LEN 24
#define IEEE80211_QOS_HDR_LEN 26

/* header length from the frame control field (ieee80211_hdrlen) */
static inline unsigned int ieee80211_hdrlen(u16 fc)
{
	unsigned int len = 24;

	if ((fc & IEEE80211_FCTL_FTYPE) == IEEE80211_FTYPE_DATA) {
		if ((fc & IEEE80211_FCTL_TODS) && (fc & IEEE80211_FCTL_FROMDS))
			len += 6;
		if (fc & 0x0080) /* QoS subtypes */
			len += 2;
	} else if ((fc & IEEE80211_FCTL_FTYPE) == IEEE80211_FTYPE_CTL) {
		len = 10; /* not handled further; only needs to be sane */
	}

	return len;
}
