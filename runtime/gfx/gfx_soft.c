/*
 * Software graphics backend: draws on the CPU into the guest's own render targets (the colour
 * surface's pixels in the arena), so a presented frame is the game's framebuffer. Slow but
 * API-free, and a reference to check GPU backends against.
 *
 * env: RECOMP_DUMP_FRAME=n   print every draw of frame n in full
 *      RECOMP_SHOT=n         save every n-th presented frame as shot_NNNNN.ppm (in the cwd)
 *      RECOMP_SHADERS=interp run the game's own vertex/fragment GXP programs through usse.c's
 *                            interpreter instead of the parameter-name stand-ins below. A draw
 *                            whose program uses an instruction the interpreter doesn't support
 *                            falls back to the stand-in path (logged once per program pair).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include "rt.h"
#include "gfx.h"
#include "usse.h"
#include "platform.h"

static GfxTarget target;
/* depth buffers, one per depth/stencil surface address (ds_data; t->data, the COLOUR address, for
 * an old capture that never carried the surface -- see gfx.h's GfxTarget comment) */
typedef struct { uint32_t key, w, h; float *z; uint8_t *st; uint64_t used; } Depth;
#define MAXDEPTH 64
static Depth depths[MAXDEPTH];
static Depth *depth;
static uint64_t depth_clock;
static int scene_has_depth_plane, scene_has_stencil_plane;   /* an S8-only (or depth-only) surface must not depth-test (or stencil-test) */
static int ds_has_depth(uint32_t fmt) { return (fmt & 0x00044000u) != 0; }
static int ds_has_stencil(uint32_t fmt) { return (fmt & 0x00022000u) != 0; }
static int ds_packed(uint32_t fmt) { return fmt == 0x01266000u; }   /* S8D24: depth+stencil interleaved, one 32-bit word/pixel */
/* (a 3D level cycles through more targets than a fixed 16; when full, the least recently used one
 * goes -- only ever called after a flush, so no band is still reading it). *fresh says whether
 * this surface wasn't already cached (so its guest-memory contents, if force-load wants them,
 * still need pulling in) -- gfx_soft_begin_scene. */
static Depth *depth_for(const GfxTarget *t, int *fresh) {
	uint32_t key = t->ds_data ? t->ds_data : t->data;
	Depth *free_slot = 0, *oldest = &depths[0];
	for (int i = 0; i < MAXDEPTH; i++) {
		Depth *d = &depths[i];
		if (d->key == key && d->w == t->stride && d->h == t->height) { d->used = ++depth_clock; if (fresh) *fresh = 0; return d; }
		if (!d->key && !free_slot) free_slot = d;
		if (d->used < oldest->used) oldest = d;
	}
	Depth *d = free_slot ? free_slot : oldest;
	free(d->z); free(d->st);
	*d = (Depth){key, t->stride, t->height, malloc(sizeof(float) * t->stride * t->height), malloc(t->stride * t->height), ++depth_clock};
	if (fresh) *fresh = 1;
	return d;
}
/* guest memory <-> the cached depth/stencil buffer, in the surface's own declared format --
 * force-load pulls in whatever is already there (a previous scene's result the game wants kept,
 * or a value it wrote itself); force-store publishes this scene's result back (e.g. for a later
 * pass that samples it as an X8U24 texture: gfx_soft.c's texel_raw already decodes that format). */
/* a sample's index in the depth/stencil surface: linear rows of `stride` samples, or tiled (gfx.h
 * GfxTarget.ds_type) -- 32x32 tiles, stride/32 per tile row, each tile row by row, as a tiled
 * texture of that width is addressed */
static uint32_t ds_index(const GfxTarget *t, uint32_t stride, uint32_t x, uint32_t y) {
	if (t->ds_type == 1) { uint32_t tiles_x = stride / 32 ? stride / 32 : 1; return ((y / 32) * tiles_x + x / 32) * 1024 + (y % 32) * 32 + x % 32; }
	return y * stride + x;
}
static void depth_load_from_guest(Depth *dp, const GfxTarget *t) {
	uint32_t fmt = t->ds_format, da = t->ds_data, sa = t->ds_stencil_data, stride = t->ds_stride ? t->ds_stride : dp->w;
	for (uint32_t i = 0; i < dp->w * dp->h; i++) {
		uint32_t m = ds_index(t, stride, i % dp->w, i / dp->w);   /* the sample's index in the surface's own layout */
		float z = t->ds_background_depth; uint8_t s = t->ds_background_stencil;
		if (ds_packed(fmt) && da) { uint32_t p = RD32(da + 4 * m); z = (p & 0xFFFFFFu) / 16777215.f; s = (uint8_t)(p >> 24); }
		else {
			if (ds_has_depth(fmt) && da) {
				if (fmt == 0x02444000u) z = RD16(da + 2 * m) / 65535.f;
				else { uint32_t raw = RD32(da + 4 * m); memcpy(&z, &raw, 4); }
			}
			if (ds_has_stencil(fmt) && sa) s = RD8(sa + m);
		}
		dp->z[i] = z; dp->st[i] = s;
	}
}
/* for gfx_gpu.c: the surface decoded into w*h depth values and stencil bytes (its preload) */
void gfx_decode_depth_stencil(const GfxTarget *t, uint32_t w, uint32_t h, float *z, uint8_t *st) {
	Depth dp = { t->ds_data ? t->ds_data : t->data, w, h, z, st, 0 };
	depth_load_from_guest(&dp, t);
}
static void depth_store_to_guest(const Depth *dp, const GfxTarget *t) {
	uint32_t fmt = t->ds_format, da = t->ds_data, sa = t->ds_stencil_data, stride = t->ds_stride ? t->ds_stride : dp->w;
	for (uint32_t i = 0; i < dp->w * dp->h; i++) {
		uint32_t m = ds_index(t, stride, i % dp->w, i / dp->w);
		if (ds_packed(fmt) && da) { uint32_t d24 = (uint32_t)(dp->z[i] * 16777215.f + 0.5f); WR32(da + 4 * m, (d24 & 0xFFFFFFu) | ((uint32_t)dp->st[i] << 24)); continue; }
		if (ds_has_depth(fmt) && da) {
			if (fmt == 0x02444000u) WR16(da + 2 * m, (uint16_t)(dp->z[i] * 65535.f + 0.5f));
			else { uint32_t raw; memcpy(&raw, &dp->z[i], 4); WR32(da + 4 * m, raw); }
		}
		if (ds_has_stencil(fmt) && sa) WR8(sa + m, dp->st[i]);
	}
}
static uint8_t stencil_op(int op, uint8_t v, uint8_t ref) {
	switch (op) {
	case 1: return 0; case 2: return ref; case 3: return v == 255 ? 255 : v + 1; case 4: return v ? v - 1 : 0;
	case 5: return ~v; case 6: return v + 1; case 7: return v - 1; default: return v;
	}
}
static int depth_pass(uint32_t f, float z, float d) {
	switch (f) {
	case 0: return 0; case 1: return z < d; case 2: return z == d; case 3: return z <= d;
	case 4: return z > d; case 5: return z != d; case 6: return z >= d; default: return 1;
	}
}
static uint64_t frame;
static long dump_frame = -1, shot_every, shot_last = -1, shot_from = 0, shot_to = -1;   /* RECOMP_SHOT_LAST=n: exit once frame n's shot is written (a test needs nothing after its frame) */
static int inited;
static int shaders_interp;

static int usse_texfn_raw(void *ctx, int unit, float u, float v, float w3, float lod, uint32_t raw[4]);
static int usse_texfn_gather(void *ctx, int unit, float u, float v, float texels[4][4], float frac[2]);
static int ndraw;   /* draws so far this frame (defined with the scene state below; the debug print above it needs it) */
/* RECOMP_DEBUG_PIXEL=x,y: the one pixel RECOMP_DEBUG_DRAW / RECOMP_TRACE_DRAW report (a glyph's interior, a seam); -1: the grid */
static void debug_pixel(int *px, int *py) {
	static int sx = -1, sy = -1, read;
	if (!read) { read = 1; const char *e = getenv("RECOMP_DEBUG_PIXEL"); if (e) sscanf(e, "%d,%d", &sx, &sy); }
	*px = sx; *py = sy;
}
static void init(void) {
	if (inited) return;
	usse_set_texfn_raw(usse_texfn_raw);
	usse_set_texfn_gather(usse_texfn_gather);
	inited = 1;
	if (getenv("RECOMP_DUMP_FRAME")) dump_frame = atol(getenv("RECOMP_DUMP_FRAME"));
	if (getenv("RECOMP_SHOT")) shot_every = atol(getenv("RECOMP_SHOT"));
	if (getenv("RECOMP_SHOT_FROM")) shot_from = atol(getenv("RECOMP_SHOT_FROM"));   /* RECOMP_SHOT_FROM/_TO=n: shots only in that frame window (every frame of a flicker, without a run's worth of them) */
	if (getenv("RECOMP_SHOT_TO")) shot_to = atol(getenv("RECOMP_SHOT_TO"));
	if (getenv("RECOMP_SHOT_LAST")) shot_last = atol(getenv("RECOMP_SHOT_LAST"));
	if (!shaders_interp) shaders_interp = getenv("RECOMP_SHADERS") && !strcmp(getenv("RECOMP_SHADERS"), "interp");
}
/* the shader interpreter regardless of RECOMP_SHADERS: gfx_gpu.c's CPU fallback and its check
 * reference -- the name-matched stand-in shaders only know Shovel Knight's programs, and skipped
 * Unit 13's 3D and UI draws ("no position attribute"), so whatever the GPU couldn't translate
 * simply vanished */
void gfx_soft_use_interp(void) { shaders_interp = 1; }

/* GXP parameter names, for the dump: "name(cat/res)" for each */
static void params(uint32_t prog, char *out, size_t n) {
	out[0] = 0;
	if (!prog) return;
	uint32_t cnt = RD32(prog + 0x24), base = prog + 0x28 + RD32(prog + 0x28);
	for (uint32_t i = 0; i < cnt && i < 12; i++) {
		uint32_t p = base + 16 * i;
		size_t l = strlen(out);
		snprintf(out + l, n - l, "%s%s(c%u r%d)", i ? " " : "", (char *)G(p + (int32_t)RD32(p)), RD16(p + 4) & 15, (int32_t)RD32(p + 12));
	}
}
static float half_to_float_soft(uint16_t h);   /* defined below; attribute F16 (fmt 8) reuses it */
static float attr_comp(uint32_t a, uint8_t fmt, int k) {
	switch (fmt) {
	case 9: { float f; memcpy(&f, G(a + 4 * k), 4); return f; }   /* F32 */
	case 4: return RD8(a + k) / 255.0f;                           /* U8N */
	case 0: return RD8(a + k);                                   /* U8 */
	case 1: return (int8_t)RD8(a + k);                           /* S8 */
	case 2: return RD16(a + 2 * k);                              /* U16 */
	case 6: return RD16(a + 2 * k) / 65535.0f;                   /* U16N */
	case 3: return (int16_t)RD16(a + 2 * k);                     /* S16 */
	case 5: { float f = (int8_t)RD8(a + k) / 127.0f; return f < -1.0f ? -1.0f : f; }   /* S8N */
	case 7: return (int16_t)RD16(a + 2 * k) / 32767.0f;          /* S16N */
	case 8: return half_to_float_soft(RD16(a + 2 * k));          /* F16 */
	case 10: { float f; memcpy(&f, G(a + 4 * k), 4); return f; }   /* UNTYPED: raw bits as float */
	default: return -999;
	}
}
static int attr_byte_size(uint8_t fmt) { switch (fmt) { case 9: case 10: return 4; case 6: case 3: case 7: case 2: case 8: return 2; default: return 1; } }   /* SceGxmAttributeFormat; see attr_comp */
static uint32_t index_at(const GfxDraw *d, uint32_t i) {
	return d->index_format ? RD32(d->indices + 4 * i) : RD16(d->indices + 2 * i);
}

/* ---- shaders, from their GXP parameter names (a stand-in until real shader translation):
 * vertex: g_matWVP (row-major 4x4) transforms vsIn.aPos, otherwise aPos is already clip space;
 *         vsIn.bTex0 is a texture coordinate, vsIn.bColor a colour
 * fragment: a sampler is sampled (times the vertex colour, plus g_addColor, when present);
 *           g_clearColor is a constant; with no parameters, the vertex colour */
#define MAX_VARY 50   /* a vertex program's O words (<= 48) plus the two point-sprite coordinate slots below */
/* the point-sprite coordinate of a SCE_GXM_PRIMITIVE_POINTS sprite rides in the last two varying
 * slots (render_points_interp writes them per corner, vary_at interpolates them like any other):
 * a fragment iterator flagged point_coord (usse.h) reads these instead of a vertex output */
#define PC_U (MAX_VARY - 2)
#define PC_V (MAX_VARY - 1)
typedef struct { float x, y, z, w, u, v, r, g, b, a, invw; float vary[MAX_VARY]; uint32_t probe; } Vert;   /* probe: RECOMP_PROBE=T|P|O:<word>@k, the vertex program's register word */
typedef struct {
	int has_mat, mat_res, pos_reg, tex_reg, col_reg;
	int sampler, has_add, add_res, has_clear, clear_res, has_tint, tint_res;
} Shader;
static uint32_t find_param(uint32_t prog, const char *name, int *res) {
	if (!prog) return 0;
	uint32_t cnt = RD32(prog + 0x24), base = prog + 0x28 + RD32(prog + 0x28);
	for (uint32_t i = 0; i < cnt; i++) {
		uint32_t p = base + 16 * i;
		const char *n = (char *)G(p + (int32_t)RD32(p));
		if (strstr(n, name)) { if (res) *res = (int32_t)RD32(p + 12); return p; }
	}
	return 0;
}
static Shader shader_of(const GfxDraw *d) {
	Shader s = {0};
	s.pos_reg = s.tex_reg = s.col_reg = -1;
	s.has_mat = find_param(d->vprog, "g_matWVP", &s.mat_res) != 0;
	find_param(d->vprog, "aPos", &s.pos_reg);
	find_param(d->vprog, "Tex", &s.tex_reg);
	find_param(d->vprog, "Color", &s.col_reg);
	s.sampler = find_param(d->fprog, "sampler", 0) != 0;
	s.has_add = find_param(d->fprog, "g_addColor", &s.add_res) != 0;
	s.has_clear = find_param(d->fprog, "g_clearColor", &s.clear_res) != 0;
	s.has_tint = find_param(d->fprog, "g_tint", &s.tint_res) != 0;
	return s;
}
static float uf(uint32_t buf, int i) { float f; memcpy(&f, G(buf + 4 * i), 4); return f; }

static void fetch(const GfxDraw *d, const Shader *s, uint32_t ix, Vert *o) {
	float pos[4] = {0, 0, 0, 1}, tex[2] = {0, 0}, col[4] = {1, 1, 1, 1};
	for (int i = 0; i < d->nattr; i++) {
		const GfxAttr *a = &d->attr[i];
		uint32_t at = d->stream[a->stream] + ix * d->stride[a->stream] + a->offset;
		float *dst = a->reg == s->pos_reg ? pos : a->reg == s->tex_reg ? tex : a->reg == s->col_reg ? col : 0;
		int n = a->reg == s->tex_reg ? 2 : a->reg == s->pos_reg ? 3 : 4;
		if (dst) for (int k = 0; k < a->comps && k < n; k++) dst[k] = attr_comp(at, a->format, k);
	}
	if (s->has_mat) {
		float out[4];
		for (int r = 0; r < 4; r++) {
			out[r] = 0;
			for (int k = 0; k < 4; k++) out[r] += uf(d->vuniforms, s->mat_res + 4 * r + k) * pos[k];
		}
		memcpy(pos, out, sizeof out);
	} else pos[3] = 1;
	o->x = pos[0]; o->y = pos[1]; o->z = pos[2]; o->w = pos[3];
	o->u = tex[0]; o->v = tex[1];
	o->r = col[0]; o->g = col[1]; o->b = col[2]; o->a = col[3];
}

/* ---- textures and render targets: pixel access by format (linear layout) */
static int tex_stride(const GfxTexture *t) { return (t->format & 0xfff) ? (int)(t->format & 0xfff) : (t->width + 7) & ~7; }
/* texel index by layout: linear rows; swizzled = Z-order (Morton) over the power-of-two square, the
 * larger dimension's extra bits above; tiled = 32x32 tiles, each row by row */
static uint32_t texel_index(const GfxTexture *t, uint32_t x, uint32_t y) {
	int layout = (t->u_mode >> 4) & 3;
	if (layout == 1) {
		uint32_t w = 1, h = 1; while (w < t->width) w <<= 1; while (h < t->height) h <<= 1;
		uint32_t m = w < h ? w : h, idx = 0, bit = 0;
		for (uint32_t k = 1; k < m; k <<= 1, bit += 2) idx |= ((y & k) ? 1u << bit : 0) | ((x & k) ? 2u << bit : 0);   /* y in the even bits, x in the odd */
		uint32_t rest = w > h ? x / m : y / m;   /* whole squares along the longer side */
		return idx + rest * m * m;
	}
	if (layout == 2) {
		uint32_t tiles_x = (t->width + 31) / 32;
		return ((y / 32) * tiles_x + x / 32) * 1024 + (y % 32) * 32 + x % 32;
	}
	return y * tex_stride(t) + x;
}
/* block-compressed formats address by 4x4 block, not by texel: reuse texel_index's layout maths
 * (linear/swizzled/tiled) at block granularity by handing it a texture sized in blocks. */
static uint32_t block_index(const GfxTexture *t, uint32_t bx, uint32_t by) {
	GfxTexture bt = *t; bt.width = (t->width + 3) / 4; bt.height = (t->height + 3) / 4;
	return texel_index(&bt, bx, by);
}
/* a texel coordinate under SceGxmTextureAddrMode: 0 REPEAT; 1 MIRROR (every other period
 * reflected: 0..w-1, w-1..0, ...); 3 MIRROR_CLAMP (reflected once about 0, then clamped); anything
 * else clamps to the edge (CLAMP, and the border modes 4-7 as before -- 4, REPEAT_IGNORE_BORDER,
 * still clamps here while gfx_gpu.c repeats it; no capture of ours uses it). Mirror used to clamp:
 * Killzone: Mercenary's UI samples 12K draws' worth of textures MIRROR in U. */
static int wrap_coord(int x, int w, int mode) {
	switch (mode & 15) {
	case 0: x %= w; return x < 0 ? x + w : x;
	case 1: { int p = x % (2 * w); if (p < 0) p += 2 * w; return p < w ? p : 2 * w - 1 - p; }
	case 3: if (x < 0) x = -x - 1; return x >= w ? w - 1 : x;
	default: return x < 0 ? 0 : x >= w ? w - 1 : x;
	}
}
static float half_to_float_soft(uint16_t h) {
	uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 0x1F, m = h & 0x3FF, bits;
	if (e == 0) { if (m == 0) bits = s; else { int sh = 0; while (!(m & 0x400)) { m <<= 1; sh++; } m &= 0x3FF; bits = s | ((uint32_t)(127 - 15 - sh + 1) << 23) | (m << 13); } }
	else if (e == 31) bits = s | 0x7F800000u | (m << 13);
	else bits = s | ((e + 112) << 23) | (m << 13);
	float f; memcpy(&f, &bits, 4); return f;
}
/* BC3's alpha half, which is also the whole of a BC4 block: two 8-bit endpoints, then sixteen
 * 3-bit indices (texel tib's at bit 3*tib of the 48 that follow) -- 8 interpolated values when
 * a0 > a1, else 6 plus 0 and 255. Returns 0..255. */
static float bc_alpha_block(uint32_t ba, uint32_t tib) {
	uint32_t a0 = RD8(ba), a1 = RD8(ba + 1);
	uint64_t aidx = RD32(ba + 2) | ((uint64_t)RD16(ba + 6) << 32);
	uint32_t sel = (uint32_t)((aidx >> (3 * tib)) & 7);
	if (a0 > a1) return sel == 0 ? a0 : sel == 1 ? a1 : ((8 - sel) * a0 + (sel - 1) * a1) / 7.f;
	return sel == 0 ? a0 : sel == 1 ? a1 : sel == 6 ? 0 : sel == 7 ? 255 : ((6 - sel) * a0 + (sel - 1) * a1) / 5.f;
}
/* bytes per 4x4 block of the BC formats (UBC1 and UBC4 8, UBC2/3 16), 0 for anything else */
static uint32_t bc_block_bytes(uint32_t base) {
	return base == 0x85000000u || base == 0x88000000u ? 8 : base == 0x86000000u || base == 0x87000000u ? 16 : 0;
}
/* BC1/2/3 (DXT1/3/5, "UBC1/2/3"): the shared RGB565 4-colour interpolation block. BC2/3's colour
 * half always uses 4-colour mode (no punch-through; that only applies to standalone BC1). */
static void dxt_rgb(uint32_t c0, uint32_t c1, uint32_t idx2, int tib, int bc1_mode, float out[3], int *transparent) {
	float p[4][3];
	p[0][0] = ((c0 >> 11) & 31) / 31.f; p[0][1] = ((c0 >> 5) & 63) / 63.f; p[0][2] = (c0 & 31) / 31.f;
	p[1][0] = ((c1 >> 11) & 31) / 31.f; p[1][1] = ((c1 >> 5) & 63) / 63.f; p[1][2] = (c1 & 31) / 31.f;
	*transparent = 0;
	if (bc1_mode && c0 <= c1) {
		for (int k = 0; k < 3; k++) p[2][k] = (p[0][k] + p[1][k]) * 0.5f;
		p[3][0] = p[3][1] = p[3][2] = 0;
	} else {
		for (int k = 0; k < 3; k++) { p[2][k] = (2 * p[0][k] + p[1][k]) / 3; p[3][k] = (p[0][k] + 2 * p[1][k]) / 3; }
	}
	int sel = (int)((idx2 >> (2 * tib)) & 3);
	if (bc1_mode && c0 <= c1 && sel == 3) *transparent = 1;
	out[0] = p[sel][0]; out[1] = p[sel][1]; out[2] = p[sel][2];
}
/* ---- PVRTC / PVRTC-II (PVRT2BPP, PVRT4BPP, PVRTII2BPP, PVRTII4BPP): a 64-bit block covers 4x4
 * texels (4bpp) or 8x4 (2bpp); the stored colours are a deliberately low-res image that every
 * texel bilinearly interpolates from its surrounding 2x2 neighbourhood of blocks, modulated by a
 * full-resolution but low-precision per-texel signal (Fenney, "Texture Compression using
 * Low-Frequency Signal Modulation", 2003 -- the algorithm Imagination's own PVRTexTool/SDK sample
 * decompressor implements; written from scratch here from the public description, not copied).
 * PVRTC-II keeps the 64-bit block (modulation word, mode bit 32, two colours) but re-reads two
 * bits: bit 63 is the ONE opaque flag for both colours (v1 gives A its own at bit 47) and bit 47
 * is the block's hard-transition flag -- set, the 4x4 texels of the cell whose top-left block it
 * is are not bilinear ramps between the four blocks but each quadrant's own block colour (mode
 * bit 0) or a per-texel local palette of the four blocks' colours (mode bit 1); a translucent
 * colour B's 3-bit alpha widens with a 1 so it reaches opaque. Decoded as v1, Uncharted's
 * PVRTC-II character textures (all blocks opaque, bit 47 set in the 5-10% hard blocks) read
 * colour A as a translucent ARGB3443 in 90% of blocks -- the "face" was a red-green swirl -- and
 * the characters came out magenta/blue off the G-buffer. The flag bits were confirmed on those
 * textures' block statistics (bit 63 set in every block of an opaque texture, bit 47 in a few);
 * the mode-1 hard-block palette isn't implemented (see pvrtc_cell_texel); borders wrap as in v1. */
#define PVRT_MAX2(a, b) ((a) > (b) ? (a) : (b))
typedef struct { uint32_t lo, hi; } PvrtcBlock;
/* block-granularity fetch, reusing the existing swizzled/tiled/linear addressing at the block's
 * own grid resolution (bw x bh blocks) -- the same trick already used for UBC1/2/3 above. */
static PvrtcBlock pvrtc_block_at(const GfxTexture *t, int bx, int by, int bw, int bh) {
	GfxTexture bt = *t; bt.width = (uint16_t)bw; bt.height = (uint16_t)bh;
	uint32_t idx = texel_index(&bt, (uint32_t)bx, (uint32_t)by);
	PvrtcBlock b; b.lo = RD32(t->data + 8u * idx); b.hi = RD32(t->data + 8u * idx + 4);
	return b;
}
static void pvrtc_shiftl(uint16_t *x, int n, int *out) { *out = *x >> (16 - n); *x = (uint16_t)(*x << n); }
static int pvrtc_rep(int x) { return x | (x >> 4); }   /* widen a 4-bit field to look like 5 bits */
/* unpack ColourA (which=0) or ColourB (which=1) from the block's high word into a 5554-ish (R,G,B,A).
 * v1: each colour carries its own opaque flag (bit 15 of the word for A, bit 31 for B). PVRTC-II
 * (is2): bit 31 is the one opaque flag for both and bit 15 is the hard-transition flag, not a
 * colour bit (it stays masked off); a translucent B's 3-bit alpha widens with a 1, A's with a 0. */
