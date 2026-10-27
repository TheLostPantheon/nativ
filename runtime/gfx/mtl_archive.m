/* mtl_archive.m (macOS): this machine's own archive of what Metal compiled for a game, kept in the game's shader cache
 * (gfx_gpu.c), so that a shader the game has used compiles once on this machine, whatever the OS's shader cache lets go
 * (it is shared by every program and size-limited: a night of test runs pushed a Killzone level's builds out of it and
 * ~270 shaders compiled cold mid-play, 2026-10-25). Two parts, measured with the OS's cache emptied (M4 Pro):
 *  - a shader's library (AIR, Metal's intermediate code). SDL compiles our MSL text into one, 70 ms for a mid-sized shader;
 *    given the library itself (SDL_GPU_SHADERFORMAT_METALLIB) it takes 0.02 ms. Metal has no call that writes a library
 *    out, but a binary archive's file holds its pipelines' libraries: a one-pipeline archive is written and its library
 *    slice kept (in the shader's MSL cache file: gfx_gpu.c air_store). A vertex shader's comes from a vertex-only pipeline;
 *    a fragment shader's carries its pipeline's vertex function too (and Metal renames one of the two main0s, so the
 *    entry's name is kept with it).
 *  - the pipelines (the GPU's own code, 20-50 ms each from a library): a MTLBinaryArchive. SDL has no way to be given one,
 *    so -[MTLDevice newRenderPipelineStateWithDescriptor:error:], the call SDL makes every pipeline with, is swizzled: the
 *    archive is tried first (MTLPipelineOptionFailOnBinaryArchiveMiss, 0.2 ms) and a miss compiles as before and joins it.
 * The archive is segments, seg_<n>.bin, each written whole by a flush with what was added since the one before (when the
 * warm-up is done, ten minutes after a pipeline was added, at exit), in a folder per GPU and OS build: an OS update leaves
 * the GPU code stale, so another build's folder is deleted. A start that finds more than MAXSEG segments compacts: every
 * pipeline it makes joins its own, and at its exit, when its warm-up made every pipeline the recipes name, the segments
 * it found go: its own then hold the recipes' pipelines and every other one it used. (They went when the warm-up ended
 * before, and took the pipelines made later in play with them, views and passes the recipes don't name: a minute's
 * flushes wrote 13 segments in one 14-minute session, so nearly every start compacted and recompiled those in play.) Manual reference counting (no ARC): the swizzled call returns a retained object, as the
 * original does. RECOMP_MTL_ARCHIVE=0: off. */
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include "mtl_archive.h"

#define MAXSEG 48
static pthread_mutex_t seg_m = PTHREAD_MUTEX_INITIALIZER, cur_m = PTHREAD_MUTEX_INITIALIZER;
static id<MTLDevice> dev;
static NSArray *segs;                 /* the segments, for lookups (replaced whole, under seg_m) */
static int nfound;                    /* compacting: the first nfound of segs are the ones this start found */
static char (*found_paths)[1100];
static id<MTLBinaryArchive> cur;      /* added since the last flush (under cur_m) */
static int cur_n, seg_next, compact, on;
static uint64_t dirty_ms;             /* when cur's first pipeline was added (0: none) */
static char dir[1024];
static IMP orig;
static MtlAirStore air_store;
static uint32_t n_hit, n_added, n_libs; static uint64_t us_add, us_keep;   /* (us: the workers' time in add and keep_library) */
typedef struct { int set, archive; uint64_t vkey, fkey; } Want;   /* set 0: no word from the caller -- archived (the renderer's own pipelines) */
static __thread Want want;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000; }
static uint64_t now_ms(void) { return now_us() / 1000; }
void mtl_archive_want(int archive, uint64_t vkey, uint64_t fkey) { want = (Want){ 1, archive, vkey, fkey }; }
void mtl_archive_want_clear(void) { want = (Want){ 0, 0, 0, 0 }; }
void mtl_archive_report(void) {
	fprintf(stderr, "    gpu: shader archive: so far %u pipelines from it, %u compiled and added (%.0f ms), %u shader libraries kept (%.0f ms)\n",
	        __atomic_load_n(&n_hit, __ATOMIC_RELAXED), __atomic_load_n(&n_added, __ATOMIC_RELAXED), __atomic_load_n(&us_add, __ATOMIC_RELAXED) / 1e3,
	        __atomic_load_n(&n_libs, __ATOMIC_RELAXED), __atomic_load_n(&us_keep, __ATOMIC_RELAXED) / 1e3);
}

