/*
 * Screen-space ambient occlusion for a game's main 3D scene (gfx_gpu.c decides which scene, reads the camera's projection
 * and calls ssao_apply at that scene's end, before anything samples it). Three full-screen passes on the GPU:
 *   1. ao     -- half resolution: each pixel's view-space position from the scene's depth (the projection's own depth
 *               mapping, so the distances are the game's units), a normal from its neighbours (the smaller depth step on
 *               each axis: no seams at silhouettes), and 12 taps on a spiral within `radius` of it (McGuire et al.'s
 *               scalable ambient obscurance estimator: occluders count by how far they rise above the surface and fade out
 *               at the radius) -- less a quarter of each tap's distance: a fold shallower than ~14 degrees occludes
 *               nothing (the Vita's low-poly walls are big slightly-bent triangles; counted, every fold drew a dark
 *               wedge across the wall). Out: AO and the pixel's distance (RG16F).
 *   2. blur   -- twice, across then down: a 9-tap Gaussian that skips taps at another depth (relative), so occlusion
 *               doesn't bleed across edges.
 *   3. apply  -- full resolution into the scene's colour target, multiplied in by the blend unit (the colour target is
 *               never read): the AO of the nearest-depth half-resolution sample where the bilinear one is at another
 *               depth (silhouettes), else the bilinear one, eased toward 1 by the strength setting.
 * The sky (beyond 1000 units) and anything nearer than 0.3 (a first-person weapon) are left alone.
 *
 *   settings "ssao"           0 | 1 (default 1)      on, for a game that names its main scene (RECOMP_SSAO; RECOMP_SSAO_ON wins)
 *   settings "ssao_strength"  0..2 (default 1.0)     how dark
 *
 * Metal Shading Language here, SPIR-V twins for Vulkan (runtime/gfx/shaders/ssao_ao.frag, ssao_blur.frag, ssao_apply.frag with
 * ssao_common.glsl, and post.vert's full-screen triangle; vk_shaders.h by vk/build.sh): a change to one side is made to
 * the other. Written from the published estimator, not copied code.
 */
#include <SDL3/SDL.h>
#include "vk_shaders.h"
#include "ssao.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

int settings_int(const char *key, int def);
double settings_float(const char *key, double def);