static void pvrtc_unpack_color(uint32_t hi, int which, int is2, int out[4]) {
	uint16_t raw = which == 0 ? (uint16_t)(hi & 0xFFFEu) : (uint16_t)(hi >> 16);
	int opaque; pvrtc_shiftl(&raw, 1, &opaque);
	if (is2) opaque = (int)(hi >> 31);
	if (opaque) {
		int r, g, b;
		pvrtc_shiftl(&raw, 5, &r); pvrtc_shiftl(&raw, 5, &g); pvrtc_shiftl(&raw, 5, &b);
		out[0] = r; out[1] = g; out[2] = which == 0 ? pvrtc_rep(b) : b; out[3] = 0xF;
	} else {
		int a, r, g, b;
		pvrtc_shiftl(&raw, 3, &a); a = (a << 1) | (is2 && which == 1);
		pvrtc_shiftl(&raw, 4, &r); r = pvrtc_rep(r << 1);
		pvrtc_shiftl(&raw, 4, &g); g = pvrtc_rep(g << 1);
		pvrtc_shiftl(&raw, 4, &b); b <<= 1;
		out[0] = r; out[1] = g; out[2] = which == 0 ? (b | (b >> 3)) : pvrtc_rep(b); out[3] = a;
	}
}
/* bilinear-interpolate one of the two low-res colour images (P,Q top row; R,S bottom row are the
 * four neighbouring blocks' colours) at (x,y)'s position within its 2x2 block neighbourhood */
static void pvrtc_interp(const int P[4], const int Q[4], const int R[4], const int S[4], int is2bpp, int x, int y, int out[4]) {
	int v = (y & 3) | ((~y & 2) << 1); v -= 2;
	int u = is2bpp ? ((x & 7) | ((~x & 4) << 1)) : ((x & 3) | ((~x & 2) << 1));
	int uscale = is2bpp ? 8 : 4; u -= is2bpp ? 4 : 2;
	for (int k = 0; k < 4; k++) {
		int t1 = P[k] * uscale + u * (Q[k] - P[k]);
		int t2 = R[k] * uscale + u * (S[k] - R[k]);
		out[k] = t1 * 4 + v * (t2 - t1);
	}
	if (is2bpp) { for (int k = 0; k < 3; k++) out[k] >>= 2; out[3] >>= 1; }
	else for (int k = 0; k < 3; k++) out[k] >>= 1;
	for (int k = 0; k < 3; k++) out[k] += out[k] >> 5;
	out[3] += out[3] >> 4;
}
/* the per-texel modulation weight (numerator of eighths) and punch-through flag, from the 2x2
 * block neighbourhood's four 32-bit modulation words (lo[row][col], row/col matching P,Q/R,S);
 * also which of the four blocks the texel lies in (q[0] row, q[1] column) and its stored 2-bit
 * value (4bpp; the PVRTC-II hard-transition path indexes a palette by it) */
static void pvrtc_modulation(const uint32_t lo[2][2], const uint32_t modebit[2][2], int is2bpp, int x, int y, int *mod, int *punch, int q[2], int *raw) {
	static const int rep0[4] = {0, 3, 5, 8}, rep1[4] = {0, 4, 4, 8};
	int ly = (y & 3) | ((~y & 2) << 1);
	int lx = is2bpp ? ((x & 7) | ((~x & 4) << 1)) : ((x & 3) | ((~x & 2) << 1));
	int bw = is2bpp ? 8 : 4;   /* a 2x2 neighbourhood of blocks is bw*2 wide, 8 tall */
	*punch = 0; *raw = 0;
	if (!is2bpp) {   /* 4bpp: each texel has its own 2-bit value, no cross-block averaging */
		int by = ly / 4, bx = lx / 4, ty = ly % 4, tx = lx % 4;
		int v = (int)((lo[by][bx] >> (2 * (ty * 4 + tx))) & 3);
		q[0] = by; q[1] = bx; *raw = v;
		/* the block's mode bit: 0 = weights 0, 3/8, 5/8, 1; 1 = punch-through mode, 0, 4/8,
		 * 4/8 with alpha forced to 0, 1 (Fenney's two 4bpp modes). Every block used to be read
		 * as punch-through: a mode-0 texture lost a quarter of its texels to alpha 0. */
		if (modebit[by][bx]) { *mod = rep1[v]; *punch = (v == 2); }
		else *mod = rep0[v];
		return;
	}
	/* 2bpp, as Imagination's reference decoder reads it (PVRTDecompress unpackModulations / getModulationValues): bit 0 of
	 * the block's high word (modebit) picks direct -- one bit a texel, 0 or the full weight 8/8 -- or interpolated: 2-bit
	 * values stored on a checkerboard (in raster order, two bits each) and the other texels the average of their stored
	 * neighbours, H and V (mode 1), H only (2) or V only (3). Bit 0 of the modulation word marks H/V-only, the centre
	 * texel's bit 20 then tells V from H and takes bit 21's value; bit 0 itself takes bit 1's. A neighbour in a direct
	 * block counts with its own bit (0 or 3). This read a set direct bit as 3/8 and never decoded H/V-only blocks:
	 * Killzone's 2bpp directional lightmaps (unit 4 of its level shaders) came out in the wrong directions, the lit
	 * rubble and broken walls blue-purple. */
	int by = ly / 4, bx = lx / bw, ty = ly % 4, tx = lx % bw;
	q[0] = by; q[1] = bx;
	uint32_t bits[2][2]; int mm[2][2];
	for (int r = 0; r < 2; r++) for (int k = 0; k < 2; k++) {
		uint32_t b = lo[r][k]; int m = (int)modebit[r][k];
		if (m) {
			if (b & 1u) { m = (b & (1u << 20)) ? 3 : 2; b = (b & (1u << 21)) ? (b | (1u << 20)) : (b & ~(1u << 20)); }
			b = (b & 2u) ? (b | 1u) : (b & ~1u);
		}
		bits[r][k] = b; mm[r][k] = m;
	}
	#define MODV(byy, bxx, tyy, txx) (mm[byy][bxx] ? (int)((bits[byy][bxx] >> (((tyy) * bw + (txx)) / 2 * 2)) & 3) : (int)((bits[byy][bxx] >> ((tyy) * bw + (txx))) & 1) * 3)
	int mode = mm[by][bx];
	if (!mode || ((tx ^ ty) & 1) == 0) { *mod = rep0[MODV(by, bx, ty, tx)]; *raw = MODV(by, bx, ty, tx); return; }
	/* an unstored texel: its neighbours, which may lie in the next block of the cell's 2x2 (the cell's texels sit at
	 * lx 4..11, ly 2..5, so a neighbour never leaves the four blocks) */
	int lb = tx > 0 ? bx : bx - 1, lt = tx > 0 ? tx - 1 : bw - 1, rb = tx + 1 < bw ? bx : bx + 1, rt = tx + 1 < bw ? tx + 1 : 0;
	int ub = ty > 0 ? by : by - 1, ut = ty > 0 ? ty - 1 : 3, db = ty + 1 < 4 ? by : by + 1, dt = ty + 1 < 4 ? ty + 1 : 0;
	int left = rep0[MODV(by, lb, ty, lt)], right = rep0[MODV(by, rb, ty, rt)], up = rep0[MODV(ub, bx, ut, tx)], down = rep0[MODV(db, bx, dt, tx)];
	if (mode == 1) *mod = (left + right + up + down + 2) / 4;
	else if (mode == 2) *mod = (left + right + 1) / 2;
	else *mod = (up + down + 1) / 2;
	#undef MODV
}
/* a PVRTC level's block grid (at least 2x2 blocks) and the texel image it decodes to (pw x ph:
 * the grid's size in texels, which is the texture's own size except for levels under 2x2 blocks) */
typedef struct { int is2bpp, is2, bw, bh, pw, ph, sx; } PvrtcGeom;   /* is2: PVRTC-II (PVRTII2BPP/PVRTII4BPP) */
static PvrtcGeom pvrtc_geom(const GfxTexture *t, int is2bpp) {
	PvrtcGeom g; g.is2bpp = is2bpp; g.sx = is2bpp ? 8 : 4;
	g.is2 = (t->format & 0xff000000u) == 0x82000000u || (t->format & 0xff000000u) == 0x83000000u;
	g.bw = PVRT_MAX2(2, (int)(t->width ? t->width : 1) / g.sx);
	g.bh = PVRT_MAX2(2, (int)(t->height ? t->height : 1) / 4);
	g.pw = g.bw * g.sx; g.ph = g.bh * 4;
	return g;
}
/* the four blocks of one bilinear cell (top-left block bx0,by0 and its right/lower neighbours,
 * wrapped), unpacked once: every texel of the cell is decoded from exactly these */
typedef struct { uint32_t los[2][2], modebit[2][2]; int A[2][2][4], B[2][2][4]; int hard; } PvrtcCell;   /* hard: PVRTC-II, the top-left block's bit 47 */
static void pvrtc_gather(const GfxTexture *t, const PvrtcGeom *g, int bx0, int by0, PvrtcCell *c) {
	int bx[2] = {bx0, (bx0 + 1) % g->bw}, by[2] = {by0, (by0 + 1) % g->bh};
	c->hard = 0;
	for (int r = 0; r < 2; r++) for (int k = 0; k < 2; k++) {
		PvrtcBlock blk = pvrtc_block_at(t, bx[k], by[r], g->bw, g->bh);
		c->los[r][k] = blk.lo; c->modebit[r][k] = blk.hi & 1;
		if (g->is2 && !r && !k) c->hard = (int)((blk.hi >> 15) & 1);
		pvrtc_unpack_color(blk.hi, 0, g->is2, c->A[r][k]); pvrtc_unpack_color(blk.hi, 1, g->is2, c->B[r][k]);
	}
}
/* a block colour on its own, widened to 0..255 the way pvrtc_interp widens a ramp value (5-bit
 * RGB: c*8 + c/4; 4-bit A: c*17) -- the PVRTC-II hard-transition path uses the blocks' colours
 * unramped */
static void pvrtc_widen(const int c[4], int out[4]) { for (int k = 0; k < 3; k++) out[k] = (c[k] << 3) | (c[k] >> 2); out[3] = (c[3] << 4) | c[3]; }
/* texel (px,py) of the cell it lies in: 0..255 per channel (pvrtc_interp's widening tops out at
 * exactly 255, and the modulation blends between two such values) */
static void pvrtc_cell_texel(const PvrtcCell *c, int is2bpp, int px, int py, int res[4]) {
	int ca[4], cb[4], mod, punch, q[2], raw;
	pvrtc_modulation(c->los, c->modebit, is2bpp, px, py, &mod, &punch, q, &raw);
	if (c->hard) {
		/* PVRTC-II hard transition: the cell's texels take their own block's two colours with no
		 * ramp to the neighbours, weighted 0, 3/8, 5/8, 1 (no punch-through here). A 4bpp mode-1
		 * hard block selects, per texel, from a small palette over the four blocks' colours instead;
		 * that table isn't publicly documented (Imagination's SDK decoder has no PVRTC-II) and no
		 * capture of ours has such a block, so it takes the same unramped path -- approximate. */
		pvrtc_widen(c->A[q[0]][q[1]], ca); pvrtc_widen(c->B[q[0]][q[1]], cb);
		for (int k = 0; k < 4; k++) res[k] = ca[k] + ((mod * (cb[k] - ca[k])) >> 3);
		return;
	}
	pvrtc_interp(c->A[0][0], c->A[0][1], c->A[1][0], c->A[1][1], is2bpp, px, py, ca);
	pvrtc_interp(c->B[0][0], c->B[0][1], c->B[1][0], c->B[1][1], is2bpp, px, py, cb);
	for (int k = 0; k < 4; k++) res[k] = ca[k] + ((mod * (cb[k] - ca[k])) >> 3);
	if (punch) res[3] = 0;
}
/* one texel, from scratch: each texel sits in the bilinear cell between the 2x2 blocks straddling
 * it, found by walking back half a block first (Euclidean mod/div so this is correct for every
 * wrapped px, py) */
static void pvrtc_decode_texel(const GfxTexture *t, const PvrtcGeom *g, int px, int py, int res[4]) {
	int bx0 = (((px - g->sx / 2) % g->pw + g->pw) / g->sx) % g->bw;
	int by0 = (((py - 2) % g->ph + g->ph) / 4) % g->bh;
	PvrtcCell c; pvrtc_gather(t, g, bx0, by0, &c);
	pvrtc_cell_texel(&c, g->is2bpp, px, py, res);
}
/* the whole level into pw*ph RGBA bytes, cell by cell: the four blocks are fetched (Morton
 * addressing and all) and unpacked once per cell rather than once per texel, and every texel is
 * produced by the very pvrtc_cell_texel call pvrtc_decode_texel makes for it -- cell (bx0,by0)
 * covers px = bx0*sx + sx/2 + 0..sx-1 and py = by0*4 + 2 + 0..3 (mod the image), precisely the
 * texels pvrtc_decode_texel's walk-back maps to that cell. Returns 0 if a channel left 0..255
 * (cannot happen by the arithmetic above; checked so a byte image can never differ from the
 * per-texel path). */
static int pvrtc_decode_rows(const GfxTexture *t, const PvrtcGeom *g, uint8_t *out, int by_lo, int by_hi) {   /* cell rows [by_lo, by_hi): every texel is written by exactly one cell, so row ranges can run at once */
	int ok = 1;
	for (int by0 = by_lo; by0 < by_hi; by0++) for (int bx0 = 0; bx0 < g->bw; bx0++) {
		PvrtcCell c; pvrtc_gather(t, g, bx0, by0, &c);
		for (int dy = 0; dy < 4; dy++) {
			int py = (by0 * 4 + 2 + dy) % g->ph;
			for (int dx = 0; dx < g->sx; dx++) {
				int px = (bx0 * g->sx + g->sx / 2 + dx) % g->pw, res[4];
				pvrtc_cell_texel(&c, g->is2bpp, px, py, res);
				uint8_t *p = out + 4 * ((size_t)py * (size_t)g->pw + (size_t)px);
				for (int k = 0; k < 4; k++) { ok &= res[k] >= 0 && res[k] <= 255; p[k] = (uint8_t)res[k]; }
			}
		}
	}
	return ok;
}
static int pvrtc_decode_image(const GfxTexture *t, const PvrtcGeom *g, uint8_t *out) { return pvrtc_decode_rows(t, g, out, 0, g->bh); }
/* a small pool for one caller's whole-image decodes -- the GPU backend's texture uploads, on the render thread
 * (gfx_decode_texture_rgba8): a 512x512 PVRTC-II level took ~9.5 ms on one core of the Linux machine, a dropped
 * frame for every texture Uncharted streams in (112 in 94 s of play). Never used from the rasteriser bands
 * (their own texcache decodes stay serial: no pool inside a pool). Chunks are handed out by an atomic counter;
 * the caller works too. RECOMP_DECODE_THREADS=n: n workers (0: serial), default min(8, cores - 1). */
#define DEC_MAX 16
static int dec_n = -1;   /* workers: -1 not started */
static pthread_mutex_t dec_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dec_go = PTHREAD_COND_INITIALIZER, dec_done = PTHREAD_COND_INITIALIZER;
static uint64_t dec_gen; static int dec_busy, dec_chunks, dec_next;
static void (*dec_fn)(void *arg, int chunk); static void *dec_arg;
static void dec_drain(void) { int c; while ((c = __atomic_fetch_add(&dec_next, 1, __ATOMIC_ACQ_REL)) < dec_chunks) dec_fn(dec_arg, c); }
static void *dec_worker(void *unused) {
	(void)unused; uint64_t seen = 0;
	for (;;) {
		pthread_mutex_lock(&dec_m);
		while (dec_gen == seen) pthread_cond_wait(&dec_go, &dec_m);
		seen = dec_gen; pthread_mutex_unlock(&dec_m);
		dec_drain();
		pthread_mutex_lock(&dec_m); if (--dec_busy == 0) pthread_cond_signal(&dec_done); pthread_mutex_unlock(&dec_m);
	}
	return 0;
}
static void dec_parallel(int chunks, void (*fn)(void *, int), void *arg) {
	if (dec_n < 0) {
		const char *e = getenv("RECOMP_DECODE_THREADS"); long nc = plat_cpu_count();
		dec_n = e ? atoi(e) : (int)(nc > 1 ? (nc - 1 < 8 ? nc - 1 : 8) : 0); if (dec_n > DEC_MAX) dec_n = DEC_MAX;
		for (int i = 0; i < dec_n; i++) { pthread_t th; if (pthread_create(&th, 0, dec_worker, 0)) { dec_n = i; break; } pthread_detach(th); }
	}
	if (dec_n <= 0 || chunks < 2) { for (int c = 0; c < chunks; c++) fn(arg, c); return; }
	static pthread_mutex_t call_m = PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&call_m);   /* (one job at a time: a second caller waits its turn) */
	pthread_mutex_lock(&dec_m);
	dec_fn = fn; dec_arg = arg; dec_chunks = chunks; __atomic_store_n(&dec_next, 0, __ATOMIC_RELEASE); dec_busy = dec_n; dec_gen++;
	pthread_cond_broadcast(&dec_go); pthread_mutex_unlock(&dec_m);
	dec_drain();
	pthread_mutex_lock(&dec_m); while (dec_busy) pthread_cond_wait(&dec_done, &dec_m); pthread_mutex_unlock(&dec_m);
	pthread_mutex_unlock(&call_m);
}
typedef struct { const GfxTexture *t; const PvrtcGeom *g; uint8_t *out; int rows, ok; } PvrtcJob;
static void pvrtc_job(void *arg, int chunk) {
	PvrtcJob *j = arg; int lo = chunk * j->rows, hi = lo + j->rows < j->g->bh ? lo + j->rows : j->g->bh;
	if (!pvrtc_decode_rows(j->t, j->g, j->out, lo, hi)) __atomic_store_n(&j->ok, 0, __ATOMIC_RELAXED);
}
static int pvrtc_decode_image_par(const GfxTexture *t, const PvrtcGeom *g, uint8_t *out) {
	PvrtcJob j = { t, g, out, 4, 1 };   /* 4 cell rows (16 texel rows) a chunk */
	if ((size_t)g->bw * (size_t)g->bh < 1024) return pvrtc_decode_image(t, g, out);   /* (a small level: not worth waking anyone) */
	dec_parallel((g->bh + j.rows - 1) / j.rows, pvrtc_job, &j);
	return j.ok;
}

/* ---- decoded-texture cache (PVRTC). Decoding PVRTC per sampled texel costs four block fetches
 * (each a Morton-order texel_index walk), eight colour unpacks and two bilinear interpolations:
 * Uncharted's PVRTC loading screen took ~10 s a frame on the interpreter with every rasteriser
 * worker inside pvrtc_texel. Each PVRTC level a flush samples is decoded once to RGBA8 instead,
 * and a sample reads its texel from that image -- the same 0..255 integers pvrtc_texel divided by
 * 255, so filtering, wrap and mip selection are untouched and the result is bit-identical
 * (RECOMP_SOFT_TEXCACHE_CHECK=1 re-decodes every cached texel the old way and compares). Keyed as
 * gfx_gpu.c's texture cache is: address, format, size, layout -- one entry per mip level, since a
 * level is its own GfxTexture (mip_view) -- plus a hash of the compressed bytes the level reads,
 * taken the first time a flush samples it (the guest may rewrite a texture in place between
 * scenes; a replay's memory records do), so a changed texture is decoded again. Nothing is done
 * for a level until a fragment samples it: Unit 13 binds stale descriptors that read as PVRTC
 * (8459x33033 at a garbage address) in units its shaders never sample, and hashing those up
 * front cost more than its whole frame's texturing.
 * Threads: the rasteriser bands look entries up and decode; tc_m guards the table, the LRU and
 * the byte count, an entry's own lock its verify-and-decode. An entry touched this flush (used ==
 * tc_gen) is never evicted or freed before the flush ends, so a band can hold its image unlocked.
 * env: RECOMP_SOFT_NO_TEXCACHE=1   off (every PVRTC texel decoded per sample, as before; for A/B)
 *      RECOMP_SOFT_TEXCACHE_MB=n   memory cap for decoded images (default 256 MB): to make room,
 *                                  the least recently sampled images this flush hasn't touched go
 *                                  first; a level that still doesn't fit is sampled the old way
 *      RECOMP_SOFT_TEXCACHE_CHECK=1 verify every decoded image texel by texel against
 *                                  pvrtc_decode_texel (slow; mismatches are printed)
 *      RECOMP_SOFT_TEXCACHE_LOG=1  one line per image decoded (key, size, milliseconds) */
typedef struct {
	uint32_t addr, format; uint16_t width, height; uint8_t layout;   /* key: the level's GfxTexture */
	uint64_t used;           /* the flush (tc_gen) that last touched it (tc_m) */
	uint64_t ok, failed;     /* the flush in which img was verified current / in which it couldn't be cached (atomic) */
	uint64_t hash;           /* of the compressed bytes img was decoded from */
	int next;                /* bucket chain (index into tc, -1 ends) */
	pthread_mutex_t lock;    /* held while one band verifies and decodes */
	uint8_t *img;            /* pw*ph RGBA bytes, or 0 */
	size_t bytes;            /* img's size, counted in tc_bytes */
} TcEntry;
#define TC_MAX 4096
#define TC_BUCKETS 8192
static TcEntry tc[TC_MAX];
static int tc_bucket[TC_BUCKETS], tc_n, tc_inited, tc_off, tc_check, tc_log;
static uint64_t tc_gen;          /* the current flush (tc_prepare bumps it, between flushes) */
static size_t tc_bytes, tc_cap;  /* decoded bytes held (tc_m), the cap */
static pthread_mutex_t tc_m = PTHREAD_MUTEX_INITIALIZER;
static __thread int tc_active;   /* this thread is a rasteriser band of a flush */
static uint64_t now_ns(void);
static uint32_t tc_hash_key(uint32_t addr, uint32_t format, uint32_t w, uint32_t h, uint32_t layout) {
	uint32_t k = addr * 0x9E3779B1u ^ format * 0x85EBCA77u ^ (w << 16 | h) * 0xC2B2AE3Du ^ layout;
	return (k ^ (k >> 15)) & (TC_BUCKETS - 1);
}
static int tc_key_eq(const TcEntry *e, const GfxTexture *t) {
	return e->addr == t->data && e->format == t->format && e->width == t->width && e->height == t->height && e->layout == ((t->u_mode >> 4) & 3);
}
static uint64_t tc_hash_bytes(const uint8_t *b, size_t n) {   /* gfx_gpu.c's word-at-a-time FNV, kept 64-bit */
	uint64_t h = 0x9E3779B97F4A7C15ull; size_t i = 0;
	for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 0x100000001B3ull; h ^= h >> 29; }
	for (; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
	return h;
}
static void tc_drop_image(TcEntry *e) { free(e->img); tc_bytes -= e->bytes; e->img = 0; e->bytes = 0; }   /* tc_m held */
/* the entry for a level, created if new, marked touched by this flush (tc_m held). 0 only when
 * every one of TC_MAX entries is in use by this very flush. */
static TcEntry *tc_touch(const GfxTexture *t) {
	uint32_t layout = (t->u_mode >> 4) & 3, b = tc_hash_key(t->data, t->format, t->width, t->height, layout);
	for (int i = tc_bucket[b]; i >= 0; i = tc[i].next) if (tc_key_eq(&tc[i], t)) { tc[i].used = tc_gen; return &tc[i]; }
	int i;
	if (tc_n < TC_MAX) { i = tc_n++; pthread_mutex_init(&tc[i].lock, 0); }
	else {   /* full: the least recently touched entry not touched by this flush is reused */
		i = -1;
		for (int k = 0; k < TC_MAX; k++) if (tc[k].used != tc_gen && (i < 0 || tc[k].used < tc[i].used)) i = k;
		if (i < 0) return 0;
		TcEntry *old = &tc[i];
		tc_drop_image(old);
		int *pp = &tc_bucket[tc_hash_key(old->addr, old->format, old->width, old->height, old->layout)];
		while (*pp != i) pp = &tc[*pp].next;
		*pp = old->next;
	}
	TcEntry *e = &tc[i];
	e->addr = t->data; e->format = t->format; e->width = t->width; e->height = t->height; e->layout = (uint8_t)layout;
	e->used = tc_gen; e->ok = e->failed = 0; e->hash = 0; e->img = 0; e->bytes = 0;
	e->next = tc_bucket[b]; tc_bucket[b] = i;
	return e;
}
/* room for `need` more decoded bytes under the cap, freeing images no band of this flush has
 * touched (least recently used first); reserves it on success (tc_m held) */
static int tc_reserve(size_t need) {
	while (tc_bytes + need > tc_cap) {
		TcEntry *lru = 0;
		for (int k = 0; k < tc_n; k++) if (tc[k].img && tc[k].used != tc_gen && (!lru || tc[k].used < lru->used)) lru = &tc[k];
		if (!lru) return 0;
		tc_drop_image(lru);
	}
	tc_bytes += need;
	return 1;
}
/* the first sample of a level in this flush (e->lock held): hash what it reads now, decode again
 * if that changed or it has no image yet. Returns whether img is usable for this flush. */
