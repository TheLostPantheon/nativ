/* Discovery (ONLINE.md): the rooms of other copies of the same game. Every copy multicasts an advert of the rooms it
 * hosts once a second (and at once when one changes) to a group on the local network, loopback included, so two copies
 * on one machine hear each other too; what it hears it keeps for 5 s. An advert is our own format, nothing of Sony's:
 *   "NVM2", u8 version 4, u8 room count, u64 the copy's id, char comm id[12], u16 its room server's port,
 *   u16 the port of its in-game messages (basic_msg.c), its player's online id[16], u16 its overlay port (0: none),
 *   u64 its player's account ID (np.c: which of two players with one name keeps it),
 *   per room: u16 world, lobby, room; u32 flags; u16 max; 16-byte slot map; u64 password mask; u8 which ints;
 *            u32 ints[8] (attributes 0x4c-0x53); the owner's NpId (24); u8 + 16 bytes: its addresses (tag 0x6c);
 *            u8 its generation (takeovers so far: m2_rooms.c)
 * all big-endian; the room server's address is where the advert came from. A copy that starts sends a query ("NVMQ", u8 1,
 * the same id and comm id) that makes the others advertise at once, and its first searches wait a moment for them: the
 * game searches right after it logs in, sooner than the next round of adverts. A copy with no rooms advertises too:
 * its player is reachable by in-game messages.
 * Known peers (settings "online_peers" = <host>[,<host>...], RECOMP_ONLINE_PEERS wins): where multicast doesn't
 * carry (Wi-Fi that filters it, a firewall that drops what it didn't ask for), each copy also sends its query and its
 * advert to those hosts every second, from the discovery port itself, and answers any query straight back to where it
 * came from: a stateful firewall lets the answers in as replies, so a copy behind one still finds the rooms of a peer it
 * names (and joins them: everything after the search starts from the joiner's side too).
 * With the overlay on (overlay.h), what multicast and known peers bring is only where a copy's overlay may be reached:
 * queries and adverts travel over the overlay links, and a room's server and a player are at the copy's virtual
 * address. (Milestone 3 adds the internet: the DHT.) */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSESOCK close
#endif
#include "m2_internal.h"
#include "overlay.h"

void plat_random(void *buf, size_t n);
void plat_sleep_ns(uint64_t ns);
void np_online_id(char out[17]);
uint64_t np_account_id(void);
static void name_clash(const uint8_t their[16], uint64_t their_account);   /* (below) */
void overlay_toast_titled(const char *title, const char *body, double seconds);
void settings_set_own(const char *key, const char *v);
void settings_save(void);
const char *settings_str(const char *key, const char *def, char *buf, int bufsize);   /* (../settings.h) */

#define GROUP "239.255.86.80"
#define PORT 36580
enum { ROOM_BYTES = 6 + 4 + 2 + 16 + 8 + 1 + 32 + 24 + 1 + 16 + 1, HEAD_BYTES = 4 + 1 + 1 + 8 + 12 + 2 + 2 + 16 + 2 + 8, MAXREMOTE = 64, TTL_MS = 5000 };
static uint64_t self; static char comm[12], player[17]; static int rx = -1, tx = -1; static uint16_t msg_port, ov_port;
static int no_lan;   /* RECOMP_ONLINE_NO_LAN=1 (a test): no multicast, as copies on different networks (they meet by the DHT) */
static struct { RemoteRoom r; uint64_t from, seen; } remote[MAXREMOTE];
static struct { uint64_t from, seen; char player[16], host[64]; uint16_t port, room_port; int rooms; } peers[MAXREMOTE];   /* every copy heard of */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t poke_cv = PTHREAD_COND_INITIALIZER; static int poked;

