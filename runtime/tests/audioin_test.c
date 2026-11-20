/*
 * Unit harness for runtime/audio/audioin.c, the silent microphone: opening a port of each type and the errors for a
 * second one, a bad type or size; an input's silence (exactly its grain's bytes, nothing past them); its pacing, one
 * buffer's time per input on the host clock and handed to the guest-clock wait under the fixed clock; release, and
 * the errors for a port that isn't open, isn't one, or a null buffer. It includes audioin.c so the handlers are
 * reachable and supplies what it takes from the rest of the runtime. Build and run: see README.md in this directory.
 */
#include <stdarg.h>
#include <sys/mman.h>
#include <time.h>
#include "../audio/audioin.c"

/* ---- the runtime audioin.c expects */
uint8_t *recomp_mem;
static int fixed;   /* guest_clock_fixed's answer */
int guest_clock_fixed(void) { return fixed; }
uint64_t plat_now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000; }
int sched_block_begin(void) { return 0; }
void sched_block_end(void) {}
static int waits; static uint64_t wait_len, *wait_slot;   /* audio_wait_guest_at's calls */
void audio_wait_guest_at(uint64_t *vnext, uint32_t i, uint64_t len) { (void)i; waits++; wait_len = len; wait_slot = vnext; }

static int checks, fails;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKEQ(a, b) do { uint32_t _a = (a), _b = (b); checks++; if (_a != _b) { fails++; fprintf(stderr, "FAIL %s:%d: %s = %#x, expected %s = %#x\n", __FILE__, __LINE__, #a, _a, #b, _b); } } while (0)

static uint32_t call(HleFn f, int n, ...) {
	Cpu c = { 0 };
	va_list ap; va_start(ap, n);
	for (int i = 0; i < n && i < 4; i++) c.r[i] = va_arg(ap, uint32_t);
	va_end(ap);
	f(&c);
	return c.r[0];
}
enum { BUF = 0x81000000 };

static void test_ports(void) {
	CHECKEQ(call(h_audio_in_open, 4, 0, 256, 16000, 0), 1);   /* VOICE */
	CHECKEQ(call(h_audio_in_open, 4, 0, 256, 16000, 0), AUDIO_IN_ERROR_PORT_FULL);
	CHECKEQ(call(h_audio_in_open, 4, 2, 256, 48000, 0), 2);   /* RAW */
	CHECKEQ(call(h_audio_in_open, 4, 1, 256, 16000, 0), AUDIO_IN_ERROR_INVALID_PORT_TYPE);
	CHECKEQ(call(h_audio_in_status, 1, 1), 0);   /* not muted */
	CHECKEQ(call(h_audio_in_adopt, 1, 0), 1); CHECKEQ(call(h_audio_in_adopt, 1, 2), 1); CHECKEQ(call(h_audio_in_adopt, 1, 1), 0);
	CHECKEQ(call(h_audio_in_release, 1, 2), 0);
	CHECKEQ(call(h_audio_in_release, 1, 2), AUDIO_IN_ERROR_NOT_OPENED);
	CHECKEQ(call(h_audio_in_release, 1, 3), AUDIO_IN_ERROR_INVALID_PORT);
	CHECKEQ(call(h_audio_in_open, 4, 2, 0, 48000, 0), AUDIO_IN_ERROR_INVALID_SIZE);
	CHECKEQ(call(h_audio_in_input, 2, 2, BUF), AUDIO_IN_ERROR_NOT_OPENED);
	CHECKEQ(call(h_audio_in_input, 2, 3, BUF), AUDIO_IN_ERROR_INVALID_PORT);
	CHECKEQ(call(h_audio_in_input, 2, 1, 0), AUDIO_IN_ERROR_INVALID_POINTER);
}
static void test_silence(void) {   /* port 1 (VOICE, 256 samples) is open */
	memset(G(BUF), 0xab, 1024);
	CHECKEQ(call(h_audio_in_input, 2, 1, BUF), 0);
	int zero = 1; for (int i = 0; i < 512; i++) if (G(BUF)[i]) zero = 0;
	CHECK(zero);
	CHECKEQ(G(BUF)[512], 0xab);   /* nothing past the grain */
}
static void test_pacing(void) {   /* 256 samples at 16 kHz: 16 ms an input */
	const uint64_t t0 = plat_now_us();
	for (int i = 0; i < 10; i++) call(h_audio_in_input, 2, 1, BUF);
	const uint64_t ms = (plat_now_us() - t0) / 1000;
	CHECK(ms >= 140 && ms < 400);   /* ten inputs, the first due at once (a little after the last test's) */
	if (ms < 140 || ms >= 400) fprintf(stderr, "  ten inputs took %llu ms\n", (unsigned long long)ms);
	fixed = 1; waits = 0;
	const uint64_t t1 = plat_now_us();
	CHECKEQ(call(h_audio_in_input, 2, 1, BUF), 0);
	CHECKEQ(waits, 1); CHECKEQ((uint32_t)wait_len, 16000); CHECK(wait_slot == &in_vnext[0]);
	CHECK(plat_now_us() - t1 < 10000);   /* the guest clock's wait, not the host's */
	fixed = 0;
	CHECKEQ(call(h_audio_in_release, 1, 1), 0);
}

int main(void) {
	recomp_mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (recomp_mem == MAP_FAILED) { perror("mmap"); return 2; }
	test_ports(); test_silence(); test_pacing();
	fprintf(stderr, "%d checks, %d failed\n", checks, fails);
	if (!fails) fprintf(stderr, "all passed\n");
	return fails ? 1 : 0;
}
