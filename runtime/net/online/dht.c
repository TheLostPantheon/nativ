/* The BitTorrent DHT (BEP 5) as Nativ copies' meeting place on the internet (ONLINE.md, milestone 3): every copy of a
 * game announces itself under one key, SHA-1("Project Nativ online 1:" + the game's communication id), and looks up
 * who else did. It speaks from the overlay's UDP socket (overlay.c hands it every packet that starts with 'd', a
 * bencoded dict, and sends for it), so the port the DHT records with implied_port is that socket's mapping in the NAT
 * in front of it: the endpoint other copies say hello to (both sides do, which opens both NATs: hole punching).
 * Read-only (BEP 43: "ro" in every query): it asks, never answers, and no node keeps it in its routing table.
 * A lookup: get_peers to the nodes closest to the key it knows (bootstrap routers first), three at a time, until the
 * eight closest that answered have all been asked; the peers they return are probed (overlay.c), then announce_peer
 * goes to those eight with their tokens. Again after a minute, then every three. settings online_dht=0: off. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include "m2_internal.h"
#include "overlay.h"
#include "../sha1.h"

void plat_random(void *buf, size_t n);
int settings_int(const char *key, int def);
void ov_send_udp(const struct sockaddr_in *to, const uint8_t *b, size_t n);   /* (overlay.c) */
void ov_probe_addr(uint32_t ip, uint16_t port);

enum { MAXNODE = 128, K = 8, ALPHA = 6, MAXFLIGHT = 16 };   /* (6 at once: a lookup over a phone's latency in seconds, not 20) */
typedef struct { uint8_t id[20]; int has_id; struct sockaddr_in a; int queried, answered; uint64_t sent; uint8_t token[32]; int tlen; uint16_t tid; } Node;
static Node nodes[MAXNODE]; static int nnodes;
static char pub[32];   /* this copy's address on the internet, as the last node to say so saw it ("": none yet) */
static uint8_t self_id[20], key[20];
static int running, lookup_on; static uint64_t lookup_at, next_lookup; static int lookups; static uint16_t next_tid;
static uint16_t ov_port_n; static int explicit_port;   /* (network order) the announced port: implied (the NAT's mapping, as the node saw it) unless UPnP mapped one */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static const char *const routers[] = { "router.bittorrent.com:6881", "dht.transmissionbt.com:6881", "router.utorrent.com:6881", "dht.libtorrent.org:25401" };

static uint64_t now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000; }
static int closer(const uint8_t *a, const uint8_t *b) {   /* a nearer the key than b (XOR metric) */
	for (int i = 0; i < 20; i++) { const uint8_t x = a[i] ^ key[i], y = b[i] ^ key[i]; if (x != y) return x < y; }
	return 0;
}

/* ---- bencode: just enough */
static size_t bstr(uint8_t *p, const void *s, size_t n) { const int k = sprintf((char *)p, "%zu:", n); memcpy(p + k, s, n); return (size_t)k + n; }
static size_t bint(uint8_t *p, long v) { return (size_t)sprintf((char *)p, "i%lde", v); }
/* one value at b[*at] skipped: 0, -1 when malformed. *s, *sn: a string's bounds, or 0 when the value is anything else
 * (cleared first: a caller never reads a pointer left from another value -- the fuzzer's first find, dev/fuzz: a
 * non-string dict key, or a number where a string belongs, had bget and bgetstr compare memory from an earlier packet) */
static int bskip(const uint8_t *b, size_t n, size_t *at, const uint8_t **s, size_t *sn) {
	if (s) { *s = 0; *sn = 0; }
	if (*at >= n) return -1;
	const uint8_t c = b[*at];
	if (c >= '0' && c <= '9') {
		size_t l = 0; while (*at < n && b[*at] >= '0' && b[*at] <= '9') l = l * 10 + (b[(*at)++] - '0');
		if (*at >= n || b[*at] != ':' || *at + 1 + l > n) return -1;
		if (s) { *s = b + *at + 1; *sn = l; }
		*at += 1 + l; return 0;
	}
	if (c == 'i') { while (*at < n && b[*at] != 'e') (*at)++; if (*at >= n) return -1; (*at)++; return 0; }
	if (c == 'l' || c == 'd') { (*at)++; while (*at < n && b[*at] != 'e') if (bskip(b, n, at, 0, 0)) return -1; if (*at >= n) return -1; (*at)++; return 0; }
	return -1;
}
/* in the dict at b[at] (at its 'd'), the value under key k: its offset, or 0 */
static size_t bget(const uint8_t *b, size_t n, size_t at, const char *k) {
	if (at >= n || b[at] != 'd') return 0;
	at++;
	while (at < n && b[at] != 'e') {
		const uint8_t *s; size_t sn; if (bskip(b, n, &at, &s, &sn) || !s) return 0;   /* (a key is a string, always) */
		if (sn == strlen(k) && !memcmp(s, k, sn)) return at;
		if (bskip(b, n, &at, 0, 0)) return 0;
	}
	return 0;
}
static int bgetstr(const uint8_t *b, size_t n, size_t at, const uint8_t **s, size_t *sn) { return at && !bskip(b, n, &at, s, sn) && *s ? 0 : -1; }   /* (a string, nothing else) */

