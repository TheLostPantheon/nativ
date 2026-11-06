/* fixture_neon_harness.c -- executed semantics checks for the NEON/ARM emitter additions:
 * ssat/usat, vzip.8, vqmovn/vqmovun, vqshrn/vqshrun, vmlal/vmlsl, vqabs.s8 (recompiler/recomp.py).
 *
 * Compiled together with the generated C of fixture_neon_emitters.py's synthetic module; each
 * case presets the Cpu registers, runs the one-instruction guest function and compares the
 * result registers against hand-computed values (architecture saturation/interleave rules:
 * SSAT clamps to [-2^(n-1), 2^(n-1)-1], USAT to [0, 2^n-1], vqmovn halves each element with
 * saturation, vzip interleaves element streams, vmlal widens and accumulates, vqabs saturates
 * at INT_MIN). An UNIMPL in a body (the pre-fix emitter) fails its case.
 *
 * Build and run: see README.md in this directory (driven by fixture_neon_emitters.py).
 */
#include "fixture_neon_out.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t *recomp_mem;
void recomp_call_external(Cpu *c, uint32_t addr) { (void)c; (void)addr; }
static int unimpls;
void recomp_unimplemented(Cpu *c, uint32_t addr) { (void)c; unimpls++; fprintf(stderr, "  UNIMPL at %#x\n", addr); }

static int checks, failed;
static void ck(int ok, const char *name) {
	checks++;
	if (!ok) { failed++; fprintf(stderr, "FAIL %s\n", name); }
}

/* ---- scalar saturation: ssat/usat r0, #8, r1 ---- */
static void scalar_case(void (*fn)(Cpu *), const char *name,
                        const uint32_t *in, const uint32_t *want, int n) {
	for (int i = 0; i < n; i++) {
		Cpu c; memset(&c, 0, sizeof c);
		c.r[1] = in[i];
		int u0 = unimpls;
		fn(&c);
		ck(unimpls == u0, name);                       /* the body must not fall to UNIMPL */
		if (c.r[0] != want[i]) {
			checks++; failed++;
			fprintf(stderr, "FAIL %s: in %#x got %#x want %#x\n", name, in[i], c.r[0], want[i]);
		} else checks++;
	}
}
static void case_ssat(void) {
	static const uint32_t in[] = { 300, (uint32_t)-300, 127, 0xffffff80u /* -128 */, 0, 12 };
	static const uint32_t want[] = { 127, 0xffffff80u, 127, 0xffffff80u, 0, 12 };
	scalar_case(fn_81000000, "ssat r0,#8,r1", in, want, 6);
}
static void case_usat(void) {
	static const uint32_t in[] = { 300, (uint32_t)-300, 255, (uint32_t)-5, 0, 100 };
	static const uint32_t want[] = { 255, 0, 255, 0, 0, 100 };
	scalar_case(fn_81000010, "usat r0,#8,r1", in, want, 6);
}

/* ---- vqmovn.s16 d0, q1: signed 16-bit lanes clamped to [-128, 127] bytes ---- */
static void case_vqmovn_s16(void) {
	static const int16_t in[8] = { 256, -256, 5, -5, 32767, -32768, 0, 127 };
	static const uint8_t want[8] = { 127, 0x80, 5, 0xfb, 127, 0x80, 0, 127 };
	Cpu c; memset(&c, 0, sizeof c);
	memcpy(&c.vfp.d[2], in, 16);                        /* q1 = d2:d3 */
	int u0 = unimpls;
	fn_81000020(&c);
	ck(unimpls == u0, "vqmovn.s16: no UNIMPL");
	ck(memcmp(&c.vfp.d[0], want, 8) == 0, "vqmovn.s16 d0,q1 (sat + narrow + lane order)");
}

/* ---- vqmovun.s16 d0, q1: signed lanes clamped to [0, 255] bytes ---- */
static void case_vqmovun_s16(void) {
	static const int16_t in[8] = { 256, -256, 5, -5, 32767, -32768, 0, 127 };
	static const uint8_t want[8] = { 255, 0, 5, 0, 255, 0, 0, 127 };
	Cpu c; memset(&c, 0, sizeof c);
	memcpy(&c.vfp.d[2], in, 16);
	int u0 = unimpls;
	fn_81000030(&c);
	ck(unimpls == u0, "vqmovun.s16: no UNIMPL");
	ck(memcmp(&c.vfp.d[0], want, 8) == 0, "vqmovun.s16 d0,q1 (signed in, unsigned out)");
}

/* ---- vqmovn.u16 d0, q1: unsigned lanes clamped to [0, 255] bytes ---- */
static void case_vqmovn_u16(void) {
	static const uint16_t in[8] = { 256, 65535, 5, 0, 300, 255, 256, 1 };
	static const uint8_t want[8] = { 255, 255, 5, 0, 255, 255, 255, 1 };
	Cpu c; memset(&c, 0, sizeof c);
	memcpy(&c.vfp.d[2], in, 16);
	int u0 = unimpls;
	fn_81000040(&c);
	ck(unimpls == u0, "vqmovn.u16: no UNIMPL");
	ck(memcmp(&c.vfp.d[0], want, 8) == 0, "vqmovn.u16 d0,q1 (unsigned clamp)");
}

