/*
 * SPIR-V module builder + USSE-to-SPIR-V translator. See usse_spirv.h for scope/coverage.
 *
 * Builder design: a SPIR-V module is just words; this emits them straight into a handful of
 * growable buffers (one per logical section the spec requires in order -- capabilities, ext
 * inst imports, memory model, entry point, execution modes, decorations, types/constants/global
 * variables, then the one function's body) and concatenates them at the end. Types, constants
 * and pointer types are cached (SPIR-V requires each to be declared exactly once); everything
 * else is id-at-a-time with no further optimisation (no CSE across the translated program --
 * the single-pass, bounded-size shaders this is used for don't need it; if that stops being true,
 * generate-then-strip in spirv-opt is a far better place for that than here).
 *
 * Register file: every USSE bank the translator supports (TEMP, PA, O, SA, FPINTERNAL) becomes a
 * flat `Private`-storage array of 32-bit words, sized and zero-initialised exactly like
 * UsseInst/UsseDraw (OpVariable's initialiser operand, not an explicit zeroing loop -- `Private`
 * is per-invocation, so this is the same "caller zeroes it first" contract usse_run_primary
 * documents). A UOp's bank/num/swizzle/type are resolved at *translate* time (they're decoded,
 * compile-time-constant properties of the program) except INDEXED1/2 addressing, which needs a
 * runtime array index: the index registers idx1/idx2 are one more Private word array (var_idx),
 * and an INDEXED operand becomes an OpAccessChain whose index is computed from it (see idx_ptr).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "usse_internal.h"
#include "usse_spirv.h"

/* ---------------------------------------------------------------- SPIR-V opcodes/constants used */
enum {
	SpvOpExtInstImport = 11, SpvOpExtInst = 12, SpvOpMemoryModel = 14, SpvOpEntryPoint = 15,
	SpvOpExecutionMode = 16, SpvOpCapability = 17, SpvOpTypeVoid = 19, SpvOpTypeBool = 20,
	SpvOpTypeInt = 21, SpvOpTypeFloat = 22, SpvOpTypeVector = 23, SpvOpTypeImage = 25,
	SpvOpTypeSampledImage = 27, SpvOpTypeArray = 28, SpvOpTypePointer = 32, SpvOpTypeFunction = 33,
	SpvOpConstantTrue = 41, SpvOpConstantFalse = 42, SpvOpConstant = 43, SpvOpConstantComposite = 44,
	SpvOpConstantNull = 46, SpvOpFunction = 54, SpvOpFunctionEnd = 56, SpvOpVariable = 59,
	SpvOpLoad = 61, SpvOpStore = 62, SpvOpAccessChain = 65, SpvOpDecorate = 71, SpvOpMemberDecorate = 72,
	SpvOpVectorShuffle = 79, SpvOpCompositeConstruct = 80, SpvOpCompositeExtract = 81,
	SpvOpConvertFToU = 109, SpvOpConvertFToS = 110, SpvOpConvertSToF = 111, SpvOpConvertUToF = 112,
	SpvOpBitcast = 124, SpvOpFNegate = 127, SpvOpIAdd = 128, SpvOpFAdd = 129, SpvOpISub = 130,
	SpvOpFSub = 131, SpvOpIMul = 132, SpvOpFMul = 133, SpvOpFDiv = 136, SpvOpDot = 148,
	SpvOpShiftRightLogical = 194, SpvOpShiftLeftLogical = 196, SpvOpBitwiseOr = 197,
	SpvOpBitwiseAnd = 199, SpvOpBitFieldInsert = 201, SpvOpBitFieldUExtract = 203,
	SpvOpSelect = 169, SpvOpIEqual = 170, SpvOpINotEqual = 171, SpvOpFOrdLessThan = 184,
	SpvOpFOrdGreaterThanEqual = 190, SpvOpLabel = 248, SpvOpReturn = 253, SpvOpKill = 252,
	SpvOpImageSampleImplicitLod = 87, SpvOpImageSampleExplicitLod = 88, SpvOpUGreaterThanEqual = 174,
	SpvOpFOrdEqual = 180, SpvOpFOrdNotEqual = 182, SpvOpFOrdLessThanEqual = 188, SpvOpFOrdGreaterThan = 186,
	SpvOpLogicalOr = 166, SpvOpLogicalAnd = 167, SpvOpLogicalNot = 168, SpvOpBitwiseXor = 198, SpvOpINotEqualB = 171,
	SpvOpSLessThan = 177, SpvOpSLessThanEqual = 179, SpvOpSGreaterThanEqual = 175,
	SpvOpBranch = 249, SpvOpBranchConditional = 250, SpvOpSelectionMerge = 247,
	SpvOpSNegate = 126, SpvOpShiftRightArithmetic = 195, SpvOpNot = 200, SpvOpBitFieldSExtract = 202,
	SpvOpULessThan = 176, SpvOpUGreaterThan = 172, SpvOpUMod = 137,
	SpvOpImage = 100, SpvOpImageFetch = 95, SpvOpImageGather = 96, SpvOpImageQuerySizeLod = 103, SpvOpDPdx = 207, SpvOpDPdy = 208,
	SpvOpImageRead = 98,
};
enum { SpvStorageUniformConstant = 0, SpvStorageInput = 1, SpvStorageUniform = 2, SpvStorageOutput = 3, SpvStoragePrivate = 6 };
enum { SpvExecModelVertex = 0, SpvExecModelFragment = 4 };
enum { SpvDecorationLocation = 30, SpvDecorationBinding = 33, SpvDecorationDescriptorSet = 34,
       SpvDecorationBuiltIn = 11, SpvDecorationFlat = 14, SpvDecorationBlock = 2, SpvDecorationOffset = 35, SpvDecorationInputAttachmentIndex = 43 };
enum { SpvBuiltInPosition = 0, SpvBuiltInVertexIndex = 42, SpvBuiltInFragDepth = 22, SpvBuiltInFragCoord = 15 };
enum { GLSLRoundEven = 2, GLSLFAbs = 4, GLSLFloor = 8, GLSLFClamp = 43, GLSLLog2 = 30, GLSLExp2 = 29, GLSLSqrt = 31, GLSLInverseSqrt = 32, GLSLFMin = 37, GLSLFMax = 40, GLSLPow = 26 };

/* ---------------------------------------------------------------- growable word buffer */
typedef struct { uint32_t *w; uint32_t n, cap; } Buf;
static void buf_reserve(Buf *b, uint32_t more) {
	if (b->n + more <= b->cap) return;
	b->cap = b->cap ? b->cap * 2 : 64; if (b->cap < b->n + more) b->cap = b->n + more;
	b->w = realloc(b->w, b->cap * 4);
}
static void buf_push(Buf *b, uint32_t v) { buf_reserve(b, 1); b->w[b->n++] = v; }
/* one instruction: opcode, then `n` operand words (NOT including the opcode/word-count word) */
static void ins(Buf *b, int op, int n, const uint32_t *args) {
	buf_reserve(b, (uint32_t)n + 1);
	b->w[b->n++] = (uint32_t)(((n + 1) << 16) | op);
	for (int i = 0; i < n; i++) b->w[b->n++] = args[i];
}
#define INS(b, op, ...) do { uint32_t _a[] = {__VA_ARGS__, 0}; ins(b, op, (int)(sizeof(_a) / 4) - 1, _a); } while (0)
#define INS0(b, op) ins(b, op, 0, 0)

/* ---------------------------------------------------------------- the builder */
#define MAXTYPES 256
typedef struct {
	uint32_t next_id;
	Buf caps, ext, memmodel, entry, execmodes, decos, types, func;
	uint32_t glsl_ext;
	uint32_t t_void, t_bool, t_float, t_uint, t_int, t_fn_void;
	/* cache: kind 'f'=float vec, 'u'=uint vec, 'i'=int vec, 'p'=pointer(storage,pointee), 'a'=array(elem,len) */
	struct { char kind; uint32_t a, b; uint32_t id; } cache[MAXTYPES]; int ncache;
	struct { uint32_t bits; uint32_t id; } fconsts[256]; int nfconst;   /* OpConstant float, keyed by bit pattern */
	struct { uint32_t v; uint32_t id; } uconsts[256]; int nuconst;       /* OpConstant uint */
	struct { int32_t v; uint32_t id; } iconsts[256]; int niconst;        /* OpConstant int */
} Spv;
static uint32_t nid(Spv *s) { return s->next_id++; }

static uint32_t cache_find(Spv *s, char kind, uint32_t a, uint32_t b) {
	for (int i = 0; i < s->ncache; i++) if (s->cache[i].kind == kind && s->cache[i].a == a && s->cache[i].b == b) return s->cache[i].id;
	return 0;
}
static void cache_put(Spv *s, char kind, uint32_t a, uint32_t b, uint32_t id) {
	if (s->ncache < MAXTYPES) s->cache[s->ncache++] = (typeof(s->cache[0])){kind, a, b, id};
}
static uint32_t t_void(Spv *s) { if (!s->t_void) { s->t_void = nid(s); INS(&s->types, SpvOpTypeVoid, s->t_void); } return s->t_void; }
static uint32_t t_bool(Spv *s) { if (!s->t_bool) { s->t_bool = nid(s); INS(&s->types, SpvOpTypeBool, s->t_bool); } return s->t_bool; }
static uint32_t t_float(Spv *s) { if (!s->t_float) { s->t_float = nid(s); INS(&s->types, SpvOpTypeFloat, s->t_float, 32); } return s->t_float; }
static uint32_t t_uint(Spv *s) { if (!s->t_uint) { s->t_uint = nid(s); INS(&s->types, SpvOpTypeInt, s->t_uint, 32, 0); } return s->t_uint; }
static uint32_t t_int(Spv *s) { if (!s->t_int) { s->t_int = nid(s); INS(&s->types, SpvOpTypeInt, s->t_int, 32, 1); } return s->t_int; }
static uint32_t t_vec(Spv *s, uint32_t comp, int n) {
	uint32_t id = cache_find(s, 'V', comp, (uint32_t)n); if (id) return id;
	id = nid(s); INS(&s->types, 23 /*OpTypeVector*/, id, comp, (uint32_t)n); cache_put(s, 'V', comp, (uint32_t)n, id); return id;
}
static uint32_t t_ptr(Spv *s, uint32_t storage, uint32_t pointee) {
	uint32_t id = cache_find(s, 'P', storage, pointee); if (id) return id;
	id = nid(s); INS(&s->types, SpvOpTypePointer, id, storage, pointee); cache_put(s, 'P', storage, pointee, id); return id;
}
static uint32_t t_array(Spv *s, uint32_t elem, uint32_t len_id, uint32_t len_val) {
	uint32_t id = cache_find(s, 'A', elem, len_val); if (id) return id;
	id = nid(s); INS(&s->types, SpvOpTypeArray, id, elem, len_id); cache_put(s, 'A', elem, len_val, id); return id;
}
static uint32_t c_float(Spv *s, float v) {
	uint32_t bits; memcpy(&bits, &v, 4);
	for (int i = 0; i < s->nfconst; i++) if (s->fconsts[i].bits == bits) return s->fconsts[i].id;
	uint32_t id = nid(s); INS(&s->types, SpvOpConstant, t_float(s), id, bits);
	if (s->nfconst < 256) s->fconsts[s->nfconst++] = (typeof(s->fconsts[0])){bits, id};
	return id;
}
static uint32_t c_uint(Spv *s, uint32_t v) {
	for (int i = 0; i < s->nuconst; i++) if (s->uconsts[i].v == v) return s->uconsts[i].id;
	uint32_t id = nid(s); INS(&s->types, SpvOpConstant, t_uint(s), id, v);
	if (s->nuconst < 256) s->uconsts[s->nuconst++] = (typeof(s->uconsts[0])){v, id};
	return id;
}
static uint32_t c_int(Spv *s, int32_t v) {
	for (int i = 0; i < s->niconst; i++) if (s->iconsts[i].v == v) return s->iconsts[i].id;
	uint32_t id = nid(s); INS(&s->types, SpvOpConstant, t_int(s), id, (uint32_t)v);
	if (s->niconst < 256) s->iconsts[s->niconst++] = (typeof(s->iconsts[0])){v, id};
	return id;
}
static uint32_t global_var(Spv *s, uint32_t ptr_type, uint32_t storage, uint32_t init_id /*0 if none*/) {
	uint32_t id = nid(s);
	if (init_id) INS(&s->types, SpvOpVariable, ptr_type, id, storage, init_id);
	else INS(&s->types, SpvOpVariable, ptr_type, id, storage);
	return id;
}
/* every argument here is always an SPIR-V <id> (even BitFieldUExtract/Insert's offset/count,
 * which reference an OpConstant, not a raw literal word) -- a 0 among them is always a bug
 * (id 0 doesn't exist), so these assert instead of silently emitting a corrupt module. */
static void assert_ids(const char *where, uint32_t type, uint32_t a0, uint32_t a1, uint32_t a2) {
	if (!type || !a0 || !a1 || !a2) { fprintf(stderr, "usse_spirv: internal error: zero id in %s (type=%u a0=%u a1=%u a2=%u)\n", where, type, a0, a1, a2); abort(); }
}
static uint32_t result1(Spv *s, Buf *b, int op, uint32_t type, uint32_t a0) { assert_ids("result1", type, a0, 1, 1); uint32_t id = nid(s); INS(b, op, type, id, a0); return id; }
static uint32_t result2(Spv *s, Buf *b, int op, uint32_t type, uint32_t a0, uint32_t a1) { assert_ids("result2", type, a0, a1, 1); uint32_t id = nid(s); INS(b, op, type, id, a0, a1); return id; }
static uint32_t result3(Spv *s, Buf *b, int op, uint32_t type, uint32_t a0, uint32_t a1, uint32_t a2) { assert_ids("result3", type, a0, a1, a2); uint32_t id = nid(s); INS(b, op, type, id, a0, a1, a2); return id; }
static uint32_t glsl1(Spv *s, uint32_t inst, uint32_t a0) { return result3(s, &s->func, SpvOpExtInst, t_float(s), s->glsl_ext, inst, a0); }
static uint32_t glsl2(Spv *s, uint32_t inst, uint32_t a0, uint32_t a1) {
	uint32_t id = nid(s); INS(&s->func, SpvOpExtInst, t_float(s), id, s->glsl_ext, inst, a0, a1); return id;
}
static uint32_t fop2(Spv *s, int op, uint32_t a, uint32_t b) { return result2(s, &s->func, op, t_float(s), a, b); }
static uint32_t uop2(Spv *s, int op, uint32_t a, uint32_t b) { return result2(s, &s->func, op, t_uint(s), a, b); }
static uint32_t load(Spv *s, uint32_t type, uint32_t ptr) { return result1(s, &s->func, SpvOpLoad, type, ptr); }
static void store(Spv *s, uint32_t ptr, uint32_t val) { INS(&s->func, SpvOpStore, ptr, val); }
static uint32_t access1(Spv *s, uint32_t ptr_type, uint32_t base, uint32_t idx) { return result2(s, &s->func, SpvOpAccessChain, ptr_type, base, idx); }
static uint32_t access2(Spv *s, uint32_t ptr_type, uint32_t base, uint32_t idx0, uint32_t idx1) { return result3(s, &s->func, SpvOpAccessChain, ptr_type, base, idx0, idx1); }
/* OpCompositeExtract's last operand is a raw literal index, not an <id> -- it can legitimately be
 * 0, so this goes around result1/2/3's nonzero-id assertion (unlike their other operands, which
 * really are always ids: see the comment by assert_ids) */
static uint32_t composite_extract1(Spv *s, uint32_t type, uint32_t composite, uint32_t literal_index) {
	uint32_t id = nid(s); INS(&s->func, SpvOpCompositeExtract, type, id, composite, literal_index); return id;
}

/* ---------------------------------------------------------------- register file */
/* banks this pass models: TEMP/PA/O (160 words each, matching UsseInst), SA (256 words, read-only,
 * from the uniform buffer, matching UsseDraw), FPINTERNAL (16 words, matching UsseInst.i) */
typedef struct {
	Spv s;
	uint32_t ptr_priv_u, ptr_sa_u;             /* Private uint*, Uniform-block uint* (SA) */
	uint32_t var_r, var_pa, var_o, var_i;        /* Private arrays */
	uint32_t var_sa;                               /* Uniform block instance (SA) */
	uint32_t var_p;                                 /* Private bool[4] predicates (always false: VTST/VTSTMSK unsupported here) */
	uint32_t var_idx;                                /* Private uint[2] index registers idx1/idx2 (UsseInst.idx), zero-initialised */
	uint32_t smp_sampler[16];                          /* UniformConstant sampled-image vars actually used, 0 if unused */
	uint32_t t_simg, t_simg_cube;                        /* their OpTypeSampledImage (2D, and a 6-layer 2D array for cube units), to OpLoad through */
	int smp_units[16], nsmp;
	uint32_t cube_mask;                                  /* units that are cube maps (see usse_spirv.h) */
	uint32_t snap_mask;                                  /* units read at their 1x pixel centres (see usse_spirv.h, snap_fetch) */
	int cap_query;                                       /* Capability ImageQuery declared (SMP's gather weights) */
	int is_fragment;
	const UsseVertexLayout *vl; const UsseFragLayout *fl;
	int color_kind;   /* the target's texture family, for raw-word colour (see usse_spirv.h) */
	uint32_t var_pos;                 /* vertex: Output vec4 gl_Position */
	uint32_t var_vary[16];             /* vertex: Output vec4 per Location; fragment: Input vec4 per Location (word group (loc*4+4)) */
	int nvary;
	uint32_t var_frag_color;            /* fragment: Output vec4 colour */
	uint32_t var_frag_coord, fetch_var, t_img2d, t_subpass; int fetch;   /* fetch: 0 none, 1 the sampled framebuffer copy, 2 the input attachment (framebuffer fetch) -- usse_spirv_set_fetch_mode; 3 zeros, 4 zeros then ones in a second run (usse_spirv_set_fetch_zero) */
	char why[120];       /* the first reason this program was given up on, for SpirvModule.err */
	uint32_t var_vprobe; /* RECOMP_PROBE=T|P|O... (vertex mode): the vertex's snapshot travels to the fragment as a flat varying at Location nvary */
	uint32_t probe_id;   /* RECOMP_PROBE ...@k: the register word loaded after primary instruction k (emit_frag_epilogue shows it) */
	int pcf_unit; uint32_t pcf_du, pcf_dv;
	int points; uint32_t var_point;   /* usse_spirv_set_points: the point-sprite variant, and its sprite-coordinate varying (vertex Output, fragment Input) */   /* usse_spirv_set_pcf: the shadow map's unit (-1 none) and this tap's UV offset (float ids, 0 for no offset) */
	/* FRAGCOLOR_USED: gl_FragCoord, the framebuffer copy's sampled image, the 2D image type */
	uint32_t var_frag_depth;              /* fragment: Output float FragDepth, if used */
	int uses_depth;
	uint32_t var_win; int nwin; int win_sa[8];   /* vertex LDST: storage buffer 1 holds nwin windows of guest memory, one per SA pointer word (see ldst_pointer_sa) */
	uint32_t ldst_img[8];                        /* fragment LDST: window w's render target, a sampled image (emit_ldst_target) */
	uint32_t var_vin, ptr_vin_u, vindex;   /* vertex: PA input SSBO, its element-pointer type, loaded (uint) gl_VertexIndex */
	int motion, pos_only; uint32_t var_sa_prev, var_mv_cur, var_mv_prev;   /* usse_spirv_set_motion: the previous frame's SA block, the two positions'
	                                                                        * outputs; pos_only: the epilogue writes only the position, to var_mv_prev */
	int8_t inc[4][17];   /* RM_SLMSI state (SMLSI/SMBO, §2.12): entirely a function of their own
	                       * immediate operands, not of any register value, so (with no branches in
	                       * this pass's coverage -- BR is unsupported) it's fully resolved by
	                       * simulating SMLSI/SMBO in program order AT TRANSLATE TIME, same as
	                       * usse.c's run_program does at run time (see slot_off) */
} Gen;

static uint32_t const_null_array(Spv *s, uint32_t arr_type) { uint32_t id = nid(s); INS(&s->types, SpvOpConstantNull, arr_type, id); return id; }

/* ---------------------------------------------------------------- operand resolution (mirrors usse.c's resolve_addr/read_f32/...) */
typedef struct { int ok; uint32_t ptr; uint32_t elem_type; } RegPtr;   /* a pointer to one WORD (uint) of a bank array */

/* RECOMP_GPU_F16=exact|masked|native: how F16 register lanes convert to and from float. The USSE keeps
 * F16 lanes packed two to a word, so every F16 operand read and every F16 write converts; done in
 * software (the select chains below) that was most of the GPU's time in Unit 13's lighting at render
 * scale 3 (measured 2026-10-09, same session: 19.7 ms/frame with the software pair, 6.9 with native
 * conversions -- the light-volume shader's MSL 107 KB -> 27 KB). `exact`: the bit-exact software pair,
 * usse.c's half_to_float/float_to_half ported. `masked` (default): the GPU's own conversions, kept
 * exact -- a half widens exactly in hardware, and usse.c's float_to_half TRUNCATES (round toward
 * zero, denormals kept, below 2^-24 -> signed zero), so the float's low mantissa bits are cleared
 * first (13, or more for the denormal range: the value is then exactly representable and the
 * hardware's rounding mode never matters); overflow and NaN/infinity keep their explicit encodings.
 * `native`: the GPU's conversions as they are (round to nearest even): not the interpreter's bits. */
/* f16_lean (usse_spirv_set_f16_lean; Metal on Apple silicon): `masked`'s overflow and NaN cases left to the GPU's own
 * conversion -- which gives exactly usse.c's encodings there (2^16 and up to infinity, denormals as the mask leaves them)
 * except for NaNs (a NaN whose payload the mask clears comes out infinity; every NaN comes out +0x7e00), so a NaN gets its
 * quiet bit set before the mask and its sign ORed back after -- integer arithmetic, NO select: bit-exact over all 2^32
 * inputs on this Mac's GPU (2026-10-20, a full sweep against the masked form). Metal's back end is slow on selects:
 * Killzone's 1.4 MB fragment shaders built their pipelines in ~8 s with the masked form's nested ones, ~5.5 s with one
 * select a conversion, 0.28 s with none (library 2.3 -> 1.6 s) -- the stalls entering new areas. Not on Vulkan: there
 * the conversion's overflow is the implementation's choice. */
static int f16_lean;
void usse_spirv_set_f16_lean(int on) { f16_lean = on != 0; }
/* canonical varyings (usse_spirv_set_canonical_varyings): Location k holds semantic k -- TEXCOORD0-9, COLOR0, COLOR1, FOG
 * (USSE_CANON_VARY) -- instead of the vertex program's O words in order, so a fragment module no longer depends on its
 * vertex program. The count of components the vertex program writes for each arrives per draw (usse_spirv_vary_ncomp) */
static int canon_vary;
void usse_spirv_set_canonical_varyings(int on) { canon_vary = on != 0; }
static int vary_sem(const UsseVertexLayout *vl, int k, int *ncomp) {   /* semantic k's first O word in a vertex program, -1 if it has none */
	int base = -1, n = 0;
	if (vl) { if (k < 10) { base = vl->o_tex[k]; n = vl->ncomp_tex[k]; } else if (k == 10) { base = vl->o_color0; n = 4; } else if (k == 11) { base = vl->o_color1; n = 4; } else if (k == 12) { base = vl->o_fog; n = 2; } }
	if (base < 0) n = 0;
	*ncomp = n < 0 ? 0 : n > 4 ? 4 : n;
	return base;
}
uint32_t usse_spirv_vary_ncomp(const UsseVertexLayout *vl, int word) {
	uint32_t w = 0; const int k0 = word ? 10 : 0, k1 = word ? USSE_CANON_VARY : 10;
	for (int k = k0; k < k1; k++) { int n; vary_sem(vl, k, &n); w |= (uint32_t)n << (3 * (k - k0)); }
	return w;
}
static int f16_mode(void) {
	static int m = -1;
	if (m < 0) { const char *e = getenv("RECOMP_GPU_F16"); m = !e || !*e || !strcmp(e, "masked") ? 1 : !strcmp(e, "native") ? 2 : 0; }
	return m;
}
static uint32_t half_to_float_spv(Spv *s, uint32_t h_u32) {
	if (f16_mode()) {   /* GLSL.std.450 UnpackHalf2x16: the low half of the word, exact */
		uint32_t v2 = result3(s, &s->func, SpvOpExtInst, t_vec(s, t_float(s), 2), s->glsl_ext, 62 /*UnpackHalf2x16*/, h_u32);
		return composite_extract1(s, t_float(s), v2, 0);
	}
	/* port of usse.c's half_to_float, bit for bit: sign/exp/mant split and its three cases, picked
	 * with OpSelect. h_u32 holds the 16-bit half value in its low bits. The normal-number formula
	 * alone (all this did before) read a zero half as 2^-15 and a denormal as a normal number:
	 * Killzone: Mercenary's fragment 0x112ef460 keeps a VDUAL F16 product in an internal register
	 * that comes out denormal along one column of its quad, and the register probe of the next
	 * instruction disagreed there; every F16 zero a program read was 3.05e-5 instead of 0. */
	uint32_t sign = uop2(s, SpvOpShiftLeftLogical, uop2(s, SpvOpBitwiseAnd, h_u32, c_uint(s, 0x8000)), c_uint(s, 16));
	uint32_t exp10 = result3(s, &s->func, SpvOpBitFieldUExtract, t_uint(s), h_u32, c_uint(s, 10), c_uint(s, 5));
	uint32_t man = uop2(s, SpvOpBitwiseAnd, h_u32, c_uint(s, 0x3ff));
	/* normal: sign | ((exp10+112)<<23) | (man<<13) */
	uint32_t norm_bits = uop2(s, SpvOpBitwiseOr, sign,
		uop2(s, SpvOpBitwiseOr, uop2(s, SpvOpShiftLeftLogical, uop2(s, SpvOpIAdd, exp10, c_uint(s, 112)), c_uint(s, 23)),
		     uop2(s, SpvOpShiftLeftLogical, man, c_uint(s, 13))));
	/* zero and denormal (exp10 0): man * 2^-24, exact in F32 (the number the interpreter's
	 * normalising loop builds), with the sign -- +-0 for man 0 */
	uint32_t sub = uop2(s, SpvOpBitwiseOr, sign, result1(s, &s->func, SpvOpBitcast, t_uint(s),
		fop2(s, SpvOpFMul, result1(s, &s->func, SpvOpConvertUToF, t_float(s), man), c_float(s, 1.0f / 16777216.0f))));
	/* infinity and NaN (exp10 31): sign | 0x7f800000 | (man<<13) */
	uint32_t naninf = uop2(s, SpvOpBitwiseOr, sign, uop2(s, SpvOpBitwiseOr, c_uint(s, 0x7f800000u), uop2(s, SpvOpShiftLeftLogical, man, c_uint(s, 13))));
	uint32_t is_sub = result2(s, &s->func, SpvOpIEqual, t_bool(s), exp10, c_uint(s, 0)), is_top = result2(s, &s->func, SpvOpIEqual, t_bool(s), exp10, c_uint(s, 31));
	uint32_t bits = result3(s, &s->func, SpvOpSelect, t_uint(s), is_sub, sub, result3(s, &s->func, SpvOpSelect, t_uint(s), is_top, naninf, norm_bits));
	return result1(s, &s->func, SpvOpBitcast, t_float(s), bits);
}
/* ports usse.c's float_to_half exactly (bit for bit), including the cases the first version of
 * this function skipped: a zero/small value (exp8<113, i.e. ne<=0) does NOT belong in the "normal"
 * formula's shift range -- shifting a negative amount there is undefined and, in practice, a real
 * bug caught live: every zero-valued colour channel (black or zero alpha, extremely common) packed
 * to half as 0x4000 (2.0), turning black/transparent texels white once draws finally reached the
 * screen. Denormals, flush-to-zero, overflow-to-infinity and NaN's quiet bit are all handled now,
 * via OpSelect instead of real branches (this pass stays single-basic-block throughout). */