static id<MTLBinaryArchive> archive_new(NSURL *url) {   /* (retained) */
	MTLBinaryArchiveDescriptor *ad = [MTLBinaryArchiveDescriptor new]; ad.url = url;
	NSError *e = nil; id<MTLBinaryArchive> a = [dev newBinaryArchiveWithDescriptor:ad error:&e];
	[ad release];
	return a;
}
static void add(MTLRenderPipelineDescriptor *pd) {   /* a pipeline joins this session's segment */
	const uint64_t t0 = now_us();
	pthread_mutex_lock(&cur_m);
	if (!cur) cur = archive_new(nil);
	NSError *e = nil;
	if (cur && [cur addRenderPipelineFunctionsWithDescriptor:pd error:&e]) { cur_n++; if (!dirty_ms) dirty_ms = now_ms(); __atomic_add_fetch(&n_added, 1, __ATOMIC_RELAXED); }
	else { static int said; if (!said++) fprintf(stderr, "    gpu: shader archive: a pipeline wasn't added (%s)\n", e ? e.localizedDescription.UTF8String : "no archive"); }
	pthread_mutex_unlock(&cur_m);
	__atomic_add_fetch(&us_add, now_us() - t0, __ATOMIC_RELAXED);
}
/* an archive file's library: the MTLB slice of its fat header (big-endian: magic, count, then cputype, subtype, offset,
 * size, align per slice); a file that is a library already, whole */