static uint8_t *w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return p + 2; }
static uint8_t *w32(uint8_t *p, uint32_t v) { p = w16(p, (uint16_t)(v >> 16)); return w16(p, (uint16_t)v); }
static uint8_t *w64(uint8_t *p, uint64_t v) { p = w32(p, (uint32_t)(v >> 32)); return w32(p, (uint32_t)v); }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)r16(p) << 16 | r16(p + 2); }
static uint64_t r64(const uint8_t *p) { return (uint64_t)r32(p) << 32 | r32(p + 4); }

enum { MAXPEERS = 8 };
static struct sockaddr_in known[MAXPEERS]; static int nknown;   /* online_peers, resolved */
static void unicast(const uint8_t *b, size_t n, const struct sockaddr_in *to) { sendto(rx, (const char *)b, (int)n, 0, (const struct sockaddr *)to, sizeof *to); }   /* (from the discovery port: replies come back to it) */
static size_t advert(uint8_t *b) {
	RoomAd ads[8]; const int n = m2_rooms_snapshot(ads, 8);
	uint8_t *p = b;
	memcpy(p, "NVM2", 4); p += 4; *p++ = 6; *p++ = (uint8_t)n; p = w64(p, self); memcpy(p, comm, 12); p += 12; p = w16(p, (uint16_t)m2_room_port());
	p = w16(p, msg_port); memset(p, 0, 16); memcpy(p, player, strnlen(player, 16)); p += 16; p = w16(p, ov_port); p = w64(p, np_account_id());
	for (int i = 0; i < n; i++) {
		const RoomAd *a = &ads[i];
		p = w16(p, a->world); p = w16(p, a->lobby); p = w16(p, a->room); p = w32(p, a->flags); p = w16(p, a->max);
		memcpy(p, a->slots, 16); p += 16; p = w64(p, a->pwmask); *p++ = a->have_ints;
		for (int k = 0; k < 8; k++) p = w32(p, a->ints[k]);
		memcpy(p, a->owner, 24); p += 24;
		*p++ = a->addr_len; memcpy(p, a->addr, 16); p += 16; *p++ = a->gen;
	}
	return (size_t)(p - b);
}
static void advertise(void) {
	uint8_t b[HEAD_BYTES + 8 * ROOM_BYTES]; const size_t n = advert(b);
	struct sockaddr_in g; memset(&g, 0, sizeof g); g.sin_family = AF_INET; g.sin_port = htons(PORT); inet_pton(AF_INET, GROUP, &g.sin_addr);
	if (!no_lan) sendto(tx, (const char *)b, (int)n, 0, (struct sockaddr *)&g, sizeof g);
	for (int i = 0; i < nknown; i++) unicast(b, n, &known[i]);
	if (ov_port) ov_broadcast_disc(b, n);
}
static uint64_t started_ms;
static uint64_t now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000; }
static void query(int multicast) {
	uint8_t b[4 + 1 + 8 + 12], *p = b; memcpy(p, "NVMQ", 4); p += 4; *p++ = 1; p = w64(p, self); memcpy(p, comm, 12); p += 12;
	struct sockaddr_in g; memset(&g, 0, sizeof g); g.sin_family = AF_INET; g.sin_port = htons(PORT); inet_pton(AF_INET, GROUP, &g.sin_addr);
	if (multicast && !no_lan) sendto(tx, (const char *)b, (int)(p - b), 0, (struct sockaddr *)&g, sizeof g);
	for (int i = 0; i < nknown; i++) unicast(b, (size_t)(p - b), &known[i]);
	if (ov_port && multicast) ov_broadcast_disc(b, (size_t)(p - b));   /* (the overlay's linked copies hear this copy's adverts every second already) */
}
static void heard(const uint8_t *b, size_t n, const struct sockaddr_in *from, int via_overlay) {
	if (n >= 25 && !memcmp(b, "NVMQ", 4) && b[4] == 1) {   /* someone new: tell it now (and it, straight back: a firewall in front of it lets a reply in) */
		if (r64(b + 5) != self && !memcmp(b + 13, comm, 12)) {
			uint8_t a[HEAD_BYTES + 8 * ROOM_BYTES]; const size_t an = advert(a);
			if (via_overlay) ov_broadcast_disc(a, an); else unicast(a, an, from);
			/* (no disc_poke: the answer is this advert. Poking made this copy query everyone too, whose answers poked them:
			 * over the overlay, with four copies linked, thousands of adverts a second) */
		}
		return;
	}
	if (n < HEAD_BYTES || memcmp(b, "NVM2", 4) || b[4] != 6) return;
	const int k = b[5]; const uint64_t id = r64(b + 6);
	if (id == self || memcmp(b + 14, comm, 12) || n < HEAD_BYTES + (size_t)k * ROOM_BYTES) return;   /* (ours, or another game's) */
	const uint16_t port = r16(b + 26), their_ov = r16(b + 46);
	if (ov_port && !via_overlay) {   /* (its rooms come over the link: a plain advert only says where to say hello) */
		if (m2_log() >= 2) { char h[32]; inet_ntop(AF_INET, &from->sin_addr, h, sizeof h); fprintf(stderr, "    m2: discovery: copy %016llx at %s, its overlay port %u\n", (unsigned long long)id, h, their_ov); }
		if (their_ov) ov_add_candidate(id, from->sin_addr.s_addr, htons(their_ov));
		return;
	}
	if (via_overlay && ov_vip_of(id) != from->sin_addr.s_addr) return;   /* (a linked copy speaks for itself only) */
	name_clash(b + 30, r64(b + 48));   /* (from a sealed link, or the LAN with the overlay off) */
	char host[64]; inet_ntop(AF_INET, &from->sin_addr, host, sizeof host);
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); const uint64_t now = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
	pthread_mutex_lock(&lock);
	{
		int i, free_i = -1, old = 0;
		for (i = 0; i < MAXREMOTE && peers[i].from != id; i++) { if (free_i < 0 && (!peers[i].from || now - peers[i].seen > TTL_MS)) free_i = i; if (peers[i].seen < peers[old].seen) old = i; }
		if (i == MAXREMOTE) i = free_i >= 0 ? free_i : old;
		if (peers[i].from != id || peers[i].rooms != k) fprintf(stderr, "    m2: discovery: %d room(s) from %s:%u\n", k, host, port);   /* (a copy new, or its rooms changed) */
		peers[i].rooms = k;
		peers[i].from = id; peers[i].seen = now; memcpy(peers[i].player, b + 30, 16); peers[i].port = r16(b + 28); peers[i].room_port = port;
		snprintf(peers[i].host, sizeof peers[i].host, "%s", host);
	}
	for (int i = 0; i < MAXREMOTE; i++) if (remote[i].from == id) remote[i].from = 0;   /* (its rooms as it says now: the rest are gone) */
	const uint8_t *p = b + HEAD_BYTES;
	for (int j = 0; j < k; j++, p += ROOM_BYTES) {
		int i; for (i = 0; i < MAXREMOTE && remote[i].from; i++) {}
		if (i == MAXREMOTE) break;
		RoomAd *a = &remote[i].r.ad; memset(a, 0, sizeof *a);
		a->world = r16(p); a->lobby = r16(p + 2); a->room = r16(p + 4); a->flags = r32(p + 6); a->max = r16(p + 10);
		memcpy(a->slots, p + 12, 16); a->pwmask = r64(p + 28); a->have_ints = p[36];
		for (int q = 0; q < 8; q++) a->ints[q] = r32(p + 37 + 4 * q);
		memcpy(a->owner, p + 69, 24); a->addr_len = p[93] <= 16 ? p[93] : 16; memcpy(a->addr, p + 94, 16); a->gen = p[110];
		snprintf(remote[i].r.host, sizeof remote[i].r.host, "%s", host); remote[i].r.port = port;
		remote[i].from = id; remote[i].seen = now;
	}
	pthread_mutex_unlock(&lock);
}
void disc_input(const uint8_t *b, size_t n, uint32_t from_vip) {
	struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = from_vip; heard(b, n, &a, 1);
}
/* a name clash (notes/shipping-shape.md "Online names"): another player in this game with this copy's player's name, told
 * apart case-insensitively (the game shows names in capitals; two players of one name break a match). The one with the
 * lower account ID keeps the name; the other plays as the name and two digits of its account ID from its next session
 * (np.c, settings online_suffix, this game's own), and joins nothing in this one (m2_rooms.c locate: the game can't rename
 * a player mid-session) */