static uint32_t float_to_half_spv(Spv *s, uint32_t f) {
	if (f16_mode()) {
		uint32_t bits = result1(s, &s->func, SpvOpBitcast, t_uint(s), f), src = f;
		uint32_t e = result3(s, &s->func, SpvOpBitFieldUExtract, t_uint(s), bits, c_uint(s, 23), c_uint(s, 8));
		uint32_t nanf = 0;   /* (f16_lean: 1 for a NaN, 0 otherwise -- unsigned 0x7f800000 - |bits| goes negative exactly then) */
		if (f16_lean && f16_mode() == 1) {
			nanf = uop2(s, SpvOpShiftRightLogical, uop2(s, SpvOpISub, c_uint(s, 0x7f800000u), uop2(s, SpvOpBitwiseAnd, bits, c_uint(s, 0x7fffffffu))), c_uint(s, 31));
			bits = uop2(s, SpvOpBitwiseOr, bits, uop2(s, SpvOpShiftLeftLogical, nanf, c_uint(s, 22)));   /* a NaN stays one when the mask clears its payload */
		}
		if (f16_mode() == 1) {   /* truncate: clear the mantissa bits a half can't hold -- 13 for a normal half, up to 23 below (shift = clamp(126 - e, 13, 23)) */
			uint32_t sh_i = uop2(s, SpvOpISub, c_int(s, 126), result1(s, &s->func, SpvOpBitcast, t_int(s), e));
			uint32_t sh = nid(s); INS(&s->func, SpvOpExtInst, t_int(s), sh, s->glsl_ext, 45 /*SClamp*/, sh_i, c_int(s, 13), c_int(s, 23));
			uint32_t low = uop2(s, SpvOpISub, uop2(s, SpvOpShiftLeftLogical, c_uint(s, 1), result1(s, &s->func, SpvOpBitcast, t_uint(s), sh)), c_uint(s, 1));
			src = result1(s, &s->func, SpvOpBitcast, t_float(s), uop2(s, SpvOpBitwiseAnd, bits, result1(s, &s->func, SpvOpNot, t_uint(s), low)));
		}
		uint32_t vecid = result2(s, &s->func, SpvOpCompositeConstruct, t_vec(s, t_float(s), 2), src, c_float(s, 0.0f));
		if (f16_lean && f16_mode() == 1) {   /* (the high half packs the 0.0: zero) -- the GPU gives +0x7e00 for every NaN: its sign ORed back */
			uint32_t packed = result3(s, &s->func, SpvOpExtInst, t_uint(s), s->glsl_ext, 58 /*PackHalf2x16*/, vecid);
			uint32_t sign = uop2(s, SpvOpBitwiseAnd, uop2(s, SpvOpShiftRightLogical, bits, c_uint(s, 16)), c_uint(s, 0x8000u));
			return uop2(s, SpvOpBitwiseOr, packed, uop2(s, SpvOpBitwiseAnd, sign, uop2(s, SpvOpISub, c_uint(s, 0), nanf)));
		}
		uint32_t packed = uop2(s, SpvOpBitwiseAnd, result3(s, &s->func, SpvOpExtInst, t_uint(s), s->glsl_ext, 58 /*PackHalf2x16*/, vecid), c_uint(s, 0xffffu));
		/* 2^16 and up (exponent 143+) and infinity/NaN: usse.c's encodings (infinity; NaN as the quiet 0x7e00 pattern) */
		uint32_t sign = uop2(s, SpvOpShiftRightLogical, uop2(s, SpvOpBitwiseAnd, bits, c_uint(s, 0x80000000u)), c_uint(s, 16));
		uint32_t nan = result2(s, &s->func, SpvOpLogicalAnd, t_bool(s), result2(s, &s->func, SpvOpIEqual, t_bool(s), e, c_uint(s, 255)),
			result2(s, &s->func, SpvOpINotEqual, t_bool(s), uop2(s, SpvOpBitwiseAnd, bits, c_uint(s, 0x7fffffu)), c_uint(s, 0)));
		uint32_t big = uop2(s, SpvOpBitwiseOr, sign, result3(s, &s->func, SpvOpSelect, t_uint(s), nan, c_uint(s, 0x7e00u), c_uint(s, 0x7c00u)));
		return result3(s, &s->func, SpvOpSelect, t_uint(s), result2(s, &s->func, SpvOpUGreaterThanEqual, t_bool(s), e, c_uint(s, 143)), big, packed);
	}
	uint32_t bits = result1(s, &s->func, SpvOpBitcast, t_uint(s), f);
	uint32_t sign = uop2(s, SpvOpShiftRightLogical, uop2(s, SpvOpBitwiseAnd, bits, c_uint(s, 0x80000000u)), c_uint(s, 16));
	uint32_t e = result3(s, &s->func, SpvOpBitFieldUExtract, t_uint(s), bits, c_uint(s, 23), c_uint(s, 8));
	uint32_t mfull = uop2(s, SpvOpBitwiseAnd, bits, c_uint(s, 0x7fffffu));
	uint32_t man10 = result3(s, &s->func, SpvOpBitFieldUExtract, t_uint(s), bits, c_uint(s, 13), c_uint(s, 10));
	uint32_t ne_u = uop2(s, SpvOpISub, e, c_uint(s, 112));
	uint32_t ne_s = result1(s, &s->func, SpvOpBitcast, t_int(s), ne_u);

	uint32_t normal = uop2(s, SpvOpBitwiseOr, sign, uop2(s, SpvOpBitwiseOr, uop2(s, SpvOpShiftLeftLogical, ne_u, c_uint(s, 10)), man10));

	uint32_t shift_amt = result1(s, &s->func, SpvOpBitcast, t_uint(s), uop2(s, SpvOpISub, c_int(s, 14), ne_s));
	uint32_t denormal = uop2(s, SpvOpBitwiseOr, sign, uop2(s, SpvOpShiftRightLogical, uop2(s, SpvOpBitwiseOr, mfull, c_uint(s, 0x800000u)), shift_amt));

	uint32_t is_naninf = result2(s, &s->func, SpvOpIEqual, t_bool(s), e, c_uint(s, 255));
	uint32_t mfull_nz = result2(s, &s->func, SpvOpINotEqual, t_bool(s), mfull, c_uint(s, 0));
	uint32_t is_nan = result2(s, &s->func, SpvOpLogicalAnd, t_bool(s), is_naninf, mfull_nz);
	uint32_t overflow = uop2(s, SpvOpBitwiseOr, sign, uop2(s, SpvOpBitwiseOr, c_uint(s, 0x7c00), result3(s, &s->func, SpvOpSelect, t_uint(s), is_nan, c_uint(s, 0x200), c_uint(s, 0))));

	uint32_t is_flush = result2(s, &s->func, SpvOpSLessThan, t_bool(s), ne_s, c_int(s, -10));
	uint32_t is_overflow = result2(s, &s->func, SpvOpSGreaterThanEqual, t_bool(s), ne_s, c_int(s, 31));
	uint32_t is_le0 = result2(s, &s->func, SpvOpSLessThanEqual, t_bool(s), ne_s, c_int(s, 0));

	uint32_t small = result3(s, &s->func, SpvOpSelect, t_uint(s), is_flush, sign, denormal);   /* ne<=0: flush or denormal */
	uint32_t in_range = result3(s, &s->func, SpvOpSelect, t_uint(s), is_le0, small, normal);     /* ne<=0 vs normal */
	return result3(s, &s->func, SpvOpSelect, t_uint(s), is_overflow, overflow, in_range);        /* ne>=31 (incl. e==255) overrides everything */
}

/* get a Private/Uniform pointer to word `num` (already repeat-stepped) of `bank`; returns ok=0 for
 * banks this pass doesn't model as addressable memory (FPCONST/IMM handled by the caller directly;
 * IDXD1/2 have a runtime address and go through idx_ptr instead -- lane_ptr routes them; GLOBAL and
 * SPECIAL not here) */
static RegPtr bank_ptr(Gen *g, int bank, int num) {
	Spv *s = &g->s; RegPtr r = {0, 0, t_uint(s)};
	uint32_t idx;
	switch (bank) {
	case BK_TEMP: idx = c_uint(s, (uint32_t)num % 160); r.ptr = access1(s, g->ptr_priv_u, g->var_r, idx); r.ok = 1; return r;
	case BK_PA:   /* vertex stage: copied in from the input SSBO once at the top of the shader (see
	               * the prologue in usse_spirv_translate), not read from it directly -- a program
	               * that writes PA (allowed; it's a plain register bank, not read-only hardware
	               * input) must not write into the shared per-vertex SSBO through it */
		idx = c_uint(s, (uint32_t)num % 160); r.ptr = access1(s, g->ptr_priv_u, g->var_pa, idx); r.ok = 1; return r;
	case BK_O:    idx = c_uint(s, (uint32_t)num % 160); r.ptr = access1(s, g->ptr_priv_u, g->var_o, idx); r.ok = 1; return r;
	case BK_FPI:  idx = c_uint(s, (uint32_t)num % 16);  r.ptr = access1(s, g->ptr_priv_u, g->var_i, idx); r.ok = 1; return r;
	case BK_IDX:  /* the index registers themselves (usse.c's bank_read_raw/bank_write_raw): word 1 is
	               * idx1, word 2 idx2. Any other word reads as 0 there and swallows writes -- one
	               * pointer can't be both, so those (no program seen addresses them) stay rejected */
		if (num != 1 && num != 2) break;
		r.ptr = access1(s, g->ptr_priv_u, g->var_idx, c_uint(s, (uint32_t)num - 1)); r.ok = 1; return r;
	case BK_SA: {   /* word n = vector n/4, component n%4 */
		uint32_t n = (uint32_t)num % 256, id = nid(s);
		INS(&s->func, SpvOpAccessChain, g->ptr_sa_u, id, g->var_sa, c_uint(s, 0), c_uint(s, n / 4), c_uint(s, n % 4));
		r.ptr = id; r.ok = 1; return r;
	}
	default: break;
	}
	if (!g->why[0]) snprintf(g->why, sizeof g->why, bank == BK_IDX ? "index register word %d (only idx1/idx2 modelled)" : "register bank %d (special) not modelled", bank == BK_IDX ? num : bank);
	return r;
}
/* INDEXED1/2 operands (usse.c's resolve_indexed + bank_read_raw, §2.4): the decoded 7-bit number
 * splits into a base bank (bits 6:5 -- TEMP, O, PA, SA) and an offset k (bits 4:0), and the word is
 * idxN*2 + k + `add` (the repeat offset and the lane's word, both compile-time) in that bank, wrapped
 * the way bank_read_raw wraps a word number: (unsigned) % 160 for TEMP/PA/O, % 256 for SA. The
 * interpreter does that sum in 32-bit two's complement (an int from a uint32), so a uint IAdd here
 * wraps identically. Unit 13's cascaded-shadow fragment program (0x200d8eec, frame_1174) is the
 * user: it builds idx1 = cascade*8 + 6 with an I16MAD into the INDEX bank, then reads the selected
 * g_mShadowTransform rows out of SA as sa[idx1*2 + k] (raw numbers 98/102/106/110/112: SA, k=2..16). */
static RegPtr idx_ptr(Gen *g, const UOperand *o, int add) {
	static const int basebanks[4] = {BK_TEMP, BK_O, BK_PA, BK_SA};
	Spv *s = &g->s; RegPtr r = {0, 0, t_uint(s)};
	int base = basebanks[(o->num >> 5) & 3], k = o->num & 0x1F;
	uint32_t idxv = load(s, t_uint(s), access1(s, g->ptr_priv_u, g->var_idx, c_uint(s, o->bank == BK_IDXD1 ? 0 : 1)));
	uint32_t w = uop2(s, SpvOpIAdd, uop2(s, SpvOpShiftLeftLogical, idxv, c_uint(s, 1)), c_uint(s, (uint32_t)(k + add)));
	if (base == BK_SA) {   /* word n = vector n/4, component n%4 of the uniform block, as bank_ptr's BK_SA */
		w = uop2(s, SpvOpBitwiseAnd, w, c_uint(s, 255));
		uint32_t vec = uop2(s, SpvOpShiftRightLogical, w, c_uint(s, 2)), comp = uop2(s, SpvOpBitwiseAnd, w, c_uint(s, 3)), id = nid(s);
		INS(&s->func, SpvOpAccessChain, g->ptr_sa_u, id, g->var_sa, c_uint(s, 0), vec, comp);
		r.ptr = id;
	} else r.ptr = access1(s, g->ptr_priv_u, base == BK_TEMP ? g->var_r : base == BK_PA ? g->var_pa : g->var_o, uop2(s, SpvOpUMod, w, c_uint(s, 160)));
	r.ok = 1; return r;
}
static uint32_t bitfield_insert(Spv *s, uint32_t base, uint32_t insert, uint32_t offset, uint32_t count) {
	uint32_t id = nid(s); INS(&s->func, SpvOpBitFieldInsert, t_uint(s), id, base, insert, offset, count); return id;
}
static void write_comp_f16(Gen *g, const UOperand *o, int repeat_off, int comp, uint32_t fval, int *supported);

/* ---- typed register lanes: ports of usse.c's resolve_addr / extract_lane / insert_lane and its four
 * lane<->float conversions, so every register type the interpreter models (F32, F16, C10, U8/S8/O8,
 * U16/S16, U32/S32) reads and writes the same bits here. Registers are uint words throughout. */
static int lpw_of(int type) { switch (type) { case TY_F16: case TY_U16: case TY_S16: return 2; case TY_U8: case TY_S8: case TY_O8: return 4; case TY_C10: return 3; default: return 1; } }
static int lane_bits(int type) { switch (type) { case TY_F16: case TY_U16: case TY_S16: return 16; case TY_U8: case TY_S8: case TY_O8: return 8; case TY_C10: return 10; default: return 32; } }
/* the word holding lane `ch` of operand `o` (FPINTERNAL counts in 4-word registers, like resolve_addr) */
static RegPtr lane_ptr(Gen *g, const UOperand *o, int repeat_off, int ch, int *lane) {
	int lpw = lpw_of(o->type);
	int base = o->bank == BK_FPI ? ((int)o->num + repeat_off) * 4 : (int)o->num + repeat_off;
	*lane = ch % lpw;
	if (o->bank == BK_IDXD1 || o->bank == BK_IDXD2) return idx_ptr(g, o, repeat_off + ch / lpw);   /* resolve_addr's indexed case: no FPI scaling (the base bank is never FPI) */
	return bank_ptr(g, o->bank, base + ch / lpw);
}
static uint32_t extract_lane_spv(Spv *s, uint32_t word, int type, int lane) {
	int bits = lane_bits(type);
	if (bits == 32) return word;
	int sgn = type == TY_S8 || type == TY_S16 || type == TY_C10;
	return result3(s, &s->func, sgn ? SpvOpBitFieldSExtract : SpvOpBitFieldUExtract, t_uint(s), word, c_uint(s, (uint32_t)(lane * bits)), c_uint(s, (uint32_t)bits));
}
static uint32_t lane_to_float_spv(Spv *s, uint32_t raw, int type) {
	switch (type) {
	case TY_F32: return result1(s, &s->func, SpvOpBitcast, t_float(s), raw);
	case TY_F16: return half_to_float_spv(s, raw);
	case TY_C10: return glsl2(s, GLSLFMax, fop2(s, SpvOpFDiv, result1(s, &s->func, SpvOpConvertSToF, t_float(s), result1(s, &s->func, SpvOpBitcast, t_int(s), raw)), c_float(s, 255.0f)), c_float(s, -2.0f));
	case TY_U8: case TY_U16: case TY_U32: case TY_O8: return result1(s, &s->func, SpvOpConvertUToF, t_float(s), raw);
	default: return result1(s, &s->func, SpvOpConvertSToF, t_float(s), result1(s, &s->func, SpvOpBitcast, t_int(s), raw));
	}
}
static uint32_t lane_to_float_norm_spv(Spv *s, uint32_t raw, int type) {
	float div; int sgn;
	switch (type) {
	case TY_U8: div = 255.0f; sgn = 0; break;
	case TY_S8: div = 127.0f; sgn = 1; break;
	case TY_U16: div = 65535.0f; sgn = 0; break;
	case TY_S16: div = 32767.0f; sgn = 1; break;
	case TY_U32: div = 4294967295.0f; sgn = 0; break;
	case TY_S32: div = 2147483647.0f; sgn = 1; break;
	default: return lane_to_float_spv(s, raw, type);   /* float types and C10: no normalization */
	}
	uint32_t v = fop2(s, SpvOpFDiv, lane_to_float_spv(s, raw, type), c_float(s, div));
	return sgn ? glsl2(s, GLSLFMax, v, c_float(s, -1.0f)) : v;
}
static uint32_t f_to_u(Spv *s, uint32_t f) { return result1(s, &s->func, SpvOpConvertFToU, t_uint(s), glsl2(s, GLSLFMax, f, c_float(s, 0.0f))); }
static uint32_t f_to_s(Spv *s, uint32_t f) { return result1(s, &s->func, SpvOpBitcast, t_uint(s), result1(s, &s->func, SpvOpConvertFToS, t_int(s), f)); }
static uint32_t fclamp(Spv *s, uint32_t f, float lo, float hi) {
	uint32_t id = nid(s); INS(&s->func, SpvOpExtInst, t_float(s), id, s->glsl_ext, GLSLFClamp, f, c_float(s, lo), c_float(s, hi)); return id;
}
static uint32_t float_to_lane_spv(Spv *s, uint32_t v, int type) {
	switch (type) {
	case TY_F32: return result1(s, &s->func, SpvOpBitcast, t_uint(s), v);
	case TY_F16: return float_to_half_spv(s, v);
	case TY_C10: return uop2(s, SpvOpBitwiseAnd, f_to_s(s, fclamp(s, glsl1(s, GLSLRoundEven, v), -512.0f, 511.0f)), c_uint(s, 0x3FF));
	case TY_U8: case TY_U16: case TY_U32: case TY_O8: return f_to_u(s, v);
	default: return f_to_s(s, v);
	}
}
static uint32_t float_to_lane_norm_spv(Spv *s, uint32_t v, int type) {
	switch (type) {
	case TY_U8: return f_to_u(s, glsl1(s, GLSLRoundEven, fop2(s, SpvOpFMul, fclamp(s, v, 0, 1), c_float(s, 255.0f))));
	case TY_S8: return f_to_s(s, glsl1(s, GLSLRoundEven, fop2(s, SpvOpFMul, fclamp(s, v, -1, 1), c_float(s, 127.0f))));
	case TY_U16: return f_to_u(s, glsl1(s, GLSLRoundEven, fop2(s, SpvOpFMul, fclamp(s, v, 0, 1), c_float(s, 65535.0f))));
	case TY_S16: return f_to_s(s, glsl1(s, GLSLRoundEven, fop2(s, SpvOpFMul, fclamp(s, v, -1, 1), c_float(s, 32767.0f))));
	case TY_U32: return f_to_u(s, fop2(s, SpvOpFMul, fclamp(s, v, 0, 1), c_float(s, 4294967295.0f)));
	case TY_S32: return f_to_s(s, fop2(s, SpvOpFMul, fclamp(s, v, -1, 1), c_float(s, 2147483647.0f)));
	case TY_C10: return uop2(s, SpvOpBitwiseAnd, f_to_s(s, glsl1(s, GLSLRoundEven, fop2(s, SpvOpFMul, fclamp(s, v, -2, 2), c_float(s, 255.0f)))), c_uint(s, 0x3FF));
	default: return float_to_lane_spv(s, v, type);
	}
}
/* fpi_widened (usse.c, §2.1): the internal registers i0-i3 hold one F32 per word whatever the
 * operand's type -- component c of i<k> is word 4k+c, an F16 write stores the half widened to F32
 * there and an F16 read takes the word back (as the F32 for a float read, as the half's bits for a
 * raw one). Packing halves there, as into every other bank, broke Killzone: Mercenary's text shader
 * (an F16 VNMAD into i0 read back by an F32 VCOMP: every glyph's alpha -3.6) and its F16 VDUALs,
 * whose internal sources and destination are i-registers F32 instructions also use. */
static int fpi_widened(const UOperand *o) { return o->bank == BK_FPI && o->type == TY_F16; }
static RegPtr fpi_word(Gen *g, const UOperand *o, int repeat_off, int ch) { return bank_ptr(g, BK_FPI, ((int)o->num + repeat_off) * 4 + ch); }
/* read one component (post-swizzle) as a raw lane, sign-extended for signed types -- read_i32 */
static uint32_t read_raw_comp(Gen *g, const UOperand *o, int repeat_off, int comp, int *ok) {
	Spv *s = &g->s;
	int ch = o->swiz[comp & 3];
	uint32_t v;
	if (ch >= 4) v = c_uint(s, ch == 5 ? 1u : ch == 6 ? 2u : 0u);
	else if (o->bank == BK_IMM) v = c_uint(s, o->num);
	else if (o->bank == BK_FPCONST) {   /* the constant's lane in the operand's type, as usse.c's read_i32 (word ch/lpw, lane ch%lpw; signed lanes sign-extended) */
		int lpw = lpw_of(o->type), bits = lane_bits(o->type), lane = ch % lpw;
		uint32_t word = usse_fpconst[o->num & 63][(ch / lpw) & 1], raw = bits == 32 ? word : (word >> (lane * bits)) & ((1u << bits) - 1);
		if (bits < 32 && (o->type == TY_S8 || o->type == TY_S16 || o->type == TY_C10) && (raw & (1u << (bits - 1)))) raw |= ~((1u << bits) - 1);
		v = c_uint(s, raw);
	}
	else if (o->bank == BK_NONE || o->bank == BK_GLOBAL) v = c_uint(s, 0);
	else if (fpi_widened(o)) {   /* the F32 word as the half the operand asks for */
		RegPtr p = fpi_word(g, o, repeat_off, ch);
		if (!p.ok) { *ok = 0; return c_uint(s, 0); }
		v = float_to_half_spv(s, result1(s, &s->func, SpvOpBitcast, t_float(s), load(s, t_uint(s), p.ptr)));
	} else {
		int lane; RegPtr p = lane_ptr(g, o, repeat_off, ch, &lane);
		if (!p.ok) { *ok = 0; return c_uint(s, 0); }
		v = extract_lane_spv(s, load(s, t_uint(s), p.ptr), o->type, lane);
	}
	if (o->negabs & 2) {
		uint32_t isneg = result2(s, &s->func, SpvOpSLessThan, t_bool(s), result1(s, &s->func, SpvOpBitcast, t_int(s), v), c_int(s, 0));
		v = result3(s, &s->func, SpvOpSelect, t_uint(s), isneg, result1(s, &s->func, SpvOpSNegate, t_uint(s), v), v);
	}
	if (o->negabs & 1) v = result1(s, &s->func, SpvOpSNegate, t_uint(s), v);
	return v;
}
/* write a raw lane into the destination's word (insert_lane), `comp` the absolute lane index */
static void write_raw_comp(Gen *g, const UOperand *o, int repeat_off, int comp, uint32_t raw, int *ok) {
	Spv *s = &g->s;
	if ((o->bank == BK_IDXD1 || o->bank == BK_IDXD2) && ((o->num >> 5) & 3) == 3) {   /* idx_ptr's SA is the read-only uniform block here */
		if (!g->why[0]) snprintf(g->why, sizeof g->why, "indexed write into SA"); *ok = 0; return;
	}
	if (fpi_widened(o)) {   /* the half widened to F32, in the component's own word */
		RegPtr p = fpi_word(g, o, repeat_off, comp);
		if (!p.ok) { *ok = 0; return; }
		store(s, p.ptr, result1(s, &s->func, SpvOpBitcast, t_uint(s), half_to_float_spv(s, raw)));
		return;
	}
	int lane; RegPtr p = lane_ptr(g, o, repeat_off, comp, &lane);
	if (!p.ok) { *ok = 0; return; }
	int bits = lane_bits(o->type);
	if (bits == 32) { store(s, p.ptr, raw); return; }
	store(s, p.ptr, bitfield_insert(s, load(s, t_uint(s), p.ptr), raw, c_uint(s, (uint32_t)(lane * bits)), c_uint(s, (uint32_t)bits)));
}

/* read one component (post-swizzle), as a float -- read_f32: constants by channel, FPCONST by its
 * own F16/F32 word rule, register lanes of any type through lane_to_float, then negate/abs */
static uint32_t read_comp_f(Gen *g, const UOperand *o, int repeat_off, int comp, int *supported) {
	Spv *s = &g->s;
	int ch = o->swiz[comp & 3];
	uint32_t v;
	if (ch >= 4) v = c_float(s, ch == 4 ? 0.0f : ch == 5 ? 1.0f : ch == 6 ? 2.0f : 0.5f);
	else if (o->bank == BK_FPCONST) {
		/* word selection differs by type (usse.c's read_f32): F16 packs two constants per word
		 * (ch<2 -> word0, ch>=2 -> word1, ch&1 the lane), F32 has one per word, selected by ch&1 */
		if (o->type == TY_F16) {
			uint32_t word = usse_fpconst[o->num & 63][ch < 2 ? 0 : 1];
			v = half_to_float_spv(s, c_uint(s, (word >> (16 * (ch & 1))) & 0xFFFF));
		} else { uint32_t word = usse_fpconst[o->num & 63][ch & 1]; float fv; memcpy(&fv, &word, 4); v = c_float(s, fv); }
	}
	else if (o->bank == BK_IMM) v = c_float(s, (float)(int32_t)o->num);
	else if (o->bank == BK_NONE || o->bank == BK_GLOBAL) v = c_float(s, 0);
	else if (fpi_widened(o)) {   /* the F32 word itself, unrounded (usse.c's read_f32) */
		RegPtr p = fpi_word(g, o, repeat_off, ch);
		if (!p.ok) { *supported = 0; return c_float(s, 0); }
		v = result1(s, &s->func, SpvOpBitcast, t_float(s), load(s, t_uint(s), p.ptr));
	} else {
		int lane; RegPtr p = lane_ptr(g, o, repeat_off, ch, &lane);
		if (!p.ok) { *supported = 0; return c_float(s, 0); }
		v = lane_to_float_spv(s, extract_lane_spv(s, load(s, t_uint(s), p.ptr), o->type, lane), o->type);
	}
	if (o->negabs & 2) v = glsl1(s, GLSLFAbs, v);
	if (o->negabs & 1) v = result1(s, &s->func, SpvOpFNegate, t_float(s), v);
	return v;
}
/* write one float component in the destination's type -- write_f32 */
static void write_comp_f(Gen *g, const UOperand *o, int repeat_off, int comp, uint32_t fval, int *supported) {
	write_raw_comp(g, o, repeat_off, comp, float_to_lane_spv(&g->s, fval, o->type), supported);
}
/* a destination written as packed F16 (two lanes/word): SMP overrides its dst.type to F16 by hand
 * for this (see exec_smp); VPCK uses its dst.type directly (see emit_vpck). `comp` is the absolute
 * lane index (word = o->num + repeat_off + comp/2, lane = comp&1), matching write_raw_lane's own
 * F16 case (insert_lane) exactly. */
static void write_comp_f16(Gen *g, const UOperand *o, int repeat_off, int comp, uint32_t fval, int *supported) {
	UOperand h = *o; h.type = TY_F16;
	write_comp_f(g, &h, repeat_off, comp, fval, supported);
}

void usse_spirv_free(SpirvModule *m) { free(m->words); memset(m, 0, sizeof *m); }

/* ---------------------------------------------------------------- per-op translation (ports of usse.c's exec_*) */
static int in_mult_set_g(int bank) { return bank == BK_TEMP || bank == BK_PA || bank == BK_O || bank == BK_SA || bank == BK_IDXD1 || bank == BK_IDXD2; }   /* usse.c's in_mult_set */
/* mirrors usse.c's slot_offset exactly, including RM_SLMSI: that mode's cx->inc[][] is populated
 * only by SMLSI/SMBO, both pure functions of their own immediate operands (no register reads) --
 * so with no branches in this pass's coverage (BR isn't supported), walking the program in order
 * and updating g->inc[][] at each SMLSI/SMBO (done in the main translate loop) resolves it fully
 * at translate time, same as usse.c's run_program resolves it at run time. */
static int32_t slot_off(Gen *g, int mode, int bank, int r, const int mult[4], int slot, int *supported) {
	int is_fpi = (bank == BK_FPI); (void)supported;
	switch (mode) {
	case RM_EXTERNAL: return is_fpi ? 0 : 4 * r;
	case RM_INTERNAL: return is_fpi ? r : 0;
	case RM_BOTH: return is_fpi ? r : 4 * r;
	default: { int rr = r < 0 ? 0 : r > 16 ? 16 : r; int v = g->inc[slot & 3][rr]; return in_mult_set_g(bank) ? v * mult[slot & 3] : v; }
	}
}
/* GPI operands (VMAD's gpi0/gpi1, VDP's gpi0) step only in INTERNAL/BOTH mode, by one internal register a repeat,
 * never by the SMLSI table (§2.12) -- usse.c's gpi_offset, and the same Unit 13 scope-lens sighting */
static int gpi_off(int mode, int r) { return (mode == RM_INTERNAL || mode == RM_BOTH) ? r : 0; }
static void get_mult_g(const UOp *op, int mult[4]) {
	mult[0] = mult[1] = mult[2] = mult[3] = 2;
	if (op->op == OP_VMAD) mult[3] = 4;
	else if (op->op == OP_VMOV) { int f = (op->sub3 == 4 || op->sub3 == 5); mult[0] = mult[1] = mult[2] = mult[3] = f ? 2 : 1; }
	else if (op->op == OP_VCOMP) mult[0] = mult[1] = mult[2] = mult[3] = 1;
	else if (op->op == OP_VPCK) {
		int sf = (op->s1.type == TY_F16 || op->s1.type == TY_F32 || op->s1.type == TY_C10);
		int df = (op->dst.type == TY_F16 || op->dst.type == TY_F32 || op->dst.type == TY_C10);
		if (sf || df) { mult[0] = 1; mult[1] = mult[2] = 2; mult[3] = 1; } else mult[0] = mult[1] = mult[2] = mult[3] = 1;   /* float operands step in 64-bit registers (usse.c's get_mult: Uncharted's repeated F32->F16 light colours) */
	}
}

