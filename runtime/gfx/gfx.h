/*
 * The graphics layer between GXM and a rendering backend. gxm_sony.c reads the state Sony's libgxm
 * keeps and hands each draw over as an API-neutral GfxDraw (guest addresses for everything the backend
 * reads: GXP programs, vertex streams, indices, uniform buffers, texture data, the render
 * target's pixels); a backend implements these calls. Two backends link into every binary
 * (gfx_soft.c, gfx_gpu.c); gfx_dispatch.c picks one per RECOMP_GFX ("soft", the default, or
 * "gpu") and forwards gfx_begin_scene/draw/end_scene/present to it.
 */
#ifndef RECOMP_GFX_H
#define RECOMP_GFX_H
#include <stdint.h>
#include "usse.h"

typedef struct { uint16_t stream, offset; uint8_t format, comps; uint16_t reg; } GfxAttr;   /* SceGxmVertexAttribute */
typedef struct {   /* our layout of the game's 16-byte SceGxmTexture; u_mode/v_mode low nibble: the address
	                    mode; u_mode high nibble: layout (0 linear, 1 swizzled, 2 tiled, 3 cube); v_mode high
	                    nibble: mip count. format: the SceGxmTextureFormat (bits 31, 24-28, 12-14) plus, in its
	                    unused bits 0-11, a linear texture's row stride in pixels when it isn't the natural
	                    (width+7)&~7 -- LINEAR_STRIDED textures, e.g. a sub-rectangle of a bloom atlas (0 = natural) */
	uint32_t data, format;
	uint16_t width, height;
	uint8_t min_filter, mag_filter, u_mode, v_mode;
} GfxTexture;
/* sRGB ("gamma") modes. A texture's SceGxmTextureGammaMode (control word 0 bits 27-28: R/BGR 1,
 * GR 3) rides in GfxTexture.format bits 16-17, which the format leaves unused; a colour surface's
 * SceGxmColorSurfaceGammaMode keeps its own place in GfxTarget.format, bits 12-13 (R/BGR 1, GR 3).
 * Non-zero: the texture's 8-bit colour channels are sRGB-encoded and sample as linear values; the
 * surface stores sRGB and the shader's linear output (and blending) is encoded on write. Both used
 * to be masked away: Uncharted marks ~950 textures and its scene surface sRGB, and without the
 * decode/encode its frames were dark and flat ("missing the vibrant greens"). Captures from before
 * carry 0 there, i.e. no gamma, as they rendered. */
#define GFX_TEX_GAMMA(fmt) (((fmt) >> 16) & 3u)
#define GFX_TGT_GAMMA(fmt) (((fmt) >> 12) & 3u)
/* A texture's mip filter and LOD bias ride in GfxTexture.format too: bit 15 the mip filter
 * (SceGxmTextureMipFilter ENABLED, control word 0 bit 9: the two nearest levels blended), bits 18-23
 * the LOD bias (sceGxmTextureSetLodBias, word 0 bits 21-26, 31 = none) less 31, signed, in eighths of
 * a level -- added to the level of detail of every sample. Killzone sets them on most of its
 * textures (-1.5 to +1.25 levels, trilinear on its LINEAR ones); ignored, its positively biased
 * textures sampled finer levels than the game asked for and shimmered in motion. Captures from
 * before carry 0: no bias, the mip filter by min filter alone, as they rendered. */
