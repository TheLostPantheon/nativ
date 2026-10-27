/* The overlay (overlay.h, ONLINE.md milestone 3): this copy's link to the other copies, over one UDP socket (settings
 * online_port, default 36581; the next free one when another copy on this machine has it). Every packet:
 *   "NVL2", u8 type, u8 0, u64 the sender's copy id, u64 the receiver's (0: whoever is there), the payload
 * all big-endian. A copy's id is its static key's (crypto.c: no copy can stand in for another), its virtual address its
 * id's. A handshake links two copies (crypto.c): the initiator's hello, the responder's ack
 *   HELLO       char comm id[12], its static key[32], its ephemeral key[32]
 *   HELLO_ACK   char comm id[12], its static key[32], its ephemeral key[32], the hello's ephemeral key[32], proof[16]
 * and every other packet is sealed with the session's key for that direction (XChaCha20-Poly1305, the header and the
 * counter authenticated): u64 counter, the payload encrypted, its tag[16]. The responder takes the initiator as linked at
 * its first sealed packet (the initiator sends a KEEP as soon as the ack checks out), the initiator the responder at its
 * ack; a sealed packet counts only once (a 64-packet window), and moves the copy's endpoint to where it came from (a
 * reply to it passes the NAT or firewall in front of it, which saw it send; a hello moves nothing). Two copies that say
 * hello to each other at once (hole punching) link once: the one with the lower id keeps its own hello, the other
 * answers it. A copy keeps its last two sessions with each other copy (a new handshake while the old one is in use).
 * Hellos go to every endpoint a copy may be at (its LAN address from discovery, known peers, the DHT) until one answers,
 * then keepalives every 10 s hold the path open; a copy silent for 30 s is looked for again by hellos (what goes to it
 * still goes to its last address meanwhile). Sealed:
 *   KEEP               nothing
 *   DGRAM              u16 source port, u16 destination port, the datagram (net.c's P2P ports)
 *   DISC               a discovery packet (discovery.c: queries, adverts)
 *   BASIC              an in-game data message (basic_msg.c)
 *   SYN                u32 stream id, u16 the port to connect to      SYNACK / RST   u32 stream id
 *   DATA               u32 stream id, u32 segment number, u8 flags (1: the sender's last), the bytes
 *   ACK                u32 stream id, u32 the next segment number expected
 * Streams carry a TCP connection to another copy's port (the joiner's to the host's room server): go-back-N over
 * segments of up to 1024 bytes, 32 in flight, resent after 400 ms without an ACK. What arrives waits in the stream's
 * buffer for its thread to write it to the local connection (the link's receiving thread never waits on a reader); a
 * segment that doesn't fit is left unacknowledged, and comes again. */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSESOCK closesocket
#define POLL WSAPoll
#define SHUT_WR SD_SEND
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSESOCK close
#define POLL poll
#endif
#include "m2_internal.h"
#include "overlay.h"
#include "crypto.h"

int settings_int(const char *key, int def);   /* (../settings.h) */
const char *settings_str(const char *key, const char *def, char *buf, int bufsize);
void plat_random(void *buf, size_t n);
void dht_start(const char comm[12], uint16_t port);   /* (dht.c) */
void dht_input(const uint8_t *b, size_t n, const struct sockaddr_in *from);
void dht_tick(void);
void upnp_start(uint16_t port);   /* (upnp.c) */

enum { T_HELLO = 1, T_HELLO_ACK, T_KEEP, T_DGRAM, T_DISC, T_BASIC, T_SYN, T_SYNACK, T_DATA, T_ACK, T_RST, T_RELAY, T_RELAYED, T_PEERS };
enum { HDR = 4 + 1 + 1 + 8 + 8, SEAL = 8 + 16, MAXPKT = 1500, MAXPEER = 64, MAXPROBE = 32, MAXCAND = 4, MSS = 1024, WIN = 32, MAXSTREAM = 32,
       RBUF = 65536, DEFAULT_PORT = 36581 };   /* SEAL: a sealed packet's counter and tag */
static int sock = -1, port; static uint64_t self; static uint32_t vip; static char comm[12];
static uint8_t self_pk[32];   /* this copy's static key (crypto.c) */
static uint8_t probe_sk[32], probe_pk[32]; static uint64_t probe_made;   /* the ephemeral key of hellos to endpoints with no copy known there yet */
static int relay_mode;   /* this is a node (servers/nativ-node): it relays between the copies linked to it, of any game */
static int no_direct;    /* RECOMP_ONLINE_NO_DIRECT=1 (a test): packets that don't come from a relay are dropped, as two copies behind NATs that won't let each other through */
static struct sockaddr_in relay_eps[8]; static int nrelay_eps;   /* the relays named (settings online_relays) */
void ov_relay(int on) { relay_mode = on; }
static uint64_t relayed;   /* (a node: the bytes it passed on) */
uint64_t ov_relayed(void) { return __atomic_load_n(&relayed, __ATOMIC_RELAXED); }
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static OvDgramSink dgram_sink;
static uint16_t allowed[8]; static int nallowed;

typedef struct { int on; uint8_t tx[32], rx[32]; uint64_t tx_ctr, rx_top, rx_bits; } Link;   /* rx_top/rx_bits: the replay window */
typedef struct { int used, up; uint64_t id, learned, last_rx, last_tx, last_hello; uint32_t vip; struct sockaddr_in at, cand[MAXCAND]; int ncand;
	uint8_t e_sk[32], e_pk[32]; int initiating; uint64_t e_made;   /* our ephemeral key for the handshake we started with it */
	uint64_t via, relay, last_direct; int is_relay; char pcomm[12];   /* via: the relay its packets go through (0: straight); relay: one that knows it (T_PEERS); is_relay: it relays; pcomm: its game's id (a node's peers) */
	uint64_t rl_at; size_t rl_bytes;   /* (a node: what it relayed for it this second) */
	Link cur, old; } Peer;   /* (cur sends; a packet under old makes it cur again: the other side may not have the new one yet) */