static int tc_verify(TcEntry *e, const GfxTexture *t, const PvrtcGeom *g) {
	size_t need = 4 * (size_t)g->pw * (size_t)g->ph;
	if (need > tc_cap) return 0;   /* before hashing anything: it could never be cached */
	/* the bytes the level's blocks occupy: pvrtc_block_at's addressing at the grid's far corner
	 * (every layout's index grows with x and with y) */
	GfxTexture bt = *t; bt.width = (uint16_t)g->bw; bt.height = (uint16_t)g->bh;
	uint64_t span = 8ull * ((uint64_t)texel_index(&bt, (uint32_t)g->bw - 1, (uint32_t)g->bh - 1) + 1);
	if ((uint64_t)t->data + span > (1ull << 32)) return 0;   /* runs off the arena: the per-texel path's wrapped reads stay */
	uint64_t h = tc_hash_bytes(G(t->data), (size_t)span);
	if (e->img && e->hash == h) return 1;
	pthread_mutex_lock(&tc_m);
	if (e->img) { if (tc_log) fprintf(stderr, "    soft texcache: %#x fmt %#x %ux%u changed in place, decoding again\n", t->data, t->format, t->width, t->height); tc_drop_image(e); }
	int room = tc_reserve(need);
	pthread_mutex_unlock(&tc_m);
	if (!room) return 0;
	uint8_t *img = malloc(need);
	uint64_t t0 = tc_log ? now_ns() : 0;
	if (img && !pvrtc_decode_image(t, g, img)) { fprintf(stderr, "    soft texcache: %#x fmt %#x %ux%u decoded out of range, not cached\n", t->data, t->format, t->width, t->height); free(img); img = 0; }
	if (img && tc_log) fprintf(stderr, "    soft texcache: decoded %#x fmt %#x %ux%u layout %u (%dx%d texels, %zu KB) in %.2f ms\n", t->data, t->format, t->width, t->height, (t->u_mode >> 4) & 3, g->pw, g->ph, need >> 10, (now_ns() - t0) / 1e6);
	if (img && tc_check) {
		uint32_t bad = 0;
		for (int py = 0; py < g->ph; py++) for (int px = 0; px < g->pw; px++) {
			int res[4]; pvrtc_decode_texel(t, g, px, py, res);
			const uint8_t *p = img + 4 * ((size_t)py * (size_t)g->pw + (size_t)px);
			for (int k = 0; k < 4; k++) if (p[k] != res[k] && bad++ < 4) fprintf(stderr, "    soft texcache CHECK: %#x fmt %#x %ux%u texel %d,%d ch %d: cached %u, per-texel %d\n", t->data, t->format, t->width, t->height, px, py, k, p[k], res[k]);
		}
		fprintf(stderr, "    soft texcache check: %#x fmt %#x %ux%u (%dx%d texels): %u mismatches\n", t->data, t->format, t->width, t->height, g->pw, g->ph, bad);
	}
	pthread_mutex_lock(&tc_m);
	if (img) { e->img = img; e->bytes = need; e->hash = h; } else tc_bytes -= need;
	pthread_mutex_unlock(&tc_m);
	return img != 0;
}
/* the decoded image of a level for this flush, verifying/decoding it if no band has yet; 0 =
 * sample the old way. A small per-thread most-recently-used list in front of the table: a band
 * shades one draw's triangles at a time, so the same one to four levels (two for a trilinear
 * blend, one per texture unit) answer nearly every lookup without taking a lock. */
static __thread struct { TcEntry *e; uint64_t gen; } tc_mru[4];
static __thread int tc_mru_next;
static const uint8_t *tc_pvrtc_image(const GfxTexture *t, const PvrtcGeom *g) {
	TcEntry *e = 0;
	for (int i = 0; i < 4; i++) if (tc_mru[i].gen == tc_gen && tc_key_eq(tc_mru[i].e, t)) { e = tc_mru[i].e; break; }
	if (!e) {
		pthread_mutex_lock(&tc_m); e = tc_touch(t); pthread_mutex_unlock(&tc_m);
		if (!e) return 0;
		tc_mru[tc_mru_next].e = e; tc_mru[tc_mru_next].gen = tc_gen; tc_mru_next = (tc_mru_next + 1) & 3;
	}
	if (__atomic_load_n(&e->ok, __ATOMIC_ACQUIRE) == tc_gen) return e->img;
	if (__atomic_load_n(&e->failed, __ATOMIC_RELAXED) == tc_gen) return 0;
	pthread_mutex_lock(&e->lock);
	if (e->ok != tc_gen && e->failed != tc_gen) {
		if (tc_verify(e, t, g)) __atomic_store_n(&e->ok, tc_gen, __ATOMIC_RELEASE);
		else __atomic_store_n(&e->failed, tc_gen, __ATOMIC_RELAXED);
	}
	pthread_mutex_unlock(&e->lock);
	return e->ok == tc_gen ? e->img : 0;
}
static void pvrtc_texel(const GfxTexture *t, int is2bpp, float u, float v, float out[4]) {
	PvrtcGeom g = pvrtc_geom(t, is2bpp);
	int px = (int)(u * g.pw), py = (int)(v * g.ph);
	px = ((px % g.pw) + g.pw) % g.pw; py = ((py % g.ph) + g.ph) % g.ph;   /* PVRTC textures always wrap */
	int res[4];
	const uint8_t *img = tc_active ? tc_pvrtc_image(t, &g) : 0;
	if (img) { const uint8_t *p = img + 4 * ((size_t)py * (size_t)g.pw + (size_t)px); res[0] = p[0]; res[1] = p[1]; res[2] = p[2]; res[3] = p[3]; }
	else pvrtc_decode_texel(t, &g, px, py, res);
	out[0] = res[0] / 255.f; out[1] = res[1] / 255.f; out[2] = res[2] / 255.f; out[3] = res[3] / 255.f;
}
/* the format's component swizzle (bits 12-14), named most significant first as GXM does: for four
 * components the stored c0..c3 (lowest bits first) are ABGR = R c0, G c1, B c2, A c3 by default;
 * one component (U8...) can be R, 000R, 111R, RRRR, 0RRR, 1RRR, R000, R111 */
static void apply_swizzle(uint32_t format, int ncomp, float o[4]) {
	uint32_t sw = (format >> 12) & 7;
	float c0 = o[0], c1 = o[1], c2 = o[2], c3 = o[3];
	if (ncomp == 4) switch (sw) {
		case 0: break;                                                        /* ABGR */
		case 1: o[0] = c2; o[1] = c1; o[2] = c0; o[3] = c3; break;            /* ARGB */
		case 2: o[0] = c3; o[1] = c2; o[2] = c1; o[3] = c0; break;            /* RGBA */
		case 3: o[0] = c1; o[1] = c2; o[2] = c3; o[3] = c0; break;            /* BGRA */
		case 4: o[3] = 1; break;                                              /* 1BGR */
		case 5: o[0] = c2; o[1] = c1; o[2] = c0; o[3] = 1; break;             /* 1RGB */
		case 6: o[0] = c3; o[1] = c2; o[2] = c1; o[3] = 1; break;             /* RGB1 */
		case 7: o[0] = c1; o[1] = c2; o[2] = c3; o[3] = 1; break;             /* BGR1 */
	} else if (ncomp == 3) switch (sw) {   /* SWIZZLE3 (U8U8U8 / S8S8S8): bytes c0 c1 c2, the 4-component modes' pattern without alpha */
		case 0: o[3] = 1; break;                                              /* BGR: c0 is R, as ABGR's */
		case 1: o[0] = c2; o[1] = c1; o[2] = c0; o[3] = 1; break;             /* RGB */
		default: o[3] = 1; break;
	} else if (ncomp == 1) {
		float r = c0;
		static const float k[8][4] = {{-1, 0, 0, 1}, {-1, 0, 0, 0}, {-1, 1, 1, 1}, {-1, -1, -1, -1}, {-1, -1, -1, 0}, {-1, -1, -1, 1}, {0, 0, 0, -1}, {1, 1, 1, -1}};
		for (int i = 0; i < 4; i++) o[i] = k[sw][i] < 0 ? r : k[sw][i];
	}
}
/* which swizzle family a texture base format's components take (1: the SWIZZLE1 modes, 4: the
 * SWIZZLE4 ones), 0 for formats whose components are placed by the decode itself */
static int swizzle_ncomp(uint32_t base) {
	return base == 0x00000000u || base == 0x01000000u || base == 0x09000000u || base == 0x0a000000u || base == 0x0b000000u || base == 0x12000000u || base == 0x88000000u ? 1
	     : base == 0x0c000000u || base == 0x0d000000u || base == 0x1b000000u || base == 0x02000000u || base == 0x04000000u || (base >= 0x85000000u && base <= 0x87000000u) ? 4
	     : base == 0x98000000u || base == 0x99000000u ? 3 : 0;
}
static float small_float(uint32_t v, int mbits);
static void texel_raw(const GfxTexture *t, float u, float v, float out[4]);
static void tex_gamma(const GfxTexture *t, float out[4]);   /* below */
/* ---- mip levels and filtering. GfxTexture.v_mode's high nibble is the level count (1 = no
 * mips). Level l is max(1, w>>l) x max(1, h>>l) texels and follows the previous levels in memory;
 * a level's size follows the layout's padding (as Vita3K established from the hardware: swizzled
 * levels are power-of-two, tiled ones 32-aligned, linear ones 8-texel-aligned rows; block formats
 * count whole blocks, PVRTC at least 2x2 of them). Strided-linear and cube textures sample level 0. */
static uint32_t tex_bytes_per_texel(uint32_t base);   /* below */
static int mip_levels(const GfxTexture *t) { int n = (t->v_mode >> 4) & 15; return n ? n : 1; }
static void block_dims(uint32_t base, int *bw, int *bh, int *bytes, int *minblocks) {
	*minblocks = 1;
	switch (base) {
	case 0x80000000u: case 0x82000000u: *bw = 8; *bh = 4; *bytes = 8; *minblocks = 2; return;   /* PVRT2BPP / PVRTII2BPP */
	case 0x81000000u: case 0x83000000u: *bw = 4; *bh = 4; *bytes = 8; *minblocks = 2; return;   /* PVRT4BPP / PVRTII4BPP */
	case 0x85000000u: case 0x88000000u: *bw = 4; *bh = 4; *bytes = 8; return;                 /* UBC1 / UBC4 */
	case 0x86000000u: case 0x87000000u: *bw = 4; *bh = 4; *bytes = 16; return;                 /* UBC2 / UBC3 */
	case 0x90000000u: case 0x91000000u: *bw = 2; *bh = 2; *bytes = 6; return;                  /* YUV420P2 / P3: 12 bits a texel (yuv_texel) */
	case 0x92000000u: *bw = 1; *bh = 1; *bytes = 2; return;                                     /* YUV422 */
	default: *bw = *bh = 1; *bytes = (int)tex_bytes_per_texel(base); if (!*bytes) *bytes = 4; return;
	}
}
/* the bytes level `level` reserves. A texture with mip levels lays them out at power-of-two sizes whatever its layout --
 * a linear or tiled level's rows keep the real width (aligned to 8 / 32), but the level reserves the rounded-up area, as
 * Vita3K's texture cache reads them. (A single level keeps its real size: tex_span, the change-detection extent.)
 * Linear levels were taken at the real size: Killzone's 384x64 reprojection and streak strips (5 levels, a 512x64 first
 * level per the game's own sceGxmTransferDownscale chain, level 1 at +0x20000) had every level past 0 read from the
 * padding after level 0 -- what its glass refracted and reflected at any distance (2026-10-17). */
static uint32_t level_bytes(const GfxTexture *t, int level) {
	uint32_t base = t->format & 0xff000000u; int layout = (t->u_mode >> 4) & 3;
	uint32_t w = t->width, h = t->height;
	if (layout == 1 || ((t->v_mode >> 4) & 15) > 1) { uint32_t p = 1; while (p < w) p <<= 1; w = p; p = 1; while (p < h) p <<= 1; h = p; }
	w >>= level; h >>= level; if (!w) w = 1; if (!h) h = 1;
	if (layout == 2) { w = (w + 31) & ~31u; h = (h + 31) & ~31u; }
	else if (layout == 0) w = (w + 7) & ~7u;
	int bw, bh, bytes, minb; block_dims(base, &bw, &bh, &bytes, &minb);
	uint32_t nbx = (w + (uint32_t)bw - 1) / (uint32_t)bw, nby = (h + (uint32_t)bh - 1) / (uint32_t)bh;
	if (nbx < (uint32_t)minb) nbx = (uint32_t)minb; if (nby < (uint32_t)minb) nby = (uint32_t)minb;
	return nbx * nby * (uint32_t)bytes;
}
static void mip_view(const GfxTexture *t, int level, GfxTexture *out) {
	*out = *t;
	if (level <= 0 || (t->format & 0xfff) || ((t->u_mode >> 4) & 3) == 3) return;
	uint32_t off = 0; for (int l = 0; l < level; l++) off += level_bytes(t, l);
	out->data = t->data + off;
	out->width = (uint16_t)(t->width >> level ? t->width >> level : 1); out->height = (uint16_t)(t->height >> level ? t->height >> level : 1);
	out->format &= 0xfffff000u;   /* (a natural stride at every level) */
}
/* one level, bilinear (SCE_GXM_TEXTURE_FILTER_LINEAR and the mipmap variants) or point */
static void texel_level(const GfxTexture *lv, float u, float v, int linear, float out[4]) {
	if (!linear) { texel_raw(lv, u, v, out); return; }
	float w = lv->width ? lv->width : 1, h = lv->height ? lv->height : 1;
	float fx = u * w - 0.5f, fy = v * h - 0.5f, x0 = floorf(fx), y0 = floorf(fy), tx = fx - x0, ty = fy - y0;
	float c00[4], c10[4], c01[4], c11[4];
	texel_raw(lv, (x0 + 0.5f) / w, (y0 + 0.5f) / h, c00); texel_raw(lv, (x0 + 1.5f) / w, (y0 + 0.5f) / h, c10);
	texel_raw(lv, (x0 + 0.5f) / w, (y0 + 1.5f) / h, c01); texel_raw(lv, (x0 + 1.5f) / w, (y0 + 1.5f) / h, c11);
	for (int k = 0; k < 4; k++) out[k] = (c00[k] * (1 - tx) + c10[k] * tx) * (1 - ty) + (c01[k] * (1 - tx) + c11[k] * tx) * ty;
}
/* the texture at level-of-detail `lod` (log2 of texels per pixel; 0 = magnified or unknown) plus
 * the texture's own LOD bias (gfx.h GFX_TEX_LOD_BIAS): min_filter chooses the within-level filter;
 * the mip filter (GFX_TEX_MIP_LINEAR) or min filter MIPMAP_LINEAR (2) blends the two nearest levels,
 * otherwise the nearest level. The swizzle is applied once, after filtering. */
static void texel_lod(const GfxTexture *t, float u, float v, float lod, float out[4]) {
	lod += GFX_TEX_LOD_BIAS(t->format);
	int levels = mip_levels(t), minified = lod > 0 && levels > 1;
	int filt = minified ? t->min_filter : t->mag_filter, linear = (filt & 3) != 0;
	if (!minified) texel_level(t, u, v, linear, out);
	else {
		float lf = lod > levels - 1 ? (float)(levels - 1) : lod;
		GfxTexture lv;
		if (((filt & 3) == 2 || GFX_TEX_MIP_LINEAR(t->format)) && lf < levels - 1) {   /* trilinear */
			int l0 = (int)floorf(lf); float f = lf - (float)l0, a[4], b[4];
			mip_view(t, l0, &lv); texel_level(&lv, u, v, 1, a);
			mip_view(t, l0 + 1, &lv); texel_level(&lv, u, v, 1, b);
			for (int k = 0; k < 4; k++) out[k] = a[k] * (1 - f) + b[k] * f;
		} else { mip_view(t, (int)floorf(lf + 0.5f), &lv); texel_level(&lv, u, v, linear, out); }
	}
	int ncomp = swizzle_ncomp(t->format & 0xff000000u);
	if (ncomp) apply_swizzle(t->format, ncomp, out);
	tex_gamma(t, out);
}
int gfx_texture_levels(const GfxTexture *t) { return ((t->format & 0xfff) || ((t->u_mode >> 4) & 3) == 3) ? 1 : mip_levels(t); }   /* for gfx_gpu.c */
void gfx_texture_level(const GfxTexture *t, int level, GfxTexture *out) { mip_view(t, level, out); }
/* one face of a cube texture (layout 3) as a swizzled 2D texture: faces +X -X +Y -Y +Z -Z follow each
 * other in memory, each a whole image (cube_texel's addressing) */
/* the distance from one cube face to the next. With a mip count in the control word (anything but
 * 0xF, "no mips": GfxTexture.v_mode high nibble 0) each face reserves its whole possible mip chain,
 * down to 1x1 whatever the count, every level padded to the format's block, and the next face starts
 * 2 KB-aligned once the face is big enough (32x32 for <= 8 bits a texel and block formats, 16x16 for
 * 16-32 bits, 8x8 for 64) -- the layout Vita3K's texture cache reads cubes with. Faces were taken as
 * back to back level 0s: Uncharted's (and Unit 13's) 32x32 F16 ambient cube with 6 levels is 12 KB a
 * face, read 8 KB apart every face after +X came out of the previous face's mips -- the "up" face
 * read nearly black, and every floor's ambient light with it. */
static uint32_t cube_face_bytes(const GfxTexture *t) {
	uint32_t base = t->format & 0xff000000u, bpt = tex_bytes_per_texel(base);
	if (!((t->v_mode >> 4) & 15))   /* no mips: the faces follow each other directly */
		return bpt ? (uint32_t)tex_stride(t) * t->height * bpt : ((t->width + 3) / 4) * ((t->height + 3) / 4) * bc_block_bytes(base);
	int bw, bh, bbytes, minblocks; block_dims(base, &bw, &bh, &bbytes, &minblocks);
	uint32_t w = 1, h = 1, total = 0; while (w < t->width) w <<= 1; while (h < t->height) h <<= 1;
	uint32_t w0 = w, h0 = h, bits = (uint32_t)bbytes * 8 / (uint32_t)(bw * bh);
	for (; w && h; w >>= 1, h >>= 1) total += ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * (uint32_t)bbytes;
	int block = bw > 1 || bh > 1;
	int align2k = (w0 >= 32 && h0 >= 32 && (bits <= 8 || block)) || (w0 >= 16 && h0 >= 16 && (bits == 16 || bits == 32)) || (w0 >= 8 && h0 >= 8 && bits == 64);
	uint32_t a = align2k ? 2048 : 4;
	return (total + a - 1) / a * a;
}
void gfx_texture_face_level(const GfxTexture *t, int face, int level, GfxTexture *out) {   /* (gfx_gpu.c: a cube's levels) */
	GfxTexture ft; ft = *t; ft.u_mode = (uint8_t)((t->u_mode & 0x0f) | 0x10); ft.data = t->data + cube_face_bytes(t) * (uint32_t)face;
	mip_view(&ft, level, out);
}
int gfx_texture_cube_levels(const GfxTexture *t) { return mip_levels(t); }
void gfx_texture_face(const GfxTexture *t, int face, GfxTexture *out) {
	*out = *t; out->u_mode = (uint8_t)((t->u_mode & 0x0f) | 0x10); out->v_mode = (uint8_t)(t->v_mode & 0x0f);   /* a swizzled face (as cube_texel), one level */
	out->data = t->data + cube_face_bytes(t) * (uint32_t)face;
}
/* a UBC1/2/3/4 texture's 4x4 blocks as stored, in row-major block order (((w+3)/4) x ((h+3)/4) of
 * them, 8 bytes each for UBC1/4, 16 for UBC2/3) -- for gfx_gpu.c to hand to the GPU's own BC decoder
 * instead of decoding every texel here. The addressing is texel_raw's exactly (block_index: the
 * layout at block granularity, the linear stride rule counted in blocks), so the GPU sees the very
 * blocks the software decoder would have read. `t` is one level or face (gfx_texture_level/face).
 * Returns the block size, 0 for any other format (nothing written). */
int gfx_texture_blocks(const GfxTexture *t, uint8_t *out) {
	uint32_t base = t->format & 0xff000000u;
	uint32_t bsz = bc_block_bytes(base);
	if (!bsz) return 0;
	GfxTexture bt = *t; bt.width = (uint16_t)((t->width + 3) / 4); bt.height = (uint16_t)((t->height + 3) / 4);   /* block_index's view, built once */
	uint32_t nbx = bt.width ? bt.width : 1, nby = bt.height ? bt.height : 1;
	int layout = (t->u_mode >> 4) & 3;
	for (uint32_t by = 0; by < nby; by++) {
		if (layout != 1 && layout != 2) { memcpy(out, G(t->data + bsz * texel_index(&bt, 0, by)), (size_t)bsz * nbx); out += bsz * nbx; continue; }   /* linear: a row of blocks is contiguous */
		for (uint32_t bx = 0; bx < nbx; bx++, out += bsz) memcpy(out, G(t->data + bsz * texel_index(&bt, bx, by)), bsz);
	}
	return (int)bsz;
}
/* sRGB <-> linear, per channel (the IEC 61966-2-1 curve the GPU's sRGB formats implement) */
static float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float linear_to_srgb(float c) { c = c < 0 ? 0 : c > 1 ? 1 : c; return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f; }
/* a texture's gamma mode on a sampled value (gfx.h GFX_TEX_GAMMA): the colour channels decode to
 * linear, alpha stays as stored. Applied to what the shader sees (texel_lod, cube_texel), not in
 * texel() itself, which also feeds gfx_gpu.c's uploads (the GPU decodes in its shader, after
 * filtering, as this does). Formats wider than 8-bit UNORM carry no gamma. */
static void tex_gamma(const GfxTexture *t, float out[4]) {
	if (!GFX_TEX_GAMMA(t->format)) return;
	uint32_t b = t->format >> 24;
	if (b == 0x01 || b == 0x06 || (b >= 0x08 && b <= 0x1f && b != 0x0c)) return;   /* signed/16/32-bit/float families (gfx_gpu.c tex_wide) */
	for (int k = 0; k < 3; k++) out[k] = srgb_to_linear(out[k] < 0 ? 0 : out[k] > 1 ? 1 : out[k]);
}
static void texel(const GfxTexture *t, float u, float v, float out[4]) {
	texel_raw(t, u, v, out);
	int ncomp = swizzle_ncomp(t->format & 0xff000000u);
	if (ncomp) apply_swizzle(t->format, ncomp, out);
}
/* bytes/texel for texel_raw's non-block-compressed cases (0 for block-compressed/PVRTC: those
 * address by 4x4 block via block_index, not a flat per-texel stride -- see cube_texel) */
static uint32_t tex_bytes_per_texel(uint32_t base) {
	switch (base) {
	case 0x1b000000u: return 8;                                                           /* F16F16F16F16 */
	case 0x0c000000u: case 0x0d000000u: case 0x15000000u: case 0x9a000000u: case 0x12000000u:
	case 0x11000000u: case 0x1a000000u: return 4;                                          /* *8U8U8U8/S8S8S8S8/X8U24/U2F10F10F10/F32/F16F16/F11F11F10 */
	case 0x05000000u: case 0x09000000u: case 0x0a000000u: case 0x0b000000u: case 0x07000000u:
	case 0x02000000u: case 0x04000000u: return 2;                                          /* U5U6U5/U16/S16/F16/U8U8/U4U4U4U4/U1U5U5U5 */
	case 0x98000000u: case 0x99000000u: return 3;                                          /* U8U8U8 / S8S8S8: three bytes, no padding */
	case 0x00000000u: return 1;                                                            /* U8 */
	default: return 0;                                                                     /* block-compressed / PVRTC / unknown: no flat per-face stride */
	}
}
/* a cube texture (layout 3): 6 faces stored consecutively (each addressed linearly, row-major, at
 * the texture's own width/height -- GXM's SceGxmTextureInitCube takes power-of-two square faces,
 * but nothing here has ever exercised the swizzled-within-a-face case a real driver might use, so
 * this is the simpler, always-well-defined choice). u/v/w: the direction vector SMP's dependent
 * read already carries (usse.c's exec_smp always reads a 3rd coordinate; 2D sampling just never
 * used it) -- the standard major-axis face selection, s/t mapped to each face the usual way. */
