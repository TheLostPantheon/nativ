/*
 * Shared between usse.c (the decoder+interpreter) and usse_spirv.c (the GPU translator): the
 * decoded micro-op representation (UOp/UOperand) and the small enums/tables that give its fields
 * meaning. usse.c decodes a GXP's instructions into this once per program (see usse_get); the
 * interpreter (usse.c) and the SPIR-V translator (usse_spirv.c) are two different ways of
 * executing the SAME decoded array -- one on the CPU per vertex/fragment instance, the other
 * compiled once into a GPU shader -- so they share this header rather than keep two decoders.
 * Not part of usse.h's public API: usse_dis.c and gfx_soft.c only need the opaque UsseProgram
 * and usse.h's accessors; this header is for the two things that actually read UOp fields.
 */
#ifndef RECOMP_USSE_INTERNAL_H
#define RECOMP_USSE_INTERNAL_H
#include <stdint.h>
#include "usse.h"

enum {
	BK_TEMP, BK_PA, BK_SA, BK_O, BK_FPI, BK_FPCONST, BK_IMM, BK_IDX, BK_IDXD1, BK_IDXD2, BK_GLOBAL, BK_SPECIAL, BK_NONE
};
enum { TY_U8, TY_S8, TY_O8, TY_U16, TY_S16, TY_F16, TY_F32, TY_C10, TY_U32, TY_S32 };

typedef struct {
	uint8_t bank, type, negabs;   /* negabs: bit0 neg, bit1 abs */
	uint8_t swiz[4];                /* 0 x,1 y,2 z,3 w,4 '0',5 '1',6 '2',7 'H' */
	uint16_t num;                     /* resolved word (normal banks), FPCONST index, IMMEDIATE value, or raw 7-bit split (IDXD1/2) */
} UOperand;

enum {
	OP_NOP = 0, OP_VMAD2, OP_VNMAD, OP_VMAD, OP_VDP, OP_VDUAL, OP_VCOMP, OP_VMOV, OP_VPCK, OP_VTST, OP_VTSTMSK,
	OP_VBW, OP_SOP2, OP_SOP2M, OP_SOP3, OP_I8MAD, OP_I16MAD, OP_I32MAD, OP_I32MAD2, OP_SMP, OP_BR,
	OP_SMLSI, OP_SMBO, OP_KILL, OP_LIMM, OP_DEPTHF, OP_LDST, OP_UNSUPPORTED
};
/* repeat modes (§2.12) */
enum { RM_EXTERNAL, RM_INTERNAL, RM_BOTH, RM_SLMSI };
/* VDUAL op table (§3.5) */
enum { DU_VMAD, DU_VDP, DU_VSSQ, DU_VMUL, DU_VADD, DU_VMOV, DU_FRSQ, DU_FRCP, DU_FMAD, DU_FADD, DU_FMUL, DU_FSUBFLR, DU_FEXP, DU_FLOG, DU_INVALID };

typedef struct {
	uint8_t op;
	uint8_t pred_kind, pred_reg;    /* 0 always, 1 pN, 2 !pN; reg 0-3 */
	uint8_t mask;                    /* 4-bit write mask, already bank-rewritten where §2.9 applies */
	uint8_t reps, repmode;
	uint8_t sub, sub2, sub3;          /* opcode-specific small selectors (op2, move_type, dim, ...) */
	UOperand dst, s0, s1, s2;
	int32_t imm;
	int32_t args[4];                   /* SMLSI/SMBO tables (packed inc/mode), BR target */
	uint64_t raw;
} UOp;

/* FPCONSTANT ROM (§2.7): 64 entries, (low word, high word); a translator can emit these as plain
 * SPIR-V constants (the ROM is fixed, not draw state) instead of modelling the bank at all. */
static const uint32_t usse_fpconst[64][2] = {
	{0x00000000,0x00000000},{0x00000000,0x3F800000},{0x3F800000,0x00000000},{0x3F800000,0x3F800000},
	{0x40000000,0x40800000},{0x41000000,0x41800000},{0x42000000,0x42800000},{0x43000000,0x43800000},
	{0x44000000,0x44800000},{0x45000000,0x45800000},{0x46000000,0x46800000},{0x47000000,0x47800000},
	{0x3F000000,0x3E800000},{0x3E000000,0x3D800000},{0x3D000000,0x3C800000},{0x3C000000,0x3B800000},
	{0x3B000000,0x3A800000},{0x3A000000,0x39800000},{0x39000000,0x38800000},{0x38000000,0x37800000},
	{0x402DF854,0x3EBC5AB2},{0x3FB504F3,0x3F3504F3},{0x40490FDB,0x3FC90FDB},{0x3F490FDB,0x3EC90FDB},
	{0x40C90FDB,0x41490FDB},{0x41C90FDB,0x00000000},{0x37800000,0x38000000},{0x37800080,0x38000100},
	{0x35D00D01,0x37B60B61},{0x39888889,0x3B2AAAAB},{0x3CAAAAAB,0x3E000000},{0x3F000000,0x3F800000},
	{0x00000000,0x3C000000},{0x00000000,0x00000000},{0x3C003C00,0x3C003C00},{0x44004000,0x4C004800},
	{0x54005000,0x5C005800},{0x64006000,0x6C006800},{0x74007000,0x00007800},{0x34003800,0x2C003000},
	{0x24002800,0x1C002000},{0x14001800,0x0C001000},{0x04000800,0x00000000},{0x35E2416F,0x00000000},
	{0x39A83DA8,0x00000000},{0x3E484248,0x36483A48},{0x4A484648,0x00004E48},{0x00000000,0x00000000},
	{0x00000000,0x19550C44},{0x30002555,0x3C003800},{0x00000000,0x00000000},{0x00000000,0x00000000},
	{0x00000000,0x00000000},{0x00000000,0x00000000},{0x00000000,0x00000000},{0x00000000,0x00000000},
	{0xFFFFFFFF,0x00000000},{0xFFFFFFFF,0x00000000},{0xFFFFFFFF,0x00000000},{0xFFFFFFFF,0x00000000},
	{0x7FFF7FFF,0x00000000},{0x7FFF7FFF,0x00000000},{0x7FFF7FFF,0x00000000},{0x7FFF7FFF,0x00000000},
};

/* the decoded micro-op array for a program's primary (vertex/fragment) or secondary (uniform
 * setup) code, straight from usse_get's cache -- the same array usse_run_primary/secondary walk. */
const UOp *usse_program_code(const UsseProgram *p, int secondary, int *count);

/* VDUAL (§3.5) has more operand roles than a UOp has slots -- a unified-store source (US, any
 * bank), three internal-register sources (I0-I2), a unified and an internal destination -- so its
 * UOp keeps only the raw word and both executors take it apart with this one decoder (usse.c's
 * exec_vdual and usse_spirv.c's emit_vdual: they can't disagree on an operand). */
enum { DUS_US, DUS_I0, DUS_I1, DUS_I2 };
typedef struct {
	int op1, op2;             /* DU_*; DU_INVALID: that op writes nothing */
	int ncomp;                /* vector ops' lane count: 4 (comp_count_type) or 3 */
	UOperand src[4];          /* DUS_US..DUS_I2, type F16 or F32 */
	int8_t s1[3], s2[3];      /* each op's sources as DUS_* indices; -1: the layout table has no entry, the source reads as 0.0 */
	UOperand d1, d2;          /* op1's and op2's destinations (one is the unified, the other the internal) */
	int m1, m2;               /* their write masks, final (scalar ops: lane x only; an invalid op: 0) */
} UsseDual;
void usse_vdual_decode(const UOp *op, UsseDual *v);
#endif
