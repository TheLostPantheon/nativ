/*
 * RECOMP_MEMSCAN=frame:op,frame:op,... -- finds a game's state that follows an input, the way a memory scanner does, so
 * a mouse hook knows what to write. In a recorded session (RECOMP_INPUT_PLAY), each step filters the 4-byte-aligned
 * floats of the game's memory by what they did since the previous step:
 *   s (the first step: the snapshot)   c changed   = held still (within RECOMP_MEMSCAN_EPS, default 1e-4)
 *   + changed the same way as at the first c   - changed the other way
 * e.g. "4943:s,4969:c,4978:=,5019:+,5031:=,5047:-" -- a right turn, still, a right turn, still, a left turn: an aim yaw.
 * Each step logs its survivor count; the last, up to 64 survivors with their value at every step. The writer of the
 * right one is the hook point (an lldb watchpoint, see lldb-guest-watchpoints). Called once per presented frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "rt.h"
#include "hle.h"

void memscan_frame(uint64_t frame);
__attribute__((weak)) int datawatch_add(uint32_t addr) { (void)addr; return 0; }   /* verify.c's (a VERIFY build) replaces this: 0 = no write watch here */
uint32_t memscan_best;   /* RECOMP_MEMSCAN_WATCH's best fit, for a debugger to watch exactly (lldb: a hardware watchpoint once it is
                          * non-zero) -- the page-protection watch misses a store whose function wrote the same page just before */

#define MAXSTEP 12
typedef struct { uint32_t addr; float v[MAXSTEP]; } Cand;
static Cand *cands; static size_t ncand;
static uint8_t *snap; static uint32_t snap_lo[65], snap_hi[65]; static int nsnap;
static long step_frame[MAXSTEP]; static char step_op[MAXSTEP]; static int nsteps, cur, parsed, first_c = -1;

static float rdf(uint32_t a) { float f; memcpy(&f, G(a), 4); return f; }
static int sane(float f) { return isfinite(f) && fabsf(f) < 1.0e5f; }

/* RECOMP_MEMWATCH=addr[,addr...] (hex, up to 8) with RECOMP_MEMWATCH_FROM/_TO=frame: those floats logged every presented
 * frame in the window -- a candidate from a scan followed through an input (does it move when the stick does?) */