static void emit_vmad2(Gen *g, const UOp *op, int *ok) {
	Spv *s = &g->s; int mult[4] = {2, 2, 2, 2};
	int off0 = slot_off(g, op->repmode, op->s0.bank, 0, mult, 0, ok), off1 = slot_off(g, op->repmode, op->s1.bank, 0, mult, 1, ok);
	int off2 = slot_off(g, op->repmode, op->s2.bank, 0, mult, 2, ok), offd = slot_off(g, op->repmode, op->dst.bank, 0, mult, 3, ok);
	if (!*ok) return;
	uint32_t res[4] = {0, 0, 0, 0};   /* all lanes read before any is written (usse.c's staging: a destination that is also a source) */
	for (int c = 0; c < 4; c++) if (op->mask & (1 << c)) {
		uint32_t a = read_comp_f(g, &op->s0, off0, c, ok), b = read_comp_f(g, &op->s1, off1, c, ok), cc = read_comp_f(g, &op->s2, off2, c, ok);
		if (!*ok) return;
		res[c] = fop2(s, SpvOpFAdd, fop2(s, SpvOpFMul, a, b), cc);
	}
	for (int c = 0; c < 4; c++) if (op->mask & (1 << c)) write_comp_f(g, &op->dst, offd, c, res[c], ok);
}
static void emit_vnmad(Gen *g, const UOp *op, int *ok) {
	Spv *s = &g->s;
	uint32_t res[4] = {0, 0, 0, 0};   /* read all lanes first (see emit_vmad2); Unit 13's occlusion multiply (PA0 = PA0.xxx1 * O0) read its own first result */
	for (int c = 0; c < 4; c++) if (op->mask & (1 << c)) {
		uint32_t a = read_comp_f(g, &op->s1, 0, c, ok), b = read_comp_f(g, &op->s2, 0, c, ok), r;
		if (!*ok) return;
		switch (op->sub) {
		case 0: r = fop2(s, SpvOpFMul, a, b); break;
		case 1: r = fop2(s, SpvOpFAdd, a, b); break;
		case 2: r = fop2(s, SpvOpFSub, a, glsl1(s, GLSLFloor, b)); break;
		case 3: case 4: r = c_float(s, 0);   /* VDSX/VDSY: no fragment-quad derivatives, same approximation as usse.c */ break;
		case 5: r = glsl2(s, GLSLFMin, a, b); break;
		case 6: r = glsl2(s, GLSLFMax, a, b); break;
		case 7: { uint32_t d = c_float(s, 0); for (int k = 0; k < 4; k++) { uint32_t ak = read_comp_f(g, &op->s1, 0, k, ok), bk = read_comp_f(g, &op->s2, 0, k, ok); d = fop2(s, SpvOpFAdd, d, fop2(s, SpvOpFMul, ak, bk)); } r = d; break; }
		default: r = fop2(s, SpvOpFAdd, a, b); break;
		}
		if (!*ok) return;
		res[c] = r;
	}
	for (int c = 0; c < 4; c++) if (op->mask & (1 << c)) write_comp_f(g, &op->dst, 0, c, res[c], ok);
}
static void emit_vmad(Gen *g, const UOp *op, int *ok) {
	Spv *s = &g->s; int mult[4] = {2, 2, 2, 4};
	for (int r = 0; r < op->reps; r++) {
		int off1 = slot_off(g, op->repmode, op->s1.bank, r, mult, 1, ok), offd = slot_off(g, op->repmode, op->dst.bank, r, mult, 3, ok);
		int off0 = gpi_off(op->repmode, r), off2 = off0;   /* gpi0, gpi1 */
		if (!*ok) return;
		uint32_t res[4] = {0, 0, 0, 0};
		for (int c = 0; c < 4; c++) if (op->mask & (1 << c)) {
			uint32_t a = read_comp_f(g, &op->s0, off0, c, ok), b = read_comp_f(g, &op->s1, off1, c, ok), cc = read_comp_f(g, &op->s2, off2, c, ok);
			if (!*ok) return;
			res[c] = fop2(s, SpvOpFAdd, fop2(s, SpvOpFMul, a, b), cc);
		}
		for (int c = 0; c < 4; c++) if (op->mask & (1 << c)) write_comp_f(g, &op->dst, offd, c, res[c], ok);
	}
}
static void emit_vdp(Gen *g, const UOp *op, int *ok) {
	Spv *s = &g->s; int mult[4] = {2, 2, 2, 2}; int mask = op->mask, n = op->sub2;
	for (int r = 0; r < op->reps; r++) {
		int off1 = slot_off(g, op->repmode, op->s1.bank, r, mult, 1, ok), off2 = gpi_off(op->repmode, r);   /* s2 is gpi0 */
		if (!*ok) return;
		uint32_t dot = c_float(s, 0);
		for (int k = 0; k < n; k++) { uint32_t a = read_comp_f(g, &op->s1, off1, k, ok), b = read_comp_f(g, &op->s2, off2, k, ok); dot = fop2(s, SpvOpFAdd, dot, fop2(s, SpvOpFMul, a, b)); }
		if (!*ok) return;
		for (int c = 0; c < 4; c++) if (mask & (1 << c)) write_comp_f(g, &op->dst, 0, c, dot, ok);
		mask = ((mask << 1) | (mask >> 3)) & 0xF;
	}
}
static void emit_vcomp(Gen *g, const UOp *op, int *ok) {
	Spv *s = &g->s; int mult[4] = {1, 1, 1, 1};
	for (int r = 0; r < op->reps; r++) {
		int off1 = slot_off(g, op->repmode, op->s1.bank, r, mult, 1, ok), offd = slot_off(g, op->repmode, op->dst.bank, r, mult, 3, ok);
		if (!*ok) return;
		uint32_t x = read_comp_f(g, &op->s1, off1, op->sub2 & 3, ok); if (!*ok) return;
		uint32_t result;
		switch (op->sub) { case 0: result = fop2(s, SpvOpFDiv, c_float(s, 1), x); break; case 1: result = glsl1(s, GLSLInverseSqrt, x); break;
		case 2: result = glsl1(s, GLSLLog2, x); break; default: result = glsl1(s, GLSLExp2, x); break; }
		for (int c = 0; c < 4; c++) if (op->mask & (1 << c)) write_comp_f(g, &op->dst, offd, c, result, ok);
	}
}
/* VDUAL (§3.5): usse.c's exec_vdual, from the same usse_vdual_decode (operands, layouts, doubling,
 * masks), so the two can't take the word apart differently. Killzone: Mercenary uses it in two
 * vertex programs (0x10c409d0, 0x112ef2c0) and three fragment programs (its gradient 0x11305670,
 * glyph/text 0x11312920 and path 0x112ef460 shaders); until this, every draw of theirs -- about
 * 21 a frame over its test run -- fell back to the software path. A layout-table hole
 * reads as 0.0 (exec_vdual's null source); each op's float sequence is the interpreter's
 * (VDP sums from 0 in lane order, FRSQ is 1/sqrt, FEXP/FLOG base 2). */
static uint32_t du_src_spv(Gen *g, const UsseDual *v, int k, int c, int *ok) { return k < 0 ? c_float(&g->s, 0) : read_comp_f(g, &v->src[k], 0, c, ok); }
static void du_eval_spv(Gen *g, const UsseDual *v, int dop, const int8_t *sx, uint32_t r[4], int *ok) {
	Spv *s = &g->s; int n = v->ncomp;
	for (int c = 0; c < 4; c++) r[c] = c_float(s, 0);
	switch (dop) {
	case DU_VMAD: for (int c = 0; c < n; c++) r[c] = fop2(s, SpvOpFAdd, fop2(s, SpvOpFMul, du_src_spv(g, v, sx[0], c, ok), du_src_spv(g, v, sx[1], c, ok)), du_src_spv(g, v, sx[2], c, ok)); break;
	case DU_VDP: case DU_VSSQ: {
		uint32_t d = c_float(s, 0); int b = dop == DU_VSSQ ? sx[0] : sx[1];
		for (int c = 0; c < n; c++) d = fop2(s, SpvOpFAdd, d, fop2(s, SpvOpFMul, du_src_spv(g, v, sx[0], c, ok), du_src_spv(g, v, b, c, ok)));
		for (int c = 0; c < 4; c++) r[c] = d;
		break;
	}
	case DU_VMUL: for (int c = 0; c < n; c++) r[c] = fop2(s, SpvOpFMul, du_src_spv(g, v, sx[0], c, ok), du_src_spv(g, v, sx[1], c, ok)); break;
	case DU_VADD: for (int c = 0; c < n; c++) r[c] = fop2(s, SpvOpFAdd, du_src_spv(g, v, sx[0], c, ok), du_src_spv(g, v, sx[1], c, ok)); break;
	case DU_VMOV: for (int c = 0; c < n; c++) r[c] = du_src_spv(g, v, sx[0], c, ok); break;
	case DU_FRSQ: r[0] = fop2(s, SpvOpFDiv, c_float(s, 1), glsl1(s, GLSLSqrt, du_src_spv(g, v, sx[0], 0, ok))); break;
	case DU_FRCP: r[0] = fop2(s, SpvOpFDiv, c_float(s, 1), du_src_spv(g, v, sx[0], 0, ok)); break;
	case DU_FMAD: r[0] = fop2(s, SpvOpFAdd, fop2(s, SpvOpFMul, du_src_spv(g, v, sx[0], 0, ok), du_src_spv(g, v, sx[1], 0, ok)), du_src_spv(g, v, sx[2], 0, ok)); break;
	case DU_FADD: r[0] = fop2(s, SpvOpFAdd, du_src_spv(g, v, sx[0], 0, ok), du_src_spv(g, v, sx[1], 0, ok)); break;
	case DU_FMUL: r[0] = fop2(s, SpvOpFMul, du_src_spv(g, v, sx[0], 0, ok), du_src_spv(g, v, sx[1], 0, ok)); break;
	case DU_FSUBFLR: r[0] = fop2(s, SpvOpFSub, du_src_spv(g, v, sx[0], 0, ok), glsl1(s, GLSLFloor, du_src_spv(g, v, sx[1], 0, ok))); break;
	case DU_FEXP: r[0] = glsl1(s, GLSLExp2, du_src_spv(g, v, sx[0], 0, ok)); break;
	case DU_FLOG: r[0] = glsl1(s, GLSLLog2, du_src_spv(g, v, sx[0], 0, ok)); break;
	default: break;   /* DU_INVALID: its mask is 0 */
	}
}
static void emit_vdual(Gen *g, const UOp *op, int *ok) {
	UsseDual v; usse_vdual_decode(op, &v);
	if (v.op1 == DU_INVALID && v.op2 == DU_INVALID) return;
	uint32_t r1[4], r2[4];   /* both ops read every source before either writes (exec_vdual's staging) */
	du_eval_spv(g, &v, v.op1, v.s1, r1, ok); du_eval_spv(g, &v, v.op2, v.s2, r2, ok);
	if (!*ok) return;
	for (int c = 0; c < 4; c++) if (v.m1 & (1 << c)) write_comp_f(g, &v.d1, 0, c, r1[c], ok);
	for (int c = 0; c < 4; c++) if (v.m2 & (1 << c)) write_comp_f(g, &v.d2, 0, c, r2[c], ok);
}
/* ports usse.c's alu_eval_f table (§3.9); DSX/DSY (11/12) are approximated as 0, same as usse.c */
static uint32_t alu_eval_f_spv(Spv *s, int op, uint32_t a, uint32_t b) {
	switch (op) {
	case 3: return fop2(s, SpvOpFSub, a, glsl1(s, GLSLFloor, b));
	case 4: return fop2(s, SpvOpFDiv, c_float(s, 1), a);
	case 5: return glsl1(s, GLSLInverseSqrt, a);
	case 6: return glsl1(s, GLSLLog2, a);
	case 7: return glsl1(s, GLSLExp2, a);
	case 9: return glsl2(s, GLSLFMin, a, b);
	case 10: return glsl2(s, GLSLFMax, a, b);
	case 11: case 12: return c_float(s, 0);
	case 13: return fop2(s, SpvOpFMul, a, b);
	case 14: return fop2(s, SpvOpFSub, a, b);
	default: return fop2(s, SpvOpFAdd, a, b);
	}
}
static uint32_t pred_ptr(Gen *g, int idx) { return access1(&g->s, t_ptr(&g->s, SpvStoragePrivate, t_bool(&g->s)), g->var_p, c_uint(&g->s, (uint32_t)idx & 3)); }
/* VTST: an ALU op (alu_sel==0: float, matching this pass's F32-only register coverage; alu_sel!=0,
 * the integer ALU table, isn't covered) against 0, per sign_test/zero_test (§3.9), written to one
 * of the 4 predicate registers -- the only register-file write this pass's coverage makes that
 * ISN'T to TEMP/PA/O/SA/FPINTERNAL (see KILL, the only consumer this pass covers, for why reading
 * these back is worth the real control flow it needs). VTSTMSK (writes a normal register instead
 * of a predicate) isn't covered. */
/* usse.c's alu_eval_i: alu_sel 3 the bitwise/shift table, otherwise add/sub/mul */
static uint32_t alu_eval_i_spv(Spv *s, int sel, int op, uint32_t a, uint32_t b) {
	if (sel == 3) {
		uint32_t sh = uop2(s, SpvOpBitwiseAnd, b, c_uint(s, 31));
		switch (op) {
		case 0: return uop2(s, SpvOpBitwiseAnd, a, b);
		case 1: return uop2(s, SpvOpBitwiseOr, a, b);
		case 2: return uop2(s, SpvOpBitwiseXor, a, b);
		case 3: return uop2(s, SpvOpShiftLeftLogical, a, sh);
		case 4: return uop2(s, SpvOpShiftRightLogical, a, sh);
		case 5: {   /* rotate left; a shift by 32 is undefined, so a zero rotate selects `a` */
			uint32_t r = uop2(s, SpvOpBitwiseOr, uop2(s, SpvOpShiftLeftLogical, a, sh), uop2(s, SpvOpShiftRightLogical, a, uop2(s, SpvOpBitwiseAnd, uop2(s, SpvOpISub, c_uint(s, 32), sh), c_uint(s, 31))));
			return result3(s, &s->func, SpvOpSelect, t_uint(s), result2(s, &s->func, SpvOpIEqual, t_bool(s), sh, c_uint(s, 0)), a, r);
		}
		case 7: return uop2(s, SpvOpShiftRightArithmetic, a, sh);
		default: return c_uint(s, 0);
		}
	}
	switch (op) { case 10: case 11: case 7: case 13: case 14: case 15: return uop2(s, SpvOpISub, a, b); case 8: case 9: return uop2(s, SpvOpIMul, a, b); default: return uop2(s, SpvOpIAdd, a, b); }
}
/* VTST/VTSTMSK's shared test: the ALU result (float table, its dot product, or the integer table
 * converted to float like usse.c does) against 0 per sign_test/zero_test */
static uint32_t vtst_take(Gen *g, const UOp *op, int chan, int *ok) {
	Spv *s = &g->s;
	int alu_sel = op->sub, alu_op = op->sub2 & 0xF;
	uint32_t result;
	if (alu_sel == 0 && alu_op == 8) {
		result = c_float(s, 0);
		for (int k = 0; k < 4; k++) result = fop2(s, SpvOpFAdd, result, fop2(s, SpvOpFMul, read_comp_f(g, &op->s1, 0, k, ok), read_comp_f(g, &op->s2, 0, k, ok)));
	} else if (alu_sel == 0) result = alu_eval_f_spv(s, alu_op, read_comp_f(g, &op->s1, 0, chan, ok), read_comp_f(g, &op->s2, 0, chan, ok));
	else {
		uint32_t iv = alu_eval_i_spv(s, alu_sel, alu_op, read_raw_comp(g, &op->s1, 0, chan, ok), read_raw_comp(g, &op->s2, 0, chan, ok));
		result = result1(s, &s->func, SpvOpConvertSToF, t_float(s), result1(s, &s->func, SpvOpBitcast, t_int(s), iv));
	}
	int sign_test = (op->sub3 >> 2) & 3, zero_test = op->sub3 & 3;
	uint32_t zero = c_float(s, 0);
	switch (sign_test) {
	case 1: return result2(s, &s->func, zero_test == 1 ? SpvOpFOrdLessThanEqual : SpvOpFOrdLessThan, t_bool(s), result, zero);
	case 2: return result2(s, &s->func, zero_test == 1 ? SpvOpFOrdGreaterThanEqual : SpvOpFOrdGreaterThan, t_bool(s), result, zero);
	default: return result2(s, &s->func, zero_test == 1 ? SpvOpFOrdEqual : SpvOpFOrdNotEqual, t_bool(s), result, zero);
	}
}
static void emit_vtst(Gen *g, const UOp *op, int *ok) {
	uint32_t take = vtst_take(g, op, op->imm & 3, ok);
	if (*ok) store(&g->s, pred_ptr(g, op->dst.num), take);
}
/* VTSTMSK: the test's result as a mask value of the destination's type (all-ones / 1.0 / 0) */
static void emit_vtstmsk(Gen *g, const UOp *op, int *ok) {
	Spv *s = &g->s;
	int type = op->dst.type, n = ((op->imm & 3) == 2 && op->sub == 0) ? 4 : 1, per_lane = n == 4 && (op->sub2 & 0xF) != 8;   /* usse.c: a vec4 test per lane (a DP's one sum to all four) */
	uint32_t takes[4];
	for (int c = 0; c < n; c++) takes[c] = (c && !per_lane) ? takes[0] : vtst_take(g, op, per_lane ? c : 0, ok);   /* every lane read before any write (dst may be a source) */
	if (!*ok) return;
	uint32_t on;
	switch (type) {
	case TY_F32: on = 0x3F800000u; break;
	case TY_F16: on = 0x3C00u; break;
	case TY_U8: on = 0xFFu; break; case TY_U16: on = 0xFFFFu; break; case TY_U32: on = 0xFFFFFFFFu; break;
	case TY_S8: on = 0x7Fu; break; case TY_S16: on = 0x7FFFu; break; default: on = 0x7FFFFFFFu; break;
	}
	for (int c = 0; c < n; c++) write_raw_comp(g, &op->dst, 0, c, result3(s, &s->func, SpvOpSelect, t_uint(s), takes[c], c_uint(s, on), c_uint(s, 0)), ok);
}
/* VBW: bitwise op on one 32-bit (or, sub3, 16-bit) lane; the s2 rotate/invert is folded at
 * translate time when s2 is an immediate -- usse.c's exec_vbw */
static void emit_vbw(Gen *g, const UOp *op, int *ok) {
	Spv *s = &g->s;
	uint32_t a = read_raw_comp(g, &op->s1, 0, 0, ok), b;
	int rot = op->args[0] & 31;
	if (op->s2.bank == BK_IMM) {
		uint32_t bi = (uint32_t)op->s2.num | ((uint32_t)op->imm << 7) | ((uint32_t)op->args[2] << 14);
		if (op->sub3) { int r16 = rot & 15; bi = ((bi << r16) | (bi >> (16 - r16))) & 0xFFFF; } else bi = rot ? (bi << rot) | (bi >> (32 - rot)) : bi;
		if (op->args[1]) bi = op->sub3 ? (~bi & 0xFFFFu) : ~bi;
		b = c_uint(s, bi);
	} else {
		b = read_raw_comp(g, &op->s2, 0, 0, ok);
		if (op->sub3) {
			int r16 = rot & 15;
			b = r16 ? uop2(s, SpvOpBitwiseAnd, uop2(s, SpvOpBitwiseOr, uop2(s, SpvOpShiftLeftLogical, b, c_uint(s, (uint32_t)r16)), uop2(s, SpvOpShiftRightLogical, b, c_uint(s, (uint32_t)(16 - r16)))), c_uint(s, 0xFFFF))
			        : uop2(s, SpvOpBitwiseAnd, b, c_uint(s, 0xFFFF));
		} else if (rot) b = uop2(s, SpvOpBitwiseOr, uop2(s, SpvOpShiftLeftLogical, b, c_uint(s, (uint32_t)rot)), uop2(s, SpvOpShiftRightLogical, b, c_uint(s, (uint32_t)(32 - rot))));
		if (op->args[1]) b = op->sub3 ? uop2(s, SpvOpBitwiseAnd, result1(s, &s->func, SpvOpNot, t_uint(s), b), c_uint(s, 0xFFFF)) : result1(s, &s->func, SpvOpNot, t_uint(s), b);
	}
	if (!*ok) return;
	uint32_t r;
	switch (op->sub) {
	case 2: r = uop2(s, op->sub2 ? SpvOpBitwiseOr : SpvOpBitwiseAnd, a, b); break;
	case 3: r = uop2(s, SpvOpBitwiseXor, a, b); break;
	case 4: r = op->sub2 ? alu_eval_i_spv(s, 3, 5, a, b) : alu_eval_i_spv(s, 3, 3, a, b); break;
	case 5: r = op->sub2 ? alu_eval_i_spv(s, 3, 7, a, b) : alu_eval_i_spv(s, 3, 4, a, b); break;
	default: r = a; break;
	}
	if (op->sub3) r = uop2(s, SpvOpBitwiseAnd, r, c_uint(s, 0xFFFF));
	write_raw_comp(g, &op->dst, 0, 0, r, ok);
}
/* usse.c's test_lane_raw: zero/nonzero/negative/negative-or-zero of a raw lane of `type` */
static uint32_t test_lane_spv(Spv *s, uint32_t raw, int type, int cmpsel) {
	uint32_t iszero, isneg;
	if (type == TY_F32 || type == TY_F16) {
		uint32_t f = type == TY_F32 ? result1(s, &s->func, SpvOpBitcast, t_float(s), raw) : half_to_float_spv(s, raw);
		iszero = result2(s, &s->func, SpvOpFOrdEqual, t_bool(s), f, c_float(s, 0));
		isneg = result2(s, &s->func, SpvOpFOrdLessThan, t_bool(s), f, c_float(s, 0));
	} else {
		iszero = result2(s, &s->func, SpvOpIEqual, t_bool(s), raw, c_uint(s, 0));
		isneg = (type == TY_U8 || type == TY_U16 || type == TY_U32) ? result2(s, &s->func, SpvOpULessThan, t_bool(s), raw, c_uint(s, 0))   /* never */
		        : result2(s, &s->func, SpvOpSLessThan, t_bool(s), result1(s, &s->func, SpvOpBitcast, t_int(s), raw), c_int(s, 0));
	}
	switch (cmpsel & 3) {
	case 0: return iszero;
	case 1: return result1(s, &s->func, SpvOpLogicalNot, t_bool(s), iszero);
	case 2: return isneg;
	default: return result2(s, &s->func, SpvOpLogicalOr, t_bool(s), isneg, iszero);
	}
}
/* VMOV: raw lanes, as usse.c's exec_vmov -- sub==0 a plain move, otherwise a conditional select
 * on s0 (sub==2 tests s0 as U8) */
static void emit_vmov(Gen *g, const UOp *op, int *ok) {
	Spv *s = &g->s; int mult[4]; get_mult_g(op, mult);
	for (int r = 0; r < op->reps; r++) {
		int off0 = slot_off(g, op->repmode, op->s0.bank, r, mult, 0, ok), off1 = slot_off(g, op->repmode, op->s1.bank, r, mult, 1, ok);
		int off2 = slot_off(g, op->repmode, op->s2.bank, r, mult, 2, ok), offd = slot_off(g, op->repmode, op->dst.bank, r, mult, 3, ok);
		uint32_t res[4] = {0, 0, 0, 0};
		for (int c = 0; c < 4; c++) if (op->mask & (1 << c)) {
			uint32_t raw;
			if (op->sub == 0) raw = read_raw_comp(g, &op->s1, off1, c, ok);
			else {
				uint32_t cond = test_lane_spv(s, read_raw_comp(g, &op->s0, off0, c, ok), op->sub == 2 ? TY_U8 : op->s1.type, op->sub2);
				raw = result3(s, &s->func, SpvOpSelect, t_uint(s), cond, read_raw_comp(g, &op->s1, off1, c, ok), read_raw_comp(g, &op->s2, off2, c, ok));
			}
			if (!*ok) return;
			res[c] = raw;
		}
		for (int c = 0; c < 4; c++) if (op->mask & (1 << c)) write_raw_comp(g, &op->dst, offd, c, res[c], ok);
	}
}
/* VPCK: a type-converting pack, as usse.c's exec_vpck: s1[x,y]/s2[z,w] by swizzle (F32 sources;
 * "wide" when s1==s2 with an identity swizzle), converted plainly, normalized (sub), or moved raw
 * between two integer types */
static void emit_vpck(Gen *g, const UOp *op, int *ok) {
	Spv *s = &g->s; int mult[4]; get_mult_g(op, mult);
	int src_f32 = op->s1.type == TY_F32;
	int src_float = op->s1.type == TY_F16 || op->s1.type == TY_F32 || op->s1.type == TY_C10;
	int dst_float = op->dst.type == TY_F16 || op->dst.type == TY_F32 || op->dst.type == TY_C10;
	for (int r = 0; r < op->reps; r++) {
		int off1 = slot_off(g, op->repmode, op->s1.bank, r, mult, 1, ok), off2 = slot_off(g, op->repmode, op->s2.bank, r, mult, 2, ok);
		int offd = slot_off(g, op->repmode, op->dst.bank, r, mult, 3, ok);
		/* usse.c's rule: an F32 vec4 spans src1 (x,y) and src2 (z,w) -- except in the 128-bit
		 * internal registers (i0-i3), where src1 carries all four itself. Unit 13's depth-to-position
		 * pass packs i0.xyzw to F16: without the FPI case the GPU wrote every position's z and w as 0
		 * (read from an empty src2), and the ambient pass reflected off garbage. */
		int wide = src_f32 && (op->s1.bank == BK_FPI ||
		           (op->s1.bank == op->s2.bank && op->s1.num == op->s2.num && off1 == off2 &&
		            op->s1.swiz[0] == 0 && op->s1.swiz[1] == 1 && op->s1.swiz[2] == 2 && op->s1.swiz[3] == 3 &&
		            (op->mask == 1 || op->mask == 3 || op->mask == 7 || op->mask == 15)));
		uint32_t res[4] = {0, 0, 0, 0};
		for (int c = 0; c < 4; c++) if (op->mask & (1 << c)) {
			int sel = op->s1.swiz[c] & 3;
			const UOperand *src = (!src_f32 || wide || sel < 2) ? &op->s1 : &op->s2;
			int lpw = lpw_of(src->type), idx = src == &op->s1 ? sel : sel - 2;
			int base = (int)src->num + (src == &op->s1 ? off1 : off2);   /* usse.c's vpck_src_word: by word, an internal register by register (i<k> = words 4k..) */
			if (src->bank == BK_FPI) base *= 4;
			uint32_t word, out, rawv = 0;
			int widened = fpi_widened(&op->s1);   /* usse.c: an F16 internal-register source is the F32 word sel of src1 (vpck_src_word's word numbering), as a half */
			if (widened) { src = &op->s1; lpw = 1; idx = sel; base = ((int)op->s1.num + off1) * 4; }   /* (widened: always an internal register) */
			/* an INDEXED1/2 source resolves through the index register (usse.c's vpck_src_word: Unit 13's
			 * shadow program moves its shadow matrix's translation row with VPCK.f32 i1 <- sa[idx1*2+14..]) */
			if (src->bank == BK_FPCONST) {   /* the constant ROM, word by word as usse.c's vpck_src_word (constant k = words 2k, 2k+1): Uncharted's swipe-prompt threshold pass loads its selectors this way; read as 0 the arrow was (0,0,0,0) */
				int wi = (src == &op->s1 ? off1 : off2) + idx / lpw;
				word = c_uint(s, usse_fpconst[((int)src->num + wi / 2) & 63][wi & 1]);
			}
			else if (src->bank == BK_IMM) word = c_uint(s, src->num);
			else if (src->bank == BK_GLOBAL || src->bank == BK_NONE) word = c_uint(s, 0);   /* usse.c's bank_read_raw: these read as 0 */
			else if (src->bank == BK_IDXD1 || src->bank == BK_IDXD2) {
				RegPtr p = idx_ptr(g, src, (src == &op->s1 ? off1 : off2) + idx / lpw);
				if (!p.ok) { *ok = 0; return; }
				word = load(s, t_uint(s), p.ptr);
			} else {
				RegPtr p = bank_ptr(g, src->bank, base + idx / lpw);
				if (!p.ok) { *ok = 0; return; }
				word = load(s, t_uint(s), p.ptr);
			}
			rawv = widened ? float_to_half_spv(s, result1(s, &s->func, SpvOpBitcast, t_float(s), word)) : extract_lane_spv(s, word, src->type, idx % lpw);
			if (op->sub) out = float_to_lane_norm_spv(s, lane_to_float_norm_spv(s, rawv, op->s1.type), op->dst.type);
			else if (!src_float && !dst_float) out = rawv;
			else {
				uint32_t fv = lane_to_float_spv(s, rawv, op->s1.type);
				out = dst_float ? float_to_lane_spv(s, fv, op->dst.type) : f_to_s(s, fv);
			}
			res[c] = out;
		}
		for (int c = 0; c < 4; c++) if (op->mask & (1 << c)) write_raw_comp(g, &op->dst, offd, c, res[c], ok);
	}
}
/* ---- the integer multiply-adds, as usse.c's exec_i8mad/i16mad/i32mad/i32mad2: raw 32-bit operand
 * words, unit repeat stepping */
static uint32_t ubits(Spv *s, uint32_t w, int off, int n) { return result3(s, &s->func, SpvOpBitFieldUExtract, t_uint(s), w, c_uint(s, (uint32_t)off), c_uint(s, (uint32_t)n)); }
static uint32_t sbits(Spv *s, uint32_t w, int off, int n) { return result3(s, &s->func, SpvOpBitFieldSExtract, t_uint(s), w, c_uint(s, (uint32_t)off), c_uint(s, (uint32_t)n)); }
static void imad_offsets(Gen *g, const UOp *op, int r, int off[4], int *ok) {
	static const int unit[4] = {1, 1, 1, 1};
	off[0] = slot_off(g, op->repmode, op->s0.bank, r, unit, 0, ok); off[1] = slot_off(g, op->repmode, op->s1.bank, r, unit, 1, ok);
	off[2] = slot_off(g, op->repmode, op->s2.bank, r, unit, 2, ok); off[3] = slot_off(g, op->repmode, op->dst.bank, r, unit, 3, ok);
}
/* component 0 of the operand in its decoded type, as usse.c's read_i32(.., 0): the whole word for
 * I32MAD's S32 operands, lane 0 for I16MAD's U16 and I8MAD's U8 (the interpreter is the reference;
 * whether I8MAD should see the full packed word is a question for it, not this port) */
