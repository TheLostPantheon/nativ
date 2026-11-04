/*
 * Unit harness for the kernel timer handlers in runtime/core/kernel.c
 * (sceKernelCreateTimer / StartTimer / StopTimer / GetTimerTime / GetTimerTimeWide),
 * asserted against the semantics measured from the firmware side: the value is in
 * microseconds (the loader deadline polls read back tens of thousands of us), it advances
 * monotonically while the timer runs, a stopped timer freezes, restart resumes, a bad uid
 * fails, and the wide variant returns the 64-bit value split lo/hi (r0/r1).
 *
 * Pre-fix control: without these handlers the timer imports were not dispatched at all and fell to
 * the generic stub whose time source read 0 ("a stub that always read 0: its poll
 * conditions could never fire"). Building with -DTIMER_STUB_CONTROL mirrors exactly that
 * contract behind the same wrapper calls: the suite must then FAIL (the checks can tell the
 * stub from the real clock -- the PASS criterion of the suite).
 *
 * Build and run: see run_timer_tests.sh in this directory (post-fix, stub control, kernel gate).
 */
#include <sys/mman.h>
#include <signal.h>
#include <stdarg.h>
#include "../core/kernel.c"

/* ---- the runtime kernel.c expects (same shim set as kernel_test.c) */
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
uint64_t guest_time_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
int guest_clock_fixed(void) { return 0; }
void guest_time_slept(uint32_t us) { (void)us; }
void display_vblank_callbacks(void) {}
uint32_t io_guest_fd(int fd) { return (uint32_t)fd + 0x100; }
void plat_random(void *buf, size_t n) { for (size_t i = 0; i < n; i++) ((unsigned char *)buf)[i] = (unsigned char)rand(); }
static GuestFn guest_fns[32];
GuestFn guest_lookup(uint32_t a) { return a >= 0x1000 && a < 0x1000 + 32 * 4 ? guest_fns[(a - 0x1000) / 4] : 0; }

