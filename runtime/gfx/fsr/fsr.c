/*
 * fsr.c: AMD FidelityFX FSR 3.1's upscaler run as Nativ's temporal anti-aliasing ("native AA": the render size is the
 * display size) on the main scene's colour, depth and motion vectors (gfx_gpu.c's motion pass, notes/motion-vectors.md
 * M4). Its passes are FidelityFX SDK v1.1.4's GLSL (third_party/fidelityfx, MIT) bound for SDL_GPU (passes.py,
 * ffx_fsr3upscaler_callbacks_nativ.h) and compiled ahead of time (build.sh -> ../fsr_shaders.h). This file is the
 * SDK's host side (sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp) in C: its resources, its constants and the
 * order of its dispatches, with what Nativ does differently said where it does it.
 */
#include "fsr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL_timer.h>
#include "../fsr_shaders.h"
#ifdef RECOMP_SPIRV_CROSS
#include <spirv_cross/spirv_cross_c.h>
#endif

/* ---- the passes: the bindings each takes, in the order passes.py numbers them (set 0: inputs then read-only buffers;
 * set 1: writable images then writable buffers; set 2: constant blocks) */
enum { P_PREPARE_INPUTS, P_LUMA_PYRAMID, P_SHADING_CHANGE_PYRAMID, P_SHADING_CHANGE, P_PREPARE_REACTIVITY, P_LUMA_INSTABILITY, P_ACCUMULATE, P_ACCUMULATE_SHARPEN, P_AUTOGEN_REACTIVE, P_RCAS, P_CLEARS, NPASS };
typedef struct { const char *name; const uint32_t *code[8]; size_t size[8]; int nsamp, nrobuf, nimg, nrwbuf, ncb, gx, gy; } Pass;   /* code[perm]: perm = FP16 * 4 + HDR * 2 + inverted depth */
#define PERM(n) { fsr_##n##_h0_i0, fsr_##n##_h0_i1, fsr_##n##_h1_i0, fsr_##n##_h1_i1, fsr_##n##_h0_i0_f16, fsr_##n##_h0_i1_f16, fsr_##n##_h1_i0_f16, fsr_##n##_h1_i1_f16 }, \
	{ sizeof fsr_##n##_h0_i0, sizeof fsr_##n##_h0_i1, sizeof fsr_##n##_h1_i0, sizeof fsr_##n##_h1_i1, sizeof fsr_##n##_h0_i0_f16, sizeof fsr_##n##_h0_i1_f16, sizeof fsr_##n##_h1_i0_f16, sizeof fsr_##n##_h1_i1_f16 }
static const Pass passes[NPASS] = {
	{ "prepare_inputs",         PERM(prepare_inputs),         3, 0, 4, 1, 1,   8, 8 },
	{ "luma_pyramid",           PERM(luma_pyramid),           2, 0, 8, 1, 2, 256, 1 },
	{ "shading_change_pyramid", PERM(shading_change_pyramid), 4, 0, 6, 1, 2, 256, 1 },
	{ "shading_change",         PERM(shading_change),         1, 0, 1, 0, 1,   8, 8 },
	{ "prepare_reactivity",     PERM(prepare_reactivity),     8, 1, 3, 0, 1,   8, 8 },
	{ "luma_instability",       PERM(luma_instability),       7, 0, 2, 0, 1,   8, 8 },
	{ "accumulate",             PERM(accumulate),             9, 0, 3, 0, 1,   8, 8 },
	{ "accumulate_sharpen",     PERM(accumulate_sharpen),     9, 0, 2, 0, 1,   8, 8 },
	{ "autogen_reactive",       PERM(autogen_reactive),       2, 0, 1, 0, 2,   8, 8 },
	{ "rcas",                   PERM(rcas),                   3, 0, 1, 0, 2,  64, 1 },
	{ "clears", { fsr_clears, fsr_clears, fsr_clears, fsr_clears, fsr_clears, fsr_clears, fsr_clears, fsr_clears }, { sizeof fsr_clears, sizeof fsr_clears, sizeof fsr_clears, sizeof fsr_clears, sizeof fsr_clears, sizeof fsr_clears, sizeof fsr_clears, sizeof fsr_clears }, 0, 0, 6, 2, 1, 64, 1 },
};

#ifdef RECOMP_SPIRV_CROSS
/* a pass as MSL, its resources where SDL_GPU's Metal backend binds them: [[texture]] the inputs then the writable images,
 * [[sampler]] beside each input, [[buffer]] the constant blocks, then the read-only buffers, then the writable ones */
static char *msl_of(const uint32_t *words, size_t nwords, const Pass *p) {
	spvc_context ctx; spvc_parsed_ir ir; spvc_compiler comp; spvc_compiler_options opts; const char *src; char *out = 0;
	if (spvc_context_create(&ctx) != SPVC_SUCCESS) return 0;
	if (spvc_context_parse_spirv(ctx, words, nwords, &ir) != SPVC_SUCCESS || spvc_context_create_compiler(ctx, SPVC_BACKEND_MSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &comp) != SPVC_SUCCESS) goto done;
	spvc_compiler_create_compiler_options(comp, &opts);
	spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_VERSION, SPVC_MAKE_MSL_VERSION(2, 3, 0));   /* (macOS 11, as gfx_gpu.c's) */
	spvc_compiler_install_compiler_options(comp, opts);
	#define BIND(set, b, tex, buf) do { spvc_msl_resource_binding_2 r; spvc_msl_resource_binding_init_2(&r); r.stage = SpvExecutionModelGLCompute; r.desc_set = (set); r.binding = (unsigned)(b); r.count = 1; \
		r.msl_texture = r.msl_sampler = (unsigned)(tex); r.msl_buffer = (unsigned)(buf); spvc_compiler_msl_add_resource_binding_2(comp, &r); } while (0)
	for (int i = 0; i < p->nsamp; i++) BIND(0, i, i, 0);
	for (int i = 0; i < p->nrobuf; i++) BIND(0, p->nsamp + i, 0, p->ncb + i);
	for (int i = 0; i < p->nimg; i++) BIND(1, i, p->nsamp + i, 0);
	for (int i = 0; i < p->nrwbuf; i++) BIND(1, p->nimg + i, 0, p->ncb + p->nrobuf + i);
	for (int i = 0; i < p->ncb; i++) BIND(2, i, 0, i);
	#undef BIND
	if (spvc_compiler_compile(comp, &src) == SPVC_SUCCESS) out = strdup(src);
