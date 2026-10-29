/*
 * Present-time post-processing on the finished frame (window.c's present loop): FXAA 3.11 (the quality
 * variant) at the frame's own resolution -- before any downsampling, so its edge search sees the real
 * pixels -- then AMD's Contrast Adaptive Sharpening at the window's output resolution, after the
 * downscale, to give back the crispness a supersampled image loses on the way down.
 *
 *   settings "post_aa"  0 | 1 (default 1)    FXAA          (RECOMP_POST_AA wins)
 *   settings "sharpen"  0..1  (default 0.35) CAS strength  (RECOMP_SHARPEN wins; 0 = off)
 *
 * The shaders are Metal Shading Language here, with SPIR-V twins for Vulkan (runtime/gfx/shaders/post.vert,
 * fxaa.frag, cas.frag, compiled into vk_shaders.h by vk/build.sh; a change to one side is made to the
 * other). Both passes are written from the published algorithms (Lottes'
 * FXAA 3.11, AMD FidelityFX CAS), not copied code.
 */
#include <SDL3/SDL.h>
#include "vk_shaders.h"   /* the post shaders' SPIR-V twins, for Vulkan */
#include <stdlib.h>
#include <string.h>

int settings_int(const char *key, int def);
double settings_float(const char *key, double def);

static const char post_msl[] =
	"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct VOut { float4 pos [[position]]; };\n"
	"vertex VOut post_vs(uint vid [[vertex_id]]) { float2 p = float2((vid << 1) & 2, vid & 2); VOut o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o; }\n"
	"struct PostU { float2 rcp; float2 origin; float4 k; };\n"   /* k.x: sharpness */
	"static float lum(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
	"static float3 tap(texture2d<float> t, sampler s, float2 uv) { return t.sample(s, uv, level(0.0)).rgb; }\n"
	/* FXAA 3.11 quality: local contrast test, edge direction, a walk along the edge in both directions to
	 * its ends, then a blend across the edge by how far this pixel is from the nearer end (and a subpixel
	 * term for single-pixel features) */
	"fragment float4 fxaa_fs(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant PostU &u [[buffer(0)]]) {\n"
	"  float2 r = u.rcp, uv = (in.pos.xy - u.origin) * r;\n"
	"  float3 cM = tap(t, s, uv); float lM = lum(cM);\n"
	"  float lN = lum(tap(t, s, uv + float2(0.0, -r.y))), lS = lum(tap(t, s, uv + float2(0.0, r.y)));\n"
	"  float lE = lum(tap(t, s, uv + float2(r.x, 0.0))), lW = lum(tap(t, s, uv + float2(-r.x, 0.0)));\n"
	"  float lMin = min(lM, min(min(lN, lS), min(lE, lW))), lMax = max(lM, max(max(lN, lS), max(lE, lW))), range = lMax - lMin;\n"
	"  if (range < max(0.0312, lMax * 0.125)) return float4(cM, 1.0);\n"
	"  float lNW = lum(tap(t, s, uv + float2(-r.x, -r.y))), lNE = lum(tap(t, s, uv + float2(r.x, -r.y)));\n"
	"  float lSW = lum(tap(t, s, uv + float2(-r.x, r.y))), lSE = lum(tap(t, s, uv + float2(r.x, r.y)));\n"
	"  float lNS = lN + lS, lWE = lW + lE;\n"
	"  float eH = abs(-2.0 * lW + lNW + lSW) + abs(-2.0 * lM + lNS) * 2.0 + abs(-2.0 * lE + lNE + lSE);\n"
	"  float eV = abs(-2.0 * lN + lNW + lNE) + abs(-2.0 * lM + lWE) * 2.0 + abs(-2.0 * lS + lSW + lSE);\n"
	"  bool horz = eH >= eV;\n"
	"  float l1 = horz ? lN : lW, l2 = horz ? lS : lE, g1 = l1 - lM, g2 = l2 - lM;\n"
	"  bool steep1 = abs(g1) >= abs(g2); float gS = 0.25 * max(abs(g1), abs(g2));\n"
	"  float stepL = horz ? r.y : r.x, lAvg;\n"
	"  if (steep1) { stepL = -stepL; lAvg = 0.5 * (l1 + lM); } else lAvg = 0.5 * (l2 + lM);\n"
	"  float2 cuv = uv; if (horz) cuv.y += stepL * 0.5; else cuv.x += stepL * 0.5;\n"
	"  float2 off = horz ? float2(r.x, 0.0) : float2(0.0, r.y), uv1 = cuv - off, uv2 = cuv + off;\n"
	"  float e1 = lum(tap(t, s, uv1)) - lAvg, e2 = lum(tap(t, s, uv2)) - lAvg;\n"
	"  bool r1 = abs(e1) >= gS, r2 = abs(e2) >= gS;\n"
	"  const float Q[12] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };\n"
	"  if (!r1) uv1 -= off; if (!r2) uv2 += off;\n"
	"  for (int i = 2; i < 12 && !(r1 && r2); i++) {\n"
	"    if (!r1) { e1 = lum(tap(t, s, uv1)) - lAvg; r1 = abs(e1) >= gS; if (!r1) uv1 -= off * Q[i]; }\n"
	"    if (!r2) { e2 = lum(tap(t, s, uv2)) - lAvg; r2 = abs(e2) >= gS; if (!r2) uv2 += off * Q[i]; }\n"
	"  }\n"
	"  float d1 = horz ? uv.x - uv1.x : uv.y - uv1.y, d2 = horz ? uv2.x - uv.x : uv2.y - uv.y;\n"
	"  bool dir1 = d1 < d2; float dF = min(d1, d2), thick = max(d1 + d2, 1e-6);\n"
	"  float pOff = -dF / thick + 0.5;\n"
	"  bool smaller = lM < lAvg, right = ((dir1 ? e1 : e2) < 0.0) != smaller;\n"
	"  float fOff = right ? pOff : 0.0;\n"
	"  float lAll = (1.0 / 12.0) * (2.0 * (lNS + lWE) + lNW + lNE + lSW + lSE);\n"
	"  float sp = clamp(abs(lAll - lM) / range, 0.0, 1.0); sp = (-2.0 * sp + 3.0) * sp * sp;\n"
	"  fOff = max(fOff, sp * sp * 0.75);\n"
	"  float2 fuv = uv; if (horz) fuv.y += fOff * stepL; else fuv.x += fOff * stepL;\n"
	"  return float4(tap(t, s, fuv), 1.0);\n"
	"}\n"
	/* CAS: each pixel pulled away from its 4 neighbours by a weight that shrinks where the local 3x3 range
	 * is already high (no ringing on hard edges) */
	"fragment float4 cas_fs(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant PostU &u [[buffer(0)]]) {\n"
	"  float2 r = u.rcp, uv = (in.pos.xy - u.origin) * r;\n"
	"  float3 a = tap(t, s, uv + float2(-r.x, -r.y)), b = tap(t, s, uv + float2(0.0, -r.y)), c = tap(t, s, uv + float2(r.x, -r.y));\n"
	"  float3 d = tap(t, s, uv + float2(-r.x, 0.0)), e = tap(t, s, uv), f = tap(t, s, uv + float2(r.x, 0.0));\n"
	"  float3 g = tap(t, s, uv + float2(-r.x, r.y)), h = tap(t, s, uv + float2(0.0, r.y)), i = tap(t, s, uv + float2(r.x, r.y));\n"
	"  float3 mn = min(min(min(d, e), min(f, b)), h), mx = max(max(max(d, e), max(f, b)), h);\n"
	"  mn += min(mn, min(min(a, c), min(g, i))); mx += max(mx, max(max(a, c), max(g, i)));\n"
	"  float3 amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, float3(1e-5)), 0.0, 1.0));\n"
	"  float3 w = amp * (-1.0 / mix(8.0, 5.0, clamp(u.k.x, 0.0, 1.0)));\n"
	"  return float4(clamp((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w), 0.0, 1.0), 1.0);\n"
	"}\n";

