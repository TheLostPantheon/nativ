/*
 * Synthetic USSE subroutine-call fixture: usse.c's inline_calls.
 *
 * A compiled program can put a subroutine at the start of its code -- instructions 0..r ending in a return (control
 * group, function 40-38 = 2) -- and call it from the main body after it with a branch to instruction 0 (save_link set).
 * Execution starts after the last return. The decoder took the return for a plain no-op and each call for a jump back
 * to 0: the interpreter ran the main body again and again until its 100000-branch guard (every vertex), and the GPU
 * translator refused the program ("branch ... outside the structured forms translated"), so every draw with it went
 * to the CPU. Seen in a vertex program that runs a 9-instruction matrix routine twice (~1.5 s per 6-vertex draw).
 *
 * The programs here are hand-encoded control-group words (docs/usse-isa.md: op1 = 0x1F, predicate in bits 58-56,
 * function 40-38: 1 BR / 2 return / 5 NOP, save_link bit 41, br_type bit 38, a signed 20-bit offset), decoded by the
 * real decoder and handed to usse_spirv_translate. fail-before: the decoded order keeps the calls (8 instructions, not
 * 7) and the translator refuses; pass-after: the calls are inlined into straight code, the module is made and -- when
 * SPIRV_VAL names a spirv-val binary -- validates. A predicated call and a return with no call stay as decoded.
 *
 *     I="-I ../gfx -I ../core -I ../../recompiler -I ../.."
 *     cc -O1 -g -Wall -D_GNU_SOURCE $I usse_call_test.c ../gfx/usse.c ../gfx/usse_spirv.c -o /tmp/usse_call_test -lm && /tmp/usse_call_test
 *     (SPIRV_VAL=<spirv-val> also validates every module; SPIRV_TMP=<dir> for the module files, default /tmp)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "usse.h"
#include "usse_internal.h"
#include "usse_spirv.h"

uint8_t *recomp_mem;

static int checks, failed;
static void ck(int cond, const char *what) { checks++; printf("%-76s %s\n", what, cond ? "ok" : "FAIL"); if (!cond) failed++; }

/* synthetic GXP header (usse.c's H_* layout) */
#define GXP_ADDR 0x10000000u
#define H_FLAGS 20
#define H_PRIM_REGS 48
#define H_PRIM_INSTR_COUNT 60
#define H_PRIM_OFF 64
#define CODE_OFF 128
static void put32(uint32_t a, uint32_t v) { memcpy(recomp_mem + a, &v, 4); }
static void put64(uint32_t a, uint64_t v) { memcpy(recomp_mem + a, &v, 8); }

enum { P_NONE = 0, P0 = 1 };   /* the extended predicate field */
#define NOP 0xF800014000000000ull    /* function 5 */
#define RET 0xF800008000000000ull    /* function 2: return */
#define DUMMY 0xF920000000000000ull  /* a SPEC no-op (op2 1, opcat 2): a marker the decoder keeps as OP_NOP, not plain */
static uint64_t br(int pred, int off) { return 0xF800004000000000ull | (uint64_t)pred << 56 | ((uint64_t)(uint32_t)off & 0xFFFFF); }
static uint64_t call(int off) { return 0xF800024000000000ull | ((uint64_t)(uint32_t)off & 0xFFFFF); }   /* BR + save_link */

