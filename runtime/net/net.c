/*
 * net.c: the Vita's network stack on the host's own (SceNet, SceNetCtl) -- online play talks to real servers.
 *
 *   sockets   sceNetSocket/Bind/Connect/Listen/Accept/Send(to)/Recv(from)/Setsockopt/Getsockopt/SocketClose over host
 *             sockets: the guest's SceNetSockaddrIn (16 bytes: len, family, port, address, vport, zero) to and from the
 *             host's sockaddr_in, the Vita's option numbers (SOL_SOCKET 0xffff, SO_NBIO, microsecond timeouts) to the
 *             host's, BSD errno numbers to the guest's sceNetErrnoLoc word and the 0x80410100 | errno results.
 *   P2P       DGRAM_P2P sockets: the Vita's UDPP2P, sockets told apart by a virtual port over a UDP port (below).
 *             STREAM_P2P is plain TCP.
 *   epoll     sceNetEpoll* over poll(): each id's interest set and data word, the ready ones written as SceNetEpollEvent
 *             (0x20 bytes: events, reserved, 16 system bytes, the 8-byte data) -- timeouts in microseconds.
 *   resolver  sceNetResolverStartNtoa over getaddrinfo (IPv4), after the private-server redirect (below).
 *   netctl    connected (SCE_NETCTL_STATE_CONNECTED), the host's own IPv4 address, netmask, route and DNS as the
 *             connection's info, NAT type 1 (open, as a LAN is) with STUN available.
 *
 * The private-server redirect: the game's own server names (its RUN_ENV: RECOMP_NET_SERVERS) to the player's private
 * server (settings "private_server" = <host>[:<port>], the F10 menu's Game page); or, by hand, settings "net_redirect"
 * (RECOMP_NET_REDIRECT wins) = "<host>=<target>[:<port>][,...]": the game's server names answered with another host -- Killzone's kzv.online.scee.com, Sony's own servers long gone,
 * pointed at a community server (Mirage: mirage.yuv.pink). The resolver returns the target's address; HTTP (http.c) sends
 * the request to a named server by its own name, its certificate verified, or to an address or localhost as the game's
 * name, unverified (net_redirect_lookup). Everything else resolves as usual.
 *
 * Sony's own hosts (net_blocked) are never contacted unless redirected elsewhere.
 *
 * RECOMP_NET_LOG=1: every socket, resolver and connection call with its result (2: every datagram too).
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>
#define CLOSESOCK close
#endif
#include "hle.h"
#include "settings.h"
#include "net.h"
#include "platform.h"
#include "online/m2.h"
#include "online/overlay.h"

static int net_log(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_NET_LOG"); v = e ? (atoi(e) > 0 ? atoi(e) : 1) : 0; } return v; }
const char *net_online_needs(void) { const char *e = getenv("RECOMP_ONLINE_NEEDS"); return e && *e ? e : 0; }   /* (net.h) */
int net_online(void) {   /* (net.h) */
	static int v = -1;
	if (v < 0) {
		const char *e = getenv("RECOMP_ONLINE");
		v = e ? atoi(e) > 0 : !getenv("RECOMP_HEADLESS") && settings_game_option("online") && settings_int("online", 1) > 0 && !net_online_needs();
		if (!e && settings_game_option("online")) fprintf(stderr, "    net: online %s (settings \"online\")%s%s\n", v ? "on" : "off", net_online_needs() ? ": this copy isn't a version online play is for, which needs update " : "", net_online_needs() ? net_online_needs() : "");
	}
	return v;
}

/* ---- the private-server redirect */
int net_redirect_lookup(const char *host, char *target, int tsize, int *port) {
	char spec[512]; const char *e = getenv("RECOMP_NET_REDIRECT");
	if (e) snprintf(spec, sizeof spec, "%s", e); else settings_str("net_redirect", "", spec, sizeof spec);
	size_t hl = strlen(host);
	for (char *p = spec; *p; ) {
		char *end = strchr(p, ','); if (end) *end = 0;
		while (*p == ' ') p++;
		char *eq = strchr(p, '=');
		if (eq && (size_t)(eq - p) == hl && !strncasecmp(p, host, hl)) {
			char *t = eq + 1, *colon = strrchr(t, ':');
			if (port) *port = 0;
			if (colon && colon > t && strchr(t, '.') && strspn(colon + 1, "0123456789") == strlen(colon + 1)) { if (port) *port = atoi(colon + 1); *colon = 0; }
			snprintf(target, (size_t)tsize, "%s", t);
			return 1;
		}
		if (!end) break;
		p = end + 1;
	}
	/* the game's own server names (its RUN_ENV: RECOMP_NET_SERVERS=<host>[,...]) go to the player's private server
	 * (settings "private_server" = <host>[:<port>], set on the F10 menu's Game page) */
	const char *names = getenv("RECOMP_NET_SERVERS"); char ps[256]; settings_str("private_server", "", ps, sizeof ps);
	if (names && ps[0]) {
		for (const char *q = names; *q; ) {
			size_t l = strcspn(q, ",");
			if (l == hl && !strncasecmp(q, host, hl)) {
				char *colon = strrchr(ps, ':'); if (port) *port = 0;
				if (colon && strspn(colon + 1, "0123456789") == strlen(colon + 1) && colon[1]) { if (port) *port = atoi(colon + 1); *colon = 0; }
				snprintf(target, (size_t)tsize, "%s", ps);
				return 1;
			}
			q += l; if (*q == ',') q++;
		}
	}
	return 0;
}

/* Sony's own hosts are never reached: a name under these domains that no redirect points elsewhere does not resolve, and
 * http.c refuses it. Running Sony's own client modules (np_matching2 fetches its server list from playstation.net, which
 * Sony still serves for Killzone) must not put an unofficial client with an unsigned ticket on PSN: the account and
 * console ban risk, and PSN's terms. No setting turns this off. */
int net_blocked(const char *host) {
	static const char *const sony[] = { "playstation.net", "playstation.com", "sonyentertainmentnetwork.com", "scee.com", "scea.com", "sie.com" };
	char target[256]; int port;
	if (net_redirect_lookup(host, target, sizeof target, &port)) return 0;
	size_t hl = strlen(host);
	for (size_t i = 0; i < sizeof sony / sizeof *sony; i++) {
		size_t dl = strlen(sony[i]);
		if (hl >= dl && !strcasecmp(host + hl - dl, sony[i]) && (hl == dl || host[hl - dl - 1] == '.')) { fprintf(stderr, "    net: %s is Sony's: not contacted\n", host); return 1; }
	}
	return 0;
}

/* ---- errno: the host's numbers to the Vita's (BSD's: macOS's are the same, Linux's differ) */
static int vita_errno(int e) {
	switch (e) {
	case EPERM: return 1; case ENOENT: return 2; case EINTR: return 4; case EBADF: return 9; case EACCES: return 13; case EFAULT: return 14;
	case EINVAL: return 22; case EMFILE: return 24; case EPIPE: return 32;
#if EWOULDBLOCK != EAGAIN
	case EWOULDBLOCK:
#endif
	case EAGAIN: return 35;
	case EINPROGRESS: return 36; case EALREADY: return 37; case ENOTSOCK: return 38; case EDESTADDRREQ: return 39; case EMSGSIZE: return 40;
	case EPROTOTYPE: return 41; case ENOPROTOOPT: return 42; case EPROTONOSUPPORT: return 43; case EOPNOTSUPP: return 45; case EAFNOSUPPORT: return 47;
	case EADDRINUSE: return 48; case EADDRNOTAVAIL: return 49; case ENETDOWN: return 50; case ENETUNREACH: return 51; case ENETRESET: return 52;
	case ECONNABORTED: return 53; case ECONNRESET: return 54; case ENOBUFS: return 55; case EISCONN: return 56; case ENOTCONN: return 57;
	case ETIMEDOUT: return 60; case ECONNREFUSED: return 61;
#ifdef EHOSTDOWN
	case EHOSTDOWN: return 64;   /* (mingw has no EHOSTDOWN) */
#endif
	case EHOSTUNREACH: return 65;
	default: return 22;
	}
}
/* Winsock leaves errno alone: a failed socket call's error is WSAGetLastError()'s, in Winsock's own numbers. SOCK_ERRNO()
 * right after one puts it in errno as the POSIX number, for everything after (fail_host, the logs, the EADDRINUSE retries) */
