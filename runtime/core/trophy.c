/*
 * trophy.c: SceNpTrophy, the game's trophies kept on this machine. A Vita keeps them locally too (online only syncs them):
 * a context reads the game's own trophy pack, app0:sce_sys/trophy/<NPWRnnnnn_nn>/TROPHY.TRP -- its trophy list in plain XML
 * (TROP.SFM, or TROP_<language>.SFM in the game's language) and its icons -- and an unlock is kept in
 * ux0:user/00/trophy/data/<NPWRnnnnn_nn>/nativ_trophies.txt (one line a trophy: its id and the SceRtcTick it was unlocked
 * at), so it lasts across sessions with the game's saves. Unlocking shows a toast. The platinum unlocks itself once every
 * trophy that counts towards it (its pid) is unlocked.
 *
 * Layouts and errors are the firmware's np_trophy (its size checks: game and group details 0x49c, game data 0x1c, group
 * data 0x20, trophy details 0x494, trophy data 0x18; INVALID_ARGUMENT 0x80551604) with the error numbering
 * 0x80551600 + n. Before, only CreateContext and CreateHandle answered (apputil.c, writing 1); RegisterContext, UnlockTrophy
 * and every query were main.c's catch-all ("not initialised"). Tested by runtime/tests/trophy_test.c.
 */
#include "hle.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include "win32_compat.h"
#endif

const char *host_path_str(const char *p, char *buf, size_t n);   /* io.c: a Vita path as the host's */
uint32_t game_system_language(void);                             /* apputil.c: the language the game is told (SCE_SYSTEM_PARAM_LANG_*) */
void overlay_toast_titled(const char *title, const char *body, double seconds);

enum {
	TR_NOT_INITIALIZED = 0x80551601u, TR_ALREADY_INITIALIZED = 0x80551602u, TR_INVALID_ARGUMENT = 0x80551604u,
	TR_INVALID_CONTEXT = 0x80551609u, TR_INVALID_TROPHY_ID = 0x8055160eu, TR_ALREADY_UNLOCKED = 0x8055160fu,
	TR_PLATINUM_CANNOT_UNLOCK = 0x80551610u, TR_CONTEXT_ALREADY_EXISTS = 0x80551616u, TR_TRP_NOT_FOUND = 0x80551619u,
};
enum { MAXTROPHY = 128, MAXGROUP = 32, MAXCTX = 4, NAMEMAX = 128, DETAILMAX = 1024 };
enum { GRADE_PLATINUM = 1, GRADE_GOLD = 2, GRADE_SILVER = 3, GRADE_BRONZE = 4 };

typedef struct { int used, grade, hidden, gid, pid; char name[NAMEMAX], detail[DETAILMAX]; } Trophy;
typedef struct { int used; char name[NAMEMAX], detail[DETAILMAX]; } Group;
typedef struct {
	int used, count;
	char comm[16], trp[1024], save[1024], title[NAMEMAX], detail[DETAILMAX];
	Trophy t[MAXTROPHY]; Group g[MAXGROUP];
	uint32_t flags[MAXTROPHY / 32]; uint64_t when[MAXTROPHY];
} Ctx;
static Ctx ctxs[MAXCTX];
static int inited, next_handle = 1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int unlocked(const Ctx *x, int id) { return (x->flags[id >> 5] >> (id & 31)) & 1; }
static Ctx *ctx_of(uint32_t id) { return id >= 1 && id <= MAXCTX && ctxs[id - 1].used ? &ctxs[id - 1] : 0; }