done:
	if (!out) fprintf(stderr, "    fsr: %s to MSL: %s\n", p->name, spvc_context_get_last_error_string(ctx));
	spvc_context_destroy(ctx);
	return out;
}
#endif

static SDL_GPUComputePipeline *pipe[NPASS][8]; static int pipe_tried[NPASS][8];
static SDL_GPUComputePipeline *pipeline(SDL_GPUDevice *dev, int pass, int perm) {
	if (pipe_tried[pass][perm]) return pipe[pass][perm];
	pipe_tried[pass][perm] = 1;
	const Pass *p = &passes[pass];
	SDL_GPUComputePipelineCreateInfo ci = { .num_samplers = (Uint32)p->nsamp, .num_readonly_storage_buffers = (Uint32)p->nrobuf, .num_readwrite_storage_textures = (Uint32)p->nimg,
		.num_readwrite_storage_buffers = (Uint32)p->nrwbuf, .num_uniform_buffers = (Uint32)p->ncb, .threadcount_x = (Uint32)p->gx, .threadcount_y = (Uint32)p->gy, .threadcount_z = 1 };
	char *msl = 0;
	if (SDL_GetGPUShaderFormats(dev) & SDL_GPU_SHADERFORMAT_SPIRV) { ci.code = (const Uint8 *)p->code[perm]; ci.code_size = p->size[perm]; ci.entrypoint = "main"; ci.format = SDL_GPU_SHADERFORMAT_SPIRV; }
#ifdef RECOMP_SPIRV_CROSS
	else if (SDL_GetGPUShaderFormats(dev) & SDL_GPU_SHADERFORMAT_MSL) {
		if (!(msl = msl_of(p->code[perm], p->size[perm] / 4, p))) return 0;
		ci.code = (const Uint8 *)msl; ci.code_size = strlen(msl) + 1; ci.entrypoint = "main0"; ci.format = SDL_GPU_SHADERFORMAT_MSL;
	}
#endif
	else { fprintf(stderr, "    fsr: no shader format this device takes\n"); return 0; }
	pipe[pass][perm] = SDL_CreateGPUComputePipeline(dev, &ci);
	if (!pipe[pass][perm]) fprintf(stderr, "    fsr: %s pipeline: %s\n", p->name, SDL_GetError());
	free(msl);
	return pipe[pass][perm];
}

/* ---- the constants (ffx_fsr3upscaler_private.h's Fsr3UpscalerConstants = the shaders' cbFSR3Upscaler, std140) */
typedef struct {
	int32_t renderSize[2], previousFrameRenderSize[2], upscaleSize[2], previousFrameUpscaleSize[2], maxRenderSize[2], maxUpscaleSize[2];
	float deviceToViewDepth[4];
	float jitterOffset[2], previousFrameJitterOffset[2];
	float motionVectorScale[2], downscaleFactor[2];
	float motionVectorJitterCancellation[2];
	float tanHalfFOV, jitterPhaseCount;
	float deltaTime, deltaPreExposure, viewSpaceToMetersFactor, frameIndex;
	float velocityFactor, reactivenessScale, shadingChangeScale, accumulationAddedPerFrame, minDisocclusionAccumulation;
	float pad[3];   /* (std140 rounds the block to 16 bytes: 160) */
} Constants;
typedef struct { uint32_t mips, numWorkGroups, workGroupOffset[2], renderSize[2]; } SpdConstants;

/* ---- the resources (fsr3upscalerCreate's internalSurfaceDesc), made for one size and remade when it changes */
static struct {
	uint32_t rw, rh, dw, dh;   /* the render and display sizes they were made for */
	SDL_GPUTexture *dil_mv, *dil_depth, *inter16, *luma[2], *spd, *depth_mip1, *frame_info, *shading, *reactive_masks, *new_locks, *acc[2], *upscaled[2], *luma_hist[2], *lut, *zero8, *out, *kept_in, *autoreactive;
	SDL_GPUBuffer *rpnd, *spd_counter;   /* (Nativ: storage buffers for the SDK's two R32_UINT atomic textures, see the binding layer) */
	SDL_GPUSampler *linear;
	int first, failed; uint32_t frame_index; float prev_jitter[2]; Constants c;
	int keep; SDL_GPUTexture *last_acc;   /* (RECOMP_FSR_DUMP: keep the input; the history weight this frame wrote) */
} F;

