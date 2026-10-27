/* Vita system calls on the host: file I/O (SceIofilemgr, at libkernel's sceIo* level) with Vita
 * paths mapped under RECOMP_FS, and the FIOS2 kernel's overlay resolver (SceFios2User). These are
 * the game's direct sceIo calls and, when a game's conf loads libfios2 (Sony's FIOS as recompiled
 * code), everything FIOS does: its scheduler threads end in exactly these calls. Errors are the
 * kernel's: SCE_ERROR_ERRNO_* (0x80010000 | the newlib errno), so FIOS turns them into its own
 * SCE_FIOS_ERROR_* codes the way it does on the console. */
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* (strcasecmp) */
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "hle.h"
#include "guestwatch.h"

/* ---- files: a Vita path "dev:rest" is <RECOMP_FS>/dev/rest on the host (app0: is the game's
 * install folder, ux0:data its data); "/dev/rest" (FIOS's own spelling) the same; a relative path
 * is the app's folder (fios.c maps them the same way) */
/* The console's file systems ignore case, as macOS's and Windows' do; Linux's don't. A path that isn't there as spelled
 * is matched one name at a time against what each folder holds, ignoring case, as far as the folders go (a file about
 * to be created keeps its own name). Killzone asks for game_data/MOVIES_KZV/..., its folder is Movies_KZV: on Linux its
 * title screen played no video and showed no logo. Only misses pay for it. */
void host_path_fold(char *buf, size_t root_len) {
	struct stat st;
	if (!stat(buf, &st) || root_len >= strlen(buf)) return;
	char *q = buf + root_len; while (*q == '/') q++;
	while (*q) {
		char *end = strchr(q, '/'), save = end ? *end : 0;
		if (end) *end = 0;
		if (stat(buf, &st)) {   /* this name isn't there as spelled: its folder's entry that matches it ignoring case */
			char *sl = q - 1; *sl = 0;
			DIR *d = opendir(buf); *sl = '/';
			struct dirent *e; int found = 0;
			if (d) { while ((e = readdir(d))) if (!strcasecmp(e->d_name, q) && strlen(e->d_name) == strlen(q)) { memcpy(q, e->d_name, strlen(q)); found = 1; break; } closedir(d); }
			if (!found) { if (end) *end = save; return; }
		}
		if (!end) return;
		*end = save; q = end + 1; while (*q == '/') q++;
	}
}
static const char *host_path_s(const char *p, char *buf, size_t n) {
	const char *root = getenv("RECOMP_FS") ? getenv("RECOMP_FS") : "fs";
	if (*p != '/' && !strchr(p, ':')) { snprintf(buf, n, "%s/app0/%s", root, p); return buf; }
	while (*p == '/') p++;
	const char *colon = strchr(p, ':'), *slash = strchr(p, '/');
	if (colon && (!slash || colon < slash)) {
		const char *rest = colon + 1; while (*rest == '/') rest++;
		snprintf(buf, n, "%s/%.*s/%s", root, (int)(colon - p), p, rest);
	} else snprintf(buf, n, "%s/%s", root, p);
	for (char *q = buf + strlen(root); *q; q++) if (*q == '\\') *q = '/';   /* a separator on the console (exFAT forbids it in names): Uncharted saves "slot_000/saves\checkpoint.bin" */
	host_path_fold(buf, strlen(root));
	return buf;
}
static const char *host_path(uint32_t gp, char *buf, size_t n) { return host_path_s((char *)G(gp), buf, n); }
#define SCE_ENOENT 0x80010002u
#define SCE_EBADF 0x80010009u
#define SCE_EINVAL 0x80010016u
#define SCE_ENAMETOOLONG 0x8001005Bu
#define SCE_ENOTSUP 0x80010086u
/* a host errno as the kernel's error: newlib's numbering where it differs from the host's (the
 * BSD/Linux values agree with newlib up to ERANGE; ENOTEMPTY and ENAMETOOLONG don't) */
static uint32_t sce_err(int e) {
	switch (e) {
	case ENOTEMPTY: return 0x8001005Au;
	case ENAMETOOLONG: return SCE_ENAMETOOLONG;
	case ELOOP: return 0x8001005Cu;
	case ENOTSUP: return SCE_ENOTSUP;
	default: return e > 0 && e <= 34 ? 0x80010000u | (uint32_t)e : 0x80010005u;   /* EIO for the rest */
	}
}
/* app0: is mounted read-only on the console: a game can't write, create, delete, rename or make
 * folders there. Here it is the player's own copy of the game, so the same holds, and every delete
 * or rename the game asks for anywhere is logged. (Uncharted's whole app0 vanished during a gate
 * run on 2026-10-13, cause not found.) A host path is in app0 when it, or for one that doesn't
 * exist yet its folder, resolves under <RECOMP_FS>/app0 -- links and ".." included. */