static int checks = 0, fails = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKEQ(a, b) do { uint32_t _a = (a), _b = (b); checks++; if (_a != _b) { fails++; fprintf(stderr, "FAIL %s:%d: %s = %#x, expected %s = %#x\n", __FILE__, __LINE__, #a, _a, #b, _b); } } while (0)

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

static void sleep_us(uint32_t us) { struct timespec t = { us / 1000000, (long)(us % 1000000) * 1000 }; nanosleep(&t, 0); }

/* ---- the timer API through one wrapper layer, so the control can swap the contract ---- */
#ifdef TIMER_STUB_CONTROL
/* the pre-fix contract: the generic stub read 0 for every time query */
static uint32_t t_create(const char *name, uint32_t attr, uint32_t opt) { (void)name; (void)attr; (void)opt; return 0x40001000; }
static uint32_t t_start(uint32_t uid) { (void)uid; return 0; }
static uint32_t t_stop(uint32_t uid) { (void)uid; return 0; }
static uint32_t t_get(uint32_t uid, uint32_t ptr) { (void)uid; WR64(ptr, 0); return 0; }
static void t_get_wide(uint32_t uid, uint32_t *lo, uint32_t *hi) { (void)uid; *lo = 0; *hi = 0; }
#else
static uint32_t t_create(const char *name, uint32_t attr, uint32_t opt) { return call(h_create_timer, 3, (uint32_t)(uintptr_t)name, attr, opt); }
static uint32_t t_start(uint32_t uid) { return call(h_start_timer, 1, uid); }
static uint32_t t_stop(uint32_t uid) { return call(h_stop_timer, 1, uid); }
static uint32_t t_get(uint32_t uid, uint32_t ptr) { return call(h_get_timer_time, 2, uid, ptr); }
static void t_get_wide(uint32_t uid, uint32_t *lo, uint32_t *hi) {
	if (!my_sp) my_sp = guest_alloc(0x10000, 16) + 0x10000 - 256;
	Cpu c = { 0 }; c.r[13] = my_sp; c.r[14] = 0xfffffffe; c.r[0] = uid;
	h_get_timer_time_wide(&c);
	*lo = c.r[0]; *hi = c.r[1];
}
#endif

static uint64_t t_now(uint32_t uid) {   /* read the timer as one 64-bit value (the non-wide shape) */
	uint32_t ptr = guest_alloc(8, 8);
	CHECKEQ(t_get(uid, ptr), 0);
	return *(uint64_t *)(recomp_mem + ptr);
}

static void test_timer(void) {
	/* handle lifecycle: a created timer is a usable uid */
	uint32_t uid = t_create("t0", 0, 0);
	CHECK(uid != 0);
	CHECKEQ(t_start(uid), 0);                        /* a fresh start: 0 (Timer::start) */
	CHECKEQ(t_start(uid), 1);                        /* starting a running timer: 1 */

	/* units and advancement while running: a 20 ms sleep is tens of THOUSANDS of us */
	sleep_us(20000);
	uint64_t t1 = t_now(uid);
	CHECK(t1 >= 10000); CHECK(t1 <= 2000000);      /* microseconds, not ms (~20) or ns (~20M) */
	sleep_us(20000);
	uint64_t t2 = t_now(uid);
	CHECK(t2 > t1);                                  /* monotonic while running */

	/* stop freezes the value (and reports 1 when it froze, 0 when already stopped) */
	CHECKEQ(t_stop(uid), 1);
	uint64_t t3 = t_now(uid);
	sleep_us(20000);
	CHECK(t_now(uid) == t3);
	CHECKEQ(t_stop(uid), 0);

	/* restart continues the count from where it stopped (Timer::start: "goes on from where it stopped") */
	CHECKEQ(t_start(uid), 0);
	sleep_us(20000);
	uint64_t t4 = t_now(uid);
	CHECK(t4 > t3);                                  /* resumed, not reset */

	/* the wide variant splits the 64-bit value lo (r0) / hi (r1) */
	uint32_t lo = 0, hi = 0xdead;
	t_get_wide(uid, &lo, &hi);
	CHECK(hi == 0);                                  /* small times: the high word is 0 */
	CHECK((uint64_t)lo <= t4 + 500000 && (uint64_t)lo + 500000 >= t4);

	/* a bad uid: SCE_KERNEL_ERROR_UNKNOWN_TIMER_ID (0x80028241) everywhere, and the Wide
	 * query gives all ones (an error code could be taken for a time) */
	uint32_t ptr = guest_alloc(8, 8);
	CHECKEQ(t_get(0xdeadbeef, ptr), (uint32_t)E_UNKNOWN_TIMER);
	CHECKEQ(t_start(0xdeadbeef), (uint32_t)E_UNKNOWN_TIMER);
	CHECKEQ(t_stop(0xdeadbeef), (uint32_t)E_UNKNOWN_TIMER);
	lo = 0xdead; hi = 0xdead;
	t_get_wide(0xdeadbeef, &lo, &hi);
	CHECKEQ(lo, 0xffffffffu); CHECKEQ(hi, 0xffffffffu);

	/* several timers run independently */
	uint32_t u2 = t_create("t1", 0, 0);
	CHECKEQ(t_start(u2), 0);
	sleep_us(20000);
	CHECK(t_now(uid) > t4);
	CHECK(t_now(u2) > 0);
	CHECKEQ(t_stop(uid), 1); CHECKEQ(t_stop(u2), 1);
}

int main(void) {
	recomp_mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (recomp_mem == MAP_FAILED) { perror("mmap"); return 2; }
	test_timer();
#ifdef TIMER_STUB_CONTROL
	printf("%s: %d checks, %d failed (pre-fix stub control)\n", __FILE__, checks, fails);
#else
	printf("%s: %d checks, %d failed (post-fix)\n", __FILE__, checks, fails);
#endif
	return fails ? 1 : 0;
}
