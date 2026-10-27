/*
 * gxp_index.c: where a game's shader programs (GXPs) are in its own files, for the shader recipes' loader (gfx_gpu.c).
 *
 * A recipe names its programs by content hash; the game registers them only as it loads them (a level's on its load
 * screen), and building them cold takes longer than that screen lasts. But the programs are in the game's files as they
 * are registered -- Killzone's sit uncompressed inside its lumps, in game_data/data_ngp.psarc (mission 2's art lump alone
 * holds 3070, byte-identical to what the game registers). So every file under app0 is scanned once for GXP headers
 * ("GXP\0", the size at +8): each PSARC's entries block by block (psarc.c), other files as they are, media skipped. The
 * index -- each program's content hash, size, file, archive entry and offset; not a byte of the program -- is cached in
 * the shader cache (programs.idx) and rebuilt when any file's size or time changes. Then the programs the recipes want
 * are read: a range read inflates only the one or two 64 KB blocks a program sits in.
 * All of it on background threads at a low priority (the scan's first run: a few seconds of a few cores).
 */
#include "gxp_index.h"
#include "platform.h"
#include "psarc.h"
#include "usse.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

#define IDX_HEADER "# ativ program index 1"
#define MAXSRC 4096
typedef struct { uint32_t hash, size; int32_t src, entry; uint64_t off; } Loc;   /* entry -1: a plain file */
typedef struct { char *rel; int psarc; uint64_t size; int64_t mtime; Psarc ps; } Source;
static struct {
	char app0[1024], cache[1024];
	Source src[MAXSRC]; int nsrc;
	Loc *loc; int nloc, cap; pthread_mutex_t m;
	struct Item { int src, entry; } *items; int nitems; int next;   /* the scan's work: a file, or one entry of an archive (-1: the file itself) */
	uint32_t *want; int nwant, want_all;   /* want_all: every program the index finds (gxp_index_start with no list: gfx_gpu.c's predicted shaders) */
	struct Have { uint32_t hash, size; uint8_t *bytes; } *have; int nhave;   /* sorted by hash */
	int ready, started;
} gi = { .m = PTHREAD_MUTEX_INITIALIZER };

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void low_priority(void) {
#ifdef __APPLE__
	pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(__linux__)
	setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 15);
#endif
}
static int skipped(const char *name) {   /* media: no programs in them, and most of a game's bytes */
	static const char *ext[] = { ".mp4", ".at9", ".at3", ".wav", ".mp3", ".ogg", ".pam", ".pmf", ".avi", ".bik", ".png", ".jpg", ".jpeg", ".gxt", ".dds", ".sfo", ".ttf", ".otf", 0 };
	const char *dot = strrchr(name, '.'); if (!dot) return 0;
	for (int i = 0; ext[i]; i++) if (!strcasecmp(dot, ext[i])) return 1;
	return 0;
}
static void walk(const char *dir, const char *rel) {   /* every regular file under app0, into gi.src */
	char p[2048]; DIR *d = opendir(dir); if (!d) return;
	struct dirent *de;
	while ((de = readdir(d)) && gi.nsrc < MAXSRC) {
		if (de->d_name[0] == '.' || skipped(de->d_name)) continue;
		snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
		char r[1024]; snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", de->d_name);
		struct stat st; if (stat(p, &st)) continue;
		if (S_ISDIR(st.st_mode)) { walk(p, r); continue; }
		if (!S_ISREG(st.st_mode) || st.st_size < 16) continue;
		Source *s = &gi.src[gi.nsrc++]; memset(s, 0, sizeof *s); s->ps.fd = -1;
		s->rel = strdup(r); s->size = (uint64_t)st.st_size; s->mtime = (int64_t)st.st_mtime;
		char m[4] = {0}; int fd = open(p, O_RDONLY); if (fd >= 0) { if (pread(fd, m, 4, 0) != 4) m[0] = 0; close(fd); }
		s->psarc = !memcmp(m, "PSAR", 4);
	}
	closedir(d);
}
static void add(uint32_t hash, uint32_t size, int src, int entry, uint64_t off) {
	pthread_mutex_lock(&gi.m);
	if (gi.nloc == gi.cap) { int c = gi.cap ? 2 * gi.cap : 16384; Loc *n = realloc(gi.loc, c * sizeof *n); if (!n) { pthread_mutex_unlock(&gi.m); return; } gi.loc = n; gi.cap = c; }
	gi.loc[gi.nloc++] = (Loc){ hash, size, src, entry, off };
	pthread_mutex_unlock(&gi.m);
}
/* the GXP headers in buf[0, n) (an item's bytes from offset base on), each one's whole program from `get` when it runs
 * past buf; magic at buf[i] for i < limit only (the window's overlap is searched again with the next one) */
