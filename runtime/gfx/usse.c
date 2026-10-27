/*
 * USSE decoder and interpreter. See usse.h and docs/usse-isa.md (our own spec; written from scratch,
 * Vita3K used only as a reference to resolve behavioural doubts, no code copied).
 *
 * Layout of this file:
 *   1. bit helpers, the FPCONSTANT ROM, swizzle tables
 *   2. GXP header/parameter/container/literal/dependent-sampler parsing
 *   3. instruction decode (one function per §3 group) -> a flat UOp array, decoded once per program
 *   4. the program cache (usse_get)
 *   5. register access (banks, doubling, FPINTERNAL aliasing, repeat stepping)
 *   6. the interpreter core (usse_run_secondary/usse_run_primary)
 *   7. per-draw register setup, vertex attribute / fragment iterator packing, colour output
 *   8. the disassembler (usse_dis.c's engine)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "hle.h"     /* guest_alloc; brings in rt.h (G/RD32/.../recomp_mem) */
#include "usse.h"
#include "usse_internal.h"   /* UOp/UOperand + the enums below: shared with usse_spirv.c */

/* ---------------------------------------------------------------- 1. bit helpers / tables */
#define BITS(w, hi, lo) (uint32_t)(((w) >> (lo)) & ((((uint64_t)1) << ((hi) - (lo) + 1)) - 1))
#define BIT(w, n) (uint32_t)(((w) >> (n)) & 1)

/* UOperand, UOp, the bank/type/op/repeat-mode/VDUAL enums and the FPCONSTANT ROM (usse_fpconst)
 * now live in usse_internal.h, shared with usse_spirv.c's translator */
#define fpconst usse_fpconst

/* §2.8b: 4-bit table swizzle. swiz[] filled with channel codes 0-7 (x,y,z,w,0,1,2,H). */
static void decode_vec34(int code, int ext, int kind /*0 vec4,1 vec3,2 scalar*/, uint8_t out[4]) {
	static const char *vec4_0[16] = {"xxxx","yyyy","zzzz","wwww","xyzw","yzww","xyzz","xxyz","xyxy","xywz","zxyw","zwzw","yzxz","xxyy","xzww","xyz1"};
	static const char *vec4_1[16] = {"yzxw","zwxy","xzwy","yyww","wyzw","wzwz","xyzx","zzww","xwzx","yyyx","yyyz","xzyw","xxxy","zyxw","yyzz","zzzy"};
	static const char *vec3_0[16] = {"xxx","yyy","zzz","www","xyz","yzw","xxy","xyx","yyx","yyz","zxy","xzy","yzx","zyx","zzy","xy1"};
	static const char *vec3_1[11] = {"xyy","yxy","xxz","yxx","xy0","x10","000","111","HHH","222","x00"};
	static const char *scalar_t[8] = {"xxxx","yyyy","zzzz","wwww","0000","1111","2222","HHHH"};
	const char *s = "xxxx";
	if (kind == 2) s = (code >= 0 && code < 8) ? scalar_t[code] : "xxxx";
	else if (kind == 1) { if (!ext && code < 16) s = vec3_0[code]; else if (ext && code < 11) s = vec3_1[code]; else s = "xxx1"; }
	else { if (!ext && code < 16) s = vec4_0[code]; else if (ext && code < 16) s = vec4_1[code]; else s = "xyzw"; }
	for (int i = 0; i < 4; i++) {
		char c = s[i < (int)strlen(s) ? i : (int)strlen(s) - 1];
		out[i] = c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : c == 'w' ? 3 : c == '0' ? 4 : c == '1' ? 5 : c == '2' ? 6 : c == 'H' ? 7 : 0;
	}
	if (kind == 1) out[3] = out[0];   /* vec3: w forced to x (of the 3-swizzle result) */
}
/* §2.8c: VDUAL's own table */
static void decode_dual(int code, int ext, int vec4, uint8_t out[4]) {
	static const char *v4_0[16] = {"xxxx","yyyy","zzzz","wwww","xyzw","yzww","xyzz","xxyz","xyxy","xywz","zxyw","zwzw","0000","HHHH","1111","2222"};
	static const char *v4_1[16] = {"yzxw","zwxy","xzwy","yyww","wyzw","wzwz","xyzx","zzww","xwzx","yyyx","yyyz","zwzw","yzxz","xxyy","xzww","xyz1"};
	static const char *v3_0[16] = {"xxx","yyy","zzz","www","xyz","yzw","xxy","xyx","yyx","yyz","zxy","xzy","000","HHH","111","222"};
	static const char *v3_1[11] = {"xyy","yxy","xxz","yxx","xy0","x10","xzy","yzx","zyx","zzy","xy1"};
	const char *s = "xyzw";
	if (vec4) s = (!ext) ? v4_0[code & 15] : v4_1[code & 15];
	else s = (!ext) ? v3_0[code & 15] : (code < 11 ? v3_1[code] : "xyy");
	for (int i = 0; i < 4; i++) {
		char c = s[i < (int)strlen(s) ? i : (int)strlen(s) - 1];
		out[i] = c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : c == 'w' ? 3 : c == '0' ? 4 : c == '1' ? 5 : c == '2' ? 6 : c == 'H' ? 7 : 0;
	}
	if (!vec4) out[3] = out[0];
}

/* ---------------------------------------------------------------- 2. GXP parsing */
/* header field offsets (§1.1) */
enum {
	H_FLAGS = 20, H_BUFFER_FLAGS = 24, H_PARAM_COUNT = 36, H_PARAM_OFF = 40, H_VARY_OFF = 44,
	H_PRIM_REGS = 48, H_SEC_REGS = 50, H_TEMP_REGS1 = 52, H_TEMP_REGS2 = 56,
	H_PRIM_INSTR_COUNT = 60, H_PRIM_OFF = 64, H_SEC_INSTR_COUNT = 68, H_SEC_OFF = 72, H_SEC_OFF_END = 76,
	H_LITERAL_SIZE = 88, H_DEFAULT_UNIFORMS = 100, H_LITERAL_DATA_OFF = 104,
	H_LITERALS_COUNT = 112, H_LITERALS_OFF = 116, H_UBINFO_COUNT = 120, H_UBINFO_OFF = 124,
	H_DEPSAMP_COUNT = 128, H_DEPSAMP_OFF = 132, H_CONTAINER_COUNT = 144, H_CONTAINER_OFF = 148,
};
static uint32_t h_param_base(uint32_t p) { return RD32(p + H_PARAM_OFF) ? p + H_PARAM_OFF + RD32(p + H_PARAM_OFF) : 0; }
static uint32_t param_at(uint32_t base, int i) { return base + 16 * i; }
/* a container's (base_sa_offset, size_in_f32), or -1 if that container_index is absent */
static int find_container(uint32_t p, int idx, int *base_sa, int *size_f32) {
	uint32_t n = RD32(p + H_CONTAINER_COUNT), base = RD32(p + H_CONTAINER_OFF) ? p + H_CONTAINER_OFF + RD32(p + H_CONTAINER_OFF) : 0;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t c = base + 8 * i;
		if (RD16(c) == (uint16_t)idx) { *base_sa = RD16(c + 4); *size_f32 = RD16(c + 6); return 1; }
	}
	return 0;
}

/* ---------------------------------------------------------------- 3. instruction decode */
/* bank lookup tables, §2.2 */
static int bank_dst2(int b2, int ext) { static const int t[4][2] = {{BK_TEMP,BK_SA},{BK_O,BK_SPECIAL},{BK_PA,BK_IDX},{BK_IDXD1,BK_IDXD2}}; return t[b2 & 3][ext & 1]; }
static int bank_src0(int b1, int ext) { static const int t[2][2] = {{BK_TEMP,BK_O},{BK_PA,BK_SA}}; return t[b1 & 1][ext & 1]; }
static int bank_src12(int b2, int ext) { static const int t[4][2] = {{BK_TEMP,BK_IDXD1},{BK_O,BK_SPECIAL},{BK_PA,BK_IMM},{BK_SA,BK_IDXD2}}; return t[b2 & 3][ext & 1]; }

/* register-number decode: bank+raw field -> final (bank,num), §2.3. width = original field width
 * (6 or 7 bits) before doubling; doubled = this operand addresses 64-bit units. */
static void set_operand(UOperand *o, int bank, uint32_t field, int width, int doubled, int type, int secondary) {
	int num = (int)field;
	if (bank == BK_TEMP) {
		int top = (1 << width) - 4;
		if (num >= top) { o->bank = BK_FPI; o->num = (uint16_t)(num - top); o->type = (uint8_t)type; return; }
	}
	if (doubled && bank != BK_SPECIAL && bank != BK_IMM) num <<= 1;
	if (bank == BK_SPECIAL) { if (field & 0x40) { bank = BK_GLOBAL; num = field & 0x3F; } else { bank = BK_FPCONST; num = (int)field; } }
	if (secondary && (bank == BK_TEMP || bank == BK_PA || bank == BK_O || bank == BK_SA || bank == BK_IDX || bank == BK_GLOBAL)) bank = BK_SA;
	o->bank = (uint8_t)bank; o->num = (uint16_t)num; o->type = (uint8_t)type;
}
static void str_to_swiz(const char *s, uint8_t out[4]) {
	int n = (int)strlen(s);
	for (int i = 0; i < 4; i++) {
		char c = s[i < n ? i : n - 1];
		out[i] = c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : c == 'w' ? 3 : c == '0' ? 4 : c == '1' ? 5 : c == '2' ? 6 : c == 'H' ? 7 : 0;
	}
}
static void identity_swiz(uint8_t out[4]) { out[0] = 0; out[1] = 1; out[2] = 2; out[3] = 3; }
static void apply_modcode(UOperand *o, int code) { o->negabs = (uint8_t)((code & 1 ? 1 : 0) | (code & 2 ? 2 : 0)); }   /* 0 none,1 neg,2 abs,3 neg(abs) */

static void set_pred_ext(UOp *op, int v) { static const uint8_t k[8] = {0,1,1,1,1,2,2,0}, r[8] = {0,0,1,2,3,0,1,0}; op->pred_kind = k[v & 7]; op->pred_reg = r[v & 7]; }
static void set_pred_extvec(UOp *op, int v) { static const uint8_t k[8] = {0,1,1,1,2,2,2,0}, r[8] = {0,0,1,2,0,1,2,0}; op->pred_kind = k[v & 7]; op->pred_reg = r[v & 7]; }
static void set_pred_short(UOp *op, int v) { static const uint8_t k[4] = {0,1,2,2}; op->pred_kind = k[v & 3]; op->pred_reg = 0; }
static void set_pred_sv(UOp *op, int v) { static const uint8_t k[4] = {0,1,2,0}; op->pred_kind = k[v & 3]; op->pred_reg = 0; }

/* §2.9: write-mask rewrite for F32/F16/C10 destinations in a 64-bit-unit bank */
static int mask_rewrite(int type, int bank, int mask4) {
	if (bank == BK_FPI) return mask4;
	if (type == TY_F32) return mask4 & 3;
	if (type == TY_F16) return (mask4 & 1 ? 3 : 0) | (mask4 & 4 ? 0xC : 0);
	return mask4;   /* C10, and anything else: as is */
}

/* ---- VMAD2 (§3.1) */
static void dec_vmad2(uint64_t w, int sec, UOp *op) {
	op->op = OP_VMAD2;
	int dat_fmt = BIT(w, 58), pv = BITS(w, 57, 56);
	op->pred_kind = pv ? 1 : 0; op->pred_reg = pv ? (int)pv - 1 : 0;
	int type = dat_fmt ? TY_F16 : TY_F32;
	int src0_bank = BIT(w, 34), dest_bank2 = BITS(w, 33, 32), src1_bank2 = BITS(w, 31, 30), src2_bank2 = BITS(w, 29, 28);
	int dest_n = BITS(w, 27, 22), src0_n = BITS(w, 17, 12), src1_n = BITS(w, 11, 6), src2_n = BITS(w, 5, 0);
	set_operand(&op->dst, bank_dst2(dest_bank2, 0), dest_n, 6, 1, type, sec);
	set_operand(&op->s0, bank_src0(src0_bank, 0), src0_n, 6, 1, type, sec);
	set_operand(&op->s1, bank_src12(src1_bank2, BIT(w, 49)), src1_n, 6, 1, type, sec);
	set_operand(&op->s2, bank_src12(src2_bank2, BIT(w, 48)), src2_n, 6, 1, type, sec);
	int sc0 = (BIT(w, 53) << 2) | BITS(w, 19, 18), sc1 = (BIT(w, 44) << 2) | BITS(w, 21, 20), sc2 = BITS(w, 47, 45);
	static const char *t0[8] = {"xxxx","yyyy","zzzz","wwww","xyzw","yzxw","xyww","zwxy"};
	static const char *t1[8] = {"xxxx","yyyy","zzzz","wwww","xyzw","xyyz","yyww","wyzw"};
	static const char *t2[8] = {"xxxx","yyyy","zzzz","wwww","xyzw","xzww","xxyz","xyzz"};
	str_to_swiz(t0[sc0 & 7], op->s0.swiz); str_to_swiz(t1[sc1 & 7], op->s1.swiz); str_to_swiz(t2[sc2 & 7], op->s2.swiz);
	if (BIT(w, 50)) op->s0.negabs |= 2;   /* src0_abs */
	apply_modcode(&op->s1, BITS(w, 38, 37)); apply_modcode(&op->s2, BITS(w, 36, 35));
	op->mask = (uint8_t)mask_rewrite(type, op->dst.bank, BITS(w, 42, 39));
	op->reps = 1; op->repmode = RM_EXTERNAL;
}

/* ---- V32NMAD / V16NMAD (§3.2) */
static void dec_vnmad(uint64_t w, int sec, UOp *op) {
	op->op = OP_VNMAD;
	int f16 = BIT(w, 60);   /* op1 bits 63:59: 00001 (bit60=0) -> F32, 00010 (bit60=1) -> F16 */
	int type = f16 ? TY_F16 : TY_F32;
	set_pred_extvec(op, BITS(w, 58, 56));
	int dest_b = BITS(w, 33, 32), src1_b = BITS(w, 31, 30), src2_b = BITS(w, 29, 28);
	set_operand(&op->dst, bank_dst2(dest_b, BIT(w, 51)), BITS(w, 27, 22), 6, 1, type, sec);
	set_operand(&op->s1, bank_src12(src1_b, BIT(w, 49)), BITS(w, 11, 6), 6, 1, type, sec);
	set_operand(&op->s2, bank_src12(src2_b, BIT(w, 48)), BITS(w, 5, 0), 6, 1, type, sec);
	op->s0.bank = BK_NONE;
	uint32_t sw12 = BITS(w, 21, 15) | (BITS(w, 35, 34) << 7) | (BIT(w, 50) << 9) | (BITS(w, 54, 53) << 10);
	for (int i = 0; i < 4; i++) op->s1.swiz[i] = (uint8_t)((sw12 >> (3 * i)) & 7);
	decode_vec34((int)BITS(w, 47, 44), 0, 0, op->s2.swiz);
	apply_modcode(&op->s1, BITS(w, 38, 37));
	if (BIT(w, 36)) op->s2.negabs |= 2;
	op->mask = (uint8_t)BITS(w, 42, 39);   /* used directly, not rewritten */
	op->sub = (uint8_t)BITS(w, 14, 12);    /* op2: MUL/ADD/FRC/DSX/DSY/MIN/MAX/DP */
	op->reps = 1; op->repmode = RM_EXTERNAL;
}

/* ---- VMAD (GPI form, §3.3) ---- */
static void dec_vmad(uint64_t w, int sec, UOp *op) {
	op->op = OP_VMAD;
	set_pred_ext(op, (int)BITS(w, 58, 56));
	int kind = BIT(w, 52) ? 0 : 1;   /* opcode2: 1 VMAD4(vec4,kind0) else VMAD3(vec3,kind1) */
	set_operand(&op->dst, bank_dst2((int)BITS(w, 33, 32), BIT(w, 51)), BITS(w, 27, 22), 6, 1, TY_F32, sec);
	set_operand(&op->s1, bank_src12((int)BITS(w, 31, 30), BIT(w, 49)), BITS(w, 5, 0), 6, 1, TY_F32, sec);
	op->s0.bank = BK_FPI; op->s0.num = (uint16_t)BITS(w, 29, 28); op->s0.type = TY_F32;
	op->s2.bank = BK_FPI; op->s2.num = (uint16_t)BITS(w, 13, 12); op->s2.type = TY_F32;
	decode_vec34((int)BITS(w, 21, 18), BIT(w, 34), kind, op->s0.swiz);
	decode_vec34((int)BITS(w, 17, 14), BIT(w, 54), kind, op->s2.swiz);
	decode_vec34((int)BITS(w, 9, 6), BIT(w, 10), kind, op->s1.swiz);
	if (BIT(w, 46)) op->s0.negabs |= 2; if (BIT(w, 11)) op->s0.negabs |= 1;
	if (BIT(w, 37)) op->s1.negabs |= 2; if (BIT(w, 38)) op->s1.negabs |= 1;
	if (BIT(w, 35)) op->s2.negabs |= 2; if (BIT(w, 36)) op->s2.negabs |= 1;
	op->mask = (uint8_t)BITS(w, 42, 39);
	op->repmode = (uint8_t)BITS(w, 48, 47); op->reps = (uint8_t)(BITS(w, 45, 44) + 1);
	op->sub = 0xAA;   /* marker: GPI-form mad, mult (2,2,2,4) */
}

/* ---- VDP (§3.4) ---- */
static void dec_vdp(uint64_t w, int sec, UOp *op) {
	op->op = OP_VDP;
	set_pred_ext(op, (int)BITS(w, 58, 56));
	int kind = BIT(w, 52) ? 0 : 1;   /* opcode2: VDP4/VDP3 */
	op->sub2 = (uint8_t)(kind == 0 ? 4 : 3);
	set_operand(&op->dst, bank_dst2((int)BITS(w, 33, 32), BIT(w, 51)), BITS(w, 27, 22), 6, 1, TY_F32, sec);
	set_operand(&op->s1, bank_src12((int)BITS(w, 31, 30), BIT(w, 49)), BITS(w, 5, 0), 6, 1, TY_F32, sec);
	op->s2.bank = BK_FPI; op->s2.num = (uint16_t)BITS(w, 29, 28); op->s2.type = TY_F32;
	decode_vec34((int)BITS(w, 21, 18), 0, kind, op->s2.swiz);
	op->s1.swiz[0] = (uint8_t)BITS(w, 8, 6); op->s1.swiz[1] = (uint8_t)BITS(w, 11, 9);
	op->s1.swiz[2] = (uint8_t)BITS(w, 14, 12); op->s1.swiz[3] = (uint8_t)BITS(w, 17, 15);
	if (BIT(w, 37)) op->s1.negabs |= 2; if (BIT(w, 38)) op->s1.negabs |= 1;
	if (BIT(w, 46)) op->s2.negabs |= 2;
	op->mask = (uint8_t)BITS(w, 42, 39);
	op->repmode = (uint8_t)BITS(w, 48, 47); op->reps = (uint8_t)(BITS(w, 45, 44) + 1);
	op->s0.bank = BK_NONE;
}

/* ---- VDUAL (§3.5): two independent F32/F16 ops issued together, with too many distinct operand
 * roles (US, I0, I1, I2, two destinations) to fit UOp's fixed src/dst slots. Decode only the
 * predicate here; exec_vdual() (§6) re-derives everything else straight from the raw word, per
 * docs/usse-isa.md's tables. sub3 bit2 carries whether this instruction sits in a secondary program. */
static void dec_vdual(uint64_t w, int sec, UOp *op) {
	op->op = OP_VDUAL;
	set_pred_sv(op, (int)BITS(w, 57, 56));
	op->sub3 = (uint8_t)(sec ? 4 : 0);
	op->reps = 1; op->repmode = RM_EXTERNAL;
	op->raw = w;
}

/* ---- VCOMP (§3.6) ---- */
static void dec_vcomp(uint64_t w, int sec, UOp *op) {
	op->op = OP_VCOMP;
	set_pred_ext(op, (int)BITS(w, 58, 56));
	int dty[4] = {TY_F32, TY_F16, TY_C10, TY_F32}, sty[4] = {TY_F32, TY_F16, TY_C10, TY_F32};
	int dt = dty[BITS(w, 54, 53) & 3], st = sty[BITS(w, 40, 39) & 3];
	set_operand(&op->dst, bank_dst2((int)BITS(w, 33, 32), BIT(w, 51)), BITS(w, 27, 21), 7, 1, dt, sec);
	set_operand(&op->s1, bank_src12((int)BITS(w, 31, 30), BIT(w, 49)), BITS(w, 13, 7), 7, 1, st, sec);
	op->sub = (uint8_t)BITS(w, 42, 41);         /* op2: RCP/RSQ/LOG/EXP */
	op->sub2 = (uint8_t)BITS(w, 36, 35);        /* src_comp */
	apply_modcode(&op->s1, BITS(w, 38, 37));
	identity_swiz(op->s1.swiz);
	op->mask = (uint8_t)BITS(w, 3, 0);
	op->reps = (uint8_t)(BITS(w, 47, 44) + 1); op->repmode = RM_SLMSI;
	op->s0.bank = BK_NONE;
}