/* ---- nodes */
static Node *node_add(const uint8_t *id, const struct sockaddr_in *a) {   /* (lock held) */
	for (int i = 0; i < nnodes; i++) if (nodes[i].a.sin_addr.s_addr == a->sin_addr.s_addr && nodes[i].a.sin_port == a->sin_port) { if (id && !nodes[i].has_id) { memcpy(nodes[i].id, id, 20); nodes[i].has_id = 1; } return &nodes[i]; }
	int at = nnodes < MAXNODE ? nnodes++ : -1;
	if (at < 0) {   /* (full: the farthest unasked goes) */
		for (int i = 0; i < MAXNODE; i++) if (!nodes[i].queried && nodes[i].has_id && id && (at < 0 || closer(nodes[at].id, nodes[i].id))) at = i;
		if (at < 0 || !closer(id, nodes[at].id)) return 0;
	}
	Node *x = &nodes[at]; memset(x, 0, sizeof *x); x->a = *a; if (id) { memcpy(x->id, id, 20); x->has_id = 1; }
	return x;
}
static void query(Node *x, int announce) {   /* (lock held) */
	uint8_t b[256], *p = b; x->tid = ++next_tid; if (!x->tid) x->tid = ++next_tid;
	const uint8_t t[2] = { (uint8_t)(x->tid >> 8), (uint8_t)x->tid };
	*p++ = 'd'; p += bstr(p, "a", 1); *p++ = 'd'; p += bstr(p, "id", 2); p += bstr(p, self_id, 20);
	if (announce && !explicit_port) { p += bstr(p, "implied_port", 12); p += bint(p, 1); }
	p += bstr(p, "info_hash", 9); p += bstr(p, key, 20);
	if (announce) { p += bstr(p, "port", 4); p += bint(p, ntohs(ov_port_n)); p += bstr(p, "token", 5); p += bstr(p, x->token, (size_t)x->tlen); }
	*p++ = 'e';
	p += bstr(p, "q", 1); p += announce ? bstr(p, "announce_peer", 13) : bstr(p, "get_peers", 9);
	p += bstr(p, "ro", 2); p += bint(p, 1); p += bstr(p, "t", 1); p += bstr(p, t, 2); p += bstr(p, "y", 1); p += bstr(p, "q", 1); *p++ = 'e';
	ov_send_udp(&x->a, b, (size_t)(p - b));
	if (!announce) { x->queried = 1; x->sent = now_ms(); }
}
static void lookup_start(void) {   /* (lock held) */
	for (int i = 0; i < nnodes; i++) { nodes[i].queried = nodes[i].answered = 0; nodes[i].tlen = 0; }
	for (size_t r = 0; r < sizeof routers / sizeof *routers; r++) {
		char h[64]; snprintf(h, sizeof h, "%s", routers[r]); char *c = strrchr(h, ':'); if (!c) continue; *c = 0;
		struct addrinfo hints, *res = 0; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
		if (getaddrinfo(h, 0, &hints, &res) || !res) continue;
		struct sockaddr_in a = *(struct sockaddr_in *)res->ai_addr; a.sin_port = htons((uint16_t)atoi(c + 1)); freeaddrinfo(res);
		node_add(0, &a);
	}
	lookup_on = 1; lookup_at = now_ms(); lookups++;
}
static void lookup_step(void) {   /* (lock held) more queries, or the end: the announces */
	const uint64_t now = now_ms(); int flight = 0;
	for (int i = 0; i < nnodes; i++) if (nodes[i].queried && !nodes[i].answered && now - nodes[i].sent < 2000) flight++;
	int best[K], nb = 0;   /* the K closest that answered, nearest first */
	for (int i = 0; i < nnodes; i++) {
		if (!nodes[i].answered || !nodes[i].has_id) continue;
		int j = nb;
		while (j > 0 && closer(nodes[i].id, nodes[best[j - 1]].id)) { if (j < K) best[j] = best[j - 1]; j--; }
		if (j < K) { best[j] = i; if (nb < K) nb++; }
	}
	for (int sent = 0; flight < MAXFLIGHT && sent < ALPHA; sent++, flight++) {
		int pick = -1;
		for (int i = 0; i < nnodes; i++) {
			if (nodes[i].queried) continue;
			if (!nodes[i].has_id) { pick = i; break; }   /* (bootstrap routers first) */
			if (nb == K && !closer(nodes[i].id, nodes[best[K - 1]].id)) continue;   /* (no nearer than what answered) */
			if (pick < 0 || closer(nodes[i].id, nodes[pick].id)) pick = i;
		}
		if (pick < 0) break;
		query(&nodes[pick], 0);
	}
	if (!flight || now - lookup_at > 20000) {   /* the end: announce to the closest with tokens */
		int ann = 0; for (int i = 0; i < nb; i++) if (nodes[best[i]].tlen) { query(&nodes[best[i]], 1); ann++; }
		if (m2_log() || lookups == 1) fprintf(stderr, "    m2: dht: lookup %d done, %d nodes asked, announced to %d\n", lookups, nnodes, ann);
		/* (a thin one: again soon; then four 20 s apart, to meet the copies that start just after this one -- the first internet
		 * test: one copy looked once, before the other had announced, and the other gave up searching before the next, 60 s on --
		 * then every minute a while, then every 3; a lookup is ~100 small queries, 50-100 KB) */
		lookup_on = 0; next_lookup = now + (nb < K ? 10000 : lookups < 5 ? 20000 : lookups < 10 ? 60000 : 180000);
	}
}