static const char ssao_msl[] =
	"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct VOut { float4 pos [[position]]; };\n"
	"vertex VOut ao_vs(uint vid [[vertex_id]]) { float2 p = float2((vid << 1) & 2, vid & 2); VOut o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o; }\n"
	"struct AoU { float p00, p11, p22, p32, zoff, zscale, radius, intensity, fw, fh, hw, hh, bias, maxpx, dirx, diry, strength, pad0, pad1, pad2; };\n"
	/* the distance in front of the eye at a depth-buffer value: the projection's z row through the viewport's z mapping, inverted */
	"static float lin(constant AoU &u, float z) { return u.p32 / ((z - u.zoff) / u.zscale + u.p22); }\n"
	"static float3 vpos(constant AoU &u, float2 px, float d) { float2 ndc = float2(px.x / u.fw * 2.0 - 1.0, 1.0 - px.y / u.fh * 2.0); return float3(ndc.x * d / u.p00, ndc.y * d / u.p11, -d); }\n"
	"static float dep(depth2d<float> t, constant AoU &u, float2 px) { return lin(u, t.read(uint2(clamp(px, float2(0.0), float2(u.fw - 1.0, u.fh - 1.0))))); }\n"
	"fragment float4 ao_fs(VOut in [[stage_in]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant AoU &u [[buffer(0)]]) {\n"
	"  float2 fp = floor(in.pos.xy) * 2.0 + 0.5;\n"
	"  float d = dep(t, u, fp);\n"
	"  if (!(d > 0.3) || d > 1000.0) return float4(1.0, d, 0.0, 1.0);\n"
	"  float3 P = vpos(u, fp, d);\n"
	"  float3 pr = vpos(u, fp + float2(2.0, 0.0), dep(t, u, fp + float2(2.0, 0.0))), pl = vpos(u, fp - float2(2.0, 0.0), dep(t, u, fp - float2(2.0, 0.0)));\n"
	"  float3 pd = vpos(u, fp + float2(0.0, 2.0), dep(t, u, fp + float2(0.0, 2.0))), pu = vpos(u, fp - float2(0.0, 2.0), dep(t, u, fp - float2(0.0, 2.0)));\n"
	"  float3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl, dy = abs(pd.z - P.z) < abs(P.z - pu.z) ? pd - P : P - pu;\n"
	"  float3 n = normalize(cross(dx, dy)); if (dot(n, P) > 0.0) n = -n;\n"
	"  if (u.pad0 > 2.5) return float4(n.x * 0.5 + 0.5, n.y * 0.5 + 0.5, 0.0, 1.0);\n"
	"  float rpx = min(u.radius * u.p11 * 0.5 * u.fh / d, u.maxpx);\n"
	"  if (rpx < 2.0) return float4(1.0, d, 0.0, 1.0);\n"
	"  float spin = fract(52.9829189 * fract(dot(floor(in.pos.xy), float2(0.06711056, 0.00583715)))) * 6.2831853;\n"
	"  float r2 = u.radius * u.radius, sum = 0.0;\n"
	"  for (int i = 0; i < 12; i++) {\n"
	"    float a = (float(i) + 0.5) / 12.0, ang = a * 31.4159265 + spin;\n"
	"    float2 q = fp + float2(cos(ang), sin(ang)) * (a * rpx);\n"
	"    if (q.x < 0.0 || q.y < 0.0 || q.x >= u.fw || q.y >= u.fh) continue;\n"   /* off screen: nothing known, nothing counted (clamped to the edge, its column's depth drew streaks) */
	"    float3 v = vpos(u, q, dep(t, u, q)) - P; float vv = dot(v, v), vn = dot(v, n);\n"
	"    float f = max(r2 - vv, 0.0); sum += f * f * f * max((vn - 0.25 * sqrt(vv) - u.bias * max(1.0, 0.1 * d)) / (vv + 0.01 * r2), 0.0);\n"
	"  }\n"
	"  return float4(max(0.0, 1.0 - sum * u.intensity / (r2 * r2 * r2) * (5.0 / 12.0)), d, 0.0, 1.0);\n"
	"}\n"
	"fragment float4 blur_fs(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant AoU &u [[buffer(0)]]) {\n"
	"  int2 p = int2(in.pos.xy); float2 c = t.read(uint2(p)).rg; float sum = c.r, ws = 1.0;\n"
	"  for (int k = -4; k <= 4; k++) { if (k == 0) continue;\n"
	"    int2 q = clamp(p + int2(int(float(k) * u.dirx), int(float(k) * u.diry)), int2(0), int2(int(u.hw) - 1, int(u.hh) - 1));\n"
	"    float2 sv = t.read(uint2(q)).rg; float w = exp(-0.18 * float(k * k)) * max(0.0, 1.0 - abs(sv.g - c.g) / (0.04 * c.g + 0.001));\n"
	"    sum += sv.r * w; ws += w; }\n"
	"  return float4(sum / ws, c.g, 0.0, 1.0);\n"
	"}\n"
	"fragment float4 apply_fs(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], depth2d<float> dt [[texture(1)]], sampler s2 [[sampler(1)]], constant AoU &u [[buffer(0)]]) {\n"
	"  float d0 = dep(dt, u, in.pos.xy);\n"
	"  if (u.pad0 > 3.5) return float4(float3(fract(d0) < 0.08 ? 0.0 : 1.0), 1.0);\n"
	"  if (u.pad0 > 2.5) { float2 nn = t.sample(s, in.pos.xy / float2(u.fw, u.fh)).rg; return float4(nn, 0.0, 1.0); }\n"
	"  if (u.pad0 > 1.5) return float4(fract(d0 / 10.0), dt.read(uint2(in.pos.xy)), d0 > 0.3 && d0 < 1000.0 ? 1.0 : 0.0, 1.0);\n"
	"  float2 b = t.sample(s, in.pos.xy / float2(u.fw, u.fh)).rg; float ao = b.r;\n"
	"  if (abs(b.g - d0) > 0.05 * d0) {\n"
	"    int2 h = int2(floor(in.pos.xy * 0.5 - 0.5)); float best = 1e30;\n"
	"    for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) { float2 sv = t.read(uint2(clamp(h + int2(i, j), int2(0), int2(int(u.hw) - 1, int(u.hh) - 1)))).rg;\n"
	"      float e = abs(sv.g - d0); if (e < best) { best = e; ao = sv.r; } }\n"
	"  }\n"
	"  return float4(float3(mix(1.0, ao, clamp(u.strength, 0.0, 1.0)) * (u.strength > 1.0 ? mix(1.0, ao, u.strength - 1.0) : 1.0)), 1.0);\n"
	"}\n";

typedef struct { float p00, p11, p22, p32, zoff, zscale, radius, intensity, fw, fh, hw, hh, bias, maxpx, dirx, diry, strength, pad0, pad1, pad2; } AoU;