/* ---- the trophy pack: a big-endian table of named entries (header 0x40 bytes: magic 0xdca24d00, the entry count at
 * 0x10 and their size at 0x14; each entry its name, then its offset and size as 64-bit numbers) */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static uint8_t *trp_entry(const char *trp, const char *name, uint32_t *size) {   /* the entry's bytes (NUL after them), or 0 */
	FILE *f = fopen(trp, "rb"); if (!f) return 0;
	uint8_t h[0x40], e[0x40]; uint8_t *out = 0;
	if (fread(h, 1, sizeof h, f) == sizeof h && be32(h) == 0xdca24d00u) {
		const uint32_t n = be32(h + 0x10), es = be32(h + 0x14);
		for (uint32_t i = 0; i < n && i < 4096 && es >= 0x30 && es <= sizeof e; i++) {
			if (fseek(f, 0x40 + (long)i * (long)es, SEEK_SET) || fread(e, 1, es, f) != es) break;
			if (strncmp((const char *)e, name, 32)) continue;
			const uint64_t off = be64(e + 32), sz = be64(e + 40);
			if (sz > (64u << 20) || fseek(f, (long)off, SEEK_SET)) break;
			out = malloc((size_t)sz + 1);
			if (out && fread(out, 1, (size_t)sz, f) == sz) { out[sz] = 0; *size = (uint32_t)sz; }
			else { free(out); out = 0; }
			break;
		}
	}
	fclose(f);
	return out;
}

/* ---- the trophy list: TROP.SFM's XML, read tolerantly (the tags and attributes it has; entities unescaped) */
static void xml_text(char *dst, size_t cap, const char *s, const char *end) {
	size_t n = 0;
	while (s < end && n + 1 < cap) {
		if (*s == '&') {
			static const struct { const char *e; char c; } ents[] = { {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''} };
			int k = 0; for (; k < 5 && strncmp(s, ents[k].e, strlen(ents[k].e)); k++) {}
			if (k < 5) { dst[n++] = ents[k].c; s += strlen(ents[k].e); continue; }
		}
		dst[n++] = *s++;
	}
	dst[n] = 0;
}
static int xml_elem(const char *from, const char *end, const char *tag, char *dst, size_t cap) {   /* <tag>text</tag> between from and end */
	char open[48], close[48]; snprintf(open, sizeof open, "<%s>", tag); snprintf(close, sizeof close, "</%s>", tag);
	const char *a = strstr(from, open); if (!a || a >= end) return 0;
	a += strlen(open); const char *b = strstr(a, close); if (!b || b > end) return 0;
	xml_text(dst, cap, a, b); return 1;
}
static int xml_attr(const char *tag, const char *tag_end, const char *name, char *dst, size_t cap) {   /* name="value" inside <...> */
	char pat[32]; snprintf(pat, sizeof pat, " %s=\"", name);
	const char *a = strstr(tag, pat); if (!a || a >= tag_end) return 0;
	a += strlen(pat); const char *b = strchr(a, '"'); if (!b || b > tag_end) return 0;
	xml_text(dst, cap, a, b); return 1;
}
static void parse_list(Ctx *x, const char *xml) {
	const char *end = xml + strlen(xml);
	const char *first = strstr(xml, "<group "); const char *t0 = strstr(xml, "<trophy ");
	const char *head_end = first && (!t0 || first < t0) ? first : t0 ? t0 : end;
	xml_elem(xml, head_end, "title-name", x->title, sizeof x->title);
	xml_elem(xml, head_end, "title-detail", x->detail, sizeof x->detail);
	for (const char *p = strstr(xml, "<group "); p; p = strstr(p + 1, "<group ")) {
		const char *te = strchr(p, '>'), *ge = strstr(p, "</group>"); if (!te || !ge) break;
		char v[16]; if (!xml_attr(p, te, "id", v, sizeof v)) continue;
		const int id = atoi(v); if (id < 0 || id >= MAXGROUP) continue;
		Group *g = &x->g[id]; g->used = 1;
		xml_elem(te, ge, "name", g->name, sizeof g->name); xml_elem(te, ge, "detail", g->detail, sizeof g->detail);
	}
	for (const char *p = strstr(xml, "<trophy "); p; p = strstr(p + 1, "<trophy ")) {
		const char *te = strchr(p, '>'), *tend = strstr(p, "</trophy>"); if (!te || !tend) break;
		char v[16]; if (!xml_attr(p, te, "id", v, sizeof v)) continue;
		const int id = atoi(v); if (id < 0 || id >= MAXTROPHY) continue;
		Trophy *t = &x->t[id]; t->used = 1; t->gid = -1; t->pid = -1;
		if (xml_attr(p, te, "ttype", v, sizeof v)) t->grade = v[0] == 'P' ? GRADE_PLATINUM : v[0] == 'G' ? GRADE_GOLD : v[0] == 'S' ? GRADE_SILVER : GRADE_BRONZE;
		if (xml_attr(p, te, "hidden", v, sizeof v)) t->hidden = !strcmp(v, "yes");
		if (xml_attr(p, te, "gid", v, sizeof v)) t->gid = atoi(v);
		if (xml_attr(p, te, "pid", v, sizeof v)) t->pid = atoi(v);
		xml_elem(te, tend, "name", t->name, sizeof t->name); xml_elem(te, tend, "detail", t->detail, sizeof t->detail);
		if (id + 1 > x->count) x->count = id + 1;
	}
}