int guest_path_readonly(const char *hp) {
	static char app0[PATH_MAX]; static int have = -1;
	if (have < 0) { char p[1024]; snprintf(p, sizeof p, "%s/app0", getenv("RECOMP_FS") ? getenv("RECOMP_FS") : "fs"); have = realpath(p, app0) != 0; }
	if (!have) return 0;
	char rp[PATH_MAX];
	if (!realpath(hp, rp)) {
		char dir[1024]; snprintf(dir, sizeof dir, "%s", hp);
		char *sl = strrchr(dir, '/'); if (!sl) return 0;
		*sl = 0; if (!realpath(dir, rp)) return 0;
	}
	size_t n = strlen(app0);
	return !strncmp(rp, app0, n) && (rp[n] == 0 || rp[n] == '/');
}
static int readonly_refused(const char *what, const char *gp, const char *hp) {
	if (!guest_path_readonly(hp)) return 0;
	fprintf(stderr, "    %s %s: refused (app0 is read-only)\n", what, gp);
	return 1;
}
/* A file's descriptor is its host fd + 0x100, for the host fds handed out here (and kernel.c's standard streams):
 * the others are refused (EBADF). A handle closed twice used to close whatever host descriptor had reused the number
 * since: Killzone's sound streamer closes one of its files twice, and the second close took the room server's
 * connection to another player with it (a Vita's second close just fails). */
static uint8_t owned[4096];
uint32_t io_guest_fd(int fd) { if (fd >= 0 && fd < 4096) __atomic_store_n(&owned[fd], 1, __ATOMIC_RELEASE); return (uint32_t)fd + 0x100; }
/* RECOMP_ADDCONT_MARKERS="<add-on>/<file> ...": files a game opens to tell whether an add-on is owned, and never reads.
 * Every add-on counts as owned (apputil.c h_drm_open); a game that checks one by such a file gets the file, made empty
 * when it's missing. Unit 13's Network Pass: addcont0:UNIT13ONLINEPASS/unlock.dat, opened and closed at once */
static void addcont_marker(const char *gp, const char *hp) {
	const char *m = getenv("RECOMP_ADDCONT_MARKERS");
	if (!m || strncasecmp(gp, "addcont0:", 9)) return;
	const char *rest = gp + 9; while (*rest == '/') rest++;
	const size_t n = strlen(rest);
	for (const char *p = m; *p; ) {
		while (*p == ' ' || *p == ',') p++;
		const size_t l = strcspn(p, " ,");
		if (l && l == n && !strncasecmp(p, rest, n)) {
			struct stat st; if (!stat(hp, &st)) return;
			char d[1024]; snprintf(d, sizeof d, "%s", hp);
			for (char *q = d + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(d, 0755); *q = '/'; }
			const int fd = open(hp, O_WRONLY | O_CREAT, 0644); if (fd >= 0) close(fd);
			fprintf(stderr, "    addcont: %s counts as owned (%s, empty)\n", gp, hp);
			return;
		}
		p += l;
	}
}
static void h_io_open(Cpu *c) {   /* (path, SCE_O_* flags, mode) -> fd */
	/* the console's TTYs (the C library's stdin/stdout/stderr): the host's standard streams */
	const char *gp = (char *)G(A0);
	if (!strncmp(gp, "tty", 3) && gp[3] >= '0' && gp[3] <= '9' && gp[4] == ':') { RET(io_guest_fd(dup(gp[3] == '0' && (A1 & 3) == 1 ? 0 : gp[3] == '0' ? 1 : 2))); return; }
	char hp[1024]; host_path(A0, hp, sizeof hp);
	int fl = A1, mode = O_RDONLY;
	if ((fl & 3) == 2) mode = O_WRONLY; else if ((fl & 3) == 3) mode = O_RDWR;
	if (fl & 0x200) mode |= O_CREAT; if (fl & 0x400) mode |= O_TRUNC; if (fl & 0x100) mode |= O_APPEND; if (fl & 0x800) mode |= O_EXCL;
	if ((mode & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) && readonly_refused("open for writing", gp, hp)) { RET(sce_err(EROFS)); return; }
	if (mode == O_RDONLY) addcont_marker(gp, hp);
	int fd = open(hp, mode, 0644), e = errno;
	fprintf(stderr, "    open %s -> %s%s (fd %#x)\n", (char *)G(A0), hp, fd < 0 ? " (not found)" : "", fd < 0 ? 0 : (uint32_t)fd + 0x100);
	RET(fd < 0 ? sce_err(e) : io_guest_fd(fd));
}
static int hfd(uint32_t fd) { const int h = fd >= 0x100 && fd < 0x100 + 4096 ? (int)(fd - 0x100) : -1; return h >= 0 && __atomic_load_n(&owned[h], __ATOMIC_ACQUIRE) ? h : -1; }
static void h_io_close(Cpu *c) {
	const int h = hfd(A0);
	if (h < 0 || !__atomic_exchange_n(&owned[h], 0, __ATOMIC_ACQ_REL)) { fprintf(stderr, "    close fd %#x: not open (closed twice?)\n", A0); RET(SCE_EBADF); return; }   /* (nothing of ours to close) */
	RET(close(h) < 0 ? SCE_EBADF : 0);
}
/* the kernel's copy into guest memory doesn't fault on a page the texture watch made read-only, it
 * fails (EFAULT): the destination is opened around the call (guestwatch.h) */
/* RECOMP_HITCH_LOG (display.c): the guest's file reads -- calls, bytes and time in read(2)/pread(2) (here and
 * fios.c), and archive entries inflated at open (fios.c's PSARC path) -- taken per presented frame */