typedef struct { float rcp[2], origin[2], k[4]; } PostU;
static SDL_GPUShader *vs, *fs_fxaa, *fs_cas;
static SDL_GPUGraphicsPipeline *p_fxaa, *p_cas; static SDL_GPUTextureFormat cas_fmt = SDL_GPU_TEXTUREFORMAT_INVALID;
static SDL_GPUSampler *smp;
static SDL_GPUTexture *aa_tex, *out_tex; static uint32_t aa_w, aa_h, aa_l, out_w, out_h;
static int broken;   /* a creation failed: say so once and fall back to the plain blit for the rest of the run */

static SDL_GPUShader *shader(SDL_GPUDevice *gpu, const char *entry, SDL_GPUShaderStage stage) {
	SDL_GPUShaderCreateInfo ci = {0};
	if (SDL_GetGPUShaderFormats(gpu) & SDL_GPU_SHADERFORMAT_MSL) { ci.code = (const Uint8 *)post_msl; ci.code_size = sizeof post_msl; ci.entrypoint = entry; ci.format = SDL_GPU_SHADERFORMAT_MSL; }
	else {   /* Vulkan: the SPIR-V twins (runtime/gfx/shaders/post.vert, fxaa.frag, cas.frag; vk_shaders.h) */
		int v = !strcmp(entry, "post_vs"), f = !strcmp(entry, "fxaa_fs");
		ci.code = (const Uint8 *)(v ? vk_post_vert : f ? vk_fxaa_frag : vk_cas_frag); ci.code_size = v ? sizeof vk_post_vert : f ? sizeof vk_fxaa_frag : sizeof vk_cas_frag;
		ci.entrypoint = "main"; ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
	}
	ci.stage = stage;
	if (stage == SDL_GPU_SHADERSTAGE_FRAGMENT) { ci.num_samplers = 1; ci.num_uniform_buffers = 1; }
	return SDL_CreateGPUShader(gpu, &ci);
}
static SDL_GPUGraphicsPipeline *pipeline(SDL_GPUDevice *gpu, SDL_GPUShader *fs, SDL_GPUTextureFormat fmt) {
	SDL_GPUColorTargetDescription ct = { .format = fmt };
	SDL_GPUGraphicsPipelineCreateInfo pi = {0};
	pi.vertex_shader = vs; pi.fragment_shader = fs; pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE; pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
	pi.target_info.color_target_descriptions = &ct; pi.target_info.num_color_targets = 1;
	return SDL_CreateGPUGraphicsPipeline(gpu, &pi);
}
static int setup(SDL_GPUDevice *gpu, SDL_GPUTextureFormat swap_fmt) {
	if (broken) return 0;
	if (!vs) {
		vs = shader(gpu, "post_vs", SDL_GPU_SHADERSTAGE_VERTEX); fs_fxaa = shader(gpu, "fxaa_fs", SDL_GPU_SHADERSTAGE_FRAGMENT); fs_cas = shader(gpu, "cas_fs", SDL_GPU_SHADERSTAGE_FRAGMENT);
		SDL_GPUSamplerCreateInfo sci = {0};
		sci.min_filter = sci.mag_filter = SDL_GPU_FILTER_LINEAR; sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
		sci.address_mode_u = sci.address_mode_v = sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		smp = SDL_CreateGPUSampler(gpu, &sci);
		if (vs && fs_fxaa) p_fxaa = pipeline(gpu, fs_fxaa, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
		if (!vs || !fs_fxaa || !fs_cas || !smp || !p_fxaa) { SDL_Log("post: FXAA/CAS unavailable (%s): plain blit", SDL_GetError()); broken = 1; return 0; }
	}
	if (cas_fmt != swap_fmt) {
		if (p_cas) SDL_ReleaseGPUGraphicsPipeline(gpu, p_cas);
		p_cas = pipeline(gpu, fs_cas, swap_fmt); cas_fmt = swap_fmt;
		if (!p_cas) { SDL_Log("post: CAS pipeline unavailable (%s): plain blit", SDL_GetError()); broken = 1; return 0; }
	}
	return 1;
}
static SDL_GPUTexture *ensure(SDL_GPUDevice *gpu, SDL_GPUTexture **t, uint32_t *tw, uint32_t *th, uint32_t *tl, uint32_t w, uint32_t h, uint32_t levels) {
	if (*t && *tw == w && *th == h && (!tl || *tl == levels)) return *t;
	if (*t) SDL_ReleaseGPUTexture(gpu, *t);
	SDL_GPUTextureCreateInfo ci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
		.width = w, .height = h, .layer_count_or_depth = 1, .num_levels = levels, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
	*t = SDL_CreateGPUTexture(gpu, &ci); *tw = w; *th = h; if (tl) *tl = levels;
	return *t;
}
static void pass(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *dst, SDL_GPULoadOp load, SDL_GPUGraphicsPipeline *p, SDL_GPUTexture *src,
                 float vx, float vy, float vw, float vh, float sharp) {
	SDL_GPUColorTargetInfo ct = { .texture = dst, .load_op = load, .store_op = SDL_GPU_STOREOP_STORE, .clear_color = { 0, 0, 0, 1 } };
	SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
	SDL_GPUViewport v = { vx, vy, vw, vh, 0.0f, 1.0f }; SDL_SetGPUViewport(rp, &v);
	SDL_BindGPUGraphicsPipeline(rp, p);
	SDL_GPUTextureSamplerBinding b = { .texture = src, .sampler = smp }; SDL_BindGPUFragmentSamplers(rp, 0, &b, 1);
	PostU u = { { 1.0f / vw, 1.0f / vh }, { vx, vy }, { sharp, 0, 0, 0 } };
	SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof u);
	SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
	SDL_EndGPURenderPass(rp);
}

