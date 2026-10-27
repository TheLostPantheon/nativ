/*
 * In-place correctness checker (a VERIFY=1 build): while the game runs natively, sampled calls to
 * recompiled functions are checked against the original ARM code from the game's real state.
 *
 * At a sampled function entry (recomp_enter, from the ENTER hook recomp.py puts in every
 * function) the calling thread forks: the child is a complete copy-on-write snapshot of the
 * game. The child forks again; the grandchild runs the original instructions in Unicorn (the
 * whole guest arena mapped in place) until the function returns or reaches its first import,
 * recording every memory write; the child runs the recompiled function from the same state to
 * the same point and compares stop reason, core and VFP registers, and memory at every address
 * Unicorn wrote. The game continues unaffected; results are logged as they come.
 *
 * A call into a loaded module (Sony's libc and libm, libgxm, libfios2: recompiled like the game) is
 * followed on both sides -- Unicorn runs the export's original instructions, the child its
 * recompiled code -- so only a host-implemented import (the kernel, the display, the GXM rendering
 * boundary) ends a comparison. Until 2026-10-07 every import ended it: no module function and no
 * caller's code after a module call (Unit 13's AI turning its heading through atan2f/asinf) had
 * ever been compared.
 *
 * RECOMP_VERIFY=k by itself only ever checks a function's first k calls: once those have gone by,
 * nothing about that function is checked again, so a translation wrong only for some later input
 * passes forever (Unit 13's NPC 2 passed 16,000 checks because its first calls all came from
 * standing still). Past the first k, every call is still a candidate -- sampled exponentially by
 * default (each call whose count so far is a power of two) or every RECOMP_VERIFY_EVERY-th call, metered by
 * a RECOMP_VERIFY_RATE budget so a session left running costs a bounded amount of host time instead
 * of one proportional to how long it ran. A normal play session becomes a validation run over
 * whatever the player actually does, not just whatever happened first.
 *
 * env: RECOMP_VERIFY=k        check the first k calls of each function (default 1)
 *      RECOMP_VERIFY_EVERY=n  past the first k, check every n-th call instead of the exponential
 *                             default (2^n of each function's calls, n growing)
 *      RECOMP_VERIFY_RATE=n   budget in checks/second of host time for calls past the first k
 *                             (default 200); over budget a sample is skipped, not stalled for
 *      RECOMP_VERIFY_REPORT=f at exit, one machine-readable line per checked function: address,
 *                             calls seen, checks, ok, okf, mismatches, skips, first mismatch
 *      RECOMP_VERIFY_LOG=file mismatch details (default stderr)
 *      RECOMP_VERIFY_FOLLOW=0 stop at every import, as before
 *      RECOMP_VERIFY_CAPTURE=<dir> each of the first RECOMP_VERIFY_CAPTURE_MAX (3) mismatches leaves a
 *                             reproducible case: the checker's untouched snapshot of memory (<dir>/<addr>_<k>.<frame>),
 *                             the call's full register state (.cpu) and the command that replays it (.txt) --
 *                             RECOMP_SNAP with RECOMP_SNAP_CPU, offline, in a PROFILE=1 VERIFY=1 build naming
 *                             the first instruction that differs
 *      RECOMP_VERIFY_LIBM=1   first, every libm export on a spread of inputs (=exit: then stop)
 */
#include <pthread.h>
#include <setjmp.h>
#include <stddef.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <unicorn/unicorn.h>
#include "hle.h"

#define RET_ADDR 0xffff0000u           /* return sentinel page (bkpt) */
#define MAX_WRITES 65536
#define MAX_BYTES (8u << 20)

typedef struct {
	int status;                        /* 0 returned, 1 stopped at an import, 2 error, 3 timeout */
	uint32_t stop_at, r[16], nzcv;
	uint64_t d[32];
	uint32_t nw, nbytes;
	uint32_t w_addr[MAX_WRITES], w_len[MAX_WRITES], w_off[MAX_WRITES];
	uint8_t bytes[MAX_BYTES];
	char err[256];
} Result;

static Result *shared;                 /* MAP_SHARED: the grandchild's result */
static char *report;                   /* MAP_SHARED: the child's verdict line */
static pthread_mutex_t vlock = PTHREAD_MUTEX_INITIALIZER;
#define CODE_SLOTS (0x1000000 / 2)      /* 16 MB of code, one slot per 2-byte address (Uncharted has 9.4) */
static uint32_t *calls;                /* per function: total entries, ever -- the sampling clock past the first k */
static uint32_t *checks;               /* per function: checks actually run (the first per_fn of these are the guarantee) */
static int per_fn = 1, enabled = -1;
static int verify_every;               /* RECOMP_VERIFY_EVERY: 0 = the exponential default, else every n-th call */
static int verify_rate = 200;          /* RECOMP_VERIFY_RATE: budget for calls past the first k, per second of host time */
static int rate_budget; static time_t rate_sec;   /* vlock-protected: refilled lazily, once per second */
/* per-function detail for the exit report and the top-mismatches list: a hash table like watch_hit's
 * WatchFn below, since most functions a long session runs are never checked at all -- only the ones
 * that are need a slot here */
typedef struct { uint32_t addr, checks, ok, okf, bad, skip; char first_mismatch[200]; } FnStat;
#define FNSTAT_HASH 65536
static FnStat *fnstat_tab; static int n_fnstat, fnstat_full;
/* RECOMP_VERIFY_AT_LUA=<line>[:<frames>]: no checks until the mission script calls the Lua function
 * defined at that line (and that many frames more) -- every function the game runs from then on gets
 * its first RECOMP_VERIFY calls checked: the code path of one moment of play (Unit 13's NPC 2,
 * whose walk never reaches his movement input), which a check from the start of the game never
 * reaches with the same values */
static volatile int verify_open = 1; static int verify_lua_line, verify_lua_delay; static uint64_t verify_open_at = ~0ull;
static void verify_lua(int line, uint64_t frame) {
	if (verify_open || line != verify_lua_line || verify_open_at != ~0ull) return;
	verify_open_at = frame + (uint64_t)verify_lua_delay;
	fprintf(stderr, "verify: the script reached line %d at frame %llu; checks from frame %llu\n", line, (unsigned long long)frame, (unsigned long long)verify_open_at);
}
static uint64_t nchecks, nok, nbad, nskip, nflags;   /* nflags: of the ok, those that differed only in FPSCR exception flags */
static int follow = 1;                 /* RECOMP_VERIFY_FOLLOW: calls into module code are compared through */
void verify_summary(void);
/* RECOMP_SNAP_TIME=step_us (snapshot mode): the kernel's clocks answered on both sides from one
 * sequence -- each read returns the call's base time plus step_us more than the last -- so code that
 * meters itself with clock() (Kynapse's path search) runs through instead of stopping at the call */
static int snap_time_on; static uint64_t snap_time_step = 1;
static uint64_t *snap_time_base;       /* MAP_SHARED: the current call's starting time */
static __thread uint64_t snap_time_now; static __thread int snap_time_started;
static int snap_time_import(const char *n) {
	if (!snap_time_on || !n) return 0;
	if (!strcmp(n, "sceKernelLibcClock") || !strcmp(n, "sceKernelGetProcessTimeLow") || !strcmp(n, "sceKernelGetSystemTimeLow")) return 1;
	if (!strcmp(n, "sceKernelGetProcessTimeWide") || !strcmp(n, "sceKernelGetSystemTimeWide")) return 2;
	/* one thread replays a snapshot: every lightweight-mutex operation succeeds at once (result 0) */
	if (!strncmp(n, "sceKernelLockLwMutex", 20) || !strncmp(n, "sceKernelUnlockLwMutex", 22) || !strncmp(n, "sceKernelTryLockLwMutex", 23)) return 3;
	return 0;
}
static uint64_t snap_time_next(void) {
	if (!snap_time_started) { snap_time_started = 1; snap_time_now = *snap_time_base; }
	return snap_time_now += snap_time_step;
}
extern const char *recomp_import_name_any(uint32_t a);
extern uint32_t recomp_module_target(uint32_t stub);
extern int recomp_is_arm_any(uint32_t a);
extern GuestFn guest_lookup(uint32_t a);
static FILE *vlog;
int verify_child;                      /* set in the child: imports stop the native run */
static jmp_buf stop_jmp;
static uint32_t stop_addr;
static Cpu stop_cpu;

static int step_mode;
typedef struct { uint32_t addr, r[14], nzcv; uint64_t d[32]; } Step;
#define MAX_STEPS 60000
typedef struct { uint32_t n; Step s[MAX_STEPS]; } Trace;
static Trace *trace_orig, *trace_native;
static void hook_step(uc_engine *uc, uint64_t addr, uint32_t size, void *u);

/* ---- the native side, in the child: an import call ends the run (main.c calls this first) */
int verify_external(Cpu *c, uint32_t addr) {
	if (!verify_child) return 0;
	int tk = snap_time_import(recomp_import_name_any(addr));
	if (tk == 3) { c->r[0] = 0; return 1; }
	if (tk) { uint64_t t = snap_time_next(); c->r[0] = (uint32_t)t; if (tk == 2) c->r[1] = (uint32_t)(t >> 32); return 1; }
	uint32_t t = follow ? recomp_module_target(addr) : 0;
	if (t) {   /* a module's export: its recompiled code, as Unicorn runs its original (even where the
	            * host would answer instead, memcpy and the like: both sides must run the same code) */
		GuestFn f = guest_lookup(t & ~1u);
		if (f) { f(c); return 1; }
	}
	if (follow && !recomp_import_name_any(addr)) {   /* a call through a pointer into another image's code (a libc
	                                                  * function the game holds a pointer to): Unicorn just runs it,
	                                                  * so this side does too -- stopping here was a false mismatch */
		GuestFn f = guest_lookup(addr & ~1u);
		if (f) { f(c); return 1; }
	}
	stop_addr = addr; stop_cpu = *c;
	longjmp(stop_jmp, 1);
}

