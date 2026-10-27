/*
 * Translates a decoded USSE program (usse.c's UOp micro-op array, via usse_internal.h) into a
 * SPIR-V shader module, for the GPU backend (gfx_gpu.c). Only the PRIMARY program is translated:
 * the secondary ("uniform setup") program still runs once per draw on the CPU exactly as the
 * software interpreter does (usse_run_secondary), producing the resolved SA register array that
 * gets uploaded as a uniform buffer -- the GPU shader only needs to reproduce what
 * usse_run_primary does, reading SA as read-only input. This mirrors the existing split in
 * gfx_soft.c (vdraw/fdraw are built once per draw, not per vertex/fragment instance) and avoids
 * translating container/literal/buffer-address-table resolution into GPU code at all.
 *
 * Coverage (checked by usse_spirv_translate, same philosophy as usse_supported()): VMOV, VPCK,
 * VMAD2, VNMAD, VMAD, VDP, VDUAL, VCOMP, SMP, KILL, DEPTHF, LIMM, SMLSI/SMBO, simple (pN / !pN)
 * predication, INDEXED1/2 operands and the INDEX bank (a runtime OpAccessChain index computed from
 * the idx1/idx2 registers, see usse_spirv.c's idx_ptr). NOT yet covered: VTST/VTSTMSK, VBW,
 * SOP2/SOP2M/SOP3, I8/16/32MAD/I32MAD2, LDST, BR -- a program using any of these fails translation
 * (ok=0) and the caller should fall back to the software path for that draw, same as
 * render_interp() already does for usse_supported().
 */
#ifndef RECOMP_USSE_SPIRV_H
#define RECOMP_USSE_SPIRV_H
#include <stdint.h>
#include "usse.h"

typedef struct {
	uint32_t *words; uint32_t nwords;   /* a SPIR-V module (binary, little-endian words) */
	int ok;                                /* 0: `err` says which instruction/feature stopped translation */
	int frag_fetch;                        /* fragment: the program reads the framebuffer colour (FRAGCOLOR_USED), 0 if not. 1: the
	                                        * module samples it from one more 2D texture at set 2, binding nsmp (the caller copies the
	                                        * colour target there before the draw and binds a point sampler); 2: it reads an input
	                                        * attachment (InputAttachmentIndex 0) -- spirv-cross's MSL framebuffer fetch, a [[color(0)]]
	                                        * input straight from tile memory: nothing to copy or bind (usse_spirv_set_fetch_mode) */
	int pa_words;                          /* vertex: words per vertex record in the PA storage buffer (usse_program_pa_words) */
	int ldst_sa[8], nldst_sa;              /* vertex: the SA words whose values are guest pointers the program's LDSTs load through
	                                        * (bone matrices: address = index*stride + SA[k]); the host binds a window of guest
	                                        * memory from each as storage buffer 1 and describes them in UsseDrawUniforms.win */
	char err[160];
} SpirvModule;

/* vl/fl: this pipeline's vertex/fragment layout (usse_vertex_layout/usse_fragment_layout of the
 * OTHER program in the pair) -- needed so the vertex shader's varying outputs and the fragment
 * shader's varying inputs agree on Location numbers (word (o_reg-4)/4 of the vertex's O bank,
 * §4.1's fixed order) without a separate negotiation step; with canonical varyings
 * (usse_spirv_set_canonical_varyings) a fragment module needs no vl. tex_unit_of_smp, if non-null, is
 * filled in with the resolved (compile-time-constant, see usse_spirv.c's block comment) texture
 * unit for each SMP instruction found, in program order, for the caller to bind. */
/* the uniform block both stages read: the 256 SA words, then the draw's viewport z mapping
 * (zOffset, zScale) the vertex epilogue applies to clip z; the host pushes one of these per stage */
#define USSE_LDST_WINDOW (64u << 10)   /* bytes of guest memory bound per LDST pointer */
/* win[w] = { guest base, bytes, word offset in storage buffer 1, 0 }; tex_srgb = { units whose sampled
 * colour the shader decodes sRGB -> linear (a gamma texture bound to plain UNORM data), units whose
 * sampled colour it re-encodes (a non-gamma texture over an sRGB-format render target, which the GPU
 * decodes on sampling), 0, 0 } (gfx.h GFX_TEX_GAMMA) */
/* vfetch = { lo, nstream, 0, 0x3f800000 (an opaque 0 and 1.0f for the prologue) } and stream[s] = { word offset in storage buffer 0 of stream s's copy, stride, 0, 0 }: the
 * GPU-side vertex fetch (emit_vertex_fetch, below) -- the draw's lowest index and where each raw vertex stream's bytes
 * [lo*stride, ...) start in the ring; zeros for a draw whose records the CPU packed. Members 4 and 5 of the SA block
 * (bytes 1184 and 1200); then lod_bias (member 6, byte 1456), 1520 bytes in all */