static SDL_GPUTexture *tex(SDL_GPUDevice *dev, SDL_GPUTextureFormat fmt, uint32_t w, uint32_t h, uint32_t levels, SDL_GPUTextureUsageFlags usage) {
	SDL_GPUTextureCreateInfo ci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = fmt, .usage = usage, .width = w ? w : 1, .height = h ? h : 1, .layer_count_or_depth = 1, .num_levels = levels, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
	SDL_GPUTexture *t = SDL_CreateGPUTexture(dev, &ci);
	if (!t) { fprintf(stderr, "    fsr: a %ux%u texture (format %d): %s\n", w, h, (int)fmt, SDL_GetError()); F.failed = 1; }
	return t;
}
static void release_all(SDL_GPUDevice *dev) {
	SDL_GPUTexture **t[] = { &F.dil_mv, &F.dil_depth, &F.inter16, &F.luma[0], &F.luma[1], &F.spd, &F.depth_mip1, &F.frame_info, &F.shading, &F.reactive_masks, &F.new_locks,
		&F.acc[0], &F.acc[1], &F.upscaled[0], &F.upscaled[1], &F.luma_hist[0], &F.luma_hist[1], &F.lut, &F.zero8, &F.out, &F.kept_in, &F.autoreactive };
	for (size_t i = 0; i < sizeof t / sizeof *t; i++) if (*t[i]) { SDL_ReleaseGPUTexture(dev, *t[i]); *t[i] = 0; }
	if (F.rpnd) { SDL_ReleaseGPUBuffer(dev, F.rpnd); F.rpnd = 0; }
	if (F.spd_counter) { SDL_ReleaseGPUBuffer(dev, F.spd_counter); F.spd_counter = 0; }
}
static uint16_t f2h(float f) {   /* (round to nearest; RCAS's sharpness, a number in [0.25, 1]) */
	uint32_t x; memcpy(&x, &f, 4); const uint32_t sign = (x >> 16) & 0x8000; int e = (int)((x >> 23) & 0xff) - 127 + 15; uint32_t m = x & 0x7fffff;
	if (e <= 0) return (uint16_t)sign; if (e >= 31) return (uint16_t)(sign | 0x7c00);
	m += 0x1000; if (m & 0x800000) { m = 0; e++; } if (e >= 31) return (uint16_t)(sign | 0x7c00);
	return (uint16_t)(sign | (uint32_t)e << 10 | m >> 13);
}
static float lanczos2(float x) { return x == 0.0f ? 1.0f : sinf((float)M_PI * x) / ((float)M_PI * x) * (sinf(0.5f * (float)M_PI * x) / (0.5f * (float)M_PI * x)); }
static int make_all(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, uint32_t w, uint32_t h, uint32_t dw, uint32_t dh) {   /* w, h: the render size; dw, dh: the display size */
	release_all(dev);
	F.failed = 0; F.rw = w; F.rh = h; F.dw = dw; F.dh = dh;
	const uint32_t w2 = w / 2 ? w / 2 : 1, h2 = h / 2 ? h / 2 : 1;
	const SDL_GPUTextureUsageFlags S = SDL_GPU_TEXTUREUSAGE_SAMPLER, W = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE, RW = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE, C = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	/* (RW: read back in the pass that writes them -- new locks, frame info, SPD's mip 5; C: cleared by a render pass) */
	F.dil_mv = tex(dev, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, w, h, 1, S | W);
	F.dil_depth = tex(dev, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, w, h, 1, S | W);
	F.inter16 = tex(dev, SDL_GPU_TEXTUREFORMAT_R16_FLOAT, w, h, 1, S | W);   /* (the farthest depth, then the luma instability: aliased as in the SDK) */
	for (int i = 0; i < 2; i++) {
		F.luma[i] = tex(dev, SDL_GPU_TEXTUREFORMAT_R16_FLOAT, w, h, 1, S | W | C);
		F.acc[i] = tex(dev, SDL_GPU_TEXTUREFORMAT_R8_UNORM, w, h, 1, S | W | C);
		F.upscaled[i] = tex(dev, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, dw, dh, 1, S | W);
		F.luma_hist[i] = tex(dev, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, w, h, 1, S | W);
	}
	F.spd = tex(dev, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, w2, h2, 6, S | W | RW | C);   /* (Nativ: RGBA16F for the SDK's RG16F, see the binding layer; the six levels SPD stores) */
	F.depth_mip1 = tex(dev, SDL_GPU_TEXTUREFORMAT_R16_FLOAT, w2, h2, 1, S | W);
	F.frame_info = tex(dev, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, 1, 1, 1, S | W | RW | C);
	F.shading = tex(dev, SDL_GPU_TEXTUREFORMAT_R8_UNORM, w2, h2, 1, S | W);
	F.reactive_masks = tex(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, w, h, 1, S | W);
	F.autoreactive = tex(dev, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, w, h, 1, S | W);   /* (the reactive mask the generation pass makes) */
	F.new_locks = tex(dev, SDL_GPU_TEXTUREFORMAT_R8_UNORM, dw, dh, 1, S | W | RW);
	F.lut = tex(dev, SDL_GPU_TEXTUREFORMAT_R16_SNORM, 128, 1, 1, S);
	F.zero8 = tex(dev, SDL_GPU_TEXTUREFORMAT_R8_UNORM, 1, 1, 1, S | C);   /* (the SDK's default reactivity: no reactive or composition mask yet) */
	F.out = tex(dev, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, dw, dh, 1, S | W);   /* (the display-size ones: the history, its locks, the output; the rest at the render size, as the SDK's) */
	SDL_GPUBufferCreateInfo bi = { .usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, .size = 4 * w * h };
	F.rpnd = SDL_CreateGPUBuffer(dev, &bi);
	bi.size = 16; F.spd_counter = SDL_CreateGPUBuffer(dev, &bi);
	if (!F.rpnd || !F.spd_counter) { fprintf(stderr, "    fsr: buffers: %s\n", SDL_GetError()); F.failed = 1; }
	if (!F.linear) { SDL_GPUSamplerCreateInfo si = { .min_filter = SDL_GPU_FILTER_LINEAR, .mag_filter = SDL_GPU_FILTER_LINEAR, .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
		.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE, .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE, .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE };
		F.linear = SDL_CreateGPUSampler(dev, &si); if (!F.linear) F.failed = 1; }
	if (F.failed) { release_all(dev); return 0; }
	/* the Lanczos-2 table (128 weights over [0, 2], R16_SNORM) */
	int16_t lut[128]; for (int i = 0; i < 128; i++) lut[i] = (int16_t)lroundf(lanczos2(2.0f * (float)i / 127.0f) * 32767.0f);
	SDL_GPUTransferBufferCreateInfo xi = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = sizeof lut };
	SDL_GPUTransferBuffer *xb = SDL_CreateGPUTransferBuffer(dev, &xi);
	if (xb) {
		void *m = SDL_MapGPUTransferBuffer(dev, xb, false); if (m) { memcpy(m, lut, sizeof lut); SDL_UnmapGPUTransferBuffer(dev, xb); }
		SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
		SDL_GPUTextureTransferInfo src = { .transfer_buffer = xb, .pixels_per_row = 128, .rows_per_layer = 1 };
		SDL_GPUTextureRegion dst = { .texture = F.lut, .w = 128, .h = 1, .d = 1 };
		SDL_UploadToGPUTexture(cp, &src, &dst, false); SDL_EndGPUCopyPass(cp);
		SDL_ReleaseGPUTransferBuffer(dev, xb);   /* (released once the command buffer is done with it) */
	}
	F.first = 1; F.frame_index = 0; memset(&F.c, 0, sizeof F.c); F.prev_jitter[0] = F.prev_jitter[1] = 0;
	F.c.velocityFactor = 1.0f; F.c.reactivenessScale = 1.0f; F.c.shadingChangeScale = 1.0f; F.c.accumulationAddedPerFrame = 1.0f / 3.0f; F.c.minDisocclusionAccumulation = -1.0f / 3.0f;   /* (fsr3upscalerCreate's) */
	return 1;
}

