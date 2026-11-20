/*
 * framegen.c: frame generation. A game drawing fewer frames than the display shows (30 a second on a 60 Hz display,
 * or 60 on 120 Hz) has each frame shown twice or more; the present loop (window.c) shows one made between the last
 * two in its place. It is made from the finished pictures alone, so it works in any game: the motion between the two
 * is measured on their luma (an optical flow), and each pixel of the new frame blends the two along it.
 *
 *   1. reduce   each new frame's luma at a working size (at most WORK_MAX wide) and a pyramid of halvings below it
 *   2. search   a vector per 4x4 block at each level, coarsest first: the previous frame at -v/2 matched against the
 *               current one at +v/2 (where the generated frame needs its vectors), each level starting from the
 *               coarser one's, the finest to half a pixel
 *   3. stat     the finest level's mean match (a cut: frames that don't match as a whole aren't blended) and mean
 *               vector (the scene's motion as a whole)
 *   3b. fsearch per block of the finest field, its symmetric vector checked forward (the previous frame's window
 *               against the current one's along it); where that doesn't explain it -- something left or came in: a
 *               thin object, an edge being covered -- every vector within a radius at half the working size, forward
 *   3c. project that forward motion carried to time t, per block: what lands there, furthest from the mean motion
 *               (in front)
 *   4. pick     per pixel of the working size at time t, the block vector around it (or zero, or the projected one)
 *               the two frames agree on best there; a projected one also wins a tie when it is further from the mean
 *               motion: a still HUD keeps its own pixels still over a moving scene, a thin object keeps its motion
 *               over a still one
 *   5. warp     the frame: each pixel the previous frame at -t*v blended with the current one at +(1-t)*v, v the one of
 *               the picked vectors around it the two agree on best at that pixel (an object's edge inside a working
 *               pixel); where none agrees, the side nearer to the pixel as the nearer frame has it, not a ghost of both
 *
 * A thin object (narrower than its motion over the pair) is what 3b-3c are for: the symmetric search sees the background
 * in its blocks and "still" matches it as well as its own motion; framegen_test --objects checks it.
 *
 * 1 runs once per game frame, 2-3b once per pair (on its first generated frame), 3c-5 per generated frame. The passes
 * are GLSL compute shaders (*.comp here, compiled by build.sh into ../framegen_shaders.h), run as SPIR-V on Vulkan
 * and through SPIRV-Cross on Metal. Written for Nativ, not taken from an existing interpolator.
 */
#include "framegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../framegen_shaders.h"
#ifdef RECOMP_SPIRV_CROSS
#include <spirv_cross/spirv_cross_c.h>
#endif

int settings_int(const char *key, int def);

#define WORK_MAX 1280   /* the working size's widest: the Vita's own 960 at render scale 1 and 2, 720 at 3 */
#define NL 5            /* pyramid levels: at most 1/16 of the working size, where the search covers +-6 pixels, so +-96 */
#define RADIUS 6
#define LAMBDA 0.002f   /* a vector's cost per pixel of difference from its parent's (mean luma difference units) */
#define ZERO_BIAS 0.003f
#define CUT 0.10f       /* the finest level's mean difference above which the pair is a cut */
#define FRADIUS 10      /* the forward search's radius at half the working size: +-20 working pixels */
#define FEXPLAINED 0.03f   /* a forward window difference below which the symmetric vector explains a block */
#define PRADIUS 4       /* the projection's, in blocks: a motion of up to 4 * 4 / t working pixels lands within it */

enum { P_REDUCE, P_SEARCH, P_PICK, P_WARP, P_STAT, P_FSEARCH, P_PROJECT, NP };
typedef struct { const char *name; const uint32_t *code; size_t size; int nsamp, gx, gy; } Pass;
static const Pass passes[NP] = {   /* each writes one image and takes the one constant block */
	{ "reduce", fg_reduce, sizeof fg_reduce, 1, 8, 8 },
	{ "search", fg_search, sizeof fg_search, 3, 8, 8 },
	{ "pick", fg_pick, sizeof fg_pick, 5, 8, 8 },
	{ "warp", fg_warp, sizeof fg_warp, 4, 8, 8 },
	{ "stat", fg_stat, sizeof fg_stat, 1, 16, 16 },
	{ "fsearch", fg_fsearch, sizeof fg_fsearch, 3, 8, 8 },
	{ "project", fg_project, sizeof fg_project, 2, 8, 8 },
};
typedef struct { int32_t a[4], b[4]; float c[4], d[4]; } U;   /* common.glsl's block (std140) */