/* the frame src (sw x sh, `levels` mip levels: window.c gives a frame over twice the window's size a mip
 * chain to downsample through) presented into the swapchain's rect (dx, dy, dw, dh) from its source
 * rect (px, py, pw, ph). 1: done (cleared outside the rect, as the plain blit does); 0: nothing done --
 * post-processing off or unavailable, the caller blits. */
int post_present(SDL_GPUDevice *gpu, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, uint32_t sw, uint32_t sh, uint32_t levels,
                 float px, float py, float pw, float ph, SDL_GPUTexture *swap, SDL_GPUTextureFormat swap_fmt, float dx, float dy, float dw, float dh) {
	const char *e = getenv("RECOMP_POST_AA"), *k = getenv("RECOMP_SHARPEN");
	extern int gfx_gpu_fsr_active(void);
	int aa = e ? atoi(e) != 0 : settings_int("post_aa", 1) != 0 && !gfx_gpu_fsr_active();   /* (FSR anti-aliases the 3D frame itself: FXAA over it would only blur) */
	float sharp = k ? (float)atof(k) : (float)settings_float("sharpen", 0.35);
	if (sharp < 0) sharp = 0; if (sharp > 1) sharp = 1;
	if ((!aa && sharp <= 0) || !src || dw < 1 || dh < 1 || !setup(gpu, swap_fmt)) return 0;
	SDL_GPUTexture *cur = src;
	if (aa) {   /* FXAA at the frame's own resolution, into a copy with the same mip levels (rebuilt from it) */
		if (!ensure(gpu, &aa_tex, &aa_w, &aa_h, &aa_l, sw, sh, levels)) return 0;
		pass(cmd, aa_tex, SDL_GPU_LOADOP_DONT_CARE, p_fxaa, src, 0, 0, (float)sw, (float)sh, 0);
		if (levels > 1) SDL_GenerateMipmapsForGPUTexture(cmd, aa_tex);
		cur = aa_tex;
	}
	uint32_t lv = 0;   /* the smallest level still at least the size it is drawn at (as window.c's own blit) */
	while (lv + 1 < levels && ph / (float)(2u << lv) >= dh && pw / (float)(2u << lv) >= dw) lv++;
	if (sharp <= 0) {   /* AA only: the scaled blit straight into the swapchain */
		SDL_GPUBlitInfo bl = { .source = { .texture = cur, .mip_level = lv, .x = (Uint32)px >> lv, .y = (Uint32)py >> lv, .w = (Uint32)pw >> lv, .h = (Uint32)ph >> lv },
			.destination = { .texture = swap, .x = (Uint32)dx, .y = (Uint32)dy, .w = (Uint32)dw, .h = (Uint32)dh },
			.load_op = SDL_GPU_LOADOP_CLEAR, .clear_color = { 0, 0, 0, 1 }, .filter = SDL_GPU_FILTER_LINEAR };
		SDL_BlitGPUTexture(cmd, &bl);
		return 1;
	}
	/* sharpening at output resolution: scale into a window-sized copy, then CAS into the swapchain's rect */
	uint32_t ow = (uint32_t)(dw + 0.5f), oh = (uint32_t)(dh + 0.5f);
	if (!ensure(gpu, &out_tex, &out_w, &out_h, NULL, ow, oh, 1)) return 0;
	SDL_GPUBlitInfo bl = { .source = { .texture = cur, .mip_level = lv, .x = (Uint32)px >> lv, .y = (Uint32)py >> lv, .w = (Uint32)pw >> lv, .h = (Uint32)ph >> lv },
		.destination = { .texture = out_tex, .w = ow, .h = oh }, .load_op = SDL_GPU_LOADOP_DONT_CARE, .filter = SDL_GPU_FILTER_LINEAR };
	SDL_BlitGPUTexture(cmd, &bl);
	pass(cmd, swap, SDL_GPU_LOADOP_CLEAR, p_cas, out_tex, dx, dy, (float)ow, (float)oh, sharp);
	return 1;
}