/* ---- the unlocks: nativ_trophies.txt, "<id> <tick>" a line */
static void mkdirs(char *path) { for (char *q = path + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(path, 0755); *q = '/'; } }
static void load_unlocks(Ctx *x) {
	FILE *f = fopen(x->save, "r"); if (!f) return;
	int id; unsigned long long tick;
	while (fscanf(f, "%d %llu", &id, &tick) == 2) if (id >= 0 && id < MAXTROPHY && x->t[id].used) { x->flags[id >> 5] |= 1u << (id & 31); x->when[id] = tick; }
	fclose(f);
}
static void save_unlocks(Ctx *x) {
	char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.new", x->save); mkdirs(tmp);
	FILE *f = fopen(tmp, "w"); if (!f) { fprintf(stderr, "    trophy: %s: %s\n", tmp, strerror(errno)); return; }
	for (int id = 0; id < MAXTROPHY; id++) if (unlocked(x, id)) fprintf(f, "%d %llu\n", id, (unsigned long long)x->when[id]);
	fclose(f);
	remove(x->save); rename(tmp, x->save);
}
static uint64_t rtc_now(void) {   /* SceRtcTick: microseconds since 0001-01-01 */
	struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
	return ((uint64_t)t.tv_sec + 62135596800ull) * 1000000ull + (uint64_t)t.tv_nsec / 1000;
}
static const char *grade_name(int g) { return g == GRADE_PLATINUM ? "Platinum" : g == GRADE_GOLD ? "Gold" : g == GRADE_SILVER ? "Silver" : "Bronze"; }
static void unlock(Ctx *x, int id) {
	x->flags[id >> 5] |= 1u << (id & 31); x->when[id] = rtc_now();
	char title[64]; snprintf(title, sizeof title, "%s trophy", grade_name(x->t[id].grade));
	fprintf(stderr, "    trophy: %s: %s (%s)\n", x->comm, x->t[id].name, grade_name(x->t[id].grade));
	overlay_toast_titled(title, x->t[id].name, 4.0);
}

