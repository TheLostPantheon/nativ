/*
 * RECOMP_GFX_COVERAGE=<file.json>: a map of what a game's graphics actually ask for against what
 * each backend honours. Instead of finding gaps one wrong-looking frame at a time, a run tallies:
 *   api       every sceGxm/sceDisplay import called, with counts and whether it is implemented,
 *             a no-op stub, tracked but not passed to the backends, or missing; for the last three
 *             the distinct argument words it was called with (r0-r3, s0-s5 -- the hard-float ABI
 *             passes float arguments there -- and three stack words)
 *   features  each draw-state value seen (primitive, blend function/factors/mask, cull, depth,
 *             stencil, vertex attribute formats, texture formats/layouts/address modes/filters,
 *             colour target and display formats), with whether gfx_soft.c and gfx_gpu.c honour it
 *   shaders   each GXP program: does the interpreter decode it (and the first unsupported
 *             encoding if not), does usse_spirv translate it (and its reason if not)
 * all weighted by draws and frames, with the first frame each was seen (to find it again with
 * RECOMP_SHOT). The support columns below are this file's reading of the backends, kept next to
 * the code they describe: when a backend gains a feature, flip its entry here.
 * dev/gfxcov.py turns one or more of these files (one per game) into a gap table.
 * RECOMP_SPIRV_DUMP=<dir> also writes every module translated here, for spirv-val.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "rt.h"
#include "hle.h"
#include "gfx.h"
#include "usse.h"
#include "usse_spirv.h"
#include "gfx_coverage.h"

static int enabled = -1;
static const char *out_path;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t frame;          /* presents so far */
static uint64_t ndraws, nscenes;

int gfxcov_on(void) {
	if (enabled < 0) { out_path = getenv("RECOMP_GFX_COVERAGE"); enabled = out_path && *out_path; if (enabled) atexit(gfxcov_write); }
	return enabled;
}

/* ---- api ---- */
enum { API_IMPL, API_NOOP, API_TRACKED, API_MISSING, API_MODULE };   /* MODULE: Sony's own code handles it (GXM); IMPL: the host's (display) */
static const char *const api_kind_name[] = {"implemented", "no-op", "tracked-unused", "missing", "module"};
#define MAXARGS 8
typedef struct { const char *name; int kind; uint64_t calls; uint32_t first_frame; int nargs; uint32_t args[MAXARGS][13]; } Api;
static Api apis[512];
static int napis;

void gfxcov_api(const char *name, HleFn fn, Cpu *c, int module) {
	if (!gfxcov_on() || (strncmp(name, "sceGxm", 6) && strncmp(name, "sceDisplay", 10))) return;
	pthread_mutex_lock(&lock);
	Api *a = 0;
	for (int i = 0; i < napis; i++) if (apis[i].name == name || !strcmp(apis[i].name, name)) { a = &apis[i]; break; }
	if (!a && napis < 512) {
		a = &apis[napis++]; memset(a, 0, sizeof *a); a->name = name; a->first_frame = frame;
		a->kind = module ? API_MODULE : !fn ? API_MISSING : API_IMPL;
	}
	if (a) {
		a->calls++;
		if (a->kind != API_IMPL && a->kind != API_MODULE) {
			uint32_t w[13] = {c->r[0], c->r[1], c->r[2], c->r[3], c->vfp.s[0], c->vfp.s[1], c->vfp.s[2], c->vfp.s[3], c->vfp.s[4], c->vfp.s[5],
			                  RD32(c->r[13]), RD32(c->r[13] + 4), RD32(c->r[13] + 8)};
			int seen = 0;
			for (int i = 0; i < a->nargs; i++) if (!memcmp(a->args[i], w, sizeof w)) seen = 1;
			if (!seen && a->nargs < MAXARGS) memcpy(a->args[a->nargs++], w, sizeof w);
		}
	}
	pthread_mutex_unlock(&lock);
}

