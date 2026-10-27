// SPDX-License-Identifier: GPL-3.0-or-later
/* Known-answer tests for sha256.c: FIPS 180-2 vectors and RFC 4231 HMAC case 2.
 *
 *   cc -O2 -Wall -o test_sha256 tests/test_sha256.c core/sha256.c && ./test_sha256
 */
#include <stdio.h>
#include "../core/sha256.h"

static int check(const char *name, const u8 *got, const char *hex)
{
	char s[65];
	for (int i = 0; i < 32; i++)
		sprintf(s + 2 * i, "%02x", got[i]);
	int ok = !strcmp(s, hex);
	printf("%s %s\n", ok ? "ok  " : "FAIL", name);
	return ok;
}

int main(void)
{
	u8 out[32];
	struct sha256 c;
	struct hmac_sha256 h;
	int ok = 1;

	sha256_init(&c); sha256_update(&c, "abc", 3); sha256_final(&c, out);
	ok &= check("sha256(abc)", out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

	const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	sha256_init(&c); sha256_update(&c, m, strlen(m)); sha256_final(&c, out);
	ok &= check("sha256(448-bit msg)", out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

	sha256_init(&c);
	for (int i = 0; i < 1000000; i++) sha256_update(&c, "a", 1);
	sha256_peek(&c, out);
	ok &= check("sha256(1M x a) via peek", out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

	hmac_sha256_init(&h, (const u8 *)"Jefe", 4);
	hmac_sha256_update(&h, "what do ya want for nothing?", 28);
	hmac_sha256_final(&h, out);
	ok &= check("hmac-sha256 RFC4231 #2", out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

	return ok ? 0 : 1;
}