static uint64_t io_rd_bytes, io_rd_us, io_ar_bytes, io_ar_us; static uint32_t io_rd_calls, io_ar_calls;
void io_read_note(int64_t n, uint64_t us, int archive) {
	if (archive) { if (n > 0) __atomic_add_fetch(&io_ar_bytes, (uint64_t)n, __ATOMIC_RELAXED); __atomic_add_fetch(&io_ar_us, us, __ATOMIC_RELAXED); __atomic_add_fetch(&io_ar_calls, 1, __ATOMIC_RELAXED); }
	else { if (n > 0) __atomic_add_fetch(&io_rd_bytes, (uint64_t)n, __ATOMIC_RELAXED); __atomic_add_fetch(&io_rd_us, us, __ATOMIC_RELAXED); __atomic_add_fetch(&io_rd_calls, 1, __ATOMIC_RELAXED); }
}
void io_read_stats_take(uint64_t *rd_bytes, uint32_t *rd_calls, uint64_t *rd_us, uint64_t *ar_bytes, uint32_t *ar_calls, uint64_t *ar_us) {
	*rd_bytes = __atomic_exchange_n(&io_rd_bytes, 0, __ATOMIC_RELAXED); *rd_calls = __atomic_exchange_n(&io_rd_calls, 0, __ATOMIC_RELAXED); *rd_us = __atomic_exchange_n(&io_rd_us, 0, __ATOMIC_RELAXED);
	*ar_bytes = __atomic_exchange_n(&io_ar_bytes, 0, __ATOMIC_RELAXED); *ar_calls = __atomic_exchange_n(&io_ar_calls, 0, __ATOMIC_RELAXED); *ar_us = __atomic_exchange_n(&io_ar_us, 0, __ATOMIC_RELAXED);
}
static uint64_t io_now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
static void h_io_read(Cpu *c) { uint64_t t0 = io_now_us(); gwatch_write_begin(A1, A2); ssize_t n = read(hfd(A0), G(A1), A2); int e = errno; gwatch_write_end(A1, A2); io_read_note(n, io_now_us() - t0, 0); RET(n < 0 ? sce_err(e) : (uint32_t)n); }
static void h_io_write(Cpu *c) { gwatch_read_open(A1, A2); ssize_t n = write(hfd(A0), G(A1), A2); RET(n < 0 ? sce_err(errno) : (uint32_t)n); }
static void h_io_pread(Cpu *c) {   /* (fd, buf, size, SceOff offset: 8-aligned, so on the stack) */
	uint64_t off = RD64(c->r[13]);
	uint64_t t0 = io_now_us(); gwatch_write_begin(A1, A2); ssize_t n = pread(hfd(A0), G(A1), A2, (off_t)off); int e = errno; gwatch_write_end(A1, A2); io_read_note(n, io_now_us() - t0, 0); RET(n < 0 ? sce_err(e) : (uint32_t)n);
}
static void h_io_pwrite(Cpu *c) { gwatch_read_open(A1, A2); ssize_t n = pwrite(hfd(A0), G(A1), A2, (off_t)RD64(c->r[13])); RET(n < 0 ? sce_err(errno) : (uint32_t)n); }   /* as pread */
static void h_io_lseek(Cpu *c) {   /* (fd, SceOff offset in r2:r3, whence on the stack) -> SceOff */
	off_t o = lseek(hfd(A0), (off_t)((uint64_t)A2 | (uint64_t)A3 << 32), (int)STK(0));
	if (o < 0) { c->r[0] = sce_err(errno); c->r[1] = 0xffffffffu; return; }   /* a negative SceOff: the error, sign-extended */
	c->r[0] = (uint32_t)o; c->r[1] = (uint32_t)((uint64_t)o >> 32);
}
static void h_io_lseek32(Cpu *c) { RET(lseek(hfd(A0), (int32_t)A1, (int)A2)); }
/* SceDateTime: u16 year, month, day, hour, minute, second; u32 microsecond (UTC) */
static void put_date(uint32_t a, time_t t, long ns) {
	struct tm tm; gmtime_r(&t, &tm);
	WR16(a, (uint16_t)(tm.tm_year + 1900)); WR16(a + 2, (uint16_t)(tm.tm_mon + 1)); WR16(a + 4, (uint16_t)tm.tm_mday);
	WR16(a + 6, (uint16_t)tm.tm_hour); WR16(a + 8, (uint16_t)tm.tm_min); WR16(a + 10, (uint16_t)tm.tm_sec); WR32(a + 12, (uint32_t)(ns / 1000));
}
static time_t get_date(uint32_t a) {
	struct tm tm = {0};
	tm.tm_year = RD16(a) - 1900; tm.tm_mon = RD16(a + 2) - 1; tm.tm_mday = RD16(a + 4);
	tm.tm_hour = RD16(a + 6); tm.tm_min = RD16(a + 8); tm.tm_sec = RD16(a + 10);
	return timegm(&tm);
}
#ifdef __APPLE__
#define ST_TIME(s, w) (s)->st_##w##timespec
#elif defined(_WIN32)
#define ST_TIME(s, w) ((struct timespec){ (s)->st_##w##time, 0 })   /* (whole seconds) */
#else
#define ST_TIME(s, w) (s)->st_##w##tim
#endif
/* SceIoStat (0x58): st_mode (SCE_S_IFDIR 0x1000 / SCE_S_IFREG 0x2000 | the user rwx bits), st_attr
 * (SCE_SO_IFDIR 0x10 / SCE_SO_IFREG 0x20 | the hidden rwx bits), size, then the creation, access
 * and modification times */
