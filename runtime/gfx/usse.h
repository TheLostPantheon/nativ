/*
 * USSE (the Vita GPU's shader core) decoder and CPU interpreter. See docs/usse-isa.md for the format
 * this implements (our own write-up; Vita3K was consulted only as a reference, nothing copied).
 *
 * A GXP program is decoded once (usse_get, keyed by its guest address) into a flat array of
 * micro-ops; usse_run_secondary/usse_run_primary then execute that array many times (once per
 * draw for the secondary program, once per vertex/fragment instance for the primary one) with no
 * further parsing or allocation. Register files are plain arrays; FPINTERNAL doubles as the
 * float/int bit-punning union so no casts are needed at the call sites.
 */
#ifndef RECOMP_USSE_H
#define RECOMP_USSE_H
#include <stdint.h>

typedef union { uint32_t u; float f; int32_t i; } UWord;

/* per vertex/fragment instance: private register files, reset by the caller before each one */
typedef struct {
	/* Sized to the hardware's own documented maximums (§2.1: TEMP 80+margin, PA/SA 128, O 80+margin)
	 * plus room for repeat-offset overrun, not the much larger figures used while this interpreter
	 * was first being brought up: usse_run_primary/secondary memset this struct once per vertex or
	 * fragment instance, so its size is directly a per-instance cost (profiled: ~90% of a frame's
	 * budget on busy 3D scenes goes to everything around the micro-op loop, not the loop itself). */
	UWord r[160];      /* TEMP (also serves INDEXED1/2 bases) */
	UWord pa[160];      /* PRIMATTR: inputs, also a result bank for the fragment colour */
	UWord o[160];       /* OUTPUT: vertex outputs, fragment native colour */
	UWord i[16];         /* FPINTERNAL: register k = words i[4k..4k+3] */
	uint8_t p[4];         /* predicates p0..p3 */
	uint32_t idx[2];      /* index registers idx1, idx2 */
	int killed;            /* KILL executed */
	int depth_written; float depth;   /* DEPTHF */
	uint32_t thread_buf;    /* guest scratch for container 18 (thread buffer), one per draw, reused per instance */
	uint32_t probe;          /* RECOMP_PROBE ...@k: the register word after primary instruction k (usse.c's run_program; gfx_soft.c shows it) */
} UsseInst;

/* shared per draw: SA registers, filled once by usse_setup_registers before the secondary program,
 * then read-only for every vertex/fragment instance */
typedef struct {
	UWord sa[256];
	/* a dependent sampler's texture unit by the SA word its state starts at, +1 (0: none), from the
	 * program's dependent-sampler table: SMP finds its unit here, and the SA words themselves stay 0
	 * (Vita3K's layout: it binds the sampler by that offset and writes nothing there). Writing the
	 * unit number into the SA word (as until 2026-10-15) let a program's own arithmetic read it:
	 * Uncharted's splash-refraction quads (fragment 0x1f1945c8) test that word with a VMOVC to pick
	 * their blend weight -- 0 over 0, 1 over our "10" -- and every quad copied the pre-water scene over
	 * the river up to its borders */
	uint8_t sa_unit[256];
} UsseDraw;

/* resolved from the vertex GXP's attribute parameters + varyings block (usse_vertex_layout) */
#define USSE_MAX_ATTRS 32
typedef struct {
	int reg;                 /* PA register (resource_index) of element 0 */
	uint8_t type, comps;      /* declared parameter type/component count (0 if untyped: raw copy) */
	uint8_t words;             /* words per array element (type/comps packing), for range matching */
	int array_size;
	int untyped;
} UsseAttrSlot;
typedef struct {
	UsseAttrSlot attrs[USSE_MAX_ATTRS]; int nattrs;
	int index_reg, instance_reg;         /* -1 if unused; PA registers for vertex/instance index */
	/* output layout, in the fixed §4.1 order: O register ranges, -1 if the output is absent */
	int o_color0, o_color1, o_fog, o_psize;
	int o_tex[10], ncomp_tex[10];     /* TEXCOORD0..9: O register and component count, -1 if absent */
	int total_o;                       /* highest O word used + 1 (position .. last varying) */
} UsseVertexLayout;