static NSData *library_slice(NSData *b) {
	const uint8_t *p = b.bytes; const size_t n = b.length;
	if (n >= 4 && !memcmp(p, "MTLB", 4)) return b;
	if (n < 8) return nil;
	const uint32_t k = (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7];
	for (uint32_t i = 0; i < k && 8 + 20 * (size_t)(i + 1) <= n; i++) {
		const uint8_t *a = p + 8 + 20 * i;
		const uint32_t off = (uint32_t)a[8] << 24 | (uint32_t)a[9] << 16 | (uint32_t)a[10] << 8 | a[11], size = (uint32_t)a[12] << 24 | (uint32_t)a[13] << 16 | (uint32_t)a[14] << 8 | a[15];
		if ((size_t)off + size <= n && size >= 4 && !memcmp(p + off, "MTLB", 4)) return [b subdataWithRange:NSMakeRange(off, size)];
	}
	return nil;
}
/* the function of the stage wanted in a library, by name: main0 when Metal kept it, else whichever it renamed */
static int entry_of(NSData *lib, int frag, char out[64]) {
	dispatch_data_t dd = dispatch_data_create(lib.bytes, lib.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
	NSError *e = nil; id<MTLLibrary> l = [dev newLibraryWithData:dd error:&e];
	dispatch_release(dd);
	if (!l) return 0;
	const MTLFunctionType t = frag ? MTLFunctionTypeFragment : MTLFunctionTypeVertex; int ok = 0;
	for (NSString *name in [@[@"main0"] arrayByAddingObjectsFromArray:l.functionNames]) {
		id<MTLFunction> f = [l newFunctionWithName:name];
		if (f && f.functionType == t && name.length < 64) { snprintf(out, 64, "%s", name.UTF8String); ok = 1; }
		[f release];
		if (ok) break;
	}
	[l release];
	return ok;
}
/* a library being kept, or kept, this session: once per key (the warm-up's copy of a shader and the game's own have one
 * key; two workers writing one archive file at once crashed in Metal's serializer) */
#define KEPT 32768
static uint64_t kept[KEPT]; static pthread_mutex_t kept_m = PTHREAD_MUTEX_INITIALIZER;
static int keep_first(uint64_t key) {
	int r = 1;
	pthread_mutex_lock(&kept_m);
	for (uint32_t i = (uint32_t)(key ^ key >> 32) & (KEPT - 1), n = 0; n < KEPT; i = (i + 1) & (KEPT - 1), n++) {
		if (kept[i] == key) { r = 0; break; }
		if (!kept[i]) { kept[i] = key; break; }
	}
	pthread_mutex_unlock(&kept_m);
	return r;
}
static void keep_library(MTLRenderPipelineDescriptor *pd, int frag, uint64_t key) {   /* a one-pipeline archive written, its library kept */
	if (!keep_first(key)) return;
	static uint32_t seq; const uint64_t t0 = now_us();
	MTLRenderPipelineDescriptor *one = [pd copy];
	one.binaryArchives = nil;
	if (!frag) one.fragmentFunction = nil;   /* (a vertex-only pipeline: the vertex function alone) */
	id<MTLBinaryArchive> a = archive_new(nil);
	char tmp[1100]; snprintf(tmp, sizeof tmp, "%s/lib.%d.%u.tmp", dir, (int)getpid(), __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED));
	NSError *e = nil; NSData *b = nil;
	if (a && [a addRenderPipelineFunctionsWithDescriptor:one error:&e] && [a serializeToURL:[NSURL fileURLWithPath:@(tmp)] error:&e]) b = [NSData dataWithContentsOfFile:@(tmp)];
	unlink(tmp);
	[a release]; [one release];
	NSData *lib = b ? library_slice(b) : nil; char entry[64];
	if (lib && entry_of(lib, frag, entry)) { air_store(key, frag, entry, lib.bytes, lib.length); __atomic_add_fetch(&n_libs, 1, __ATOMIC_RELAXED); }
	else { static int said; if (!said++) fprintf(stderr, "    gpu: shader archive: a %s shader's library wasn't kept (%s)\n", frag ? "fragment" : "vertex", e ? e.localizedDescription.UTF8String : "no library in the archive"); }
	__atomic_add_fetch(&us_keep, now_us() - t0, __ATOMIC_RELAXED);
}
typedef id (*NewPipe)(id, SEL, MTLRenderPipelineDescriptor *, NSError **);
static id new_pipe(id self, SEL sel, MTLRenderPipelineDescriptor *pd, NSError **err) {   /* (swizzled in for SDL's pipelines: the archive first) */
	const Want w = want; want = (Want){ 0, 0, 0, 0 };
	const int archive = w.set ? w.archive : 1;
	if (!on || self != dev || pd.binaryArchives.count) return ((NewPipe)orig)(self, sel, pd, err);   /* (another device's: its archives would have to be its own) */
	id ps = nil;
	@autoreleasepool {
		pthread_mutex_lock(&seg_m); NSArray *look = [[segs retain] autorelease]; pthread_mutex_unlock(&seg_m);
		if (look.count) {
			pd.binaryArchives = look;
			ps = [self newRenderPipelineStateWithDescriptor:pd options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
			pd.binaryArchives = nil;
		}
		const int hit = ps != nil;
		if (hit) __atomic_add_fetch(&n_hit, 1, __ATOMIC_RELAXED);
		else ps = ((NewPipe)orig)(self, sel, pd, err);
		if (ps) {
			if (archive && (!hit || compact)) add(pd);
			if (w.vkey) keep_library(pd, 0, w.vkey);
			if (w.fkey) keep_library(pd, 1, w.fkey);
		}
	}
	return ps;
}

static int warmed;   /* this session's warm-up made every pipeline the recipes name */
void mtl_archive_flush(int how) {
	if (!on) return;
	if (how == 1) warmed = 1;
	@autoreleasepool {
		pthread_mutex_lock(&cur_m);
		id<MTLBinaryArchive> a = cur; const int n = cur_n; cur = nil; cur_n = 0; dirty_ms = 0;
		pthread_mutex_unlock(&cur_m);
		if (a && n) {
			static uint32_t seq; char path[1100], tmp[1200]; snprintf(tmp, sizeof tmp, "%s/seg.%d.%u.tmp", dir, (int)getpid(), __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED));
			const uint64_t t0 = now_ms(); NSError *e = nil; struct stat st; int placed = 0;
			if ([a serializeToURL:[NSURL fileURLWithPath:@(tmp)] error:&e])
				for (int k = 0; k < 64 && !placed; k++) {   /* (the next number no other game process sharing the folder took: never replacing one) */
					pthread_mutex_lock(&cur_m); const int s = seg_next++; pthread_mutex_unlock(&cur_m);
					snprintf(path, sizeof path, "%s/seg_%06d.bin", dir, s);
					placed = !renamex_np(tmp, path, RENAME_EXCL);
				}
			if (placed && !stat(path, &st)) {
				pthread_mutex_lock(&seg_m); NSArray *o = segs; segs = [[o arrayByAddingObject:a] retain]; [o release]; pthread_mutex_unlock(&seg_m);
				fprintf(stderr, "    gpu: shader archive: %d pipelines written (%s, %.1f MB, %llu ms)\n", n, path, st.st_size / 1048576.0, (unsigned long long)(now_ms() - t0));
			} else { unlink(tmp); fprintf(stderr, "    gpu: shader archive: writing %s failed (%s)\n", tmp, e ? e.localizedDescription.UTF8String : "no segment number free"); }
		}
		[a release];
		if (how == 2 && warmed && compact && nfound) {   /* this session made every pipeline the recipes name and every other it used: the segments it found go */
			pthread_mutex_lock(&seg_m);
			NSArray *o = segs; segs = [[o subarrayWithRange:NSMakeRange(nfound, o.count - nfound)] retain]; [o release];
			const int k = nfound; nfound = 0; compact = 0;
			pthread_mutex_unlock(&seg_m);
			for (int i = 0; i < k; i++) unlink(found_paths[i]);
			fprintf(stderr, "    gpu: shader archive: compacted (%d older segments removed)\n", k);
		}
	}
}
static pthread_mutex_t fl_m = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t fl_c = PTHREAD_COND_INITIALIZER; static int fl_req;   /* 1 flush, 2 the warm-up's */
void mtl_archive_flush_soon(int warm) {   /* (the render thread: a first session's segment is hundreds of MB) */
	pthread_mutex_lock(&fl_m); if (fl_req < 1 + !!warm) fl_req = 1 + !!warm; pthread_cond_signal(&fl_c); pthread_mutex_unlock(&fl_m);
}
static void *flusher(void *arg) {   /* asked, or ten minutes after a pipeline joined */
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&fl_m);
		if (!fl_req) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); t.tv_sec += 10; pthread_cond_timedwait(&fl_c, &fl_m, &t); }
		const int r = fl_req; fl_req = 0;
		pthread_mutex_unlock(&fl_m);
		pthread_mutex_lock(&cur_m); const uint64_t d = dirty_ms; pthread_mutex_unlock(&cur_m);
		if (r || (d && now_ms() - d > 600000)) mtl_archive_flush(r == 2);
	}
	return 0;
}
static void at_exit(void) { mtl_archive_report(); mtl_archive_flush(2); }   /* (exit; the window's close is _exit, and calls mtl_archive_flush itself: gfx_gpu_quit) */
static void remove_dir(const char *path) {   /* another GPU's or OS build's folder (files only: the archive makes no folders in it) */
	DIR *d = opendir(path); if (!d) return;
	struct dirent *de; char p[2200];
	while ((de = readdir(d))) if (de->d_name[0] != '.') { snprintf(p, sizeof p, "%s/%s", path, de->d_name); unlink(p); }
	closedir(d); rmdir(path);
}
static int cmp_str(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

int mtl_archive_open(const char *cache_dir, MtlAirStore store) {
	static int done; if (done) return on;
	done = 1;
	const char *env = getenv("RECOMP_MTL_ARCHIVE");
	if ((env && *env == '0') || !cache_dir || !store) return 0;
	@autoreleasepool {
		if (!(dev = MTLCreateSystemDefaultDevice())) return 0;
		char os[64] = "unknown"; size_t osn = sizeof os; sysctlbyname("kern.osversion", os, &osn, 0, 0);
		char fp[200]; snprintf(fp, sizeof fp, "%s_%s", dev.name.UTF8String, os);
		for (char *c = fp; *c; c++) if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '_')) *c = '_';
		char base[1024]; snprintf(base, sizeof base, "%s/metal", cache_dir); mkdir(base, 0755);
		snprintf(dir, sizeof dir, "%s/%s", base, fp);
		if (mkdir(dir, 0755) && access(dir, W_OK)) { fprintf(stderr, "    gpu: shader archive: cannot use %s: off\n", dir); return 0; }
		DIR *d = opendir(base); struct dirent *de; char p[2200];
		while (d && (de = readdir(d))) if (de->d_name[0] != '.' && strcmp(de->d_name, fp)) { snprintf(p, sizeof p, "%s/%s", base, de->d_name); remove_dir(p); fprintf(stderr, "    gpu: shader archive: %s is another GPU's or OS build's: removed\n", p); }
		if (d) closedir(d);
		/* the segments, in the order they were written; a file Metal won't open (cut short) is deleted */
		int nf = 0, cap = 0; char (*names)[64] = 0;
		if ((d = opendir(dir))) {
			while ((de = readdir(d))) {
				const size_t l = strlen(de->d_name);
				if (strncmp(de->d_name, "seg_", 4) || l < 8 || l >= 64 || strcmp(de->d_name + l - 4, ".bin")) {   /* a killed session's temporaries (ours name their process; Metal's own, from a serialize cut short, don't) */
					snprintf(p, sizeof p, "%s/%s", dir, de->d_name); int pid = 0; struct stat st;
					const int ours = (sscanf(de->d_name, "lib.%d.", &pid) == 1 || sscanf(de->d_name, "seg.%d.", &pid) == 1) && pid > 0;
					if (ours ? kill(pid, 0) && errno == ESRCH : !stat(p, &st) && time(0) - st.st_mtime > 600) unlink(p);
					continue;
				}
				if (nf == cap) { cap = cap ? 2 * cap : 64; names = realloc(names, (size_t)cap * sizeof *names); }
				snprintf(names[nf++], 64, "%s", de->d_name);
			}
			closedir(d);
		}
		qsort(names, (size_t)nf, sizeof *names, cmp_str);
		NSMutableArray *got = [NSMutableArray array]; uint64_t bytes = 0;
		found_paths = calloc((size_t)nf + 1, sizeof *found_paths);
		const int start_over = nf > 3 * MAXSEG;   /* (never compacted: a headless run's, which has no warm-up) */
		for (int i = 0; i < nf; i++) {
			snprintf(p, sizeof p, "%s/%s", dir, names[i]);
			const int sn = atoi(names[i] + 4); if (sn >= seg_next) seg_next = sn + 1;
			id<MTLBinaryArchive> a = start_over ? nil : archive_new([NSURL fileURLWithPath:@(p)]);
			struct stat st;
			if (!a) { unlink(p); continue; }
			if (!stat(p, &st)) bytes += (uint64_t)st.st_size;
			snprintf(found_paths[got.count], sizeof *found_paths, "%s", p);
			[got addObject:a]; [a release];
		}
		free(names);
		segs = [got copy];
		compact = segs.count > MAXSEG; nfound = compact ? (int)segs.count : 0;
		/* SDL's pipelines through the archive: its device's class (Apple silicon: one GPU, one class) */
		Method m = class_getInstanceMethod(object_getClass(dev), @selector(newRenderPipelineStateWithDescriptor:error:));
		if (!m || method_getImplementation(m) == (IMP)new_pipe) { fprintf(stderr, "    gpu: shader archive: no pipeline call to go through: off\n"); return 0; }
		air_store = store;
		orig = method_setImplementation(m, (IMP)new_pipe);
		on = 1;
		pthread_t t; if (!pthread_create(&t, 0, flusher, 0)) pthread_detach(t);
		atexit(at_exit);
		fprintf(stderr, "    gpu: shader archive: %d segments, %.1f MB (%s)%s%s\n", (int)segs.count, bytes / 1048576.0, dir, compact ? ": compacting this session" : "", start_over ? ": too many segments, started over" : "");
	}
	return 1;
}