/* ---- VMOV / VMOVC / VMOVCU8 (§3.7) ---- */
static void dec_vmov(uint64_t w, int sec, UOp *op) {
	op->op = OP_VMOV;
	set_pred_ext(op, (int)BITS(w, 58, 56));
	int move_type = (int)BITS(w, 47, 46);
	int mdt = (int)BITS(w, 42, 40);
	static const uint8_t totype[8] = {TY_S8, TY_S16, TY_S32, TY_C10, TY_F16, TY_F32, TY_U8, TY_U16};
	int type = totype[mdt & 7];
	int isfloat = (mdt == 3 || mdt == 4 || mdt == 5);   /* C10, F16, F32 */
	int doubled = isfloat;
	set_operand(&op->dst, bank_dst2((int)BITS(w, 33, 32), BIT(w, 51)), BITS(w, 23, 18), 6, doubled, type, sec);
	set_operand(&op->s1, bank_src12((int)BITS(w, 31, 30), BIT(w, 49)), BITS(w, 11, 6), 6, doubled, type, sec);
	set_operand(&op->s2, bank_src12((int)BITS(w, 29, 28), BIT(w, 48)), BITS(w, 5, 0), 6, doubled, type, sec);
	set_operand(&op->s0, bank_src0(BIT(w, 34), BIT(w, 50)), BITS(w, 17, 12), 6, doubled, type, sec);
	if (isfloat) { decode_vec34((int)BITS(w, 38, 35), 0, 0, op->s1.swiz); memcpy(op->s2.swiz, op->s1.swiz, 4); }
	else { identity_swiz(op->s1.swiz); identity_swiz(op->s2.swiz); }
	if (BIT(w, 53)) memcpy(op->s0.swiz, op->s1.swiz, 4); else identity_swiz(op->s0.swiz);
	int mask4 = (int)BITS(w, 27, 24);
	op->mask = isfloat ? (uint8_t)mask_rewrite(type, op->dst.bank, mask4) : 1;
	op->sub = (uint8_t)move_type;
	op->sub2 = (uint8_t)((BIT(w, 54) << 1) | BIT(w, 39));   /* cmp selector test_bit_2:test_bit_1 */
	op->sub3 = (uint8_t)mdt;
	op->reps = (uint8_t)(BITS(w, 45, 44) + 1); op->repmode = RM_SLMSI;
}

/* ---- VPCK (§3.8): formats U8,S8,O8,U16,S16,F16,F32,C10. Component selection implemented as a
 * direct (non-compacted) mapping -- the common case per §3.8's own exception list; see docs/usse-isa.md. */
static void dec_vpck(uint64_t w, int sec, UOp *op) {
	op->op = OP_VPCK;
	set_pred_ext(op, (int)BITS(w, 58, 56));
	int src_fmt = (int)BITS(w, 43, 41), dest_fmt = (int)BITS(w, 40, 38);
	static const uint8_t totype[8] = {TY_U8, TY_S8, TY_O8, TY_U16, TY_S16, TY_F16, TY_F32, TY_C10};
	int st = totype[src_fmt], dt = totype[dest_fmt];
	int src_float = (st == TY_F16 || st == TY_F32 || st == TY_C10);
	set_operand(&op->dst, bank_dst2((int)BITS(w, 33, 32), BIT(w, 51)), BITS(w, 27, 21), 7, 0, dt, sec);
	uint32_t src1n = src_float ? BITS(w, 13, 8) : ((BITS(w, 13, 8) << 1) | BIT(w, 7));
	set_operand(&op->s1, bank_src12((int)BITS(w, 31, 30), BIT(w, 49)), src1n, src_float ? 6 : 7, src_float, st, sec);
	set_operand(&op->s2, bank_src12((int)BITS(w, 29, 28), BIT(w, 48)), BITS(w, 6, 1), 6, 1, st, sec);
	int cs0b1 = (src_fmt == 6 /*F32*/) ? (int)BIT(w, 7) : (int)(BITS(w, 6, 1) & 1);
	uint8_t comp_sel[4] = {(uint8_t)(BIT(w, 0) | (cs0b1 << 1)), (uint8_t)BITS(w, 17, 16), (uint8_t)BITS(w, 15, 14), (uint8_t)BITS(w, 20, 19)};
	op->sub = (uint8_t)BIT(w, 18);   /* scale */
	op->mask = (uint8_t)BITS(w, 37, 34);
	/* The component selects are per WRITTEN lane, in mask order: a pack into .zw with selects
	 * (2,3,..) takes the source's z and w -- read per absolute lane it took x for both (Unit 13's
	 * G-buffer normals came out x,y,x,x). Exceptions where the selects are per absolute lane:
	 * float-to-float conversions, and scaled packs involving U8; a U8 pack of all four lanes
	 * repeats selects 0,1 (the behaviour Vita3K established empirically). */
	{
		int dst_float = (dt == TY_F16 || dt == TY_F32 || dt == TY_C10), u8 = (st == TY_U8 || dt == TY_U8);
		int no_cycle = (src_float && dst_float) || (op->sub && u8);
		if (!no_cycle) {
			uint8_t tmp[4]; memcpy(tmp, comp_sel, 4); int k = 0;
			for (int i = 0; i < 4; i++) if (op->mask & (1 << i)) comp_sel[i] = (u8 && op->mask == 0xF) ? tmp[i % 2] : tmp[k++];
		}
	}
	memcpy(op->s1.swiz, comp_sel, 4); memcpy(op->s2.swiz, comp_sel, 4);
	op->reps = (uint8_t)(BITS(w, 47, 44) + 1); op->repmode = RM_SLMSI;
	op->s0.bank = BK_NONE;
}

/* ---- VTST / VTSTMSK (§3.9) ---- */
static int alu_type_for(int alu_sel, int prec) {
	if (alu_sel == 0) return prec ? TY_F32 : TY_F16;
	if (alu_sel == 2) return TY_U8;
	return TY_U32;
}
static void dec_vtst_common(uint64_t w, int sec, UOp *op, int is_msk) {
	op->op = is_msk ? OP_VTSTMSK : OP_VTST;
	set_pred_ext(op, (int)BITS(w, 58, 56));
	int alu_sel = (int)BITS(w, 19, 18), alu_op = (int)BITS(w, 17, 14), prec = (int)BIT(w, 47);
	int type = alu_type_for(alu_sel, prec);
	int doubled = (alu_sel == 0);
	set_operand(&op->s1, bank_src12((int)BITS(w, 31, 30), BIT(w, 49)), BITS(w, 13, 7), 7, doubled, type, sec);
	set_operand(&op->s2, bank_src12((int)BITS(w, 29, 28), BIT(w, 48)), BITS(w, 6, 0), 7, doubled, type, sec);
	identity_swiz(op->s1.swiz); identity_swiz(op->s2.swiz);
	if (BIT(w, 46) && alu_sel == 0) { op->s2.swiz[1] = op->s2.swiz[2] = op->s2.swiz[3] = op->s2.swiz[0]; }   /* src2_vscomp: broadcast x */
	if (!is_msk && BIT(w, 50)) op->s1.negabs |= 1;      /* VTST src1_neg */
	if (is_msk && BIT(w, 50)) op->s1.negabs |= 1;        /* VTSTMSK test_flag_2 reuses the slot */
	op->sub = (uint8_t)alu_sel; op->sub2 = (uint8_t)alu_op;
	op->sub3 = (uint8_t)(((int)BITS(w, 43, 42) << 2) | (int)BITS(w, 41, 40) | (BIT(w, 39) << 4));   /* sign_test:zero_test:crcomb */
	if (!is_msk) { op->dst.bank = BK_NONE; op->dst.num = (uint16_t)BITS(w, 35, 34); op->imm = (int32_t)BITS(w, 38, 36); }
	else { set_operand(&op->dst, bank_dst2((int)BITS(w, 33, 32), BIT(w, 51)), BITS(w, 27, 21), 7, doubled, type, sec); op->imm = (int32_t)BITS(w, 37, 36); }
	op->reps = 1; op->repmode = RM_EXTERNAL;
}
static void dec_vtst(uint64_t w, int sec, UOp *op) { dec_vtst_common(w, sec, op, 0); }
static void dec_vtstmsk(uint64_t w, int sec, UOp *op) { dec_vtst_common(w, sec, op, 1); }

/* ---- VBW (§3.10) ---- */
static void dec_vbw(uint64_t w, int sec, UOp *op) {
	op->op = OP_VBW;
	set_pred_ext(op, (int)BITS(w, 58, 56));
	int op1 = (int)BITS(w, 61, 59), op2 = (int)BIT(w, 35);
	set_operand(&op->dst, bank_dst2((int)BITS(w, 33, 32), BIT(w, 51)), BITS(w, 27, 21), 7, 0, TY_U32, sec);
	set_operand(&op->s1, bank_src12((int)BITS(w, 31, 30), BIT(w, 49)), BITS(w, 13, 7), 7, 0, TY_U32, sec);
	set_operand(&op->s2, bank_src12((int)BITS(w, 29, 28), BIT(w, 48)), BITS(w, 6, 0), 7, 0, TY_U32, sec);
	identity_swiz(op->s1.swiz); identity_swiz(op->s2.swiz); identity_swiz(op->dst.swiz);
	op->sub = (uint8_t)op1; op->sub2 = (uint8_t)op2;
	op->sub3 = (uint8_t)BIT(w, 34);   /* bitwise_partial: 16-bit op */
	op->imm = (int32_t)(BITS(w, 20, 14) /*src2_sel, for immediates*/ | 0);
	op->args[0] = (int32_t)BITS(w, 42, 38);   /* src2_rot */
	op->args[1] = (int32_t)BIT(w, 43);          /* src2_invert */
	op->args[2] = (int32_t)BITS(w, 37, 36);     /* src2_exth */
	op->mask = 0xF;
	op->s0.bank = BK_NONE;
	op->reps = 1; op->repmode = RM_EXTERNAL;
}

/* ---- SOP2 / SOP2M / SOP3 (§3.11): U8 RGBA blend-style sum of products ---- */
static void dec_sop2(uint64_t w, int sec, UOp *op) {
	op->op = OP_SOP2;
	set_pred_short(op, (int)BITS(w, 58, 57));
	set_operand(&op->dst, bank_dst2((int)BITS(w, 33, 32), BIT(w, 51)), BITS(w, 27, 21), 7, 0, TY_U8, sec);
	set_operand(&op->s1, bank_src12((int)BITS(w, 31, 30), BIT(w, 49)), BITS(w, 13, 7), 7, 0, TY_U8, sec);
	set_operand(&op->s2, bank_src12((int)BITS(w, 29, 28), BIT(w, 48)), BITS(w, 6, 0), 7, 0, TY_U8, sec);
	if (BIT(w, 20)) op->s1.negabs |= 1;   /* src1_mod: complement */
	op->sub = (uint8_t)BITS(w, 19, 18);     /* cop */
	op->sub2 = (uint8_t)BITS(w, 17, 16);    /* aop */
	op->args[0] = (int32_t)BITS(w, 40, 38); op->args[1] = (int32_t)BITS(w, 37, 35);   /* csel1, csel2 */
	op->args[2] = (int32_t)BITS(w, 53, 52); op->args[3] = (int32_t)BITS(w, 42, 41);   /* asel1, asel2 */
	if (BIT(w, 56)) op->args[0] |= 0x100; if (BIT(w, 47)) op->args[1] |= 0x100;    /* cmod1/cmod2 */
	if (BIT(w, 43)) op->args[2] |= 0x100; if (BIT(w, 34)) op->args[3] |= 0x100;    /* amod1/amod2 */
	op->reps = (uint8_t)(BITS(w, 46, 44) + 1); op->repmode = RM_SLMSI;
	op->mask = 0xF; op->s0.bank = BK_NONE;
}
static void dec_sop2m(uint64_t w, int sec, UOp *op) {
	op->op = OP_SOP2M;
	set_pred_short(op, (int)BITS(w, 58, 57));
	set_operand(&op->dst, bank_dst2((int)BITS(w, 33, 32), BIT(w, 51)), BITS(w, 27, 21), 7, 0, TY_U8, sec);
	set_operand(&op->s1, bank_src12((int)BITS(w, 31, 30), BIT(w, 49)), BITS(w, 13, 7), 7, 0, TY_U8, sec);
	set_operand(&op->s2, bank_src12((int)BITS(w, 29, 28), BIT(w, 48)), BITS(w, 6, 0), 7, 0, TY_U8, sec);
	op->sub = (uint8_t)BITS(w, 53, 52); op->sub2 = (uint8_t)BITS(w, 42, 41);
	op->args[0] = (int32_t)BITS(w, 40, 38) | (BIT(w, 56) ? 0x100 : 0);
	op->args[1] = (int32_t)BITS(w, 37, 35) | (BIT(w, 47) ? 0x100 : 0);
	int wm = (int)BITS(w, 46, 43);
	op->mask = (uint8_t)(((wm >> 1) & 7) | ((wm & 1) << 3));
	op->s0.bank = BK_NONE; op->reps = 1; op->repmode = RM_EXTERNAL;
}
static void dec_sop3(uint64_t w, int sec, UOp *op) {
	op->op = OP_SOP3;
	set_pred_short(op, (int)BITS(w, 58, 57));
	set_operand(&op->dst, bank_dst2((int)BITS(w, 33, 32), BIT(w, 51)), BITS(w, 27, 21), 7, 0, TY_U8, sec);
	set_operand(&op->s0, bank_src0(BIT(w, 34), 0), BITS(w, 20, 14), 7, 0, TY_U8, sec);
	set_operand(&op->s1, bank_src12((int)BITS(w, 31, 30), BIT(w, 49)), BITS(w, 13, 7), 7, 0, TY_U8, sec);
	set_operand(&op->s2, bank_src12((int)BITS(w, 29, 28), BIT(w, 48)), BITS(w, 6, 0), 7, 0, TY_U8, sec);
	op->sub = (uint8_t)BITS(w, 53, 52); op->sub2 = (uint8_t)BITS(w, 42, 41);
	op->args[0] = (int32_t)BITS(w, 40, 38) | (BIT(w, 56) ? 0x100 : 0);
	op->args[1] = (int32_t)BITS(w, 37, 35) | (BIT(w, 47) ? 0x100 : 0);
	op->args[2] = (int32_t)BITS(w, 45, 44) | (BIT(w, 46) ? 0x100 : 0);   /* asel1 + amod1 */
	op->mask = 0xF; op->reps = 1; op->repmode = RM_EXTERNAL;
}

/* ---- Integer MAD family (§3.12) ---- */
static void dec_i8mad(uint64_t w, int sec, UOp *op) {
	op->op = OP_I8MAD;
	set_pred_short(op, (int)BITS(w, 58, 57));
	set_operand(&op->dst, bank_dst2((int)BITS(w, 33, 32), BIT(w, 51)), BITS(w, 27, 21), 7, 0, TY_U8, sec);
	set_operand(&op->s0, bank_src0(BIT(w, 34), 0), BITS(w, 20, 14), 7, 0, TY_U8, sec);
	set_operand(&op->s1, bank_src12((int)BITS(w, 31, 30), BIT(w, 49)), BITS(w, 13, 7), 7, 0, TY_U8, sec);
	set_operand(&op->s2, bank_src12((int)BITS(w, 29, 28), BIT(w, 48)), BITS(w, 6, 0), 7, 0, TY_U8, sec);
	if (BIT(w, 35)) op->s0.negabs |= 1;
	op->sub = (uint8_t)BITS(w, 53, 52);           /* csel0 */
	op->sub2 = (uint8_t)(BIT(w, 41) | (BIT(w, 37) << 1) | (BIT(w, 36) << 2));   /* asel0,csel1,csel2 */
	op->reps = (uint8_t)(BITS(w, 46, 44) + 1); op->repmode = RM_SLMSI;
	op->mask = 0xF;
}
static void dec_i16mad(uint64_t w, int sec, UOp *op) {
	op->op = OP_I16MAD;
	set_pred_short(op, (int)BITS(w, 58, 57));
	set_operand(&op->dst, bank_dst2((int)BITS(w, 33, 32), BIT(w, 51)), BITS(w, 27, 21), 7, 0, TY_U16, sec);
	set_operand(&op->s0, bank_src0(BIT(w, 34), 0), BITS(w, 20, 14), 7, 0, TY_U16, sec);
	set_operand(&op->s1, bank_src12((int)BITS(w, 31, 30), BIT(w, 49)), BITS(w, 13, 7), 7, 0, TY_U16, sec);
	set_operand(&op->s2, bank_src12((int)BITS(w, 29, 28), BIT(w, 48)), BITS(w, 6, 0), 7, 0, TY_U16, sec);
	op->sub = (uint8_t)BITS(w, 43, 42);    /* mode: bit1 signed, bit0 saturate */
	op->sub2 = (uint8_t)(BITS(w, 41, 40) | (BITS(w, 39, 38) << 2));   /* src2_format, src1_format */
	op->args[0] = BIT(w, 52); op->args[1] = BIT(w, 37);   /* sel1h_upper8, sel2h_upper8 */
	op->reps = (uint8_t)(BITS(w, 46, 44) + 1); op->repmode = RM_SLMSI;
	op->mask = 1;
}
static void dec_i32mad(uint64_t w, int sec, UOp *op) {
	op->op = OP_I32MAD;
	set_pred_short(op, (int)BITS(w, 58, 57));
	set_operand(&op->dst, bank_dst2((int)BITS(w, 33, 32), BIT(w, 51)), BITS(w, 27, 21), 7, 0, TY_S32, sec);
	set_operand(&op->s0, bank_src0(BIT(w, 34), 0), BITS(w, 20, 14), 7, 0, TY_S32, sec);
	set_operand(&op->s1, bank_src12((int)BITS(w, 31, 30), BIT(w, 49)), BITS(w, 13, 7), 7, 0, TY_S32, sec);
	set_operand(&op->s2, bank_src12((int)BITS(w, 29, 28), BIT(w, 48)), BITS(w, 6, 0), 7, 0, TY_S32, sec);
	op->sub = (uint8_t)BIT(w, 43);              /* is_signed */
	op->sub2 = (uint8_t)BITS(w, 39, 38);         /* src2_type: 2 = full 32-bit */
	op->args[0] = BIT(w, 56); op->args[1] = BIT(w, 53); op->args[2] = BIT(w, 52);   /* src0_high,src1_high,src2_high */
	op->reps = (uint8_t)(BITS(w, 46, 44) + 1); op->repmode = RM_SLMSI;
	op->mask = 1;
}
static void dec_i32mad2(uint64_t w, int sec, UOp *op) {
	op->op = OP_I32MAD2;
	set_pred_ext(op, (int)BITS(w, 58, 56));
	set_operand(&op->dst, bank_dst2((int)BITS(w, 33, 32), BIT(w, 51)), BITS(w, 27, 21), 7, 0, TY_S32, sec);
	set_operand(&op->s0, bank_src0(BIT(w, 34), BIT(w, 47)), BITS(w, 20, 14), 7, 0, TY_S32, sec);
	set_operand(&op->s1, bank_src12((int)BITS(w, 31, 30), BIT(w, 49)), BITS(w, 13, 7), 7, 0, TY_S32, sec);
	set_operand(&op->s2, bank_src12((int)BITS(w, 29, 28), BIT(w, 48)), BITS(w, 6, 0), 7, 0, TY_S32, sec);
	op->sub = (uint8_t)BIT(w, 41);      /* is_signed */
	if (BIT(w, 40)) op->s1.negabs |= 1; if (BIT(w, 39)) op->s2.negabs |= 1;
	op->sub2 = (uint8_t)BITS(w, 53, 52);  /* sn: 0 normal, else "pass src2 through" (uncertain) */
	op->reps = (uint8_t)(BITS(w, 46, 44) + 1); op->repmode = RM_SLMSI;
	op->mask = 1;
}

/* ---- SMP: texture sample (§3.13) ---- */
static void dec_smp(uint64_t w, int sec, UOp *op) {
	op->op = OP_SMP;
	set_pred_ext(op, (int)BITS(w, 58, 56));
	int src0_type = (int)BITS(w, 36, 35);
	static const uint8_t totype[4] = {TY_F32, TY_F16, TY_C10, TY_F32};
	int st = totype[src0_type & 3];
	set_operand(&op->s0, bank_src0(BIT(w, 34), BIT(w, 50)), BITS(w, 20, 14), 7, 1, st, sec);
	set_operand(&op->s1, bank_src12((int)BITS(w, 31, 30), BIT(w, 49)), BITS(w, 13, 7), 7, 1, TY_U32, sec);
	set_operand(&op->s2, bank_src12((int)BITS(w, 29, 28), BIT(w, 48)), BITS(w, 6, 0), 7, 1, st, sec);
	identity_swiz(op->s0.swiz); identity_swiz(op->s1.swiz); identity_swiz(op->s2.swiz);
	int dest_bank = BIT(w, 39) ? BK_PA : BK_TEMP;
	if (sec && dest_bank == BK_PA) dest_bank = BK_SA;
	op->dst.bank = (uint8_t)dest_bank; op->dst.num = (uint16_t)BITS(w, 27, 21); op->dst.type = TY_F32; identity_swiz(op->dst.swiz);
	op->sub = (uint8_t)BITS(w, 43, 42);     /* dim: 0 1D,1 2D,2 3D/cube */
	op->sub2 = (uint8_t)BITS(w, 41, 40);    /* lod_mode */
	op->sub3 = (uint8_t)BITS(w, 47, 46);    /* fconv_type */
	op->args[0] = (int32_t)BITS(w, 38, 37); /* sb_mode */
	op->mask = 0xF; op->reps = 1; op->repmode = RM_EXTERNAL;
}