/* a direction's coordinates on a given cube face (-1..1): the other two components over the face's axis */
static void cube_face_uv(int face, float u, float v, float w, float *fu, float *fv) {
	switch (face) {
	case 0: *fu = -w / u; *fv = -v / u; break;
	case 1: *fu = -w / u; *fv = v / u; break;     /* m = -u: (w, -v) / m */
	case 2: *fu = u / v; *fv = w / v; break;
	case 3: *fu = -u / v; *fv = w / v; break;     /* m = -v: (u, -w) / m */
	case 4: *fu = u / w; *fv = -v / w; break;
	default: *fu = u / w; *fv = v / w; break;     /* m = -w: (-u, -v) / m */
	}
}
/* the face a direction hits: the dominant axis, +X -X +Y -Y +Z -Z = 0-5; -1 for (0,0,0) or a NaN */
static int cube_face_of(float u, float v, float w) {
	if (!isfinite(u) || !isfinite(v) || !isfinite(w)) return -1;
	float au = fabsf(u), av = fabsf(v), aw = fabsf(w);
	if (au == 0 && av == 0 && aw == 0) return -1;
	return au >= av && au >= aw ? (u > 0 ? 0 : 1) : av >= au && av >= aw ? (v > 0 ? 2 : 3) : (w > 0 ? 4 : 5);
}
/* GXM stores a cube map's faces swizzled (sceGxmTextureInitCube takes swizzled faces; the same Morton order
 * as layout 1), each a power-of-two square followed by its mip chain (cube_face_bytes): a face is a swizzled
 * 2D texture with the cube's wrap modes and levels. Read linearly the ambient cubes came out as stripes. */
static void cube_face_tex(const GfxTexture *t, int face, GfxTexture *ft) {
	*ft = *t; ft->u_mode = (uint8_t)((t->u_mode & 0x0f) | 0x10);
	ft->data = t->data + cube_face_bytes(t) * (uint32_t)face;
}
/* a cube map at level of detail `lod` (texel_lod's: log2 of face texels per pixel, plus the texture's
 * bias), filtered as a 2D texture would be. It was level 0, point-sampled, whatever the filters and
 * mips: Killzone's environment cube (128x128, 8 levels) sparkled on its glass and metal at a distance
 * where the hardware takes a blurred level, and the GPU path filtered what this didn't. */
static void cube_texel(const GfxTexture *t, float u, float v, float w, float lod, float out[4]) {
	int face = cube_face_of(u, v, w);
	if (face < 0) { texel(t, 0, 0, out); return; }   /* a degenerate direction (0,0,0), or NaN from further up the pipeline: sample something rather than cast a NaN to int below */
	float fu, fv; cube_face_uv(face, u, v, w, &fu, &fv);
	GfxTexture ft; cube_face_tex(t, face, &ft);
	texel_lod(&ft, (fu + 1) * 0.5f, (fv + 1) * 0.5f, lod, out);   /* (the swizzle and gamma with it) */
}
/* bulk-decode a whole texture to linear R8G8B8A8 (one texel() call per pixel, no filtering) -- for
 * the GPU backend's texture upload: every format/swizzle/block-compression/PVRTC decode this file
 * already implements correctly is reused as-is, instead of reimplementing any of it on the GPU
 * side. `out` must hold 4*width*height bytes. */
static void decode_rgba8_generic(const GfxTexture *t, uint8_t *out) {   /* every format, through texel(): the reference the fast paths below must match */
	int w = t->width ? t->width : 1, h = t->height ? t->height : 1;
	for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
		float c[4]; texel(t, (x + 0.5f) / w, (y + 0.5f) / h, c);
		uint8_t *p = out + 4 * (y * w + x);
		for (int k = 0; k < 4; k++) { float v = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k]; p[k] = (uint8_t)(v * 255.f + 0.5f); }
	}
}
static int pvrtc_slow(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_SOFT_PVRTC_SLOW") != 0; return v; }   /* RECOMP_SOFT_PVRTC_SLOW=1: PVRTC levels decoded texel by texel for the GPU backend, as before */
void gfx_decode_texture_rgba8(const GfxTexture *t, uint8_t *out) {
	uint32_t w = t->width ? t->width : 1, h = t->height ? t->height : 1;
	uint32_t base = t->format & 0xff000000u, sw = (t->format >> 12) & 7;
	/* The plain byte formats straight from their bytes: the same bytes the generic path gives for them
	 * (texel_raw's component order, apply_swizzle's placement; a byte through /255 and back is itself),
	 * without its float path and format dispatch per texel. The GPU backend (gfx_gpu.c upload_texture)
	 * decodes every non-BC texture through here on the render thread, at first use and at every CPU
	 * rewrite: Unit 13 redraws a 1024x1024 U8 texture up to four times a frame in its levels, 7.7 ms
	 * each through texel() -- a 30 ms frame at 60 fps -- and well under a millisecond here.
	 * RECOMP_GPU_DECODE_CHECK=1: the generic path runs too and any differing byte is reported. */
	int fast = 0;
	if (base == 0x00000000u) {   /* U8: the one byte, placed by its SWIZZLE1 mode (apply_swizzle's table: -1 the byte, else 0 or 1) */
		static const int k[8][4] = {{-1, 0, 0, 1}, {-1, 0, 0, 0}, {-1, 1, 1, 1}, {-1, -1, -1, -1}, {-1, -1, -1, 0}, {-1, -1, -1, 1}, {0, 0, 0, -1}, {1, 1, 1, -1}};
		int from_r[4]; uint8_t fixed[4];
		for (int i = 0; i < 4; i++) { from_r[i] = k[sw][i] < 0; fixed[i] = k[sw][i] > 0 ? 255 : 0; }
		for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
			uint8_t r = RD8(t->data + texel_index(t, x, y)), *p = out + 4 * (y * w + x);
			for (int i = 0; i < 4; i++) p[i] = from_r[i] ? r : fixed[i];
		}
		fast = 1;
	} else if (base == 0x0c000000u) {   /* U8U8U8U8: bytes c0 c1 c2 c3, R G B A by default; the SWIZZLE4 modes reorder them (-1: 1.0) */
		static const int8_t m[8][4] = {{0, 1, 2, 3}, {2, 1, 0, 3}, {3, 2, 1, 0}, {1, 2, 3, 0}, {0, 1, 2, -1}, {2, 1, 0, -1}, {3, 2, 1, -1}, {1, 2, 3, -1}};
		for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
			uint32_t v = RD32(t->data + 4 * texel_index(t, x, y)); uint8_t *p = out + 4 * (y * w + x);
			uint8_t c[4] = { (uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff), (uint8_t)((v >> 16) & 0xff), (uint8_t)(v >> 24) };
			for (int i = 0; i < 4; i++) p[i] = m[sw][i] < 0 ? 255 : c[m[sw][i]];
		}
		fast = 1;
	} else if ((base == 0x80000000u || base == 0x81000000u || base == 0x82000000u || base == 0x83000000u) && !pvrtc_slow()) {
		/* PVRTC / PVRTC-II: the whole level decoded cell by cell (pvrtc_decode_image, the software
		 * renderer's own texcache decode) instead of texel by texel through texel() -> pvrtc_texel ->
		 * pvrtc_decode_texel, which fetches and unpacks a texel's four blocks for every texel: ~76 ns a
		 * texel, 20 ms for a 512x512 and 84 ms for a 1024x1024 on the render thread, 2.3 s over the 136
		 * uploads of one Uncharted run (its 370-510 ms card-transition frame). The image is the block
		 * grid's pw x ph texels; the texture's w x h texels are picked from it the way the generic path
		 * does -- texel (x,y) samples u = (x+0.5)/w, and pvrtc_texel takes texel (int)(u*pw) of the
		 * image, wrapped -- which is the identity except for the levels under 2x2 blocks (pw > w). No
		 * swizzle: texel() applies none to the PVRTC formats (swizzle_ncomp), and the generic path's
		 * /255 then *255+0.5 of a 0..255 integer is that integer. RECOMP_SOFT_PVRTC_SLOW=1: the
		 * per-texel path, as before. */
		PvrtcGeom g = pvrtc_geom(t, base == 0x80000000u || base == 0x82000000u);
		uint8_t *img = malloc(4u * (size_t)g.pw * (size_t)g.ph);
		if (img && pvrtc_decode_image_par(t, &g, img)) {
			for (uint32_t y = 0; y < h; y++) {
				int py = (int)(((y + 0.5f) / h) * g.ph); py = ((py % g.ph) + g.ph) % g.ph;
				const uint8_t *row = img + 4 * ((size_t)py * (size_t)g.pw); uint8_t *p = out + 4 * ((size_t)y * w);
				if (g.pw == (int)w) memcpy(p, row, 4u * w);
				else for (uint32_t x = 0; x < w; x++, p += 4) { int px = (int)(((x + 0.5f) / w) * g.pw); px = ((px % g.pw) + g.pw) % g.pw; memcpy(p, row + 4 * (size_t)px, 4); }
			}
			fast = 1;
		}
		free(img);
	}
	if (!fast) { decode_rgba8_generic(t, out); return; }
	static int check = -1; if (check < 0) check = getenv("RECOMP_GPU_DECODE_CHECK") != 0;
	if (check) {
		uint8_t *ref = malloc(4u * w * h);
		if (ref) {
			decode_rgba8_generic(t, ref);
			size_t i, n = 4u * w * h; for (i = 0; i < n && ref[i] == out[i]; i++);
			static int said; if (i < n && said++ < 20) fprintf(stderr, "    gfx: decode check: texture %#x fmt %#x %ux%u layout %d: byte %zu differs (fast %u, generic %u)\n", t->data, t->format, w, h, (t->u_mode >> 4) & 3, i, out[i], ref[i]);
			static unsigned ok; if (i == n && ++ok % 200 == 1) fprintf(stderr, "    gfx: decode check: %u textures identical so far (last %#x fmt %#x %ux%u layout %d)\n", ok, t->data, t->format, w, h, (t->u_mode >> 4) & 3);
			free(ref);
		}
	}
}
/* the same at full precision (RGBA floats, no clamp): the GPU backend uploads the formats whose
 * texels don't fit 8-bit UNORM (F16/F32 G-buffers holding positions and normals, X8U24 depth,
 * S16) this way -- decoded to RGBA8 they came back clamped to 0..1 and quantised, and Unit 13's
 * ambient pass read garbage reflection directions from them */
void gfx_decode_texture_f32(const GfxTexture *t, float *out) {
	int w = t->width ? t->width : 1, h = t->height ? t->height : 1;
	for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) texel(t, (x + 0.5f) / w, (y + 0.5f) / h, out + 4 * (y * w + x));
}
/* YUV textures (linear; Uncharted's photo minigame shows its photos as YUV420P3, undecoded they came out
 * as a flat box): YUV420P3 is a Y plane (stride x height bytes), then U and V planes at half size in
 * both directions; P2 the Y plane and one plane of interleaved chroma pairs
 * at half height; YUV422 one plane of 4-byte pairs of texels (YUYV / YVYU / UYVY / VYUY by swizzle).
 * The 420 swizzles' plane order reads backwards from their names: Uncharted decodes its photos with
 * sceJpegDecodeMJpegYCbCr (planes Y, Cb, Cr) and samples them as YVU420P3_CSC1 -- so the YVU
 * swizzles take Cb first and the YUV ones Cr first (one game's evidence; unchecked on hardware). The
 * swizzle also picks the conversion: CSC0 BT.601 video range (Y 16-235), CSC1 full
 * range (JPEG's, as the photo's CSC1 says). Plane layout as Vita3K's texture cache reads it (read for
 * semantics only). */
static void yuv_texel(const GfxTexture *t, uint32_t x, uint32_t y, float out[4]) {
	uint32_t base = t->format & 0xff000000u, sw = (t->format >> 12) & 7, at = t->data, st = (uint32_t)tex_stride(t), h = t->height;
	int yy, uu, vv, full;
	if (base == 0x92000000u) {   /* YUV422: bytes of the pair holding x */
		uint32_t a = at + y * st * 2 + (x & ~1u) * 2, b0 = RD8(a), b1 = RD8(a + 1), b2 = RD8(a + 2), b3 = RD8(a + 3);
		int odd = x & 1;
		switch (sw & 3) {
		case 0: yy = odd ? b2 : b0; uu = b1; vv = b3; break;   /* YUYV */
		case 1: yy = odd ? b2 : b0; vv = b1; uu = b3; break;   /* YVYU */
		case 2: yy = odd ? b3 : b1; uu = b0; vv = b2; break;   /* UYVY */
		default: yy = odd ? b3 : b1; vv = b0; uu = b2; break;  /* VYUY */
		}
		full = sw >= 4;
	} else {
		yy = RD8(at + y * st + x);
		uint32_t c = at + st * h, cx = x / 2, cy = y / 2;
		int a, b;
		if (base == 0x91000000u) { uint32_t cst = st / 2, plane = cst * ((h + 1) / 2); a = RD8(c + cy * cst + cx); b = RD8(c + plane + cy * cst + cx); }
		else { a = RD8(c + cy * st + cx * 2); b = RD8(c + cy * st + cx * 2 + 1); }
		if (sw & 1) { uu = a; vv = b; } else { vv = a; uu = b; }   /* see above: "YVU" reads Cb first */
		full = sw >= 2;
	}
	float Y = full ? yy / 255.f : (yy - 16) * (1.f / 219.f), U = full ? (uu - 128) / 255.f : (uu - 128) * (1.f / 224.f), V = full ? (vv - 128) / 255.f : (vv - 128) * (1.f / 224.f);
	float r = Y + 1.402f * V, g = Y - 0.344136f * U - 0.714136f * V, bl = Y + 1.772f * U;
	out[0] = r < 0 ? 0 : r > 1 ? 1 : r; out[1] = g < 0 ? 0 : g > 1 ? 1 : g; out[2] = bl < 0 ? 0 : bl > 1 ? 1 : bl; out[3] = 1;
}
static void texel_raw(const GfxTexture *t, float u, float v, float out[4]) {
	int w = t->width ? t->width : 1, h = t->height ? t->height : 1;
	int x = wrap_coord((int)(u * w), w, t->u_mode), y = wrap_coord((int)(v * h), h, t->v_mode);
	uint32_t base = t->format & 0xff000000u, at = t->data;
	if (base == 0x0c000000u) {           /* U8U8U8U8, ABGR: bytes R G B A */
		uint32_t p = RD32(at + 4 * texel_index(t, x, y));
		out[0] = (p & 0xff) / 255.f; out[1] = ((p >> 8) & 0xff) / 255.f; out[2] = ((p >> 16) & 0xff) / 255.f; out[3] = (p >> 24) / 255.f;
	} else if (base == 0x98000000u || base == 0x99000000u) {   /* U8U8U8 / S8S8S8: three bytes a texel (Killzone's colour-grading textures in its final composite:
		 * undecoded, every texel came out as the fallback magenta and so did the whole frame) */
		uint32_t a = at + 3 * texel_index(t, x, y);
		for (int k = 0; k < 3; k++) { uint8_t b = RD8(a + (uint32_t)k); if (base == 0x98000000u) out[k] = b / 255.f; else { int8_t sv = (int8_t)b; out[k] = sv <= -127 ? -1.f : sv / 127.f; } }
		out[3] = 1;
	} else if (base == 0x05000000u) {    /* U5U6U5 (as our render targets write it) */
		uint16_t p = RD16(at + 2 * texel_index(t, x, y));
		out[0] = (p >> 11) / 31.f; out[1] = ((p >> 5) & 63) / 63.f; out[2] = (p & 31) / 31.f; out[3] = 1;
	} else if (base == 0x00000000u) {    /* U8: the one component (the swizzle places it) */
		out[0] = RD8(at + texel_index(t, x, y)) / 255.f; out[1] = out[2] = 0; out[3] = 1;
	} else if (base == 0x0d000000u) {    /* S8S8S8S8: signed-normalized bytes (e.g. tangent-space normal maps) */
		uint32_t p = RD32(at + 4 * texel_index(t, x, y));
		for (int k = 0; k < 4; k++) { int8_t v = (int8_t)((p >> (8 * k)) & 0xff); out[k] = v <= -127 ? -1.f : v / 127.f; }
	} else if (base == 0x1b000000u) {    /* F16F16F16F16 */
		uint32_t a = at + 8 * texel_index(t, x, y);
		for (int k = 0; k < 4; k++) out[k] = half_to_float_soft(RD16(a + 2 * k));
	} else if (base == 0x15000000u) {    /* X8U24: a depth buffer sampled as a texture */
		uint32_t p = RD32(at + 4 * texel_index(t, x, y)) & 0xFFFFFFu;
		out[0] = out[1] = out[2] = p / 16777215.f; out[3] = 1;
	} else if (base == 0x9a000000u) {    /* U2F10F10F10: HDR accumulation buffers. Approximated as plain
	                                       * unsigned-normalized 10-bit fields (exact 10-bit float bias/exponent layout not pinned down). */
		uint32_t p = RD32(at + 4 * texel_index(t, x, y));   /* three unsigned 10-bit floats (5-bit exponent) and 2-bit alpha */
		out[0] = small_float(p & 0x3FF, 5); out[1] = small_float((p >> 10) & 0x3FF, 5); out[2] = small_float((p >> 20) & 0x3FF, 5); out[3] = (p >> 30) / 3.f;
	} else if (base == 0x85000000u || base == 0x86000000u || base == 0x87000000u) {   /* UBC1/2/3 (DXT1/3/5) */
		uint32_t bx = (uint32_t)x / 4, by = (uint32_t)y / 4, tib = (uint32_t)(y % 4) * 4 + (uint32_t)(x % 4);
		uint32_t blk = block_index(t, bx, by), bsz = base == 0x85000000u ? 8 : 16;
		uint32_t ba = at + bsz * blk, ca = base == 0x85000000u ? ba : ba + 8;
		uint32_t c0 = RD16(ca), c1 = RD16(ca + 2), idx2 = RD32(ca + 4);
		int transparent = 0;
		dxt_rgb(c0, c1, idx2, (int)tib, base == 0x85000000u, out, &transparent);
		if (base == 0x85000000u) out[3] = transparent ? 0.f : 1.f;              /* BC1: punch-through alpha only */
		else if (base == 0x86000000u) { uint64_t abits = RD64(ba); out[3] = (float)((abits >> (4 * tib)) & 0xF) / 15.f; }   /* BC2: explicit 4-bit alpha */
		else out[3] = bc_alpha_block(ba, (uint32_t)tib) / 255.f;               /* BC3: interpolated 3-bit alpha */
	} else if (base == 0x88000000u) {    /* UBC4: one channel, a BC3 alpha block per 4x4 (the swizzle places it:
	                                       * Killzone: Mercenary's UI text and icons are UBC4 R111 -- white, the
	                                       * channel as alpha; undecoded they sampled as opaque magenta) */
		uint32_t bx = (uint32_t)x / 4, by = (uint32_t)y / 4, tib = (uint32_t)(y % 4) * 4 + (uint32_t)(x % 4);
		out[0] = bc_alpha_block(at + 8 * block_index(t, bx, by), tib) / 255.f; out[1] = out[2] = 0; out[3] = 1;
	} else if (base == 0x09000000u) {    /* U16 (one component; the swizzle places it) */
		out[0] = RD16(at + 2 * texel_index(t, x, y)) / 65535.f; out[1] = out[2] = 0; out[3] = 1;
	} else if (base == 0x0a000000u) {    /* S16 */
		float f = (int16_t)RD16(at + 2 * texel_index(t, x, y)) / 32767.f; out[0] = f < -1 ? -1 : f; out[1] = out[2] = 0; out[3] = 1;
	} else if (base == 0x0b000000u) {    /* F16 */
		out[0] = half_to_float_soft(RD16(at + 2 * texel_index(t, x, y))); out[1] = out[2] = 0; out[3] = 1;
	} else if (base == 0x12000000u) {    /* F32 */
		uint32_t p = RD32(at + 4 * texel_index(t, x, y)); memcpy(&out[0], &p, 4); out[1] = out[2] = 0; out[3] = 1;
	} else if (base == 0x07000000u) {    /* U8U8: two components, R then G */
		uint16_t p = RD16(at + 2 * texel_index(t, x, y)); out[0] = (p & 0xff) / 255.f; out[1] = (p >> 8) / 255.f; out[2] = 0; out[3] = 1;
	} else if (base == 0x11000000u) {    /* F16F16 */
		uint32_t p = RD32(at + 4 * texel_index(t, x, y)); out[0] = half_to_float_soft(p & 0xffff); out[1] = half_to_float_soft(p >> 16); out[2] = 0; out[3] = 1;
	} else if (base == 0x02000000u) {    /* U4U4U4U4, ABGR: R in the low nibble (as U8U8U8U8's byte order; Killzone: Mercenary's
	                                       * font atlas keeps glyph coverage there and its text shader selects it with a one-hot
	                                       * dot product -- reading the nibbles the other way round emptied every glyph) */
		uint16_t p = RD16(at + 2 * texel_index(t, x, y)); for (int k = 0; k < 4; k++) out[k] = ((p >> (4 * k)) & 15) / 15.f;
	} else if (base == 0x04000000u) {    /* U1U5U5U5, ABGR: R in the low 5 bits, A the top bit */
		uint16_t p = RD16(at + 2 * texel_index(t, x, y)); out[0] = (p & 31) / 31.f; out[1] = ((p >> 5) & 31) / 31.f; out[2] = ((p >> 10) & 31) / 31.f; out[3] = (float)(p >> 15);
	} else if (base == 0x1a000000u) {    /* F11F11F10: unsigned 6-bit-mantissa R, G and 5-bit B floats, R lowest (an HDR
	                                       * buffer sampled as a texture: Unit 13's colour-grading pass; undecoded, it read as magenta) */
		uint32_t p = RD32(at + 4 * texel_index(t, x, y));
		out[0] = small_float(p & 0x7ff, 6); out[1] = small_float((p >> 11) & 0x7ff, 6); out[2] = small_float(p >> 22, 5); out[3] = 1;
	} else if (base == 0x80000000u || base == 0x82000000u) { pvrtc_texel(t, 1, u, v, out); }   /* PVRT2BPP / PVRTII2BPP */
	else if (base == 0x81000000u || base == 0x83000000u) { pvrtc_texel(t, 0, u, v, out); }      /* PVRT4BPP / PVRTII4BPP */
	else if (base >= 0x90000000u && base <= 0x92000000u) yuv_texel(t, (uint32_t)x, (uint32_t)y, out);
	else {
		static uint32_t warned[64]; static int nw;
		int seen = 0; for (int i = 0; i < nw; i++) if (warned[i] == t->format) seen = 1;
		if (!seen && nw < 64) { warned[nw++] = t->format; fprintf(stderr, "    soft: texture format %#x not handled\n", t->format); }
		out[0] = 1; out[1] = 0; out[2] = 1; out[3] = 1;
	}
}
/* ---- colour targets by format (SceGxmColorFormat: base format in the bits 0xF1800000, swizzle
 * in 0x00300000, components named most significant first). HDR and G-buffer targets (half/float,
 * packed 10/11-bit floats) are written and read in their own encoding, so a later pass that samples
 * them as a texture sees what the GPU would have stored */
static float small_float(uint32_t v, int mbits) {   /* unsigned, 5-bit exponent (bias 15), mbits mantissa */
	uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
	if (e == 0) return ldexpf((float)m, -14 - mbits);
	if (e == 31) return m ? 0.f : 65504.f;
	return ldexpf(1.f + (float)m / (1u << mbits), (int)e - 15);
}
static uint32_t to_small_float(float f, int mbits) {
	if (!(f > 0)) return 0;
	if (f >= 65504.f) return (30u << mbits) | ((1u << mbits) - 1);
	int e; float m = frexpf(f, &e);   /* f = m * 2^e, m in [0.5, 1) */
	int be = e - 1 + 15;
	if (be <= 0) return (uint32_t)(f * ldexpf(1.f, 14 + mbits) + .5f) & ((1u << mbits) - 1);
	uint32_t mant = (uint32_t)((m * 2 - 1) * (1u << mbits) + .5f);
	if (mant >> mbits) { mant = 0; be++; }
	return ((uint32_t)be << mbits) | mant;
}
static uint16_t float_to_half_soft(float f) {
	uint32_t b; memcpy(&b, &f, 4);
	uint32_t s = (b >> 16) & 0x8000, e = (b >> 23) & 0xff, m = b & 0x7fffff;
	if (e == 0xff) return (uint16_t)(s | 0x7c00 | (m ? 0x200 : 0));
	int ne = (int)e - 127 + 15;
	if (ne >= 31) return (uint16_t)(s | 0x7c00);
	if (ne <= 0) { if (ne < -10) return (uint16_t)s; m |= 0x800000; return (uint16_t)(s | (m >> (14 - ne))); }
	return (uint16_t)(s | (uint32_t)ne << 10 | (m >> 13));
}
static uint32_t target_base_of(const GfxTarget *t) { return t->format & 0xF1800000u; }
static int target_bpp_of(const GfxTarget *t) {
	switch (target_base_of(t)) {
	case 0x30000000u: case 0x40000000u: case 0x50000000u: case 0x60000000u: case 0xF0000000u: case 0x20800000u: case 0x40800000u:
	case 0xA0800000u: case 0xB0800000u: case 0xC0800000u: return 2;
	case 0x80800000u: case 0x90800000u: return 1;
	case 0x01000000u: case 0x11000000u: return 8;
	default: return 4;
	}
}
static uint32_t target_base(void) { return target_base_of(&target); }
int gfx_target_bpp(const GfxTarget *t) { return target_bpp_of(t); }
int gfx_texture_bpp(const GfxTexture *t) { return (int)tex_bytes_per_texel(t->format & 0xff000000u); }   /* for gfx_gpu.c: how many raw bytes/pixel this target's declared format actually is */
static int target_16(void) { return target_base() == 0x30000000u; }   /* U5U6U5 */
/* `t` isolates this from the live scene's `target` global -- gfx_gpu.c's RECOMP_GFX_CHECK and its
 * format-aware guest-memory round trip call these directly with their own GfxTarget, independent
 * of whatever the software renderer itself is doing (if anything) at that moment. */