#define GFX_TEX_MIP_LINEAR(fmt) (((fmt) >> 15) & 1u)
#define GFX_TEX_LOD_BIAS(fmt) ((float)((int32_t)((uint32_t)(fmt) << 8) >> 26) * 0.125f)
typedef struct {
	uint32_t data, format, width, height, stride, depth;   /* a colour surface; depth: a depth/stencil surface is bound */
	/* the depth/stencil surface itself (sceGxmDepthStencilSurfaceInit et al.), added at the end:
	 * an old capture has ds_format 0 and depth either 0 or 1 -- when depth is set but ds_format
	 * is 0, that is the pre-existing behaviour (a depth buffer keyed by the COLOUR address,
	 * cleared to far every scene, never loaded or stored), kept exactly for those captures.
	 * ds_format: raw SceGxmDepthStencilFormat (bit 0x044000 depth present, 0x022000 stencil
	 * present, S8D24 = both packed one 32-bit word/pixel: low 24 depth, high 8 stencil). */
	uint32_t ds_format, ds_data, ds_stencil_data;
	float ds_background_depth;
	uint8_t ds_background_stencil, ds_force_load, ds_force_store;
	/* the colour surface's memory layout (SceGxmColorSurfaceType): 0 linear, 1 tiled (32x32 tiles,
	 * stride/32 tiles per row), 2 swizzled. Added at the end: an old capture reads 0 = linear, as
	 * every target used to be treated. Unit 13 renders its position and HDR buffers tiled and reads
	 * them back as tiled textures; written linearly they came back as garbage. */
	uint8_t type;
	/* the depth/stencil surface's own row stride in samples (sceGxmDepthStencilSurfaceInit's
	 * strideInSamples; a multiple of 32), 0 = the colour target's width as before. Unit 13's depth
	 * buffer is 736 wide for a 720-pixel scene and is read back as a texture with that stride. */
	uint16_t ds_stride;
	/* the render target's samples per pixel (sceGxmCreateRenderTarget's multisampleMode: NONE 1x1,
	 * 2X 1x2, 4X 2x2) and the colour surface's scale mode (sceGxmColorSurfaceGetScaleMode, word 0
	 * bits 0-1: 0 NONE, 1 MSAA_DOWNSCALE). A draw's viewport and region clip are in render-target
	 * pixels. With scale NONE the surfaces store every sample: a 4X target's colour and depth
	 * surfaces are twice its width and height (libgxm's BeginScene checks surface / samples >=
	 * valid region). With DOWNSCALE a 4X target's samples resolve into a surface of its own size,
	 * and a single-sampled target is twice the surface's size. gfx_sample_space maps a draw into
	 * the surface's own pixels for the backends. 0 (an old capture): one sample per pixel, as
	 * those rendered. Unit 13 draws its impact debris into a 4X 360x204 target over its 720x408
	 * scene surfaces; taken as one sample per pixel, every particle landed at half scale in the
	 * top-left quarter of the screen, far from where the shot hit. */
	uint8_t ms_x, ms_y, scale_mode;
	/* the render target's own size in pixels (sceGxmCreateRenderTarget's width/height), 0 in an old
	 * capture. A depth-only scene (no colour surface: Unit 13's shadow atlas) has no other size --
	 * gfx_dispatch.c gives such a scene width/height/stride from it (times the samples per pixel) */
	uint16_t rt_width, rt_height;
	/* the depth/stencil surface's layout (SceGxmDepthStencilSurfaceType, control word bits 12 and 16:
	 * TILED 0x11000): 0 linear, 1 tiled (32x32 tiles, ds_stride/32 per row, like a tiled colour
	 * surface). Unit 13's shadow atlas is tiled and is sampled back as a tiled texture; stored
	 * linearly, its cascades read as streaks. An old capture reads 0 (gfxreplay's
	 * RECOMP_REPLAY_DS_TILED=addr,... names the tiled ones) */
	uint8_t ds_type;
} GfxTarget;
typedef struct {
	int present;
	uint8_t mask, color_func, alpha_func, color_src, color_dst, alpha_src, alpha_dst;
} GfxBlend;