static uint32_t read_word(Gen *g, const UOperand *o, int off, int *ok) { return read_raw_comp(g, o, off, 0, ok); }
static void write_word(Gen *g, const UOperand *o, int off, uint32_t v, int *ok) { write_raw_comp(g, o, off, 0, v, ok); }
static void emit_i8mad(Gen *g, const UOp *op, int *ok) {
	Spv *s = &g->s;
	for (int r = 0; r < op->reps; r++) {
		int off[4]; imad_offsets(g, op, r, off, ok);
		UOperand s0 = op->s0; s0.negabs = 0;   /* s0's negate selects subtract below, as usse.c */
		uint32_t w0 = read_word(g, &s0, off[0], ok), w1 = read_word(g, &op->s1, off[1], ok), w2 = read_word(g, &op->s2, off[2], ok);
		if (!*ok) return;
		int csel0 = op->sub & 3, asel0 = op->sub2 & 1, csel1 = (op->sub2 >> 1) & 1, csel2 = (op->sub2 >> 2) & 1;
		uint32_t rep = c_uint(s, 0x010101u), rep4 = c_uint(s, 0x01010101u);
		uint32_t addrgb = csel0 == 0 ? uop2(s, SpvOpBitwiseAnd, w0, c_uint(s, 0xFFFFFF)) : csel0 == 1 ? uop2(s, SpvOpBitwiseAnd, w1, c_uint(s, 0xFFFFFF))
		                : uop2(s, SpvOpIMul, ubits(s, csel0 == 2 ? w0 : w1, 24, 8), rep);
		uint32_t adda = ubits(s, asel0 ? w1 : w0, 24, 8);
		uint32_t m1 = csel1 ? uop2(s, SpvOpIMul, ubits(s, w1, 24, 8), rep4) : w1, m2 = csel2 ? uop2(s, SpvOpIMul, ubits(s, w2, 24, 8), rep4) : w2;
		uint32_t result = c_uint(s, 0);
		for (int lane = 0; lane < 4; lane++) {
			uint32_t la1 = ubits(s, m1, 8 * lane, 8), la2 = ubits(s, m2, 8 * lane, 8);
			uint32_t ad = lane < 3 ? ubits(s, addrgb, 8 * lane, 8) : adda;
			uint32_t prod = result2(s, &s->func, 134 /*OpUDiv*/, t_uint(s), uop2(s, SpvOpIMul, la1, la2), c_uint(s, 255));
			uint32_t v = uop2(s, (op->s0.negabs & 1) ? SpvOpISub : SpvOpIAdd, prod, ad);
			uint32_t vi = result1(s, &s->func, SpvOpBitcast, t_int(s), v);   /* clamp 0..255 as a signed value */
			uint32_t lo = result3(s, &s->func, SpvOpSelect, t_uint(s), result2(s, &s->func, SpvOpSLessThan, t_bool(s), vi, c_int(s, 0)), c_uint(s, 0), v);
			uint32_t cl = result3(s, &s->func, SpvOpSelect, t_uint(s), result2(s, &s->func, SpvOpUGreaterThan, t_bool(s), lo, c_uint(s, 255)), c_uint(s, 255), lo);
			result = uop2(s, SpvOpBitwiseOr, result, uop2(s, SpvOpShiftLeftLogical, cl, c_uint(s, 8u * lane)));
		}
		write_word(g, &op->dst, off[3], result, ok);
	}
}
static void emit_i16mad(Gen *g, const UOp *op, int *ok) {
	Spv *s = &g->s;
	for (int r = 0; r < op->reps; r++) {
		int off[4]; imad_offsets(g, op, r, off, ok);
		uint32_t w0 = read_word(g, &op->s0, off[0], ok), w1 = read_word(g, &op->s1, off[1], ok), w2 = read_word(g, &op->s2, off[2], ok);
		if (!*ok) return;
		int sg = (op->sub >> 1) & 1, fmt2 = (op->sub2 >> 2) & 3, fmt1 = op->sub2 & 3;
		uint32_t v1 = fmt1 == 0 ? (sg ? sbits(s, w1, 0, 16) : ubits(s, w1, 0, 16)) : ubits(s, w1, op->args[0] ? 8 : 0, 8);
		uint32_t v2 = fmt2 == 0 ? (sg ? sbits(s, w2, 0, 16) : ubits(s, w2, 0, 16)) : ubits(s, w2, op->args[1] ? 8 : 0, 8);
		uint32_t v0 = sg ? sbits(s, w0, 0, 16) : ubits(s, w0, 0, 16);
		write_word(g, &op->dst, off[3], uop2(s, SpvOpBitwiseAnd, uop2(s, SpvOpIAdd, uop2(s, SpvOpIMul, v0, v1), v2), c_uint(s, 0xFFFF)), ok);
	}
}
static void emit_i32mad(Gen *g, const UOp *op, int *ok) {
	Spv *s = &g->s;
	for (int r = 0; r < op->reps; r++) {
		int off[4]; imad_offsets(g, op, r, off, ok);
		uint32_t w0 = read_word(g, &op->s0, off[0], ok), w1 = read_word(g, &op->s1, off[1], ok), w2 = read_word(g, &op->s2, off[2], ok);
		if (!*ok) return;
		uint32_t v0 = op->sub ? sbits(s, w0, op->args[0] ? 16 : 0, 16) : ubits(s, w0, op->args[0] ? 16 : 0, 16);
		uint32_t v1 = op->sub ? sbits(s, w1, op->args[1] ? 16 : 0, 16) : ubits(s, w1, op->args[1] ? 16 : 0, 16);
		uint32_t v2 = op->sub2 == 2 ? w2 : ubits(s, w2, op->args[2] ? 16 : 0, 16);
		write_word(g, &op->dst, off[3], uop2(s, SpvOpIAdd, uop2(s, SpvOpIMul, v0, v1), v2), ok);
	}
}
static void emit_i32mad2(Gen *g, const UOp *op, int *ok) {
	Spv *s = &g->s;
	for (int r = 0; r < op->reps; r++) {
		int off[4]; imad_offsets(g, op, r, off, ok);
		if (op->sub2) { write_word(g, &op->dst, off[3], read_word(g, &op->s2, off[2], ok), ok); continue; }
		UOperand a = op->s0, b = op->s1, c = op->s2; b.negabs &= ~1; c.negabs &= ~1;   /* usse.c negates s1/s2 after the read, s0 not at all */
		a.negabs = 0;
		uint32_t v0 = read_word(g, &a, off[0], ok), v1 = read_word(g, &b, off[1], ok), v2 = read_word(g, &c, off[2], ok);
		if (!*ok) return;
		/* src1's low 16 bits, sign-extended for a signed op (usse.c's exec_i32mad2: a 32 x 16 multiply) */
		v1 = op->sub ? uop2(s, SpvOpShiftRightArithmetic, uop2(s, SpvOpShiftLeftLogical, v1, c_uint(s, 16)), c_uint(s, 16)) : uop2(s, SpvOpBitwiseAnd, v1, c_uint(s, 0xffff));
		if (op->s1.negabs & 1) v1 = result1(s, &s->func, SpvOpSNegate, t_uint(s), v1);
		if (op->s2.negabs & 1) v2 = result1(s, &s->func, SpvOpSNegate, t_uint(s), v2);
		write_word(g, &op->dst, off[3], uop2(s, SpvOpIAdd, uop2(s, SpvOpIMul, v0, v1), v2), ok);
	}
}
/* texture unit for an SMP instruction, resolved at translate time (see usse_spirv.h's block
 * comment): an IMM operand is a literal unit index; an SA operand is read from the representative
 * draw's resolved secondary-program output the caller passed in (dependent-sampler unit
 * assignment is fixed by the program's own declaration, not draw state, so this is invariant
 * across draws sharing the compiled pipeline). Anything else, or an out-of-range result, bails. */
static uint32_t sa_read[8];   /* the sa_tex_unit entries this translation read (usse_spirv_sa_read) */
static int resolve_smp_unit(const UOp *op, const uint32_t sa_tex_unit[256]) {
	int unit;
	if (op->s1.bank == BK_IMM) unit = op->s1.num;
	else if (op->s1.bank == BK_SA) { const int i = op->s1.num % 256; sa_read[i >> 5] |= 1u << (i & 31); unit = (int)(sa_tex_unit[i] & 0xFF); }
	else return -1;
	return (unit >= 0 && unit < 16) ? unit : -1;
}
void usse_spirv_key(const UsseProgram *prog, const UsseFragLayout *fl, const uint32_t sa_tex_unit[256], int *color_kind, uint32_t *cube_mask, uint32_t *snap_mask) {
	uint32_t used = 0; int n = 0; const UOp *code = usse_program_code(prog, 0, &n);
	for (int i = 0; i < n; i++) if (code[i].op == OP_SMP) {   /* (resolve_smp_unit's, without noting the read) */
		int unit = code[i].s1.bank == BK_IMM ? code[i].s1.num : code[i].s1.bank == BK_SA ? (int)(sa_tex_unit[code[i].s1.num % 256] & 0xFF) : -1;
		if (unit >= 0 && unit < 16) used |= 1u << unit;
	}
	if (fl) for (int i = 0; i < fl->niters; i++) if (fl->iters[i].is_tex && fl->iters[i].tex_unit >= 0 && fl->iters[i].tex_unit < 16) used |= 1u << fl->iters[i].tex_unit;
	if (cube_mask) *cube_mask &= used;
	if (snap_mask) *snap_mask &= used;
	if (color_kind && *color_kind >= 0) *color_kind = fl && (fl->color_type == TY_U32 || fl->color_type == TY_S32) && (*color_kind == 1 || *color_kind == 2) ? *color_kind : 0;   /* (emit_frag_epilogue: raw words by kind 1, 2, else bytes) */
}
uint32_t usse_spirv_predict_cube(const UsseProgram *prog, const UsseFragLayout *fl, const uint32_t sa_tex_unit[256]) {
	uint32_t m = 0; int n = 0; const UOp *code = usse_program_code(prog, 0, &n);
	for (int i = 0; i < n; i++) if (code[i].op == OP_SMP && code[i].sub == 2) {   /* (dec_smp: sub is the coordinate's dimension, 2 = 3D) */
		int unit = code[i].s1.bank == BK_IMM ? code[i].s1.num : code[i].s1.bank == BK_SA ? (int)(sa_tex_unit[code[i].s1.num % 256] & 0xFF) : -1;
		if (unit >= 0 && unit < 16) m |= 1u << unit;
	}
	if (fl) for (int i = 0; i < fl->niters; i++) if (fl->iters[i].is_tex && fl->iters[i].tex_proj == 3 && fl->iters[i].tex_unit >= 0 && fl->iters[i].tex_unit < 16) m |= 1u << fl->iters[i].tex_unit;
	return m;
}
static uint32_t srgb_fixup(Gen *g, int unit, uint32_t texel);   /* below */
static uint32_t snap_fetch(Gen *g, int slot, int unit, uint32_t u, uint32_t v);   /* below */
static int sampler_slot(Gen *g, int unit) { for (int i = 0; i < g->nsmp; i++) if (g->smp_units[i] == unit) return i; return -1; }
/* OpImageSample* needs the sampled-image *value*, not the UniformConstant variable (a pointer) --
 * load it first (this was a real bug here: spirv-val caught it, see the commit this landed in) */
static int slot_is_cube(Gen *g, int slot) { return (g->cube_mask >> g->smp_units[slot]) & 1; }
static uint32_t load_sampler(Gen *g, int slot) { return load(&g->s, slot_is_cube(g, slot) ? g->t_simg_cube : g->t_simg, g->smp_sampler[slot]); }
/* the sampled-image types and one UniformConstant variable per sampler slot (the pre-scan's units, in its
 * order), in descriptor set `set` at bindings 0..nsmp-1: 2 for a fragment program, 0 for a vertex one.
 * Returns the 2D sampled-image pointer type (the framebuffer copy's variable takes one too) */
static uint32_t declare_samplers(Gen *g, uint32_t set) {
	Spv *s = &g->s;
	uint32_t t_img = nid(s); INS(&s->types, SpvOpTypeImage, t_img, t_float(s), 1 /*Dim2D*/, 0, 0, 0, 1 /*Sampled*/, 0 /*Unknown format*/);
	g->t_img2d = t_img;
	g->t_simg = nid(s); INS(&s->types, SpvOpTypeSampledImage, g->t_simg, t_img);
	uint32_t t_imgc = nid(s); INS(&s->types, SpvOpTypeImage, t_imgc, t_float(s), 1 /*Dim2D*/, 0, 1 /*Arrayed: the 6 faces*/, 0, 1 /*Sampled*/, 0 /*Unknown format*/);
	g->t_simg_cube = nid(s); INS(&s->types, SpvOpTypeSampledImage, g->t_simg_cube, t_imgc);
	uint32_t ptr_simg = t_ptr(s, SpvStorageUniformConstant, g->t_simg), ptr_simgc = t_ptr(s, SpvStorageUniformConstant, g->t_simg_cube);
	for (int i = 0; i < g->nsmp; i++) {
		g->smp_sampler[i] = global_var(s, slot_is_cube(g, i) ? ptr_simgc : ptr_simg, SpvStorageUniformConstant, 0);
		INS(&s->decos, SpvOpDecorate, g->smp_sampler[i], SpvDecorationDescriptorSet, set);
		INS(&s->decos, SpvOpDecorate, g->smp_sampler[i], SpvDecorationBinding, (uint32_t)i);
	}
	return ptr_simg;
}
/* a cube map, bound as a 6-layer 2D array (Metal sampled a real cube texture as unbound): gfx_soft.c's
 * cube_texel face selection, in SPIR-V -- the dominant axis picks the face (+X -X +Y -Y +Z -Z = layers 0-5),
 * the other two components over the dominant magnitude give the face coordinates, (f+1)/2 maps them to 0..1.
 * Shared by SMP and the non-dependent iterator samples (which built a 2D coordinate for a cube unit: Metal
 * refused the shader, "vector component access exceeds type float2" -- Killzone's first campaign level) */
/* grad (fragment programs): the face coordinates' screen derivatives, d(f/m) = (df m - f dm) / m^2 from the
 * direction's own, on THIS pixel's face -- the GPU's derivatives of the coordinate itself jump by up to the whole
 * face where a 2x2 quad straddles two faces, a line of the smallest level along every seam (sample_lod) */
static uint32_t cube_coord(Gen *g, uint32_t u, uint32_t v, uint32_t w3, uint32_t grad[2]) {
	Spv *s = &g->s;
	uint32_t tb = t_bool(s), tf = t_float(s), zero = c_float(s, 0);
	uint32_t au = glsl1(s, GLSLFAbs, u), av = glsl1(s, GLSLFAbs, v), aw = glsl1(s, GLSLFAbs, w3);
	uint32_t cx = result2(s, &s->func, SpvOpLogicalAnd, tb, result2(s, &s->func, SpvOpFOrdGreaterThanEqual, tb, au, av), result2(s, &s->func, SpvOpFOrdGreaterThanEqual, tb, au, aw));
	uint32_t cy = result2(s, &s->func, SpvOpLogicalAnd, tb, result1(s, &s->func, SpvOpLogicalNot, tb, cx),
	                      result2(s, &s->func, SpvOpLogicalAnd, tb, result2(s, &s->func, SpvOpFOrdGreaterThanEqual, tb, av, au), result2(s, &s->func, SpvOpFOrdGreaterThanEqual, tb, av, aw)));
	uint32_t upos = result2(s, &s->func, SpvOpFOrdGreaterThan, tb, u, zero), vpos = result2(s, &s->func, SpvOpFOrdGreaterThan, tb, v, zero), wpos = result2(s, &s->func, SpvOpFOrdGreaterThan, tb, w3, zero);
	uint32_t nu = result1(s, &s->func, SpvOpFNegate, tf, u), nv = result1(s, &s->func, SpvOpFNegate, tf, v), nw = result1(s, &s->func, SpvOpFNegate, tf, w3);
	uint32_t m = result3(s, &s->func, SpvOpSelect, tf, cx, au, result3(s, &s->func, SpvOpSelect, tf, cy, av, aw));
	uint32_t fu = result3(s, &s->func, SpvOpSelect, tf, cx, result3(s, &s->func, SpvOpSelect, tf, upos, nw, w3),
	                      result3(s, &s->func, SpvOpSelect, tf, cy, u, result3(s, &s->func, SpvOpSelect, tf, wpos, u, nu)));
	uint32_t fv = result3(s, &s->func, SpvOpSelect, tf, cx, nv, result3(s, &s->func, SpvOpSelect, tf, cy, result3(s, &s->func, SpvOpSelect, tf, vpos, w3, nw), nv));
	uint32_t face = result3(s, &s->func, SpvOpSelect, tf, cx, result3(s, &s->func, SpvOpSelect, tf, upos, c_float(s, 0), c_float(s, 1)),
	                        result3(s, &s->func, SpvOpSelect, tf, cy, result3(s, &s->func, SpvOpSelect, tf, vpos, c_float(s, 2), c_float(s, 3)),
	                                result3(s, &s->func, SpvOpSelect, tf, wpos, c_float(s, 4), c_float(s, 5))));
	uint32_t half = c_float(s, 0.5f), one = c_float(s, 1);
	uint32_t su = fop2(s, SpvOpFMul, fop2(s, SpvOpFAdd, fop2(s, SpvOpFDiv, fu, m), one), half);
	uint32_t sv = fop2(s, SpvOpFMul, fop2(s, SpvOpFAdd, fop2(s, SpvOpFDiv, fv, m), one), half);
	if (grad) {
		grad[0] = grad[1] = 0;
		for (int k = 0; g->is_fragment && k < 2; k++) {
			int op = k ? SpvOpDPdy : SpvOpDPdx;
			uint32_t du = result1(s, &s->func, op, tf, u), dv = result1(s, &s->func, op, tf, v), dw = result1(s, &s->func, op, tf, w3);
			uint32_t ndu = result1(s, &s->func, SpvOpFNegate, tf, du), ndv = result1(s, &s->func, SpvOpFNegate, tf, dv), ndw = result1(s, &s->func, SpvOpFNegate, tf, dw);
			uint32_t dfu = result3(s, &s->func, SpvOpSelect, tf, cx, result3(s, &s->func, SpvOpSelect, tf, upos, ndw, dw),
			                       result3(s, &s->func, SpvOpSelect, tf, cy, du, result3(s, &s->func, SpvOpSelect, tf, wpos, du, ndu)));
			uint32_t dfv = result3(s, &s->func, SpvOpSelect, tf, cx, ndv, result3(s, &s->func, SpvOpSelect, tf, cy, result3(s, &s->func, SpvOpSelect, tf, vpos, dw, ndw), ndv));
			uint32_t dm = result3(s, &s->func, SpvOpSelect, tf, cx, result3(s, &s->func, SpvOpSelect, tf, upos, du, ndu),
			                      result3(s, &s->func, SpvOpSelect, tf, cy, result3(s, &s->func, SpvOpSelect, tf, vpos, dv, ndv), result3(s, &s->func, SpvOpSelect, tf, wpos, dw, ndw)));
			uint32_t m2 = fop2(s, SpvOpFMul, m, m);
			uint32_t gu = fop2(s, SpvOpFMul, fop2(s, SpvOpFDiv, fop2(s, SpvOpFSub, fop2(s, SpvOpFMul, dfu, m), fop2(s, SpvOpFMul, fu, dm)), m2), half);
			uint32_t gv = fop2(s, SpvOpFMul, fop2(s, SpvOpFDiv, fop2(s, SpvOpFSub, fop2(s, SpvOpFMul, dfv, m), fop2(s, SpvOpFMul, fv, dm)), m2), half);
			grad[k] = result2(s, &s->func, SpvOpCompositeConstruct, t_vec(s, tf, 2), gu, gv);
		}
	}
	return result3(s, &s->func, SpvOpCompositeConstruct, t_vec(s, tf, 3), su, sv, face);
}
static uint32_t sample_lod(Gen *g, int slot, int unit, uint32_t coord, int lod_mode, uint32_t lodv, const uint32_t *grad);   /* (below, with srgb_fixup) */
static void emit_smp(Gen *g, const UOp *op, const uint32_t sa_tex_unit[256], int *ok) {
	Spv *s = &g->s;
	int unit = resolve_smp_unit(op, sa_tex_unit), slot = unit < 0 ? -1 : sampler_slot(g, unit);
	if (slot < 0) { if (!g->why[0]) snprintf(g->why, sizeof g->why, "SMP: texture unit %d has no sampler slot", unit); *ok = 0; return; }   /* samplers are pre-declared from a prescan before the walk starts */
	uint32_t u = read_comp_f(g, &op->s0, 0, 0, ok), v = read_comp_f(g, &op->s0, 0, 1, ok);
	if (!*ok) return;
	uint32_t coord, grad[2] = {0, 0};
	if (slot_is_cube(g, slot)) {
		uint32_t w3 = read_comp_f(g, &op->s0, 0, 2, ok); if (!*ok) return;
		coord = cube_coord(g, u, v, w3, grad);
	} else {
		if (unit == g->pcf_unit && g->pcf_du) { u = fop2(s, SpvOpFAdd, u, g->pcf_du); v = fop2(s, SpvOpFAdd, v, g->pcf_dv); }   /* filtered shadows: this tap's place in the grid */
		coord = result2(s, &s->func, SpvOpCompositeConstruct, t_vec(s, t_float(s), 2), u, v);
	}
	if (op->args[0]) {   /* sb_mode: usse.c's smp_gather -- the 2x2 footprint (OpImageGather's order is the hardware's: (x0,y1) (x1,y1) (x1,y0) (x0,y0)) and/or the bilinear fractions */
		if (slot_is_cube(g, slot)) { if (!g->why[0]) snprintf(g->why, sizeof g->why, "SMP gather/info of a cube map"); *ok = 0; return; }
		if (!g->cap_query) { g->cap_query = 1; INS(&s->caps, SpvOpCapability, 50 /*ImageQuery*/); }
		uint32_t simg = load_sampler(g, slot), tf = t_float(s);
		uint32_t img = result1(s, &s->func, SpvOpImage, g->t_img2d, simg);
		uint32_t isz = result2(s, &s->func, SpvOpImageQuerySizeLod, t_vec(s, t_int(s), 2), img, c_int(s, 0));
		uint32_t fsz = result1(s, &s->func, SpvOpConvertSToF, t_vec(s, tf, 2), isz), half = c_float(s, 0.5f), one = c_float(s, 1);
		uint32_t fx = fop2(s, SpvOpFSub, fop2(s, SpvOpFMul, u, composite_extract1(s, tf, fsz, 0)), half);
		uint32_t fy = fop2(s, SpvOpFSub, fop2(s, SpvOpFMul, v, composite_extract1(s, tf, fsz, 1)), half);
		uint32_t fu = fop2(s, SpvOpFSub, fx, glsl1(s, GLSLFloor, fx)), fv = fop2(s, SpvOpFSub, fy, glsl1(s, GLSLFloor, fy));
		UOperand d = op->dst; int f16 = op->sub3 == 2, words = f16 ? 2 : 4;
		if (op->args[0] == 2) {   /* info: u, v fractions, trilinear fraction 0, level 0 */
			uint32_t info[4] = {fu, fv, c_float(s, 0), 0};
			for (int c = 0; c < 3; c++) { if (f16) write_comp_f16(g, &d, 0, c, info[c], ok); else write_comp_f(g, &d, 0, c, info[c], ok); }
			write_raw_comp(g, &d, 0, 3, c_uint(s, 0), ok);
			return;
		}
		/* component 0 only: the translator doesn't see texture formats, and a gather is for a depth
		 * (one-component) shadow map -- usse.c's interpreter gathers every component */
		uint32_t g4 = result3(s, &s->func, SpvOpImageGather, t_vec(s, tf, 4), simg, coord, c_int(s, 0));
		for (int c = 0; c < 4; c++) { uint32_t id = composite_extract1(s, tf, g4, (uint32_t)c); if (f16) write_comp_f16(g, &d, 0, c, id, ok); else write_comp_f(g, &d, 0, c, id, ok); }
		if (op->args[0] == 3) {
			uint32_t nu = fop2(s, SpvOpFSub, one, fu), nv = fop2(s, SpvOpFSub, one, fv);
			uint32_t wt[4] = {fop2(s, SpvOpFMul, nu, nv), fop2(s, SpvOpFMul, fu, nv), fop2(s, SpvOpFMul, fu, fv), fop2(s, SpvOpFMul, nu, fv)};
			d.num = (uint16_t)(d.num + words);
			for (int c = 0; c < 4; c++) write_comp_f16(g, &d, 0, c, wt[c], ok);
		}
		return;
	}
	uint32_t texel, lodv = 0;
	if (op->sub2 == 1 || op->sub2 == 2) { lodv = read_comp_f(g, &op->s2, 0, 0, ok); if (!*ok) return; }   /* the lod mode's value (usse.c exec_smp) */
	if (g->is_fragment && (g->snap_mask >> (unit & 15) & 1) && !slot_is_cube(g, slot)) texel = snap_fetch(g, slot, unit, u, v);
	else texel = sample_lod(g, slot, unit, coord, op->sub2, lodv, grad);
	texel = srgb_fixup(g, unit, texel);
	int f16 = (op->sub3 == 2);
	for (int c = 0; c < 4; c++) if (op->mask & (1 << c)) {
		/* OpCompositeExtract: ResultType ResultId Composite, Literal Index -- the index is a raw
		 * literal word, not an <id>, so this goes straight through nid()+INS(), not result1/2/3
		 * (which now assert every argument is a nonzero <id>; this was a real bug here: an earlier,
		 * incomplete OpCompositeExtract via result1 -- missing its required index operand entirely
		 * -- silently corrupted every instruction after it in the stream, caught by spirv-val) */
		uint32_t id = composite_extract1(s, t_float(s), texel, (uint32_t)c);
		if (f16) write_comp_f16(g, &op->dst, 0, c, id, ok); else write_comp_f(g, &op->dst, 0, c, id, ok);
	}
}
static void emit_depthf(Gen *g, const UOp *op, int *ok) {
	uint32_t v = read_comp_f(g, &op->s0, 0, 0, ok);
	if (!*ok || !g->var_frag_depth) { if (!g->var_frag_depth) { if (!g->why[0]) snprintf(g->why, sizeof g->why, "DEPTHF outside a fragment program"); *ok = 0; } return; }
	store(&g->s, g->var_frag_depth, v);
}
/* LDST (loads, vertex programs): the address is base + offset (usse.c's exec_ldst), the base a
 * register the program formed from a guest pointer in an SA word -- directly (base in SA) or as
 * I32MAD's addend (Unit 13's skinning: PA4 = bone index * 48 + SA37). That SA word is the
 * "pointer" of a window: the host copies USSE_LDST_WINDOW bytes of guest memory from the
 * pointer's value into storage buffer 1 and describes each window in the uniform block
 * (UsseDrawUniforms.win); the shader maps an address to a window by range and reads the word. */
/* does w's destination include `word` of `bank`? A masked vector write covers its masked lanes'
 * words only: Unit 13's skinned shadow casters keep a bone-matrix pointer in pa10 and later do a
 * VDP into pa10.y (pa11) -- taken as the pointer's writer, every skinned caster fell to the CPU */
static int writes_word(const UOp *w, int bank, int word) {
	if (w->dst.bank != bank) return 0;
	if (!w->mask || w->op == OP_LDST || w->op == OP_I32MAD || w->op == OP_I32MAD2 || w->op == OP_I16MAD || w->op == OP_I8MAD) return w->dst.num == word;
	int lpw = lpw_of(w->dst.type);
	for (int c = 0; c < 4; c++) if ((w->mask >> c) & 1 && w->dst.num + c / lpw == word) return 1;
	return 0;
}
static int ldst_pointer_sa(const UOp *code, int i) {
	const UOp *op = &code[i];
	if (op->s0.bank == BK_SA) return op->s0.num;
	if (op->s0.bank != BK_PA && op->s0.bank != BK_TEMP && op->s0.bank != BK_O) return -1;   /* (an output register as scratch: Killzone's skinned meshes form the bone pointer in o0-o2) */
	for (int j = i - 1; j >= 0; j--) {   /* the latest writer of the base register */
		const UOp *w = &code[j];
		if (w->op == OP_NOP || w->op == OP_SMLSI || w->op == OP_SMBO || w->op == OP_BR || w->op == OP_KILL) continue;
		if (!writes_word(w, op->s0.bank, op->s0.num)) continue;
		if (w->op == OP_I32MAD && w->sub2 == 2 && w->s2.bank == BK_SA) return w->s2.num;   /* a full-width addend from SA: the pointer */
		if (w->op == OP_I32MAD2 && !w->sub2 && w->s2.bank == BK_SA && !(w->s2.negabs & 1)) return w->s2.num;   /* I32MAD2's addend is always whole (Killzone's quarter-res pass: (960y + x) * 32 + SA11) */
		return -1;
	}
	return -1;
}
static uint32_t sa_win_comp(Gen *g, int w, int comp) {   /* UsseDrawUniforms.win[w][comp], member 2 of the SA block */
	Spv *s = &g->s; uint32_t id = nid(s);
	INS(&s->func, SpvOpAccessChain, g->ptr_sa_u, id, g->var_sa, c_uint(s, 2), c_uint(s, (uint32_t)w), c_uint(s, (uint32_t)comp));
	return load(s, t_uint(s), id);
}
/* a sampled texel through the draw's sRGB masks (UsseDrawUniforms.tex_srgb, member 3 of the SA
 * block): bit `unit` of word 0 decodes the colour channels sRGB -> linear, of word 1 encodes them
 * back -- gfx_soft.c's tex_gamma, on the filtered value as there; alpha untouched. The program is
 * cached by content and shared by draws with and without gamma textures, so this is a runtime
 * select, not a translation-time choice. */
static uint32_t srgb_mask_comp(Gen *g, int comp) {
	Spv *s = &g->s; uint32_t id = nid(s);
	INS(&s->func, SpvOpAccessChain, g->ptr_sa_u, id, g->var_sa, c_uint(s, 3), c_uint(s, (uint32_t)comp));
	return load(s, t_uint(s), id);
}
static uint32_t srgb_fixup(Gen *g, int unit, uint32_t texel) {
	Spv *s = &g->s; uint32_t tf = t_float(s), tb = t_bool(s), tu = t_uint(s), bit = c_uint(s, 1u << (unit & 15));
	uint32_t dec = result2(s, &s->func, SpvOpINotEqual, tb, result2(s, &s->func, SpvOpBitwiseAnd, tu, srgb_mask_comp(g, 0), bit), c_uint(s, 0));
	uint32_t enc = result2(s, &s->func, SpvOpINotEqual, tb, result2(s, &s->func, SpvOpBitwiseAnd, tu, srgb_mask_comp(g, 1), bit), c_uint(s, 0));
	uint32_t ch[4];
	for (int c = 0; c < 4; c++) ch[c] = composite_extract1(s, tf, texel, (uint32_t)c);
	for (int c = 0; c < 3; c++) {
		uint32_t x = fclamp(s, ch[c], 0.0f, 1.0f);
		uint32_t lin = result3(s, &s->func, SpvOpSelect, tf, result2(s, &s->func, SpvOpFOrdLessThanEqual, tb, x, c_float(s, 0.04045f)),
		                       fop2(s, SpvOpFDiv, x, c_float(s, 12.92f)),
		                       glsl2(s, GLSLPow, fop2(s, SpvOpFDiv, fop2(s, SpvOpFAdd, x, c_float(s, 0.055f)), c_float(s, 1.055f)), c_float(s, 2.4f)));
		uint32_t srgb = result3(s, &s->func, SpvOpSelect, tf, result2(s, &s->func, SpvOpFOrdLessThanEqual, tb, x, c_float(s, 0.0031308f)),
		                        fop2(s, SpvOpFMul, x, c_float(s, 12.92f)),
		                        fop2(s, SpvOpFSub, fop2(s, SpvOpFMul, c_float(s, 1.055f), glsl2(s, GLSLPow, x, c_float(s, 1.0f / 2.4f))), c_float(s, 0.055f)));
		ch[c] = result3(s, &s->func, SpvOpSelect, tf, dec, lin, result3(s, &s->func, SpvOpSelect, tf, enc, srgb, ch[c]));
	}
	uint32_t id = nid(s); INS(&s->func, SpvOpCompositeConstruct, t_vec(s, tf, 4), id, ch[0], ch[1], ch[2], ch[3]);
	return id;
}
/* a sample of `slot` at `coord` with the level of detail the hardware would use: the bound texture's LOD bias
 * (UsseDrawUniforms.lod_bias, member 6 -- per draw like the sRGB masks, so read at run time) on top of the SMP's lod
 * mode, as usse.c's exec_smp reads it: 1 adds src2's first component, 2 replaces the level with it; 0 (and 3, gradients,
 * not decoded) the GPU's own from the fragment's derivatives. A vertex program has none: level 0 + bias (+ src2). Before,
 * every sample took the GPU's own level (fragment) or level 0 (vertex), with neither the bias nor the lod modes. */