/* ---- the original, in the grandchild: Unicorn over the arena */
static void hook_write(uc_engine *uc, uc_mem_type t, uint64_t addr, int size, int64_t value, void *u) {
	(void)uc; (void)t; (void)value; (void)u;
	Result *r = shared;
	if (r->nw && r->w_addr[r->nw - 1] + r->w_len[r->nw - 1] == addr) { r->w_len[r->nw - 1] += size; return; }
	if (r->nw < MAX_WRITES) { r->w_addr[r->nw] = (uint32_t)addr; r->w_len[r->nw] = size; r->nw++; }
}
static void hook_block(uc_engine *uc, uint64_t addr, uint32_t size, void *u) {
	(void)size; (void)u;
	uint32_t a = (uint32_t)addr & ~1u;
	const char *nm = recomp_import_name_any(a);
	if (!nm) return;
	int tk = snap_time_import(nm);
	if (tk) {   /* the same clock sequence as the native side (or a mutex's 0); return to the caller */
		uint64_t t = tk == 3 ? 0 : snap_time_next(); uint32_t lo = (uint32_t)t, hi = (uint32_t)(t >> 32), lr;
		uc_reg_write(uc, UC_ARM_REG_R0, &lo); if (tk == 2) uc_reg_write(uc, UC_ARM_REG_R1, &hi);
		uc_reg_read(uc, UC_ARM_REG_LR, &lr); uc_reg_write(uc, UC_ARM_REG_PC, &lr);
		return;
	}
	uint32_t t = follow ? recomp_module_target(a) : 0;
	if (t) {   /* the stub's module export: continue in its original code (it returns through lr as from the stub) */
		uint32_t pc = recomp_is_arm_any(t & ~1u) ? (t & ~1u) : (t | 1);
		uc_reg_write(uc, UC_ARM_REG_PC, &pc);
		return;
	}
	shared->status = 1; shared->stop_at = a; uc_emu_stop(uc);
}
static void run_original(const Cpu *in, uint32_t entry) {
	Result *r = shared;
	uc_engine *uc;
	if (uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc)) { snprintf(r->err, sizeof r->err, "uc_open"); r->status = 2; return; }
	uc_ctl_set_cpu_model(uc, UC_CPU_ARM_CORTEX_A9);
	/* the arena in place, but the first 64 KiB (never mapped on the Vita) and the top page */
	if (uc_mem_map_ptr(uc, 0x10000, RET_ADDR - 0x10000, UC_PROT_ALL, recomp_mem + 0x10000)) {
		snprintf(r->err, sizeof r->err, "uc_mem_map_ptr"); r->status = 2; return;
	}
	static uint8_t ret_page[4096] __attribute__((aligned(4096)));
	for (int i = 0; i < 8; i++) { ret_page[2 * i] = 0x00; ret_page[2 * i + 1] = 0xbe; }   /* bkpt */
	uc_mem_map_ptr(uc, RET_ADDR, 4096, UC_PROT_ALL, ret_page);
	uint32_t v;
	uc_reg_read(uc, UC_ARM_REG_C1_C0_2, &v); v |= 0xf << 20; uc_reg_write(uc, UC_ARM_REG_C1_C0_2, &v);
	v = 0x40000000; uc_reg_write(uc, UC_ARM_REG_FPEXC, &v);
	v = in->fpscr ? in->fpscr : 0x03000000; uc_reg_write(uc, UC_ARM_REG_FPSCR, &v);
	for (int k = 0; k < 13; k++) uc_reg_write(uc, UC_ARM_REG_R0 + k, &in->r[k]);
	uc_reg_write(uc, UC_ARM_REG_SP, &in->r[13]);
	v = RET_ADDR | 1; uc_reg_write(uc, UC_ARM_REG_LR, &v);
	for (int k = 0; k < 32; k++) uc_reg_write(uc, UC_ARM_REG_D0 + k, &in->vfp.d[k]);
	v = recomp_tpidruro; uc_reg_write(uc, UC_ARM_REG_C13_C0_3, &v);   /* the calling thread's TLS pointer */
	uint32_t cpsr; uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
	cpsr = (cpsr & 0x0fffffff) | in->n << 31 | in->z << 30 | in->c << 29 | in->v << 28;
	uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
	uc_hook h1, h2, h3;
	uc_hook_add(uc, &h1, UC_HOOK_MEM_WRITE, hook_write, 0, 1, 0);
	uc_hook_add(uc, &h2, UC_HOOK_BLOCK, hook_block, 0, 1, 0);
	if (step_mode && trace_orig) uc_hook_add(uc, &h3, UC_HOOK_CODE, hook_step, 0, 1, 0);
	r->status = 0;
	uc_err e = uc_emu_start(uc, recomp_is_arm_any(entry) ? entry : entry | 1, RET_ADDR, 0, 20000000);   /* ARM-mode functions start without the Thumb bit */
	uint32_t pc; uc_reg_read(uc, UC_ARM_REG_PC, &pc);
	if (r->status == 0 && (e || (pc & ~1u) != RET_ADDR)) {
		r->status = 2; snprintf(r->err, sizeof r->err, "original: %s at %#x", e ? uc_strerror(e) : "did not return", pc);
	}
	for (int k = 0; k < 13; k++) uc_reg_read(uc, UC_ARM_REG_R0 + k, &r->r[k]);
	uc_reg_read(uc, UC_ARM_REG_SP, &r->r[13]);
	uc_reg_read(uc, UC_ARM_REG_LR, &r->r[14]);
	for (int k = 0; k < 32; k++) uc_reg_read(uc, UC_ARM_REG_D0 + k, &r->d[k]);
	uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr); r->nzcv = cpsr >> 28;
	/* what it wrote, as it left it */
	for (uint32_t i = 0; i < r->nw; i++) {
		if (r->nbytes + r->w_len[i] > MAX_BYTES) { r->nw = i; break; }
		r->w_off[i] = r->nbytes;
		memcpy(r->bytes + r->nbytes, recomp_mem + r->w_addr[i], r->w_len[i]);
		r->nbytes += r->w_len[i];
	}
}

/* ---- lockstep (a VERIFY=1 PROFILE=1 build): both sides record every instruction's incoming
 * state; the first difference names the instruction before it */
static Trace *tracing;                 /* the native side records here when set (recomp_trace) */
void verify_trace(Cpu *c, uint32_t addr) {
	Trace *t = tracing;
	if (!t || t->n >= MAX_STEPS) return;
	Step *s = &t->s[t->n++];
	s->addr = addr; memcpy(s->r, c->r, sizeof s->r);
	s->nzcv = c->n << 3 | c->z << 2 | c->c << 1 | c->v;
	memcpy(s->d, c->vfp.d, sizeof s->d);
}
static void hook_step(uc_engine *uc, uint64_t addr, uint32_t size, void *u) {
	(void)size; (void)u;
	Trace *t = trace_orig;
	if (t->n >= MAX_STEPS) return;
	Step *s = &t->s[t->n++];
	s->addr = (uint32_t)addr;
	for (int k = 0; k < 13; k++) uc_reg_read(uc, UC_ARM_REG_R0 + k, &s->r[k]);
	uc_reg_read(uc, UC_ARM_REG_SP, &s->r[13]);
	uint32_t cpsr; uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr); s->nzcv = cpsr >> 28;
	for (int k = 0; k < 32; k++) uc_reg_read(uc, UC_ARM_REG_D0 + k, &s->d[k]);
}
static void lockstep(char *o, size_t n) {
	/* compare in order over the addresses both visited (an IT instruction or a stop may differ) */
	uint32_t i = 0, j = 0, prev = 0;
	while (i < trace_orig->n && j < trace_native->n) {
		Step *a = &trace_orig->s[i], *b = &trace_native->s[j];
		if (a->addr != b->addr) {   /* resync: skip the side whose address the other never had */
			if (a->addr < b->addr) i++; else j++;
			continue;
		}
		const char *what = 0; char buf[96];
		for (int k = 0; k < 14 && !what; k++) if (a->r[k] != b->r[k]) { snprintf(buf, sizeof buf, "r%d %#x vs %#x", k, b->r[k], a->r[k]); what = buf; }
		if (!what && a->nzcv != b->nzcv) { snprintf(buf, sizeof buf, "nzcv %x vs %x", b->nzcv, a->nzcv); what = buf; }
		for (int k = 0; k < 32 && !what; k++) if (a->d[k] != b->d[k]) { snprintf(buf, sizeof buf, "d%d %#llx vs %#llx", k, (unsigned long long)b->d[k], (unsigned long long)a->d[k]); what = buf; }
		if (what) {
			size_t l = strlen(o);
			snprintf(o + l, n - l, "\n    first difference before %#x (after executing %#x): recompiled %s original", a->addr, prev, what);
			return;
		}
		prev = a->addr; i++; j++;
	}
	size_t l = strlen(o);
	snprintf(o + l, n - l, "\n    traces agree over %u/%u steps", trace_orig->n, trace_native->n);
}

/* ---- the child (a pristine snapshot): the original and the recompiled run each in their own
 * fork of it, so both start from exactly the same state, then compare */
typedef struct { int status; uint32_t stop_at; Cpu c; uint32_t nbad; uint32_t bad_at; uint8_t mine, theirs; uint32_t nflagw; } Native;   /* nflagw: words differing only in FPSCR exception flags */
static Native *native;
static void run_native(const Cpu *in, uint32_t entry) {
	Cpu c = *in;
	c.r[14] = RET_ADDR | 1;
	verify_child = 1;
	if (!setjmp(stop_jmp)) { alarm(10); guest_lookup(entry)(&c); native->status = 0; native->c = c; }
	else { native->status = 1; native->stop_at = stop_addr; native->c = stop_cpu; }
	tracing = 0;
	/* memory where the original wrote; a whole aligned word that differs only in the FPSCR's
	 * cumulative exception flags (a spilled copy of it, mode bits as the call began) is set aside */
	Result *r = shared;
	uint32_t mode = (in->fpscr ? in->fpscr : 0x03000000u) & 0x07c00000u;
	for (uint32_t i = 0; i < r->nw && !native->nbad; i++)
		for (uint32_t j = 0; j < r->w_len[i] && !native->nbad; j++) {
			uint32_t a = r->w_addr[i] + j;
			if (recomp_mem[a] == r->bytes[r->w_off[i] + j]) continue;
			uint32_t w = a & ~3u, k0 = w - r->w_addr[i];
			if (w >= r->w_addr[i] && k0 + 4 <= r->w_len[i]) {   /* the original wrote that whole word */
				uint32_t mine, theirs; memcpy(&mine, recomp_mem + w, 4); memcpy(&theirs, r->bytes + r->w_off[i] + k0, 4);
				if (!((mine ^ theirs) & ~0x9fu) && (theirs & 0x07c00000u) == mode) { native->nflagw++; j = k0 + 3; continue; }
			}
			native->nbad = 1; native->bad_at = a; native->mine = recomp_mem[a]; native->theirs = r->bytes[r->w_off[i] + j];
		}
}
/* RECOMP_VERIFY_CAPTURE: a mismatch found in play becomes a case to replay offline. The check child
 * runs this after check(): its memory is still the moment the call began (both sides ran in forks
 * of it), so its memory blocks, the call's registers and the thread's TLS pointer are the whole
 * input. A rare mismatch (Unit 13's 0x81012d3e: twice in a long session) otherwise leaves only its
 * verdict. */