#ifdef _WIN32
static int wsa_errno(int w) {
	switch (w) {
	case WSAEWOULDBLOCK: return EAGAIN; case WSAEINPROGRESS: return EINPROGRESS; case WSAEALREADY: return EALREADY; case WSAENOTSOCK: return ENOTSOCK;
	case WSAEDESTADDRREQ: return EDESTADDRREQ; case WSAEMSGSIZE: return EMSGSIZE; case WSAEPROTOTYPE: return EPROTOTYPE; case WSAENOPROTOOPT: return ENOPROTOOPT;
	case WSAEPROTONOSUPPORT: return EPROTONOSUPPORT; case WSAEOPNOTSUPP: return EOPNOTSUPP; case WSAEAFNOSUPPORT: return EAFNOSUPPORT;
	case WSAEADDRINUSE: return EADDRINUSE; case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL; case WSAENETDOWN: return ENETDOWN; case WSAENETUNREACH: return ENETUNREACH;
	case WSAENETRESET: return ENETRESET; case WSAECONNABORTED: return ECONNABORTED; case WSAECONNRESET: return ECONNRESET; case WSAENOBUFS: return ENOBUFS;
	case WSAEISCONN: return EISCONN; case WSAENOTCONN: return ENOTCONN; case WSAETIMEDOUT: return ETIMEDOUT; case WSAECONNREFUSED: return ECONNREFUSED;
	case WSAEHOSTUNREACH: return EHOSTUNREACH; case WSAEMFILE: return EMFILE; case WSAEBADF: return EBADF; case WSAEINTR: return EINTR;
	case WSAEACCES: return EACCES; case WSAEFAULT: return EFAULT; default: return EINVAL;
	}
}
#define SOCK_ERRNO() (errno = wsa_errno(WSAGetLastError()))
#else
#define SOCK_ERRNO() ((void)0)
#endif
static __thread uint32_t errno_word;   /* this thread's guest errno (sceNetErrnoLoc) */
static uint32_t errno_addr(void) { if (!errno_word) { errno_word = guest_alloc(4, 4); WR32(errno_word, 0); } return errno_word; }
static uint32_t fail(int vita_e) { WR32(errno_addr(), (uint32_t)vita_e); return 0x80410100u | (uint32_t)vita_e; }
static uint32_t fail_host(void) { return fail(vita_errno(errno)); }
void net_set_guest_errno(int host_errno) { WR32(errno_addr(), (uint32_t)vita_errno(host_errno)); }   /* (ssl_local.c) */

/* ---- sockets: Vita ids 1.. over host descriptors */
#define MAXSOCK 256
static struct { int used, fd, type, nbio; char name[32]; int mux; uint16_t vport, lport; struct sockaddr_in peer; uint16_t peer_vport; uint8_t hello; uint32_t hello_ip; } socks[MAXSOCK];
/* STREAM_P2P connections accepted but not yet reported: waiting for their preamble (below, before h_net_bind) */
enum { HELLO_LEN = 12, MAXSTAGE = 8 };
static int host_flags(uint32_t f);
static struct { int used, fd; uint32_t listener; uint64_t since; struct sockaddr_in from; } staged[MAXSTAGE];   /* (sock_lock) */
static pthread_mutex_t sock_lock = PTHREAD_MUTEX_INITIALIZER;
static int sock_fd(uint32_t id) { return id > 0 && id < MAXSOCK && socks[id].used ? socks[id].fd : -1; }
int net_socket_fd(uint32_t id) { return sock_fd(id); }

static void to_host_addr(uint32_t ga, struct sockaddr_in *sa) {
	memset(sa, 0, sizeof *sa); sa->sin_family = AF_INET;
	memcpy(&sa->sin_port, G(ga + 2), 2); memcpy(&sa->sin_addr, G(ga + 4), 4);   /* both in network order, as stored */
}
static void to_guest_addr(const struct sockaddr_in *sa, uint32_t ga, uint32_t lenp) {
	if (!ga) return;
	uint8_t b[16] = { 16, 2 }; memcpy(b + 2, &sa->sin_port, 2); memcpy(b + 4, &sa->sin_addr, 4);
	memcpy(G(ga), b, 16);
	if (lenp) WR32(lenp, 16);
}
static const char *addr_str(const struct sockaddr_in *sa, char *buf, size_t n) { char ip[32]; inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip); snprintf(buf, n, "%s:%u", ip, ntohs(sa->sin_port)); return buf; }

/* ---- P2P sockets (DGRAM_P2P, type 6). On a Vita, UDPP2P carries many sockets over a UDP port, told apart by a virtual
 * port (sin_vport), and a plain UDP socket can share the port: np_matching2's signaling binds its STUN socket and a P2P
 * echo socket to one port (STUN finds the address peers reach that port at), and holds each peer connection's port with a
 * plain socket that never reads ("ConnDummy") under its P2P one ("ConnEcho", vport 0xfffe). Here a port with P2P sockets
 * is a host UDP socket, a mux, and the guest sockets on it read through loopback UDP sockets of their own, which recv,
 * poll and epoll treat like any other: {address, port, vport} ahead of the data. P2P datagrams go framed {'N', 'P', the
 * source vport, the destination vport} (both as the guest stores them: network order; Nativ to Nativ, where a Vita's
 * UDPP2P frames and encrypts its own way) and arrive at the socket bound to the destination vport; anything else arrives
 * at the plain socket sharing the port, which also sends through the mux (STUN sees the shared port). The P2P port, 3658,
 * moves to the next free one when another copy on this machine has it: getsockname reports the real port, which the
 * signaling tells its peers. As a Vita's kernel keeps an association per peer (np_matching2's 'P' ioctls), a P2P
 * datagram to a peer goes out through the port that peer was last heard on (the one signaling connected through: the
 * game's own socket on 3658 reaches the peer as signaling did), and one that arrives for a vport no socket on its port
 * has goes to the socket with that vport on another (Killzone's game socket, 3658 vport 1, hears its peers through the
 * signaling port). */
#define P2P_PORT 3658
enum { P2P_FRAME = 6, P2P_DELIVER = 8, MAXMUX = 16 };
static struct { int used, fd, closing; uint16_t gport, hport; } muxes[MAXMUX];
static int deliver_fd = -1;
enum { MAXROUTE = 32 };
static struct { uint32_t ip; uint16_t port; int mux; } routes[MAXROUTE];   /* (sock_lock) a peer -> the mux it was last heard on */
static int route_next;
static void route_note(const struct sockaddr_in *from, int m) {   /* (sock_lock held) */
	for (int i = 0; i < MAXROUTE; i++) if (routes[i].ip == from->sin_addr.s_addr && routes[i].port == from->sin_port) { routes[i].mux = m; return; }
	routes[route_next].ip = from->sin_addr.s_addr; routes[route_next].port = from->sin_port; routes[route_next].mux = m; route_next = (route_next + 1) % MAXROUTE;
}
static int route_of(const struct sockaddr_in *to) {   /* (sock_lock held) the live mux a peer was heard on, or -1 */
	for (int i = 0; i < MAXROUTE; i++)
		if (routes[i].ip == to->sin_addr.s_addr && routes[i].port == to->sin_port && routes[i].port) { const int m = routes[i].mux; return m >= 0 && muxes[m].used && !muxes[m].closing ? m : -1; }
	return -1;
}
static int is_p2p(uint32_t id) { return id > 0 && id < MAXSOCK && socks[id].used && socks[id].type == 6; }
static int on_mux(uint32_t id) { return id > 0 && id < MAXSOCK && socks[id].used && (socks[id].type == 6 || (socks[id].type == 2 && socks[id].mux >= 0)); }
static int wait_readable(int fd, int ms) {
	struct pollfd pf = { fd, POLLIN, 0 };
#ifdef _WIN32
	return WSAPoll(&pf, 1, ms);
#else
	return poll(&pf, 1, ms);
#endif
}
static uint16_t fd_port(int fd) { struct sockaddr_in a; socklen_t l = sizeof a; return getsockname(fd, (struct sockaddr *)&a, &l) ? 0 : ntohs(a.sin_port); }
static int loopback_socket(uint16_t *port) {   /* a UDP socket on 127.0.0.1, an ephemeral port: the fd, or -1 */
	int fd = (int)socket(AF_INET, SOCK_DGRAM, 0); if (fd < 0) return -1;
	struct sockaddr_in lo; memset(&lo, 0, sizeof lo); lo.sin_family = AF_INET; lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(fd, (struct sockaddr *)&lo, sizeof lo) || !(*port = fd_port(fd))) { CLOSESOCK(fd); return -1; }
	return fd;
}
/* a datagram that arrived at mux m's port from `from` (the data at b + P2P_DELIVER, n bytes; the room before it is the
 * delivery header's): to the guest socket it is for */
static void mux_deliver(int m, const struct sockaddr_in *from_p, uint8_t *b, long n) {
	const struct sockaddr_in from = *from_p;
	{
		const int framed = n >= P2P_FRAME && b[P2P_DELIVER] == 'N' && b[P2P_DELIVER + 1] == 'P';
		uint16_t src = 0, dst = 0; if (framed) { memcpy(&src, b + P2P_DELIVER + 2, 2); memcpy(&dst, b + P2P_DELIVER + 4, 2); }
		uint8_t *h = framed ? b + P2P_FRAME : b;   /* the header goes just ahead of the data (over the frame) */
		memcpy(h, &from.sin_addr, 4); memcpy(h + 4, &from.sin_port, 2); memcpy(h + 6, &src, 2);
		pthread_mutex_lock(&sock_lock);
		int to = -1, any = -1, other = -1;
		if (framed) route_note(&from, m);
		for (int i = 1; i < MAXSOCK && to < 0; i++) {
			if (!socks[i].used) continue;
			if (socks[i].mux != m) { if (framed && socks[i].type == 6 && socks[i].vport == dst && socks[i].mux >= 0 && (other < 0 || muxes[socks[i].mux].gport == P2P_PORT)) other = i; continue; }
			if (!framed) { if (socks[i].type == 2) to = i; }
			else if (socks[i].type == 6) { if (socks[i].vport == dst) to = i; else if (!socks[i].vport && any < 0) any = i; }
		}
		if (to < 0) to = other >= 0 ? other : any;   /* (the vport's socket on another port before a catch-all here) */
		const uint16_t lport = to > 0 ? socks[to].lport : 0;
		pthread_mutex_unlock(&sock_lock);
		if (!lport) { if (net_log()) fprintf(stderr, "    net: p2p port %u: nothing for %s %u\n", muxes[m].hport, framed ? "vport" : "plain datagram of", framed ? ntohs(dst) : (unsigned)n); return; }
		struct sockaddr_in lo; memset(&lo, 0, sizeof lo); lo.sin_family = AF_INET; lo.sin_port = htons(lport); lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sendto(deliver_fd, (const char *)h, (int)(framed ? n - P2P_FRAME + P2P_DELIVER : n + P2P_DELIVER), 0, (struct sockaddr *)&lo, sizeof lo);
	}
}
static void *mux_main(void *arg) {
	const int m = (int)(intptr_t)arg; uint8_t b[P2P_DELIVER + 65536];
	while (!muxes[m].closing) {
		if (wait_readable(muxes[m].fd, 200) <= 0) continue;
		struct sockaddr_in from; socklen_t l = sizeof from;
		const long n = (long)recvfrom(muxes[m].fd, (char *)b + P2P_DELIVER, sizeof b - P2P_DELIVER, 0, (struct sockaddr *)&from, &l);
		if (n > 0) mux_deliver(m, &from, b, n);
	}
	if (net_log()) fprintf(stderr, "    net: p2p port %u closed (fd %d)\n", muxes[m].hport, muxes[m].fd);
	CLOSESOCK(muxes[m].fd);
	pthread_mutex_lock(&sock_lock); muxes[m].used = 0; pthread_mutex_unlock(&sock_lock);
	return 0;
}
/* a datagram from another copy over the overlay (online/overlay.h), to the port it was sent to: the P2P port the guest
 * has there (its real port, or the one it asked for: 3658 on a second copy here is 3659) */
