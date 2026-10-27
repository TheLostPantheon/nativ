/* Discovery's adverts and queries (discovery.c heard), from the LAN or a linked copy (the first byte picks which). */
#include "../../runtime/net/online/discovery.c"
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
	static int init; if (!init) { init = 1; memcpy(comm, "NPWR02882_00", 12); self = 0x1111; snprintf(player, sizeof player, "ntv_me"); }
	if (!n) return 0;
	struct sockaddr_in from; memset(&from, 0, sizeof from); from.sin_family = AF_INET; from.sin_addr.s_addr = htonl(0x0a000001); from.sin_port = htons(36580);
	heard(d + 1, n - 1, &from, d[0] & 1);
	return 0;
}