static int *capture_n;   /* MAP_SHARED: captures written so far, counted by the children that write them */
static void capture_mismatch(const Cpu *in, uint32_t entry) {
	const char *dir = getenv("RECOMP_VERIFY_CAPTURE");
	if (!dir || !capture_n || strncmp(report, "MISMATCH", 8)) return;
	int max = getenv("RECOMP_VERIFY_CAPTURE_MAX") ? atoi(getenv("RECOMP_VERIFY_CAPTURE_MAX")) : 3;
	int k = __atomic_fetch_add(capture_n, 1, __ATOMIC_RELAXED);
	if (k >= max) return;
	char base[512]; snprintf(base, sizeof base, "%s/%08x_%d", dir, entry, k);
	extern uint64_t display_frames(void);
	uint64_t frame = display_frames();
	/* all guest memory in use -- the images' segments and the whole guest heap (memory blocks,
	 * thread stacks and TLS blocks, host-made objects) -- in RECOMP_DUMP_MEM's {address, size}
	 * record format, which RECOMP_SNAP loads. (sysmem_dump_now writes the memory blocks only: a
	 * replay from that missed the thread's TLS block and took another path.) */
	char path[560]; snprintf(path, sizeof path, "%s.%llu", base, (unsigned long long)frame);
	FILE *f = fopen(path, "wb");
	uint64_t total = 0;
	if (f) {
		uint32_t lo[80], hi[80]; int n = guest_mem_ranges(lo, hi, 80);
		for (int i = 0; i < n; i++) if (hi[i] > lo[i]) {
			uint32_t hdr[2] = { lo[i], hi[i] - lo[i] };
			fwrite(hdr, 4, 2, f); fwrite(G(lo[i]), 1, hdr[1], f); total += hdr[1];
		}
		fclose(f);
	}
	snprintf(path, sizeof path, "%s.cpu", base);
	f = fopen(path, "wb");
	if (f) { uint32_t tls = recomp_tpidruro; fwrite(in, sizeof *in, 1, f); fwrite(&tls, 4, 1, f); fwrite(&entry, 4, 1, f); fclose(f); }
	snprintf(path, sizeof path, "%s.txt", base);
	if ((f = fopen(path, "w"))) {
		fprintf(f, "%s\n\nreplay (a VERIFY build; PROFILE=1 VERIFY=1 names the first differing instruction):\n"
		           "RECOMP_VERIFY=1 RECOMP_SNAP=%s.%llu RECOMP_SNAP_CPU=%s.cpu RECOMP_SNAP_CALLS=%x\n", report, base, (unsigned long long)frame, base, entry);
		fclose(f);
	}
	fprintf(stderr, "verify: mismatch at %#x captured to %s.* (%llu bytes of guest memory)\n", entry, base, (unsigned long long)total);
}
/* a checker's forked helper dies on SIGINT/SIGTERM instead of running main.c's handler: a group-wide
 * signal (timeout(1), Ctrl-C) reaches every helper too, and each would print the summary and rewrite
 * RECOMP_VERIFY_REPORT from its own stale copy of the counters */
static void helper_signals(void) { signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL); }
static int fork_run(void (*fn)(const Cpu *, uint32_t), const Cpu *in, uint32_t entry) {
	pid_t g = fork();
	if (g == 0) { helper_signals(); fn(in, entry); _exit(0); }
	int st; waitpid(g, &st, 0);
	return WIFEXITED(st);
}
static void check(const Cpu *in, uint32_t entry) {
	memset(shared, 0, offsetof(Result, w_addr));
	memset(native, 0, sizeof *native);
	char *o = report;
	if (!fork_run(run_original, in, entry)) { snprintf(o, 512, "skip %#x: original crashed or timed out", entry); return; }
	if (shared->status == 2) { snprintf(o, 512, "skip %#x: %s", entry, shared->err); return; }
	if (!fork_run(run_native, in, entry)) { snprintf(o, 512, "MISMATCH %#x: recompiled code crashed or hung", entry); return; }
	Result *r = shared;
	Cpu *c = &native->c;
	int status = native->status;
	const char *where = status ? "at import" : "at return";
	if (status != r->status || (status == 1 && native->stop_at != r->stop_at))
		snprintf(o, 512, "MISMATCH %#x: recompiled %s %#x, original %s %#x", entry, status ? "stopped at import" : "returned",
		         status ? native->stop_at : 0, r->status ? "stopped at import" : "returned", r->status ? r->stop_at : 0);
	else {
		/* every difference is counted; one that is only the FPSCR's cumulative exception flags (IOC DZC
		 * OFC UFC IXC IDC, bits 0-4 and 7) in a copy of the FPSCR (vmrs: its mode bits as the call
		 * began) is the one known divergence -- the recompiled code doesn't keep those sticky flags,
		 * and nothing the games compute reads them (Sony's libm sets them, its results match) */
		const char *names[] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp"};
		char first[200] = ""; int nd = 0, flags_only = 1;
		uint32_t mode = (in->fpscr ? in->fpscr : 0x03000000u) & 0x07c00000u;
		for (int k = 0; k < 14; k++) if (c->r[k] != r->r[k]) {
			nd++;
			if (((c->r[k] ^ r->r[k]) & ~0x9fu) || (r->r[k] & 0x07c00000u) != mode) flags_only = 0;
			if (!first[0]) snprintf(first, sizeof first, "%s %#x vs original %#x", names[k], c->r[k], r->r[k]);
		}
		for (int k = 0; k < 32; k++) if (c->vfp.d[k] != r->d[k]) {
			nd++; flags_only = 0;
			if (!first[0]) snprintf(first, sizeof first, "d%d %#llx vs original %#llx", k, (unsigned long long)c->vfp.d[k], (unsigned long long)r->d[k]);
		}
		if (native->nbad) {
			nd++; flags_only = 0;
			if (!first[0]) snprintf(first, sizeof first, "memory at %#x: %#04x vs original %#04x", native->bad_at, native->mine, native->theirs);
		}
		if (!nd && native->nflagw) { snprintf(o, 512, "okf %#x (%s, %u writes; FPSCR exception flags only, in %u stored words)", entry, status ? "to an import" : "returned", r->nw, native->nflagw); return; }
		if (!nd) { snprintf(o, 512, "ok %#x (%s, %u writes)", entry, status ? "to an import" : "returned", r->nw); return; }
		if (flags_only) { snprintf(o, 512, "okf %#x (%s, %u writes; FPSCR exception flags only: %s)", entry, status ? "to an import" : "returned", r->nw, first); return; }
		/* the rest of what differs, briefly (a result register vs a scratch one tells a real error from noise) */
		char more[300] = ""; size_t ml = 0;
		for (int k = 0; k < 14 && ml < 260; k++) if (c->r[k] != r->r[k]) ml += snprintf(more + ml, sizeof more - ml, " %s", names[k]);
		for (int k = 0; k < 32 && ml < 260; k++) if (c->vfp.d[k] != r->d[k]) ml += snprintf(more + ml, sizeof more - ml, " d%d", k);
		if (native->nbad && ml < 260) ml += snprintf(more + ml, sizeof more - ml, " mem@%#x", native->bad_at);
		snprintf(o, 512, "MISMATCH %#x (%s): %s%s%s", entry, where, first, nd > 1 ? "; differs:" : "", nd > 1 ? more : "");
	}
	/* a mismatch: with tracing compiled in, run both again recording every step */
	if (step_mode && trace_orig) {
		trace_orig->n = 0; trace_native->n = 0;
		memset(shared, 0, offsetof(Result, w_addr));
		fork_run(run_original, in, entry);
		tracing = trace_native;
		fork_run(run_native, in, entry);
		tracing = 0;
		lockstep(o, 1024);
	}
}

/* RECOMP_COVERAGE=<file>: how often each guest function was entered, on every thread; written at
 * the end of the run as "address count" lines (which code a game actually runs) */
static uint32_t *cov;
static int cov_on = -1;
static void coverage(uint32_t addr) {
	if (cov_on < 0) {
		pthread_mutex_lock(&vlock);
		if (cov_on < 0) { cov = getenv("RECOMP_COVERAGE") ? calloc(0x1000000 / 2, 4) : 0; cov_on = cov != 0; }
		pthread_mutex_unlock(&vlock);
	}
	uint32_t i = (addr - 0x81000000u) >> 1;
	if (cov_on && i < 0x1000000 / 2) __atomic_fetch_add(&cov[i], 1, __ATOMIC_RELAXED);
}
void coverage_write(void) {
	if (cov_on != 1) return;
	FILE *f = fopen(getenv("RECOMP_COVERAGE"), "w");
	if (!f) return;
	for (uint32_t i = 0; i < 0x1000000 / 2; i++) if (cov[i]) fprintf(f, "%08x %u\n", 0x81000000u + 2 * i, cov[i]);
	fclose(f);
}

/* Function-entry watch (a VERIFY build: recomp_enter's ENTER hook runs on every guest function
 * entry; independent of RECOMP_VERIFY's checks):
 *   RECOMP_WATCH_FROM_FRAME=n RECOMP_WATCH_TO_FRAME=n  a window of presented frames (display_frames())
 *   RECOMP_WATCH_THREAD=name  only that guest thread (exact name, e.g. KynapseThread)
 *   RECOMP_WATCH_COUNT=1      count entries per function inside the window; the table (by thread
 *                             name) goes to stderr at the window's end or at exit
 *   RECOMP_WATCH_LOG=addr,... log every entry to those functions (hex): frame, thread, r0-r3, lr
 * What a subsystem actually runs per frame -- Unit 13's Kynapse thread, an AI agent's update --
 * without reading its code first. */
extern uint64_t display_frames(void);
extern const char *recomp_cur_tname(void);
#define WATCH_HASH 65536   /* distinct (function, thread) pairs; a full table drops new ones, and says so */
#define MAX_WATCH_LOG 64
typedef struct { uint32_t addr; const char *thread; uint64_t count; uint32_t from; } WatchFn;   /* from: the call's return address (watch_edges), else 0 */
static int watch_on = -1, watch_count_on, watch_printed, watch_full, watch_edges;
static uint64_t watch_from, watch_to = ~0ull;
static const char *watch_thread;
static uint32_t watch_log[MAX_WATCH_LOG]; static int n_watch_log;
static WatchFn *watch_tab; static int n_watch;
static pthread_mutex_t wlock = PTHREAD_MUTEX_INITIALIZER;
static int watch_cmp(const void *a, const void *b) {
	const WatchFn *x = a, *y = b;
	int t = strcmp(x->thread ? x->thread : "", y->thread ? y->thread : "");
	return t ? t : (x->count < y->count) - (x->count > y->count);
}
static void watch_print(void) {
	if (watch_printed || !watch_count_on) return;
	watch_printed = 1;
	pthread_mutex_lock(&wlock);
	WatchFn *v = malloc(sizeof(WatchFn) * (n_watch ? n_watch : 1)); int n = 0;
	for (int i = 0; i < WATCH_HASH; i++) if (watch_tab[i].addr) v[n++] = watch_tab[i];
	qsort(v, n, sizeof *v, watch_cmp);
	fprintf(stderr, "== watch: entries per function, frames [%llu,%llu)%s%s%s\n", (unsigned long long)watch_from,
	        (unsigned long long)watch_to, watch_thread ? ", thread " : "", watch_thread ? watch_thread : "",
	        watch_full ? " (TABLE FULL: some functions not counted)" : "");
	for (int i = 0; i < n; i++)
		if (v[i].from) fprintf(stderr, "  %-20s %#010x %llu from %#010x\n", v[i].thread ? v[i].thread : "?", v[i].addr, (unsigned long long)v[i].count, v[i].from);
		else fprintf(stderr, "  %-20s %#010x %llu\n", v[i].thread ? v[i].thread : "?", v[i].addr, (unsigned long long)v[i].count);
	free(v);
	pthread_mutex_unlock(&wlock);
}
void watch_summary(void) { watch_print(); }
static void dwn_lua(int line, uint64_t frame);   /* below, with the data watch */
static void verify_lua(int line, uint64_t frame);   /* below, with the checker */
static void watch_init_locked(void);
/* F9 in the window: a count window by hand -- F9, play the moment, F9: entries per function over it
 * go to stderr, headed with its frames and the frame rate. Uncharted's companion stuck on logs at 60
 * fps (2026-10-11): a window stuck at 60, another after switching to 30 in the menu, then compared
 * per frame. RECOMP_WATCH_COUNT isn't needed; RECOMP_WATCH_THREAD still narrows it. */