/* ---- control/special group (op1 = 11111), §3.14-3.21 ---- */
static void dec_br(uint64_t w, UOp *op) {
	op->op = OP_BR;
	set_pred_ext(op, (int)BITS(w, 58, 56));
	int32_t off = (int32_t)BITS(w, 19, 0);
	if (BIT(w, 38) && (off & 0x80000)) off -= 0x100000;   /* br_type sign-extends */
	op->imm = off;
	op->args[0] = (int32_t)BIT(w, 41);   /* save_link */
}
static void dec_smlsi(uint64_t w, UOp *op) {
	/* slots: src0=0, src1=1, src2=2, dest=3 (§2.12); fields are dest_inc(31:24) src0_inc(23:16)
	 * src1_inc(15:8) src2_inc(7:0), modes dest/src0/src1/src2 at bits 35/34/33/32 */
	op->op = OP_SMLSI;
	op->args[0] = (int32_t)(BITS(w, 23, 16) | (BIT(w, 34) << 8));   /* src0 */
	op->args[1] = (int32_t)(BITS(w, 15, 8) | (BIT(w, 33) << 8));    /* src1 */
	op->args[2] = (int32_t)(BITS(w, 7, 0) | (BIT(w, 32) << 8));      /* src2 */
	op->args[3] = (int32_t)(BITS(w, 31, 24) | (BIT(w, 35) << 8));    /* dest */
}
static void dec_smbo(uint64_t w, UOp *op) {
	op->op = OP_SMBO;
	op->args[3] = (int32_t)BITS(w, 47, 36); op->args[0] = (int32_t)BITS(w, 35, 24);
	op->args[1] = (int32_t)BITS(w, 23, 12); op->args[2] = (int32_t)BITS(w, 11, 0);
}
static void dec_kill(uint64_t w, UOp *op) { op->op = OP_KILL; op->imm = (int32_t)BITS(w, 42, 41); }
static void dec_limm(uint64_t w, int sec, UOp *op) {
	op->op = OP_LIMM;
	set_pred_ext(op, (int)BITS(w, 43, 41));
	set_operand(&op->dst, bank_dst2((int)BITS(w, 33, 32), BIT(w, 51)), BITS(w, 27, 21), 7, 0, TY_U32, sec);
	uint32_t v = BITS(w, 20, 0) | (BITS(w, 40, 36) << 21) | (BITS(w, 49, 44) << 26);
	op->imm = (int32_t)v;
}
static void dec_depthf(uint64_t w, int sec, UOp *op) {
	op->op = OP_DEPTHF;
	set_pred_short(op, (int)BITS(w, 42, 41));
	set_operand(&op->s0, bank_src0(BIT(w, 34), BIT(w, 51)), BITS(w, 20, 14), 7, 0, TY_F32, sec);
	identity_swiz(op->s0.swiz);
}

/* ---- LDST: memory load/store (§3.22) ---- */
static void dec_ldst(uint64_t w, int sec, UOp *op) {
	int is_store = (int)BITS(w, 60, 59) == 2;
	op->op = OP_LDST;
	set_pred_ext(op, (int)BITS(w, 58, 56));
	int dt = (int)BITS(w, 37, 36);
	static const uint8_t elemsz[4] = {4, 2, 1, 4};
	op->sub = (uint8_t)dt; op->sub2 = (uint8_t)is_store;
	op->args[3] = elemsz[dt & 3];
	op->reps = (uint8_t)(BITS(w, 47, 44) + 1);
	set_operand(&op->s0, bank_src0(BIT(w, 34), BIT(w, 50)), BITS(w, 20, 14), 7, 0, TY_U32, sec);   /* base address */
	set_operand(&op->s1, bank_src12((int)BITS(w, 31, 30), BIT(w, 49)), BITS(w, 13, 7), 7, 0, TY_U32, sec);   /* offset */
	set_operand(&op->s2, bank_src12((int)BITS(w, 29, 28), BIT(w, 48)), BITS(w, 6, 0), 7, 0, TY_U32, sec);   /* 2nd offset (load) / data (store) */
	int dest_bank = BIT(w, 39) ? BK_PA : BK_TEMP;
	if (sec && dest_bank == BK_PA) dest_bank = BK_SA;
	op->dst.bank = (uint8_t)dest_bank; op->dst.num = (uint16_t)BITS(w, 27, 21); op->dst.type = TY_U32;
	op->args[0] = (int32_t)BIT(w, 53);   /* moe_expand */
	op->repmode = RM_SLMSI;
	op->mask = 0xF;
}

/* ---- top-level dispatch, §3.0's match order ---- */
static UOp decode_one(uint64_t w, int secondary) {
	UOp op; memset(&op, 0, sizeof op);
	op.raw = w; op.reps = 1; op.repmode = RM_EXTERNAL; op.mask = 0xF;
	op.dst.bank = op.s0.bank = op.s1.bank = op.s2.bank = BK_NONE;
	uint32_t op1 = BITS(w, 63, 59);
	if (op1 == 0x00) { dec_vmad2(w, secondary, &op); return op; }
	if (op1 == 0x01 || op1 == 0x02) { dec_vnmad(w, secondary, &op); return op; }
	if (op1 == 0x03) { if (BIT(w, 53)) dec_vmad(w, secondary, &op); else dec_vdp(w, secondary, &op); return op; }
	if (op1 == 0x04 || op1 == 0x05) { dec_vdual(w, secondary, &op); return op; }
	if (op1 == 0x06) { dec_vcomp(w, secondary, &op); return op; }
	if (op1 == 0x07) { dec_vmov(w, secondary, &op); return op; }
	if (op1 == 0x08) { dec_vpck(w, secondary, &op); return op; }
	if (op1 == 0x09) { dec_vtst(w, secondary, &op); return op; }
	if (op1 == 0x0F) { dec_vtstmsk(w, secondary, &op); return op; }
	if (op1 >= 0x0A && op1 <= 0x0E) { dec_vbw(w, secondary, &op); return op; }
	if (op1 == 0x10) { dec_sop2(w, secondary, &op); return op; }
	if (op1 == 0x12) { dec_sop2m(w, secondary, &op); return op; }
	if (op1 == 0x11) { dec_sop3(w, secondary, &op); return op; }
	if (op1 == 0x13) { dec_i8mad(w, secondary, &op); return op; }
	if (op1 == 0x14) { dec_i16mad(w, secondary, &op); return op; }
	if (op1 == 0x15) { dec_i32mad(w, secondary, &op); return op; }
	if (op1 == 0x16 || op1 == 0x17 || op1 == 0x18 || op1 == 0x1B) { op.op = OP_UNSUPPORTED; return op; }   /* illegal */
	if (op1 == 0x19) { op.op = OP_NOP; return op; }   /* I8MAD2: unimplemented, no-op */
	if (op1 == 0x1A) { dec_i32mad2(w, secondary, &op); return op; }
	if (op1 == 0x1C) { dec_smp(w, secondary, &op); return op; }
	if (op1 == 0x1F) {
		uint32_t op2 = BITS(w, 58, 56), opcat = BITS(w, 53, 52), opcat_extra = BIT(w, 54);
		if (opcat == 0 && opcat_extra == 1 && op2 == 2) { op.op = OP_NOP; return op; }   /* PHAS */
		if (opcat == 0 && opcat_extra == 0) {
			uint32_t fc = BITS(w, 40, 38);
			if (fc == 5) { op.op = OP_NOP; return op; }               /* NOP */
			if (fc == 0 || fc == 1) { dec_br(w, &op); return op; }    /* BR */
			if (fc == 2) { op.op = OP_NOP; return op; }               /* return: treated as end-of-program (interpreter stops at instr_count) */
			op.op = OP_UNSUPPORTED; return op;
		}
		if (opcat == 1 && op2 == 2) { dec_smlsi(w, &op); return op; }
		if (opcat == 1 && op2 == 3) { dec_smbo(w, &op); return op; }
		if (opcat == 2 && op2 == 4) { dec_limm(w, secondary, &op); return op; }
		if (opcat == 3 && op2 == 1) { dec_kill(w, &op); return op; }
		if (opcat == 3 && op2 == 3) { dec_depthf(w, secondary, &op); return op; }
		op.op = OP_NOP; return op;   /* SPEC catch-all: no-op per docs/usse-isa.md §3.21 */
	}
	if (op1 == 0x1D || op1 == 0x1E) { dec_ldst(w, secondary, &op); return op; }
	op.op = OP_UNSUPPORTED;
	return op;
}
/* ---------------------------------------------------------------- 5. register access */
static float half_to_float(uint16_t h) {
	uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 0x1F, m = h & 0x3FF, bits;
	if (e == 0) { if (m == 0) bits = s; else { int sh = 0; while (!(m & 0x400)) { m <<= 1; sh++; } m &= 0x3FF; bits = s | ((uint32_t)(127 - 15 - sh + 1) << 23) | (m << 13); } }
	else if (e == 31) bits = s | 0x7F800000u | (m << 13);
	else bits = s | ((e + 112) << 23) | (m << 13);
	float f; memcpy(&f, &bits, 4); return f;
}
static uint16_t float_to_half(float f) {
	uint32_t bits; memcpy(&bits, &f, 4);
	uint32_t s = (bits >> 16) & 0x8000, e = (bits >> 23) & 0xFF, m = bits & 0x7FFFFF;
	if (e == 255) return (uint16_t)(s | 0x7C00 | (m ? 0x200 : 0));
	int32_t ne = (int32_t)e - 127 + 15;
	if (ne >= 31) return (uint16_t)(s | 0x7C00);
	if (ne <= 0) { if (ne < -10) return (uint16_t)s; m = (m | 0x800000) >> (uint32_t)(1 - ne); return (uint16_t)(s | (m >> 13)); }
	return (uint16_t)(s | (uint32_t)(ne << 10) | (m >> 13));
}
static int lanes_per_word(int type) { switch (type) { case TY_F16: case TY_U16: case TY_S16: return 2; case TY_U8: case TY_S8: case TY_O8: return 4; case TY_C10: return 3; default: return 1; } }
static uint32_t extract_lane(uint32_t word, int type, int lane) {
	switch (type) {
	case TY_U8: case TY_O8: return (word >> (8 * lane)) & 0xFF;
	case TY_S8: return (uint32_t)(int32_t)(int8_t)((word >> (8 * lane)) & 0xFF);
	case TY_U16: case TY_F16: return (word >> (16 * lane)) & 0xFFFF;
	case TY_S16: return (uint32_t)(int32_t)(int16_t)((word >> (16 * lane)) & 0xFFFF);
	case TY_C10: { uint32_t b = (word >> (10 * lane)) & 0x3FF; return (b & 0x200) ? (uint32_t)((int32_t)b - 0x400) : b; }
	default: return word;
	}
}
static uint32_t insert_lane(uint32_t word, int type, int lane, uint32_t v) {
	switch (type) {
	case TY_U8: case TY_S8: case TY_O8: { uint32_t m = 0xFFu << (8 * lane); return (word & ~m) | ((v & 0xFFu) << (8 * lane)); }
	case TY_U16: case TY_S16: case TY_F16: { uint32_t m = 0xFFFFu << (16 * lane); return (word & ~m) | ((v & 0xFFFFu) << (16 * lane)); }
	case TY_C10: { uint32_t m = 0x3FFu << (10 * lane); return (word & ~m) | ((v & 0x3FFu) << (10 * lane)); }
	default: return v;
	}
}
static float lane_to_float(uint32_t raw, int type) {
	switch (type) {
	case TY_F32: { float f; memcpy(&f, &raw, 4); return f; }
	case TY_F16: return half_to_float((uint16_t)raw);
	case TY_C10: { float v = (float)(int32_t)raw / 255.0f; return v < -2.0f ? -2.0f : v; }
	case TY_U8: case TY_U16: case TY_U32: case TY_O8: return (float)raw;
	default: return (float)(int32_t)raw;   /* S8, S16, S32: already sign-extended by extract_lane */
	}
}
static float lane_to_float_norm(uint32_t raw, int type) {
	switch (type) {
	case TY_U8: return (float)raw / 255.0f;
	case TY_S8: { float v = (float)(int32_t)raw / 127.0f; return v < -1.0f ? -1.0f : v; }
	case TY_U16: return (float)raw / 65535.0f;
	case TY_S16: { float v = (float)(int32_t)raw / 32767.0f; return v < -1.0f ? -1.0f : v; }
	case TY_U32: return (float)((double)raw / 4294967295.0);
	case TY_S32: { double v = (double)(int32_t)raw / 2147483647.0; return (float)(v < -1.0 ? -1.0 : v); }
	case TY_C10: return lane_to_float(raw, TY_C10);
	default: return lane_to_float(raw, type);   /* float types: normalization is a no-op */
	}
}
uint32_t usse_float_to_lane(float v, int type);
static uint32_t float_to_lane(float v, int type) {
	switch (type) {
	case TY_F32: { uint32_t u; memcpy(&u, &v, 4); return u; }
	case TY_F16: return float_to_half(v);
	case TY_C10: { int32_t i = (int32_t)lrintf(v); if (i > 511) i = 511; if (i < -512) i = -512; return (uint32_t)(i & 0x3FF); }
	case TY_U8: case TY_U16: case TY_U32: case TY_O8: return v <= 0 ? 0u : (uint32_t)v;
	default: return (uint32_t)(int32_t)v;
	}
}
static uint32_t float_to_lane_norm(float v, int type) {
	switch (type) {
	case TY_U8: if (v < 0) v = 0; if (v > 1) v = 1; return (uint32_t)lrintf(v * 255.0f);
	case TY_S8: if (v < -1) v = -1; if (v > 1) v = 1; return (uint32_t)(int32_t)lrintf(v * 127.0f);
	case TY_U16: if (v < 0) v = 0; if (v > 1) v = 1; return (uint32_t)lrintf(v * 65535.0f);
	case TY_S16: if (v < -1) v = -1; if (v > 1) v = 1; return (uint32_t)(int32_t)lrintf(v * 32767.0f);
	case TY_U32: if (v < 0) v = 0; if (v > 1) v = 1; return (uint32_t)((double)v * 4294967295.0);
	case TY_S32: if (v < -1) v = -1; if (v > 1) v = 1; return (uint32_t)(int32_t)((double)v * 2147483647.0);
	case TY_C10: if (v < -2) v = -2; if (v > 2) v = 2; return (uint32_t)(int32_t)lrintf(v * 255.0f) & 0x3FF;
	default: return float_to_lane(v, type);
	}
}
static float const_as_float(int ch) { switch (ch) { case 4: return 0.0f; case 5: return 1.0f; case 6: return 2.0f; default: return 0.5f; } }
static int32_t const_as_int(int ch) { switch (ch) { case 5: return 1; case 6: return 2; default: return 0; } }

struct UsseProgram {
	uint32_t addr;
	uint32_t hash, size;   /* of the GXP bytes at addr: the decode is shared between addresses holding the same program and redone when the bytes at an address change */
	int is_fragment, supported;
	int pa_words;          /* vertex: PA words the program can read (header's primary register count, the attribute slots, the index/instance registers), rounded up to 4 -- the GPU backend's per-vertex record */
	UOp *sec, *prim; int nsec, nprim;
	UsseVertexLayout vl;
	UsseFragLayout fl;
	uint32_t literal_addr, literal_words;   /* container 16's data, for the uniform-buffer-address table */
	int8_t additive;   /* usse_fragment_fb_blend's answer + 1, once asked (0: not yet) */
	struct UsseProgram *next, *hnext;   /* the address table's chain; the content table's chain (decoded entries only) */
};
typedef struct {
	UsseInst *t; UsseDraw *d; int secondary;
	int8_t inc[4][17];
	void *texctx; UsseTexFn texfn;
	/* destination writes staged during an instruction and committed after it (per repeat): the
	 * hardware reads every source lane before writing any, so "x = x * y" vector ops must not see
	 * their own earlier lanes (Unit 13's occlusion multiply wrote lane 0 into the texel register it
	 * was reading, and lanes 1-2 came out scaled by lane 0's result) */
	int staging, cur_rep, nst;
	struct { uint8_t bank; uint16_t num; uint32_t word; } st[64];
} Ctx;

static int g_trace_force;   /* usse_set_trace */
static uint32_t bank_read_raw(Ctx *cx, int bank, int num) {
	switch (bank) {
	case BK_TEMP: return cx->t->r[(unsigned)num % 160].u;
	case BK_PA: return cx->t->pa[(unsigned)num % 160].u;
	case BK_O: return cx->t->o[(unsigned)num % 160].u;
	case BK_SA: return cx->d->sa[(unsigned)num % 256].u;
	case BK_FPI: return cx->t->i[(unsigned)num % 16].u;
	case BK_IDX: return num == 1 ? cx->t->idx[0] : num == 2 ? cx->t->idx[1] : 0;
	default: return 0;   /* GLOBAL and anything else: reads as 0 */
	}
}
static void bank_write_raw(Ctx *cx, int bank, int num, uint32_t v) {
	switch (bank) {
	case BK_TEMP: cx->t->r[(unsigned)num % 160].u = v; return;
	case BK_PA: cx->t->pa[(unsigned)num % 160].u = v; return;
	case BK_O: cx->t->o[(unsigned)num % 160].u = v; return;
	case BK_SA: cx->d->sa[(unsigned)num % 256].u = v; return;
	case BK_FPI: cx->t->i[(unsigned)num % 16].u = v; return;
	case BK_IDX: if (num == 1) cx->t->idx[0] = v; else if (num == 2) cx->t->idx[1] = v; return;
	default: return;
	}
}
/* §2.4: INDEXED1/2 operand number splits into a base bank (2 bits) and offset (5 bits); the
 * effective word is idxN*2 + offset (+ repeat offset, added by the caller). */
static void resolve_indexed(Ctx *cx, int which, int raw7, int *out_bank, int *out_num) {
	static const int basebanks[4] = {BK_TEMP, BK_O, BK_PA, BK_SA};
	int base = basebanks[(raw7 >> 5) & 3], k = raw7 & 0x1F;
	uint32_t idxv = which == 1 ? cx->t->idx[0] : cx->t->idx[1];
	*out_bank = base; *out_num = (int)(idxv * 2 + (uint32_t)k);
}
typedef struct { int kind; int bank; int base; } Addr;   /* kind: 0 normal bank, 1 fpconst, 2 immediate, 3 none */
static Addr resolve_addr(Ctx *cx, const UOperand *o, int repeat_off) {
	Addr a = {3, 0, 0};
	int bank = o->bank;
	if (bank == BK_NONE || bank == BK_GLOBAL) return a;
	if (bank == BK_FPCONST) { a.kind = 1; a.base = o->num; return a; }
	if (bank == BK_IMM) { a.kind = 2; a.base = o->num; return a; }
	if (bank == BK_IDXD1 || bank == BK_IDXD2) {
		int rb, rn; resolve_indexed(cx, bank == BK_IDXD1 ? 1 : 2, o->num, &rb, &rn);
		a.kind = 0; a.bank = rb; a.base = rn + repeat_off; return a;
	}
	a.kind = 0; a.bank = bank;
	/* FPINTERNAL: num/repeat_off are in REGISTER units (register k = words 4k..4k+3, §2.1); every
	 * other bank addresses words directly. */
	a.base = bank == BK_FPI ? ((int)o->num + repeat_off) * 4 : (int)o->num + repeat_off;
	return a;
}
/* read one component (post-swizzle) as a float, for the float ALU instructions */
static float read_f32(Ctx *cx, const UOperand *o, int repeat_off, int comp) {
	int ch = o->swiz[comp & 3];
	float v;
	if (ch >= 4) v = const_as_float(ch);
	else {
		Addr a = resolve_addr(cx, o, repeat_off);
		if (a.kind == 2) v = (float)(int32_t)o->num;
		else if (a.kind == 1) {
			if (o->type == TY_F16) { uint32_t word = fpconst[a.base & 63][ch < 2 ? 0 : 1]; v = half_to_float((uint16_t)(word >> (16 * (ch & 1)))); }
			else { uint32_t raw = fpconst[a.base & 63][ch & 1]; memcpy(&v, &raw, 4); }
		} else if (a.kind == 3) v = 0;
		else if (a.bank == BK_FPI && (o->type == TY_F16 || o->type == TY_F32)) { uint32_t raw = bank_read_raw(cx, a.bank, a.base + ch); memcpy(&v, &raw, 4); }   /* internal registers: one F32 per word whatever the operand's type (fpi_widened) */
		else { int lpw = lanes_per_word(o->type); int word = a.base + ch / lpw, lane = ch % lpw; v = lane_to_float(extract_lane(bank_read_raw(cx, a.bank, word), o->type, lane), o->type); }
	}
	if (o->negabs & 2) v = fabsf(v);
	if (o->negabs & 1) v = -v;
	return v;
}
/* read one component as a raw bit/integer lane, for integer/VMOV/VPCK/VBW/SOP/LDST */
static uint32_t read_i32(Ctx *cx, const UOperand *o, int repeat_off, int comp) {
	int ch = o->swiz[comp & 3];
	uint32_t v;
	if (ch >= 4) v = (uint32_t)const_as_int(ch);
	else {
		Addr a = resolve_addr(cx, o, repeat_off);
		if (a.kind == 2) v = o->num;
		else if (a.kind == 1) { int lpw = lanes_per_word(o->type); v = extract_lane(fpconst[a.base & 63][(ch / lpw) & 1], o->type, ch % lpw); }   /* the constant's lane in the operand's type (read_f32's word rule): an F16 or U8 lane used to come back as the whole 32-bit word */
		else if (a.kind == 3) v = 0;
		else if (a.bank == BK_FPI && o->type == TY_F16) { uint32_t raw = bank_read_raw(cx, a.bank, a.base + ch); float f; memcpy(&f, &raw, 4); v = float_to_lane(f, TY_F16); }   /* fpi_widened: the F32 word as the half the operand asks for */
		else { int lpw = lanes_per_word(o->type); int word = a.base + ch / lpw, lane = ch % lpw; v = extract_lane(bank_read_raw(cx, a.bank, word), o->type, lane); }
	}
	if (o->negabs & 2) v = (v & 0x80000000u) ? (uint32_t)(-(int32_t)v) : v;
	if (o->negabs & 1) v = (uint32_t)(-(int32_t)v);
	return v;
}
static int stage_get(Ctx *cx, int bank, int num, uint32_t *word) {
	for (int i = 0; i < cx->nst; i++) if (cx->st[i].bank == bank && cx->st[i].num == num) { *word = cx->st[i].word; return 1; }
	return 0;
}
static void stage_put(Ctx *cx, int bank, int num, uint32_t word) {
	for (int i = 0; i < cx->nst; i++) if (cx->st[i].bank == bank && cx->st[i].num == num) { cx->st[i].word = word; return; }
	if (cx->nst < 64) { cx->st[cx->nst].bank = (uint8_t)bank; cx->st[cx->nst].num = (uint16_t)num; cx->st[cx->nst].word = word; cx->nst++; }
	else bank_write_raw(cx, bank, num, word);   /* (never: an instruction writes a few words) */
}
static void stage_commit(Ctx *cx) {
	for (int i = 0; i < cx->nst; i++) bank_write_raw(cx, cx->st[i].bank, cx->st[i].num, cx->st[i].word);
	cx->nst = 0;
}
/* fpi_widened (§2.1, docs/usse-isa.md note 4): the internal registers i0-i3 are 128 bits wide and hold one
 * F32 per word whatever the instruction's data type -- an F16 write lands as the widened float in
 * its own word, an F16 read converts the word back. The interpreter used to pack F16 pairs into
 * them as into every other bank, so an F16 VNMAD into i0 followed by an F32 VCOMP from i0.x (the
 * contrast/gamma chain of Killzone: Mercenary's text shader, 16-17) read two halves as one float:
 * every glyph's alpha came out at -3.6 and only the outline taps showed. VPCK's "wide" rule below
 * was the first sighting of the same width (Unit 13's depth-to-position pass). */
