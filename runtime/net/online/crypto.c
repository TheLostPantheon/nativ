/* crypto.c (crypto.h): this install's online identity and the overlay's encryption.
 *
 * Each install has a static X25519 key (online_key beside the shared settings, 32 bytes, made on first use); a copy's id is
 * the first 8 bytes of BLAKE2b(its public key), so no copy can stand in for another without its key. A handshake (the
 * overlay's hello and hello ack, overlay.c) brings fresh ephemeral keys from both sides; the session's keys are BLAKE2b
 * over the four exchanges (ephemeral-ephemeral, each static against the other's ephemeral, static-static) and the
 * transcript (both sides' keys, the game), one key per direction. Only the holder of each static key can make them: the
 * responder proves it with its ack (a tag from the keys), the initiator with its first sealed packet. Every packet after
 * that is XChaCha20-Poly1305 with a counter for its nonce (overlay.c keeps a replay window).
 *
 * Monocypher 4.0.2 (CC0 or BSD-2, third_party/monocypher) is built here, as part of this file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#include "third_party/monocypher/monocypher.c"
#include "crypto.h"

void plat_random(void *buf, size_t n);
int settings_shared_dir(char *buf, int n);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t s_sk[32], s_pk[32]; static int have;
/* the install's key file, held for this process's life (a second copy on this machine makes its own key): 1 when sk is it */
static int key_file(uint8_t sk[32]) {
	char dir[512], path[600]; if (!settings_shared_dir(dir, sizeof dir)) return 0;
	snprintf(path, sizeof path, "%sonline_key", dir);
	uint8_t b[33]; int n = 0;
#ifdef _WIN32
	HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);   /* (no sharing: the lock) */
	if (h == INVALID_HANDLE_VALUE) return 0;
	DWORD got = 0; if (ReadFile(h, b, sizeof b, &got, 0)) n = (int)got;
	if (n != 32) { plat_random(sk, 32); DWORD w = 0; SetFilePointer(h, 0, 0, FILE_BEGIN); SetEndOfFile(h); if (!WriteFile(h, sk, 32, &w, 0) || w != 32) { CloseHandle(h); return 0; } }
	else memcpy(sk, b, 32);
	/* (the handle stays open: the lock) */
#else
	const int fd = open(path, O_RDWR | O_CREAT, 0600);
	if (fd < 0) return 0;
	if (flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return 0; }
	n = (int)read(fd, b, sizeof b);
	if (n != 32) { plat_random(sk, 32); if (ftruncate(fd, 0) || pwrite(fd, sk, 32, 0) != 32) { close(fd); return 0; } fsync(fd); }
	else memcpy(sk, b, 32);
	/* (fd stays open: the lock) */
#endif
	crypto_wipe(b, sizeof b);
	return 1;
}
void oc_identity(uint8_t pk[32]) {
	pthread_mutex_lock(&lock);
	if (!have) {
		const int kept = key_file(s_sk);
		if (!kept) plat_random(s_sk, 32);
		crypto_x25519_public_key(s_pk, s_sk); have = 1;
		fprintf(stderr, "    m2: this copy's key: %s\n", kept ? "the install's" : "its own (no shared settings, or another copy has the install's)");
	}
	memcpy(pk, s_pk, 32);
	pthread_mutex_unlock(&lock);
}
uint64_t oc_id_of(const uint8_t pk[32]) {
	uint8_t h[8]; crypto_blake2b(h, sizeof h, pk, 32);
	uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | h[i];
	return v | 1;
}
void oc_keypair(uint8_t sk[32], uint8_t pk[32]) { plat_random(sk, 32); crypto_x25519_public_key(pk, sk); }
static int dh(uint8_t out[32], const uint8_t sk[32], const uint8_t pk[32]) {   /* -1 for a low-order key (an all-zero result) */
	static const uint8_t zero[32]; crypto_x25519(out, sk, pk); return crypto_verify32(out, zero) ? 0 : -1;
}
int oc_session(int initiator, const uint8_t my_e_sk[32], const uint8_t i_s[32], const uint8_t i_e[32], const uint8_t r_s[32], const uint8_t r_e[32],
               const char comm[12], uint8_t send[32], uint8_t recv[32], uint8_t proof[16]) {
	uint8_t pk[32]; oc_identity(pk);
	uint8_t m[4 * 32 + 4 + 12 + 4 * 32], k[64]; int bad = 0;
	/* ee, se (the initiator's static, the responder's ephemeral), es (the initiator's ephemeral, the responder's static), ss */
	pthread_mutex_lock(&lock);
	if (initiator) { bad |= dh(m, my_e_sk, r_e); bad |= dh(m + 32, s_sk, r_e); bad |= dh(m + 64, my_e_sk, r_s); bad |= dh(m + 96, s_sk, r_s); }
	else { bad |= dh(m, my_e_sk, i_e); bad |= dh(m + 32, my_e_sk, i_s); bad |= dh(m + 64, s_sk, i_e); bad |= dh(m + 96, s_sk, i_s); }
	pthread_mutex_unlock(&lock);
	memcpy(m + 128, "NVL2", 4); memcpy(m + 132, comm, 12);
	memcpy(m + 144, i_s, 32); memcpy(m + 176, i_e, 32); memcpy(m + 208, r_s, 32); memcpy(m + 240, r_e, 32);
	crypto_blake2b(k, sizeof k, m, sizeof m);
	memcpy(send, initiator ? k : k + 32, 32); memcpy(recv, initiator ? k + 32 : k, 32);
	uint8_t p[16]; crypto_blake2b_keyed(p, sizeof p, k, sizeof k, (const uint8_t *)"responder's proof", 17); memcpy(proof, p, 16);
	crypto_wipe(m, sizeof m); crypto_wipe(k, sizeof k);
	return bad ? -1 : 0;
}
int oc_eq16(const uint8_t a[16], const uint8_t b[16]) { return crypto_verify16(a, b) == 0; }   /* (in constant time) */
static void nonce_of(uint8_t n[24], uint64_t ctr) { memset(n, 0, 24); for (int i = 0; i < 8; i++) n[i] = (uint8_t)(ctr >> (8 * i)); }
void oc_seal(const uint8_t key[32], uint64_t ctr, const uint8_t *ad, size_t adn, const uint8_t *in, size_t n, uint8_t *out, uint8_t mac[16]) {
	uint8_t nonce[24]; nonce_of(nonce, ctr); crypto_aead_lock(out, mac, key, nonce, ad, adn, in, n);
}
int oc_open(const uint8_t key[32], uint64_t ctr, const uint8_t *ad, size_t adn, const uint8_t *in, size_t n, const uint8_t mac[16], uint8_t *out) {
	uint8_t nonce[24]; nonce_of(nonce, ctr); return crypto_aead_unlock(out, mac, key, nonce, ad, adn, in, n) ? -1 : 0;
}