static int watch_win_open, watch_win_n;
static void watch_open(int edges) {   /* a hand-held window from this frame on (an empty table) */
	extern int display_frame_rate_60(void);
	pthread_mutex_lock(&wlock);
	if (watch_on < 0) watch_init_locked();
	uint64_t frame = display_frames();
	if (!watch_tab) watch_tab = calloc(WATCH_HASH, sizeof(WatchFn));
	memset(watch_tab, 0, WATCH_HASH * sizeof(WatchFn)); n_watch = 0; watch_full = 0; watch_edges = edges;
	watch_from = frame; watch_to = ~0ull; watch_count_on = 1; watch_printed = 0; watch_on = 1;
	watch_win_open = 1; watch_win_n++;
	fprintf(stderr, "== watch window %d: from frame %llu at %d fps\n", watch_win_n, (unsigned long long)frame, display_frame_rate_60() ? 60 : 30);
	pthread_mutex_unlock(&wlock);
}
static void watch_close(void) {   /* ... to this frame: its table to stderr */
	extern int display_frame_rate_60(void);
	pthread_mutex_lock(&wlock);
	uint64_t frame = display_frames();
	watch_to = frame; watch_win_open = 0; watch_printed = 0;
	fprintf(stderr, "== watch window %d: to frame %llu (%llu frames) at %d fps\n", watch_win_n, (unsigned long long)frame,
	        (unsigned long long)(frame - watch_from), display_frame_rate_60() ? 60 : 30);
	pthread_mutex_unlock(&wlock);
	watch_print();
}
void watch_window_toggle(void) { if (!watch_win_open) watch_open(1); else watch_close(); }   /* (with callers, as F6's: the path to what one window ran and the other didn't) */
/* F6: a frame-rate A/B capture of one moment -- 60 frames counted at 60 fps, then the game switched to
 * 30 at once (no menu, same spot, same camera) and 90 more counted; both tables list each function's
 * callers (the call's return address), and the frame rate goes back to the setting after. Uncharted's
 * companion runs in place on logs at 60 and hops off at 30 (2026-10-11): A is the running in place,
 * B the hop-off, and what runs only in B is the hop-off's code. */
static void dw_f6_arm(void);   /* below, with the data watch */
static int ab_phase; static uint64_t ab_next;
static void ab_frame(uint64_t frame) {
	extern void display_force_frame_rate(int); extern void display_set_frame_hook(void (*)(uint64_t));
	if (frame < ab_next) return;
	if (ab_phase == 1) {
		watch_close(); fprintf(stderr, "== F6 capture: A (60 fps) done; switching to 30\n");
		display_force_frame_rate(30); watch_open(1); ab_phase = 2; ab_next = frame + 90;
	} else if (ab_phase == 2) {
		watch_close(); fprintf(stderr, "== F6 capture: B (30 fps) done; back to the frame-rate setting\n");
		display_force_frame_rate(-1); display_set_frame_hook(0); ab_phase = 0;
	}
}
void watch_ab_capture(void) {
	extern void display_force_frame_rate(int); extern void display_set_frame_hook(void (*)(uint64_t));
	if (ab_phase || watch_win_open) { fprintf(stderr, "F6: a capture or F9 window is already running\n"); return; }
	display_force_frame_rate(60); watch_open(1); dw_f6_arm();
	fprintf(stderr, "== F6 capture: A (60 fps) from frame %llu\n", (unsigned long long)display_frames());
	ab_phase = 1; ab_next = display_frames() + 60;
	display_set_frame_hook(ab_frame);
}
static uint64_t ab_at;   /* RECOMP_AB_AT=frame: the F6 capture started there (scripted and headless runs) */
static void ab_at_frame(uint64_t f) { if (f >= ab_at) watch_ab_capture(); }
/* RECOMP_WATCH_WINDOWS=a-b,c-d,...: F9 windows over those frames (scripted and headless runs), e.g. around
 * RECOMP_SETTINGS_SWITCH frames: what one setting runs and another doesn't, at the same moment */
#define MAX_WATCH_WINDOWS 16
static uint64_t ww_from[MAX_WATCH_WINDOWS], ww_to[MAX_WATCH_WINDOWS]; static int n_ww, ww_i;
static void ww_frame(uint64_t f) {
	if (ww_i >= n_ww) return;
	if (!watch_win_open && f >= ww_from[ww_i]) watch_open(1);
	else if (watch_win_open && f >= ww_to[ww_i]) { watch_close(); ww_i++; }
}
__attribute__((constructor)) static void watch_window_key(void) {
	extern void (*window_f9)(void), (*window_f6)(void); extern void display_set_frame_hook(void (*)(uint64_t));
	window_f9 = watch_window_toggle; window_f6 = watch_ab_capture;
	const char *e = getenv("RECOMP_AB_AT"); if (e) { ab_at = strtoull(e, 0, 0); display_set_frame_hook(ab_at_frame); }
	const char *w = getenv("RECOMP_WATCH_WINDOWS");
	if (w && !e) {
		for (const char *p = w; *p && n_ww < MAX_WATCH_WINDOWS; ) {
			char *q; ww_from[n_ww] = strtoull(p, &q, 0); if (*q != '-') break;
			ww_to[n_ww] = strtoull(q + 1, &q, 0); n_ww++; p = *q == ',' ? q + 1 : q;
		}
		if (n_ww) display_set_frame_hook(ww_frame);
	}
}
static void watch_init_locked(void) {   /* (wlock held) the environment's watch settings, once */
	const char *f = getenv("RECOMP_WATCH_FROM_FRAME"); if (f) watch_from = strtoull(f, 0, 0);
	const char *t = getenv("RECOMP_WATCH_TO_FRAME"); if (t) watch_to = strtoull(t, 0, 0);
	watch_thread = getenv("RECOMP_WATCH_THREAD");
	watch_count_on = getenv("RECOMP_WATCH_COUNT") != 0;
	if (watch_count_on) watch_tab = calloc(WATCH_HASH, sizeof(WatchFn));
	const char *l = getenv("RECOMP_WATCH_LOG");
	if (l) {
		char buf[2048]; snprintf(buf, sizeof buf, "%s", l);
		for (char *tok = strtok(buf, " ,"); tok && n_watch_log < MAX_WATCH_LOG; tok = strtok(0, " ,"))
			watch_log[n_watch_log++] = (uint32_t)strtoul(tok, 0, 16) & ~1u;
	}
	watch_on = (watch_count_on || n_watch_log || getenv("RECOMP_WATCH_LUA")) ? 1 : 0;
}
static void watch_hit(Cpu *c, uint32_t addr) {
	if (watch_on < 0) {
		pthread_mutex_lock(&wlock);
		if (watch_on < 0) watch_init_locked();
		pthread_mutex_unlock(&wlock);
	}
	if (!watch_on) return;
	uint64_t frame = display_frames();
	if (frame < watch_from) return;
	if (frame >= watch_to) { if (!watch_win_n) watch_print(); return; }   /* (an F9 window prints when it closes: a thread that read the last window's end as the next opened printed the new, empty one, and the real one was then skipped) */
	const char *tn = recomp_cur_tname();
	if (watch_thread && (!tn || strcmp(tn, watch_thread))) return;
	if (watch_count_on) {
		pthread_mutex_lock(&wlock);
		uint32_t from = watch_edges ? c->r[14] & ~1u : 0;
		uint32_t h = ((addr >> 1) * 2654435761u ^ (uint32_t)(uintptr_t)tn ^ from * 40503u) & (WATCH_HASH - 1);
		for (int k = 0; k < WATCH_HASH; k++, h = (h + 1) & (WATCH_HASH - 1)) {
			WatchFn *w = &watch_tab[h];
			if (w->addr == addr && w->thread == tn && w->from == from) { w->count++; break; }
			if (!w->addr) {
				if (n_watch >= WATCH_HASH * 3 / 4) { watch_full = 1; break; }
				w->addr = addr; w->thread = tn; w->count = 1; w->from = from; n_watch++; break;
			}
		}
		pthread_mutex_unlock(&wlock);
	}
	/* RECOMP_WATCH_LUA=<luaD_precall address>: name every Lua function called, as "luacall frame
	 * thread chunk:line" (Lua 5.1 layouts with 8-byte TValues: func {gc, tt=6}; LClosure isC +6,
	 * p +16; Proto source +32, linedefined +60; TString chars +16) -- which script functions a game
	 * actually runs (Unit 13's mission scripts) */
	static uint32_t lua_precall; static int lua_read;
	if (!lua_read) { const char *e = getenv("RECOMP_WATCH_LUA"); if (e) lua_precall = (uint32_t)strtoul(e, 0, 16) & ~1u; lua_read = 1; }
	if (lua_precall && (addr & ~1u) == lua_precall && RD32(c->r[1] + 4) == 6) {
		uint32_t cl = RD32(c->r[1]);
		if (!RD8(cl + 6)) {
			uint32_t p = RD32(cl + 16), src = RD32(p + 32);
			char name[64]; int n = 0;
			if (src) for (; n < 63 && RD8(src + 16 + n); n++) name[n] = (char)RD8(src + 16 + n);
			name[n] = 0;
			int line = (int)RD32(p + 60);
			fprintf(stderr, "luacall frame %llu %s %s:%d\n", (unsigned long long)frame, tn ? tn : "?", name, line);
			{ extern void sysmem_dump_at_lua(int, uint64_t); sysmem_dump_at_lua(line, frame); }
			dwn_lua(line, frame);
			verify_lua(line, frame);
			/* RECOMP_INPUT_ANCHORS=<the recorded session's stderr>: its main-thread "luacall" lines are
			 * anchors; reaching the next one re-aligns RECOMP_INPUT_PLAY to the frame it had then */
			static struct { uint64_t frame; int line; } *anc; static int n_anc = -1, k_anc;
			if (n_anc < 0) {
				n_anc = 0;
				const char *af = getenv("RECOMP_INPUT_ANCHORS"); FILE *f = af ? fopen(af, "r") : 0;
				if (f) {
					char buf[512]; int cap = 0;
					while (fgets(buf, sizeof buf, f)) {
						unsigned long long fr; char th[64], loc[256];
						if (sscanf(buf, "luacall frame %llu %63s %255s", &fr, th, loc) != 3 || strcmp(th, "main")) continue;
						char *colon = strrchr(loc, ':'); int ln = colon ? atoi(colon + 1) : 0;
						if (!ln) continue;
						if (n_anc == cap) { cap = cap ? cap * 2 : 1024; anc = realloc(anc, cap * sizeof *anc); }
						anc[n_anc].frame = fr; anc[n_anc].line = ln; n_anc++;
					}
					fclose(f);
					fprintf(stderr, "input anchors: %d script calls from %s\n", n_anc, af);
				}
			}
			if (line && k_anc < n_anc && tn && !strcmp(tn, "main")) {
				for (int j = k_anc; j < n_anc && j < k_anc + 8; j++) if (anc[j].line == line) {
					extern void input_play_set_offset(int64_t);
					int64_t off = (int64_t)anc[j].frame - (int64_t)frame;
					static int64_t last = 0x7fffffff;
					if (off != last) { fprintf(stderr, "input anchor %d: frame %llu = recorded %llu (offset %lld)\n", j, (unsigned long long)frame, (unsigned long long)anc[j].frame, (long long)off); last = off; }
					input_play_set_offset(off); k_anc = j + 1; break;
				}
			}
		}
	}
	/* RECOMP_WATCH_PEEK=addr,...: guest words (hex addresses: globals, a known object's fields) shown
	 * on every logged entry -- the state a function decides on, next to its arguments */
	static uint32_t peek[16]; static int npeek = -1;
	if (npeek < 0) {
		int n = 0; const char *e = getenv("RECOMP_WATCH_PEEK");
		if (e) { char buf[256]; snprintf(buf, sizeof buf, "%s", e); for (char *t = strtok(buf, " ,"); t && n < 16; t = strtok(0, " ,")) peek[n++] = (uint32_t)strtoul(t, 0, 16); }
		npeek = n;
	}
	for (int i = 0; i < n_watch_log; i++)
		if (watch_log[i] == (addr & ~1u)) {
			char pk[512] = ""; size_t pl = 0;
			for (int k = 0; k < npeek && pl < 220; k++) pl += snprintf(pk + pl, sizeof pk - pl, " [%x]=%#x", peek[k], RD32(peek[k]));
			/* RECOMP_WATCH_DEREF=r0,r2,...: the four floats each of those registers points at (a vector argument:
			 * Uncharted's swept collision probe, its endpoints passed by pointer) */
			static int deref[4], nderef = -1;
			if (nderef < 0) {
				nderef = 0; const char *e = getenv("RECOMP_WATCH_DEREF");
				for (const char *q = e; q && *q && nderef < 4; q++) if (*q == 'r' && q[1] >= '0' && q[1] <= '3') deref[nderef++] = q[1] - '0';
			}
			for (int k = 0; k < nderef && pl < 440; k++) {
				uint32_t a = c->r[deref[k]];
				if (a < 0x10000 || a > 0xfffffff0u) continue;   /* a register holding a number, not a pointer: the arena is 4 GB, mapped throughout -- only a read past its end faults (0xfffffffc + 12 crashed Uncharted's first capture) */
				float f[4]; for (int j = 0; j < 4; j++) { uint32_t w = RD32(a + 4 * j); memcpy(&f[j], &w, 4); }
				pl += snprintf(pk + pl, sizeof pk - pl, " *r%d=(%.4f,%.4f,%.4f,%.4f)", deref[k], f[0], f[1], f[2], f[3]);
			}
			fprintf(stderr, "watch %#010x: frame %llu %s r0=%#x r1=%#x r2=%#x r3=%#x lr=%#x s0=%g *r1=%#x,%#x%s\n",
			        addr, (unsigned long long)frame, tn ? tn : "?", c->r[0], c->r[1], c->r[2], c->r[3], c->r[14], c->vfp.f[0],
			        c->r[1] >= 0x10000 ? RD32(c->r[1]) : 0, c->r[1] >= 0x10000 ? RD32(c->r[1] + 4) : 0, pk);
		}
}

