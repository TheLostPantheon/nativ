/*
 * The FALLBACK FIOS: only for a game whose conf doesn't list libfios2 in MODULES. With it listed
 * (all three games since 2026-10-07), every sceFios* import resolves to Sony's own libfios2 --
 * the copy the game ships in sce_module, as the console loads it -- running as recompiled code
 * (scheduler and I/O threads, the PSARC dearchiver and its zlib), and the host implements only
 * what that code calls underneath: the sceIo* system calls and the FIOS2 kernel's overlay
 * resolver (io.c). A loaded module's export wins over this table (main.c), so nothing here runs
 * then. Its error codes are approximations (the real SCE_FIOS_ERROR_* values: io.c).
 *
 * SceFios2 (Sony's file I/O scheduler) on host files. Paths are "/dev/rest" or "dev:rest" under
 * RECOMP_FS (as io.c). Asynchronous operations complete at once: an op handle keeps the result,
 * and the wait/query calls read it back. 64-bit sizes and offsets follow the AAPCS: an even
 * register pair when one is left, otherwise 8-aligned on the stack. PSARC archives a game mounts
 * (Fios's PSARC dearchiver) are read on the host: their files open as in-memory handles.
 */
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "psarc.h"
#include "hle.h"
#include "guestwatch.h"

#define RET64(v) do { int64_t _v = (int64_t)(v); c->r[0] = (uint32_t)_v; c->r[1] = (uint32_t)((uint64_t)_v >> 32); } while (0)
#define STK64(n) RD64(c->r[13] + 4 * (n))
#define FIOS_ERROR_BAD_PATH 0x80820003u        /* approximate SCE_FIOS_ERROR_* values: negative and distinct */
#define FIOS_ERROR_BAD_HANDLE 0x80820004u

static uint64_t fios_now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
void io_read_note(int64_t n, uint64_t us, int archive);   /* io.c */
void host_path_fold(char *buf, size_t root_len);   /* io.c */
static const char *root(void) { return getenv("RECOMP_FS") ? getenv("RECOMP_FS") : "fs"; }
static const char *fios_path(uint32_t gp, char *buf, size_t n) {
	const char *p = (char *)G(gp);
	if (*p != '/' && !strchr(p, ':')) { snprintf(buf, n, "%s/app0/%s", root(), p); return buf; }   /* relative: the app's folder */
	while (*p == '/') p++;
	const char *colon = strchr(p, ':'), *slash = strchr(p, '/');
	if (colon && (!slash || colon < slash)) {   /* "dev:rest" */
		const char *rest = colon + 1; while (*rest == '/') rest++;
		snprintf(buf, n, "%s/%.*s/%s", root(), (int)(colon - p), p, rest);
	} else snprintf(buf, n, "%s/%s", root(), p);   /* "/dev/rest" */
	for (char *q = buf + strlen(root()); *q; q++) if (*q == '\\') *q = '/';   /* a separator, as io.c */
	host_path_fold(buf, strlen(root()));   /* names ignoring case, as io.c */
	return buf;
}

/* ---- handles and operations */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { int fd; DIR *dir; char path[512]; uint8_t *mem; int64_t size, pos; } Handle;   /* mem: a file from an archive */
static Handle handles[1024];
typedef struct { int64_t result; uint32_t buf; int64_t req; int done; } Op;
static Op ops[4096];
static uint32_t next_op = 1;
static uint32_t new_handle(int fd, DIR *dir, const char *path) {
	pthread_mutex_lock(&lock);
	for (uint32_t i = 1; i < 1024; i++) if (!handles[i].fd && !handles[i].dir && !handles[i].mem) {
		memset(&handles[i], 0, sizeof handles[i]);
		handles[i].fd = fd; handles[i].dir = dir; snprintf(handles[i].path, sizeof handles[i].path, "%s", path);
		pthread_mutex_unlock(&lock); return i;
	}
	pthread_mutex_unlock(&lock);
	return 0;
}
static Handle *handle(uint32_t h) { return h && h < 1024 && (handles[h].fd || handles[h].dir || handles[h].mem) ? &handles[h] : 0; }
static uint32_t op_done(int64_t result, uint32_t buf, int64_t req) {
	pthread_mutex_lock(&lock);
	uint32_t o = next_op++;
	ops[o % 4096] = (Op){result, buf, req, 1};
	pthread_mutex_unlock(&lock);
	return o;
}
static Op *op(uint32_t o) { return &ops[o % 4096]; }