typedef struct {
	uint32_t vprog, fprog;                 /* GXP program binaries */
	GfxAttr attr[16]; int nattr;
	uint16_t stride[16]; uint32_t stream[16]; int nstream;
	GfxTexture tex[16];
	uint32_t vuniforms, funiforms;         /* default uniform buffers */
	uint32_t vubuf[14], fubuf[14];         /* user uniform buffers 0-13 (0 if unbound), for the shader interpreter */
	GfxBlend blend;
	uint32_t prim, index_format, indices, count;   /* prim: the raw SceGxmPrimitiveType (gfx_prim_kind) */
	int cull;
	uint32_t depth_func;                   /* SceGxmDepthFunc >> 22: 0 never, 1 less, 2 equal, 3 less-equal,
	                                          4 greater, 5 not-equal, 6 greater-equal, 7 always */
	int depth_write;
	/* stencil (front faces): func as depth_func's numbering; ops 0 keep, 1 zero, 2 replace,
	 * 3 incr, 4 decr, 5 invert, 6 incr-wrap, 7 decr-wrap */
	uint8_t stencil_func, stencil_fail, depth_fail, depth_pass, stencil_cmask, stencil_wmask, stencil_ref;
	/* draw state the backends used to ignore, added at the end so an old capture (all zero) keeps
	 * exactly its old behaviour: */
	uint8_t viewport_set;      /* sceGxmSetViewport was called (0 in an old capture: screen = NDC
	                            * mapped by the target's own size, as today, flipping y) */
	uint8_t viewport_disabled; /* SCE_GXM_VIEWPORT_DISABLED: the clip position IS the screen position */
	float vp_xoff, vp_xscale, vp_yoff, vp_yscale, vp_zoff, vp_zscale;   /* sceGxmSetViewport */
	uint32_t clip_mode;        /* SceGxmRegionClipMode; 0 NONE (default: no clip, as today) */
	uint32_t clip_xmin, clip_ymin, clip_xmax, clip_ymax;                /* sceGxmSetRegionClip, in pixels */
	uint8_t fp_disabled;       /* sceGxmSetFrontFragmentProgramEnable(..., DISABLED): depth/stencil
	                            * still run, no colour is written (0 default: enabled, as today) */
	/* sceGxmSetFrontPolygonMode: the SceGxmPolygonMode value >> 15 (the ISP word's bits 15-17): 0
	 * TRIANGLE_FILL, 1 LINE, 2 POINT_10UV, 3 POINT, 4 POINT_01UV, 5 TRIANGLE_LINE, 6 TRIANGLE_POINT.
	 * A SCE_GXM_PRIMITIVE_POINTS draw is rasterised as point sprites (gfx_soft.c, render_points_interp)
	 * and 01UV/10UV say which way the sprite coordinate runs; the polygon modes of a triangle draw are
	 * not modelled (filled). 0 in an old capture: fill. */
	uint8_t polygon_mode;
	/* sceGxmSetFrontPointLineWidth, 1..16 pixels (the ISP word's bits 28-31, plus one): a point's size
	 * when its vertex program writes no PSIZE output. 0 in an old capture: 1 pixel. */
	uint8_t point_line_width;
	/* the vertex program's textures (sceGxmSetVertexTexture, units 0-15; a precomputed vertex state's
	 * rebuilt from its SA words), read only when the program samples any. The vertex units are their
	 * own set: the software path used to give a vertex SMP the FRAGMENT texture of the same unit
	 * (Killzone's lens flares read their visibility in the vertex program). Empty in an old capture. */
	GfxTexture vtex[16];
	/* the visibility test (sceGxmSetVisibilityBuffer, sceGxmSetFrontVisibilityTest{Enable,Index,Op}): the counter the
	 * draw's samples that pass depth and stencil go to -- core 0's word, buffer + 4 * index -- or 0 with the test off;
	 * vis_set: the SET op (written nonzero when any passes) rather than INCREMENT (the count). Written at the end of the
	 * scene, as Vita3K's occlusion queries are copied out. Killzone hides its flares and glows behind walls with it.
	 * 0 in an old capture: no test. */
	uint32_t vis_addr; uint8_t vis_set;
	/* sceGxmSetFrontDepthBias(context, factor, units): GL's polygon offset -- depth += factor * the primitive's depth slope
	 * + units * the depth format's smallest step (Vita3K passes them through as such). Killzone's decals (bullet holes,
	 * blood) lean on it to win against the wall they lie on; without it they z-fought and flashed in and out. 0 in an
	 * old capture. */
	int8_t depth_bias_factor, depth_bias_units;
} GfxDraw;