/* RECOMP_DATAWATCH=addr,addr,... (hex guest addresses, 4 bytes each; RECOMP_DATAWATCH_FROM_FRAME=n):
 * which guest function writes there. The host pages holding them go read-only; a write faults, the
 * handler notes the host pc (inside the recompiled fn_XXXXXXXX that stores) and the thread, opens the
 * page and lets the store run; the next guest function entry (any thread) prints the hit with the
 * value now there and closes the page again. Writes in that short open window are missed, so a
 * field written once can slip by -- a field written every frame won't. Symbolise the pcs with
 * `nm -n <binary>`, sliding by the printed runtime address of recomp_enter. Needs
 * RECOMP_GPU_NO_TEX_WATCH=1 (the texture tracker owns the same signals and pages). */
#include <sys/ucontext.h>
#define MAX_DW 16
static uint32_t dw_addr[MAX_DW]; static int n_dw, dw_state = -1, dw_armed;
static uint64_t dw_from;
/* RECOMP_DATAWATCH_NEW=<vtable>:<off>[+<off>...]:<lua line>:<frames> -- watch an object that doesn't
 * exist yet: when the mission script calls the Lua function defined at that line, note every object
 * with that vtable; that many frames later, the ones that have appeared since get their fields
 * watched (Unit 13: the character NPC 2's spawn creates, CharacterS4 +0xb0/+0xb8, the velocity the
 * AI's walk never reaches) */
static uint32_t dwn_vt, dwn_off[4]; static int dwn_noff, dwn_line, dwn_delay, dwn_state;   /* state: 0 off, 1 waiting for the call, 2 waiting to arm, 3 armed */
static uint64_t dwn_at; static uint32_t *dwn_before; static int dwn_nbefore, dwn_rescans;
extern int sysmem_scan_u32(uint32_t value, uint32_t *out, int max);
static void dwn_init(void) {
	const char *e = getenv("RECOMP_DATAWATCH_NEW");
	if (!e) return;
	char buf[256]; snprintf(buf, sizeof buf, "%s", e);
	char *vt = strtok(buf, ":"), *offs = strtok(0, ":"), *line = strtok(0, ":"), *delay = strtok(0, ":");
	if (!vt || !offs || !line || !delay) { fprintf(stderr, "RECOMP_DATAWATCH_NEW: want vtable:off[+off]:line:frames\n"); return; }
	dwn_vt = (uint32_t)strtoul(vt, 0, 16); dwn_line = atoi(line); dwn_delay = atoi(delay);
	for (char *o = strtok(offs, "+"); o && dwn_noff < 4; o = strtok(0, "+")) dwn_off[dwn_noff++] = (uint32_t)strtoul(o, 0, 16);
	dwn_state = 1;
}
/* the Lua tracer's hook: the call that creates the object */
static void dwn_lua(int line, uint64_t frame) {
	if (dwn_state != 1 || line != dwn_line) return;
	dwn_before = malloc(4096 * sizeof(uint32_t)); dwn_nbefore = sysmem_scan_u32(dwn_vt, dwn_before, 4096);
	dwn_at = frame + (uint64_t)dwn_delay; dwn_state = 2;
	fprintf(stderr, "datawatch: %d objects with vtable %#x at frame %llu; arming on the new ones at frame %llu\n", dwn_nbefore, dwn_vt, (unsigned long long)frame, (unsigned long long)dwn_at);
}
static void dwn_arm(uint64_t frame) {   /* wlock held */
	uint32_t now[4096]; int n = sysmem_scan_u32(dwn_vt, now, 4096);
	for (int i = 0; i < n; i++) {
		int old = 0; for (int k = 0; k < dwn_nbefore && !old; k++) old = dwn_before[k] == now[i];
		if (old) continue;
		fprintf(stderr, "datawatch: new object %#x (vtable %#x) at frame %llu\n", now[i], dwn_vt, (unsigned long long)frame);
		for (int k = 0; k < dwn_noff && n_dw < MAX_DW; k++) dw_addr[n_dw++] = now[i] + dwn_off[k];
	}
	if (!n_dw && ++dwn_rescans < 100) { dwn_at = frame + 10; return; }   /* not created yet (its files still loading): look again in 10 frames */
	dwn_state = 3; dw_from = 0;
}
static long dw_pagesz;
static struct sigaction dw_old_segv, dw_old_bus;
typedef struct { uint64_t pc; uint32_t addr; const char *thread; uint64_t frame; uint32_t before; } DwHit;
static DwHit dw_pending[64]; static volatile int n_dw_pending, dw_open;
static uint8_t *dw_page(uint32_t a) { return (uint8_t *)((uintptr_t)(recomp_mem + a) & ~(uintptr_t)(dw_pagesz - 1)); }
static void dw_protect(int ro) { for (int i = 0; i < n_dw; i++) mprotect(dw_page(dw_addr[i]), dw_pagesz, ro ? PROT_READ : PROT_READ | PROT_WRITE); }
static void dw_fault(int sig, siginfo_t *si, void *ucv) {
	uint8_t *a = si->si_addr;
	for (int i = 0; i < n_dw; i++) if (a >= dw_page(dw_addr[i]) && a < dw_page(dw_addr[i]) + dw_pagesz) {
		uint32_t ga = (uint32_t)(a - recomp_mem);
		for (int k = 0; k < n_dw; k++) if (ga >= dw_addr[k] && ga < dw_addr[k] + 4 && n_dw_pending < 64) {
			ucontext_t *uc = ucv; uint64_t pc;
#if defined(__APPLE__) && defined(__aarch64__)
			pc = uc->uc_mcontext->__ss.__pc;
#elif defined(__APPLE__)
			pc = uc->uc_mcontext->__ss.__rip;
#elif defined(__aarch64__)
			pc = uc->uc_mcontext.pc;
#else
			pc = uc->uc_mcontext.gregs[16];   /* (REG_RIP, which needs _GNU_SOURCE) */
#endif
			DwHit *h = &dw_pending[n_dw_pending++];
			h->pc = pc; h->addr = dw_addr[k]; h->thread = recomp_cur_tname(); h->frame = display_frames(); memcpy(&h->before, recomp_mem + dw_addr[k], 4);
		}
		dw_protect(0); dw_open = 1;
		return;
	}
	struct sigaction *o = sig == SIGBUS ? &dw_old_bus : &dw_old_segv;
	if (o->sa_handler == SIG_DFL || o->sa_handler == SIG_IGN) { sigaction(sig, o, 0); return; }
	if (o->sa_flags & SA_SIGINFO) o->sa_sigaction(sig, si, ucv); else o->sa_handler(sig);
}
/* RECOMP_DATAWATCH_F6=<vtable>:<ptr off>:<off>[,<off>...]: when an F6 capture starts, every object with
 * that vtable -- the pointer at <ptr off> in it -- and the words at <off>... from there are data-watched
 * (with RECOMP_GPU_NO_TEX_WATCH=1). Objects that exist only at run time, found the moment it matters:
 * Uncharted's log-end movement state 0x818c89c0, its character at +8, whose next-state field (+0x8c)
 * and current state (+0xdc) someone writes at 30 fps and nobody at 60 (2026-10-11). */
