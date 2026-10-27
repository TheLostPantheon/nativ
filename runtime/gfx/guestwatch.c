/*
 * Page-granular write tracking of the guest arena: see guestwatch.h for the contract.
 *
 * Why: the GPU backend's texture cache must notice the CPU changing a texture's bytes (Unit 13
 * draws its menu text into a glyph atlas between frames) before the next draw samples it. Hashing
 * every bound texture once a frame did that, at 27 ms of a heavy level frame. With the pages under
 * cached textures read-only, a clean texture costs one load of the write clock per bind.
 *
 * Measured on this Mac (M-series, 16 KB pages): a write fault through this handler ~6 us, an
 * mprotect call ~1 us (one call for a run of 2000 pages ~35 us) -- so a texture rewritten whole
 * every frame is cheaper to hash than to fault (gfx_gpu.c stops watching those).
 *
 * The invariant everything rests on: when gwatch_protect returns clock c, every page of the range is
 * read-only or pinned by a host write in flight, and a page leaves that state only through a stamp
 * greater than c (a fault, gwatch_write_begin, or gwatch_write_end for a pinned one). Stamps are made
 * under one spinlock (the fault handler takes it too: nothing holding it ever writes guest memory, so
 * it can't fault while held), each page's stamp stored before the clock that publishes it and
 * before the page opens.
 *
 * Hazards: a write by the kernel into a read-only page fails with EFAULT instead of faulting
 * (read(2)/pread(2) into guest memory: io.c's and fios.c's file reads bracket theirs with
 * gwatch_write_begin/end; main.c's image load freads straight into guest memory too, but runs
 * before any texture exists); host code's own stores (HLE memcpy/memset, the software renderer,
 * zlib, the GPU backend's target downloads) fault like the game's, and the bulk ones bracket
 * themselves only to take one mprotect instead of a fault per page; a debugger stops on every one
 * of these faults (lldb: `process handle SIGBUS -s false -p true`, or RECOMP_GPU_NO_TEX_WATCH=1).
 *
 * Ordering, from a stress test (writer threads faulting on the same pages, a reader thread read(2)ing
 * into them, a checker re-closing and comparing): a page is stamped BEFORE it opens -- stamped after
 * mprotect, another thread's store landed in between and was visible with no stamp yet.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "guestwatch.h"
#include "platform.h"

static int shift = 12;                /* log2 host page size (16 KB on Apple Silicon) */
static uint32_t npages;               /* 0 until set up: the fault handler then claims nothing */
static uint8_t *prot;                 /* per page: 1 read-only by us now, 3 no access (the read watch), 2 was and has been opened again (a racing fault there just retries), 0 never touched */
static uint16_t *pins;                /* per page: host writes in flight (gwatch_write_begin..end) */
static uint32_t *pclock;              /* per page: the write clock when it was last opened / written by a bracketed host write */
static uint32_t wclock;               /* the write clock */
static int lk;
static int enabled, broken;
static void (*read_hook)(uint32_t addr);   /* gwatch_read_arm's: told of each access to a no-access page */
static uint32_t n_faults, n_marks, n_protects;

static void lock(void) { while (__atomic_exchange_n(&lk, 1, __ATOMIC_ACQUIRE)) while (__atomic_load_n(&lk, __ATOMIC_RELAXED)) ; }
static void unlock(void) { __atomic_store_n(&lk, 0, __ATOMIC_RELEASE); }
static uint8_t *page_ptr(uint32_t p) { return recomp_mem + ((uint64_t)p << shift); }
/* pages [p0, p1] get the next clock value (lock held): each page's stamp lands before the clock
 * that publishes it, so a reader that sees the new clock (acquire) sees the stamps too */
static void stamp(uint32_t p0, uint32_t p1) {
	uint32_t c = wclock + 1;
	for (uint32_t p = p0; p <= p1; p++) __atomic_store_n(&pclock[p], c, __ATOMIC_RELAXED);
	__atomic_store_n(&wclock, c, __ATOMIC_RELEASE);
}