static Peer peers[MAXPEER];
static struct { int used, keep; struct sockaddr_in a; uint64_t last, added; } probes[MAXPROBE];   /* endpoints with no copy id yet (keep: a known peer's; the DHT's go after 2 minutes unanswered) */

static int sock_would_block(void) {
#ifdef _WIN32
	return WSAGetLastError() == WSAEWOULDBLOCK;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}
static uint64_t now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000; }
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void w64(uint8_t *p, uint64_t v) { w32(p, (uint32_t)(v >> 32)); w32(p + 4, (uint32_t)v); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t r64(const uint8_t *p) { return (uint64_t)r32(p) << 32 | r32(p + 4); }
static const char *ep(const struct sockaddr_in *a, char *b, size_t n) { char ip[32]; inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip); snprintf(b, n, "%s:%u", ip, ntohs(a->sin_port)); return b; }

int ov_on(void) {   /* on in play (internet play needs it; settings online_overlay=0: the LAN only, the game's own addresses); a headless run asks with RECOMP_ONLINE_OVERLAY=1 */
	static int v = -1;
	if (v < 0) { const char *e = getenv("RECOMP_ONLINE_OVERLAY"); v = e ? atoi(e) > 0 : settings_int("online_overlay", getenv("RECOMP_HEADLESS") ? 0 : 1) > 0; }
	return v;
}
int ov_counts(int *linked, int *known) {   /* (status.c) the copies linked now and known at all; its port, 0 off. Never waits: a busy lock gives the last counts */
	static int l_was, k_was;
	if (!pthread_mutex_trylock(&lock)) { int l = 0, k = 0; for (int i = 0; i < MAXPEER; i++) if (peers[i].used) { k++; l += peers[i].up; } l_was = l; k_was = k; pthread_mutex_unlock(&lock); }
	*linked = l_was; *known = k_was; return port;
}
uint64_t ov_self_id(void) {   /* this copy's id (its static key's: crypto.c), discovery's too */
	pthread_mutex_lock(&lock); if (!self) { oc_identity(self_pk); self = oc_id_of(self_pk); } const uint64_t r = self; pthread_mutex_unlock(&lock);
	return r;
}
static uint32_t vip_of(uint64_t id);
uint32_t ov_self_vip(void) { if (!vip) vip = vip_of(ov_self_id()); return vip; }
uint32_t ov_vip_of(uint64_t id) { return vip_of(id); }
int ov_is_vip(uint32_t ip) { return (ntohl(ip) & 0xfffe0000u) == 0xc6120000u; }
static uint32_t vip_of(uint64_t id) {   /* 198.18.0.0/15, from the copy id (never .0 or .255) */
	uint64_t h = id * 0x9e3779b97f4a7c15ull; uint32_t v = (uint32_t)(h >> 40) & 0x1ffffu;
	if ((v & 0xff) == 0 || (v & 0xff) == 0xff) v ^= 0x80;
	return htonl(0xc6120000u | v);
}

static size_t pack(uint8_t b[MAXPKT], uint8_t type, uint64_t dst, const uint8_t *data, size_t n, Link *seal) {   /* a packet: its length, 0 if too long; seal: the session it goes under (0: a handshake's, plain) */
	if (n > MAXPKT - HDR - SEAL) return 0;
	memcpy(b, "NVL2", 4); b[4] = type; b[5] = 0; w64(b + 6, self); w64(b + 14, dst);
	if (seal) { const uint64_t ctr = ++seal->tx_ctr; w64(b + HDR, ctr); oc_seal(seal->tx, ctr, b, HDR + 8, data, n, b + HDR + 8, b + HDR + 8 + n); return HDR + SEAL + n; }
	if (n) memcpy(b + HDR, data, n);
	return HDR + n;
}
static void xmit(const struct sockaddr_in *to, const uint8_t *b, size_t len) {
	if (sock < 0 || !len) return;
	if (sendto(sock, (const char *)b, (int)len, 0, (const struct sockaddr *)to, sizeof *to) < 0) {
		static uint64_t last; const uint64_t t = now_ms();
		if (t - last > 5000) { last = t; char e[48]; fprintf(stderr, "    m2: overlay: can't send to %s (errno %d)\n", ep(to, e, sizeof e), errno); }
	}
}
static void send_raw(const struct sockaddr_in *to, uint8_t type, uint64_t dst, const uint8_t *data, size_t n, Link *seal) {
	uint8_t b[MAXPKT]; xmit(to, b, pack(b, type, dst, data, n, seal));
}
static Peer *peer_by_id(uint64_t id) { for (int i = 0; i < MAXPEER; i++) if (peers[i].used && peers[i].id == id) return &peers[i]; return 0; }
static Peer *peer_by_vip(uint32_t v) { for (int i = 0; i < MAXPEER; i++) if (peers[i].used && (peers[i].at.sin_port || peers[i].via) && peers[i].vip == v) return &peers[i]; return 0; }
static Peer *peer_new(uint64_t id) {   /* (lock held) */
	Peer *p = peer_by_id(id); if (p) return p;
	int old = 0; for (int i = 0; i < MAXPEER; i++) { if (!peers[i].used) { old = i; break; } if (peers[i].last_rx < peers[old].last_rx) old = i; }
	p = &peers[old]; memset(p, 0, sizeof *p); p->used = 1; p->id = id; p->learned = now_ms();
	p->vip = vip_of(id);   /* (a copy's virtual address follows from its id: known from its first packet, whatever that is) */
	return p;
}
/* (lock held) a copy that said hello unasked: a place in the table only from one silent 30 s or more -- a flood of hellos
 * with made-up ids never pushes out a live link -- and at most 8 new ones from one address in 10 s; 0: not now */