/* a pixel's index in the target's memory by its layout (gfx.h GfxTarget.type): linear rows of
 * `stride` pixels; tiled = 32x32 tiles, stride/32 per tile row, each tile row by row (the same
 * addressing texel_index uses for a tiled texture of that width); swizzled = Morton order */
static uint32_t target_index(const GfxTarget *t, int x, int y) {
	if (t->type == 1) {
		uint32_t tiles_x = t->stride / 32; if (!tiles_x) tiles_x = 1;
		return (((uint32_t)y / 32) * tiles_x + (uint32_t)x / 32) * 1024 + ((uint32_t)y % 32) * 32 + (uint32_t)x % 32;
	}
	if (t->type == 2) {
		uint32_t w = 1, h = 1; while (w < t->width) w <<= 1; while (h < t->height) h <<= 1;
		uint32_t m = w < h ? w : h, idx = 0, bit = 0;
		for (uint32_t k = 1; k < m; k <<= 1, bit += 2) idx |= (((uint32_t)y & k) ? 1u << bit : 0) | (((uint32_t)x & k) ? 2u << bit : 0);
		uint32_t rest = w > h ? (uint32_t)x / m : (uint32_t)y / m;
		return idx + rest * m * m;
	}
	return (uint32_t)y * t->stride + (uint32_t)x;
}
/* U4U4U4U4 targets: output channel k (R, G, B, A) is nibble u4_order[swizzle][k] -- ABGR, ARGB,
 * RGBA, BGRA, the byte orders of the 8-bit default case (R in the low nibble, as the texture
 * decode reads the format) */
static const uint8_t u4_order[4][4] = {{0, 1, 2, 3}, {2, 1, 0, 3}, {3, 2, 1, 0}, {1, 2, 3, 0}};
static void target_read_raw_of(const GfxTarget *t, int x, int y, float c[4]);
/* an sRGB colour surface (gfx.h GFX_TGT_GAMMA; U8U8U8U8, the one family the GPU's sRGB targets
 * cover): reads decode the colour channels to linear -- blending and FRAGCOLOR preloads see what
 * the hardware's blender sees -- and writes encode them */
static int tgt_srgb(const GfxTarget *t) { return GFX_TGT_GAMMA(t->format) && (t->format & 0xF1800000u) == 0; }
static void target_read_of(const GfxTarget *t, int x, int y, float c[4]) {
	target_read_raw_of(t, x, y, c);
	if (tgt_srgb(t)) for (int k = 0; k < 3; k++) c[k] = srgb_to_linear(c[k]);
}
static void target_read_raw_of(const GfxTarget *t, int x, int y, float c[4]) {
	uint32_t a = t->data + target_bpp_of(t) * target_index(t, x, y);
	switch (target_base_of(t)) {
	case 0x30000000u: { uint16_t p = RD16(a); c[0] = (p >> 11) / 31.f; c[1] = ((p >> 5) & 63) / 63.f; c[2] = (p & 31) / 31.f; c[3] = 1; return; }
	case 0x01000000u: for (int k = 0; k < 4; k++) c[k] = half_to_float_soft(RD16(a + 2 * k)); return;
	case 0x00800000u: c[0] = half_to_float_soft(RD16(a)); c[1] = half_to_float_soft(RD16(a + 2)); c[2] = 0; c[3] = 1; return;
	case 0x11000000u: { uint32_t r = RD32(a), g = RD32(a + 4); memcpy(&c[0], &r, 4); memcpy(&c[1], &g, 4); c[2] = 0; c[3] = 1; return; }
	case 0x10800000u: { uint32_t r = RD32(a); memcpy(&c[0], &r, 4); c[1] = c[2] = 0; c[3] = 1; return; }
	case 0x41000000u: { uint32_t p = RD32(a); c[0] = small_float(p & 0x3ff, 5); c[1] = small_float((p >> 10) & 0x3ff, 5); c[2] = small_float((p >> 20) & 0x3ff, 5); c[3] = (p >> 30) / 3.f; return; }
	case 0x21000000u: { uint32_t p = RD32(a); c[0] = small_float(p & 0x7ff, 6); c[1] = small_float((p >> 11) & 0x7ff, 6); c[2] = small_float(p >> 22, 5); c[3] = 1; return; }
	/* U8/S8/U8U8/S8S8/U16/S16: 1-2 raw components at their real size -- these used to fall into the
	 * 4-byte default below (which target_bpp_of already said they weren't), reading/writing 2-4x
	 * past the pixel they meant (Uncharted's U8 target, mask R, corrupted its own neighbours) */
	case 0x80800000u: c[0] = RD8(a) / 255.f; c[1] = c[2] = 0; c[3] = 1; return;                       /* U8 */
	case 0x90800000u: { float v = (int8_t)RD8(a) / 127.f; c[0] = v < -1.f ? -1.f : v; c[1] = c[2] = 0; c[3] = 1; return; }   /* S8 */
	case 0xB0800000u: c[0] = RD8(a) / 255.f; c[1] = RD8(a + 1) / 255.f; c[2] = 0; c[3] = 1; return;   /* U8U8 */
	case 0xC0800000u: {                                                                                /* S8S8 */
		float v0 = (int8_t)RD8(a) / 127.f, v1 = (int8_t)RD8(a + 1) / 127.f;
		c[0] = v0 < -1.f ? -1.f : v0; c[1] = v1 < -1.f ? -1.f : v1; c[2] = 0; c[3] = 1; return;
	}
	case 0x40800000u: c[0] = RD16(a) / 65535.f; c[1] = c[2] = 0; c[3] = 1; return;                    /* U16 */
	case 0x20800000u: { float v = (int16_t)RD16(a) / 32767.f; c[0] = v < -1.f ? -1.f : v; c[1] = c[2] = 0; c[3] = 1; return; }   /* S16 */
	case 0x50000000u: {   /* U4U4U4U4: four nibbles in the 8-bit case's channel orders (ABGR: R lowest). Killzone:
	                       * Mercenary renders a few small masks into one; the 4-byte default below read and
	                       * wrote two pixels at a time at this format's 2-byte stride */
		uint16_t p = RD16(a); const uint8_t *o = u4_order[(t->format >> 20) & 3];
		for (int k = 0; k < 4; k++) c[k] = ((p >> (4 * o[k])) & 15) / 15.f;
		return;
	}
	default: {
		uint32_t p = RD32(a);
		float b0 = (p & 0xff) / 255.f, b1 = ((p >> 8) & 0xff) / 255.f, b2 = ((p >> 16) & 0xff) / 255.f, b3 = (p >> 24) / 255.f;
		switch ((t->format >> 20) & 3) {   /* ABGR, ARGB, RGBA, BGRA */
		case 0: c[0] = b0; c[1] = b1; c[2] = b2; c[3] = b3; break;
		case 1: c[0] = b2; c[1] = b1; c[2] = b0; c[3] = b3; break;
		case 2: c[0] = b3; c[1] = b2; c[2] = b1; c[3] = b0; break;
		case 3: c[0] = b1; c[1] = b2; c[2] = b3; c[3] = b0; break;
		}
	}
	}
}
static void target_read(int x, int y, float c[4]) { if (!target.data) { c[0] = c[1] = c[2] = c[3] = 0; return; } target_read_of(&target, x, y, c); }
static uint32_t to8(float f) { return f <= 0 ? 0 : f >= 1 ? 255 : (uint32_t)(f * 255 + .5f); }
static uint32_t to8s(float f) { return (uint32_t)(int8_t)(f <= -1.f ? -127 : f >= 1.f ? 127 : (int32_t)(f * 127.f + (f >= 0 ? .5f : -.5f))); }
static uint32_t to16(float f) { return f <= 0 ? 0 : f >= 1 ? 65535 : (uint32_t)(f * 65535.f + .5f); }
static uint32_t to16s(float f) { return (uint32_t)(int16_t)(f <= -1.f ? -32767 : f >= 1.f ? 32767 : (int32_t)(f * 32767.f + (f >= 0 ? .5f : -.5f))); }
static void target_write_raw_of(const GfxTarget *t, int x, int y, const float c[4]);
static void target_write_of(const GfxTarget *t, int x, int y, const float c[4]) {
	if (!tgt_srgb(t)) { target_write_raw_of(t, x, y, c); return; }
	float e[4] = { linear_to_srgb(c[0]), linear_to_srgb(c[1]), linear_to_srgb(c[2]), c[3] };
	target_write_raw_of(t, x, y, e);
}
static void target_write_raw_of(const GfxTarget *t, int x, int y, const float c[4]) {
	uint32_t a = t->data + target_bpp_of(t) * target_index(t, x, y);
	switch (target_base_of(t)) {
	case 0x30000000u: { uint32_t r = to8(c[0]) >> 3, g = to8(c[1]) >> 2, b = to8(c[2]) >> 3; WR16(a, (uint16_t)(r << 11 | g << 5 | b)); return; }
	case 0x01000000u: for (int k = 0; k < 4; k++) WR16(a + 2 * k, float_to_half_soft(c[k])); return;
	case 0x00800000u: WR16(a, float_to_half_soft(c[0])); WR16(a + 2, float_to_half_soft(c[1])); return;
	case 0x11000000u: { uint32_t r, g; memcpy(&r, &c[0], 4); memcpy(&g, &c[1], 4); WR32(a, r); WR32(a + 4, g); return; }
	case 0x10800000u: { uint32_t r; memcpy(&r, &c[0], 4); WR32(a, r); return; }
	case 0x41000000u: {
		uint32_t al = c[3] <= 0 ? 0 : c[3] >= 1 ? 3 : (uint32_t)(c[3] * 3 + .5f);
		WR32(a, to_small_float(c[0], 5) | to_small_float(c[1], 5) << 10 | to_small_float(c[2], 5) << 20 | al << 30); return;
	}
	case 0x21000000u: WR32(a, to_small_float(c[0], 6) | to_small_float(c[1], 6) << 11 | to_small_float(c[2], 5) << 22); return;
	case 0x80800000u: WR8(a, (uint8_t)to8(c[0])); return;                              /* U8 */
	case 0x90800000u: WR8(a, (uint8_t)to8s(c[0])); return;                             /* S8 */
	case 0xB0800000u: WR8(a, (uint8_t)to8(c[0])); WR8(a + 1, (uint8_t)to8(c[1])); return;    /* U8U8 */
	case 0xC0800000u: WR8(a, (uint8_t)to8s(c[0])); WR8(a + 1, (uint8_t)to8s(c[1])); return;  /* S8S8 */
	case 0x40800000u: WR16(a, (uint16_t)to16(c[0])); return;                           /* U16 */
	case 0x20800000u: WR16(a, (uint16_t)to16s(c[0])); return;                          /* S16 */
	case 0x50000000u: {   /* U4U4U4U4 (target_read_of's layout), quantised from the 8-bit value so the GPU path's
	                       * RGBA8 round trip (gfx_encode_target_rgba8) stores exactly the nibbles this path does */
		const uint8_t *o = u4_order[(t->format >> 20) & 3]; uint32_t p = 0;
		for (int k = 0; k < 4; k++) p |= ((to8(c[k]) * 15 + 127) / 255) << (4 * o[k]);
		WR16(a, (uint16_t)p); return;
	}
	default: {
		uint32_t r = to8(c[0]), g = to8(c[1]), b = to8(c[2]), al = to8(c[3]), p;
		switch ((t->format >> 20) & 3) {
		case 0: p = r | g << 8 | b << 16 | al << 24; break;
		case 1: p = b | g << 8 | r << 16 | al << 24; break;
		case 2: p = al | b << 8 | g << 16 | r << 24; break;
		default: p = al | r << 8 | g << 16 | b << 24; break;
		}
		WR32(a, p);
	}
	}
}
static void target_write(int x, int y, const float c[4]) { target_write_of(&target, x, y, c); }
/* bulk format-aware round trip, for gfx_gpu.c: decode a target's whole guest-memory surface to
 * linear RGBA8 (for comparing against or combining with a GPU texture's own, always-RGBA8
 * representation), and the reverse -- reusing target_read_of/target_write_of exactly, not a
 * second decoder, so this never drifts from what the renderer itself does. */
void gfx_decode_target_rgba8(const GfxTarget *t, uint8_t *out) {
	for (uint32_t y = 0; y < t->height; y++) for (uint32_t x = 0; x < t->stride; x++) {
		float c[4]; target_read_raw_of(t, (int)x, (int)y, c);   /* raw: an sRGB surface's GPU texture is sRGB-format, holding the very bytes guest memory does */
		uint8_t *p = out + 4 * (y * t->stride + x);
		for (int k = 0; k < 4; k++) p[k] = (uint8_t)to8(c[k]);
	}
}
int gfx_target_plain_rgba8(const GfxTarget *t) { return target_base_of(t) == 0 && ((t->format >> 20) & 3) == 0 && t->type == 0; }   /* bytes R,G,B,A, linear: what an RGBA8 GPU texture holds */
void gfx_encode_target_rgba8(const GfxTarget *t, const uint8_t *in) {
	for (uint32_t y = 0; y < t->height; y++) for (uint32_t x = 0; x < t->stride; x++) {
		const uint8_t *p = in + 4 * (y * t->stride + x);
		float c[4] = { p[0] / 255.f, p[1] / 255.f, p[2] / 255.f, p[3] / 255.f };
		target_write_raw_of(t, (int)x, (int)y, c);
	}
}
/* the same at full precision, RGBA floats -- for gfx_gpu.c's float render targets (F16/F32 families) */
void gfx_decode_target_f32(const GfxTarget *t, float *out) {   /* raw, as the RGBA8 pair above: the GPU keeps an sRGB surface's bytes as they are */
	for (uint32_t y = 0; y < t->height; y++) for (uint32_t x = 0; x < t->stride; x++) target_read_raw_of(t, (int)x, (int)y, out + 4 * (y * t->stride + x));
}
void gfx_encode_target_f32(const GfxTarget *t, const float *in) {
	for (uint32_t y = 0; y < t->height; y++) for (uint32_t x = 0; x < t->stride; x++) target_write_raw_of(t, (int)x, (int)y, in + 4 * (y * t->stride + x));
}
static float factor(int f, const float s[4], const float d[4], int k) {
	switch (f) {
	case 0: return 0; case 1: return 1;
	case 2: return s[k]; case 3: return 1 - s[k];
	case 4: return s[3]; case 5: return 1 - s[3];
	case 6: return d[k]; case 7: return 1 - d[k];
	case 8: return d[3]; case 9: return 1 - d[3];
	default: return 1;
	}
}
/* SceGxmBlendFunc per channel (colour_func for RGB, alpha_func for A): MIN/MAX take the raw
 * source/destination values, not the factor-weighted ones (as on real hardware/every 3D API --
 * the factors don't apply to those two ops) */
static void blend_apply(const GfxBlend *bl, const float c[4], const float dst[4], float out[4]) {
	for (int k = 0; k < 4; k++) {
		int a = k == 3;
		int func = a ? bl->alpha_func : bl->color_func;
		/* SCE_GXM_BLEND_FUNC_NONE: that channel isn't blended at all -- the source colour lands as
		 * it is (only the write mask applies). Treated as ADD with its ZERO/ONE factors it kept the
		 * destination: Unit 13's SSAO pass (func NONE, mask R) never wrote its occlusion buffer, and
		 * the occlusion multiply later darkened the whole lit frame to black */
		if (func == 0) { out[k] = c[k]; continue; }
		if (func == 4) { out[k] = c[k] < dst[k] ? c[k] : dst[k]; continue; }   /* MIN */
		if (func == 5) { out[k] = c[k] > dst[k] ? c[k] : dst[k]; continue; }   /* MAX */
		float s = c[k] * factor(a ? bl->alpha_src : bl->color_src, c, dst, k);
		float d = dst[k] * factor(a ? bl->alpha_dst : bl->color_dst, c, dst, k);
		out[k] = func == 2 ? s - d : func == 3 ? d - s : s + d;   /* SUBTRACT / REVERSE_SUBTRACT / ADD (default) */
	}
}

/* ---- deferred, banded rasterisation: draws are recorded per scene (their triangles set up once);
 * at a flush, worker threads each take a horizontal band of the target and walk every triangle in
 * submission order, clipped to the band. Per pixel the order is the GPU's, and depth, stencil and
 * blending never cross bands, so the bands need no locking. */
typedef struct {
	GfxDraw d; Shader s; float clear[4], add[4], tint[4];
	int use_interp;   /* RECOMP_SHADERS=interp and both programs are within the interpreter's support */
	const UsseProgram *vprog, *fprog;
	const UsseVertexLayout *vlayout; const UsseFragLayout *flayout;
	UsseDraw vdraw, fdraw;   /* SA registers, set up once per draw (§1.9) */
	/* RECOMP_GFX_STATS: what became of the draw -- its triangles, then its covered fragments */
	uint32_t st_tris, st_near, st_culled, st_zero, st_clipped, st_cov, st_sfail, st_dfail, st_discard, st_nocolor, st_written;
	int draw_index;   /* the frame's draw number at submission (rasterisation is deferred to the flush) */
	uint32_t vis_count;   /* its samples that passed depth and stencil, unless discarded (GfxDraw.vis_addr) */
} DrawRec;
/* RECOMP_GFX_STATS=black: for every scene in which no draw wrote colour, one line per draw naming
 * why (all culled, all clipped, every fragment failed the depth test, ...); =1: every scene. Turns a
 * black frame into the draw and the stage that emptied it. */
static int gfx_stats = -1;
static uint32_t stats_scene, stats_skipped;
#define ST(rec, f) do { if (gfx_stats > 0) __atomic_fetch_add(&((DrawRec *)(rec))->f, 1, __ATOMIC_RELAXED); } while (0)
static void stats_init(void) {
	if (gfx_stats >= 0) return;
	const char *e = getenv("RECOMP_GFX_STATS");
	gfx_stats = !e ? 0 : !strcmp(e, "black") ? 2 : 1;
}
typedef struct { float sx[3], sy[3], inv_area; int y0, y1, x0, x1; Vert v[3]; uint32_t draw; } Tri;
static DrawRec *recs; static uint32_t nrecs, cap_recs;
static Tri *tris; static uint32_t ntris, cap_tris;
static GfxTarget scene_target;   /* (defined with the scene state below) */
static const char *stats_reason(const DrawRec *r, char *buf, size_t n) {
	if (!r->st_tris) { snprintf(buf, n, "no triangles (count %u, prim %#x)", r->d.count, r->d.prim); return buf; }
	uint32_t live = r->st_tris - r->st_near - r->st_culled - r->st_zero - r->st_clipped;
	if (!live) {
		snprintf(buf, n, "%u tris, none rasterised: %u behind the near plane, %u culled (mode %d), %u zero-area, %u clipped away (region clip %#x %u-%u/%u-%u, viewport %g %g %g %g)",
		         r->st_tris, r->st_near, r->st_culled, r->d.cull, r->st_zero, r->st_clipped, r->d.clip_mode, r->d.clip_xmin, r->d.clip_xmax, r->d.clip_ymin, r->d.clip_ymax,
		         r->d.vp_xoff, r->d.vp_xscale, r->d.vp_yoff, r->d.vp_yscale);
		return buf;
	}
	if (!r->st_cov) { snprintf(buf, n, "%u tris rasterised but no pixel centre covered (sub-pixel or degenerate)", live); return buf; }
	snprintf(buf, n, "%u tris, %u fragments: %u stencil-failed, %u depth-failed (func %u), %u discarded, %u colour-disabled, %u written",
	         live, r->st_cov, r->st_sfail, r->st_dfail, r->d.depth_func, r->st_discard, r->st_nocolor, r->st_written);
	return buf;
}
static void stats_report(void) {
	if (gfx_stats <= 0) return;
	uint32_t wrote = 0;
	for (uint32_t i = 0; i < nrecs; i++) wrote += recs[i].st_written != 0;
	if (gfx_stats == 2 && (wrote || !nrecs)) return;
	fprintf(stderr, "gfx stats: scene %u target %#x %ux%u fmt %#x depth %d: %u draws (%u skipped), %u wrote colour\n", stats_scene, scene_target.data,
	        scene_target.width, scene_target.height, scene_target.format, scene_target.depth, nrecs, stats_skipped, wrote);
	char buf[320];
	for (uint32_t i = 0; i < nrecs && i < 128; i++) {
		const DrawRec *r = &recs[i];
		if (gfx_stats == 1 || !r->st_written)
			fprintf(stderr, "  draw %u (v %#x f %#x%s): %s\n", i + 1, r->d.vprog, r->d.fprog, r->use_interp ? "" : " stand-in", stats_reason(r, buf, sizeof buf));
	}
	if (nrecs > 128) fprintf(stderr, "  ... %u more draws\n", nrecs - 128);
}
static GfxTarget scene_target;
static Depth *scene_depth;

/* ---- shader interpreter integration (RECOMP_SHADERS=interp): real GXP vertex/fragment programs
 * run through usse.c instead of the stand-ins above. */
/* the texel's native packed words for the interpreter (usse.h UsseTexRawFn): what the hardware's
 * texture unit hands a shader that asked for no conversion -- Unit 13's lighting reads its F32F32
 * G-buffer back as U8U8U8U8/S8S8S8S8 bytes (a 1440-wide view of the 8-byte pixels) and its depth as
 * X8U24 this way and unpacks the bytes itself. Block-compressed texels are decoded to a U8U8U8U8 word. */
static int usse_texfn_raw(void *ctx, int unit, float u, float v, float w3, float lod, uint32_t raw[4]) {
	(void)w3; (void)lod;
	const GfxTexture *tx = (const GfxTexture *)ctx;   /* the stage's 16 units (GfxDraw.tex or .vtex) */
	if (unit < 0 || unit >= 16 || !tx[unit].data) return 0;
	const GfxTexture *t = &tx[unit];
	if (((t->u_mode >> 4) & 3) == 3) return 0;   /* cube: the float path */
	uint32_t base = t->format & 0xff000000u, bpp = tex_bytes_per_texel(base);
	int w = t->width ? t->width : 1, h = t->height ? t->height : 1;
	int x = wrap_coord((int)(u * w), w, t->u_mode), y = wrap_coord((int)(v * h), h, t->v_mode);
	if (bpp == 0) {
		if (base < 0x85000000u || base > 0x87000000u) return 0;
		float c[4]; texel_raw(t, u, v, c);
		uint32_t b[4];
		for (int k = 0; k < 4; k++) { float f = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k]; b[k] = (uint32_t)(f * 255.f + 0.5f); }
		raw[0] = b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
		return 1;
	}
	uint32_t at = t->data + bpp * texel_index(t, (uint32_t)x, (uint32_t)y);
	if (bpp == 8) { raw[0] = RD32(at); raw[1] = RD32(at + 4); return 2; }
	raw[0] = bpp == 4 ? RD32(at) : bpp == 2 ? RD16(at) : RD8(at);
	return 1;
}
/* SMP's gather (usse.h UsseTexGatherFn): texel_level's bilinear footprint at level 0, unblended, in
 * gather order, each texel decoded as texel_lod would (swizzle, gamma) */
static int usse_texfn_gather(void *ctx, int unit, float u, float v, float texels[4][4], float frac[2]) {
	const GfxTexture *tx = (const GfxTexture *)ctx;
	if (unit < 0 || unit >= 16 || !tx[unit].data) return 0;
	const GfxTexture *t = &tx[unit];
	if (((t->u_mode >> 4) & 3) == 3) return 0;   /* cube: not a 2D footprint */
	float w = t->width ? t->width : 1, h = t->height ? t->height : 1;
	float fx = u * w - 0.5f, fy = v * h - 0.5f, x0 = floorf(fx), y0 = floorf(fy);
	frac[0] = fx - x0; frac[1] = fy - y0;
	static const float off[4][2] = {{0.5f, 1.5f}, {1.5f, 1.5f}, {1.5f, 0.5f}, {0.5f, 0.5f}};   /* (x0,y1) (x1,y1) (x1,y0) (x0,y0) */
	uint32_t base = t->format & 0xff000000u;
	int ncomp = swizzle_ncomp(base);
	for (int i = 0; i < 4; i++) {
		texel_raw(t, (x0 + off[i][0]) / w, (y0 + off[i][1]) / h, texels[i]);
		if (ncomp) apply_swizzle(t->format, ncomp, texels[i]);
		tex_gamma(t, texels[i]);
	}
	return ncomp ? ncomp : 4;
}
static void usse_texfn(void *ctx, int unit, float u, float v, float w, float lod, float out[4]) {   /* ctx: the stage's 16 units, GfxDraw.tex (fragment) or .vtex (vertex) */
	const GfxTexture *tx = (const GfxTexture *)ctx;
	if (unit < 0 || unit >= 16 || !tx[unit].data) { out[0] = out[1] = out[2] = 0; out[3] = 1; return; }
	const GfxTexture *t = &tx[unit];
	if (((t->u_mode >> 4) & 3) == 3) cube_texel(t, u, v, w, lod, out);   /* layout 3: a direction vector, not 2D coordinates */
	else texel_lod(t, u, v, lod, out);   /* lod: the program's explicit level/bias where it gives one (SMP lod modes), else 0 */
}
/* vertex stage: fetch this vertex's attributes into PA per the GXP's declared types (§1.11), run
 * the primary program, and snapshot its O registers (clip position + every varying it wrote). */