/* resolved from the fragment GXP's varyings/iterator block (usse_fragment_layout) */
#define USSE_MAX_ITERS 16
typedef struct {
	int is_tex;                 /* 0 = interpolated varying, 1 = non-dependent texture read */
	/* varying: */
	int semantic;                /* 0..9 TEXCOORDn, 10 COLOR0, 11 COLOR1, 12 FOG, 13 POSITION */
	int storage;                  /* 0 F32, 1 F16, 2 C10/"fixed", 3 U8 */
	int comps;
	/* texture read: */
	int tex_coord_src, tex_unit, tex_proj, result_fmt;
	int pa;                         /* destination PA register */
	int point_coord;                /* the value (varying) or the coordinate (texture read) is the point-sprite
	                                 * coordinate -- 0..1 across a SCE_GXM_PRIMITIVE_POINTS sprite (attribute_info
	                                 * bit 30 / bit 10, docs/usse-isa.md 1.12) -- not a vertex output */
} UsseIter;
typedef struct {
	UsseIter iters[USSE_MAX_ITERS]; int niters;
	int native_color;                /* program_flags NATIVECOLOR_USED: colour in O, not PA */
	int frag_color_used;               /* FRAGCOLOR_USED: preload O0 with the framebuffer colour */
	int color_pa_start;                 /* PA/O register the colour starts at */
	int color_type, color_comps;         /* output_param_type/output_comp_count */
	int discard_used;
} UsseFragLayout;

typedef struct UsseProgram UsseProgram;   /* opaque: decoded micro-ops + parsed layout, cached by address */

/* decode (or fetch from cache) the program at this guest address; never returns null. */
const UsseProgram *usse_get(uint32_t prog_addr);   /* decoded once per distinct program content; valid until the bytes at that address change */
uint32_t usse_program_hash(const UsseProgram *p);   /* of its GXP bytes: a cache key that survives the shader patcher's ring */
uint32_t usse_content_hash(uint32_t prog_addr, uint32_t *size);   /* usse_program_hash of the GXP at this address, without decoding it (gfx_gpu.c's shader recipes: the programs the game registers) */
uint32_t usse_gxp_hash(const void *gxp, uint32_t *size);   /* the same of a GXP's bytes in host memory (its size from its header) */
int usse_gxp_sane(const void *gxp, uint32_t size);   /* its code's counts and offsets inside its size: a program decode_program can walk (gxp_index.c's scan also finds GXP headers in other data -- Killzone: 387 of 12860, counts in the millions) */
UsseProgram *usse_decode_uncached(uint32_t prog_addr);   /* a decode outside usse_get's tables (a program in scratch memory, about to be replaced) */
void usse_program_free(UsseProgram *p);
int usse_primary_reads_memory(const UsseProgram *p);   /* its primary program has an LDST: what it computes depends on memory its registers don't hold */
int usse_buffer_words(const UsseProgram *p, int c);   /* the words of uniform buffer c it copies into SA (0: reached only through LDST pointers) */
int usse_program_pa_words(const UsseProgram *p);    /* vertex: PA words per vertex record (a multiple of 4, at most 160) */
uint32_t usse_float_to_lane(float v, int type);      /* a float as the lane bits of a register type (UsseAttrSlot.type) */
int usse_is_fragment(const UsseProgram *p);
int usse_supported(const UsseProgram *p);                 /* false: hit an unimplemented instruction; caller should fall back */
const UsseVertexLayout *usse_vertex_layout(const UsseProgram *p);     /* only valid if !is_fragment */
const UsseFragLayout *usse_fragment_layout(const UsseProgram *p);     /* only valid if is_fragment */
/* find the attribute declaration (type/comps/untyped) covering PA register reg (array-aware) */
const UsseAttrSlot *usse_find_attr(const UsseVertexLayout *vl, int reg);

/* per-draw register setup (§1.9): uniform buffer windows, literals, buffer-address table,
 * dependent samplers. ubuf[0..13] are the game's user uniform buffers (0 if unbound); def is the
 * default uniform buffer (container 14); tex_units maps each dependent sampler to a 0..15 texture
 * unit index (our own simplified stand-in for the real 4-word hardware texture descriptor: SMP
 * reads this back as the unit to sample, see usse.c). */
void usse_setup_registers(const UsseProgram *p, UsseDraw *d, uint32_t def_uniform_buf, const uint32_t ubuf[14]);
/* a game's own adjustment of a draw's default uniforms (container 14, at SA base14.. base14+size14-1),
 * called by usse_setup_registers once they are loaded -- on both backends, and without touching the
 * guest's buffer (packs/PCSA00029/hooks.c: Uncharted's fade-out distance under the view-distance setting). NULL: none. */
typedef void (*UsseUniformFilter)(const UsseProgram *p, uint32_t prog_addr, UsseDraw *d, int base14, int size14);
void usse_set_uniform_filter(UsseUniformFilter f);
/* a uniform parameter's resource index (its first word in its container) by name, its container in
 * *container; -1 if the program has none */
int usse_param_resource(uint32_t prog, const char *name, int *container);

/* texture sampling callback into the backend (it already knows texel layouts/formats):
 * unit 0..15, (u,v,w) coordinates (w used for cube faces / 3-D), lod_bias, result in out[4]. */
typedef void (*UsseTexFn)(void *ctx, int unit, float u, float v, float w, float lod, float out[4]);

/* run the secondary ("uniform setup") program once per draw: writes into d->sa. inst is scratch
 * (secondary programs have no PA input and their TEMP/O/etc. references alias SA, per §1.10). */