static int on_fault(void *addr) {   /* platform.h's write-fault hook */
	uint8_t *a = (uint8_t *)addr;
	if (npages && a >= recomp_mem && a < recomp_mem + (1ull << 32)) {
		uint32_t p = (uint32_t)((uint64_t)(a - recomp_mem) >> shift);
		lock();
		uint8_t s = prot[p];
		/* stamped BEFORE the page opens: another thread's store can land the moment it is writable,
		 * and a reader that sees those bytes must see the stamp too */
		if (s == 1 || s == 3) { stamp(p, p); plat_protect(page_ptr(p), (size_t)1 << shift, 1); prot[p] = 2; n_faults++; }   /* (a read-watched page is stamped too: the access may be a store) */
		unlock();
		__atomic_thread_fence(__ATOMIC_SEQ_CST);   /* the stamp visible everywhere before the store this fault interrupted runs again */
		if (s == 3 && read_hook) read_hook((uint32_t)(a - recomp_mem));   /* (outside the lock: the hook only records) */
		if (s) return 1;   /* ours (or opened by another thread's fault an instant ago): the store runs again on a writable page */
	}
	return 0;   /* not ours: main.c's crash report */
}

static void setup(void) {
	if (getenv("RECOMP_GPU_NO_TEX_WATCH") || !recomp_mem) return;
	size_t ps = plat_page_size();
	if (ps < 4096 || (ps & (ps - 1)) || ((uintptr_t)recomp_mem & (uintptr_t)(ps - 1))) return;
	shift = __builtin_ctzll((unsigned long long)ps);
	uint32_t n = (uint32_t)((1ull << 32) >> shift);
	size_t bytes = (size_t)n * (1 + 2 + 4);
	uint8_t *m = plat_reserve(bytes);
	if (!m) return;
	pclock = (uint32_t *)m; pins = (uint16_t *)(m + (size_t)n * 4); prot = m + (size_t)n * 6;
	plat_add_fault_hook(on_fault);
	__atomic_store_n(&npages, n, __ATOMIC_RELEASE);
	enabled = 1;
}
static pthread_once_t once = PTHREAD_ONCE_INIT;
int gwatch_on(void) { pthread_once(&once, setup); return enabled && !__atomic_load_n(&broken, __ATOMIC_RELAXED); }
int gwatch_page_shift(void) { pthread_once(&once, setup); return shift; }
uint32_t gwatch_page_clock(uint32_t page) { return npages ? __atomic_load_n(&pclock[page], __ATOMIC_RELAXED) : 0; }

uint32_t gwatch_protect(uint32_t addr, uint32_t len) {
	if (!gwatch_on()) return 0;
	if (!len) return __atomic_load_n(&wclock, __ATOMIC_ACQUIRE);
	uint32_t p0 = addr >> shift, p1 = (uint32_t)(((uint64_t)addr + len - 1 > 0xffffffffull ? 0xffffffffull : (uint64_t)addr + len - 1) >> shift);
	lock();
	uint32_t run = 0; int inrun = 0, failed = 0;
	for (uint32_t p = p0; p <= p1 + 1 && !failed; p++) {   /* runs of pages to close, one mprotect each */
		int want = p <= p1 && prot[p] != 1 && !pins[p];
		if (want && !inrun) { run = p; inrun = 1; }
		if (!want && inrun) {
			inrun = 0;
			if (plat_protect(page_ptr(run), (size_t)(p - run) << shift, 0)) { failed = 1; break; }
			for (uint32_t q = run; q < p; q++) prot[q] = 1;
			n_protects += p - run;
		}
	}
	uint32_t c = wclock;
	unlock();
	if (failed) {   /* (ENOMEM: too many map entries) the range isn't watched: callers fall back to hashing every frame */
		fprintf(stderr, "    gwatch: mprotect failed (%s): texture write tracking off, textures hashed every frame\n", strerror(errno));
		__atomic_store_n(&broken, 1, __ATOMIC_RELAXED);
	}
	return c;
}

int gwatch_unchanged(uint32_t addr, uint32_t len, uint32_t *since) {
	if (!npages) return 0;
	uint32_t now = __atomic_load_n(&wclock, __ATOMIC_ACQUIRE);
	if (now == *since) return 1;
	if (len) {
		uint32_t p0 = addr >> shift, p1 = (uint32_t)(((uint64_t)addr + len - 1 > 0xffffffffull ? 0xffffffffull : (uint64_t)addr + len - 1) >> shift);
		for (uint32_t p = p0; p <= p1; p++) if (__atomic_load_n(&pclock[p], __ATOMIC_RELAXED) > *since) return 0;
	}
	*since = now;   /* every stamp <= now is visible above: none of these pages was opened since */
	return 1;
}

