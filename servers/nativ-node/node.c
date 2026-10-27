/*
 * nativ-node: a headless member of Project Nativ's online overlay that helps copies meet and relays for them
 * (docs/online.md). It runs no game: the overlay, the DHT and its own key, from the same sources as the runtime
 * (runtime/net/online/overlay.c, crypto.c, dht.c). Copies link to it like to any copy (a sealed handshake; it takes every
 * game's), and it tells each the others of its game with their public addresses as it sees them, so they can say hello
 * to each other straight (hole punching, coordinated); two copies that still can't reach each other (NATs that won't let
 * them, both sides) play through it: each wraps its packet for the other, already sealed end to end, in one sealed to
 * the node, which passes it on. It sees who talks to whom and how much, never what. At most 512 KB a second from one copy.
 * Copies find it by the settings online_relays (<host>[:<port>]), or by the DHT: it announces itself there under its
 * game (--game, Killzone: Mercenary's by default) as a copy would; copies of other games reach it by the setting.
 *
 *   nativ-node [--port 36590] [--state DIR] [--game NPWR02882_00] [--log 0|1|2]
 * --state: where its key is kept (online_key; default ~/.local/share/nativ-node): the same id from one run to the next.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "overlay.h"

static int port = 36590, loglevel = 1; static char state[512];
/* what the overlay, its crypto and the DHT ask of their host */
int settings_int(const char *key, int def) { return !strcmp(key, "online_port") ? port : def; }
const char *settings_str(const char *key, const char *def, char *buf, int n) { (void)key; snprintf(buf, (size_t)n, "%s", def ? def : ""); return buf; }
int settings_shared_dir(char *buf, int n) { return snprintf(buf, (size_t)n, "%s/", state) < n; }
void plat_random(void *buf, size_t n) {
	FILE *f = fopen("/dev/urandom", "rb"); size_t got = f ? fread(buf, 1, n, f) : 0; if (f) fclose(f);
	if (got != n) { fprintf(stderr, "nativ-node: no /dev/urandom\n"); exit(1); }
}
void plat_sleep_ns(uint64_t ns) { struct timespec t = { (time_t)(ns / 1000000000u), (long)(ns % 1000000000u) }; nanosleep(&t, 0); }
int m2_log(void) { return loglevel - 1 > 0 ? loglevel - 1 : 0; }
void disc_input(const uint8_t *b, size_t n, uint32_t vip) { (void)b; (void)n; (void)vip; }   /* (no game: no rooms to hear of) */
void bmsg_input(const uint8_t *b, size_t n) { (void)b; (void)n; }
void upnp_start(uint16_t p) { (void)p; }

int main(int argc, char **argv) {
	const char *game = "NPWR02882_00";   /* (Killzone: Mercenary's) */
	const char *home = getenv("HOME"); snprintf(state, sizeof state, "%s/.local/share/nativ-node", home ? home : ".");
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--state") && i + 1 < argc) snprintf(state, sizeof state, "%s", argv[++i]);
		else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
		else if (!strcmp(argv[i], "--log") && i + 1 < argc) loglevel = atoi(argv[++i]);
		else { fprintf(stderr, "usage: nativ-node [--port 36590] [--state DIR] [--game NPWR02882_00] [--log 0|1|2]\n"); return 2; }
	}
	char made[600]; snprintf(made, sizeof made, "%s", state);
	for (char *p = made + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(made, 0700); *p = '/'; }
	mkdir(made, 0700);
	char comm[12] = {0}; memcpy(comm, game, strnlen(game, 12));
	ov_relay(1);
	const int p = ov_start(comm);
	if (!p) { fprintf(stderr, "nativ-node: no port\n"); return 1; }
	fprintf(stderr, "nativ-node on UDP port %d, copy id %016llx, its key in %s\n", p, (unsigned long long)ov_self_id(), state);
	for (;;) {
		sleep(60);
		int linked, known; ov_counts(&linked, &known);
		if (loglevel) { time_t t = time(0); char ts[32]; strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t)); fprintf(stderr, "%s: %d copies linked (%d known), %.1f MB relayed\n", ts, linked, known, ov_relayed() / 1048576.0); }
	}
}