/* RECOMP_FSR_TIME=2: each step submitted on its own and timed (a measurement: it stalls) */
static int prof_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_FSR_TIME"); v = e && atoi(e) >= 2; } return v; }
enum { S_CLEARS = NPASS, NSTEP };
static double prof_ms[2][NSTEP]; static uint32_t prof_n[2][NSTEP]; static int prof_half;   /* (per precision: RECOMP_FSR_FP16=ab alternates them) */
static void step(SDL_GPUDevice *dev, SDL_GPUCommandBuffer **cmd, int which) {
	if (!prof_on()) return;
	const uint64_t t0 = SDL_GetTicksNS();
	SDL_GPUFence *fe = SDL_SubmitGPUCommandBufferAndAcquireFence(*cmd); if (fe) { SDL_WaitForGPUFences(dev, true, &fe, 1); SDL_ReleaseGPUFence(dev, fe); }
	prof_ms[prof_half][which] += (double)(SDL_GetTicksNS() - t0) / 1e6; prof_n[prof_half][which]++;
	*cmd = SDL_AcquireGPUCommandBuffer(dev);
}
const char *fsr_profile(void) {
	static char b[1400]; int n = 0; b[0] = 0;
	for (int h = 0; h < 2; h++) {
		double tot = 0; int any = 0;
		for (int i = 0; i < NSTEP; i++) if (prof_n[h][i]) { tot += prof_ms[h][i] / prof_n[h][i]; any = 1; }
		if (!any) continue;
		n += snprintf(b + n, sizeof b - (size_t)n, "%s%s %.2f ms (", n ? "; " : "", h ? "FP16" : "FP32", tot);
		for (int i = 0, k = 0; i < NSTEP; i++) if (prof_n[h][i]) { n += snprintf(b + n, sizeof b - (size_t)n, "%s%s %.2f", k++ ? ", " : "", i == S_CLEARS ? "clears" : passes[i].name, prof_ms[h][i] / prof_n[h][i]); prof_ms[h][i] = 0; prof_n[h][i] = 0; }
		n += snprintf(b + n, sizeof b - (size_t)n, ")");
	}
	return b;
}
static void clear(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *t, uint32_t level, float r, float g, float b, float a) {
	SDL_GPUColorTargetInfo ci = { .texture = t, .mip_level = level, .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE, .clear_color = { r, g, b, a } };
	SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ci, 1, 0); if (rp) SDL_EndGPURenderPass(rp);
}
/* one pass: its inputs (texture-sampler pairs), read-only buffers, writable images (a level each) and buffers */
static int run(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, int pass, int perm, SDL_GPUTexture *const *in, SDL_GPUBuffer *const *robuf,
               SDL_GPUTexture *const *img, const uint32_t *img_level, SDL_GPUBuffer *const *rwbuf, uint32_t gx, uint32_t gy) {
	const Pass *p = &passes[pass];
	SDL_GPUComputePipeline *pl = pipeline(dev, pass, perm); if (!pl) return 0;
	SDL_GPUStorageTextureReadWriteBinding tb[8]; SDL_GPUStorageBufferReadWriteBinding bb[4]; SDL_GPUTextureSamplerBinding sb[16];
	for (int i = 0; i < p->nimg; i++) tb[i] = (SDL_GPUStorageTextureReadWriteBinding){ .texture = img[i], .mip_level = img_level ? img_level[i] : 0 };
	for (int i = 0; i < p->nrwbuf; i++) bb[i] = (SDL_GPUStorageBufferReadWriteBinding){ .buffer = rwbuf[i] };
	for (int i = 0; i < p->nsamp; i++) sb[i] = (SDL_GPUTextureSamplerBinding){ .texture = in[i], .sampler = F.linear };
	SDL_GPUComputePass *cp = SDL_BeginGPUComputePass(cmd, tb, (Uint32)p->nimg, bb, (Uint32)p->nrwbuf);
	if (!cp) return 0;
	SDL_BindGPUComputePipeline(cp, pl);
	if (p->nsamp) SDL_BindGPUComputeSamplers(cp, 0, sb, (Uint32)p->nsamp);
	if (p->nrobuf) SDL_BindGPUComputeStorageBuffers(cp, 0, robuf, (Uint32)p->nrobuf);
	SDL_DispatchGPUCompute(cp, gx, gy, 1);
	SDL_EndGPUComputePass(cp);
	return 1;
}

