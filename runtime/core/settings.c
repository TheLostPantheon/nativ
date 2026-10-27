/* settings.h: key=value settings in two files -- the game's own, and the shared one every game reads (see the header) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include "settings.h"

#define MAXSET 160
enum { GAME, SHARED };
static struct { char key[32], val[96]; unsigned char layer, dirty, gone; } tab[MAXSET];   /* gone: removed, dropped from its file at the next save */
static int ntab;
static char file[2][512] = {"settings.ini", ""};   /* the game's, the shared one ("": none -- a test, or RECOMP_SETTINGS) */
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;

/* the game's own options: only ever in its own file (what each game's menu shows under its name: its hooks' options,
 * its server; first-launch hints are per game) -- every other key is the player's, the same in every game */
static int game_key(const char *key) {
	static const char *const own[] = {"fov", "view_distance", "scenery_detail", "character_detail", "private_server", "net_redirect",
	                                  "menu_hint_shown", NULL};
	for (int i = 0; own[i]; i++) if (!strcmp(own[i], key)) return 1;
	return 0;
}
static int find(const char *key, int layer) { for (int i = 0; i < ntab; i++) if (tab[i].layer == layer && !tab[i].gone && !strcmp(tab[i].key, key)) return i; return -1; }
static int lookup(const char *key) { int i = find(key, GAME); return i >= 0 ? i : find(key, SHARED); }   /* the game's file first: an override or its own option */
static void put(const char *key, const char *val, int layer, int dirty) {   /* (mx held) */
	int i = find(key, layer);
	if (i < 0) {
		for (i = 0; i < ntab && !(tab[i].gone && tab[i].layer == layer && !strcmp(tab[i].key, key)); i++) {}
		if (i == ntab) { if (ntab == MAXSET) return; ntab++; }
		snprintf(tab[i].key, sizeof tab[i].key, "%s", key); tab[i].layer = (unsigned char)layer;
	}
	snprintf(tab[i].val, sizeof tab[i].val, "%s", val);
	tab[i].gone = 0; tab[i].dirty |= (unsigned char)dirty;
}
static char *trim(char *s) {
	while (*s == ' ' || *s == '\t') s++;
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
	return s;
}
/* each key=value line of a file, to fn (comments and blank lines skipped) */
static void read_file(const char *path, void (*fn)(const char *k, const char *v, void *), void *u) {
	FILE *f = path[0] ? fopen(path, "r") : NULL;
	if (!f) return;
	char line[256];
	while (fgets(line, sizeof line, f)) {
		char *s = trim(line), *eq = strchr(s, '=');
		if (!*s || *s == '#' || *s == ';' || !eq) continue;
		*eq = 0;
		fn(trim(s), trim(eq + 1), u);
	}
	fclose(f);
}
static void load_into(const char *k, const char *v, void *u) { put(k, v, (int)(intptr_t)u, 0); }
void settings_load(const char *path) {
	pthread_mutex_lock(&mx);
	if (path) snprintf(file[GAME], sizeof file[GAME], "%s", path);
	read_file(file[GAME], load_into, (void *)(intptr_t)GAME);
	pthread_mutex_unlock(&mx);
}
void settings_load_shared(const char *path) {
	pthread_mutex_lock(&mx);
	snprintf(file[SHARED], sizeof file[SHARED], "%s", path ? path : "");
	read_file(file[SHARED], load_into, (void *)(intptr_t)SHARED);
	pthread_mutex_unlock(&mx);
}
int settings_shared_dir(char *buf, int n) {
	pthread_mutex_lock(&mx); const char *f = file[SHARED], *sl = strrchr(f, '/'), *bs = strrchr(f, '\\'); if (bs > sl) sl = bs;
	const int ok = f[0] && sl && (int)(sl - f) + 2 < n; if (ok) snprintf(buf, (size_t)n, "%.*s", (int)(sl - f) + 1, f);
	pthread_mutex_unlock(&mx);
	return ok;
}
int settings_game_option(const char *name) {
	const char *e = getenv("RECOMP_GAME_OPTIONS"); const size_t n = strlen(name);
	for (const char *p = e; p && *p; p += strcspn(p, ","), p += *p == ',') if (strcspn(p, ",") == n && !strncmp(p, name, n)) return 1;
	return 0;
}
int settings_has(const char *key) { pthread_mutex_lock(&mx); int i = lookup(key); pthread_mutex_unlock(&mx); return i >= 0; }
const char *settings_str(const char *key, const char *def, char *buf, int bufsize) {
	pthread_mutex_lock(&mx);
	int i = lookup(key);
	snprintf(buf, (size_t)bufsize, "%s", i >= 0 ? tab[i].val : def ? def : "");
	pthread_mutex_unlock(&mx);
	return buf;
}
int settings_int(const char *key, int def) { char b[96]; if (!settings_has(key)) return def; return atoi(settings_str(key, "", b, sizeof b)); }
double settings_float(const char *key, double def) { char b[96]; if (!settings_has(key)) return def; return atof(settings_str(key, "", b, sizeof b)); }
/* a shared key goes to the shared file and the game's override of it goes: a change made in any game's menu is made
 * for every game. The game's own options (and everything, with no shared file) stay in its own file. */