static struct {
	uint32_t w, h, levels;                      /* the frames these were made for */
	int scale, nl, flev; uint32_t lw[NL], lh[NL], fw[NL], fh[NL];   /* flev: the forward search's level (its field is the finest's grid) */
	SDL_GPUTexture *col[2], *lum[2][NL], *fld[NL], *pix, *stat, *gen, *ffld, *proj;
	SDL_GPUSampler *lin;
	SDL_GPUComputePipeline *pipe[NP]; int tried[NP];
	int cur, have, flow, failed, said;
} G;

int fg_wanted(void) {
	const char *e = getenv("RECOMP_FRAME_GEN");
	return e && *e ? *e != '0' : settings_int("frame_gen", 0) != 0;
}

#ifdef RECOMP_SPIRV_CROSS
/* a pass as MSL, its resources where SDL_GPU's Metal backend binds a compute pipeline's: [[texture]] the sampled inputs
 * then the writable image, [[sampler]] beside each input, [[buffer]] the constant block */
static char *msl_of(const Pass *p) {
	spvc_context ctx; spvc_parsed_ir ir; spvc_compiler comp; spvc_compiler_options opts; const char *src; char *out = 0;
	if (spvc_context_create(&ctx) != SPVC_SUCCESS) return 0;
	if (spvc_context_parse_spirv(ctx, p->code, p->size / 4, &ir) != SPVC_SUCCESS || spvc_context_create_compiler(ctx, SPVC_BACKEND_MSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &comp) != SPVC_SUCCESS) goto done;
	spvc_compiler_create_compiler_options(comp, &opts);
	spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_VERSION, SPVC_MAKE_MSL_VERSION(2, 3, 0));
	spvc_compiler_install_compiler_options(comp, opts);
	#define BIND(set, b, tex, buf) do { spvc_msl_resource_binding_2 r; spvc_msl_resource_binding_init_2(&r); r.stage = SpvExecutionModelGLCompute; r.desc_set = (set); r.binding = (unsigned)(b); r.count = 1; \
		r.msl_texture = r.msl_sampler = (unsigned)(tex); r.msl_buffer = (unsigned)(buf); spvc_compiler_msl_add_resource_binding_2(comp, &r); } while (0)
	for (int i = 0; i < p->nsamp; i++) BIND(0, i, i, 0);
	BIND(1, 0, p->nsamp, 0);
	BIND(2, 0, 0, 0);
	#undef BIND
	if (spvc_compiler_compile(comp, &src) == SPVC_SUCCESS) out = strdup(src);
done:
	if (!out) fprintf(stderr, "    framegen: %s to MSL: %s\n", p->name, spvc_context_get_last_error_string(ctx));
	spvc_context_destroy(ctx);
	return out;
}
#endif
static SDL_GPUComputePipeline *pipeline(SDL_GPUDevice *dev, int i) {
	if (G.tried[i]) return G.pipe[i];
	G.tried[i] = 1;
	const Pass *p = &passes[i];
	SDL_GPUComputePipelineCreateInfo ci = { .num_samplers = (Uint32)p->nsamp, .num_readwrite_storage_textures = 1, .num_uniform_buffers = 1,
		.threadcount_x = (Uint32)p->gx, .threadcount_y = (Uint32)p->gy, .threadcount_z = 1 };
	char *msl = 0;
	if (SDL_GetGPUShaderFormats(dev) & SDL_GPU_SHADERFORMAT_SPIRV) { ci.code = (const Uint8 *)p->code; ci.code_size = p->size; ci.entrypoint = "main"; ci.format = SDL_GPU_SHADERFORMAT_SPIRV; }
#ifdef RECOMP_SPIRV_CROSS
	else if (SDL_GetGPUShaderFormats(dev) & SDL_GPU_SHADERFORMAT_MSL) {
		if (!(msl = msl_of(p))) return 0;
		ci.code = (const Uint8 *)msl; ci.code_size = strlen(msl) + 1; ci.entrypoint = "main0"; ci.format = SDL_GPU_SHADERFORMAT_MSL;
	}
#endif
	else { fprintf(stderr, "    framegen: no shader format this device takes\n"); return 0; }
	G.pipe[i] = SDL_CreateGPUComputePipeline(dev, &ci);
	if (!G.pipe[i]) fprintf(stderr, "    framegen: %s pipeline: %s\n", p->name, SDL_GetError());
	free(msl);
	return G.pipe[i];
}