static void put_stat(uint32_t st, struct stat *s) {
	int dir = S_ISDIR(s->st_mode);
	memset(G(st), 0, 0x58);
	WR32(st, (dir ? 0x1000 : 0x2000) | (s->st_mode & 0777));
	WR32(st + 4, (dir ? 0x10 : 0x20) | (s->st_mode & 7));
	WR64(st + 8, dir ? 0 : (uint64_t)s->st_size);
	put_date(st + 16, ST_TIME(s, c).tv_sec, ST_TIME(s, c).tv_nsec);
	put_date(st + 32, ST_TIME(s, a).tv_sec, ST_TIME(s, a).tv_nsec);
	put_date(st + 48, ST_TIME(s, m).tv_sec, ST_TIME(s, m).tv_nsec);
}
static void h_io_getstat(Cpu *c) {
	char hp[1024]; struct stat s; host_path(A0, hp, sizeof hp);
	if (stat(hp, &s) < 0) { int e = errno; fprintf(stderr, "    stat %s (not found)\n", (char *)G(A0)); RET(sce_err(e)); return; }
	put_stat(A1, &s); RET(0);
}
static void h_io_getstat_fd(Cpu *c) {
	struct stat s;
	if (fstat(hfd(A0), &s) < 0) { RET(SCE_EBADF); return; }
	put_stat(A1, &s); RET(0);
}
/* (path, SceIoStat *, SCE_CST_* bits): MODE 1 (the user rwx bits), SIZE 4 (truncate: FIOS's
 * FileTruncateSync), AT 0x10 / MT 0x20 (times); the creation time (CT 8) the host can't set */
static void h_io_chstat(Cpu *c) {
	char hp[1024]; host_path(A0, hp, sizeof hp);
	uint32_t st = A1, bits = A2;
	struct stat s;
	if (stat(hp, &s) < 0) { RET(sce_err(errno)); return; }
	if ((bits & 0x35) && readonly_refused("chstat", (char *)G(A0), hp)) { RET(sce_err(EROFS)); return; }
	if (bits & 1 && chmod(hp, (s.st_mode & ~0777) | (RD32(st) & 0700) | 0044) < 0) { RET(sce_err(errno)); return; }
	if (bits & 4 && truncate(hp, (off_t)RD64(st + 8)) < 0) { RET(sce_err(errno)); return; }
	if (bits & 0x30) {
		struct timeval tv[2] = {{ST_TIME(&s, a).tv_sec, 0}, {ST_TIME(&s, m).tv_sec, 0}};
		if (bits & 0x10) tv[0].tv_sec = get_date(st + 32);
		if (bits & 0x20) tv[1].tv_sec = get_date(st + 48);
		if (utimes(hp, tv) < 0) { RET(sce_err(errno)); return; }
	}
	RET(0);
}
static void h_io_mkdir(Cpu *c) { char hp[1024]; host_path(A0, hp, sizeof hp); if (readonly_refused("mkdir", (char *)G(A0), hp)) { RET(sce_err(EROFS)); return; } RET(mkdir(hp, 0755) < 0 ? sce_err(errno) : 0); }   /* (path, mode) */
static void h_io_rmdir(Cpu *c) {
	char hp[1024]; host_path(A0, hp, sizeof hp);
	if (readonly_refused("rmdir", (char *)G(A0), hp)) { RET(sce_err(EROFS)); return; }
	int r = rmdir(hp); fprintf(stderr, "    rmdir %s -> %s%s\n", (char *)G(A0), hp, r < 0 ? " (failed)" : "");
	RET(r < 0 ? sce_err(errno) : 0);
}
static void h_io_remove(Cpu *c) {
	char hp[1024]; host_path(A0, hp, sizeof hp);
	if (readonly_refused("remove", (char *)G(A0), hp)) { RET(sce_err(EROFS)); return; }
	int r = unlink(hp); fprintf(stderr, "    remove %s -> %s%s\n", (char *)G(A0), hp, r < 0 ? " (failed)" : "");
	RET(r < 0 ? sce_err(errno) : 0);
}
static void h_io_rename(Cpu *c) {   /* (old, new): both on one device, as the kernel requires (EXDEV otherwise) */
	char a[1024], b[1024]; host_path(A0, a, sizeof a); host_path(A1, b, sizeof b);
	if (readonly_refused("rename from", (char *)G(A0), a) || readonly_refused("rename to", (char *)G(A1), b)) { RET(sce_err(EROFS)); return; }
	int r = rename(a, b); fprintf(stderr, "    rename %s -> %s%s\n", (char *)G(A0), (char *)G(A1), r < 0 ? " (failed)" : "");
	RET(r < 0 ? sce_err(errno) : 0);
}
static void h_io_sync(Cpu *c) { sync(); RET(0); }                                          /* (device, flag) */
static void h_io_sync_fd(Cpu *c) { RET(fsync(hfd(A0)) < 0 ? sce_err(errno) : 0); }         /* (fd, flag) */
/* devctl (device, cmd, in, inlen, out (stack), outlen (stack)): 0x3001 is the free-space query
 * (SceIoDevInfo: max size, free size, cluster size) -- the host reports a roomy card; ioctl
 * (fd, cmd, ...): no command of a plain file is emulated. Both say what they were asked. */
