// SPDX-License-Identifier: MIT
/* nsync_proto.c: see nsync_proto.h and PROTOCOL.md. */
#include "nsync_proto.h"

static size_t len_of(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void put_u64le(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

void nsync_hex(char *out, const uint8_t *in, size_t n) {
	static const char d[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) { out[2 * i] = d[in[i] >> 4]; out[2 * i + 1] = d[in[i] & 15]; }
	out[2 * n] = 0;
}
int nsync_unhex(uint8_t *out, size_t n, const char *in) {
	for (size_t i = 0; i < 2 * n; i++) {
		char c = in[i]; int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
		if (v < 0) return -1;
		if (i & 1) out[i / 2] |= (uint8_t)v; else out[i / 2] = (uint8_t)(v << 4);
	}
	char e = in[2 * n];
	return e == 0 || e == ' ' || e == '\n' || e == '\r' ? 0 : -1;
}
static int all_zero(const uint8_t *p, size_t n) { uint8_t a = 0; for (size_t i = 0; i < n; i++) a |= p[i]; return a == 0; }

void nsync_pair_start(NsyncPair *p, const uint8_t pc_nonce[16], const uint8_t vita_nonce[16], const char code[6],
                      const uint8_t random[32]) {
	static const char label[] = "NativSync CPace v1";
	for (int i = 0; i < 16; i++) { p->sid[i] = pc_nonce[i]; p->sid[16 + i] = vita_nonce[i]; }
	uint8_t hidden[32];
	crypto_blake2b_ctx h;
	crypto_blake2b_init(&h, 32);
	crypto_blake2b_update(&h, (const uint8_t *)label, sizeof label - 1);
	crypto_blake2b_update(&h, p->sid, 32);
	crypto_blake2b_update(&h, (const uint8_t *)code, 6);
	crypto_blake2b_final(&h, hidden);
	crypto_elligator_map(p->g, hidden);
	for (int i = 0; i < 32; i++) p->scalar[i] = random[i];
	crypto_x25519(p->mine, p->scalar, p->g);
	crypto_wipe(hidden, sizeof hidden);
}

int nsync_pair_finish(NsyncPair *p, int i_am_vita, const uint8_t other[32], uint8_t pair_key[32], uint8_t confirm[32],
                      uint8_t transcript[96]) {
	static const char label[] = "NativSync pair";
	if (all_zero(other, 32) || all_zero(p->mine, 32)) { crypto_wipe(p->scalar, 32); return -1; }
	uint8_t k[32], out[64];
	crypto_x25519(k, p->scalar, other);
	crypto_wipe(p->scalar, 32);
	if (all_zero(k, 32)) return -1;
	const uint8_t *a = i_am_vita ? other : p->mine, *b = i_am_vita ? p->mine : other;
	for (int i = 0; i < 32; i++) { transcript[i] = p->sid[i]; transcript[32 + i] = a[i]; transcript[64 + i] = b[i]; }
	crypto_blake2b_ctx h;
	crypto_blake2b_keyed_init(&h, 64, k, 32);
	crypto_blake2b_update(&h, (const uint8_t *)label, sizeof label - 1);
	crypto_blake2b_update(&h, transcript, 96);
	crypto_blake2b_final(&h, out);
	for (int i = 0; i < 32; i++) { pair_key[i] = out[i]; confirm[i] = out[32 + i]; }
	crypto_wipe(k, sizeof k); crypto_wipe(out, sizeof out);
	return 0;
}

void nsync_pair_tag(uint8_t tag[32], const uint8_t confirm[32], const char *role, const uint8_t transcript[96]) {
	crypto_blake2b_ctx h;
	crypto_blake2b_keyed_init(&h, 32, confirm, 32);
	crypto_blake2b_update(&h, (const uint8_t *)role, len_of(role));
	crypto_blake2b_update(&h, transcript, 96);
	crypto_blake2b_final(&h, tag);
}

void nsync_session(NsyncSession *s, const uint8_t pair_key[32], const uint8_t pc_nonce[16], const uint8_t vita_nonce[16],
                   int i_am_vita) {
	static const char label[] = "NativSync session";
	uint8_t out[64];
	crypto_blake2b_ctx h;
	crypto_blake2b_keyed_init(&h, 64, pair_key, 32);
	crypto_blake2b_update(&h, (const uint8_t *)label, sizeof label - 1);
	crypto_blake2b_update(&h, pc_nonce, 16);
	crypto_blake2b_update(&h, vita_nonce, 16);
	crypto_blake2b_final(&h, out);
	for (int i = 0; i < 32; i++) { s->tx[i] = out[i_am_vita ? 32 + i : i]; s->rx[i] = out[i_am_vita ? i : 32 + i]; }
	s->ntx = s->nrx = 0;
	crypto_wipe(out, sizeof out);
}

size_t nsync_seal(NsyncSession *s, uint8_t *frame, const uint8_t *plain, size_t len) {
	uint8_t nonce[24] = {0};
	put_u64le(nonce, s->ntx++);
	uint32_t body = (uint32_t)(len + 16);
	frame[0] = (uint8_t)(body >> 24); frame[1] = (uint8_t)(body >> 16); frame[2] = (uint8_t)(body >> 8); frame[3] = (uint8_t)body;
	crypto_aead_lock(frame + 20, frame + 4, s->tx, nonce, NULL, 0, plain, len);
	return len + NSYNC_FRAME_OVERHEAD;
}
long nsync_open(NsyncSession *s, uint8_t *plain, const uint8_t *body, size_t body_len) {
	if (body_len < 16 || body_len - 16 > NSYNC_PLAIN_MAX) return -1;
	uint8_t nonce[24] = {0};
	put_u64le(nonce, s->nrx);
	if (crypto_aead_unlock(plain, body, s->rx, nonce, NULL, 0, body + 16, body_len - 16)) return -1;
	s->nrx++;
	return (long)(body_len - 16);
}
uint32_t nsync_frame_length(const uint8_t head[4]) {
	return (uint32_t)head[0] << 24 | (uint32_t)head[1] << 16 | (uint32_t)head[2] << 8 | head[3];
}

void nsync_hash_init(NsyncHash *h) { crypto_blake2b_init(h, 32); }
void nsync_hash_update(NsyncHash *h, const void *data, size_t n) { crypto_blake2b_update(h, (const uint8_t *)data, n); }
void nsync_hash_final(NsyncHash *h, uint8_t out[32]) { crypto_blake2b_final(h, out); }

static void hash_str(NsyncHash *h, const char *s) { nsync_hash_update(h, s, len_of(s)); }
static void hash_dec(NsyncHash *h, uint64_t v) {
	char b[24]; int n = 0;
	do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n) nsync_hash_update(h, &b[--n], 1);
}
void nsync_manifest_file(NsyncHash *h, const char *path, uint64_t size, const uint8_t hash[32]) {
	char hex[65]; nsync_hex(hex, hash, 32);
	hash_str(h, "F "); hash_str(h, path); hash_str(h, "\t"); hash_dec(h, size); hash_str(h, "\t"); hash_str(h, hex); hash_str(h, "\n");
}
void nsync_manifest_slot(NsyncHash *h, int slot, const uint8_t hash[32]) {
	char hex[65]; nsync_hex(hex, hash, 32);
	hash_str(h, "S "); hash_dec(h, (uint64_t)slot); hash_str(h, "\t"); hash_str(h, hex); hash_str(h, "\n");
}