static void net_ov_dgram(uint32_t src_vip, uint16_t src_port, uint16_t dst_port, const uint8_t *data, size_t n) {
	if (n > 65536 - P2P_DELIVER) return;
	pthread_mutex_lock(&sock_lock);
	int m = -1; for (int i = 0; i < MAXMUX && m < 0; i++) if (muxes[i].used && !muxes[i].closing && muxes[i].hport == dst_port) m = i;
	for (int i = 0; i < MAXMUX && m < 0; i++) if (muxes[i].used && !muxes[i].closing && muxes[i].gport == dst_port) m = i;
	pthread_mutex_unlock(&sock_lock);
	if (m < 0) { if (net_log()) fprintf(stderr, "    net: overlay datagram for port %u: no P2P port there\n", dst_port); return; }
	static __thread uint8_t *b; if (!b) b = malloc(P2P_DELIVER + 65536);
	memcpy(b + P2P_DELIVER, data, n);
	struct sockaddr_in from; memset(&from, 0, sizeof from); from.sin_family = AF_INET; from.sin_addr.s_addr = src_vip; from.sin_port = htons(src_port);
	mux_deliver(m, &from, b, (long)n);
}
static int udp_bound(uint16_t port, int *fd_out) {   /* a host UDP socket on that port: 0, or -1 with errno */
	int fd = (int)socket(AF_INET, SOCK_DGRAM, 0); if (fd < 0) { SOCK_ERRNO(); return -1; }
	struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(fd, (struct sockaddr *)&a, sizeof a)) { SOCK_ERRNO(); const int e = errno; CLOSESOCK(fd); errno = e; return -1; }
	*fd_out = fd; return 0;
}
/* a plain UDP socket joins a mux: it reads through a loopback socket from now on (sock_lock held) */
static int attach(uint32_t id, int m) {
	uint16_t lport; const int lfd = loopback_socket(&lport); if (lfd < 0) return -1;
	if (net_log()) fprintf(stderr, "    net: socket %u '%s' joins p2p port mux %d (fd %d -> %d)\n", id, socks[id].name, m, socks[id].fd, lfd);
	CLOSESOCK(socks[id].fd); socks[id].fd = lfd; socks[id].lport = lport; socks[id].mux = m;
	if (socks[id].nbio) {
#ifdef _WIN32
		u_long nb = 1; ioctlsocket(lfd, FIONBIO, &nb);
#else
		fcntl(lfd, F_SETFL, fcntl(lfd, F_GETFL, 0) | O_NONBLOCK);
#endif
	}
	return 0;
}
/* the mux for a guest port (sock_lock held): an existing one, or a new one on that port, which a plain socket holding
 * it joins */
static int mux_for(uint16_t gport) {
	for (int m = 0; m < MAXMUX; m++) if (muxes[m].used && !muxes[m].closing && gport && muxes[m].gport == gport) return m;
	int m; for (m = 0; m < MAXMUX && muxes[m].used; m++) {}
	if (m == MAXMUX) { errno = EMFILE; return -1; }
	if (deliver_fd < 0 && (deliver_fd = (int)socket(AF_INET, SOCK_DGRAM, 0)) < 0) { SOCK_ERRNO(); return -1; }
	int holder = 0;
	if (gport) for (int i = 1; i < MAXSOCK && !holder; i++) if (socks[i].used && socks[i].type == 2 && socks[i].mux < 0 && fd_port(socks[i].fd) == gport) holder = i;
	if (holder && attach((uint32_t)holder, m)) return -1;   /* (its host socket closed: the port is free for the mux) */
	int fd = -1; uint16_t h = gport;
	while (udp_bound(h, &fd)) {
		if (errno != EADDRINUSE || gport != P2P_PORT || h >= P2P_PORT + 15) { if (holder) socks[holder].mux = -1; return -1; }
		h++;   /* (another copy on this machine has it) */
	}
	muxes[m].used = 1; muxes[m].closing = 0; muxes[m].fd = fd; muxes[m].gport = gport ? gport : fd_port(fd); muxes[m].hport = fd_port(fd);
	pthread_t t; pthread_create(&t, 0, mux_main, (void *)(intptr_t)m); pthread_detach(t);
	if (net_log() || h != gport) fprintf(stderr, "    net: p2p port %u on %u%s%s\n", gport, muxes[m].hport, holder ? ", shared with " : "", holder ? socks[holder].name : "");
	return m;
}
static void mux_release(int m) {   /* (sock_lock held) a socket on it closed: the mux goes with the last */
	if (m < 0) return;
	for (int i = 1; i < MAXSOCK; i++) if (socks[i].used && socks[i].mux == m && (socks[i].type == 6 || socks[i].type == 2)) return;
	muxes[m].closing = 1;
}
static void p2p_bind(Cpu *c) {   /* (s, SceNetSockaddrIn *): the port's mux and the vport */
	struct sockaddr_in sa; to_host_addr(A1, &sa); uint16_t vport; memcpy(&vport, G(A1 + 8), 2);
	pthread_mutex_lock(&sock_lock);
	const int old = socks[A0].mux; socks[A0].mux = -1;
	const int m = mux_for(ntohs(sa.sin_port)), e = errno;
	if (m >= 0) { socks[A0].mux = m; socks[A0].vport = vport; }
	if (old != m) mux_release(old);
	pthread_mutex_unlock(&sock_lock);
	if (net_log()) fprintf(stderr, "    net: bind %u p2p port %u vport %u -> %d\n", A0, ntohs(sa.sin_port), ntohs(vport), m < 0 ? e : 0);
	if (m < 0) { errno = e; RET(fail_host()); return; }
	RET(0);
}
/* a datagram out through the socket's mux: framed for a P2P socket, as it is for the plain one sharing the port */
static long mux_send(uint32_t id, const uint8_t *data, uint32_t n, const struct sockaddr_in *to, uint16_t to_vport) {
	pthread_mutex_lock(&sock_lock);
	if (socks[id].mux < 0) socks[id].mux = mux_for(0);   /* (an unbound P2P socket: an ephemeral port, as a Vita binds one) */
	const int framed = socks[id].type == 6, rt = framed ? route_of(to) : -1, m = rt >= 0 ? rt : socks[id].mux, fd = m >= 0 ? muxes[m].fd : -1; const uint16_t vport = socks[id].vport;
	const uint16_t hport = m >= 0 ? muxes[m].hport : 0;
	pthread_mutex_unlock(&sock_lock);
	const int virt = ov_is_vip(to->sin_addr.s_addr);   /* (another copy's virtual address: over the overlay) */
	if (fd < 0) { errno = EBADF; return -1; }
	if (n > 65536 - P2P_FRAME) { errno = EMSGSIZE; return -1; }
	long r;
	if (framed) {
		static __thread uint8_t *b; if (!b) b = malloc(65536);
		b[0] = 'N'; b[1] = 'P'; memcpy(b + 2, &vport, 2); memcpy(b + 4, &to_vport, 2); memcpy(b + P2P_FRAME, data, n);
		if (virt) r = ov_send_dgram(to->sin_addr.s_addr, ntohs(to->sin_port), hport, b, n + P2P_FRAME) ? (errno = EHOSTUNREACH, -1) : (long)n;
		else { r = (long)sendto(fd, (const char *)b, (int)(n + P2P_FRAME), 0, (const struct sockaddr *)to, sizeof *to); if (r >= 0) r -= P2P_FRAME; else SOCK_ERRNO(); }
	} else if (virt) r = ov_send_dgram(to->sin_addr.s_addr, ntohs(to->sin_port), hport, data, n) ? (errno = EHOSTUNREACH, -1) : (long)n;
	else if ((r = (long)sendto(fd, (const char *)data, (int)n, 0, (const struct sockaddr *)to, sizeof *to)) < 0) SOCK_ERRNO();
	if (net_log() > 1 || (net_log() && r < 0)) { char s[48]; fprintf(stderr, "    net: sendto %u %s %u bytes %s vport %u -> %ld%s", id, framed ? "p2p" : "shared", n, addr_str(to, s, sizeof s), ntohs(to_vport), r < 0 ? -(long)errno : r, net_log() > 2 ? ":" : "\n");
		if (net_log() > 2) { for (uint32_t i = 0; i < n && i < 64; i++) fprintf(stderr, " %02x", data[i]); fprintf(stderr, "\n"); } }
	return r;
}
static long mux_recv(uint32_t id, uint32_t buf, uint32_t len, int flags, uint32_t from, uint32_t fromlen) {
	static __thread uint8_t *b; if (!b) b = malloc(P2P_DELIVER + 65536);
	const long n = (long)recv(socks[id].fd, (char *)b, (int)(P2P_DELIVER + (len < 65536 ? len : 65536)), flags);
	if (n < 0) { SOCK_ERRNO(); return -1; }
	if (n < P2P_DELIVER) return 0;
	const uint32_t k = (uint32_t)(n - P2P_DELIVER);
	memcpy(G(buf), b + P2P_DELIVER, k);
	if (from) { uint8_t a[16] = { 16, 2 }; memcpy(a + 2, b + 4, 2); memcpy(a + 4, b, 4); memcpy(a + 8, b + 6, 2); memcpy(G(from), a, 16); if (fromlen) WR32(fromlen, 16); }
	if (net_log() > 1) { char ip[32]; inet_ntop(AF_INET, b, ip, sizeof ip); uint16_t p; memcpy(&p, b + 4, 2); fprintf(stderr, "    net: recv %u %u bytes from %s:%u%s", id, k, ip, ntohs(p), net_log() > 2 ? ":" : "\n");
		if (net_log() > 2) { for (uint32_t i = 0; i < k && i < 64; i++) fprintf(stderr, " %02x", ((const uint8_t *)G(buf))[i]); fprintf(stderr, "\n"); } }
	return k;
}

