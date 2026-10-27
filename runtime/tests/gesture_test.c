/*
 * runtime/input/gesture.c (SceSystemGesture) against the console: runs the library's own console scenario,
 * gesture_scenario.h -- the one whose trace the firmware module produced on a real Vita (a
 * firmware.trace captured on the console and kept local: no firmware output lives in this repository)
 * -- through the host's handlers, and compares the two traces record by record: every
 * return code, every event buffer byte (taps, holds, drags, a flick, a pinch-and-rotate, the
 * primitive layer's lists), error paths included.
 *
 * First, the scripted tap of a headless run (see scripted()). Then the console trace.
 * The pixel densities barely matter to that scenario (any value from 10 to 40 per mm reproduces it;
 * DENSITY=x sets all four), so it doesn't pin them.
 *
 * The scenario calls the API with host pointers; each wrapper below hands the handler guest
 * addresses (the harness's buffers live in the guest arena; a pointer to the scenario's own stack
 * is copied in and back out).
 *
 * Build and run (from this directory):
 *   cc -O1 -g -Wall -Wno-unused-function -I ../.. -I .. \
 *      gesture_test.c -o /tmp/gesture_test -lm -lpthread && /tmp/gesture_test [firmware.trace]
 * (the trace: argv[1], else GESTURE_TRACE; without one only the scripted taps run)
 */
#include <sys/mman.h>
#include <stdarg.h>
#include "../input/gesture.c"
#include "gesture_scenario.h"

uint8_t *recomp_mem;

/* ---- host pointers to guest addresses */
static uint32_t scratch = 0x10000000, scratch_top;
static struct { void *host; uint32_t guest, n; } outs[8];
static int nouts;
static uint32_t ga(const void *p, uint32_t n) {   /* in the arena: its address; else a copy, written back after the call */
	if (!p) return 0;
	if ((const uint8_t *)p >= recomp_mem && (const uint8_t *)p < recomp_mem + (1ull << 32)) return (uint32_t)((const uint8_t *)p - recomp_mem);
	uint32_t a = scratch_top; scratch_top += (n + 15) & ~15u;
	memcpy(G(a), p, n);
	outs[nouts].host = (void *)p; outs[nouts].guest = a; outs[nouts++].n = n;
	return a;
}
static uint32_t call(HleFn f, int n, ...) {
	uint32_t sp = 0x20000000;
	Cpu c = { 0 }; c.r[13] = sp;
	va_list ap; va_start(ap, n);
	for (int i = 0; i < n; i++) { uint32_t v = va_arg(ap, uint32_t); if (i < 4) c.r[i] = v; else WR32(sp + 4 * (i - 4), v); }
	va_end(ap);
	f(&c);
	for (int i = 0; i < nouts; i++) memcpy(outs[i].host, G(outs[i].guest), outs[i].n);
	nouts = 0; scratch_top = scratch;
	return c.r[0];
}

