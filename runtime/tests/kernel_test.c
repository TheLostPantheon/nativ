/*
 * Unit harness for runtime/core/kernel.c: the thread manager's semantics, driven through the h_* handlers
 * on real host threads, asserted against the firmware's (vitasdk error codes; the wake rules of
 * the SDK). It includes kernel.c directly so the handlers (file-static) and the per-thread
 * identity (cur_tid) are reachable, and supplies the few runtime symbols kernel.c takes from
 * main.c/display.c: the guest arena, the allocator, the TLS block, the function lookup. Build and
 * run: see README.md in this directory.
 */
#include <sys/mman.h>
#include <signal.h>
#include <stdarg.h>
#include "../core/kernel.c"

/* ---- the runtime kernel.c expects */
uint8_t *recomp_mem;
__thread uint32_t recomp_excl_addr, recomp_excl_val;
__thread int recomp_excl_valid;
__thread uint64_t recomp_excl_val64;
__thread uint32_t recomp_tpidruro;
char hle_last_format[1024];
static uint32_t heap = 0x81000000;
uint32_t guest_alloc(uint32_t size, uint32_t align) { heap = (heap + align - 1) & ~(align - 1); uint32_t a = heap; heap += size; return a; }
uint32_t tls_block(void) { return guest_alloc(0x100, 64); }
void recomp_runfast(void) {}
uint64_t guest_time_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
int guest_clock_fixed(void) { return 0; }
void guest_time_slept(uint32_t us) { (void)us; }
void display_vblank_callbacks(void) {}   /* display.c's vblank callbacks: none here */
uint32_t io_guest_fd(int fd) { return (uint32_t)fd + 0x100; }   /* io.c's handle for a host fd (the standard streams) */
void plat_random(void *buf, size_t n) { for (size_t i = 0; i < n; i++) ((unsigned char *)buf)[i] = (unsigned char)rand(); }
/* "recompiled" guest functions the thread tests start: a table of C functions by fake address */
static GuestFn guest_fns[32];
GuestFn guest_lookup(uint32_t a) { return a >= 0x1000 && a < 0x1000 + 32 * 4 ? guest_fns[(a - 0x1000) / 4] : 0; }

/* ---- calling a handler: r0-r3 then stack words, on a per-thread guest stack */
static __thread uint32_t my_sp;
static uint32_t call(HleFn f, int n, ...) {
	if (!my_sp) my_sp = guest_alloc(0x10000, 16) + 0x10000 - 256;
	Cpu c = { 0 }; c.r[13] = my_sp; c.r[14] = 0xfffffffe;
	va_list ap; va_start(ap, n);
	for (int i = 0; i < n; i++) { uint32_t v = va_arg(ap, uint32_t); if (i < 4) c.r[i] = v; else WR32(my_sp + 4 * (i - 4), v); }
	va_end(ap);
	f(&c);
	return c.r[0];
}
static uint64_t ret64(HleFn f) { Cpu c = { 0 }; f(&c); return c.r[0] | ((uint64_t)c.r[1] << 32); }

/* ---- helper threads: each a guest thread identity of its own (uid, priority), entered in the
 * kernel's thread table so a callback's owner lookup (thread_of) finds it */
typedef struct { void (*fn)(void *); void *arg; uint32_t tid; int prio; } Job;
static void *job_main(void *p) {
	Job *j = p;
	pthread_mutex_lock(&klock);
	Thread *me = &threads[nthreads++];
	memset(me, 0, sizeof *me);
	me->uid = j->tid = 0x40010100 + 2 * (uint32_t)(me - threads); me->prio = me->init_prio = j->prio; me->started = 1; snprintf(me->name, sizeof me->name, "t%x", j->tid);
	pthread_mutex_unlock(&klock);
	cur_tid = j->tid; cur_thread = me;
	j->fn(j->arg);
	me->done = 1;
	return 0;
}
static pthread_t go_prio(void (*fn)(void *), void *arg, int prio) {
	Job *j = malloc(sizeof *j); j->fn = fn; j->arg = arg; j->prio = prio;
	pthread_t t; pthread_create(&t, 0, job_main, j); return t;
}
static pthread_t go(void (*fn)(void *), void *arg) { return go_prio(fn, arg, 160); }
static void nap(int ms) { usleep(ms * 1000); }
static uint32_t word(void) { uint32_t a = guest_alloc(8, 8); WR64(a, 0); return a; }   /* a guest word (or doubleword) */