static UsseProgram *load(const uint64_t *w, int n, int fragment) {
	static uint32_t seq;
	uint32_t g = GXP_ADDR + (++seq) * 0x10000;   /* a fresh address per program */
	memset(recomp_mem + g, 0, CODE_OFF + 8 * (size_t)n + 64);
	put32(g + 8, CODE_OFF + 8 * (uint32_t)n);
	put32(g + H_FLAGS, fragment ? 1 : 0);
	put32(g + H_PRIM_REGS, 16);
	put32(g + H_PRIM_INSTR_COUNT, (uint32_t)n);
	put32(g + H_PRIM_OFF, CODE_OFF - H_PRIM_OFF);
	for (int i = 0; i < n; i++) put64(g + CODE_OFF + 8 * (uint32_t)i, w[i]);
	return usse_decode_uncached(g);
}
/* the decoded instructions' raw words, in order */
static int code_is(const char *name, const uint64_t *w, int n, const uint64_t *want, int nwant) {
	UsseProgram *p = load(w, n, 0); int nc = 0; const UOp *c = p ? usse_program_code(p, 0, &nc) : 0;
	int same = c && nc == nwant; for (int i = 0; same && i < nc; i++) same = c[i].raw == want[i];
	char what[160]; snprintf(what, sizeof what, "[%s] decoded order (%d instructions, want %d)", name, nc, nwant); ck(same, what);
	if (p) usse_program_free(p);
	return same;
}
static int count_op(const SpirvModule *m, uint32_t op) {
	int c = 0;
	for (int i = 5; i < m->nwords; ) { uint32_t wc = m->words[i] >> 16; if ((m->words[i] & 0xFFFF) == op) c++; if (!wc) break; i += (int)wc; }
	return c;
}
static int validate(const SpirvModule *m, const char *name) {
	const char *val = getenv("SPIRV_VAL"); if (!val || !*val) return 1;
	const char *dir = getenv("SPIRV_TMP") ? getenv("SPIRV_TMP") : "/tmp";
	char path[256]; snprintf(path, sizeof path, "%s/usse_call_%s.spv", dir, name);
	for (char *q = path + strlen(dir); *q; q++) if (*q == ' ' || *q == '/' || *q == '(' || *q == ')') *q = '_';
	FILE *f = fopen(path, "wb"); if (!f) return 0;
	fwrite(m->words, 4, (size_t)m->nwords, f); fclose(f);
	char cmd[600]; snprintf(cmd, sizeof cmd, "%s %s", val, path);
	int rc = system(cmd);
	remove(path);
	return rc == 0;
}
static void translates(const char *name, const uint64_t *w, int n, int fragment, int want_ok) {
	UsseProgram *p = load(w, n, fragment);
	usse_spirv_set_canonical_varyings(1);
	uint32_t sa[256] = {0}; int units[16] = {0}, nsmp = 0;
	SpirvModule m = usse_spirv_translate(p, 0, fragment ? usse_fragment_layout(p) : 0, sa, 16, units, &nsmp, 0, 0, 0, 0);
	char what[200];
	if (want_ok) {
		snprintf(what, sizeof what, "[%s] translates%s%s", name, m.ok ? "" : ": ", m.ok ? "" : m.err); ck(m.ok, what);
		if (m.ok) {
			snprintf(what, sizeof what, "[%s] no OpLoopMerge (%d)", name, count_op(&m, 246)); ck(count_op(&m, 246) == 0, what);
			snprintf(what, sizeof what, "[%s] spirv-val accepts the module%s", name, getenv("SPIRV_VAL") ? "" : " (SPIRV_VAL unset: skipped)");
			ck(validate(&m, name), what);
		}
	} else { snprintf(what, sizeof what, "[%s] refused cleanly (%s)", name, m.ok ? "translated!" : m.err); ck(!m.ok && m.err[0], what); }
	usse_spirv_free(&m);
	usse_program_free(p);
}

int main(void) {
	recomp_mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (recomp_mem == MAP_FAILED) { perror("mmap"); return 2; }
	const uint64_t A = DUMMY, B = DUMMY | 1, M1 = NOP | 1, M2 = NOP | 2, END = NOP | 3;   /* distinct words, same meaning */

	/* subroutine 0-1 + return at 2; main 3..7 with calls at 4 and 6 -> M1 A B M2 A B END */
	const uint64_t calls[] = { A, B, RET, M1, call(-4), M2, call(-6), END };
	const uint64_t want[] = { M1, A, B, M2, A, B, END };
	code_is("two calls", calls, 8, want, 7);
	translates("two calls (vertex)", calls, 8, 0, 1);
	translates("two calls (fragment)", calls, 8, 1, 1);

	/* a skip in main over a call is re-aimed: (p0) BR +3 (3 -> 6) becomes BR +4 past the inlined copy */
	const uint64_t skip_call[] = { A, B, RET, br(P0, 3), call(-4), M1, END };
	UsseProgram *q = load(skip_call, 7, 1); int nq = 0; const UOp *cq = usse_program_code(q, 0, &nq);
	ck(q && nq == 5 && cq[0].op == OP_BR && cq[0].imm == 4 && cq[1].raw == A && cq[3].raw == M1, "[skip over a call] 5 instructions, the skip's offset 4");
	usse_program_free(q);
	translates("skip over a call", skip_call, 7, 1, 1);

	/* left as decoded: a predicated call (refused, as before), a return that ends the program (no call) */
	const uint64_t pcall[] = { A, RET, M1, call(-3) | (uint64_t)P0 << 56, END };
	code_is("predicated call left as decoded", pcall, 5, pcall, 5);
	translates("predicated call", pcall, 5, 1, 0);
	const uint64_t ret_end[] = { M1, M2, RET };
	code_is("return at the end, no call", ret_end, 3, ret_end, 3);

	printf("%d checks, %d failed\n", checks, failed);
	if (failed) { printf("FAILED\n"); return 1; }
	printf("all passed\n");
	return 0;
}