static Peer *peer_unasked(uint64_t id, uint32_t ip) {
	static struct { uint32_t ip; uint64_t since; int n; } rate[16]; static int rnext;
	const uint64_t now = now_ms(); int r;
	for (r = 0; r < 16 && !(rate[r].ip == ip && now - rate[r].since < 10000); r++) {}
	if (r == 16) { r = rnext; rnext = (rnext + 1) % 16; rate[r].ip = ip; rate[r].since = now; rate[r].n = 0; }
	if (++rate[r].n > 8) return 0;
	int slot = -1;
	for (int i = 0; i < MAXPEER && slot < 0; i++) if (!peers[i].used) slot = i;
	for (int i = 0; i < MAXPEER && slot < 0; i++) if (now - (peers[i].last_rx ? peers[i].last_rx : peers[i].learned) > 30000) slot = i;
	if (slot < 0) return 0;
	Peer *p = &peers[slot]; memset(p, 0, sizeof *p); p->used = 1; p->id = id; p->learned = now; p->vip = vip_of(id);
	return p;
}
static void cand_add(Peer *p, const struct sockaddr_in *a) {   /* (lock held) */
	for (int i = 0; i < p->ncand; i++) if (p->cand[i].sin_addr.s_addr == a->sin_addr.s_addr && p->cand[i].sin_port == a->sin_port) return;
	if (p->ncand < MAXCAND) p->cand[p->ncand++] = *a; else p->cand[MAXCAND - 1] = *a;
}
static int relay_pkt(uint64_t relay, uint64_t to, const uint8_t *pkt, size_t len);   /* (below) */
static int peer_send(Peer *p, uint8_t type, const uint8_t *data, size_t n) {   /* (lock held; a quiet copy still gets it at its last address, or through its relay) */
	if (!p || !p->cur.on || (!p->via && !p->at.sin_port)) return -1;
	if (p->via) { uint8_t b[MAXPKT]; const size_t len = pack(b, type, p->id, data, n, &p->cur); p->last_tx = now_ms(); return len ? relay_pkt(p->via, p->id, b, len) : -1; }
	send_raw(&p->at, type, p->id, data, n, &p->cur); p->last_tx = now_ms(); return 0;
}
static int relay_pkt(uint64_t relay, uint64_t to, const uint8_t *pkt, size_t len) {   /* (lock held) a packet to a copy, through a relay: T_RELAY {u64 to, the packet} */
	Peer *r = peer_by_id(relay);
	if (!r || !r->up || r->via || len + 8 > MAXPKT - HDR - SEAL) return -1;
	uint8_t b[MAXPKT]; w64(b, to); memcpy(b + 8, pkt, len);
	return peer_send(r, T_RELAY, b, len + 8);
}
/* a hello: to a copy known (its id, our ephemeral key for it) or to an endpoint where one may be (the probes' key) */
static void hello(const struct sockaddr_in *to, Peer *p) {   /* (lock held) */
	const uint64_t now = now_ms();
	uint8_t *pk;
	if (p) {
		if (!p->initiating || now - p->e_made > 60000) { oc_keypair(p->e_sk, p->e_pk); p->e_made = now; p->initiating = 1; }   /* (a fresh one a minute on) */
		pk = p->e_pk;
	} else {
		if (!probe_made || now - probe_made > 120000) { oc_keypair(probe_sk, probe_pk); probe_made = now; }
		pk = probe_pk;
	}
	uint8_t b[12 + 32 + 32]; memcpy(b, comm, 12); memcpy(b + 12, self_pk, 32); memcpy(b + 44, pk, 32);
	if (to) send_raw(to, T_HELLO, p ? p->id : 0, b, sizeof b, 0);
	else if (p && p->relay) { uint8_t pk2[MAXPKT]; const size_t len = pack(pk2, T_HELLO, p->id, b, sizeof b, 0); relay_pkt(p->relay, p->id, pk2, len); }   /* (to a copy a relay knows: through it) */
}
static void session_install(Peer *p, const uint8_t tx[32], const uint8_t rx[32]) {   /* (lock held) the new one sends; the one before stays readable */
	p->old = p->cur;
	memset(&p->cur, 0, sizeof p->cur); p->cur.on = 1; memcpy(p->cur.tx, tx, 32); memcpy(p->cur.rx, rx, 32);
}
static int window_ok(Link *s, uint64_t ctr) {   /* (lock held) a counter not seen before, and not more than 64 behind */
	if (!ctr) return 0;
	if (ctr > s->rx_top) return 1;
	const uint64_t back = s->rx_top - ctr;
	return back < 64 && !(s->rx_bits >> back & 1);
}
static void window_mark(Link *s, uint64_t ctr) {
	if (ctr > s->rx_top) { const uint64_t sh = ctr - s->rx_top; s->rx_bits = sh >= 64 ? 0 : s->rx_bits << sh; s->rx_bits |= 1; s->rx_top = ctr; }
	else s->rx_bits |= 1ull << (s->rx_top - ctr);
}

