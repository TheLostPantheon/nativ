/*
 * Runtime for statically recompiled ARMv7 (Thumb-2, VFP, NEON) code: recompiler/recomp.py
 * turns each guest function into a C function over a Cpu (the guest register file and flags);
 * guest memory is one 4 GiB arena (guest address a lives at mem + a), so the recompiled code keeps
 * the original's 32-bit pointers inside a 64-bit host process.
 */
#ifndef RECOMP_RT_H
#define RECOMP_RT_H
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>
/* the Vita's VFP multiply-accumulates (vmla/vmls/vnmla) round the product before adding; a C
 * compiler may fuse "d + a * b" into one fused multiply-add (one rounding: different last bits,
 * found by the in-place checker), so contraction is off for all recompiled code */
#pragma STDC FP_CONTRACT OFF

typedef struct Cpu {
	uint32_t r[16];
	uint32_t n, z, c, v;          /* APSR flags, 0 or 1 */
	uint32_t q;                   /* sticky saturation */
	uint32_t _pad;                /* keeps the VFP bank 8-aligned at a fixed offset (difftest mirrors this layout) */
	union { uint32_t s[64]; uint64_t d[32]; float f[64]; double fd[32]; } vfp;
	uint32_t fpscr;               /* N Z C V in bits 31..28 */
	uint32_t stop;                /* set by the runtime to unwind (unimplemented, fault) */
} Cpu;

typedef void (*GuestFn)(Cpu *);
/* a guest setjmp (recomp.py emits _setjmp(recomp_setjmp_begin(c)->host) at each call to the import): the
 * guest's buffer, the registers a longjmp to it gives back (r4-r11, sp, lr; d8-d15; fpscr) and the host
 * frame to return to (main.c's recomp_longjmp) */
typedef struct RecompJmp { uint32_t guest, r[16], fpscr; uint64_t d[8]; jmp_buf host; } RecompJmp;
extern RecompJmp *recomp_setjmp_begin(Cpu *c);
extern uint8_t *recomp_mem;
/* provided by the host: a call to a guest address that isn't recompiled code (an import) */
extern void recomp_call_external(Cpu *c, uint32_t addr);
extern void recomp_unimplemented(Cpu *c, uint32_t addr);
extern GuestFn recomp_lookup(uint32_t addr);
extern const char *recomp_import_name(uint32_t addr);   /* the import a stub address stands for */

#define G(a) (recomp_mem + (uint32_t)(a))
static inline uint32_t RD32(uint32_t a) { uint32_t v; memcpy(&v, G(a), 4); return v; }
static inline uint16_t RD16(uint32_t a) { uint16_t v; memcpy(&v, G(a), 2); return v; }
static inline uint8_t RD8(uint32_t a) { return *G(a); }
/* ordered loads (recomp.py SYNC_MN: in lock-free code): no later load is satisfied before this one -- an acquire load
 * (LDAPR on an Apple core, a plain mov on x86); an unaligned address takes a plain load and a load fence */
static inline uint32_t RD32A(uint32_t a) { if (a & 3) { uint32_t v = RD32(a); __atomic_thread_fence(__ATOMIC_ACQUIRE); return v; } return __atomic_load_n((uint32_t *)G(a), __ATOMIC_ACQUIRE); }
static inline uint16_t RD16A(uint32_t a) { if (a & 1) { uint16_t v = RD16(a); __atomic_thread_fence(__ATOMIC_ACQUIRE); return v; } return __atomic_load_n((uint16_t *)G(a), __ATOMIC_ACQUIRE); }
static inline uint8_t RD8A(uint32_t a) { return __atomic_load_n(G(a), __ATOMIC_ACQUIRE); }
static inline void WR32(uint32_t a, uint32_t v) { memcpy(G(a), &v, 4); }
static inline void WR16(uint32_t a, uint16_t v) { memcpy(G(a), &v, 2); }
static inline void WR8(uint32_t a, uint8_t v) { *G(a) = v; }
static inline uint64_t RD64(uint32_t a) { uint64_t v; memcpy(&v, G(a), 8); return v; }
static inline void WR64(uint32_t a, uint64_t v) { memcpy(G(a), &v, 8); }