/* ---- features ---- */
enum { NO = 0, YES = 1, PARTIAL = 2 };
static const char *const sup_name[] = {"no", "yes", "partial"};
typedef struct { char key[24], value[40]; uint64_t draws, frames; uint32_t first_frame, last_frame; int soft, gpu; const char *note; } Feat;
static Feat feats[1024];
static int nfeats;

static void feat(const char *key, const char *value, int soft, int gpu, const char *note) {
	Feat *f = 0;
	for (int i = 0; i < nfeats; i++) if (!strcmp(feats[i].key, key) && !strcmp(feats[i].value, value)) { f = &feats[i]; break; }
	if (!f) {
		if (nfeats >= 1024) return;
		f = &feats[nfeats++]; memset(f, 0, sizeof *f);
		snprintf(f->key, sizeof f->key, "%s", key); snprintf(f->value, sizeof f->value, "%s", value);
		f->soft = soft; f->gpu = gpu; f->note = note; f->first_frame = frame; f->last_frame = frame; f->frames = 1;
	}
	f->draws++;
	if (f->last_frame != frame) { f->last_frame = frame; f->frames++; }
}

/* SceGxmTextureBaseFormat (format >> 24) */
static const char *tex_base_name(uint32_t b) {
	static const struct { uint8_t b; const char *n; } t[] = {
		{0x00, "U8"}, {0x01, "S8"}, {0x02, "U4U4U4U4"}, {0x03, "U8U3U3U2"}, {0x04, "U1U5U5U5"}, {0x05, "U5U6U5"}, {0x06, "S5S5U6"},
		{0x07, "U8U8"}, {0x08, "S8S8"}, {0x09, "U16"}, {0x0a, "S16"}, {0x0b, "F16"}, {0x0c, "U8U8U8U8"}, {0x0d, "S8S8S8S8"},
		{0x0e, "U2U10U10U10"}, {0x0f, "U16U16"}, {0x10, "S16S16"}, {0x11, "F16F16"}, {0x12, "F32"}, {0x13, "F32M"},
		{0x14, "X8S8S8U8"}, {0x15, "X8U24"}, {0x17, "U32"}, {0x18, "S32"}, {0x19, "SE5M9M9M9"}, {0x1a, "F11F11F10"},
		{0x1b, "F16F16F16F16"}, {0x1c, "U16U16U16U16"}, {0x1d, "S16S16S16S16"}, {0x1e, "F32F32"}, {0x1f, "U32U32"},
		{0x80, "PVRT2BPP"}, {0x81, "PVRT4BPP"}, {0x82, "PVRTII2BPP"}, {0x83, "PVRTII4BPP"}, {0x85, "UBC1"}, {0x86, "UBC2"},
		{0x87, "UBC3"}, {0x88, "UBC4"}, {0x89, "SBC4"}, {0x8a, "UBC5"}, {0x8b, "SBC5"}, {0x90, "YUV420P2"}, {0x91, "YUV420P3"},
		{0x92, "YUV422"}, {0x94, "P4"}, {0x95, "P8"}, {0x98, "U8U8U8"}, {0x99, "S8S8S8"}, {0x9a, "U2F10F10F10"}};
	for (size_t i = 0; i < sizeof t / sizeof *t; i++) if (t[i].b == b) return t[i].n;
	return 0;
}
/* gfx_soft.c's texel_raw decodes these (gfx_gpu.c uploads through the same decoder) */
static int tex_base_supported(uint32_t b) {
	static const uint8_t ok[] = {0x00, 0x02, 0x04, 0x05, 0x07, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x11, 0x12, 0x15, 0x1a, 0x1b, 0x9a, 0x90, 0x91, 0x92, 0x98, 0x99, 0x85, 0x86, 0x87, 0x88, 0x80, 0x81, 0x82, 0x83};
	for (size_t i = 0; i < sizeof ok; i++) if (ok[i] == b) return 1;
	return 0;
}
/* SceGxmColorBaseFormat (format & 0xF1800000) */
static const char *color_base_name(uint32_t b) {
	static const struct { uint32_t b; const char *n; } t[] = {
		{0x00000000u, "U8U8U8U8"}, {0x10000000u, "U8U8U8"}, {0x30000000u, "U5U6U5"}, {0x40000000u, "U1U5U5U5"}, {0x50000000u, "U4U4U4U4"},
		{0x60000000u, "U8U3U3U2"}, {0xF0000000u, "F16"}, {0x00800000u, "F16F16"}, {0x10800000u, "F32"}, {0x20800000u, "S16"},
		{0x30800000u, "S16S16"}, {0x40800000u, "U16"}, {0x50800000u, "U16U16"}, {0x60800000u, "U2U10U10U10"}, {0x80800000u, "U8"},
		{0x90800000u, "S8"}, {0xA0800000u, "S5S5U6"}, {0xB0800000u, "U8U8"}, {0xC0800000u, "S8S8"}, {0xD0800000u, "U8S8S8U8"},
		{0xE0800000u, "S8S8S8S8"}, {0x01000000u, "F16F16F16F16"}, {0x11000000u, "F32F32"}, {0x21000000u, "F11F11F10"},
		{0x31000000u, "SE5M9M9M9"}, {0x41000000u, "U2F10F10F10"}};
	for (size_t i = 0; i < sizeof t / sizeof *t; i++) if (t[i].b == b) return t[i].n;
	return 0;
}
/* gfx_soft.c's target_read_of/target_write_of: explicit cases, plus the default 4-byte case,
 * which is only right for U8U8U8U8 itself */