static void dw_f6_arm(void) {
	const char *e = getenv("RECOMP_DATAWATCH_F6");
	if (!e) return;
	char buf[256]; snprintf(buf, sizeof buf, "%s", e);
	char *vt = strtok(buf, ":"), *po = strtok(0, ":"), *offs = strtok(0, ":");
	/* <vtable>:ref: every word that points at such an object is watched instead -- a character's slot holding
	 * its current state object (Uncharted's +0xdc): the write that moves it on names the transition */
	if (vt && po && !strcmp(po, "ref")) {
		uint32_t v = (uint32_t)strtoul(vt, 0, 16), objs[64]; int n = sysmem_scan_u32(v, objs, 64);
		pthread_mutex_lock(&wlock);
		if (dw_armed) { dw_protect(0); dw_armed = 0; }
		n_dw = 0;
		for (int i = 0; i < n && n_dw < MAX_DW; i++) {
			uint32_t refs[16]; int nr = sysmem_scan_u32(objs[i], refs, 16);
			fprintf(stderr, "datawatch: F6 object %#x (vtable %#x): %d references\n", objs[i], v, nr);
			for (int k = 0; k < nr && n_dw < MAX_DW; k++) { fprintf(stderr, "datawatch:   ref at %#x\n", refs[k]); dw_addr[n_dw++] = refs[k]; }
		}
		if (!dw_pagesz) dw_pagesz = sysconf(_SC_PAGESIZE);
		dw_from = 0; dw_state = n_dw ? 1 : dw_state;
		pthread_mutex_unlock(&wlock);
		return;
	}
	if (!vt || !po || !offs) { fprintf(stderr, "RECOMP_DATAWATCH_F6: want vtable:ptroff:off[,off...] or vtable:ref\n"); return; }
	uint32_t v = (uint32_t)strtoul(vt, 0, 16), p = (uint32_t)strtoul(po, 0, 16), off[8]; int no = 0;
	for (char *o = strtok(offs, ","); o && no < 8; o = strtok(0, ",")) off[no++] = (uint32_t)strtoul(o, 0, 16);
	uint32_t objs[64]; int n = sysmem_scan_u32(v, objs, 64);
	pthread_mutex_lock(&wlock);
	if (dw_armed) { dw_protect(0); dw_armed = 0; }   /* a second capture: the objects of now */
	n_dw = 0;
	for (int i = 0; i < n && n_dw < MAX_DW; i++) {
		uint32_t base = RD32(objs[i] + p);
		fprintf(stderr, "datawatch: F6 object %#x (vtable %#x): [+%#x] = %#x\n", objs[i], v, p, base);
		if (base < 0x10000 || base > 0xfffff000u) continue;
		for (int k = 0; k < no && n_dw < MAX_DW; k++) dw_addr[n_dw++] = base + off[k];
	}
	if (!dw_pagesz) dw_pagesz = sysconf(_SC_PAGESIZE);
	dw_from = 0; dw_state = n_dw ? 1 : dw_state;
	pthread_mutex_unlock(&wlock);
}
static void datawatch_hit(void) {
	if (dw_state < 0) {
		pthread_mutex_lock(&wlock);
		if (dw_state < 0) {
			const char *e = getenv("RECOMP_DATAWATCH");
			if (e) {
				char buf[512]; snprintf(buf, sizeof buf, "%s", e);
				for (char *t = strtok(buf, " ,"); t && n_dw < MAX_DW; t = strtok(0, " ,")) dw_addr[n_dw++] = (uint32_t)strtoul(t, 0, 16);
				const char *f = getenv("RECOMP_DATAWATCH_FROM_FRAME"); if (f) dw_from = strtoull(f, 0, 0);
				dw_pagesz = sysconf(_SC_PAGESIZE);
				fprintf(stderr, "datawatch: %d addresses from frame %llu; recomp_enter at %p\n", n_dw, (unsigned long long)dw_from, (void *)recomp_enter);
			}
			dwn_init();
			if (dwn_state) dw_pagesz = sysconf(_SC_PAGESIZE);
			dw_state = (n_dw || dwn_state) ? 1 : 0;
		}
		pthread_mutex_unlock(&wlock);
	}
	if (!dw_state) return;
	if (!dw_armed) {
		if (dwn_state == 1) return;
		if (dwn_state == 2) {
			if (display_frames() < dwn_at) return;
			pthread_mutex_lock(&wlock);
			if (dwn_state == 2) dwn_arm(display_frames());
			pthread_mutex_unlock(&wlock);
			if (!n_dw) return;
		}
		if (display_frames() < dw_from) return;
		pthread_mutex_lock(&wlock);
		if (!dw_armed) {
			fprintf(stderr, "datawatch: recomp_enter at %p\n", (void *)recomp_enter);
			struct sigaction sa; memset(&sa, 0, sizeof sa);
			sa.sa_sigaction = dw_fault; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
			sigaction(SIGSEGV, &sa, &dw_old_segv); sigaction(SIGBUS, &sa, &dw_old_bus);
			for (int i = 0; i < n_dw; i++) { uint32_t v; memcpy(&v, recomp_mem + dw_addr[i], 4); fprintf(stderr, "datawatch: %#x = %#x at frame %llu\n", dw_addr[i], v, (unsigned long long)display_frames()); }
			dw_protect(1); dw_armed = 1;
		}
		pthread_mutex_unlock(&wlock);
		return;
	}
	if (!dw_open) return;
	pthread_mutex_lock(&wlock);
	if (dw_open) {
		for (int i = 0; i < n_dw_pending; i++) {
			DwHit *h = &dw_pending[i]; uint32_t v; memcpy(&v, recomp_mem + h->addr, 4);
			fprintf(stderr, "datawatch: %#x %#x -> %#x frame %llu %s host pc %#llx\n", h->addr, h->before, v, (unsigned long long)h->frame, h->thread ? h->thread : "?", (unsigned long long)h->pc);
		}
		n_dw_pending = 0; dw_open = 0;
		dw_protect(1);
	}
	pthread_mutex_unlock(&wlock);
}

/* RECOMP_VERIFY_LIBM: Sony's math library (the libc module's libm exports, recompiled) on a spread of
 * inputs -- specials, denormals, the ranges games feed it, pseudo-random values -- each call
 * recompiled against original exactly as a game function is checked */
static void libm_sweep(const Cpu *base) {
	static const struct { const char *n; int k; } fns[] = {   /* k: 1 f(x), 2 f(x, y) in floats; 3, 4 the same in doubles */
		{"sinf", 1}, {"cosf", 1}, {"tanf", 1}, {"asinf", 1}, {"acosf", 1}, {"atanf", 1}, {"sinhf", 1}, {"coshf", 1}, {"tanhf", 1},
		{"expf", 1}, {"exp2f", 1}, {"logf", 1}, {"log10f", 1}, {"log2f", 1}, {"sqrtf", 1}, {"cbrtf", 1}, {"floorf", 1},
		{"ceilf", 1}, {"roundf", 1}, {"truncf", 1}, {"fabsf", 1},
		{"atan2f", 2}, {"powf", 2}, {"fmodf", 2}, {"hypotf", 2}, {"fminf", 2}, {"fmaxf", 2}, {"copysignf", 2},
		{"sin", 3}, {"cos", 3}, {"tan", 3}, {"asin", 3}, {"acos", 3}, {"atan", 3}, {"exp", 3}, {"log", 3}, {"log10", 3},
		{"sqrt", 3}, {"floor", 3}, {"ceil", 3}, {"atan2", 4}, {"pow", 4}, {"fmod", 4}};
	float v[160]; int nv = 0;
	static const float special[] = {0.f, -0.f, 1.f, -1.f, 0.5f, -0.5f, 2.f, -2.f, 3.14159265f, -3.14159265f, 1.57079633f, -1.57079633f,
		6.28318531f, 0.70710678f, -0.70710678f, 0.99999994f, -0.99999994f, 1.0000001f, 1e-3f, -1e-3f, 1e-7f, 1e-20f, 1.17549435e-38f,
		1e-40f, -1e-40f, 10.f, 100.f, 1e4f, 1e10f, -1e10f, 3.4e38f, 88.f, 89.f, -104.f, (float)INFINITY, -(float)INFINITY, (float)NAN};
	for (size_t i = 0; i < sizeof special / sizeof *special; i++) v[nv++] = special[i];
	uint32_t seed = 12345;
	while (nv < 160) { seed = seed * 1664525u + 1013904223u; float u = (float)(seed >> 8) / 16777216.f; v[nv] = nv < 120 ? u * 16.f - 8.f : u * 2.f - 1.f; nv++; }
	uint64_t ok = 0, bad = 0, skip = 0, nres = 0, nfl = 0;
	Cpu c0 = *base; c0.r[13] = (base->r[13] - 0x4000) & ~7u;   /* clear of the caller's frame */
	for (size_t f = 0; f < sizeof fns / sizeof *fns; f++) {
		uint32_t t = 0; extern uint32_t recomp_export_addr(const char *n); t = recomp_export_addr(fns[f].n);
		if (!t || !guest_lookup(t & ~1u)) { fprintf(vlog, "libm: %s not exported by a loaded module\n", fns[f].n); continue; }
		int k = fns[f].k, n = (k & 1) ? nv : 40 * 40, fbad = 0, fres = 0;
		for (int i = 0; i < n; i++) {
			Cpu c = c0;   /* pairs: a 40 x 40 grid over every fourth input (specials and random values both) */
			float x = (k & 1) ? v[i] : v[(i / 40) * 4], y = (k & 1) ? 0.f : v[(i % 40) * 4];
			if (k <= 2) { memcpy(&c.vfp.f[0], &x, 4); memcpy(&c.vfp.f[1], &y, 4); }
			else { double dx = x, dy = y; memcpy(&c.vfp.d[0], &dx, 8); memcpy(&c.vfp.d[1], &dy, 8); }
			report[0] = 0;
			pid_t p = fork();
			if (p == 0) { check(&c, t & ~1u); _exit(0); }
			int st; waitpid(p, &st, 0);
			/* the result itself (s0 / d0), apart from the first differing register the report names */
			int rdiff = !strncmp(report, "MISMATCH", 8) && shared->status == 0 && native->status == 0 &&
			            (k <= 2 ? (uint32_t)shared->d[0] != (uint32_t)native->c.vfp.d[0] : shared->d[0] != native->c.vfp.d[0]);
			if (rdiff) {
				nres++;
				if (fres++ < 8) {
					if (k <= 2) { float a, b; uint32_t ua = (uint32_t)native->c.vfp.d[0], ub = (uint32_t)shared->d[0]; memcpy(&a, &ua, 4); memcpy(&b, &ub, 4);
						fprintf(vlog, "libm %s(%a%s%a) RESULT: recompiled %a (%#x), original %a (%#x)\n", fns[f].n, x, k & 1 ? "" : ", ", k & 1 ? 0.0 : y, a, ua, b, ub); }
					else { double a, b; memcpy(&a, &native->c.vfp.d[0], 8); memcpy(&b, &shared->d[0], 8);
						fprintf(vlog, "libm %s(%a%s%a) RESULT: recompiled %a, original %a\n", fns[f].n, x, k & 1 ? "" : ", ", k & 1 ? 0.0 : y, a, b); }
				}
			}
			if (!strncmp(report, "okf", 3)) nfl++;
			if (!strncmp(report, "ok", 2)) ok++;
			else if (!strncmp(report, "MISMATCH", 8)) { bad++; if (fbad++ < 8) fprintf(vlog, "libm %s(%a%s%a): %s\n", fns[f].n, x, k & 1 ? "" : ", ", k & 1 ? 0.0 : y, report); }
			else { skip++; if (fbad < 8 && getenv("RECOMP_VERIFY_SKIPS")) fprintf(vlog, "libm %s(%a): %s\n", fns[f].n, x, report[0] ? report : "no verdict"); }
		}
		fprintf(stderr, "    libm: %-10s %s%s\n", fns[f].n, fbad ? "MISMATCHES" : "agrees", fres ? " -- RESULTS DIFFER" : "");
	}
	fprintf(vlog, "== libm sweep: %llu ok (%llu FPSCR flags only), %llu mismatches (%llu with a different result), %llu skipped\n", (unsigned long long)ok, (unsigned long long)nfl, (unsigned long long)bad, (unsigned long long)nres, (unsigned long long)skip);
	fprintf(stderr, "== libm sweep: %llu ok (%llu FPSCR flags only), %llu mismatches (%llu with a different result), %llu skipped\n", (unsigned long long)ok, (unsigned long long)nfl, (unsigned long long)bad, (unsigned long long)nres, (unsigned long long)skip);
	fflush(vlog);
}

