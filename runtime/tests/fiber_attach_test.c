/* fiber_attach_test.c -- the internal attach pair libult creates its ULT threads with:
 * _sceFiberAttachContextAndRun / _sceFiberAttachContextAndSwitch (runtime/core/kernel.c, hle_kernel[]).
 * Give a fiber its context stack, then run it from the thread / switch to it from the current fiber
 * (Vita3K SceFiber.cpp is the reference). Upstream has neither handler (the imports fall to the
 * generic stub), so the registration checks are the fail-before.
 *
 * The test reaches the pair only through hle_kernel[] (a name lookup), never the file-static
 * handlers, so the same source compiles against an unpatched copy of kernel.c.
 * Build and run: see README.md in this directory.
 */
#include <sys/mman.h>
#include <signal.h>
#include <stdarg.h>
#ifdef FIBER_UNDER_TEST
#include FIBER_UNDER_TEST
#else
#include "../core/kernel.c"
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

/* ---- check plumbing ---- */
static int checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { failed++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)
#define CHECKEQ(a, b) do { checks++; uint32_t _a = (uint32_t)(a), _b = (uint32_t)(b); if (_a != _b) { failed++; \
	fprintf(stderr, "FAIL %s:%d %s == %s (%#x != %#x)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

static HleFn find(const char *name) {
	for (const HleEntry *e = hle_kernel; e->name; e++) if (!strcmp(e->name, name)) return e->fn;
	return 0;
}

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

/* ---- the pair under test (reached only through the lookup above) ---- */
static HleFn g_run, g_switch;

/* case 1: a fiber with a context runs on its stack and returns to its thread with argOnReturn */
static volatile int ctx_ran; static volatile uint32_t ctx_init, ctx_run_arg, ctx_sp;
static void entry_ctx(Cpu *c) {
	ctx_init = c->r[0]; ctx_run_arg = c->r[1]; ctx_sp = c->r[13]; ctx_ran = 1;
	c->r[0] = 0x2B2B; c->r[1] = 0; h_fiber_return(c);   /* back to the thread, argOnReturn */
}

/* case 2: attach_and_switch from a fiber with a fresh SceFiber creates the worker and switches */
static uint32_t fib_a, fib_b_addr, park_slot;
static uint32_t ctx_a, ctx_b; static uint32_t sz_a = 0x4000, sz_b = 0x4000;
static volatile int a_started, b_ran, a_resumed; static volatile uint32_t a_init, a_sp, b_init, b_sp, a_attach_ret, a_park_val, b_perm_ret;
static void entry_spawn(Cpu *c) {
	if (c->r[1] == 0x100) {                    /* A, first incarnation: switch to a fresh worker */
		a_init = c->r[0]; a_sp = c->r[13]; a_started = 1;
		c->r[0] = fib_b_addr; c->r[1] = ctx_b; c->r[2] = sz_b; c->r[3] = 0x200; WR32(c->r[13], park_slot);
		g_switch(c);
		/* resumed by a later Run: r0 is the switch's return, *argOnRun the resume's argOnRunTo */
		a_attach_ret = c->r[0]; a_park_val = RD32(park_slot); a_resumed = 1;
		c->r[0] = 0x2A; c->r[1] = 0; h_fiber_return(c);
		return;
	}
	if (c->r[1] == 0x200) {                    /* B, the created worker: A's entry shape, cloned */
		b_init = c->r[0]; b_sp = c->r[13]; b_ran = 1;
		b_perm_ret = call(g_run, 5, fib_b_addr, ctx_b, sz_b, 0x3030, park_slot);   /* a fiber may not attach-and-run */
		c->r[0] = 0x2B; c->r[1] = 0; h_fiber_return(c);
		return;
	}
}

static volatile int t_done; static volatile uint32_t t_ret1, t_ret2;
static void switcher(void *p) {   /* the thread A runs under: Run -> (A switches to B) -> B returns here */
	(void)p;
	uint32_t slot1 = guest_alloc(4, 4);
	CHECKEQ(call(h_fiber_run, 3, fib_a, 0x100, slot1), 0);
	t_ret1 = RD32(slot1);
	CHECKEQ(t_ret1, 0x2B);                    /* B's argOnReturn reaches the thread: one-way transfer */
	CHECKEQ(RD32(fib_b_addr), FIBER_TAG);      /* the fresh SceFiber is registered */
	CHECK(RD64(fib_b_addr + 8) != 0);         /* ... with the host object's address */
	uint32_t info = guest_alloc(0x180, 8);
	CHECKEQ(call(h_fiber_info, 2, fib_b_addr, info), 0);
	CHECKEQ(RD32(info), 0x1030);              /* the entry cloned from the spawning fiber */
	CHECKEQ(RD32(info + 4), 0x1111);          /* ... and its argOnInitialize */
	uint32_t slot2 = guest_alloc(4, 4);
	CHECKEQ(call(h_fiber_run, 3, fib_a, 0x300, slot2), 0);
	t_ret2 = RD32(slot2);
	CHECKEQ(t_ret2, 0x2A);                    /* A resumes mid-switch and returns to the thread */
	t_done = 1;
}

int main(void) {
	recomp_mem = mmap(0, (size_t)4 << 30, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (recomp_mem == MAP_FAILED) { perror("mmap"); return 2; }
	g_run = find("_sceFiberAttachContextAndRun");
	g_switch = find("_sceFiberAttachContextAndSwitch");
	CHECK(g_run != 0);        /* "_sceFiberAttachContextAndRun" registered in hle_kernel */
	CHECK(g_switch != 0);     /* "_sceFiberAttachContextAndSwitch" registered in hle_kernel */
	if (!g_run || !g_switch) { printf("%d checks FAILED (imports not registered)\n", failed); return 1; }
	guest_fns[4] = entry_ctx; guest_fns[12] = entry_spawn;

	/* ---- 1. attach_and_run: a context fiber runs on its stack and returns to the thread ---- */
	uint32_t name = guest_alloc(16, 4); strcpy((char *)G(name), "fib");
	uint32_t fib = guest_alloc(128, 8), ctx = guest_alloc(0x4000, 8), slot = guest_alloc(4, 4);
	CHECKEQ(call(g_run, 5, fib, ctx, 0x4000, 0x2020, slot), E_FIBER_INVALID);   /* never initialized */
	CHECKEQ(call(g_run, 5, 0, ctx, 0x4000, 0x2020, slot), E_FIBER_NULL);
	CHECKEQ(call(h_fiber_init, 7, fib, name, 0x1010, 0xAAAA, ctx, 0x4000, 0), 0);
	CHECKEQ(call(g_run, 5, fib, ctx, 0x4000, 0x2020, slot), 0);
	CHECK(ctx_ran);
	CHECKEQ(ctx_init, 0xAAAA);                 /* argOnInitialize at the entry */
	CHECKEQ(ctx_run_arg, 0x2020);              /* argOnRunTo at the entry */
	CHECK(ctx_sp >= ctx + 256 && ctx_sp <= ctx + 0x4000);   /* it ran on the context stack */
	CHECKEQ(RD32(slot), 0x2B2B);               /* argOnReturn reached the thread */

	/* ---- 2. attach_and_switch: a fresh SceFiber becomes the worker, one-way to the thread ---- */
	uint32_t name2 = guest_alloc(16, 4); strcpy((char *)G(name2), "sched");
	fib_a = guest_alloc(128, 8); fib_b_addr = guest_alloc(128, 8);   /* B's SceFiber: blank */
	park_slot = guest_alloc(4, 4);
	ctx_a = guest_alloc(0x4000, 8); ctx_b = guest_alloc(0x4000, 8);
	CHECKEQ(call(g_switch, 5, 0, ctx_b, sz_b, 0x200, park_slot), E_FIBER_NULL);
	CHECKEQ(call(g_switch, 5, fib_b_addr, ctx_b, sz_b, 0x200, park_slot), E_FIBER_PERMISSION);   /* a thread may not switch */
	CHECKEQ(call(h_fiber_init, 7, fib_a, name2, 0x1030, 0x1111, ctx_a, sz_a, 0), 0);
	pthread_t th = go(switcher, 0);
	for (int i = 0; i < 5000 && !t_done; i++) usleep(1000);
	CHECK(t_done);
	CHECK(a_started); CHECK(b_ran);
	CHECKEQ(a_init, 0x1111);                   /* A: argOnInitialize */
	CHECK(b_sp >= ctx_b + 256 && b_sp <= ctx_b + sz_b);   /* B ran on the context given to the attach */
	CHECK(b_init == 0x1111);                   /* the argOnInitialize cloned with the entry */
	CHECK(a_resumed);
	CHECKEQ(a_attach_ret, 0);                  /* the switch call returns SCE_FIBER_OK on resume */
	CHECKEQ(a_park_val, 0x300);                /* *argOnRun = the resume's argOnRunTo */
	CHECKEQ(b_perm_ret, E_FIBER_PERMISSION);   /* checked on B: a fiber may not attach-and-run */
	if (t_done) pthread_join(th, 0);

	if (failed) { printf("%d checks FAILED\n", failed); return 1; }
	printf("all passed (%d ok)\n", checks);
	return 0;
}
