// SPDX-License-Identifier: MIT
/* nsync_proto.h: the pieces of PROTOCOL.md both sides compute the same way: pairing (CPace), session frames, hashes.
 * Over Monocypher (CC0 / BSD-2). Plain C99 with no I/O, for the Vita service, Nativ's launcher and the test client. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "monocypher.h"

#define NSYNC_PORT 1340
#define NSYNC_PLAIN_MAX (128u << 10)             /* the most plain text one frame carries */
#define NSYNC_FRAME_OVERHEAD (4 + 16)            /* length, MAC */

void nsync_hex(char *out, const uint8_t *in, size_t n);   /* out: 2n + 1 bytes */
int nsync_unhex(uint8_t *out, size_t n, const char *in);  /* 0 when in is exactly 2n hex digits (then a space, newline or end) */

/* ---- pairing: CPace over X25519 and Elligator 2 (PROTOCOL.md, Pairing) ---- */
typedef struct { uint8_t sid[32], scalar[32], g[32], mine[32]; } NsyncPair;
/* random: 32 bytes from a secure source. mine is the point to send. */
void nsync_pair_start(NsyncPair *p, const uint8_t pc_nonce[16], const uint8_t vita_nonce[16], const char code[6],
                      const uint8_t random[32]);
/* from the other side's point: the pairing key, the confirmation key and the transcript (sid, A, B). -1 for a
 * low-order point (all zeros). Wipes the scalar. */
int nsync_pair_finish(NsyncPair *p, int i_am_vita, const uint8_t other[32], uint8_t pair_key[32], uint8_t confirm[32],
                      uint8_t transcript[96]);
void nsync_pair_tag(uint8_t tag[32], const uint8_t confirm[32], const char *role, const uint8_t transcript[96]);

/* ---- sessions (PROTOCOL.md, Connecting) ---- */
typedef struct { uint8_t tx[32], rx[32]; uint64_t ntx, nrx; } NsyncSession;
void nsync_session(NsyncSession *s, const uint8_t pair_key[32], const uint8_t pc_nonce[16], const uint8_t vita_nonce[16],
                   int i_am_vita);
/* plain (len <= NSYNC_PLAIN_MAX) into a whole frame (length, MAC, ciphertext); returns its size, len + 20 */
size_t nsync_seal(NsyncSession *s, uint8_t *frame, const uint8_t *plain, size_t len);
/* a frame's body (what follows its length: MAC, ciphertext) into plain; returns the plain length, or -1 if it doesn't open */
long nsync_open(NsyncSession *s, uint8_t *plain, const uint8_t *body, size_t body_len);
uint32_t nsync_frame_length(const uint8_t head[4]);   /* the body length a frame's first 4 bytes announce */

/* ---- hashes (PROTOCOL.md, Manifests) ---- */
typedef crypto_blake2b_ctx NsyncHash;
void nsync_hash_init(NsyncHash *h);
void nsync_hash_update(NsyncHash *h, const void *data, size_t n);
void nsync_hash_final(NsyncHash *h, uint8_t out[32]);
/* a manifest's canonical text, a line at a time: files in byte order of path, then slots in order */
void nsync_manifest_file(NsyncHash *h, const char *path, uint64_t size, const uint8_t hash[32]);
void nsync_manifest_slot(NsyncHash *h, int slot, const uint8_t hash[32]);