static SDL_GPUTexture *tex(SDL_GPUDevice *dev, SDL_GPUTextureFormat fmt, uint32_t w, uint32_t h, uint32_t levels, SDL_GPUTextureUsageFlags usage) {
	SDL_GPUTextureCreateInfo ci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = fmt, .usage = usage, .width = w ? w : 1, .height = h ? h : 1, .layer_count_or_depth = 1, .num_levels = levels, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
	SDL_GPUTexture *t = SDL_CreateGPUTexture(dev, &ci);
	if (!t) { fprintf(stderr, "    framegen: a %ux%u texture: %s\n", w, h, SDL_GetError()); G.failed = 1; }
	return t;
}
static void release_all(SDL_GPUDevice *dev) {
	SDL_GPUTexture **t[7 + 3 * NL] = { &G.col[0], &G.col[1], &G.pix, &G.stat, &G.gen, &G.ffld, &G.proj }; int n = 7;
	for (int k = 0; k < NL; k++) { t[n++] = &G.lum[0][k]; t[n++] = &G.lum[1][k]; t[n++] = &G.fld[k]; }
	for (int i = 0; i < n; i++) if (*t[i]) { SDL_ReleaseGPUTexture(dev, *t[i]); *t[i] = 0; }
}
static int make_all(SDL_GPUDevice *dev, uint32_t w, uint32_t h, uint32_t levels) {
	release_all(dev);
	G.failed = 0; G.w = w; G.h = h; G.levels = levels; G.have = 0; G.flow = 0;
	for (G.scale = 1; G.scale < 8 && w / (uint32_t)G.scale > WORK_MAX; G.scale *= 2) {}
	G.lw[0] = w / (uint32_t)G.scale; G.lh[0] = h / (uint32_t)G.scale; G.nl = 1;
	while (G.nl < NL && G.lw[G.nl - 1] / 2 >= 32 && G.lh[G.nl - 1] / 2 >= 16) { G.lw[G.nl] = G.lw[G.nl - 1] / 2; G.lh[G.nl] = G.lh[G.nl - 1] / 2; G.nl++; }
	const SDL_GPUTextureUsageFlags S = SDL_GPU_TEXTUREUSAGE_SAMPLER, W = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
	for (int i = 0; i < 2; i++) G.col[i] = tex(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, w, h, 1, S);
	for (int k = 0; k < G.nl; k++) {
		G.fw[k] = (G.lw[k] + 3) / 4; G.fh[k] = (G.lh[k] + 3) / 4;
		for (int i = 0; i < 2; i++) G.lum[i][k] = tex(dev, SDL_GPU_TEXTUREFORMAT_R16_FLOAT, G.lw[k], G.lh[k], 1, S | W);
		G.fld[k] = tex(dev, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, G.fw[k], G.fh[k], 1, S | W);
	}
	G.pix = tex(dev, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, G.lw[0], G.lh[0], 1, S | W);
	G.stat = tex(dev, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, 1, 1, 1, S | W);
	G.flev = G.nl > 1 ? 1 : 0;
	G.ffld = tex(dev, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, G.fw[0], G.fh[0], 1, S | W);
	G.proj = tex(dev, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, G.fw[0], G.fh[0], 1, S | W);
	G.gen = tex(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, w, h, levels, S | W | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);   /* (a colour target: its mips) */
	if (!G.lin) {
		SDL_GPUSamplerCreateInfo sci = { .min_filter = SDL_GPU_FILTER_LINEAR, .mag_filter = SDL_GPU_FILTER_LINEAR, .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
			.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE, .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE, .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE };
		if (!(G.lin = SDL_CreateGPUSampler(dev, &sci))) G.failed = 1;
	}
	for (int i = 0; i < NP; i++) if (!pipeline(dev, i)) G.failed = 1;
	if (G.failed && !G.said) { fprintf(stderr, "    framegen: unavailable on this device: frames shown as they are\n"); G.said = 1; }
	else if (!G.failed) fprintf(stderr, "    framegen: %ux%u frames, motion measured at %ux%u (%d levels)\n", w, h, G.lw[0], G.lh[0], G.nl);
	return !G.failed;
}

static void run(SDL_GPUCommandBuffer *cmd, int p, SDL_GPUTexture *const *in, SDL_GPUTexture *out, const U *u, uint32_t w, uint32_t h) {
	SDL_GPUStorageTextureReadWriteBinding tb = { .texture = out };
	SDL_GPUTextureSamplerBinding sb[5];
	for (int i = 0; i < passes[p].nsamp; i++) sb[i] = (SDL_GPUTextureSamplerBinding){ .texture = in[i], .sampler = G.lin };
	SDL_PushGPUComputeUniformData(cmd, 0, u, sizeof *u);
	SDL_GPUComputePass *cp = SDL_BeginGPUComputePass(cmd, &tb, 1, 0, 0);
	if (!cp) return;
	SDL_BindGPUComputePipeline(cp, G.pipe[p]);
	SDL_BindGPUComputeSamplers(cp, 0, sb, (Uint32)passes[p].nsamp);
	SDL_DispatchGPUCompute(cp, (w + (uint32_t)passes[p].gx - 1) / (uint32_t)passes[p].gx, (h + (uint32_t)passes[p].gy - 1) / (uint32_t)passes[p].gy, 1);
	SDL_EndGPUComputePass(cp);
}