void dht_input(const uint8_t *b, size_t n, const struct sockaddr_in *from) {
	const uint8_t *s; size_t sn;
	if (bgetstr(b, n, bget(b, n, 0, "y"), &s, &sn) || sn != 1 || s[0] != 'r') return;   /* (answers only: read-only) */
	if (bgetstr(b, n, bget(b, n, 0, "t"), &s, &sn) || sn != 2) return;
	const uint16_t tid = (uint16_t)(s[0] << 8 | s[1]);
	const size_t r = bget(b, n, 0, "r"); if (!r) return;
	pthread_mutex_lock(&lock);
	Node *x = 0;   /* (by transaction id: a bootstrap router may answer from another of its addresses) */
	for (int i = 0; i < nnodes && !x; i++) if (nodes[i].tid == tid && nodes[i].queried && !nodes[i].answered) x = &nodes[i];
	if (!x) { pthread_mutex_unlock(&lock); return; }
	(void)from;
	x->answered = 1;
	if (!bgetstr(b, n, bget(b, n, r, "id"), &s, &sn) && sn == 20) { memcpy(x->id, s, 20); x->has_id = 1; }
	if (!bgetstr(b, n, bget(b, n, r, "token"), &s, &sn) && sn <= sizeof x->token) { memcpy(x->token, s, sn); x->tlen = (int)sn; }
	if (!bgetstr(b, n, bget(b, n, 0, "ip"), &s, &sn) && sn == 6) {   /* (a node says where it sees this copy: BEP 42) */
		char ip[32]; inet_ntop(AF_INET, s, ip, sizeof ip); snprintf(pub, sizeof pub, "%s:%u", ip, s[4] << 8 | s[5]);   /* (the lock is held) */
		static int said; if (!said) { said = 1; fprintf(stderr, "    m2: dht: this copy is %s:%u on the internet\n", ip, s[4] << 8 | s[5]); }
	}
	if (!bgetstr(b, n, bget(b, n, r, "nodes"), &s, &sn))
		for (size_t k = 0; k + 26 <= sn; k += 26) {
			struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; memcpy(&a.sin_addr, s + k + 20, 4); memcpy(&a.sin_port, s + k + 24, 2);
			if (a.sin_port && a.sin_addr.s_addr) node_add(s + k, &a);
		}
	size_t v = bget(b, n, r, "values");
	pthread_mutex_unlock(&lock);
	if (v && b[v] == 'l')   /* the copies there: compact peers (u32 address, u16 port) */
		for (v++; v < n && b[v] != 'e'; ) {
			if (bskip(b, n, &v, &s, &sn)) break;
			if (sn == 6) { uint32_t ip; uint16_t p; memcpy(&ip, s, 4); memcpy(&p, s + 4, 2); if (m2_log()) { char t[32]; inet_ntop(AF_INET, &ip, t, sizeof t); fprintf(stderr, "    m2: dht: a copy at %s:%u\n", t, ntohs(p)); } ov_probe_addr(ip, p); }
		}
}
void dht_tick(void) {
	pthread_mutex_lock(&lock);
	if (running) { if (lookup_on) lookup_step(); else if (now_ms() >= next_lookup) lookup_start(); }
	pthread_mutex_unlock(&lock);
}
int dht_counts(char where[32]) {   /* (status.c: never waits -- a busy lock gives the last counts) */
	static int n_was; static char pub_was[32];
	if (!pthread_mutex_trylock(&lock)) { n_was = nnodes; snprintf(pub_was, sizeof pub_was, "%s", pub); pthread_mutex_unlock(&lock); }
	snprintf(where, 32, "%s", pub_was); return n_was;
}
void dht_set_port(uint16_t port) { pthread_mutex_lock(&lock); ov_port_n = htons(port); explicit_port = 1; pthread_mutex_unlock(&lock); }   /* (upnp.c) */
void dht_start(const char comm[12], uint16_t port) {
	if (running || settings_int("online_dht", 1) <= 0 || getenv("RECOMP_ONLINE_NO_DHT")) return;
	char k[64]; const int kn = snprintf(k, sizeof k, "Project Nativ online 1:%.12s", comm); sha1(k, (size_t)kn, key);
	plat_random(self_id, 20); ov_port_n = htons(port);
	pthread_mutex_lock(&lock); running = 1; next_lookup = 0; pthread_mutex_unlock(&lock);
	char hex[41]; for (int i = 0; i < 20; i++) sprintf(hex + 2 * i, "%02x", key[i]);
	fprintf(stderr, "    m2: dht: meeting other copies under %s\n", hex);
}
