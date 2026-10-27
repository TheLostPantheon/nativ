/* status.c: the online service at a glance (m2.h online_status) -- the F10 menu's Online rows, and online_status.txt
 * beside the shared settings, rewritten every 5 s while a game plays online, for the launcher ("online now" while its time
 * is fresh, else "last online"): time=<unix s>, game, name, linked, known, dht_nodes, public, rooms_heard, rooms_mine,
 * clash. A headless run (no shared settings) writes nothing.
 * The counts are gathered once a second on discovery's thread (online_status_tick), under the network's own locks, and
 * kept here; the menu only copies what was kept, under a lock of its own. It took the DHT's lock itself before
 * (2026-10-25), and when a DHT thread hung holding it, opening the Game page froze the whole game. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "m2_internal.h"
#include "overlay.h"

void np_online_id(char out[17]);
int settings_shared_dir(char *buf, int n);

static pthread_mutex_t kept_m = PTHREAD_MUTEX_INITIALIZER;
static OnlineStatus kept;   /* (the last gathered; started 0 until the first) */

static void gather(OnlineStatus *s) {   /* (discovery's thread: the network's locks, which may be slow) */
	memset(s, 0, sizeof *s);
	s->started = disc_status(&s->rooms_heard);
	s->port = ov_on() ? ov_counts(&s->linked, &s->known) : 0;
	s->dht_nodes = dht_counts(s->pub);
	RoomAd ads[8]; s->rooms_mine = s->started ? m2_rooms_snapshot(ads, 8) : 0;
	s->clash = disc_name_clash();
	np_online_id(s->name);
}
void online_status(OnlineStatus *s) {   /* (any thread, the menu's: never waits on the network) */
	pthread_mutex_lock(&kept_m); *s = kept; pthread_mutex_unlock(&kept_m);
}
void online_status_tick(void) {
	OnlineStatus s; gather(&s);
	pthread_mutex_lock(&kept_m); kept = s; pthread_mutex_unlock(&kept_m);
	static time_t last; const time_t now = time(0);
	if (now - last < 5) return;
	last = now;
	char dir[512], path[600], tmp[640]; if (!settings_shared_dir(dir, sizeof dir)) return;
	snprintf(path, sizeof path, "%sonline_status.txt", dir); snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "w"); if (!f) return;
	const char *game = getenv("RECOMP_GAME_NAME");
	fprintf(f, "time=%lld\ngame=%s\nname=%s\nlinked=%d\nknown=%d\ndht_nodes=%d\npublic=%s\nrooms_heard=%d\nrooms_mine=%d\nclash=%d\n",
	        (long long)now, game ? game : "", s.name, s.linked, s.known, s.dht_nodes, s.pub, s.rooms_heard, s.rooms_mine, s.clash);
	if (fclose(f) || rename(tmp, path)) remove(tmp);
}
