// SPDX-License-Identifier: GPL-3.0-or-later
/* SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104) and the GIP PRF. */
#include "sha256.h"

static const u32 K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(struct sha256 *ctx, const u8 *p)
{
	u32 w[64], a, b, c, d, e, f, g, h;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
	for (; i < 64; i++) {
		u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		u32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
	e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
	for (i = 0; i < 64; i++) {
		u32 t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
		u32 t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}
	ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
	ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(struct sha256 *ctx)
{
	static const u32 iv[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};

	memcpy(ctx->state, iv, sizeof(iv));
	ctx->bytes = 0;
}

void sha256_update(struct sha256 *ctx, const void *data, size_t len)
{
	const u8 *p = data;
	size_t used = ctx->bytes % SHA256_BLOCK;

	ctx->bytes += len;
	while (len) {
		size_t n = MIN(len, (size_t)(SHA256_BLOCK - used));

		memcpy(ctx->buf + used, p, n);
		used += n;
		p += n;
		len -= n;
		if (used == SHA256_BLOCK) {
			block(ctx, ctx->buf);
			used = 0;
		}
	}
}

void sha256_final(struct sha256 *ctx, u8 out[SHA256_LEN])
{
	u64 bits = ctx->bytes * 8;
	size_t used = ctx->bytes % SHA256_BLOCK;
	u8 pad[SHA256_BLOCK * 2] = { 0x80 };
	size_t pad_len = (used < 56 ? 56 : 120) - used;

	for (int i = 0; i < 8; i++)
		pad[pad_len + i] = bits >> (56 - 8 * i);
	sha256_update(ctx, pad, pad_len + 8);

	for (int i = 0; i < 8; i++) {
		out[4 * i] = ctx->state[i] >> 24;
		out[4 * i + 1] = ctx->state[i] >> 16;
		out[4 * i + 2] = ctx->state[i] >> 8;
		out[4 * i + 3] = ctx->state[i];
	}
}

void sha256_peek(const struct sha256 *ctx, u8 out[SHA256_LEN])
{
	struct sha256 copy = *ctx;

	sha256_final(&copy, out);
}

void hmac_sha256_init(struct hmac_sha256 *h, const u8 *key, size_t key_len)
{
	u8 k[SHA256_BLOCK] = { 0 }, pad[SHA256_BLOCK];

	if (key_len > SHA256_BLOCK) {
		struct sha256 t;

		sha256_init(&t);
		sha256_update(&t, key, key_len);
		sha256_final(&t, k);
	} else {
		memcpy(k, key, key_len);
	}

	for (int i = 0; i < SHA256_BLOCK; i++)
		pad[i] = k[i] ^ 0x36;
	sha256_init(&h->inner);
	sha256_update(&h->inner, pad, SHA256_BLOCK);

	for (int i = 0; i < SHA256_BLOCK; i++)
		pad[i] = k[i] ^ 0x5c;
	sha256_init(&h->outer);
	sha256_update(&h->outer, pad, SHA256_BLOCK);
}

void hmac_sha256_update(struct hmac_sha256 *h, const void *data, size_t len)
{
	sha256_update(&h->inner, data, len);
}

void hmac_sha256_final(struct hmac_sha256 *h, u8 out[SHA256_LEN])
{
	u8 inner[SHA256_LEN];

	sha256_final(&h->inner, inner);
	sha256_update(&h->outer, inner, SHA256_LEN);
	sha256_final(&h->outer, out);
}

void gip_prf(const char *label, const u8 *key, size_t key_len, const u8 *seed,
	     size_t seed_len, u8 *out, size_t out_len)
{
	size_t label_len = strlen(label);
	u8 a[SHA256_LEN], block_out[SHA256_LEN];
	struct hmac_sha256 h;

	hmac_sha256_init(&h, key, key_len);
	hmac_sha256_update(&h, label, label_len);
	hmac_sha256_update(&h, seed, seed_len);
	hmac_sha256_final(&h, a);

	while (out_len) {
		size_t n = MIN(out_len, (size_t)SHA256_LEN);

		hmac_sha256_init(&h, key, key_len);
		hmac_sha256_update(&h, a, SHA256_LEN);
		hmac_sha256_update(&h, label, label_len);
		hmac_sha256_update(&h, seed, seed_len);
		hmac_sha256_final(&h, block_out);
		memcpy(out, block_out, n);
		out += n;
		out_len -= n;

		hmac_sha256_init(&h, key, key_len);
		hmac_sha256_update(&h, a, SHA256_LEN);
		hmac_sha256_final(&h, a);
	}
}