typedef struct { uint32_t sa[256]; float viewport_z[4]; uint32_t win[8][4]; uint32_t tex_srgb[4];   /* tex_srgb words 2-3: the units bound at the render scale, the render scale (snap_fetch) */
                 uint32_t vfetch[4]; uint32_t stream[16][4];
                 float lod_bias[16]; } UsseDrawUniforms;   /* lod_bias: each unit's texture LOD bias in levels (gfx.h GFX_TEX_LOD_BIAS; usse_spirv.c sample_lod), member 6 at byte 1456 */
/* The GPU-side vertex fetch: the vertex program's prologue reads the draw's raw vertex streams (copied as they are into
 * storage buffer 0) and converts each attribute into its PA words itself, exactly as gfx_soft.c's gfx_vertex_pack does on
 * the CPU -- the plan here IS that packer's (GfxPackAttr per slotted attribute: kind 1 raw bytes, 2 F32 lanes, 3 F16
 * lanes; comps/dcomps/rawlen as it decided them; stride and offset of the attribute's stream) plus its index/instance
 * registers, so the two produce the same records word for word. The module is specialised on it (every field below is a
 * compile-time constant of the shader: the caller keys its shader cache on a hash of this struct) and only the ring
 * offsets of the streams and the draw's lowest index arrive per draw (UsseDrawUniforms.stream/vfetch). A layout the
 * packer calls `slow` (C10/U8 slots, out-of-record writes) never comes here: the caller keeps the CPU packer and the
 * PA-record prologue (vf = null) for it. */
typedef struct { uint16_t stream, offset, stride, reg; uint8_t fmt, comps, kind, dcomps, rawlen; } UsseVfAttr;
typedef struct { UsseVfAttr a[32]; int n, npa, index_reg, instance_reg; } UsseVfLayout;
SpirvModule usse_spirv_translate(const UsseProgram *prog, const UsseVertexLayout *vl, const UsseFragLayout *fl,
                                  const uint32_t sa_tex_unit[256], int max_smp, int *smp_units, int *nsmp, int color_kind, uint32_t cube_mask, uint32_t snap_mask,
                                  const UsseVfLayout *vf);
/* vf: a vertex program's fetch plan (above) -- the prologue fetches the raw streams; null: the prologue copies a PA record
 * per vertex packed by the CPU (gfx_vertex_pack), pa_words wide, from storage buffer 0. Ignored for a fragment program. */
/* snap_mask: bit u set when unit u, at a render scale above 1, reads a render target's bytes at
 * another texel size (point-sampled): its reads go through snap_fetch (the 1x pixel centre's texel,
 * the fragment's own sample of it). Part of the shader cache key; 0 at 1x */
/* cube_mask: bit u set when texture unit u holds a cube map (GfxTexture layout 3) for the draws this
 * shader serves -- its SMPs sample a Cube image with the full (u,v,w) direction; part of the caller's
 * shader cache key, since the image type is baked into the module */
/* color_kind: the render target's texture family (0 RGBA8, 1 RGBA16F, 2 RGBA32F), which decides how a
 * fragment program's raw-word colour output (U32/S32) is laid into the four output channels so the
 * target holds those bits exactly; value colours don't depend on it */
void usse_spirv_free(SpirvModule *m);
/* the sa_tex_unit entries the last usse_spirv_translate read (bit i of out[i / 32]), the low byte of each being all it used:
 * a module depends on these and on nothing else of sa_tex_unit (gfx_gpu.c's shader recipes keep just them) */
void usse_spirv_sa_read(uint32_t out[8]);
/* a fragment module with no inputs, no outputs and no instructions: the fragment stage of a draw that
 * writes no colour (gfx_gpu.c's colour-masked draws), where the device takes SPIR-V rather than MSL */
SpirvModule usse_spirv_null_fragment(void);
/* how later translations fetch the framebuffer colour for FRAGCOLOR_USED programs: 1 (default) the sampled copy,
 * 2 the input attachment (only where the consumer turns it into a framebuffer fetch: Metal via spirv-cross with
 * MSL_FRAMEBUFFER_FETCH_SUBPASS and MSL >= 2.3; SDL_GPU's Vulkan backend has no input attachments). Decided once
 * by gfx_gpu.c before its first shader; part of the module, so a caller keying a shader cache on the SPIR-V
 * words needs nothing more */