static int color_base_supported(uint32_t b) {
	static const uint32_t ok[] = {0x00000000u, 0x30000000u, 0x50000000u, 0x01000000u, 0x00800000u, 0x11000000u, 0x10800000u, 0x41000000u, 0x21000000u,
	                              0x80800000u};   /* U8: one byte a pixel (gfx_soft.c's store/load, gfx_gpu.c's target kind 5) */
	for (size_t i = 0; i < sizeof ok / sizeof *ok; i++) if (ok[i] == b) return 1;
	return 0;
}

static void named(char *buf, size_t n, const char *name, uint32_t raw) {
	if (name) snprintf(buf, n, "%s", name); else snprintf(buf, n, "%#x", raw);
}

static void draw_features(const GfxDraw *d, uint32_t units) {
	char v[40];
	/* SceGxmPrimitiveType (raw, gfx.h's gfx_prim_kind): both backends expand triangles, strips,
	 * fans and triangle edges (filled) into triangles; points are point sprites on the software
	 * path (the GPU path hands them to it); lines are dropped */
	static const char *const prim[] = {"TRIANGLES", "LINES", "POINTS", "TRIANGLE_STRIP", "TRIANGLE_FAN", "TRIANGLE_EDGES"};
	int pk = gfx_prim_kind(d->prim), pok = (d->prim & ~0x1c000000u) == 0 && pk < 6 && pk != GFX_PRIM_LINES;
	named(v, sizeof v, (d->prim & ~0x1c000000u) == 0 && pk < 6 ? prim[pk] : 0, d->prim);
	feat("prim", v, pok, pok, pok ? 0 : "draw skipped");
	feat("index", d->index_format ? "U32" : "U16", YES, YES, 0);
	/* SceGxmCullMode: gfx_soft.c culls (checked against itself with culling compiled out: an opaque
	 * 3D scene's hidden-by-depth-test back faces don't move a pixel either way); gfx_gpu.c's own
	 * winding convention against this isn't pinned down yet (see its get_pipeline), so it still
	 * doesn't cull -- a real divergence against the software reference it's checked against, not
	 * just a gap. */
	static const char *const cull[] = {"NONE", "CW", "CCW"};
	named(v, sizeof v, d->cull >= 0 && d->cull < 3 ? cull[d->cull] : 0, (uint32_t)d->cull);
	feat("cull", v, YES, YES, 0);   /* gfx_gpu.c: SDL's cull mode, resolved against the viewport's y flip (cull_sdl) */
	if (d->blend.present) {
		/* SceGxmBlendFunc: ADD/SUBTRACT/REVERSE_SUBTRACT/MIN/MAX all map directly in both backends
		 * (gfx_soft.c's blend_apply, gfx_gpu.c's conv_blendop); NONE (0) is still treated as ADD */
		static const char *const fn[] = {"NONE", "ADD", "SUBTRACT", "REVERSE_SUBTRACT", "MIN", "MAX"};
		for (int a = 0; a < 2; a++) {
			int f = a ? d->blend.alpha_func : d->blend.color_func, ok = f <= 5;
			named(v, sizeof v, f < 6 ? fn[f] : 0, (uint32_t)f);
			feat(a ? "blend.alpha_func" : "blend.color_func", v, ok, ok, ok ? 0 : "treated as ADD");
		}
		/* SceGxmBlendFactor: 0-9 in both; the two SATURATE factors become ONE */
		static const char *const fac[] = {"ZERO", "ONE", "SRC_COLOR", "1-SRC_COLOR", "SRC_ALPHA", "1-SRC_ALPHA", "DST_COLOR",
		                                  "1-DST_COLOR", "DST_ALPHA", "1-DST_ALPHA", "SRC_ALPHA_SATURATE", "DST_ALPHA_SATURATE"};
		const uint8_t fs[4] = {d->blend.color_src, d->blend.color_dst, d->blend.alpha_src, d->blend.alpha_dst};
		for (int i = 0; i < 4; i++) {
			int ok = fs[i] < 10;
			named(v, sizeof v, fs[i] < 12 ? fac[fs[i]] : 0, fs[i]);
			feat("blend.factor", v, ok, ok, ok ? 0 : "treated as ONE");
		}
		/* SceGxmColorMask: both backends honour a partial mask now (gfx_soft.c's raster_rows,
		 * gfx_gpu.c's color_write_mask) */
		int full = (d->blend.mask & 15) == 15;
		snprintf(v, sizeof v, "%s%s%s%s", d->blend.mask & 2 ? "R" : "", d->blend.mask & 4 ? "G" : "", d->blend.mask & 8 ? "B" : "", d->blend.mask & 1 ? "A" : "");
		if (!*v) snprintf(v, sizeof v, "NONE");
		feat("color_mask", v, YES, YES, full ? 0 : "raw output words (U32/S32) aren't masked");
	}
	/* sceGxmSetViewport/SetViewportEnable: both backends map offset+scale*ndc now (gfx_soft.c's
	 * record_tri, gfx_gpu.c's SDL_GPUViewport); never set (an old capture too) keeps the old
	 * implicit full-target NDC mapping */
	feat("viewport", d->viewport_set ? (d->viewport_disabled ? "disabled" : "set") : "never set", YES, YES, 0);
	/* sceGxmSetRegionClip: OUTSIDE (the common case) scissors in both backends now; INSIDE isn't a
	 * single rectangle and isn't modelled by either */
	uint32_t cm = d->clip_mode >> 30;
	const char *cmname = cm == 0 ? "NONE" : cm == 1 ? "ALL" : cm == 2 ? "OUTSIDE" : "INSIDE";
	int clip_ok = cm != 3;   /* INSIDE */
	if (d->clip_mode) feat("region_clip", cmname, clip_ok, clip_ok, clip_ok ? 0 : "not scissored (not a single rectangle)");
	/* sceGxmSetFrontFragmentProgramEnable(..., DISABLED): both backends skip the colour write now,
	 * depth/stencil still run */
	if (d->fp_disabled) feat("fragment_program_enable", "disabled", YES, YES, 0);
	static const char *const cmp[] = {"NEVER", "LESS", "EQUAL", "LESS_EQUAL", "GREATER", "NOT_EQUAL", "GREATER_EQUAL", "ALWAYS"};
	feat("depth_func", cmp[d->depth_func & 7], YES, YES, 0);
	feat("depth_write", d->depth_write ? "on" : "off", YES, YES, 0);
	if (d->depth_bias_factor || d->depth_bias_units) {   /* sceGxmSetFrontDepthBias: soft per fragment, gpu the rasterizer's bias (its slope factor x render scale) */
		snprintf(v, sizeof v, "factor %d units %d", d->depth_bias_factor, d->depth_bias_units);
		feat("depth_bias", v, YES, YES, 0);
	}
	if (d->stencil_func != 7 || d->stencil_wmask) {
		feat("stencil_func", cmp[d->stencil_func & 7], YES, YES, "front faces only");
		static const char *const op[] = {"KEEP", "ZERO", "REPLACE", "INCR", "DECR", "INVERT", "INCR_WRAP", "DECR_WRAP"};
		const uint8_t ops[3] = {d->stencil_fail, d->depth_fail, d->depth_pass};
		for (int i = 0; i < 3; i++) feat("stencil_op", op[ops[i] & 7], YES, YES, 0);
	}
	/* SceGxmAttributeFormat: gfx_soft.c's attr_comp/attr_byte_size (shared with gfx_gpu.c) */
	static const char *const af[] = {"U8", "S8", "U16", "S16", "U8N", "S8N", "U16N", "S16N", "F16", "F32", "UNTYPED"};
	for (int i = 0; i < d->nattr; i++) {
		int f = d->attr[i].format, ok = f >= 0 && f <= 10;   /* all SceGxmAttributeFormat cases attr_comp/attr_byte_size now handle */
		named(v, sizeof v, f < 11 ? af[f] : 0, (uint32_t)f);
		feat("attr_format", v, ok, ok, ok ? 0 : "components read as -999");
	}
	/* textures: what the fragment program might sample (units with data bound) */
	for (int u = 0; u < 16; u++) {
		const GfxTexture *t = &d->tex[u];
		if (!(units >> u & 1)) continue;
		if (!t->data) { feat("tex_unbound", "sampled unit with no data", YES, YES, 0); continue; }   /* soft: transparent black (usse_texfn); gpu: a 1x1 dummy texture (dummy_texture), not the scene's own render target */
		uint32_t base = t->format >> 24;
		int ok = tex_base_supported(base);
		named(v, sizeof v, tex_base_name(base), base);
		char sw[48]; snprintf(sw, sizeof sw, "%s swz%u", v, (t->format >> 12) & 7);
		feat("tex_format", v, ok, ok, ok ? 0 : "not decoded");
		feat("tex_format+swizzle", sw, ok, ok, 0);
		static const char *const lay[] = {"linear", "swizzled", "tiled", "cube"};
		int layout = (t->u_mode >> 4) & 3;
		feat("tex_layout", lay[layout], layout < 3 ? YES : PARTIAL, layout < 3 ? YES : PARTIAL, layout < 3 ? 0 : "level 0 only (soft cube_texel; gpu a 6-layer array)");
		/* SceGxmTextureAddrMode: soft does repeat, mirror, mirror-clamp and clamp (the border modes
		 * 4-7 clamp: wrap_coord); gfx_gpu.c maps 0-2 to SDL's REPEAT/MIRRORED_REPEAT/CLAMP_TO_EDGE
		 * per axis, folds 3 (no mirror-once in SDL) and 5-7 to clamp and 4 to repeat (gxm_addr_mode) */
		static const char *const am[] = {"REPEAT", "MIRROR", "CLAMP", "MIRROR_CLAMP", "REPEAT_IGNORE_BORDER", "CLAMP_FULL_BORDER",
		                                 "CLAMP_IGNORE_BORDER", "CLAMP_HALF_BORDER"};
		for (int k = 0; k < 2; k++) {
			int m = (k ? t->v_mode : t->u_mode) & 15;
			named(v, sizeof v, m < 8 ? am[m] : 0, (uint32_t)m);
			feat(k ? "tex_vaddr" : "tex_uaddr", v, m == 4 ? PARTIAL : m < 8 ? YES : NO, m == 3 ? PARTIAL : m < 8 ? YES : NO,
			     m == 3 ? "gpu clamps (no mirror-once)" : m == 4 ? "soft clamps" : 0);
		}
		int mips = (t->v_mode >> 4) & 15;
		if (mips > 1 && layout < 3) feat("tex_mips", "mipmapped", YES, YES, 0);   /* soft texel_lod, gpu the uploaded chain (a cube's: tex_layout) */
		static const char *const filt[] = {"POINT", "LINEAR", "MIPMAP_LINEAR", "MIPMAP_POINT"};
		snprintf(v, sizeof v, "min %s mag %s", filt[t->min_filter & 3], filt[t->mag_filter & 3]);
		feat("tex_filter", v, YES, YES, 0);   /* soft texel_level (bilinear), gpu tex_sampler */
		if (mips > 1 && GFX_TEX_MIP_LINEAR(t->format)) feat("tex_mip_filter", "enabled", YES, YES, 0);   /* soft texel_lod, gpu tex_sampler: levels blended */
		float lb = GFX_TEX_LOD_BIAS(t->format);
		if (lb != 0 && mips > 1) { snprintf(v, sizeof v, "%+g levels", lb); feat("tex_lod_bias", v, YES, YES, 0); }   /* soft texel_lod, gpu usse_spirv.c sample_lod */
	}
}