static void h_io_devctl(Cpu *c) {
	uint32_t out = STK(0), outlen = STK(1);
	if (A1 == 0x3001 && out && outlen >= 0x18) {
		memset(G(out), 0, outlen); WR64(out, 32ull << 30); WR64(out + 8, 16ull << 30); WR32(out + 16, 32768);
		RET(0); return;
	}
	fprintf(stderr, "    devctl %s cmd %#x: not emulated\n", A0 ? (char *)G(A0) : "?", A1);
	RET(SCE_ENOTSUP);
}
static void h_io_ioctl(Cpu *c) { fprintf(stderr, "    ioctl fd %#x cmd %#x: not emulated\n", A0, A1); RET(SCE_ENOTSUP); }

/* ---- directories: the descriptor is a slot of dirs[] + 0x100, as a file's is its host fd + 0x100 (a slot, not
 * dirfd: Windows' DIR has no descriptor) */
static DIR *dirs[4096]; static char *dir_path[4096];   /* (the path: entries are stat()ed by it) */
static pthread_mutex_t dir_lock = PTHREAD_MUTEX_INITIALIZER;
static void h_io_dopen(Cpu *c) {   /* (path) -> dfd */
	char hp[1024]; host_path(A0, hp, sizeof hp);
	DIR *d = opendir(hp);
	if (!d) { RET(sce_err(errno)); return; }
	pthread_mutex_lock(&dir_lock);
	int fd = 3; while (fd < 4096 && dirs[fd]) fd++;
	if (fd < 4096) { dirs[fd] = d; free(dir_path[fd]); dir_path[fd] = strdup(hp); }
	pthread_mutex_unlock(&dir_lock);
	if (fd >= 4096) { closedir(d); RET(0x80010018u); return; }   /* EMFILE */
	RET((uint32_t)fd + 0x100);
}
static int dslot(uint32_t dfd) { return dfd >= 0x100 && dfd < 0x100 + 4096 ? (int)(dfd - 0x100) : -1; }
static DIR *dir_of(uint32_t dfd) { int fd = dslot(dfd); return fd >= 0 ? dirs[fd] : 0; }
/* (dfd, SceIoDirent *: SceIoStat, name[256], d_private, dummy) -> 1 an entry, 0 the end ("." and
 * ".." aren't entries on the Vita's file systems) */