void usse_run_secondary(const UsseProgram *p, UsseInst *inst, UsseDraw *d);
void usse_set_trace(int on);   /* RECOMP_USSE_TRACE for one draw (RECOMP_TRACE_DRAW=k in gfx_soft.c) */

/* run the primary program for one vertex/fragment instance. inst->pa must already hold the
 * fetched attributes (vertex) or iterator results (fragment); inst->o/r/i/p/idx should be zeroed
 * by the caller first. */
void usse_run_primary(const UsseProgram *p, UsseInst *inst, UsseDraw *d, void *texctx, UsseTexFn texfn);
/* a texel in the texture's native packed format -- what the texture unit hands a shader that asked
 * for no conversion (SMP fconv_type 0/1, an iterator with result format 0; docs/usse-isa.md §SMP): a
 * U8U8U8U8 texel is one word of four bytes, an X8U24 depth texel its 32-bit word, F16F16F16F16 two
 * words. Returns the word count (0: the renderer can't, the float path is used). The shader unpacks. */
typedef int (*UsseTexRawFn)(void *ctx, int unit, float u, float v, float w, float lod, uint32_t raw[4]);
void usse_set_texfn_raw(UsseTexRawFn fn);
/* SMP's gather modes (sb_mode 1 and 3; docs/usse-isa.md §SMP): the 2x2 footprint bilinear filtering would
 * blend, unfiltered -- texels[i][k] is texel i's component k (as the float path decodes it, swizzle
 * and gamma applied), i in gather order (x0,y1) (x1,y1) (x1,y0) (x0,y0) with x0 = floor(u*w - 0.5) --
 * and frac[] the bilinear fractions (u*w - 0.5 - x0, likewise v). Returns the texture's component
 * count, 0 when the renderer can't (the filtered sample is used). Level 0. */
typedef int (*UsseTexGatherFn)(void *ctx, int unit, float u, float v, float texels[4][4], float frac[2]);
void usse_set_texfn_gather(UsseTexGatherFn fn);
void usse_pack_iter_raw(UsseInst *inst, const UsseIter *it, const uint32_t raw[4], int n);
/* FRAGCOLOR_USED: the framebuffer's current colour at the pixel, preloaded where the program's
 * colour output lives before it runs (programmable blending: the program reads and rewrites it) */
void usse_preload_color(UsseInst *inst, const UsseFragLayout *fl, const float in[4]);
/* what a FRAGCOLOR_USED program does with the colour it reads, when the blend unit can do it instead (colour out =
 * K * colour in + X; see usse.c): gfx_gpu.c runs such a program without the framebuffer and blends. Tested on the
 * interpreter, cached. */
enum { USSE_FB_NONE, USSE_FB_ADD, USSE_FB_ADD_REPLACE, USSE_FB_LERP_KEEP, USSE_FB_LERP_OVER };
int usse_fragment_fb_blend(const UsseProgram *p);

/* vertex attribute -> PA: convert one attribute's raw memory value (already read into comps[]
 * floats by the caller, per its own SceGxmVertexAttribute format) into the parameter's declared
 * PA layout. raw/rawlen non-null for untyped attributes: copy bytes verbatim instead. */
void usse_pack_attr(UsseInst *inst, const UsseAttrSlot *slot, const float comps[4], const void *raw, int rawlen);

/* fragment iterator -> PA: pack one interpolated varying value into PA per its storage type. */
void usse_pack_iter(UsseInst *inst, const UsseIter *it, const float comps[4]);

/* fragment colour output: read color_comps components of color_type starting at color_pa_start
 * (PA, or O if native_color) into out[4] (RGBA, 0..1 range already). */
void usse_read_color(const UsseInst *inst, const UsseFragLayout *fl, float out[4]);

/* disassemble one decoded program to a FILE* (usse_dis.c); returns the number of instructions
 * whose instruction word matched no known encoding at all (should be 0: everything in §3 decodes
 * to *something*, even if unsupported). */
struct FILE_tag; /* avoid pulling <stdio.h> into every includer */
int usse_disassemble(const UsseProgram *p, void *out_FILE);
/* count instructions and how many are OP_UNSUPPORTED (both secondary+primary); returns the count. */
int usse_count_unsupported(const UsseProgram *p, int *total_out);
/* does the primary program write depth (DEPTHF) / discard fragments (KILL)? A fragment program that
 * does neither can be left out of a draw that writes no colour (gfx_gpu.c's colour-masked draws). */
int usse_program_uses_depthf(const UsseProgram *p);
int usse_program_uses_kill(const UsseProgram *p);

/* for usse_dis.c and unit tests: parse just the header (no decode), to print raw facts. */
void usse_debug_header(uint32_t prog_addr, void *out_FILE);
#endif