/* GfxDraw.prim is the SceGxmPrimitiveType exactly as the game passed it to sceGxmDraw (and as
 * sceGxmDrawPrecomputed keeps it in its VDM word): 0x00000000 triangles, 0x04000000 lines,
 * 0x08000000 points, 0x0C000000 strip, 0x10000000 fan, 0x14000000 triangle edges -- every capture
 * holds those raw values. Both backends used to test it against 3 and 4, as if it were the enum's
 * ordinal: no strip or fan ever drew, and Killzone: Mercenary, whose UI and menus are strips (672K
 * of 921K draws in a 12K-frame run) and a few fans, presented black frames. The kind is the
 * ordinal (bits 26-28); the triangles a draw rasterises, in the index stream's positions, come from
 * gfx_prim_tris/gfx_prim_tri so the two backends and the coverage report agree on them. */
/* a draw's viewport and region clip from render-target pixels into the target surface's own pixels
 * (GfxTarget.ms_x): a surface that stores samples is drawn at sample resolution -- each sample is
 * shaded, where the hardware shades once per pixel and writes the covered samples; a
 * supersampled DOWNSCALE target is drawn at the surface's size -- one sample per surface pixel
 * where the hardware averages 2x2. Returns 0 when the two spaces are the same (d untouched). */
static inline int gfx_sample_space(const GfxTarget *t, GfxDraw *d) {
	float kx = 1, ky = 1;
	if (!t->ms_x || !t->ms_y) return 0;
	if (t->scale_mode == 0) { kx = t->ms_x; ky = t->ms_y; }
	else if (t->ms_x == 1 && t->ms_y == 1) kx = ky = 0.5f;
	if (kx == 1 && ky == 1) return 0;
	if (d->viewport_set && d->viewport_disabled) {   /* the clip position is the screen position, in target pixels */
		d->viewport_disabled = 0;
		d->vp_xoff = 0; d->vp_xscale = kx; d->vp_yoff = 0; d->vp_yscale = ky; d->vp_zoff = 0; d->vp_zscale = 1;
	} else if (d->viewport_set) {
		d->vp_xoff *= kx; d->vp_xscale *= kx; d->vp_yoff *= ky; d->vp_yscale *= ky;
	}
	d->clip_xmin = (uint32_t)(d->clip_xmin * kx); d->clip_xmax = (uint32_t)((d->clip_xmax + 1) * kx) - 1;
	d->clip_ymin = (uint32_t)(d->clip_ymin * ky); d->clip_ymax = (uint32_t)((d->clip_ymax + 1) * ky) - 1;
	return 1;
}

enum { GFX_PRIM_TRIANGLES, GFX_PRIM_LINES, GFX_PRIM_POINTS, GFX_PRIM_STRIP, GFX_PRIM_FAN, GFX_PRIM_EDGES };
static inline int gfx_prim_kind(uint32_t prim) { return (int)((prim >> 26) & 7); }
/* how many triangles `count` indices make (0 for lines, which neither backend draws, and for points:
 * the software path rasterises those as point sprites, render_points_interp, and the GPU path hands
 * them to it).
 * TRIANGLE_EDGES is a triangle list whose edges carry per-edge flags for line/edge rendering
 * modes; filled, it is the triangle list itself. */