#define R(p) ga(p, 0x980)
static int Create(void *r, uint32_t type, uint32_t port, int16_t *rect, uint32_t *param) { return call(h_create, 5, R(r), type, port, ga(rect, 8), ga(param, 8)); }
static int Reset(void *r) { return call(h_reset, 1, R(r)); }
static int UpdateRect(void *r, int16_t *rect) { return call(h_update_rect, 2, R(r), ga(rect, 8)); }
static int Update(void *r) { return call(h_update, 1, R(r)); }
static int Info(void *r, void *info) { return call(h_info, 2, R(r), ga(info, 0x18)); }
static int Events(void *r, void *ev, uint32_t n, uint32_t *num) { return call(h_events, 4, R(r), ga(ev, n * 0x90), n, ga(num, 4)); }
static int EventsCount(void *r) { return call(h_events_count, 1, R(r)); }
static int ByIndex(void *r, uint32_t i, void *ev) { return call(h_event_by_index, 3, R(r), i, ga(ev, 0x90)); }
static int ById(void *r, int id, void *ev) { return call(h_event_by_id, 3, R(r), (uint32_t)id, ga(ev, 0x90)); }
static int PrimInit(void) { return call(h_prim_init, 0); }
static int PrimFinalize(void) { return call(h_prim_finalize, 0); }
static int PrimReset(void) { return call(h_prim_reset, 0); }
static int PrimUpdate(const void *f, const void *b) { return call(h_prim_update, 2, ga(f, 0x90), ga(b, 0x90)); }
static int PrimEvents(void *ev, uint32_t n, int *num) { return call(h_prim_events, 3, ga(ev, n * 0x60), n, ga(num, 4)); }
static uint32_t PrimCount(void) { return call(h_prim_count, 0); }
static int PrimByIndex(uint32_t i, void *ev) { return call(h_prim_by_index, 2, i, ga(ev, 0x60)); }
static int PrimById(int16_t id, void *ev) { return call(h_prim_by_id, 2, (uint32_t)(int32_t)id, ga(ev, 0x60)); }

static const char *const NAMES[] = {
	"CREATE", "RESET", "UPDATE_RECT", "UPDATE", "INFO", "EVENTS", "EVENTS_COUNT", "EVENT_BY_INDEX",
	"EVENT_BY_ID", "PRIM_INIT", "PRIM_FINALIZE", "PRIM_RESET", "PRIM_UPDATE", "PRIM_EVENTS",
	"PRIM_EVENTS_COUNT", "PRIM_EVENT_BY_INDEX", "PRIM_EVENT_BY_ID", "NOTE",
};

/* ---- the scripted touch of a headless run (display.c's RECOMP_TOUCH): a front-panel touch held
 * over a frame range, one SceTouchData per frame at 30 fps, or `dup` identical copies per frame as
 * Killzone: Mercenary feeds them (it reads a 64-sample buffer and updates once per sample). Counts
 * the events each recognizer hands out, by state. */
static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static void scripted(int dup, int taps[5], int drag[5], int same_id) {
	uint32_t tap = 0x40000000, drg = 0x40001000, td = 0x40002000, ev = 0x40003000, num = 0x40004000;
	memset(taps, 0, 5 * sizeof *taps); memset(drag, 0, 5 * sizeof *drag);
	call(h_prim_init, 0);
	CHECK(call(h_create, 5, tap, 1, 0, 0, 0) == 0);
	CHECK(call(h_create, 5, drg, 2, 0, 0, 0) == 0);
	uint64_t t = 1000000000;
	for (int frame = 0; frame < 120; frame++) {
		/* two taps at 690,916 (Killzone's NEVER CONNECT button), frames 10-20 and 60-70: each touch a
		 * new contact id as display.c numbers them, or both id 1 */
		int down = (frame >= 10 && frame <= 20) || (frame >= 60 && frame <= 70);
		uint8_t id = same_id ? 1 : frame >= 60 ? 2 : 1;
		for (int k = 0; k < dup; k++) {
			t += k ? 1 : 33333;
			memset(G(td), 0, 0x90); WR64(td, t);
			if (down) { WR32(td + 12, 1); WR8(td + 16, id); WR8(td + 17, 128); WR16(td + 18, 690); WR16(td + 20, 916); }
			CHECK(call(h_prim_update, 2, td, 0) == 0);
			uint32_t recs[2] = {tap, drg};
			for (int r = 0; r < 2; r++) {
				call(h_update, 1, recs[r]);
				CHECK(call(h_events, 4, recs[r], ev, 16, num) == 0);
				for (uint32_t e = 0; e < RD32(num); e++) {
					uint32_t st = RD32(ev + 0x90 * e + 4);
					if (st < 5) (r ? drag : taps)[st]++;
					if (!r) CHECK(rs16(ev + 0x90 * e + 0x1a) == 690 && rs16(ev + 0x90 * e + 0x1c) == 916 && RD8(ev + 0x90 * e + 0x1e) == 1);
				}
			}
		}
	}
	call(h_prim_finalize, 0);
}