static SDL_GPUShader *vs, *fs_ao, *fs_blur, *fs_apply;
static SDL_GPUGraphicsPipeline *p_ao, *p_blur, *p_apply; static SDL_GPUTextureFormat apply_fmt = SDL_GPU_TEXTUREFORMAT_INVALID;
static SDL_GPUSampler *smp_near, *smp_lin;
static SDL_GPUTexture *tex[2]; static uint32_t tw, th;
static int broken;

static int show_mode(void) { const char *e = getenv("RECOMP_SSAO_SHOW"); return e ? atoi(e) : 0; }   /* RECOMP_SSAO_SHOW (debugging): 1 the occlusion as grey, 2 distance bands / raw depth / in range, 3 the normals, 4 one-unit distance contours */
static SDL_GPUShader *shader(SDL_GPUDevice *gpu, const char *entry, SDL_GPUShaderStage stage, int samplers) {
	SDL_GPUShaderCreateInfo ci = {0};
	if (SDL_GetGPUShaderFormats(gpu) & SDL_GPU_SHADERFORMAT_MSL) { ci.code = (const Uint8 *)ssao_msl; ci.code_size = sizeof ssao_msl; ci.entrypoint = entry; ci.format = SDL_GPU_SHADERFORMAT_MSL; }
	else {   /* Vulkan: the SPIR-V twins (runtime/gfx/shaders/ssao_*.frag, post.vert; vk_shaders.h) */
		const uint32_t *code = 0; size_t n = 0;
		if (!strcmp(entry, "ao_vs")) { code = vk_post_vert; n = sizeof vk_post_vert; }
		else if (!strcmp(entry, "ao_fs")) { code = vk_ssao_ao_frag; n = sizeof vk_ssao_ao_frag; }
		else if (!strcmp(entry, "blur_fs")) { code = vk_ssao_blur_frag; n = sizeof vk_ssao_blur_frag; }
		else { code = vk_ssao_apply_frag; n = sizeof vk_ssao_apply_frag; }
		ci.code = (const Uint8 *)code; ci.code_size = n; ci.entrypoint = "main"; ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
	}
	ci.stage = stage;
	if (stage == SDL_GPU_SHADERSTAGE_FRAGMENT) { ci.num_samplers = (Uint32)samplers; ci.num_uniform_buffers = 1; }
	return SDL_CreateGPUShader(gpu, &ci);
}
static SDL_GPUGraphicsPipeline *pipeline(SDL_GPUDevice *gpu, SDL_GPUShader *fs, SDL_GPUTextureFormat fmt, int multiply) {
	SDL_GPUColorTargetDescription ct = { .format = fmt };
	if (multiply) {   /* dst.rgb *= src.rgb; alpha kept */
		ct.blend_state.enable_blend = true;
		ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO; ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_COLOR; ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
		ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO; ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE; ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
	}
	SDL_GPUGraphicsPipelineCreateInfo pi = {0};
	pi.vertex_shader = vs; pi.fragment_shader = fs; pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE; pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
	pi.target_info.color_target_descriptions = &ct; pi.target_info.num_color_targets = 1;
	return SDL_CreateGPUGraphicsPipeline(gpu, &pi);
}
static int setup(SDL_GPUDevice *gpu, SDL_GPUTextureFormat color_fmt) {
	if (broken) return 0;
	if (!vs) {
		vs = shader(gpu, "ao_vs", SDL_GPU_SHADERSTAGE_VERTEX, 0); fs_ao = shader(gpu, "ao_fs", SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
		fs_blur = shader(gpu, "blur_fs", SDL_GPU_SHADERSTAGE_FRAGMENT, 1); fs_apply = shader(gpu, "apply_fs", SDL_GPU_SHADERSTAGE_FRAGMENT, 2);
		SDL_GPUSamplerCreateInfo sci = {0};
		sci.address_mode_u = sci.address_mode_v = sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sci.min_filter = sci.mag_filter = SDL_GPU_FILTER_NEAREST; smp_near = SDL_CreateGPUSampler(gpu, &sci);
		sci.min_filter = sci.mag_filter = SDL_GPU_FILTER_LINEAR; smp_lin = SDL_CreateGPUSampler(gpu, &sci);
		if (vs && fs_ao && fs_blur) { p_ao = pipeline(gpu, fs_ao, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, 0); p_blur = pipeline(gpu, fs_blur, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, 0); }
		if (!vs || !fs_ao || !fs_blur || !fs_apply || !smp_near || !smp_lin || !p_ao || !p_blur) { SDL_Log("ssao: unavailable (%s)", SDL_GetError()); broken = 1; return 0; }
	}
	if (apply_fmt != color_fmt) {
		if (p_apply) SDL_ReleaseGPUGraphicsPipeline(gpu, p_apply);
		p_apply = pipeline(gpu, fs_apply, color_fmt, !show_mode()); apply_fmt = color_fmt;   /* RECOMP_SSAO_SHOW=1: the occlusion itself, as grey, in place of the scene */
		if (!p_apply) { SDL_Log("ssao: apply pipeline unavailable (%s)", SDL_GetError()); broken = 1; return 0; }
	}
	return 1;
}
static int ensure(SDL_GPUDevice *gpu, uint32_t w, uint32_t h) {
	if (tex[0] && tw == w && th == h) return 1;
	for (int i = 0; i < 2; i++) { if (tex[i]) SDL_ReleaseGPUTexture(gpu, tex[i]); tex[i] = 0; }
	SDL_GPUTextureCreateInfo ci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
		.width = w, .height = h, .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
	for (int i = 0; i < 2; i++) tex[i] = SDL_CreateGPUTexture(gpu, &ci);
	tw = w; th = h;
	return tex[0] && tex[1];
}
static void pass(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *dst, SDL_GPULoadOp load, SDL_GPUGraphicsPipeline *p, const SDL_GPUTextureSamplerBinding *b, int nb, const AoU *u, float vw, float vh) {
	SDL_GPUColorTargetInfo ct = { .texture = dst, .load_op = load, .store_op = SDL_GPU_STOREOP_STORE };
	SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
	SDL_GPUViewport v = { 0, 0, vw, vh, 0.0f, 1.0f }; SDL_SetGPUViewport(rp, &v);
	SDL_BindGPUGraphicsPipeline(rp, p);
	SDL_BindGPUFragmentSamplers(rp, 0, b, (Uint32)nb);
	SDL_PushGPUFragmentUniformData(cmd, 0, u, sizeof *u);
	SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
	SDL_EndGPURenderPass(rp);
}

int ssao_enabled(void) {
	const char *e = getenv("RECOMP_SSAO_ON");
	return e ? atoi(e) != 0 : settings_int("ssao", 1) != 0;
}
int ssao_apply(SDL_GPUDevice *gpu, SDL_GPUTexture *color, SDL_GPUTextureFormat color_fmt, SDL_GPUTexture *depth, uint32_t w, uint32_t h, const SsaoCamera *cam) {
	float strength = (float)settings_float("ssao_strength", 1.0);
	if (!(strength > 0.0f) || !color || !depth || w < 4 || h < 4 || !setup(gpu, color_fmt)) return 0;
	const uint32_t hw = (w + 1) / 2, hh = (h + 1) / 2;
	if (!ensure(gpu, hw, hh)) return 0;
	AoU u = { cam->p00, cam->p11, cam->p22, cam->p32, cam->zoff, cam->zscale, 0.5f, 1.5f, (float)w, (float)h, (float)hw, (float)hh,
	          0.02f, 0.08f * (float)h, 0, 0, strength > 2.0f ? 2.0f : strength, (float)show_mode(), 0, 0 };   /* pad0: the debugging view (show_mode) */   /* radius and bias in the game's units (Killzone: metres) */
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
	if (!cmd) return 0;
	SDL_GPUTextureSamplerBinding bd = { depth, smp_near }, b0 = { tex[0], smp_near }, b1 = { tex[1], smp_near };
	pass(cmd, tex[0], SDL_GPU_LOADOP_DONT_CARE, p_ao, &bd, 1, &u, (float)hw, (float)hh);
	if (!getenv("RECOMP_SSAO_NOBLUR") && u.pad0 < 2.5f) {   /* (RECOMP_SSAO_NOBLUR=1: the raw occlusion, a debugging view) */
		u.dirx = 1; u.diry = 0; pass(cmd, tex[1], SDL_GPU_LOADOP_DONT_CARE, p_blur, &b0, 1, &u, (float)hw, (float)hh);
		u.dirx = 0; u.diry = 1; pass(cmd, tex[0], SDL_GPU_LOADOP_DONT_CARE, p_blur, &b1, 1, &u, (float)hw, (float)hh);
	}
	SDL_GPUTextureSamplerBinding ba[2] = { { tex[0], smp_lin }, { depth, smp_near } };
	pass(cmd, color, SDL_GPU_LOADOP_LOAD, p_apply, ba, 2, &u, (float)w, (float)h);
	SDL_SubmitGPUCommandBuffer(cmd);
	return 1;
}