/* ---- streams */
typedef struct {
	int used, fd, connected, done; uint64_t peer; uint32_t sid; uint16_t dport; uint64_t syn_at, opened;
	uint32_t snd_base, snd_next, rcv_next; int eof_sent, eof_rcvd, eof_done;
	struct { uint16_t n; uint8_t last; uint64_t sent; uint8_t d[MSS]; } seg[WIN];
	size_t rlen; uint8_t rbuf[RBUF];   /* (received, not yet written to the local connection) */
	uint64_t last_rx, last_ack, resends;   /* (ov_streams_dump) */
} Stream;
static Stream streams[MAXSTREAM];
static pthread_cond_t stream_cv = PTHREAD_COND_INITIALIZER;
static void stream_send_seg(Stream *s, uint32_t k) {   /* (lock held) */
	uint8_t b[9 + MSS]; const int i = (int)(k % WIN);
	w32(b, s->sid); w32(b + 4, k); b[8] = s->seg[i].last; memcpy(b + 9, s->seg[i].d, s->seg[i].n);
	peer_send(peer_by_id(s->peer), T_DATA, b, 9u + s->seg[i].n); s->seg[i].sent = now_ms();
}
static void stream_close(Stream *s, int rst) {   /* (lock held) */
	if (!s->used) return;
	if (rst) { uint8_t b[4]; w32(b, s->sid); peer_send(peer_by_id(s->peer), T_RST, b, 4); }
	if (s->fd >= 0) CLOSESOCK(s->fd);
	s->fd = -1; s->used = 0; pthread_cond_broadcast(&stream_cv);
}
static void *stream_main(void *arg) {   /* the local end: what it writes goes out as segments; resends */
	Stream *s = arg;
	for (;;) {
		pthread_mutex_lock(&lock);
		if (!s->used) { pthread_mutex_unlock(&lock); return 0; }
		const uint64_t now = now_ms();
		if (!s->connected) {
			if (now - s->opened > 6000) { stream_close(s, 1); pthread_mutex_unlock(&lock); return 0; }
			if (now - s->syn_at >= 500) { uint8_t b[6]; w32(b, s->sid); b[4] = (uint8_t)(s->dport >> 8); b[5] = (uint8_t)s->dport; peer_send(peer_by_id(s->peer), T_SYN, b, 6); s->syn_at = now; }
			pthread_mutex_unlock(&lock); struct timespec t = { 0, 50000000 }; nanosleep(&t, 0); continue;
		}
		if (s->rlen && s->fd >= 0) {   /* (what arrived, to the local connection: as much as it takes now) */
			const long w = (long)send(s->fd, (const char *)s->rbuf, (int)s->rlen, 0);
			if (w > 0) { memmove(s->rbuf, s->rbuf + w, s->rlen - (size_t)w); s->rlen -= (size_t)w; }
		}
		if (s->eof_rcvd && !s->rlen && !s->eof_done) { s->eof_done = 1; if (s->fd >= 0) shutdown(s->fd, SHUT_WR); }
		if (s->eof_sent && s->snd_base == s->snd_next && s->eof_done) { stream_close(s, 0); pthread_mutex_unlock(&lock); return 0; }
		if (s->snd_base != s->snd_next && now - s->seg[s->snd_base % WIN].sent > 400)   /* (go-back-N: everything unacked again) */
			for (uint32_t k = s->snd_base; k != s->snd_next; k++) { stream_send_seg(s, k); s->resends++; }
		const int room = !s->eof_sent && s->snd_next - s->snd_base < WIN; const int fd = s->fd;
		pthread_mutex_unlock(&lock);
		if (!room) { struct timespec t = { 0, 20000000 }; nanosleep(&t, 0); continue; }
		struct pollfd pf = { fd, POLLIN, 0 };
		if (POLL(&pf, 1, 50) <= 0) continue;
		uint8_t d[MSS]; const int n = (int)recv(fd, (char *)d, MSS, 0);
		if (n < 0 && sock_would_block()) continue;
		pthread_mutex_lock(&lock);
		if (s->used) {
			const int i = (int)(s->snd_next % WIN);
			s->seg[i].n = (uint16_t)(n > 0 ? n : 0); s->seg[i].last = n <= 0; if (n > 0) memcpy(s->seg[i].d, d, (size_t)n);
			if (n <= 0) s->eof_sent = 1;
			stream_send_seg(s, s->snd_next++);
		}
		pthread_mutex_unlock(&lock);
	}
}
static void nonblocking(int fd) {
#ifdef _WIN32
	u_long nb = 1; ioctlsocket(fd, FIONBIO, &nb);
#else
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
}
static Stream *stream_new(uint64_t peer, uint32_t sid, int fd) {   /* (lock held) */
	for (int i = 0; i < MAXSTREAM; i++) if (!streams[i].used) {
		Stream *s = &streams[i]; memset(s, 0, sizeof *s); nonblocking(fd); s->used = 1; s->peer = peer; s->sid = sid; s->fd = fd; s->opened = now_ms();
		pthread_t t; pthread_create(&t, 0, stream_main, s); pthread_detach(t); return s;
	}
	return 0;
}
void ov_streams_dump(void) {
	pthread_mutex_lock(&lock); const uint64_t now = now_ms();
	for (int i = 0; i < MAXSTREAM; i++) { const Stream *s = &streams[i]; if (!s->used) continue; const Peer *p = peer_by_id(s->peer);
		fprintf(stderr, "    m2: overlay stream %08x to %016llx (%s%s) port %u: %d sent unacked (base %u next %u), next in %u, %zu bytes waiting for the local side, data in %lld ms ago, ack %lld ms ago, %llu resends%s\n",
		        s->sid, (unsigned long long)s->peer, p && p->up ? "linked" : "not linked", p && p->via ? ", through a relay" : "", s->dport, (int)(s->snd_next - s->snd_base), s->snd_base, s->snd_next, s->rcv_next, s->rlen,
		        s->last_rx ? (long long)(now - s->last_rx) : -1, s->last_ack ? (long long)(now - s->last_ack) : -1, (unsigned long long)s->resends, s->connected ? "" : ", not connected"); }
	pthread_mutex_unlock(&lock);
}
static Stream *stream_of(uint64_t peer, uint32_t sid) { for (int i = 0; i < MAXSTREAM; i++) if (streams[i].used && streams[i].peer == peer && streams[i].sid == sid) return &streams[i]; return 0; }
static int tcp_pair(int *a, int *b) {   /* a connected loopback TCP pair (Windows has no socketpair) */
	int l = (int)socket(AF_INET, SOCK_STREAM, 0); if (l < 0) return -1;
	struct sockaddr_in sa; memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); socklen_t sl = sizeof sa;
	if (bind(l, (struct sockaddr *)&sa, sizeof sa) || listen(l, 1) || getsockname(l, (struct sockaddr *)&sa, &sl)) { CLOSESOCK(l); return -1; }
	*a = (int)socket(AF_INET, SOCK_STREAM, 0);
	if (*a < 0 || connect(*a, (struct sockaddr *)&sa, sizeof sa)) { if (*a >= 0) CLOSESOCK(*a); CLOSESOCK(l); return -1; }
	*b = (int)accept(l, 0, 0); CLOSESOCK(l);
	if (*b < 0) { CLOSESOCK(*a); return -1; }
	int one = 1; setsockopt(*a, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one); setsockopt(*b, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
	return 0;
}
int ov_stream_open(uint32_t dst_vip, uint16_t dst_port) {
	for (int k = 0; k < 30; k++) {   /* (a copy just heard of: its link may be a moment away) */
		pthread_mutex_lock(&lock); Peer *p = peer_by_vip(dst_vip); const uint64_t id = p ? p->id : 0; pthread_mutex_unlock(&lock);
		if (id) {
			int a, b; if (tcp_pair(&a, &b)) return -1;
			uint32_t sid; plat_random(&sid, 4);
			pthread_mutex_lock(&lock); Stream *s = stream_new(id, sid, b); if (s) s->dport = dst_port; pthread_mutex_unlock(&lock);
			if (!s) { CLOSESOCK(a); CLOSESOCK(b); return -1; }
			if (m2_log()) { char v[32]; inet_ntop(AF_INET, &dst_vip, v, sizeof v); fprintf(stderr, "    m2: overlay: stream %08x to %s:%u\n", sid, v, dst_port); }
			return a;
		}
		struct timespec t = { 0, 100000000 }; nanosleep(&t, 0);
	}
	return -1;
}
void ov_allow_port(uint16_t p) { pthread_mutex_lock(&lock); if (nallowed < 8) allowed[nallowed++] = p; pthread_mutex_unlock(&lock); }
static void on_stream(Peer *p, uint8_t type, const uint8_t *d, size_t n) {   /* (lock held) */
	if (n < 4) return;
	const uint32_t sid = r32(d); Stream *s = stream_of(p->id, sid);
	if (type == T_SYN && n >= 6) {
		if (s) { uint8_t b[4]; w32(b, sid); peer_send(p, T_SYNACK, b, 4); return; }   /* (our SYNACK was lost) */
		const uint16_t dport = (uint16_t)(d[4] << 8 | d[5]); int ok = 0;
		for (int i = 0; i < nallowed; i++) ok |= allowed[i] == dport;
		uint8_t b[4]; w32(b, sid);
		if (!ok) { peer_send(p, T_RST, b, 4); return; }
		const int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in sa; memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = htons(dport);
		if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa)) { if (fd >= 0) CLOSESOCK(fd); peer_send(p, T_RST, b, 4); return; }
		int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
		s = stream_new(p->id, sid, fd); if (!s) { CLOSESOCK(fd); peer_send(p, T_RST, b, 4); return; }
		s->connected = 1; s->dport = dport; peer_send(p, T_SYNACK, b, 4);
		if (m2_log()) fprintf(stderr, "    m2: overlay: stream %08x from a copy to port %u\n", sid, dport);
		return;
	}
	if (!s) { if (type == T_DATA) { uint8_t b[4]; w32(b, sid); peer_send(p, T_RST, b, 4); } return; }
	if (type == T_SYNACK) { s->connected = 1; return; }
	if (type == T_RST) { stream_close(s, 0); return; }
	if (type == T_ACK && n >= 8) { const uint32_t a = r32(d + 4); s->last_ack = now_ms(); if ((int32_t)(a - s->snd_base) > 0 && (int32_t)(s->snd_next - a) >= 0) s->snd_base = a; return; }
	if (type == T_DATA && n >= 9) {
		s->connected = 1; s->last_rx = now_ms();
		const uint32_t k = r32(d + 4);
		if (k == s->rcv_next && s->rlen + (n - 9) <= RBUF) {   /* (no room: unacknowledged, it comes again) */
			memcpy(s->rbuf + s->rlen, d + 9, n - 9); s->rlen += n - 9;
			if (d[8] & 1) s->eof_rcvd = 1;
			s->rcv_next++;
		}
		uint8_t b[8]; w32(b, sid); w32(b + 4, s->rcv_next); peer_send(p, T_ACK, b, 8);
	}
}

