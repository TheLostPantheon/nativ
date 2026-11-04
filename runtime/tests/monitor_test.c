/* monitor_test.c -- the exclusive monitor's semantics across the runtime's own context-switch
 * points and the HLE import boundary.
 *
 * The firmware's exclusive monitor (LDREX/STREX) is cleared whenever the thread is scheduled
 * away or a fiber context switch happens: a STREX after such a switch must FAIL and its loop
 * retry with fresh data. A stale monitor that survives the switch can succeed on a value
 * loaded before it while sibling fields changed underneath -- silently corrupting lock-free
 * bookkeeping far from the write.
 *
 * Case 3 (the fiber switch) is the fail-before / pass-after case: a strex after a fiber
 * context switch must FAIL, but without the park-time clear the per-thread monitor survives
 * fiber_park and the strex succeeds from a stale snapshot. Cases 1-2 pin the surrounding
 * semantics: a plain ldrex/strex pair and a non-switching HLE import pair must keep working
 * (the import boundary with no switch inside does NOT clear the monitor).
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

/* ---- the runtime kernel.c expects (same shim set as kernel_test.c) */
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

static int fails, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKEQ(a, b) do { uint32_t _a = (a), _b = (b); checks++; if (_a != _b) { fails++; fprintf(stderr, "FAIL %s:%d: %s = %#x, expected %s = %#x\n", __FILE__, __LINE__, #a, _a, #b, _b); } } while (0)
static uint32_t word(void) { uint32_t a = guest_alloc(8, 8); WR64(a, 0); return a; }

/* ---- case 1: a plain ldrex/strex lands when nothing intervenes */
static void test_plain(void) {
    uint32_t w = word();
    CHECKEQ(LDREX(w), 0);
    CHECKEQ(STREX(w, 0x11112222u), 0);          /* 0 = the store succeeded */
    CHECKEQ(RD32(w), 0x11112222u);
    CHECKEQ(STREX(w, 0x33334444u), 1);          /* the monitor is one-shot: a second strex fails */
    LDREXD(w); CHECKEQ(STREXD(w, 0x5555666677778888ull), 0);
    CHECKEQ(RD32(w), 0x77778888u);
}

/* ---- case 2: the HLE import boundary with NO context switch inside keeps the monitor (the
 * import pair kernel.c exposes to main.c's dispatch; clearing unconditionally at the dispatch
 * would break a lock-free sequence spanning a non-switching import and is not part of this
 * change) */
static void test_import_pair(void) {
    uint32_t w = word();
    LDREX(w);
    kernel_hle_in("monitor_fixture"); kernel_hle_in(0);
    CHECKEQ(STREX(w, 0xaaaa5555u), 0);          /* nothing switched: the store must land */
    CHECKEQ(RD32(w), 0xaaaa5555u);
}

/* ---- case 3 (fail-before / pass-after): a fiber context switch clears the monitor. X does an
 * LDREX, switches to Y (X parks = a real context switch), comes back and STREXes: the store
 * must FAIL because the firmware's monitor does not survive the switch. */
static uint32_t fx_word, fib_x, fib_y;
static int fx_strex = -1, fx_strexd = -1;
static void fentry_y(Cpu *c) {
    for (;;) { c->r[0] = fib_x; c->r[1] = 0; c->r[2] = 0; h_fiber_switch(c); }   /* always hand control back to X */
}
static void fentry_x(Cpu *c) {
    LDREX(fx_word);
    c->r[0] = fib_y; c->r[1] = 0; c->r[2] = 0; h_fiber_switch(c);   /* X parks; Y runs; Y switches back */
    fx_strex = STREX(fx_word, 0x12345678u);                         /* must FAIL: the switch cleared the monitor */
    LDREXD(fx_word);
    c->r[0] = fib_y; c->r[1] = 0; c->r[2] = 0; h_fiber_switch(c);   /* one more park+resume round for the 64-bit pair */
    fx_strexd = STREXD(fx_word, 0x1122334455667788ull);             /* must FAIL likewise */
    c->r[0] = 1; c->r[1] = 0; h_fiber_return(c);
}
static void test_fiber_switch(void) {
    uint32_t name = guest_alloc(16, 4); strcpy((char *)G(name), "mon");
    guest_fns[7] = fentry_x; guest_fns[8] = fentry_y;
    fib_x = guest_alloc(128, 8); fib_y = guest_alloc(128, 8);
    uint32_t ctx_x = guest_alloc(0x4000, 8), ctx_y = guest_alloc(0x4000, 8);
    CHECKEQ(call(h_fiber_init, 7, fib_x, name, 0x101c, 7, ctx_x, 0x4000, 0), 0);
    CHECKEQ(call(h_fiber_init, 7, fib_y, name, 0x1020, 8, ctx_y, 0x4000, 0), 0);
    fx_word = word();
    uint32_t ret = word();
    CHECKEQ(call(h_fiber_run, 3, fib_x, 0, ret), 0);
    CHECKEQ((uint32_t)fx_strex, 1);    /* the strex failed across the fiber switch */
    CHECKEQ((uint32_t)fx_strexd, 1);
    CHECKEQ(RD32(fx_word), 0);         /* nothing landed from the stale monitors */
    CHECKEQ(call(h_fiber_finalize, 1, fib_x), 0);
    CHECKEQ(call(h_fiber_finalize, 1, fib_y), 0);
}

int main(void) {
    recomp_mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (recomp_mem == MAP_FAILED) { perror("mmap"); return 2; }
    test_plain();
    test_import_pair();
    test_fiber_switch();
    fprintf(stderr, "%d checks, %d failed\n", checks, fails);
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