/* RECOMP_USSE_PROFILE=1: coarse timing of the interpreter's three phases (vertex/fragment runs,
 * plus everything else per draw -- attribute fetch, iterator packing, the shared rasteriser),
 * printed once a second on present. clock_gettime is cheap enough to leave armed behind the flag. */
static int usse_profile_on(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_USSE_PROFILE") != 0; return v; }
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static uint64_t prof_vert_ns, prof_vert_n, prof_frag_ns, prof_frag_n, prof_last_report;
static void usse_profile_report(void) {
	if (!usse_profile_on()) return;
	uint64_t t = now_ns();
	if (prof_last_report && t - prof_last_report < 1000000000ull) return;
	prof_last_report = t;
	fprintf(stderr, "    usse profile: vertex %llu calls %.2f ms total (%.2f us/call), fragment %llu calls %.2f ms total (%.2f us/call)\n",
	        (unsigned long long)prof_vert_n, prof_vert_ns / 1e6, prof_vert_n ? (double)prof_vert_ns / prof_vert_n / 1e3 : 0,
	        (unsigned long long)prof_frag_n, prof_frag_ns / 1e6, prof_frag_n ? (double)prof_frag_ns / prof_frag_n / 1e3 : 0);
	prof_vert_ns = prof_vert_n = prof_frag_ns = prof_frag_n = 0;
}
/* fetch and pack one vertex's attributes into PA, exactly as the interpreter's primary program
 * expects them (§1.11) -- shared with gfx_gpu.c, which packs the very same bytes into its vertex
 * input storage buffer instead of a local UsseInst (see usse_spirv.h's block comment on why the
 * GPU vertex shader can just read that buffer instead of re-deriving this packing in SPIR-V). */
static void fetch_vertex_pa_into(const GfxDraw *d, const UsseVertexLayout *vl, uint32_t ix, UsseInst *inst) {
	for (int i = 0; i < d->nattr; i++) {
		const GfxAttr *a = &d->attr[i];
		uint32_t at = d->stream[a->stream] + ix * d->stride[a->stream] + a->offset;
		const UsseAttrSlot *slot = usse_find_attr(vl, a->reg);
		if (!slot) continue;
		float comps[4] = {0, 0, 0, 1};   /* GPU input-assembler convention: components the stream doesn't supply default to (0,0,0,1) */
		for (int k = 0; k < a->comps && k < 4; k++) comps[k] = attr_comp(at, a->format, k);
		if (slot->untyped) {
			uint8_t raw[16]; int bytes = a->comps * attr_byte_size(a->format); if (bytes > 16) bytes = 16;
			memcpy(raw, G(at), (size_t)bytes);
			usse_pack_attr(inst, slot, comps, raw, bytes);
		} else usse_pack_attr(inst, slot, comps, 0, 0);
	}
	if (vl->index_reg >= 0 && vl->index_reg < 160) inst->pa[vl->index_reg].u = ix;
	if (vl->instance_reg >= 0 && vl->instance_reg < 160) inst->pa[vl->instance_reg].u = 0;
}
void gfx_fetch_vertex_pa(const GfxDraw *d, const UsseVertexLayout *vl, uint32_t ix, UsseInst *inst) { memset(inst, 0, sizeof *inst); fetch_vertex_pa_into(d, vl, ix, inst); }
/* the first npa PA words of a vertex, packed exactly as above, straight into `out` (the GPU
 * backend's vertex record): one static scratch instead of a 2 KB UsseInst zeroed per vertex */
void gfx_fetch_vertex_pa_words(const GfxDraw *d, const UsseVertexLayout *vl, uint32_t ix, uint32_t *out, int npa) {
	static UsseInst scratch;
	memset(scratch.pa, 0, sizeof(scratch.pa[0]) * (size_t)npa);
	fetch_vertex_pa_into(d, vl, ix, &scratch);
	for (int w = 0; w < npa; w++) out[w] = scratch.pa[w].u;
}
void gfx_vertex_packer_init(GfxVertexPacker *pk, const GfxDraw *d, const UsseVertexLayout *vl, int npa) {
	memset(pk, 0, sizeof *pk); pk->d = d; pk->vl = vl; pk->npa = npa;
	pk->index_reg = vl->index_reg >= 0 && vl->index_reg < npa ? vl->index_reg : -1;
	pk->instance_reg = vl->instance_reg >= 0 && vl->instance_reg < npa ? vl->instance_reg : -1;
	for (int i = 0; i < d->nattr && pk->n < 32; i++) {
		const GfxAttr *a = &d->attr[i];
		const UsseAttrSlot *slot = usse_find_attr(vl, a->reg);
		if (!slot) continue;
		GfxPackAttr *p = &pk->a[pk->n];
		p->base = d->stream[a->stream] + a->offset; p->stride = d->stride[a->stream]; p->fmt = a->format; p->comps = a->comps > 4 ? 4 : a->comps; p->reg = (uint16_t)slot->reg;
		if (slot->untyped) { int bytes = a->comps * attr_byte_size(a->format); p->kind = 1; p->rawlen = (uint8_t)(bytes > 16 ? 16 : bytes); }
		else if (slot->type == 6 /*TY_F32*/) { p->kind = 2; p->dcomps = (uint8_t)(slot->comps > 4 ? 4 : slot->comps); }
		else if (slot->type == 5 /*TY_F16*/) { p->kind = 3; p->dcomps = (uint8_t)(slot->comps > 4 ? 4 : slot->comps); }
		else { pk->slow = 1; return; }   /* C10/U8/... slots: the interpreter's own packer, per vertex */
		/* every written word must lie inside the record */
		int last = slot->reg + (p->kind == 1 ? (p->rawlen + 3) / 4 : p->kind == 2 ? p->dcomps : (p->dcomps + 1) / 2);
		if (last > npa || (p->kind != 1 && p->dcomps == 0)) { pk->slow = 1; return; }
		pk->n++;
	}
}
void gfx_vertex_pack(const GfxVertexPacker *pk, uint32_t ix, uint32_t *out) {
	if (pk->slow) { gfx_fetch_vertex_pa_words(pk->d, pk->vl, ix, out, pk->npa); return; }
	memset(out, 0, 4u * (size_t)pk->npa);
	for (int i = 0; i < pk->n; i++) {
		const GfxPackAttr *p = &pk->a[i];
		uint32_t at = p->base + ix * p->stride;
		if (p->kind == 1) { memcpy(out + p->reg, G(at), p->rawlen); continue; }
		float comps[4] = {0, 0, 0, 1};
		for (int k = 0; k < p->comps; k++) comps[k] = attr_comp(at, p->fmt, k);
		if (p->kind == 2) { for (int c = 0; c < p->dcomps; c++) memcpy(out + p->reg + c, &comps[c], 4); }
		else for (int c = 0; c < p->dcomps; c++) out[p->reg + c / 2] |= usse_float_to_lane(comps[c], 5 /*TY_F16*/) << (16 * (c & 1));
	}
	if (pk->index_reg >= 0) out[pk->index_reg] = ix;
	if (pk->instance_reg >= 0) out[pk->instance_reg] = 0;
}
static const UsseProgram *trace_vprog;   /* RECOMP_TRACE_DRAW: the next vertex of this program is traced (one vertex) */
static void fetch_interp(const GfxDraw *d, const UsseProgram *vprog, const UsseVertexLayout *vl, UsseDraw *vdraw, uint32_t ix, Vert *o) {
	UsseInst inst;
	gfx_fetch_vertex_pa(d, vl, ix, &inst);
	uint64_t t0 = usse_profile_on() ? now_ns() : 0;
	int tr = (vprog == trace_vprog);
	if (tr) { trace_vprog = 0; fprintf(stderr, "trace vertex %u: pa", ix); for (int i = 0; i < 12; i++) fprintf(stderr, " %08x(%g)", inst.pa[i].u, inst.pa[i].f); fprintf(stderr, "\n"); usse_set_trace(1); }
	usse_run_primary(vprog, &inst, vdraw, (void *)d->vtex, usse_texfn);
	if (tr) { usse_set_trace(0); fprintf(stderr, "trace vertex %u: o", ix); for (int i = 0; i < 8; i++) fprintf(stderr, " %08x(%g)", inst.o[i].u, inst.o[i].f); fprintf(stderr, "\n"); }
	if (t0) { prof_vert_ns += now_ns() - t0; prof_vert_n++; }
	memset(o, 0, sizeof *o);
	o->x = inst.o[0].f; o->y = inst.o[1].f; o->z = inst.o[2].f; o->w = inst.o[3].f; o->probe = inst.probe;
	int n = vl->total_o > PC_U ? PC_U : vl->total_o;
	for (int k = 0; k < n; k++) o->vary[k] = inst.o[k].f;
}
/* a draw's extent on screen: its own vertex program run here (gfx_gpu.c's native-shape HUD, which needs to know where an
 * element lands to decide what it stays anchored to) for up to `samples` of its indices, spread evenly from the first to
 * the last -- enough to place an element, a text string's first and last glyphs included. out: min x, max x, min y,
 * max y in NDC, min w, max w. 0 when the program is beyond the interpreter or a vertex is behind the eye (w <= 0). The
 * results are cached by everything they depend on (the program, its SA words after the secondary program, the sampled
 * vertices' PA words): an element unchanged from the last frame costs a hash, not a run. */
#define BOUNDS_CACHE 512
static struct { uint64_t key; float b[6]; int ok; } bounds_cache[BOUNDS_CACHE];
static uint64_t mix_words(uint64_t h, const void *p, size_t words) { const uint32_t *q = p; for (size_t i = 0; i < words; i++) { h ^= q[i]; h *= 0x9e3779b97f4a7c15ull; h ^= h >> 29; } return h; }
int gfx_draw_clip_bounds(const GfxDraw *d, int samples, float out[6]) {
	const UsseProgram *vp = d->vprog ? usse_get(d->vprog) : 0;
	if (!vp || !usse_supported(vp) || !d->count || samples < 2 || gfx_prim_kind(d->prim) == GFX_PRIM_POINTS) return 0;
	const UsseVertexLayout *vl = usse_vertex_layout(vp);
	static UsseDraw vd; static UsseInst pa[64]; UsseInst inst;   /* (static: big; the render thread's only) */
	if (samples > 64) samples = 64;
	const uint32_t n = d->count < (uint32_t)samples ? d->count : (uint32_t)samples;
	memset(&vd, 0, sizeof vd);
	usse_setup_registers(vp, &vd, d->vuniforms, d->vubuf);
	memset(&inst, 0, sizeof inst); usse_run_secondary(vp, &inst, &vd);
	const int npa = usse_program_pa_words(vp) > 0 && usse_program_pa_words(vp) <= 160 ? usse_program_pa_words(vp) : 160;
	uint64_t key = mix_words(0x84222325cbf29ce4ull ^ d->vprog, vd.sa, sizeof vd.sa / 4);
	for (uint32_t k = 0; k < n; k++) {
		uint32_t i = n == 1 ? 0 : (uint32_t)((uint64_t)k * (d->count - 1) / (n - 1));
		gfx_fetch_vertex_pa(d, vl, index_at(d, i), &pa[k]);
		key = mix_words(key, pa[k].pa, (size_t)npa);
	}
	int slot = (int)(key % BOUNDS_CACHE);
	if (bounds_cache[slot].key == key) { memcpy(out, bounds_cache[slot].b, sizeof bounds_cache[slot].b); return bounds_cache[slot].ok; }
	float b[6] = { 1e30f, -1e30f, 1e30f, -1e30f, 1e30f, -1e30f };
	int ok = 1;
	for (uint32_t k = 0; k < n && ok; k++) {
		usse_run_primary(vp, &pa[k], &vd, (void *)d->vtex, usse_texfn);
		float x = pa[k].o[0].f, y = pa[k].o[1].f, w = pa[k].o[3].f;
		if (!(w > 0) || !isfinite(x) || !isfinite(y) || !isfinite(w)) { ok = 0; break; }
		x /= w; y /= w;
		if (x < b[0]) b[0] = x; if (x > b[1]) b[1] = x; if (y < b[2]) b[2] = y; if (y > b[3]) b[3] = y; if (w < b[4]) b[4] = w; if (w > b[5]) b[5] = w;
	}
	bounds_cache[slot].key = key; bounds_cache[slot].ok = ok; memcpy(bounds_cache[slot].b, b, sizeof b);
	memcpy(out, b, sizeof b);
	return ok;
}
struct GfxBoundsJob {
	const UsseProgram *vp; UsseDraw vd; GfxTexture vtex[16];
	uint32_t *pa; int cap, nv, npa;   /* nv distinct vertices' first npa PA words */
	uint64_t key; int ok, cache; float b[6];
};
GfxBoundsJob *gfx_bounds_job_new(void) { return calloc(1, sizeof(GfxBoundsJob)); }
int gfx_bounds_prepare(GfxBoundsJob *j, const GfxDraw *d) {
	const UsseProgram *vp = d->vprog ? usse_get(d->vprog) : 0;
	if (!vp || !usse_supported(vp) || !d->count || d->count > GFX_BOUNDS_MAX || gfx_prim_kind(d->prim) == GFX_PRIM_POINTS) return 0;
	const UsseVertexLayout *vl = usse_vertex_layout(vp);
	UsseInst inst;
	j->vp = vp; memcpy(j->vtex, d->vtex, sizeof j->vtex);
	memset(&j->vd, 0, sizeof j->vd);
	usse_setup_registers(vp, &j->vd, d->vuniforms, d->vubuf);
	memset(&inst, 0, sizeof inst); usse_run_secondary(vp, &inst, &j->vd);
	j->npa = usse_program_pa_words(vp) > 0 && usse_program_pa_words(vp) <= 160 ? usse_program_pa_words(vp) : 160;
	if (j->cap < GFX_BOUNDS_MAX * j->npa) { j->cap = GFX_BOUNDS_MAX * j->npa; free(j->pa); j->pa = malloc((size_t)j->cap * 4); if (!j->pa) { j->cap = 0; return 0; } }
	uint32_t seen[2 * GFX_BOUNDS_MAX]; memset(seen, 0xff, sizeof seen);   /* (open addressing over the indices: a quad's 6 are 4 vertices) */
	uint64_t key = mix_words(0x3c6ef372fe94f82bull ^ d->vprog, j->vd.sa, sizeof j->vd.sa / 4);
	j->nv = 0;
	for (uint32_t i = 0; i < d->count; i++) {
		uint32_t ix = index_at(d, i), h = (ix * 0x9e3779b1u) % (2 * GFX_BOUNDS_MAX);
		while (seen[h] != 0xffffffffu && seen[h] != ix) h = (h + 1) % (2 * GFX_BOUNDS_MAX);
		if (seen[h] == ix) continue;
		seen[h] = ix;
		uint32_t *w = &j->pa[(size_t)j->nv++ * j->npa];
		gfx_fetch_vertex_pa_words(d, vl, ix, w, j->npa);
		key = mix_words(key, w, (size_t)j->npa);
	}
	j->key = key;
	int slot = (int)(key % BOUNDS_CACHE);
	const int cache = !usse_primary_reads_memory(vp);   /* (an LDST's bytes, a skinned mesh's bones, aren't in the key) */
	if (cache && bounds_cache[slot].key == key) { j->ok = bounds_cache[slot].ok; memcpy(j->b, bounds_cache[slot].b, sizeof j->b); return 2; }
	j->cache = cache;
	return 1;
}
void gfx_bounds_run(GfxBoundsJob *j) {
	float b[6] = { 1e30f, -1e30f, 1e30f, -1e30f, 1e30f, -1e30f };
	int ok = 1;
	UsseInst inst;
	for (int k = 0; k < j->nv && ok; k++) {
		memset(&inst, 0, sizeof inst);
		for (int w = 0; w < j->npa; w++) inst.pa[w].u = j->pa[(size_t)k * j->npa + w];
		usse_run_primary(j->vp, &inst, &j->vd, (void *)j->vtex, usse_texfn);
		float x = inst.o[0].f, y = inst.o[1].f, w = inst.o[3].f;
		if (!(w > 0) || !isfinite(x) || !isfinite(y) || !isfinite(w)) { ok = 0; break; }
		x /= w; y /= w;
		if (x < b[0]) b[0] = x; if (x > b[1]) b[1] = x; if (y < b[2]) b[2] = y; if (y > b[3]) b[3] = y; if (w < b[4]) b[4] = w; if (w > b[5]) b[5] = w;
	}
	j->ok = ok; memcpy(j->b, b, sizeof b);
}
int gfx_bounds_result(GfxBoundsJob *j, float out[6]) {
	int slot = (int)(j->key % BOUNDS_CACHE);
	if (j->cache) { bounds_cache[slot].key = j->key; bounds_cache[slot].ok = j->ok; memcpy(bounds_cache[slot].b, j->b, sizeof j->b); }
	memcpy(out, j->b, sizeof j->b);
	return j->ok;
}
/* which O-register range (of the VERTEX program's own output layout, §4.1) feeds a given
 * fragment-iterator semantic (§1.12's semantic numbering, usse.h); -1 if the vertex never wrote it */
static int vertex_o_for_semantic(const UsseVertexLayout *vl, int semantic, int *ncomp) {
	if (semantic >= 0 && semantic <= 9) { *ncomp = vl->ncomp_tex[semantic]; return vl->o_tex[semantic]; }
	if (semantic == 10) { *ncomp = 4; return vl->o_color0; }
	if (semantic == 11) { *ncomp = 4; return vl->o_color1; }
	if (semantic == 12) { *ncomp = 2; return vl->o_fog; }
	if (semantic == 13) { *ncomp = 4; return 0; }   /* POSITION: always O0-3 */
	return -1;
}
/* the divisor of a projective non-dependent read (docs/usse-isa.md, attribute_info bits 9:8): mode 2 is
 * ".xyw", the 4th component (screen-space reads of a G-buffer from a light volume's clip position),
 * mode 3 ".xyz", the 3rd. Dividing by z for both put every lighting read out of range: Unit 13's
 * level lit as a gradient of its G-buffer's clamped edge column. */
static float proj_divisor(const UsseIter *it, const float vary[MAX_VARY], int base, int ncomp) {
	if (it->tex_proj == 2 && ncomp > 3) return vary[base + 3];
	if (it->tex_proj >= 2 && ncomp > 2) return vary[base + 2];
	return 1;
}
/* a non-dependent texture read (§1.12 B): the coordinate is an already-interpolated vertex
 * texcoord output, not something the fragment program computes. */
/* an iterator's texture coordinate from a set of (perspective-corrected) varyings */
static void iter_uv(const UsseIter *it, const UsseVertexLayout *vl, const float vary[MAX_VARY], float *u, float *v) {
	if (it->point_coord) { *u = vary[PC_U]; *v = vary[PC_V]; return; }   /* a point sprite's own 0..1 */
	int base = (it->tex_coord_src >= 0 && it->tex_coord_src < 10) ? vl->o_tex[it->tex_coord_src] : -1;
	*u = 0; *v = 0;
	if (base < 0) return;
	int ncomp = vl->ncomp_tex[it->tex_coord_src]; float w = proj_divisor(it, vary, base, ncomp);
	*u = vary[base]; if (ncomp > 1) *v = vary[base + 1];
	if (it->tex_proj >= 2 && w != 0) { *u /= w; *v /= w; }
}
/* a cube iterator's direction: its texcoord's three components, unprojected */
static void iter_dir(const UsseIter *it, const UsseVertexLayout *vl, const float vary[MAX_VARY], float c[3]) {
	int base = (it->tex_coord_src >= 0 && it->tex_coord_src < 10) ? vl->o_tex[it->tex_coord_src] : -1, nc = base >= 0 ? vl->ncomp_tex[it->tex_coord_src] : 0;
	for (int k = 0; k < 3; k++) c[k] = nc > k ? vary[base + k] : 0;
}
static void sample_nondep(const GfxDraw *d, const UsseIter *it, const UsseVertexLayout *vl, const float vary[MAX_VARY], float lod, float out[4]) {
	if (it->tex_unit >= 0 && it->tex_unit < 16 && d->tex[it->tex_unit].data && ((d->tex[it->tex_unit].u_mode >> 4) & 3) == 3 && !it->point_coord) {
		/* a cube map: the texcoord is a direction (its three components, unprojected), as SMP reads one (usse_texfn) --
		 * sampled as a 2D texture before, and the GPU translator emitted a 2D coordinate for a cube unit that Metal
		 * refused (Killzone's first campaign level) */
		float c[3]; iter_dir(it, vl, vary, c);
		cube_texel(&d->tex[it->tex_unit], c[0], c[1], c[2], lod, out);   /* lod: iter_lod's, on the face */
		return;
	}
	float u, v; iter_uv(it, vl, vary, &u, &v);
	if (it->tex_unit >= 0 && it->tex_unit < 16 && d->tex[it->tex_unit].data) texel_lod(&d->tex[it->tex_unit], u, v, lod, out);
	else { out[0] = out[1] = out[2] = 0; out[3] = 1; }
}
/* the perspective-correct varyings at barycentrics (b0,b1,b2) of a triangle */
static void vary_at(const Tri *t, float b0, float b1, float b2, float vary[MAX_VARY]) {
	const Vert *v0 = &t->v[0], *v1 = &t->v[1], *v2 = &t->v[2];
	float pb0 = b0 * v0->invw, pb1 = b1 * v1->invw, pb2 = b2 * v2->invw, psum = pb0 + pb1 + pb2;
	float c0 = psum != 0 ? pb0 / psum : b0, c1 = psum != 0 ? pb1 / psum : b1, c2 = psum != 0 ? pb2 / psum : b2;
	for (int k = 0; k < MAX_VARY; k++) vary[k] = c0 * v0->vary[k] + c1 * v1->vary[k] + c2 * v2->vary[k];
}
/* the level of detail of an iterator's texture at this pixel: log2 of the longer screen-space
 * derivative of its texel coordinate, from the varyings one pixel to the right and one down
 * (the barycentric gradients are constant over the triangle) -- the quad derivatives the
 * hardware's texture unit takes. 0 when the texture is magnified. */
static float iter_lod(const Tri *t, const UsseIter *it, const UsseVertexLayout *vl, const GfxTexture *tex, float b0, float b1, float b2, const float vary[MAX_VARY]) {
	if (mip_levels(tex) <= 1) return 0;
	float db0dx = (t->sy[1] - t->sy[2]) * t->inv_area, db1dx = (t->sy[2] - t->sy[0]) * t->inv_area;
	float db0dy = (t->sx[2] - t->sx[1]) * t->inv_area, db1dy = (t->sx[0] - t->sx[2]) * t->inv_area;
	float vx[MAX_VARY], vy[MAX_VARY], u, v, ux, vxx, uy, vyy;
	vary_at(t, b0 + db0dx, b1 + db1dx, b2 - db0dx - db1dx, vx);
	vary_at(t, b0 + db0dy, b1 + db1dy, b2 - db0dy - db1dy, vy);
	float w = tex->width ? tex->width : 1, h = tex->height ? tex->height : 1;
	if (((tex->u_mode >> 4) & 3) == 3 && !it->point_coord) {   /* a cube map: the direction's coordinates on this pixel's face, its neighbours' on the same face */
		float c[3], cx[3], cy[3]; iter_dir(it, vl, vary, c); iter_dir(it, vl, vx, cx); iter_dir(it, vl, vy, cy);
		int face = cube_face_of(c[0], c[1], c[2]); if (face < 0) return 0;
		cube_face_uv(face, c[0], c[1], c[2], &u, &v); cube_face_uv(face, cx[0], cx[1], cx[2], &ux, &vxx); cube_face_uv(face, cy[0], cy[1], cy[2], &uy, &vyy);
		w *= 0.5f; h *= 0.5f;   /* face coordinates span 2 */
	} else { iter_uv(it, vl, vary, &u, &v); iter_uv(it, vl, vx, &ux, &vxx); iter_uv(it, vl, vy, &uy, &vyy); }
	float dx = sqrtf(((ux - u) * w) * ((ux - u) * w) + ((vxx - v) * h) * ((vxx - v) * h));
	float dy = sqrtf(((uy - u) * w) * ((uy - u) * w) + ((vyy - v) * h) * ((vyy - v) * h));
	float rho = dx > dy ? dx : dy;
	return rho > 0 && isfinite(rho) ? log2f(rho) : 0;   /* negative when magnified (texel_lod adds the texture's LOD bias to it) */
}
/* fragment stage for one covered pixel of an interpreter-shaded triangle: perspective-correct
 * interpolation of the vertex's varyings into PA per the fragment's iterator table (§1.12), the
 * primary program, then the colour (and optional KILL / DEPTHF) it produced. */
/* a colour output declared as raw 32-bit words (U32/S32: Unit 13's G-buffer packs bytes into two
 * words for its F32F32 surface) is stored bit for bit, as the hardware's pixel back end stores
 * output-register bits -- converting it as a normalized integer destroyed every packed byte and the
 * lighting that reads them came out magenta. raw/nraw: those words, or nraw 0 for a value colour */
static void shade_interp(const Tri *t, const DrawRec *r, int x, int y, float b0, float b1, float b2, float *z, float c[4], int *discard, uint32_t raw[4], int *nraw) {
	const Vert *v0 = &t->v[0], *v1 = &t->v[1], *v2 = &t->v[2];
	float pb0 = b0 * v0->invw, pb1 = b1 * v1->invw, pb2 = b2 * v2->invw, psum = pb0 + pb1 + pb2;
	float c0 = psum != 0 ? pb0 / psum : b0, c1 = psum != 0 ? pb1 / psum : b1, c2 = psum != 0 ? pb2 / psum : b2;
	float vary[MAX_VARY];
	for (int k = 0; k < MAX_VARY; k++) vary[k] = c0 * v0->vary[k] + c1 * v1->vary[k] + c2 * v2->vary[k];
	UsseInst finst; memset(&finst, 0, sizeof finst);
	const UsseFragLayout *fl = r->flayout;
	for (int i = 0; i < fl->niters; i++) {
		const UsseIter *it = &fl->iters[i];
		float comps[4] = {0, 0, 0, 1};
		int base = -1, ncomp = 0;
		if (it->is_tex) {
			if (it->result_fmt == 0) {   /* the native packed texel (see usse_texfn_raw) */
				int tb = (it->tex_coord_src >= 0 && it->tex_coord_src < 10) ? r->vlayout->o_tex[it->tex_coord_src] : -1;
				float u = 0, v = 0, w = 1; uint32_t raw[4] = {0, 0, 0, 0};
				if (it->point_coord) { u = vary[PC_U]; v = vary[PC_V]; }
				else if (tb >= 0) { int nc = r->vlayout->ncomp_tex[it->tex_coord_src]; u = vary[tb]; if (nc > 1) v = vary[tb + 1]; w = proj_divisor(it, vary, tb, nc); }
				if (it->tex_proj >= 2 && w != 0) { u /= w; v /= w; }
				int n = usse_texfn_raw((void *)r->d.tex, it->tex_unit, u, v, w, 0, raw);
				if (n) { usse_pack_iter_raw(&finst, it, raw, n); continue; }
			}
			float lod = (it->tex_unit >= 0 && it->tex_unit < 16 && r->d.tex[it->tex_unit].data) ? iter_lod(t, it, r->vlayout, &r->d.tex[it->tex_unit], b0, b1, b2, vary) : 0;
			sample_nondep(&r->d, it, r->vlayout, vary, lod, comps);
		} else if (it->point_coord) { comps[0] = vary[PC_U]; comps[1] = vary[PC_V]; comps[2] = 0; comps[3] = 1; }
		else if (it->semantic == 13) { comps[0] = (float)x + 0.5f; comps[1] = (float)y + 0.5f; comps[2] = *z; comps[3] = psum; }   /* POSITION: the fragment's window position -- pixel centre, depth, 1/w (Vita3K: gl_FragCoord). Read as the interpolated clip position, Killzone's quarter-res downscale built its HDR read addresses from (-0.16, 0.11) and loaded zeros */
		else { base = vertex_o_for_semantic(r->vlayout, it->semantic, &ncomp); if (base >= 0) for (int k = 0; k < ncomp && k < 4; k++) comps[k] = vary[base + k]; }
		usse_pack_iter(&finst, it, comps);
	}
	UsseDraw *fdraw = (UsseDraw *)&r->fdraw;   /* shared per draw (SA is read-only here); cast drops const */
	/* FRAGCOLOR_USED: the program reads the pixel's current colour (programmable blending -- Unit 13's
	 * occlusion pass multiplies the lit HDR buffer by its half-res AO; without the preload it
	 * multiplied by zero and the whole frame went black under the later passes) */
	if (fl->frag_color_used) { float cur[4]; target_read(x, y, cur); usse_preload_color(&finst, fl, cur); }
	uint64_t t0 = usse_profile_on() ? now_ns() : 0;
	/* RECOMP_TRACE_DRAW=k with RECOMP_DEBUG_PIXEL=x,y: that pixel's fragment program instruction by
	 * instruction (the vertex trace above covers vertex 0 only; a text glyph's alpha went negative
	 * somewhere in Killzone's 30-instruction text shader and this is how it was found) */
	int ftr = 0;
	{ static long td = -2; if (td == -2) td = getenv("RECOMP_TRACE_DRAW") ? atol(getenv("RECOMP_TRACE_DRAW")) : -1;
	  int px, py; debug_pixel(&px, &py); ftr = td == r->draw_index && px >= 0 && x == px && y == py; }
	if (ftr) { fprintf(stderr, "trace draw %d: fragment primary at %d,%d: pa", r->draw_index, x, y); for (int i = 0; i < 16; i++) fprintf(stderr, " %08x", finst.pa[i].u); fprintf(stderr, "\n"); usse_set_trace(1); }
	usse_run_primary(r->fprog, &finst, fdraw, (void *)r->d.tex, usse_texfn);
	if (ftr) { usse_set_trace(0); fprintf(stderr, "trace draw %d: fragment o", r->draw_index); for (int i = 0; i < 8; i++) fprintf(stderr, " %08x(%g)", finst.o[i].u, finst.o[i].f); fprintf(stderr, "\n"); }
	if (t0) { prof_frag_ns += now_ns() - t0; prof_frag_n++; }
	*discard = finst.killed;
	if (finst.depth_written) *z = finst.depth;
	usse_read_color(&finst, fl, c);
	int probed = 0;
	{	/* RECOMP_PROBE=t|p|o:<word>[@<instr>] (see README, "register probes"): the fragment's colour becomes
		 * that register word -- its two F16 lanes as (x*scale*0.5+0.5, y*scale*0.5+0.5, 0.5, 1), or with
		 * RECOMP_PROBE_F32=1 the word as one float -- read after primary instruction <instr> (usse.c
		 * snapshots it) or at the end. usse_spirv.c emits the same for the GPU path, so the two
		 * backends' dumps of a draw compare any intermediate register, not just the final colour. */
		static const char *pr = 0; static int pri = 0; static float sc = 1; static int f32 = 0;
		if (!pri) { pri = 1; pr = getenv("RECOMP_PROBE"); if (getenv("RECOMP_PROBE_SCALE")) sc = (float)atof(getenv("RECOMP_PROBE_SCALE")); f32 = getenv("RECOMP_PROBE_F32") != 0; }
		static long pf = -1; if (pf < 0) { const char *e = getenv("RECOMP_PROBE_FPROG"); pf = e ? (long)strtoul(e, 0, 16) : 0; }   /* RECOMP_PROBE_FPROG=<hex addr>: that fragment program only (gfx_gpu.c the same) */
		probed = pr && pr[1] == ':' && (!pf || r->d.fprog == (uint32_t)pf);
		if (probed) {
			const UWord *b = pr[0] == 't' ? finst.r : pr[0] == 'p' ? finst.pa : finst.o;
			uint32_t w = (pr[0] >= 'A' && pr[0] <= 'Z') ? t->v[0].probe : strchr(pr, '@') ? finst.probe : b[atoi(pr + 2) % 160].u;   /* upper case: the vertex program's word, from the triangle's first vertex (flat) */
			if (getenv("RECOMP_PROBE_U32")) { c[0] = (w & 0xffff) / 65536.f; c[1] = (w >> 16) / 65536.f; }   /* the raw word's halves (addresses, indices) */
			else if (f32) { float fw; memcpy(&fw, &w, 4); c[0] = fw * sc * 0.5f + 0.5f; c[1] = 0.5f; }
			else { c[0] = half_to_float_soft((uint16_t)(w & 0xffff)) * sc * 0.5f + 0.5f; c[1] = half_to_float_soft((uint16_t)(w >> 16)) * sc * 0.5f + 0.5f; }
			c[2] = 0.5f; c[3] = 1;
		}
	}
	{   /* RECOMP_DEBUG_DRAW=k: that draw's inputs and output at a grid of pixels; =all with RECOMP_DEBUG_PIXEL:
		 * every draw that shades that pixel (which draws wrote it -- its history through the frame) */
		static long dbg = -2; if (dbg == -2) { const char *e = getenv("RECOMP_DEBUG_DRAW"); dbg = !e ? -1 : !strcmp(e, "all") ? -3 : atol(e); }
		int px, py; debug_pixel(&px, &py);
		if ((dbg == r->draw_index || (dbg == -3 && px >= 0)) && (px >= 0 ? (x == px && y == py) : (x % 180 == 90 && y % 102 == 51))) {
			static const void *sa_shown; if (sa_shown != (const void *)r && dbg >= 0) {   /* the fragment program's secondary attributes (its uniforms), once per draw (a capture's every frame has a draw k) */
				sa_shown = r; fprintf(stderr, "debug draw %ld fragment SA:", dbg);
				for (int i = 0; i < 40; i++) fprintf(stderr, "%s%2d=%08x(%g)", i % 8 ? " " : "\n   ", i, r->fdraw.sa[i].u, r->fdraw.sa[i].f);
				fprintf(stderr, "\n");
			}
			fprintf(stderr, "debug draw %d pixel %d,%d f %#x (%d iterators, %s):", r->draw_index, x, y, r->d.fprog, fl->niters, r->use_interp ? "interp" : "stand-in");
			for (int i = 0; i < fl->niters; i++) {
				const UsseIter *it = &fl->iters[i]; float comps[4] = {0, 0, 0, 1}; int base, ncomp;
				if (it->is_tex) sample_nondep(&r->d, it, r->vlayout, vary, 0, comps);
				else { base = vertex_o_for_semantic(r->vlayout, it->semantic, &ncomp); if (base >= 0) for (int k = 0; k < ncomp && k < 4; k++) comps[k] = vary[base + k]; }
				fprintf(stderr, "  %s%d=(%g %g %g %g)", it->is_tex ? "tex" : "var", it->is_tex ? it->tex_unit : it->semantic, comps[0], comps[1], comps[2], comps[3]);
			}
			fprintf(stderr, "  -> (%g %g %g %g)\n", c[0], c[1], c[2], c[3]);
		}
	}
	*nraw = 0;
	if (!probed && (fl->color_type == 8 /* TY_U32 */ || fl->color_type == 9 /* TY_S32 */)) {   /* a probe shows as floats even on a raw-word target */
		const UWord *bank = fl->native_color ? finst.o : finst.pa;
		int n = fl->color_comps > 0 && fl->color_comps <= 4 ? fl->color_comps : 1;
		for (int k = 0; k < n; k++) raw[k] = bank[(fl->color_pa_start + k) % 160].u;
		*nraw = n;
	}
}
/* those words into the pixel, as many bytes as the surface holds */
static void target_write_raw(int x, int y, const uint32_t *raw, int nraw) {
	int bpp = target_bpp_of(&target);
	uint8_t bytes[16] = {0}; memcpy(bytes, raw, 4u * (unsigned)nraw);
	uint32_t a = target.data + (uint32_t)bpp * target_index(&target, x, y);
	memcpy(G(a), bytes, (size_t)bpp);
}

static void raster_rows(const Tri *t, const DrawRec *r, int ylo, int yhi) {
	const GfxDraw *d = &r->d; const Shader *s = &r->s;
	const Vert *v0 = &t->v[0], *v1 = &t->v[1], *v2 = &t->v[2];
	const float *sx = t->sx, *sy = t->sy;
	int y0 = t->y0 > ylo ? t->y0 : ylo, y1 = t->y1 < yhi ? t->y1 : yhi;
	Depth *dp = scene_depth;
	/* RECOMP_PIXEL_CENTER: where in the pixel the sample point sits (default 0.5, the GL/D3D10 convention) */
	static float centre = -1; if (centre < 0) centre = getenv("RECOMP_PIXEL_CENTER") ? (float)atof(getenv("RECOMP_PIXEL_CENTER")) : .5f;
	float zbias = 0;   /* polygon offset (GfxDraw.depth_bias_*): factor * the steeper of z's screen slopes + units * 2^-24 (a 24-bit depth step) */
	if (d->depth_bias_factor || d->depth_bias_units) {
		float a = t->inv_area, dzdx = (sy[1] - sy[2]) * a * (v0->z - v2->z) + (sy[2] - sy[0]) * a * (v1->z - v2->z);
		float dzdy = (sx[2] - sx[1]) * a * (v0->z - v2->z) + (sx[0] - sx[2]) * a * (v1->z - v2->z);
		float m = fabsf(dzdx) > fabsf(dzdy) ? fabsf(dzdx) : fabsf(dzdy);
		zbias = (float)d->depth_bias_factor * m + (float)d->depth_bias_units * (1.0f / 16777216.0f);
	}
	for (int y = y0; y < y1; y++)
		for (int x = t->x0; x < t->x1; x++) {
			float px = x + centre, py = y + centre;
			float b0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) * t->inv_area;
			float b1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) * t->inv_area;
			float b2 = 1 - b0 - b1;
			if (b0 < 0 || b1 < 0 || b2 < 0) continue;
			ST(r, st_cov);
			float z = 0, *dz = 0;
			uint8_t *sp = 0;
			if (dp) {
				int do_depth = scene_has_depth_plane, do_stencil = scene_has_stencil_plane;
				if (do_depth) { z = b0 * v0->z + b1 * v1->z + b2 * v2->z + zbias; dz = &dp->z[y * dp->w + x]; }   /* v.z is z/w already */
				if (do_stencil) sp = &dp->st[y * dp->w + x];
				uint8_t m = d->stencil_cmask, ref = d->stencil_ref;
				if (do_stencil && !depth_pass(d->stencil_func, (float)(ref & m), (float)(*sp & m))) {
					*sp = (*sp & ~d->stencil_wmask) | (stencil_op(d->stencil_fail, *sp, ref) & d->stencil_wmask);
					ST(r, st_sfail);
					continue;
				}
				if (do_depth && !depth_pass(d->depth_func, z, *dz)) {
					if (do_stencil) *sp = (*sp & ~d->stencil_wmask) | (stencil_op(d->depth_fail, *sp, ref) & d->stencil_wmask);
					ST(r, st_dfail);
					continue;
				}
			}
			float c[4]; uint32_t raw[4]; int nraw = 0;
			if (r->use_interp) {
				int discard = 0;
				shade_interp(t, r, x, y, b0, b1, b2, &z, c, &discard, raw, &nraw);
				if (discard) { ST(r, st_discard); continue; }
			} else {
				float col[4] = {b0 * v0->r + b1 * v1->r + b2 * v2->r, b0 * v0->g + b1 * v1->g + b2 * v2->g,
				                b0 * v0->b + b1 * v1->b + b2 * v2->b, b0 * v0->a + b1 * v1->a + b2 * v2->a};
				if (s->has_clear) memcpy(c, r->clear, sizeof c);
				else if (s->sampler) {
					texel(&d->tex[0], b0 * v0->u + b1 * v1->u + b2 * v2->u, b0 * v0->v + b1 * v1->v + b2 * v2->v, c);
					if (s->col_reg >= 0) for (int k = 0; k < 4; k++) c[k] *= col[k];
					if (s->has_add) for (int k = 0; k < 3; k++) c[k] += r->add[k];
					if (s->has_tint) for (int k = 0; k < 4; k++) c[k] *= r->tint[k];
				} else memcpy(c, col, sizeof c);
				/* sprite shaders discard fully transparent texels (an alpha test): no colour, no depth */
				if (s->sampler && c[3] <= 0.0f) { ST(r, st_discard); continue; }
			}
			if (dz && d->depth_write) *dz = z;
			if (sp) *sp = (*sp & ~d->stencil_wmask) | (stencil_op(d->depth_pass, *sp, d->stencil_ref) & d->stencil_wmask);
			if (d->vis_addr) __atomic_fetch_add(&((DrawRec *)r)->vis_count, 1, __ATOMIC_RELAXED);
			/* sceGxmSetFrontFragmentProgramEnable(..., DISABLED): depth/stencil above still ran
			 * (matching real hardware: a depth/shadow prepass that writes no colour at all) */
			if (d->fp_disabled || !target.data) { ST(r, st_nocolor); continue; }   /* (no colour surface: a depth-only scene, the shader ran for its discards) */
			ST(r, st_written);
			if (nraw) {   /* raw output words: stored as they are; blending reads them back in the surface's format */
				float dst[4];
				if (d->blend.present) target_read(x, y, dst);
				target_write_raw(x, y, raw, nraw);
				if (!d->blend.present) continue;
				target_read(x, y, c);
				float out[4]; blend_apply(&d->blend, c, dst, out);
				target_write(x, y, out);
				continue;
			}
			if (d->blend.present) {
				float dst[4], out[4];
				target_read(x, y, dst);
				blend_apply(&d->blend, c, dst, out);
				memcpy(c, out, sizeof c);
				if ((d->blend.mask & 0xf) != 0xf) {   /* SceGxmColorMask: A=1 R=2 G=4 B=8 */
					static const uint8_t bit[4] = {2, 4, 8, 1};
					for (int k = 0; k < 4; k++) if (!(d->blend.mask & bit[k])) c[k] = dst[k];
				}
			}
			target_write(x, y, c);
		}
}

