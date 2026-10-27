/* The local matching service (docs/online.md): Sony's NP Matching2 protocol served to the game's own
 * np_matching2 module over loopback, so a player's own Nativ can host rooms. Plain C: no guest memory, no Cpu. */
#ifndef M2_H
#define M2_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ---- the service (m2_server.c) */
/* Starts it on first use: the port of its plain connection on 127.0.0.1, which the signed server list names (0: it
 * couldn't start). Its second listener stands for the server's TLS port 443, mapped there by net.c. */
int m2_service_port(void);
int m2_log(void);   /* RECOMP_M2_LOG=1: every message in and out, decoded (2: whole fields) */
void m2_online_start(const char *comm_id);   /* the service, discovery, the overlay and the DHT now (np.c, at sceNpInit) */
/* the online service at a glance (status.c): the F10 menu's Online rows; while a game plays online the same goes beside
 * the shared settings every 5 s (online_status.txt, the launcher's) */
typedef struct { int started, port, linked, known, dht_nodes, rooms_heard, rooms_mine, clash; char name[17], pub[32]; } OnlineStatus;
void online_status(OnlineStatus *s);
void online_status_tick(void);   /* (discovery's once-a-second loop) */

/* ---- the wire (m2_wire.c). A message is a 4-byte prefix {0x13, class, u16 BE total length} and a 0x1c-byte header
 * {u16 BE opcode, two bytes, u32 BE, four bytes, the first 16 bytes of an HMAC-SHA1 over the message}, then fields:
 * {u16 BE tag (its type in the top 4 bits), u16 BE length, the data}. */
enum { M2_PREFIX = 4, M2_HEAD = 0x1c };
typedef struct {
	uint8_t ver, cls; uint16_t len;   /* the prefix */
	uint16_t op; uint8_t b6, b7; uint32_t id; uint8_t r[4]; uint8_t mac[16];   /* the header */
} M2Head;
int m2_parse_head(const uint8_t *p, size_t n, M2Head *h);   /* 0, or -1 when n is short or the prefix isn't Sony's */
typedef struct { uint16_t tag; uint8_t type; uint16_t len; const uint8_t *data; } M2Field;
/* the fields of a message body one by one: 1 and the next in *f, 0 at the end, -1 when a length runs past the body */
int m2_next_field(const uint8_t *body, size_t n, size_t *at, M2Field *f);
void m2_dump(FILE *out, const char *dir, const uint8_t *p, size_t n);   /* a message decoded, for the log */
/* writing a server message: m2_begin (class 2 a reply, 4 an event; the header's opcode, result and request id), fields
 * appended with m2_put (the tag word as the client writes it: type << 12 | tag), m2_end the total length; at most cap */
typedef struct { uint8_t *p; size_t n, cap; int bad; } M2Out;
void m2_begin(M2Out *o, uint8_t cls, uint16_t op, uint8_t result, uint32_t id);
void m2_put(M2Out *o, uint16_t tagword, const void *data, size_t len);
void m2_put16(M2Out *o, uint16_t tagword, uint16_t v);
size_t m2_end(M2Out *o);   /* the message's length, 0 when it didn't fit */
/* a request's MAC: the first 16 bytes of HMAC-SHA1 over the message with the MAC field zeroed */
void m2_mac(const uint8_t *key, size_t kn, const uint8_t *msg, size_t n, uint8_t out[16]);

/* ---- NP Basic in-game data messages between players (basic_msg.c), for np.c: up to 512 bytes to a player by online
 * id from the sender's NpId (36 bytes); -1 when that player's copy isn't known. bmsg_poll: the next one received, its
 * size (at most cap), or -1 when none waits. */
int bmsg_send(const char to[16], const uint8_t from[36], const uint8_t *data, size_t n);
int bmsg_poll(uint8_t from[36], uint8_t *data, size_t cap);

/* ---- STUN for the module's signaling (stun.c): started when net.c resolves the STUN server's name to this machine;
 * -1 when another copy here answers instead */
int stun_start(void);

#endif