static void h_net_init(Cpu *c) {
#ifdef _WIN32
	static int wsa; if (!wsa) { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); wsa = 1; }
#endif
	if (ov_on()) ov_set_dgram_sink(net_ov_dgram);
	RET(0);
}
static void h_net_term(Cpu *c) { RET(0); }
static void h_net_errno_loc(Cpu *c) { RET(errno_addr()); }
static void h_net_socket(Cpu *c) {   /* (name, domain, type, protocol) */
	int type = (int)A2, htype = (type == 1 || type == 10) ? SOCK_STREAM : (type == 2 || type == 6 || (type == 3 && A3 == 1)) ? SOCK_DGRAM : -1;
	if (A1 != 2 || htype < 0) { RET(fail(type == 3 ? 43 : 47)); return; }   /* (raw sockets other than ICMP: unsupported) */
	uint16_t lport = 0;
	/* P2P: the loopback socket its port's mux delivers to; raw ICMP (np_matching2's signaling opens one and gives up
	 * without it): one that sends nowhere and hears nothing, as on a network that answers no pings */
	int fd = type == 6 || type == 3 ? loopback_socket(&lport) : (int)socket(AF_INET, htype, 0);
	if (fd < 0) { SOCK_ERRNO(); RET(fail_host()); return; }
#ifdef SO_NOSIGPIPE
	{ int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
	pthread_mutex_lock(&sock_lock);
	int id; for (id = 1; id < MAXSOCK && socks[id].used; id++) {}
	if (id < MAXSOCK) {
		socks[id].used = 1; socks[id].fd = fd; socks[id].type = type; socks[id].nbio = 0; snprintf(socks[id].name, sizeof socks[id].name, "%s", A0 ? (const char *)G(A0) : "");
		socks[id].mux = -1; socks[id].vport = 0; socks[id].lport = lport; socks[id].peer_vport = 0; memset(&socks[id].peer, 0, sizeof socks[id].peer);
		socks[id].hello = 0; socks[id].hello_ip = 0;
	}
	pthread_mutex_unlock(&sock_lock);
	if (id == MAXSOCK) { CLOSESOCK(fd); RET(fail(24)); return; }
	if (net_log()) fprintf(stderr, "    net: socket %d '%s' type %d (fd %d)\n", id, socks[id].name, type, fd);
	RET(id);
}
static void h_net_close(Cpu *c) {
	int fd = sock_fd(A0); if (fd < 0) { RET(fail(9)); return; }
	pthread_mutex_lock(&sock_lock); socks[A0].used = 0; if (socks[A0].type == 6 || socks[A0].type == 2) mux_release(socks[A0].mux);
	for (int i = 0; i < MAXSTAGE; i++) if (staged[i].used && staged[i].listener == A0) { CLOSESOCK(staged[i].fd); staged[i].used = 0; }
	pthread_mutex_unlock(&sock_lock);
	CLOSESOCK(fd);
	if (net_log()) fprintf(stderr, "    net: close %u (fd %d)\n", A0, fd);
	RET(0);
}
/* ---- STREAM_P2P (type 10) between copies. On a Vita a P2P stream runs over the UDPP2P port, so an accepted connection's
 * address carries the peer's P2P port in sin_vport: the port signaling knows that peer by. Unit 13 looks the peer up by
 * {sin_addr, sin_vport} and closes a connection it doesn't know (2026-10-26). Here the stream is TCP, so the connecting
 * copy sends a preamble first: "NVS1", u32 its address as its peers know it (0: the connection's own source; over the
 * overlay its virtual address), u16 the port its signaling uses (network order), u16 0. Accept reports those; a
 * connection whose preamble doesn't come within 2 s, or that sends something else first, is reported as before. */
static uint64_t stage_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u; }
static uint16_t signaling_port(void) {   /* (sock_lock held) the host port np_matching2's signaling uses, network order; 0: none */
	for (int i = 1; i < MAXSOCK; i++)
		if (socks[i].used && socks[i].type == 6 && socks[i].mux >= 0 && muxes[socks[i].mux].used && !strcmp(socks[i].name, "SceNpMatching2SignalingEcho"))
			return htons(muxes[socks[i].mux].hport);
	return 0;
}
static int stream_hello(uint32_t id) {   /* the preamble out once the connection is up: 1 when nothing is left to send */
	if (!socks[id].hello) return 1;
	uint8_t b[HELLO_LEN] = { 'N', 'V', 'S', '1' };
	pthread_mutex_lock(&sock_lock); const uint16_t sp = signaling_port(); pthread_mutex_unlock(&sock_lock);
	memcpy(b + 4, &socks[id].hello_ip, 4); memcpy(b + 8, &sp, 2);
	if ((long)send(socks[id].fd, (const char *)b, HELLO_LEN, host_flags(0)) != HELLO_LEN) return 0;   /* (not connected yet) */
	socks[id].hello = 0;
	if (net_log()) fprintf(stderr, "    net: stream %u: told the other copy its signaling port, %u\n", id, ntohs(sp));
	return 1;
}
static int stage_take(uint32_t listener, struct sockaddr_in *sa, uint16_t *vport) {   /* (sock_lock held) a staged connection that's ready: its fd, or -1 */
	const uint64_t now = stage_ms();
	for (int i = 0; i < MAXSTAGE; i++) {
		if (!staged[i].used || staged[i].listener != listener) continue;
		uint8_t b[HELLO_LEN]; long n = wait_readable(staged[i].fd, 0) > 0 ? (long)recv(staged[i].fd, (char *)b, HELLO_LEN, MSG_PEEK) : -2;
		if (n == 0 || n == -1) { CLOSESOCK(staged[i].fd); staged[i].used = 0; continue; }   /* (gone before it said who it is) */
		const int hello = n == HELLO_LEN && !memcmp(b, "NVS1", 4), other = n > 0 && memcmp(b, "NVS1", (size_t)n < 4 ? (size_t)n : 4);
		if (!hello && !other && now - staged[i].since < 2000) continue;   /* (not here yet) */
		*sa = staged[i].from; *vport = 0;
		if (hello) {
			if (recv(staged[i].fd, (char *)b, HELLO_LEN, 0) != HELLO_LEN) { CLOSESOCK(staged[i].fd); staged[i].used = 0; continue; }
			uint32_t ip; memcpy(&ip, b + 4, 4); if (ip) sa->sin_addr.s_addr = ip;
			memcpy(vport, b + 8, 2);
		}
		staged[i].used = 0; return staged[i].fd;
	}
	return -1;
}
static void h_net_bind(Cpu *c) {
	int fd = sock_fd(A0); if (fd < 0 || !A1) { RET(fail(9)); return; }
	if (is_p2p(A0)) { p2p_bind(c); return; }
	if (socks[A0].type == 3) { RET(0); return; }
	struct sockaddr_in sa; to_host_addr(A1, &sa); char s[48];
	if (socks[A0].type == 2 && sa.sin_port) {   /* (a port P2P sockets have: shared through its mux) */
		pthread_mutex_lock(&sock_lock);
		int m; for (m = 0; m < MAXMUX && !(muxes[m].used && !muxes[m].closing && muxes[m].gport == ntohs(sa.sin_port)); m++) {}
		const int r = m < MAXMUX ? attach(A0, m) : 1;
		pthread_mutex_unlock(&sock_lock);
		if (r <= 0) { if (net_log()) fprintf(stderr, "    net: bind %u %s shared with p2p -> %d\n", A0, addr_str(&sa, s, sizeof s), r); RET(r ? fail_host() : 0); return; }
	}
	int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
	int r = bind(fd, (struct sockaddr *)&sa, sizeof sa); if (r < 0) SOCK_ERRNO();
	/* a UDP port another copy on this machine has: the next free one (getsockname tells the game; np_matching2's
	 * signaling binds a fixed STUN port, 10070, and reads the port back) */
	const uint16_t asked = ntohs(sa.sin_port);
	for (int k = 1; r < 0 && errno == EADDRINUSE && socks[A0].type == 2 && asked && k < 16; k++) {
		sa.sin_port = htons((uint16_t)(asked + k)); r = bind(fd, (struct sockaddr *)&sa, sizeof sa); if (r < 0) SOCK_ERRNO();
		if (!r) fprintf(stderr, "    net: port %u is in use here: %u instead\n", asked, asked + k);
	}
	if (net_log()) fprintf(stderr, "    net: bind %u %s -> %d\n", A0, addr_str(&sa, s, sizeof s), r < 0 ? errno : 0);
	RET(r < 0 ? fail_host() : 0);
}
/* A connection to a mapped address and port goes to another port instead: the local matching service's (net_portmap_add;
 * np_matching2 makes its TLS connection on 443, which a user can't listen on), or by hand RECOMP_NET_PORTMAP=
 * "<ipv4>:<port>=<port>,..." (a matching server of one's own on this machine) */