/* ---- the link */
static int relay_ep(const struct sockaddr_in *a) {   /* a relay named in the settings is there */
	for (int i = 0; i < nrelay_eps; i++) if (relay_eps[i].sin_addr.s_addr == a->sin_addr.s_addr && relay_eps[i].sin_port == a->sin_port) return 1;
	return 0;
}
static void linked(Peer *p, const struct sockaddr_in *from) {   /* (lock held) */
	char e[48], v[32]; inet_ntop(AF_INET, &p->vip, v, sizeof v);
	if (p->via) fprintf(stderr, "    m2: overlay: linked to a copy through the relay at %s (virtual %s, sealed end to end)\n", ep(from, e, sizeof e), v);
	else fprintf(stderr, "    m2: overlay: linked to a %s at %s (virtual %s, sealed)\n", !relay_mode && (p->is_relay || relay_ep(from)) ? "relay" : "copy", ep(from, e, sizeof e), v);
}
static void probe_done(const struct sockaddr_in *from) {   /* (lock held) a copy answered there */
	for (int i = 0; i < MAXPROBE; i++) if (probes[i].used && probes[i].a.sin_addr.s_addr == from->sin_addr.s_addr && probes[i].a.sin_port == from->sin_port) probes[i].used = 0;
}
static void relay_forward(Peer *x, uint64_t to, const uint8_t *pkt, size_t len) {   /* (lock held; a node) x's packet on to the copy it names: T_RELAYED {u64 x's id, the packet} */
	Peer *y = peer_by_id(to);
	if (!y || !y->up || y->via || y == x || memcmp(y->pcomm, x->pcomm, 12) || len + 8 > MAXPKT - HDR - SEAL) return;   /* (copies of one game only) */
	const uint64_t now = now_ms();
	if (now - x->rl_at >= 1000) { x->rl_at = now; x->rl_bytes = 0; }
	if (x->rl_bytes + len > 512 * 1024) return;   /* (512 KB a second from one copy: a match is ~20-50 KB) */
	x->rl_bytes += len; __atomic_add_fetch(&relayed, len, __ATOMIC_RELAXED);
	uint8_t b[MAXPKT]; w64(b, x->id); memcpy(b + 8, pkt, len);
	peer_send(y, T_RELAYED, b, len + 8);
}
static void input_at(const uint8_t *b, size_t n, const struct sockaddr_in *from, uint64_t via, uint64_t via_from);
static void probe_add(const struct sockaddr_in *a, int keep);
static void input(const uint8_t *b, size_t n, const struct sockaddr_in *from) { input_at(b, n, from, 0, 0); }
/* a packet from `from`, or (via: the relay's id) one a relay passed on from the copy via_from */
static void input_at(const uint8_t *b, size_t n, const struct sockaddr_in *from, uint64_t via, uint64_t via_from) {
	if (n < HDR || memcmp(b, "NVL2", 4)) return;
	const uint8_t type = b[4]; const uint64_t src = r64(b + 6), dst = r64(b + 14);
	if (src == self || (dst && dst != self) || (via && src != via_from)) return;
	if (!via && no_direct && !relay_ep(from)) return;   /* (the test of copies that can't reach each other) */
	const uint8_t *d = b + HDR; size_t dn = n - HDR;
	pthread_mutex_lock(&lock);
	Peer *p = peer_by_id(src);
	if (m2_log() >= 2 && (type == T_HELLO || type == T_HELLO_ACK)) { char e[48]; fprintf(stderr, "    m2: overlay: %s from %s (copy %016llx, %zu bytes)\n", type == T_HELLO ? "hello" : "hello ack", ep(from, e, sizeof e), (unsigned long long)src, dn); }
	if (type == T_HELLO) {   /* comm, its static key, its ephemeral key: answered as the responder */
		if (dn < 76 || (!relay_mode && memcmp(d, comm, 12)) || oc_id_of(d + 12) != src) { if (m2_log() >= 2) fprintf(stderr, "    m2: overlay: that hello isn't this game's, or its id isn't its key's\n"); pthread_mutex_unlock(&lock); return; }   /* (another game's copy, or an id that isn't its key's; a node takes every game's) */
		if (p && !p->up && p->initiating && self < src) { if (!via) cand_add(p, from); pthread_mutex_unlock(&lock); return; }   /* (both said hello: the lower id's goes on, and its next one goes here) */
		if (!p && !(p = peer_unasked(src, from->sin_addr.s_addr))) { pthread_mutex_unlock(&lock); return; }
		const char *pc = relay_mode ? (const char *)d : comm; memcpy(p->pcomm, pc, 12);
		uint8_t r_sk[32], r_pk[32], tx[32], rx[32], proof[16]; oc_keypair(r_sk, r_pk);
		if (oc_session(0, r_sk, d + 12, d + 44, self_pk, r_pk, pc, tx, rx, proof)) { pthread_mutex_unlock(&lock); return; }
		session_install(p, tx, rx); p->initiating = 0;
		if (!p->up && !via) cand_add(p, from);
		uint8_t a[12 + 32 + 32 + 32 + 16]; memcpy(a, pc, 12); memcpy(a + 12, self_pk, 32); memcpy(a + 44, r_pk, 32); memcpy(a + 76, d + 44, 32); memcpy(a + 108, proof, 16);
		if (via) { uint8_t pk2[MAXPKT]; const size_t len = pack(pk2, T_HELLO_ACK, src, a, sizeof a, 0); relay_pkt(via, src, pk2, len); }
		else { send_raw(from, T_HELLO_ACK, src, a, sizeof a, 0); probe_done(from); }
		pthread_mutex_unlock(&lock);
		return;
	}
	if (type == T_HELLO_ACK) {   /* comm, its static key, its ephemeral key, our hello's, its proof: the responder, linked */
		if (dn < 124 || memcmp(d, comm, 12) || oc_id_of(d + 12) != src) { pthread_mutex_unlock(&lock); return; }
		const uint8_t *e_sk = 0, *e_pk = 0;
		if (p && p->initiating && !memcmp(d + 76, p->e_pk, 32)) { e_sk = p->e_sk; e_pk = p->e_pk; }
		else if (probe_made && !memcmp(d + 76, probe_pk, 32)) { e_sk = probe_sk; e_pk = probe_pk; }
		uint8_t tx[32], rx[32], proof[16];
		if (!e_sk || oc_session(1, e_sk, self_pk, e_pk, d + 12, d + 44, comm, tx, rx, proof) || !oc_eq16(proof, d + 108)) { if (m2_log() >= 2) fprintf(stderr, "    m2: overlay: that ack %s\n", e_sk ? "fails its proof" : "answers no hello of ours"); pthread_mutex_unlock(&lock); return; }   /* (an ack to no hello of ours, or a proof that fails) */
		if (!p) p = peer_new(src);
		session_install(p, tx, rx); p->initiating = 0; memcpy(p->pcomm, comm, 12);
		const int was = p->up; p->up = 1; p->last_rx = now_ms();
		if (via) p->via = via; else { p->at = *from; p->via = 0; p->last_direct = p->last_rx; probe_done(from); }
		if (!was) linked(p, from);
		peer_send(p, T_KEEP, 0, 0);   /* (sealed: the responder's proof that this copy is who it said) */
		pthread_mutex_unlock(&lock);
		return;
	}
	if (!p || dn < SEAL || dn - SEAL > MAXPKT) { pthread_mutex_unlock(&lock); return; }
	const uint64_t ctr = r64(d); const size_t cn = dn - SEAL; uint8_t pt[MAXPKT]; Link *ss = 0;
	if (p->cur.on && window_ok(&p->cur, ctr) && !oc_open(p->cur.rx, ctr, b, HDR + 8, d + 8, cn, d + 8 + cn, pt)) ss = &p->cur;
	else if (p->old.on && window_ok(&p->old, ctr) && !oc_open(p->old.rx, ctr, b, HDR + 8, d + 8, cn, d + 8 + cn, pt)) {   /* (the other copy goes on with the one before: it is ours again) */
		const Link t = p->cur; p->cur = p->old; p->old = t; ss = &p->cur;
	}
	if (!ss) { pthread_mutex_unlock(&lock); return; }   /* (not sealed by that copy, or a replay) */
	window_mark(ss, ctr);
	const uint64_t now = now_ms();
	const int was = p->up; p->up = 1; p->last_rx = now; p->initiating = 0;
	if (!via) { p->at = *from; p->via = 0; p->last_direct = now; }   /* (straight when it can be) */
	else if (!was || p->via || now - p->last_direct > 15000) p->via = via;
	if (!was) linked(p, from);
	d = pt; dn = cn;
	const uint32_t pvip = p->vip;
	switch (type) {
	case T_RELAY: if (relay_mode && dn >= 8 + HDR) relay_forward(p, r64(d), d + 8, dn - 8); break;
	case T_RELAYED:   /* a copy's packet, passed on by p: as if it had come from that copy */
		if (dn >= 8 + HDR && !via) { const uint64_t rid = p->id, fid = r64(d); p->is_relay = 1; uint8_t inner[MAXPKT]; memcpy(inner, d + 8, dn - 8); pthread_mutex_unlock(&lock); input_at(inner, dn - 8, from, rid, fid); return; }
		break;
	case T_PEERS:   /* the copies of this game a relay has: {u64 id, u32 address, u16 port} each, as it sees them */
		if (via || relay_mode) break;
		p->is_relay = 1;
		for (size_t k = 0; k + 14 <= dn; k += 14) {
			const uint64_t id = r64(d + k); if (id == self || id == p->id) continue;
			Peer *q = peer_by_id(id); if (!q) q = peer_new(id);
			q->relay = p->id;
			if (!q->up) { struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; memcpy(&a.sin_addr, d + k + 8, 4); memcpy(&a.sin_port, d + k + 12, 2); if (a.sin_port) cand_add(q, &a); }
		}
		break;
	case T_DGRAM: if (dn >= 4 && dgram_sink) { const uint16_t sp = (uint16_t)(d[0] << 8 | d[1]), dp = (uint16_t)(d[2] << 8 | d[3]); OvDgramSink f = dgram_sink; pthread_mutex_unlock(&lock); f(pvip, sp, dp, d + 4, dn - 4); return; } break;
	case T_DISC: pthread_mutex_unlock(&lock); disc_input(d, dn, pvip); return;
	case T_BASIC: pthread_mutex_unlock(&lock); bmsg_input(d, dn); return;
	case T_SYN: case T_SYNACK: case T_DATA: case T_ACK: case T_RST: on_stream(p, type, d, dn); break;
	default: break;
	}
	pthread_mutex_unlock(&lock);
}
static void *rx_main(void *arg) {
	(void)arg; uint8_t b[MAXPKT];
	for (;;) {
		struct sockaddr_in from; socklen_t l = sizeof from;
		const int n = (int)recvfrom(sock, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &l);
		if (n > 0 && b[0] == 'd') dht_input(b, (size_t)n, &from);   /* (a bencoded dict: the DHT's) */
		else if (n > 0) input(b, (size_t)n, &from);
	}
	return 0;
}
static void *timer_main(void *arg) {   /* hellos to copies not linked, keepalives to the linked, the silent unlinked */
	(void)arg;
	for (;;) {
		struct timespec t = { 0, 250000000 }; nanosleep(&t, 0);
		pthread_mutex_lock(&lock);
		const uint64_t now = now_ms();   /* (after the lock: a packet the receiving thread noted meanwhile is older than now) */
		for (int i = 0; i < MAXPEER; i++) {
			Peer *p = &peers[i]; if (!p->used) continue;
			if (p->up && now - p->last_rx > 30000) {
				char e[48]; p->up = 0; fprintf(stderr, "    m2: overlay: a copy went quiet (nothing from %s for %llu ms, %llu ms since we sent)\n", ep(&p->at, e, sizeof e), (unsigned long long)(now - p->last_rx), (unsigned long long)(now - p->last_tx));
				if (p->at.sin_port) cand_add(p, &p->at);
				if (p->via) { p->relay = p->via; p->via = 0; }   /* (hellos through that relay again) */
			}
			if (p->up) { if (now - p->last_tx > 10000) peer_send(p, T_KEEP, 0, 0); continue; }
			if (now - p->last_rx > 600000 && now - p->learned > 600000) { p->used = 0; continue; }
			if (now - p->last_hello >= 1000) {
				for (int k = 0; k < p->ncand; k++) hello(&p->cand[k], p);
				if (p->relay && (no_direct || now - p->learned > 3000)) hello(0, p);   /* (through the relay that knows it, once straight hellos had their chance) */
				p->last_hello = now;
			}
		}
		static uint64_t last_peers;
		if (relay_mode && now - last_peers >= 5000) {   /* a node: each copy hears of the others of its game, where it sees them (they try straight, then through here) */
			last_peers = now;
			for (int i = 0; i < MAXPEER; i++) {
				Peer *x = &peers[i]; if (!x->used || !x->up || x->via) continue;
				uint8_t b[64 * 14]; int k = 0;
				for (int j = 0; j < MAXPEER && k < 64; j++) {
					const Peer *y = &peers[j]; if (j == i || !y->used || !y->up || y->via || memcmp(y->pcomm, x->pcomm, 12)) continue;
					w64(b + 14 * k, y->id); memcpy(b + 14 * k + 8, &y->at.sin_addr, 4); memcpy(b + 14 * k + 12, &y->at.sin_port, 2); k++;
				}
				if (k) peer_send(x, T_PEERS, b, 14u * (unsigned)k);
			}
		}
		for (int i = 0; i < MAXPROBE; i++) {
			if (!probes[i].used) continue;
			if (!probes[i].keep && now - probes[i].added > 120000) { probes[i].used = 0; continue; }
			if (now - probes[i].last >= 2000) { hello(&probes[i].a, 0); probes[i].last = now; }
		}
		pthread_mutex_unlock(&lock);
		dht_tick();
	}
	return 0;
}