int main(int argc, char **argv) {
	recomp_mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (recomp_mem == MAP_FAILED) { perror("mmap"); return 2; }
	scratch_top = scratch;
	/* the console the golden trace was recorded on: its densities and its front panel's active
	 * area (the scenario's rect[0] asks the library to look it up) */
	if (getenv("DENSITY")) { float d = strtof(getenv("DENSITY"), 0); dens_x[0] = dens_y[0] = dens_x[1] = dens_y[1] = d; }
	int taps[5], drag[5];
	scripted(1, taps, drag, 0);   /* one sample per frame: one TAP per tap; a DRAG that begins and ends per touch */
	CHECK(taps[2] == 2 && taps[1] + taps[3] + taps[4] == 0);
	CHECK(drag[1] == 2 && drag[3] == 2 && drag[4] == 0);
	scripted(64, taps, drag, 0);   /* Killzone's 64 copies a frame: still exactly one event per tap */
	CHECK(taps[2] == 2 && drag[1] == 2 && drag[3] == 2);
	scripted(1, taps, drag, 1);   /* both touches id 1: TAP doesn't care; the firmware's DRAG won't restart a finished id */
	CHECK(taps[2] == 2 && drag[1] == 1 && drag[3] == 1);
	printf("scripted taps: %d checks, %d failed\n", checks, fails);

	const char *path = argc > 1 ? argv[1] : getenv("GESTURE_TRACE");
	if (!path) { printf("console trace: none given (argv[1] or GESTURE_TRACE), skipped\n"); return fails != 0; }
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return 2; }
	static uint8_t gold[1 << 22]; size_t glen = fread(gold, 1, sizeof gold, f); fclose(f);

	static uint8_t tbuf[1 << 22];
	Trace t = { tbuf, 0, sizeof tbuf, 0, 0 };
	GestureHarness h = { 0 };
	h.t = &t;
	h.mem = G(0x30000000);
	GestureApi api = { Create, Reset, UpdateRect, Update, Info, Events, EventsCount, ByIndex, ById,
		PrimInit, PrimFinalize, PrimReset, PrimUpdate, PrimEvents, PrimCount, PrimByIndex, PrimById };
	gesture_run(&api, &h);

	/* record by record: u16 step, u16 name, s32 ret, u32 len, len bytes */
	size_t a = 0, b = 0; int recs = 0, bad = 0;
	while (a + 12 <= t.len && b + 12 <= glen) {
		uint16_t name; int32_t r1, r2; uint32_t n1, n2;
		memcpy(&name, tbuf + a + 2, 2); memcpy(&r1, tbuf + a + 4, 4); memcpy(&n1, tbuf + a + 8, 4);
		memcpy(&r2, gold + b + 4, 4); memcpy(&n2, gold + b + 8, 4);
		int same = r1 == r2 && n1 == n2 && !memcmp(tbuf + a + 12, gold + b + 12, n1);
		if (!same && bad++ < 10) {
			fprintf(stderr, "record %d (%s): host ret %#x len %u, console ret %#x len %u\n", recs, name < 18 ? NAMES[name] : "?", r1, n1, r2, n2);
			for (uint32_t k = 0; n1 == n2 && k < n1; k++)
				if (tbuf[a + 12 + k] != gold[b + 12 + k]) fprintf(stderr, "    byte %#x: host %02x console %02x\n", k, tbuf[a + 12 + k], gold[b + 12 + k]);
		}
		a += 12 + n1; b += 12 + n2; recs++;
	}
	if (a != t.len || b != glen) { fprintf(stderr, "trace lengths differ: host %u bytes, console %zu\n", t.len, glen); bad++; }
	printf("console trace: %d records, %d differ\n", recs, bad);
	return bad != 0 || fails != 0;
}