/* the camera FSR asks for (view-space depth: its disocclusion and lock tests in metres). Nativ doesn't know a game's:
 * a near 0.1, far 1000, 60 degree vertical camera, as the SDK's samples, until a per-game value is worth it */
static void depth_to_view(Constants *c, uint32_t w, uint32_t h, int inverted) {
	float fmin = 0.1f, fmax = 1000.0f; const float fov = 1.0472f;
	if (inverted) { float t = fmin; fmin = fmax; fmax = t; }
	const float q = fmax / (fmin - fmax);
	c->deviceToViewDepth[0] = -1.0f * q;   /* (d * matrix_elem_c, not infinite) */
	c->deviceToViewDepth[1] = q * fmin;
	const float aspect = (float)w / (float)h, cot = cosf(0.5f * fov) / sinf(0.5f * fov);
	c->deviceToViewDepth[2] = 1.0f / (cot / aspect);
	c->deviceToViewDepth[3] = 1.0f / cot;
	c->tanHalfFOV = tanf(atanf(tanf(fov / 2) * aspect));   /* (the horizontal half angle from the vertical one) */
	c->viewSpaceToMetersFactor = 1.0f;
}

int fsr_dispatch(SDL_GPUDevice *dev, SDL_GPUCommandBuffer **cmdp, const FsrFrame *f) {
	SDL_GPUCommandBuffer *cmd = *cmdp;
	static int dead; if (dead) return 0;
	if (!f->color || !f->depth || !f->motion || !f->dest || !f->rw || !f->rh || f->dw < f->rw || f->dh < f->rh) return 0;
	if ((F.rw != f->rw || F.rh != f->rh || F.dw != f->dw || F.dh != f->dh || !F.out) && !make_all(dev, cmd, f->rw, f->rh, f->dw, f->dh)) { dead = 1; fprintf(stderr, "    fsr: off (its resources)\n"); return 0; }
	/* FP16 (RECOMP_FSR_FP16=1; =ab alternates frames, for a profile): Apple GPUs run half at twice the rate (3.9 -> 3.2 ms
	 * profiled), but on Metal it darkened the output -- Killzone's frame mean 77 -> 65, its puddles' highlights halved --
	 * so it stays off until that's understood */
	static int fp16 = -1; static uint32_t nframe; if (fp16 < 0) { const char *e = getenv("RECOMP_FSR_FP16"); fp16 = e && !strcmp(e, "ab") ? 2 : e ? *e != '0' : 0; }
	const int half = fp16 == 2 ? (int)(nframe++ & 1) : fp16; prof_half = half;
	const int perm = (half ? 4 : 0) | (f->hdr ? 2 : 0) | (f->inverted ? 1 : 0);
	const int odd = F.frame_index & 1, reset = f->reset || F.first;
	SDL_GPUTexture *acc_srv = F.acc[odd], *acc_uav = F.acc[!odd], *up_srv = F.upscaled[odd], *up_uav = F.upscaled[!odd];
	SDL_GPUTexture *lh_srv = F.luma_hist[odd], *lh_uav = F.luma_hist[!odd], *cur_luma = F.luma[odd], *prev_luma = F.luma[!odd];
	if (F.first) { for (int i = 0; i < 2; i++) { clear(cmd, F.acc[i], 0, 0, 0, 0, 0); clear(cmd, F.luma[i], 0, 0, 0, 0, 0); } clear(cmd, F.zero8, 0, 0, 0, 0, 0); }
	if (F.keep) {   /* (the input as FSR got it: the blit below overwrites the colour target) */
		if (!F.kept_in) F.kept_in = tex(dev, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, f->rw, f->rh, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
		SDL_GPUBlitInfo kb = { .source = { .texture = f->color, .w = f->rw, .h = f->rh }, .destination = { .texture = F.kept_in, .w = f->rw, .h = f->rh }, .load_op = SDL_GPU_LOADOP_DONT_CARE, .filter = SDL_GPU_FILTER_NEAREST };
		if (F.kept_in) SDL_BlitGPUTexture(cmd, &kb);
	}
	F.last_acc = acc_uav;
	F.first = 0;

	/* the constants (fsr3upscalerDispatch's) */
	Constants *c = &F.c;
	c->previousFrameJitterOffset[0] = c->jitterOffset[0]; c->previousFrameJitterOffset[1] = c->jitterOffset[1];
	c->jitterOffset[0] = f->jitter_x; c->jitterOffset[1] = f->jitter_y;
	c->previousFrameRenderSize[0] = c->renderSize[0]; c->previousFrameRenderSize[1] = c->renderSize[1];
	c->renderSize[0] = c->maxRenderSize[0] = (int32_t)f->rw; c->renderSize[1] = c->maxRenderSize[1] = (int32_t)f->rh;
	c->previousFrameUpscaleSize[0] = c->upscaleSize[0]; c->previousFrameUpscaleSize[1] = c->upscaleSize[1];
	c->upscaleSize[0] = c->maxUpscaleSize[0] = (int32_t)f->dw; c->upscaleSize[1] = c->maxUpscaleSize[1] = (int32_t)f->dh;
	c->downscaleFactor[0] = (float)f->rw / (float)f->dw; c->downscaleFactor[1] = (float)f->rh / (float)f->dh;
	depth_to_view(c, f->rw, f->rh, f->inverted);
	c->deltaPreExposure = 1.0f;
	/* the vectors: ours are (current - previous) / 2 in clip units with y up, FSR's go from the current pixel to the previous
	 * one in UV units with y down -- the shaders take raw * motionVectorScale, the SDK's (params / render size) */
	c->motionVectorScale[0] = -1.0f; c->motionVectorScale[1] = 1.0f;
	c->motionVectorJitterCancellation[0] = c->motionVectorJitterCancellation[1] = 0;   /* (the vectors carry no jitter) */
	{ const float k = (float)f->dw / (float)f->rw; const int want = (int)(8.0f * k * k);   /* (ffxFsr3UpscalerGetJitterPhaseCount: 8 * (display / render)^2, approached a phase a frame) */
	  if (reset || c->jitterPhaseCount == 0) c->jitterPhaseCount = (float)want; else if (want > c->jitterPhaseCount) c->jitterPhaseCount++; else if (want < c->jitterPhaseCount) c->jitterPhaseCount--; }
	c->deltaTime = f->frame_ms <= 0 ? 0 : f->frame_ms >= 1000 ? 1.0f : f->frame_ms / 1000.0f;
	c->frameIndex = reset ? 0.0f : c->frameIndex + 1.0f;
	/* SPD over the render size (ffxSpdSetup, mips from the size) */
	const uint32_t spd_x = (f->rw - 1) / 64 + 1, spd_y = (f->rh - 1) / 64 + 1;
	uint32_t mx = f->rw > f->rh ? f->rw : f->rh, mips = 0; while (mx > 1 && mips < 12) { mx >>= 1; mips++; }
	const SpdConstants spd = { mips, spd_x * spd_y, { 0, 0 }, { f->rw, f->rh } };

	/* the clears (the SDK's jobs before its dispatches) */
	if (reset) { clear(cmd, acc_srv, 0, 0, 0, 0, 0); clear(cmd, F.frame_info, 0, -1.0f, 1.0f, 0, 0); }
	{	/* each frame, one pass (clears.glsl): SPD's mips (it reads pixels it never wrote otherwise), the reconstructed previous depth, SPD's counter */
		const float far = f->inverted ? 0.0f : 1.0f; uint32_t u[4]; memcpy(&u[0], &far, 4); u[1] = f->rw * f->rh; u[2] = f->rw / 2 ? f->rw / 2 : 1; u[3] = f->rh / 2 ? f->rh / 2 : 1;
		SDL_PushGPUComputeUniformData(cmd, 0, u, sizeof u);
		static const uint32_t lv[6] = { 0, 1, 2, 3, 4, 5 }; SDL_GPUTexture *img[6] = { F.spd, F.spd, F.spd, F.spd, F.spd, F.spd }; SDL_GPUBuffer *rw[2] = { F.rpnd, F.spd_counter };
		const uint32_t n = u[1] > u[2] * u[3] ? u[1] : u[2] * u[3];
		if (!run(dev, cmd, P_CLEARS, 0, 0, 0, img, lv, rw, (n + 63) / 64, 1)) { dead = 1; fprintf(stderr, "    fsr: off (its clears pass)\n"); *cmdp = cmd; return 0; }
	}
	step(dev, &cmd, S_CLEARS);
	#define PUSH() do { SDL_PushGPUComputeUniformData(cmd, 0, c, sizeof *c); SDL_PushGPUComputeUniformData(cmd, 1, &spd, sizeof spd); } while (0)   /* (again in each new command buffer of a profile run) */
	PUSH();
	int ok = 1;
	if (f->opaque) {   /* the reactive mask (ffxFsr3UpscalerContextGenerateReactiveMask, the SDK sample's settings: tonemapped, the largest channel's change, thresholded) */
		struct { float scale, threshold, binary; uint32_t flags; } gr = { 1.0f, f->reactive_threshold, f->reactive_value, 1u | 4u | 8u };
		SDL_PushGPUComputeUniformData(cmd, 1, &gr, sizeof gr);
		SDL_GPUTexture *in[] = { f->opaque, f->color }, *img[] = { F.autoreactive };
		ok = run(dev, cmd, P_AUTOGEN_REACTIVE, perm, in, 0, img, 0, 0, (f->rw + 7) / 8, (f->rh + 7) / 8); step(dev, &cmd, P_AUTOGEN_REACTIVE);
		PUSH();   /* (the SPD block back in slot 1) */
	}

	const uint32_t gx = (f->rw + 7) / 8, gy = (f->rh + 7) / 8, gx2 = (f->rw / 2 + 7) / 8, gy2 = (f->rh / 2 + 7) / 8, dx = (f->dw + 7) / 8, dy = (f->dh + 7) / 8;   /* (d: the display-size passes) */
	static const uint32_t spd_levels[8] = { 0, 1, 2, 3, 4, 5, 0, 0 }, lp_levels[8] = { 0, 0, 1, 2, 3, 4, 5, 0 };   /* (the luma pyramid's images: frame info, SPD 0-5, depth mip 1) */
	{ SDL_GPUTexture *in[] = { f->motion, f->depth, f->color }, *img[] = { F.dil_mv, F.dil_depth, F.inter16, cur_luma }; SDL_GPUBuffer *rw[] = { F.rpnd };
	  ok = ok && run(dev, cmd, P_PREPARE_INPUTS, perm, in, 0, img, 0, rw, gx, gy); step(dev, &cmd, P_PREPARE_INPUTS); if (prof_on()) PUSH(); }
	{ SDL_GPUTexture *in[] = { cur_luma, F.inter16 }, *img[] = { F.frame_info, F.spd, F.spd, F.spd, F.spd, F.spd, F.spd, F.depth_mip1 }; SDL_GPUBuffer *rw[] = { F.spd_counter };
	  ok = ok && run(dev, cmd, P_LUMA_PYRAMID, perm, in, 0, img, lp_levels, rw, spd_x, spd_y); step(dev, &cmd, P_LUMA_PYRAMID); if (prof_on()) PUSH(); }
	{ SDL_GPUTexture *in[] = { cur_luma, prev_luma, F.dil_mv, F.frame_info }, *img[] = { F.spd, F.spd, F.spd, F.spd, F.spd, F.spd }; SDL_GPUBuffer *rw[] = { F.spd_counter };
	  ok = ok && run(dev, cmd, P_SHADING_CHANGE_PYRAMID, perm, in, 0, img, spd_levels, rw, spd_x, spd_y); step(dev, &cmd, P_SHADING_CHANGE_PYRAMID); if (prof_on()) PUSH(); }
	{ SDL_GPUTexture *in[] = { F.spd }, *img[] = { F.shading };
	  ok = ok && run(dev, cmd, P_SHADING_CHANGE, perm, in, 0, img, 0, 0, gx2, gy2); step(dev, &cmd, P_SHADING_CHANGE); if (prof_on()) PUSH(); }
	{ SDL_GPUTexture *in[] = { F.dil_mv, F.dil_depth, f->opaque ? F.autoreactive : F.zero8, F.zero8, acc_srv, F.shading, cur_luma, F.frame_info }, *img[] = { F.reactive_masks, F.new_locks, acc_uav }; SDL_GPUBuffer *ro[] = { F.rpnd };
	  ok = ok && run(dev, cmd, P_PREPARE_REACTIVITY, perm, in, ro, img, 0, 0, gx, gy); step(dev, &cmd, P_PREPARE_REACTIVITY); if (prof_on()) PUSH(); }
	{ SDL_GPUTexture *in[] = { F.frame_info, F.reactive_masks, F.dil_mv, F.frame_info, lh_srv, F.depth_mip1, cur_luma }, *img[] = { lh_uav, F.inter16 };
	  ok = ok && run(dev, cmd, P_LUMA_INSTABILITY, perm, in, 0, img, 0, 0, gx, gy); step(dev, &cmd, P_LUMA_INSTABILITY); if (prof_on()) PUSH(); }
	if (f->sharpness < 0) {
		SDL_GPUTexture *in[] = { F.frame_info, F.reactive_masks, F.dil_mv, up_srv, F.lut, F.depth_mip1, cur_luma, F.inter16, f->color }, *img[] = { up_uav, F.out, F.new_locks };
		ok = ok && run(dev, cmd, P_ACCUMULATE, perm, in, 0, img, 0, 0, dx, dy); step(dev, &cmd, P_ACCUMULATE); if (prof_on()) PUSH();
	} else {   /* the accumulation into the history, then RCAS from it into the output (FsrRcasCon: the SDK maps sharpness s to 2 - 2s stops) */
		SDL_GPUTexture *in[] = { F.frame_info, F.reactive_masks, F.dil_mv, up_srv, F.lut, F.depth_mip1, cur_luma, F.inter16, f->color }, *img[] = { up_uav, F.new_locks };
		ok = ok && run(dev, cmd, P_ACCUMULATE_SHARPEN, perm, in, 0, img, 0, 0, dx, dy); step(dev, &cmd, P_ACCUMULATE_SHARPEN); if (prof_on()) PUSH();
		const float s = f->sharpness > 1 ? 1.0f : f->sharpness, lin = exp2f(-(2.0f - 2.0f * s)); const uint16_t hs = f2h(lin);
		uint32_t rcas[4]; memcpy(&rcas[0], &lin, 4); rcas[1] = (uint32_t)hs | (uint32_t)hs << 16; rcas[2] = rcas[3] = 0;
		SDL_PushGPUComputeUniformData(cmd, 1, rcas, sizeof rcas);
		SDL_GPUTexture *rin[] = { F.frame_info, up_uav, f->color }, *rimg[] = { F.out };
		ok = ok && run(dev, cmd, P_RCAS, perm, rin, 0, rimg, 0, 0, (f->dw + 15) / 16, (f->dh + 15) / 16); step(dev, &cmd, P_RCAS);
	}
	if (!ok) { dead = 1; fprintf(stderr, "    fsr: off (a pipeline failed: above)\n"); *cmdp = cmd; return 0; }
	/* the anti-aliased frame into the colour target (a blit: its format may be RGBA8, sRGB or RGBA16F) */
	SDL_GPUBlitInfo bl = { .source = { .texture = F.out, .w = f->dw, .h = f->dh }, .destination = { .texture = f->dest, .w = f->dw, .h = f->dh }, .load_op = SDL_GPU_LOADOP_DONT_CARE, .filter = SDL_GPU_FILTER_NEAREST };
	SDL_BlitGPUTexture(cmd, &bl);
	#undef PUSH
	F.frame_index = (F.frame_index + 1) % 16;
	*cmdp = cmd;
	return 1;
}

/* ---- RECOMP_FSR_DUMP (gfx_gpu.c): FSR's view of a frame, to see why it does what it does */
void fsr_keep_input(int keep) { F.keep = keep; }
static void *download(SDL_GPUDevice *dev, SDL_GPUTexture *t, uint32_t w, uint32_t h, uint32_t bpp) {
	SDL_GPUTransferBufferCreateInfo xi = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = bpp * w * h };
	SDL_GPUTransferBuffer *xb = SDL_CreateGPUTransferBuffer(dev, &xi); if (!xb) return 0;
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev); SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureRegion src = { .texture = t, .w = w, .h = h, .d = 1 }; SDL_GPUTextureTransferInfo dst = { .transfer_buffer = xb, .pixels_per_row = w, .rows_per_layer = h };
	SDL_DownloadFromGPUTexture(cp, &src, &dst); SDL_EndGPUCopyPass(cp);
	SDL_GPUFence *fe = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd); SDL_WaitForGPUFences(dev, true, &fe, 1); SDL_ReleaseGPUFence(dev, fe);
	void *out = malloc((size_t)bpp * w * h); const void *m = SDL_MapGPUTransferBuffer(dev, xb, false);
	if (out && m) memcpy(out, m, (size_t)bpp * w * h); else { free(out); out = 0; }
	if (m) SDL_UnmapGPUTransferBuffer(dev, xb);
	SDL_ReleaseGPUTransferBuffer(dev, xb);
	return out;
}
static float h2f(uint16_t h) { uint32_t e = (h >> 10) & 0x1f, m = h & 0x3ff; float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? 65504.0f : ldexpf((float)(m | 0x400), (int)e - 25); return h & 0x8000 ? -v : v; }
static uint8_t u8f(float v) { return (uint8_t)(v != v ? 0 : v <= 0 ? 0 : v >= 1 ? 255 : v * 255 + 0.5f); }
/* kind: 0 RGBA16F colour (clamped), 1 R8 grey, 2 RGBA8, 3 RG16F vectors (around grey, gain), 4 R32F depth, 5 R16F grey */
static void write_ppm(SDL_GPUDevice *dev, SDL_GPUTexture *t, int kind, const char *prefix, const char *what, int display) {   /* (display: a display-size texture) */
	if (!t) return;
	const uint32_t w = display ? F.dw : F.rw, h = display ? F.dh : F.rh, bpp = kind == 0 ? 8 : kind == 1 ? 1 : kind == 2 || kind == 3 || kind == 4 ? 4 : 2;
	uint8_t *px = download(dev, t, w, h, bpp); if (!px) return;
	char path[600]; snprintf(path, sizeof path, "%s_%s.ppm", prefix, what);
	FILE *f = fopen(path, "wb");
	if (f) {
		fprintf(f, "P6\n%u %u\n255\n", w, h);
		for (uint32_t i = 0; i < w * h; i++) {
			uint8_t c[3]; const uint8_t *q = px + (size_t)bpp * i; uint16_t hv[4];
			switch (kind) {
			case 0: memcpy(hv, q, 8); for (int k = 0; k < 3; k++) c[k] = u8f(h2f(hv[k])); break;
			case 1: c[0] = c[1] = c[2] = q[0]; break;
			case 2: memcpy(c, q, 3); break;
			case 3: memcpy(hv, q, 4); c[0] = u8f(0.5f + 30 * h2f(hv[0])); c[1] = u8f(0.5f + 30 * h2f(hv[1])); c[2] = 128; break;
			case 4: { float d; memcpy(&d, q, 4); c[0] = c[1] = c[2] = u8f(d); break; }
			default: memcpy(hv, q, 2); c[0] = c[1] = c[2] = u8f(h2f(hv[0])); break;
			}
			fwrite(c, 1, 3, f);
		}
		fclose(f);
	}
	free(px);
}
void fsr_dump(SDL_GPUDevice *dev, const char *prefix) {
	write_ppm(dev, F.kept_in, 0, prefix, "in", 0);
	write_ppm(dev, F.out, 0, prefix, "out", 1);
	write_ppm(dev, F.last_acc, 1, prefix, "acc", 0);
	write_ppm(dev, F.reactive_masks, 2, prefix, "reactive", 0);
	write_ppm(dev, F.dil_mv, 3, prefix, "mv", 0);
	write_ppm(dev, F.dil_depth, 4, prefix, "depth", 0);
	write_ppm(dev, F.inter16, 5, prefix, "instability", 0);
	write_ppm(dev, F.autoreactive, 4, prefix, "autoreactive", 0);
	{ float *fi = download(dev, F.frame_info, 1, 1, 16);   /* (frame info: exposure, log luma, scene average luma) */
	  if (fi) { fprintf(stderr, "    fsr: frame info: %g %g %g %g\n", fi[0], fi[1], fi[2], fi[3]); free(fi); } }
	fprintf(stderr, "    fsr: frame %u: constants: jitter %.3f %.3f (previous %.3f %.3f), frame index %.0f, phases %.0f, delta %.4f s, mv scale %.1f %.1f, view %.3f %.3f %.3f %.3f\n",
	        F.frame_index, F.c.jitterOffset[0], F.c.jitterOffset[1], F.c.previousFrameJitterOffset[0], F.c.previousFrameJitterOffset[1], F.c.frameIndex, F.c.jitterPhaseCount, F.c.deltaTime,
	        F.c.motionVectorScale[0], F.c.motionVectorScale[1], F.c.deviceToViewDepth[0], F.c.deviceToViewDepth[1], F.c.deviceToViewDepth[2], F.c.deviceToViewDepth[3]);
}
