/*
 * The GXM rendering boundary over Sony's libgxm: observers that run just before
 * Sony's own BeginScene / Draw* / EndScene (main.c dispatches observer, then Sony's function). Each
 * reads the state Sony's code keeps -- the context its setters wrote, the surfaces and textures its
 * Init functions built, the programs its shader patcher made -- and drives the host's renderer
 * (gfx.h) with it. Sony's function then runs as on the console: it commits the uniform buffers,
 * builds the GPU's command words in the context's rings and submits them to the GPU driver, which
 * the host completes (gpu.c's kick).
 *
 * The layouts are the firmware's (dump/decrypted/maps/gxm-objects.md, from dev/research/gxmobjects.py);
 * offsets below cite it.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "hle.h"
#include "gfx.h"
#include "gfx_coverage.h"

/* RECOMP_LOG_CLIP: each scene's valid region, every sceGxmSetRegionClip and the context clip at a scene's first draw */
static int clip_log(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_LOG_CLIP") != 0; return v; }
static int clip_first_draw;   /* RECOMP_LOG_CLIP: the next draw is a scene's first */

/* ---- SceGxmContext (the game's hostMem; +offsets) */
#define CX_FPROG 0x44            /* SceGxmFragmentProgram* */
#define CX_FUBUF 0x4c            /* fragment uniform buffers 0-13, then the default (14) at +0x84 */
#define CX_FTEX 0xa0             /* fragment textures 0-15, 16 bytes each */
#define CX_PFSTATE 0x1a0         /* bound SceGxmPrecomputedFragmentState* */
#define CX_FRONT 0x1c4           /* front ISP word */
#define CX_FRONT_VIS 0x1c8       /* front: visibility index bits 0-11 */
#define CX_FRONT_STENCIL 0x1cc   /* front stencil word */
#define CX_CLIP 0x1e8            /* region clip words A, B */
#define CX_VIEWPORT 0x1f0        /* six floats */
#define CX_CULL 0x214            /* cull mode, viewport-disable bit 16 */
#define CX_VPROG 0x224           /* SceGxmVertexProgram* */
#define CX_VUBUF 0x22c           /* vertex uniform buffers 0-13, then the default (14) at +0x264 */
#define CX_STREAM 0x280          /* vertex streams 0-15 */
#define CX_VTEX 0x2c0            /* vertex textures 0-15, 16 bytes each (sceGxmSetVertexTexture) */
#define CX_PVSTATE 0x3c0         /* bound SceGxmPrecomputedVertexState* */
#define CX_VIS_BASE 0x544        /* visibility buffer, its per-core stride at +0x548 */

/* a pointer slot the context initialises with a sentinel (0x3e000000 | i << 16 for uniform buffers,
 * 0x3d000000 | i << 20 for streams) is unbound */
static uint32_t bound(uint32_t v, uint32_t sentinel_mask) { return (v & 0xff000000u) == sentinel_mask ? 0 : v; }

/* bytes per texel of the uncompressed texture base formats (0: block-compressed or unknown) */
static uint32_t tex_bpp(uint32_t format) {
	switch (format & 0x1f000000u) {
	case 0x1b000000u: case 0x19000000u: return 8;
	case 0x0c000000u: case 0x0d000000u: case 0x0e000000u: case 0x0f000000u: case 0x10000000u: case 0x11000000u: case 0x12000000u:
	case 0x13000000u: case 0x14000000u: case 0x15000000u: case 0x16000000u: case 0x17000000u: case 0x1a000000u: return 4;
	case 0x02000000u: case 0x03000000u: case 0x04000000u: case 0x05000000u: case 0x06000000u: case 0x07000000u: case 0x08000000u:
	case 0x09000000u: case 0x0a000000u: case 0x0b000000u: return 2;
	case 0x00000000u: case 0x01000000u: return 1;
	default: return 0;
	}
}
/* RECOMP_SRGB=1: carry the textures' and colour surfaces' sRGB (gamma) modes to the renderers (gfx.h
 * GFX_TEX_GAMMA / GFX_TGT_GAMMA). Off by default (2026-10-07): the bits are where Sony's own setters
 * put them (texture word 0 bits 27-28, colour surface word 0 bits 12-13), but decoding the textures
 * and encoding Uncharted's one sRGB surface as the GPU's sRGB formats would made its title menu
 * either far too dark (decode only) or hazy (both), while without them it is closest to the real
 * Vita's -- some part of the hardware's sRGB behaviour (which pass reads that surface, with what
 * mode; preload/blend in which space) isn't modelled yet. Its gameplay frames don't use them.
 * Unit 13 turns them on (packs/PCSA00014/pack.toml): its deferred lighting is gamma-correct (sRGB albedo
 * textures, an sRGB tonemap surface the final pass reads raw), and without the modes every sunlit
 * surface lit ~3x too bright and its filmic tonemap clipped the floors to white (2026-10-08). */
static int srgb_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_SRGB"); v = e && *e == '1'; } return v; }
/* ---- SceGxmTexture (4 control words) -> the renderer's GfxTexture */
static GfxTexture texture_words(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3);
static GfxTexture texture(uint32_t t) { return texture_words(RD32(t), RD32(t + 4), RD32(t + 8), RD32(t + 12)); }
static GfxTexture texture_words(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
	uint32_t type = w1 & 0xe0000000u;
	GfxTexture g = {0};
	g.data = w2 & ~3u;
	g.format = (w0 & 0x80000000u) | (w1 & 0x1f000000u) | ((w3 >> 16) & 0x7000u) | (srgb_on() ? ((w0 >> 27) & 3u) << 16 : 0);   /* + the gamma mode (gfx.h GFX_TEX_GAMMA), opt-in */
	int log2size = type == 0 || type == 0x40000000u;   /* swizzled and cube store log2 sizes */
	g.width = (uint16_t)(log2size ? 1u << ((w1 >> 16) & 15) : ((w1 >> 12) & 0xfff) + 1);
	g.height = (uint16_t)(log2size ? 1u << (w1 & 15) : (w1 & 0xfff) + 1);
	int strided = type == 0xc0000000u;
	if (strided) {   /* the byte stride, scattered over word 0 (S = stride/4 - 1: bits 0-2 at 9-11, 3-12 at 17-26, 13-14 at 1-2), as a pixel stride */
		uint32_t s4 = (((w0 >> 1) & 3) << 13 | ((w0 >> 17) & 0x3ff) << 3 | ((w0 >> 9) & 7)) + 1, bpp = tex_bpp(g.format);
		if (bpp) g.format |= (s4 * 4 / bpp) & 0xfff;
	}
	g.min_filter = (uint8_t)(strided ? (w0 >> 12) & 3 : (w0 >> 10) & 3);
	if (!strided) {   /* the mip filter (bit 9) and the LOD bias (bits 21-26, 31 = none: (b - 31) / 8 levels, as Vita3K reads it) -- gfx.h GFX_TEX_MIP_LINEAR / _LOD_BIAS */
		int lb = (int)((w0 >> 21) & 63) - 31; if (lb > 31) lb = 31;
		g.format |= ((uint32_t)lb & 63u) << 18 | (w0 & 0x200u) << 6;
		if (g.min_filter == 3 && gfxcov_on()) gfxcov_gap("tex_min_filter", "MIPMAP_POINT", 1, "filtered as LINEAR (Vita3K: POINT)");
	}
	g.mag_filter = (uint8_t)((w0 >> 12) & 3);
	/* layout (high nibble of u_mode): 0 linear, 1 swizzled, 2 tiled, 3 cube */
	int layout = type == 0 || type == 0xa0000000u ? 1 : type == 0x80000000u ? 2 : type == 0x40000000u || type == 0xe0000000u ? 3 : 0;
	g.u_mode = (uint8_t)(layout << 4 | ((w0 >> 6) & 7));
	g.v_mode = (uint8_t)((strided ? 1 : ((w0 >> 17) & 15) + 1) << 4 | ((w0 >> 3) & 7));
	/* sceGxmTextureSetLodMin: the finest level the unit may sample (word 2 bits 0-1 = its bits 2-3, word 3 bits
	 * 26-27 = bits 0-1). A streaming engine raises it while the finer levels load: Killzone's streamed textures
	 * sampled their not-yet-loaded top level as blocks of colour noise. Taken as the chain from that level on
	 * (the sampled level, max(lod, lodMin), is the same whichever level the LOD counts from) */
	uint32_t lodmin = (w2 & 3u) << 2 | ((w3 >> 26) & 3u);
	int levels = (g.v_mode >> 4) & 15;
	if (lodmin && levels > 1 && layout != 3 && !strided) {
		int k = (int)lodmin < levels ? (int)lodmin : levels - 1;
		GfxTexture lv; gfx_texture_level(&g, k, &lv);
		lv.v_mode = (uint8_t)((levels - k) << 4 | (g.v_mode & 15));
		static uint32_t seen[64]; static int nseen; int i;
		for (i = 0; i < nseen && seen[i] != g.data; i++);
		if (i == nseen && nseen < 64) { seen[nseen++] = g.data; if (getenv("RECOMP_LOG_LODMIN")) fprintf(stderr, "    gxm: texture %#x fmt %#x %ux%u, %d levels: LOD min %u -> level %d at %#x %ux%u\n", g.data, g.format, g.width, g.height, levels, lodmin, k, lv.data, lv.width, lv.height); }
		g = lv;
	}
	return g;
}

/* the default uniform buffer a draw uses: the one reserved for it (state 1: Reserve*DefaultUniformBuffer
 * puts it in the reserved slot, and Sony's Draw -- which runs after the observer -- commits it), else
 * the committed/explicitly set one (states 2, 3); the slot's sentinel (0x3e0e0000) means none */
static uint32_t default_ubuf(uint32_t cx, uint32_t state, uint32_t reserved, uint32_t slot) {
	uint32_t b = RD32(cx + state) == 1 ? RD32(cx + reserved) : RD32(cx + slot);
	return (b & 0xff000000u) == 0x3e000000u ? 0 : b;
}

/* ---- the GXP tables a precomputed state's uniform buffers are recovered through (self-relative) */
static int checking(void);
static void issue(const char *fmt, ...);
static int mapped(uint32_t a, uint32_t size);
static uint32_t gxp_table(uint32_t gxp, uint32_t off) { return gxp + off + RD32(gxp + off); }
/* uniform buffer b of a precomputed state: from its DMA table (entry k for the GXP's +0x94 entry k,
 * word 0 = buffer + 4 * wordOffset), else its SA block A (GXP +0x7c: SA[sa] = buffer + byteOffset) */
static uint32_t state_ubuf(uint32_t gxp, uint32_t dma, uint32_t sa, int b) {
	uint32_t n = RD32(gxp + 0x90), t = gxp_table(gxp, 0x94);
	for (uint32_t k = 0; dma && k < n && k < 64; k++)
		if (RD16(t + 8 * k) == b) {
			uint32_t v = RD32(dma + 8 * k) - 4 * RD16(t + 8 * k + 2);
			if (checking() && v && !mapped(v, 4))
				issue("precomputed uniform buffer %d via DMA entry %u: table row %08x %08x, entry %08x %08x", b, k,
				      RD32(t + 8 * k), RD32(t + 8 * k + 4), RD32(dma + 8 * k), RD32(dma + 8 * k + 4));
			return v;
		}
	n = RD32(gxp + 0x78); t = gxp_table(gxp, 0x7c);
	for (uint32_t k = 0; sa && k < n && k < 64; k++)
		if (RD16(t + 8 * k) == b) return RD32(sa + 4 * RD16(t + 8 * k + 2)) - RD32(t + 8 * k + 4);
	return 0;
}

/* ---- visibility (occlusion) queries: the backends don't count pixels, so a tested draw reports
 * visible (as gxm.c did: with no answer every query read 0 and Unit 13 culled its whole room) */
static void vis_record(uint32_t cx, uint32_t front) {
	uint32_t base = RD32(cx + CX_VIS_BASE);
	if (!(front & 0x4000) || !base) return;
	uint32_t index = (RD32(cx + CX_FRONT_VIS) & 0xfff) | (front & 0x3000);
	uint32_t a = base + 4 * index;
	WR32(a, front & 0x40000 ? 1 : RD32(a) + 256);   /* set: visible; increment: some pixels passed */
}

/* ---- contract checks (RECOMP_GXM_CHECK=1): what the boundary decodes is checked against the firmware
 * map's invariants, and every distinct violation is named once (with the draw it was first seen at)
 * and counted -- so a wrong picture comes with the field that is wrong, not just the pixels */
static int checking(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_GXM_CHECK"); v = e && *e == '1'; } return v; }
static uint64_t ndraws, nscenes;
static struct { char what[160]; uint64_t n; } issues[256];
static int nissues;
static void issue(const char *fmt, ...) {
	char w[160]; va_list ap; va_start(ap, fmt); vsnprintf(w, sizeof w, fmt, ap); va_end(ap);
	for (int i = 0; i < nissues; i++) if (!strcmp(issues[i].what, w)) { issues[i].n++; return; }
	if (nissues < 256) { snprintf(issues[nissues].what, 160, "%s", w); issues[nissues++].n = 1; }
	fprintf(stderr, "gxm check: %s (first at draw %llu, scene %llu)\n", w, (unsigned long long)ndraws, (unsigned long long)nscenes);
}
void gxm_sony_report(void) {
	static int done;
	if (!checking() || done) return;
	done = 1;
	if (!nissues) { fprintf(stderr, "gxm check: no issues in %llu draws, %llu scenes\n", (unsigned long long)ndraws, (unsigned long long)nscenes); return; }
	fprintf(stderr, "gxm check: %d kinds of issue in %llu draws, %llu scenes:\n", nissues, (unsigned long long)ndraws, (unsigned long long)nscenes);
	for (int i = 0; i < nissues; i++) fprintf(stderr, "  %8llu  %s\n", (unsigned long long)issues[i].n, issues[i].what);
}
/* an address the guest has memory at (the images and the heap in use); the ranges are fetched once
 * per checked draw (check_draw), not per address -- the checks cost a few percent, not a 2.5x slowdown.
 * A miss fetches them again before saying no: the heap grows. Fetched only at the first call (and per checked
 * draw, which normal runs don't do), every program the game loaded after its first draw read as unmapped, so
 * is_gxp() refused it and what hangs on it was skipped without a word -- a precomputed state's dependent textures
 * (Killzone's body-shadow decal sampled units 1-4 as black: its whole box went dark, the shadow flicker) and the
 * vertex program's textures (its flares read their visibility through one). */
static uint32_t map_lo[64], map_hi[64]; static int map_n;
static void map_refresh(void) { map_n = guest_mem_ranges(map_lo, map_hi, 64); }
static int mapped(uint32_t a, uint32_t size) {
	for (int pass = 0; pass < 2; pass++) {
		if (!map_n || pass) map_refresh();
		for (int i = 0; i < map_n; i++) if (a >= map_lo[i] && a + size <= map_hi[i] && a + size >= a) return 1;
	}
	return 0;
}
static int is_gxp(uint32_t p) { return p && mapped(p, 4) && !memcmp(G(p), "GXP", 3); }
/* a precomputed state's textures as the SA words it keeps them in: blocks A (saA, through the GXP's +0x80
 * table) and B (saB, +0x88), entries {u16 unit << 2 | word, u16 saIndex}. A unit is filled when its word 2
 * (the data address) is there and tex[] has none for it yet. A fragment state: the dependent units its PDS
 * sampler table doesn't cover -- sceGxmPrecomputedFragmentStateSetTexture takes a texture UNIT and writes
 * its words here besides its PDS slot. Left unbound they sampled as transparent black: Uncharted's water
 * refraction (unit 8, its HDR colour buffer), its spotlights' shadow map and projector (11, 12), Unit 13's
 * sun pass's shadow mask (11). (Read from the context instead, the spotlights got the ambient cubes another
 * pass had left there, and every one of their draws fell to the CPU: a gather on a cube.) A vertex state:
 * all of its textures */
static void state_sa_textures(uint32_t g, uint32_t saA, uint32_t saB, GfxTexture tex[16], const char *stage) {
	uint32_t sw[16][4] = {{0}}, have[16] = {0};
	for (int blk = 0; blk < 2 && is_gxp(g); blk++) {
		uint32_t sa = blk ? saB : saA, n2 = RD32(g + (blk ? 0x88 : 0x80)), tab = gxp_table(g, blk ? 0x8c : 0x84);
		if (!sa || !n2 || n2 > 64) continue;
		for (uint32_t e = 0; e < n2; e++) {
			uint32_t uw = RD16(tab + 4 * e), si = RD16(tab + 4 * e + 2), u = uw >> 2;
			if (u < 16) { sw[u][uw & 3] = RD32(sa + 4 * si); have[u] |= 1u << (uw & 3); }
		}
	}
	for (int u = 0; u < 16; u++) {
		if (tex[u].data || !(have[u] & 4)) continue;   /* (word 2 holds the data address) */
		tex[u] = texture_words(sw[u][0], sw[u][1], sw[u][2], sw[u][3]);
		static uint32_t seen[64]; static int nseen; uint32_t key = (g & ~15u) | (uint32_t)u; int i;
		for (i = 0; i < nseen && seen[i] != key; i++);
		if (i == nseen && nseen < 64) { seen[nseen++] = key; if (getenv("RECOMP_LOG_DEPTEX")) fprintf(stderr, "    gxm: %s program %#x: unit %d from its precomputed state's SA words: texture %#x fmt %#x %ux%u layout %u\n", stage, g, u, tex[u].data, tex[u].format, tex[u].width, tex[u].height, (tex[u].u_mode >> 4) & 3); }
	}
}
static void check_texture(const char *kind, int i, uint32_t t, const GfxTexture *g) {
	uint32_t w1 = RD32(t + 4);
	if (!mapped(g->data, 1)) issue("%s texture %d: data %#x not in guest memory", kind, i, g->data);
	if (!g->width || !g->height || g->width > 4096 || g->height > 4096) issue("%s texture %d: size %ux%u out of range", kind, i, g->width, g->height);
	if (((w1 >> 24) & 0x1f) > 0x1f) issue("%s texture %d: base format bits %#x", kind, i, w1 & 0x1f000000u);
}
static void check_draw(uint32_t cx, const GfxDraw *d, uint32_t vp, uint32_t fp, uint32_t pv, uint32_t pf) {
	map_refresh();
	if (!is_gxp(d->vprog)) issue("vertex program %#x: its GXP %#x has no 'GXP' magic", vp, d->vprog);
	if (!is_gxp(d->fprog)) issue("fragment program %#x: its GXP %#x has no 'GXP' magic", fp, d->fprog);
	if (!RD8(cx + 0x3d8)) issue("draw outside a scene (context +0x3d8 is 0)");
	if (!d->count) issue("draw of 0 indices");
	if (!mapped(d->indices, 2)) issue("index data %#x not in guest memory", d->indices);
	for (int k = 0; k < d->nattr; k++) {
		int s = d->attr[k].stream;
		if (s >= d->nstream) issue("attribute %d reads stream %d, past the program's %d streams", k, s, d->nstream);
		else if (!d->stream[s]) issue("attribute %d reads stream %d, which is unbound%s", k, s, pv ? " (precomputed vertex state bound)" : "");
		else if (!mapped(d->stream[s], 4)) issue("stream %d: data %#x not in guest memory", s, d->stream[s]);
		else if (!d->stride[s]) issue("stream %d: stride 0", s);
	}
	uint32_t vgxp = d->vprog, fgxp = d->fprog;
	if (is_gxp(vgxp) && RD32(vgxp + 0x64) && !d->vuniforms) issue("vertex program needs a default uniform buffer (%u words) but none is %s", RD32(vgxp + 0x64), pv ? "in the precomputed state" : "reserved");
	if (is_gxp(fgxp) && RD32(fgxp + 0x64) && !d->funiforms) issue("fragment program needs a default uniform buffer (%u words) but none is %s", RD32(fgxp + 0x64), pf ? "in the precomputed state" : "reserved");
	if (d->vuniforms && !mapped(d->vuniforms, 4)) issue("vertex default uniform buffer %#x not in guest memory", d->vuniforms);
	if (d->funiforms && !mapped(d->funiforms, 4)) issue("fragment default uniform buffer %#x not in guest memory", d->funiforms);
	/* a buffer the program uses (GXP +0x18: 2 bits per buffer) must be bound and in memory */
	for (int i = 0; i < 14; i++) {
		int vu = is_gxp(vgxp) && (RD32(vgxp + 0x18) >> (2 * i)) & 3, fu = is_gxp(fgxp) && (RD32(fgxp + 0x18) >> (2 * i)) & 3;
		if (vu && !mapped(d->vubuf[i], 4)) issue("vertex program uses uniform buffer %d, which is %#x", i, d->vubuf[i]);
		if (fu && !mapped(d->fubuf[i], 4)) issue("fragment program uses uniform buffer %d, which is %#x", i, d->fubuf[i]);
	}
	/* every texture unit the fragment program samples must be bound */
	if (is_gxp(fgxp)) {
		uint32_t n = RD32(fp + 0x34), units = RD32(fp + 0x38);
		for (uint32_t k = 0; k < n && k < 16; k++) {
			uint32_t u = RD32(units + 4 * k);
			if (u >= 16) issue("fragment program %#x: sampler %u names unit %u", fp, k, u);
			else if (!d->tex[u].data) issue("fragment program samples unit %u, which has no texture%s", u, pf ? " (precomputed fragment state bound)" : "");
			else check_texture("fragment", (int)u, pf ? RD32(pf + 4) + 16 * k : cx + CX_FTEX + 16 * u, &d->tex[u]);
		}
	}
	if (!d->viewport_disabled && (d->vp_xscale == 0 || d->vp_yscale == 0)) issue("viewport scale 0 (x %g, y %g)", d->vp_xscale, d->vp_yscale);
	if (d->clip_mode == 0x40000000u) issue("region clip ALL: every pixel is clipped");
	if (d->fp_disabled && !d->depth_write && d->stencil_func == 7 && !(RD32(cx + CX_FRONT) & 0x4000)) issue("draw writes nothing: fragment program disabled, no depth write, no stencil");
}

/* RECOMP_GFX_COVERAGE: the state libgxm keeps for a draw that draw() below does not pass on, tallied
 * whenever a draw sets it to something other than its default. The backends' own coverage entries
 * can only judge what they are given; these are the fields they never see. The list is
 * gxm-objects.md's context table, field by field; the render target's multisample mode was one
 * of them until Unit 13's impact debris was found drawing at half scale in a 4X target. */
static void decode_gaps(uint32_t cx, uint32_t vgxp, uint32_t fp, uint32_t front, uint32_t cull, uint32_t prim) {
	char v[64];
	if (front & 0x800) {   /* +0x1c4 bit 11 two-sided: back faces use +0x1d0 (ISP word), +0x1d4 (bias), +0x1d8 (stencil) */
		uint32_t back = RD32(cx + 0x1d0), diff = (back ^ front) & 0x01f001ffu;   /* stencil ref/enable, depth write, depth func */
		int same = !diff && RD32(cx + 0x1d8) == RD32(cx + CX_FRONT_STENCIL) && ((RD32(cx + 0x1d4) ^ RD32(cx + CX_FRONT_VIS)) & 0x03ff0000u) == 0;
		gfxcov_gap("two_sided", same ? "back state = front" : "back state differs", same, "back faces drawn with the front state");
	}
	uint32_t pm = (front >> 15) & 7;   /* SceGxmPolygonMode >> 15 (gfx.h GfxDraw.polygon_mode) */
	int kind = gfx_prim_kind(prim);
	if (pm) {
		static const char *const n[] = {"fill", "line", "point_10uv", "point", "point_01uv", "triangle_line", "triangle_point", "7"};
		int sprite = kind == GFX_PRIM_POINTS && pm >= 2 && pm <= 4;   /* a point draw's sprite mode: drawn as such */
		gfxcov_gap("polygon_mode", n[pm], sprite, sprite ? "point sprites, expanded to quads by the software path" : "triangles filled");
	}
	if ((pm || kind == GFX_PRIM_LINES || kind == GFX_PRIM_POINTS) && (front >> 28)) {
		snprintf(v, sizeof v, "%u", (front >> 28) + 1);
		gfxcov_gap("point_line_width", v, kind == GFX_PRIM_POINTS, "lines are not drawn; a point whose vertex program writes no PSIZE takes this size");
	}
	uint32_t pass = (front >> 25) & 7;   /* SceGxmPassType from the fragment program (+0x20) */
	if (pass >= 2) {
		static const char *const n[] = {"opaque", "translucent", "discard", "mask_update", "4", "depth_replace", "6", "7"};
		int done = pass == 2 || pass == 5;   /* the shader's KILL / DEPTHF are what these change: honoured by both */
		gfxcov_gap("pass_type", n[pass], done, done ? "the shader's kill / depth output honoured; no pass-type ordering" : "the pass type is not passed to the backends");
	}
	/* the depth bias (+0x1c8 bits 16-25) reaches the backends from sceGxmSetFrontDepthBias's own arguments (o_depth_bias):
	 * gfx_coverage.c's depth_bias feature */
	if (cull & 0x4000) gfxcov_gap("w_buffer", "enabled", 0, "depth compares z, not w");
	if (cull & 0x8000) { float w; uint32_t raw = RD32(cx + 0x210); memcpy(&w, &raw, 4); snprintf(v, sizeof v, "%g", w); gfxcov_gap("w_clamp", v, 1, "libgxm's context default; the backends clip at the near plane instead of clamping w"); }
	uint32_t ms = RD32(fp + 0x6c);   /* the fragment program's SceGxmMultisampleMode */
	if (ms) gfxcov_gap("fragment_program_msaa", ms == 1 ? "2x" : ms == 2 ? "4x" : "?", 1, "shaded per sample at sample resolution");
}

/* one draw: the programs, their inputs and the fixed-function state, from the context and any bound
 * precomputed states; streams/primitive/indices as given */
/* ---- the visibility test: libgxm keeps it in the context's state words; its four setters are observed here, where
 * their arguments are typed (SceGxmVisibilityTestMode ENABLED 0x4000, SceGxmVisibilityTestOp SET 0x40000), and every
 * draw carries the counter it feeds (GfxDraw.vis_addr). Per context (Killzone has one). */
static struct { uint32_t cx, buf, stride, index; int on, set; } vis[4];
static int vis_slot(uint32_t cx) { for (int i = 0; i < 4; i++) if (vis[i].cx == cx) return i; for (int i = 0; i < 4; i++) if (!vis[i].cx) { vis[i].cx = cx; return i; } return 0; }
static int vis_log(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_LOG_VIS") != 0; return v; }
static void o_vis_buffer(Cpu *c) { int i = vis_slot(A0); vis[i].buf = A1; vis[i].stride = A2; if (vis_log()) { static int n; if (n++ < 8) fprintf(stderr, "    vis: context %#x buffer %#x stride per core %u\n", A0, A1, A2); } }   /* (context, bufferBase, stridePerCore) */
static void o_vis_enable(Cpu *c) { vis[vis_slot(A0)].on = A1 == 0x4000u; if (vis_log()) { static int n; if (n++ < 20) fprintf(stderr, "    vis: test %s (index %u)\n", A1 == 0x4000u ? "on" : "off", vis[vis_slot(A0)].index); } }
static void o_vis_index(Cpu *c) { vis[vis_slot(A0)].index = A1; }
static void o_vis_op(Cpu *c) { vis[vis_slot(A0)].set = A1 == 0x40000u; }
/* sceGxmSetFrontDepthBias(context, factor, units), observed for its typed arguments (GfxDraw.depth_bias_*); per context */
static struct { uint32_t cx; int factor, units; } dbias[4];
static int dbias_slot(uint32_t cx) { for (int i = 0; i < 4; i++) if (dbias[i].cx == cx) return i; for (int i = 0; i < 4; i++) if (!dbias[i].cx) { dbias[i].cx = cx; return i; } return 0; }
static void o_depth_bias(Cpu *c) {
	int i = dbias_slot(A0); dbias[i].factor = (int32_t)A1; dbias[i].units = (int32_t)A2;
	static int log = -1; if (log < 0) log = getenv("RECOMP_LOG_DEPTHBIAS") != 0;
	if (log) {   /* each distinct pair once, with how many calls came before it */
		static int seen[64][2], nseen; static unsigned long calls; calls++; int k;
		for (k = 0; k < nseen && !(seen[k][0] == (int32_t)A1 && seen[k][1] == (int32_t)A2); k++) {}
		if (k == nseen && nseen < 64) { seen[nseen][0] = (int32_t)A1; seen[nseen][1] = (int32_t)A2; nseen++; fprintf(stderr, "    gxm: depth bias factor %d units %d (first at call %lu)\n", (int32_t)A1, (int32_t)A2, calls); }
	}
}
static int8_t clamp8(int v) { return (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v); }
static void draw(uint32_t cx, const uint32_t *stream, uint32_t prim, uint32_t index_format, uint32_t indices, uint32_t count) {
	GfxDraw d;
	memset(&d, 0, sizeof d);
	uint32_t pv = RD32(cx + CX_PVSTATE), pf = RD32(cx + CX_PFSTATE);
	uint32_t vp = pv ? RD32(pv) : RD32(cx + CX_VPROG), fp = pf ? RD32(pf) : RD32(cx + CX_FPROG);
	if (!vp || !fp) return;
	/* vertex program: GXP +0, attributes +0x58 (count +0x5c), streams {stride, index source} +0x60 (count +0x64) */
	d.vprog = RD32(vp);
	d.nattr = (int)RD32(vp + 0x5c); if (d.nattr > 16) d.nattr = 16;
	memcpy(d.attr, G(RD32(vp + 0x58)), 8 * d.nattr);
	d.nstream = (int)RD32(vp + 0x64); if (d.nstream > 16) d.nstream = 16;
	for (int i = 0; i < d.nstream; i++) d.stride[i] = RD16(RD32(vp + 0x60) + 4 * i);
	memcpy(d.stream, stream, sizeof d.stream);
	/* fragment program: GXP +0, the blend info copy at [+0x68] (0: no blending) */
	d.fprog = RD32(fp);
	if (RD32(fp + 0x68)) {
		uint32_t b = RD32(RD32(fp + 0x68));
		d.blend = (GfxBlend){1, b & 0xff, (b >> 8) & 15, (b >> 12) & 15, (b >> 16) & 15, (b >> 20) & 15, (b >> 24) & 15, (b >> 28) & 15};
	}
	/* uniform buffers and textures */
	if (pv) {
		uint32_t gxp = d.vprog;
		d.vuniforms = RD32(pv + 8);
		for (int i = 0; i < 14; i++) d.vubuf[i] = state_ubuf(gxp, RD32(pv + 4), RD32(pv + 0xc), i);
	} else {
		d.vuniforms = default_ubuf(cx, 0x228, 0x27c, CX_VUBUF + 4 * 14);
		for (int i = 0; i < 14; i++) d.vubuf[i] = bound(RD32(cx + CX_VUBUF + 4 * i), 0x3e000000u);
	}
	if (pf) {
		uint32_t gxp = d.fprog;
		d.funiforms = RD32(pf + 0xc);
		for (int i = 0; i < 14; i++) d.fubuf[i] = state_ubuf(gxp, RD32(pf + 8), RD32(pf + 0x10), i);
		/* textures in sampler order at [state+4], sampler k -> unit fprog[+0x38][k] */
		uint32_t n = RD32(fp + 0x34), units = RD32(fp + 0x38);
		for (uint32_t k = 0; k < n && k < 16; k++) {
			uint32_t u = RD32(units + 4 * k);
			if (u < 16) d.tex[u] = texture(RD32(pf + 4) + 16 * k);
		}
		/* a unit the program reads that the state's sampler table doesn't cover: a dependent texture (sampled
		 * by an SMP from words in the program's secondary attributes, not preloaded by the PDS) */
		state_sa_textures(d.fprog, RD32(pf + 0x10), RD32(pf + 0x14), d.tex, "fragment");
	} else {
		d.funiforms = default_ubuf(cx, 0x48, 0x9c, CX_FUBUF + 4 * 14);
		for (int i = 0; i < 14; i++) d.fubuf[i] = bound(RD32(cx + CX_FUBUF + 4 * i), 0x3e000000u);
		for (int i = 0; i < 16; i++) if (RD32(cx + CX_FTEX + 16 * i + 8)) d.tex[i] = texture(cx + CX_FTEX + 16 * i);
	}
	/* the vertex program's textures, when it samples any (GXP +0x1c/+0x20: 4 usage bits a unit): the
	 * context's (sceGxmSetVertexTexture), or a precomputed vertex state's SA words -- the vertex stage
	 * has no PDS texture block, every vertex texture reaches the program as SA words */
	if (is_gxp(d.vprog) && (RD32(d.vprog + 0x1c) | RD32(d.vprog + 0x20))) {
		if (pv) state_sa_textures(d.vprog, RD32(pv + 0xc), RD32(pv + 0x10), d.vtex, "vertex");
		else for (int i = 0; i < 16; i++) if (RD32(cx + CX_VTEX + 16 * i + 8)) d.vtex[i] = texture(cx + CX_VTEX + 16 * i);
	}
	/* RECOMP_LOG_DEPTEX=1: a fragment program's dependent sampler (GXP +128/+132 table: SA slot -> unit) whose unit the
	 * draw has no texture for -- its SMP samples transparent black. Once per program and unit, with the context's
	 * words for the unit (Killzone's body-shadow decal 0x2a4db910 reads units 1-4, none bound: its whole box went black) */
	if (getenv("RECOMP_LOG_DEPTEX") && is_gxp(d.fprog)) {
		uint32_t dc = RD32(d.fprog + 128), dof = RD32(d.fprog + 132), db = dof ? d.fprog + 132 + dof : 0;
		for (uint32_t i = 0; db && i < dc && i < 32; i++) {
			uint32_t rilo = RD16(db + 4 * i), u = rilo / 4;
			if ((rilo & 3) || u >= 16 || d.tex[u].data) continue;
			static uint32_t seen[128]; static int nseen; uint32_t key = (d.fprog & ~15u) | u; int k;
			for (k = 0; k < nseen && seen[k] != key; k++);
			if (k < nseen || nseen >= 128) continue;
			seen[nseen++] = key;
			uint32_t t = cx + CX_FTEX + 16 * u;
			fprintf(stderr, "    gxm: fragment %#x: dependent sampler unit %u unbound (pf %#x; context words %08x %08x %08x %08x)\n", d.fprog, u, pf, RD32(t), RD32(t + 4), RD32(t + 8), RD32(t + 12));
		}
	}
	d.prim = prim; d.index_format = index_format; d.indices = indices; d.count = count;
	/* fixed function: the front ISP and stencil words (the renderer has no separate back-face state) */
	uint32_t front = RD32(cx + CX_FRONT), st = RD32(cx + CX_FRONT_STENCIL), cull = RD32(cx + CX_CULL);
	d.cull = (int)(cull & 3);
	d.polygon_mode = (uint8_t)((front >> 15) & 7); d.point_line_width = (uint8_t)((front >> 28) + 1);
	d.depth_func = (front >> 22) & 7;
	d.depth_write = !(front & 0x100000);
	if (front & 0x100) {   /* stencil test enabled */
		d.stencil_func = (st >> 25) & 7; d.stencil_fail = (st >> 22) & 7; d.depth_fail = (st >> 19) & 7; d.depth_pass = (st >> 16) & 7;
		d.stencil_cmask = (st >> 8) & 0xff; d.stencil_wmask = st & 0xff;
	} else {
		d.stencil_func = 7; d.stencil_cmask = d.stencil_wmask = 0xff;   /* ALWAYS, KEEP */
	}
	d.stencil_ref = front & 0xff;
	d.fp_disabled = (front & 0x200000) != 0;
	d.viewport_set = 1; d.viewport_disabled = (cull & 0x10000) != 0;
	float v[6]; memcpy(v, G(cx + CX_VIEWPORT), sizeof v);
	d.vp_xoff = v[0]; d.vp_xscale = v[1]; d.vp_yoff = v[2]; d.vp_yscale = v[3]; d.vp_zoff = v[4]; d.vp_zscale = v[5];
	uint32_t ca = RD32(cx + CX_CLIP), cb = RD32(cx + CX_CLIP + 4);   /* in 32-pixel tiles */
	if (clip_first_draw) { clip_first_draw = 0; fprintf(stderr, "    clip: first draw: context clip %#x %#x\n", ca, cb); }
	d.clip_mode = ca & 0xc0000000u;
	d.clip_xmin = ((ca >> 16) & 0xff) << 5; d.clip_xmax = ((ca & 0xff) << 5) + 31;
	d.clip_ymin = ((cb >> 16) & 0xff) << 5; d.clip_ymax = ((cb & 0xff) << 5) + 31;
	ndraws++;
	if (checking()) check_draw(cx, &d, vp, fp, pv, pf);
	if (gfxcov_on()) decode_gaps(cx, d.vprog, fp, front, cull, prim);
	/* the visibility test: every draw with it on carries its counter to the renderer (GfxDraw.vis_addr, vis_set), SET-op
	 * tests (Killzone's occlusion culling: a few hundred bounding boxes a frame against a 128x64 depth tile of its own) and
	 * INCREMENT ones (its flares: the count is their brightness) alike. Each renderer decides what it can count truly: the
	 * GPU counts both; the software path reports SET tests visible (gfx_soft.c vis_report). SET tests are written 1 here
	 * for every renderer unless RECOMP_VIS_SET=gpu (counted on the GPU) or real (counted by the software path too): the GPU
	 * counts aren't right yet -- with them on (6eecbdc, 2026-10-21) Killzone hid a door and other things in plain view, and
	 * two consecutive frames of one view disagreed on ~20 of 73 boxes (a 1.12 capture, frame_4802: scenes 6 and 12). */
	{ int i = dbias_slot(cx); d.depth_bias_factor = clamp8(dbias[i].factor); d.depth_bias_units = clamp8(dbias[i].units); }
	{ int i = vis_slot(cx); if (vis[i].on && vis[i].buf) {
		static int stub = -1; if (stub < 0) { const char *e = getenv("RECOMP_VIS_SET"); stub = !(e && (!strcmp(e, "gpu") || !strcmp(e, "real"))); }
		if (vis[i].set && stub) gfx_vis_write(vis[i].buf + 4u * vis[i].index, 1);
		else { d.vis_addr = vis[i].buf + 4u * vis[i].index; d.vis_set = (uint8_t)vis[i].set; } } }
	gfx_draw(&d);
	vis_record(cx, front);
}

static void streams_of(uint32_t cx, uint32_t *s) { for (int i = 0; i < 16; i++) s[i] = bound(RD32(cx + CX_STREAM + 4 * i), 0x3d000000u); }

static void o_draw(Cpu *c) {   /* sceGxmDraw(context, primitive, index format, indices, count) */
	uint32_t s[16]; streams_of(A0, s);
	draw(A0, s, A1, A2, A3, STK(0));
}
static void o_draw_instanced(Cpu *c) {   /* (context, primitive, index format, indices, count, index wrap): one instance, as gxm.c */
	uint32_t s[16]; streams_of(A0, s);
	draw(A0, s, A1, A2, A3, STK(0));
}
/* sceGxmDrawPrecomputed(context, draw): the draw keeps its parameters only as the six VDM words at
 * +0x14 and its streams patched into its PDS fetch instance ([+4]) through the vertex program's
 * patch table (vprog+0x30: {u16 stream, u16 offset, u32 word index}, count vprog+0x34) */
static void o_draw_precomputed(Cpu *c) {
	uint32_t pd = A1;
	if (!pd) return;
	uint32_t vp = RD32(pd), inst = RD32(pd + 4), w0 = RD32(pd + 0x14);
	uint32_t s[16] = {0}, table = RD32(vp + 0x30), n = RD32(vp + 0x34);
	for (uint32_t k = 0; k < n && k < 32; k++) {
		uint32_t si = RD16(table + 8 * k);
		if (si < 16) s[si] = RD32(inst + 4 * RD32(table + 8 * k + 4)) - RD16(table + 8 * k + 2);
	}
	/* the draw's own vertex program stands in for the context's when no precomputed vertex state says otherwise */
	uint32_t saved = RD32(A0 + CX_VPROG);
	if (!RD32(A0 + CX_PVSTATE)) WR32(A0 + CX_VPROG, vp);
	draw(A0, s, w0 & 0x1c000000u, RD32(pd + 0x1c) & 0x01000000u, RD32(pd + 0x18) & ~1u, w0 & 0x3fffff);
	WR32(A0 + CX_VPROG, saved);
}

/* sceGxmBeginScene(context, flags, render target, valid region, vertex sync, fragment sync, colour
 * surface, depth/stencil) and sceGxmBeginSceneEx(..., colour surface, load depth/stencil, store
 * depth/stencil) -- the surfaces in Sony's layouts:
 *   SceGxmColorSurface: +0 bit 2 valid, +4 format (& 0xf1b00000), +8 data, +0xc stride/2 - 1 (bits 0-13),
 *     +0x20 a SceGxmTexture of the surface (its width/height)
 *   SceGxmDepthStencilSurface: +0 control (bit 1 force load, bit 2 force store, format & 0x07eee000;
 *     0 = disabled), +4 depth data, +8 stencil data, +0xc background depth, +0x10 bits 0-7 background stencil */
static void begin_scene(uint32_t rt, uint32_t cs, uint32_t ds_load, uint32_t ds_store, uint32_t valid) {
	GfxTarget t;
	if (clip_log()) {
		fprintf(stderr, "    clip: scene %u colour %#x valid region %s", nscenes + 1, cs ? RD32(cs + 8) & ~3u : 0, valid ? "" : "none");
		if (valid) fprintf(stderr, "%u,%u - %u,%u", RD32(valid), RD32(valid + 4), RD32(valid + 8), RD32(valid + 12));
		fprintf(stderr, "\n"); clip_first_draw = 1;
	}
	memset(&t, 0, sizeof t);
	nscenes++;
	if (checking()) {
		if (!cs) issue("scene without a colour surface");
		else if (!(RD32(cs) & 4)) issue("scene's colour surface %#x is not initialised (word 0 bit 2 clear)", cs);
		else if (!mapped(RD32(cs + 8) & ~3u, 4)) issue("scene's colour surface data %#x not in guest memory", RD32(cs + 8) & ~3u);
		uint32_t ds = ds_store ? ds_store : ds_load;
		if (ds && RD32(ds) && RD32(ds + 4) && !mapped(RD32(ds + 4), 4)) issue("scene's depth data %#x not in guest memory", RD32(ds + 4));
	}
	if (cs && RD32(cs) & 4) {
		GfxTexture bt = texture(cs + 0x20);
		t.data = RD32(cs + 8) & ~3u; t.format = (RD32(cs + 4) & 0xf1b00000u) | (srgb_on() ? RD32(cs) & 0x3000u : 0);   /* format + the gamma mode: word 0 bits 12-13, as Sony's sceGxmColorSurfaceGetGammaMode reads it (ldr [r0]; and #0x3000), kept in the format's unused bits 12-13 (gfx.h GFX_TGT_GAMMA) */
		if ((RD32(cs) & 0x3000u) && getenv("RECOMP_LOG_GAMMA")) {   /* which surfaces are sRGB (once each; logged whether or not RECOMP_SRGB applies them) */
			static uint32_t seen[32]; static int nseen; int k; for (k = 0; k < nseen; k++) if (seen[k] == t.data) break;
			if (k == nseen && nseen < 32) { seen[nseen++] = t.data; fprintf(stderr, "    gamma: colour surface %#x format %#x mode %u (%ux%u)\n", t.data, t.format, (RD32(cs) >> 12) & 3u, t.width, t.height); }
		}
		t.width = bt.width; t.height = bt.height; t.stride = ((RD32(cs + 0xc) & 0x3fff) + 1) * 2;
		t.type = (uint8_t)((RD32(cs + 4) >> 26) & 3);   /* surface type (+4 bits 26-27): LINEAR 0, TILED 1, SWIZZLED 2 */
		t.scale_mode = (uint8_t)(RD32(cs) & 3);         /* sceGxmColorSurfaceGetScaleMode: word 0 & 3 */
	}
	uint32_t rtw = 0, rth = 0, ms = gpu_render_target_info(rt, &rtw, &rth);   /* the render target is the driver's handle (gpu.c) */
	t.ms_x = ms ? (uint8_t)(ms & 0xffff) : 1; t.ms_y = ms ? (uint8_t)(ms >> 16) : 1;
	t.rt_width = (uint16_t)rtw; t.rt_height = (uint16_t)rth;
	uint32_t ds = ds_store ? ds_store : ds_load;
	if (ds && RD32(ds)) {
		uint32_t ctl = RD32(ds);
		t.depth = 1;
		t.ds_format = ctl & 0x07eee000u;
		t.ds_data = RD32(ds + 4); t.ds_stencil_data = RD32(ds + 8);
		uint32_t bg = RD32(ds + 0xc); memcpy(&t.ds_background_depth, &bg, 4);
		t.ds_background_stencil = (uint8_t)RD32(ds + 0x10);
		t.ds_force_load = (RD32(ds_load ? ds_load : ds) >> 1) & 1;
		t.ds_force_store = (ctl >> 2) & 1;
		t.ds_stride = (uint16_t)((((ctl >> 3) & 0xff) + 1) << 5);   /* strideInSamples/32 - 1 at bits 3-10 */
		t.ds_type = (ctl & 0x11000u) ? 1 : 0;                         /* SCE_GXM_DEPTH_STENCIL_SURFACE_TILED */
	}
	if (gfxcov_on()) {   /* the scene's share of decode_gaps */
		if (cs && (RD32(cs) & 4) && (RD32(cs) & 8)) gfxcov_gap("target_dither", "enabled", 0, "written without dither");
		if (t.ms_x * t.ms_y > 1 && !t.scale_mode) gfxcov_gap("render_target_msaa", t.ms_y > t.ms_x ? "2x, samples stored" : "4x, samples stored", 1, "drawn at sample resolution, shaded per sample");
		if (t.ms_x * t.ms_y > 1 && t.scale_mode) gfxcov_gap("render_target_msaa", t.ms_y > t.ms_x ? "2x, downscaled" : "4x, downscaled", 1, "drawn single-sampled: no antialiasing");
		if (t.ms_x * t.ms_y == 1 && t.scale_mode) gfxcov_gap("render_target_msaa", "1x target at 2x the surface, downscaled", 1, "drawn at the surface's size: no supersampling");
	}
	if (getenv("RECOMP_LOG_RT")) {   /* each distinct render target / surface pairing, once */
		static uint32_t seen[64][3]; static int nseen; int k;
		for (k = 0; k < nseen; k++) if (seen[k][0] == rt && seen[k][1] == t.data && seen[k][2] == ms) break;
		if (k == nseen && nseen < 64) {
			seen[nseen][0] = rt; seen[nseen][1] = t.data; seen[nseen][2] = ms; nseen++;
			fprintf(stderr, "    render target %#x %ux%u samples %ux%u: colour %#x %ux%u scale mode %u, depth %#x stride %u\n",
			        rt, rtw, rth, t.ms_x, t.ms_y, t.data, t.width, t.height, t.scale_mode, t.ds_data, t.ds_stride);
		}
	}
	gfx_begin_scene(&t);
}
static void o_begin_scene(Cpu *c) { begin_scene(A2, STK(2), STK(3), STK(3), A3); }
static void o_begin_scene_ex(Cpu *c) {
	if (clip_log()) {
		uint32_t rt = A2, l = STK(3), st = STK(4);
		fprintf(stderr, "    clip: BeginSceneEx flags %#x rt %#x [%08x %08x %08x %08x %08x %08x %08x %08x] vs %#x fs %#x cs %#x load %#x store %#x\n", A1, rt,
		        RD32(rt), RD32(rt + 4), RD32(rt + 8), RD32(rt + 12), RD32(rt + 16), RD32(rt + 20), RD32(rt + 24), RD32(rt + 28), STK(0), STK(1), STK(2), l, st);
		if (l) fprintf(stderr, "    clip:   load  %08x %08x %08x %08x %08x %08x\n", RD32(l), RD32(l + 4), RD32(l + 8), RD32(l + 12), RD32(l + 16), RD32(l + 20));
		if (st) fprintf(stderr, "    clip:   store %08x %08x %08x %08x %08x %08x\n", RD32(st), RD32(st + 4), RD32(st + 8), RD32(st + 12), RD32(st + 16), RD32(st + 20));
	}
	begin_scene(A2, STK(2), STK(3), STK(4), A3);
}
/* sceGxmSetRegionClip(context, mode, xMin, yMin, xMax, yMax): logged with its caller (RECOMP_LOG_CLIP) */
static void o_set_region_clip(Cpu *c) {
	if (!clip_log()) return;
	static long from = -1; if (from < 0) from = atol(getenv("RECOMP_LOG_CLIP"));   /* =N: from scene N */
	static int n; if ((long)nscenes >= from && n++ < 20000) fprintf(stderr, "    clip: SetRegionClip mode %#x %u,%u - %u,%u from %#x (scene %u)\n", A1, A2, A3, STK(0), STK(1), c->r[14], (unsigned)nscenes);
}

/* sceGxmEndScene(context, vertex notification, fragment notification): the scene is drawn when the
 * renderer's end_scene returns; Sony's EndScene then kicks it, and the kick's completion (gpu.c)
 * writes its notifications and status words */
static void o_end_scene(Cpu *c) { gfx_end_scene(); }

/* sceGxmFinish(context): everything drawn back in guest memory (the game may read its render targets) */
static void o_finish(Cpu *c) { gfx_sync_to_guest(); }

/* RECOMP_DUMP_GXP=<dir>: every program the game registers, as <dir>/<address>.gxp (GXP header:
 * "GXP\0", version, then the total size at +8), for usse_dis / usse_run offline */
static void o_register_program(Cpu *c) {   /* sceGxmShaderPatcherRegisterProgram(patcher, program, *id) */
	const char *dir = getenv("RECOMP_DUMP_GXP"); uint32_t prog = A1;
	if (!dir || !prog || memcmp(G(prog), "GXP", 3)) return;
	uint32_t size = RD32(prog + 8);
	if (size < 16 || size > (16u << 20)) return;
	char path[1024]; snprintf(path, sizeof path, "%s/%08x.gxp", dir, prog);
	FILE *f = fopen(path, "wb"); if (f) { fwrite(G(prog), 1, size, f); fclose(f); }
}

/* ---- the TRANSFER engine (sceGxmTransferCopy/Downscale/Fill): a fixed-function 2D blit block the
 * game drives directly (no context, no scene) -- libgxm turns each call into opaque 2D-blit command
 * words for libgpu (SceGpuEs4User_5564C1BE), which the kernel queues and the host completes at once
 * (gpu.c's 0x108f SUBMIT_TRANSFER: a notification-record write + a signal bump, the kick's shape for
 * one record). The blit has to happen here, at the GXM boundary: the bridge only ever sees the
 * opaque command words, while the typed surfaces (address, x/y/w/h, stride, format, layout) are
 * only in the SDK call's own arguments (psp2/gxm.h). Killzone's bloom chain is four
 * sceGxmTransferDownscale calls a frame, each halving the one before -- every distinct
 * format/layout combination any game actually submits is logged once (stderr), so an unsupported
 * one shows up immediately instead of a silently wrong frame. */

/* SceGxmTransferFormat (psp2/gxm.h) -> bytes per pixel; 0 = not decoded here */
static int transfer_bytes(uint32_t fmt) {
	switch (fmt) {
	case 0x00000000u: return 1;    /* U8_R */
	case 0x00010000u: case 0x00020000u: case 0x00030000u: case 0x00040000u:                         /* U4U4U4U4_ABGR, U1U5U5U5_ABGR, U5U6U5_BGR, U8U8_GR */
	case 0x00070000u: case 0x00080000u: case 0x00090000u: case 0x000a0000u: case 0x000f0000u: return 2;   /* VYUY/YVYU/UYVY/YUYV422, RAW16 */
	case 0x00050000u: return 3;    /* U8U8U8_BGR */
	case 0x00060000u: case 0x000d0000u: case 0x00110000u: return 4;   /* U8U8U8U8_ABGR, U2U10U10U10_ABGR, RAW32 */
	case 0x00120000u: return 8;    /* RAW64 */
	case 0x00130000u: return 16;   /* RAW128 */
	default: return 0;
	}
}
/* a pixel's index (not byte offset -- the caller multiplies by transfer_bytes) in a transfer surface
 * by its SceGxmTransferType: linear rows of stride/bpp pixels; tiled = 32x32 tiles, stride/bpp/32
 * tiles per row, each tile row by row (gfx_soft.c's target_index uses the identical formula for a
 * tiled colour surface); swizzled = Morton order (ditto, its type 2). `w`,`h` are the transfer's own
 * width/height (the SDK call carries no others, and every observed caller transfers a whole surface
 * from (0,0), where that is exactly right). */
static uint32_t transfer_index(uint32_t x, uint32_t y, int32_t stride, int bytes, uint32_t type, uint32_t w, uint32_t h) {
	if (type == 0x00400000u) {   /* SCE_GXM_TRANSFER_TILED */
		uint32_t tiles_x = bytes ? (uint32_t)(stride / bytes) / 32 : 0; if (!tiles_x) tiles_x = 1;
		return ((y / 32) * tiles_x + x / 32) * 1024 + (y % 32) * 32 + (x % 32);
	}
	if (type == 0x00800000u) {   /* SCE_GXM_TRANSFER_SWIZZLED */
		uint32_t mw = 1, mh = 1; while (mw < w) mw <<= 1; while (mh < h) mh <<= 1;
		uint32_t m = mw < mh ? mw : mh, idx = 0, bit = 0;
		for (uint32_t k = 1; k < m; k <<= 1, bit += 2) idx |= ((y & k) ? 1u << bit : 0) | ((x & k) ? 2u << bit : 0);
		uint32_t rest = mw > mh ? x / m : y / m;
		return idx + rest * m * m;
	}
	return y * (bytes ? (uint32_t)(stride / bytes) : 0) + x;   /* SCE_GXM_TRANSFER_LINEAR */
}
static void transfer_log_once(const char *op, uint32_t fmt, uint32_t type, uint32_t w, uint32_t h, int ok) {
	struct Seen { const char *op; uint32_t fmt, type; }; static struct Seen seen[64]; static int nseen;
	for (int i = 0; i < nseen; i++) if (!strcmp(seen[i].op, op) && seen[i].fmt == fmt && seen[i].type == type) return;
	if (nseen < 64) { seen[nseen].op = op; seen[nseen].fmt = fmt; seen[nseen].type = type; nseen++; }
	fprintf(stderr, "transfer %s: format %#x type %#x %ux%u%s\n", op, fmt, type, w, h, ok ? "" : " (unsupported format/layout -- not performed)");
}
/* every call's own CPU time printed as it happens (a run a timeout SIGTERMs mid-level-load, as the
 * verify runs below do, never reaches the atexit summary); a running total at a clean exit too.
 * There is no GPU-side transfer path yet -- this is the whole cost, all of it on the CPU */
static uint64_t transfer_ns, transfer_n;
static uint64_t transfer_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static int transfer_log(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_LOG_TRANSFER") != 0; return v; }   /* RECOMP_LOG_TRANSFER=1: every transfer with its time, and a total at exit */
static void transfer_report(void) {
	if (!transfer_n || !transfer_log()) return;
	fprintf(stderr, "transfer: %llu calls, %.3f ms total CPU time, %.1f us avg\n",
	        (unsigned long long)transfer_n, transfer_ns / 1e6, transfer_ns / 1e3 / transfer_n);
}
static uint64_t transfer_begin(void) { static int once; if (!once) { once = 1; atexit(transfer_report); } return transfer_now_ns(); }
static void transfer_end(const char *op, uint64_t t0, uint32_t w, uint32_t h, uint32_t src, uint32_t dst, uint32_t fmt) {
	uint64_t dt = transfer_now_ns() - t0; transfer_ns += dt; transfer_n++;
	if (transfer_log()) fprintf(stderr, "transfer %s: %ux%u %#x -> %#x format %#x in %.1f us\n", op, w, h, src, dst, fmt, dt / 1e3);
}

/* a 2x2 box filter average, rounded */
static uint32_t box4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return (a + b + c + d + 2) / 4; }
/* U5U6U5_BGR's three bit-fields (widths 5,6,5 at shifts 11,5,0 -- the standard 16-bit packed RGB
 * layout), each averaged independently */
static uint16_t downscale_u565(uint32_t s00, uint32_t s10, uint32_t s01, uint32_t s11) {
	uint32_t hi = box4((s00 >> 11) & 31, (s10 >> 11) & 31, (s01 >> 11) & 31, (s11 >> 11) & 31);
	uint32_t mid = box4((s00 >> 5) & 63, (s10 >> 5) & 63, (s01 >> 5) & 63, (s11 >> 5) & 63);
	uint32_t lo = box4(s00 & 31, s10 & 31, s01 & 31, s11 & 31);
	return (uint16_t)((hi << 11) | (mid << 5) | lo);
}

/* sceGxmTransferDownscale(srcFormat, srcAddress, srcX, srcY, srcWidth, srcHeight, srcStride,
 * destFormat, destAddress, destX, destY, destStride, syncObject, syncFlags, notification): a 2x2 box
 * filter, destination (srcWidth/2) x (srcHeight/2) -- both surfaces linear (the SDK signature carries
 * no SceGxmTransferType for either side). Real hardware only downscales the RGB formats (Vita3K's
 * own transfer.cpp: "only rgb formats are supported ... for downscaling"); Killzone's bloom chain --
 * the only caller seen so far -- uses exactly this path four times a frame, each halving the last. */
static void o_transfer_downscale(Cpu *c) {
	uint32_t src_fmt = A0, src_addr = A1, src_x = A2, src_y = A3;
	uint32_t src_w = STK(0), src_h = STK(1); int32_t src_stride = (int32_t)STK(2);
	uint32_t dst_fmt = STK(3), dst_addr = STK(4), dst_x = STK(5), dst_y = STK(6);
	int32_t dst_stride = (int32_t)STK(7);
	if (!src_addr || !dst_addr || !src_w || !src_h) return;
	uint32_t dst_w = src_w / 2, dst_h = src_h / 2;
	int bytes = transfer_bytes(src_fmt);
	/* U5U6U5_BGR 0x30000, U8U8U8_BGR 0x50000, U8U8U8U8_ABGR 0x60000: the formats real hardware box-filters */
	int rgb = src_fmt == 0x00030000u || src_fmt == 0x00050000u || src_fmt == 0x00060000u;
	int ok = bytes && rgb && src_fmt == dst_fmt && dst_w && dst_h;
	transfer_log_once("downscale", src_fmt, src_fmt == dst_fmt ? 0 : 1, src_w, src_h, ok);
	if (!ok) return;
	if (src_stride <= 0) src_stride = (int32_t)(src_w * (uint32_t)bytes);
	if (dst_stride <= 0) dst_stride = (int32_t)(dst_w * (uint32_t)bytes);
	uint32_t sbase = src_addr + src_y * (uint32_t)src_stride + src_x * (uint32_t)bytes;
	uint32_t dbase = dst_addr + dst_y * (uint32_t)dst_stride + dst_x * (uint32_t)bytes;
	uint32_t slen = (uint32_t)src_stride * src_h, dlen = (uint32_t)dst_stride * dst_h;
	uint64_t t0 = transfer_begin();
	if (bytes == 4 && gfx_transfer_downscale(sbase, src_w, src_h, (uint32_t)src_stride, dbase, (uint32_t)dst_stride)) {   /* a GPU-drawn source: done there, no download (gfx_gpu.c) */
		static int check = -1; if (check < 0) check = getenv("RECOMP_GPU_TRANSFER_CHECK") != 0;
		if (check) {   /* RECOMP_GPU_TRANSFER_CHECK=1 (render scale 1): the box filter below over the source's bytes, against what the GPU wrote */
			uint8_t *want = malloc(dlen); uint32_t bad = 0, maxd = 0;
			gfx_guest_reading(sbase, slen);
			for (uint32_t y = 0; y < dst_h; y++) for (uint32_t x = 0; x < dst_w; x++) for (int k = 0; k < 4; k++) {
				uint32_t s0 = sbase + 2 * y * (uint32_t)src_stride + 8 * x + (uint32_t)k, s1 = s0 + (uint32_t)src_stride;
				want[y * (uint32_t)dst_stride + 4 * x + (uint32_t)k] = (uint8_t)box4(RD8(s0), RD8(s0 + 4), RD8(s1), RD8(s1 + 4));
			}
			gfx_guest_reading(dbase, dlen);
			for (uint32_t y = 0; y < dst_h; y++) for (uint32_t i = 0; i < 4 * dst_w; i++) {
				int dd = abs((int)want[y * (uint32_t)dst_stride + i] - (int)RD8(dbase + y * (uint32_t)dst_stride + i)); if (dd) { bad++; if ((uint32_t)dd > maxd) maxd = (uint32_t)dd; }
			}
			static unsigned said; if (said++ < 40 || bad) fprintf(stderr, "transfer check: downscale %ux%u %#x -> %#x on the GPU: %u of %u bytes differ from the CPU's (max %u)\n", src_w, src_h, sbase, dbase, bad, 4 * dst_w * dst_h, maxd);
			free(want);
		}
		transfer_end("downscale (GPU)", t0, src_w, src_h, sbase, dbase, src_fmt); return;
	}
	gfx_guest_reading(sbase, slen); gfx_guest_writing(dbase, dlen);   /* a GPU-rendered source (or a destination mip the GPU still owns) comes down first */
	for (uint32_t y = 0; y < dst_h; y++) {
		uint32_t s0 = sbase + (2 * y) * (uint32_t)src_stride, s1 = s0 + (uint32_t)src_stride, d0 = dbase + y * (uint32_t)dst_stride;
		for (uint32_t x = 0; x < dst_w; x++) {
			uint32_t sx = 2 * x * (uint32_t)bytes, dx = x * (uint32_t)bytes;
			if (bytes == 2) WR16(d0 + dx, downscale_u565(RD16(s0 + sx), RD16(s0 + sx + 2), RD16(s1 + sx), RD16(s1 + sx + 2)));
			else for (int k = 0; k < bytes; k++)   /* U8U8U8_BGR, U8U8U8U8_ABGR: every byte is an independent lane */
				WR8(d0 + dx + (uint32_t)k, (uint8_t)box4(RD8(s0 + sx + (uint32_t)k), RD8(s0 + sx + (uint32_t)bytes + (uint32_t)k),
				                                          RD8(s1 + sx + (uint32_t)k), RD8(s1 + sx + (uint32_t)bytes + (uint32_t)k)));
		}
	}
	gfx_guest_written(dbase, dlen);   /* so a GPU-resident target re-uploads before its next GPU use (the source only came down: it stays the GPU's) */
	transfer_end("downscale", t0, src_w, src_h, sbase, dbase, src_fmt);
}

/* sceGxmTransferCopy(width, height, colorKeyValue, colorKeyMask, colorKeyMode, srcFormat, srcType,
 * srcAddress, srcX, srcY, srcStride, destFormat, destType, destAddress, destX, destY, destStride,
 * syncObject, syncFlags, notification): a byte-for-byte copy -- the hardware requires srcFormat ==
 * destFormat, no conversion -- between two rectangles, each independently linear, tiled or
 * swizzled. colorKeyMode (PASS keeps matches, REJECT drops them) compares the raw bits under
 * colorKeyMask, 32-bit formats only (Vita3K's own note: "not handled for other formats"). */
static void o_transfer_copy(Cpu *c) {
	uint32_t width = A0, height = A1, key_value = A2, key_mask = A3;
	uint32_t key_mode = STK(0), src_fmt = STK(1), src_type = STK(2), src_addr = STK(3);
	uint32_t src_x = STK(4), src_y = STK(5); int32_t src_stride = (int32_t)STK(6);
	uint32_t dst_fmt = STK(7), dst_type = STK(8), dst_addr = STK(9);
	uint32_t dst_x = STK(10), dst_y = STK(11); int32_t dst_stride = (int32_t)STK(12);
	if (!width || !height || !src_addr || !dst_addr) return;
	int bytes = transfer_bytes(src_fmt);
	int ok = bytes != 0 && src_fmt == dst_fmt;
	transfer_log_once("copy", src_fmt, (src_type >> 22) | ((dst_type >> 22) << 2) | (src_fmt == dst_fmt ? 0 : 16), width, height, ok);
	if (!ok) return;
	if (key_mode && bytes != 4) { transfer_log_once("copy colorkey", src_fmt, 0, width, height, 0); key_mode = 0; }
	if (src_stride <= 0) src_stride = (int32_t)(width * (uint32_t)bytes);
	if (dst_stride <= 0) dst_stride = (int32_t)(width * (uint32_t)bytes);
	/* a conservative bound (exact for linear; tiled/swizzled cover the same total bytes, reordered) */
	uint32_t slen = width * height * (uint32_t)bytes, dlen = slen;
	uint64_t t0 = transfer_begin();
	gfx_guest_reading(src_addr, slen); gfx_guest_writing(dst_addr, dlen);
	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			uint32_t si = transfer_index(src_x + x, src_y + y, src_stride, bytes, src_type, width, height) * (uint32_t)bytes;
			uint32_t di = transfer_index(dst_x + x, dst_y + y, dst_stride, bytes, dst_type, width, height) * (uint32_t)bytes;
			if (key_mode) {
				uint32_t v = RD32(src_addr + si) & key_mask;
				int match = v == (key_value & key_mask);
				if ((key_mode == 1) != match) continue;   /* PASS(1): keep on match; REJECT(2): keep on mismatch */
			}
			memmove(G(dst_addr + di), G(src_addr + si), (size_t)bytes);
		}
	}
	gfx_guest_written(dst_addr, dlen);
	transfer_end("copy", t0, width, height, src_addr, dst_addr, src_fmt);
}

/* sceGxmTransferFill(color, destFormat, destAddress, destX, destY, destWidth, destHeight,
 * destStride, syncObject, syncFlags, notification): color's low bytesPerPixel bytes, replicated
 * across the rectangle; linear only -- the SDK gives sceGxmTransferFill no SceGxmTransferType at all */
static void o_transfer_fill(Cpu *c) {
	uint32_t color = A0, dst_fmt = A1, dst_addr = A2, dst_x = A3;
	uint32_t dst_y = STK(0), width = STK(1), height = STK(2); int32_t dst_stride = (int32_t)STK(3);
	if (!dst_addr || !width || !height) return;
	int bytes = transfer_bytes(dst_fmt);
	transfer_log_once("fill", dst_fmt, 0, width, height, bytes != 0);
	if (!bytes) return;
	if (dst_stride <= 0) dst_stride = (int32_t)(width * (uint32_t)bytes);
	uint32_t base = dst_addr + dst_y * (uint32_t)dst_stride + dst_x * (uint32_t)bytes;
	uint32_t len = (uint32_t)dst_stride * height;
	uint64_t t0 = transfer_begin();
	gfx_guest_writing(base, len);
	for (uint32_t y = 0; y < height; y++)
		for (uint32_t x = 0; x < width; x++)
			memcpy(G(base + y * (uint32_t)dst_stride + x * (uint32_t)bytes), &color, (size_t)bytes);
	gfx_guest_written(base, len);
	transfer_end("fill", t0, width, height, 0, base, dst_fmt);
}

/* sceGxmDisplayQueueAddEntry: the game's frame is done, on the thread that drew it (the flip itself lands later, on the
 * display queue's thread: gfx_present) -- the motion vectors' frame boundary (gfx_gpu.c mv_frame_end) */
static void o_display_queue(Cpu *c) { (void)c; gfx_frame_queued(); }
static const HleEntry observers[] = {
	{"sceGxmFinish", o_finish}, {"sceGxmShaderPatcherRegisterProgram", o_register_program},
	{"sceGxmDraw", o_draw}, {"sceGxmDrawInstanced", o_draw_instanced}, {"sceGxmDrawPrecomputed", o_draw_precomputed},
	{"sceGxmBeginScene", o_begin_scene}, {"sceGxmBeginSceneEx", o_begin_scene_ex}, {"sceGxmSetRegionClip", o_set_region_clip}, {"sceGxmEndScene", o_end_scene},
	{"sceGxmTransferCopy", o_transfer_copy}, {"sceGxmTransferDownscale", o_transfer_downscale}, {"sceGxmTransferFill", o_transfer_fill},
	{"sceGxmDisplayQueueAddEntry", o_display_queue},
	{"sceGxmSetVisibilityBuffer", o_vis_buffer}, {"sceGxmSetFrontVisibilityTestEnable", o_vis_enable}, {"sceGxmSetFrontVisibilityTestIndex", o_vis_index}, {"sceGxmSetFrontVisibilityTestOp", o_vis_op}, {"sceGxmSetFrontDepthBias", o_depth_bias},
	{0, 0},
};

HleFn gxm_observer(const char *name) {
	static int once;
	if (!once && checking()) { once = 1; atexit(gxm_sony_report); }
	for (const HleEntry *e = observers; e->name; e++)
		if (!strcmp(e->name, name)) return e->fn;
	return 0;
}