static struct { char ip[32]; unsigned from, to; } maps[8]; static int nmaps;
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
void net_portmap_add(const char *ip, unsigned from, unsigned to) {   /* (host/online: the local matching service's TLS port) */
	pthread_mutex_lock(&map_lock);
	if (nmaps < 8) { snprintf(maps[nmaps].ip, sizeof maps[nmaps].ip, "%s", ip); maps[nmaps].from = from; maps[nmaps].to = to; nmaps++; }
	pthread_mutex_unlock(&map_lock);
}
static void portmap(struct sockaddr_in *sa) {
	char ip[32]; inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip); const unsigned port = ntohs(sa->sin_port);
	pthread_mutex_lock(&map_lock);
	for (int i = 0; i < nmaps; i++) if (maps[i].from == port && !strcmp(maps[i].ip, ip)) { sa->sin_port = htons((uint16_t)maps[i].to); pthread_mutex_unlock(&map_lock); return; }
	pthread_mutex_unlock(&map_lock);
	const char *e = getenv("RECOMP_NET_PORTMAP"); if (!e) return;
	for (const char *p = e; *p; ) {
		char a[32]; unsigned from, to; int n = 0;
		if (sscanf(p, "%31[^:]:%u=%u%n", a, &from, &to, &n) == 3 && !strcmp(a, ip) && from == port) { sa->sin_port = htons((uint16_t)to); return; }
		p += strcspn(p, ","); if (*p) p++;
	}
}
static void h_net_connect(Cpu *c) {
	int fd = sock_fd(A0); if (fd < 0 || !A1) { RET(fail(9)); return; }
	struct sockaddr_in sa; to_host_addr(A1, &sa); char s[48];
	if (on_mux(A0)) { socks[A0].peer = sa; memcpy(&socks[A0].peer_vport, G(A1 + 8), 2); RET(0); return; }   /* (the address send uses) */
	if (ov_is_vip(sa.sin_addr.s_addr) && (socks[A0].type == 1 || socks[A0].type == 10)) {   /* another copy's: a stream over the overlay */
		const int nfd = ov_stream_open(sa.sin_addr.s_addr, ntohs(sa.sin_port));
		if (net_log()) fprintf(stderr, "    net: connect %u %s over the overlay -> %s\n", A0, addr_str(&sa, s, sizeof s), nfd < 0 ? "no link" : "0");
		if (nfd < 0) { RET(fail(61)); return; }   /* ECONNREFUSED */
		pthread_mutex_lock(&sock_lock); CLOSESOCK(socks[A0].fd); socks[A0].fd = nfd; pthread_mutex_unlock(&sock_lock);
		if (socks[A0].type == 10) { socks[A0].hello = 1; socks[A0].hello_ip = ov_self_vip(); stream_hello(A0); }
		if (socks[A0].nbio) {
#ifdef _WIN32
			u_long nb = 1; ioctlsocket(nfd, FIONBIO, &nb);
#else
			fcntl(nfd, F_SETFL, fcntl(nfd, F_GETFL, 0) | O_NONBLOCK);
#endif
		}
		RET(0); return;
	}
	portmap(&sa);
	int r = connect(fd, (struct sockaddr *)&sa, sizeof sa);
	if (r < 0) {
		SOCK_ERRNO();
#ifdef _WIN32
		if (errno == EAGAIN) errno = EINPROGRESS;   /* (a non-blocking connect under way: Winsock's WSAEWOULDBLOCK) */
#endif
	}
	if (socks[A0].type == 10) { const int e = errno; socks[A0].hello = 1; socks[A0].hello_ip = 0; stream_hello(A0); errno = e; }
	if (net_log()) { struct sockaddr_in la; socklen_t ll = sizeof la; memset(&la, 0, sizeof la); getsockname(fd, (struct sockaddr *)&la, &ll); fprintf(stderr, "    net: connect %u %s from port %u -> %d\n", A0, addr_str(&sa, s, sizeof s), ntohs(la.sin_port), r < 0 ? errno : 0); }
	RET(r < 0 ? fail_host() : 0);
}
static void h_net_listen(Cpu *c) {
	int fd = sock_fd(A0); if (fd < 0) { RET(fail(9)); return; }
	if (listen(fd, (int)A1) < 0) { SOCK_ERRNO(); RET(fail_host()); return; }
	if (socks[A0].type == 10 && fd_port(fd)) ov_allow_port(fd_port(fd));   /* (a P2P stream port: other copies reach it over the overlay too) */
	RET(0);
}
static void h_net_accept(Cpu *c) {
	int fd = sock_fd(A0); if (fd < 0) { RET(fail(9)); return; }
	struct sockaddr_in sa; socklen_t l = sizeof sa; uint16_t vport = 0; int nfd;
	if (socks[A0].type == 10) {   /* (staged until its preamble says which copy it is: above) */
		for (;;) {
			struct sockaddr_in f; socklen_t fl = sizeof f;
			while (wait_readable(fd, 0) > 0 && (nfd = (int)accept(fd, (struct sockaddr *)&f, &fl)) >= 0) {
				pthread_mutex_lock(&sock_lock);
				int i; for (i = 0; i < MAXSTAGE && staged[i].used; i++) {}
				if (i < MAXSTAGE) { staged[i].used = 1; staged[i].fd = nfd; staged[i].listener = A0; staged[i].since = stage_ms(); staged[i].from = f; } else CLOSESOCK(nfd);
				pthread_mutex_unlock(&sock_lock); fl = sizeof f;
			}
			pthread_mutex_lock(&sock_lock); nfd = stage_take(A0, &sa, &vport); pthread_mutex_unlock(&sock_lock);
			if (nfd >= 0) break;
			if (socks[A0].nbio) { RET(fail(35)); return; }   /* EAGAIN */
			wait_readable(fd, 50);
		}
		if (net_log()) { char s1[48]; fprintf(stderr, "    net: accept %u: %s, signaling port %u\n", A0, addr_str(&sa, s1, sizeof s1), ntohs(vport)); }
	} else {
		nfd = (int)accept(fd, (struct sockaddr *)&sa, &l);
		if (nfd < 0) { SOCK_ERRNO(); RET(fail_host()); return; }
	}
	pthread_mutex_lock(&sock_lock);
	int id; for (id = 1; id < MAXSOCK && socks[id].used; id++) {}
	if (id < MAXSOCK) { socks[id].used = 1; socks[id].fd = nfd; socks[id].type = socks[A0].type; socks[id].nbio = 0; socks[id].mux = -1; socks[id].vport = 0; socks[id].lport = 0; socks[id].hello = 0; }
	pthread_mutex_unlock(&sock_lock);
	if (id == MAXSOCK) { CLOSESOCK(nfd); RET(fail(24)); return; }
	to_guest_addr(&sa, A1, A2);
	if (A1 && vport) memcpy(G(A1 + 8), &vport, 2);   /* sin_vport: the peer's signaling port (a P2P stream's preamble) */
	RET(id);
}
static int host_flags(uint32_t f) { int h = 0; if (f & 0x2) h |= MSG_PEEK; if (f & 0x40) h |= MSG_WAITALL;
#ifdef MSG_DONTWAIT
	if (f & 0x80) h |= MSG_DONTWAIT;
#endif
#ifdef MSG_NOSIGNAL
	h |= MSG_NOSIGNAL;
#endif
	return h; }