static uint32_t sample_lod(Gen *g, int slot, int unit, uint32_t coord, int lod_mode, uint32_t lodv, const uint32_t *grad) {
	Spv *s = &g->s; uint32_t tv4 = t_vec(s, t_float(s), 4), id = nid(s), bid = nid(s);
	INS(&s->func, SpvOpAccessChain, g->ptr_sa_u, bid, g->var_sa, c_uint(s, 6), c_uint(s, (uint32_t)(unit & 15) >> 2), c_uint(s, (uint32_t)unit & 3));
	uint32_t bias = result1(s, &s->func, SpvOpBitcast, t_float(s), load(s, t_uint(s), bid));
	if (lod_mode == 1 && lodv) bias = fop2(s, SpvOpFAdd, bias, lodv);
	if (g->is_fragment && grad && grad[0] && grad[1] && !(lod_mode == 2 && lodv)) {   /* a cube face: cube_coord's derivatives, the bias as their scale */
		uint32_t k = glsl1(s, GLSLExp2, bias), tv2 = t_vec(s, t_float(s), 2);
		INS(&s->func, SpvOpImageSampleExplicitLod, tv4, id, load_sampler(g, slot), coord, 4 /*Grad*/,
		    result2(s, &s->func, 142 /*OpVectorTimesScalar*/, tv2, grad[0], k), result2(s, &s->func, 142, tv2, grad[1], k));
	}
	else if (g->is_fragment && !(lod_mode == 2 && lodv)) INS(&s->func, SpvOpImageSampleImplicitLod, tv4, id, load_sampler(g, slot), coord, 1 /*Bias*/, bias);
	else INS(&s->func, SpvOpImageSampleExplicitLod, tv4, id, load_sampler(g, slot), coord, 2 /*Lod*/, lod_mode == 2 && lodv ? fop2(s, SpvOpFAdd, lodv, bias) : bias);
	return id;
}
/* render scale > 1 (gfx_gpu.c RECOMP_RENDER_SCALE): a texture that reads a render target's bytes at
 * another texel size -- Unit 13's lighting reads its 720-wide F32F32 G-buffer as 1440-wide bytes,
 * two texels a pixel -- picks WHICH word of the pixel by where in the pixel it samples, a game
 * shader written for fragments at pixel centres. At n times the resolution its fragments sit at
 * +-1/(2n) of a pixel from those centres and land in the neighbouring word: every lit surface came
 * out in stripes of swapped G-buffer words. Such a read (gfx_gpu.c's snap_mask: point-sampled, over
 * a GPU target, another texel size) is taken as the game meant it: the coordinate it would have had
 * at this pixel's 1x centre (from its screen derivatives: the coordinate is affine across the
 * pixel's n x n fragments), the 1x texel that names, and that texel's own n x n block at this
 * fragment's place in it -- the right word, from the scaled surface's own sample. A texture not at
 * the render scale (decoded from guest memory) gives the 1x texel itself. UsseDrawUniforms.tex_srgb
 * word 2: the units bound at the render scale; word 3: the render scale, x | y << 16. */
static uint32_t snap_fetch(Gen *g, int slot, int unit, uint32_t u, uint32_t v) {
	Spv *s = &g->s; uint32_t tf = t_float(s), tb = t_bool(s), tu = t_uint(s), ti = t_int(s);
	if (!g->cap_query) { g->cap_query = 1; INS(&s->caps, SpvOpCapability, 50 /*ImageQuery*/); }
	uint32_t w3 = srgb_mask_comp(g, 3);   /* the render scale: x in the low half, y in the high (wider aspect ratios scale x more) */
	uint32_t Sk[2] = { result1(s, &s->func, SpvOpConvertUToF, tf, result2(s, &s->func, SpvOpBitwiseAnd, tu, w3, c_uint(s, 0xffffu))),
	                   result1(s, &s->func, SpvOpConvertUToF, tf, result2(s, &s->func, SpvOpShiftRightLogical, tu, w3, c_uint(s, 16))) };
	uint32_t scaled = result2(s, &s->func, SpvOpINotEqual, tb, result2(s, &s->func, SpvOpBitwiseAnd, tu, srgb_mask_comp(g, 2), c_uint(s, 1u << (unit & 15))), c_uint(s, 0));
	uint32_t su[2] = { result3(s, &s->func, SpvOpSelect, tf, scaled, Sk[0], c_float(s, 1.0f)), result3(s, &s->func, SpvOpSelect, tf, scaled, Sk[1], c_float(s, 1.0f)) };
	uint32_t fc = load(s, t_vec(s, tf, 4), g->var_frag_coord), zero = c_float(s, 0), half = c_float(s, 0.5f);
	uint32_t f[2], o[2];
	for (int k = 0; k < 2; k++) {   /* this fragment's place in its n x n block, and the offset to the block's centre (scaled pixels) */
		uint32_t X = glsl1(s, GLSLFloor, composite_extract1(s, tf, fc, (uint32_t)k));
		f[k] = fop2(s, SpvOpFSub, X, fop2(s, SpvOpFMul, Sk[k], glsl1(s, GLSLFloor, fop2(s, SpvOpFDiv, X, Sk[k]))));
		o[k] = fop2(s, SpvOpFSub, fop2(s, SpvOpFMul, Sk[k], half), fop2(s, SpvOpFAdd, f[k], half));
	}
	uint32_t c[2] = { u, v }, img = result1(s, &s->func, SpvOpImage, g->t_img2d, load_sampler(g, slot));
	uint32_t isz = result2(s, &s->func, SpvOpImageQuerySizeLod, t_vec(s, ti, 2), img, c_int(s, 0));
	uint32_t T[2];
	for (int k = 0; k < 2; k++) {
		uint32_t dx = result1(s, &s->func, SpvOpDPdx, tf, c[k]), dy = result1(s, &s->func, SpvOpDPdy, tf, c[k]);
		uint32_t c1 = fop2(s, SpvOpFAdd, c[k], fop2(s, SpvOpFAdd, fop2(s, SpvOpFMul, dx, o[0]), fop2(s, SpvOpFMul, dy, o[1])));   /* the coordinate at the 1x centre */
		uint32_t n1 = fop2(s, SpvOpFDiv, result1(s, &s->func, SpvOpConvertSToF, tf, composite_extract1(s, ti, isz, (uint32_t)k)), su[k]);   /* the texture's 1x size */
		uint32_t t1 = glsl2(s, GLSLFMin, glsl2(s, GLSLFMax, glsl1(s, GLSLFloor, fop2(s, SpvOpFMul, c1, n1)), zero), fop2(s, SpvOpFSub, n1, c_float(s, 1.0f)));
		uint32_t Tk = fop2(s, SpvOpFAdd, fop2(s, SpvOpFMul, t1, su[k]), result3(s, &s->func, SpvOpSelect, tf, scaled, f[k], zero));
		T[k] = result1(s, &s->func, SpvOpConvertFToS, ti, Tk);
	}
	uint32_t coord = result2(s, &s->func, SpvOpCompositeConstruct, t_vec(s, ti, 2), T[0], T[1]);
	uint32_t texel = nid(s); INS(&s->func, SpvOpImageFetch, t_vec(s, tf, 4), texel, img, coord, 2 /*Lod*/, c_int(s, 0));
	return texel;
}
static void emit_ldst(Gen *g, const UOp *op, const UOp *code, int i, int *ok) {
	Spv *s = &g->s;
	if (op->sub2) { if (!g->why[0]) snprintf(g->why, sizeof g->why, "LDST store"); *ok = 0; return; }
	int sa = ldst_pointer_sa(code, i), w;
	for (w = 0; w < g->nwin && g->win_sa[w] != sa; w++);
	if (sa < 0 || w == g->nwin) { if (!g->why[0]) snprintf(g->why, sizeof g->why, "LDST base register %d.%d isn't formed from an SA pointer", op->s0.bank, op->s0.num); *ok = 0; return; }
	int elemsz = op->args[3];
	uint32_t base = read_raw_comp(g, &op->s0, 0, 0, ok);
	uint32_t off = op->s1.bank == BK_IMM ? c_uint(s, (uint32_t)op->s1.num * (uint32_t)elemsz) : uop2(s, SpvOpISub, read_raw_comp(g, &op->s1, 0, 0, ok), c_uint(s, 0x10000));
	if (!op->args[0]) off = uop2(s, SpvOpIAdd, off, c_uint(s, 4));
	uint32_t off2 = read_raw_comp(g, &op->s2, 0, 0, ok);
	if (!*ok) return;
	uint32_t addr = uop2(s, SpvOpIAdd, uop2(s, SpvOpIAdd, base, off), off2);
	uint32_t wbase = sa_win_comp(g, w, 0), wlen = sa_win_comp(g, w, 1), woff = sa_win_comp(g, w, 2);   /* uvec4 win[w] = { base, bytes, word offset, 0 } */
	for (int k = 0; k < op->reps; k++) {
		uint32_t a = k ? uop2(s, SpvOpIAdd, addr, c_uint(s, (uint32_t)(k * elemsz))) : addr;
		uint32_t rel = uop2(s, SpvOpISub, a, wbase);
		uint32_t inw = result2(s, &s->func, SpvOpULessThan, t_bool(s), rel, wlen);
		uint32_t idx = uop2(s, SpvOpIAdd, woff, uop2(s, SpvOpShiftRightLogical, rel, c_uint(s, 2)));
		idx = result3(s, &s->func, SpvOpSelect, t_uint(s), inw, idx, c_uint(s, 0));   /* out of the window: read word 0, then zero it */
		uint32_t word = load(s, t_uint(s), access2(s, g->ptr_vin_u, g->var_win, c_uint(s, 0), idx));
		if (elemsz < 4) {
			uint32_t shift = uop2(s, SpvOpShiftLeftLogical, uop2(s, SpvOpBitwiseAnd, rel, c_uint(s, (uint32_t)(4 - elemsz))), c_uint(s, 3));
			word = uop2(s, SpvOpBitwiseAnd, uop2(s, SpvOpShiftRightLogical, word, shift), c_uint(s, elemsz == 2 ? 0xFFFFu : 0xFFu));
		}
		word = result3(s, &s->func, SpvOpSelect, t_uint(s), inw, word, c_uint(s, 0));
		if (op->dst.bank == BK_IDXD1 || op->dst.bank == BK_IDXD2) continue;   /* usse.c's exec_ldst stores with bank_write_raw, which drops indexed words */
		RegPtr p = bank_ptr(g, op->dst.bank, op->dst.num + k);
		if (!p.ok) { *ok = 0; return; }
		store(s, p.ptr, word);
	}
}
/* LDST in a fragment program: its pointers (the SA words of ldst_pointer_sa) lie in a GPU render target -- Killzone's
 * quarter-res pass reads four rows of its HDR scene through four uniform-buffer pointers. The target is bound as a
 * texture (window w: ldst_img[w]) and a load becomes a texel fetch, packed back into the guest's bytes; the window's
 * place in the uniform block is UsseDrawUniforms.win[w] = { the target's address, its bytes, its row bytes, kind |
 * bytes a pixel << 8 } (kind 1: F16F16F16F16 -> PackHalf2x16 of .xy or .zw; else 8-bit RGBA -> PackUnorm4x8).
 * gfx_gpu.c sends the draw to the software path when a pointer isn't inside a GPU target. Outside the window: 0. */
static uint32_t umax2(Spv *s, uint32_t a, uint32_t b) { return result3(s, &s->func, SpvOpSelect, t_uint(s), result2(s, &s->func, SpvOpUGreaterThan, t_bool(s), a, b), a, b); }
static void emit_ldst_target(Gen *g, const UOp *op, const UOp *code, int i, int *ok) {
	Spv *s = &g->s;
	if (op->sub2) { if (!g->why[0]) snprintf(g->why, sizeof g->why, "LDST store"); *ok = 0; return; }
	int sa = ldst_pointer_sa(code, i), w;
	for (w = 0; w < g->nwin && g->win_sa[w] != sa; w++);
	if (sa < 0 || w == g->nwin || !g->ldst_img[w]) { if (!g->why[0]) snprintf(g->why, sizeof g->why, "LDST base register %d.%d isn't formed from an SA pointer", op->s0.bank, op->s0.num); *ok = 0; return; }
	int elemsz = op->args[3];
	uint32_t tu = t_uint(s), ti = t_int(s), tf = t_float(s), tb = t_bool(s);
	uint32_t base = read_raw_comp(g, &op->s0, 0, 0, ok);
	uint32_t off = op->s1.bank == BK_IMM ? c_uint(s, (uint32_t)op->s1.num * (uint32_t)elemsz) : uop2(s, SpvOpISub, read_raw_comp(g, &op->s1, 0, 0, ok), c_uint(s, 0x10000));
	if (!op->args[0]) off = uop2(s, SpvOpIAdd, off, c_uint(s, 4));
	uint32_t off2 = read_raw_comp(g, &op->s2, 0, 0, ok);
	if (!*ok) return;
	uint32_t addr = uop2(s, SpvOpIAdd, uop2(s, SpvOpIAdd, base, off), off2);
	uint32_t one = c_uint(s, 1);
	uint32_t tbase = sa_win_comp(g, w, 0), tbytes = sa_win_comp(g, w, 1), rowb = umax2(s, sa_win_comp(g, w, 2), one), kb = sa_win_comp(g, w, 3);
	uint32_t bpp = umax2(s, uop2(s, SpvOpBitwiseAnd, uop2(s, SpvOpShiftRightLogical, kb, c_uint(s, 8)), c_uint(s, 0xff)), one);
	uint32_t is_f16 = result2(s, &s->func, SpvOpIEqual, tb, uop2(s, SpvOpBitwiseAnd, kb, c_uint(s, 0xff)), one);
	uint32_t sc = srgb_mask_comp(g, 3);   /* the render scale: a guest pixel is sx x sy texels */
	uint32_t sx = umax2(s, uop2(s, SpvOpBitwiseAnd, sc, c_uint(s, 0xffff)), one), sy = umax2(s, uop2(s, SpvOpShiftRightLogical, sc, c_uint(s, 16)), one);
	uint32_t img = result1(s, &s->func, SpvOpImage, g->t_img2d, load(s, g->t_simg, g->ldst_img[w]));
	for (int k = 0; k < op->reps; k++) {
		uint32_t a = k ? uop2(s, SpvOpIAdd, addr, c_uint(s, (uint32_t)(k * elemsz))) : addr;
		uint32_t rel = uop2(s, SpvOpISub, a, tbase);
		uint32_t inw = result2(s, &s->func, SpvOpULessThan, tb, rel, tbytes);
		uint32_t row = uop2(s, 134 /*UDiv*/, rel, rowb), inrow = uop2(s, 137 /*UMod*/, rel, rowb);
		uint32_t col = uop2(s, 134 /*UDiv*/, inrow, bpp), sub = uop2(s, SpvOpShiftRightLogical, uop2(s, 137 /*UMod*/, inrow, bpp), c_uint(s, 2));
		row = result3(s, &s->func, SpvOpSelect, tu, inw, row, c_uint(s, 0)); col = result3(s, &s->func, SpvOpSelect, tu, inw, col, c_uint(s, 0));
		uint32_t cx = result1(s, &s->func, SpvOpBitcast, ti, uop2(s, SpvOpIMul, col, sx)), cy = result1(s, &s->func, SpvOpBitcast, ti, uop2(s, SpvOpIMul, row, sy));
		uint32_t coord = result2(s, &s->func, SpvOpCompositeConstruct, t_vec(s, ti, 2), cx, cy);
		uint32_t texel = nid(s); INS(&s->func, SpvOpImageFetch, t_vec(s, tf, 4), texel, img, coord, 2 /*Lod*/, c_int(s, 0));
		uint32_t xy = result2(s, &s->func, SpvOpCompositeConstruct, t_vec(s, tf, 2), composite_extract1(s, tf, texel, 0), composite_extract1(s, tf, texel, 1));
		uint32_t zw = result2(s, &s->func, SpvOpCompositeConstruct, t_vec(s, tf, 2), composite_extract1(s, tf, texel, 2), composite_extract1(s, tf, texel, 3));
		uint32_t h01 = result3(s, &s->func, SpvOpExtInst, tu, s->glsl_ext, 58 /*PackHalf2x16*/, xy), h23 = result3(s, &s->func, SpvOpExtInst, tu, s->glsl_ext, 58 /*PackHalf2x16*/, zw);
		uint32_t w16 = result3(s, &s->func, SpvOpSelect, tu, result2(s, &s->func, SpvOpIEqual, tb, sub, c_uint(s, 0)), h01, h23);
		uint32_t w8 = result3(s, &s->func, SpvOpExtInst, tu, s->glsl_ext, 55 /*PackUnorm4x8*/, texel);
		uint32_t word = result3(s, &s->func, SpvOpSelect, tu, is_f16, w16, w8);
		if (elemsz < 4) {
			uint32_t shift = uop2(s, SpvOpShiftLeftLogical, uop2(s, SpvOpBitwiseAnd, rel, c_uint(s, (uint32_t)(4 - elemsz))), c_uint(s, 3));
			word = uop2(s, SpvOpBitwiseAnd, uop2(s, SpvOpShiftRightLogical, word, shift), c_uint(s, elemsz == 2 ? 0xFFFFu : 0xFFu));
		}
		word = result3(s, &s->func, SpvOpSelect, tu, inw, word, c_uint(s, 0));
		if (op->dst.bank == BK_IDXD1 || op->dst.bank == BK_IDXD2) continue;
		RegPtr p = bank_ptr(g, op->dst.bank, op->dst.num + k);
		if (!p.ok) { *ok = 0; return; }
		store(s, p.ptr, word);
	}
}
static void emit_limm(Gen *g, const UOp *op, int *ok) {
	if (op->dst.bank == BK_IDXD1 || op->dst.bank == BK_IDXD2) return;   /* usse.c's exec_limm: bank_write_raw drops indexed words */
	RegPtr p = bank_ptr(g, op->dst.bank, op->dst.num);
	if (!p.ok) { *ok = 0; return; }
	store(&g->s, p.ptr, c_uint(&g->s, (uint32_t)op->imm));
}
/* KILL: op->imm 0 is unconditional; 1/2 test !p[0]/!p[1]; 3 (else) tests p[0] (exec_kill's own
 * switch, ported directly: predicates are real runtime values now that VTST is supported). An
 * unconditional kill is a block terminator with nothing conditional about it, so it's simplest as
 * its own case (no branch needed); the conditional cases need real structured control flow: branch
 * to a block that kills, or fall through to one that doesn't, both landing in the same merge block
 * the rest of the program continues in. */
static void emit_kill(Gen *g, const UOp *op, int *ok) {
	Spv *s = &g->s;
	if (op->imm == 0) { INS0(&s->func, SpvOpKill); uint32_t lbl = nid(s); INS(&s->func, SpvOpLabel, lbl); return; }
	uint32_t p0 = load(s, t_bool(s), pred_ptr(g, 0)), p1 = load(s, t_bool(s), pred_ptr(g, 1));
	uint32_t cond;
	if (op->imm == 1) cond = result1(s, &s->func, SpvOpLogicalNot, t_bool(s), p0);
	else if (op->imm == 2) cond = result1(s, &s->func, SpvOpLogicalNot, t_bool(s), p1);
	else cond = p0;
	uint32_t killblk = nid(s), merge = nid(s);
	INS(&s->func, SpvOpSelectionMerge, merge, 0 /*None*/);
	INS(&s->func, SpvOpBranchConditional, cond, killblk, merge);
	INS(&s->func, SpvOpLabel, killblk);
	INS0(&s->func, SpvOpKill);
	INS(&s->func, SpvOpLabel, merge);
	(void)ok;
}

/* ---------------------------------------------------------------- top-level translate */
static void module_header(Spv *s, int is_fragment, uint32_t entry_fn, uint32_t *iface, int niface) {
	INS(&s->caps, SpvOpCapability, 1 /*Shader*/);
	/* s->glsl_ext is allocated up front in usse_spirv_translate, not here: glsl1/glsl2 (floor,
	 * inversesqrt, log2, exp2 -- VNMAD/VCOMP/VTST's ALU table) are called during the instruction
	 * walk, long before this function runs; allocating it here left it 0 (not-yet-an-id) for every
	 * one of those calls, corrupting the OpExtInst instructions with id 0 -- caught by the
	 * assertion in result1/2/3 once that was added, see the commit this landed in. */
	/* OpExtInstImport's literal string "GLSL.std.450" packed little-endian, NUL-padded to a word multiple */
	{ uint32_t words[4] = {0}; memcpy(words, "GLSL.std.450", 12); uint32_t args[1 + 4]; args[0] = s->glsl_ext; memcpy(args + 1, words, 16); ins(&s->ext, SpvOpExtInstImport, 5, args); }
	INS(&s->memmodel, SpvOpMemoryModel, 0 /*Logical*/, 1 /*GLSL450*/);
	{
		uint32_t args[3 + 16 + 4]; int n = 0;
		args[n++] = (uint32_t)(is_fragment ? SpvExecModelFragment : SpvExecModelVertex); args[n++] = entry_fn;
		/* name "main": a SPIR-V LiteralString is NUL-terminated within its packed words -- "main" is
		 * exactly 4 bytes, so the terminator needs a whole extra all-zero word, not just a byte
		 * tacked onto the same one (that was a real bug here: without it, a conformant reader keeps
		 * scanning for the NUL into the next word -- the first interface id -- and misparses
		 * everything after) */
		args[n++] = 0x6E69616D /*"main"*/; args[n++] = 0;
		for (int i = 0; i < niface; i++) args[n++] = iface[i];
		ins(&s->entry, SpvOpEntryPoint, n, args);
	}
	if (is_fragment) INS(&s->execmodes, SpvOpExecutionMode, entry_fn, 7 /*OriginUpperLeft*/);
}
static void finish_module(SpirvModule *out, Spv *s, uint32_t entry_fn) {
	INS0(&s->func, SpvOpReturn);
	Buf hdr = {0}; buf_push(&hdr, 0x07230203); buf_push(&hdr, 0x00010300); buf_push(&hdr, 0); buf_push(&hdr, s->next_id); buf_push(&hdr, 0);
	Buf fn_type = {0}; { uint32_t ft = nid(s); INS(&fn_type, SpvOpTypeFunction, ft, t_void(s)); /* ft used below */
		Buf fnhdr = {0}; INS(&fnhdr, 54 /*OpFunction*/, t_void(s), entry_fn, 0 /*None*/, ft);
		Buf lbl = {0}; INS(&lbl, SpvOpLabel, nid(s));
		/* recompute header bound now that fn_type/fnhdr/lbl allocated more ids */
		hdr.w[3] = s->next_id;
		Buf all = {0};
		Buf *parts[] = {&hdr, &s->caps, &s->ext, &s->memmodel, &s->entry, &s->execmodes, &s->decos, &s->types, &fn_type, &fnhdr, &lbl, &s->func};
		for (unsigned i = 0; i < sizeof parts / sizeof *parts; i++) for (uint32_t j = 0; j < parts[i]->n; j++) buf_push(&all, parts[i]->w[j]);
		buf_push(&all, (6u << 16) | 56 /*OpFunctionEnd -- wrong word count fixed below*/);
		all.n--;   /* OpFunctionEnd takes no operands: word count 1 */
		buf_push(&all, (1u << 16) | 56);
		out->words = all.w; out->nwords = all.n; out->ok = 1;
		free(fn_type.w); free(fnhdr.w); free(lbl.w);
	}
}

/* a non-dependent texture read (a fragment iterator with is_tex set): the coordinate is an
 * already-interpolated vertex output, not something the fragment program computes -- mirrors
 * gfx_soft.c's sample_nondep, reading from the hardware-interpolated varying Input instead of a
 * software-interpolated vary[] array. */
static uint32_t read_vary_comp(Gen *g, int word, int *ok) {
	int loc = (word - 4) / 4, comp = (word - 4) & 3;
	if (word < 4 || loc >= g->nvary) { if (!g->why[0]) snprintf(g->why, sizeof g->why, "iterator reads vertex output word %d, the vertex program writes %d varyings", word, g->nvary); *ok = 0; return c_float(&g->s, 0); }
	Spv *s = &g->s;
	uint32_t v4 = load(s, t_vec(s, t_float(s), 4), g->var_vary[loc]);
	return composite_extract1(s, t_float(s), v4, (uint32_t)comp);
}
/* canonical varyings: the count of components the draw's vertex program writes for semantic k (usse_spirv_vary_ncomp's
 * words, UsseDrawUniforms.vfetch[2..3]), and the fragment's Input for it (declared for every semantic an iterator names) */
static uint32_t canon_ncomp(Gen *g, int k) {
	Spv *s = &g->s; uint32_t pw = nid(s); INS(&s->func, SpvOpAccessChain, g->ptr_sa_u, pw, g->var_sa, c_uint(s, 4), c_uint(s, k < 10 ? 2u : 3u));
	return uop2(s, SpvOpBitwiseAnd, uop2(s, SpvOpShiftRightLogical, load(s, t_uint(s), pw), c_uint(s, 3u * (uint32_t)(k < 10 ? k : k - 10))), c_uint(s, 7));
}
static uint32_t canon_in(Gen *g, int k, int c) { Spv *s = &g->s; return composite_extract1(s, t_float(s), load(s, t_vec(s, t_float(s), 4), g->var_vary[k]), (uint32_t)c); }
static uint32_t ugt(Gen *g, uint32_t a, uint32_t k) { Spv *s = &g->s; return result2(s, &s->func, SpvOpUGreaterThan, t_bool(s), a, c_uint(s, k)); }
static uint32_t fsel(Gen *g, uint32_t cond, uint32_t a, uint32_t b) { Spv *s = &g->s; return result3(s, &s->func, SpvOpSelect, t_float(s), cond, a, b); }
/* emit_sample_nondep's coordinate with canonical varyings: what the positional layout's reads give for every count the
 * vertex program can have (u from a present semantic, v from 2 components, the projection by the 4th (mode 2) or 3rd
 * component from 3 or 4, a cube map's direction unprojected with w from 3). The components a vertex program lacks it
 * writes as 0, and an interpolated 0 is exactly 0: u, v and w come straight from the varying -- a coordinate that is an
 * interpolant as it stands, as the positional module's, rather than one passed through a select (Metal sampled those
 * slightly differently: single pixels off by up to 171/255 in Killzone captures). Only the projection selects on the
 * draw's count */
static void canon_nondep_coord(Gen *g, const UsseIter *it, int cube, uint32_t *u, uint32_t *v, uint32_t *w3) {
	Spv *s = &g->s; const int k = it->tex_coord_src;
	uint32_t in[4]; for (int c = 0; c < 4; c++) in[c] = canon_in(g, k, c);
	*u = in[0]; *v = in[1];
	if (cube) { *w3 = in[2]; return; }
	if (it->tex_proj >= 2) {
		uint32_t n = canon_ncomp(g, k);
		uint32_t w = it->tex_proj == 2 ? fsel(g, ugt(g, n, 3), in[3], in[2]) : in[2], div = ugt(g, n, 2);
		*u = fsel(g, div, fop2(s, SpvOpFDiv, *u, w), *u); *v = fsel(g, div, fop2(s, SpvOpFDiv, *v, w), *v);
	}
}
static void emit_sample_nondep(Gen *g, const UsseIter *it, const uint32_t sa_tex_unit[256], int *ok) {
	Spv *s = &g->s; (void)sa_tex_unit;   /* non-dependent iterator sampling: the unit comes straight from fl->iters[].tex_unit, no SA lookup needed */
	const int canon = canon_vary && !(it->point_coord && g->var_point) && it->tex_coord_src >= 0 && it->tex_coord_src < 10;
	int base = !canon && it->tex_coord_src >= 0 && it->tex_coord_src < 10 && g->vl ? g->vl->o_tex[it->tex_coord_src] : -1;
	uint32_t u = c_float(s, 0), v = c_float(s, 0), cw3 = 0;
	if (canon) {
		int slot = it->tex_unit >= 0 && it->tex_unit < 16 ? sampler_slot(g, it->tex_unit) : -1;
		canon_nondep_coord(g, it, slot >= 0 && slot_is_cube(g, slot), &u, &v, &cw3);
	} else if (it->point_coord && g->var_point) {   /* the point-sprite coordinate is the texture coordinate */
		uint32_t pc = load(s, t_vec(s, t_float(s), 4), g->var_point);
		u = composite_extract1(s, t_float(s), pc, 0); v = composite_extract1(s, t_float(s), pc, 1); base = -1;
	} else if (base >= 0) {
		u = read_vary_comp(g, base, ok);
		if (g->vl->ncomp_tex[it->tex_coord_src] > 1) v = read_vary_comp(g, base + 1, ok);
		if (it->tex_proj >= 2 && g->vl->ncomp_tex[it->tex_coord_src] > 2) {   /* gfx_soft.c's proj_divisor: mode 2 divides by the 4th component (.xyw) when there is one, else (and mode 3) by the 3rd -- dividing mode 2 by the 3rd put Uncharted's river water's third normal map off and its refraction showed the paddle as dark holes in the surface (2026-10-15) */
			int wc = it->tex_proj == 2 && g->vl->ncomp_tex[it->tex_coord_src] > 3 ? 3 : 2;
			uint32_t w = read_vary_comp(g, base + wc, ok);
			u = fop2(s, SpvOpFDiv, u, w); v = fop2(s, SpvOpFDiv, v, w);
		}
	}
	int slot = it->tex_unit >= 0 && it->tex_unit < 16 ? sampler_slot(g, it->tex_unit) : -1;
	/* the iterator's result format decides the PA packing (usse_pack_iter): F16 pairs or F32 words.
	 * Writing F32 where the program reads F16 turned a white texel into green (1.0f's high half
	 * lands in G as ~1.9, its zero low half in R) -- Shovel Knight's logo composite */
	UOperand dst; memset(&dst, 0, sizeof dst); dst.bank = BK_PA; dst.num = (uint16_t)it->pa; dst.type = it->result_fmt == 2 ? TY_F16 : TY_F32;
	if (slot < 0) { for (int c = 0; c < 4; c++) write_comp_f(g, &dst, 0, c, c_float(s, c == 3 ? 1.0f : 0.0f), ok); return; }
	uint32_t coord, grad[2] = {0, 0};
	if (slot_is_cube(g, slot)) {   /* the third component unprojected (a direction), as gfx_soft.c's iterator sampling reads it */
		uint32_t w3 = canon ? cw3 : base >= 0 && g->vl->ncomp_tex[it->tex_coord_src] > 2 ? read_vary_comp(g, base + 2, ok) : c_float(s, 0);
		if (base >= 0 && it->tex_proj >= 2 && g->vl->ncomp_tex[it->tex_coord_src] > 2) { u = read_vary_comp(g, base, ok); v = read_vary_comp(g, base + 1, ok); }
		coord = cube_coord(g, u, v, w3, grad);
	} else coord = result2(s, &s->func, SpvOpCompositeConstruct, t_vec(s, t_float(s), 2), u, v);
	uint32_t texel = srgb_fixup(g, it->tex_unit, (g->snap_mask >> (it->tex_unit & 15) & 1) && !slot_is_cube(g, slot) ? snap_fetch(g, slot, it->tex_unit, u, v)
	                                               : sample_lod(g, slot, it->tex_unit, coord, 0, 0, grad));
	for (int c = 0; c < 4; c++) write_comp_f(g, &dst, 0, c, composite_extract1(s, t_float(s), texel, (uint32_t)c), ok);
}
/* fragment-shader prologue: pack every iterator (usse_fragment_layout) into PA exactly like
 * gfx_soft.c's shade_interp does per pixel -- the difference is where the varying value comes
 * from (the GPU's own perspective-correct interpolator, via var_vary, instead of a software
 * barycentric blend). Packing follows usse_pack_iter: F16 or F32 per the iterator's format. */