void usse_spirv_set_fetch_mode(int mode);
/* F32 -> F16 writes in the lean form (RECOMP_GPU_F16=masked's overflow and NaN cases by the GPU's own conversion, NaNs by one
 * select): only where verified bit-exact -- Metal on Apple silicon. Decided once by gfx_gpu.c before its first shader;
 * part of the module, like the fetch mode */
void usse_spirv_set_f16_lean(int on);
/* canonical varyings: a vertex module writes Location k = semantic k (TEXCOORD0-9, COLOR0, COLOR1, FOG: USSE_CANON_VARY,
 * the components its program lacks as 0, w 1), and a fragment module reads them by semantic, taking the count of components
 * the draw's vertex program writes from UsseDrawUniforms.vfetch[2..3] (usse_spirv_vary_ncomp: 3 bits a semantic, TEXCOORD0-9
 * in word 0, COLOR0/COLOR1/FOG in word 1) -- a fragment module then depends on its own program alone (vl is ignored for
 * it, and may be null). Off: Location i = the vertex program's O words 4+4i.., the pairing baked into the fragment module.
 * Decided once by gfx_gpu.c before its first shader; part of the module, like the fetch mode */
#define USSE_CANON_VARY 13
void usse_spirv_set_canonical_varyings(int on);
uint32_t usse_spirv_vary_ncomp(const UsseVertexLayout *vl, int word);
/* the variant fields of a shader key that make a difference to the module, normalised: the colour kind only for a raw-word
 * colour output (0, 1 RGBA16F or 2 RGBA32F; 0 for value colours), the cube and snap units only where the program samples
 * (its SMPs, resolved through sa_tex_unit, and its non-dependent texture reads). fl null for a vertex program */
void usse_spirv_key(const UsseProgram *prog, const UsseFragLayout *fl, const uint32_t sa_tex_unit[256], int *color_kind, uint32_t *cube_mask, uint32_t *snap_mask);
/* the units a program reads as cube maps, from its own code: SMPs with a 3-component coordinate (GXM has no volume
 * textures) and non-dependent reads in projection mode 3 -- every cube unit of Killzone's and Uncharted's recorded shaders
 * and nothing else (gfx_gpu.c's predicted shaders) */
uint32_t usse_spirv_predict_cube(const UsseProgram *prog, const UsseFragLayout *fl, const uint32_t sa_tex_unit[256]);
/* the NEXT translation of a FRAGCOLOR_USED program reads no framebuffer (no resource) and leaves the blend to the
 * blend unit (usse_fragment_fb_blend's classes, gfx_gpu.c): 1 -- zeros preloaded as the colour, the module
 * outputs what the program adds, X (SpirvModule.frag_fetch 3); 2 -- the program runs twice, colour 0 then 1,
 * and the module outputs (X.rgb, 1 - K) for colour out = K * colour in + X (frag_fetch 4). 0 (and after every
 * translation the caller resets it): the mode above. */
void usse_spirv_set_fetch_zero(int mode);
/* filtered shadows (gfx_gpu.c's shadow_filter): the NEXT translation runs the fragment program `taps` times (a 4x4
 * grid for 16), each with its texture-unit `unit` SMPs moved by one grid step, and writes the mean colour -- a
 * shadow-map compare averaged over its neighbourhood, percentage-closer filtering. The step in UV arrives per draw
 * in UsseDrawUniforms.vfetch[0..1] (floats: the fragment stage has no vertex fetch). Branch-free programs without
 * KILL, DEPTHF or a framebuffer fetch only; others translate as usual. unit -1 / taps < 2: off. */
void usse_spirv_set_pcf(int unit, int taps);
/* point sprites (SCE_GXM_PRIMITIVE_POINTS, gfx_gpu.c): the NEXT translation is the pair's point-sprite variant. Vertex:
 * every point is six vertices of two triangles -- the caller draws 6 x points records (each point's record repeated,
 * starting at a multiple of 6), and vertex i is corner (0,1,2,1,3,2)[i % 6] (0 top-left .. 3 bottom-right) of a square
 * of the program's PSIZE output (or UsseDrawUniforms.vfetch[0], a float, when it writes none) in target pixels: clip x
 * moves by +-size * w * viewport_z[2] and y by +-size * w * viewport_z[3] (half a pixel in NDC, signed for the screen's
 * direction), and the 0..1 sprite coordinate (v flipped when vfetch[1] is nonzero: POINT_10UV) is an extra varying.
 * Fragment: iterators flagged point_coord read that varying (gfx_soft.c's render_points_interp, on the GPU). */
void usse_spirv_set_points(int on);
void usse_spirv_set_probe_off(int off);   /* the next translation without RECOMP_PROBE (gfx_gpu.c: RECOMP_PROBE_FPROG=<addr> probes that fragment program only) */
#endif