static void write_raw_lane(Ctx *cx, const UOperand *o, int repeat_off, int comp, uint32_t rawval) {
	Addr a = resolve_addr(cx, o, repeat_off);
	if (a.kind != 0) return;
	if (a.bank == BK_FPI && o->type == TY_F16) {
		float f = half_to_float((uint16_t)rawval); uint32_t bits; memcpy(&bits, &f, 4);
		if (!cx->staging) bank_write_raw(cx, a.bank, a.base + comp, bits); else stage_put(cx, a.bank, a.base + comp, bits);
		return;
	}
	int lpw = lanes_per_word(o->type); int word = a.base + comp / lpw, lane = comp % lpw;
	uint32_t old;
	if (!cx->staging) { old = bank_read_raw(cx, a.bank, word); bank_write_raw(cx, a.bank, word, insert_lane(old, o->type, lane, rawval)); return; }
	if (!stage_get(cx, a.bank, word, &old)) old = bank_read_raw(cx, a.bank, word);
	stage_put(cx, a.bank, word, insert_lane(old, o->type, lane, rawval));
}
static void write_f32(Ctx *cx, const UOperand *o, int repeat_off, int comp, float val) { write_raw_lane(cx, o, repeat_off, comp, float_to_lane(val, o->type)); }

static int in_mult_set(int bank) { return bank == BK_TEMP || bank == BK_PA || bank == BK_O || bank == BK_SA || bank == BK_IDXD1 || bank == BK_IDXD2; }
static int32_t slot_offset(Ctx *cx, int mode, int bank, int slot, int r, const int mult[4]) {
	if (r != cx->cur_rep) { stage_commit(cx); cx->cur_rep = r; }   /* a new repeat: the previous one's writes land first */
	int is_fpi = (bank == BK_FPI);
	switch (mode) {
	case RM_EXTERNAL: return is_fpi ? 0 : 4 * r;
	case RM_INTERNAL: return is_fpi ? r : 0;
	case RM_BOTH: return is_fpi ? r : 4 * r;
	default: { int rr = r < 0 ? 0 : r > 16 ? 16 : r; int v = cx->inc[slot & 3][rr]; return in_mult_set(bank) ? v * mult[slot & 3] : v; }
	}
}
/* §2.12: an operand encoded as a gpi<n> field (VMAD's gpi0/gpi1, VDP's gpi0) is not an SMLSI slot: it steps only in
 * INTERNAL and BOTH mode, by one internal register a repeat, and stays put in EXTERNAL and SLMSI mode (Vita3K
 * agrees). Until this, SLMSI mode stepped it by inc[slot][r] like a bank operand -- Unit 13's sniper-scope lens
 * vertex program (SMLSI dest +1 src0 +1 src1 -2 src2 -2, then VDP3 rpt3 O18 = dot(SA50/46/42, i2)) read i0 (the
 * view vector) in place of i2 (the binormal) on the second repeat, and i(-2) on the third: the lens's world
 * binormal.y and normal.z came out as garbage, and the Fresnel vignette (1 - pow(1 - max(0, n.v), 3)) closed the
 * whole lens whenever the aim left the heading where the garbage happened to be harmless. */
static int gpi_offset(int mode, int r) { return (mode == RM_INTERNAL || mode == RM_BOTH) ? r : 0; }
static void get_mult(const UOp *op, int mult[4]) {
	mult[0] = mult[1] = mult[2] = mult[3] = 2;
	if (op->op == OP_VMAD) mult[3] = 4;
	else if (op->op == OP_VMOV) { int f = (op->sub3 == 4 || op->sub3 == 5); mult[0] = mult[1] = mult[2] = mult[3] = f ? 2 : 1; }
	else if (op->op == OP_VPCK) { int sf = (op->s1.type == TY_F16 || op->s1.type == TY_F32 || op->s1.type == TY_C10), df = (op->dst.type == TY_F16 || op->dst.type == TY_F32 || op->dst.type == TY_C10); /* repeats step each operand in its own register size: a float source's 6-bit field (and src2's, always) counts 64-bit registers -- two words a step -- the 7-bit destination 32-bit ones. Float-to-float packs used to step their sources one word: Uncharted's cutscene character lights pack three F32 colours with one VPCK repeated three times (SMLSI -4/-2), the 2nd and 3rd read the wrong words, an F32 1.9e9 became an F16 infinity and every lit character pixel went NaN (black) */
		if (sf || df) { mult[0] = 1; mult[1] = mult[2] = 2; mult[3] = 1; } else { mult[0] = mult[1] = mult[2] = mult[3] = 1; } }
	else if (op->op == OP_LDST || op->op == OP_VCOMP || op->op == OP_SOP2 || op->op == OP_SOP2M || op->op == OP_SOP3 ||
	         op->op == OP_I8MAD || op->op == OP_I16MAD || op->op == OP_I32MAD || op->op == OP_I32MAD2)
		mult[0] = mult[1] = mult[2] = mult[3] = 1;
}
/* ---------------------------------------------------------------- 6. the interpreter core */
static int pred_true(Ctx *cx, const UOp *op) {
	if (op->pred_kind == 0) return 1;
	int v = cx->t->p[op->pred_reg & 3];
	return op->pred_kind == 1 ? v != 0 : v == 0;
}
static void exec_vmad2(Ctx *cx, const UOp *op) {
	int mult[4] = {2, 2, 2, 2};
	int off0 = slot_offset(cx, op->repmode, op->s0.bank, 0, 0, mult), off1 = slot_offset(cx, op->repmode, op->s1.bank, 1, 0, mult);
	int off2 = slot_offset(cx, op->repmode, op->s2.bank, 2, 0, mult), offd = slot_offset(cx, op->repmode, op->dst.bank, 3, 0, mult);
	for (int c = 0; c < 4; c++) if (op->mask & (1 << c))
		write_f32(cx, &op->dst, offd, c, read_f32(cx, &op->s0, off0, c) * read_f32(cx, &op->s1, off1, c) + read_f32(cx, &op->s2, off2, c));
}
static void exec_vnmad(Ctx *cx, const UOp *op) {
	static int warned_dsxy;
	for (int c = 0; c < 4; c++) if (op->mask & (1 << c)) {
		float a = read_f32(cx, &op->s1, 0, c), b = read_f32(cx, &op->s2, 0, c), r;
		switch (op->sub) {
		case 0: r = a * b; break;
		case 1: r = a + b; break;
		case 2: r = a - floorf(b); break;
		case 3: case 4: if (!warned_dsxy) { warned_dsxy++; fprintf(stderr, "usse: VDSX/VDSY approximated as 0 (no fragment-quad derivatives)\n"); } r = 0; break;
		case 5: r = a < b ? a : b; break;
		case 6: r = a > b ? a : b; break;
		case 7: { float d = 0; for (int k = 0; k < 4; k++) d += read_f32(cx, &op->s1, 0, k) * read_f32(cx, &op->s2, 0, k); r = d; break; }
		default: r = a + b; break;
		}
		write_f32(cx, &op->dst, 0, c, r);
	}
}
static void exec_vmad(Ctx *cx, const UOp *op) {
	int mult[4] = {2, 2, 2, 4};
	for (int r = 0; r < op->reps; r++) {
		int off1 = slot_offset(cx, op->repmode, op->s1.bank, 1, r, mult), offd = slot_offset(cx, op->repmode, op->dst.bank, 3, r, mult);
		int off0 = gpi_offset(op->repmode, r), off2 = off0;   /* gpi0, gpi1 */
		for (int c = 0; c < 4; c++) if (op->mask & (1 << c))
			write_f32(cx, &op->dst, offd, c, read_f32(cx, &op->s0, off0, c) * read_f32(cx, &op->s1, off1, c) + read_f32(cx, &op->s2, off2, c));
	}
}
static void exec_vdp(Ctx *cx, const UOp *op) {
	int mult[4] = {2, 2, 2, 2};
	int mask = op->mask, n = op->sub2;
	for (int r = 0; r < op->reps; r++) {
		int off1 = slot_offset(cx, op->repmode, op->s1.bank, 1, r, mult), off2 = gpi_offset(op->repmode, r);   /* s2 is gpi0 */
		/* §2.12: the destination is NOT stepped by the repeat offset -- only the sources step (to
		 * the next matrix row); the destination word stays fixed and the rotating mask picks which
		 * of its components this iteration writes (so VDP4 rpt4 produces a full matrix*vector). */
		float dot = 0; for (int k = 0; k < n; k++) dot += read_f32(cx, &op->s1, off1, k) * read_f32(cx, &op->s2, off2, k);
		for (int c = 0; c < 4; c++) if (mask & (1 << c)) write_f32(cx, &op->dst, 0, c, dot);
		mask = ((mask << 1) | (mask >> 3)) & 0xF;   /* §2.12: VDP rotates its write mask each iteration */
	}
}
static void exec_vcomp(Ctx *cx, const UOp *op) {
	int mult[4] = {1, 1, 1, 1};
	for (int r = 0; r < op->reps; r++) {
		int off1 = slot_offset(cx, op->repmode, op->s1.bank, 1, r, mult), offd = slot_offset(cx, op->repmode, op->dst.bank, 3, r, mult);
		float x = read_f32(cx, &op->s1, off1, op->sub2 & 3), result;
		switch (op->sub) { case 0: result = 1.0f / x; break; case 1: result = 1.0f / sqrtf(x); break; case 2: result = log2f(x); break; default: result = exp2f(x); break; }
		for (int c = 0; c < 4; c++) if (op->mask & (1 << c)) write_f32(cx, &op->dst, offd, c, result);
	}
}
static int test_lane_raw(uint32_t raw, int type, int cmpsel) {
	int iszero, isneg;
	if (type == TY_F32) { float f; memcpy(&f, &raw, 4); iszero = (f == 0); isneg = (f < 0); }
	else if (type == TY_F16) { float f = half_to_float((uint16_t)raw); iszero = (f == 0); isneg = (f < 0); }
	else if (type == TY_U8 || type == TY_U16 || type == TY_U32) { iszero = (raw == 0); isneg = 0; }
	else { int32_t v = (int32_t)raw; iszero = (v == 0); isneg = (v < 0); }
	switch (cmpsel) { case 0: return iszero; case 1: return !iszero; case 2: return isneg; default: return isneg || iszero; }
}
static void exec_vmov(Ctx *cx, const UOp *op) {
	int mult[4]; get_mult(op, mult);
	for (int r = 0; r < op->reps; r++) {
		int off0 = slot_offset(cx, op->repmode, op->s0.bank, 0, r, mult), off1 = slot_offset(cx, op->repmode, op->s1.bank, 1, r, mult);
		int off2 = slot_offset(cx, op->repmode, op->s2.bank, 2, r, mult), offd = slot_offset(cx, op->repmode, op->dst.bank, 3, r, mult);
		for (int c = 0; c < 4; c++) if (op->mask & (1 << c)) {
			uint32_t raw;
			if (op->sub == 0) raw = read_i32(cx, &op->s1, off1, c);
			else {
				int testtype = op->sub == 2 ? TY_U8 : op->s1.type;
				uint32_t s0raw = read_i32(cx, &op->s0, off0, c);
				raw = test_lane_raw(s0raw, testtype, op->sub2) ? read_i32(cx, &op->s1, off1, c) : read_i32(cx, &op->s2, off2, c);
			}
			write_raw_lane(cx, &op->dst, offd, c, raw);
		}
	}
}
/* a VPCK source word: by word number like every other VPCK read, but an INDEXED1/2 operand resolves
 * through the index register first (resolve_addr does this for every other op; read raw here it was
 * 0 -- Unit 13's cascaded-shadow program moves its shadow matrix's translation row with
 * VPCK.f32 i1 <- sa[idx1*2+14..17], got 0, projected w = 0 and every shadow coordinate inf: the
 * shadow test never did anything). An internal register is numbered in registers, i<k> = words
 * 4k..4k+3, as resolve_addr counts it: read as a word number, the same program's closing
 * VPCK.f16 o <- i1 (the shadow factor) packed word 1 -- i0.y, the shadow-map y coordinate -- so
 * the mask came out as a coordinate whatever the shadow map held: no shadow from the cascades. */
static uint32_t vpck_src_word(Ctx *cx, const UOperand *o, int off, int widx) {
	if (o->bank == BK_IDXD1 || o->bank == BK_IDXD2) { int rb, rn; resolve_indexed(cx, o->bank == BK_IDXD1 ? 1 : 2, o->num, &rb, &rn); return bank_read_raw(cx, rb, rn + off + widx); }
	if (o->bank == BK_FPI) return bank_read_raw(cx, o->bank, ((int)o->num + off) * 4 + widx);
	/* the constant ROM (§2.7) and immediates went through bank_read_raw, which reads both as 0 (read_i32
	 * and read_f32 resolve them). Constant k is 64 bits: word 0 holds its F32 x or F16/U8 lanes 0-1,
	 * word 1 its y or lanes 2-3, as every other bank's words; an offset past word 1 runs on into the
	 * next constant. Uncharted's swipe prompt (the arrow a melee combo or climb asks the player to
	 * trace, GuiAnimatedGestureWidget's threshold pass, fragment 0x1f1a9fc8) loads its inside/outline
	 * selectors in its secondary program with VPCK.f16->f32 sa8..9 <- c15.y (1.0) and sa10..11 <- c0
	 * (0.0): read as 0, every pixel of the arrow came out (0,0,0,0) and the prompt never showed */
	if (o->bank == BK_FPCONST) { int wi = off + widx; return fpconst[((int)o->num + wi / 2) & 63][wi & 1]; }
	if (o->bank == BK_IMM) return (uint32_t)o->num;
	return bank_read_raw(cx, o->bank, (int)o->num + off + widx);
}
static void exec_vpck(Ctx *cx, const UOp *op) {
	int mult[4]; get_mult(op, mult);
	int src_f32 = (op->s1.type == TY_F32);
	int is_src_float = (op->s1.type == TY_F16 || op->s1.type == TY_F32 || op->s1.type == TY_C10);
	int is_dst_float = (op->dst.type == TY_F16 || op->dst.type == TY_F32 || op->dst.type == TY_C10);
	for (int r = 0; r < op->reps; r++) {
		int off1 = slot_offset(cx, op->repmode, op->s1.bank, 1, r, mult), off2 = slot_offset(cx, op->repmode, op->s2.bank, 2, r, mult);
		int offd = slot_offset(cx, op->repmode, op->dst.bank, 3, r, mult);
		/* an F32 vec4 spans two 64-bit units: src1 gives x,y and src2 z,w -- except that the internal
		 * registers (i0-i3) are 128 bits wide, so an F32 source there carries all four components
		 * itself (Unit 13's depth-to-position pass packs i0.xyzw to F16: z and w were read from an
		 * empty src2 and every reconstructed position lost its depth) */
		int wide = src_f32 && (op->s1.bank == BK_FPI ||
		           (op->s1.bank == op->s2.bank && op->s1.num == op->s2.num && off1 == off2 &&
		            op->s1.swiz[0] == 0 && op->s1.swiz[1] == 1 && op->s1.swiz[2] == 2 && op->s1.swiz[3] == 3 &&
		            (op->mask == 1 || op->mask == 3 || op->mask == 7 || op->mask == 15)));
		for (int c = 0; c < 4; c++) if (op->mask & (1 << c)) {
			int sel = op->s1.swiz[c] & 3, lpw, word, lane; uint32_t rawv;
			if (op->s1.bank == BK_FPI && op->s1.type == TY_F16) { float f; uint32_t raw = vpck_src_word(cx, &op->s1, off1, sel); memcpy(&f, &raw, 4); rawv = float_to_lane(f, TY_F16); }   /* fpi_widened */
			else if (!src_f32 || wide || sel < 2) { lpw = lanes_per_word(op->s1.type); word = sel / lpw; lane = sel % lpw; rawv = extract_lane(vpck_src_word(cx, &op->s1, off1, word), op->s1.type, lane); }
			else { lpw = lanes_per_word(op->s2.type); word = (sel - 2) / lpw; lane = (sel - 2) % lpw; rawv = extract_lane(vpck_src_word(cx, &op->s2, off2, word), op->s2.type, lane); }
			uint32_t outraw;
			if (op->sub) { float fv = lane_to_float_norm(rawv, op->s1.type); outraw = float_to_lane_norm(fv, op->dst.type); }
			else if (!is_src_float && !is_dst_float) outraw = rawv;
			else { float fv = lane_to_float(rawv, op->s1.type); outraw = is_dst_float ? float_to_lane(fv, op->dst.type) : (uint32_t)(int32_t)fv; }
			write_raw_lane(cx, &op->dst, offd, c, outraw);
		}
	}
}
/* §3.9's ALU table: 2 ADD, 3 FRC, 4 RCP, 5 RSQ, 6 LOG, 7 EXP, 8 DP (handled by the caller), 9 MIN,
 * 10 MAX, 11 DSX, 12 DSY, 13 MUL, 14 SUB (0,1,15 invalid). */
static float alu_eval_f(int op, float a, float b) {
	switch (op) { case 3: return a - floorf(b); case 4: return 1.0f / a; case 5: return 1.0f / sqrtf(a); case 6: return log2f(a); case 7: return exp2f(a);
	case 9: return a < b ? a : b; case 10: return a > b ? a : b; case 11: case 12: return 0; case 13: return a * b; case 14: return a - b; default: return a + b; }
}
static uint32_t alu_eval_i(int sel, int op, uint32_t a, uint32_t b) {
	if (sel == 3) switch (op) { case 0: return a & b; case 1: return a | b; case 2: return a ^ b; case 3: return a << (b & 31); case 4: return a >> (b & 31);
		case 5: { int s = (int)(b & 31); return s ? (a << s) | (a >> (32 - s)) : a; } case 7: return (uint32_t)((int32_t)a >> (b & 31)); default: return 0; }
	switch (op) { case 10: case 11: case 7: case 13: case 14: case 15: return a - b; case 8: case 9: return a * b; default: return a + b; }
}
static void exec_vtst(Ctx *cx, const UOp *op) {
	int alu_sel = op->sub, alu_op = op->sub2 & 0xF; float result;
	if (alu_sel == 0 && alu_op == 8) { float d = 0; for (int k = 0; k < 4; k++) d += read_f32(cx, &op->s1, 0, k) * read_f32(cx, &op->s2, 0, k); result = d; }
	else if (alu_sel == 0) { int chan = op->imm & 3; result = alu_eval_f(alu_op, read_f32(cx, &op->s1, 0, chan), read_f32(cx, &op->s2, 0, chan)); }
	else { int chan = op->imm & 3; result = (float)(int32_t)alu_eval_i(alu_sel, alu_op, read_i32(cx, &op->s1, 0, chan), read_i32(cx, &op->s2, 0, chan)); }
	int sign_test = (op->sub3 >> 2) & 3, zero_test = op->sub3 & 3, take;
	switch (sign_test) { case 1: take = zero_test == 1 ? result <= 0 : result < 0; break; case 2: take = zero_test == 1 ? result >= 0 : result > 0; break; default: take = zero_test == 1 ? result == 0 : result != 0; break; }
	cx->t->p[op->dst.num & 3] = (uint8_t)(take ? 1 : 0);
}
/* VTSTMSK: with mask type 2 and a float ALU the test is a vec4 one -- each lane tested on its own
 * components and written as its own mask (a dot product tests its one sum, written to all four). It
 * used to test lane 0 and copy the result: the PCF in Unit 13's shadow mask compared one of its four
 * shadow-map depths four times. */