/* triangle setup at record time: screen space, bounds, z/w */
#define NEAR_W_EPS 1e-5f
/* linear interpolation of every per-vertex attribute (clip x/y/z/w, the legacy u/v/rgba, every
 * interpreter varying): all of them are affine in the triangle's own parameterisation before the
 * perspective divide, the same reasoning that lets shade_interp interpolate them across a whole
 * triangle -- so lerping along one clipped edge is exact, not an approximation. invw is left for
 * record_tri_clipped to fill in once w is final. */
static Vert vert_lerp(const Vert *p, const Vert *q, float t) {
	Vert r;
	r.x = p->x + t * (q->x - p->x); r.y = p->y + t * (q->y - p->y);
	r.z = p->z + t * (q->z - p->z); r.w = p->w + t * (q->w - p->w);
	r.u = p->u + t * (q->u - p->u); r.v = p->v + t * (q->v - p->v);
	r.r = p->r + t * (q->r - p->r); r.g = p->g + t * (q->g - p->g);
	r.b = p->b + t * (q->b - p->b); r.a = p->a + t * (q->a - p->a);
	r.invw = 0;
	for (int k = 0; k < MAX_VARY; k++) r.vary[k] = p->vary[k] + t * (q->vary[k] - p->vary[k]);
	return r;
}
/* Sutherland-Hodgman against the single near plane w > NEAR_W_EPS, in clip space (before the
 * perspective divide) -- the plane a triangle that straddles w == 0 needs clipping against: past
 * it, x/w and y/w blow up or invert sign, so a tri with one vertex behind the eye used to span (and
 * invert winding across) the whole screen instead of being clipped to what's actually visible. */
static int clip_near(const Vert *in[3], Vert out[4]) {
	int n = 0;
	for (int i = 0; i < 3; i++) {
		const Vert *cur = in[i], *nxt = in[(i + 1) % 3];
		int cur_in = cur->w > NEAR_W_EPS, nxt_in = nxt->w > NEAR_W_EPS;
		if (cur_in) out[n++] = *cur;
		if (cur_in != nxt_in) out[n++] = vert_lerp(cur, nxt, (NEAR_W_EPS - cur->w) / (nxt->w - cur->w));
	}
	return n;
}
static void record_tri_clipped(const Vert *a, const Vert *b, const Vert *c3) {
	if ((!scene_target.data && !scene_depth) || !scene_target.width) return;   /* nothing to draw into (a depth-only scene still has its depth) */
	DrawRec *rec = &recs[nrecs - 1];   /* record_tri always runs for the draw render() just pushed */
	const GfxDraw *d = &rec->d;
	Tri t;
	const Vert *v[3] = {a, b, c3};
	float xn[3], yn[3];
	for (int i = 0; i < 3; i++) {
		t.v[i] = *v[i];
		float w = v[i]->w ? v[i]->w : 1;
		xn[i] = v[i]->x / w; yn[i] = v[i]->y / w; float zn = v[i]->z / w;
		/* sceGxmSetViewport: screen = offset + scale * ndc (verified against the real driver, see
		 * gxm.c); SCE_GXM_VIEWPORT_DISABLED: the clip position IS the screen position, untransformed.
		 * No viewport ever set (an old capture): the old implicit full-target mapping, unchanged. */
		if (d->viewport_set && d->viewport_disabled) { t.sx[i] = xn[i]; t.sy[i] = yn[i]; }
		else if (d->viewport_set) { t.sx[i] = d->vp_xoff + d->vp_xscale * xn[i]; t.sy[i] = d->vp_yoff + d->vp_yscale * yn[i]; }
		else { t.sx[i] = (xn[i] + 1) * 0.5f * scene_target.width; t.sy[i] = (1 - yn[i]) * 0.5f * scene_target.height; }
		t.v[i].z = (d->viewport_set && !d->viewport_disabled) ? d->vp_zoff + d->vp_zscale * zn : zn;
		t.v[i].invw = 1.0f / w;   /* for perspective-correct interpolation of shader varyings (interp path only) */
	}
	/* SceGxmCullMode: in NDC (x right, y up -- independent of whatever the viewport does with it,
	 * so this doesn't need to know whether/how one is set), a positive signed area is CCW; the real
	 * driver's CULL_CW/CULL_CCW name the winding to cull by that same (clip-space, pre-viewport)
	 * sense (Vita3K's gxm_to_vulkan.cpp maps CULL_CW to culling the winding opposite its own
	 * CCW-is-front pipeline convention, i.e. literally "cull clockwise"). */
	if (d->cull == 1 || d->cull == 2) {
		float area_ndc = (xn[1] - xn[0]) * (yn[2] - yn[0]) - (xn[2] - xn[0]) * (yn[1] - yn[0]);
		if ((d->cull == 1 && area_ndc < 0) || (d->cull == 2 && area_ndc > 0)) { ST(rec, st_culled); return; }
	}
	float area = (t.sx[1] - t.sx[0]) * (t.sy[2] - t.sy[0]) - (t.sx[2] - t.sx[0]) * (t.sy[1] - t.sy[0]);
	if (area == 0) { ST(rec, st_zero); return; }
	t.inv_area = 1.0f / area;
	t.x0 = (int)fminf(fminf(t.sx[0], t.sx[1]), t.sx[2]); t.x1 = (int)fmaxf(fmaxf(t.sx[0], t.sx[1]), t.sx[2]) + 1;
	t.y0 = (int)fminf(fminf(t.sy[0], t.sy[1]), t.sy[2]); t.y1 = (int)fmaxf(fmaxf(t.sy[0], t.sy[1]), t.sy[2]) + 1;
	if (t.x0 < 0) t.x0 = 0; if (t.y0 < 0) t.y0 = 0;
	if (t.x1 > (int)scene_target.width) t.x1 = scene_target.width;
	if (t.y1 > (int)scene_target.height) t.y1 = scene_target.height;
	/* sceGxmSetRegionClip: ALL clips the whole draw; OUTSIDE (the common case) scissors to the box
	 * (xMin/yMin/xMax/yMax inclusive); NONE (default, 0: an old capture too) leaves the bbox alone.
	 * INSIDE (clip what's inside the box, keep what's outside) isn't a single rectangle and isn't
	 * modelled here -- rare, and this bbox-scissor model has no way to represent it. */
	if (d->clip_mode == 0x40000000u) { ST(rec, st_clipped); return; }
	if (d->clip_mode == 0x80000000u) {
		if ((int)d->clip_xmin > t.x0) t.x0 = (int)d->clip_xmin;
		if ((int)d->clip_ymin > t.y0) t.y0 = (int)d->clip_ymin;
		if ((int)d->clip_xmax + 1 < t.x1) t.x1 = (int)d->clip_xmax + 1;
		if ((int)d->clip_ymax + 1 < t.y1) t.y1 = (int)d->clip_ymax + 1;
	}
	if (t.x0 >= t.x1 || t.y0 >= t.y1) { ST(rec, st_clipped); return; }
	t.draw = nrecs - 1;
	{   /* RECOMP_DEBUG_DRAW=k: that draw's triangle depths and the scene depth buffer's range (why does its depth test pass?) */
		static long dbg = -2; if (dbg == -2) dbg = getenv("RECOMP_DEBUG_DRAW") ? atol(getenv("RECOMP_DEBUG_DRAW")) : -1;
		if (dbg == ndraw) {
			float lo = 1e9f, hi = -1e9f;
			if (scene_depth) for (uint32_t i = 0; i < scene_depth->w * scene_depth->h; i++) { if (scene_depth->z[i] < lo) lo = scene_depth->z[i]; if (scene_depth->z[i] > hi) hi = scene_depth->z[i]; }
			fprintf(stderr, "debug draw %ld: tri z %.6f %.6f %.6f (clip z/w %.4f %.4f %.4f, w %.3f %.3f %.3f) viewport z %g+%g*z depth func %u write %d; scene depth %g..%g load %d\n",
			        dbg, t.v[0].z, t.v[1].z, t.v[2].z, a->z / (a->w ? a->w : 1), b->z / (b->w ? b->w : 1), c3->z / (c3->w ? c3->w : 1), a->w, b->w, c3->w,
			        d->vp_zoff, d->vp_zscale, d->depth_func, d->depth_write, lo, hi, scene_target.ds_force_load);
		}
	}
	if (ntris == cap_tris) { cap_tris = cap_tris ? 2 * cap_tris : 4096; tris = realloc(tris, cap_tris * sizeof *tris); }
	tris[ntris++] = t;
}
/* triangle setup, clipping a straddling-the-eye triangle against the near plane first (the common
 * case -- every vertex already in front of the eye -- skips the clip entirely) */