static int clash_lost;
int disc_name_clash(void) { return clash_lost; }
static void name_clash(const uint8_t their[16], uint64_t their_account) {
	const uint64_t mine = np_account_id();
	if (clash_lost || !their_account || their_account == mine || strncasecmp((const char *)their, player, 16)) return;
	char shown[17]; memcpy(shown, their, 16); shown[16] = 0;
	if (their_account > mine) { static int said; if (!said++) fprintf(stderr, "    m2: %s is this copy's name too: that player's copy gives way\n", shown); return; }
	clash_lost = 1;
	char sfx[8]; snprintf(sfx, sizeof sfx, "%02u", (unsigned)(mine >> 20) % 100u);
	settings_set_own("online_suffix", sfx); settings_save();
	char next[24]; snprintf(next, sizeof next, "%.14s%s", player, sfx);
	fprintf(stderr, "    m2: another player here is %s too, and had it first: this copy joins no rooms now, and plays as %s from its next session\n", player, next);
	char body[256]; snprintf(body, sizeof body, "Another player in this game is also %s. You'll be %s from the next time you start it; until then you can host a match but not join one.", player, next);
	overlay_toast_titled("Online name taken", body, 12.0);
}
int disc_rooms(RemoteRoom *out, int max) {
	if (started_ms && now_ms() - started_ms < 1500) plat_sleep_ns((1500 - (now_ms() - started_ms)) * 1000000ull);   /* (just started: the answers to our query) */
	const uint64_t now = now_ms();
	pthread_mutex_lock(&lock);
	int n = 0; for (int i = 0; i < MAXREMOTE; i++) { if (remote[i].from && now - remote[i].seen > TTL_MS) remote[i].from = 0; if (remote[i].from && n < max) out[n++] = remote[i].r; }
	pthread_mutex_unlock(&lock);
	return n;
}
int disc_status(int *rooms) {   /* (status.c) 1 once started, and the rooms heard of lately -- without disc_rooms' wait */
	const uint64_t now = now_ms(); int n = 0;
	pthread_mutex_lock(&lock); for (int i = 0; i < MAXREMOTE; i++) n += remote[i].from && now - remote[i].seen <= TTL_MS; pthread_mutex_unlock(&lock);
	*rooms = n; return started_ms != 0;
}
int disc_player(const char online_id[16], char host[64], uint16_t *port) {
	const uint64_t now = now_ms(); int r = -1;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAXREMOTE && r; i++)
		if (peers[i].from && now - peers[i].seen <= TTL_MS && peers[i].port && !strncmp(peers[i].player, online_id, 16)) { snprintf(host, 64, "%s", peers[i].host); *port = peers[i].port; r = 0; }
	pthread_mutex_unlock(&lock);
	return r;
}
int disc_room_server(const char online_id[16], char host[64], uint16_t *port) {
	const uint64_t now = now_ms(); int r = -1;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAXREMOTE && r; i++)
		if (peers[i].from && now - peers[i].seen <= TTL_MS && peers[i].room_port && !strncmp(peers[i].player, online_id, 16)) { snprintf(host, 64, "%s", peers[i].host); *port = peers[i].room_port; r = 0; }
	pthread_mutex_unlock(&lock);
	return r;
}
void disc_poke(void) { pthread_mutex_lock(&lock); poked = 1; pthread_cond_signal(&poke_cv); pthread_mutex_unlock(&lock); }