void settings_set_str(const char *key, const char *v) {
	pthread_mutex_lock(&mx);
	if (game_key(key) || !file[SHARED][0]) put(key, v, GAME, 1);
	else {
		put(key, v, SHARED, 1);
		int i = find(key, GAME); if (i >= 0) { tab[i].gone = 1; tab[i].dirty = 1; }
	}
	pthread_mutex_unlock(&mx);
}
void settings_set_own(const char *key, const char *v) { pthread_mutex_lock(&mx); put(key, v, GAME, 1); pthread_mutex_unlock(&mx); }
void settings_set_int(const char *key, int v) { char b[32]; snprintf(b, sizeof b, "%d", v); settings_set_str(key, b); }
void settings_set_float(const char *key, double v) { char b[32]; snprintf(b, sizeof b, "%g", v); settings_set_str(key, b); }

/* saving merges: the file as it is on disk now (the launcher, or another game, may have written it since it was read),
 * with this run's changes over it -- set, or removed -- so nobody's edit is lost to a whole-table write */
#define MAXLINE 200
struct merged { int n; char key[MAXLINE][32], val[MAXLINE][96]; };
static void merge_in(const char *k, const char *v, void *u) {
	struct merged *m = u;
	if (m->n == MAXLINE) return;
	snprintf(m->key[m->n], sizeof m->key[0], "%s", k); snprintf(m->val[m->n], sizeof m->val[0], "%s", v); m->n++;
}
static void save_layer(int layer, const char *header) {
	int any = 0;
	for (int i = 0; i < ntab; i++) any |= tab[i].layer == layer && tab[i].dirty;
	if (!any || !file[layer][0]) return;
	static struct merged m; m.n = 0;
	read_file(file[layer], merge_in, &m);
	for (int i = 0; i < ntab; i++) {
		if (tab[i].layer != layer || !tab[i].dirty) continue;
		int j = 0; while (j < m.n && strcmp(m.key[j], tab[i].key)) j++;
		if (tab[i].gone) { if (j < m.n) { m.n--; memmove(&m.key[j], &m.key[j + 1], sizeof m.key[0] * (size_t)(m.n - j)); memmove(&m.val[j], &m.val[j + 1], sizeof m.val[0] * (size_t)(m.n - j)); } }
		else { if (j == m.n && m.n < MAXLINE) snprintf(m.key[m.n++], sizeof m.key[0], "%s", tab[i].key); if (j < m.n) snprintf(m.val[j], sizeof m.val[0], "%s", tab[i].val); }
		tab[i].dirty = 0;
	}
	char tmp[530]; snprintf(tmp, sizeof tmp, "%s.tmp", file[layer]);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	fprintf(f, "%s\n", header);
	for (int j = 0; j < m.n; j++) fprintf(f, "%s = %s\n", m.key[j], m.val[j]);
	if (fclose(f) == 0) rename(tmp, file[layer]);   /* whole or not at all */
}
void settings_save(void) {
	pthread_mutex_lock(&mx);
	save_layer(GAME, "# vitarecomp settings for this game: its own options, and where it differs from the shared settings (runtime/core/settings.h)");
	save_layer(SHARED, "# Project Nativ's settings for every game (the launcher's Settings, and each game's menu; runtime/core/settings.h)");
	pthread_mutex_unlock(&mx);
}