/* ---- shaders ---- */
typedef struct { uint32_t addr; int frag, parsed, interp, gpu; uint32_t units; uint64_t draws; uint32_t first_frame; char interp_bad[48], gpu_err[160]; } Shader;
static Shader shaders[2048];
static int nshaders;
typedef struct { uint32_t v, f; } Pair;
static Pair pairs[4096];
static int npairs;

static Shader *shader_of(uint32_t addr) {
	for (int i = 0; i < nshaders; i++) if (shaders[i].addr == addr) return &shaders[i];
	if (nshaders >= 2048) return 0;
	Shader *s = &shaders[nshaders++]; memset(s, 0, sizeof *s);
	s->addr = addr; s->first_frame = frame; s->gpu = 1;
	/* the texture units it declares (sampler parameters: category 2, resource index = unit; the
	 * table as gxm.c's param_at reads it) -- the other units hold whatever was bound last */
	uint32_t np = RD32(addr + 0x24);
	for (uint32_t i = 0; i < np && i < 256; i++) {
		uint32_t pa = addr + 0x28 + RD32(addr + 0x28) + 16 * i;
		if ((RD16(pa + 4) & 15) == 2 && RD32(pa + 12) < 16) s->units |= 1u << RD32(pa + 12);
	}
	const UsseProgram *p = usse_get(addr);
	if (!p) { snprintf(s->interp_bad, sizeof s->interp_bad, "GXP not parsed"); s->gpu = 0; return s; }
	s->parsed = 1; s->frag = usse_is_fragment(p); s->interp = usse_supported(p);
	if (!s->interp) {   /* the first unsupported encoding, from the disassembly (op1 is its top 5 bits) */
		char *buf = 0; size_t len = 0;
#ifdef _WIN32   /* (no open_memstream: the disassembly through a temporary file) */
		FILE *m = tmpfile();
		if (m) {
			usse_disassemble(p, m); len = (size_t)ftell(m); rewind(m);
			buf = malloc(len + 1); if (buf) { len = fread(buf, 1, len, m); buf[len] = 0; }
			fclose(m);
#else
		FILE *m = open_memstream(&buf, &len);
		if (m) {
			usse_disassemble(p, m); fclose(m);
#endif
			for (char *ln = buf; ln && *ln; ) {
				char *nl = strchr(ln, '\n');
				unsigned long long raw;
				if (strstr(ln, "unsupported encoding") && sscanf(ln, " %*d: %llx", &raw) == 1) {
					snprintf(s->interp_bad, sizeof s->interp_bad, "op1 %#llx raw %016llx", raw >> 59, raw); break;
				}
				ln = nl ? nl + 1 : 0;
			}
			free(buf);
		}
	}
	return s;
}
/* translation needs the pair (the varying layout is agreed between the two programs) */
static void pair_check(uint32_t vaddr, uint32_t faddr, Shader *vs, Shader *fs) {
	for (int i = 0; i < npairs; i++) if (pairs[i].v == vaddr && pairs[i].f == faddr) return;
	if (npairs < 4096) pairs[npairs++] = (Pair){vaddr, faddr};
	if (!vs || !fs || !vs->parsed || !fs->parsed) return;
	const UsseProgram *vp = usse_get(vaddr), *fp = usse_get(faddr);
	const UsseVertexLayout *vl = usse_vertex_layout(vp);
	const UsseFragLayout *fl = usse_fragment_layout(fp);
	static const uint32_t sa0[256];
	Shader *both[2] = {vs, fs};
	const UsseProgram *progs[2] = {vp, fp};
	for (int k = 0; k < 2; k++) {
		int units[16], nsmp = 0;
		SpirvModule m = usse_spirv_translate(progs[k], vl, fl, sa0, 16, units, &nsmp, 0, 0, 0, 0);
		if (!m.ok && both[k]->gpu) { both[k]->gpu = 0; snprintf(both[k]->gpu_err, sizeof both[k]->gpu_err, "%s", m.err); }
		const char *dir = getenv("RECOMP_SPIRV_DUMP");   /* every translated module, for spirv-val */
		if (m.ok && dir) {
			char path[512]; snprintf(path, sizeof path, "%s/%08x_%08x_%s.spv", dir, vaddr, faddr, k ? "frag" : "vert");
			FILE *f = fopen(path, "wb"); if (f) { fwrite(m.words, 4, m.nwords, f); fclose(f); }
		}
		usse_spirv_free(&m);
	}
}

/* ---- targets ---- */
void gfxcov_scene(const GfxTarget *t) {
	if (!gfxcov_on()) return;
	pthread_mutex_lock(&lock);
	nscenes++;
	uint32_t base = t->format & 0xF1800000u;
	int ok = color_base_supported(base);
	char v[40]; named(v, sizeof v, color_base_name(base), base);
	feat("target_format", v, ok, ok, ok ? 0 : "read/written as U8U8U8U8");   /* gfx_gpu.c target_kind: 8-bit families RGBA8, half-float ones RGBA16F, F32 R32G32F */
	char sw[48]; snprintf(sw, sizeof sw, "%s swz%u", v, (t->format >> 20) & 3);
	feat("target_format+swizzle", sw, ok, ok ? PARTIAL : NO, 0);
	feat("target_depth", t->depth ? "depth/stencil bound" : "colour only", YES, YES, 0);
	pthread_mutex_unlock(&lock);
}

void gfxcov_draw(const GfxDraw *d) {
	if (!gfxcov_on()) return;
	pthread_mutex_lock(&lock);
	ndraws++;
	Shader *vs = d->vprog ? shader_of(d->vprog) : 0, *fs = d->fprog ? shader_of(d->fprog) : 0;
	draw_features(d, fs ? fs->units : 0);
	if (vs) vs->draws++;
	if (fs) fs->draws++;
	if (vs && fs) pair_check(d->vprog, d->fprog, vs, fs);
	pthread_mutex_unlock(&lock);
}

void gfxcov_gap(const char *key, const char *value, int partial, const char *note) {
	if (!gfxcov_on()) return;
	pthread_mutex_lock(&lock);
	feat(key, value, partial ? PARTIAL : NO, partial ? PARTIAL : NO, note);
	pthread_mutex_unlock(&lock);
}

void gfxcov_present(uint32_t format) {
	if (!gfxcov_on()) return;
	pthread_mutex_lock(&lock);
	/* SceDisplayPixelFormat: A8B8G8R8 (0) is the one the backends present; A2B10G10R10 is not */
	feat("display_format", format == 0 ? "A8B8G8R8" : "A2B10G10R10", format == 0, format == 0, 0);
	frame++;
	int flush = frame % 120 == 0;   /* also written periodically: a window close ends the process with _exit */
	pthread_mutex_unlock(&lock);
	if (flush) gfxcov_write();
}

/* ---- output ---- */
static void json_str(FILE *f, const char *s) {
	fputc('"', f);
	for (; s && *s; s++) { if (*s == '"' || *s == '\\') fputc('\\', f); if ((unsigned char)*s >= 32) fputc(*s, f); }
	fputc('"', f);
}
void gfxcov_write(void) {
	if (!gfxcov_on()) return;
	pthread_mutex_lock(&lock);
	char tmp[1024]; snprintf(tmp, sizeof tmp, "%s.tmp", out_path);
	FILE *f = fopen(tmp, "w");
	if (!f) { pthread_mutex_unlock(&lock); return; }
	const char *title = getenv("RECOMP_TITLE");
	fprintf(f, "{\n \"title\": "); json_str(f, title ? title : "");
	fprintf(f, ",\n \"frames\": %u, \"scenes\": %llu, \"draws\": %llu,\n \"api\": [", frame, (unsigned long long)nscenes, (unsigned long long)ndraws);
	for (int i = 0; i < napis; i++) {
		const Api *a = &apis[i];
		fprintf(f, "%s\n  {\"name\": \"%s\", \"kind\": \"%s\", \"calls\": %llu, \"first_frame\": %u, \"args\": [", i ? "," : "", a->name,
		        api_kind_name[a->kind], (unsigned long long)a->calls, a->first_frame);
		for (int j = 0; j < a->nargs; j++) {
			fprintf(f, "%s[", j ? ", " : "");
			for (int k = 0; k < 13; k++) fprintf(f, "%s%u", k ? ", " : "", a->args[j][k]);
			fprintf(f, "]");
		}
		fprintf(f, "]}");
	}
	fprintf(f, "\n ],\n \"features\": [");
	for (int i = 0; i < nfeats; i++) {
		const Feat *x = &feats[i];
		fprintf(f, "%s\n  {\"key\": \"%s\", \"value\": ", i ? "," : "", x->key); json_str(f, x->value);
		fprintf(f, ", \"draws\": %llu, \"frames\": %llu, \"first_frame\": %u, \"soft\": \"%s\", \"gpu\": \"%s\", \"note\": ",
		        (unsigned long long)x->draws, (unsigned long long)x->frames, x->first_frame, sup_name[x->soft], sup_name[x->gpu]);
		json_str(f, x->note ? x->note : ""); fprintf(f, "}");
	}
	fprintf(f, "\n ],\n \"shaders\": [");
	for (int i = 0; i < nshaders; i++) {
		const Shader *s = &shaders[i];
		fprintf(f, "%s\n  {\"addr\": \"%#x\", \"stage\": \"%s\", \"draws\": %llu, \"first_frame\": %u, \"interp\": %s, \"interp_bad\": ",
		        i ? "," : "", s->addr, !s->parsed ? "?" : s->frag ? "fragment" : "vertex", (unsigned long long)s->draws, s->first_frame,
		        s->interp ? "true" : "false");
		json_str(f, s->interp_bad);
		fprintf(f, ", \"gpu\": %s, \"gpu_err\": ", s->gpu ? "true" : "false"); json_str(f, s->gpu_err); fprintf(f, "}");
	}
	fprintf(f, "\n ]\n}\n");
	fclose(f);
	rename(tmp, out_path);
	pthread_mutex_unlock(&lock);
}