static inline uint32_t gfx_prim_tris(uint32_t prim, uint32_t count) {
	switch (gfx_prim_kind(prim)) {
	case GFX_PRIM_TRIANGLES: case GFX_PRIM_EDGES: return count / 3;
	case GFX_PRIM_STRIP: case GFX_PRIM_FAN: return count >= 3 ? count - 2 : 0;
	default: return 0;
	}
}
/* triangle t's three positions in the index stream: a strip's odd triangles swap their first two
 * vertices so every triangle keeps the strip's winding (culling depends on it); a fan's triangles
 * all start at its first vertex */
static inline void gfx_prim_tri(uint32_t prim, uint32_t t, uint32_t pos[3]) {
	switch (gfx_prim_kind(prim)) {
	case GFX_PRIM_STRIP: pos[0] = t + (t & 1); pos[1] = t + 1 - (t & 1); pos[2] = t + 2; return;
	case GFX_PRIM_FAN: pos[0] = 0; pos[1] = t + 1; pos[2] = t + 2; return;
	default: pos[0] = 3 * t; pos[1] = 3 * t + 1; pos[2] = 3 * t + 2; return;
	}
}

void gfx_begin_scene(const GfxTarget *t);
void gfx_draw(const GfxDraw *d);
void gfx_end_scene(void);
void gfx_present(uint32_t base, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format);
void gfx_frame_queued(void);   /* sceGxmDisplayQueueAddEntry, on the game's thread: its frame is done (the motion vectors' frame boundary) */
void gfx_shader_prep_tick(void);   /* the window's loop while the game is paused: the shader warm-up steps on (gfx_gpu.c's shader prep card) */
void gfx_sync_to_guest(void);   /* the active backend's render targets written back to guest memory */

/* each backend (gfx_soft.c, gfx_gpu.c) implements this same set under its own prefix; gfx_dispatch.c forwards to one */
void gfx_soft_begin_scene(const GfxTarget *t);
void gfx_soft_draw(const GfxDraw *d);
void gfx_soft_end_scene(void);
void gfx_soft_present(uint32_t base, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format);
int gfx_soft_shot_due(void);        /* the coming present writes a RECOMP_SHOT from guest memory */
extern int gfx_soft_skip_window;    /* the GPU backend has shown this frame itself */

void gfx_gpu_begin_scene(const GfxTarget *t);
void gfx_gpu_draw(const GfxDraw *d);
void gfx_gpu_end_scene(void);
void gfx_gpu_frame_queued(void);
int gfx_gpu_mv_on(void);   /* the motion vectors are on (RECOMP_MV, or RECOMP_FSR which needs them): no lock needed */
void gfx_gpu_shader_prep_tick(void);
int gfx_gpu_shader_prep(int *done, int *total, double *eta_s);   /* the shader prep card's state for the overlay: no lock needed */
void gfx_gpu_shader_prep_skip(void);
void gfx_gpu_present(uint32_t base, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format);
int gfx_gpu_readbacks_due(void **fences, int max);   /* under the renderer lock, right before present: the fences of the readbacks present would wait for, pinned for the caller (gfx_gpu.c) */
void gfx_gpu_readbacks_wait(void **fences, int n);   /* with the lock released: wait for them -- the presenting thread pacing itself to the GPU, not renderer time */
void gfx_gpu_sync_all(void);   /* every GPU render target written back to guest memory */
/* RECOMP_HITCH_LOG (display.c): what the GPU backend did in the frame just presented -- its first-use work
 * (shaders built: translate/msl/compile; pipelines created; textures uploaded) and its waits (target
 * downloads, readbacks, ring wraps), counts and microseconds. Zeros on the software backend. */
typedef struct { uint32_t draws, fallback, shaders, pipes, tex_uploads, tex_new, tgt_downloads, readback_waits, ring_waits, flushes, shaders_cached, skipped;   /* skipped: draws waiting for an asynchronous shader/pipeline build (gfx_gpu.c's gpu_async) */
	uint64_t t_total, t_shader, t_xlate, t_msl, t_compile, t_pipe, t_texup, t_tgt_download, t_readback_wait, t_ring_wait, t_vfetch, t_tsync, t_scene_sync, t_vfetch_max, t_map;
	const char *fb_why[4]; uint32_t fb_n[4];   /* why the software-path draws went there (the first four reasons) */
	uint32_t nshaders, npipes; } GfxGpuFrameCosts;   /* the shader and pipeline tables' sizes so far (each has a fixed capacity) */
