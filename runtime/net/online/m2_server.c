/* The local matching service (m2.h, ONLINE.md). The game's np_matching2 reaches it as its matching server: the signed
 * server list (http.c) names 127.0.0.1 and the service's plain port; the module's TLS connection to the same host's
 * port 443 lands on the second listener (net.c maps it). Both listen on ports the system picks, so two copies of a
 * game on one machine never collide. A connection is served by its own thread. */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSESOCK close
#endif
#include <time.h>
#include "m2_internal.h"
#include "overlay.h"
#include "../net.h"

int m2_log(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_M2_LOG"); v = e ? atoi(e) : 0; } return v; }

static int listen_on(int *port, int any) {   /* a TCP listener on 127.0.0.1 (or every interface: the room server), a port the system picks */
	int s = (int)socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return -1;
	struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(any ? INADDR_ANY : INADDR_LOOPBACK);
	socklen_t l = sizeof a;
	if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 8) || getsockname(s, (struct sockaddr *)&a, &l)) { CLOSESOCK(s); return -1; }
	*port = ntohs(a.sin_port);
	return s;
}
static int read_all(int s, uint8_t *p, size_t n) {   /* 0 when all n bytes came, -1 at the end of the connection */
	for (size_t got = 0; got < n; ) { const int r = (int)recv(s, (char *)p + got, (int)(n - got), 0); if (r <= 0) return -1; got += (size_t)r; }
	return 0;
}

/* ---- sessions: made by a login (on the TLS connection), named by their tag 0x14 token in every later request's header
 * (+0x0c) -- the module may reconnect its plain connection after 60 s idle without a new hello, so a session is not a
 * socket. Each holds the request MAC key it was given (tag 0x22). */
#define MAXSESS 32
static Session sess[MAXSESS];
static pthread_mutex_t sess_lock = PTHREAD_MUTEX_INITIALIZER;
void plat_random(void *buf, size_t n);   /* (../platform.h) */
static Session *session_new(void) {
	pthread_mutex_lock(&sess_lock);
	int i; for (i = 0; i < MAXSESS && sess[i].used; i++) {}
	if (i == MAXSESS) i = 0;   /* (full: the oldest slot goes) */
	Session *se = &sess[i]; memset(se, 0, sizeof *se); se->used = 1;
	plat_random(se->token, 4); plat_random(se->key, 16); plat_random(se->b5d, 16);
	pthread_mutex_unlock(&sess_lock);
	return se;
}
static Session *session_of(const uint8_t token[4]) {
	pthread_mutex_lock(&sess_lock);
	Session *se = 0; for (int i = 0; i < MAXSESS && !se; i++) if (sess[i].used && !memcmp(sess[i].token, token, 4)) se = &sess[i];
	pthread_mutex_unlock(&sess_lock);
	return se;
}

int m2_cred_parse(const uint8_t *cred, size_t n, uint8_t key[16], uint8_t npid[24]) {
	if (n != CRED_LEN || memcmp(cred, "NVC1", 4)) return -1;
	memcpy(key, cred + 4, 16); memcpy(npid, cred + 20, 24); return 0;
}
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
int m2_send(int s, const uint8_t *p, size_t n) {
	int ok = 0;
	pthread_mutex_lock(&send_lock);
	for (size_t sent = 0; sent < n; ) { const int r = (int)send(s, (const char *)p + sent, (int)(n - sent), 0); if (r <= 0) { ok = -1; break; } sent += (size_t)r; }
	pthread_mutex_unlock(&send_lock);
	if (ok) shutdown(s, 2);   /* (a peer that stopped reading, or went: its connection's thread ends it -- a member's leaving) */
	return ok;
}
void m2_reply(int s, M2Out *o) {
	const size_t n = m2_end(o);
	if (!n) { fprintf(stderr, "    m2: a message didn't fit\n"); return; }
	if (m2_log()) m2_dump(stderr, "->", o->p, n);
	m2_send(s, o->p, n);
}
const M2Field *m2_get(const uint8_t *msg, size_t n, uint16_t tag, M2Field *f) {
	size_t at = 0; const uint8_t *body = msg + M2_PREFIX + M2_HEAD;
	while (m2_next_field(body, n - M2_PREFIX - M2_HEAD, &at, f) == 1) if (f->tag == tag) return f;
	return 0;
}
uint16_t m2_u16(const M2Field *f) { return f && f->len >= 2 ? (uint16_t)(f->data[0] << 8 | f->data[1]) : 0; }
uint32_t m2_u32(const M2Field *f) { return f && f->len >= 4 ? (uint32_t)f->data[0] << 24 | (uint32_t)f->data[1] << 16 | (uint32_t)f->data[2] << 8 | f->data[3] : 0; }
uint64_t m2_now_ms(void) { return (uint64_t)time(0) * 1000; }