static void emit_frag_prologue(Gen *g, const uint32_t sa_tex_unit[256], int *ok) {
	const UsseFragLayout *fl = g->fl; if (!fl || (!g->vl && !canon_vary)) { if (!g->why[0]) snprintf(g->why, sizeof g->why, "no %s layout", fl ? "vertex" : "fragment"); *ok = 0; return; }
	for (int i = 0; i < fl->niters && *ok; i++) {
		const UsseIter *it = &fl->iters[i];
		if (it->is_tex) { emit_sample_nondep(g, it, sa_tex_unit, ok); continue; }
		if (it->point_coord && g->var_point) {   /* the point-sprite coordinate (usse_spirv_set_points): (u, v, 0, 1) */
			uint32_t pc = load(&g->s, t_vec(&g->s, t_float(&g->s), 4), g->var_point);
			UOperand dst; memset(&dst, 0, sizeof dst); dst.bank = BK_PA; dst.num = (uint16_t)it->pa;
			dst.type = it->storage == 1 ? TY_F16 : it->storage == 2 ? TY_C10 : it->storage == 3 ? TY_U8 : TY_F32;
			for (int c = 0; c < it->comps && c < 4; c++) {
				uint32_t v = composite_extract1(&g->s, t_float(&g->s), pc, (uint32_t)c);
				if (dst.type == TY_U8) write_raw_comp(g, &dst, 0, c, result1(&g->s, &g->s.func, SpvOpConvertFToU, t_uint(&g->s), fop2(&g->s, SpvOpFAdd, fop2(&g->s, SpvOpFMul, fclamp(&g->s, v, 0, 1), c_float(&g->s, 255.0f)), c_float(&g->s, 0.5f))), ok);
				else if (dst.type == TY_C10) write_raw_comp(g, &dst, 0, c, float_to_lane_norm_spv(&g->s, v, TY_C10), ok);
				else write_comp_f(g, &dst, 0, c, v, ok);
			}
			continue;
		}
		int ncomp = 0, base = -1;
		const int canon = canon_vary && it->semantic >= 0 && it->semantic < USSE_CANON_VARY;   /* (component c: Location semantic's when the draw's vertex program writes it) */
		if (canon) {}
		else if (it->semantic >= 0 && it->semantic <= 9) { ncomp = g->vl->ncomp_tex[it->semantic]; base = g->vl->o_tex[it->semantic]; }
		else if (it->semantic == 10) { ncomp = 4; base = g->vl->o_color0; }
		else if (it->semantic == 11) { ncomp = 4; base = g->vl->o_color1; }
		else if (it->semantic == 12) { ncomp = 2; base = g->vl->o_fog; }
		uint32_t pos = 0;   /* POSITION: the fragment's window position (gfx_soft.c's shade_interp; Vita3K: gl_FragCoord), xy at the 1x pixel (the render scale divided out) */
		if (it->semantic == 13 && g->var_frag_coord) {
			Spv *s = &g->s; uint32_t tf = t_float(s), fc = load(s, t_vec(s, tf, 4), g->var_frag_coord), sc = srgb_mask_comp(g, 3);
			uint32_t kx = result1(s, &s->func, SpvOpConvertUToF, tf, uop2(s, SpvOpBitwiseAnd, sc, c_uint(s, 0xffff))), ky = result1(s, &s->func, SpvOpConvertUToF, tf, uop2(s, SpvOpShiftRightLogical, sc, c_uint(s, 16)));
			uint32_t one = c_float(s, 1.0f);
			kx = glsl2(s, GLSLFMax, kx, one); ky = glsl2(s, GLSLFMax, ky, one);
			uint32_t px = fop2(s, SpvOpFDiv, composite_extract1(s, tf, fc, 0), kx), py = fop2(s, SpvOpFDiv, composite_extract1(s, tf, fc, 1), ky);
			uint32_t w = composite_extract1(s, tf, fc, 3);
			uint32_t id = nid(s); INS(&s->func, SpvOpCompositeConstruct, t_vec(s, tf, 4), id, px, py, composite_extract1(s, tf, fc, 2), w); pos = id;
		}
		/* packed as usse_pack_iter does: F32, F16, C10 (normalized) or U8 (clamped, x255, +0.5 truncated) */
		UOperand dst; memset(&dst, 0, sizeof dst); dst.bank = BK_PA; dst.num = (uint16_t)it->pa;
		dst.type = it->storage == 1 ? TY_F16 : it->storage == 2 ? TY_C10 : it->storage == 3 ? TY_U8 : TY_F32;
		for (int c = 0; c < it->comps && c < 4; c++) {
			uint32_t v = pos ? composite_extract1(&g->s, t_float(&g->s), pos, (uint32_t)c)
			           : canon ? (c < 3 ? canon_in(g, it->semantic, c) : fsel(g, ugt(g, canon_ncomp(g, it->semantic), 3), canon_in(g, it->semantic, 3), c_float(&g->s, 1.0f)))   /* (x, y, z: a lacking component is written 0, interpolated exactly 0; w's default 1 isn't exact through interpolation) */
			           : base >= 0 && c < ncomp ? read_vary_comp(g, base + c, ok) : c_float(&g->s, c == 3 ? 1.0f : 0.0f);
			if (dst.type == TY_U8) write_raw_comp(g, &dst, 0, c, result1(&g->s, &g->s.func, SpvOpConvertFToU, t_uint(&g->s), fop2(&g->s, SpvOpFAdd, fop2(&g->s, SpvOpFMul, fclamp(&g->s, v, 0, 1), c_float(&g->s, 255.0f)), c_float(&g->s, 0.5f))), ok);
			else if (dst.type == TY_C10) write_raw_comp(g, &dst, 0, c, float_to_lane_norm_spv(&g->s, v, TY_C10), ok);
			else write_comp_f(g, &dst, 0, c, v, ok);
		}
	}
}
/* how a FRAGCOLOR_USED program gets the framebuffer's colour (see usse_spirv.h): 1 = sampled from a copy of
 * the colour target the caller binds at set 2, binding nsmp (the default: every backend); 2 = read from an
 * input attachment (OpTypeImage SubpassData, InputAttachmentIndex 0) -- which spirv-cross turns into a
 * [[color(0)]] fragment input on Metal, the tile's own colour at this pixel as the draws before it, and this
 * draw's earlier primitives, left it: the Vita's own programmable-blend semantics, no copy and no pass split */
static int fetch_mode = 1;
void usse_spirv_set_fetch_mode(int mode) { fetch_mode = mode == 2 ? 2 : 1; }
void usse_spirv_sa_read(uint32_t out[8]) { memcpy(out, sa_read, sizeof sa_read); }
static int pcf_unit_next = -1, pcf_taps_next;   /* usse_spirv_set_pcf: the next translation's */
static int fetch_zero_next;   /* usse_spirv_set_fetch_zero: the next translation's (1 zeros, 2 zeros then ones) */
void usse_spirv_set_fetch_zero(int mode) { fetch_zero_next = mode; }
void usse_spirv_set_pcf(int unit, int taps) { pcf_unit_next = unit; pcf_taps_next = taps; }
static int motion_next;
void usse_spirv_set_motion(int on) { motion_next = on; }
static int points_next;   /* usse_spirv_set_points: the next translation's */
static int probe_off_next;   /* usse_spirv_set_probe_off: the next translation leaves RECOMP_PROBE out (RECOMP_PROBE_FPROG names another program) */
void usse_spirv_set_probe_off(int off) { probe_off_next = off != 0; }
static const char *probe_env(void) { return probe_off_next ? 0 : getenv("RECOMP_PROBE"); }
void usse_spirv_set_points(int on) { points_next = on != 0; }
/* a colour (vec4) into the colour register lanes in their declared type (integer types normalized): usse_preload_color */
static void emit_frag_color_preload(Gen *g, uint32_t texel, int *ok) {
	const UsseFragLayout *fl = g->fl; Spv *s = &g->s;
	UOperand o; memset(&o, 0, sizeof o);
	o.bank = (uint8_t)(fl->native_color ? BK_O : BK_PA); o.num = (uint16_t)fl->color_pa_start; o.type = (uint8_t)fl->color_type;
	for (int k = 0; k < 4; k++) o.swiz[k] = (uint8_t)k;
	int n = fl->color_comps > 0 ? fl->color_comps : 4;
	int norm = o.type == TY_U8 || o.type == TY_S8 || o.type == TY_U16 || o.type == TY_S16;
	for (int c = 0; c < n && c < 4 && *ok; c++) {
		uint32_t v = composite_extract1(s, t_float(s), texel, (uint32_t)c);
		if (norm) write_raw_comp(g, &o, 0, c, float_to_lane_norm_spv(s, v, o.type), ok);
		else write_comp_f(g, &o, 0, c, v, ok);
	}
}
static uint32_t c_vec4(Spv *s, float v) { uint32_t id = nid(s), z = c_float(s, v), a[6] = {t_vec(s, t_float(s), 4), id, z, z, z, z}; ins(&s->types, SpvOpConstantComposite, 6, a); return id; }
/* FRAGCOLOR_USED: usse_preload_color in SPIR-V -- texelFetch the framebuffer copy at gl_FragCoord (mode 1) or
 * read the input attachment (mode 2: the coordinate is relative to the fragment, (0,0) = this pixel) and
 * write it into the colour register; modes 3 and 4 write zeros (a program whose framebuffer read the blend
 * unit does instead: what it adds, X -- gfx_gpu.c; mode 4's second run writes ones, see the tap loop) */
static void emit_frag_fetch(Gen *g, int *ok) {
	Spv *s = &g->s;
	if (g->fetch == 3 || g->fetch == 4) { emit_frag_color_preload(g, c_vec4(s, 0.0f), ok); return; }
	uint32_t texel = nid(s);
	if (g->fetch == 2) {
		uint32_t t_iv2 = t_vec(s, t_int(s), 2), zero = c_int(s, 0);
		uint32_t coord = nid(s); { uint32_t a[4] = {t_iv2, coord, zero, zero}; ins(&s->types, SpvOpConstantComposite, 4, a); }
		uint32_t img = load(s, g->t_subpass, g->fetch_var);
		INS(&s->func, SpvOpImageRead, t_vec(s, t_float(s), 4), texel, img, coord);
	} else {
		uint32_t fc = load(s, t_vec(s, t_float(s), 4), g->var_frag_coord);
		uint32_t x = result1(s, &s->func, SpvOpConvertFToS, t_int(s), composite_extract1(s, t_float(s), fc, 0));
		uint32_t y = result1(s, &s->func, SpvOpConvertFToS, t_int(s), composite_extract1(s, t_float(s), fc, 1));
		uint32_t coord = result2(s, &s->func, SpvOpCompositeConstruct, t_vec(s, t_int(s), 2), x, y);
		uint32_t img = result1(s, &s->func, SpvOpImage, g->t_img2d, load(s, g->t_simg, g->fetch_var));
		INS(&s->func, SpvOpImageFetch, t_vec(s, t_float(s), 4), texel, img, coord, 2 /*Lod*/, c_int(s, 0));
	}
	emit_frag_color_preload(g, texel, ok);
}
/* fragment-shader epilogue: usse_read_color -- the colour register's raw lanes in its declared type,
 * integer types normalized (U8 colour = /255), float types and C10 converted plainly */
static void emit_frag_epilogue(Gen *g, int *ok) {
	const UsseFragLayout *fl = g->fl;
	if (!fl) { *ok = 0; return; }
	Spv *s = &g->s;
	UOperand o; memset(&o, 0, sizeof o);
	o.bank = (uint8_t)(fl->native_color ? BK_O : BK_PA); o.num = (uint16_t)fl->color_pa_start; o.type = (uint8_t)fl->color_type;
	for (int c = 0; c < 4; c++) o.swiz[c] = (uint8_t)c;
	int n = fl->color_comps > 0 ? fl->color_comps : 4;
	{	/* RECOMP_PROBE=t|p|o:<word>[@<instr>] (see README, "register probes"): the colour becomes that register
		 * word, encoded exactly as gfx_soft.c's render does it, so a GPU dump and an interpreter dump of
		 * the same draw compare the register rather than the final colour */
		const char *pr = probe_env();
		if (pr && pr[1] == ':') {
			int vmode = pr[0] >= 'A' && pr[0] <= 'Z', bl = pr[0] | 0x20;
			int bank = bl == 't' ? BK_TEMP : bl == 'p' ? BK_PA : BK_O; RegPtr rp = vmode ? (RegPtr){1, 0, 0} : bank_ptr(g, bank, atoi(pr + 2));
			if (rp.ok) {
				uint32_t word = vmode ? result1(s, &s->func, SpvOpBitcast, t_uint(s), composite_extract1(s, t_float(s), load(s, t_vec(s, t_float(s), 4), g->var_vprobe), 0))
				              : g->probe_id ? g->probe_id : load(s, t_uint(s), rp.ptr);
				uint32_t half = c_float(s, 0.5f * (getenv("RECOMP_PROBE_SCALE") ? (float)atof(getenv("RECOMP_PROBE_SCALE")) : 1.0f)), h5 = c_float(s, 0.5f);
				uint32_t lo = half_to_float_spv(s, uop2(s, SpvOpBitwiseAnd, word, c_uint(s, 0xffff))), hi = half_to_float_spv(s, uop2(s, SpvOpShiftRightLogical, word, c_uint(s, 16)));
				if (getenv("RECOMP_PROBE_F32")) { lo = result1(s, &s->func, SpvOpBitcast, t_float(s), word); hi = c_float(s, 0); }
				if (getenv("RECOMP_PROBE_U32")) {   /* the raw word's halves / 65536, no scale */
					uint32_t f16 = c_float(s, 1.0f / 65536), two = c_float(s, 2.0f), one_u = c_float(s, 1.0f);
					lo = fop2(s, SpvOpFMul, result1(s, &s->func, SpvOpConvertUToF, t_float(s), uop2(s, SpvOpBitwiseAnd, word, c_uint(s, 0xffff))), f16);
					hi = fop2(s, SpvOpFMul, result1(s, &s->func, SpvOpConvertUToF, t_float(s), uop2(s, SpvOpShiftRightLogical, word, c_uint(s, 16))), f16);
					lo = fop2(s, SpvOpFSub, fop2(s, SpvOpFMul, lo, two), one_u); hi = fop2(s, SpvOpFSub, fop2(s, SpvOpFMul, hi, two), one_u);   /* undo the x*0.5+0.5 below */
					half = c_float(s, 0.5f);
				}
				uint32_t pv[4] = { fop2(s, SpvOpFAdd, fop2(s, SpvOpFMul, lo, half), h5), fop2(s, SpvOpFAdd, fop2(s, SpvOpFMul, hi, half), h5), h5, c_float(s, 1) };
				uint32_t pid = nid(s); INS(&s->func, SpvOpCompositeConstruct, t_vec(s, t_float(s), 4), pid, pv[0], pv[1], pv[2], pv[3]);
				store(s, g->var_frag_color, pid);   /* straight to the output, whatever the colour register's type (raw-word G-buffers included) */
				return;
			}
		}
	}
	uint32_t comp[4];
	if (o.type == TY_U32 || o.type == TY_S32) {   /* raw words (Unit 13's packed G-buffer): placed so the target holds the bits */
		uint32_t w[4];
		for (int k = 0; k < 4; k++) w[k] = k < n ? read_raw_comp(g, &o, 0, k, ok) : c_uint(s, 0);
		for (int c = 0; c < 4; c++) {
			if (g->color_kind == 2) comp[c] = result1(s, &s->func, SpvOpBitcast, t_float(s), w[c]);                 /* RGBA32F: a word a channel */
			else if (g->color_kind == 1) comp[c] = half_to_float_spv(s, extract_lane_spv(s, w[c / 2], TY_F16, c & 1));   /* RGBA16F: halves */
			else comp[c] = fop2(s, SpvOpFDiv, result1(s, &s->func, SpvOpConvertUToF, t_float(s), extract_lane_spv(s, w[0], TY_U8, c)), c_float(s, 255.0f));   /* RGBA8: bytes */
		}
		if (!*ok) return;
		uint32_t id = nid(s); INS(&s->func, SpvOpCompositeConstruct, t_vec(s, t_float(s), 4), id, comp[0], comp[1], comp[2], comp[3]);
		store(s, g->var_frag_color, id);
		return;
	}
	int norm = o.type == TY_U8 || o.type == TY_S8 || o.type == TY_U16 || o.type == TY_S16;
	for (int c = 0; c < 4; c++) {
		if (c >= n) { comp[c] = c_float(s, c == 3 ? 1.0f : 0.0f); continue; }
		uint32_t raw = read_raw_comp(g, &o, 0, c, ok);
		comp[c] = norm ? lane_to_float_norm_spv(s, raw, o.type) : lane_to_float_spv(s, raw, o.type);
	}
	if (!*ok) return;
	uint32_t id = nid(s); INS(&s->func, SpvOpCompositeConstruct, t_vec(s, t_float(s), 4), id, comp[0], comp[1], comp[2], comp[3]);
	store(s, g->var_frag_color, id);
}
/* vertex-shader epilogue: O[0..3] -> gl_Position, O[4..total_o) -> the varying Outputs, 4 words/Location */
static void emit_vert_epilogue(Gen *g, int *ok) {
	Spv *s = &g->s;
	UOperand o; memset(&o, 0, sizeof o); o.bank = BK_O; o.type = TY_F32; for (int c = 0; c < 4; c++) o.swiz[c] = (uint8_t)c;
	uint32_t pos[4]; for (int c = 0; c < 4; c++) { o.num = (uint16_t)c; pos[c] = read_comp_f(g, &o, 0, 0, ok); }
	if (!*ok) return;
	/* sceGxmSetViewport's z mapping, gfx_soft.c's record_tri: depth = zOffset + zScale * (z/w). The
	 * GPU's own mapping is depth = z/w over the viewport's [0,1] range, so clip z becomes
	 * zOffset*w + zScale*z here (the host pushes the draw's pair after the SA words; 0,1 when no
	 * viewport is set or it is disabled). Unit 13 projects GL-style z in [-w,w] with 0.5/0.5: left
	 * as z/w, every depth test against the preloaded scene depth (its light volumes) disagreed. */
	uint32_t pz = nid(s); INS(&s->func, SpvOpAccessChain, t_ptr(s, SpvStorageUniform, t_vec(s, t_float(s), 4)), pz, g->var_sa, c_uint(s, 1));
	uint32_t vz = load(s, t_vec(s, t_float(s), 4), pz);
	uint32_t zoff = composite_extract1(s, t_float(s), vz, 0), zscale = composite_extract1(s, t_float(s), vz, 1);
	pos[2] = fop2(s, SpvOpFAdd, fop2(s, SpvOpFMul, zoff, pos[3]), fop2(s, SpvOpFMul, zscale, pos[2]));
	if (g->pos_only) {   /* the motion variant's second run: its position, nothing else */
		uint32_t id = nid(s); INS(&s->func, SpvOpCompositeConstruct, t_vec(s, t_float(s), 4), id, pos[0], pos[1], pos[2], pos[3]);
		store(s, g->var_mv_prev, id); return;
	}
	if (g->points) {   /* point sprites (usse_spirv_set_points): this vertex's corner of its point's square */
		uint32_t tu = t_uint(s), tf = t_float(s), tb = t_bool(s);
		uint32_t c6 = uop2(s, SpvOpUMod, g->vindex, c_uint(s, 6));
		#define EQ(k) result2(s, &s->func, SpvOpIEqual, tb, c6, c_uint(s, k))
		uint32_t right = result2(s, &s->func, SpvOpLogicalOr, tb, EQ(1), result2(s, &s->func, SpvOpLogicalOr, tb, EQ(3), EQ(4)));
		uint32_t down = result2(s, &s->func, SpvOpLogicalOr, tb, EQ(2), result2(s, &s->func, SpvOpLogicalOr, tb, EQ(4), EQ(5)));
		#undef EQ
		uint32_t vf[2];
		for (int k = 0; k < 2; k++) { uint32_t pw = nid(s); INS(&s->func, SpvOpAccessChain, g->ptr_sa_u, pw, g->var_sa, c_uint(s, 4), c_uint(s, (uint32_t)k)); vf[k] = result1(s, &s->func, SpvOpBitcast, tf, load(s, tu, pw)); }
		uint32_t size = vf[0];
		if (g->vl && g->vl->o_psize >= 0) { UOperand ps = o; ps.num = (uint16_t)g->vl->o_psize; size = read_comp_f(g, &ps, 0, 0, ok); if (!*ok) return; }
		uint32_t one = c_float(s, 1.0f), mone = c_float(s, -1.0f), zero = c_float(s, 0.0f);
		uint32_t sw = fop2(s, SpvOpFMul, size, pos[3]);
		uint32_t sx = result3(s, &s->func, SpvOpSelect, tf, right, one, mone), sy = result3(s, &s->func, SpvOpSelect, tf, down, one, mone);
		pos[0] = fop2(s, SpvOpFAdd, pos[0], fop2(s, SpvOpFMul, fop2(s, SpvOpFMul, sx, sw), composite_extract1(s, tf, vz, 2)));
		pos[1] = fop2(s, SpvOpFAdd, pos[1], fop2(s, SpvOpFMul, fop2(s, SpvOpFMul, sy, sw), composite_extract1(s, tf, vz, 3)));
		uint32_t u = result3(s, &s->func, SpvOpSelect, tf, right, one, zero), dv = result3(s, &s->func, SpvOpSelect, tf, down, one, zero);
		uint32_t flip = result2(s, &s->func, SpvOpFOrdNotEqual, tb, vf[1], zero);
		uint32_t v = result3(s, &s->func, SpvOpSelect, tf, flip, fop2(s, SpvOpFSub, one, dv), dv);
		uint32_t pv = nid(s); INS(&s->func, SpvOpCompositeConstruct, t_vec(s, tf, 4), pv, u, v, zero, one);
		store(s, g->var_point, pv);
	}
	uint32_t id = nid(s); INS(&s->func, SpvOpCompositeConstruct, t_vec(s, t_float(s), 4), id, pos[0], pos[1], pos[2], pos[3]);
	store(s, g->var_pos, id);
	if (g->var_vprobe) {   /* RECOMP_PROBE (vertex mode): the snapshot's bits as the extra flat varying's .x */
		uint32_t w = g->probe_id ? g->probe_id : c_uint(s, 0), f = result1(s, &s->func, SpvOpBitcast, t_float(s), w), z = c_float(s, 0);
		uint32_t pv = nid(s); INS(&s->func, SpvOpCompositeConstruct, t_vec(s, t_float(s), 4), pv, f, z, z, z);
		store(s, g->var_vprobe, pv);
	}
	for (int i = 0; i < g->nvary; i++) {
		uint32_t v[4];
		if (canon_vary) {   /* semantic i's components from its O words, the ones the program lacks 0 (w 1) */
			int n, base = vary_sem(g->vl, i, &n);
			for (int c = 0; c < 4; c++) { if (c < n) { o.num = (uint16_t)(base + c); v[c] = read_comp_f(g, &o, 0, 0, ok); } else v[c] = c_float(s, c == 3 ? 1.0f : 0.0f); }
		} else {
			for (int c = 0; c < 4; c++) { o.num = (uint16_t)(4 + 4 * i + c); v[c] = read_comp_f(g, &o, 0, 0, ok); }
		}
		if (!*ok) return;
		uint32_t vid = nid(s); INS(&s->func, SpvOpCompositeConstruct, t_vec(s, t_float(s), 4), vid, v[0], v[1], v[2], v[3]);
		store(s, g->var_vary[i], vid);
	}
}

/* ---- GPU-side vertex fetch (usse_spirv.h's UsseVfLayout): the vertex program's prologue builds its PA words from the
 * draw's raw vertex streams, which the host copies as they are into storage buffer 0 (one 4-byte-aligned copy per stream,
 * starting at the draw's lowest index lo), instead of reading a record the CPU packed per vertex. Everything the CPU
 * packer (gfx_soft.c's gfx_vertex_pack) does per vertex happens here per invocation, in the same order and with the same
 * conversions (attr_comp's), so a record read by the program is the same word for word:
 *   rel = gl_VertexIndex - lo; byte b = stream_base*4 + rel*stride + offset + k*size, read as uint words at b>>2 (two words,
 *   shifted and or'd, when a 2- or 4-byte value straddles a word boundary -- all strides and offsets seen so far are
 *   multiples of 4, so that is a compile-time choice: `stride` and `offset` are constants of the module);
 *   F32/UNTYPED the word; U8/S8/U16/S16 the (sign-extended) integer converted; F16 half_to_float_spv (the interpreter's,
 *   bit for bit); U8N/U16N/S8N/S16N exactly x/(2^k-1) as the CPU computes it (vf_norm_u, below) -- no float division
 *   anywhere in this prologue, so Metal's fast-math (division to 2.5 ulp) cannot move a vertex;
 *   F32 slots store the floats' words at pa[reg+c], F16 slots pack truncated halves (float_to_half_spv) in pairs, or'd
 *   into the word as the packer does, untyped slots copy rawlen bytes into words; components the stream doesn't supply
 *   are (0,0,0,1); then pa[index_reg] = gl_VertexIndex and pa[instance_reg] = 0. Every other PA word is the zero the
 *   packer's memset gave -- as a word loaded from the uniform, not a constant (see the function's first lines). */
static uint32_t vf_word(Gen *g, uint32_t wi) { Spv *s = &g->s; return load(s, t_uint(s), access2(s, g->ptr_vin_u, g->var_vin, c_uint(s, 0), wi)); }
/* x/(2^k-1) correctly rounded, for 0 <= x <= 2^k-1 (k 7, 8, 15 or 16), as the bits of a float: the binary expansion of
 * x/(2^k-1) is x's k-bit pattern repeating forever, so the 24-bit mantissa is the 24 bits of that repetition from x's
 * leading 1 on, and the bit after it decides the rounding (never a tie: the tail is never all zeros). Checked on the CPU
 * against (float)x / (2^k-1) for every x of the four widths (host/build/p2_normtest.c at the time): identical bits. */
static uint32_t vf_norm_u(Gen *g, uint32_t x, int k) {
	Spv *s = &g->s; uint32_t tu = t_uint(s), tb = t_bool(s);
	uint32_t N = (1u << k) - 1;
	uint32_t msb = nid(s); INS(&s->func, SpvOpExtInst, tu, msb, s->glsl_ext, 75 /*FindUMsb*/, x);
	uint32_t is0 = result2(s, &s->func, SpvOpIEqual, tb, x, c_uint(s, 0));
	msb = result3(s, &s->func, SpvOpSelect, tu, is0, c_uint(s, 0), msb);   /* (FindUMsb(0) is -1: keep every shift below in range) */
	uint32_t lz = uop2(s, SpvOpISub, c_uint(s, (uint32_t)(k - 1)), msb);
	uint32_t xr = uop2(s, SpvOpBitwiseAnd, uop2(s, SpvOpBitwiseOr, uop2(s, SpvOpShiftLeftLogical, x, lz), uop2(s, SpvOpShiftRightLogical, x, uop2(s, SpvOpISub, c_uint(s, (uint32_t)k), lz))), c_uint(s, N));   /* x rotated left by lz within k bits: the pattern from its leading 1 */
	uint32_t W;   /* the 32 bits of the expansion from the leading 1 on: xr repeated from the top */
	if (k == 8) W = uop2(s, SpvOpIMul, xr, c_uint(s, 0x01010101u));
	else if (k == 16) W = uop2(s, SpvOpIMul, xr, c_uint(s, 0x00010001u));
	else if (k == 7) W = uop2(s, SpvOpBitwiseOr, uop2(s, SpvOpIMul, xr, c_uint(s, (1u << 25) | (1u << 18) | (1u << 11) | (1u << 4))), uop2(s, SpvOpShiftRightLogical, xr, c_uint(s, 3)));
	else W = uop2(s, SpvOpBitwiseOr, uop2(s, SpvOpIMul, xr, c_uint(s, (1u << 17) | (1u << 2))), uop2(s, SpvOpShiftRightLogical, xr, c_uint(s, 13)));   /* k 15 */
	uint32_t m24 = uop2(s, SpvOpShiftRightLogical, W, c_uint(s, 8)), round = uop2(s, SpvOpBitwiseAnd, uop2(s, SpvOpShiftRightLogical, W, c_uint(s, 7)), c_uint(s, 1));
	uint32_t bits = uop2(s, SpvOpIAdd, uop2(s, SpvOpIAdd, uop2(s, SpvOpShiftLeftLogical, uop2(s, SpvOpISub, c_uint(s, 126), lz), c_uint(s, 23)), uop2(s, SpvOpISub, m24, c_uint(s, 0x800000u))), round);
	return result3(s, &s->func, SpvOpSelect, tu, is0, c_uint(s, 0), bits);
}
/* the signed k-bit value x (zero-extended) over 2^(k-1)-1: the magnitude through vf_norm_u, the sign put back; the most
 * negative value is the one constant the formula doesn't cover (attr_comp: S8N clamps it to -1, S16N doesn't) */
