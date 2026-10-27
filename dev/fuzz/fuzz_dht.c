/* The DHT's replies and queries (dht.c dht_input: bencoded dictionaries from any node on the internet). */
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
void dht_start(const char comm[12], uint16_t port);
void dht_input(const uint8_t *b, size_t n, const struct sockaddr_in *from);
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
	static int init; if (!init) { init = 1; dht_start("NPWR02882_00", 36581); }
	struct sockaddr_in from; memset(&from, 0, sizeof from); from.sin_family = AF_INET; from.sin_addr.s_addr = htonl(0x0a000001); from.sin_port = htons(6881);
	dht_input(d, n, &from);
	return 0;
}