static void *rx_main(void *arg) {
	(void)arg; uint8_t b[2048];
	for (;;) { struct sockaddr_in from; socklen_t l = sizeof from; const int n = (int)recvfrom(rx, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &l); if (n > 0 && !no_lan) heard(b, (size_t)n, &from, 0); }
	return 0;
}
static void *tx_main(void *arg) {
	(void)arg;
	for (;;) {
		advertise(); query(0);   /* (the known peers: their adverts come back as replies) */
		online_status_tick();
		pthread_mutex_lock(&lock);
		struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += 1;
		while (!poked && pthread_cond_timedwait(&poke_cv, &lock, &until) == 0) {}
		poked = 0;
		pthread_mutex_unlock(&lock);
	}
	return 0;
}
void disc_start(const char *comm_id) {
	static int started; if (started) return; started = 1;
	no_lan = getenv("RECOMP_ONLINE_NO_LAN") && atoi(getenv("RECOMP_ONLINE_NO_LAN")) > 0;
	self = ov_self_id(); memset(comm, 0, sizeof comm); memcpy(comm, comm_id, strnlen(comm_id, sizeof comm));
	np_online_id(player); msg_port = (uint16_t)bmsg_start();
	if (ov_on() && (ov_port = (uint16_t)ov_start(comm))) ov_allow_port((uint16_t)m2_room_port());   /* (a joiner's stream to this copy's room server) */
	rx = (int)socket(AF_INET, SOCK_DGRAM, 0); tx = (int)socket(AF_INET, SOCK_DGRAM, 0);
	if (rx < 0 || tx < 0) { fprintf(stderr, "    m2: discovery: no sockets\n"); return; }
	int one = 1; setsockopt(rx, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
#ifdef SO_REUSEPORT
	setsockopt(rx, SOL_SOCKET, SO_REUSEPORT, (const char *)&one, sizeof one);   /* (every copy on this machine listens on the port) */
#endif
	struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(rx, (struct sockaddr *)&a, sizeof a)) { fprintf(stderr, "    m2: discovery: can't listen on %d\n", PORT); return; }
	struct ip_mreq mr; memset(&mr, 0, sizeof mr); inet_pton(AF_INET, GROUP, &mr.imr_multiaddr); mr.imr_interface.s_addr = htonl(INADDR_ANY);
	if (setsockopt(rx, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mr, sizeof mr)) fprintf(stderr, "    m2: discovery: can't join the group\n");
	unsigned char ttl = 1, loop = 1;   /* the local network only; this machine's other copies hear it too */
	setsockopt(tx, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof ttl); setsockopt(tx, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *)&loop, sizeof loop);
	pthread_t t; pthread_create(&t, 0, rx_main, 0); pthread_detach(t); pthread_create(&t, 0, tx_main, 0); pthread_detach(t);
	char peers[512]; const char *e = getenv("RECOMP_ONLINE_PEERS");
	if (e) snprintf(peers, sizeof peers, "%s", e); else settings_str("online_peers", "", peers, sizeof peers);
	for (char *q = strtok(peers, ", "); q && nknown < MAXPEERS; q = strtok(0, ", ")) {
		char *colon = strchr(q, ':'); const int qp = colon ? atoi(colon + 1) : 0; if (colon) *colon = 0;   /* (host[:overlay port]) */
		if (ov_port) ov_probe(q, (uint16_t)(qp ? qp : 36581));
		struct addrinfo hints, *res = 0; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
		if (getaddrinfo(q, 0, &hints, &res) || !res) { fprintf(stderr, "    m2: discovery: peer %s: no such host\n", q); continue; }
		known[nknown] = *(struct sockaddr_in *)res->ai_addr; known[nknown].sin_port = htons(PORT); nknown++; freeaddrinfo(res);
	}
	started_ms = now_ms(); query(1);
	fprintf(stderr, "    m2: discovery for %.12s on %s:%d%s%s\n", comm, GROUP, PORT, nknown ? ", and peers " : "", nknown ? (e ? e : "(settings)") : "");
}
