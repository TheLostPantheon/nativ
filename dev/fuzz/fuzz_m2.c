/* The matching service (m2_server.c, m2_rooms.c, m2_wire.c): room and agent requests (a run of messages, each framed by
 * its own length, so rooms are made and joined within one input), a proxy's own frames, and room snapshots. The first
 * byte picks which. */
#include <string.h>
#include "m2_internal.h"
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
	if (n < 2) return 0;
	const int mode = d[0] % 4; d++; n--;
	if (mode == 3) { m2_replica_update(d, n); return 0; }
	if (mode == 2) { m2_room_link(-1, d, n); return 0; }
	static Session se;
	for (size_t at = 0; at + 4 <= n; ) {
		const size_t len = (size_t)(d[at + 2] << 8 | d[at + 3]);
		if (len < 4 || at + len > n) break;
		M2Head h;
		if (!m2_parse_head(d + at, len, &h)) { if (mode == 0) m2_room_op(-1, d + at, len, &h); else m2_agent_room_op(-1, d + at, len, &h, &se); }
		at += len;
	}
	return 0;
}