static void exec_vtstmsk(Ctx *cx, const UOp *op) {
	int alu_sel = op->sub, alu_op = op->sub2 & 0xF;
	int type = op->dst.type, n = ((op->imm & 3) == 2 && alu_sel == 0) ? 4 : 1, per_lane = n == 4 && alu_op != 8;
	int takes[4];
	for (int c = 0; c < n; c++) {
		if (c && !per_lane) { takes[c] = takes[0]; continue; }
		int ch = per_lane ? c : 0; float result;
		if (alu_sel == 0 && alu_op == 8) { float d = 0; for (int k = 0; k < 4; k++) d += read_f32(cx, &op->s1, 0, k) * read_f32(cx, &op->s2, 0, k); result = d; }
		else if (alu_sel == 0) { result = alu_eval_f(alu_op, read_f32(cx, &op->s1, 0, ch), read_f32(cx, &op->s2, 0, ch)); }
		else { result = (float)(int32_t)alu_eval_i(alu_sel, alu_op, read_i32(cx, &op->s1, 0, ch), read_i32(cx, &op->s2, 0, ch)); }
		int sign_test = (op->sub3 >> 2) & 3, zero_test = op->sub3 & 3, take;
		switch (sign_test) { case 1: take = zero_test == 1 ? result <= 0 : result < 0; break; case 2: take = zero_test == 1 ? result >= 0 : result > 0; break; default: take = zero_test == 1 ? result == 0 : result != 0; break; }
		takes[c] = take;
	}
	for (int c = 0; c < n; c++) {
		uint32_t v; int take = takes[c];
		if (type == TY_F32) { float f = take ? 1.0f : 0.0f; memcpy(&v, &f, 4); }
		else if (type == TY_F16) v = take ? float_to_half(1.0f) : 0;
		else if (type == TY_U8) v = take ? 0xFFu : 0; else if (type == TY_U16) v = take ? 0xFFFFu : 0; else if (type == TY_U32) v = take ? 0xFFFFFFFFu : 0;
		else if (type == TY_S8) v = take ? 0x7Fu : 0; else if (type == TY_S16) v = take ? 0x7FFFu : 0; else v = take ? 0x7FFFFFFFu : 0;
		write_raw_lane(cx, &op->dst, 0, c, v);
	}
}
static uint32_t rol32(uint32_t v, int s) { s &= 31; return s ? (v << s) | (v >> (32 - s)) : v; }
static void exec_vbw(Ctx *cx, const UOp *op) {
	uint32_t a = read_i32(cx, &op->s1, 0, 0), b;
	if (op->s2.bank == BK_IMM) b = (uint32_t)op->s2.num | ((uint32_t)op->imm << 7) | ((uint32_t)op->args[2] << 14);
	else b = read_i32(cx, &op->s2, 0, 0);
	int rot = op->args[0] & 31;
	b = op->sub3 ? (uint32_t)(((b << (rot & 15)) | (b >> (16 - (rot & 15)))) & 0xFFFF) : rol32(b, rot);
	if (op->args[1]) b = op->sub3 ? (~b & 0xFFFFu) : ~b;
	uint32_t result;
	switch (op->sub) {
	case 2: result = op->sub2 ? (a | b) : (a & b); break;
	case 3: result = a ^ b; break;
	case 4: result = op->sub2 ? rol32(a, (int)(b & 31)) : (a << (b & 31)); break;
	case 5: result = op->sub2 ? (uint32_t)((int32_t)a >> (b & 31)) : (a >> (b & 31)); break;
	default: result = a; break;
	}
	if (op->sub3) result &= 0xFFFFu;
	write_raw_lane(cx, &op->dst, 0, 0, result);
}
static void u8_unpack(uint32_t w, float rgb[3], float *a) { rgb[0] = (w & 0xFF) / 255.0f; rgb[1] = ((w >> 8) & 0xFF) / 255.0f; rgb[2] = ((w >> 16) & 0xFF) / 255.0f; *a = ((w >> 24) & 0xFF) / 255.0f; }
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static uint32_t u8_pack4(const float v[4]) {
	uint32_t r = (uint32_t)(clamp01(v[0]) * 255.0f + 0.5f), g = (uint32_t)(clamp01(v[1]) * 255.0f + 0.5f);
	uint32_t b = (uint32_t)(clamp01(v[2]) * 255.0f + 0.5f), a = (uint32_t)(clamp01(v[3]) * 255.0f + 0.5f);
	return r | (g << 8) | (b << 16) | (a << 24);
}
static float cop_eval(int op, float x, float y) { switch (op) { case 0: return x + y; case 1: return x - y; case 2: return x < y ? x : y; default: return x > y ? x : y; } }
static void exec_sop2(Ctx *cx, const UOp *op) {
	static const int unit[4] = {1, 1, 1, 1};
	for (int r = 0; r < op->reps; r++) {
		int off1 = slot_offset(cx, op->repmode, op->s1.bank, 1, r, unit), off2 = slot_offset(cx, op->repmode, op->s2.bank, 2, r, unit), offd = slot_offset(cx, op->repmode, op->dst.bank, 3, r, unit);
		uint32_t w1 = read_i32(cx, &op->s1, off1, 0), w2 = read_i32(cx, &op->s2, off2, 0);
		float c1[3], a1, c2[3], a2; u8_unpack(w1, c1, &a1); u8_unpack(w2, c2, &a2);
		if (op->s1.negabs & 1) { for (int k = 0; k < 3; k++) c1[k] = 1 - c1[k]; a1 = 1 - a1; }
		int csel1 = op->args[0] & 0xFF, cmod1 = op->args[0] & 0x100, csel2 = op->args[1] & 0xFF, cmod2 = op->args[1] & 0x100;
		int asel1 = op->args[2] & 0xFF, amod1 = op->args[2] & 0x100, asel2 = op->args[3] & 0xFF, amod2 = op->args[3] & 0x100;
		float cf1[3], cf2[3], af1, af2;
		for (int k = 0; k < 3; k++) {
			cf1[k] = csel1 == 1 ? c1[k] : csel1 == 2 ? c2[k] : csel1 == 3 ? a1 : csel1 == 4 ? a2 : 0;
			cf2[k] = csel2 == 1 ? c1[k] : csel2 == 2 ? c2[k] : csel2 == 3 ? a1 : csel2 == 4 ? a2 : 0;
			if (cmod1) cf1[k] = 1 - cf1[k]; if (cmod2) cf2[k] = 1 - cf2[k];
		}
		af1 = asel1 == 1 ? a1 : asel1 == 2 ? a2 : 0; af2 = asel2 == 1 ? a1 : asel2 == 2 ? a2 : 0;
		if (amod1) af1 = 1 - af1; if (amod2) af2 = 1 - af2;
		float out[4];
		for (int k = 0; k < 3; k++) out[k] = cop_eval(op->sub, cf1[k] * c1[k], cf2[k] * c2[k]);
		out[3] = cop_eval(op->sub2, af1 * a1, af2 * a2);
		write_raw_lane(cx, &op->dst, offd, 0, u8_pack4(out));
	}
}
static void exec_sop2m(Ctx *cx, const UOp *op) {
	uint32_t w1 = read_i32(cx, &op->s1, 0, 0), w2 = read_i32(cx, &op->s2, 0, 0);
	float c1[3], a1, c2[3], a2; u8_unpack(w1, c1, &a1); u8_unpack(w2, c2, &a2);
	float s1v[4] = {c1[0], c1[1], c1[2], a1}, s2v[4] = {c2[0], c2[1], c2[2], a2};
	int sel1 = op->args[0] & 0xFF, mod1 = op->args[0] & 0x100, sel2 = op->args[1] & 0xFF, mod2 = op->args[1] & 0x100;
	float f1[4], f2[4], t1[4], t2[4], res[4];
	for (int k = 0; k < 4; k++) {
		f1[k] = sel1 == 2 ? s1v[k] : sel1 == 3 ? a1 : sel1 == 6 ? s2v[k] : sel1 == 7 ? a2 : 0;
		f2[k] = sel2 == 2 ? s1v[k] : sel2 == 3 ? a1 : sel2 == 6 ? s2v[k] : sel2 == 7 ? a2 : 0;
		if (mod1) f1[k] = 1 - f1[k]; if (mod2) f2[k] = 1 - f2[k];
		t1[k] = f1[k] * s1v[k]; t2[k] = f2[k] * s2v[k];
	}
	for (int k = 0; k < 3; k++) res[k] = cop_eval(op->sub, t1[k], t2[k]);
	res[3] = cop_eval(op->sub2, t1[3], t2[3]);
	uint32_t old = read_i32(cx, &op->dst, 0, 0); float oldc[3], olda; u8_unpack(old, oldc, &olda);
	float finalv[4] = {oldc[0], oldc[1], oldc[2], olda};
	for (int k = 0; k < 4; k++) if (op->mask & (1 << k)) finalv[k] = res[k];
	write_raw_lane(cx, &op->dst, 0, 0, u8_pack4(finalv));
}
static void exec_sop3(Ctx *cx, const UOp *op) {
	uint32_t w0 = read_i32(cx, &op->s0, 0, 0), w1 = read_i32(cx, &op->s1, 0, 0), w2 = read_i32(cx, &op->s2, 0, 0);
	float c0[3], a0, c1[3], a1, c2[3], a2; u8_unpack(w0, c0, &a0); u8_unpack(w1, c1, &a1); u8_unpack(w2, c2, &a2);
	int csel1 = op->args[0] & 0xFF, cmod1 = op->args[0] & 0x100, csel2 = op->args[1] & 0xFF, cmod2 = op->args[1] & 0x100;
	int asel1 = op->args[2] & 0xFF, amod1 = op->args[2] & 0x100;
	float cf1[3], cf2[3], af1, af2;
	for (int k = 0; k < 3; k++) {
		cf1[k] = csel1 == 2 ? c1[k] : csel1 == 3 ? a1 : csel1 == 4 ? c0[k] : csel1 == 5 ? a0 : csel1 == 6 ? c2[k] : csel1 == 7 ? a2 : 0;
		cf2[k] = csel2 == 2 ? c1[k] : csel2 == 3 ? a1 : csel2 == 4 ? c0[k] : csel2 == 5 ? a0 : csel2 == 6 ? c2[k] : csel2 == 7 ? a2 : 0;
	}
	af1 = asel1 == 1 ? a0 : asel1 == 2 ? a1 : asel1 == 3 ? a2 : 0;
	af2 = csel2 == 2 ? a1 : csel2 == 3 ? a1 : csel2 == 4 ? a0 : csel2 == 5 ? a0 : csel2 == 6 ? a2 : csel2 == 7 ? a2 : 0;
	if (cmod1) for (int k = 0; k < 3; k++) cf1[k] = 1 - cf1[k];
	if (cmod2) for (int k = 0; k < 3; k++) cf2[k] = 1 - cf2[k];
	if (amod1) af1 = 1 - af1;
	if (cmod2) af2 = 1 - af2;
	float out[4];
	for (int k = 0; k < 3; k++) out[k] = cop_eval(op->sub, cf1[k] * c1[k], cf2[k] * c2[k]);
	out[3] = cop_eval(op->sub2, af1 * a1, af2 * a2);
	write_raw_lane(cx, &op->dst, 0, 0, u8_pack4(out));
}
static void exec_i8mad(Ctx *cx, const UOp *op) {
	static const int unit[4] = {1, 1, 1, 1};
	for (int r = 0; r < op->reps; r++) {
		int off0 = slot_offset(cx, op->repmode, op->s0.bank, 0, r, unit), off1 = slot_offset(cx, op->repmode, op->s1.bank, 1, r, unit);
		int off2 = slot_offset(cx, op->repmode, op->s2.bank, 2, r, unit), offd = slot_offset(cx, op->repmode, op->dst.bank, 3, r, unit);
		uint32_t w0 = read_i32(cx, &op->s0, off0, 0), w1 = read_i32(cx, &op->s1, off1, 0), w2 = read_i32(cx, &op->s2, off2, 0);
		int csel0 = op->sub & 3, asel0 = op->sub2 & 1, csel1 = (op->sub2 >> 1) & 1, csel2 = (op->sub2 >> 2) & 1;
		uint32_t addrgb = csel0 == 0 ? (w0 & 0xFFFFFF) : csel0 == 1 ? (w1 & 0xFFFFFF) : csel0 == 2 ? ((w0 >> 24) * 0x010101u) : ((w1 >> 24) * 0x010101u);
		uint32_t adda = asel0 == 0 ? (w0 >> 24) : (w1 >> 24);
		uint32_t m1 = csel1 ? ((w1 >> 24) * 0x01010101u) : w1, m2 = csel2 ? ((w2 >> 24) * 0x01010101u) : w2;
		uint32_t result = 0;
		for (int lane = 0; lane < 4; lane++) {
			int la1 = (int)((m1 >> (8 * lane)) & 0xFF), la2 = (int)((m2 >> (8 * lane)) & 0xFF);
			int ad = lane < 3 ? (int)((addrgb >> (8 * lane)) & 0xFF) : (int)adda;
			int prod = la1 * la2 / 255, v = (op->s0.negabs & 1) ? prod - ad : prod + ad;
			if (v < 0) v = 0; if (v > 255) v = 255;
			result |= ((uint32_t)v & 0xFF) << (8 * lane);
		}
		write_raw_lane(cx, &op->dst, offd, 0, result);
	}
}
static void exec_i16mad(Ctx *cx, const UOp *op) {
	static const int unit[4] = {1, 1, 1, 1};
	for (int r = 0; r < op->reps; r++) {
		int off0 = slot_offset(cx, op->repmode, op->s0.bank, 0, r, unit), off1 = slot_offset(cx, op->repmode, op->s1.bank, 1, r, unit);
		int off2 = slot_offset(cx, op->repmode, op->s2.bank, 2, r, unit), offd = slot_offset(cx, op->repmode, op->dst.bank, 3, r, unit);
		uint32_t w0 = read_i32(cx, &op->s0, off0, 0), w1 = read_i32(cx, &op->s1, off1, 0), w2 = read_i32(cx, &op->s2, off2, 0);
		int signed_ = (op->sub >> 1) & 1, fmt2 = (op->sub2 >> 2) & 3, fmt1 = op->sub2 & 3;
		int v1 = fmt1 == 0 ? (int)(w1 & 0xFFFF) : (int)(op->args[0] ? (w1 >> 8) & 0xFF : w1 & 0xFF);
		int v2 = fmt2 == 0 ? (int)(w2 & 0xFFFF) : (int)(op->args[1] ? (w2 >> 8) & 0xFF : w2 & 0xFF);
		int v0 = (int)(w0 & 0xFFFF);
		if (signed_) { v0 = (int16_t)v0; if (fmt1 == 0) v1 = (int16_t)v1; if (fmt2 == 0) v2 = (int16_t)v2; }
		write_raw_lane(cx, &op->dst, offd, 0, (uint32_t)(v0 * v1 + v2) & 0xFFFF);
	}
}
static void exec_i32mad(Ctx *cx, const UOp *op) {
	static const int unit[4] = {1, 1, 1, 1};
	for (int r = 0; r < op->reps; r++) {
		int off0 = slot_offset(cx, op->repmode, op->s0.bank, 0, r, unit), off1 = slot_offset(cx, op->repmode, op->s1.bank, 1, r, unit);
		int off2 = slot_offset(cx, op->repmode, op->s2.bank, 2, r, unit), offd = slot_offset(cx, op->repmode, op->dst.bank, 3, r, unit);
		uint32_t w0 = read_i32(cx, &op->s0, off0, 0), w1 = read_i32(cx, &op->s1, off1, 0), w2 = read_i32(cx, &op->s2, off2, 0);
		int32_t v0 = op->args[0] ? (int32_t)((w0 >> 16) & 0xFFFF) : (int32_t)(w0 & 0xFFFF);
		int32_t v1 = op->args[1] ? (int32_t)((w1 >> 16) & 0xFFFF) : (int32_t)(w1 & 0xFFFF);
		if (op->sub) { v0 = (int16_t)v0; v1 = (int16_t)v1; }
		int32_t v2 = op->sub2 == 2 ? (int32_t)w2 : (op->args[2] ? (int32_t)((w2 >> 16) & 0xFFFF) : (int32_t)(w2 & 0xFFFF));
		write_raw_lane(cx, &op->dst, offd, 0, (uint32_t)(v0 * v1 + v2));
	}
}
static void exec_i32mad2(Ctx *cx, const UOp *op) {
	static const int unit[4] = {1, 1, 1, 1};
	for (int r = 0; r < op->reps; r++) {
		int off0 = slot_offset(cx, op->repmode, op->s0.bank, 0, r, unit), off1 = slot_offset(cx, op->repmode, op->s1.bank, 1, r, unit);
		int off2 = slot_offset(cx, op->repmode, op->s2.bank, 2, r, unit), offd = slot_offset(cx, op->repmode, op->dst.bank, 3, r, unit);
		if (op->sub2) { write_raw_lane(cx, &op->dst, offd, 0, read_i32(cx, &op->s2, off2, 0)); continue; }
		/* a 32 x 16 multiply: src1's low 16 bits (signed or not), src0 and src2 whole. Killzone's quarter-res downscale
		 * computes its read address as (960 * y + x) * 32 + row pointer with two of these: the first's src1 holds y in
		 * a 16-bit lane over an old float's high half (0x4270003c), the second's src0 is 960y + x, up to 130,000 --
		 * only this reading gives both (32 x 32 read the stale half: stripes; 16 x 16 wrapped the lower half of the
		 * image back to its top). Vita3K reads 32 x 32; its skinning cases (bone index x stride) can't tell. */
		int32_t v0 = (int32_t)read_i32(cx, &op->s0, off0, 0), v1 = (int32_t)read_i32(cx, &op->s1, off1, 0), v2 = (int32_t)read_i32(cx, &op->s2, off2, 0);
		v1 = op->sub ? (int32_t)(int16_t)v1 : (int32_t)(v1 & 0xffff);
		if (op->s1.negabs & 1) v1 = -v1; if (op->s2.negabs & 1) v2 = -v2;
		write_raw_lane(cx, &op->dst, offd, 0, (uint32_t)(v0 * v1 + v2));
	}
}
/* SMP: a dependent sampler's unit comes from the program's table (UsseDraw.sa_unit, filled by
 * usse_setup_registers; its texture-control-word SA slots stay 0, not real SceGxmTexture bits) --
 * the backend's own GfxTexture already carries everything a sample needs, so there is nothing else
 * to decode here. */
static UsseTexRawFn g_texfn_raw;
void usse_set_texfn_raw(UsseTexRawFn fn) { g_texfn_raw = fn; }
static UsseTexGatherFn g_texfn_gather;
void usse_set_texfn_gather(UsseTexGatherFn fn) { g_texfn_gather = fn; }
/* SMP sb_mode (bits 38-37): 0 the filtered sample; 1 "gather4" -- the 2x2 footprint unfiltered, each
 * component's four texels in gather order as the destination type (F32: 4 words, F16: 2), component
 * after component; 3 the same followed by the four bilinear weights as F16x4 (2 words), in the order
 * that pairs with the texels reversed: (1-u)(1-v), u(1-v), uv, (1-u)v; 2 "info" -- the fractions
 * (u, v), the trilinear fraction and the level, no texels. Unit 13's cascaded shadow mask is a
 * hardware PCF: SMP.gather4.uv of the shadow map, a per-lane VTSTMSK of the four depths against the
 * fragment's, then a dot product with the weights. Taken as a plain sample (one filtered depth in
 * r0, zeros in r1-r3 and in the weights) every fragment came out unshadowed. (Semantics as Vita3K's
 * translator reads them.) Returns 0 when it can't (no gather callback): the caller samples. */