static void record_tri(const Vert *a, const Vert *b, const Vert *c3) {
	ST(&recs[nrecs - 1], st_tris);
	if (a->w > NEAR_W_EPS && b->w > NEAR_W_EPS && c3->w > NEAR_W_EPS) { record_tri_clipped(a, b, c3); return; }
	const Vert *in[3] = {a, b, c3};
	Vert clipped[4];
	int n = clip_near(in, clipped);
	if (n < 3) ST(&recs[nrecs - 1], st_near);
	for (int i = 0; i + 2 < n; i++) record_tri_clipped(&clipped[0], &clipped[i + 1], &clipped[i + 2]);
}

/* the worker pool: a flush hands every worker its band and waits for all of them */
#define MAX_WORKERS 16
static int nworkers;
static pthread_mutex_t pool_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pool_go = PTHREAD_COND_INITIALIZER, pool_done = PTHREAD_COND_INITIALIZER;
static uint64_t pool_gen; static int pool_left;
static void run_band(int k) {
	int h = scene_target.height, y0 = h * k / nworkers, y1 = h * (k + 1) / nworkers;
	tc_active = !tc_off;
	for (uint32_t i = 0; i < ntris; i++) {
		const Tri *t = &tris[i];
		if (t->y1 <= y0 || t->y0 >= y1) continue;
		raster_rows(t, &recs[t->draw], y0, y1);
	}
	tc_active = 0;
}

/* between flushes (no band running): a new generation for the decoded-texture cache, whose
 * entries the bands then verify afresh on first touch (see TcEntry) */
static void tc_prepare(void) {
	if (!tc_inited) {
		tc_inited = 1;
		const char *e = getenv("RECOMP_SOFT_NO_TEXCACHE");
		tc_off = e && *e && strcmp(e, "0");
		tc_check = getenv("RECOMP_SOFT_TEXCACHE_CHECK") != 0;
		tc_log = getenv("RECOMP_SOFT_TEXCACHE_LOG") != 0;
		long mb = getenv("RECOMP_SOFT_TEXCACHE_MB") ? atol(getenv("RECOMP_SOFT_TEXCACHE_MB")) : 256;
		tc_cap = (size_t)(mb > 0 ? mb : 0) << 20;
		for (int i = 0; i < TC_BUCKETS; i++) tc_bucket[i] = -1;
	}
	tc_gen++;
}
static void *worker(void *arg) {
	int k = (int)(intptr_t)arg;
	uint64_t seen = 0;
	for (;;) {
		pthread_mutex_lock(&pool_m);
		while (pool_gen == seen) pthread_cond_wait(&pool_go, &pool_m);
		seen = pool_gen;
		pthread_mutex_unlock(&pool_m);
		run_band(k);
		pthread_mutex_lock(&pool_m);
		if (--pool_left == 0) pthread_cond_signal(&pool_done);
		pthread_mutex_unlock(&pool_m);
	}
	return 0;
}
/* the scene's visibility counters (GfxDraw.vis_addr), once its draws are rasterised: each counter the total of its
 * draws' passing samples (SET: the same, nonzero when any passed) -- a draw that drew nothing counts 0. SET tests (occlusion
 * culling) are reported visible instead: this rasteriser doesn't clip at the near plane, so an occluder mesh around the camera
 * smears over the whole depth tile and hides what is in plain view (Killzone frame 1900: 24 of 311 tests hidden here that the
 * GPU, clipping, counts visible; with the depth test off the two agree). RECOMP_VIS_SET=real: counted here too. */
static void vis_report(void) {
	static uint32_t addr[512], cnt[512]; int n = 0;   /* (Killzone: ~200 a scene) */
	static int real = -1; if (real < 0) { const char *e = getenv("RECOMP_VIS_SET"); real = e && !strcmp(e, "real"); }
	for (uint32_t i = 0; i < nrecs; i++) {
		const DrawRec *r = &recs[i]; if (!r->d.vis_addr) continue;
		if (r->d.vis_set && !real) { gfx_vis_write(r->d.vis_addr, 1); continue; }
		int k; for (k = 0; k < n && addr[k] != r->d.vis_addr; k++) {}
		if (k == n) { if (n == 512) continue; addr[n] = r->d.vis_addr; cnt[n] = 0; n++; }
		cnt[k] += r->vis_count;
	}
	for (int k = 0; k < n; k++) gfx_vis_write(addr[k], cnt[k]);
}
static void flush(void) {
	if (!ntris) { vis_report(); stats_report(); nrecs = 0; return; }
	if (!nworkers) {
		long n = plat_cpu_count();
		nworkers = getenv("RECOMP_RASTER_THREADS") ? atoi(getenv("RECOMP_RASTER_THREADS")) : (int)(n > 2 ? n - 1 : 1);
		if (nworkers > MAX_WORKERS) nworkers = MAX_WORKERS;
		if (nworkers < 1) nworkers = 1;
		for (int k = 1; k < nworkers; k++) { pthread_t th; pthread_create(&th, 0, worker, (void *)(intptr_t)k); }
	}
	tc_prepare();
	pthread_mutex_lock(&pool_m);
	pool_left = nworkers - 1; pool_gen++;
	pthread_cond_broadcast(&pool_go);
	pthread_mutex_unlock(&pool_m);
	run_band(0);   /* this thread takes band 0 */
	pthread_mutex_lock(&pool_m);
	while (pool_left) pthread_cond_wait(&pool_done, &pool_m);
	pthread_mutex_unlock(&pool_m);
	vis_report();
	stats_report();
	ntris = 0; nrecs = 0;
}

static void dump(const GfxDraw *d, int k) {
	char vp[512], fp[512];
	params(d->vprog, vp, sizeof vp); params(d->fprog, fp, sizeof fp);
	fprintf(stderr, "  draw %d: prim %u count %u index fmt %#x  vprog %#x [%s]  fprog %#x [%s]\n",
	        k, d->prim, d->count, d->index_format, d->vprog, vp, d->fprog, fp);
	for (int i = 0; i < 16; i++) if (d->tex[i].data)
		fprintf(stderr, "    tex %d: %ux%u format %#x layout %u data %#x\n", i, d->tex[i].width, d->tex[i].height,
		        d->tex[i].format, (d->tex[i].u_mode >> 4) & 3, d->tex[i].data);
	for (int i = 0; i < d->nattr; i++)
		fprintf(stderr, "    attr reg %u: stream %u off %u fmt %u x%u (stride %u, data %#x)\n", d->attr[i].reg, d->attr[i].stream,
		        d->attr[i].offset, d->attr[i].format, d->attr[i].comps, d->stride[d->attr[i].stream], d->stream[d->attr[i].stream]);
	for (uint32_t v = 0; v < d->count && v < 12; v++) {
		uint32_t ix = index_at(d, v);
		fprintf(stderr, "    v%u (index %u):", v, ix);
		for (int i = 0; i < d->nattr; i++) {
			uint32_t a = d->stream[d->attr[i].stream] + ix * d->stride[d->attr[i].stream] + d->attr[i].offset;
			fprintf(stderr, " [");
			for (int k2 = 0; k2 < d->attr[i].comps; k2++) fprintf(stderr, "%s%g", k2 ? " " : "", attr_comp(a, d->attr[i].format, k2));
			fprintf(stderr, "]");
		}
		fprintf(stderr, "\n");
	}
	{
		Shader sh = shader_of(d);
		float x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
		for (uint32_t v = 0; v < d->count; v++) {
			Vert o; fetch(d, &sh, index_at(d, v), &o);
			if (o.x < x0) x0 = o.x; if (o.x > x1) x1 = o.x; if (o.y < y0) y0 = o.y; if (o.y > y1) y1 = o.y;
		}
		fprintf(stderr, "    clip-space extent x %.2f..%.2f y %.2f..%.2f\n", x0, x1, y0, y1);
	}
	fprintf(stderr, "    vuniforms:");
	for (int i = 0; i < 16; i++) { float f; memcpy(&f, G(d->vuniforms + 4 * i), 4); fprintf(stderr, " %g", f); }
	fprintf(stderr, "\n    funiforms:");
	for (int i = 0; i < 8; i++) { float f; memcpy(&f, G(d->funiforms + 4 * i), 4); fprintf(stderr, " %g", f); }
	fprintf(stderr, "\n");
	for (int i = 0; i < 4; i++) if (d->tex[i].data)
	{
		fprintf(stderr, "    tex%d: %ux%u fmt %#x at %#x filt %u/%u addr %u/%u", i, d->tex[i].width, d->tex[i].height,
		        d->tex[i].format, d->tex[i].data, d->tex[i].min_filter, d->tex[i].mag_filter, d->tex[i].u_mode, d->tex[i].v_mode);
		/* content: how many texels (of the first 64K) are non-zero / opaque */
		uint32_t n = d->tex[i].width * d->tex[i].height; if (n > 65536) n = 65536;
		uint32_t nz = 0, op = 0;
		if ((d->tex[i].format & 0xff000000u) == 0x0c000000u)
			for (uint32_t k = 0; k < n; k++) { uint32_t px = RD32(d->tex[i].data + 4 * k); nz += px != 0; op += (px >> 24) == 0xff; }
		fprintf(stderr, "  [%u/%u non-zero, %u opaque]\n", nz, n, op);
	}
	if (d->blend.present)
		fprintf(stderr, "    blend mask %#x func %u/%u src %u dst %u / %u %u\n", d->blend.mask, d->blend.color_func,
		        d->blend.alpha_func, d->blend.color_src, d->blend.color_dst, d->blend.alpha_src, d->blend.alpha_dst);
}

/* each program pair the stand-in shaders can't fully interpret, once: its parameter names */
static void unhandled(const GfxDraw *d, const char *why) {
	static uint64_t seen[256]; static int n;
	uint64_t k = (uint64_t)d->vprog << 32 | d->fprog;
	for (int i = 0; i < n; i++) if (seen[i] == k) return;
	if (n < 256) seen[n++] = k;
	char vp[512], fp[512];
	params(d->vprog, vp, sizeof vp); params(d->fprog, fp, sizeof fp);
	fprintf(stderr, "    soft: %s: vprog %#x [%s] fprog %#x [%s]\n", why, d->vprog, vp, d->fprog, fp);
}
/* fragment parameters the stand-ins know; anything else means the shader does more than they do */
static void check_known(const GfxDraw *d) {
	static const char *known[] = {"sampler", "g_addColor", "g_clearColor", "g_tint", 0};
	uint32_t prog = d->fprog;
	if (!prog) return;
	uint32_t cnt = RD32(prog + 0x24), base = prog + 0x28 + RD32(prog + 0x28);
	for (uint32_t i = 0; i < cnt; i++) {
		uint32_t p = base + 16 * i;
		const char *nm = (char *)G(p + (int32_t)RD32(p));
		int ok = 0;
		for (int k = 0; known[k]; k++) if (strstr(nm, known[k])) ok = 1;
		if (!ok) { unhandled(d, "unknown fragment parameter"); return; }
	}
}
/* SCE_GXM_PRIMITIVE_POINTS: point sprites. The hardware rasterises each point as a screen-aligned
 * square of PSIZE pixels (the vertex program's PSIZE output; sceGxmSetFrontPointLineWidth's 1..16
 * when it writes none), centred on the point, and hands the fragment a point-sprite coordinate that
 * runs 0..1 across the square (SCE_GXM_POLYGON_MODE_POINT_01UV; _10UV runs 1..0 -- taken as v
 * flipped, unverified; plain _POINT generates none and reads as 01 here). This makes four copies of
 * the vertex, moved apart in clip space by half the size -- pixels over the viewport's scale is NDC,
 * times w so the perspective divide gives the pixels back -- with the sprite coordinate in the two
 * reserved varying slots, and records two triangles; the fragment side reads the slots through the
 * iterator's point_coord flag. Uncharted's swipe prompts -- the arrow and the trail of blobs a melee
 * combo or a climb asks the player to trace (GuiAnimatedGestureWidget: int_ui_scr_gesture_blob/head
 * shaders, POINT_01UV, into a widget-sized target the HUD then draws) -- are point sprites; with
 * points dropped by both backends the prompt's texture stayed clear and no arrow ever showed. */
static void render_points_interp(const GfxDraw *d, const UsseProgram *vp, DrawRec *r) {
	const UsseVertexLayout *vl = r->vlayout;
	r->d.cull = 0;   /* a sprite has no winding (record_tri_clipped culls by the record's state) */
	/* pixels per NDC unit on each axis, and which way +y and +x go on screen: record_tri_clipped's mapping */
	float kx, ky; int ydown, xleft = 0;
	if (d->viewport_set && d->viewport_disabled) { kx = ky = 1; ydown = 1; }   /* clip = screen pixels, y down */
	else if (d->viewport_set) { kx = fabsf(d->vp_xscale); ky = fabsf(d->vp_yscale); ydown = d->vp_yscale > 0; xleft = d->vp_xscale < 0; }
	else { kx = scene_target.width * 0.5f; ky = scene_target.height * 0.5f; ydown = 0; }
	if (!(kx > 0)) kx = 1;
	if (!(ky > 0)) ky = 1;
	int flip_v = d->polygon_mode == 2;   /* POINT_10UV */
	for (uint32_t i = 0; i < d->count; i++) {
		Vert a; fetch_interp(d, vp, vl, &r->vdraw, index_at(d, i), &a);
		float size = (vl->o_psize >= 0 && vl->o_psize < PC_U) ? a.vary[vl->o_psize] : (float)(d->point_line_width ? d->point_line_width : 1);
		if (!(size > 0)) continue;   /* (a NaN size too) */
		if (size > 2048) size = 2048;
		float w = a.w ? a.w : 1;
		float hx = 0.5f * size / kx * w, hy = 0.5f * size / ky * w;
		float sx = xleft ? -hx : hx, sy = ydown ? hy : -hy;   /* the clip-space step to the right and downward corners */
		Vert q[4];
		for (int c = 0; c < 4; c++) {   /* 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right, on screen */
			int right = c & 1, down = c >> 1;
			q[c] = a;
			q[c].x += right ? sx : -sx;
			q[c].y += down ? sy : -sy;
			q[c].vary[PC_U] = (float)right;
			q[c].vary[PC_V] = flip_v ? 1.0f - (float)down : (float)down;
		}
		record_tri(&q[0], &q[1], &q[2]); record_tri(&q[1], &q[3], &q[2]);
	}
}
/* RECOMP_SHADERS=interp: run the draw's own vertex/fragment GXP programs. Returns 0 (caller
 * should fall back to the stand-in path) if either program uses an instruction usse.c doesn't
 * support yet. */
static int render_interp(const GfxDraw *d) {
	const UsseProgram *vp = usse_get(d->vprog), *fp = usse_get(d->fprog);
	if (!usse_supported(vp) || !usse_supported(fp)) { unhandled(d, "shader interpreter: unsupported instruction, falling back to stand-in"); return 0; }
	if (nrecs == cap_recs) { cap_recs = cap_recs ? 2 * cap_recs : 256; recs = realloc(recs, cap_recs * sizeof *recs); }
	DrawRec *r = &recs[nrecs++];
	memset(r, 0, sizeof *r);
	r->d = *d; r->use_interp = 1; r->vprog = vp; r->fprog = fp; r->draw_index = ndraw;
	r->vlayout = usse_vertex_layout(vp); r->flayout = usse_fragment_layout(fp);
	usse_setup_registers(vp, &r->vdraw, d->vuniforms, d->vubuf);
	usse_setup_registers(fp, &r->fdraw, d->funiforms, d->fubuf);
	UsseInst scratch;
	/* RECOMP_TRACE_DRAW=k: RECOMP_USSE_TRACE for that draw alone -- both secondary programs here,
	 * then its first vertex (fetch_interp); RECOMP_DEBUG_DRAW covers its fragments */
	static long tdraw = -2; if (tdraw == -2) tdraw = getenv("RECOMP_TRACE_DRAW") ? atol(getenv("RECOMP_TRACE_DRAW")) : -1;
	int tr = (ndraw == tdraw);
	if (tr) { fprintf(stderr, "trace draw %d: vertex secondary\n", ndraw); usse_set_trace(1); }
	memset(&scratch, 0, sizeof scratch); usse_run_secondary(vp, &scratch, &r->vdraw);
	if (tr) { fprintf(stderr, "trace draw %d: vertex SA:", ndraw); for (int i = 0; i < 40; i++) fprintf(stderr, "%s%2d=%08x(%g)", i % 8 ? " " : "\n   ", i, r->vdraw.sa[i].u, r->vdraw.sa[i].f); fprintf(stderr, "\ntrace draw %d: fragment secondary\n", ndraw); }
	memset(&scratch, 0, sizeof scratch); usse_run_secondary(fp, &scratch, &r->fdraw);
	if (tr) { usse_set_trace(0); trace_vprog = vp; }
	if (gfx_prim_kind(d->prim) == GFX_PRIM_POINTS) { render_points_interp(d, vp, r); return 1; }
	/* triangles, strips (alternate ones rewound) and fans: gfx.h's gfx_prim_tri, as gfx_gpu.c expands them */
	uint32_t ntri = gfx_prim_tris(d->prim, d->count), pos[3];
	Vert a, b, cc;
	for (uint32_t t = 0; t < ntri; t++) {
		gfx_prim_tri(d->prim, t, pos);
		fetch_interp(d, vp, r->vlayout, &r->vdraw, index_at(d, pos[0]), &a); fetch_interp(d, vp, r->vlayout, &r->vdraw, index_at(d, pos[1]), &b);
		fetch_interp(d, vp, r->vlayout, &r->vdraw, index_at(d, pos[2]), &cc);
		record_tri(&a, &b, &cc);
	}
	return 1;
}
static void render(const GfxDraw *d) {
	if (shaders_interp && render_interp(d)) return;
	Shader s = shader_of(d);
	if (s.pos_reg < 0) { unhandled(d, "no position attribute, draw skipped"); stats_skipped++; return; }
	check_known(d);
	if (nrecs == cap_recs) { cap_recs = cap_recs ? 2 * cap_recs : 256; recs = realloc(recs, cap_recs * sizeof *recs); }
	DrawRec *r = &recs[nrecs++];
	memset(r, 0, sizeof *r);
	r->d = *d; r->s = s; r->draw_index = ndraw;
	for (int k = 0; k < 4; k++) { r->clear[k] = r->add[k] = 0; r->tint[k] = 1; }
	if (s.has_tint) for (int k = 0; k < 4; k++) r->tint[k] = uf(d->funiforms, s.tint_res + k);
	if (s.has_clear) for (int k = 0; k < 4; k++) r->clear[k] = uf(d->funiforms, s.clear_res + k);
	if (s.has_add) for (int k = 0; k < 4; k++) r->add[k] = uf(d->funiforms, s.add_res + k);
	uint32_t ntri = gfx_prim_tris(d->prim, d->count), pos[3];   /* triangles, strips, fans (gfx.h) */
	Vert a, b, cc;
	for (uint32_t t = 0; t < ntri; t++) {
		gfx_prim_tri(d->prim, t, pos);
		fetch(d, &s, index_at(d, pos[0]), &a); fetch(d, &s, index_at(d, pos[1]), &b); fetch(d, &s, index_at(d, pos[2]), &cc);
		record_tri(&a, &b, &cc);
	}
}

/* RECOMP_DUMP_PAIRS=file: append "vprog fprog" (hex) the first time each unique pair is drawn,
 * across the whole run -- a GPU pipeline is compiled per (vprog,fprog) pair, so this is the set
 * usse_spirv.c's translator coverage needs to be measured against, not just "every .gxp file" */
static void dump_pairs(uint32_t vprog, uint32_t fprog) {
	static const char *path; static int inited2;
	static uint32_t seen_v[256], seen_f[256]; static int nseen;
	if (!inited2) { inited2 = 1; path = getenv("RECOMP_DUMP_PAIRS"); }
	if (!path) return;
	for (int i = 0; i < nseen; i++) if (seen_v[i] == vprog && seen_f[i] == fprog) return;
	if (nseen < 256) { seen_v[nseen] = vprog; seen_f[nseen] = fprog; nseen++; }
	FILE *f = fopen(path, "a"); if (!f) return;
	fprintf(f, "%08x %08x\n", vprog, fprog); fclose(f);
}

void gfx_soft_begin_scene(const GfxTarget *t) {
	init();
	stats_init();
	flush();
	stats_scene++; stats_skipped = 0;
	target = *t;
	scene_target = *t;
	scene_has_depth_plane = !t->ds_data || ds_has_depth(t->ds_format);     /* no surface (old capture): assume both, as before */
	scene_has_stencil_plane = !t->ds_data || ds_has_stencil(t->ds_format);
	int fresh = 0;
	depth = t->depth && (t->data || t->ds_data) ? depth_for(t, &fresh) : 0;   /* a depth-only scene too (gfx_dispatch.c's scene_dims sized it) */
	scene_depth = depth;
	if (depth) {
		if (t->ds_data && t->ds_force_load) {
			if (fresh) depth_load_from_guest(depth, t);   /* else: keep this scene's accumulated contents -- the point of force-load */
		} else {
			float bg = t->ds_data ? t->ds_background_depth : 1.0f;    /* old captures: always far, as before */
			uint8_t bgs = t->ds_data ? t->ds_background_stencil : 0;
			for (uint32_t i = 0; i < depth->w * depth->h; i++) depth->z[i] = bg;
			memset(depth->st, bgs, depth->w * depth->h);
		}
	}
	if ((long)frame == dump_frame)
		fprintf(stderr, " scene: target %ux%u stride %u fmt %#x at %#x\n", t->width, t->height, t->stride, t->format, t->data);
}
void gfx_soft_draw(const GfxDraw *d) {
	dump_pairs(d->vprog, d->fprog);
	if ((long)frame == dump_frame) dump(d, ndraw);
	static int skip = -1; if (skip < 0) skip = getenv("RECOMP_NORENDER") != 0;   /* measuring: everything but drawing */
	if (!skip) render(d);
	/* RECOMP_SOFT_DRAW_ORDER=1: every draw rasterised whole before the next is recorded, as the GPU path draws -- the scene's
	 * bands otherwise run in parallel over all its draws at once, and a draw that samples a region of the target an
	 * earlier draw of the scene wrote reads it before or after that band ran (Killzone's post-processing atlas came out
	 * different on every run: no reference for the GPU path there). Diagnosis only: a flush a draw. */
	static int order = -1; if (order < 0) order = getenv("RECOMP_SOFT_DRAW_ORDER") != 0;
	if (order) flush();
	ndraw++;
}
void gfx_soft_end_scene(void) {
	flush();
	if (scene_depth && scene_target.ds_data && scene_target.ds_force_store) depth_store_to_guest(scene_depth, &scene_target);
}

static void shot(uint32_t base, uint32_t w, uint32_t h, uint32_t pitch) {
	char name[64]; snprintf(name, sizeof name, "shot_%05llu.ppm", (unsigned long long)frame);
	FILE *f = fopen(name, "wb");
	if (!f) return;
	fprintf(f, "P6\n%u %u\n255\n", w, h);
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) { uint32_t p = RD32(base + 4 * (y * pitch + x)); uint8_t rgb[3] = {p, p >> 8, p >> 16}; fwrite(rgb, 1, 3, f); }
	fclose(f);
}
static int shot_window(void) { return (long)frame >= shot_from && (shot_to < 0 || (long)frame <= shot_to); }
int gfx_soft_shot_due(void) { init(); return shot_every && frame % shot_every == 0 && shot_window(); }   /* will this present write a RECOMP_SHOT? (gfx_gpu.c syncs guest memory only then) */
int gfx_soft_skip_window;   /* set by gfx_gpu.c when it has shown the frame itself */
void gfx_soft_present(uint32_t base, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format) {
	init();
	if ((long)frame == dump_frame) fprintf(stderr, " present frame %llu: %u draws\n", (unsigned long long)frame, ndraw);
	if (shot_every && frame % shot_every == 0 && shot_window()) shot(base, width, height, pitch);
	if (shot_last >= 0 && (long)frame >= shot_last) { fprintf(stderr, "frame %llu shot: the last one asked for (RECOMP_SHOT_LAST), exiting\n", (unsigned long long)frame); fflush(stderr); exit(0); }   /* a scripted test used to run to its timeout: minutes for a frame that takes seconds */
	usse_profile_report();
	extern void window_present(uint32_t base, uint32_t w, uint32_t h, uint32_t pitch);
	if (format == 0 && !gfx_soft_skip_window) window_present(base, width, height, pitch);   /* A8B8G8R8 */
	gfx_soft_skip_window = 0;
	frame++;
	ndraw = 0;
}