static void h_net_send(Cpu *c) {
	int fd = sock_fd(A0); if (fd < 0) { RET(fail(9)); return; }
	if (socks[A0].type == 3) { RET(A2); return; }   /* (ICMP: sent, as far as anyone knows) */
	if (on_mux(A0)) { if (!socks[A0].peer.sin_port) { RET(fail(39)); return; } long n = mux_send(A0, G(A1), A2, &socks[A0].peer, socks[A0].peer_vport); RET(n < 0 ? fail_host() : (uint32_t)n); return; }
	if (socks[A0].hello && !stream_hello(A0)) { RET(fail(35)); return; }   /* (the preamble first: EAGAIN until it's out) */
	long n = (long)send(fd, (const char *)G(A1), A2, host_flags(A3)); if (n < 0) SOCK_ERRNO();
	if (net_log() > 2 && n > 0) { fprintf(stderr, "    net: send %u %ld bytes:", A0, n); for (long i = 0; i < n && i < 64; i++) fprintf(stderr, " %02x", ((const uint8_t *)G(A1))[i]); fprintf(stderr, "\n"); }
	RET(n < 0 ? fail_host() : (uint32_t)n);
}
static void h_net_sendto(Cpu *c) {   /* (s, buf, len, flags, to, tolen) */
	int fd = sock_fd(A0); if (fd < 0) { RET(fail(9)); return; }
	uint32_t to = STK(0); struct sockaddr_in sa; if (to) to_host_addr(to, &sa);
	if (socks[A0].type == 3) { RET(A2); return; }   /* (ICMP: sent, as far as anyone knows) */
	if (on_mux(A0)) {
		uint16_t v = socks[A0].peer_vport; if (to) memcpy(&v, G(to + 8), 2); else sa = socks[A0].peer;
		if (!sa.sin_port) { RET(fail(39)); return; }
		long n = mux_send(A0, G(A1), A2, &sa, v); RET(n < 0 ? fail_host() : (uint32_t)n); return;
	}
	long n = (long)sendto(fd, (const char *)G(A1), A2, host_flags(A3), to ? (struct sockaddr *)&sa : 0, to ? sizeof sa : 0); if (n < 0) SOCK_ERRNO();
	if (net_log() > 1 || (net_log() && n < 0)) { char s[48]; fprintf(stderr, "    net: sendto %u %u bytes %s -> %ld\n", A0, A2, to ? addr_str(&sa, s, sizeof s) : "-", n < 0 ? -(long)errno : n); }
	RET(n < 0 ? fail_host() : (uint32_t)n);
}
static void h_net_recv(Cpu *c) {
	int fd = sock_fd(A0); if (fd < 0) { RET(fail(9)); return; }
	if (on_mux(A0)) { long n = mux_recv(A0, A1, A2, host_flags(A3), 0, 0); RET(n < 0 ? fail_host() : (uint32_t)n); return; }
	if (socks[A0].hello) stream_hello(A0);
	long n = (long)recv(fd, (char *)G(A1), A2, host_flags(A3)); if (n < 0) SOCK_ERRNO();
	if (net_log() > 2 && n > 0) { fprintf(stderr, "    net: recv %u %ld bytes:", A0, n); for (long i = 0; i < n && i < 64; i++) fprintf(stderr, " %02x", ((const uint8_t *)G(A1))[i]); fprintf(stderr, "\n"); }
	if (!n && A2 && net_log()) {
		struct sockaddr_in la, pa; socklen_t ll = sizeof la, pl = sizeof pa; memset(&la, 0, sizeof la); memset(&pa, 0, sizeof pa);
		getsockname(fd, (struct sockaddr *)&la, &ll); getpeername(fd, (struct sockaddr *)&pa, &pl); char l1[48], p1[48];
		struct timespec tw; clock_gettime(CLOCK_REALTIME, &tw); fprintf(stderr, "    net: recv %u (fd %d, type %d, %s -> %s): end of stream at %ld.%06ld\n", A0, fd, socks[A0].type, addr_str(&la, l1, sizeof l1), addr_str(&pa, p1, sizeof p1), (long)tw.tv_sec, tw.tv_nsec / 1000);
	}
	RET(n < 0 ? fail_host() : (uint32_t)n);
}
static void h_net_recvfrom(Cpu *c) {   /* (s, buf, len, flags, from, fromlen) */
	int fd = sock_fd(A0); if (fd < 0) { RET(fail(9)); return; }
	if (on_mux(A0)) { long n = mux_recv(A0, A1, A2, host_flags(A3), STK(0), STK(1)); RET(n < 0 ? fail_host() : (uint32_t)n); return; }
	struct sockaddr_in sa; socklen_t l = sizeof sa; memset(&sa, 0, sizeof sa);
	long n = (long)recvfrom(fd, (char *)G(A1), A2, host_flags(A3), (struct sockaddr *)&sa, &l);
	if (n < 0) { SOCK_ERRNO(); RET(fail_host()); return; }
	to_guest_addr(&sa, STK(0), STK(1));
	RET((uint32_t)n);
}
/* options: the Vita's level/name to the host's; an option with no host counterpart is accepted and ignored */
static void h_net_setsockopt(Cpu *c) {   /* (s, level, optname, optval, optlen) */
	int fd = sock_fd(A0); if (fd < 0) { RET(fail(9)); return; }
	uint32_t level = A1, name = A2, val = A3 ? RD32(A3) : 0;
	int r = 0;
	if (level == 0xffff) {
		switch (name) {
		case 0x1100: {   /* SO_NBIO */
#ifdef _WIN32
			u_long nb = val != 0; r = ioctlsocket(fd, FIONBIO, &nb);
#else
			int fl = fcntl(fd, F_GETFL, 0); r = fcntl(fd, F_SETFL, val ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
			socks[A0].nbio = val != 0; break; }
		case 0x1005: case 0x1006: {   /* SO_SNDTIMEO / SO_RCVTIMEO: microseconds on the Vita */
#ifdef _WIN32
			DWORD ms = val / 1000; r = setsockopt(fd, SOL_SOCKET, name == 0x1005 ? SO_SNDTIMEO : SO_RCVTIMEO, (const char *)&ms, sizeof ms);
#else
			struct timeval tv = { (time_t)(val / 1000000), (suseconds_t)(val % 1000000) }; r = setsockopt(fd, SOL_SOCKET, name == 0x1005 ? SO_SNDTIMEO : SO_RCVTIMEO, &tv, sizeof tv);
#endif
			break; }
		case 0x4: r = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&val, sizeof val); break;
		case 0x8: r = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char *)&val, sizeof val); break;
		case 0x20: r = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char *)&val, sizeof val); break;
#ifdef SO_REUSEPORT
		case 0x200: r = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char *)&val, sizeof val); break;
#endif
		case 0x1001: r = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char *)&val, sizeof val); break;
		case 0x1002: r = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char *)&val, sizeof val); break;
		case 0x80: { struct linger lg = { (int)RD32(A3), (int)RD32(A3 + 4) }; r = setsockopt(fd, SOL_SOCKET, SO_LINGER, (const char *)&lg, sizeof lg); break; }
		default: break;
		}
	} else if (level == 6 && name == 1) r = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&val, sizeof val);
	else if (level == 0 && name == 4) r = setsockopt(fd, IPPROTO_IP, IP_TTL, (const char *)&val, sizeof val);
	else if (level == 0 && name == 3) r = setsockopt(fd, IPPROTO_IP, IP_TOS, (const char *)&val, sizeof val);
	if (r < 0) SOCK_ERRNO();
	if (net_log()) fprintf(stderr, "    net: setsockopt %u level %#x name %#x value %u -> %d\n", A0, level, name, val, r < 0 ? errno : 0);
	RET(r < 0 ? fail_host() : 0);
}
static void h_net_getsockopt(Cpu *c) {   /* (s, level, optname, optval, optlen) */
	int fd = sock_fd(A0); if (fd < 0) { RET(fail(9)); return; }
	uint32_t level = A1, name = A2, out = A3, lenp = STK(0); int v = 0; socklen_t l = sizeof v; int r = 0;
	if (level == 0xffff && name == 0x1007) {   /* SO_ERROR (on Windows a Winsock number) */
		r = getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&v, &l);
#ifdef _WIN32
		if (v) v = wsa_errno(v);
#endif
		v = vita_errno(v) * (v != 0); if (!r && !v && socks[A0].hello) stream_hello(A0);
	}
	else if (level == 0xffff && name == 0x1008) v = socks[A0].type;
	else if (level == 0xffff && name == 0x1100) v = socks[A0].nbio;
	else if (level == 0xffff && name == 0x1001) r = getsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char *)&v, &l);
	else if (level == 0xffff && name == 0x1002) r = getsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char *)&v, &l);
	if (r < 0) SOCK_ERRNO();
	if (out) WR32(out, (uint32_t)v);
	if (lenp) WR32(lenp, 4);
	RET(r < 0 ? fail_host() : 0);
}
static void h_net_htons(Cpu *c) { uint16_t v = (uint16_t)A0; RET((uint16_t)((v >> 8) | (v << 8))); }
static void h_net_htonl(Cpu *c) { uint32_t v = A0; RET((v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24)); }
static void h_net_inet_pton(Cpu *c) {   /* (af, src, dst): 1 parsed, 0 not */
	struct in_addr a; if (A0 != 2 || !A1 || !A2) { RET(0); return; }
	if (inet_pton(AF_INET, (const char *)G(A1), &a) != 1) { RET(0); return; }
	memcpy(G(A2), &a, 4); RET(1);
}
static void h_net_inet_ntop(Cpu *c) {   /* (af, src, dst, size): dst or 0 */
	char b[32]; if (A0 != 2 || !A1 || !A2 || !inet_ntop(AF_INET, G(A1), b, sizeof b) || strlen(b) + 1 > A3) { RET(0); return; }
	memcpy(G(A2), b, strlen(b) + 1); RET(A2);
}
/* sceNetGetSockInfo(s, SceNetSockInfo *, n, flags): a socket's record, 0x6c bytes: name[32], pid, s (+0x24), type, policy,
 * the receive queue's length (+0x2c: the next datagram's size, without the P2P layer's own header; a stream's bytes),
 * the send queue's, local and remote addresses (+0x34, +0x38), ports (+0x3c, +0x3e: network order), vports, state,
 * flags. Killzone sizes the buffer it reads a P2P datagram into by the queue length (stubbed, it read a record never
 * written and handed RUDP a null buffer). One socket only (s >= 0): its record and 1. */
