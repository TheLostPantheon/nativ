/* The overlay (overlay.c, ONLINE.md milestone 3): every copy's link to the others, over one UDP socket. Each copy has a
 * virtual IPv4 address (198.18.0.0/15, a range real networks don't route) that it reports to the game as its own; what
 * the game sends to another copy's virtual address travels over the link, wherever that copy is (the same LAN, behind
 * a NAT on the internet). Plain C, no guest memory: net.c is its only user on the system side. */
#ifndef OVERLAY_H
#define OVERLAY_H
#include <stddef.h>
#include <stdint.h>

int ov_on(void);                   /* the overlay carries play (settings online_overlay, on in play; RECOMP_ONLINE_OVERLAY wins) */
int ov_start(const char comm[12]);   /* its socket's port, or 0 (started once; discovery starts it) */
uint64_t ov_self_id(void);         /* this copy's id (random, made on first use; discovery's too) */
uint32_t ov_self_vip(void);        /* this copy's virtual address (network order), from its id: known before ov_start */
uint32_t ov_vip_of(uint64_t id);   /* a copy's virtual address, from its id */
int ov_is_vip(uint32_t ip);        /* (network order) in the overlay's range */

/* datagrams between ports of two copies (net.c's P2P ports): to a copy by virtual address, 0 or -1 (not linked) */
int ov_send_dgram(uint32_t dst_vip, uint16_t dst_port, uint16_t src_port, const uint8_t *data, size_t n);
typedef void (*OvDgramSink)(uint32_t src_vip, uint16_t src_port, uint16_t dst_port, const uint8_t *data, size_t n);
void ov_set_dgram_sink(OvDgramSink sink);

/* a TCP connection to another copy's port, carried as a reliable stream: the host socket the caller uses as that
 * connection (one end of a loopback pair), or -1. A copy accepts streams only to ports it allows (its room server). */
int ov_stream_open(uint32_t dst_vip, uint16_t dst_port);
void ov_allow_port(uint16_t port);

/* discovery.c and basic_msg.c: their datagrams to every linked copy / one copy */
void ov_broadcast_disc(const uint8_t *data, size_t n);
int ov_send_basic(uint32_t dst_vip, const uint8_t *data, size_t n);
void ov_probe(const char *host, uint16_t port);   /* a copy may be there: hellos until one answers (known peers, the DHT) */
void ov_add_candidate(uint64_t id, uint32_t ip, uint16_t port);   /* (network order) where a copy heard of may be reached */
int ov_counts(int *linked, int *known);   /* the copies linked now and known at all; the overlay's port (0: off) */
void ov_relay(int on);   /* a node (servers/nativ-node): relays between the copies linked to it, of any game; before ov_start */
uint64_t ov_relayed(void);   /* a node: the bytes it has passed on */
void ov_streams_dump(void);   /* every stream's state, to stderr (a proxy that gave up on its room: RECOMP_M2_LOG) */
#endif