/* ---- PSARC archives (psarc.c), mounted at a guest path */
typedef PsarcEntry PsEntry;
typedef struct { char mount[256]; Psarc ps; } Archive;
static Archive archives[16];
static int narchives;
/* a guest path as "dev/rest" (relative paths are app0's) */
static void logical(const char *p, char *out, size_t n) {
	while (*p == '/') p++;
	const char *colon = strchr(p, ':'), *slash = strchr(p, '/');
	if (colon && (!slash || colon < slash)) { const char *r = colon + 1; while (*r == '/') r++; snprintf(out, n, "%.*s/%s", (int)(colon - p), p, r); }
	else if (!strncmp(p, "app0/", 5) || !strcmp(p, "app0") || !strncmp(p, "ux0/", 4) || !strncmp(p, "savedata0/", 10)) snprintf(out, n, "%s", p);
	else snprintf(out, n, "app0/%s", p);
	size_t l = strlen(out); while (l && out[l - 1] == '/') out[--l] = 0;
}
static int ps_mount(const char *host, const char *mount) {
	if (narchives == 16) return -1;
	Archive *a = &archives[narchives];
	if (psarc_open(&a->ps, host)) return -1;
	snprintf(a->mount, sizeof a->mount, "%s", mount);
	narchives++;
	fprintf(stderr, "    fios: mounted %s at %s (%d files)\n", host, mount, a->ps.n);
	return narchives - 1;
}
/* a guest path inside a mounted archive: its entry, or (dir) whether it's a directory there */
static PsEntry *ps_find(uint32_t gp, Archive **arch, int dir) {
	char lp[1024]; logical((char *)G(gp), lp, sizeof lp);
	for (int i = narchives - 1; i >= 0; i--) {
		Archive *a = &archives[i];
		size_t ml = strlen(a->mount);
		const char *rel = lp;
		if (ml) { if (strncmp(lp, a->mount, ml) || (lp[ml] && lp[ml] != '/')) continue; rel = lp + ml; while (*rel == '/') rel++; }
		char key[1024]; snprintf(key, sizeof key, "%s", rel);
		if (a->ps.icase) for (char *q = key; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
		if (dir) {   /* any name below it */
			size_t kl = strlen(key);
			for (int k = 0; k < a->ps.n; k++) if (!strncmp(a->ps.e[k].name, key, kl) && a->ps.e[k].name[kl] == '/') { if (arch) *arch = a; return &a->ps.e[k]; }
			continue;
		}
		PsEntry *e = (PsEntry *)psarc_find(&a->ps, key);
		if (e) { if (arch) *arch = a; return e; }
	}
	return 0;
}
static int open_archived(uint32_t path) {
	Archive *a; PsEntry *e = ps_find(path, &a, 0);
	if (!e) return 0;
	uint64_t t0 = fios_now_us();
	pthread_mutex_lock(&lock);
	uint8_t *mem = psarc_read(&a->ps, e);
	pthread_mutex_unlock(&lock);
	if (!mem) { fprintf(stderr, "    fios open %s: the archive entry couldn't be read\n", (char *)G(path)); return -1; }
	io_read_note((int64_t)e->size, fios_now_us() - t0, 1);   /* RECOMP_HITCH_LOG: an archive entry read and inflated at open (io.c) */
	uint32_t h = new_handle(0, 0, e->name);
	if (!h) { free(mem); return -1; }
	handles[h].mem = mem; handles[h].size = (int64_t)e->size;
	static int dbg = -1; if (dbg < 0) dbg = getenv("RECOMP_FIOS_DEBUG") != 0;
	if (dbg) fprintf(stderr, "    fios open %s -> archive (%llu bytes)\n", (char *)G(path), (unsigned long long)e->size);
	return (int)h;
}

/* SceFiosOpenParams: openFlags (low 16 bits): 1 read, 2 write, 4 append, 8 create, 16 truncate */
static int open_file(uint32_t path, uint32_t params) {
	int ah = open_archived(path);
	if (ah) return ah;
	char hp[1024]; fios_path(path, hp, sizeof hp);
	uint32_t fl = params ? RD32(params) & 0xffff : 1;
	int mode = (fl & 3) == 3 ? O_RDWR : (fl & 2) ? O_WRONLY : O_RDONLY;
	if (fl & 8) mode |= O_CREAT; if (fl & 16) mode |= O_TRUNC; if (fl & 4) mode |= O_APPEND;
	extern int guest_path_readonly(const char *);   /* (io.c: app0 is read-only, as on the console) */
	if ((mode & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) && guest_path_readonly(hp)) { fprintf(stderr, "    fios open for writing %s: refused (app0 is read-only)\n", (char *)G(path)); return -1; }
	int fd = open(hp, mode, 0644);
	fprintf(stderr, "    fios open %s -> %s%s\n", (char *)G(path), hp, fd < 0 ? " (not found)" : "");
	if (fd < 0) return -1;
	return (int)new_handle(fd + 1, 0, hp);   /* +1: fd 0 marks a free slot */
}
static void h_init(Cpu *c) { RET(0); }
static void h_fh_open_sync(Cpu *c) {   /* (attr, *fh, path, params) -> error */
	int h = open_file(A2, A3);
	if (A1) WR32(A1, h < 0 ? 0 : (uint32_t)h);
	RET(h < 0 ? FIOS_ERROR_BAD_PATH : 0);
}
static void h_fh_open(Cpu *c) {   /* (attr, *fh, path, params) -> op */
	int h = open_file(A2, A3);
	if (A1) WR32(A1, h < 0 ? 0 : (uint32_t)h);
	RET(op_done(h < 0 ? (int32_t)FIOS_ERROR_BAD_PATH : 0, 0, 0));
}
static int64_t do_read(uint32_t fh, uint32_t buf, int64_t len, int64_t off, int at) {
	Handle *h = handle(fh);
	if (!h) return (int32_t)FIOS_ERROR_BAD_HANDLE;
	if (h->mem) {
		int64_t from = at ? off : h->pos, n = from >= h->size ? 0 : (h->size - from < len ? h->size - from : len);
		if (n > 0) memcpy(G(buf), h->mem + from, (size_t)n);
		if (!at) h->pos = from + n;
		return n;
	}
	uint64_t t0 = fios_now_us();
	gwatch_write_begin(buf, (uint32_t)len);   /* a read(2) into a page the texture watch made read-only fails (EFAULT) instead of faulting: open it first */
	ssize_t n = at ? pread(h->fd - 1, G(buf), (size_t)len, (off_t)off) : read(h->fd - 1, G(buf), (size_t)len);
	gwatch_write_end(buf, (uint32_t)len);
	io_read_note(n, fios_now_us() - t0, 0);   /* RECOMP_HITCH_LOG's per-frame read stats (io.c) */
	static int dbg = -1; if (dbg < 0) dbg = getenv("RECOMP_FIOS_DEBUG") != 0;
	if (dbg) fprintf(stderr, "    fios read fh %u len %lld off %lld -> %zd\n", fh, (long long)len, (long long)off, n);
	return n < 0 ? (int32_t)FIOS_ERROR_BAD_HANDLE : n;
}
static void h_fh_read_sync(Cpu *c) { RET64(do_read(A1, A2, (int64_t)STK64(0), 0, 0)); }         /* (attr, fh, buf, len) */
static void h_fh_read(Cpu *c) { int64_t len = (int64_t)STK64(0); RET(op_done(do_read(A1, A2, len, 0, 0), A2, len)); }
static void h_fh_pread_sync(Cpu *c) { RET64(do_read(A1, A2, (int64_t)STK64(0), (int64_t)STK64(2), 1)); }   /* (attr, fh, buf, len, off) */
static void h_fh_pread(Cpu *c) { int64_t len = (int64_t)STK64(0); RET(op_done(do_read(A1, A2, len, (int64_t)STK64(2), 1), A2, len)); }
static int64_t do_write(uint32_t fh, uint32_t buf, int64_t len) {
	Handle *h = handle(fh);
	if (!h) return (int32_t)FIOS_ERROR_BAD_HANDLE;
	gwatch_read_open(buf, (uint32_t)len);   /* (a read-watched page can't be read by the kernel: guestwatch.h) */
	ssize_t n = write(h->fd - 1, G(buf), (size_t)len);
	return n < 0 ? (int32_t)FIOS_ERROR_BAD_HANDLE : n;
}
static void h_fh_write_sync(Cpu *c) { RET64(do_write(A1, A2, (int64_t)STK64(0))); }
static void h_fh_write(Cpu *c) { int64_t len = (int64_t)STK64(0); RET(op_done(do_write(A1, A2, len), A2, len)); }
static void h_fh_seek(Cpu *c) {   /* (fh, offset r2:r3, whence on the stack) -> new position */
	Handle *h = handle(A0);
	if (!h) { RET64((int32_t)FIOS_ERROR_BAD_HANDLE); return; }
	int64_t o = (int64_t)((uint64_t)A2 | (uint64_t)A3 << 32);
	if (h->mem) { int w = (int)STK(0); h->pos = w == 0 ? o : w == 1 ? h->pos + o : h->size + o; RET64(h->pos); return; }
	RET64(lseek(h->fd - 1, (off_t)o, (int)STK(0)));
}
static void h_fh_tell(Cpu *c) { Handle *h = handle(A0); RET64(!h ? -1 : h->mem ? h->pos : lseek(h->fd - 1, 0, SEEK_CUR)); }
static void h_fh_size(Cpu *c) { Handle *h = handle(A0); struct stat st; RET64(!h ? -1 : h->mem ? h->size : !fstat(h->fd - 1, &st) ? st.st_size : -1); }
static int close_handle(uint32_t fh) {
	Handle *h = handle(fh);
	if (!h) return -1;
	if (h->fd) close(h->fd - 1);
	if (h->dir) closedir(h->dir);
	free(h->mem);
	pthread_mutex_lock(&lock); memset(h, 0, sizeof *h); pthread_mutex_unlock(&lock);
	return 0;
}
static void h_fh_close_sync(Cpu *c) { RET(close_handle(A1) ? FIOS_ERROR_BAD_HANDLE : 0); }   /* (attr, fh) */
static void h_fh_close(Cpu *c) { RET(op_done(close_handle(A1) ? (int32_t)FIOS_ERROR_BAD_HANDLE : 0, 0, 0)); }
static void h_valid(Cpu *c) { RET(handle(A0) != 0); }

/* SceFiosStat: size (s64), access/modify/create dates (u64), stat flags (u32: 1 directory, 2 read-only) */
static void put_stat(uint32_t out, struct stat *st) {
	memset(G(out), 0, 64);
	WR64(out, (uint64_t)st->st_size);
	WR32(out + 32, S_ISDIR(st->st_mode) ? 1 : 0);
}
static void h_stat_sync(Cpu *c) {   /* (attr, path, stat) */
	char hp[1024]; struct stat st; fios_path(A1, hp, sizeof hp);
	PsEntry *e = ps_find(A1, 0, 0);
	if (e || ps_find(A1, 0, 1)) { if (A2) { memset(G(A2), 0, 64); if (e) WR64(A2, e->size); else WR32(A2 + 32, 1); } RET(0); return; }
	if (stat(hp, &st)) { RET(FIOS_ERROR_BAD_PATH); return; }
	if (A2) put_stat(A2, &st);
	RET(0);
}
static void h_fh_stat_sync(Cpu *c) { Handle *h = handle(A1); struct stat st;
	if (h && h->mem) { memset(G(A2), 0, 64); WR64(A2, (uint64_t)h->size); RET(0); return; }
	if (!h || fstat(h->fd - 1, &st)) { RET(FIOS_ERROR_BAD_HANDLE); return; } put_stat(A2, &st); RET(0); }
static int exists(uint32_t path, int dir) {
	if (ps_find(path, 0, dir)) return 1;
	char hp[1024]; struct stat st; fios_path(path, hp, sizeof hp);
	return !stat(hp, &st) && (dir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode));
}
static int64_t file_size(uint32_t path) {
	PsEntry *e = ps_find(path, 0, 0);
	if (e) return (int64_t)e->size;
	char hp[1024]; struct stat st; fios_path(path, hp, sizeof hp);
	return stat(hp, &st) || !S_ISREG(st.st_mode) ? (int32_t)FIOS_ERROR_BAD_PATH : st.st_size;
}
static void h_exists(Cpu *c, int dir) {   /* (attr, path) -> bool: the answer is the return value */
	char hp[1024]; fios_path(A1, hp, sizeof hp);
	int e = exists(A1, dir);
	fprintf(stderr, "    fios exists %s -> %s: %d\n", (char *)G(A1), hp, e);
	RET(e);
}
static void h_file_exists(Cpu *c) { h_exists(c, 0); }
/* the asynchronous forms: (attr, path, *out) -> op */
static void h_file_exists_op(Cpu *c) { int e = exists(A1, 0); if (A2) WR8(A2, (uint8_t)e); RET(op_done(0, 0, 0)); }
static void h_dir_exists_op(Cpu *c) { int e = exists(A1, 1); if (A2) WR8(A2, (uint8_t)e); RET(op_done(0, 0, 0)); }
static void h_file_size_op(Cpu *c) { int64_t n = file_size(A1); if (A2 && n >= 0) WR64(A2, (uint64_t)n); RET(op_done(n < 0 ? n : 0, 0, 0)); }
static void h_file_size_sync(Cpu *c) { RET64(file_size(A1)); }
/* (attr, path, buf, length (stack), offset (stack)) -> op / bytes read */
static int64_t file_read(uint32_t path, uint32_t buf, int64_t len, int64_t off) {
	int h = open_file(path, 0);
	if (h <= 0) return (int32_t)FIOS_ERROR_BAD_PATH;
	int64_t n = do_read((uint32_t)h, buf, len, off, 1);
	close_handle((uint32_t)h);
	return n;
}
static void h_file_read_op(Cpu *c) { int64_t len = (int64_t)STK64(0); RET(op_done(file_read(A1, A2, len, (int64_t)STK64(2)), A2, len)); }
static void h_file_read_sync(Cpu *c) { RET64(file_read(A1, A2, (int64_t)STK64(0), (int64_t)STK64(2))); }
/* archives: (attr, *fh, archive path, mount point, SceFiosBuffer {data, length} (stack), params) -> error */
static void h_archive_mount(Cpu *c) {
	char hp[1024]; fios_path(A2, hp, sizeof hp);
	char mp[256]; logical((char *)G(A3), mp, sizeof mp);
	pthread_mutex_lock(&lock); int k = ps_mount(hp, mp); pthread_mutex_unlock(&lock);
	if (A1) WR32(A1, k < 0 ? 0 : 0x7000u + (uint32_t)k);
	RET(k < 0 ? FIOS_ERROR_BAD_PATH : 0);
}
static void h_archive_buffer_size(Cpu *c) { RET64(0x10000); }   /* (attr, archive path, params): our index lives on the host */
static void h_dir_exists(Cpu *c) { h_exists(c, 1); }
static void h_file_delete(Cpu *c) {
	extern int guest_path_readonly(const char *);
	char hp[1024]; fios_path(A1, hp, sizeof hp);
	if (guest_path_readonly(hp)) { fprintf(stderr, "    fios delete %s: refused (app0 is read-only)\n", (char *)G(A1)); RET(FIOS_ERROR_BAD_PATH); return; }
	int r = unlink(hp); fprintf(stderr, "    fios delete %s -> %s%s\n", (char *)G(A1), hp, r ? " (failed)" : "");
	RET(r ? FIOS_ERROR_BAD_PATH : 0);
}
static void h_dir_create(Cpu *c) {
	extern int guest_path_readonly(const char *);
	char hp[1024]; fios_path(A1, hp, sizeof hp);
	if (guest_path_readonly(hp)) { RET(FIOS_ERROR_BAD_PATH); return; }
	mkdir(hp, 0755); RET(0);
}
/* directories: (attr, *dh, path, buf) / (attr, dh, SceFiosDirEntry *: s64 size, u32 stat flags, u16 name length, u16 ?, name[]) */
static void h_dh_open_sync(Cpu *c) {
	char hp[1024]; fios_path(A2, hp, sizeof hp);
	DIR *d = opendir(hp);
	if (!d) { RET(FIOS_ERROR_BAD_PATH); return; }
	uint32_t h = new_handle(0, d, hp);
	if (A1) WR32(A1, h);
	RET(0);
}
static void h_dh_read_sync(Cpu *c) {
	Handle *h = handle(A1);
	if (!h || !h->dir) { RET(FIOS_ERROR_BAD_HANDLE); return; }
	struct dirent *e;
	do e = readdir(h->dir); while (e && e->d_name[0] == '.');
	if (!e) { RET(0x80820001u); return; }   /* end of directory */
	char full[1280]; struct stat st; snprintf(full, sizeof full, "%s/%s", h->path, e->d_name);
	stat(full, &st);
	memset(G(A2), 0, 0x110);
	WR64(A2, (uint64_t)st.st_size); WR32(A2 + 8, S_ISDIR(st.st_mode) ? 1 : 0);
	WR16(A2 + 12, (uint16_t)strlen(e->d_name));
	snprintf((char *)G(A2 + 16), 256, "%s", e->d_name);
	RET(0);
}
static void h_dh_close(Cpu *c) { RET(op_done(close_handle(A1) ? (int32_t)FIOS_ERROR_BAD_HANDLE : 0, 0, 0)); }
static void h_dh_close_sync(Cpu *c) { RET(close_handle(A1) ? (uint32_t)FIOS_ERROR_BAD_HANDLE : 0); }   /* (attr, dh) -> error */