static void h_net_get_sock_info(Cpu *c) {
	const int fd = sock_fd(A0);
	if ((int32_t)A0 < 0 || !A1 || !A2) { RET(0); return; }
	if (fd < 0) { RET(fail(9)); return; }
	uint8_t r[0x6c]; memset(r, 0, sizeof r);
	snprintf((char *)r, 32, "%s", socks[A0].name); const uint32_t s = A0; memcpy(r + 0x24, &s, 4); r[0x28] = (uint8_t)socks[A0].type;
	int32_t q = 0;
	if (socks[A0].type == 1 || socks[A0].type == 10) {
#ifdef _WIN32
		u_long avail = 0; ioctlsocket(fd, FIONREAD, &avail); q = (int32_t)avail;
#else
		int avail = 0; ioctl(fd, FIONREAD, &avail); q = avail;
#endif
	} else {   /* (a datagram's size: peeked) */
		static __thread uint8_t *pk; if (!pk) pk = malloc(P2P_DELIVER + 65536);
		const long n = wait_readable(fd, 0) > 0 ? (long)recv(fd, (char *)pk, P2P_DELIVER + 65536, MSG_PEEK) : 0;
		if (n > 0) q = (int32_t)(on_mux(A0) ? (n > P2P_DELIVER ? n - P2P_DELIVER : 0) : n);
	}
	memcpy(r + 0x2c, &q, 4);
	struct sockaddr_in la, pa; socklen_t ll = sizeof la, pl = sizeof pa; memset(&la, 0, sizeof la); memset(&pa, 0, sizeof pa);
	if (on_mux(A0)) {
		pthread_mutex_lock(&sock_lock); const int m = socks[A0].mux; if (m >= 0) la.sin_port = htons(muxes[m].hport); pa = socks[A0].peer; pthread_mutex_unlock(&sock_lock);
		memcpy(r + 0x40, &socks[A0].vport, 2); memcpy(r + 0x42, &socks[A0].peer_vport, 2);
	} else { getsockname(fd, (struct sockaddr *)&la, &ll); getpeername(fd, (struct sockaddr *)&pa, &pl); }
	memcpy(r + 0x34, &la.sin_addr, 4); memcpy(r + 0x38, &pa.sin_addr, 4); memcpy(r + 0x3c, &la.sin_port, 2); memcpy(r + 0x3e, &pa.sin_port, 2);
	memcpy(G(A1), r, sizeof r);
	RET(1);
}
static void h_net_getsockname(Cpu *c) {   /* (s, SceNetSockaddrIn *, *len): a P2P socket's real port (its mux's) and vport */
	int fd = sock_fd(A0); if (fd < 0) { RET(fail(9)); return; }
	if (!A1) { RET(fail(22)); return; }
	struct sockaddr_in sa; socklen_t l = sizeof sa; memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET;
	if (on_mux(A0)) {
		pthread_mutex_lock(&sock_lock); const int m = socks[A0].mux; if (m >= 0) sa.sin_port = htons(muxes[m].hport); const uint16_t v = socks[A0].vport; pthread_mutex_unlock(&sock_lock);
		to_guest_addr(&sa, A1, A2); memcpy(G(A1 + 8), &v, 2); RET(0); return;
	}
	if (getsockname(fd, (struct sockaddr *)&sa, &l)) { SOCK_ERRNO(); RET(fail_host()); return; }
	to_guest_addr(&sa, A1, A2); RET(0);
}

/* ---- resolver */
static void h_net_resolver_create(Cpu *c) { static uint32_t next = 1; RET(__atomic_fetch_add(&next, 1, __ATOMIC_RELAXED)); }
static void h_net_resolver_destroy(Cpu *c) { RET(0); }
static int resolve4(const char *host, struct in_addr *out) {
	struct addrinfo hints, *res = 0; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET;
	if (getaddrinfo(host, 0, &hints, &res) || !res) return -1;
	*out = ((struct sockaddr_in *)res->ai_addr)->sin_addr; freeaddrinfo(res); return 0;
}
static void h_net_resolver_ntoa(Cpu *c) {   /* (rid, hostname, SceNetInAddr *addr, timeout, retry, flags) */
	if (!A1 || !A2) { RET(0x804101E6u); return; }
	const char *host = (const char *)G(A1); char target[256]; int port = 0;
	const size_t hl = strlen(host);
	if (hl >= 20 && !strcasecmp(host + hl - 20, "stun.playstation.net") && matching_local()) {   /* (np_matching2's STUN, "usstun.playstation.net": online/stun.c) */
		stun_start(); const uint32_t lo = htonl(INADDR_LOOPBACK); memcpy(G(A2), &lo, 4);   /* (not a struct in_addr initializer: Windows' is a union whose first member is 4 bytes, and { x } made it 127.0.0.0) */
		if (net_log()) fprintf(stderr, "    net: resolve %s -> 127.0.0.1 (this machine's STUN)\n", host);
		RET(0); return;
	}
	if (net_blocked(host)) { RET(0x804101E6u); return; }
	const int redirected = net_redirect_lookup(host, target, sizeof target, &port);
	struct in_addr a;
	int r = resolve4(redirected ? target : host, &a);
	if (net_log() || redirected) { char ip[32] = "-"; if (!r) inet_ntop(AF_INET, &a, ip, sizeof ip); fprintf(stderr, "    net: resolve %s%s%s -> %s\n", host, redirected ? " (redirected to " : "", redirected ? target : "", r ? "no host" : ip); }
	if (r) { RET(0x804101E6u); return; }   /* SCE_NET_ERROR_RESOLVER_ENOHOST */
	memcpy(G(A2), &a, 4);
	RET(0);
}
static void h_net_resolver_aton(Cpu *c) { RET(0x804101E6u); }

/* ---- epoll over poll() */
#define MAXEPOLL 32
#define EPOLL_IDS 64
static struct { int used, n; struct { uint32_t id, events; uint8_t data[8]; } e[EPOLL_IDS]; } epolls[MAXEPOLL];
static pthread_mutex_t ep_lock = PTHREAD_MUTEX_INITIALIZER;
static void h_net_epoll_create(Cpu *c) {
	pthread_mutex_lock(&ep_lock);
	int id; for (id = 1; id < MAXEPOLL && epolls[id].used; id++) {}
	if (id < MAXEPOLL) { epolls[id].used = 1; epolls[id].n = 0; }
	pthread_mutex_unlock(&ep_lock);
	RET(id < MAXEPOLL ? (uint32_t)id : fail(24));
}
static void h_net_epoll_destroy(Cpu *c) { if (A0 < MAXEPOLL) { pthread_mutex_lock(&ep_lock); epolls[A0].used = 0; pthread_mutex_unlock(&ep_lock); } RET(0); }
static void h_net_epoll_control(Cpu *c) {   /* (eid, op, id, event) */
	if (A0 >= MAXEPOLL || !epolls[A0].used) { RET(fail(9)); return; }
	pthread_mutex_lock(&ep_lock);
	__typeof__(epolls[0]) *ep = &epolls[A0]; int k; for (k = 0; k < ep->n && ep->e[k].id != A2; k++) {}
	uint32_t r = 0;
	if (A1 == 1 || A1 == 2) {   /* ADD, MOD */
		if (k == ep->n) { if (A1 == 2 || ep->n == EPOLL_IDS) r = fail(A1 == 2 ? 2 : 12); else ep->n++; }
		if (!r) { ep->e[k].id = A2; ep->e[k].events = A3 ? RD32(A3) : 0; if (A3) memcpy(ep->e[k].data, G(A3 + 24), 8); else memset(ep->e[k].data, 0, 8); }
	} else if (A1 == 3) { if (k < ep->n) ep->e[k] = ep->e[--ep->n]; else r = fail(2); }
	pthread_mutex_unlock(&ep_lock);
	RET(r);
}
static void h_net_epoll_wait(Cpu *c) {   /* (eid, events, maxevents, timeout us) */
	if (A0 >= MAXEPOLL || !epolls[A0].used) { RET(fail(9)); return; }
	struct pollfd pf[EPOLL_IDS]; uint32_t ids[EPOLL_IDS]; uint8_t data[EPOLL_IDS][8]; int n = 0;
	pthread_mutex_lock(&ep_lock);
	for (int k = 0; k < epolls[A0].n; k++) {
		int fd = sock_fd(epolls[A0].e[k].id); if (fd < 0) continue;
		pf[n].fd = fd; pf[n].events = (short)(((epolls[A0].e[k].events & 1) ? POLLIN : 0) | ((epolls[A0].e[k].events & 2) ? POLLOUT : 0)); pf[n].revents = 0;
		ids[n] = epolls[A0].e[k].id; memcpy(data[n], epolls[A0].e[k].data, 8); n++;
	}
	pthread_mutex_unlock(&ep_lock);
	int tmo = (int32_t)A3 < 0 ? -1 : (int)((A3 + 999) / 1000);
#ifdef _WIN32
	int r = n ? WSAPoll(pf, (ULONG)n, tmo) : (Sleep(tmo < 0 ? 1000 : tmo), 0);
#else
	int r = poll(pf, (nfds_t)n, tmo);
#endif
	if (r < 0) { SOCK_ERRNO(); RET(fail_host()); return; }
	int out = 0;
	for (int k = 0; k < n && out < (int)A2; k++) {
		if (!pf[k].revents) continue;
		uint32_t ev = ((pf[k].revents & POLLIN) ? 1u : 0) | ((pf[k].revents & POLLOUT) ? 2u : 0) | ((pf[k].revents & POLLERR) ? 8u : 0) | ((pf[k].revents & POLLHUP) ? 0x10u : 0);
		uint32_t at = A1 + 0x20u * (uint32_t)out; memset(G(at), 0, 0x20); WR32(at, ev); memcpy(G(at + 24), data[k], 8);
		out++;
	}
	(void)ids;
	RET((uint32_t)out);
}
static void h_net_epoll_abort(Cpu *c) { RET(0); }

