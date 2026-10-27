/* The matching service's pieces between its files (m2_server.c connections and sessions, m2_rooms.c rooms, discovery.c
 * the rooms of other copies). */
#ifndef M2_INTERNAL_H
#define M2_INTERNAL_H
#include "m2.h"

/* A login (0x1002): its token (tag 0x14, in every later request header), its request MAC key (0x22), the two values the
 * module relays to room servers (0x23, 0x5d) and the player. Our 0x23 is the credential a room server of another copy
 * reads: "NVC1", the session key (a room server sends each member the room key encrypted with it, tag 0x24), the NpId. */
enum { CRED_LEN = 4 + 16 + 24 };
typedef struct { int used; uint8_t token[4], key[16], cred[CRED_LEN], b5d[16], npid[24]; char online_id[17]; } Session;
int m2_cred_parse(const uint8_t *cred, size_t n, uint8_t key[16], uint8_t npid[24]);   /* 0 when it is ours */

int m2_send(int s, const uint8_t *p, size_t n);   /* a whole message (one lock for every connection: no interleaving) */
void m2_reply(int s, M2Out *o);                   /* m2_end, the log, m2_send */
const M2Field *m2_get(const uint8_t *msg, size_t n, uint16_t tag, M2Field *f);   /* a request's field by its 12-bit tag */
uint16_t m2_u16(const M2Field *f);
uint32_t m2_u32(const M2Field *f);
uint64_t m2_now_ms(void);                         /* the protocol's times: ms since 1970 */
int m2_room_port(void);                           /* this copy's room server (all interfaces) */

/* m2_rooms.c */
void m2_agent_room_op(int s, const uint8_t *msg, size_t n, const M2Head *h, Session *se);   /* 0x1008, 0x120x on the agent */
void m2_room_op(int s, const uint8_t *msg, size_t n, const M2Head *h);                     /* 0x2001, 0x32xx on a room connection */
void m2_room_conn_closed(int s);
/* a room outliving its owner's copy (m2_rooms.c, m2_proxy.c) */
void m2_rooms_start(void);                                    /* the heartbeat: each room's snapshot to its proxies every second */
int m2_room_link(int s, const uint8_t *frame, size_t n);      /* a proxy's "NP" hello or "NR" resume on a room connection: 0 accepted */
void m2_replica_update(const uint8_t *frame, size_t n);       /* an "NS" snapshot a proxy was sent: this copy's copy of the room */
int m2_room_successor(uint16_t world, uint16_t room, int nth, char host[64], int *port);   /* the nth to serve it next: 1 this copy, 0 host:port, 2 unheard of, -1 none */
int m2_room_promote(uint16_t world, uint16_t room);           /* this copy serves its copy of the room now: 0, or -1 */
size_t m2_room_resume_frame(uint16_t world, uint16_t room, uint8_t out[64]);   /* "NR" for this copy's player, 0 when it isn't a member */
int m2_proxy_port(uint16_t world, uint16_t room, const char *host, int port);   /* m2_proxy.c: the port the game reaches that room at (0: none) */
/* a room this copy hosts, as an advert carries it (discovery.c) */
typedef struct {
	uint16_t world, lobby, room; uint32_t flags; uint16_t max; uint8_t slots[16]; uint64_t pwmask;
	uint8_t have_ints; uint32_t ints[8]; uint8_t owner[24];
	uint8_t gen;   /* takeovers so far (m2_rooms.c): the higher serves it */
	uint8_t addr_len, addr[16];   /* tag 0x6c: the owner's addresses as its game reported them (mapped IPv4, 0, 0, local IPv4):
	                               * a joining game contacts the owner's own game at them before it joins (Killzone's state 6) */
} RoomAd;
int m2_rooms_snapshot(RoomAd *out, int max);   /* the rooms this copy hosts that are registered (0x1202 done) */

/* discovery.c: the rooms of other copies of the same game */
typedef struct { RoomAd ad; char host[64]; uint16_t port; } RemoteRoom;
void disc_start(const char *comm_id);
int disc_rooms(RemoteRoom *out, int max);   /* the rooms heard of lately */
void disc_poke(void);                       /* advertise now (a room changed) */
int disc_player(const char online_id[16], char host[64], uint16_t *port);   /* where that player's in-game messages go: 0 when known */
int disc_room_server(const char online_id[16], char host[64], uint16_t *port);   /* where that player's copy serves rooms: 0 when known */
int disc_name_clash(void);   /* this copy's player gave way to another of the same name: no joins this session */
int disc_status(int *rooms);   /* 1 once discovery started, and the rooms heard of lately (no wait) */
int dht_counts(char where[32]);   /* the DHT's nodes, and this copy's address on the internet as a node saw it ("": none yet) */
void disc_input(const uint8_t *b, size_t n, uint32_t from_vip);             /* a discovery packet from a linked copy (overlay.c) */

/* basic_msg.c */
int bmsg_start(void);   /* its socket: the port discovery advertises */
void bmsg_input(const uint8_t *b, size_t n);   /* a message from a linked copy (overlay.c) */
#endif