static void h_io_dread(Cpu *c) {
	DIR *d = dir_of(A0);
	if (!d) { RET(SCE_EBADF); return; }
	struct dirent *e;
	do e = readdir(d); while (e && (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")));
	if (!e) { RET(0); return; }
	struct stat s; char ep[2048]; const char *dp = dir_path[dslot(A0)];
	snprintf(ep, sizeof ep, "%s/%s", dp ? dp : ".", e->d_name);
	if (stat(ep, &s) < 0) memset(&s, 0, sizeof s);
	uint32_t priv = RD32(A1 + 0x158);   /* d_private is the caller's (an input on some devices): kept */
	put_stat(A1, &s);
	memset(G(A1 + 0x58), 0, 256); snprintf((char *)G(A1 + 0x58), 256, "%s", e->d_name);
	WR32(A1 + 0x158, priv);
	RET(1);
}
static void h_io_dclose(Cpu *c) {
	DIR *d = dir_of(A0);
	if (!d) { RET(SCE_EBADF); return; }
	pthread_mutex_lock(&dir_lock); dirs[dslot(A0)] = 0; pthread_mutex_unlock(&dir_lock);
	closedir(d); RET(0);
}

/* ---- the FIOS2 kernel's overlays (SceFios2Kernel's _sceFiosKernelOverlay* system calls, which the
 * libfios2 Unit 13 and Uncharted ship imports directly; a newer libfios2 -- Shovel Knight's,
 * Killzone's, the firmware's own -- calls SceFios2User's sceFiosOverlay*02 wrappers of the same
 * calls, which take the arguments flat). An overlay maps one path prefix (dst, the path the game
 * uses) onto another (src, where the files are) for a process; libfios2 resolves every path
 * through the table before it touches the file system. Types (SceFiosOverlayType): OPAQUE 0 --
 * every access goes to src; TRANSLUCENT 1 -- src when the file exists there, else dst; NEWER 2 --
 * the newer of the two (the host takes src when it exists); WRITABLE 3 -- translucent for reads,
 * src for writes. Lower orders resolve first; a resolve-with-range applies only the orders in
 * [min, max]. Killzone: Mercenary adds two: "cache:" -> its LocalCacheNGP folder (the asset lumps
 * the PSARC serves under that folder are opened as "cache:lumps/<name>.core") -- refused, its
 * asset lookup returns null and the game calls through a poisoned vtable. Error codes are FIOS's
 * (SCE_FIOS_ERROR_*: 0x80820000 + their order in libfios2's own name table: UNIMPLEMENTED 0,
 * BAD_PTR 6, PATH_TOO_LONG 0x18, BAD_OVERLAY 0x1a) and the kernel's copy checks' 0x8002710b, as
 * the firmware's SceFios2Kernel returns them. */
#define FIOS_UNIMPLEMENTED 0x80820000u
#define FIOS_BAD_PTR 0x80820006u
#define FIOS_PATH_TOO_LONG 0x80820018u
#define FIOS_BAD_OVERLAY 0x8082001Au
#define KERNEL_ILLEGAL_SIZE 0x8002710Bu
#define OVERLAY_POINT_MAX 292   /* SCE_FIOS_OVERLAY_POINT_MAX: dst and src are char[292] */
/* SceFiosOverlay: u8 type, u8 order, u16 dst_len, u16 src_len, u16 -, SceUID pid, id, char dst[292], src[292] (600 bytes) */
typedef struct { uint8_t type, order; int32_t id; char dst[OVERLAY_POINT_MAX], src[OVERLAY_POINT_MAX]; } Overlay;
#define MAX_OVERLAYS 64
static Overlay overlays[MAX_OVERLAYS];
static int n_overlays, next_overlay_id = 1;
static pthread_mutex_t overlay_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int overlays_disabled;   /* per thread, as the kernel keeps it (thread-local state) */
static uint32_t overlay_read(uint32_t gp, Overlay *o) {
	if (!gp) return FIOS_BAD_PTR;
	const uint8_t *b = G(gp);
	o->type = b[0]; o->order = b[1]; o->id = (int32_t)RD32(gp + 12);
	if (o->type > 3) return FIOS_BAD_OVERLAY;
	if (strnlen((char *)b + 16, OVERLAY_POINT_MAX) >= OVERLAY_POINT_MAX || strnlen((char *)b + 16 + OVERLAY_POINT_MAX, OVERLAY_POINT_MAX) >= OVERLAY_POINT_MAX) return FIOS_PATH_TOO_LONG;
	snprintf(o->dst, sizeof o->dst, "%s", (char *)b + 16); snprintf(o->src, sizeof o->src, "%s", (char *)b + 16 + OVERLAY_POINT_MAX);
	if (!o->dst[0] || !o->src[0]) return FIOS_BAD_OVERLAY;
	return 0;
}
static void overlay_write(uint32_t gp, const Overlay *o) {
	uint8_t *b = G(gp); memset(b, 0, 16 + 2 * OVERLAY_POINT_MAX);
	b[0] = o->type; b[1] = o->order;
	WR16(gp + 2, (uint16_t)strlen(o->dst)); WR16(gp + 4, (uint16_t)strlen(o->src)); WR32(gp + 12, (uint32_t)o->id);
	memcpy(b + 16, o->dst, strlen(o->dst)); memcpy(b + 16 + OVERLAY_POINT_MAX, o->src, strlen(o->src));
}
static Overlay *overlay_find(int32_t id) { for (int i = 0; i < n_overlays; i++) if (overlays[i].id == id) return &overlays[i]; return 0; }
/* add (overlay, out id) -- the table keeps its entries sorted by order, insertion order within one */
static uint32_t overlay_add(uint32_t ovp, uint32_t idp) {
	Overlay o; uint32_t e = overlay_read(ovp, &o);
	if (e) { fprintf(stderr, "    fios overlay add: refused (%#x)\n", e); return e; }
	pthread_mutex_lock(&overlay_lock);
	if (n_overlays == MAX_OVERLAYS) { pthread_mutex_unlock(&overlay_lock); return FIOS_BAD_OVERLAY; }
	int at = n_overlays; while (at > 0 && overlays[at - 1].order > o.order) at--;
	memmove(&overlays[at + 1], &overlays[at], (size_t)(n_overlays - at) * sizeof o);
	o.id = next_overlay_id++; overlays[at] = o; n_overlays++;
	pthread_mutex_unlock(&overlay_lock);
	static const char *const types[] = {"opaque", "translucent", "newer", "writable"};
	fprintf(stderr, "    fios overlay %d: %s order %d: %s -> %s\n", o.id, types[o.type], o.order, o.dst, o.src);
	if (idp) WR32(idp, (uint32_t)o.id);
	return 0;
}
static uint32_t overlay_modify(uint32_t ovp) {
	Overlay o; uint32_t e = overlay_read(ovp, &o);
	if (e) return e;
	pthread_mutex_lock(&overlay_lock);
	Overlay *t = overlay_find(o.id);
	if (t) { *t = o; }   /* (a changed order is not re-sorted; no game of ours modifies one) */
	pthread_mutex_unlock(&overlay_lock);
	return t ? 0 : FIOS_BAD_OVERLAY;
}
static uint32_t overlay_remove(int32_t id) {
	pthread_mutex_lock(&overlay_lock);
	Overlay *t = overlay_find(id);
	if (t) { memmove(t, t + 1, (size_t)(&overlays[n_overlays] - (t + 1)) * sizeof *t); n_overlays--; }
	pthread_mutex_unlock(&overlay_lock);
	return t ? 0 : FIOS_BAD_OVERLAY;
}
static uint32_t overlay_info(int32_t id, uint32_t out) {
	if (!out) return FIOS_BAD_PTR;
	pthread_mutex_lock(&overlay_lock);
	Overlay *t = overlay_find(id);
	if (t) overlay_write(out, t);
	pthread_mutex_unlock(&overlay_lock);
	return t ? 0 : FIOS_BAD_OVERLAY;
}
/* the ids of the overlays whose order lies in [lo, hi], up to max of them; the count found */
static uint32_t overlay_list(int lo, int hi, uint32_t out, uint32_t max, uint32_t countp) {
	uint32_t n = 0;
	pthread_mutex_lock(&overlay_lock);
	for (int i = 0; i < n_overlays; i++) if (overlays[i].order >= lo && overlays[i].order <= hi) { if (out && n < max) WR32(out + 4 * n, (uint32_t)overlays[i].id); n++; }
	pthread_mutex_unlock(&overlay_lock);
	if (countp) WR32(countp, n);
	return 0;
}
/* does the overlay's dst prefix match path at a path-component boundary? (the prefix ends in ':' or
 * '/', or the path continues with '/' or ends) */
/* a path in the kernel's canonical spelling: no slashes right after the device's colon and no
 * doubled slashes ("app0:/game_data//x" and "app0:game_data/x" are the same file; Killzone adds its
 * overlays in the second spelling and opens in the first) */
static void path_canon(const char *in, char *out, size_t cap) {
	size_t n = 0; int after_colon = 0;
	for (const char *p = in; *p && n + 1 < cap; p++) {
		if (*p == '/' && (after_colon || (n && out[n - 1] == '/'))) continue;
		after_colon = *p == ':' && p == strchr(in, ':');   /* only the device's colon */
		out[n++] = *p;
	}
	out[n] = 0;
}
static int overlay_matches(const Overlay *o, const char *path, size_t *dl) {
	char d[OVERLAY_POINT_MAX]; path_canon(o->dst, d, sizeof d);
	*dl = strlen(d);
	if (strncmp(path, d, *dl)) return 0;
	char last = d[*dl - 1], next = path[*dl];
	return last == ':' || last == '/' || next == '/' || next == 0;
}
/* path, resolved through the overlays of orders [lo, hi] in order (an opaque or writable-for-write
 * overlay rewrites unconditionally; the others when the file exists under src); the rewritten
 * path is what the later overlays see */
static void overlay_apply(char *path, size_t cap, int write, int lo, int hi) {
	if (overlays_disabled || !n_overlays) return;
	char in[0x400]; snprintf(in, sizeof in, "%s", path);
	path_canon(in, path, cap);
	pthread_mutex_lock(&overlay_lock);
	for (int i = 0; i < n_overlays; i++) {
		const Overlay *o = &overlays[i]; size_t dl;
		if (o->order < lo || o->order > hi || !overlay_matches(o, path, &dl)) continue;
		const char *rest = path + dl; size_t sl = strlen(o->src);
		char cand[0x400];
		int join = rest[0] && rest[0] != '/' && o->src[sl - 1] != '/' && o->src[sl - 1] != ':';   /* "cache:" + "lumps/x" needs no slash; "ux0:data/kz" + "lumps/x" does */
		if (!rest[0] || (rest[0] == '/' && (o->src[sl - 1] == '/' || o->src[sl - 1] == ':'))) { if (rest[0] == '/') rest++; }
		snprintf(cand, sizeof cand, "%s%s%s", o->src, join ? "/" : "", rest);
		int use = o->type == 0 || (o->type == 3 && write);
		if (!use) { char hp[1024]; struct stat st; use = stat(host_path_s(cand, hp, sizeof hp), &st) == 0; }
		if (use) snprintf(path, cap, "%s", cand);
	}
	pthread_mutex_unlock(&overlay_lock);
	if (strcmp(in, path) && getenv("RECOMP_LOG_FIOS")) fprintf(stderr, "    fios resolve %s -> %s\n", in, path);
}
/* (pid, resolve for write, in path, SceFiosResolveWithRangeSyncSyscallArgs *: out path, max path,
 * min order (u8 at 8), max order (u8 at 9), the in path's size at 0xc, the out buffer's size at 0x10) */
static uint32_t resolve(uint32_t in, uint32_t out, uint32_t max, uint32_t outsize, int write, int lo, int hi) {
	if (in && strnlen((char *)G(in), 0x400) == 0x400) return KERNEL_ILLEGAL_SIZE;   /* the kernel's 0x400-byte copy of it */
	if (outsize > 0x400) return KERNEL_ILLEGAL_SIZE;
	if (!in || !out) return FIOS_BAD_PTR;
	size_t n = strlen((char *)G(in));
	if (n >= max) return FIOS_PATH_TOO_LONG;
	char buf[0x400] = {0}; memcpy(buf, G(in), n);   /* the kernel resolves into a zeroed 0x400 buffer and copies out the caller's size */
	overlay_apply(buf, sizeof buf, write, lo, hi);
	if (strlen(buf) >= max) return FIOS_PATH_TOO_LONG;
	memcpy(G(out), buf, outsize);
	return 0;
}
static void h_overlay_resolve_range(Cpu *c) { uint32_t a = A3; RET(resolve(A2, RD32(a), RD32(a + 4), RD32(a + 0x10), A1 != 0, RD8(a + 8), RD8(a + 9))); }
static void h_overlay_resolve(Cpu *c) { uint32_t a = A3; RET(resolve(A2, RD32(a), RD32(a + 4), RD32(a + 0xc), A1 != 0, 0, 255)); }   /* args: out path, max path, -, out size */
/* (pid, min order, max order, args: out ids, max ids, *actual count, out size) */
static void h_overlay_list(Cpu *c) { uint32_t a = A3; RET(overlay_list(A1 & 0xff, A2 & 0xff, RD32(a), RD32(a + 4), RD32(a + 8))); }
/* (scheduler count, partially resolved path, args): 1 only for a host0..9: path with several schedulers */
static void h_overlay_scheduler(Cpu *c) {
	const char *p = A1 ? (char *)G(A1) : "";
	RET((int32_t)A0 > 1 && !strncmp(p, "host", 4) && p[4] >= '0' && p[4] <= '9' && p[5] == ':');
}
static void h_overlay_set_disabled(Cpu *c) { overlays_disabled = A0 != 0; RET(0); }
static void h_overlay_is_disabled(Cpu *c) { RET(overlays_disabled); }
/* the process's own table: (overlay, out id) / (overlay) / (id, out) / (id); the ForProcess forms
 * (and the 02 wrappers) take the pid first */
static void h_overlay_add(Cpu *c) { RET(overlay_add(A0, A1)); }
static void h_overlay_add_proc(Cpu *c) { RET(overlay_add(A1, A2)); }
static void h_overlay_modify(Cpu *c) { RET(overlay_modify(A0)); }
static void h_overlay_modify_proc(Cpu *c) { RET(overlay_modify(A1)); }
static void h_overlay_info(Cpu *c) { RET(overlay_info((int32_t)A0, A1)); }
static void h_overlay_info_proc(Cpu *c) { RET(overlay_info((int32_t)A1, A2)); }
static void h_overlay_remove(Cpu *c) { RET(overlay_remove((int32_t)A0)); }
static void h_overlay_remove_proc(Cpu *c) { RET(overlay_remove((int32_t)A1)); }
/* the 02 wrappers take the arguments flat -- the resolver's output buffer size is its max path.
 * (pid, write, in, out, max path (stack), min order (stack), max order (stack)) */
static void h_overlay_resolve_range02(Cpu *c) { RET(resolve(A2, A3, STK(0), STK(0), A1 != 0, STK(1) & 0xff, STK(2) & 0xff)); }
/* (pid, min order, max order, out ids, max ids (stack), *actual count (stack)) */
static void h_overlay_list02(Cpu *c) { RET(overlay_list(A1 & 0xff, A2 & 0xff, A3, STK(0), STK(1))); }
/* the overlay directory handles: the firmware itself answers every one with SCE_FIOS_ERROR_UNIMPLEMENTED */
static void h_overlay_dh_open(Cpu *c) { if (A0) WR32(A0, 0); RET(FIOS_UNIMPLEMENTED); }
static void h_overlay_dh(Cpu *c) { RET(FIOS_UNIMPLEMENTED); }

const HleEntry hle_io[] = {
	{"sceIoOpen", h_io_open},
	{"sceIoClose", h_io_close},
	{"sceIoRead", h_io_read},
	{"sceIoWrite", h_io_write},
	{"sceIoPread", h_io_pread},
	{"sceIoPwrite", h_io_pwrite},
	{"sceIoLseek", h_io_lseek},
	{"sceIoLseek32", h_io_lseek32},
	{"sceIoGetstat", h_io_getstat},
	{"sceIoGetstatByFd", h_io_getstat_fd},
	{"sceIoChstat", h_io_chstat},
	{"sceIoMkdir", h_io_mkdir},
	{"sceIoRmdir", h_io_rmdir},
	{"sceIoRemove", h_io_remove},
	{"sceIoRename", h_io_rename},
	{"sceIoSync", h_io_sync},
	{"sceIoSyncByFd", h_io_sync_fd},
	{"sceIoDevctl", h_io_devctl},
	{"sceIoIoctl", h_io_ioctl},
	{"sceIoDopen", h_io_dopen},
	{"sceIoDread", h_io_dread},
	{"sceIoDclose", h_io_dclose},
	{"_sceFiosKernelOverlayResolveWithRangeSync", h_overlay_resolve_range},
	{"_sceFiosKernelOverlayResolveSync", h_overlay_resolve},
	{"_sceFiosKernelOverlayGetList", h_overlay_list},
	{"_sceFiosKernelOverlayGetRecommendedScheduler", h_overlay_scheduler},
	{"_sceFiosKernelOverlayThreadSetDisabled", h_overlay_set_disabled},
	{"_sceFiosKernelOverlayThreadIsDisabled", h_overlay_is_disabled},
	{"_sceFiosKernelOverlayAdd", h_overlay_add}, {"_sceFiosKernelOverlayAddForProcess", h_overlay_add_proc},
	{"_sceFiosKernelOverlayModify", h_overlay_modify}, {"_sceFiosKernelOverlayModifyForProcess", h_overlay_modify_proc},
	{"_sceFiosKernelOverlayGetInfo", h_overlay_info}, {"_sceFiosKernelOverlayGetInfoForProcess", h_overlay_info_proc},
	{"_sceFiosKernelOverlayRemove", h_overlay_remove}, {"_sceFiosKernelOverlayRemoveForProcess", h_overlay_remove_proc},
	{"_sceFiosKernelOverlayDHOpenSync", h_overlay_dh_open}, {"_sceFiosKernelOverlayDHReadSync", h_overlay_dh},
	{"_sceFiosKernelOverlayDHCloseSync", h_overlay_dh}, {"_sceFiosKernelOverlayDHStatSync", h_overlay_dh},
	{"_sceFiosKernelOverlayDHSyncSync", h_overlay_dh}, {"_sceFiosKernelOverlayDHChstatSync", h_overlay_dh},
	{"sceFiosOverlayResolveWithRangeSync02", h_overlay_resolve_range02}, {"sceFiosOverlayGetList02", h_overlay_list02},
	{"sceFiosOverlayGetRecommendedScheduler02", h_overlay_scheduler}, {"sceFiosOverlayThreadSetDisabled02", h_overlay_set_disabled},
	{"sceFiosOverlayThreadIsDisabled02", h_overlay_is_disabled}, {"sceFiosOverlayAddForProcess02", h_overlay_add_proc},
	{"sceFiosOverlayModifyForProcess02", h_overlay_modify_proc}, {"sceFiosOverlayGetInfoForProcess02", h_overlay_info_proc},
	{"sceFiosOverlayRemoveForProcess02", h_overlay_remove_proc},
	{0, 0},
};