/* ---- netctl: connected, with the host's own address */
static void host_ipv4(char *ip, char *mask, size_t n) {
	snprintf(ip, n, "127.0.0.1"); snprintf(mask, n, "255.0.0.0");
#ifndef _WIN32
	struct ifaddrs *ifs = 0;
	if (getifaddrs(&ifs)) return;
	int best = 0;   /* a home or office network's address (192.168/16, 10/8, 172.16/12) before any other: a VPN's (Tailscale's
	                 * 100.64/10) reaches only its own peers */
	for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
		const uint32_t a = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
		const int rank = (a >> 16) == 0xc0a8 || (a >> 24) == 10 || (a >> 20) == 0xac1 ? 3 : (a >> 16) == 0xa9fe ? 1 : 2;
		if (rank <= best) continue;
		best = rank;
		inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, (socklen_t)n);
		if (i->ifa_netmask) inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_netmask)->sin_addr, mask, (socklen_t)n);
	}
	freeifaddrs(ifs);
#endif
}
/* the address the game is told it has: with the overlay on, this copy's virtual one (what other copies reach it at) */
static void game_ipv4(char *ip, char *mask, size_t n) {
	host_ipv4(ip, mask, n);
	if (ov_on()) { const uint32_t v = ov_self_vip(); inet_ntop(AF_INET, &v, ip, (socklen_t)n); snprintf(mask, n, "255.254.0.0"); }
}
void net_host_ipv4(char *ip, size_t n) { char mask[32]; game_ipv4(ip, mask, n); }
static void h_netctl_init(Cpu *c) { RET(0); }
static void h_netctl_term(Cpu *c) { RET(0); }
static void h_netctl_state(Cpu *c) { if (A0) WR32(A0, 3); RET(0); }   /* SCE_NETCTL_STATE_CONNECTED */
static void h_netctl_info(Cpu *c) {   /* (code, SceNetCtlInfo *): a 0x100-byte union */
	if (!A1) { RET(0x80412102u); return; }
	uint8_t *o = G(A1); memset(o, 0, 0x100);
	char ip[32], mask[32]; game_ipv4(ip, mask, sizeof ip);
	switch (A0) {
	case 1: strcpy((char *)o, "Nativ"); break;                       /* CNF_NAME */
	case 2: break;                                                    /* DEVICE: wireless */
	case 3: { static const uint8_t mac[6] = { 0x02, 0x41, 0x54, 0x49, 0x56, 0x01 }; memcpy(o, mac, 6); break; }   /* ETHER_ADDR (locally administered) */
	case 4: WR32(A1, 1500); break;                                    /* MTU */
	case 5: WR32(A1, 1); break;                                       /* LINK: up */
	case 7: strcpy((char *)o, "Nativ"); break;                        /* SSID */
	case 8: WR32(A1, 2); break;                                       /* WIFI_SECURITY: WPA-PSK */
	case 10: WR32(A1, 100); break;                                    /* RSSI_PERCENTAGE */
	case 11: WR32(A1, 6); break;                                      /* CHANNEL */
	case 15: strcpy((char *)o, ip); break;                            /* IP_ADDRESS */
	case 16: strcpy((char *)o, mask); break;                          /* NETMASK */
	case 17: case 18: { char gw[32]; snprintf(gw, sizeof gw, "%s", ip); char *d = strrchr(gw, '.'); if (d) strcpy(d, ".1"); strcpy((char *)o, gw); break; }   /* DEFAULT_ROUTE, PRIMARY_DNS */
	case 19: strcpy((char *)o, "0.0.0.0"); break;                     /* SECONDARY_DNS */
	default: break;
	}
	RET(0);
}
static void h_netctl_nat(Cpu *c) {   /* (SceNetCtlNatInfo *): {size, stun_status, nat_type, mapped_addr} */
	if (!A0) { RET(0x80412102u); return; }
	char ip[32], mask[32]; game_ipv4(ip, mask, sizeof ip); struct in_addr a; inet_pton(AF_INET, ip, &a);
	WR32(A0 + 4, 1); WR32(A0 + 8, 1); memcpy(G(A0 + 12), &a, 4);   /* (type 1, open: np_matching2's signaling then offers its
	                                                                   * peers its addresses at once; type 2 waits on STUN keepalives) */
	RET(0);
}
static void h_netctl_register(Cpu *c) { if (A2) WR32(A2, 1); RET(0); }   /* (func, arg, *cid): the state never changes, so it never runs */
static void h_netctl_zero(Cpu *c) { RET(0); }
static void h_netctl_adhoc_state(Cpu *c) { if (A0) WR32(A0, 0); RET(0); }
/* SceNetCtl_E8E8DF18(int *state): np_matching2's signaling waits for 4 before it starts STUN (with sceNetCtlGetNatInfo's
 * STUN status set) -- connected, as far as it is concerned */
static void h_netctl_if_state(Cpu *c) { if (A0) WR32(A0, 4); RET(0); }

/* SceNetInternal_689B9D7D(uint32_t *out): a random 32-bit number (libnet's own generator; np_matching2 seeds its connection
 * and server ids with it) */
static void h_net_random32(Cpu *c) { if (!A0) { RET(0x80410116u); return; } uint32_t v; plat_random(&v, 4); WR32(A0, v); RET(0); }
/* SceNetInternal_333F9CB3(s, cmd, arg): the kernel's UDPP2P crypto associations, set up by np_matching2's signaling ('P'
 * ioctls on its P2P sockets: 0x802450c8 flush, 0xc02450ca the local key (16 bytes at +0x10, sent to peers), 0x802450cb add a
 * peer, 0x802450cc remove one, 0x800250cf bind the echo socket). Nativ-to-Nativ P2P traffic is not encrypted: all succeed. */
static void h_net_internal_ioctl(Cpu *c) { if (A1 == 0xc02450cau && A2) plat_random(G(A2 + 0x10), 16); RET(0); }
/* sceNetHtonll / sceNetNtohll(u64): the 64-bit swap, in r0:r1 (np_matching2 reads a room's password slot mask and a
 * member's join date with it: the stub's error code as the mask made every slot private, so no room could be joined) */
static void h_net_swap64(Cpu *c) { const uint32_t lo = c->r[0], hi = c->r[1]; c->r[0] = __builtin_bswap32(hi); c->r[1] = __builtin_bswap32(lo); }
const HleEntry hle_net[] = {
	{"SceNetInternal_689B9D7D", h_net_random32}, {"SceNetInternal_333F9CB3", h_net_internal_ioctl},
	{"sceNetHtonll", h_net_swap64}, {"sceNetNtohll", h_net_swap64},
	{"sceNetInit", h_net_init}, {"sceNetTerm", h_net_term}, {"sceNetErrnoLoc", h_net_errno_loc},
	{"sceNetSocket", h_net_socket}, {"sceNetSocketClose", h_net_close}, {"sceNetBind", h_net_bind}, {"sceNetConnect", h_net_connect},
	{"sceNetListen", h_net_listen}, {"sceNetAccept", h_net_accept}, {"sceNetSend", h_net_send}, {"sceNetSendto", h_net_sendto},
	{"sceNetRecv", h_net_recv}, {"sceNetRecvfrom", h_net_recvfrom}, {"sceNetSetsockopt", h_net_setsockopt}, {"sceNetGetsockopt", h_net_getsockopt},
	{"sceNetHtons", h_net_htons}, {"sceNetNtohs", h_net_htons}, {"sceNetHtonl", h_net_htonl}, {"sceNetNtohl", h_net_htonl},
	{"sceNetInetPton", h_net_inet_pton}, {"sceNetInetNtop", h_net_inet_ntop}, {"sceNetGetSockInfo", h_net_get_sock_info}, {"sceNetGetsockname", h_net_getsockname},
	{"sceNetResolverCreate", h_net_resolver_create}, {"sceNetResolverDestroy", h_net_resolver_destroy}, {"sceNetResolverStartNtoa", h_net_resolver_ntoa},
	{"sceNetResolverStartAton", h_net_resolver_aton}, {"sceNetResolverAbort", h_netctl_zero},
	{"sceNetEpollCreate", h_net_epoll_create}, {"sceNetEpollDestroy", h_net_epoll_destroy}, {"sceNetEpollControl", h_net_epoll_control},
	{"sceNetEpollWait", h_net_epoll_wait}, {"sceNetEpollAbort", h_net_epoll_abort},
	{"sceNetCtlInit", h_netctl_init}, {"sceNetCtlTerm", h_netctl_term}, {"sceNetCtlInetGetState", h_netctl_state}, {"sceNetCtlInetGetInfo", h_netctl_info},
	{"sceNetCtlGetNatInfo", h_netctl_nat}, {"sceNetCtlInetRegisterCallback", h_netctl_register}, {"sceNetCtlInetUnregisterCallback", h_netctl_zero},
	{"sceNetCtlCheckCallback", h_netctl_zero}, {"sceNetCtlAdhocRegisterCallback", h_netctl_register}, {"sceNetCtlAdhocUnregisterCallback", h_netctl_zero},
	{"sceNetCtlAdhocGetState", h_netctl_adhoc_state}, {"SceNetCtl_E8E8DF18", h_netctl_if_state},
	{0, 0},
};