static int smp_gather(Ctx *cx, const UOp *op, int unit, float u, float v) {
	int sb = op->args[0];
	if (sb == 0 || !g_texfn_gather) return 0;
	float tx[4][4] = {{0}}, fr[2] = {0, 0};
	int n = g_texfn_gather(cx->texctx, unit, u, v, tx, fr);
	if (n <= 0) return 0;
	UOperand d = op->dst; d.type = op->sub3 == 2 ? TY_F16 : TY_F32;   /* fconv 2: F16; 3 (and the raw 0/1, unknown here): F32 */
	int words = d.type == TY_F16 ? 2 : 4;
	if (sb == 2) {   /* info: u, v fractions, trilinear fraction, level (level 0: no mip selection here) */
		write_f32(cx, &d, 0, 0, fr[0]); write_f32(cx, &d, 0, 1, fr[1]); write_f32(cx, &d, 0, 2, 0); write_raw_lane(cx, &d, 0, 3, 0);
		return 1;
	}
	for (int k = 0; k < n; k++, d.num = (uint16_t)(d.num + words))
		for (int i = 0; i < 4; i++) write_f32(cx, &d, 0, i, tx[i][k]);
	if (sb == 3) {
		float fu = fr[0], fv = fr[1], wt[4] = {(1 - fu) * (1 - fv), fu * (1 - fv), fu * fv, (1 - fu) * fv};
		d.type = TY_F16;
		for (int i = 0; i < 4; i++) write_f32(cx, &d, 0, i, wt[i]);
	}
	return 1;
}
static void exec_smp(Ctx *cx, const UOp *op) {
	float u = read_f32(cx, &op->s0, 0, 0), v = read_f32(cx, &op->s0, 0, 1), w3 = read_f32(cx, &op->s0, 0, 2);
	uint32_t unit = op->s1.bank == BK_SA && cx->d && cx->d->sa_unit[op->s1.num & 255] ? cx->d->sa_unit[op->s1.num & 255] - 1u : read_i32(cx, &op->s1, 0, 0) & 0xFF;   /* a dependent sampler by its table (UsseDraw.sa_unit), else the SA word (usse_run's saN=) */
	float lod = 0;
	if (op->sub2 == 1 || op->sub2 == 2) lod = read_f32(cx, &op->s2, 0, 0);
	if (smp_gather(cx, op, (int)unit, u, v)) return;
	/* fconv_type 0 (and 1): the texel's native packed words, which the program unpacks itself --
	 * converting them to floats here handed deferred renderers float bit patterns for their packed
	 * G-buffer bytes and depth (Unit 13's lighting came out a flat gradient) */
	if ((op->sub3 == 0 || op->sub3 == 1) && g_texfn_raw) {
		uint32_t raw[4] = {0, 0, 0, 0};
		int n = g_texfn_raw(cx->texctx, (int)unit, u, v, w3, lod, raw);
		if (n > 0) {
			for (int c = 0; c < n && c < 4; c++) if (op->mask & (1 << c)) write_raw_lane(cx, &op->dst, 0, c, raw[c]);
			return;
		}
	}
	float out[4] = {0, 0, 0, 1};
	if (cx->texfn) cx->texfn(cx->texctx, (int)unit, u, v, w3, lod, out);
	/* fconv_type 2: the result is F16x4, two lanes per word -- two words, not four. Writing a
	 * half per word spilled the sample over the two following registers (Unit 13's ambient pass
	 * sampled its cube maps over the albedo it was about to multiply) */
	UOperand d = op->dst; if (op->sub3 == 2) d.type = TY_F16;
	for (int c = 0; c < 4; c++) if (op->mask & (1 << c)) write_f32(cx, &d, 0, c, out[c]);
}
static void exec_ldst(Ctx *cx, const UOp *op) {
	int elemsz = op->args[3], is_store = op->sub2;
	static const int unit[4] = {1, 1, 1, 1};
	for (int r = 0; r < op->reps; r++) {
		int off0 = slot_offset(cx, op->repmode, op->s0.bank, 0, r, unit), off1 = slot_offset(cx, op->repmode, op->s1.bank, 1, r, unit);
		int off2 = slot_offset(cx, op->repmode, op->s2.bank, 2, r, unit), offd = slot_offset(cx, op->repmode, op->dst.bank, 3, r, unit);
		uint32_t base = read_i32(cx, &op->s0, off0, 0);
		int32_t off = op->s1.bank == BK_IMM ? (int32_t)op->s1.num * elemsz : (int32_t)read_i32(cx, &op->s1, off1, 0) - 0x10000;
		if (!op->args[0]) off += 4;
		if (!is_store) {
			/* a load's repeat count is the number of CONSECUTIVE elements fetched from the one
			 * address into consecutive destination words (mask_count+1), not a repeated fetch of
			 * the same word: Unit 13's skinned characters read every bone-matrix row as its first
			 * element repeated, and their G-buffer light colour as (R,R,R,R) */
			uint32_t addr = base + (uint32_t)off + read_i32(cx, &op->s2, off2, 0);
			for (int i = 0; i < op->reps; i++) {
				uint32_t a = addr + (uint32_t)(i * elemsz);
				uint32_t v = elemsz == 4 ? RD32(a) : elemsz == 2 ? RD16(a) : RD8(a);
				bank_write_raw(cx, op->dst.bank, op->dst.num + i, v);
			}
			break;
		} else {
			uint32_t addr = base + (uint32_t)off;
			WR32(addr, read_i32(cx, &op->s2, off2, 0));
		}
	}
}
static void exec_kill(Ctx *cx, const UOp *op) {
	int v = op->imm, kill = v == 0 ? 1 : v == 1 ? !cx->t->p[0] : v == 2 ? !cx->t->p[1] : cx->t->p[0];
	if (kill) cx->t->killed = 1;
}
static void exec_depthf(Ctx *cx, const UOp *op) { cx->t->depth = read_f32(cx, &op->s0, 0, 0); cx->t->depth_written = 1; }
static void exec_limm(Ctx *cx, const UOp *op) { bank_write_raw(cx, op->dst.bank, op->dst.num, (uint32_t)op->imm); }

/* VDUAL: too many distinct operand roles for UOp's fixed slots, so usse_vdual_decode re-derives
 * them from the raw word (§3.5) -- for exec_vdual here and for usse_spirv.c's emit_vdual alike. */
static int du_nsrc(int op) { static const int n[15] = {3, 2, 1, 2, 2, 1, 1, 1, 3, 2, 2, 2, 1, 1, 0}; return op >= 0 && op < 15 ? n[op] : 0; }
static int du_is_scalar(int op) { return op >= DU_FRSQ && op <= DU_FLOG; }
/* register numbers are in 64-bit units (doubled, §2.3) only for an operand a VECTOR op reads or
 * writes; a scalar op's unified operand counts 32-bit words. Doubling them all (as this did) put
 * Killzone: Mercenary's FEXPs in the wrong registers: vertex program 0x10c409d0's "FEXP O6.x" (its
 * TEXCOORD1.x, nothing else writes it) went to O12, which isn't an output at all, fragment
 * 0x11312920's "FADD PA0.x = i0.x + SA9.y" read SA18 (the program has 18 SA words, SA0-17), and
 * 0x112ef460's "FMUL r6.x" wrote r12 (7 temps). VDP/VSSQ load vectors but store a scalar. */
static int du_vec_load(int op) { return op >= DU_VMAD && op <= DU_VMOV; }
static int du_vec_store(int op) { return op == DU_VMAD || op == DU_VMUL || op == DU_VADD || op == DU_VMOV; }
void usse_vdual_decode(const UOp *op, UsseDual *v) {
	uint64_t w = op->raw;
	memset(v, 0, sizeof *v);
	for (int k = 0; k < 3; k++) v->s1[k] = v->s2[k] = -1;
	int sec = (op->sub3 >> 2) & 1, comp4 = (int)BIT(w, 59), f16 = (int)BIT(w, 53);
	int type = f16 ? TY_F16 : TY_F32;
	int op1ext = (!comp4 && BIT(w, 54)) ? 8 : 0, op1sel = op1ext | (int)BITS(w, 46, 44);
	int op2ext = BIT(w, 43) ? 8 : 0, op2sel = op2ext | (int)BITS(w, 20, 18);
	static const uint8_t t1[16] = {DU_VMAD,DU_VDP,DU_VSSQ,DU_VMUL,DU_VADD,DU_VMOV,DU_FRSQ,DU_FRCP,DU_FMAD,DU_FADD,DU_FMUL,DU_FSUBFLR,DU_FEXP,DU_FLOG,DU_INVALID,DU_INVALID};
	static const uint8_t t2[16] = {DU_INVALID,DU_VDP,DU_VSSQ,DU_VMUL,DU_VADD,DU_VMOV,DU_FRSQ,DU_FRCP,DU_FMAD,DU_FADD,DU_FMUL,DU_FSUBFLR,DU_FEXP,DU_FLOG,DU_INVALID,DU_INVALID};
	int op1 = t1[op1sel & 15], op2 = t2[op2sel & 15];
	v->op1 = op1; v->op2 = op2; v->ncomp = comp4 ? 4 : 3;
	if (op1 == DU_INVALID && op2 == DU_INVALID) return;
	int n1 = du_nsrc(op1), n2 = du_nsrc(op2), prim_ustore = (int)BIT(w, 42), src_config = (int)BITS(w, 17, 16);
	/* the source layouts (§3.5's two tables), as DUS_* indices */
	enum { U = DUS_US, A = DUS_I0, B = DUS_I1, C = DUS_I2 };
	int8_t *s1 = v->s1, *s2 = v->s2;
	if (n1 == 1) s1[0] = src_config == 0 ? U : src_config == 1 ? A : src_config == 2 ? B : C;
	else if (n1 == 2) { if (src_config == 0) { s1[0] = U; s1[1] = B; } else if (src_config == 1) { s1[0] = A; s1[1] = U; } else { s1[0] = A; s1[1] = B; } }
	else if (n1 == 3) { if (src_config == 0) { s1[0] = U; s1[1] = B; s1[2] = C; } else if (src_config == 1) { s1[0] = A; s1[1] = U; s1[2] = C; } else if (src_config == 2) { s1[0] = A; s1[1] = B; s1[2] = U; } else { s1[0] = A; s1[1] = B; s1[2] = C; } }
	if (n1 == 1 && n2 == 1) s2[0] = src_config == 0 ? A : U;
	else if (n1 == 1 && n2 == 2) { if (src_config == 0) { s2[0] = A; s2[1] = B; } else if (src_config == 1) { s2[0] = U; s2[1] = B; } else if (src_config == 2) { s2[0] = A; s2[1] = U; } else { s2[0] = B; s2[1] = U; } }
	else if (n1 == 1 && n2 == 3) { if (src_config == 0) { s2[0] = A; s2[1] = B; s2[2] = C; } else if (src_config == 1) { s2[0] = U; s2[1] = B; s2[2] = C; } else if (src_config == 2) { s2[0] = A; s2[1] = U; s2[2] = C; } else { s2[0] = A; s2[1] = U; s2[2] = B; } }
	else if (n1 == 2 && n2 == 1) s2[0] = src_config == 0 ? A : src_config == 1 ? B : U;
	else if (n1 == 2 && n2 == 2) { if (src_config == 0) { s2[0] = A; s2[1] = C; } else if (src_config == 1) { s2[0] = B; s2[1] = C; } else { s2[0] = C; s2[1] = U; } }
	else if (n1 == 3 && n2 == 1) s2[0] = src_config == 0 ? A : src_config == 1 ? B : src_config == 2 ? C : U;
	int us_reader = -1;   /* the op that reads US (the layouts give it to one op at most) */
	for (int k = 0; k < 3; k++) { if (s1[k] == U) us_reader = op1; if (s2[k] == U) us_reader = op2; }
	UOperand *US = &v->src[U], *I0 = &v->src[A], *I1 = &v->src[B], *I2 = &v->src[C];
	/* the unified store's bank is the src1/2 table's with ext 0: bit 14 is the US abs flag (op1
	 * with < 2 sources) or I2's low number bit, never a bank extension. Reading it as one turned
	 * Killzone: Mercenary's vertex 0x112ef2c0 "VMAD i1 = SA0.xyzw * i0.xxxx + i1" (its MVP's
	 * first row times x, I2 = i1 so bit 14 set) into a read through INDEXED2 -- temps that program
	 * never has -- so every vertex lost its x term. */
	set_operand(US, bank_src12((int)BITS(w, 31, 30), 0), BITS(w, 6, 0), 7, us_reader >= 0 && du_vec_load(us_reader), type, sec);
	int us_ext = (n1 < 2) ? (int)BIT(w, 15) : 0;
	decode_dual((int)BITS(w, 51, 48), us_ext, comp4, US->swiz);
	if (BIT(w, 47)) US->negabs |= 1;
	if (n1 < 2 && BIT(w, 14)) US->negabs |= 2;
	I0->bank = I1->bank = I2->bank = BK_FPI; I0->type = I1->type = I2->type = (uint8_t)type;
	I0->num = (uint16_t)BITS(w, 11, 10); I1->num = (uint16_t)BITS(w, 13, 12);
	I2->num = (uint16_t)(n1 >= 2 ? ((BIT(w, 15) << 1) | BIT(w, 14)) : 2);
	decode_dual((int)BITS(w, 41, 38), 0, comp4, I0->swiz);
	decode_dual((int)BITS(w, 37, 34), (int)BIT(w, 52), comp4, I1->swiz);
	identity_swiz(I2->swiz);
	if (BIT(w, 58)) I1->negabs |= 1;
	UOperand UD, ID; memset(&UD, 0, sizeof UD); memset(&ID, 0, sizeof ID);
	set_operand(&UD, bank_dst2((int)BITS(w, 33, 32), 0), BITS(w, 27, 21), 7, du_vec_store(prim_ustore ? op1 : op2), type, sec);
	ID.bank = BK_FPI; ID.num = (uint16_t)BITS(w, 29, 28); ID.type = (uint8_t)type;
	v->d1 = prim_ustore ? UD : ID; v->d2 = prim_ustore ? ID : UD;
	int um = (int)(BITS(w, 9, 7) | ((comp4 && BIT(w, 54)) ? 8 : 0)), im = comp4 ? 0xF : 0x7;
	v->m1 = prim_ustore ? um : im; v->m2 = prim_ustore ? im : um;
	if (du_is_scalar(op1)) v->m1 &= 1; if (du_is_scalar(op2)) v->m2 &= 1;
	/* The write masks (2026-10-16, Killzone: Mercenary in-level): the op writing the unified store writes ONE register
	 * -- 64 bits for a vector op (F32 .xy, F16 .xyzw), a word for a scalar, VDP or VSSQ result -- and the encoded
	 * mask (bits 9:7 + 54) is the internal-register op's (Vita3K's reading, its alu.cpp vdual). Giving the encoded
	 * mask to the unified write instead stored a VDP's scalar into two or more words, clobbering whatever lay past
	 * it (a vertex's UV attribute, its position terms): the level's geometry came out a slab of garbage on the
	 * interpreter. With this the interpreter draws the ISA lobby, and the other games' gate frames are unchanged.
	 * RECOMP_VDUAL_MASKS=old: the previous reading, to compare. */
	{	static int old = -1; if (old < 0) { const char *e = getenv("RECOMP_VDUAL_MASKS"); old = e && !strcmp(e, "old"); }
		if (!old) {
			int f16m = type == TY_F16;
			int uop = prim_ustore ? op1 : op2, umask = du_vec_store(uop) ? (f16m ? 0xF : 0x3) : (f16m ? 0x3 : 0x1);
			/* ...unless the unified store's register is an internal one (a TEMP number past the temps, set_operand): those
			 * are 128 bits, and the op writes the whole vector, .xyz (.xyzw with the 4-component encoding), whatever it
			 * is -- Vita3K's get_dual_op_write_mask(dest_internal). Killzone's lightmapped walls (0x2c45e100: "VDUAL ...
			 * vmul i2 = SA.yzwy * i1.x", the specular light colour) kept i2.z from the directional lightmap unpacked into
			 * i2 earlier (.xy written), and the light colour came out (0, 0, dir.z): saturated blue light on walls, cables
			 * and metal, on both backends (2026-10-17) */
			if ((prim_ustore ? v->d1.bank : v->d2.bank) == BK_FPI) umask = comp4 ? 0xF : 0x7;
			v->m1 = prim_ustore ? umask : um; v->m2 = prim_ustore ? um : umask;
		}
	}
	if (op1 == DU_INVALID) v->m1 = 0; if (op2 == DU_INVALID) v->m2 = 0;
}
static float du_src(Ctx *cx, const UsseDual *v, int k, int c) { return k < 0 ? 0.0f : read_f32(cx, &v->src[k], 0, c); }
static void du_eval(Ctx *cx, const UsseDual *v, int dop, const int8_t *s, float r[4]) {
	int n = v->ncomp;
	switch (dop) {
	case DU_VMAD: for (int c = 0; c < n; c++) r[c] = du_src(cx, v, s[0], c) * du_src(cx, v, s[1], c) + du_src(cx, v, s[2], c); break;
	case DU_VDP: case DU_VSSQ: { float d = 0; int b = dop == DU_VSSQ ? s[0] : s[1]; for (int c = 0; c < n; c++) d += du_src(cx, v, s[0], c) * du_src(cx, v, b, c); for (int c = 0; c < 4; c++) r[c] = d; break; }
	case DU_VMUL: for (int c = 0; c < n; c++) r[c] = du_src(cx, v, s[0], c) * du_src(cx, v, s[1], c); break;
	case DU_VADD: for (int c = 0; c < n; c++) r[c] = du_src(cx, v, s[0], c) + du_src(cx, v, s[1], c); break;
	case DU_VMOV: for (int c = 0; c < n; c++) r[c] = du_src(cx, v, s[0], c); break;
	case DU_FRSQ: r[0] = 1.0f / sqrtf(du_src(cx, v, s[0], 0)); break;
	case DU_FRCP: r[0] = 1.0f / du_src(cx, v, s[0], 0); break;
	case DU_FMAD: r[0] = du_src(cx, v, s[0], 0) * du_src(cx, v, s[1], 0) + du_src(cx, v, s[2], 0); break;
	case DU_FADD: r[0] = du_src(cx, v, s[0], 0) + du_src(cx, v, s[1], 0); break;
	case DU_FMUL: r[0] = du_src(cx, v, s[0], 0) * du_src(cx, v, s[1], 0); break;
	case DU_FSUBFLR: r[0] = du_src(cx, v, s[0], 0) - floorf(du_src(cx, v, s[1], 0)); break;
	case DU_FEXP: r[0] = exp2f(du_src(cx, v, s[0], 0)); break;
	case DU_FLOG: r[0] = log2f(du_src(cx, v, s[0], 0)); break;
	default: break;   /* DU_INVALID: its mask is 0 */
	}
}
static void exec_vdual(Ctx *cx, const UOp *op) {
	UsseDual v; usse_vdual_decode(op, &v);
	if (v.op1 == DU_INVALID && v.op2 == DU_INVALID) return;
	float r1[4] = {0, 0, 0, 0}, r2[4] = {0, 0, 0, 0};
	du_eval(cx, &v, v.op1, v.s1, r1); du_eval(cx, &v, v.op2, v.s2, r2);   /* both read before either writes (staged anyway) */
	for (int c = 0; c < 4; c++) if (v.m1 & (1 << c)) write_f32(cx, &v.d1, 0, c, r1[c]);
	for (int c = 0; c < 4; c++) if (v.m2 & (1 << c)) write_f32(cx, &v.d2, 0, c, r2[c]);
}
static void exec_instruction(Ctx *cx, const UOp *op) {
	switch (op->op) {
	case OP_VMAD2: exec_vmad2(cx, op); break;
	case OP_VNMAD: exec_vnmad(cx, op); break;
	case OP_VMAD: exec_vmad(cx, op); break;
	case OP_VDP: exec_vdp(cx, op); break;
	case OP_VDUAL: exec_vdual(cx, op); break;
	case OP_VCOMP: exec_vcomp(cx, op); break;
	case OP_VMOV: exec_vmov(cx, op); break;
	case OP_VPCK: exec_vpck(cx, op); break;
	case OP_VTST: exec_vtst(cx, op); break;
	case OP_VTSTMSK: exec_vtstmsk(cx, op); break;
	case OP_VBW: exec_vbw(cx, op); break;
	case OP_SOP2: exec_sop2(cx, op); break;
	case OP_SOP2M: exec_sop2m(cx, op); break;
	case OP_SOP3: exec_sop3(cx, op); break;
	case OP_I8MAD: exec_i8mad(cx, op); break;
	case OP_I16MAD: exec_i16mad(cx, op); break;
	case OP_I32MAD: exec_i32mad(cx, op); break;
	case OP_I32MAD2: exec_i32mad2(cx, op); break;
	case OP_SMP: exec_smp(cx, op); break;
	case OP_KILL: exec_kill(cx, op); break;
	case OP_LIMM: exec_limm(cx, op); break;
	case OP_DEPTHF: exec_depthf(cx, op); break;
	case OP_LDST: exec_ldst(cx, op); break;
	default: break;   /* NOP and everything we treat as a no-op */
	}
}
static const char *opname(int op);
static void run_program(Ctx *cx, const UOp *code, int n) {
	for (int s = 0; s < 4; s++) for (int r = 0; r <= 16; r++) cx->inc[s][r] = (int8_t)r;
	int guard = 0;
	for (int pc = 0; pc < n && !cx->t->killed; ) {
		const UOp *op = &code[pc];
		if (op->op == OP_BR) {
			if (pred_true(cx, op)) {
				pc = pc + op->imm;
				if (++guard > 100000) break;   /* a runaway backward branch; stop rather than hang the pixel/vertex */
				continue;
			}
			pc++; continue;
		}
		if (op->op == OP_SMLSI) {
			for (int s = 0; s < 4; s++) {
				int v = op->args[s], inc = (int8_t)(v & 0xFF), mode = (v >> 8) & 1;
				if (!mode) for (int r = 0; r <= 16; r++) cx->inc[s][r] = (int8_t)(r * inc);
				else for (int r = 0; r < 4; r++) cx->inc[s][r] = (int8_t)((inc >> (2 * r)) & 3);
			}
			pc++; continue;
		}
		if (op->op == OP_SMBO) { for (int s = 0; s < 4; s++) for (int r = 0; r <= 16; r++) cx->inc[s][r] = (int8_t)(r + op->args[s]); pc++; continue; }
		cx->staging = 1; cx->cur_rep = 0; cx->nst = 0;
		if (pred_true(cx, op)) exec_instruction(cx, op);
		stage_commit(cx); cx->staging = 0;
		{	/* RECOMP_PROBE=t|p|o:<word>@<instr>: snapshot that register word after primary instruction <instr> (gfx_soft.c shows it as the fragment's colour; see README) */
			static const char *pr = 0; static int pri = 0, pat = -1, pbank = 0, pnum = 0;
			if (!pri) { pri = 1; pr = getenv("RECOMP_PROBE"); if (pr && strchr(pr, '@')) { pat = atoi(strchr(pr, '@') + 1); pbank = pr[0] | 0x20; pnum = atoi(pr + 2); } }   /* T/P/O (upper case): the vertex program's register, shown per triangle by its first vertex */
			if (pat == pc) { const UWord *b = pbank == 't' ? cx->t->r : pbank == 'p' ? cx->t->pa : cx->t->o; cx->t->probe = b[pnum % 160].u; }
		}
		/* RECOMP_USSE_TRACE=1: every executed instruction with its destination's words afterwards
		 * (the harness usse_run and gfxreplay both honour it) */
		static int trace = -1; if (trace < 0) trace = getenv("RECOMP_USSE_TRACE") != 0;
		if ((trace || g_trace_force) && op->op != OP_NOP) {
			fprintf(stderr, "  %3d %-8s", pc, opname(op->op));
			if (op->op == OP_VDUAL) {   /* its UOp has no dst: each op's sources and destination words, from the shared decode */
				static const char *const dn[15] = {"vmad","vdp","vssq","vmul","vadd","vmov","frsq","frcp","fmad","fadd","fmul","fsubflr","fexp","flog","-"};
				static const char *const srcname[4] = {"us", "i0", "i1", "i2"};
				UsseDual v; usse_vdual_decode(op, &v);
				for (int k = 0; k < 2; k++) {
					const UOperand *d = k ? &v.d2 : &v.d1; const int8_t *sx = k ? v.s2 : v.s1;
					int o = k ? v.op2 : v.op1, m = k ? v.m2 : v.m1, base = d->bank == BK_FPI ? d->num * 4 : d->num;
					fprintf(stderr, " | op%d %s", k + 1, dn[o < 15 ? o : 14]);
					for (int i = 0; i < 3 && sx[i] >= 0; i++) {
						const UOperand *q = &v.src[sx[i]];
						fprintf(stderr, " %s(bank%d r%d).%d%d%d%d%s", srcname[sx[i]], q->bank, q->num, q->swiz[0], q->swiz[1], q->swiz[2], q->swiz[3], q->negabs & 1 ? "(neg)" : q->negabs & 2 ? "(abs)" : "");
					}
					fprintf(stderr, " -> bank%d r%d type%d mask%x =", d->bank, d->num, d->type, m);
					for (int w = 0; w < 2; w++) {   /* i<k> holds one F32 per word whatever the type (§2.1) */
						uint32_t u = bank_read_raw(cx, d->bank, base + w); float f; memcpy(&f, &u, 4);
						if (d->type == TY_F16 && d->bank != BK_FPI) fprintf(stderr, " %08x(%g|%g)", u, half_to_float((uint16_t)u), half_to_float((uint16_t)(u >> 16))); else fprintf(stderr, " %08x(%g)", u, f);
					}
				}
			} else if (op->dst.bank != BK_NONE) {
				fprintf(stderr, " dst bank%d r%d type%d mask%x =", op->dst.bank, op->dst.num, op->dst.type, op->mask);
				for (int w = 0; w < 2; w++) {
					uint32_t v = bank_read_raw(cx, op->dst.bank, (op->dst.bank == BK_FPI ? op->dst.num * 4 : op->dst.num) + w); float f; memcpy(&f, &v, 4);   /* i<k> is words 4k..4k+3 */
					if (op->dst.type == TY_F16) fprintf(stderr, " %08x(%g|%g)", v, half_to_float((uint16_t)v), half_to_float((uint16_t)(v >> 16)));   /* lane x | lane y */
					else fprintf(stderr, " %08x(%g)", v, f);
				}
			}
			if (op->s0.bank != BK_NONE) fprintf(stderr, "   s0 bank%d r%d type%d swz%d%d%d%d%s", op->s0.bank, op->s0.num, op->s0.type, op->s0.swiz[0], op->s0.swiz[1], op->s0.swiz[2], op->s0.swiz[3], op->s0.negabs & 1 ? "(neg)" : op->s0.negabs & 2 ? "(abs)" : "");
			fprintf(stderr, "   s1 bank%d r%d type%d swz%d%d%d%d%s  s2 bank%d r%d type%d swz%d%d%d%d%s\n", op->s1.bank, op->s1.num, op->s1.type, op->s1.swiz[0], op->s1.swiz[1], op->s1.swiz[2], op->s1.swiz[3], op->s1.negabs & 1 ? "(neg)" : op->s1.negabs & 2 ? "(abs)" : "",
			        op->s2.bank, op->s2.num, op->s2.type, op->s2.swiz[0], op->s2.swiz[1], op->s2.swiz[2], op->s2.swiz[3], op->s2.negabs & 1 ? "(neg)" : op->s2.negabs & 2 ? "(abs)" : "");
		}
		pc++;
	}
}
/* RECOMP_TRACE_DRAW (gfx_soft.c): the trace switched on for one draw's programs only */
void usse_set_trace(int on) { g_trace_force = on; }
void usse_run_secondary(const UsseProgram *p, UsseInst *inst, UsseDraw *d) {
	Ctx cx = {0}; cx.t = inst; cx.d = d; cx.secondary = 1;
	run_program(&cx, p->sec, p->nsec);
}
void usse_run_primary(const UsseProgram *p, UsseInst *inst, UsseDraw *d, void *texctx, UsseTexFn texfn) {
	Ctx cx = {0}; cx.t = inst; cx.d = d; cx.texctx = texctx; cx.texfn = texfn;
	run_program(&cx, p->prim, p->nprim);
}
/* ---------------------------------------------------------------- 4/7. program cache + layouts */
static int gxp_param_type_for(int cat_type) {   /* §1.3 type code -> our TY_* */
	switch (cat_type) { case 0: return TY_F32; case 1: return TY_F16; case 2: return TY_C10; case 3: return TY_U32; case 4: return TY_S32;
	case 5: return TY_U16; case 6: return TY_S16; case 7: return TY_U8; case 8: return TY_S8; default: return TY_F32; }
}
static void parse_vertex_layout(uint32_t p, UsseVertexLayout *vl) {
	memset(vl, 0, sizeof *vl);
	vl->index_reg = vl->instance_reg = -1;
	for (int i = 0; i < 10; i++) vl->o_tex[i] = -1;
	vl->o_color0 = vl->o_color1 = vl->o_fog = vl->o_psize = -1;
	uint32_t cnt = RD32(p + H_PARAM_COUNT), base = h_param_base(p);
	for (uint32_t i = 0; i < cnt && vl->nattrs < USSE_MAX_ATTRS; i++) {
		uint32_t pr = param_at(base, (int)i);
		int cat = RD16(pr + 4) & 15;
		if (cat != 0) continue;   /* ATTRIBUTE only */
		UsseAttrSlot *s = &vl->attrs[vl->nattrs++];
		s->reg = (int)RD32(pr + 12);
		s->type = (uint8_t)gxp_param_type_for((RD16(pr + 4) >> 4) & 15);
		s->comps = RD8(pr + 5) & 15;
		s->array_size = (int)RD32(pr + 8); if (s->array_size < 1) s->array_size = 1;
		s->words = (uint8_t)((s->comps + lanes_per_word(s->type) - 1) / (lanes_per_word(s->type) ? lanes_per_word(s->type) : 1));
		if (!s->words) s->words = 1;
		s->untyped = 0;
	}
	uint32_t vo = RD32(p + H_VARY_OFF);
	if (vo) {
		uint32_t vb = p + H_VARY_OFF + vo;
		uint64_t untyped = RD64(vb + 8);
		for (int i = 0; i < vl->nattrs; i++) if (vl->attrs[i].reg >= 0 && vl->attrs[i].reg < 64 && (untyped & (1ull << vl->attrs[i].reg))) vl->attrs[i].untyped = 1;
		uint32_t vout1 = RD32(vb + 16), vout2 = RD32(vb + 20);
		int cur = 4;
		if (vout1 & 0x800) { vl->o_color0 = cur; cur += 4; }
		if (vout1 & 0x400) { vl->o_color1 = cur; cur += 4; }
		if (vout1 & 0x200) { vl->o_fog = cur; cur += 2; }
		for (int t = 0; t < 10; t++) { int g = (int)((vout2 >> (3 * t)) & 7); if (g) { int comps = 2 * (g & 1) + ((g >> 1) & 1) + ((g >> 2) & 1); vl->o_tex[t] = cur; vl->ncomp_tex[t] = comps; cur += comps; } }
		if (vout1 & 0x100) { vl->o_psize = cur; cur += 1; }
		vl->total_o = cur;
		vl->index_reg = (int)(int16_t)RD16(vb + 28); vl->instance_reg = (int)(int16_t)RD16(vb + 30);
		if (!(RD32(p + H_FLAGS) & 0x20000)) vl->index_reg = -1;
		if (!(RD32(p + H_FLAGS) & 0x40000)) vl->instance_reg = -1;
	} else vl->total_o = 4;
}
static void parse_fragment_layout(uint32_t p, UsseFragLayout *fl) {
	memset(fl, 0, sizeof *fl);
	uint32_t flags = RD32(p + H_FLAGS);
	fl->native_color = (flags >> 6) & 1; fl->frag_color_used = (flags >> 7) & 1; fl->discard_used = (flags >> 3) & 1;
	uint32_t vo = RD32(p + H_VARY_OFF);
	if (!vo) return;
	uint32_t vb = p + H_VARY_OFF + vo;
	fl->color_pa_start = fl->native_color ? 0 : (int)RD8(vb + 8);
	fl->color_type = gxp_param_type_for(RD8(vb + 10) & 15); fl->color_comps = RD8(vb + 11);
	int n = RD16(vb + 12);
	uint32_t descoff = RD32(vb + 16);
	if (!descoff) return;
	uint32_t db = vb + 16 + descoff;
	int pa = 0;
	for (int i = 0; i < n && fl->niters < USSE_MAX_ITERS - 1; i++) {
		uint32_t d = db + 16 * (uint32_t)i;
		uint32_t info = RD32(d), size = RD32(d + 8), comp_info = RD32(d + 12);
		int resource_index = (int)RD32(d + 4);
		if ((info & 0x4000F000u) != 0xF000u) {
			UsseIter *it = &fl->iters[fl->niters++];
			it->is_tex = 0;
			int sem = (int)((info >> 12) & 0xF);
			it->semantic = sem <= 9 ? sem : sem == 0xA ? 10 : sem == 0xB ? 11 : sem == 0xC ? 12 : 13;
			it->point_coord = (int)((info >> 30) & 1);
			it->comps = (int)((info >> 22) & 3) + 1;
			it->storage = (info & 0x20000000u) ? 1 : (info & 0x10000000u) ? 2 : (info & 0x00100000u) ? 0 : (it->semantic == 10 || it->semantic == 11) ? 3 : 0;
			it->pa = pa;
			pa += (int)((size >> 4) & 3) + 1;
		}
		if ((info & 0x40Fu) != 0xFu) {
			UsseIter *it = &fl->iters[fl->niters++];
			it->is_tex = 1;
			it->tex_coord_src = (int)(info & 0xF); it->tex_proj = (int)((info >> 8) & 3); it->tex_unit = resource_index;
			it->point_coord = (int)((info >> 10) & 1);
			it->result_fmt = (int)((comp_info >> 4) & 3); it->pa = pa;
			pa += (int)((size >> 6) & 3) + 1;
		}
	}
}
static UOp *decode_slice(uint32_t code_base, int n, int secondary, int *supported_out) {
	if (n <= 0) { if (supported_out) *supported_out &= 1; return 0; }
	UOp *ops = malloc(sizeof(UOp) * (size_t)n);
	for (int i = 0; i < n; i++) {
		uint64_t w = RD64(code_base + 8u * (uint32_t)i);
		ops[i] = decode_one(w, secondary);
		if (ops[i].op == OP_UNSUPPORTED) *supported_out = 0;
	}
	return ops;
}
#define PROG_HASH_N 256
static UsseProgram *prog_table[PROG_HASH_N];     /* by address */
static UsseProgram *content_table[PROG_HASH_N];  /* by content hash: the decoded entries */
/* the GXP's bytes hashed (its header's size field, bounded): the shader patcher writes its patched
 * programs into a ring, so one address holds different programs over time and one program lands at
 * a new address every frame -- keyed by address alone, the GPU backend recompiled 52 shaders a
 * frame (213 ms) and would have run a stale decode once the ring wrapped */