void fg_forget(void) { G.have = 0; G.flow = 0; }

void fg_frame(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, uint32_t w, uint32_t h, uint32_t levels) {
	if (!src || !w || !h) return;
	if (G.w != w || G.h != h || G.levels != levels || !G.col[0]) { if (!make_all(dev, w, h, levels)) return; }
	else if (G.failed) return;
	G.cur ^= 1;
	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureLocation s = { .texture = src }, d = { .texture = G.col[G.cur] };
	SDL_CopyGPUTextureToTexture(cp, &s, &d, w, h, 1, false);
	SDL_EndGPUCopyPass(cp);
	U u = { { (int)G.lw[0], (int)G.lh[0], G.scale, 1 }, { 0 }, { 1.0f / (float)w, 1.0f / (float)h } };
	run(cmd, P_REDUCE, &G.col[G.cur], G.lum[G.cur][0], &u, G.lw[0], G.lh[0]);
	for (int k = 1; k < G.nl; k++) {
		U v = { { (int)G.lw[k], (int)G.lh[k], 2, 0 }, { 0 }, { 1.0f / (float)G.lw[k - 1], 1.0f / (float)G.lh[k - 1] } };
		run(cmd, P_REDUCE, &G.lum[G.cur][k - 1], G.lum[G.cur][k], &v, G.lw[k], G.lh[k]);
	}
	if (G.have < 2) G.have++;
	G.flow = 0;
}

SDL_GPUTexture *fg_make(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, float t) {
	(void)dev;
	if (G.have < 2 || G.failed) return 0;
	const int c = G.cur, p = c ^ 1;
	if (!G.flow) {   /* the pair's motion, coarsest level first */
		for (int k = G.nl - 1; k >= 0; k--) {
			int top = k == G.nl - 1;
			U u = { { (int)G.lw[k], (int)G.lh[k], (int)G.fw[k], (int)G.fh[k] }, { top ? 0 : (int)G.fw[k + 1], top ? 0 : (int)G.fh[k + 1], RADIUS, k == 0 },
			        { 1.0f / (float)G.lw[k], 1.0f / (float)G.lh[k], LAMBDA, ZERO_BIAS } };
			SDL_GPUTexture *in[3] = { G.lum[p][k], G.lum[c][k], top ? G.lum[c][k] : G.fld[k + 1] };   /* (the coarsest reads no coarser field: something bound) */
			run(cmd, P_SEARCH, in, G.fld[k], &u, G.fw[k], G.fh[k]);
		}
		U s = { { (int)G.fw[0], (int)G.fh[0] } };
		run(cmd, P_STAT, &G.fld[0], G.stat, &s, 16, 16);
		const int k = G.flev;
		U f = { { (int)G.lw[k], (int)G.lh[k], (int)G.fw[0], (int)G.fh[0] }, { 0, 1 << k, FRADIUS, 0 }, { 1.0f / (float)G.lw[k], 1.0f / (float)G.lh[k], FEXPLAINED, ZERO_BIAS } };
		SDL_GPUTexture *fin[3] = { G.lum[p][k], G.lum[c][k], G.fld[0] };
		run(cmd, P_FSEARCH, fin, G.ffld, &f, G.fw[0], G.fh[0]);
		G.flow = 1;
	}
	U pj = { { (int)G.fw[0], (int)G.fh[0] }, { PRADIUS }, { 0, 0, t } };
	SDL_GPUTexture *pin[2] = { G.ffld, G.stat };
	run(cmd, P_PROJECT, pin, G.proj, &pj, G.fw[0], G.fh[0]);
	U u = { { (int)G.lw[0], (int)G.lh[0], (int)G.fw[0], (int)G.fh[0] }, { 0 }, { 1.0f / (float)G.lw[0], 1.0f / (float)G.lh[0], t } };
	SDL_GPUTexture *in[5] = { G.lum[p][0], G.lum[c][0], G.fld[0], G.proj, G.stat };
	run(cmd, P_PICK, in, G.pix, &u, G.lw[0], G.lh[0]);
	U w = { { (int)G.w, (int)G.h, (int)G.lw[0], (int)G.lh[0] }, { G.scale }, { 1.0f / (float)G.w, 1.0f / (float)G.h, t, CUT } };
	SDL_GPUTexture *win[4] = { G.col[p], G.col[c], G.pix, G.stat };
	run(cmd, P_WARP, win, G.gen, &w, G.w, G.h);
	if (G.levels > 1) SDL_GenerateMipmapsForGPUTexture(cmd, G.gen);
	return G.gen;
}