int ov_start(const char comm_id[12]) {
	static int started; if (started) return port; started = 1;
	ov_self_vip(); memcpy(comm, comm_id, 12);   /* (ov_self_vip: the key, and the id from it, before the first packet) */
	const int want = settings_int("online_port", DEFAULT_PORT);
	for (int k = 0; k < 16 && sock < 0; k++) {
		int s = (int)socket(AF_INET, SOCK_DGRAM, 0); if (s < 0) break;
		struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)(want + k)); a.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(s, (struct sockaddr *)&a, sizeof a)) { CLOSESOCK(s); continue; }
		sock = s; port = want + k;
	}
	if (sock < 0) { fprintf(stderr, "    m2: overlay: no port\n"); return 0; }
	pthread_t t; pthread_create(&t, 0, rx_main, 0); pthread_detach(t); pthread_create(&t, 0, timer_main, 0); pthread_detach(t);
	char v[32]; inet_ntop(AF_INET, &vip, v, sizeof v);
	fprintf(stderr, "    m2: overlay on port %d, this copy's virtual address %s\n", port, v);
	no_direct = getenv("RECOMP_ONLINE_NO_DIRECT") && atoi(getenv("RECOMP_ONLINE_NO_DIRECT")) > 0;
	if (!relay_mode) {   /* the relays named (settings online_relays = <host>[:<port>][,...], RECOMP_ONLINE_RELAYS wins): linked to as any copy */
		char rl[512]; const char *e = getenv("RECOMP_ONLINE_RELAYS");
		if (e) snprintf(rl, sizeof rl, "%s", e); else settings_str("online_relays", "", rl, sizeof rl);
		for (char *q = strtok(rl, ", "); q && nrelay_eps < 8; q = strtok(0, ", ")) {
			char *c = strrchr(q, ':'); const int rp = c ? atoi(c + 1) : DEFAULT_PORT; if (c) *c = 0;
			struct addrinfo hints, *res = 0; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
			if (getaddrinfo(q, 0, &hints, &res) || !res) { fprintf(stderr, "    m2: overlay: relay %s: no such host\n", q); continue; }
			struct sockaddr_in a = *(struct sockaddr_in *)res->ai_addr; a.sin_port = htons((uint16_t)rp); freeaddrinfo(res);
			relay_eps[nrelay_eps++] = a;
			probe_add(&a, 1);
		}
	}
	upnp_start((uint16_t)port); dht_start(comm, (uint16_t)port);
	return port;
}
void ov_set_dgram_sink(OvDgramSink sink) { dgram_sink = sink; }
int ov_send_dgram(uint32_t dst_vip, uint16_t dst_port, uint16_t src_port, const uint8_t *data, size_t n) {
	if (n > MAXPKT - HDR - SEAL - 4) return -1;
	uint8_t b[MAXPKT]; b[0] = (uint8_t)(src_port >> 8); b[1] = (uint8_t)src_port; b[2] = (uint8_t)(dst_port >> 8); b[3] = (uint8_t)dst_port; memcpy(b + 4, data, n);
	pthread_mutex_lock(&lock); Peer *p = peer_by_vip(dst_vip); const int r = peer_send(p, T_DGRAM, b, n + 4); const int relayed = p && p->via; pthread_mutex_unlock(&lock);
	if (r && relayed && n + 4 + 8 + HDR + SEAL > MAXPKT - HDR - SEAL) { static int said; if (!said++) fprintf(stderr, "    m2: overlay: a %zu-byte datagram is too long to go through a relay: dropped\n", n); }
	return r;
}
void ov_broadcast_disc(const uint8_t *data, size_t n) {
	pthread_mutex_lock(&lock); for (int i = 0; i < MAXPEER; i++) if (peers[i].used && peers[i].up) peer_send(&peers[i], T_DISC, data, n); pthread_mutex_unlock(&lock);
}
int ov_send_basic(uint32_t dst_vip, const uint8_t *data, size_t n) {
	pthread_mutex_lock(&lock); const int r = peer_send(peer_by_vip(dst_vip), T_BASIC, data, n); pthread_mutex_unlock(&lock);
	return r;
}
static void probe_add(const struct sockaddr_in *a, int keep) {
	pthread_mutex_lock(&lock);
	for (int k = 0; k < MAXPEER; k++) if (peers[k].used && peers[k].up && peers[k].at.sin_addr.s_addr == a->sin_addr.s_addr && peers[k].at.sin_port == a->sin_port) { pthread_mutex_unlock(&lock); return; }   /* (linked already) */
	int i, free_i = -1; for (i = 0; i < MAXPROBE; i++) { if (probes[i].used && probes[i].a.sin_addr.s_addr == a->sin_addr.s_addr && probes[i].a.sin_port == a->sin_port) break; if (!probes[i].used && free_i < 0) free_i = i; }
	if (i < MAXPROBE) probes[i].added = now_ms();
	else if (free_i >= 0) { probes[free_i].used = 1; probes[free_i].keep = keep; probes[free_i].a = *a; probes[free_i].last = 0; probes[free_i].added = now_ms(); }
	pthread_mutex_unlock(&lock);
}
void ov_probe(const char *host, uint16_t p) {
	struct addrinfo hints, *res = 0; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host, 0, &hints, &res) || !res) { fprintf(stderr, "    m2: overlay: %s: no such host\n", host); return; }
	struct sockaddr_in a = *(struct sockaddr_in *)res->ai_addr; a.sin_port = htons(p); freeaddrinfo(res);
	probe_add(&a, 1);
}
void ov_probe_addr(uint32_t ip, uint16_t p) {   /* (network order: the DHT's) */
	struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = ip; a.sin_port = p;
	probe_add(&a, 0);
}
void ov_send_udp(const struct sockaddr_in *to, const uint8_t *b, size_t n) { if (sock >= 0) sendto(sock, (const char *)b, (int)n, 0, (const struct sockaddr *)to, sizeof *to); }
void ov_add_candidate(uint64_t id, uint32_t ip, uint16_t p) {
	if (!id || id == self) return;
	struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = ip; a.sin_port = p;
	pthread_mutex_lock(&lock); Peer *q = peer_by_id(id); if (!q) q = peer_new(id); if (!q->up) cand_add(q, &a); pthread_mutex_unlock(&lock);
}
