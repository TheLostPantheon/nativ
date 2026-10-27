/* The STUN responder (stun.c answer): binding requests, from anything that reaches its ports. */
#include "../../runtime/net/online/stun.c"
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
	if (!n) return 0;
	struct sockaddr_in from; memset(&from, 0, sizeof from); from.sin_family = AF_INET; from.sin_addr.s_addr = htonl(0x0a000001); from.sin_port = htons(10070);
	answer(d[0] & 1, d + 1, n - 1, from);
	return 0;
}