uint32_t usse_gxp_hash(const void *gxp, uint32_t *size) {   /* (of bytes anywhere: gxp_index.c hashes the game's files) */
	const uint8_t *b = gxp; uint32_t n; memcpy(&n, b + 8, 4); if (n < 128 || n > (1u << 20)) n = 128;
	*size = n;
	uint64_t h = 0x9E3779B97F4A7C15ull; uint32_t i = 0;
	for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 0x100000001B3ull; h ^= h >> 29; }
	for (; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
	return (uint32_t)(h ^ (h >> 32));
}
int usse_gxp_sane(const void *gxp, uint32_t size) {
	const uint8_t *b = gxp; uint32_t f[5]; memcpy(f, b + H_PRIM_INSTR_COUNT, sizeof f);   /* (60..76: primary count, offset; secondary count, start, end) */
	const uint64_t end = size, pn = f[0], pc = H_PRIM_OFF + (uint64_t)f[1], s0 = f[3], s1 = f[4];
	if (size < 128 || pn > (1u << 16) || (pn && pc + 8 * pn > end)) return 0;
	uint64_t sn = s1 + 4 >= s0 ? (s1 + 4 - s0) / 8 : 0; if (f[2] && f[2] < sn) sn = f[2];   /* (decode_program's) */
	return !sn || (sn <= (1u << 16) && H_SEC_OFF + s0 + 8 * sn <= end);
}
static uint32_t prog_content_hash(uint32_t addr, uint32_t *size) { return usse_gxp_hash(G(addr), size); }
uint32_t usse_program_hash(const UsseProgram *p) { return p->hash; }
uint32_t usse_content_hash(uint32_t prog_addr, uint32_t *size) { return prog_content_hash(prog_addr, size); }
int usse_buffer_words(const UsseProgram *p, int c) { int base = 0, size = 0; return p && find_container(p->addr, c, &base, &size) && size > 0 ? size : 0; }   /* the words usse_setup_registers copies from uniform buffer c (0: the program reaches it only through pointers, LDST) */
int usse_program_pa_words(const UsseProgram *p) { return p->pa_words; }
uint32_t usse_float_to_lane(float v, int type) { return float_to_lane(v, type); }   /* the interpreter's own conversion, for gfx_soft.c's direct vertex packer */
/* the decode itself, into p (usse_get's tables, or usse_decode_uncached's own) */
static void decode_program(UsseProgram *p, uint32_t prog_addr, uint32_t hash, uint32_t size) {
	p->hash = hash; p->size = size; p->supported = 1; p->additive = 0;   /* (new bytes at a known address: decided again) */
	uint32_t flags = RD32(prog_addr + H_FLAGS);
	p->is_fragment = flags & 1;
	uint32_t pn = RD32(prog_addr + H_PRIM_INSTR_COUNT), pc = pn ? prog_addr + H_PRIM_OFF + RD32(prog_addr + H_PRIM_OFF) : 0;
	/* the secondary program's bounds are each relative to their own header field (+72 start, +76
	 * end), so the range is (end+4) - start; measured as end - start it lost the last instruction
	 * of every secondary program -- the one with the END flag, typically the pack of the last
	 * uniform the primary reads as F16 (Unit 13's fog pass read its height-fog colour as the F32
	 * bits of an unpacked word: green frames) */
	uint32_t s0 = RD32(prog_addr + H_SEC_OFF), s1 = RD32(prog_addr + H_SEC_OFF_END);
	int sn = (s1 + 4 >= s0) ? (int)((s1 + 4 - s0) / 8) : 0;
	uint32_t hn = RD32(prog_addr + H_SEC_INSTR_COUNT); if (hn && (int)hn < sn) sn = (int)hn;
	uint32_t sc = sn ? prog_addr + H_SEC_OFF + s0 : 0;
	p->nprim = (int)pn; p->nsec = sn;
	p->prim = decode_slice(pc, (int)pn, 0, &p->supported);
	p->sec = decode_slice(sc, sn, 1, &p->supported);
	if (p->is_fragment) parse_fragment_layout(prog_addr, &p->fl); else parse_vertex_layout(prog_addr, &p->vl);
	{	int n = RD16(prog_addr + H_PRIM_REGS);
		if (!p->is_fragment) {
			for (int i = 0; i < p->vl.nattrs; i++) { const UsseAttrSlot *a = &p->vl.attrs[i]; int e = a->reg + (a->words ? a->words : 4) * (a->array_size > 0 ? a->array_size : 1); if (e > n) n = e; }
			if (p->vl.index_reg >= 0 && p->vl.index_reg + 1 > n) n = p->vl.index_reg + 1;
			if (p->vl.instance_reg >= 0 && p->vl.instance_reg + 1 > n) n = p->vl.instance_reg + 1;
		}
		n = (n + 3) & ~3; if (n < 4) n = 4; if (n > 160) n = 160;
		p->pa_words = n;
	}
	p->literal_words = RD32(prog_addr + H_LITERAL_SIZE) / 4;
	p->literal_addr = prog_addr + H_LITERAL_DATA_OFF + RD32(prog_addr + H_LITERAL_DATA_OFF);
}
const UsseProgram *usse_get(uint32_t prog_addr) {
	uint32_t size, hash = prog_content_hash(prog_addr, &size);
	int h = (int)((prog_addr >> 4) % PROG_HASH_N), hc = (int)(hash % PROG_HASH_N);
	UsseProgram *e = 0;
	for (UsseProgram *p = prog_table[h]; p; p = p->next) if (p->addr == prog_addr) { e = p; break; }
	if (e && e->hash == hash && e->size == size) return e;
	if (!e) { e = calloc(1, sizeof *e); e->addr = prog_addr; e->next = prog_table[h]; prog_table[h] = e; }
	for (UsseProgram *q = content_table[hc]; q; q = q->hnext) if (q->hash == hash && q->size == size && q != e) {   /* the same program decoded at another address: share the decode */
		UsseProgram *next = e->next; *e = *q; e->next = next; e->hnext = 0; e->addr = prog_addr;
		e->literal_addr = prog_addr + H_LITERAL_DATA_OFF + RD32(prog_addr + H_LITERAL_DATA_OFF);
		return e;
	}
	UsseProgram *p = e; decode_program(p, prog_addr, hash, size);
	p->hnext = content_table[hc]; content_table[hc] = p;
	return p;
}
/* a decode kept out of usse_get's tables, of a program copied into scratch memory (the shader recipes' loader: programs
 * read from the game's files); usse_program_free releases it */
