/* NP Basic's in-game data messages between players (np.c's sceNpBasicSendInGameDataMessage and the handler the game
 * registers for them): a game sends up to 512 bytes to a player by NpId. Killzone's matchmaking asks a room's owner to
 * let it in this way before it joins the room, and the owner answers the same way. Each copy has a UDP socket for them;
 * discovery's adverts carry its port and the copy's player, so a message goes straight to the copy whose player it is
 * for. Our own format, nothing of Sony's:
 *   "NVDM", u8 version 1, the online id it is for (16), the sender's NpId (36), u16 BE size, the data
 * What arrives waits in a queue for the game's next sceNpBasicCheckCallback. With the overlay on, a player's address is
 * its copy's virtual one, and the message travels over the link instead. */
#include <pthread.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include "m2_internal.h"
#include "overlay.h"

void np_online_id(char out[17]);   /* (../np.c: this copy's player) */

enum { HEAD = 4 + 1 + 16 + 36 + 2, MAXDATA = 512, QUEUE = 16 };
static int sock = -1, port;
static struct { uint8_t from[36]; uint16_t n; uint8_t data[MAXDATA]; } queue[QUEUE];
static int q_head, q_count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void bmsg_input(const uint8_t *b, size_t n) {
	char me[17]; np_online_id(me);
	if (n < HEAD || memcmp(b, "NVDM", 4) || b[4] != 1) return;
	const size_t size = (size_t)(b[HEAD - 2] << 8 | b[HEAD - 1]);
	if (strncmp((const char *)b + 5, me, 16) || size > MAXDATA || HEAD + size > n) return;
	pthread_mutex_lock(&lock);
	if (q_count < QUEUE) {
		const int i = (q_head + q_count++) % QUEUE;
		memcpy(queue[i].from, b + 21, 36); queue[i].n = (uint16_t)size; memcpy(queue[i].data, b + HEAD, size);
	}
	pthread_mutex_unlock(&lock);
	fprintf(stderr, "    m2: in-game message of %zu bytes from %.16s\n", size, (const char *)b + 21);
}
static void *rx_main(void *arg) {
	(void)arg; uint8_t b[HEAD + MAXDATA];
	for (;;) { const int n = (int)recvfrom(sock, (char *)b, sizeof b, 0, 0, 0); if (n > 0) bmsg_input(b, (size_t)n); }
	return 0;
}
int bmsg_start(void) {
	if (sock >= 0) return port;
	sock = (int)socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) return 0;
	struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(ov_on() ? INADDR_LOOPBACK : INADDR_ANY);   /* (over the overlay, messages come through it: no port open to the network) */
	socklen_t l = sizeof a;
	if (bind(sock, (struct sockaddr *)&a, sizeof a) || getsockname(sock, (struct sockaddr *)&a, &l)) { fprintf(stderr, "    m2: no socket for in-game messages\n"); return 0; }
	port = ntohs(a.sin_port);
	pthread_t t; pthread_create(&t, 0, rx_main, 0); pthread_detach(t);
	return port;
}
int bmsg_send(const char to[16], const uint8_t from[36], const uint8_t *data, size_t n) {
	char host[64]; uint16_t p;
	if (sock < 0 || n > MAXDATA || disc_player(to, host, &p)) return -1;
	uint8_t b[HEAD + MAXDATA];
	memcpy(b, "NVDM", 4); b[4] = 1; memset(b + 5, 0, 16); memcpy(b + 5, to, strnlen(to, 16)); memcpy(b + 21, from, 36);
	b[HEAD - 2] = (uint8_t)(n >> 8); b[HEAD - 1] = (uint8_t)n; memcpy(b + HEAD, data, n);
	struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(p);
	if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return -1;
	if (ov_is_vip(a.sin_addr.s_addr)) return ov_send_basic(a.sin_addr.s_addr, b, HEAD + n);
	return sendto(sock, (const char *)b, (int)(HEAD + n), 0, (struct sockaddr *)&a, sizeof a) == (int)(HEAD + n) ? 0 : -1;
}
int bmsg_poll(uint8_t from[36], uint8_t *data, size_t cap) {
	pthread_mutex_lock(&lock);
	int n = -1;
	if (q_count) {
		const int i = q_head; q_head = (q_head + 1) % QUEUE; q_count--;
		memcpy(from, queue[i].from, 36); n = queue[i].n < cap ? queue[i].n : (int)cap; memcpy(data, queue[i].data, (size_t)n);
	}
	pthread_mutex_unlock(&lock);
	return n;
}