void gfx_gpu_last_frame_costs(GfxGpuFrameCosts *out);
void gfx_gpu_builds_stop(void);   /* before exit(): the build workers stop (gfx_gpu.c) */
void gfx_gpu_builds_wait(void);   /* asynchronous shader/pipeline builds (RECOMP_GPU_ASYNC): wait for every queued one */
void gfx_gpu_guest_writing(uint32_t addr, uint32_t len);   /* guest memory is about to change behind the GPU's back: overlapping GPU-drawn targets download first */
void gfx_gpu_guest_written(uint32_t addr, uint32_t len);   /* ... and has changed: overlapping targets re-upload before their next GPU use */
void gfx_guest_writing(uint32_t addr, uint32_t len);
void gfx_guest_written(uint32_t addr, uint32_t len);
void gfx_gpu_guest_reading(uint32_t addr, uint32_t len);   /* the host is about to READ guest memory (a transfer's source): overlapping GPU-drawn targets download first, nothing is marked changed */
void gfx_guest_reading(uint32_t addr, uint32_t len);
/* sceGxmTransferDownscale on the GPU when its source is a GPU colour target (gfx_gpu.c): 1 done, 0 the caller's CPU path */
int gfx_gpu_transfer_downscale(uint32_t src, uint32_t sw, uint32_t sh, uint32_t sstride, uint32_t dst, uint32_t dstride);
int gfx_transfer_downscale(uint32_t src, uint32_t sw, uint32_t sh, uint32_t sstride, uint32_t dst, uint32_t dstride);
void gfx_vis_write(uint32_t addr, uint32_t count);   /* a visibility counter's result into guest memory (gfx_dispatch.c: bracketed as a host write) */
void gfx_soft_use_interp(void);   /* the software path on the shader interpreter, whatever RECOMP_SHADERS says */

/* gfx_soft.c's vertex-attribute-fetch/pack (§1.11), shared with gfx_gpu.c: see its definition */
void gfx_fetch_vertex_pa(const GfxDraw *d, const UsseVertexLayout *vl, uint32_t ix, UsseInst *inst);
int gfx_draw_clip_bounds(const GfxDraw *d, int samples, float out[6]);
/* the same extent over EVERY vertex of a draw of up to GFX_BOUNDS_MAX indices, in three steps so the vertex program runs can
 * go to another thread: prepare (render thread: the program, its secondary, each distinct vertex's PA words copied; 0 can't,
 * 1 run it, 2 the cached result is ready), run (any thread, the job's own copies only), result (render thread: caches it;
 * 0 no extent: a vertex behind the eye) */
#define GFX_BOUNDS_MAX 512
typedef struct GfxBoundsJob GfxBoundsJob;
GfxBoundsJob *gfx_bounds_job_new(void);
int gfx_bounds_prepare(GfxBoundsJob *j, const GfxDraw *d);
void gfx_bounds_run(GfxBoundsJob *j);
int gfx_bounds_result(GfxBoundsJob *j, float out[6]);   /* a draw's NDC extent and w range, its vertex program run on the CPU for up to `samples` of its vertices (gfx_soft.c) */
void gfx_fetch_vertex_pa_words(const GfxDraw *d, const UsseVertexLayout *vl, uint32_t ix, uint32_t *out, int npa);   /* the first npa PA words only (gfx_gpu.c's vertex record) */
/* the same packing with the per-draw decisions (slot lookups, types, strides) made once: a plan per
 * attribute, then one call per vertex writing npa words -- the GPU backend packs hundreds of
 * thousands of vertices a frame */