/* operations: every op completed when it was issued */
static void h_op_wait(Cpu *c) { RET(op(A0)->result < 0 ? (uint32_t)op(A0)->result : 0); }   /* -> error code */
static void h_op_sync_wait(Cpu *c) { RET64(op(A0)->result); }                                  /* -> result */
static void h_op_count(Cpu *c) { RET64(op(A0)->result > 0 ? op(A0)->result : 0); }
static void h_op_error(Cpu *c) { RET(op(A0)->result < 0 ? (uint32_t)op(A0)->result : 0); }
static void h_op_done(Cpu *c) { RET(1); }
static void h_op_buffer(Cpu *c) { RET(op(A0)->buf); }
static void h_op_request(Cpu *c) { RET64(op(A0)->req); }
static void h_zero(Cpu *c) { RET(0); }
static void h_one(Cpu *c) { RET(1); }
static void h_ns_interval(Cpu *c) { RET64((uint64_t)A0 | (uint64_t)A1 << 32); }   /* (s64 ns) -> interval: kept in ns */

const HleEntry hle_fios[] = {
	{"sceFiosInitialize", h_init}, {"sceFiosShutdownAndCancelOps", h_zero},
	{"sceFiosFHOpenSync", h_fh_open_sync}, {"sceFiosFHOpen", h_fh_open},
	{"sceFiosFHOpenWithModeSync", h_fh_open_sync},   /* (attr, *fh, path, params, nativeMode): the same open, the mode (permissions) ignored -- libc's fopen comes through here, and stubbed it "succeeded" with no handle */
	{"sceFiosFHReadSync", h_fh_read_sync}, {"sceFiosFHRead", h_fh_read},
	{"sceFiosFHPreadSync", h_fh_pread_sync}, {"sceFiosFHPread", h_fh_pread},
	{"sceFiosFHWriteSync", h_fh_write_sync}, {"sceFiosFHWrite", h_fh_write},
	{"sceFiosFHSeek", h_fh_seek}, {"sceFiosFHTell", h_fh_tell}, {"sceFiosFHGetSize", h_fh_size},
	{"sceFiosFHCloseSync", h_fh_close_sync}, {"sceFiosFHClose", h_fh_close}, {"sceFiosIsValidHandle", h_valid},
	{"sceFiosStatSync", h_stat_sync}, {"sceFiosFHStatSync", h_fh_stat_sync},
	{"sceFiosFileExistsSync", h_file_exists}, {"sceFiosDirectoryExistsSync", h_dir_exists},
	{"sceFiosFileDeleteSync", h_file_delete}, {"sceFiosDirectoryCreateSync", h_dir_create},
	{"sceFiosDHOpenSync", h_dh_open_sync}, {"sceFiosDHReadSync", h_dh_read_sync}, {"sceFiosDHClose", h_dh_close}, {"sceFiosDHCloseSync", h_dh_close_sync},
	{"sceFiosOpWait", h_op_wait}, {"sceFiosOpSyncWait", h_op_sync_wait}, {"sceFiosOpSyncWaitForIO", h_op_sync_wait},
	{"sceFiosOpGetActualCount", h_op_count}, {"sceFiosOpGetError", h_op_error}, {"sceFiosOpIsDone", h_op_done},
	{"sceFiosOpIsCancelled", h_zero}, {"sceFiosOpCancel", h_zero}, {"sceFiosOpDelete", h_zero},
	{"sceFiosOpGetBuffer", h_op_buffer}, {"sceFiosOpGetRequestCount", h_op_request},
	{"sceFiosGetAllOps", h_zero}, {"sceFiosGetAllFHs", h_zero}, {"sceFiosGetAllDHs", h_zero},
	{"sceFiosFileExists", h_file_exists_op}, {"sceFiosDirectoryExists", h_dir_exists_op},
	{"sceFiosFileGetSize", h_file_size_op}, {"sceFiosFileGetSizeSync", h_file_size_sync},
	{"sceFiosFileRead", h_file_read_op}, {"sceFiosFileReadSync", h_file_read_sync},
	{"sceFiosArchiveMountSync", h_archive_mount}, {"sceFiosArchiveUnmountSync", h_zero},
	{"sceFiosArchiveGetMountBufferSizeSync", h_archive_buffer_size}, {"sceFiosIOFilterAdd", h_zero}, {"sceFiosIOFilterRemove", h_zero},
	{"sceFiosTimeIntervalFromNanoseconds", h_ns_interval}, {"sceFiosUpdateParameters", h_zero}, {"sceFiosIsInitialized", h_one},
	{0, 0},
};