static void memwatch_frame(uint64_t frame) {
	static uint32_t wa[8]; static int nw = -1; static long from, to;
	if (nw < 0) {
		nw = 0; const char *e = getenv("RECOMP_MEMWATCH");
		for (const char *p = e; p && *p && nw < 8; ) { wa[nw++] = (uint32_t)strtoul(p, 0, 16); p = strchr(p, ','); if (p) p++; }
		from = getenv("RECOMP_MEMWATCH_FROM") ? atol(getenv("RECOMP_MEMWATCH_FROM")) : 0; to = getenv("RECOMP_MEMWATCH_TO") ? atol(getenv("RECOMP_MEMWATCH_TO")) : (long)1e12;
	}
	if (!nw || (long)frame < from || (long)frame > to) return;
	fprintf(stderr, "    memwatch: frame %llu", (unsigned long long)frame);
	for (int i = 0; i < nw; i++) fprintf(stderr, " %#x=%g", wa[i], rdf(wa[i]));
	fprintf(stderr, "\n");
}
void memscan_frame(uint64_t frame) {
	memwatch_frame(frame);
	if (!parsed) {
		parsed = 1; const char *e = getenv("RECOMP_MEMSCAN");
		for (const char *p = e; p && *p && nsteps < MAXSTEP; ) {
			long f; char op; if (sscanf(p, "%ld:%c", &f, &op) != 2) break;
			step_frame[nsteps] = f; step_op[nsteps++] = op;
			p = strchr(p, ','); if (p) p++;
		}
	}
	if (cur >= nsteps || (long)frame != step_frame[cur]) return;
	float eps = getenv("RECOMP_MEMSCAN_EPS") ? (float)atof(getenv("RECOMP_MEMSCAN_EPS")) : 1e-4f;
	char op = step_op[cur];
	if (cur == 0) {   /* a raw snapshot of every range in use */
		nsnap = guest_mem_ranges(snap_lo, snap_hi, 65); size_t bytes = 0;
		for (int i = 0; i < nsnap; i++) bytes += snap_hi[i] - snap_lo[i];
		free(snap); snap = malloc(bytes);
		size_t o = 0; for (int i = 0; i < nsnap; i++) { memcpy(snap + o, G(snap_lo[i]), snap_hi[i] - snap_lo[i]); o += snap_hi[i] - snap_lo[i]; }
		ncand = bytes / 4;
	} else if (cur == 1) {   /* the candidates: what changed since the snapshot (the first step after it must be 'c') */
		size_t cap = 1 << 20, o = 0; cands = malloc(cap * sizeof *cands); ncand = 0;
		for (int i = 0; i < nsnap; i++) {
			for (uint32_t a = snap_lo[i] & ~3u; a + 4 <= snap_hi[i]; a += 4) {
				float was; memcpy(&was, snap + o + (a - snap_lo[i]), 4); float now = rdf(a);
				if (!sane(was) || !sane(now) || fabsf(now - was) <= eps) continue;
				if (ncand == cap) { cap *= 2; cands = realloc(cands, cap * sizeof *cands); }
				cands[ncand].addr = a; cands[ncand].v[0] = was; cands[ncand].v[1] = now; ncand++;
			}
			o += snap_hi[i] - snap_lo[i];
		}
		free(snap); snap = 0; first_c = 1;
	} else {
		size_t k = 0;
		for (size_t i = 0; i < ncand; i++) {
			float now = rdf(cands[i].addr), was = cands[i].v[cur - 1], d = now - was; int keep = sane(now);
			if (op == 'c') keep = keep && fabsf(d) > eps;
			else if (op == '=') keep = keep && fabsf(d) <= eps;
			else if (op == '+' || op == '-') {
				float d0 = cands[i].v[first_c] - cands[i].v[first_c - 1];
				keep = keep && fabsf(d) > eps && ((d > 0) == (d0 > 0)) == (op == '+');
			}
			if (keep) { cands[k] = cands[i]; cands[k].v[cur] = now; k++; }
		}
		ncand = k;
		if (op == 'c' && first_c < 0) first_c = cur;
	}
	fprintf(stderr, "    memscan: frame %llu step %d '%c': %zu candidates\n", (unsigned long long)frame, cur, op, ncand);
	if (++cur == nsteps) {
		/* RECOMP_MEMSCAN_RATIO=r,r,...: each change step's expected change relative to the first one's (how far the stick
		 * moved over it, say), within a factor of 2; RECOMP_MEMSCAN_MAG=lo,hi: the first change's size. Survivors only. */
		const char *re = getenv("RECOMP_MEMSCAN_RATIO"), *me = getenv("RECOMP_MEMSCAN_MAG");
		float ratio[MAXSTEP]; int nr = 0; for (const char *p = re; p && *p && nr < MAXSTEP; ) { ratio[nr++] = (float)atof(p); p = strchr(p, ','); if (p) p++; }
		float mlo = 0, mhi = 1e9f; if (me) sscanf(me, "%f,%f", &mlo, &mhi);
		size_t k = 0;
		for (size_t i = 0; i < ncand; i++) {
			float d0 = cands[i].v[first_c] - cands[i].v[first_c - 1]; int keep = fabsf(d0) >= mlo && fabsf(d0) <= mhi, r = 0;
			for (int s2 = first_c + 1; s2 < nsteps && keep; s2++) {
				if (step_op[s2] != '+' && step_op[s2] != '-' && step_op[s2] != 'c') continue;
				if (r >= nr) break;
				float want = ratio[r++] * d0, got = cands[i].v[s2] - cands[i].v[s2 - 1];
				keep = (want > 0) == (got > 0) && fabsf(got) >= fabsf(want) / 2 && fabsf(got) <= fabsf(want) * 2;
			}
			if (keep) cands[k++] = cands[i];
		}
		if (re || me) { ncand = k; fprintf(stderr, "    memscan: %zu fit the ratios %s and size %s\n", ncand, re ? re : "-", me ? me : "-"); }
		/* RECOMP_MEMSCAN_WATCH=n (a VERIFY build): the n best fits that also leave [-1, 1] somewhere (an angle, not a direction
		 * component) get write watches (verify.c datawatch_add) for the rest of the run: which function writes them */
		int nwatch = getenv("RECOMP_MEMSCAN_WATCH") ? atoi(getenv("RECOMP_MEMSCAN_WATCH")) : 0;
		if (nwatch > 0) {
			typedef struct { float score; size_t i; } Sc; Sc *sc = malloc((ncand ? ncand : 1) * sizeof *sc); size_t ns = 0;
			for (size_t i = 0; i < ncand; i++) {
				float m = 0, score = 0, d0 = cands[i].v[first_c] - cands[i].v[first_c - 1]; int r = 0;
				for (int s2 = 0; s2 < nsteps; s2++) if (fabsf(cands[i].v[s2]) > m) m = fabsf(cands[i].v[s2]);
				if (m <= 1.01f) continue;
				for (int s2 = first_c + 1; s2 < nsteps && r < nr; s2++) if (step_op[s2] == '+' || step_op[s2] == '-' || step_op[s2] == 'c') score += fabsf((cands[i].v[s2] - cands[i].v[s2 - 1]) / d0 - ratio[r++]);
				sc[ns].score = score; sc[ns].i = i; ns++;
			}
			for (size_t a2 = 1; a2 < ns; a2++) for (size_t b2 = a2; b2 > 0 && sc[b2].score < sc[b2 - 1].score; b2--) { Sc t = sc[b2]; sc[b2] = sc[b2 - 1]; sc[b2 - 1] = t; }
			for (size_t w = 0; w < ns && (int)w < nwatch; w++) {
				Cand *cw = &cands[sc[w].i];
				if (!w) __atomic_store_n(&memscan_best, cw->addr, __ATOMIC_RELEASE);
				fprintf(stderr, "    memscan: watching %#010x (fit %.3f):", cw->addr, sc[w].score);
				for (int s2 = 0; s2 < nsteps; s2++) fprintf(stderr, " %g", cw->v[s2]);
				fprintf(stderr, "\n");
				if (!datawatch_add(cw->addr)) { fprintf(stderr, "    memscan: (no write watch in this build: RECOMP_MEMSCAN_WATCH needs the verify build)\n"); break; }
			}
			free(sc);
		}
		const char *op2 = getenv("RECOMP_MEMSCAN_OUT");   /* every survivor, one line each: address and its value at each step */
		FILE *of = op2 ? fopen(op2, "w") : 0;
		for (size_t i = 0; of && i < ncand; i++) { fprintf(of, "%#010x", cands[i].addr); for (int s2 = 0; s2 < nsteps; s2++) fprintf(of, " %g", cands[i].v[s2]); fprintf(of, "\n"); }
		if (of) { fclose(of); fprintf(stderr, "    memscan: %zu survivors written to %s\n", ncand, op2); }
		for (size_t i = 0; i < ncand && i < 64; i++) {
			fprintf(stderr, "    memscan:   %#010x", cands[i].addr);
			for (int s = 0; s < nsteps; s++) fprintf(stderr, " %g", cands[i].v[s]);
			fprintf(stderr, "\n");
		}
		free(cands); cands = 0;
	}
}
