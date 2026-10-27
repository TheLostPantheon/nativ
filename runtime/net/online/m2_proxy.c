/* The proxy a member's game reaches another copy's room through (m2_rooms.c, "a room outliving its owner's copy"): a
 * listener on 127.0.0.1 for each room this copy's game joins (locate hands the game its port), and for each connection the
 * game makes there a relay to the copy that serves the room. The game's stream passes through whole frames at a time;
 * what the room server sends the proxy itself ("NS" snapshots, "NA") is kept out of it. When the serving copy goes -- the
 * connection ends, or no snapshot for 5 s -- the relay finds the copy that serves the room next (the same choice in every
 * copy: m2_room_successor), and when that is this one makes the room its own (m2_room_promote), then reconnects and
 * resumes as its member ("NR"): the game's own connection stays open all along. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define CLOSESOCK closesocket
#define poll WSAPoll
typedef WSAPOLLFD PollFd;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSESOCK close
typedef struct pollfd PollFd;
#endif
#include "m2_internal.h"
#include "overlay.h"

enum { MAXPROXY = 8, BUF = 0x14000, HEARTBEAT_MS = 5000, FAILOVER_MS = 20000, PATIENCE_MS = 3000 };
typedef struct { int used, lsock, lport; uint16_t world, room; char host[64]; int port; } Proxy;
static Proxy px[MAXPROXY];
static pthread_mutex_t px_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
void plat_sleep_ns(uint64_t ns);   /* (../platform.h) */
static void nap_ms(int ms) { plat_sleep_ns((uint64_t)ms * 1000000); }
static int send_all(int s, const uint8_t *p, size_t n) {
	for (size_t sent = 0; sent < n; ) { const int r = (int)send(s, (const char *)p + sent, (int)(n - sent), 0); if (r <= 0) return -1; sent += (size_t)r; }
	return 0;
}
static int readable(int s, int ms) { PollFd pf = { s, POLLIN, 0 }; return poll(&pf, 1, ms) > 0; }
static int connect_to(const char *host, int port) {   /* a room server: over TCP, or a stream over the overlay to a copy's virtual address */
	struct in_addr ia; if (inet_pton(AF_INET, host, &ia) != 1) return -1;
	if (ov_is_vip(ia.s_addr)) return ov_stream_open(ia.s_addr, (uint16_t)port);
	int s = (int)socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return -1;
	struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr = ia; a.sin_port = htons((uint16_t)port);
	if (connect(s, (struct sockaddr *)&a, sizeof a)) { CLOSESOCK(s); return -1; }
	int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
	return s;
}
/* a whole frame at b (the module's keepalive byte 0, or {type, u16 BE total length}): its length; 0 not all here yet; -1 garbage */
static long frame_len(const uint8_t *b, size_t n) {
	if (!n) return 0;
	if (b[0] == 0) return 1;
	if (n < 4) return 0;
	const size_t len = (size_t)(b[2] << 8 | b[3]);
	if (len < 4) return -1;
	return n >= len ? (long)len : 0;
}

/* the copy that serves the room next, its room server reached and resumed at: the connection (host, port set), or -1 */
static int take_over(uint16_t world, uint16_t room, char host[64], int *port) {
	const uint64_t t0 = now_ms();
	while (now_ms() - t0 < FAILOVER_MS) {
		for (int nth = 0; ; nth++) {
			char h[64]; int p = 0; const int w = m2_room_successor(world, room, nth, h, &p);
			if (w < 0) break;
			if (w == 2) continue;   /* (that member's copy isn't heard of: the next) */
			if (w == 1) {   /* this copy: it serves the room now -- at once when it is the first choice, else after the earlier ones had their chance */
				if (nth && now_ms() - t0 < PATIENCE_MS) break;
				if (m2_room_promote(world, room)) continue;
				snprintf(h, sizeof h, "127.0.0.1"); p = m2_room_port();
			}
			const int s = connect_to(h, p); if (s < 0) continue;
			uint8_t f[64]; const size_t fn = m2_room_resume_frame(world, room, f);
			uint8_t a[4]; int got = 0;
			if (fn && !send_all(s, f, fn) && readable(s, 1500)) for (int k = 0; k < 4; ) { const int r = (int)recv(s, (char *)a + k, 4 - k, 0); if (r <= 0) break; k += r; got = k; }
			if (got == 4 && a[0] == 'N' && a[1] == 'A') {
				snprintf(host, 64, "%s", h); *port = p;
				fprintf(stderr, "    m2: room %u/%u resumed at %s:%d after %llu ms\n", world, room, h, p, (unsigned long long)(now_ms() - t0));
				return s;
			}
			CLOSESOCK(s);   /* (not serving it yet: the next, then all of them again) */
		}
		nap_ms(400);
	}
	fprintf(stderr, "    m2: room %u/%u: no copy took it over in %d s\n", world, room, FAILOVER_MS / 1000);
	return -1;
}

