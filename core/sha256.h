/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SHA-256, HMAC-SHA256 and the GIP key-derivation PRF. Byte-oriented, endian-neutral. */
#pragma once

#include "compat.h"

#define SHA256_LEN 32
#define SHA256_BLOCK 64

struct sha256 {
	u32 state[8];
	u64 bytes;
	u8 buf[SHA256_BLOCK];
};

void sha256_init(struct sha256 *ctx);
void sha256_update(struct sha256 *ctx, const void *data, size_t len);
void sha256_final(struct sha256 *ctx, u8 out[SHA256_LEN]);

/* the hash of everything so far, leaving ctx usable (a copy is finalised) */
void sha256_peek(const struct sha256 *ctx, u8 out[SHA256_LEN]);

struct hmac_sha256 {
	struct sha256 inner, outer;
};

void hmac_sha256_init(struct hmac_sha256 *h, const u8 *key, size_t key_len);
void hmac_sha256_update(struct hmac_sha256 *h, const void *data, size_t len);
void hmac_sha256_final(struct hmac_sha256 *h, u8 out[SHA256_LEN]);

/*
 * GIP PRF (xone gip_auth_compute_prf): A1 = HMAC(key, label | seed), then
 * out_i = HMAC(key, A_i | label | seed), A_i+1 = HMAC(key, A_i).
 */
void gip_prf(const char *label, const u8 *key, size_t key_len, const u8 *seed,
	     size_t seed_len, u8 *out, size_t out_len);