/* 0x1001 hello (plain connection): the server is available (tag 0x1d: a u16 the module drops, then status 1) */
static void on_hello(int s, const uint8_t *msg, size_t n) {
	M2Field f; uint16_t agent = 1; if (m2_get(msg, n, 0x73, &f) && f.len == 2) agent = (uint16_t)(f.data[0] << 8 | f.data[1]);
	if (m2_get(msg, n, 0x10, &f) && f.len <= 12) { char comm[13]; memcpy(comm, f.data, f.len); comm[f.len] = 0; disc_start(comm); }   /* (the game's rooms: found by its id) */
	uint8_t buf[256]; M2Out o = { buf, 0, sizeof buf, 0 };
	m2_begin(&o, 2, 0x1001, 0, 0);
	const uint8_t st[3] = { (uint8_t)(agent >> 8), (uint8_t)agent, 1 };
	m2_put(&o, 0x001d, st, 3);
	m2_reply(s, &o);
}
/* 0x1002 login (TLS connection): a session -- its token (0x14, in every later header), the request MAC key (0x22), the
 * two values the module echoes in lobby and room requests (0x23, 0x5d), the refresh period (0x74: under 10, none) */
static void on_login(int s, const uint8_t *msg, size_t n) {
	Session *se = session_new(); M2Field f;
	if (m2_get(msg, n, 0x0b, &f) && f.len == 24) { memcpy(se->npid, f.data, 24); memcpy(se->online_id, f.data, 16); se->online_id[16] = 0; }
	memcpy(se->cred, "NVC1", 4); memcpy(se->cred + 4, se->key, 16); memcpy(se->cred + 20, se->npid, 24);
	uint8_t buf[512]; M2Out o = { buf, 0, sizeof buf, 0 };
	m2_begin(&o, 2, 0x1002, 0, 0);
	m2_put(&o, 0x5014, se->token, 4); m2_put(&o, 0x2022, se->key, 16); m2_put(&o, 0x2023, se->cred, CRED_LEN); m2_put(&o, 0x205d, se->b5d, 16);
	m2_put16(&o, 0x4074, 0);
	fprintf(stderr, "    m2: %s logged in\n", se->online_id);
	m2_reply(s, &o);
}
/* a request after login: its session by the token, its MAC checked with the session's key */
static Session *request_session(const uint8_t *msg, size_t n, const M2Head *h) {
	Session *se = session_of(h->r);
	if (!se) { fprintf(stderr, "    m2: op %#06x from no session (token %02x%02x%02x%02x)\n", h->op, h->r[0], h->r[1], h->r[2], h->r[3]); return 0; }
	uint8_t mac[16]; m2_mac(se->key, 16, msg, n, mac);
	if (memcmp(mac, h->mac, 16)) fprintf(stderr, "    m2: op %#06x from %s: the MAC doesn't match\n", h->op, se->online_id);
	return se;
}
/* 0x1003 refresh: a new 0x5d value */
static void on_refresh(int s, Session *se, const M2Head *h) {
	uint8_t buf[256]; M2Out o = { buf, 0, sizeof buf, 0 };
	m2_begin(&o, 2, 0x1003, 0, h->id); m2_put(&o, 0x205d, se->b5d, 16); m2_reply(s, &o);
}

enum { AGENT, AGENT_TLS, ROOM };   /* a connection's listener: the agent (this copy's game), its login, the room server */
static void dispatch(int s, int kind, const uint8_t *msg, size_t n) {
	M2Head h; if (m2_parse_head(msg, n, &h)) return;
	if (h.cls != 1) { fprintf(stderr, "    m2: a message of class %u\n", h.cls); return; }
	if (kind == ROOM) { m2_room_op(s, msg, n, &h); return; }   /* (a room server answers anyone in its rooms) */
	if (h.op == 0x1001) { on_hello(s, msg, n); return; }
	if (h.op == 0x1002) { if (kind == AGENT_TLS) on_login(s, msg, n); return; }
	Session *se = request_session(msg, n, &h); if (!se) return;
	if (h.op == 0x1003) { on_refresh(s, se, &h); return; }
	m2_agent_room_op(s, msg, n, &h, se);
}