/* RECOMP_SNAP=<a RECOMP_DUMP_MEM file> RECOMP_SNAP_CALLS="addr:r0,r1,r2,r3[:s0,s1,s2,s3][;...]" (hex;
 * an s list may be "@addr", four floats read from the snapshot): at the game's first function entry,
 * a fork takes that snapshot over the guest arena -- heap, stacks, the images' data: one moment of
 * play -- and calls each function from it, original against recompiled exactly as check() does,
 * printing both sides' results. RECOMP_SNAP_SHOW=addr:len,... also lists the original's writes
 * there. A moment of play re-executed exactly and repeatably, no replay needed. Then the process exits. */
static void snap_calls(const Cpu *base) {
	const char *path = getenv("RECOMP_SNAP"), *calls = getenv("RECOMP_SNAP_CALLS"), *show = getenv("RECOMP_SNAP_SHOW");
	/* RECOMP_SNAP_CPU=<.cpu from RECOMP_VERIFY_CAPTURE>: the captured call's whole register state
	 * (sp into its own stack, VFP, flags) and its thread's TLS pointer, as every call's starting
	 * point; RECOMP_SNAP_CALLS then defaults to the captured function */
	Cpu cap; uint32_t cap_tls = 0, cap_entry = 0; int have_cap = 0;
	const char *cp = getenv("RECOMP_SNAP_CPU");
	if (cp) {
		FILE *cf = fopen(cp, "rb");
		have_cap = cf && fread(&cap, sizeof cap, 1, cf) == 1 && fread(&cap_tls, 4, 1, cf) == 1 && fread(&cap_entry, 4, 1, cf) == 1;
		if (cf) fclose(cf);
		if (!have_cap) { fprintf(stderr, "snap: cannot read %s\n", cp); exit(1); }
		base = &cap;
		static char one[16]; if (!calls) { snprintf(one, sizeof one, "%x", cap_entry); calls = one; }
	}
	FILE *f = fopen(path, "rb");
	if (!f || !calls) { fprintf(stderr, "snap: need RECOMP_SNAP=<file> and RECOMP_SNAP_CALLS (or RECOMP_SNAP_CPU)\n"); exit(1); }
	pid_t p = fork();
	if (p == 0) {
		uint32_t hdr[2]; uint64_t total = 0; int nb = 0;
		while (fread(hdr, 4, 2, f) == 2) {
			if ((uint64_t)hdr[0] + hdr[1] > 0xffff0000ull || fread(recomp_mem + hdr[0], 1, hdr[1], f) != hdr[1]) break;
			total += hdr[1]; nb++;
		}
		fclose(f);
		if (have_cap) recomp_tpidruro = cap_tls;   /* both sides read the captured thread's TLS block */
		fprintf(stderr, "snap: %d blocks, %llu bytes from %s\n", nb, (unsigned long long)total, path);
		uint32_t sh_a[16], sh_n[16]; int nsh = 0;
		if (show) { char b[512]; snprintf(b, sizeof b, "%s", show); for (char *t = strtok(b, ","); t && nsh < 16; t = strtok(0, ",")) { char *e; sh_a[nsh] = strtoul(t, &e, 16); sh_n[nsh++] = *e == ':' ? strtoul(e + 1, 0, 16) : 4; } }
		const char *tm = getenv("RECOMP_SNAP_TIME");
		uint64_t frame_us = getenv("RECOMP_SNAP_FRAME_US") ? strtoull(getenv("RECOMP_SNAP_FRAME_US"), 0, 0) : 33333;
		snap_time_base = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
		*snap_time_base = 100000000ull;
		if (tm) { snap_time_on = 1; snap_time_step = strtoull(tm, 0, 0); }
		int repeat = getenv("RECOMP_SNAP_REPEAT") ? atoi(getenv("RECOMP_SNAP_REPEAT")) : 1;   /* >1: the original's writes are kept between rounds -- the state carries on */
		for (int round = 0; round < repeat; round++) {
		if (repeat > 1) fprintf(stderr, "snap round %d (time base %llu us)\n", round, (unsigned long long)*snap_time_base);
		char buf[2048]; snprintf(buf, sizeof buf, "%s", calls);
		char *save = 0;
		for (char *t = strtok_r(buf, ";", &save); t; t = strtok_r(0, ";", &save)) {
			Cpu c = *base; char *e;
			uint32_t addr = strtoul(t, &e, 16);
			e = strchr(t, ':');
			if (e) {   /* registers r0.. */
				e++; for (int k = 0; k < 13 && *e && *e != ':'; k++) { c.r[k] = strtoul(e, &e, 16); if (*e == ',') e++; }
				if (*e == ':') {   /* s0.. : raw hex words, or @addr for four floats from memory */
					e++;
					if (*e == '@') { uint32_t a = strtoul(e + 1, &e, 16); for (int k = 0; k < 4; k++) { uint32_t w = RD32(a + 4 * k); memcpy(&c.vfp.f[k], &w, 4); } }
					else for (int k = 0; k < 16 && *e; k++) { uint32_t w = strtoul(e, &e, 16); memcpy(&c.vfp.f[k], &w, 4); if (*e == ',') e++; }
				}
			}
			if (!have_cap) c.r[13] = 0x9f0ff000u;   /* a scratch stack no snapshot covers (a captured call keeps its own) */
			report[0] = 0;
			pid_t q = fork();
			if (q == 0) { check(&c, addr); _exit(0); }
			int st; waitpid(q, &st, 0);
			float of[4], nf[4]; for (int k = 0; k < 4; k++) { uint32_t w = (uint32_t)(shared->d[k / 2] >> (32 * (k & 1))); memcpy(&of[k], &w, 4); nf[k] = native->c.vfp.f[k]; }
			fprintf(stderr, "snap call %#x: %s\n  original:   %s %#x r0=%#x s0-3=%g %g %g %g\n  recompiled: %s %#x r0=%#x s0-3=%g %g %g %g\n", addr, report[0] ? report : "(no verdict)",
			        shared->status == 1 ? "stopped at import" : shared->status == 0 ? "returned" : "error", shared->status == 1 ? shared->stop_at : 0, shared->r[0], of[0], of[1], of[2], of[3],
			        native->status == 1 ? "stopped at import" : "returned", native->status == 1 ? native->stop_at : 0, native->c.r[0], nf[0], nf[1], nf[2], nf[3]);
			if (shared->status == 1) { const char *n = recomp_import_name_any(shared->stop_at); fprintf(stderr, "  (the import: %s)\n", n ? n : "?"); }
			for (uint32_t i = 0; i < shared->nw && nsh; i++)
				for (int k = 0; k < nsh; k++)
					if (shared->w_addr[i] < sh_a[k] + sh_n[k] && shared->w_addr[i] + shared->w_len[i] > sh_a[k]) {
						fprintf(stderr, "  original wrote %#x+%u:", shared->w_addr[i], shared->w_len[i]);
						for (uint32_t j = 0; j < shared->w_len[i] && j < 32; j += 4) { uint32_t w; memcpy(&w, shared->bytes + shared->w_off[i] + j, 4); float fl; memcpy(&fl, &w, 4); fprintf(stderr, " %08x(%g)", w, fl); }
						fprintf(stderr, "\n");
					}
			if (repeat > 1 || getenv("RECOMP_SNAP_COMMIT"))   /* carry the original's writes into the state the next call starts from */
				for (uint32_t i = 0; i < shared->nw; i++) memcpy(recomp_mem + shared->w_addr[i], shared->bytes + shared->w_off[i], shared->w_len[i]);
		}
		*snap_time_base += frame_us;
		}
		_exit(0);
	}
	int st; waitpid(p, &st, 0);
	fflush(0); _exit(0);
}

/* ---- sampling past RECOMP_VERIFY=k's guaranteed first k calls: due() decides whether this call of
 * a function is a scheduled sample, rate_take() whether the budget allows spending a check on it */
static int due(uint32_t slot) {
	uint32_t n = __atomic_load_n(&calls[slot], __ATOMIC_RELAXED);
	if (verify_every > 0) return n % (uint32_t)verify_every == 0;
	return n != 0 && (n & (n - 1)) == 0;   /* a power of two: calls 2, 4, 8, 16, ... of this function */
}
static int rate_take(uint32_t have) {   /* vlock held */
	time_t now = time(0);
	if (now != rate_sec) { rate_sec = now; rate_budget = verify_rate; }
	if (rate_budget <= 0) return 0;
	uint32_t avg = n_fnstat ? (uint32_t)(nchecks / (uint64_t)n_fnstat) : 0;
	if (have > avg && rate_budget < verify_rate) return 0;   /* under pressure this second: a function with fewer checks so far goes first */
	rate_budget--;
	return 1;
}
static FnStat *fnstat_get(uint32_t addr) {   /* vlock held; open addressing like watch_tab -- a full table just stops tracking new ones */
	uint32_t h = ((addr >> 1) * 2654435761u) & (FNSTAT_HASH - 1);
	for (int k = 0; k < FNSTAT_HASH; k++, h = (h + 1) & (FNSTAT_HASH - 1)) {
		FnStat *s = &fnstat_tab[h];
		if (s->addr == addr) return s;
		if (!s->addr) {
			if (n_fnstat >= FNSTAT_HASH * 3 / 4) { fnstat_full = 1; return 0; }
			s->addr = addr; n_fnstat++; return s;
		}
	}
	return 0;
}
/* the running totals plus this function's own tally; a mismatch is logged in full only the first
 * time a function has one -- repeats just count, so a session that keeps re-finding the same bug
 * doesn't keep re-printing it (vlock held) */
static void record_result(uint32_t addr, const char *rep) {
	FnStat *s = fnstat_get(addr);
	if (s) s->checks++;
	if (!strncmp(rep, "okf", 3)) { nflags++; nok++; if (s) s->okf++; }   /* an ok too (the summary's "of the ok"); the per-function columns stay disjoint */
	else if (!strncmp(rep, "ok", 2)) { nok++; if (s) s->ok++; }
	else if (!strncmp(rep, "MISMATCH", 8)) {
		nbad++;
		if (!s || !s->bad++) {   /* no table slot (full), or this function's first mismatch: log it */
			fprintf(vlog, "%s\n", rep); fflush(vlog);
			if (s) {
				char buf[200]; int n = 0;
				for (const char *p = rep; *p && n < (int)sizeof buf - 1; p++) buf[n++] = *p == '\n' ? ' ' : *p;
				buf[n] = 0;
				snprintf(s->first_mismatch, sizeof s->first_mismatch, "%s", buf);
			}
		}
	} else {
		nskip++;
		if (s) s->skip++;
		if (getenv("RECOMP_VERIFY_SKIPS")) { fprintf(vlog, "%s\n", rep); fflush(vlog); }
	}
}