/* ---- vzip.8 d0, d1: element interleave ---- */
static void case_vzip_8(void) {
	static const uint8_t x[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	static const uint8_t y[8] = { 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18 };
	static const uint8_t want_d0[8] = { 1, 0x11, 2, 0x12, 3, 0x13, 4, 0x14 };
	static const uint8_t want_d1[8] = { 5, 0x15, 6, 0x16, 7, 0x17, 8, 0x18 };
	Cpu c; memset(&c, 0, sizeof c);
	memcpy(&c.vfp.d[0], x, 8); memcpy(&c.vfp.d[1], y, 8);
	int u0 = unimpls;
	fn_81000050(&c);
	ck(unimpls == u0, "vzip.8: no UNIMPL");
	ck(memcmp(&c.vfp.d[0], want_d0, 8) == 0, "vzip.8 d0 (interleave order)");
	ck(memcmp(&c.vfp.d[1], want_d1, 8) == 0, "vzip.8 d1 (second half)");
}

/* ---- vmlal.s16 q0, d2, d3: widening multiply-accumulate ---- */
static void case_vmlal_s16(void) {
	static const int16_t x[4] = { 2, -3, 100, -200 };
	static const int16_t y[4] = { 40, 50, -2, 3 };
	static const int32_t acc[4] = { 1000, -1000, 7, -7 };
	static const int32_t want[4] = { 1080, -1150, -193, -607 };   /* acc + x*y */
	Cpu c; memset(&c, 0, sizeof c);
	memcpy(&c.vfp.d[2], x, 8); memcpy(&c.vfp.d[3], y, 8); memcpy(&c.vfp.d[0], acc, 16);
	int u0 = unimpls;
	fn_81000060(&c);
	ck(unimpls == u0, "vmlal.s16: no UNIMPL");
	ck(memcmp(&c.vfp.d[0], want, 16) == 0, "vmlal.s16 q0,d2,d3 (widening accumulate)");
}

/* ---- vqshrn.s16 d0, q1, #4: arithmetic shift, then signed saturation ---- */
static void case_vqshrn_s16(void) {
	static const int16_t in[8] = { 32767, -32768, 256, -256, 4095, 16, -17, 0 };
	static const uint8_t want[8] = { 127, 0x80, 16, 0xf0, 127, 1, 0xfe, 0 };
	Cpu c; memset(&c, 0, sizeof c);
	memcpy(&c.vfp.d[2], in, 16);
	int u0 = unimpls;
	fn_81000070(&c);
	ck(unimpls == u0, "vqshrn.s16: no UNIMPL");
	ck(memcmp(&c.vfp.d[0], want, 8) == 0, "vqshrn.s16 d0,q1,#4 (shift + sat)");
}

/* ---- vqshrun.s16 d0, q1, #4: arithmetic shift, then unsigned saturation ---- */
static void case_vqshrun_s16(void) {
	static const int16_t in[8] = { 32767, -32768, 256, -256, 4095, 16, -17, 0 };
	static const uint8_t want[8] = { 255, 0, 16, 0, 255, 1, 0, 0 };
	Cpu c; memset(&c, 0, sizeof c);
	memcpy(&c.vfp.d[2], in, 16);
	int u0 = unimpls;
	fn_81000080(&c);
	ck(unimpls == u0, "vqshrun.s16: no UNIMPL");
	ck(memcmp(&c.vfp.d[0], want, 8) == 0, "vqshrun.s16 d0,q1,#4 (shift + unsigned sat)");
}

/* ---- vqabs.s8 d0, d1: |x| with INT_MIN saturating to INT_MAX ---- */
static void case_vqabs_s8(void) {
	static const int8_t in[8] = { -128, -1, 127, 0, -5, 5, -127, 64 };
	static const uint8_t want[8] = { 127, 1, 127, 0, 5, 5, 127, 64 };
	Cpu c; memset(&c, 0, sizeof c);
	memcpy(&c.vfp.d[1], in, 8);
	int u0 = unimpls;
	fn_81000090(&c);
	ck(unimpls == u0, "vqabs.s8: no UNIMPL");
	ck(memcmp(&c.vfp.d[0], want, 8) == 0, "vqabs.s8 d0,d1 (INT_MIN -> INT_MAX)");
}

int main(void) {
	recomp_mem = calloc((size_t)4 << 30, 1);
	if (!recomp_mem) { perror("calloc"); return 2; }
	case_ssat();
	case_usat();
	case_vqmovn_s16();
	case_vqmovun_s16();
	case_vqmovn_u16();
	case_vzip_8();
	case_vmlal_s16();
	case_vqshrn_s16();
	case_vqshrun_s16();
	case_vqabs_s8();
	if (failed) { printf("%d checks FAILED\n", failed); return 1; }
	printf("all passed (%d ok)\n", checks);
	return 0;
}