/* condition codes (capstone's ARM_CC_*: EQ=1 .. LE=14, AL=15) */
static inline int COND(Cpu *c, int cc) {
	switch (cc) {
	case 1: return c->z;               case 2: return !c->z;
	case 3: return c->c;               case 4: return !c->c;
	case 5: return c->n;               case 6: return !c->n;
	case 7: return c->v;               case 8: return !c->v;
	case 9: return c->c && !c->z;      case 10: return !c->c || c->z;
	case 11: return c->n == c->v;      case 12: return c->n != c->v;
	case 13: return !c->z && c->n == c->v; case 14: return c->z || c->n != c->v;
	default: return 1;
	}
}
static inline void NZ(Cpu *c, uint32_t r) { c->n = r >> 31; c->z = r == 0; }
static inline uint32_t ADDC(Cpu *c, uint32_t a, uint32_t b, uint32_t cin, int setf) {
	uint64_t u = (uint64_t)a + b + cin;
	uint32_t r = (uint32_t)u;
	if (setf) { NZ(c, r); c->c = (uint32_t)(u >> 32); c->v = ((~(a ^ b) & (a ^ r)) >> 31) & 1; }
	return r;
}
/* a - b - !cin, as the ARM ARM's AddWithCarry(a, NOT(b), cin) */
static inline uint32_t SUBC(Cpu *c, uint32_t a, uint32_t b, uint32_t cin, int setf) { return ADDC(c, a, ~b, cin, setf); }

/* shifter: type 1 ASR, 2 LSL, 3 LSR, 4 ROR, 5 RRX (capstone ARM_SFT_*; *_REG variants 6..10
 * take the amount from a register). Sets *carry like the barrel shifter when setc. */
static inline uint32_t SHIFT(Cpu *c, uint32_t v, int type, uint32_t amt, int setc) {
	uint32_t r = v, co = c->c;
	if (type >= 6) type -= 5;
	switch (type) {
	case 2: if (amt == 0) break; if (amt < 32) { co = (v >> (32 - amt)) & 1; r = v << amt; }
	        else { co = amt == 32 ? v & 1 : 0; r = 0; } break;
	case 3: if (amt == 0) break; if (amt < 32) { co = (v >> (amt - 1)) & 1; r = v >> amt; }
	        else { co = amt == 32 ? v >> 31 : 0; r = 0; } break;
	case 1: if (amt == 0) break; if (amt < 32) { co = ((int32_t)v >> (amt - 1)) & 1; r = (uint32_t)((int32_t)v >> amt); }
	        else { co = v >> 31; r = co ? 0xffffffffu : 0; } break;
	case 4: if (amt == 0) break; amt &= 31; if (amt == 0) { co = v >> 31; break; }
	        r = (v >> amt) | (v << (32 - amt)); co = r >> 31; break;
	case 5: co = v & 1; r = (v >> 1) | (c->c << 31); break;
	}
	if (setc) c->c = co;
	return r;
}
/* Thumb-2 modified immediates that rotate set C to bit 31 of the constant when flag-setting */
static inline uint32_t CLZ(uint32_t v) { return v ? __builtin_clz(v) : 32; }
static inline uint32_t BSWAP(uint32_t v) { return __builtin_bswap32(v); }

/* VFP comparison into FPSCR NZCV, as vcmp/vcmpe (NaN: 0011) */
static inline void VCMP(Cpu *c, double a, double b) {
	uint32_t f;
	if (a != a || b != b) f = 0x3; else if (a == b) f = 0x6; else if (a < b) f = 0x8; else f = 0x2;
	c->fpscr = (c->fpscr & 0x0fffffffu) | (f << 28);
}
static inline void VMRS_APSR(Cpu *c) {
	c->n = c->fpscr >> 31; c->z = (c->fpscr >> 30) & 1; c->c = (c->fpscr >> 29) & 1; c->v = (c->fpscr >> 28) & 1;
}
/* float-to-int conversions saturate and truncate (vcvt.s32/u32.f32 round toward zero) */
static inline int32_t F2S(double x) { if (x != x) return 0; if (x >= 2147483647.0) return 0x7fffffff; if (x <= -2147483648.0) return (int32_t)0x80000000; return (int32_t)x; }
static inline uint32_t F2U(double x) { if (x != x || x <= 0) return 0; if (x >= 4294967295.0) return 0xffffffffu; return (uint32_t)x; }

/* exclusive access (ldrex/strex): a per-thread monitor holding the address and the value the
 * load saw; the store is a compare-and-swap against that value (fails if another thread changed it
 * meanwhile, as the hardware monitor would; an ABA change in between goes unnoticed, which the
 * lock-free code compilers emit for atomics tolerates) */