static uint32_t vf_norm_s(Gen *g, uint32_t x, int k, uint32_t min_bits) {
	Spv *s = &g->s; uint32_t tu = t_uint(s), tb = t_bool(s);
	uint32_t half = 1u << (k - 1), N = (1u << k) - 1;
	uint32_t neg = result2(s, &s->func, SpvOpUGreaterThanEqual, tb, x, c_uint(s, half));
	uint32_t a = result3(s, &s->func, SpvOpSelect, tu, neg, uop2(s, SpvOpBitwiseAnd, uop2(s, SpvOpISub, c_uint(s, half << 1), x), c_uint(s, N)), x);
	uint32_t amin = nid(s); INS(&s->func, SpvOpExtInst, tu, amin, s->glsl_ext, 38 /*UMin*/, a, c_uint(s, half - 1));   /* (the minimum's magnitude 2^(k-1) stays inside the formula's range; it is replaced below) */
	uint32_t bits = uop2(s, SpvOpBitwiseOr, vf_norm_u(g, amin, k - 1), result3(s, &s->func, SpvOpSelect, tu, neg, c_uint(s, 0x80000000u), c_uint(s, 0)));
	uint32_t ismin = result2(s, &s->func, SpvOpIEqual, tb, x, c_uint(s, half));
	return result3(s, &s->func, SpvOpSelect, tu, ismin, c_uint(s, min_bits), bits);
}
/* half_to_float_spv's value, built with integer ops only: the denormal case normalises the mantissa with FindUMsb
 * (man * 2^-24 = 1.f * 2^(msb-24): exponent field 103 + msb, mantissa man << (23 - msb)) instead of the float multiply
 * half_to_float_spv uses (exact as well, but a float op inside a select chain of the prologue, which Metal's fast-math
 * compiler may fold through; all-integer, the prologue hands the body nothing but opaque words, like the record path's
 * loads). Checked against the C half_to_float for all 65536 halves: identical bits. Zero and -0: the sign; infinity
 * and NaN: sign | 0x7f800000 | man << 13, as half_to_float. */
static uint32_t vf_half_to_float(Gen *g, uint32_t h) {
	Spv *s = &g->s; uint32_t tu = t_uint(s), tb = t_bool(s);
	uint32_t sign = uop2(s, SpvOpShiftLeftLogical, uop2(s, SpvOpBitwiseAnd, h, c_uint(s, 0x8000)), c_uint(s, 16));
	uint32_t exp10 = result3(s, &s->func, SpvOpBitFieldUExtract, tu, h, c_uint(s, 10), c_uint(s, 5));
	uint32_t man = uop2(s, SpvOpBitwiseAnd, h, c_uint(s, 0x3ff));
	uint32_t norm = uop2(s, SpvOpBitwiseOr, sign, uop2(s, SpvOpBitwiseOr, uop2(s, SpvOpShiftLeftLogical, uop2(s, SpvOpIAdd, exp10, c_uint(s, 112)), c_uint(s, 23)), uop2(s, SpvOpShiftLeftLogical, man, c_uint(s, 13))));
	uint32_t msb = nid(s); INS(&s->func, SpvOpExtInst, tu, msb, s->glsl_ext, 75 /*FindUMsb*/, man);
	uint32_t man0 = result2(s, &s->func, SpvOpIEqual, tb, man, c_uint(s, 0));
	msb = result3(s, &s->func, SpvOpSelect, tu, man0, c_uint(s, 0), msb);   /* (FindUMsb(0) is -1: keep the shifts in range) */
	uint32_t sub = uop2(s, SpvOpBitwiseOr, sign, uop2(s, SpvOpBitwiseOr, uop2(s, SpvOpShiftLeftLogical, uop2(s, SpvOpIAdd, msb, c_uint(s, 103)), c_uint(s, 23)),
	                                                   uop2(s, SpvOpBitwiseAnd, uop2(s, SpvOpShiftLeftLogical, man, uop2(s, SpvOpISub, c_uint(s, 23), msb)), c_uint(s, 0x7fffff))));
	sub = result3(s, &s->func, SpvOpSelect, tu, man0, sign, sub);
	uint32_t naninf = uop2(s, SpvOpBitwiseOr, sign, uop2(s, SpvOpBitwiseOr, c_uint(s, 0x7f800000u), uop2(s, SpvOpShiftLeftLogical, man, c_uint(s, 13))));
	uint32_t is_sub = result2(s, &s->func, SpvOpIEqual, tb, exp10, c_uint(s, 0)), is_top = result2(s, &s->func, SpvOpIEqual, tb, exp10, c_uint(s, 31));
	uint32_t bits = result3(s, &s->func, SpvOpSelect, tu, is_sub, sub, result3(s, &s->func, SpvOpSelect, tu, is_top, naninf, norm));
	return result1(s, &s->func, SpvOpBitcast, t_float(s), bits);
}
/* attr_comp: the raw `size`-byte value (zero-extended in a uint) of attribute format fmt as a float */
static uint32_t vf_convert(Gen *g, uint32_t raw, int fmt) {
	Spv *s = &g->s; uint32_t tf = t_float(s), tu = t_uint(s), ti = t_int(s);
	switch (fmt) {
	case 9: case 10: return result1(s, &s->func, SpvOpBitcast, tf, raw);                              /* F32, UNTYPED: the bits */
	case 0: case 2: return result1(s, &s->func, SpvOpConvertUToF, tf, raw);                           /* U8, U16 */
	case 1: case 3: return result1(s, &s->func, SpvOpConvertSToF, tf, result1(s, &s->func, SpvOpBitcast, ti, result3(s, &s->func, SpvOpBitFieldSExtract, tu, raw, c_uint(s, 0), c_uint(s, fmt == 1 ? 8 : 16))));   /* S8, S16 */
	case 4: return result1(s, &s->func, SpvOpBitcast, tf, vf_norm_u(g, raw, 8));                      /* U8N */
	case 6: return result1(s, &s->func, SpvOpBitcast, tf, vf_norm_u(g, raw, 16));                     /* U16N */
	case 5: { float m = -1.0f; uint32_t mb; memcpy(&mb, &m, 4); return result1(s, &s->func, SpvOpBitcast, tf, vf_norm_s(g, raw, 8, mb)); }   /* S8N: -128/127 clamped to -1 */
	case 7: { float m = (float)(int16_t)-32768 / 32767.0f; uint32_t mb; memcpy(&mb, &m, 4); return result1(s, &s->func, SpvOpBitcast, tf, vf_norm_s(g, raw, 16, mb)); }   /* S16N: -32768/32767, as the CPU rounds it */
	case 8: return vf_half_to_float(g, raw);                                                            /* F16 (integer-only, above) */
	default: return c_float(s, -999.0f);
	}
}
static int vf_size(int fmt) { switch (fmt) { case 9: case 10: return 4; case 6: case 3: case 7: case 2: case 8: return 2; default: return 1; } }   /* gfx_soft.c's attr_byte_size */
/* the `size`-byte value at byte offset `off` of this vertex's record in stream `st` (words cached per record offset, so the
 * four U8N bytes of a colour, or two F16 lanes, cost one load) */
typedef struct { uint32_t base_w[16], base_b[16]; int stride[16]; uint32_t cache_w[64], cache_id[64]; int cache_s[64]; int ncache; } VfCtx;
static uint32_t vf_word_at(Gen *g, VfCtx *c, int st, uint32_t woff) {   /* word `woff` of the record (known alignment: stride a multiple of 4) */
	for (int i = 0; i < c->ncache; i++) if (c->cache_s[i] == st && c->cache_w[i] == woff) return c->cache_id[i];
	uint32_t id = vf_word(g, uop2(&g->s, SpvOpIAdd, c->base_w[st], c_uint(&g->s, woff)));
	if (c->ncache < 64) { c->cache_s[c->ncache] = st; c->cache_w[c->ncache] = woff; c->cache_id[c->ncache++] = id; }
	return id;
}
static uint32_t vf_fetch(Gen *g, VfCtx *c, int st, uint32_t off, int size) {
	Spv *s = &g->s; uint32_t tu = t_uint(s), tb = t_bool(s);
	uint32_t mask = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;
	if ((c->stride[st] & 3) == 0) {   /* the alignment is a constant of the module */
		uint32_t w = off >> 2, al = (off & 3) * 8;
		uint32_t v = vf_word_at(g, c, st, w);
		if (al) v = uop2(s, SpvOpShiftRightLogical, v, c_uint(s, al));
		if (al + 8 * (uint32_t)size > 32) v = uop2(s, SpvOpBitwiseOr, v, uop2(s, SpvOpShiftLeftLogical, vf_word_at(g, c, st, w + 1), c_uint(s, 32 - al)));   /* straddles the next word */
		return size == 4 ? v : uop2(s, SpvOpBitwiseAnd, v, c_uint(s, mask));
	}
	uint32_t b = uop2(s, SpvOpIAdd, c->base_b[st], c_uint(s, off));   /* the byte address in the buffer, alignment at run time */
	uint32_t w = uop2(s, SpvOpShiftRightLogical, b, c_uint(s, 2)), sh = uop2(s, SpvOpShiftLeftLogical, uop2(s, SpvOpBitwiseAnd, b, c_uint(s, 3)), c_uint(s, 3));
	uint32_t v = uop2(s, SpvOpShiftRightLogical, vf_word(g, w), sh);
	if (size > 1) {   /* the next word's low bytes when the value straddles: (w1 << (32 - sh)), nothing at sh 0 (a shift by 32 is undefined) */
		uint32_t w1 = vf_word(g, uop2(s, SpvOpIAdd, w, c_uint(s, 1)));
		uint32_t hi = uop2(s, SpvOpShiftLeftLogical, w1, uop2(s, SpvOpISub, c_uint(s, 32), sh));
		uint32_t sh0 = result2(s, &s->func, SpvOpIEqual, tb, sh, c_uint(s, 0));
		v = uop2(s, SpvOpBitwiseOr, v, result3(s, &s->func, SpvOpSelect, tu, sh0, c_uint(s, 0), hi));
	}
	return size == 4 ? v : uop2(s, SpvOpBitwiseAnd, v, c_uint(s, mask));
}
static void emit_vertex_fetch(Gen *g, const UsseVfLayout *vf, int npa, int *ok) {
	Spv *s = &g->s; uint32_t tu = t_uint(s);
	VfCtx c; memset(&c, 0, sizeof c);
	/* the draw's lowest index (UsseDrawUniforms.vfetch.x, member 4 of the SA block) and each stream's ring word offset
	 * (UsseDrawUniforms.stream[s].x, member 5): base_w = word offset + rel*stride/4 for a stride that is a multiple of
	 * 4, else base_b = the byte address, for vf_fetch */
	uint32_t plo = nid(s); INS(&s->func, SpvOpAccessChain, g->ptr_sa_u, plo, g->var_sa, c_uint(s, 4), c_uint(s, 0));
	uint32_t rel = uop2(s, SpvOpISub, g->vindex, load(s, tu, plo));
	/* an opaque zero and one (UsseDrawUniforms.vfetch.z/.w: the host writes 0 and 1.0f there for every fetched draw). The
	 * record path loads all npa words of the record from the storage buffer, so the compiler knows none of them; written
	 * as constants here (the (0,0,0,1) components a stream doesn't supply, the instance register, the words no attribute
	 * writes) they would let Metal's fast-math fold the program's arithmetic differently from the packer's variant.
	 * Loaded from the uniform they are the same unknown words the record path had. (Integration, 2026-10-09: this took
	 * a few pixels off the fetch path's residual difference from the packer's frames, see the README's
	 * RECOMP_GPU_CPU_VFETCH note; the rest of that residual is not in the words this prologue builds.) */
	uint32_t pz = nid(s); INS(&s->func, SpvOpAccessChain, g->ptr_sa_u, pz, g->var_sa, c_uint(s, 4), c_uint(s, 2)); uint32_t zero_w = load(s, tu, pz);
	uint32_t po = nid(s); INS(&s->func, SpvOpAccessChain, g->ptr_sa_u, po, g->var_sa, c_uint(s, 4), c_uint(s, 3)); uint32_t one_w = load(s, tu, po);
	/* and per vertex as far as any compiler can tell (the vertex index AND the opaque 0): loaded as they are, they were the same
	 * for the whole draw, and NVIDIA compiled a fragment stage that reads such a varying for that -- a different stage from the
	 * one it builds with any other vertex shader, so the fragment stages predicted pipelines had built were never reused
	 * (Killzone frame_12587: 0 of 200 pipelines faster; with the CPU packer's records, which come from a buffer, 1.9 s vs 14.1 s) */
	zero_w = uop2(s, SpvOpBitwiseAnd, g->vindex, zero_w); one_w = uop2(s, SpvOpBitwiseOr, zero_w, one_w);
	for (int w = 0; w < npa; w++) store(s, access1(s, g->ptr_priv_u, g->var_pa, c_uint(s, (uint32_t)w)), zero_w);   /* every record word the packer's memset gave, as an unknown */
	for (int i = 0; i < vf->n; i++) {
		int st = vf->a[i].stream & 15;
		if (c.base_w[st] || c.base_b[st]) continue;
		c.stride[st] = vf->a[i].stride;
		uint32_t pb = nid(s); INS(&s->func, SpvOpAccessChain, g->ptr_sa_u, pb, g->var_sa, c_uint(s, 5), c_uint(s, (uint32_t)st), c_uint(s, 0));
		uint32_t base = load(s, tu, pb);
		if ((c.stride[st] & 3) == 0) c.base_w[st] = c.stride[st] ? uop2(s, SpvOpIAdd, base, uop2(s, SpvOpIMul, rel, c_uint(s, (uint32_t)c.stride[st] / 4))) : base;   /* (stride 0: every vertex reads the one record) */
		else c.base_b[st] = uop2(s, SpvOpIAdd, uop2(s, SpvOpShiftLeftLogical, base, c_uint(s, 2)), uop2(s, SpvOpIMul, rel, c_uint(s, (uint32_t)c.stride[st])));
	}
	for (int i = 0; i < vf->n && *ok; i++) {
		const UsseVfAttr *a = &vf->a[i]; int st = a->stream & 15, size = vf_size(a->fmt);
		if (a->kind == 1) {   /* untyped slot: rawlen bytes copied into words, the tail of the last word zero as the packer's memset left it */
			if (a->reg + (a->rawlen + 3) / 4 > npa) { snprintf(g->why, sizeof g->why, "vertex fetch: raw attribute outside the record"); *ok = 0; return; }
			for (int w = 0; w < (a->rawlen + 3) / 4; w++) {
				uint32_t v = vf_fetch(g, &c, st, a->offset + 4u * (uint32_t)w, 4);
				int tail = a->rawlen - 4 * w; if (tail < 4) v = uop2(s, SpvOpBitwiseAnd, v, c_uint(s, (1u << (8 * tail)) - 1));
				store(s, access1(s, g->ptr_priv_u, g->var_pa, c_uint(s, (uint32_t)(a->reg + w))), v);
			}
			continue;
		}
		if (a->kind != 2 && a->kind != 3) { snprintf(g->why, sizeof g->why, "vertex fetch: attribute kind %d", a->kind); *ok = 0; return; }
		int nw = a->kind == 2 ? a->dcomps : (a->dcomps + 1) / 2;
		if (a->reg + nw > npa || a->dcomps == 0 || a->dcomps > 4 || a->comps > 4) { snprintf(g->why, sizeof g->why, "vertex fetch: attribute outside the record"); *ok = 0; return; }
		uint32_t comps[4], raw[4];
		for (int k = 0; k < 4; k++) {
			raw[k] = k < a->comps ? vf_fetch(g, &c, st, a->offset + (uint32_t)(k * size), size) : 0;
			comps[k] = k < a->comps ? vf_convert(g, raw[k], a->fmt) : result1(s, &s->func, SpvOpBitcast, t_float(s), k == 3 ? one_w : zero_w);   /* (0,0,0,1), opaque (above) */
		}
		if (a->kind == 2) { for (int k = 0; k < a->dcomps; k++) store(s, access1(s, g->ptr_priv_u, g->var_pa, c_uint(s, (uint32_t)(a->reg + k))), result1(s, &s->func, SpvOpBitcast, tu, comps[k])); }
		else for (int k = 0; k < a->dcomps; k += 2) {   /* F16 lanes, or'd into the word like the packer's |= (the array starts zeroed) */
			uint32_t h[2];
			for (int j = 0; j < 2 && k + j < a->dcomps; j++) {
				if (a->fmt == 8 && k + j < a->comps) {   /* an F16 attribute into an F16 slot: float_to_half(half_to_float(h)) is h itself for every zero, denormal, normal and infinite half, and
					                                      * a quiet NaN with the sign for a NaN -- the round trip's one change, selected here instead of the ~40 ops of the two conversions */
					uint32_t isnan = result2(s, &s->func, SpvOpLogicalAnd, t_bool(s), result2(s, &s->func, SpvOpIEqual, t_bool(s), uop2(s, SpvOpBitwiseAnd, raw[k + j], c_uint(s, 0x7C00)), c_uint(s, 0x7C00)),
					                                                                   result2(s, &s->func, SpvOpINotEqual, t_bool(s), uop2(s, SpvOpBitwiseAnd, raw[k + j], c_uint(s, 0x3FF)), c_uint(s, 0)));
					h[j] = result3(s, &s->func, SpvOpSelect, tu, isnan, uop2(s, SpvOpBitwiseOr, uop2(s, SpvOpBitwiseAnd, raw[k + j], c_uint(s, 0x8000)), c_uint(s, 0x7E00)), raw[k + j]);
				} else h[j] = float_to_half_spv(s, comps[k + j]);
			}
			uint32_t w = h[0];
			if (k + 1 < a->dcomps) w = uop2(s, SpvOpBitwiseOr, w, uop2(s, SpvOpShiftLeftLogical, h[1], c_uint(s, 16)));
			uint32_t p = access1(s, g->ptr_priv_u, g->var_pa, c_uint(s, (uint32_t)(a->reg + k / 2)));
			store(s, p, uop2(s, SpvOpBitwiseOr, load(s, tu, p), w));
		}
	}
	if (vf->index_reg >= 0 && vf->index_reg < npa) store(s, access1(s, g->ptr_priv_u, g->var_pa, c_uint(s, (uint32_t)vf->index_reg)), g->vindex);
	if (vf->instance_reg >= 0 && vf->instance_reg < npa) store(s, access1(s, g->ptr_priv_u, g->var_pa, c_uint(s, (uint32_t)vf->instance_reg)), zero_w);
}

