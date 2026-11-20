/*
 * Unit harness for runtime/net/np.c's parts every game gets: SceNpCommon's comparisons (sceNpCmpNpId, sceNpCmpOnlineId,
 * sceNpCmpNpIdInOrder, as the firmware's np_common has them) and the offline service state (SIGNED_OUT, and a registered
 * state callback hearing it once at the next sceNpCheckCallback). It includes np.c and supplies what it takes from the
 * rest of the runtime (online play: off; a guest call: recorded). Build and run: see README.md in this directory.
 */
#include <stdarg.h>
#include <sys/mman.h>
#include "../net/np.c"

/* ---- the runtime np.c expects */
uint8_t *recomp_mem;
static uint32_t heap = 0x81000000;
uint32_t guest_alloc(uint32_t size, uint32_t align) { heap = (heap + align - 1) & ~(align - 1); uint32_t a = heap; heap += size; return a; }
int net_online(void) { return 0; }
int ov_on(void) { return 0; }
void m2_online_start(const char *comm) { (void)comm; }
int settings_int(const char *k, int def) { (void)k; return def; }
const char *settings_str(const char *k, const char *def, char *buf, int n) { (void)k; snprintf(buf, (size_t)n, "%s", def ? def : ""); return buf; }
void settings_save(void) {}
void settings_set_str(const char *k, const char *v) { (void)k; (void)v; }
int bmsg_send(const char to[16], const uint8_t from[36], const uint8_t *data, size_t n) { (void)to; (void)from; (void)data; (void)n; return -1; }
int bmsg_poll(uint8_t from[36], uint8_t *data, size_t cap) { (void)from; (void)data; (void)cap; return -1; }
static int calls; static uint32_t call_fn, call_r0, call_r1;   /* guest_call4's last call */
uint32_t guest_call4(Cpu *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) { (void)c; (void)a2; (void)a3; calls++; call_fn = fn; call_r0 = a0; call_r1 = a1; return 0; }

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
/* a SceNpId as the system makes one: the online id, opt (four bytes, then the platform), reserved[0] = 1 */
static uint32_t npid(const char *id, uint32_t opt, const char *platform, int made) {
	uint32_t a = guest_alloc(36, 4); memset(G(a), 0, 36);
	strncpy((char *)G(a), id, 16); WR32(a + 0x14, opt);
	if (platform) memcpy(G(a + 0x18), platform, 4);
	G(a)[0x1c] = (uint8_t)made;
	return a;
}

static void test_cmp(void) {
	const uint32_t me = npid("Sackboy", 0, "psp2", 1), me2 = npid("Sackboy", 0, "psp2", 1), me_bare = npid("Sackboy", 0, 0, 1);
	const uint32_t other = npid("Sackgirl", 0, "psp2", 1), me_ps4 = npid("Sackboy", 0, "ps4\0", 1), me_opt = npid("Sackboy", 7, "psp2", 1);
	const uint32_t zero = npid("", 0, 0, 0);   /* an id nothing filled in (what a failed GetNpId leaves) */
	CHECKEQ(call(h_np_cmp_np_id, 2, me, me2), 0);
	CHECKEQ(call(h_np_cmp_np_id, 2, me, me_bare), 0);   /* no platform on one side: the same player */
	CHECKEQ(call(h_np_cmp_np_id, 2, me, other), NP_UTIL_NOT_MATCH);
	CHECKEQ(call(h_np_cmp_np_id, 2, me, me_ps4), NP_UTIL_NOT_MATCH);
	CHECKEQ(call(h_np_cmp_np_id, 2, me, me_opt), NP_UTIL_NOT_MATCH);
	CHECKEQ(call(h_np_cmp_np_id, 2, me, zero), NP_UTIL_INVALID_NP_ID);
	CHECKEQ(call(h_np_cmp_np_id, 2, 0, me), NP_UTIL_INVALID_ARGUMENT);
	CHECKEQ(call(h_np_cmp_online_id, 2, me, me_ps4), 0);   /* the online ids alone */
	CHECKEQ(call(h_np_cmp_online_id, 2, me, other), NP_UTIL_NOT_MATCH);
	const uint32_t order = guest_alloc(4, 4);
	CHECKEQ(call(h_np_cmp_np_id_in_order, 3, me, other, order), 0); CHECKEQ(RD32(order), (uint32_t)-1);   /* "Sackboy" < "Sackgirl" */
	CHECKEQ(call(h_np_cmp_np_id_in_order, 3, other, me, order), 0); CHECKEQ(RD32(order), 1);
	CHECKEQ(call(h_np_cmp_np_id_in_order, 3, me, me_bare, order), 0); CHECKEQ(RD32(order), 0);   /* no platform = "psp2" */
	CHECKEQ(call(h_np_cmp_np_id_in_order, 3, me_ps4, me, order), 0); CHECKEQ(RD32(order), (uint32_t)-1);   /* "ps4" < "psp2" */
	CHECKEQ(call(h_np_cmp_np_id_in_order, 3, me, me, 0), NP_UTIL_INVALID_ARGUMENT);
}
static void test_offline_state(void) {
	const uint32_t st = guest_alloc(4, 4); WR32(st, 0xdeadbeef);
	CHECKEQ(call(h_np_offline_state, 1, st), 0); CHECKEQ(RD32(st), 1);   /* SIGNED_OUT */
	CHECKEQ(call(h_np_register_state, 2, 0x81234567, 0x55), 0);
	calls = 0;
	CHECKEQ(call(h_np_offline_check_callback, 0), 0);
	CHECKEQ(calls, 1); CHECKEQ(call_fn, 0x81234567); CHECKEQ(call_r0, 1); CHECKEQ(call_r1, 0x55);   /* told SIGNED_OUT once */
	CHECKEQ(call(h_np_offline_check_callback, 0), 0); CHECKEQ(calls, 1);   /* and not again */
	CHECKEQ(call(h_np_unregister_state, 0), 0);
}

int main(void) {
	recomp_mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (recomp_mem == MAP_FAILED) { perror("mmap"); return 2; }
	test_cmp(); test_offline_state();
	fprintf(stderr, "%d checks, %d failed\n", checks, fails);
	if (!fails) fprintf(stderr, "all passed\n");
	return fails ? 1 : 0;
}
