/* signals_test.c -- PR-SIGNALS: _sceKernelWaitSignal / sceKernelSendSignal (runtime/core/kernel.c,
 * hle_kernel[]). The firmware's per-thread signal pair (SceThreadmgr; Vita3K ThreadState::wait_for_signal /
 * send_signal): a signal sent before the wait ends it at once and is consumed by it, a second signal before
 * the first is taken is E_ALREADY_SENT for the waker. Upstream has neither handler (the imports fell to the
 * generic stub), so the registration checks are the fail-before.
 *
 * The test reaches the pair only through hle_kernel[] (a name lookup), never the file-static handlers, so
 * the same source compiles against an upstream copy of kernel.c: SIGNALS_UNDER_TEST='<path>' selects it.
 * Build and run: see README.md in this directory.
 */
#include <sys/mman.h>
#include <signal.h>
#include <stdarg.h>
#ifdef SIGNALS_UNDER_TEST
#include SIGNALS_UNDER_TEST
#else
#include "../core/kernel.c"
#endif
#ifndef E_ALREADY_SENT   /* the local kernel.c defines it; upstream's copy does not (the fail-before compile) */
#define E_ALREADY_SENT 0x80028121u
#endif

/* ---- the runtime kernel.c expects (the kernel_test.c stubs) ---- */
uint8_t *recomp_mem;
__thread uint32_t recomp_excl_addr, recomp_excl_val;
__thread int recomp_excl_valid;
__thread uint64_t recomp_excl_val64;
__thread uint32_t recomp_switch_seq, recomp_excl_seq;
__thread int recomp_excl_drop_reason;
__thread uint32_t recomp_tpidruro;
char hle_last_format[1024];
static uint32_t heap = 0x81000000;
uint32_t guest_alloc(uint32_t size, uint32_t align) { heap = (heap + align - 1) & ~(align - 1); uint32_t a = heap; heap += size; return a; }
uint32_t tls_block(void) { return guest_alloc(0x100, 64); }
void recomp_runfast(void) {}
void ev_log(const char *kind, const char *fmt, ...) { (void)kind; (void)fmt; }
void ev_dump(const char *why) { (void)why; }
uint64_t guest_time_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
int guest_clock_fixed(void) { return 0; }
void guest_time_slept(uint32_t us) { (void)us; }
void display_vblank_callbacks(void) {}
uint32_t io_guest_fd(int fd) { return (uint32_t)fd + 0x100; }
void plat_random(void *buf, size_t n) { for (size_t i = 0; i < n; i++) ((unsigned char *)buf)[i] = (unsigned char)rand(); }
int waitstats_on(void) { return 0; }
uint64_t waitstats_now(void) { return 0; }
void waitstats_begin(uint64_t t0) { (void)t0; }
void waitstats_wait(uint64_t t0, uint64_t woken_at) { (void)t0; (void)woken_at; }
void waitstats_sleep(uint64_t t0) { (void)t0; }
void waitstats_name(const char *n) { (void)n; }
static GuestFn guest_fns[32];
GuestFn guest_lookup(uint32_t a) { return a >= 0x1000 && a < 0x1000 + 32 * 4 ? guest_fns[(a - 0x1000) / 4] : 0; }
#ifdef SIGNALS_UNDER_TEST   /* the local kernel.c defines the watch feature; upstream's copy (the fail-before compile) has none, and the local rt.h WR32 reads it */
uint32_t recomp_watch_lo, recomp_watch_hi;
void recomp_watch_write(uint32_t a, uint32_t v, int width) { (void)a; (void)v; (void)width; }
#endif

/* ---- calling a handler: r0-r3 then stack words, on a per-thread guest stack ---- */
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

/* ---- helper threads: a guest thread identity of its own, in the kernel's thread table ---- */
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
static pthread_t go(void (*fn)(void *), void *arg) {
	Job *j = malloc(sizeof *j); j->fn = fn; j->arg = arg; j->prio = 160;
	pthread_t t; pthread_create(&t, 0, job_main, j); return t;
}
static void nap(int ms) { usleep(ms * 1000); }