SpirvModule usse_spirv_translate(const UsseProgram *prog, const UsseVertexLayout *vl, const UsseFragLayout *fl,
                                  const uint32_t sa_tex_unit[256], int max_smp, int *smp_units_out, int *nsmp_out, int color_kind, uint32_t cube_mask, uint32_t snap_mask,
                                  const UsseVfLayout *vf) {
	SpirvModule m; memset(&m, 0, sizeof m);
	Gen g; memset(&g, 0, sizeof g);
	memset(sa_read, 0, sizeof sa_read);
	g.color_kind = color_kind;
	Spv *s = &g.s;
	g.is_fragment = usse_is_fragment(prog);
	g.cube_mask = cube_mask;
	g.snap_mask = g.is_fragment ? snap_mask : 0;
	g.vl = vl; g.fl = fl;
	/* usse.c's run_program initialises inc[s][r]=r (RM_SLMSI's default, identity stepping) before
	 * every run, unconditionally -- SMLSI/SMBO only override it from wherever they appear in program
	 * order. Leaving this at all-zero (this pass's own default-initialised state) meant any RM_SLMSI
	 * instruction BEFORE the first SMLSI in a program (or in a program with no SMLSI at all, which
	 * is common: plenty of straight-line vertex shaders use RM_SLMSI for a repeated VMOV/VDP with
	 * only the hardware default) read a stuck offset of 0 on every repeat iteration instead of
	 * stepping -- confirmed live: Shovel Knight's logo vertex shader's 2-rep VMOV (RM_SLMSI, no
	 * SMLSI anywhere in the program) wrote the same O-words twice instead of two different ones,
	 * leaving the back half of its "colour" varying at its zero-initialised default. */
	for (int sl = 0; sl < 4; sl++) for (int r = 0; r <= 16; r++) g.inc[sl][r] = (int8_t)r;
	int n = 0; const UOp *code = usse_program_code(prog, 0, &n);
	s->next_id = 1;
	s->glsl_ext = nid(s);   /* allocated up front: glsl1/glsl2 are called during the instruction walk below */

	/* pre-scan (before any code is emitted: everything declared here may be referenced by the
	 * function body, and SPIR-V ids must be defined before use) */
	int uses_depth = 0; int ok = 1;
	for (int i = 0; i < n; i++) {
		if (code[i].op == OP_DEPTHF) uses_depth = 1;
		if (code[i].op == OP_SMP) {
			int unit = resolve_smp_unit(&code[i], sa_tex_unit);
			if (unit >= 0 && sampler_slot(&g, unit) < 0 && g.nsmp < 16) g.smp_units[g.nsmp++] = unit;
		}
		if (code[i].op == OP_LDST && !code[i].sub2) {   /* a load: which SA word its address comes from (a fragment program's: a render target, emit_ldst_target) */
			int sa = ldst_pointer_sa(code, i), k;
			if (sa >= 0) { for (k = 0; k < g.nwin && g.win_sa[k] != sa; k++); if (k == g.nwin && g.nwin < 8) g.win_sa[g.nwin++] = sa; }
		}
	}
	if (g.is_fragment && fl) for (int i = 0; i < fl->niters; i++) if (fl->iters[i].is_tex) {
		int unit = fl->iters[i].tex_unit;
		if (unit >= 0 && unit < 16 && sampler_slot(&g, unit) < 0 && g.nsmp < 16) g.smp_units[g.nsmp++] = unit;
	}

	/* register file: Private word arrays, zero-initialised (OpVariable's Initializer) */
	uint32_t t_u = t_uint(s);
	g.ptr_priv_u = t_ptr(s, SpvStoragePrivate, t_u);
	uint32_t arr160 = t_array(s, t_u, c_uint(s, 160), 160), arr16 = t_array(s, t_u, c_uint(s, 16), 16);
	uint32_t ptr_arr160 = t_ptr(s, SpvStoragePrivate, arr160), ptr_arr16 = t_ptr(s, SpvStoragePrivate, arr16);
	g.var_r = global_var(s, ptr_arr160, SpvStoragePrivate, const_null_array(s, arr160));
	g.var_pa = global_var(s, ptr_arr160, SpvStoragePrivate, const_null_array(s, arr160));
	g.var_o = global_var(s, ptr_arr160, SpvStoragePrivate, const_null_array(s, arr160));
	g.var_i = global_var(s, ptr_arr16, SpvStoragePrivate, const_null_array(s, arr16));
	{	uint32_t arr2 = t_array(s, t_u, c_uint(s, 2), 2);   /* idx1/idx2: zero at the start of every instance, like UsseInst.idx */
		g.var_idx = global_var(s, t_ptr(s, SpvStoragePrivate, arr2), SpvStoragePrivate, const_null_array(s, arr2));
	}
	/* predicates p0..p3 (VTST/VTSTMSK write these; KILL and instruction-level (pN)/(!pN)
	 * predication read them -- only KILL does today, see emit_kill) */
	{
		uint32_t arr4b = t_array(s, t_bool(s), c_uint(s, 4), 4);
		uint32_t false4 = nid(s); INS(&s->types, SpvOpConstantFalse, t_bool(s), false4);
		uint32_t init4 = nid(s); { uint32_t a[6] = {arr4b, init4, false4, false4, false4, false4}; ins(&s->types, SpvOpConstantComposite, 6, a); }
		g.var_p = global_var(s, t_ptr(s, SpvStoragePrivate, arr4b), SpvStoragePrivate, init4);
	}

	/* SA: a 256-word uniform buffer (Block-decorated struct wrapping the array), set=(frag?3:1) binding=0.
	 * Declared as uvec4[64] -- the same 1024 bytes -- because a Uniform-class array needs a 16-byte
	 * stride under Vulkan's standard layout rules (uint[256] only validates with the optional
	 * uniformBufferStandardLayout feature; Metal never minded, Vulkan drivers may) */
	uint32_t arr64 = t_array(s, t_vec(s, t_u, 4), c_uint(s, 64), 64);
	INS(&s->decos, SpvOpDecorate, arr64, 6 /*ArrayStride*/, 16);
	/* member 1: the draw's viewport z mapping (zOffset, zScale, 0, 0) at byte 1024 -- the vertex
	 * epilogue applies it to clip z (see emit_vert_epilogue); the host pushes UsseDrawUniforms */
	uint32_t arr8 = t_array(s, t_vec(s, t_u, 4), c_uint(s, 8), 8);   /* member 2: the LDST windows, UsseDrawUniforms.win */
	INS(&s->decos, SpvOpDecorate, arr8, 6 /*ArrayStride*/, 16);
	/* member 3: the sRGB masks (UsseDrawUniforms.tex_srgb, srgb_fixup) at byte 1168 */
	/* members 4 and 5: the GPU-side vertex fetch's lowest index (UsseDrawUniforms.vfetch) at 1184 and the 16 vertex
	 * streams' ring word offsets (UsseDrawUniforms.stream) at 1200 -- see emit_vertex_fetch */
	uint32_t arr16v = t_array(s, t_vec(s, t_u, 4), c_uint(s, 16), 16);
	INS(&s->decos, SpvOpDecorate, arr16v, 6 /*ArrayStride*/, 16);
	/* member 6: the units' LOD biases (UsseDrawUniforms.lod_bias, sample_lod) at 1456, as float bits */
	uint32_t arr4v = t_array(s, t_vec(s, t_u, 4), c_uint(s, 4), 4);
	INS(&s->decos, SpvOpDecorate, arr4v, 6 /*ArrayStride*/, 16);
	uint32_t sa_struct = nid(s); { uint32_t sm[8] = { sa_struct, arr64, t_vec(s, t_float(s), 4), arr8, t_vec(s, t_u, 4), t_vec(s, t_u, 4), arr16v, arr4v }; ins(&s->types, 30 /*OpTypeStruct*/, 8, sm); }
	INS(&s->decos, SpvOpDecorate, sa_struct, SpvDecorationBlock);
	INS(&s->decos, SpvOpMemberDecorate, sa_struct, 0, SpvDecorationOffset, 0);
	INS(&s->decos, SpvOpMemberDecorate, sa_struct, 1, SpvDecorationOffset, 1024);
	INS(&s->decos, SpvOpMemberDecorate, sa_struct, 2, SpvDecorationOffset, 1040);
	INS(&s->decos, SpvOpMemberDecorate, sa_struct, 3, SpvDecorationOffset, 1168);
	INS(&s->decos, SpvOpMemberDecorate, sa_struct, 4, SpvDecorationOffset, 1184);
	INS(&s->decos, SpvOpMemberDecorate, sa_struct, 5, SpvDecorationOffset, 1200);
	INS(&s->decos, SpvOpMemberDecorate, sa_struct, 6, SpvDecorationOffset, 1456);
	g.var_sa = global_var(s, t_ptr(s, SpvStorageUniform, sa_struct), SpvStorageUniform, 0);
	INS(&s->decos, SpvOpDecorate, g.var_sa, SpvDecorationDescriptorSet, (uint32_t)(g.is_fragment ? 3 : 1));
	INS(&s->decos, SpvOpDecorate, g.var_sa, SpvDecorationBinding, 0);
	g.ptr_sa_u = t_ptr(s, SpvStorageUniform, t_u);
	if (!g.is_fragment && motion_next) {   /* the motion variant: the same draw's SA block of the previous frame (usse_spirv_set_motion) */
		g.motion = 1;
		g.var_sa_prev = global_var(s, t_ptr(s, SpvStorageUniform, sa_struct), SpvStorageUniform, 0);
		INS(&s->decos, SpvOpDecorate, g.var_sa_prev, SpvDecorationDescriptorSet, 1);
		INS(&s->decos, SpvOpDecorate, g.var_sa_prev, SpvDecorationBinding, 1);
	}

	/* vertex input (vertex stage only): one StorageBuffer of uint words holding, per draw, either the raw vertex
	 * streams the prologue fetches attributes from itself (vf: emit_vertex_fetch) or one PA record per vertex that
	 * gfx_gpu.c packed exactly like fetch_interp's CPU path does (usse_pack_attr), indexed by gl_VertexIndex */
	uint32_t var_vidx = 0;
	if (!g.is_fragment) {
		/* its textures (SMP in a vertex program: Killzone's lens flares read their visibility so): set 0, bindings
		 * 0..nsmp-1, the storage buffers after them -- SDL_GPU's vertex-stage order (sampled textures, storage
		 * textures, storage buffers); spirv_cross_to_msl pins the same Metal indices */
		if (g.nsmp) declare_samplers(&g, 0);
		uint32_t rtarr = nid(s); INS(&s->types, 29 /*OpTypeRuntimeArray*/, rtarr, t_u);
		INS(&s->decos, SpvOpDecorate, rtarr, 6 /*ArrayStride*/, 4);
		uint32_t vstruct = nid(s); INS(&s->types, 30, vstruct, rtarr);
		INS(&s->decos, SpvOpDecorate, vstruct, SpvDecorationBlock);
		INS(&s->decos, SpvOpMemberDecorate, vstruct, 0, SpvDecorationOffset, 0);
		g.var_vin = global_var(s, t_ptr(s, 12 /*StorageBuffer*/, vstruct), 12, 0);
		INS(&s->decos, SpvOpDecorate, g.var_vin, SpvDecorationDescriptorSet, 0);
		INS(&s->decos, SpvOpDecorate, g.var_vin, SpvDecorationBinding, (uint32_t)g.nsmp);
		g.ptr_vin_u = t_ptr(s, 12, t_u);
		if (g.nwin) {   /* the LDST windows: the same runtime-array-of-words block, the binding after the PA input's */
			g.var_win = global_var(s, t_ptr(s, 12 /*StorageBuffer*/, vstruct), 12, 0);
			INS(&s->decos, SpvOpDecorate, g.var_win, SpvDecorationDescriptorSet, 0);
			INS(&s->decos, SpvOpDecorate, g.var_win, SpvDecorationBinding, (uint32_t)g.nsmp + 1);
		}
		uint32_t ptr_in_int = t_ptr(s, SpvStorageInput, t_int(s));
		var_vidx = global_var(s, ptr_in_int, SpvStorageInput, 0);
		INS(&s->decos, SpvOpDecorate, var_vidx, SpvDecorationBuiltIn, SpvBuiltInVertexIndex);
		g.vindex = result1(s, &s->func, SpvOpBitcast, t_u, load(s, t_int(s), var_vidx));
		/* copy this vertex's 160 attribute words from the input SSBO into the Private PA array once,
		 * up front -- the program reads/writes PA like any other register bank from here on (see
		 * bank_ptr's BK_PA case); writing straight into the (shared, per-draw) SSBO through it was a
		 * real bug here, caught by SDL_GPU's Metal backend refusing the resulting pipeline */
		uint32_t npa = (uint32_t)usse_program_pa_words(prog);   /* the record is only as wide as the program reads: 160 words a vertex was 640 bytes uploaded per vertex */
		m.pa_words = (int)npa;
		if (vf) emit_vertex_fetch(&g, vf, (int)npa, &ok);   /* the raw streams, converted here (the module is specialised on the layout) */
		else for (uint32_t w = 0; w < npa; w++) {
			uint32_t word = uop2(s, SpvOpIAdd, uop2(s, SpvOpIMul, g.vindex, c_uint(s, npa)), c_uint(s, w));
			uint32_t v = load(s, t_u, access2(s, g.ptr_vin_u, g.var_vin, c_uint(s, 0), word));
			store(s, access1(s, g.ptr_priv_u, g.var_pa, c_uint(s, w)), v);
		}
	}

	/* varyings: one vec4 per 4-word group of O[4..total_o), Location = group index; vertex writes
	 * Output, fragment reads Input (word order/count agreed via the shared vl, see usse_spirv.h) */
	int total_o = vl ? vl->total_o : 4; if (total_o < 4) total_o = 4;
	int nlocs = (total_o - 4 + 3) / 4; if (nlocs > 16) nlocs = 16;
	uint32_t t_v4 = t_vec(s, t_float(s), 4);
	uint32_t want = ~0u;   /* canonical: a vertex module writes every semantic, a fragment module reads the ones its iterators name */
	if (canon_vary) {
		nlocs = USSE_CANON_VARY;
		if (g.is_fragment) { want = 0; if (fl) for (int i = 0; i < fl->niters; i++) { const UsseIter *it = &fl->iters[i]; int k = it->is_tex ? it->tex_coord_src : it->semantic; if (k >= 0 && k < USSE_CANON_VARY) want |= 1u << k; } }
	}
	g.nvary = nlocs;
	for (int i = 0; i < nlocs; i++) {
		if (!(want >> i & 1)) continue;
		uint32_t storage = g.is_fragment ? SpvStorageInput : SpvStorageOutput;
		g.var_vary[i] = global_var(s, t_ptr(s, storage, t_v4), storage, 0);
		INS(&s->decos, SpvOpDecorate, g.var_vary[i], SpvDecorationLocation, (uint32_t)i);
	}
	{	const char *pr = probe_env();
		if (pr && pr[0] >= 'A' && pr[0] <= 'Z' && pr[1] == ':') {   /* vertex-mode probe: one more varying, flat (the word's bits must survive interpolation) */
			uint32_t storage = g.is_fragment ? SpvStorageInput : SpvStorageOutput;
			g.var_vprobe = global_var(s, t_ptr(s, storage, t_v4), storage, 0);
			INS(&s->decos, SpvOpDecorate, g.var_vprobe, SpvDecorationLocation, (uint32_t)nlocs);
			INS(&s->decos, SpvOpDecorate, g.var_vprobe, SpvDecorationFlat);
		}
	}
	g.points = points_next;
	if (g.points) {   /* the sprite coordinate: the location after the varyings (and the probe's) */
		uint32_t storage = g.is_fragment ? SpvStorageInput : SpvStorageOutput;
		g.var_point = global_var(s, t_ptr(s, storage, t_v4), storage, 0);
		INS(&s->decos, SpvOpDecorate, g.var_point, SpvDecorationLocation, (uint32_t)(nlocs + (g.var_vprobe ? 1 : 0)));
	}
	if (!g.is_fragment) {
		g.var_pos = global_var(s, t_ptr(s, SpvStorageOutput, t_v4), SpvStorageOutput, 0);
		INS(&s->decos, SpvOpDecorate, g.var_pos, SpvDecorationBuiltIn, SpvBuiltInPosition);
		if (g.motion) {   /* the two clip positions (usse_spirv_set_motion) */
			g.var_mv_cur = global_var(s, t_ptr(s, SpvStorageOutput, t_v4), SpvStorageOutput, 0);
			INS(&s->decos, SpvOpDecorate, g.var_mv_cur, SpvDecorationLocation, 30);
			g.var_mv_prev = global_var(s, t_ptr(s, SpvStorageOutput, t_v4), SpvStorageOutput, 0);
			INS(&s->decos, SpvOpDecorate, g.var_mv_prev, SpvDecorationLocation, 31);
		}
	}

	/* fragment: output colour (+ depth, if DEPTHF is used) and the sampled-image bindings the
	 * pre-scan found (set=2, binding = first-use order, matching the num_samplers the caller must
	 * pass to SDL_CreateGPUShader in that same order -- see usse_spirv.h) */
	uint32_t entry_fn = nid(s);   /* allocated early: OpExecutionMode needs it and may be emitted before OpFunction */
	if (g.is_fragment) {
		g.var_frag_color = global_var(s, t_ptr(s, SpvStorageOutput, t_v4), SpvStorageOutput, 0);
		INS(&s->decos, SpvOpDecorate, g.var_frag_color, SpvDecorationLocation, 0);
		if (uses_depth) {
			g.var_frag_depth = global_var(s, t_ptr(s, SpvStorageOutput, t_float(s)), SpvStorageOutput, 0);
			INS(&s->decos, SpvOpDecorate, g.var_frag_depth, SpvDecorationBuiltIn, SpvBuiltInFragDepth);
			INS(&s->execmodes, SpvOpExecutionMode, entry_fn, 12 /*DepthReplacing (11, as it was, is Xfb: spirv-val rejected the module)*/);
		}
		/* FRAGCOLOR_USED (usse_preload_color): the colour register starts as the framebuffer's colour
		 * at this pixel -- fetched from a copy of the colour target the caller binds after the real
		 * samplers (mode 1), or read from an input attachment, Metal's framebuffer fetch (mode 2, see
		 * usse_spirv_set_fetch_mode); raw-word (U32/S32) colour programs are left to the software path */
		g.fetch = fl && fl->frag_color_used && fl->color_type != TY_U32 && fl->color_type != TY_S32 ? (fetch_zero_next == 2 ? 4 : fetch_zero_next ? 3 : fetch_mode) : 0;
		if (g.nwin && g.fetch == 2) { snprintf(g.why, sizeof g.why, "fragment LDST in a framebuffer-fetch program"); ok = 0; }   /* (the input attachment's binding slot sits where the LDST targets would start) */
		if (g.nsmp || g.fetch || g.nwin) {
			uint32_t ptr_simg = declare_samplers(&g, 2);
			for (int w = 0; w < g.nwin; w++) {   /* the LDST windows' render targets: after the program's samplers and the framebuffer copy (gfx_gpu.c binds them in that order) */
				g.ldst_img[w] = global_var(s, ptr_simg, SpvStorageUniformConstant, 0);
				INS(&s->decos, SpvOpDecorate, g.ldst_img[w], SpvDecorationDescriptorSet, 2);
				INS(&s->decos, SpvOpDecorate, g.ldst_img[w], SpvDecorationBinding, (uint32_t)(g.nsmp + (g.fetch == 1) + w));
			}
			if (g.fetch == 1) {
				g.fetch_var = global_var(s, ptr_simg, SpvStorageUniformConstant, 0);
				INS(&s->decos, SpvOpDecorate, g.fetch_var, SpvDecorationDescriptorSet, 2);
				INS(&s->decos, SpvOpDecorate, g.fetch_var, SpvDecorationBinding, (uint32_t)g.nsmp);
				g.var_frag_coord = global_var(s, t_ptr(s, SpvStorageInput, t_v4), SpvStorageInput, 0);
				INS(&s->decos, SpvOpDecorate, g.var_frag_coord, SpvDecorationBuiltIn, SpvBuiltInFragCoord);
			} else if (g.fetch == 2) {   /* the input attachment: Sampled=2 (read, not sampled), Unknown format (allowed for SubpassData); set/binding kept for validity, no resource is bound */
				INS(&s->caps, SpvOpCapability, 40 /*InputAttachment*/);
				g.t_subpass = nid(s); INS(&s->types, SpvOpTypeImage, g.t_subpass, t_float(s), 6 /*SubpassData*/, 0, 0, 0, 2 /*read*/, 0 /*Unknown format*/);
				g.fetch_var = global_var(s, t_ptr(s, SpvStorageUniformConstant, g.t_subpass), SpvStorageUniformConstant, 0);
				INS(&s->decos, SpvOpDecorate, g.fetch_var, SpvDecorationInputAttachmentIndex, 0);
				INS(&s->decos, SpvOpDecorate, g.fetch_var, SpvDecorationDescriptorSet, 2);
				INS(&s->decos, SpvOpDecorate, g.fetch_var, SpvDecorationBinding, (uint32_t)g.nsmp);
			}
			if (g.snap_mask && !g.var_frag_coord) {   /* snap_fetch: this fragment's place in its block */
				g.var_frag_coord = global_var(s, t_ptr(s, SpvStorageInput, t_v4), SpvStorageInput, 0);
				INS(&s->decos, SpvOpDecorate, g.var_frag_coord, SpvDecorationBuiltIn, SpvBuiltInFragCoord);
			}
		}
		if (fl && !g.var_frag_coord) for (int i = 0; i < fl->niters; i++) if (!fl->iters[i].is_tex && !fl->iters[i].point_coord && fl->iters[i].semantic == 13) {   /* a POSITION iterator (emit_frag_prologue) */
			g.var_frag_coord = global_var(s, t_ptr(s, SpvStorageInput, t_v4), SpvStorageInput, 0);
			INS(&s->decos, SpvOpDecorate, g.var_frag_coord, SpvDecorationBuiltIn, SpvBuiltInFragCoord);
			break;
		}
		emit_frag_prologue(&g, sa_tex_unit, &ok);
		if (ok && g.fetch) emit_frag_fetch(&g, &ok);
	}

	/* walk the program. Predicated forward branches ("(!p0) BR +k": skip the next k-1 instructions
	 * unless p0) become a structured if around the skipped range; ranges nest, each closed when the
	 * walk reaches its target. Backward branches, unconditional ones and link-saving calls aren't
	 * translated (usse.c runs them; none of the three games' programs seen so far use them). */
	/* filtered shadows (usse_spirv_set_pcf): the walk and the epilogue below run once per tap, each from the
	 * register file as the prologue left it and with the shadow map's SMPs moved by its grid step, and the
	 * colours they write are averaged. The game's own compare and everything after it run per tap, so the
	 * mean is exactly the share of the neighbourhood that passes: percentage-closer filtering. (Uncharted's
	 * resolve takes one sample a pixel: its sun dapples every leaf and blade of grass with hard texel-sized
	 * steps.) */
	int taps = 1; g.pcf_unit = -1;
	if (ok && g.is_fragment && pcf_taps_next > 1 && pcf_unit_next >= 0 && !g.var_frag_depth && !g.fetch) {
		int plain = 1;
		for (int i = 0; i < n; i++) if (code[i].op == OP_BR || code[i].op == OP_KILL || code[i].op == OP_DEPTHF) plain = 0;
		/* a program that gathers its shadow map WITH the bilinear weights (sb_mode 3: gather4 + weights, then
		 * a per-lane compare and a weighted sum -- Unit 13's lights) filters it already, as the hardware's
		 * PCF: 16 taps over that changed no pixel of its frames and cost 16 times its lighting passes.
		 * Uncharted's resolve gathers too, but without the weights (four hard taps): that one stays filtered */
		for (int i = 0; i < n; i++) if (code[i].op == OP_SMP && code[i].args[0] == 3 && resolve_smp_unit(&code[i], sa_tex_unit) == pcf_unit_next) plain = 0;
		if (plain) { taps = pcf_taps_next >= 16 ? 16 : pcf_taps_next >= 9 ? 9 : 4; g.pcf_unit = pcf_unit_next; }
	}
	/* fetch mode 4: the program twice, the colour register 0 then 1 -- out(0) = X, out(1) = K + X -- and the
	 * module outputs (X.rgb, 1 - K) for gfx_gpu.c's ONE / ONE_MINUS_SRC_ALPHA blend (usse_fragment_fb_blend) */
	int lerp = ok && g.is_fragment && g.fetch == 4; uint32_t lerp_col[2] = {0, 0};
	if (lerp) taps = 2;
	if (g.motion) taps = 2;   /* the motion variant: the program again from the prologue's registers, with the previous frame's SA */
	const uint32_t var_sa_now = g.var_sa;
	uint32_t pcf_vars[6] = {g.var_r, g.var_pa, g.var_o, g.var_i, g.var_idx, g.var_p}, pcf_types[6], pcf_saved[6], pcf_step[2] = {0, 0}, pcf_acc = 0;
	int8_t pcf_inc0[4][17]; memcpy(pcf_inc0, g.inc, sizeof pcf_inc0);
	if (taps > 1) {
		pcf_types[0] = pcf_types[1] = pcf_types[2] = t_array(s, t_u, c_uint(s, 160), 160); pcf_types[3] = t_array(s, t_u, c_uint(s, 16), 16);
		pcf_types[4] = t_array(s, t_u, c_uint(s, 2), 2); pcf_types[5] = t_array(s, t_bool(s), c_uint(s, 4), 4);
		for (int k = 0; k < 6; k++) pcf_saved[k] = load(s, pcf_types[k], pcf_vars[k]);
		for (int k = 0; k < 2 && !lerp && g.is_fragment; k++) {   /* the grid step in UV, UsseDrawUniforms.vfetch[0..1] */
			uint32_t pw = nid(s); INS(&s->func, SpvOpAccessChain, g.ptr_sa_u, pw, g.var_sa, c_uint(s, 4), c_uint(s, (uint32_t)k));
			pcf_step[k] = result1(s, &s->func, SpvOpBitcast, t_float(s), load(s, t_u, pw));
		}
	}
	for (int tap = 0; tap < taps && ok; tap++) {
		if (taps > 1) {
			int side = taps == 16 ? 4 : taps == 9 ? 3 : 2;
			if (tap) { for (int k = 0; k < 6; k++) store(s, pcf_vars[k], pcf_saved[k]); memcpy(g.inc, pcf_inc0, sizeof pcf_inc0); }
			if (g.motion) { g.var_sa = tap ? g.var_sa_prev : var_sa_now; g.pos_only = tap; }
			else if (lerp) { if (tap) emit_frag_color_preload(&g, c_vec4(s, 1.0f), &ok); }
			else {
				g.pcf_du = fop2(s, SpvOpFMul, pcf_step[0], c_float(s, (float)(tap % side) - (float)(side - 1) * 0.5f));
				g.pcf_dv = fop2(s, SpvOpFMul, pcf_step[1], c_float(s, (float)(tap / side) - (float)(side - 1) * 0.5f));
			}
		}
		struct { int end; uint32_t merge; } brstack[8]; int nbr = 0;
		for (int i = 0; i < n && ok; i++) {
			const UOp *op = &code[i];
			while (nbr && brstack[nbr - 1].end <= i) { INS(&s->func, SpvOpBranch, brstack[nbr - 1].merge); INS(&s->func, SpvOpLabel, brstack[nbr - 1].merge); nbr--; }
			if (op->op == OP_BR) {
				int target = i + (int)op->imm;
				if (op->pred_kind == 0 || op->imm <= 0 || op->args[0] || target > n || (nbr && target > brstack[nbr - 1].end) || nbr >= 8) {
					snprintf(m.err, sizeof m.err, "branch at instruction %d (%s, offset %d) outside the structured forms translated", i, op->pred_kind ? "predicated" : "unconditional", (int)op->imm); ok = 0; break;
				}
				uint32_t p = load(s, t_bool(s), pred_ptr(&g, op->pred_reg));
				uint32_t taken = op->pred_kind == 1 ? p : result1(s, &s->func, SpvOpLogicalNot, t_bool(s), p);
				uint32_t run = result1(s, &s->func, SpvOpLogicalNot, t_bool(s), taken);   /* the skipped range runs when the branch is NOT taken */
				uint32_t then = nid(s), merge = nid(s);
				INS(&s->func, SpvOpSelectionMerge, merge, 0 /*None*/);
				INS(&s->func, SpvOpBranchConditional, run, then, merge);
				INS(&s->func, SpvOpLabel, then);
				brstack[nbr].end = target; brstack[nbr].merge = merge; nbr++;
				continue;
			}
			/* (pN) / (!pN) predication: the instruction runs inside a structured if on the predicate
			 * register (usse.c's pred_true). Every emit_* writes through the Private register
			 * variables, so nothing defined inside the then-block is read after the merge. KILL and BR
			 * carry their own condition logic and aren't wrapped (KILL's imm encodes it; BR isn't
			 * translated). */
			uint32_t pred_merge = 0;
			if (op->pred_kind != 0 && op->op != OP_KILL && op->op != OP_BR && op->op != OP_NOP) {
				uint32_t p = load(s, t_bool(s), pred_ptr(&g, op->pred_reg));
				uint32_t cond = op->pred_kind == 1 ? p : result1(s, &s->func, SpvOpLogicalNot, t_bool(s), p);
				uint32_t then = nid(s); pred_merge = nid(s);
				INS(&s->func, SpvOpSelectionMerge, pred_merge, 0 /*None*/);
				INS(&s->func, SpvOpBranchConditional, cond, then, pred_merge);
				INS(&s->func, SpvOpLabel, then);
			} else if (op->pred_kind != 0 && op->op != OP_NOP) { snprintf(m.err, sizeof m.err, "predicated %s at instruction %d", op->op == OP_KILL ? "KILL" : "branch", i); ok = 0; break; }
			switch (op->op) {
			case OP_NOP: break;
			case OP_SMLSI:   /* ports usse.c's run_program SMLSI handling, at translate time (see slot_off) */
				for (int sl = 0; sl < 4; sl++) {
					int v = op->args[sl], inc = (int8_t)(v & 0xFF), mode2 = (v >> 8) & 1;
					if (!mode2) for (int r = 0; r <= 16; r++) g.inc[sl][r] = (int8_t)(r * inc);
					else for (int r = 0; r < 4; r++) g.inc[sl][r] = (int8_t)((inc >> (2 * r)) & 3);
				}
				break;
			case OP_SMBO: for (int sl = 0; sl < 4; sl++) for (int r = 0; r <= 16; r++) g.inc[sl][r] = (int8_t)(r + op->args[sl]); break;
			case OP_VMAD2: emit_vmad2(&g, op, &ok); break;
			case OP_VNMAD: emit_vnmad(&g, op, &ok); break;
			case OP_VMAD: emit_vmad(&g, op, &ok); break;
			case OP_VDP: emit_vdp(&g, op, &ok); break;
			case OP_VDUAL: emit_vdual(&g, op, &ok); break;
			case OP_VCOMP: emit_vcomp(&g, op, &ok); break;
			case OP_VMOV: emit_vmov(&g, op, &ok); break;
			case OP_VPCK: emit_vpck(&g, op, &ok); break;
			case OP_VTST: emit_vtst(&g, op, &ok); break;
			case OP_VTSTMSK: emit_vtstmsk(&g, op, &ok); break;
			case OP_VBW: emit_vbw(&g, op, &ok); break;
			case OP_I8MAD: emit_i8mad(&g, op, &ok); break;
			case OP_I16MAD: emit_i16mad(&g, op, &ok); break;
			case OP_I32MAD: emit_i32mad(&g, op, &ok); break;
			case OP_I32MAD2: emit_i32mad2(&g, op, &ok); break;
			case OP_SMP: emit_smp(&g, op, sa_tex_unit, &ok); break;   /* (a vertex program's: its own units, explicit level 0) */
			case OP_KILL: if (!g.is_fragment) ok = 0; else emit_kill(&g, op, &ok); break;
			case OP_DEPTHF: emit_depthf(&g, op, &ok); break;
			case OP_LIMM: emit_limm(&g, op, &ok); break;
			case OP_LDST: if (g.is_fragment) emit_ldst_target(&g, op, code, i, &ok); else emit_ldst(&g, op, code, i, &ok); break;
			default: ok = 0; break;
			}
			if (pred_merge) { INS(&s->func, SpvOpBranch, pred_merge); INS(&s->func, SpvOpLabel, pred_merge); }
			if (!ok) snprintf(m.err, sizeof m.err, "unsupported at primary instruction %d (op %d)%s%s", i, op->op, g.why[0] ? ": " : "", g.why);
			{	/* RECOMP_PROBE=t|p|o:<word>@<instr>: the register word as of this instruction, for emit_frag_epilogue */
				const char *pr = probe_env(); const char *at = pr ? strchr(pr, '@') : 0;
				int vmode = pr && pr[0] >= 'A' && pr[0] <= 'Z', bl = pr ? (pr[0] | 0x20) : 0;
				if (ok && at && atoi(at + 1) == i && vmode == !g.is_fragment) { RegPtr rp = bank_ptr(&g, bl == 't' ? BK_TEMP : bl == 'p' ? BK_PA : BK_O, atoi(pr + 2)); if (rp.ok) g.probe_id = load(&g.s, t_uint(&g.s), rp.ptr); }
			}
		}
		while (ok && nbr) { INS(&s->func, SpvOpBranch, brstack[nbr - 1].merge); INS(&s->func, SpvOpLabel, brstack[nbr - 1].merge); nbr--; }   /* ranges ending at the program's end */
		if (ok) { if (g.is_fragment) emit_frag_epilogue(&g, &ok); else emit_vert_epilogue(&g, &ok); }
		if (g.motion && ok && !tap) store(s, g.var_mv_cur, load(s, t_v4, g.var_pos));   /* (this frame's position, as gl_Position got it) */
		if (taps > 1 && ok && g.is_fragment) { uint32_t col = load(s, t_v4, g.var_frag_color); if (lerp) lerp_col[tap] = col; else pcf_acc = pcf_acc ? result2(s, &s->func, SpvOpFAdd, t_v4, pcf_acc, col) : col; }
	}
	if (lerp && ok) {   /* K: red, green and blue's mean (the test made them one K); alpha 1 - K */
		uint32_t x[3], ksum = 0, tf = t_float(s);
		for (int c = 0; c < 3; c++) {
			x[c] = composite_extract1(s, tf, lerp_col[0], (uint32_t)c);
			uint32_t kc = fop2(s, SpvOpFSub, composite_extract1(s, tf, lerp_col[1], (uint32_t)c), x[c]);
			ksum = ksum ? fop2(s, SpvOpFAdd, ksum, kc) : kc;
		}
		uint32_t a = fop2(s, SpvOpFSub, c_float(s, 1.0f), fop2(s, SpvOpFMul, ksum, c_float(s, 1.0f / 3.0f)));
		uint32_t id = nid(s); INS(&s->func, SpvOpCompositeConstruct, t_v4, id, x[0], x[1], x[2], a);
		store(s, g.var_frag_color, id);
	} else if (taps > 1 && ok && g.is_fragment) store(s, g.var_frag_color, result2(s, &s->func, 142 /*OpVectorTimesScalar*/, t_v4, pcf_acc, c_float(s, 1.0f / (float)taps)));
	g.var_sa = var_sa_now; g.pos_only = 0;
	if (!ok && !m.err[0]) snprintf(m.err, sizeof m.err, "I/O setup failed: %s", g.why[0] ? g.why : "layout/colour type outside this pass's coverage");

	if (max_smp >= 0 && smp_units_out) for (int i = 0; i < g.nsmp && i < max_smp; i++) smp_units_out[i] = g.smp_units[i];
	if (nsmp_out) *nsmp_out = g.nsmp;
	m.nldst_sa = g.nwin; memcpy(m.ldst_sa, g.win_sa, sizeof m.ldst_sa); m.motion = g.motion;
	if (!ok) { free(s->caps.w); free(s->ext.w); free(s->memmodel.w); free(s->entry.w); free(s->execmodes.w); free(s->decos.w); free(s->types.w); free(s->func.w); m.ok = 0; return m; }

	uint32_t iface[2 + 16 + 7]; int niface = 0;
	if (g.var_vprobe) iface[niface++] = g.var_vprobe;
	if (g.var_point) iface[niface++] = g.var_point;
	if (g.var_vin) iface[niface++] = var_vidx;
	if (g.var_pos) iface[niface++] = g.var_pos;
	if (g.var_mv_cur) { iface[niface++] = g.var_mv_cur; iface[niface++] = g.var_mv_prev; }
	for (int i = 0; i < nlocs; i++) if (g.var_vary[i]) iface[niface++] = g.var_vary[i];
	if (g.var_frag_color) iface[niface++] = g.var_frag_color;
	if (g.var_frag_depth) iface[niface++] = g.var_frag_depth;
	if (g.var_frag_coord) iface[niface++] = g.var_frag_coord;
	m.frag_fetch = g.fetch;
	module_header(s, g.is_fragment, entry_fn, iface, niface);
	finish_module(&m, s, entry_fn);
	free(s->caps.w); free(s->ext.w); free(s->memmodel.w); free(s->entry.w); free(s->execmodes.w); free(s->decos.w); free(s->types.w); free(s->func.w);
	return m;
}

SpirvModule usse_spirv_motion_fragment(void) {   /* (cur.xy/cur.w - prev.xy/prev.w) / 2: the pixel's move since the previous frame, in UV units */
	SpirvModule m; memset(&m, 0, sizeof m);
	Spv s; memset(&s, 0, sizeof s);
	s.next_id = 1;
	s.glsl_ext = nid(&s);
	uint32_t entry_fn = nid(&s);
	uint32_t tf = t_float(&s), t_v4 = t_vec(&s, tf, 4);
	uint32_t in_c = global_var(&s, t_ptr(&s, SpvStorageInput, t_v4), SpvStorageInput, 0), in_p = global_var(&s, t_ptr(&s, SpvStorageInput, t_v4), SpvStorageInput, 0);
	INS(&s.decos, SpvOpDecorate, in_c, SpvDecorationLocation, 30); INS(&s.decos, SpvOpDecorate, in_p, SpvDecorationLocation, 31);
	uint32_t out = global_var(&s, t_ptr(&s, SpvStorageOutput, t_v4), SpvStorageOutput, 0);
	INS(&s.decos, SpvOpDecorate, out, SpvDecorationLocation, 0);
	uint32_t c = load(&s, t_v4, in_c), pr = load(&s, t_v4, in_p), d[2];
	uint32_t cw = composite_extract1(&s, tf, c, 3), pw = composite_extract1(&s, tf, pr, 3);
	for (int k = 0; k < 2; k++) {
		uint32_t a1 = fop2(&s, SpvOpFDiv, composite_extract1(&s, tf, c, (uint32_t)k), cw), b1 = fop2(&s, SpvOpFDiv, composite_extract1(&s, tf, pr, (uint32_t)k), pw);
		d[k] = fop2(&s, SpvOpFMul, fop2(&s, SpvOpFSub, a1, b1), c_float(&s, 0.5f));
	}
	uint32_t zero = c_float(&s, 0.0f), v = nid(&s);
	INS(&s.func, SpvOpCompositeConstruct, t_v4, v, d[0], d[1], zero, zero);
	INS(&s.func, SpvOpStore, out, v);
	uint32_t iface[3] = { in_c, in_p, out };
	module_header(&s, 1, entry_fn, iface, 3);
	finish_module(&m, &s, entry_fn);
	free(s.caps.w); free(s.ext.w); free(s.memmodel.w); free(s.entry.w); free(s.execmodes.w); free(s.decos.w); free(s.types.w); free(s.func.w);
	return m;
}
/* the empty fragment module (see usse_spirv.h): the same header and function shell as a translated
 * program, with an empty interface -- OriginUpperLeft, "main", OpReturn and nothing in between */
SpirvModule usse_spirv_null_fragment(void) {
	SpirvModule m; memset(&m, 0, sizeof m);
	Spv s; memset(&s, 0, sizeof s);
	s.next_id = 1;
	s.glsl_ext = nid(&s);   /* (module_header imports GLSL.std.450 for it, as for every module; unused here) */
	uint32_t entry_fn = nid(&s);
	module_header(&s, 1, entry_fn, 0, 0);
	finish_module(&m, &s, entry_fn);
	free(s.caps.w); free(s.ext.w); free(s.memmodel.w); free(s.entry.w); free(s.execmodes.w); free(s.decos.w); free(s.types.w); free(s.func.w);
	return m;
}