extern __thread uint32_t recomp_excl_addr, recomp_excl_val;
extern __thread int recomp_excl_valid;
static inline uint32_t LDREX(uint32_t a) {
	uint32_t v = __atomic_load_n((uint32_t *)G(a), __ATOMIC_SEQ_CST);
	recomp_excl_addr = a; recomp_excl_val = v; recomp_excl_valid = 1;
	return v;
}
static inline uint32_t STREX(uint32_t a, uint32_t v) {
	if (!recomp_excl_valid || recomp_excl_addr != a) return 1;
	recomp_excl_valid = 0;
	uint32_t expect = recomp_excl_val;
	return __atomic_compare_exchange_n((uint32_t *)G(a), &expect, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
}
static inline void CLREX(void) { recomp_excl_valid = 0; }
/* 64-bit exclusive pair (ldrexd/strexd): the same monitor over a doubleword */
extern __thread uint64_t recomp_excl_val64;
static inline uint64_t LDREXD(uint32_t a) {
	uint64_t v = __atomic_load_n((uint64_t *)G(a), __ATOMIC_SEQ_CST);
	recomp_excl_addr = a; recomp_excl_val64 = v; recomp_excl_valid = 2;
	return v;
}
static inline uint32_t STREXD(uint32_t a, uint64_t v) {
	if (recomp_excl_valid != 2 || recomp_excl_addr != a) return 1;
	recomp_excl_valid = 0;
	uint64_t expect = recomp_excl_val64;
	return __atomic_compare_exchange_n((uint64_t *)G(a), &expect, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
}
/* byte and halfword exclusives (ldrexb/strexb, ldrexh/strexh): the same monitor over 1 or 2 bytes */
#define RECOMP_EXCL_NARROW(N, T, K) \
static inline uint32_t LDREX##N(uint32_t a) { \
	T v = __atomic_load_n((T *)G(a), __ATOMIC_SEQ_CST); \
	recomp_excl_addr = a; recomp_excl_val = v; recomp_excl_valid = K; \
	return v; \
} \
static inline uint32_t STREX##N(uint32_t a, uint32_t v) { \
	if (recomp_excl_valid != K || recomp_excl_addr != a) return 1; \
	recomp_excl_valid = 0; \
	T expect = (T)recomp_excl_val; \
	return __atomic_compare_exchange_n((T *)G(a), &expect, (T)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1; \
}
RECOMP_EXCL_NARROW(B, uint8_t, 3)
RECOMP_EXCL_NARROW(H, uint16_t, 4)
/* NEON floating point always runs in the "standard FPSCR" mode: denormals flushed to zero */
static inline float FZF(float x) {   /* and NaNs become the default NaN (NEON's DN) */
	uint32_t b; memcpy(&b, &x, 4);
	if (!(b & 0x7f800000u)) b &= 0x80000000u;
	else if ((b & 0x7f800000u) == 0x7f800000u && (b & 0x7fffff)) b = 0x7fc00000u;
	memcpy(&x, &b, 4); return x;
}
static inline float B2F(uint32_t b) { float x; memcpy(&x, &b, 4); return x; }
static inline uint32_t F2B(float x) { uint32_t b; memcpy(&b, &x, 4); return b; }
/* VRSQRTE.F32: the ARMv7 ARM's FPRSqrtEstimate (bit-exact) */
static inline uint32_t RSQRTE_F32(uint32_t b) {
	uint32_t sign = b & 0x80000000u, exp = (b >> 23) & 0xff, frac = b & 0x7fffff;
	if (exp == 0xff && frac) return 0x7fc00000u;               /* NaN */
	if (exp == 0) return sign | 0x7f800000u;                    /* zero, flushed denormal: inf */
	if (sign) return 0x7fc00000u;                               /* negative: default NaN */
	if (exp == 0xff) return 0;                                  /* +inf: +0 */
	double a;   /* the operand's significand scaled into [0.25, 1) by the exponent's parity */
	uint64_t frac64 = (uint64_t)frac << 29;
	uint64_t bits = ((exp & 1) ? 0x3fd0000000000000ull : 0x3fe0000000000000ull) | frac64;
	memcpy(&a, &bits, 8);
	double r;
	if (a < 0.5) { double q = (double)(int)(a * 512.0); r = 1.0 / sqrt((q + 0.5) / 512.0); }
	else { double q = (double)(int)(a * 256.0); r = 1.0 / sqrt((q + 0.5) / 256.0); }
	uint32_t e = (uint32_t)(int)(256.0 * r + 0.5);              /* 256..511 */
	uint32_t result_exp = (380 - exp) / 2;
	return (result_exp << 23) | ((e & 0xff) << 15);
}
/* VRSQRTS.F32: (3 - a*b) / 2, inf*0 gives 1.5 */
static inline float RSQRTS_F32(float a, float b) {
	a = FZF(a); b = FZF(b);
	if ((isinf(a) && b == 0) || (isinf(b) && a == 0)) return 1.5f;
	return FZF(FZF(3.0f - FZF(a * b)) / 2.0f);
}
/* thread-local storage: TPIDRURO (mrc p15, 0, rX, c13, c0, 3), set per guest thread by the host */
extern __thread uint32_t recomp_tpidruro;
/* IEEE half precision (FPSCR.AHP = 0): to single exactly; from single rounding to nearest even,
 * overflow to infinity; a NaN becomes the default NaN either way (the console runs FPSCR.DN = 1,
 * and NEON always does: insnsweep.py found the payload carried through) */
static inline uint32_t H2F(uint16_t h) {
	uint32_t sign = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 31, m = h & 0x3ff;
	if (e == 31) return m ? 0x7fc00000u : sign | 0x7f800000u;
	if (e == 0) {
		if (!m) return sign;
		while (!(m & 0x400)) { m <<= 1; e--; }   /* subnormal half: normalise */
		m &= 0x3ff; e++;
	}
	return sign | ((e + 112) << 23) | (m << 13);
}
static inline uint16_t F2H(uint32_t f) {
	uint32_t sign = (f >> 16) & 0x8000, e = (f >> 23) & 0xff, m = f & 0x7fffff;
	if (e == 0xff) return m ? 0x7e00 : (uint16_t)(sign | 0x7c00);
	int ne = (int)e - 112;
	if (ne >= 31) return (uint16_t)(sign | 0x7c00);
	if (ne <= 0) {                                /* subnormal or zero half */
		if (ne < -10) return (uint16_t)sign;
		m |= 0x800000;
		uint32_t shift = (uint32_t)(14 - ne), hm = m >> shift, rem = m & ((1u << shift) - 1), half = 1u << (shift - 1);
		if (rem > half || (rem == half && (hm & 1))) hm++;
		return (uint16_t)(sign | hm);
	}
	uint32_t hm = m >> 13, rem = m & 0x1fff;
	uint32_t r = sign | ((uint32_t)ne << 10) | hm;
	if (rem > 0x1000 || (rem == 0x1000 && (hm & 1))) r++;   /* may carry into the exponent: correct */
	return (uint16_t)r;
}
/* VRECPE.F32: the ARMv7 ARM's FPRecipEstimate (a 9-bit table-free estimate, bit-exact) */
static inline uint32_t RECPE_F32(uint32_t b) {
	uint32_t sign = b & 0x80000000u, exp = (b >> 23) & 0xff, frac = b & 0x7fffff;
	if (exp == 0xff && frac) return 0x7fc00000u;               /* NaN -> default NaN */
	if (exp == 0) return sign | 0x7f800000u;                    /* zero, flushed denormal -> inf */
	if (exp == 0xff) return sign;                               /* inf -> zero */
	if (exp >= 253) return sign;                                /* |x| >= 2^126 -> zero (flushed) */
	uint32_t q = 256 + (frac >> 15);                            /* a = q/512 in [0.5, 1) */
	double r = 1.0 / (((double)q + 0.5) / 512.0);
	uint32_t e = (uint32_t)(256.0 * r + 0.5);                   /* estimate = e/256 in [1, 2) */
	return sign | ((253 - exp) << 23) | ((e & 0xff) << 15);
}
/* VRECPS.F32: 2 - a*b, two roundings, inf*0 gives 2 */
static inline float RECPS_F32(float a, float b) {
	a = FZF(a); b = FZF(b);
	if ((isinf(a) && b == 0) || (isinf(b) && a == 0)) return 2.0f;
	return FZF(2.0f - FZF(a * b));
}
/* NEON sub-lanes: byte b (0..3) and halfword h (0..1) of a 32-bit word, and the words packed back
 * from elements (each truncated to its width); recomp.py's narrow-lane emitter computes every
 * element of a result as a uint32_t and packs them per word */
#define LANE8(w, b) ((uint32_t)(uint8_t)((w) >> (8 * (b))))
#define LANE16(w, h) ((uint32_t)(uint16_t)((w) >> (16 * (h))))
#define PACK8(a, b, c, d) ((uint32_t)(uint8_t)(a) | (uint32_t)(uint8_t)(b) << 8 | (uint32_t)(uint8_t)(c) << 16 | (uint32_t)(uint8_t)(d) << 24)
#define PACK16(a, b) ((uint32_t)(uint16_t)(a) | (uint32_t)(uint16_t)(b) << 16)
/* saturation to a bits-wide signed/unsigned integer (vqadd/vqsub/vqshl; the Q flag isn't modelled) */
static inline int64_t SATS(int64_t v, int bits) { int64_t mx = ((int64_t)1 << (bits - 1)) - 1; return v > mx ? mx : v < -mx - 1 ? -mx - 1 : v; }
static inline uint64_t SATU(int64_t v, int bits) { int64_t mx = ((int64_t)1 << bits) - 1; return v < 0 ? 0 : v > mx ? (uint64_t)mx : (uint64_t)v; }
/* VSHL/VQSHL by register: the shift is the signed low byte of the other operand's element, negative
 * shifts right (logical/arithmetic by sign), results truncated or saturated to the element */
static inline uint32_t SHLR_U(uint32_t v, int sh, int bits) { return sh >= bits || sh <= -bits ? 0 : sh >= 0 ? v << sh : v >> -sh; }
static inline uint32_t SHLR_S(int32_t v, int sh, int bits) { return sh >= bits ? 0 : sh <= -bits ? (uint32_t)(v >> 31) : sh >= 0 ? (uint32_t)v << sh : (uint32_t)(v >> -sh); }
static inline uint64_t QSHLU(uint64_t v, int sh, int bits) {
	uint64_t mx = bits == 64 ? ~0ull : (1ull << bits) - 1;
	if (sh <= -64) return 0;
	if (sh < 0) return v >> -sh;
	if (sh >= 64) return v ? mx : 0;
	return v > (mx >> sh) ? mx : v << sh;
}
static inline int64_t QSHLS(int64_t v, int sh, int bits) {
	int64_t mx = bits == 64 ? INT64_MAX : ((int64_t)1 << (bits - 1)) - 1, mn = -mx - 1;
	if (sh <= -64) return v < 0 ? -1 : 0;
	if (sh < 0) return v >> -sh;
	if (sh >= bits) return v > 0 ? mx : v < 0 ? mn : 0;   /* (below that, mn >> sh is exact) */
	return v > (mx >> sh) ? mx : v < (mn >> sh) ? mn : (int64_t)((uint64_t)v << sh);
}
/* rounding shift right by 1..64 (vrshr/vrsra/vraddhn's RoundingShiftRight: floor((v + 2^(n-1)) / 2^n)
 * in infinite precision, which is the truncating shift plus the last bit shifted out) */
static inline uint64_t RSHR64_U(uint64_t v, int n) { return n >= 64 ? v >> 63 : (v >> n) + ((v >> (n - 1)) & 1); }
static inline int64_t RSHR64_S(int64_t v, int n) { return n >= 64 ? 0 : (v >> n) + ((v >> (n - 1)) & 1); }
/* NEON VMAX/VMIN.F32 (FPMax/FPMin with the standard FPSCR): any NaN gives the default NaN; equal
 * values (the two zeros) take the sign that wins (+0 for max, -0 for min); denormals flushed */
static inline uint32_t FMAX_F32(uint32_t a, uint32_t b) {
	float x = FZF(B2F(a)), y = FZF(B2F(b));
	if (x != x || y != y) return 0x7fc00000u;
	if (x == y) return F2B(x) & F2B(y);
	return F2B(x > y ? x : y);
}
static inline uint32_t FMIN_F32(uint32_t a, uint32_t b) {
	float x = FZF(B2F(a)), y = FZF(B2F(b));
	if (x != x || y != y) return 0x7fc00000u;
	if (x == y) return F2B(x) | F2B(y);
	return F2B(x < y ? x : y);
}
/* RBIT: the 32 bits reversed */
static inline uint32_t RBIT(uint32_t v) {
	v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
	v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
	v = ((v >> 4) & 0x0f0f0f0fu) | ((v & 0x0f0f0f0fu) << 4);
	return __builtin_bswap32(v);
}
/* -DRECOMP_TRACE: every guest instruction reports the state before it runs (difftest's lockstep mode) */
#ifdef RECOMP_TRACE
extern void recomp_trace(Cpu *c, uint32_t addr);
#define TR(a) recomp_trace(c, a)
#else
#define TR(a) ((void)0)
#endif
/* -DRECOMP_VERIFY: every function entry reports itself (the host's in-place checker, which runs
 * sampled calls both natively and as the original code in Unicorn, from the game's real state) */
#ifdef RECOMP_VERIFY
extern void recomp_enter(Cpu *c, uint32_t addr);
#define ENTER(c, a) recomp_enter(c, a)
#else
#define ENTER(c, a) ((void)0)
#endif
#define UNIMPL(c, a) do { recomp_unimplemented(c, a); return; } while (0)
#endif