typedef int (*GetFn)(void *ctx, uint64_t off, void *out, size_t len);
static void find(const uint8_t *buf, size_t n, size_t limit, uint64_t base, uint64_t total, GetFn get, void *ctx, int src, int entry) {
	for (size_t i = 0; i < limit; i++) {
		const uint8_t *g = memchr(buf + i, 'G', limit - i); if (!g) return;
		i = (size_t)(g - buf);
		if (i + 4 > n || memcmp(g, "GXP", 4)) continue;
		uint8_t hdr[12]; const uint8_t *hp = g;
		if (i + 12 > n) { if (base + i + 12 > total || get(ctx, base + i, hdr, 12)) continue; hp = hdr; }   /* (a header across the window's end) */
		uint32_t size; memcpy(&size, hp + 8, 4);
		const uint32_t span = size < 128 ? 128 : size;   /* (what usse_gxp_hash reads) */
		if (size < 32 || size > (1u << 20) || base + i + span > total) continue;
		uint32_t h, sz;
		if (i + span <= n) h = usse_gxp_hash(g, &sz);
		else { uint8_t *t = malloc(span); if (!t) continue; if (get(ctx, base + i, t, span)) { free(t); continue; } h = usse_gxp_hash(t, &sz); free(t); }
		add(h, size, src, entry, base + i);
	}
}
typedef struct { Source *s; const PsarcEntry *e; int fd; } Ctx;
static int get_any(void *c, uint64_t off, void *out, size_t len) {
	Ctx *x = c;
	if (x->e) return psarc_read_range(&x->s->ps, x->e, off, out, len);
	return pread(x->fd, out, len, (off_t)off) == (ssize_t)len ? 0 : -1;
}
static void scan_item(struct Item it) {
	const int si = it.src, ei = it.entry; Source *s = &gi.src[si];
	if (ei >= 0) {   /* a PSARC entry: block by block, each searched with the previous block's last bytes before it */
		const PsarcEntry *e = &s->ps.e[ei]; const uint32_t bs = s->ps.block_size;
		uint8_t *buf = malloc(2 * (size_t)bs); if (!buf) return;
		Ctx x = { s, e, -1 }; uint64_t foff = e->off; size_t carry = 0;
		for (uint32_t k = 0; (uint64_t)k * bs < e->size; k++) {
			uint32_t n;
			if (psarc_block(&s->ps, e, k, &foff, buf + carry, &n)) break;
			const uint64_t base = (uint64_t)k * bs - carry;
			const size_t have = carry + n, last = (uint64_t)k * bs + n >= e->size;
			find(buf, have, last ? have : have - 3, base, e->size, get_any, &x, si, ei);
			carry = last ? 0 : 3; memmove(buf, buf + have - carry, carry);
		}
		free(buf);
	} else {   /* a plain file, 1 MB at a time */
		char p[2048]; snprintf(p, sizeof p, "%s/%s", gi.app0, s->rel);
		const int fd = open(p, O_RDONLY); if (fd < 0) return;
		const size_t win = 1 << 20; uint8_t *buf = malloc(win + 3); Ctx x = { s, 0, fd };
		for (uint64_t at = 0; buf && at < s->size; ) {
			const size_t want = s->size - at < win ? (size_t)(s->size - at) : win;
			if (pread(fd, buf, want + (at + want < s->size ? 3 : 0), (off_t)at) < (ssize_t)want) break;
			find(buf, want + (at + want < s->size ? 3 : 0), want, at, s->size, get_any, &x, si, -1);
			at += want;
		}
		free(buf); close(fd);
	}
}
static void *scan_worker(void *arg) {
	(void)arg; low_priority();
	for (;;) {
		const int i = __atomic_fetch_add(&gi.next, 1, __ATOMIC_RELAXED);
		if (i >= gi.nitems) return 0;
		scan_item(gi.items[i]);
	}
}
static int loc_cmp(const void *a, const void *b) { const Loc *x = a, *y = b; return x->hash < y->hash ? -1 : x->hash > y->hash ? 1 : x->src != y->src ? x->src - y->src : x->entry != y->entry ? x->entry - y->entry : x->off < y->off ? -1 : x->off > y->off; }
static int load_cache(const char *path) {   /* 1: the index as it was saved, its files unchanged */
	FILE *f = fopen(path, "r"); if (!f) return 0;
	char line[4096]; int ok = fgets(line, sizeof line, f) && !strncmp(line, IDX_HEADER, strlen(IDX_HEADER)), nf = 0;
	while (ok && fgets(line, sizeof line, f)) {
		if (line[0] == 'F') {
			int i, ps; unsigned long long sz; long long mt; char rel[2048];
			if (sscanf(line, "F %d %d %llu %lld %2047[^\n]", &i, &ps, &sz, &mt, rel) != 5 || i != nf || i >= gi.nsrc
			    || strcmp(rel, gi.src[i].rel) || ps != gi.src[i].psarc || sz != gi.src[i].size || mt != gi.src[i].mtime) ok = 0;
			nf++;
		} else if (line[0] == 'G') {
			unsigned h, sz; int s, e; unsigned long long off;
			if (sscanf(line, "G %x %u %d %d %llu", &h, &sz, &s, &e, &off) != 5 || s < 0 || s >= gi.nsrc) { ok = 0; break; }
			add(h, sz, s, e, off);
		}
	}
	fclose(f);
	if (!ok || nf != gi.nsrc) { gi.nloc = 0; return 0; }
	return 1;
}
static void save_cache(const char *path) {
	char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, (int)getpid());
	FILE *f = fopen(tmp, "w"); if (!f) return;
	fprintf(f, "%s\n", IDX_HEADER);
	for (int i = 0; i < gi.nsrc; i++) fprintf(f, "F %d %d %llu %lld %s\n", i, gi.src[i].psarc, (unsigned long long)gi.src[i].size, (long long)gi.src[i].mtime, gi.src[i].rel);
	for (int i = 0; i < gi.nloc; i++) fprintf(f, "G %08x %u %d %d %llu\n", gi.loc[i].hash, gi.loc[i].size, gi.loc[i].src, gi.loc[i].entry, (unsigned long long)gi.loc[i].off);
	if (fclose(f) || rename(tmp, path)) unlink(tmp);
}
static int u32_cmp(const void *a, const void *b) { const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return (x > y) - (x < y); }
static int have_cmp(const void *a, const void *b) { const uint32_t x = ((const struct Have *)a)->hash, y = ((const struct Have *)b)->hash; return (x > y) - (x < y); }
static void *index_main(void *arg) {
	(void)arg; low_priority();
	const double t0 = now_s();
	walk(gi.app0, "");
	for (int i = 0; i < gi.nsrc; i++) if (gi.src[i].psarc) { char p[2048]; snprintf(p, sizeof p, "%s/%s", gi.app0, gi.src[i].rel); if (psarc_open(&gi.src[i].ps, p)) gi.src[i].psarc = 0; }
	char idx[1100]; snprintf(idx, sizeof idx, "%s/programs.idx", gi.cache);
	const int cached = load_cache(idx);
	if (!cached) {   /* the scan: every entry and file, on a few threads */
		int total = 0; for (int i = 0; i < gi.nsrc; i++) total += gi.src[i].psarc ? gi.src[i].ps.n : 1;
		gi.items = malloc((size_t)total * sizeof *gi.items);
		for (int i = 0; gi.items && i < gi.nsrc; i++) {
			if (!gi.src[i].psarc) { gi.items[gi.nitems++] = (struct Item){ i, -1 }; continue; }
			for (int k = 0; k < gi.src[i].ps.n; k++) gi.items[gi.nitems++] = (struct Item){ i, k };
		}
		int nt = plat_cpu_count() / 3; if (nt < 1) nt = 1; if (nt > 4) nt = 4;
		pthread_t t[4]; int started = 0;
		for (int i = 0; i < nt; i++) if (!pthread_create(&t[started], 0, scan_worker, 0)) started++;
		if (!started) scan_worker(0);
		for (int i = 0; i < started; i++) pthread_join(t[i], 0);
		free(gi.items); gi.items = 0;
	}
	qsort(gi.loc, (size_t)gi.nloc, sizeof *gi.loc, loc_cmp);
	int u = 0; for (int i = 0; i < gi.nloc; i++) if (!u || gi.loc[i].hash != gi.loc[u - 1].hash) gi.loc[u++] = gi.loc[i];   /* (one place per program) */
	gi.nloc = u;
	if (!cached) save_cache(idx);
	const double t1 = now_s();
	/* the wanted programs' bytes */
	if (gi.want_all) {   /* every program the index found */
		free(gi.want); gi.want = malloc(((size_t)gi.nloc + 1) * sizeof *gi.want); gi.nwant = 0;
		for (int i = 0; gi.want && i < gi.nloc; i++) gi.want[gi.nwant++] = gi.loc[i].hash;
	}
	qsort(gi.want, (size_t)gi.nwant, sizeof *gi.want, u32_cmp);
	gi.have = calloc((size_t)gi.nwant + 1, sizeof *gi.have);
	int nunique = 0;
	for (int w = 0; gi.have && w < gi.nwant; w++) {
		if (w && gi.want[w] == gi.want[w - 1]) continue;
		nunique++;
		int lo = 0, hi = gi.nloc;
		while (lo < hi) { int mid = (lo + hi) / 2; if (gi.loc[mid].hash < gi.want[w]) lo = mid + 1; else hi = mid; }
		if (lo == gi.nloc || gi.loc[lo].hash != gi.want[w]) continue;
		const Loc *l = &gi.loc[lo]; Source *s = &gi.src[l->src];
		const uint32_t span = l->size < 128 ? 128 : l->size;
		uint8_t *b = malloc(span); if (!b) continue;
		int bad;
		if (l->entry >= 0) bad = !s->psarc || l->entry >= s->ps.n || psarc_read_range(&s->ps, &s->ps.e[l->entry], l->off, b, span);
		else { char p[2048]; snprintf(p, sizeof p, "%s/%s", gi.app0, s->rel); int fd = open(p, O_RDONLY); bad = fd < 0 || pread(fd, b, span, (off_t)l->off) != (ssize_t)span; if (fd >= 0) close(fd); }
		uint32_t sz;
		if (bad || usse_gxp_hash(b, &sz) != l->hash) { free(b); continue; }   /* (the index went stale) */
		if (!usse_gxp_sane(b, l->size)) { free(b); continue; }   /* (not a program: a GXP header inside other data) */
		gi.have[gi.nhave++] = (struct Have){ l->hash, l->size, b };
	}
	qsort(gi.have, (size_t)gi.nhave, sizeof *gi.have, have_cmp);
	for (int i = 0; i < gi.nsrc; i++) if (gi.src[i].psarc) psarc_close(&gi.src[i].ps);
	fprintf(stderr, "    gxp index: %d programs in %d files under %s (%s in %.1f s); %d of the %d the shader recipes want read in %.0f ms\n",
	        gi.nloc, gi.nsrc, gi.app0, cached ? "from the cache" : "scanned", t1 - t0, gi.nhave, nunique, (now_s() - t1) * 1e3);
	free(gi.loc); gi.loc = 0; gi.nloc = gi.cap = 0;
	__atomic_store_n(&gi.ready, 1, __ATOMIC_RELEASE);
	return 0;
}
void gxp_index_start(const char *app0, const char *cache_dir, const uint32_t *want, int nwant) {
	if (gi.started++) return;
	snprintf(gi.app0, sizeof gi.app0, "%s", app0); snprintf(gi.cache, sizeof gi.cache, "%s", cache_dir);
	gi.want_all = !want;
	gi.want = malloc(((size_t)(want ? nwant : 0) + 1) * sizeof *gi.want);
	if (gi.want && want) { memcpy(gi.want, want, (size_t)nwant * sizeof *want); gi.nwant = nwant; }
	pthread_t t;
	if (pthread_create(&t, 0, index_main, 0)) { __atomic_store_n(&gi.ready, 1, __ATOMIC_RELEASE); return; }
	pthread_detach(t);
}
int gxp_index_ready(void) { return __atomic_load_n(&gi.ready, __ATOMIC_ACQUIRE); }
int gxp_index_count(void) { return gxp_index_ready() ? gi.nhave : 0; }
uint32_t gxp_index_hash_at(int i) { return gxp_index_ready() && i >= 0 && i < gi.nhave ? gi.have[i].hash : 0; }
const uint8_t *gxp_index_program(uint32_t hash, uint32_t *size) {
	if (!gxp_index_ready()) return 0;
	int lo = 0, hi = gi.nhave;
	while (lo < hi) { int mid = (lo + hi) / 2; if (gi.have[mid].hash < hash) lo = mid + 1; else hi = mid; }
	if (lo == gi.nhave || gi.have[lo].hash != hash) return 0;
	*size = gi.have[lo].size; return gi.have[lo].bytes;
}