typedef struct { int pi, down; } Relay;
static void *relay_main(void *arg) {
	Relay rl = *(Relay *)arg; free(arg);
	pthread_mutex_lock(&px_lock); const uint16_t world = px[rl.pi].world, room = px[rl.pi].room; char host[64]; snprintf(host, sizeof host, "%s", px[rl.pi].host); int port = px[rl.pi].port; pthread_mutex_unlock(&px_lock);
	uint8_t *ub = malloc(BUF), *db = malloc(BUF); size_t un = 0, dn = 0;
	int up = ub && db ? connect_to(host, port) : -1;
	const uint8_t hello[8] = { 'N', 'P', 0, 8, (uint8_t)(world >> 8), (uint8_t)world, (uint8_t)(room >> 8), (uint8_t)room };
	if (up >= 0 && send_all(up, hello, sizeof hello)) { CLOSESOCK(up); up = -1; }
	if (m2_log()) fprintf(stderr, "    m2: proxy for room %u/%u: %s:%d%s\n", world, room, host, port, up < 0 ? " unreachable" : "");
	uint64_t last = now_ms(); int heard = 0;   /* (heard: the room server sends this proxy snapshots: it has the room's heartbeat) */
	while (up >= 0) {
		PollFd pf[2] = { { rl.down, POLLIN, 0 }, { up, POLLIN, 0 } };
		poll(pf, 2, 500);
		const char *why = 0;
		if (pf[0].revents) {   /* the game's stream, to the room server whole frames at a time */
			const long k = recv(rl.down, (char *)db + dn, (int)(BUF - dn), 0); if (k <= 0) break;   /* (the game closed it: done) */
			dn += (size_t)k; long fl;
			while ((fl = frame_len(db, dn)) > 0) {
				const uint64_t t0 = now_ms();
				if (send_all(up, db, (size_t)fl)) { why = "the connection broke"; break; }
				if (m2_log() && now_ms() - t0 > 500) fprintf(stderr, "    m2: proxy for room %u/%u: a %ld-byte frame to the serving copy took %llu ms to send\n", world, room, fl, (unsigned long long)(now_ms() - t0));
				memmove(db, db + fl, dn - (size_t)fl); dn -= (size_t)fl;
			}
			if (fl < 0) break;
		}
		if (!why && pf[1].revents) {   /* the room server's: the game's frames on, ours kept */
			const long k = recv(up, (char *)ub + un, (int)(BUF - un), 0);
			if (k <= 0) why = "the connection ended";
			else {
				un += (size_t)k; long fl;
				while ((fl = frame_len(ub, un)) > 0) {
					if (ub[0] == 'N') { if (ub[1] == 'S') { m2_replica_update(ub, (size_t)fl); last = now_ms(); heard = 1; } }
					else if (send_all(rl.down, ub, (size_t)fl)) goto done;
					memmove(ub, ub + fl, un - (size_t)fl); un -= (size_t)fl;
				}
				if (fl < 0) why = "it sent garbage";
			}
		}
		if (!why && heard && now_ms() - last > HEARTBEAT_MS) why = "no word from it for 5 s";
		if (why) {
			fprintf(stderr, "    m2: room %u/%u: its serving copy at %s:%d went (%s): the next takes over\n", world, room, host, port, why);
			if (m2_log()) { fprintf(stderr, "    m2: (the last snapshot %llu ms ago, %zu bytes from it unread, %zu from the game unsent)\n", (unsigned long long)(now_ms() - last), un, dn); ov_streams_dump(); }
			CLOSESOCK(up); un = 0;
			up = take_over(world, room, host, &port);
			last = now_ms();
		}
	}
done:
	if (up >= 0) CLOSESOCK(up);
	CLOSESOCK(rl.down); free(ub); free(db);
	return 0;
}
static void *accept_main(void *arg) {
	const int pi = (int)(intptr_t)arg;
	for (;;) {
		const int s = (int)accept(px[pi].lsock, 0, 0); if (s < 0) continue;
		int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
		Relay *r = malloc(sizeof *r); if (!r) { CLOSESOCK(s); continue; }
		r->pi = pi; r->down = s;
		pthread_t t; if (pthread_create(&t, 0, relay_main, r)) { CLOSESOCK(s); free(r); } else pthread_detach(t);
	}
	return 0;
}
int m2_proxy_port(uint16_t world, uint16_t room, const char *host, int port) {
	pthread_mutex_lock(&px_lock);
	int i; for (i = 0; i < MAXPROXY && !(px[i].used && px[i].world == world && px[i].room == room); i++) {}
	if (i == MAXPROXY) {
		for (i = 0; i < MAXPROXY && px[i].used; i++) {}
		if (i == MAXPROXY) i = 0;   /* (full: the oldest room's goes; its listener stays, unused) */
		const int s = (int)socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); socklen_t l = sizeof a;
		if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 4) || getsockname(s, (struct sockaddr *)&a, &l)) { if (s >= 0) CLOSESOCK(s); pthread_mutex_unlock(&px_lock); return 0; }
		memset(&px[i], 0, sizeof px[i]); px[i].used = 1; px[i].lsock = s; px[i].lport = ntohs(a.sin_port); px[i].world = world; px[i].room = room;
		pthread_t t; if (pthread_create(&t, 0, accept_main, (void *)(intptr_t)i)) { CLOSESOCK(s); px[i].used = 0; pthread_mutex_unlock(&px_lock); return 0; }
		pthread_detach(t);
	}
	snprintf(px[i].host, sizeof px[i].host, "%s", host); px[i].port = port;   /* (where the room is served now, as discovery says) */
	const int p = px[i].lport;
	pthread_mutex_unlock(&px_lock);
	return p;
}