static int fails, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKEQ(a, b) do { uint32_t _a = (a), _b = (b); checks++; if (_a != _b) { fails++; fprintf(stderr, "FAIL %s:%d: %s = %#x, expected %s = %#x\n", __FILE__, __LINE__, #a, _a, #b, _b); } } while (0)

/* ================= event flags */
typedef struct { uint32_t uid, bits, mode, out, timeout, result; volatile int done; } EvfWait;
static void evf_waiter(void *p) { EvfWait *w = p; w->result = call(h_wait_evf, 5, w->uid, w->bits, w->mode, w->out, w->timeout); w->done = 1; }
static void test_evf(void) {
	uint32_t name = guest_alloc(16, 4); strcpy((char *)G(name), "evf");
	/* two waiters with different masks, WAITOR | WAITCLEAR_PAT: a set of one bit wakes only its
	 * waiter, which clears only its own bit; the other keeps waiting and gets its bit later */
	uint32_t e = call(h_create_evf, 4, name, 0x1000, 0, 0);
	CHECK(e >= 0x40020001 && !(e & 0x80000000));
	EvfWait a = { e, 1, 1 | 4, word(), 0 }, b = { e, 2, 1 | 4, word(), 0 };
	pthread_t ta = go(evf_waiter, &a); nap(20); pthread_t tb = go(evf_waiter, &b); nap(20);
	CHECK(!a.done && !b.done);
	CHECKEQ(call(h_set_evf, 2, e, 2), 0); nap(20);
	CHECK(!a.done && b.done);
	CHECKEQ(b.result, 0); CHECKEQ(RD32(b.out), 2);          /* the pattern as it was when the wait came true */
	CHECKEQ(call(h_set_evf, 2, e, 1), 0); nap(20);
	CHECK(a.done); CHECKEQ(a.result, 0); CHECKEQ(RD32(a.out), 1);
	pthread_join(ta, 0); pthread_join(tb, 0);
	/* WAITCLEAR_PAT (4) clears the waited bits only; WAITCLEAR (2) clears the whole pattern */
	CHECKEQ(call(h_set_evf, 2, e, 0xf0), 0);
	uint32_t out = word();
	CHECKEQ(call(h_wait_evf, 5, e, 0x10, 1 | 4, out, 0), 0); CHECKEQ(RD32(out), 0xf0);
	CHECKEQ(call(h_poll_evf, 4, e, 0xe0, 0, out), 0); CHECKEQ(RD32(out), 0xe0);   /* AND of three bits, no clear */
	CHECKEQ(call(h_wait_evf, 5, e, 0x20, 1 | 2, out, 0), 0); CHECKEQ(RD32(out), 0xe0);
	CHECKEQ(call(h_poll_evf, 4, e, 0xc0, 1, out), E_EVF_COND); CHECKEQ(RD32(out), 0);   /* WAITCLEAR took everything */
	/* AND mode waits for all of its bits, across several sets */
	EvfWait c = { e, 0x3, 0 | 4, word(), 0 };
	pthread_t tc = go(evf_waiter, &c); nap(20);
	call(h_set_evf, 2, e, 1); nap(20); CHECK(!c.done);
	call(h_set_evf, 2, e, 2); nap(20); CHECK(c.done); CHECKEQ(c.result, 0); CHECKEQ(RD32(c.out), 3);
	pthread_join(tc, 0);
	/* a timeout: WAIT_TIMEOUT, *out the pattern at that time, *timeout 0 */
	uint32_t tmo = word(); WR32(tmo, 20000);
	call(h_set_evf, 2, e, 0x100);
	CHECKEQ(call(h_wait_evf, 5, e, 0x200, 1, out, tmo), E_WAIT_TIMEOUT); CHECKEQ(RD32(out), 0x100); CHECKEQ(RD32(tmo), 0);
	/* ClearEventFlag keeps the bits given */
	CHECKEQ(call(h_clear_evf, 2, e, ~0x100u), 0); CHECKEQ(call(h_poll_evf, 4, e, 0x100, 1, out), E_EVF_COND);
	/* illegal arguments */
	CHECKEQ(call(h_wait_evf, 5, e, 0, 1, out, 0), E_ILLEGAL_PATTERN);
	CHECKEQ(call(h_wait_evf, 5, e, 1, 8, out, 0), E_ILLEGAL_MODE);
	CHECKEQ(call(h_wait_evf, 5, 0x40020fff, 1, 1, out, 0), E_UNKNOWN_EVF_ID);
	/* without SCE_EVENT_WAITMULTIPLE a second waiter is refused */
	uint32_t s = call(h_create_evf, 4, name, 0, 0, 0);
	EvfWait d = { s, 1, 1, 0, 0 };
	pthread_t td = go(evf_waiter, &d); nap(20);
	CHECKEQ(call(h_wait_evf, 5, s, 2, 1, 0, 0), E_EVF_MULTI);
	/* delete wakes the waiter with WAIT_DELETE; the uid is gone */
	CHECKEQ(call(h_delete_evf, 1, s), 0); pthread_join(td, 0);
	CHECKEQ(d.result, E_WAIT_DELETE); CHECKEQ(call(h_set_evf, 2, s, 1), E_UNKNOWN_EVF_ID);
	/* the info structure */
	uint32_t info = guest_alloc(0x38, 4); WR32(info, 0x38);
	CHECKEQ(call(h_evf_info, 2, e, info), 0); CHECKEQ(RD32(info + 0x30), 0); CHECKEQ(RD32(info + 0x34), 0);
	WR32(info, 0x30); CHECKEQ(call(h_evf_info, 2, e, info), E_INVALID_ARGUMENT_SIZE);
}

/* ================= semaphores */
typedef struct { uint32_t uid, need, timeout, result, t_done; volatile int done; } SemaWait;
static void sema_waiter(void *p) { SemaWait *w = p; w->result = call(h_wait_sema, 3, w->uid, w->need, w->timeout); w->t_done = (uint32_t)guest_time_us(); w->done = 1; }
static void test_sema(void) {
	uint32_t name = guest_alloc(16, 4); strcpy((char *)G(name), "sema");
	CHECKEQ(call(h_create_sema, 5, name, 0, 3, 2, 0), E_ILLEGAL_COUNT);   /* init > max */
	uint32_t s = call(h_create_sema, 5, name, 0, 1, 2, 0);
	CHECK(s & 1);
	CHECKEQ(call(h_poll_sema, 2, s, 1), 0);
	CHECKEQ(call(h_poll_sema, 2, s, 1), E_SEMA_ZERO);
	CHECKEQ(call(h_signal_sema, 2, s, 2), 0);
	CHECKEQ(call(h_signal_sema, 2, s, 1), E_SEMA_OVF);   /* past the maximum: refused, count unchanged */
	CHECKEQ(call(h_wait_sema, 3, s, 2, 0), 0);
	CHECKEQ(call(h_wait_sema, 3, s, 3, 0), E_ILLEGAL_COUNT);   /* more than the maximum can ever hold */
	CHECKEQ(call(h_wait_sema, 3, s, 0, 0), E_ILLEGAL_COUNT);
	uint32_t tmo = word(); WR32(tmo, 20000);
	CHECKEQ(call(h_wait_sema, 3, s, 1, tmo), E_WAIT_TIMEOUT); CHECKEQ(RD32(tmo), 0);
	/* queue order: the first waiter (needing 2) is served before a later one (needing 1), and a
	 * signal of 1 wakes neither; the time left is written back to a waiter's *timeout */
	SemaWait a = { s, 2, 0 }, b = { s, 1, word() }; WR32(b.timeout, 5000000);
	pthread_t ta = go(sema_waiter, &a); nap(20); pthread_t tb = go(sema_waiter, &b); nap(20);
	CHECKEQ(call(h_signal_sema, 2, s, 1), 0); nap(20); CHECK(!a.done && !b.done);
	CHECKEQ(call(h_signal_sema, 2, s, 1), 0); nap(20); CHECK(a.done && !b.done); CHECKEQ(a.result, 0);
	CHECKEQ(call(h_signal_sema, 2, s, 1), 0); nap(20); CHECK(b.done); CHECKEQ(b.result, 0);
	CHECK(RD32(b.timeout) > 4000000 && RD32(b.timeout) < 5000000);
	pthread_join(ta, 0); pthread_join(tb, 0);
	/* priority-ordered queue (attr 0x2000): the higher-priority (lower number) waiter goes first */
	uint32_t p = call(h_create_sema, 5, name, 0x2000, 0, 10, 0);
	SemaWait lo = { p, 1, 0 }, hi = { p, 1, 0 };
	pthread_t tlo = go_prio(sema_waiter, &lo, 180); nap(20); pthread_t thi = go_prio(sema_waiter, &hi, 100); nap(20);
	CHECKEQ(call(h_signal_sema, 2, p, 1), 0); nap(20); CHECK(hi.done && !lo.done);
	CHECKEQ(call(h_signal_sema, 2, p, 1), 0); nap(20); CHECK(lo.done);
	pthread_join(tlo, 0); pthread_join(thi, 0);
	/* delete under a waiter */
	SemaWait d = { s, 1, 0 };
	pthread_t td = go(sema_waiter, &d); nap(20);
	CHECKEQ(call(h_delete_sema, 1, s), 0); pthread_join(td, 0); CHECKEQ(d.result, E_WAIT_DELETE);
	CHECKEQ(call(h_wait_sema, 3, s, 1, 0), E_UNKNOWN_SEMA_ID);
	/* open by name finds a live one */
	CHECKEQ(call(h_open_sema, 1, name), p);
	uint32_t info = guest_alloc(0x3c, 4); WR32(info, 0x3c);
	CHECKEQ(call(h_sema_info, 2, p, info), 0); CHECKEQ(RD32(info + 0x30), 0); CHECKEQ(RD32(info + 0x34), 10);
}

/* ================= mutexes and lightweight mutexes */
typedef struct { uint32_t work, uid, n, timeout, result; int lw; volatile int done, locked; volatile int release; } MxWait;
static void mx_locker(void *p) {   /* lock, hold until told, unlock */
	MxWait *w = p;
	w->result = w->lw ? call(h_lock_lwmutex, 3, w->work, w->n, w->timeout) : call(h_lock_mutex, 3, w->uid, w->n, w->timeout);
	w->locked = 1;
	while (!w->release && !w->result) nap(1);
	if (!w->result) w->lw ? call(h_unlock_lwmutex, 2, w->work, w->n) : call(h_unlock_mutex, 2, w->uid, w->n);
	w->done = 1;
}
static void test_mutex(void) {
	uint32_t name = guest_alloc(16, 4); strcpy((char *)G(name), "mx");
	/* non-recursive kernel mutex */
	uint32_t m = call(h_create_mutex, 4, name, 0, 0, 0);
	CHECK(m & 1);
	CHECKEQ(call(h_unlock_mutex, 2, m, 1), E_MUTEX_NOT_OWNED);
	CHECKEQ(call(h_lock_mutex, 3, m, 1, 0), 0);
	CHECKEQ(call(h_lock_mutex, 3, m, 1, 0), E_MUTEX_RECURSIVE);
	CHECKEQ(call(h_lock_mutex, 3, m, 2, 0), E_ILLEGAL_COUNT);
	CHECKEQ(call(h_trylock_mutex, 2, m, 1), E_MUTEX_RECURSIVE);
	/* another thread: trylock fails with FAILED_TO_OWN, a timed lock times out, a lock waits and gets the handoff */
	MxWait t1 = { 0, m, 1, word() }; WR32(t1.timeout, 20000);
	pthread_t a = go(mx_locker, &t1); pthread_join(a, 0); CHECKEQ(t1.result, E_WAIT_TIMEOUT);
	MxWait t2 = { 0, m, 1, 0 };
	a = go(mx_locker, &t2); nap(20); CHECK(!t2.locked);
	CHECKEQ(call(h_unlock_mutex, 2, m, 1), 0); nap(20); CHECK(t2.locked); CHECKEQ(t2.result, 0);
	CHECKEQ(call(h_trylock_mutex, 2, m, 1), E_MUTEX_FAILED_TO_OWN);
	CHECKEQ(call(h_unlock_mutex, 2, m, 1), E_MUTEX_NOT_OWNED);   /* held by the other thread */
	t2.release = 1; pthread_join(a, 0);
	CHECKEQ(call(h_trylock_mutex, 2, m, 1), 0); CHECKEQ(call(h_unlock_mutex, 2, m, 1), 0);
	/* recursive lightweight mutex: counts, underflow, overflow of the unlock count */
	uint32_t w = guest_alloc(32, 8); memset(G(w), 0, 32);
	CHECKEQ(call(h_create_lwmutex, 5, w, name, 2, 1, 0), 0);   /* created locked once by the creator */
	CHECKEQ(call(h_lock_lwmutex, 3, w, 2, 0), 0);               /* count 3 */
	CHECKEQ(call(h_unlock_lwmutex, 2, w, 4), E_MUTEX_UNLOCK_UDF + 0x40);
	CHECKEQ(call(h_unlock_lwmutex, 2, w, 2), 0);
	MxWait t3 = { w, 0, 1, 0, 0, 1 };
	a = go(mx_locker, &t3); nap(20); CHECK(!t3.locked);
	CHECKEQ(call(h_trylock_lwmutex, 2, w, 1), 0);               /* the owner may re-enter */
	CHECKEQ(call(h_unlock_lwmutex, 2, w, 2), 0); nap(20); CHECK(t3.locked);
	CHECKEQ(call(h_trylock_lwmutex, 2, w, 1), E_MUTEX_FAILED_TO_OWN + 0x40);
	CHECKEQ(call(h_unlock_lwmutex, 2, w, 1), E_LW_MUTEX_NOT_OWNED);
	t3.release = 1; pthread_join(a, 0);
	CHECKEQ(call(h_lock_lwmutex, 3, w, 1, 0), 0); CHECKEQ(call(h_unlock_lwmutex, 2, w, 1), 0);
	/* an lwmutex never created (zeroed work area) works as a recursive one */
	uint32_t w2 = guest_alloc(32, 8); memset(G(w2), 0, 32);
	CHECKEQ(call(h_lock_lwmutex, 3, w2, 1, 0), 0); CHECKEQ(call(h_lock_lwmutex, 3, w2, 1, 0), 0); CHECKEQ(call(h_unlock_lwmutex, 2, w2, 2), 0);
	/* delete: a waiter gets WAIT_DELETE_LW_MUTEX; the work area then resolves to nothing created (a fresh lock creates anew) */
	CHECKEQ(call(h_lock_lwmutex, 3, w, 1, 0), 0);
	MxWait t4 = { w, 0, 1, 0, 0, 1 };
	a = go(mx_locker, &t4); nap(20);
	CHECKEQ(call(h_delete_lwmutex, 1, w), 0); pthread_join(a, 0); CHECKEQ(t4.result, E_WAIT_DELETE_LW_MUTEX);
	CHECKEQ(call(h_delete_lwmutex, 1, w), E_UNKNOWN_LW_MUTEX_ID);
	CHECKEQ(call(h_create_mutex, 4, name, 0, 2, 0), E_ILLEGAL_COUNT);   /* a non-recursive mutex can't start at 2 */
	uint32_t info = guest_alloc(0x3c, 4); WR32(info, 0x3c);
	CHECKEQ(call(h_mutex_info, 2, m, info), 0); CHECKEQ(RD32(info + 0x30), 0); CHECKEQ(RD32(info + 0x34), 0);
}

/* ================= conditions */
typedef struct { uint32_t work, uid, mx, timeout, result; int lw; volatile int done, waiting; uint32_t count_after; } CondWait;
static void cond_waiter(void *p) {   /* lock the mutex (count 2 if recursive is asked), wait, note the mutex is held again, unlock */
	CondWait *w = p;
	int n = w->count_after ? w->count_after : 1;
	if (w->lw) { CHECKEQ(call(h_lock_lwmutex, 3, w->mx, n, 0), 0); } else { CHECKEQ(call(h_lock_mutex, 3, w->mx, n, 0), 0); }
	w->waiting = 1;
	w->result = w->lw ? call(h_wait_lwcond, 2, w->work, w->timeout) : call(h_wait_cond, 2, w->uid, w->timeout);
	Obj *o = w->lw ? lwmutex(w->mx) : by_uid(w->mx, MUTEX);
	CHECK(o->owner == cur_tid && o->count == n);   /* re-held with the same count, whatever the outcome */
	if (w->lw) call(h_unlock_lwmutex, 2, w->mx, n); else call(h_unlock_mutex, 2, w->mx, n);
	w->done = 1;
}
static void test_cond(void) {
	uint32_t name = guest_alloc(16, 4); strcpy((char *)G(name), "cv");
	uint32_t mw = guest_alloc(32, 8), cw = guest_alloc(16, 8); memset(G(mw), 0, 32); memset(G(cw), 0, 16);
	CHECKEQ(call(h_create_lwmutex, 5, mw, name, 2, 0, 0), 0);
	CHECKEQ(call(h_create_lwcond, 5, cw, name, 0, mw, 0), 0);
	/* a wait releases the mutex: this thread can lock it while the waiter sleeps */
	CondWait a = { cw, 0, mw, 0, 0, 1, 0, 0, 2 };   /* held with count 2 */
	pthread_t ta = go(cond_waiter, &a); nap(20); CHECK(a.waiting && !a.done);
	CHECKEQ(call(h_trylock_lwmutex, 2, mw, 1), 0);
	/* Signal wakes exactly one of two waiters; SignalAll the rest */
	CondWait b = { cw, 0, mw, 0, 0, 1 };
	CHECKEQ(call(h_unlock_lwmutex, 2, mw, 1), 0);
	pthread_t tb = go(cond_waiter, &b); nap(20); CHECK(b.waiting && !b.done);
	CHECKEQ(call(h_signal_lwcond, 1, cw), 0); nap(30); CHECK(a.done != b.done);
	CHECKEQ(call(h_signal_lwcond_all, 1, cw), 0); nap(30); CHECK(a.done && b.done);
	CHECKEQ(a.result, 0); CHECKEQ(b.result, 0);
	pthread_join(ta, 0); pthread_join(tb, 0);
	/* SignalTo wakes the named thread only */
	CondWait c = { cw, 0, mw, 0, 0, 1 }, d = { cw, 0, mw, 0, 0, 1 };
	pthread_t tc = go(cond_waiter, &c); nap(20); pthread_t td = go(cond_waiter, &d); nap(20);
	Obj *o = by_work(cw, LWCOND); uint32_t tid_d = o->tail->tid;
	CHECKEQ(call(h_signal_lwcond_to, 2, cw, 0x40010001), E_UNKNOWN_THREAD_ID);   /* not a waiter */
	CHECKEQ(call(h_signal_lwcond_to, 2, cw, tid_d), 0); nap(30); CHECK(d.done && !c.done);
	CHECKEQ(call(h_signal_lwcond, 1, cw), 0); pthread_join(tc, 0); pthread_join(td, 0);
	/* a signal with no waiter is lost (no memory), as on the kernel */
	CHECKEQ(call(h_signal_lwcond, 1, cw), 0);
	CondWait e = { cw, 0, mw, word(), 0, 1 }; WR32(e.timeout, 30000);
	pthread_t te = go(cond_waiter, &e); pthread_join(te, 0); CHECKEQ(e.result, E_WAIT_TIMEOUT); CHECKEQ(RD32(e.timeout), 0);
	/* waiting without the mutex is refused */
	CHECKEQ(call(h_wait_lwcond, 2, cw, 0), E_LW_MUTEX_NOT_OWNED);
	/* delete under a waiter */
	CondWait f = { cw, 0, mw, 0, 0, 1 };
	pthread_t tf = go(cond_waiter, &f); nap(20);
	CHECKEQ(call(h_delete_lwcond, 1, cw), 0); pthread_join(tf, 0); CHECKEQ(f.result, E_WAIT_DELETE_LW_COND);
	CHECKEQ(call(h_wait_lwcond, 2, cw, 0), E_UNKNOWN_LW_COND_ID);
	/* kernel condition on a kernel mutex: the same, by uid */
	uint32_t m = call(h_create_mutex, 4, name, 2, 0, 0);
	CHECKEQ(call(h_create_cond, 4, name, 0, 0x40020fff, 0), E_UNKNOWN_MUTEX_ID);
	uint32_t cv = call(h_create_cond, 4, name, 0, m, 0); CHECK(cv & 1);
	CHECKEQ(call(h_wait_cond, 2, cv, 0), E_MUTEX_NOT_OWNED);
	CondWait g = { 0, cv, m, 0, 0, 0 }, h = { 0, cv, m, 0, 0, 0 };
	pthread_t tg = go(cond_waiter, &g); nap(20); pthread_t th = go(cond_waiter, &h); nap(20);
	CHECKEQ(call(h_signal_cond, 1, cv), 0); nap(30); CHECK(g.done != h.done);
	CHECKEQ(call(h_signal_cond_all, 1, cv), 0); pthread_join(tg, 0); pthread_join(th, 0);
	uint32_t info = guest_alloc(0x34, 4); WR32(info, 0x34);
	CHECKEQ(call(h_cond_info, 2, cv, info), 0); CHECKEQ(RD32(info + 0x2c), m); CHECKEQ(RD32(info + 0x30), 0);
}

/* ================= simple events */
typedef struct { uint32_t uid, bits, out, udata, timeout, result; volatile int done; } SevWait;
static void sev_waiter(void *p) { SevWait *w = p; w->result = call(h_wait_sevent, 5, w->uid, w->bits, w->out, w->udata, w->timeout); w->done = 1; }
static void test_sevent(void) {
	uint32_t name = guest_alloc(16, 4); strcpy((char *)G(name), "ev");
	uint32_t e = call(h_create_sevent, 4, name, 0x100, 0, 0);   /* auto-reset */
	SevWait a = { e, 1, word(), word(), 0 }, b = { e, 2, word(), word(), 0 };
	pthread_t ta = go(sev_waiter, &a); nap(20); pthread_t tb = go(sev_waiter, &b); nap(20);
	Cpu c = { 0 }; c.r[0] = e; c.r[1] = 2; c.r[2] = 0x11223344; c.r[3] = 0x55667788; h_set_sevent(&c); CHECKEQ(c.r[0], 0);
	nap(20); CHECK(b.done && !a.done); CHECKEQ(b.result, 0); CHECKEQ(RD32(b.out), 2); CHECK(RD64(b.udata) == 0x5566778811223344ull);
	c.r[0] = e; c.r[1] = 1; c.r[2] = 7; c.r[3] = 0; h_set_sevent(&c);
	nap(20); CHECK(a.done); CHECKEQ(RD32(a.out), 1); CHECK(RD64(a.udata) == 7);
	pthread_join(ta, 0); pthread_join(tb, 0);
	uint32_t out = word(), ud = word();
	CHECKEQ(call(h_poll_sevent, 4, e, 3, out, ud), E_EVENT_COND);   /* auto-reset took the bits */
	/* without auto-reset the bits stay */
	uint32_t m = call(h_create_sevent, 4, name, 0, 0, 0);
	c.r[0] = m; c.r[1] = 4; c.r[2] = 9; c.r[3] = 0; h_set_sevent(&c);
	CHECKEQ(call(h_wait_sevent, 5, m, 4, out, ud, 0), 0); CHECKEQ(RD32(out), 4); CHECK(RD64(ud) == 9);
	CHECKEQ(call(h_poll_sevent, 4, m, 4, out, ud), 0);
	CHECKEQ(call(h_clear_sevent, 2, m, 0), 0); CHECKEQ(call(h_poll_sevent, 4, m, 4, out, ud), E_EVENT_COND);
	uint32_t tmo = word(); WR32(tmo, 20000);
	CHECKEQ(call(h_wait_sevent, 5, m, 4, out, ud, tmo), E_WAIT_TIMEOUT);
	CHECKEQ(call(h_wait_sevent, 5, m, 0, out, ud, 0), E_ILLEGAL_PATTERN);
	CHECKEQ(call(h_delete_sevent, 1, m), 0); CHECKEQ(call(h_wait_sevent, 5, m, 4, out, ud, 0), E_UNKNOWN_EVENT_ID);
}

/* ================= SceUlt */
typedef struct { uint32_t obj, n, result; volatile int done; } UltWait;
static void ult_acquirer(void *p) { UltWait *w = p; w->result = call(h_ult_sema_acquire, 2, w->obj, w->n); w->done = 1; }
static void test_ult(void) {
	uint32_t name = guest_alloc(16, 4); strcpy((char *)G(name), "ult");
	CHECK(call(h_ult_pool_size, 2, 4, 8) > 0);
	uint32_t s = guest_alloc(64, 8);
	CHECKEQ(call(h_ult_sema_create, 6, s, name, 1, 0, 0, 0), 0);
	CHECKEQ(call(h_ult_sema_try, 2, s, 1), 0);
	CHECKEQ(call(h_ult_sema_try, 2, s, 1), E_ULT_BUSY);
	CHECKEQ(call(h_ult_sema_acquire, 2, s, 0), E_ULT_RANGE);
	UltWait a = { s, 2 };
	pthread_t ta = go(ult_acquirer, &a); nap(20); CHECK(!a.done);
	CHECKEQ(call(h_ult_sema_release, 2, s, 1), 0); nap(20); CHECK(!a.done);
	CHECKEQ(call(h_ult_sema_release, 2, s, 1), 0); nap(20); CHECK(a.done); CHECKEQ(a.result, 0);
	pthread_join(ta, 0);
	for (int i = 0; i < 100; i++) CHECKEQ(call(h_ult_sema_release, 2, s, 1000), 0);   /* no maximum */
	uint32_t m = guest_alloc(64, 8), cv = guest_alloc(64, 8);
	CHECKEQ(call(h_ult_mutex_create, 5, m, name, 0, 0, 0), 0);
	CHECKEQ(call(h_ult_mutex_unlock, 1, m), E_ULT_PERMISSION);
	CHECKEQ(call(h_ult_mutex_lock, 1, m), 0); CHECKEQ(call(h_ult_mutex_trylock, 1, m), 0);
	CHECKEQ(call(h_ult_mutex_unlock, 1, m), 0); CHECKEQ(call(h_ult_mutex_unlock, 1, m), 0);
	CHECKEQ(call(h_ult_cond_create, 5, cv, name, m, 0, 0), 0);
	CHECKEQ(call(h_ult_cond_wait, 1, cv), E_ULT_PERMISSION);   /* without the mutex */
	CHECKEQ(call(h_ult_sema_destroy, 1, s), 0); CHECKEQ(call(h_ult_sema_destroy, 1, s), 0x80810006u);
	CHECKEQ(call(h_ult_cond_destroy, 1, cv), 0); CHECKEQ(call(h_ult_mutex_destroy, 1, m), 0);
}

/* ================= threads */
static void guest_entry_7(Cpu *c) { c->r[0] = 7; }                           /* returns 7 */
static void guest_entry_exit(Cpu *c) { c->r[0] = 9; h_exit_thread(c); }        /* sceKernelExitThread(9) */
static void guest_entry_args(Cpu *c) { c->r[0] = c->r[0] == 4 && RD32(c->r[1]) == 0xabcd ? 1 : 0; }   /* (args, argp) */
static volatile int handler_calls; static uint32_t handler_common, handler_type_seen;
static uint32_t the_sema;   /* the semaphore block_entry's thread waits on (set by test_threads) */
static void block_entry(Cpu *c) { c->r[0] = call(h_wait_sema, 3, the_sema, 1, 0) == 0 ? 3 : 4; }
static void guest_handler(Cpu *c) { handler_calls++; handler_type_seen |= c->r[0]; handler_common = c->r[3]; c->r[0] = 0; }
static void test_threads(void) {
	uint32_t name = guest_alloc(16, 4); strcpy((char *)G(name), "worker");
	guest_fns[0] = guest_entry_7; guest_fns[1] = guest_entry_exit; guest_fns[2] = guest_entry_args; guest_fns[3] = guest_handler;
	/* the main thread's info */
	uint32_t info = guest_alloc(0x80, 4); WR32(info, 0x80);
	CHECKEQ(call(h_thread_info, 2, 0, info), 0); CHECKEQ(RD32(info + 0x2c), 1); CHECKEQ(RD32(info + 0x40), 160);
	CHECKEQ(call(h_thread_current_priority, 0), 160);
	CHECKEQ(call(h_get_process_id, 0), RD32(info + 4));   /* the process id GetThreadInfo reports */
	CHECKEQ(call(h_change_priority, 2, 0, 0x10000100 - 10), 0); CHECKEQ(call(h_thread_current_priority, 0), 150);
	CHECKEQ(call(h_change_priority, 2, 0, 160), 0);
	CHECKEQ(call(h_thread_affinity, 1, 0), 0x70000);
	WR32(info, 0x7c); CHECKEQ(call(h_thread_info, 2, 0, info), E_INVALID_ARGUMENT_SIZE); WR32(info, 0x80);
	CHECKEQ(call(h_thread_info, 2, 0x40010fff, info), E_UNKNOWN_THREAD_ID);
	/* create, start with an argument block, wait: 0 and the status through the pointer */
	uint32_t t = call(h_create_thread, 7, name, 0x1008, 0x10000100 + 5, 0x1000, 0x80000000u, 0, 0);
	CHECK(t >= 0x40010100 && !(t & 1));
	CHECKEQ(call(h_thread_info, 2, t, info), 0); CHECKEQ(RD32(info + 0x2c), 16); CHECKEQ(RD32(info + 0x3c), 165); CHECKEQ(RD32(info + 0x28), 0x80000000u);
	uint32_t arg = word(); WR32(arg, 0xabcd);
	CHECKEQ(call(h_start_thread, 3, t, 4, arg), 0);
	CHECKEQ(call(h_start_thread, 3, t, 4, arg), E_NOT_DORMANT);
	uint32_t st = word(); WR32(st, 0xffffffff);
	CHECKEQ(call(h_wait_thread_end, 3, t, st, 0), 0); CHECKEQ(RD32(st), 1);
	CHECKEQ(call(h_thread_exit_status, 2, t, st), 0); CHECKEQ(RD32(st), 1);
	CHECKEQ(call(h_delete_thread, 1, t), 0); CHECKEQ(call(h_wait_thread_end, 3, t, st, 0), E_UNKNOWN_THREAD_ID);
	/* a thread ending through sceKernelExitThread; event handlers see its start and exit */
	uint32_t h = call(h_register_thread_event, 5, name, 0xfffffff0u, 4 | 8, 0x100c, 0xc0ffee);
	CHECK(h & 1);
	uint32_t t2 = call(h_create_thread, 7, name, 0x1004, 160, 0x1000, 0, 0, 0);
	CHECKEQ(call(h_start_thread, 3, t2, 0, 0), 0);
	CHECKEQ(call(h_wait_thread_end, 3, t2, st, 0), 0); CHECKEQ(RD32(st), 9);
	CHECKEQ((uint32_t)handler_calls, 2); CHECKEQ(handler_type_seen, 12); CHECKEQ(handler_common, 0xc0ffee);
	CHECKEQ(call(h_unregister_thread_event, 1, h), 0); CHECKEQ(call(h_unregister_thread_event, 1, h), 0x80028061u);
	/* a thread that runs on: exit status not available, a timed wait times out */
	uint32_t s = the_sema = call(h_create_sema, 5, name, 0, 0, 1, 0);
	guest_fns[4] = block_entry;
	uint32_t t3 = call(h_create_thread, 7, name, 0x1010, 160, 0x1000, 0, 0, 0);
	CHECKEQ(call(h_start_thread, 3, t3, 0, 0), 0); nap(20);
	CHECKEQ(call(h_thread_exit_status, 2, t3, st), E_NOT_DORMANT);
	CHECKEQ(call(h_delete_thread, 1, t3), E_NOT_DORMANT);
	uint32_t tmo = word(); WR32(tmo, 20000);
	CHECKEQ(call(h_wait_thread_end, 3, t3, st, tmo), E_WAIT_TIMEOUT);
	CHECKEQ(call(h_thread_info, 2, t3, info), 0); CHECKEQ(RD32(info + 0x2c), 8); CHECKEQ(RD32(info + 0x58), s);   /* WAITING on the semaphore */
	CHECKEQ(call(h_signal_sema, 2, s, 1), 0);
	CHECKEQ(call(h_wait_thread_end, 3, t3, st, 0), 0); CHECKEQ(RD32(st), 3);
	/* delay sleeps at least what was asked */
	uint64_t t0 = guest_time_us(); CHECKEQ(call(h_delay, 1, 20000), 0); CHECK(guest_time_us() - t0 >= 19000);
	CHECK(ret64(h_system_time_wide) > 0);
}


/* ================= reader-writer locks */
typedef struct { uint32_t uid, timeout, result; int write; volatile int done, locked, release; } RwJob;
static void rw_locker(void *p) {   /* lock (read or write), hold until told, unlock */
	RwJob *w = p;
	w->result = w->write ? call(h_lock_write_rwlock, 2, w->uid, w->timeout) : call(h_lock_read_rwlock, 2, w->uid, w->timeout);
	w->locked = 1;
	while (!w->release && !w->result) nap(1);
	if (!w->result) w->write ? call(h_unlock_write_rwlock, 1, w->uid) : call(h_unlock_read_rwlock, 1, w->uid);
	w->done = 1;
}
static void test_rwlock(void) {
	uint32_t name = guest_alloc(16, 4); strcpy((char *)G(name), "rw");
	uint32_t l = call(h_create_rwlock, 3, name, 0, 0);
	CHECK(l & 1);
	CHECKEQ(call(h_unlock_read_rwlock, 1, l), E_RW_LOCK_NOT_OWNED); CHECKEQ(call(h_unlock_write_rwlock, 1, l), E_RW_LOCK_NOT_OWNED);
	/* two read holds by one thread; a writer waits for both to go */
	CHECKEQ(call(h_lock_read_rwlock, 2, l, 0), 0); CHECKEQ(call(h_lock_read_rwlock, 2, l, 0), 0);
	CHECKEQ(call(h_trylock_write_rwlock, 1, l), E_RW_LOCK_FAILED_TO_LOCK);
	CHECKEQ(call(h_lock_write_rwlock, 2, l, 0), E_RW_LOCK_RECURSIVE);   /* a reader asking to write would deadlock */
	RwJob w = { l, 0, 0, 1 };
	pthread_t tw = go(rw_locker, &w); nap(20); CHECK(!w.locked);
	/* writer preference: a new reader waits behind the queued writer; a reader that holds it passes */
	RwJob r2 = { l, 0, 0, 0 };
	pthread_t tr2 = go(rw_locker, &r2); nap(20); CHECK(!r2.locked);
	CHECKEQ(call(h_trylock_read_rwlock, 1, l), 0); CHECKEQ(call(h_unlock_read_rwlock, 1, l), 0);
	uint32_t info = guest_alloc(0x3c, 4); WR32(info, 0x3c);
	CHECKEQ(call(h_rwlock_info, 2, l, info), 0); CHECKEQ(RD32(info + 0x2c), 2); CHECKEQ(RD32(info + 0x30), 0); CHECKEQ(RD32(info + 0x34), 1); CHECKEQ(RD32(info + 0x38), 1);
	CHECKEQ(call(h_unlock_read_rwlock, 1, l), 0); nap(20); CHECK(!w.locked);
	CHECKEQ(call(h_unlock_read_rwlock, 1, l), 0); nap(20); CHECK(w.locked && !r2.locked); CHECKEQ(w.result, 0);
	CHECKEQ(call(h_rwlock_info, 2, l, info), 0); CHECKEQ(RD32(info + 0x2c), 1); CHECK(RD32(info + 0x30) != 0); CHECKEQ(RD32(info + 0x34), 1);
	CHECKEQ(call(h_trylock_read_rwlock, 1, l), E_RW_LOCK_FAILED_TO_LOCK);
	CHECKEQ(call(h_unlock_write_rwlock, 1, l), E_RW_LOCK_NOT_OWNED);   /* held by the other thread */
	w.release = 1; pthread_join(tw, 0); nap(20); CHECK(r2.locked); CHECKEQ(r2.result, 0);   /* the writer's unlock granted the reader */
	/* several readers are granted together; the writer behind them waits for all */
	RwJob r3 = { l, 0, 0, 0 }, w2 = { l, 0, 0, 1 };
	pthread_t tr3 = go(rw_locker, &r3); nap(20); CHECK(r3.locked);   /* no writer queued: passes */
	pthread_t tw2 = go(rw_locker, &w2); nap(20); CHECK(!w2.locked);
	r2.release = 1; pthread_join(tr2, 0); nap(20); CHECK(!w2.locked);
	r3.release = 1; pthread_join(tr3, 0); nap(20); CHECK(w2.locked);
	w2.release = 1; pthread_join(tw2, 0);
	/* a timed lock times out; a non-recursive writer can't lock again; a recursive one (attr 2) can */
	CHECKEQ(call(h_lock_write_rwlock, 2, l, 0), 0);
	CHECKEQ(call(h_lock_write_rwlock, 2, l, 0), E_RW_LOCK_RECURSIVE);
	CHECKEQ(call(h_lock_read_rwlock, 2, l, 0), 0); CHECKEQ(call(h_unlock_read_rwlock, 1, l), 0);   /* the writer may read */
	RwJob t1 = { l, word(), 0, 0 }; WR32(t1.timeout, 20000);
	pthread_t tt = go(rw_locker, &t1); pthread_join(tt, 0); CHECKEQ(t1.result, E_WAIT_TIMEOUT); CHECKEQ(RD32(t1.timeout), 0);
	CHECKEQ(call(h_unlock_write_rwlock, 1, l), 0);
	uint32_t lr = call(h_create_rwlock, 3, name, 2, 0);
	CHECKEQ(call(h_lock_write_rwlock, 2, lr, 0), 0); CHECKEQ(call(h_lock_write_rwlock, 2, lr, 0), 0);
	CHECKEQ(call(h_unlock_write_rwlock, 1, lr), 0);
	RwJob r4 = { lr, 0, 0, 0 };
	pthread_t tr4 = go(rw_locker, &r4); nap(20); CHECK(!r4.locked);   /* still held once */
	CHECKEQ(call(h_unlock_write_rwlock, 1, lr), 0); nap(20); CHECK(r4.locked);
	r4.release = 1; pthread_join(tr4, 0);
	/* delete under a waiter */
	CHECKEQ(call(h_lock_write_rwlock, 2, lr, 0), 0);
	RwJob d = { lr, 0, 0, 0 };
	pthread_t td = go(rw_locker, &d); nap(20);
	CHECKEQ(call(h_delete_rwlock, 1, lr), 0); pthread_join(td, 0); CHECKEQ(d.result, E_WAIT_DELETE);
	CHECKEQ(call(h_lock_read_rwlock, 2, lr, 0), E_UNKNOWN_RW_LOCK_ID);
	WR32(info, 0x38); CHECKEQ(call(h_rwlock_info, 2, l, info), E_INVALID_ARGUMENT_SIZE);
}

/* ================= message pipes */
typedef struct { uint32_t uid, buf, size, mode, result_p, timeout, result; int send; volatile int done; } PipeJob;
static void pipe_job(void *p) {
	PipeJob *w = p;
	w->result = call(w->send ? h_send_msgpipe : h_receive_msgpipe, 6, w->uid, w->buf, w->size, w->mode, w->result_p, w->timeout);
	w->done = 1;
}
static uint32_t bytes(const char *s) { uint32_t a = guest_alloc(64, 8); memset(G(a), 0, 64); strcpy((char *)G(a), s); return a; }
static void test_msgpipe(void) {
	uint32_t name = guest_alloc(16, 4); strcpy((char *)G(name), "pipe");
	CHECKEQ(call(h_create_msgpipe, 5, name, 0x40, 0, 0, 0), E_ILLEGAL_SIZE);
	uint32_t p = call(h_create_msgpipe, 5, name, 0x40, 0, 16, 0);
	CHECK(p & 1);
	uint32_t res = word(), out = guest_alloc(64, 8);
	/* 16 bytes of room: a 10-byte message goes; a second doesn't fit whole (FULL) but ASAP sends what fits */
	CHECKEQ(call(h_trysend_msgpipe, 5, p, bytes("0123456789"), 10, 1, res), 0); CHECKEQ(RD32(res), 10);
	CHECKEQ(call(h_trysend_msgpipe, 5, p, bytes("abcdefghij"), 10, 1, res), E_MSG_PIPE_FULL);
	CHECKEQ(call(h_trysend_msgpipe, 5, p, bytes("abcdefghij"), 10, 0, res), 0); CHECKEQ(RD32(res), 6);
	CHECKEQ(call(h_trysend_msgpipe, 5, p, bytes("x"), 1, 0, res), E_MSG_PIPE_FULL);
	CHECKEQ(call(h_trysend_msgpipe, 5, p, bytes("x"), 17, 0, res), E_ILLEGAL_SIZE);
	uint32_t info = guest_alloc(0x3c, 4); WR32(info, 0x3c);
	CHECKEQ(call(h_msgpipe_info, 2, p, info), 0); CHECKEQ(RD32(info + 0x2c), 16); CHECKEQ(RD32(info + 0x30), 0);
	/* a peek leaves the bytes; a receive takes them in order */
	CHECKEQ(call(h_tryreceive_msgpipe, 5, p, out, 4, 0x100, res), 0); CHECKEQ(RD32(res), 4); CHECK(!memcmp(G(out), "0123", 4));
	CHECKEQ(call(h_tryreceive_msgpipe, 5, p, out, 16, 1, res), 0); CHECKEQ(RD32(res), 16); CHECK(!memcmp(G(out), "0123456789abcdef", 16));
	CHECKEQ(call(h_tryreceive_msgpipe, 5, p, out, 1, 0, res), E_MSG_PIPE_EMPTY);
	CHECKEQ(call(h_receive_msgpipe, 6, p, out, 1, 0x10, res, 0), E_MSG_PIPE_EMPTY);   /* DONT_WAIT */
	/* a FULL receiver waits for all 8 bytes, across two sends */
	PipeJob r = { p, out, 8, 1, word(), 0, 0, 0 };
	pthread_t tr = go(pipe_job, &r); nap(20); CHECK(!r.done);
	CHECKEQ(call(h_trysend_msgpipe, 5, p, bytes("ABCD"), 4, 1, res), 0); nap(20); CHECK(!r.done);
	CHECKEQ(call(h_trysend_msgpipe, 5, p, bytes("EFGH"), 4, 1, res), 0); pthread_join(tr, 0);
	CHECKEQ(r.result, 0); CHECKEQ(RD32(r.result_p), 8); CHECK(!memcmp(G(out), "ABCDEFGH", 8));
	/* an ASAP receiver takes what arrives */
	PipeJob ra = { p, out, 8, 0, word(), 0, 0, 0 };
	pthread_t tra = go(pipe_job, &ra); nap(20); CHECK(!ra.done);
	CHECKEQ(call(h_trysend_msgpipe, 5, p, bytes("xyz"), 3, 1, res), 0); pthread_join(tra, 0);
	CHECKEQ(ra.result, 0); CHECKEQ(RD32(ra.result_p), 3); CHECK(!memcmp(G(out), "xyz", 3));
	/* a full pipe: two FULL senders queue in order and go one at a time as room is made; a later sender never overtakes an earlier one */
	CHECKEQ(call(h_trysend_msgpipe, 5, p, bytes("0123456789abcdef"), 16, 1, res), 0);
	PipeJob s1 = { p, bytes("11111111"), 8, 1, word(), 0, 0, 1 }, s2 = { p, bytes("22222222"), 8, 1, word(), 0, 0, 1 };
	pthread_t ts1 = go(pipe_job, &s1); nap(20); pthread_t ts2 = go(pipe_job, &s2); nap(20);
	CHECKEQ(call(h_msgpipe_info, 2, p, info), 0); CHECKEQ(RD32(info + 0x34), 2); CHECKEQ(RD32(info + 0x38), 0);
	CHECKEQ(call(h_tryreceive_msgpipe, 5, p, out, 4, 1, res), 0); nap(20); CHECK(!s1.done && !s2.done);   /* 4 free: neither fits */
	CHECKEQ(call(h_tryreceive_msgpipe, 5, p, out, 4, 1, res), 0); nap(20); CHECK(s1.done && !s2.done); CHECKEQ(RD32(s1.result_p), 8);
	CHECKEQ(call(h_tryreceive_msgpipe, 5, p, out, 8, 1, res), 0); CHECK(!memcmp(G(out), "89abcdef", 8)); nap(20); CHECK(s2.done);
	CHECKEQ(call(h_tryreceive_msgpipe, 5, p, out, 16, 1, res), 0); CHECK(!memcmp(G(out), "1111111122222222", 16));
	pthread_join(ts1, 0); pthread_join(ts2, 0);
	/* a timed receive times out; Cancel wakes waiters with WAIT_CANCEL and counts them; Delete with WAIT_DELETE */
	uint32_t tmo = word(); WR32(tmo, 20000);
	CHECKEQ(call(h_receive_msgpipe, 6, p, out, 1, 0, res, tmo), E_WAIT_TIMEOUT); CHECKEQ(RD32(tmo), 0);
	PipeJob c1 = { p, out, 8, 1, 0, 0, 0, 0 };
	pthread_t tc1 = go(pipe_job, &c1); nap(20);
	uint32_t ns = word(), nr = word();
	CHECKEQ(call(h_cancel_msgpipe, 3, p, ns, nr), 0); pthread_join(tc1, 0); CHECKEQ(c1.result, E_WAIT_CANCEL); CHECKEQ(RD32(ns), 0); CHECKEQ(RD32(nr), 1);
	PipeJob d1 = { p, out, 8, 1, 0, 0, 0, 0 };
	pthread_t td1 = go(pipe_job, &d1); nap(20);
	CHECKEQ(call(h_delete_msgpipe, 1, p), 0); pthread_join(td1, 0); CHECKEQ(d1.result, E_WAIT_DELETE);
	CHECKEQ(call(h_trysend_msgpipe, 5, p, out, 1, 0, res), E_UNKNOWN_MSG_PIPE_ID);
}

/* ================= callbacks and the CB waits */
static volatile int cb_calls; static uint32_t cb_id, cb_count, cb_arg, cb_common, cb_tid, cb_ret;
static void cb_fn(Cpu *c) { cb_calls++; cb_id = c->r[0]; cb_count = c->r[1]; cb_arg = c->r[2]; cb_common = c->r[3]; cb_tid = cur_tid; c->r[0] = cb_ret; }
typedef struct { uint32_t name, cb, sema, result, delay_result; volatile int done, ready; uint64_t t_end; } CbJob;
static void cb_owner(void *p) {   /* creates the callback (so it owns it), then waits with callbacks */
	CbJob *j = p;
	j->cb = call(h_create_callback, 4, j->name, 0, 0x1018, 0xbeef);
	j->ready = 1;
	j->result = call(h_wait_sema_cb, 3, j->sema, 1, 0);
	j->done = 1;
}
static void cb_delayer(void *p) { CbJob *j = p; j->ready = 1; j->delay_result = call(h_delay_cb, 1, 200000); j->t_end = guest_time_us(); j->done = 1; }
static void cb_end_waiter(void *p) { CbJob *j = p; j->ready = 1; j->result = call(h_wait_thread_end_cb, 3, j->sema /* a thread uid here */, 0, 0); j->done = 1; }
static void test_callbacks(void) {
	uint32_t name = guest_alloc(16, 4); strcpy((char *)G(name), "cb");
	guest_fns[6] = cb_fn;
	CHECKEQ(call(h_create_callback, 4, name, 1, 0x1018, 0), E_ILLEGAL_ATTR);
	CHECKEQ(call(h_create_callback, 4, name, 0, 0, 0), E_ILLEGAL_ADDR);
	/* a callback owned by another thread: notified twice, undelivered until the owner checks; the count accumulates; Cancel clears it */
	CbJob j = { name, 0, call(h_create_sema, 5, name, 0, 0, 1, 0) };
	pthread_t tj = go(cb_owner, &j); while (!j.ready) nap(1); nap(20);   /* the owner is in WaitSemaCB */
	CHECK(j.cb & 1);
	uint32_t info = guest_alloc(0x3c, 4); WR32(info, 0x3c);
	CHECKEQ(call(h_callback_info, 2, j.cb, info), 0); CHECK(RD32(info + 0x28) != cur_tid); CHECKEQ(RD32(info + 0x2c), 0x1018); CHECKEQ(RD32(info + 0x30), 0xbeef); CHECKEQ(RD32(info + 0x34), 0);
	CHECKEQ(call(h_check_callback, 0), 0);   /* nothing pending for this thread */
	/* notify: the owner's CB wait is interrupted, the callback runs on the owner with (notifier, count, arg, common), the wait goes on */
	CHECKEQ(call(h_notify_callback, 2, j.cb, 42), 0); nap(30);
	CHECKEQ((uint32_t)cb_calls, 1); CHECKEQ(cb_id, cur_tid); CHECKEQ(cb_count, 1); CHECKEQ(cb_arg, 42); CHECKEQ(cb_common, 0xbeef); CHECKEQ(cb_tid, RD32(info + 0x28));
	CHECK(!j.done);
	CHECKEQ(call(h_signal_sema, 2, j.sema, 1), 0); pthread_join(tj, 0); CHECKEQ(j.result, 0);
	/* with the owner gone from its wait, notifications pile up for its next check */
	CHECKEQ(call(h_notify_callback, 2, j.cb, 7), 0); CHECKEQ(call(h_notify_callback, 2, j.cb, 8), 0);
	CHECKEQ(call(h_callback_count, 1, j.cb), 2);
	CHECKEQ(call(h_cancel_callback, 1, j.cb), 0); CHECKEQ(call(h_callback_count, 1, j.cb), 0);
	CHECKEQ(call(h_delete_callback, 1, j.cb), 0); CHECKEQ(call(h_notify_callback, 2, j.cb, 1), E_UNKNOWN_CALLBACK_ID);
	/* this thread's own callback: CheckCallback runs it and says so; one returning nonzero deletes itself */
	uint32_t mine = call(h_create_callback, 4, name, 0, 0x1018, 0x77);
	CHECKEQ(call(h_notify_callback, 2, mine, 5), 0); CHECKEQ(call(h_notify_callback, 2, mine, 6), 0);
	cb_calls = 0; cb_ret = 1;
	CHECKEQ(call(h_check_callback, 0), 1); CHECKEQ((uint32_t)cb_calls, 1); CHECKEQ(cb_count, 2); CHECKEQ(cb_arg, 6); CHECKEQ(cb_common, 0x77); CHECKEQ(cb_tid, cur_tid);
	CHECKEQ(call(h_callback_count, 1, mine), E_UNKNOWN_CALLBACK_ID);   /* deleted by its own return value */
	cb_ret = 0;
	/* a CB wait that is satisfied at once still delivers pending callbacks first */
	uint32_t mine2 = call(h_create_callback, 4, name, 0, 0x1018, 0x78);
	CHECKEQ(call(h_notify_callback, 2, mine2, 9), 0); cb_calls = 0;
	CHECKEQ(call(h_signal_sema, 2, j.sema, 1), 0); CHECKEQ(call(h_wait_sema_cb, 3, j.sema, 1, 0), 0); CHECKEQ((uint32_t)cb_calls, 1); CHECKEQ(cb_arg, 9);
	/* a CB wait with a timeout keeps the time left across the interruption */
	uint32_t tmo = word(); WR32(tmo, 100000);
	CbJob k = { name, 0, j.sema };
	pthread_t tk = go(cb_owner, &k); while (!k.ready) nap(1); nap(20);
	cb_calls = 0; CHECKEQ(call(h_notify_callback, 2, k.cb, 1), 0); nap(30); CHECKEQ((uint32_t)cb_calls, 1); CHECK(!k.done);
	CHECKEQ(call(h_delete_sema, 1, j.sema), 0); pthread_join(tk, 0); CHECKEQ(k.result, E_WAIT_DELETE);
	/* DelayThreadCB: the callback runs during the delay, which still lasts its full time */
	CbJob d = { name, 0, 0 };
	pthread_t td = go(cb_delayer, &d); while (!d.ready) nap(1); nap(20);
	uint32_t dcb; { Thread *t = 0; for (int i = 0; i < nthreads; i++) if (threads[i].started && !threads[i].done && threads[i].wait_type == 2) t = &threads[i]; CHECK(t != 0);
		Obj *o = new_obj(CALLBACK, name, 0); o->owner = t ? t->uid : 0; o->func = 0x1018; o->common = 0x79; dcb = o->uid; }   /* a callback owned by the delaying thread */
	uint64_t t0 = guest_time_us(); cb_calls = 0;
	CHECKEQ(call(h_notify_callback, 2, dcb, 3), 0); nap(30); CHECKEQ((uint32_t)cb_calls, 1); CHECKEQ(cb_common, 0x79); CHECK(!d.done);
	pthread_join(td, 0); CHECKEQ(d.delay_result, 0); CHECK(d.t_end - t0 >= 140000);
	/* WaitThreadEndCB likewise */
	uint32_t s2 = the_sema = call(h_create_sema, 5, name, 0, 0, 1, 0);
	uint32_t t3 = call(h_create_thread, 7, name, 0x1010, 160, 0x1000, 0, 0, 0);
	CHECKEQ(call(h_start_thread, 3, t3, 0, 0), 0);
	CbJob e = { name, 0, t3 };
	pthread_t te = go(cb_end_waiter, &e); while (!e.ready) nap(1); nap(20);
	uint32_t ecb; { Thread *t = 0; for (int i = 0; i < nthreads; i++) if (threads[i].started && !threads[i].done && threads[i].wait_type == 9) t = &threads[i]; CHECK(t != 0);
		Obj *o = new_obj(CALLBACK, name, 0); o->owner = t ? t->uid : 0; o->func = 0x1018; o->common = 0x7a; ecb = o->uid; }
	cb_calls = 0; CHECKEQ(call(h_notify_callback, 2, ecb, 4), 0); nap(30); CHECKEQ((uint32_t)cb_calls, 1); CHECKEQ(cb_common, 0x7a); CHECK(!e.done);
	CHECKEQ(call(h_signal_sema, 2, s2, 1), 0); pthread_join(te, 0); CHECKEQ(e.result, 0);
}

/* ================= fibers */
static uint32_t fib_a, fib_b, fib_c, fib_d;   /* the guest SceFibers */
static uint32_t fa_init, fa_run, fb_init, fb_run, fa_resumed1, fa_resumed2, fc_err1, fc_err2; static int fd_starts, fa_tid_ok;
static void fentry_a(Cpu *c) {
	fa_init = c->r[0]; fa_run = c->r[1]; fa_tid_ok = cur_tid == main_thread.uid;
	uint32_t self = word(); c->r[0] = self; h_fiber_get_self(c); CHECKEQ(c->r[0], 0); CHECKEQ(RD32(self), fib_a);
	c->r[0] = fib_a; c->r[1] = 1; c->r[2] = 0; h_fiber_run(c); CHECKEQ(c->r[0], E_FIBER_PERMISSION);   /* Run is a thread's call */
	uint32_t ar = word(); c->r[0] = 11; c->r[1] = ar; h_fiber_return(c); CHECKEQ(c->r[0], 0);   /* back to the thread with 11; resumed with... */
	fa_resumed1 = RD32(ar);
	uint32_t ar2 = word(); c->r[0] = fib_b; c->r[1] = 33; c->r[2] = ar2; h_fiber_switch(c); CHECKEQ(c->r[0], 0);   /* to B; resumed by a later Run */
	fa_resumed2 = RD32(ar2);
	c->r[0] = 66; c->r[1] = 0; h_fiber_return(c);
}
static void fentry_b(Cpu *c) { fb_init = c->r[0]; fb_run = c->r[1]; c->r[0] = 44; c->r[1] = 0; h_fiber_return(c); CHECK(0); /* never resumed: finalized while parked */ }
static void fentry_c(Cpu *c) {   /* switching to itself: STATE; returning without a pointer */
	c->r[0] = fib_c; c->r[1] = 0; c->r[2] = 0; h_fiber_switch(c); fc_err1 = c->r[0];
	c->r[0] = fib_a; c->r[1] = 0; c->r[2] = 0; h_fiber_switch(c); fc_err2 = c->r[0];   /* A is suspended (parked): allowed -- but A then returns 66 to the thread; see test */
	c->r[0] = 1; c->r[1] = 0; h_fiber_return(c);
}
static void fentry_d(Cpu *c) { fd_starts++; CHECKEQ(c->r[0], 4); c->r[0] = 5 + c->r[1]; c->r[1] = 0; h_fiber_return(c); CHECK(0); }   /* context-less: starts over every run */
static void test_fibers(void) {
	uint32_t name = guest_alloc(16, 4); strcpy((char *)G(name), "fib");
	guest_fns[7] = fentry_a; guest_fns[8] = fentry_b; guest_fns[9] = fentry_c; guest_fns[10] = fentry_d;
	fib_a = guest_alloc(128, 8); fib_b = guest_alloc(128, 8); fib_c = guest_alloc(128, 8); fib_d = guest_alloc(128, 8);
	uint32_t ctx_a = guest_alloc(0x4000, 8), ctx_b = guest_alloc(0x4000, 8), ctx_c = guest_alloc(0x4000, 8);
	CHECKEQ(call(h_fiber_init, 7, fib_a, name, 0x101c, 7, ctx_a, 100, 0), E_FIBER_RANGE);
	CHECKEQ(call(h_fiber_init, 7, fib_a, name, 0x101c, 7, ctx_a, 0, 0), E_FIBER_INVALID);
	CHECKEQ(call(h_fiber_init, 7, fib_a, name, 0x101c, 7, 0, 0x4000, 0), E_FIBER_INVALID);
	CHECKEQ(call(h_fiber_init, 7, fib_a + 4, name, 0x101c, 7, ctx_a, 0x4000, 0), E_FIBER_ALIGNMENT);
	CHECKEQ(call(h_fiber_init, 7, 0, name, 0x101c, 7, ctx_a, 0x4000, 0), E_FIBER_NULL);
	CHECKEQ(call(h_fiber_init, 7, fib_a, name, 0x101c, 7, ctx_a, 0x4000, 0), 0);
	CHECKEQ(call(h_fiber_init, 7, fib_b, name, 0x1020, 8, ctx_b, 0x4000, 0), 0);
	CHECKEQ(call(h_fiber_init, 7, fib_c, name, 0x1024, 9, ctx_c, 0x4000, 0), 0);
	CHECKEQ(call(h_fiber_init, 7, fib_d, name, 0x1028, 4, 0, 0, 0), 0);
	uint32_t self = word(); CHECKEQ(call(h_fiber_get_self, 1, self), 0); CHECKEQ(RD32(self), 0);   /* on a thread */
	CHECKEQ(call(h_fiber_return, 2, 0, 0), E_FIBER_PERMISSION); CHECKEQ(call(h_fiber_switch, 3, fib_a, 0, 0), E_FIBER_PERMISSION);
	CHECKEQ(call(h_fiber_run, 3, 0, 0, 0), E_FIBER_NULL); CHECKEQ(call(h_fiber_run, 3, ctx_a, 0, 0), E_FIBER_INVALID);
	uint32_t ret = word();
	/* run A: entry(7, 10); it returns 11 */
	CHECKEQ(call(h_fiber_run, 3, fib_a, 10, ret), 0); CHECKEQ(RD32(ret), 11); CHECKEQ(fa_init, 7); CHECKEQ(fa_run, 10); CHECK(fa_tid_ok);
	/* run A again with 22: it resumes after its return, switches to B (entry(8, 33)), B returns 44 to the thread */
	CHECKEQ(call(h_fiber_run, 3, fib_a, 22, ret), 0); CHECKEQ(RD32(ret), 44); CHECKEQ(fa_resumed1, 22); CHECKEQ(fb_init, 8); CHECKEQ(fb_run, 33);
	/* run A with 55: it resumes after its switch, returns 66 */
	CHECKEQ(call(h_fiber_run, 3, fib_a, 55, ret), 0); CHECKEQ(RD32(ret), 66); CHECKEQ(fa_resumed2, 55);
	/* C switches to itself (STATE), then to A (parked after its last return): A runs to its end... A's entry returns -- then back to the thread */
	CHECKEQ(call(h_fiber_run, 3, fib_c, 0, ret), 0); CHECKEQ(fc_err1, E_FIBER_STATE); CHECKEQ(fc_err2, 0); CHECKEQ(RD32(ret), 0);   /* A's entry returned: the host hands control back with 0 */
	/* a context-less fiber starts from its entry at every run */
	CHECKEQ(call(h_fiber_run, 3, fib_d, 1, ret), 0); CHECKEQ(RD32(ret), 6); CHECKEQ((uint32_t)fd_starts, 1);
	CHECKEQ(call(h_fiber_run, 3, fib_d, 2, ret), 0); CHECKEQ(RD32(ret), 7); CHECKEQ((uint32_t)fd_starts, 2);
	/* info; finalize the parked ones; a finalized fiber is no longer one */
	uint32_t info = guest_alloc(0x170, 8);
	CHECKEQ(call(h_fiber_info, 2, fib_b, info), 0); CHECKEQ(RD32(info), 0x1020); CHECKEQ(RD32(info + 4), 8); CHECKEQ(RD32(info + 8), ctx_b); CHECKEQ(RD32(info + 12), 0x4000);
	CHECKEQ(call(h_fiber_finalize, 1, fib_b), 0); CHECKEQ(call(h_fiber_finalize, 1, fib_b), E_FIBER_INVALID);
	CHECKEQ(call(h_fiber_finalize, 1, fib_a), 0); CHECKEQ(call(h_fiber_finalize, 1, fib_c), 0); CHECKEQ(call(h_fiber_finalize, 1, fib_d), 0);
	CHECKEQ(call(h_fiber_run, 3, fib_a, 0, ret), E_FIBER_INVALID);
	nap(20);
}

/* ================= the odds and ends */
static void test_misc(void) {
	uint32_t name = guest_alloc(16, 4); strcpy((char *)G(name), "lw");
	uint32_t w = guest_alloc(32, 8); memset(G(w), 0, 32);
	uint32_t info = guest_alloc(0x40, 4); WR32(info, 0x40);
	CHECKEQ(call(h_lwmutex_info, 2, w, info), E_UNKNOWN_LW_MUTEX_ID);
	CHECKEQ(call(h_create_lwmutex, 5, w, name, 2, 1, 0), 0);
	CHECKEQ(call(h_lwmutex_info, 2, w, info), 0); CHECKEQ(RD32(info + 0x28), 2); CHECKEQ(RD32(info + 0x2c), w); CHECKEQ(RD32(info + 0x30), 1); CHECKEQ(RD32(info + 0x34), 1); CHECKEQ(RD32(info + 0x38), cur_tid); CHECKEQ(RD32(info + 0x3c), 0);
	WR32(info, 0x3c); CHECKEQ(call(h_lwmutex_info, 2, w, info), E_INVALID_ARGUMENT_SIZE);
	uint32_t n = word(); WR32(n, 99);
	CHECKEQ(call(h_backtrace_self, 4, 0, 0, n, 0), 0); CHECKEQ(RD32(n), 0);
}

/* ================= the scheduler (RECOMP_SCHED=vita), run last and switched on only here: every
 * other test runs with it off, as the host does by default. Threads are real guest threads
 * (CreateThread/StartThread, so they are admitted and wait for a slot); their "guest code" calls
 * sched_boundary() where main.c's dispatch would pass a system-call boundary. Each check that
 * something has NOT happened yet holds because the only slot is held by the checking thread: the
 * other thread cannot run however long the check waits. */
static char slog[64]; static int slog_n;   /* the order things ran in, one letter each */
static pthread_mutex_t slog_m = PTHREAD_MUTEX_INITIALIZER;
static void note(char ch) { pthread_mutex_lock(&slog_m); if (slog_n < 63) { slog[slog_n++] = ch; slog[slog_n] = 0; } pthread_mutex_unlock(&slog_m); }
static int slog_is(const char *s) { pthread_mutex_lock(&slog_m); int r = !strcmp(slog, s); pthread_mutex_unlock(&slog_m); return r; }
static void slog_clear(void) { pthread_mutex_lock(&slog_m); slog_n = 0; slog[0] = 0; pthread_mutex_unlock(&slog_m); }
static void sched_boundary(void) { if (recomp_sched_on) sched_yield_if_preempted(); }
static void sched_for_test(int cores, uint64_t quantum) {   /* (re)configured only while nothing waits for a slot */
	CHECK(!sched_head);
	sched_cores = cores; sched_quantum = quantum;
	if (!recomp_sched_on) { recomp_sched_on = 1; sched_admit(&main_thread); pthread_t th; pthread_create(&th, 0, sched_monitor, 0); pthread_detach(th); }   /* with the timer */
}
static uint32_t sc_sema, sc_sema2, sc_sema3, sc_never;
static uint32_t sc_thread(uint32_t name, int fn, int prio) { return call(h_create_thread, 7, name, 0x1000 + 4 * fn, prio, 0x1000, 0, 0, 0); }
static void sc_start(uint32_t t, uint32_t arg) { uint32_t a = word(); WR32(a, arg); CHECKEQ(call(h_start_thread, 3, t, 4, a), 0); }
static void sc_join(uint32_t t) { CHECKEQ(call(h_wait_thread_end, 3, t, 0, 0), 0); }
/* preemption: H runs, blocks on a semaphore, and resumes only at the next boundary after it is signalled */
static void sc_high(Cpu *c) { (void)c; note('H'); CHECKEQ(call(h_wait_sema, 3, sc_sema, 1, 0), 0); note('h'); }
/* round robin: equal-priority spinners that never block; "inside" proves one runs at a time on one core */
static volatile int rr_stop, rr_inside, rr_overlap, rr_last = -1, rr_switches; static volatile uint64_t rr_spins[3];
static void sc_spinner(Cpu *c) {
	int me = (int)RD32(c->r[1]);
	while (!rr_stop) {
		sched_boundary();
		if (__atomic_add_fetch(&rr_inside, 1, __ATOMIC_SEQ_CST) != 1) rr_overlap++;
		if (rr_last != me) { rr_switches++; rr_last = me; }
		rr_spins[me]++;
		for (volatile int k = 0; k < 2000; k++) ;
		__atomic_sub_fetch(&rr_inside, 1, __ATOMIC_SEQ_CST);
	}
}
/* a blocked thread's slot: L (low) runs while H (high) waits on a semaphore; L's signal hands the core to H at L's next boundary */
static uint32_t sc_h_uid;
static void sc_waiter_high(Cpu *c) { (void)c; note('A'); CHECKEQ(call(h_wait_sema, 3, sc_sema2, 1, 0), 0); note('C'); }
static void sc_runner_low(Cpu *c) {
	(void)c; note('B');
	Thread *h = thread_of(sc_h_uid); CHECK(h && h->wait_type == 3 && h->s_state == S_BLOCKED);   /* H waits on the semaphore, slotless */
	CHECKEQ(call(h_signal_sema, 2, sc_sema2, 1), 0);
	nap(20); CHECK(slog_is("AB"));   /* H is ready, but L holds the one core until a boundary */
	sched_boundary();
	note('D');
}
static void sc_note_arg(Cpu *c) { note((char)RD32(c->r[1])); }
/* every thread blocked: T1 on a semaphore, T2 in a delay (then it signals T1), main in a timed wait */
static void sc_sleeper(Cpu *c) { (void)c; CHECKEQ(call(h_wait_sema, 3, sc_sema3, 1, 0), 0); note('1'); }
static void sc_delayer(Cpu *c) { (void)c; CHECKEQ(call(h_delay, 1, 30000), 0); note('2'); CHECKEQ(call(h_signal_sema, 2, sc_sema3, 1), 0); }
/* two cores: a low-priority spinner passing boundaries is the one a new high-priority thread preempts, not main */
static volatile int lo_stop;
static void sc_low_spinner(Cpu *c) { (void)c; note('L'); while (!lo_stop) { sched_boundary(); for (volatile int k = 0; k < 1000; k++) ; } }
/* the timer: a spinner that never passes a boundary (a spinlock's ldrex loop) */
static volatile int p_stop; static volatile uint64_t p_spins;
static void sc_pure_spinner(Cpu *c) { (void)c; while (!p_stop) p_spins++; sched_boundary(); note('p'); }
static void sched_watchdog(int sig) {   /* a deadlock: what ran, who holds the cores, every thread's state (read without the lock: the process ends anyway) */
	(void)sig;
	fprintf(stderr, "FAIL: the scheduler tests deadlocked (no progress in 30 s); ran \"%s\"\n", slog);
	for (int i = 0; i < sched_cores; i++) fprintf(stderr, "  core %d: %s\n", i, sched_slot[i] ? sched_slot[i]->name : "-");
	fprintf(stderr, "  main: state %d prio %d slot %d\n", main_thread.s_state, main_thread.prio, main_thread.s_slot);
	for (int i = 0; i < nthreads; i++) if (threads[i].s_state) fprintf(stderr, "  %s %#x: state %d prio %d slot %d done %d wait %u\n", threads[i].name, threads[i].uid, threads[i].s_state, threads[i].prio, threads[i].s_slot, threads[i].done, threads[i].wait_type);
	_exit(1);
}
static void test_sched(void) {
	uint32_t name = guest_alloc(16, 4); strcpy((char *)G(name), "sched");
	guest_fns[16] = sc_high; guest_fns[17] = sc_spinner; guest_fns[18] = sc_waiter_high; guest_fns[19] = sc_runner_low;
	guest_fns[20] = sc_note_arg; guest_fns[21] = sc_sleeper; guest_fns[22] = sc_delayer; guest_fns[23] = sc_low_spinner; guest_fns[24] = sc_pure_spinner;
	signal(SIGALRM, sched_watchdog); alarm(30);
	sc_sema = call(h_create_sema, 5, name, 0, 0, 1, 0); sc_sema2 = call(h_create_sema, 5, name, 0, 0, 1, 0);
	sc_sema3 = call(h_create_sema, 5, name, 0, 0, 1, 0); sc_never = call(h_create_sema, 5, name, 0, 0, 1, 0);
	sched_for_test(1, 10000000);   /* a long quantum: neither round robin nor the timer acts while a check naps on the core */
	CHECK(main_thread.s_state == S_RUN);

	/* 1. one core: a higher-priority thread started by main waits for main's next boundary, runs
	 * until it blocks, and after its semaphore is signalled resumes only at main's next boundary */
	slog_clear();
	uint32_t h = sc_thread(name, 16, 100);
	sc_start(h, 0); nap(30); CHECK(slog_is(""));   /* ready, but main holds the core */
	sched_boundary(); CHECK(slog_is("H"));       /* preempted: H ran until it blocked, then the core came back */
	CHECKEQ(call(h_signal_sema, 2, sc_sema, 1), 0); nap(30); CHECK(slog_is("H"));
	sched_boundary(); CHECK(slog_is("Hh"));
	sc_join(h);

	/* 2. one core, three equal-priority spinners that never block (main sleeps meanwhile): each gets
	 * the core in turn every quantum, never two at once */
	rr_stop = 0; rr_overlap = 0; rr_switches = 0; rr_last = -1;
	uint32_t sp[3];
	for (int i = 0; i < 3; i++) { sp[i] = sc_thread(name, 17, 160); sc_start(sp[i], (uint32_t)i); }
	nap(20); CHECKEQ((uint32_t)(rr_spins[0] + rr_spins[1] + rr_spins[2]), 0);   /* main still holds the core */
	sched_quantum = 5000;
	CHECKEQ(call(h_delay, 1, 200000), 0);   /* main's own turn comes back by round robin too */
	rr_stop = 1;
	for (int i = 0; i < 3; i++) sc_join(sp[i]);
	sched_quantum = 10000000;
	for (int i = 0; i < 3; i++) CHECK(rr_spins[i] > 0);
	CHECKEQ((uint32_t)rr_overlap, 0);
	CHECK(rr_switches >= 12);   /* 200 ms in 5 ms quanta: about 40 turns */
	fprintf(stderr, "    round robin: %llu / %llu / %llu spins, %d turns\n", (unsigned long long)rr_spins[0], (unsigned long long)rr_spins[1], (unsigned long long)rr_spins[2], rr_switches);

	/* 2b. RECOMP_SCHED_RR=0, the firmware's run-to-block (no time slice in its thread control
	 * block): three equal-priority spinners below main; while main sleeps the first one keeps the
	 * core for the whole 200 ms though it passes boundaries and its quantum runs out many times;
	 * main (higher) takes the core back at that spinner's next boundary when it wakes */
	sched_rr = 0;
	rr_stop = 0; rr_overlap = 0; rr_switches = 0; rr_last = -1; rr_spins[0] = rr_spins[1] = rr_spins[2] = 0;
	for (int i = 0; i < 3; i++) { sp[i] = sc_thread(name, 17, 170); sc_start(sp[i], (uint32_t)i); }
	sched_quantum = 5000;
	CHECKEQ(call(h_delay, 1, 200000), 0);
	rr_stop = 1;
	int ran = 0; for (int i = 0; i < 3; i++) ran += rr_spins[i] > 0;
	CHECKEQ((uint32_t)ran, 1);              /* one spinner ran; its equals never got the core */
	CHECKEQ((uint32_t)rr_switches, 1);
	for (int i = 0; i < 3; i++) sc_join(sp[i]);
	sched_quantum = 10000000; sched_rr = 1;
	CHECKEQ((uint32_t)rr_overlap, 0);

	/* 3. a blocked thread releases its slot: H (100) blocks on a semaphore, so L (180) runs while
	 * H waits; L's signal makes H ready, and H takes the core at L's next boundary */
	slog_clear();
	uint32_t hh = sc_h_uid = sc_thread(name, 18, 100), ll = sc_thread(name, 19, 180);
	sc_start(hh, 0); sc_start(ll, 0);
	sched_boundary(); CHECK(slog_is("A"));   /* H ran and blocked; main (160) outranks L (180) */
	sc_join(ll);                             /* main blocks: L runs, signals, is preempted by H, finishes */
	CHECK(slog_is("ABCD"));
	sc_join(hh);

	/* 4. a priority change re-orders the wait list and preempts at once: B (180) raised to 150
	 * overtakes A (170) and main (160); main lowering itself to 190 hands the core to A */
	slog_clear();
	uint32_t a = sc_thread(name, 20, 170), b = sc_thread(name, 20, 180);
	sc_start(a, 'a'); sc_start(b, 'b'); nap(20); CHECK(slog_is(""));
	CHECKEQ(call(h_change_priority, 2, b, 150), 0); CHECK(slog_is("b"));
	CHECKEQ(call(h_change_priority, 2, 0, 190), 0); CHECK(slog_is("ba"));
	CHECKEQ(call(h_change_priority, 2, 0, 160), 0);
	sc_join(a); sc_join(b);
	/* lowering a waiting thread below main keeps it waiting past main's boundary */
	slog_clear();
	uint32_t d = sc_thread(name, 20, 100);
	sc_start(d, 'd'); CHECKEQ(call(h_change_priority, 2, d, 170), 0); sched_boundary(); CHECK(slog_is(""));
	sc_join(d); CHECK(slog_is("d"));

	/* 5. no deadlock when every thread blocks: the core is free while all three sleep; whoever wakes
	 * first takes it (T2 from its delay, then T1 from T2's signal, then main from its timeout) */
	slog_clear();
	uint32_t t1 = sc_thread(name, 21, 160), t2 = sc_thread(name, 22, 160);
	sc_start(t1, 0); sc_start(t2, 0);
	uint32_t tmo = word(); WR32(tmo, 100000);
	CHECKEQ(call(h_wait_sema, 3, sc_never, 1, tmo), E_WAIT_TIMEOUT);
	CHECK(slog_is("21"));
	sc_join(t1); sc_join(t2);
	CHECK(sched_slot[0] == &main_thread && !sched_head);

	/* 7. the timer: P spins in pure guest code (no system call, no boundary) on the one core while
	 * main waits for it; after a quantum without a boundary P loses the core to main (its host
	 * thread runs on, evicted), and at its next boundary P waits for a slot like any preempted thread */
	slog_clear(); p_stop = 0; p_spins = 0;
	sched_quantum = 5000;
	uint32_t pt = sc_thread(name, 24, 160);
	sc_start(pt, 0);
	CHECKEQ(call(h_delay, 1, 30000), 0);   /* P takes the core; main gets it back only by the timer */
	{ Thread *p = thread_of(pt); CHECK(p && p->s_state == S_EVICTED); }
	CHECK(p_spins > 0);
	p_stop = 1;
	{ Thread *p = thread_of(pt); for (int k = 0; k < 100 && p->s_state != S_READY; k++) nap(1); CHECK(p->s_state == S_READY); }   /* its boundary: it waits now */
	CHECK(slog_is(""));
	sc_join(pt); CHECK(slog_is("p"));
	sched_quantum = 10000000;

	/* 6. two cores (main at 150 on one, a low-priority spinner at 180 on the other): a new
	 * high-priority thread preempts the lowest-priority holder, not main (the quantum is set long
	 * so that only the priority rule can decide) */
	slog_clear(); lo_stop = 0;
	sched_for_test(2, 10000000);
	CHECKEQ(call(h_change_priority, 2, 0, 150), 0);
	uint32_t lo = sc_thread(name, 23, 180);
	sc_start(lo, 0); while (!slog_is("L")) nap(1);   /* it took the free core at once */
	uint64_t main_since = main_thread.s_since;
	uint32_t hi = sc_thread(name, 20, 100);
	sc_start(hi, 'H');
	for (int k = 0; k < 50 && !slog_is("LH"); k++) { sched_boundary(); nap(1); }
	CHECK(slog_is("LH"));
	CHECK(main_thread.s_since == main_since && main_thread.s_state == S_RUN);   /* main never gave its core up */
	lo_stop = 1; sc_join(lo); sc_join(hi);
	CHECKEQ(call(h_change_priority, 2, 0, 160), 0);

	/* 8. affinity, two cores, main on core 0: X (100) pinned to core 0 can't use the free core 1,
	 * so it preempts main at main's boundary and main moves to core 1 (X then blocks on a
	 * semaphore); Y (180) pinned to core 1 waits behind main there although core 0 is free, until
	 * main pins itself to core 0 (ChangeThreadCpuAffinityMask); X, signalled, preempts main on
	 * core 0 and main, pinned, waits for it although core 1 is free */
	slog_clear();
	CHECK(main_thread.s_slot == 0);
	uint32_t x = sc_thread(name, 16, 100);   /* 'H', blocks on sc_sema, 'h' */
	call(h_change_affinity, 2, x, 0x10000);
	sc_start(x, 0); nap(20); CHECK(slog_is(""));
	sched_boundary(); CHECKEQ((uint32_t)main_thread.s_slot, 1);
	for (int k = 0; k < 100 && !slog_is("H"); k++) nap(1);
	CHECK(slog_is("H"));
	uint32_t y = sc_thread(name, 20, 180);
	call(h_change_affinity, 2, y, 0x20000);
	sc_start(y, 'Y'); nap(20); sched_boundary(); CHECK(slog_is("H"));   /* core 0 is free, but not Y's */
	CHECKEQ(call(h_change_affinity, 2, 0, 0x10000), 0);
	CHECKEQ((uint32_t)main_thread.s_slot, 0);
	sc_join(y); CHECK(slog_is("HY"));
	CHECKEQ(call(h_signal_sema, 2, sc_sema, 1), 0); nap(20); CHECK(slog_is("HY"));
	sched_boundary(); CHECK(slog_is("HYh"));
	sc_join(x);
	CHECKEQ(call(h_change_affinity, 2, 0, 0), 0);

	alarm(0);
	pthread_mutex_lock(&sched_m);
	CHECK(!sched_head);
	recomp_sched_on = 0; main_thread.s_state = S_NONE; memset(sched_slot, 0, sizeof sched_slot);
	pthread_mutex_unlock(&sched_m);
}

int main(void) {
	recomp_mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (recomp_mem == MAP_FAILED) { perror("mmap"); return 2; }
	test_evf();
	test_sema();
	test_mutex();
	test_cond();
	test_sevent();
	test_ult();
	test_threads();
	test_rwlock();
	test_msgpipe();
	test_callbacks();
	test_fibers();
	test_misc();
	int before = checks, failed_before = fails;
	test_sched();
	fprintf(stderr, "scheduler (RECOMP_SCHED=vita): %d checks, %d failed\n", checks - before, fails - failed_before);
	fprintf(stderr, "%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
