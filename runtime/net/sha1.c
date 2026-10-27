/* SHA-1 (FIPS 180-4) and HMAC-SHA1 (RFC 2104), written from the published algorithms: the signed matching server list
 * np_matching2 checks (http.c). */
#include <string.h>
#include "sha1.h"

static uint32_t rol(uint32_t x, int n) { return x << n | x >> (32 - n); }
static void sha1_block(uint32_t h[5], const uint8_t *p) {
	uint32_t w[80];
	for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
	for (int i = 0; i < 80; i++) {
		uint32_t f, k;
		if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
		else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
		else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
		else { f = b ^ c ^ d; k = 0xca62c1d6; }
		uint32_t t = rol(a, 5) + f + e + k + w[i];
		e = d; d = c; c = rol(b, 30); b = a; a = t;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
void sha1(const void *data, size_t n, uint8_t out[20]) {
	uint32_t h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
	const uint8_t *p = data; size_t left = n;
	for (; left >= 64; p += 64, left -= 64) sha1_block(h, p);
	uint8_t last[128] = { 0 }; memcpy(last, p, left); last[left] = 0x80;
	const size_t blocks = left + 9 > 64 ? 2 : 1; const uint64_t bits = (uint64_t)n * 8;
	for (int i = 0; i < 8; i++) last[blocks * 64 - 1 - i] = (uint8_t)(bits >> (8 * i));
	for (size_t i = 0; i < blocks; i++) sha1_block(h, last + 64 * i);
	for (int i = 0; i < 5; i++) { out[4 * i] = (uint8_t)(h[i] >> 24); out[4 * i + 1] = (uint8_t)(h[i] >> 16); out[4 * i + 2] = (uint8_t)(h[i] >> 8); out[4 * i + 3] = (uint8_t)h[i]; }
}
void hmac_sha1(const void *key, size_t kn, const void *data, size_t n, uint8_t out[20]) {
	uint8_t k[64] = { 0 };
	if (kn > 64) sha1(key, kn, k); else memcpy(k, key, kn);
	uint8_t pad[64]; uint8_t inner[20];
	for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
	{	/* sha1(ipad || data) without a copy of data: the first block is the pad */
		uint32_t h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
		sha1_block(h, pad);
		const uint8_t *p = data; size_t left = n;
		for (; left >= 64; p += 64, left -= 64) sha1_block(h, p);
		uint8_t last[128] = { 0 }; memcpy(last, p, left); last[left] = 0x80;
		const size_t blocks = left + 9 > 64 ? 2 : 1; const uint64_t bits = (uint64_t)(n + 64) * 8;
		for (int i = 0; i < 8; i++) last[blocks * 64 - 1 - i] = (uint8_t)(bits >> (8 * i));
		for (size_t i = 0; i < blocks; i++) sha1_block(h, last + 64 * i);
		for (int i = 0; i < 5; i++) { inner[4 * i] = (uint8_t)(h[i] >> 24); inner[4 * i + 1] = (uint8_t)(h[i] >> 16); inner[4 * i + 2] = (uint8_t)(h[i] >> 8); inner[4 * i + 3] = (uint8_t)h[i]; }
	}
	uint8_t outer[84];
	for (int i = 0; i < 64; i++) outer[i] = k[i] ^ 0x5c;
	memcpy(outer + 64, inner, 20);
	sha1(outer, sizeof outer, out);
}
