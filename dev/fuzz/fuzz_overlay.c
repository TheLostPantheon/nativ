/* The overlay's packets (overlay.c input), as a stranger on the internet sends them. The first byte picks how the rest
 * arrives: even, as it is (hellos, acks, anything unsealed); odd, sealed by a linked copy with the session the harness
 * gave it (byte 2 its type, the rest its payload: what lies behind the seal -- keepalives, datagrams, discovery,
 * in-game messages, streams). */
#include "../../runtime/net/online/overlay.c"
static uint8_t key[32];   /* (the linked copy's send key: this copy's receive key) */
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
	static int init; static uint64_t ctr;
	if (!init) {
		init = 1; memcpy(comm, "NPWR02882_00", 12); ov_self_id(); memset(key, 0x42, 32);
		pthread_mutex_lock(&lock);
		Peer *p = peer_new(0x2223); p->cur.on = 1; memcpy(p->cur.rx, key, 32); memset(p->cur.tx, 0x24, 32); p->up = 1;
		p->at.sin_family = AF_INET; p->at.sin_port = htons(36581);
		pthread_mutex_unlock(&lock);
	}
	struct sockaddr_in from; memset(&from, 0, sizeof from); from.sin_family = AF_INET; from.sin_addr.s_addr = htonl(0x0a000001); from.sin_port = htons(36581);
	if (!n) return 0;
	if (d[0] & 1) {
		if (n < 2 || n - 2 > MAXPKT - HDR - SEAL) return 0;
		uint8_t b[MAXPKT]; memcpy(b, "NVL2", 4); b[4] = d[1]; b[5] = 0; w64(b + 6, 0x2223); w64(b + 14, self);
		const uint64_t c = ++ctr; w64(b + HDR, c);
		oc_seal(key, c, b, HDR + 8, d + 2, n - 2, b + HDR + 8, b + HDR + 8 + (n - 2));
		input(b, HDR + SEAL + (n - 2), &from);
	} else input(d + 1, n - 1, &from);
	return 0;
}