UsseProgram *usse_decode_uncached(uint32_t prog_addr) {
	uint32_t size, hash = prog_content_hash(prog_addr, &size);
	UsseProgram *p = calloc(1, sizeof *p); if (!p) return 0;
	p->addr = prog_addr; decode_program(p, prog_addr, hash, size);
	return p;
}
void usse_program_free(UsseProgram *p) { if (p) { free(p->prim); free(p->sec); free(p); } }
int usse_is_fragment(const UsseProgram *p) { return p->is_fragment; }
int usse_supported(const UsseProgram *p) { return p->supported; }
int usse_primary_reads_memory(const UsseProgram *p) { for (int i = 0; i < p->nprim; i++) if (p->prim[i].op == OP_LDST) return 1; return 0; }
const UOp *usse_program_code(const UsseProgram *p, int secondary, int *count) {
	*count = secondary ? p->nsec : p->nprim;
	return secondary ? p->sec : p->prim;
}
const UsseVertexLayout *usse_vertex_layout(const UsseProgram *p) { return &p->vl; }
const UsseFragLayout *usse_fragment_layout(const UsseProgram *p) { return &p->fl; }
const UsseAttrSlot *usse_find_attr(const UsseVertexLayout *vl, int reg) {
	for (int i = 0; i < vl->nattrs; i++) {
		const UsseAttrSlot *s = &vl->attrs[i];
		int span = s->words * s->array_size;
		if (reg >= s->reg && reg < s->reg + (span > 0 ? span : s->words)) return s;
	}
	return 0;
}
/* ---------------------------------------------------------------- 7. per-draw setup / packing */
static UsseUniformFilter uniform_filter;
void usse_set_uniform_filter(UsseUniformFilter f) { uniform_filter = f; }
int usse_param_resource(uint32_t prog, const char *name, int *container) {
	uint32_t cnt = RD32(prog + H_PARAM_COUNT), base = h_param_base(prog);
	for (uint32_t i = 0; base && i < cnt && i < 256; i++) {
		uint32_t pr = param_at(base, (int)i);
		if (strcmp((const char *)G(pr + RD32(pr)), name)) continue;
		if (container) *container = (RD16(pr + 4) >> 12) & 15;
		return (int)RD32(pr + 12);
	}
	return -1;
}
/* the program's literal constants into their SA words (container 16, else 19) */
static void literals_load(const UsseProgram *p, UsseDraw *d) {
	uint32_t prog = p->addr;
	int c19base = 0, c19size = 0; find_container(prog, 19, &c19base, &c19size);
	int c16base = 0, c16size = 0;
	int litbase = find_container(prog, 16, &c16base, &c16size) ? c16base : c19base;
	uint32_t lcnt = RD32(prog + H_LITERALS_COUNT), lbase = RD32(prog + H_LITERALS_OFF) ? prog + H_LITERALS_OFF + RD32(prog + H_LITERALS_OFF) : 0;
	for (uint32_t i = 0; i < lcnt; i++) {
		uint32_t e = lbase + 8 * i; int off = (int)RD32(e); uint32_t val = RD32(e + 4);
		int at = litbase + off; if (at >= 0 && at < 256) d->sa[at].u = val;
	}
}
void usse_setup_registers(const UsseProgram *p, UsseDraw *d, uint32_t def_uniform_buf, const uint32_t ubuf[14]) {
	uint32_t prog = p->addr;
	memset(d->sa, 0, sizeof d->sa); memset(d->sa_unit, 0, sizeof d->sa_unit);
	for (int c = 0; c <= 14; c++) {
		int base_sa, size_f32;
		if (!find_container(prog, c, &base_sa, &size_f32) || size_f32 <= 0) continue;
		uint32_t bufaddr = c == 14 ? def_uniform_buf : (ubuf ? ubuf[c] : 0);
		if (!bufaddr) continue;
		for (int w = 0; w < size_f32 && base_sa + w < 256 && base_sa + w >= 0; w++) d->sa[base_sa + w].u = RD32(bufaddr + 4 * (uint32_t)w);
		if (c == 14 && uniform_filter) uniform_filter(p, prog, d, base_sa, size_f32);
	}
	int c19base = 0, c19size = 0; find_container(prog, 19, &c19base, &c19size);
	literals_load(p, d);
	uint32_t ucnt = RD32(prog + H_UBINFO_COUNT), ubase = RD32(prog + H_UBINFO_OFF) ? prog + H_UBINFO_OFF + RD32(prog + H_UBINFO_OFF) : 0;
	for (uint32_t i = 0; i < ucnt; i++) {
		uint32_t e = ubase + 8 * i; int reside = RD16(e); int ldst_off = RD16(e + 2); int32_t bias = (int32_t)RD32(e + 4);
		uint32_t addr = 0;
		if (reside <= 13) addr = ubuf ? ubuf[reside] : 0;
		else if (reside == 14) addr = def_uniform_buf;
		else if (reside == 16) addr = p->literal_addr;
		if (addr) { int at = c19base + ldst_off; if (at >= 0 && at < 256) d->sa[at].u = addr + (uint32_t)bias; }
	}
	uint32_t depcnt = RD32(prog + H_DEPSAMP_COUNT), depbase = RD32(prog + H_DEPSAMP_OFF) ? prog + H_DEPSAMP_OFF + RD32(prog + H_DEPSAMP_OFF) : 0;
	for (uint32_t i = 0; i < depcnt; i++) {
		uint32_t e = depbase + 4 * i; int rilo = RD16(e), sa_off = RD16(e + 2);
		int unitn = rilo / 4, word = rilo % 4;
		if (word == 0) { int at = c19base + sa_off; if (at >= 0 && at < 256 && unitn < 255) d->sa_unit[at] = (uint8_t)(unitn + 1); }   /* the unit, beside the SA words (UsseDraw.sa_unit): those stay 0 */
	}
}
void usse_pack_attr(UsseInst *inst, const UsseAttrSlot *slot, const float comps[4], const void *raw, int rawlen) {
	if (!slot) return;
	if (slot->untyped) {
		if (raw && rawlen > 0) {
			int reg = slot->reg & 159, maxlen = (160 - reg) * 4;
			memcpy(&inst->pa[reg], raw, (size_t)(rawlen < maxlen ? rawlen : maxlen));
		}
		return;
	}
	UOperand o; memset(&o, 0, sizeof o);
	o.bank = BK_PA; o.num = (uint16_t)slot->reg; o.type = slot->type;
	Ctx cx; memset(&cx, 0, sizeof cx); cx.t = inst;
	for (int c = 0; c < slot->comps && c < 4; c++) write_f32(&cx, &o, 0, c, comps[c]);
}
void usse_pack_iter(UsseInst *inst, const UsseIter *it, const float comps[4]) {
	UOperand o; memset(&o, 0, sizeof o);
	o.bank = BK_PA; o.num = (uint16_t)it->pa;
	if (it->is_tex) o.type = (uint8_t)(it->result_fmt == 2 ? TY_F16 : TY_F32);
	else o.type = (uint8_t)(it->storage == 1 ? TY_F16 : it->storage == 2 ? TY_C10 : it->storage == 3 ? TY_U8 : TY_F32);
	Ctx cx; memset(&cx, 0, sizeof cx); cx.t = inst;
	int n = it->is_tex ? 4 : it->comps;
	for (int c = 0; c < n && c < 4; c++) {
		if (o.type == TY_U8) write_raw_lane(&cx, &o, 0, c, (uint32_t)(clamp01(comps[c]) * 255.0f + 0.5f));
		else if (o.type == TY_C10) write_raw_lane(&cx, &o, 0, c, float_to_lane_norm(comps[c], TY_C10));
		else write_f32(&cx, &o, 0, c, comps[c]);
	}
}
void usse_pack_iter_raw(UsseInst *inst, const UsseIter *it, const uint32_t raw[4], int n) {
	UOperand o; memset(&o, 0, sizeof o);
	o.bank = BK_PA; o.num = (uint16_t)it->pa; o.type = TY_F32;   /* one word per lane: the packed texel as it is */
	Ctx cx; memset(&cx, 0, sizeof cx); cx.t = inst;
	for (int c = 0; c < n && c < 4; c++) write_raw_lane(&cx, &o, 0, c, raw[c]);
}
void usse_read_color(const UsseInst *inst, const UsseFragLayout *fl, float out[4]) {
	UOperand o; memset(&o, 0, sizeof o);
	o.bank = (uint8_t)(fl->native_color ? BK_O : BK_PA); o.num = (uint16_t)fl->color_pa_start; o.type = (uint8_t)fl->color_type;
	identity_swiz(o.swiz);
	Ctx cx; memset(&cx, 0, sizeof cx); cx.t = (UsseInst *)inst;
	out[0] = out[1] = out[2] = 0; out[3] = 1;
	int n = fl->color_comps > 0 ? fl->color_comps : 4;
	int norm = (o.type == TY_U8 || o.type == TY_S8 || o.type == TY_U16 || o.type == TY_S16 || o.type == TY_U32 || o.type == TY_S32);
	for (int c = 0; c < n && c < 4; c++) {
		uint32_t raw = read_i32(&cx, &o, 0, c);
		out[c] = norm ? lane_to_float_norm(raw, o.type) : lane_to_float(raw, o.type);
	}
}
void usse_preload_color(UsseInst *inst, const UsseFragLayout *fl, const float in[4]) {
	UOperand o; memset(&o, 0, sizeof o);
	o.bank = (uint8_t)(fl->native_color ? BK_O : BK_PA); o.num = (uint16_t)fl->color_pa_start; o.type = (uint8_t)fl->color_type;
	identity_swiz(o.swiz);
	Ctx cx; memset(&cx, 0, sizeof cx); cx.t = inst;
	int n = fl->color_comps > 0 ? fl->color_comps : 4;
	int norm = (o.type == TY_U8 || o.type == TY_S8 || o.type == TY_U16 || o.type == TY_S16 || o.type == TY_U32 || o.type == TY_S32);
	for (int c = 0; c < n && c < 4; c++) {
		if (norm) write_raw_lane(&cx, &o, 0, c, float_to_lane_norm(in[c], o.type));
		else write_f32(&cx, &o, 0, c, in[c]);
	}
}
/* usse_fragment_fb_blend: what a FRAGCOLOR_USED program does with the framebuffer colour it reads, when that is
 * a blend the GPU's blend unit can do -- so a GPU without a framebuffer read (SDL_GPU's Vulkan backend) can run
 * it without one, instead of copying the whole target before every such draw (gfx_gpu.c's fetch mode 1:
 * Uncharted's deferred lights accumulate into the HDR target that way, its particles and glass alpha-blend into
 * it in the shader -- 120 full-target copies a frame). Per component, colour out = K * colour in + X, with K
 * and X independent of the colour in:
 *   USSE_FB_ADD          K = 1 on red, green and blue; alpha K = 1 too (or no alpha)  -> ONE / ONE
 *   USSE_FB_ADD_REPLACE  K = 1 on red, green and blue; alpha K = 0                     -> ONE / ONE, alpha ONE / ZERO
 *   USSE_FB_LERP_KEEP    one K (any, per pixel) on red, green and blue; alpha kept    -> ONE / ONE_MINUS_SRC_ALPHA, alpha ZERO / ONE
 *   USSE_FB_LERP_OVER    one K on all four, alpha's X = 1 - K ("over")                 -> ONE / ONE_MINUS_SRC_ALPHA on all four
 * (ADD: the module runs the program once with zeros for the colour and outputs X; LERP: twice, 0 and 1, and
 * outputs (X.rgb, 1 - K) -- usse_spirv_set_fetch_zero.)
 * Decided by running it, not by reading it: the interpreter's own register and lane semantics (F16 halves,
 * repeats, swizzles) with no second model of them to drift. Straight-line programs only (no branch,
 * predicate, KILL, DEPTHF or memory access: every path is the one path, nothing escapes but the colour), then
 * 16 random input sets (PA, the uniforms around the program's real literals, texture samples by coordinate;
 * every other set positive only, as colours and samples are; a set whose out(0) isn't finite -- a square root
 * of a negative -- says nothing and is skipped, 8 valid needed), each run with six framebuffer colours -- 0,
 * +-A, 2A, +-B, components distinct so a swizzled read shows, B large so a clamp or saturation shows; K from
 * B, every run must agree with K * colour + out(0) to the colour type's rounding, and K's own uncertainty
 * (that rounding over B) is the tolerance of the comparisons between K's. Evidence, not proof: a program linear in the colour except at values never tried
 * would pass. The cost of a wrong answer is one shader's blend, and RECOMP_GPU_NO_ADDITIVE=1 turns it all off.
 * Cached in the program. */
static uint32_t add_rng(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x ? x : 0x9e3779b9u; }
static uint32_t add_word(uint32_t *s, int pos) {   /* +-[0.25, 4) as F32 (pos: + only), its low half a finite F16 too (bit 14, that half's top exponent bit, clear) */
	float v = (0.25f + 3.75f * (float)(add_rng(s) & 0xffffff) / 16777216.0f) * ((add_rng(s) & 1) && !pos ? -1.0f : 1.0f);
	uint32_t b; memcpy(&b, &v, 4); return b & ~0x4000u;
}
typedef struct { uint32_t seed; } AddTex;
static void add_texfn(void *ctx, int unit, float u, float v, float w, float lod, float out[4]) {   /* a texel by coordinate: the same sample for the same coordinates in every run of one input set */
	uint32_t h = ((AddTex *)ctx)->seed ^ (uint32_t)unit * 0x9e3779b1u, bits[4]; float c[4] = {u, v, w, lod};
	memcpy(bits, c, sizeof bits);
	for (int k = 0; k < 4; k++) { h ^= bits[k]; h *= 0x85ebca6bu; h ^= h >> 13; }
	for (int k = 0; k < 4; k++) { h = add_rng(&h); out[k] = (float)(h & 0xffff) / 32768.0f; }
}
static int fb_log(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_FB_BLEND_LOG") != 0; return v; }   /* RECOMP_FB_BLEND_LOG=1: each program's class, and what ruled the others out */
int usse_fragment_fb_blend(const UsseProgram *cp) {
	UsseProgram *p = (UsseProgram *)cp;
	if (p->additive) return p->additive - 1;
	p->additive = 1;   /* (USSE_FB_NONE, cached as +1) */
	const UsseFragLayout *fl = &p->fl;
	if (!p->is_fragment || !p->supported || !fl->frag_color_used || (fl->color_type != TY_F16 && fl->color_type != TY_F32)) return USSE_FB_NONE;
	for (int i = 0; i < p->nprim; i++) {
		const UOp *op = &p->prim[i];
		if (op->pred_kind || op->op == OP_BR || op->op == OP_KILL || op->op == OP_DEPTHF || op->op == OP_LDST || op->op == OP_UNSUPPORTED) return USSE_FB_NONE;
	}
	int sec_ok = 1; for (int i = 0; i < p->nsec; i++) if (p->sec[i].op == OP_LDST || p->sec[i].op == OP_UNSUPPORTED) sec_ok = 0;   /* (a secondary that reads memory isn't run: random SA stands in) */
	static const float A[4] = {0.375f, 0.625f, 1.25f, 0.875f}, B[4] = {24.0f, 40.0f, 12.0f, 3.0f};
	const float scale[6] = {0, 1, -1, 2, 1, -1}; const float *base[6] = {A, A, A, A, B, B};
	UsseTexRawFn raw_saved = g_texfn_raw; g_texfn_raw = 0;   /* (the backend's raw fetch would read our ctx as its own) */
	int n = fl->color_comps > 0 ? fl->color_comps : 4, linear = 1;
	int add = 1, lerp = 1, a_keep = 1, a_add = 1, a_repl = 1, a_over = 1;   /* what every input set so far allows */
	float ulp = fl->color_type == TY_F16 ? 1.0f / 1024.0f : 1.0f / 1048576.0f;
	static UsseInst inst, tmpl; static UsseDraw d;
	int valid = 0;
	for (int t = 0; t < 32 && linear && valid < 16; t++) {
		uint32_t rs = 0x2545f491u * (uint32_t)(t + 1);
		memset(&tmpl, 0, sizeof tmpl); memset(&d, 0, sizeof d);
		for (int k = 0; k < 160; k++) tmpl.pa[k].u = add_word(&rs, t & 1);
		for (int k = 0; k < 256; k++) d.sa[k].u = add_word(&rs, t & 1);
		literals_load(p, &d);
		if (sec_ok && p->nsec) { memset(&inst, 0, sizeof inst); usse_run_secondary(p, &inst, &d); }
		AddTex tc = { add_rng(&rs) };
		float out[6][4];
		for (int r = 0; r < 6; r++) {
			float fb[4]; for (int c = 0; c < 4; c++) fb[c] = scale[r] * base[r][c];
			inst = tmpl; usse_preload_color(&inst, fl, fb);
			usse_run_primary(p, &inst, &d, &tc, add_texfn);
			usse_read_color(&inst, fl, out[r]);
		}
		int fin = 1; for (int c = 0; c < n; c++) if (!isfinite(out[0][c])) fin = 0;
		if (!fin) continue;   /* inputs this program can't take: no evidence either way */
		valid++;
		float K[4] = {1, 1, 1, 1}, X[4] = {0, 0, 0, 0}, tk[4] = {0, 0, 0, 0};
		for (int c = 0; c < n && linear; c++) {
			K[c] = (out[4][c] - out[0][c]) / B[c]; X[c] = out[0][c];   /* from B, the largest colour: the least rounding */
			tk[c] = 4.0f * ulp * fmaxf(1.0f, fmaxf(fabsf(out[4][c]), fmaxf(fabsf(out[0][c]), B[c]))) / B[c];   /* K's uncertainty: that rounding, over B */
			for (int r = 1; r < 6 && linear; r++) {
				float fb = scale[r] * base[r][c], want = K[c] * fb + out[0][c];
				float mag = fmaxf(1.0f, fmaxf(fabsf(out[r][c]), fmaxf(fabsf(out[0][c]), fabsf(fb))));
				if (!isfinite(out[r][c]) || !isfinite(out[0][c]) || fabsf(out[r][c] - want) > 2.0f * ulp * mag) {
					linear = 0;
					if (fb_log()) fprintf(stderr, "usse: fb blend %#x: not linear in the colour: set %d comp %d colour %g: out %g, K %g * colour + out(0) %g = %g\n", p->addr, t, c, fb, out[r][c], K[c], out[0][c], want);
				}
			}
		}
		if (!linear) break;
		for (int c = 0; c < 3 && c < n; c++) { if (fabsf(K[c] - 1) > tk[c]) add = 0; if (fabsf(K[c] - K[0]) > tk[c] + tk[0]) lerp = 0; }
		if (n >= 4) {
			float tol = 2.0f * ulp * fmaxf(1.0f, fabsf(X[3]));
			if (fabsf(K[3] - 1) > tk[3] || fabsf(X[3]) > tol) a_keep = 0;
			if (fabsf(K[3] - 1) > tk[3]) a_add = 0;
			if (fabsf(K[3]) > tk[3]) a_repl = 0;
			if (fabsf(K[3] - K[0]) > tk[3] + tk[0] || fabsf(X[3] - (1 - K[0])) > tol + tk[0]) a_over = 0;
		}
		if (fb_log() && !add && !lerp && linear) { fprintf(stderr, "usse: fb blend %#x: set %d K %g %g %g %g (+-%g %g %g %g)\n", p->addr, t, K[0], K[1], K[2], K[3], tk[0], tk[1], tk[2], tk[3]); }
	}
	if (valid < 8) { linear = 0; if (fb_log()) fprintf(stderr, "usse: fb blend %#x: %d input sets of 32 gave a finite colour\n", p->addr, valid); }
	g_texfn_raw = raw_saved;
	int cls = USSE_FB_NONE;
	if (linear && add && (a_keep || a_add)) cls = USSE_FB_ADD;
	else if (linear && add && a_repl) cls = USSE_FB_ADD_REPLACE;
	else if (linear && lerp && a_keep) cls = USSE_FB_LERP_KEEP;
	else if (linear && lerp && a_over) cls = USSE_FB_LERP_OVER;
	p->additive = (int8_t)(cls + 1);
	if (fb_log()) fprintf(stderr, "usse: fb blend %#x: class %d (linear %d add %d lerp %d; alpha keep %d add %d replace %d over %d)\n", p->addr, cls, linear, add, lerp, a_keep, a_add, a_repl, a_over);
	return cls;
}
/* ---------------------------------------------------------------- 8. disassembler */
static const char *opname(int op) {
	switch (op) {
	case OP_NOP: return "NOP"; case OP_VMAD2: return "VMAD2"; case OP_VNMAD: return "VNMAD"; case OP_VMAD: return "VMAD"; case OP_VDP: return "VDP";
	case OP_VDUAL: return "VDUAL"; case OP_VCOMP: return "VCOMP"; case OP_VMOV: return "VMOV"; case OP_VPCK: return "VPCK"; case OP_VTST: return "VTST";
	case OP_VTSTMSK: return "VTSTMSK"; case OP_VBW: return "VBW"; case OP_SOP2: return "SOP2"; case OP_SOP2M: return "SOP2M"; case OP_SOP3: return "SOP3";
	case OP_I8MAD: return "I8MAD"; case OP_I16MAD: return "I16MAD"; case OP_I32MAD: return "I32MAD"; case OP_I32MAD2: return "I32MAD2"; case OP_SMP: return "SMP";
	case OP_BR: return "BR"; case OP_SMLSI: return "SMLSI"; case OP_SMBO: return "SMBO"; case OP_KILL: return "KILL"; case OP_LIMM: return "LIMM";
	case OP_DEPTHF: return "DEPTHF"; case OP_LDST: return "LDST"; default: return "???";
	}
}
int usse_count_unsupported(const UsseProgram *p, int *total_out) {
	int bad = 0, tot = p->nsec + p->nprim;
	for (int i = 0; i < p->nsec; i++) if (p->sec[i].op == OP_UNSUPPORTED) bad++;
	for (int i = 0; i < p->nprim; i++) if (p->prim[i].op == OP_UNSUPPORTED) bad++;
	if (total_out) *total_out = tot;
	return bad;
}
int usse_program_uses_depthf(const UsseProgram *p) { for (int i = 0; i < p->nprim; i++) if (p->prim[i].op == OP_DEPTHF) return 1; return 0; }
int usse_program_uses_kill(const UsseProgram *p) { for (int i = 0; i < p->nprim; i++) if (p->prim[i].op == OP_KILL) return 1; return 0; }
int usse_disassemble(const UsseProgram *p, void *out_FILE) {
	FILE *f = (FILE *)out_FILE; int unk = 0;
	fprintf(f, "program %#x: %s, %d secondary + %d primary instructions%s\n", p->addr, p->is_fragment ? "fragment" : "vertex",
	        p->nsec, p->nprim, p->supported ? "" : "  [UNSUPPORTED INSTRUCTION PRESENT]");
	for (int which = 0; which < 2; which++) {
		const UOp *ops = which ? p->prim : p->sec; int n = which ? p->nprim : p->nsec;
		if (!n) continue;
		fprintf(f, " %s:\n", which ? "primary" : "secondary");
		for (int i = 0; i < n; i++) {
			const UOp *o = &ops[i];
			fprintf(f, "  %4d: %016llx  %-8s", i, (unsigned long long)o->raw, opname(o->op));
			if (o->op == OP_UNSUPPORTED) { unk++; fprintf(f, "  <-- unsupported encoding"); }
			fprintf(f, "\n");
		}
	}
	return unk;
}
void usse_debug_header(uint32_t prog_addr, void *out_FILE) {
	FILE *f = (FILE *)out_FILE;
	uint32_t flags = RD32(prog_addr + H_FLAGS);
	uint32_t s0 = RD32(prog_addr + H_SEC_OFF), s1 = RD32(prog_addr + H_SEC_OFF_END);
	uint32_t sec_instr = (s1 + 4 >= s0) ? (s1 + 4 - s0) / 8 : 0;   /* as usse_get: the two offsets are relative to their own fields */
	fprintf(f, "GXP @ %#x: %s size=%u params=%u primary_instr=%u secondary_instr=%u temp1=%u temp2=%u pa=%u sa=%u\n",
	        prog_addr, (flags & 1) ? "fragment" : "vertex", RD32(prog_addr + 8), RD32(prog_addr + H_PARAM_COUNT),
	        RD32(prog_addr + H_PRIM_INSTR_COUNT), sec_instr,
	        RD32(prog_addr + H_TEMP_REGS1), (uint32_t)RD16(prog_addr + H_TEMP_REGS2), (uint32_t)RD16(prog_addr + H_PRIM_REGS), (uint32_t)RD16(prog_addr + H_SEC_REGS));
}