static int fails, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKEQ(a, b) do { uint32_t _a = (a), _b = (b); checks++; if (_a != _b) { fails++; fprintf(stderr, "FAIL %s:%d: %s = %#x, expected %s = %#x\n", __FILE__, __LINE__, #a, _a, #b, _b); } } while (0)

static HleFn find(const char *name) {
	for (const HleEntry *e = hle_kernel; e->name; e++) if (!strcmp(e->name, name)) return e->fn;
	return 0;
}

typedef struct { uint32_t tid, result; volatile int done; } SigWait;
static HleFn g_wait, g_send;
static void sig_waiter(void *p) {   /* case 2: a signal sent before the wait ends it at once */
	SigWait *w = p;
	w->tid = cur_thread->uid;
	call(g_send, 1, w->tid);
	w->result = call(g_wait, 3, 0, 0, 0);
	w->done = 1;
}
static void sig_waiter_plain(void *p) {   /* cases 3 and 5: park first, take later */
	SigWait *w = p;
	w->tid = cur_thread->uid;
	w->result = call(g_wait, 3, 0, 0, 0);
	w->done = 1;
}
static void sig_waiter_twice(void *p) {   /* case 5: the wait consumes; the next needs a fresh send */
	SigWait *w = p;
	w->tid = cur_thread->uid;
	w->result = call(g_wait, 3, 0, 0, 0);
	w->done = 1;
	w->result = call(g_wait, 3, 0, 0, 0);
	w->done = 2;
}
static volatile int park_go;
static void parker(void *p) { (void)p; while (!park_go) nap(10); }   /* a live identity for thread_of(uid) */

int main(void) {
	HleFn wait_sig = find("_sceKernelWaitSignal"), send_sig = find("sceKernelSendSignal");
	CHECK(wait_sig != 0);   /* "_sceKernelWaitSignal" registered in hle_kernel */
	CHECK(send_sig != 0);   /* "sceKernelSendSignal" registered in hle_kernel */
	if (!wait_sig || !send_sig) { printf("%d checks FAILED (imports not registered)\n", fails); return 1; }
	g_wait = wait_sig; g_send = send_sig;

	pthread_t park = go(parker, 0); nap(30);
	uint32_t park_tid = 0;
	for (unsigned i = 0; i < nthreads; i++) if (threads[i].started && !threads[i].done) { park_tid = threads[i].uid; break; }
	CHECK(park_tid != 0);

	/* 1. a signal sent BEFORE the wait ends it at once and is consumed */
	SigWait w1 = { 0, 0, 0 };
	pthread_t t1 = go(sig_waiter, &w1); nap(60);
	CHECK(w1.done == 1); CHECKEQ(w1.result, 0);
	pthread_join(t1, 0);

	/* 2. the wait blocks first, a send wakes it with 0 */
	SigWait w2 = { 0, 0, 0 };
	pthread_t t2 = go(sig_waiter_plain, &w2); nap(60);
	CHECK(!w2.done);
	CHECKEQ(call(send_sig, 1, w2.tid), 0); nap(60);
	CHECK(w2.done == 1); CHECKEQ(w2.result, 0);
	pthread_join(t2, 0);

	/* 3. a second signal before the first is taken: E_ALREADY_SENT for the waker */
	CHECKEQ(call(send_sig, 1, park_tid), 0);
	CHECKEQ(call(send_sig, 1, park_tid), E_ALREADY_SENT);

	/* 4. the wait consumes the pending signal: the first returns, the second blocks until a fresh send */
	SigWait w4 = { 0, 0, 0 };
	pthread_t t4 = go(sig_waiter_twice, &w4); nap(60);
	CHECK(!w4.done);   /* the first wait parks */
	CHECKEQ(call(send_sig, 1, w4.tid), 0); nap(60);
	CHECK(w4.done == 1); CHECKEQ(w4.result, 0);   /* taken -- the second wait is still parked */
	CHECKEQ(call(send_sig, 1, w4.tid), 0); nap(60);
	CHECK(w4.done == 2); CHECKEQ(w4.result, 0);
	pthread_join(t4, 0);

	park_go = 1; pthread_join(park, 0);

	/* 5. the argument errors */
	CHECKEQ(call(send_sig, 1, 0), E_ILLEGAL_THREAD_ID);
	CHECKEQ(call(send_sig, 1, 0x40020fff), E_UNKNOWN_THREAD_ID);

	if (fails) { printf("%d checks FAILED\n", fails); return 1; }
	printf("all passed (%d ok)\n", checks);
	return 0;
}