typedef struct { int s, kind; } Conn;
static void *conn_main(void *arg) {
	Conn cn = *(Conn *)arg; free(arg);
	const char *kind = cn.kind == AGENT_TLS ? "tls" : cn.kind == ROOM ? "room" : "plain";
	if (m2_log()) { struct sockaddr_in pa; socklen_t pl = sizeof pa; char ip[32] = "?"; unsigned pp = 0; if (!getpeername(cn.s, (struct sockaddr *)&pa, &pl)) { inet_ntop(AF_INET, &pa.sin_addr, ip, sizeof ip); pp = ntohs(pa.sin_port); } struct timespec tw; clock_gettime(CLOCK_REALTIME, &tw); fprintf(stderr, "    m2: %s connection from %s:%u (fd %d) at %ld.%06ld\n", kind, ip, pp, cn.s, (long)tw.tv_sec, tw.tv_nsec / 1000); }
	uint8_t buf[0x10000];
	for (;;) {
		if (read_all(cn.s, buf, 1)) break;
		if (buf[0] == 0) {   /* the module's keepalive, every 8 s: answered, as it wants a byte from the server at least every 20 s */
			if (m2_log()) fprintf(stderr, "    m2: %s keepalive\n", kind);
			const uint8_t z = 0; m2_send(cn.s, &z, 1); continue;
		}
		if (read_all(cn.s, buf + 1, 3)) break;
		const size_t len = (size_t)(buf[2] << 8 | buf[3]);
		if (len < 4 || read_all(cn.s, buf + 4, len - 4)) break;
		if (buf[0] == 'N' && cn.kind == ROOM) { if (m2_room_link(cn.s, buf, len)) break; continue; }   /* (a proxy's own frame: m2_rooms.c) */
		if (m2_log()) m2_dump(stderr, cn.kind == AGENT_TLS ? "<- tls" : cn.kind == ROOM ? "<- room" : "<-", buf, len);
		dispatch(cn.s, cn.kind, buf, len);
	}
	if (m2_log()) fprintf(stderr, "    m2: %s connection closed (fd %d)\n", kind, cn.s);
	if (cn.kind == ROOM) m2_room_conn_closed(cn.s);
	CLOSESOCK(cn.s);
	return 0;
}
static int lsock[3], lport[3];
int m2_room_port(void) { return lport[ROOM]; }
/* who may connect to the room server while it listens on every interface (the overlay off): this machine and the private
 * networks a LAN uses (RFC 1918, link-local, carrier-grade NAT space, which VPNs like Tailscale use); never the internet */
static int local_peer(const struct sockaddr_in *a) {
	const uint32_t ip = ntohl(a->sin_addr.s_addr);
	return (ip >> 24) == 127 || (ip >> 24) == 10 || (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8 || (ip >> 16) == 0xa9fe || (ip >> 22) == 0x191;
}
static void *accept_main(void *arg) {
	(void)arg;
	for (;;) {
#ifdef _WIN32
		WSAPOLLFD pf[3] = { { (SOCKET)lsock[0], POLLRDNORM, 0 }, { (SOCKET)lsock[1], POLLRDNORM, 0 }, { (SOCKET)lsock[2], POLLRDNORM, 0 } };
		if (WSAPoll(pf, 3, -1) < 0) continue;
#else
		struct pollfd pf[3] = { { lsock[0], POLLIN, 0 }, { lsock[1], POLLIN, 0 }, { lsock[2], POLLIN, 0 } };
		if (poll(pf, 3, -1) < 0) continue;
#endif
		for (int i = 0; i < 3; i++) {
			if (!pf[i].revents) continue;
			struct sockaddr_in from; socklen_t fl = sizeof from; memset(&from, 0, sizeof from);
			const int s = (int)accept(lsock[i], (struct sockaddr *)&from, &fl); if (s < 0) continue;
			if (i == ROOM && !local_peer(&from)) { CLOSESOCK(s); continue; }   /* (only reachable from outside with the overlay off) */
			int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
			/* a send waits at most 3 s: one member whose network went quiet mustn't hold the room server (every send takes one lock) */
#ifdef _WIN32
			DWORD tmo = 3000; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof tmo);
#else
			struct timeval tmo = { 3, 0 }; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof tmo);
#endif
			Conn *c = malloc(sizeof *c); c->s = s; c->kind = i;
			pthread_t t; if (pthread_create(&t, 0, conn_main, c)) { CLOSESOCK(s); free(c); } else pthread_detach(t);
		}
	}
	return 0;
}

void m2_online_start(const char *comm_id) { if (m2_service_port()) disc_start(comm_id); }
int m2_service_port(void) {
	static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER; static int started;
	pthread_mutex_lock(&m);
	if (!started) {
		started = -1;
#ifdef _WIN32
		WSADATA d; WSAStartup(MAKEWORD(2, 2), &d);
#endif
		/* the room server: on every interface for the LAN with the overlay off (other machines' games connect to it; accept
		 * takes only private-network peers, local_peer), on loopback alone with the overlay on (other copies' streams arrive
		 * through it, connected from 127.0.0.1): nothing on the internet reaches it unasked */
		lsock[AGENT] = listen_on(&lport[AGENT], 0); lsock[AGENT_TLS] = listen_on(&lport[AGENT_TLS], 0); lsock[ROOM] = listen_on(&lport[ROOM], !ov_on());
		pthread_t t;
		if (lsock[0] >= 0 && lsock[1] >= 0 && lsock[2] >= 0 && !pthread_create(&t, 0, accept_main, 0)) {
			pthread_detach(t); started = 1; m2_rooms_start();
			net_portmap_add("127.0.0.1", 443, (unsigned)lport[AGENT_TLS]);   /* the module's TLS connection to the server's port 443 */
			fprintf(stderr, "    m2: the matching service on 127.0.0.1:%d (TLS port as 443: %d), room server on port %d\n", lport[AGENT], lport[AGENT_TLS], lport[ROOM]);
		} else fprintf(stderr, "    m2: the matching service couldn't start: %s\n", strerror(errno));
	}
	const int p = started > 0 ? lport[0] : 0;
	pthread_mutex_unlock(&m);
	return p;
}
