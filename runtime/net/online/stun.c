/* STUN for np_matching2's signaling (ONLINE.md): before it connects peers, the module asks a STUN server
 * ("usstun.playstation.net", UDP 3478; Sony's is never contacted) for the address its datagrams come from, and runs the
 * classic RFC 3489 NAT tests. With the matching service on this machine, net.c resolves that name to loopback, and this
 * answers on 3478 and 3479, all interfaces: a Binding Response (0x0101, the request's 16-byte transaction id) with
 * MAPPED-ADDRESS (where the request came from; from loopback, the address the game is told it has: what its peers reach
 * it at, its virtual one with the overlay on), SOURCE-ADDRESS and CHANGED-ADDRESS (127.0.0.1 and the other port: the
 * module's tests send there too), all three of which the module requires. A CHANGE-REQUEST (answer from another IP or port) is answered from the other port: one address is
 * all there is, and an answer makes the module rate the network open, as a LAN is (no answer: it never finishes). On
 * the internet (milestone 3) a public STUN server takes this one's place. */
#include <pthread.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSESOCK close
#endif
#include "m2_internal.h"

void net_host_ipv4(char *ip, size_t n);   /* (../net.c: this machine's network address) */

enum { STUN_PORT = 3478 };
static int socks[2] = { -1, -1 };   /* on 3478 and 3479 */

static uint8_t *attr_addr(uint8_t *p, uint16_t type, const struct sockaddr_in *a) {
	p[0] = (uint8_t)(type >> 8); p[1] = (uint8_t)type; p[2] = 0; p[3] = 8;
	p[4] = 0; p[5] = 1; memcpy(p + 6, &a->sin_port, 2); memcpy(p + 8, &a->sin_addr, 4);
	return p + 12;
}
static void answer(int k, const uint8_t *q, size_t n, struct sockaddr_in from) {
	if (n < 20 || q[0] != 0x00 || q[1] != 0x01) return;   /* Binding Request */
	const size_t len = (size_t)(q[2] << 8 | q[3]);
	if (20 + len > n) return;
	uint32_t change = 0; struct sockaddr_in reply_to = from;
	for (size_t at = 20; at + 4 <= 20 + len; ) {
		const uint16_t t = (uint16_t)(q[at] << 8 | q[at + 1]), l = (uint16_t)(q[at + 2] << 8 | q[at + 3]);
		const uint8_t *v = q + at + 4;
		if (at + 4 + l > 20 + len) break;
		if (t == 0x0003 && l == 4) change = (uint32_t)v[3];   /* CHANGE-REQUEST: 4 another IP, 2 another port */
		if (t == 0x0002 && l == 8 && v[1] == 1) { memcpy(&reply_to.sin_port, v + 2, 2); memcpy(&reply_to.sin_addr, v + 4, 4); }   /* RESPONSE-ADDRESS */
		at += 4 + ((l + 3u) & ~3u);
	}
	const int out = (change & 6) ? !k : k;
	struct sockaddr_in mapped = from;
	if (ntohl(from.sin_addr.s_addr) >> 24 == 127) { char ip[64]; net_host_ipv4(ip, sizeof ip); inet_pton(AF_INET, ip, &mapped.sin_addr); }
	struct sockaddr_in self, other; memset(&self, 0, sizeof self); self.sin_family = AF_INET;
	self.sin_addr.s_addr = htonl(INADDR_LOOPBACK); self.sin_port = htons((uint16_t)(STUN_PORT + out)); other = self; other.sin_port = htons((uint16_t)(STUN_PORT + !out));   /* (the server's own: the module's tests to them stay on this machine) */
	uint8_t r[20 + 3 * 12], *p = r + 20;
	p = attr_addr(p, 0x0001, &mapped); p = attr_addr(p, 0x0004, &self); p = attr_addr(p, 0x0005, &other);
	r[0] = 0x01; r[1] = 0x01; r[2] = 0; r[3] = (uint8_t)(p - r - 20); memcpy(r + 4, q + 4, 16);
	sendto(socks[out], (const char *)r, (int)(p - r), 0, (struct sockaddr *)&reply_to, sizeof reply_to);
	if (m2_log()) { char ip[32]; inet_ntop(AF_INET, &mapped.sin_addr, ip, sizeof ip); fprintf(stderr, "    m2: stun: %s:%u%s\n", ip, ntohs(mapped.sin_port), change ? " (another port)" : ""); }
}
static void *stun_main(void *arg) {
	const int k = (int)(intptr_t)arg; uint8_t b[1500];
	for (;;) {
		struct sockaddr_in from; socklen_t l = sizeof from;
		const int n = (int)recvfrom(socks[k], (char *)b, sizeof b, 0, (struct sockaddr *)&from, &l);
		if (n > 0) answer(k, b, (size_t)n, from);
	}
	return 0;
}
/* Starts it on first use: 0, or -1 when another copy on this machine already answers (it does for this one too) */
int stun_start(void) {
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; static int state;   /* 0 not tried, 1 running, -1 another copy's */
	pthread_mutex_lock(&lock);
	if (!state) {
		state = -1;
		for (int k = 0; k < 2; k++) {
			socks[k] = (int)socket(AF_INET, SOCK_DGRAM, 0);
			struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)(STUN_PORT + k)); a.sin_addr.s_addr = htonl(INADDR_ANY);
			if (socks[k] < 0 || bind(socks[k], (struct sockaddr *)&a, sizeof a)) break;
			if (k == 1) state = 1;
		}
		for (int k = 0; k < 2; k++)
			if (state == 1) { pthread_t t; pthread_create(&t, 0, stun_main, (void *)(intptr_t)k); pthread_detach(t); }
			else if (socks[k] >= 0) { CLOSESOCK(socks[k]); socks[k] = -1; }
		fprintf(stderr, "    m2: stun on port %d%s\n", STUN_PORT, state == 1 ? "" : ": another copy has it");
	}
	const int r = state > 0 ? 0 : -1;
	pthread_mutex_unlock(&lock);
	return r;
}