/* ---- the calls */
static void h_init(Cpu *c) { pthread_mutex_lock(&lock); const int was = inited; inited = 1; pthread_mutex_unlock(&lock); RET(was ? TR_ALREADY_INITIALIZED : 0); }
static void h_term(Cpu *c) { pthread_mutex_lock(&lock); const int was = inited; inited = 0; pthread_mutex_unlock(&lock); RET(was ? 0 : TR_NOT_INITIALIZED); }
static void h_create_context(Cpu *c) {   /* (SceNpTrophyContext *, const SceNpCommunicationId *, signature, options) */
	if (!A0 || !A1) { RET(TR_INVALID_ARGUMENT); return; }
	char comm[16]; snprintf(comm, sizeof comm, "%.9s_%02u", (const char *)G(A1), (unsigned)RD8(A1 + 10));   /* {data[9], term, num, dummy} */
	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAXCTX; i++) if (ctxs[i].used && !strcmp(ctxs[i].comm, comm)) { pthread_mutex_unlock(&lock); RET(TR_CONTEXT_ALREADY_EXISTS); return; }
	int k = 0; while (k < MAXCTX && ctxs[k].used) k++;
	if (k == MAXCTX) { pthread_mutex_unlock(&lock); RET(TR_INVALID_CONTEXT); return; }
	Ctx *x = &ctxs[k]; memset(x, 0, sizeof *x);
	snprintf(x->comm, sizeof x->comm, "%s", comm);
	char p[256]; snprintf(p, sizeof p, "app0:sce_sys/trophy/%s/TROPHY.TRP", comm); host_path_str(p, x->trp, sizeof x->trp);
	snprintf(p, sizeof p, "ux0:user/00/trophy/data/%s/nativ_trophies.txt", comm); host_path_str(p, x->save, sizeof x->save);
	char sfm[32]; snprintf(sfm, sizeof sfm, "TROP_%02u.SFM", game_system_language());
	uint32_t n; uint8_t *xml = trp_entry(x->trp, sfm, &n);
	if (!xml) xml = trp_entry(x->trp, "TROP.SFM", &n);
	if (!xml) { pthread_mutex_unlock(&lock); fprintf(stderr, "    trophy: %s: no trophy list (%s)\n", comm, x->trp); RET(TR_TRP_NOT_FOUND); return; }
	parse_list(x, (const char *)xml); free(xml);
	load_unlocks(x);
	x->used = 1;
	pthread_mutex_unlock(&lock);
	fprintf(stderr, "    trophy: %s \"%s\": %d trophies\n", comm, x->title, x->count);
	WR32(A0, (uint32_t)k + 1);
	RET(0);
}
static void h_destroy_context(Cpu *c) { pthread_mutex_lock(&lock); Ctx *x = ctx_of(A0); if (x) x->used = 0; pthread_mutex_unlock(&lock); RET(x ? 0 : TR_INVALID_CONTEXT); }
static void h_create_handle(Cpu *c) { if (!A0) { RET(TR_INVALID_ARGUMENT); return; } pthread_mutex_lock(&lock); WR32(A0, (uint32_t)next_handle++); pthread_mutex_unlock(&lock); RET(0); }
static void h_zero(Cpu *c) { RET(0); }   /* DestroyHandle, AbortHandle */
static void h_register(Cpu *c) { pthread_mutex_lock(&lock); Ctx *x = ctx_of(A0); pthread_mutex_unlock(&lock); RET(x ? 0 : TR_INVALID_CONTEXT); }   /* (ctx, handle, options): installed already */
static void h_unlock(Cpu *c) {   /* (ctx, handle, trophyId, SceNpTrophyId *platinumId) */
	const int id = (int)A2;
	if (!A3) { RET(TR_INVALID_ARGUMENT); return; }
	pthread_mutex_lock(&lock);
	Ctx *x = ctx_of(A0);
	uint32_t r = !x ? TR_INVALID_CONTEXT : id < 0 || id >= MAXTROPHY || !x->t[id].used ? TR_INVALID_TROPHY_ID
	           : x->t[id].grade == GRADE_PLATINUM ? TR_PLATINUM_CANNOT_UNLOCK : unlocked(x, id) ? TR_ALREADY_UNLOCKED : 0;
	int plat = -1;
	if (!r) {
		unlock(x, id);
		const int p = x->t[id].pid;   /* the platinum it counts towards: unlocked with the last of them */
		if (p >= 0 && p < MAXTROPHY && x->t[p].used && x->t[p].grade == GRADE_PLATINUM && !unlocked(x, p)) {
			int all = 1; for (int k = 0; k < MAXTROPHY && all; k++) if (k != p && x->t[k].used && x->t[k].pid == p && !unlocked(x, k)) all = 0;
			if (all) { unlock(x, p); plat = p; }
		}
		save_unlocks(x);
	}
	pthread_mutex_unlock(&lock);
	if (!r) WR32(A3, (uint32_t)plat);
	RET(r);
}
static void h_unlock_state(Cpu *c) {   /* (ctx, handle, SceNpTrophyFlagArray *, uint32_t *count) */
	if (!A2 || !A3) { RET(TR_INVALID_ARGUMENT); return; }
	pthread_mutex_lock(&lock);
	Ctx *x = ctx_of(A0);
	if (x) { for (int i = 0; i < MAXTROPHY / 32; i++) WR32(A2 + 4u * (uint32_t)i, x->flags[i]); WR32(A3, (uint32_t)x->count); }
	pthread_mutex_unlock(&lock);
	RET(x ? 0 : TR_INVALID_CONTEXT);
}
/* counts over the trophies of one group (gid; -1: the base game's) or all (-2): numbers, then unlocked numbers */
static void tally(const Ctx *x, int gid, uint32_t n[5], uint32_t u[5]) {
	memset(n, 0, 5 * sizeof *n); memset(u, 0, 5 * sizeof *u);
	for (int k = 0; k < MAXTROPHY; k++) {
		const Trophy *t = &x->t[k]; if (!t->used || (gid != -2 && t->gid != gid)) continue;
		n[0]++; n[t->grade]++; if (unlocked(x, k)) { u[0]++; u[t->grade]++; }
	}
}
static void put_str(uint32_t a, const char *s, uint32_t cap) { memset(G(a), 0, cap); memcpy(G(a), s, strnlen(s, cap - 1)); }
static void h_game_info(Cpu *c) {   /* (ctx, handle, SceNpTrophyGameDetails *, SceNpTrophyGameData *) */
	if ((A2 && RD32(A2) != 0x49c) || (A3 && RD32(A3) != 0x1c)) { RET(TR_INVALID_ARGUMENT); return; }
	pthread_mutex_lock(&lock);
	Ctx *x = ctx_of(A0);
	if (x) {
		uint32_t n[5], u[5]; tally(x, -2, n, u);
		if (A2) { uint32_t groups = 0; for (int g = 0; g < MAXGROUP; g++) groups += (uint32_t)x->g[g].used;
			WR32(A2 + 4, groups); for (int k = 0; k < 5; k++) WR32(A2 + 8 + 4u * (uint32_t)k, n[k]);
			put_str(A2 + 28, x->title, NAMEMAX); put_str(A2 + 156, x->detail, DETAILMAX); }
		if (A3) { for (int k = 0; k < 5; k++) WR32(A3 + 4 + 4u * (uint32_t)k, u[k]); WR32(A3 + 24, n[0] ? u[0] * 100 / n[0] : 0); }
	}
	pthread_mutex_unlock(&lock);
	RET(x ? 0 : TR_INVALID_CONTEXT);
}
static void h_group_info(Cpu *c) {   /* (ctx, handle, groupId: -1 the base game, SceNpTrophyGroupDetails *, SceNpTrophyGroupData *) */
	const int gid = (int)A2; const uint32_t det = A3, dat = STK(0);
	if ((det && RD32(det) != 0x49c) || (dat && RD32(dat) != 0x20) || gid < -1 || gid >= MAXGROUP) { RET(TR_INVALID_ARGUMENT); return; }
	pthread_mutex_lock(&lock);
	Ctx *x = ctx_of(A0);
	uint32_t r = !x ? TR_INVALID_CONTEXT : gid >= 0 && !x->g[gid].used ? TR_INVALID_ARGUMENT : 0;
	if (!r) {
		uint32_t n[5], u[5]; tally(x, gid, n, u);
		if (det) { WR32(det + 4, (uint32_t)gid); for (int k = 0; k < 5; k++) WR32(det + 8 + 4u * (uint32_t)k, n[k]);
			put_str(det + 28, gid < 0 ? x->title : x->g[gid].name, NAMEMAX); put_str(det + 156, gid < 0 ? x->detail : x->g[gid].detail, DETAILMAX); }
		if (dat) { WR32(dat + 4, (uint32_t)gid); for (int k = 0; k < 5; k++) WR32(dat + 8 + 4u * (uint32_t)k, u[k]); WR32(dat + 28, n[0] ? u[0] * 100 / n[0] : 0); }
	}
	pthread_mutex_unlock(&lock);
	RET(r);
}
static void h_trophy_info(Cpu *c) {   /* (ctx, handle, trophyId, SceNpTrophyDetails *, SceNpTrophyData *) */
	const int id = (int)A2; const uint32_t det = A3, dat = STK(0);
	if ((det && RD32(det) != 0x494) || (dat && RD32(dat) != 0x18)) { RET(TR_INVALID_ARGUMENT); return; }
	pthread_mutex_lock(&lock);
	Ctx *x = ctx_of(A0);
	uint32_t r = !x ? TR_INVALID_CONTEXT : id < 0 || id >= MAXTROPHY || !x->t[id].used ? TR_INVALID_TROPHY_ID : 0;
	if (!r) {
		const Trophy *t = &x->t[id];
		if (det) { WR32(det + 4, (uint32_t)id); WR32(det + 8, (uint32_t)t->grade); WR32(det + 12, (uint32_t)t->gid); WR32(det + 16, (uint32_t)t->hidden);
			put_str(det + 20, t->name, NAMEMAX); put_str(det + 148, t->detail, DETAILMAX); }
		if (dat) { WR32(dat + 4, (uint32_t)id); WR32(dat + 8, (uint32_t)unlocked(x, id)); WR32(dat + 12, 0);
			const uint64_t w = unlocked(x, id) ? x->when[id] : 0; WR32(dat + 16, (uint32_t)w); WR32(dat + 20, (uint32_t)(w >> 32)); }
	}
	pthread_mutex_unlock(&lock);
	RET(r);
}
static uint32_t icon(Ctx *x, const char *name, uint32_t buf, uint32_t sizep) {   /* a PNG from the pack: its size through *size, its bytes when buf */
	if (!sizep) return TR_INVALID_ARGUMENT;
	uint32_t n; uint8_t *png = trp_entry(x->trp, name, &n);
	if (!png) return 0x80551617u;   /* ICON_FILE_NOT_FOUND */
	if (buf) { if (RD32(sizep) < n) { free(png); return 0x80551605u; } memcpy(G(buf), png, n); }   /* (INSUFFICIENT_BUFFER) */
	WR32(sizep, n); free(png);
	return 0;
}
static void h_game_icon(Cpu *c) {   /* (ctx, handle, buffer, SceSize *size) */
	pthread_mutex_lock(&lock); Ctx *x = ctx_of(A0); pthread_mutex_unlock(&lock);   /* (its pack's path is fixed once made) */
	RET(x ? icon(x, "ICON0.PNG", A2, A3) : TR_INVALID_CONTEXT);
}
static void h_group_icon(Cpu *c) {   /* (ctx, handle, groupId, buffer, SceSize *size) */
	char name[16]; snprintf(name, sizeof name, "GR%03d.PNG", (int)A2);
	pthread_mutex_lock(&lock); Ctx *x = ctx_of(A0); pthread_mutex_unlock(&lock);
	RET(x ? icon(x, name, A3, STK(0)) : TR_INVALID_CONTEXT);
}
static void h_trophy_icon(Cpu *c) {   /* (ctx, handle, trophyId, buffer, SceSize *size) */
	char name[16]; snprintf(name, sizeof name, "TROP%03d.PNG", (int)A2);
	pthread_mutex_lock(&lock); Ctx *x = ctx_of(A0); pthread_mutex_unlock(&lock);
	RET(x ? icon(x, name, A3, STK(0)) : TR_INVALID_CONTEXT);
}

const HleEntry hle_trophy[] = {
	{"sceNpTrophyInit", h_init}, {"sceNpTrophyTerm", h_term},
	{"sceNpTrophyCreateContext", h_create_context}, {"sceNpTrophyDestroyContext", h_destroy_context},
	{"sceNpTrophyCreateHandle", h_create_handle}, {"sceNpTrophyDestroyHandle", h_zero}, {"sceNpTrophyAbortHandle", h_zero},
	{"sceNpTrophyRegisterContext", h_register}, {"sceNpTrophyUnlockTrophy", h_unlock}, {"sceNpTrophyGetTrophyUnlockState", h_unlock_state},
	{"sceNpTrophyGetGameInfo", h_game_info}, {"sceNpTrophyGetGroupInfo", h_group_info}, {"sceNpTrophyGetTrophyInfo", h_trophy_info},
	{"sceNpTrophyGetGameIcon", h_game_icon}, {"sceNpTrophyGetGroupIcon", h_group_icon}, {"sceNpTrophyGetTrophyIcon", h_trophy_icon},
	{0, 0},
};