/* a call-sequence capture a hook arms (hook_seq_arm, e.g. packs/PCSA00029/hooks.c's uc_gesture_end at Uncharted's gesture success):
 * the next RECOMP_SEQ_MAX (30000) guest function entries on the thread that armed it, in order, each with its return address,
 * printed as "seq <n> <fn> <lr>" -- two such runs (60 and 30 fps from the same moment) diff to the first
 * call where they part */
#define SEQ_MAX_DEFAULT 30000   /* RECOMP_SEQ_MAX=n: a longer record */
static uint32_t (*seq_buf)[2]; static int seq_max; static volatile int seq_n = -1; static const char *seq_tn;
static void seq_arm(const char *why) {
	pthread_mutex_lock(&wlock);
	if (!seq_buf) { const char *e = getenv("RECOMP_SEQ_MAX"); seq_max = e ? atoi(e) : SEQ_MAX_DEFAULT; if (seq_max < 1000) seq_max = 1000; seq_buf = malloc(sizeof *seq_buf * (size_t)seq_max); }
	if (seq_n < 0 && seq_buf) { seq_tn = recomp_cur_tname(); seq_n = 0; fprintf(stderr, "seq: armed (%s) at frame %llu, thread %s\n", why, (unsigned long long)display_frames(), seq_tn ? seq_tn : "?"); }
	pthread_mutex_unlock(&wlock);
}
__attribute__((constructor)) static void seq_arm_hook(void) { extern void (*hook_seq_arm)(const char *); hook_seq_arm = seq_arm; }
static void seq_hit(Cpu *c, uint32_t addr) {
	if (recomp_cur_tname() != seq_tn) return;
	seq_buf[seq_n][0] = addr; seq_buf[seq_n][1] = c->r[14];
	if (++seq_n < seq_max) return;
	for (int i = 0; i < seq_max; i++) fprintf(stderr, "seq %d %08x %08x\n", i, seq_buf[i][0], seq_buf[i][1]);
	fprintf(stderr, "seq: done\n");
	seq_n = -1;
}

void recomp_enter(Cpu *c, uint32_t addr) {
	if (seq_n >= 0) seq_hit(c, addr);
	if (cov_on) coverage(addr);
	watch_hit(c, addr);
	datawatch_hit();
	if (enabled == 0 || verify_child) return;
	if (enabled < 0) {
		pthread_mutex_lock(&vlock);
		if (enabled < 0) {
			enabled = getenv("RECOMP_VERIFY") != 0;
			if (enabled) {
				per_fn = atoi(getenv("RECOMP_VERIFY")); if (per_fn < 1) per_fn = 1;
				{ const char *ev = getenv("RECOMP_VERIFY_EVERY"); if (ev) verify_every = atoi(ev); }
				{ const char *rr = getenv("RECOMP_VERIFY_RATE"); if (rr) { verify_rate = atoi(rr); if (verify_rate < 0) verify_rate = 0; } }
				calls = calloc(CODE_SLOTS, sizeof *calls);
				checks = calloc(CODE_SLOTS, sizeof *checks);
				fnstat_tab = calloc(FNSTAT_HASH, sizeof *fnstat_tab);
				shared = mmap(0, sizeof(Result), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
				capture_n = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
				report = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
				native = mmap(0, sizeof(Native), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
#ifdef RECOMP_TRACE
				step_mode = 1;
				trace_orig = mmap(0, sizeof(Trace), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
				trace_native = mmap(0, sizeof(Trace), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
#endif
				vlog = getenv("RECOMP_VERIFY_LOG") ? fopen(getenv("RECOMP_VERIFY_LOG"), "w") : stderr;
				if (!vlog) vlog = stderr;
				if (getenv("RECOMP_VERIFY_FOLLOW") && !atoi(getenv("RECOMP_VERIFY_FOLLOW"))) follow = 0;
				atexit(verify_summary);   /* a run ending through exit() (RECOMP_SHOT_LAST) still gets its summary and report */
				const char *lm = getenv("RECOMP_VERIFY_LIBM");
				if (lm) { libm_sweep(c); if (!strcmp(lm, "exit")) { fflush(0); _exit(0); } }
				if (getenv("RECOMP_SNAP")) snap_calls(c);
				const char *al = getenv("RECOMP_VERIFY_AT_LUA");
				if (al) { verify_lua_line = atoi(al); const char *d = strchr(al, ':'); verify_lua_delay = d ? atoi(d + 1) : 0; verify_open = 0; }
			}
		}
		pthread_mutex_unlock(&vlock);
		if (!enabled) return;
	}
	if (!verify_open) {
		if (display_frames() < verify_open_at) return;
		verify_open = 1;
		fprintf(stderr, "verify: checking from frame %llu\n", (unsigned long long)display_frames());
	}
	/* RECOMP_VERIFY_RANGE=lo-hi (hex guest addresses): check only the functions there (one
	 * subsystem in depth, e.g. a codec, with a high RECOMP_VERIFY count) */
	static uint32_t range_lo, range_hi = 0xffffffffu; static int range_read;
	if (!range_read) { const char *r = getenv("RECOMP_VERIFY_RANGE"); if (r) { char *e; range_lo = strtoul(r, &e, 16); if (*e == '-') range_hi = strtoul(e + 1, 0, 16); } range_read = 1; }
	if (addr < range_lo || addr >= range_hi) return;
	uint32_t slot = (addr - 0x81000000u) >> 1;
	if (slot >= CODE_SLOTS) return;
	__atomic_fetch_add(&calls[slot], 1, __ATOMIC_RELAXED);   /* every entry, whether or not it ends up sampled */
	if (checks[slot] >= (uint32_t)per_fn && !due(slot)) return;
	if (pthread_mutex_trylock(&vlock)) return;   /* one check at a time; other threads just run */
	uint32_t have = checks[slot];
	int extra = have >= (uint32_t)per_fn;   /* past RECOMP_VERIFY=k's guarantee: subject to the rate budget */
	if (extra && (!due(slot) || !rate_take(have))) { pthread_mutex_unlock(&vlock); return; }
	checks[slot] = have + 1;
	report[0] = 0;
	pid_t p = fork();
	if (p == 0) { helper_signals(); check(c, addr); capture_mismatch(c, addr); _exit(0); }
	int st; waitpid(p, &st, 0);
	nchecks++;
	if (!report[0]) snprintf(report, 512, "skip %#x: checker %s", addr, WIFSIGNALED(st) ? "crashed" : "gave no verdict");
	record_result(addr, report);
	if (nchecks % 500 == 0)
		fprintf(stderr, "    verify: %llu checks, %llu ok, %llu mismatches, %llu skipped\n", (unsigned long long)nchecks,
		        (unsigned long long)nok, (unsigned long long)nbad, (unsigned long long)nskip);
	pthread_mutex_unlock(&vlock);
}

static int fnstat_cmp_bad(const void *a, const void *b) {
	const FnStat *x = a, *y = b;
	return (x->bad < y->bad) - (x->bad > y->bad);
}
void verify_summary(void) {
	static int done;   /* main.c's stop paths and the atexit hook below may both get here */
	if (enabled <= 0 || done++) return;
	/* the counters and the per-function table are written under vlock; main.c's signal handler calls
	 * this too, possibly on the very thread holding it, so only try for the lock (a check in flight
	 * leaves one function's row a check behind) */
	int locked = pthread_mutex_trylock(&vlock) == 0;
	uint64_t entered = 0;
	for (uint32_t i = 0; i < CODE_SLOTS; i++) if (calls[i]) entered++;
	fprintf(stderr, "== verify: %llu checks, %llu ok (%llu differing only in FPSCR exception flags), %llu mismatches, %llu skipped\n", (unsigned long long)nchecks,
	        (unsigned long long)nok, (unsigned long long)nflags, (unsigned long long)nbad, (unsigned long long)nskip);
	fprintf(stderr, "== verify: %llu functions entered, %d checked at least once%s\n", (unsigned long long)entered, n_fnstat,
	        fnstat_full ? " (table full: some not tracked individually)" : "");
	if (nbad) {   /* the functions worth looking at first, most mismatches down */
		FnStat *v = malloc(sizeof(FnStat) * (n_fnstat ? n_fnstat : 1)); int n = 0;
		for (int i = 0; i < FNSTAT_HASH; i++) if (fnstat_tab[i].addr) v[n++] = fnstat_tab[i];
		qsort(v, n, sizeof *v, fnstat_cmp_bad);
		fprintf(stderr, "== verify: top mismatching functions:\n");
		for (int i = 0; i < n && i < 10 && v[i].bad; i++)
			fprintf(stderr, "  %#010x  %u/%u checks mismatch: %s\n", v[i].addr, v[i].bad, v[i].checks, v[i].first_mismatch);
		free(v);
	}
	/* RECOMP_VERIFY_REPORT=<file>: a machine-readable line per checked function, for a session too
	 * long to read off stderr -- '#' the one summary line, then one line per function: address,
	 * calls seen (ever, checked or not), checks, ok, okf, mismatches, skips, first mismatch */
	const char *rp = getenv("RECOMP_VERIFY_REPORT");
	FILE *f = rp ? fopen(rp, "w") : 0;
	if (!f) { if (locked) pthread_mutex_unlock(&vlock); return; }
	fprintf(f, "# entered %llu checked %d%s\n", (unsigned long long)entered, n_fnstat, fnstat_full ? " (table full)" : "");
	for (int i = 0; i < FNSTAT_HASH; i++) {
		FnStat *s = &fnstat_tab[i];
		if (!s->addr) continue;
		uint32_t slot = (s->addr - 0x81000000u) >> 1;
		fprintf(f, "%#010x %u %u %u %u %u %u %s\n", s->addr, slot < CODE_SLOTS ? calls[slot] : 0, s->checks, s->ok, s->okf, s->bad, s->skip,
		        s->first_mismatch[0] ? s->first_mismatch : "-");
	}
	fclose(f);
	if (locked) pthread_mutex_unlock(&vlock);
}

/* memscan.c's RECOMP_MEMSCAN_WATCH: addresses a scan found during this run, watched from now on (an address found in
 * one run is no use in the next -- a replay's heap layout drifts -- so the scan and the watch share the run) */
int datawatch_add(uint32_t a) {
	pthread_mutex_lock(&wlock);
	if (n_dw < MAX_DW) dw_addr[n_dw++] = a;
	if (!dw_pagesz) dw_pagesz = sysconf(_SC_PAGESIZE);
	dw_from = 0;
	if (dw_armed) dw_protect(1); else dw_state = 1;
	pthread_mutex_unlock(&wlock);
	fprintf(stderr, "datawatch: %#x added by the scan (%d watched); recomp_enter at %p\n", a, n_dw, (void *)recomp_enter);
	return 1;
}