typedef struct { uint32_t base, stride; uint16_t reg; uint8_t fmt, comps, dcomps, kind, rawlen; } GfxPackAttr;   /* kind: 1 raw bytes, 2 F32 lanes, 3 F16 lanes */
typedef struct { GfxPackAttr a[32]; int n, npa, slow, index_reg, instance_reg; const GfxDraw *d; const UsseVertexLayout *vl; } GfxVertexPacker;
void gfx_vertex_packer_init(GfxVertexPacker *pk, const GfxDraw *d, const UsseVertexLayout *vl, int npa);
void gfx_vertex_pack(const GfxVertexPacker *pk, uint32_t ix, uint32_t *out);
/* gfx_soft.c's texture format/swizzle/block-compression/PVRTC decode, bulk (one texel() call per
 * pixel) instead of per-sample -- for gfx_gpu.c's texture upload; see its definition */
void gfx_decode_texture_rgba8(const GfxTexture *t, uint8_t *out);
void gfx_decode_texture_f32(const GfxTexture *t, float *out);   /* the same as RGBA floats, unclamped (wide formats for the GPU backend) */
/* gfx_soft.c's mip chain: the number of levels a texture carries (1 for strided-linear and cube
 * textures, which sample level 0), and a GfxTexture view of one level (its own data address and
 * size) that gfx_decode_texture_rgba8 can decode -- the same addressing the software sampler uses */
int gfx_texture_levels(const GfxTexture *t);
void gfx_texture_level(const GfxTexture *t, int level, GfxTexture *out);
void gfx_texture_face(const GfxTexture *t, int face, GfxTexture *out);
void gfx_texture_face_level(const GfxTexture *t, int face, int level, GfxTexture *out);   /* level `level` of that face (its mip chain follows it) */
int gfx_texture_cube_levels(const GfxTexture *t);   /* a cube's levels per face */   /* a cube texture's face (+X -X +Y -Y +Z -Z) as a linear 2D view */
/* a UBC1/2/3 (DXT1/3/5) level's or face's blocks in row-major block order, addressed exactly as the
 * software decoder reads them (layout at block granularity): the GPU backend uploads these to a
 * BC texture instead of decoding. Returns the block size (8/16), 0 for other formats. */
int gfx_texture_blocks(const GfxTexture *t, uint8_t *out);
/* gfx_soft.c's colour-target format encode/decode (565, U2F10F10F10, F16, F32, plain RGBA8 variants
 * and their channel-order swizzles), bulk and format-aware, reusing target_read_of/target_write_of
 * exactly -- for gfx_gpu.c's guest-memory round trip and RECOMP_GFX_CHECK; see their definitions.
 * `out`/`in` are tightly packed RGBA8, t->stride*t->height pixels. */
void gfx_decode_target_rgba8(const GfxTarget *t, uint8_t *out);
void gfx_encode_target_rgba8(const GfxTarget *t, const uint8_t *in);
void gfx_decode_target_f32(const GfxTarget *t, float *out);       /* the same, RGBA floats at full precision */
void gfx_encode_target_f32(const GfxTarget *t, const float *in);
int gfx_target_plain_rgba8(const GfxTarget *t);   /* a linear U8U8U8U8 surface in R,G,B,A byte order: an RGBA8 GPU texture's bytes are its bytes (no float round trip) */
/* gfx_soft.c's own raw bytes/pixel for a target's declared colour format (1/2/4/8) */
int gfx_target_bpp(const GfxTarget *t);
int gfx_texture_bpp(const GfxTexture *t);   /* bytes per texel of the texture's format, 0 for a block format (gfx_soft.c) */
/* gfx_soft.c's depth/stencil surface decode (its force-load): w*h depth values (0..1) and stencil
 * bytes from guest memory in the surface's declared format, background values where it has no plane */
void gfx_decode_depth_stencil(const GfxTarget *t, uint32_t w, uint32_t h, float *z, uint8_t *st);
#endif
