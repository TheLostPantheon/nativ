/* fiber_stack_test.c -- a context-less fiber's stack must be its own private guest memory.
 *
 * The runtime's old placement for a context-less fiber (addrContext 0) was a 512-byte carve
 * below the caller's sp. Two such fibers' stacks carved a few hundred bytes apart could then be
 * live at once (a fiber is a host thread parked on a handoff when not running): the upper
 * strand's deep call chain grew down into the lower strand's live frames and overwrote its saved
 * registers.
 *
 * Case (the fail-before / pass-after case): strand A runs with a canary in its frames; strand B is
 * resumed from another thread with its stack carved 568 bytes above A's (the failure geometry)
 * and uses 0x400 bytes of stack. While A is alive its frames must survive: B must not touch them.
 * Fail-before: B smashes A's canary. Pass-after: intact.
 *
 * Both strands end by returning from their entry (fiber_main hands control back to the thread),
 * which is the point where their frames die and the stack space may be reused.
 *
 * Includes kernel.c directly like kernel_test.c and supplies its few external symbols.
 * Build and run: see README.md in this directory.
 */
#include <sys/mman.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include "../core/kernel.c"

/* ---- the runtime kernel.c expects (same shim set as monitor_test.c) */
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
void ev_log(const char *kind, const char *fmt, ...) { (void)kind; (void)fmt; }
void ev_dump(const char *why) { (void)why; }
uint64_t guest_time_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
int guest_clock_fixed(void) { return 0; }
void guest_time_slept(uint32_t us) { (void)us; }
void display_vblank_callbacks(void) {}
uint32_t io_guest_fd(int fd) { return (uint32_t)fd + 0x100; }
void plat_random(void *buf, size_t n) { for (size_t i = 0; i < n; i++) ((unsigned char *)buf)[i] = (unsigned char)rand(); }
static GuestFn guest_fns[32];
GuestFn guest_lookup(uint32_t a) { return a >= 0x1000 && a < 0x1000 + 32 * 4 ? guest_fns[(a - 0x1000) / 4] : 0; }

/* ---- check plumbing */
static int checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { failed++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)
#define CHECKEQ(a, b) do { checks++; uint32_t _a = (uint32_t)(a), _b = (uint32_t)(b); if (_a != _b) { failed++; \
	fprintf(stderr, "FAIL %s:%d %s == %s (%#x != %#x)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

/* ---- calling a handler: r0-r3 then stack words, on an explicit guest stack */
static uint32_t call_sp(HleFn f, uint32_t sp, int n, ...) {
	Cpu c = { 0 }; c.r[13] = sp; c.r[14] = 0xfffffffe;
	va_list ap; va_start(ap, n);
	for (int i = 0; i < n; i++) { uint32_t v = va_arg(ap, uint32_t); if (i < 4) c.r[i] = v; else WR32(sp + 4 * (i - 4), v); }
	va_end(ap);
	f(&c);
	return c.r[0];
}

/* ---- the two strands */
#define CANARY 0xC0FFEE42u
static uint32_t fib_a, fib_b, s2_uid;     /* the guest SceFibers and the second thread */
static uint32_t X;                        /* strand A's stack top (its carve) */
static volatile int a_ready, b_wrote, b_done;
static uint32_t a_check_result;           /* what A saw in its canary slot before it ended */

/* strand A: its frames carry a canary; it ends (its frames die) only after B had its chance */
static void fentry_a(Cpu *c) {
	WR32(c->r[13] - 0x58, CANARY);       /* the saved register of A's deeper frame (the measured slot) */
	a_ready = 1;
	usleep(300000);                        /* B is started meanwhile: it must NOT touch A's frames */
	a_check_result = RD32(c->r[13] - 0x58);
}                                          /* returns from the entry: the strand ends */
/* strand B: carved 568 bytes above A's top; a 0x400-byte chain (a deep call chain's worth) */
static void fentry_b(Cpu *c) {
	for (uint32_t p = 0x400; p >= 8; p -= 8) WR32(c->r[13] - p, 0xB0B0B0B0u);
	b_wrote = 1;
}
/* the second thread: runs strand B with its carve 568 bytes above A's */
static void s2_entry(Cpu *c) {
	while (!a_ready) usleep(1000);
	c->r[13] = X + 0x438;                  /* -> B's sp0 = sp - 512 = X + 0x238 (the measured gap) */
	c->r[0] = fib_b; c->r[1] = 0; c->r[2] = 0; h_fiber_run(c);
	CHECKEQ(c->r[0], 0);
	b_done = 1;
	c->r[0] = 0; h_exit_thread(c);
}

int main(void) {
	recomp_mem = mmap(0, (size_t)4 << 30, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (recomp_mem == MAP_FAILED) { perror("mmap"); return 2; }
	uint32_t name = guest_alloc(16, 4); strcpy((char *)G(name), "fib");
	guest_fns[4] = fentry_a; guest_fns[5] = fentry_b; guest_fns[12] = s2_entry;
	fib_a = guest_alloc(128, 8); fib_b = guest_alloc(128, 8);
	uint32_t region = guest_alloc(0x10000, 16);
	X = region + 0x8000;                   /* A's carve top; room below for A's frames and B's pattern */
	uint32_t s2_name = guest_alloc(16, 4);
	strcpy((char *)G(s2_name), "s2");
	CHECKEQ(call_sp(h_fiber_init, X + 512, 7, fib_a, name, 0x1010, 4, 0, 0, 0), 0);   /* context-less A */
	CHECKEQ(call_sp(h_fiber_init, X + 0x438, 7, fib_b, name, 0x1014, 5, 0, 0, 0), 0);  /* context-less B */
	s2_uid = call_sp(h_create_thread, X + 0x800, 7, s2_name, 0x1030, 160, 0x4000, 0, 0, 0);
	CHECK(s2_uid);
	CHECKEQ(call_sp(h_start_thread, X + 0x800, 3, s2_uid, 0, 0), 0);
	/* run A from a caller whose sp makes A's carve land at X */
	CHECKEQ(call_sp(h_fiber_run, X + 512, 3, fib_a, 0, 0), 0);
	for (int i = 0; i < 2000 && !b_done; i++) usleep(1000);
	CHECK(b_done);                          /* B did run (after A ended, in the fixed runtime) */
	CHECK(b_wrote);
	/* A's frames had to survive while A was alive: the canary it read before ending */
	CHECKEQ(a_check_result, CANARY);
	fprintf(stderr, "%d checks, %d failed\n", checks, failed);
	return failed ? 1 : 0;
}