static int page_range(uint32_t addr, uint32_t len, uint32_t *p0, uint32_t *p1) {
	if (!len || !gwatch_on()) return 0;
	*p0 = addr >> shift; *p1 = (uint32_t)(((uint64_t)addr + len - 1 > 0xffffffffull ? 0xffffffffull : (uint64_t)addr + len - 1) >> shift);
	return 1;
}
void gwatch_write_begin(uint32_t addr, uint32_t len) {
	uint32_t p0, p1; if (!page_range(addr, len, &p0, &p1)) return;
	lock();
	int closed = 0;
	for (uint32_t p = p0; p <= p1 && !closed; p++) closed = prot[p] == 1 || prot[p] == 3;
	if (closed) stamp(p0, p1);   /* before any page opens (see on_fault) */
	for (uint32_t p = p0; p <= p1; p++) if (pins[p] != 0xffff) pins[p]++;
	for (uint32_t p = p0; p <= p1; p++) {   /* runs of closed pages open with one mprotect each */
		if (prot[p] != 1 && prot[p] != 3) continue;
		uint32_t q = p; while (q < p1 && (prot[q + 1] == 1 || prot[q + 1] == 3)) q++;
		plat_protect(page_ptr(p), (size_t)(q - p + 1) << shift, 1);
		for (uint32_t r = p; r <= q; r++) prot[r] = 2;
		p = q;
	}
	n_marks++;
	unlock();
}
void gwatch_write_end(uint32_t addr, uint32_t len) {
	uint32_t p0, p1; if (!page_range(addr, len, &p0, &p1)) return;
	lock();
	for (uint32_t p = p0; p <= p1; p++) if (pins[p] && pins[p] != 0xffff) pins[p]--;
	stamp(p0, p1);   /* a texture checked while the write was in flight couldn't close these pages: it sees this stamp */
	unlock();
}
void gwatch_set_read_hook(void (*hook)(uint32_t addr)) { read_hook = hook; }
void gwatch_read_arm(uint32_t addr, uint32_t len) {
	if (!len || !gwatch_on() || !read_hook) return;
	const uint64_t ps = 1ull << shift, a = ((uint64_t)addr + ps - 1) & ~(ps - 1), b = ((uint64_t)addr + len) & ~(ps - 1);   /* whole pages inside the range only: the rest of a shared page is someone else's */
	if (b <= a) return;
	const uint32_t p0 = (uint32_t)(a >> shift), p1 = (uint32_t)((b >> shift) - 1);
	lock();
	int failed = 0;
	for (uint32_t p = p0; p <= p1 && !failed; p++) {   /* runs of pages to close, one mprotect each */
		if (prot[p] == 3 || pins[p]) continue;
		uint32_t q = p; while (q < p1 && prot[q + 1] != 3 && !pins[q + 1]) q++;
		if (plat_protect(page_ptr(p), (size_t)(q - p + 1) << shift, -1)) failed = 1;
		else { for (uint32_t r = p; r <= q; r++) prot[r] = 3; n_protects += q - p + 1; }
		p = q;
	}
	unlock();
	if (failed) {
		fprintf(stderr, "    gwatch: mprotect failed (%s): guest-memory tracking off\n", strerror(errno));
		__atomic_store_n(&broken, 1, __ATOMIC_RELAXED);
	}
}
void gwatch_read_open(uint32_t addr, uint32_t len) {
	uint32_t p0, p1; if (!page_range(addr, len, &p0, &p1)) return;
	lock();
	for (uint32_t p = p0; p <= p1; p++) {
		if (prot[p] != 3) continue;
		uint32_t q = p; while (q < p1 && prot[q + 1] == 3) q++;
		stamp(p, q); plat_protect(page_ptr(p), (size_t)(q - p + 1) << shift, 1);
		for (uint32_t r = p; r <= q; r++) prot[r] = 2;
		p = q;
	}
	unlock();
}
void gwatch_stats(uint32_t *faults, uint32_t *marks, uint32_t *protects) {
	lock(); *faults = n_faults; *marks = n_marks; *protects = n_protects; n_faults = n_marks = n_protects = 0; unlock();
}
