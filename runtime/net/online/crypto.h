/* crypto.c: this install's online identity and the overlay's encryption (Monocypher, third_party/monocypher). Plain C. */
#ifndef ONLINE_CRYPTO_H
#define ONLINE_CRYPTO_H
#include <stddef.h>
#include <stdint.h>

/* this copy's static public key (X25519): the install's (online_key beside the shared settings, made on first use), or
 * this process's own when that is in use by another copy on this machine or there are no shared settings (a test) */
void oc_identity(uint8_t pk[32]);
uint64_t oc_id_of(const uint8_t pk[32]);   /* a copy's id from its static key (odd, never 0) */
void oc_keypair(uint8_t sk[32], uint8_t pk[32]);   /* an ephemeral key pair (one per handshake) */
/* a handshake's session: the initiator's and responder's keys, static and ephemeral (this copy's ephemeral secret,
 * both sides' public keys), the game's communication id. The keys to send and receive with, and the responder's proof
 * (its hello ack carries it; the initiator checks it). 0, or -1 for a key that makes a degenerate exchange */
int oc_session(int initiator, const uint8_t my_e_sk[32], const uint8_t i_s[32], const uint8_t i_e[32], const uint8_t r_s[32], const uint8_t r_e[32],
               const char comm[12], uint8_t send[32], uint8_t recv[32], uint8_t proof[16]);
/* sealing a packet: XChaCha20-Poly1305, the nonce its counter; ad authenticated, not encrypted. oc_open: 0 if it's genuine */
void oc_seal(const uint8_t key[32], uint64_t ctr, const uint8_t *ad, size_t adn, const uint8_t *in, size_t n, uint8_t *out, uint8_t mac[16]);
int oc_open(const uint8_t key[32], uint64_t ctr, const uint8_t *ad, size_t adn, const uint8_t *in, size_t n, const uint8_t mac[16], uint8_t *out);
int oc_eq16(const uint8_t a[16], const uint8_t b[16]);   /* equal, compared in constant time */
#endif
