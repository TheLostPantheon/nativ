/*
 * fsr1.c: AMD FidelityFX FSR1's spatial EASU upscaler followed by RCAS, using the
 * SDK shader in third_party/fidelityfx (MIT). This is presentation-only: it takes
 * the finished game frame, so the renderer backend can upscale before its one GPU
 * readback while the D3D11 overlay stays at output resolution.
 */
#include "fsr1.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../fsr1_shaders.h"
#ifdef RECOMP_SPIRV_CROSS
#include <spirv_cross/spirv_cross_c.h>
#endif

enum { P_EASU, P_RCAS, P_COUNT };
typedef struct {
	const char *name;
	const uint32_t *code;
	size_t size;
} Pass;
static const Pass passes[P_COUNT] = {
	{"EASU", fsr1_easu, sizeof fsr1_easu},
	{"RCAS", fsr1_rcas, sizeof fsr1_rcas},
};

#ifdef RECOMP_SPIRV_CROSS
static char *msl_of(const uint32_t *words, size_t nwords, const char *name) {
	spvc_context ctx;
	spvc_parsed_ir ir;
	spvc_compiler comp;
	spvc_compiler_options opts;
	const char *source;
	char *out = 0;
	if (spvc_context_create(&ctx) != SPVC_SUCCESS) return 0;
	if (spvc_context_parse_spirv(ctx, words, nwords, &ir) != SPVC_SUCCESS ||
	    spvc_context_create_compiler(ctx, SPVC_BACKEND_MSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &comp) != SPVC_SUCCESS) goto done;
	spvc_compiler_create_compiler_options(comp, &opts);
	spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_VERSION, SPVC_MAKE_MSL_VERSION(2, 3, 0));
	spvc_compiler_install_compiler_options(comp, opts);
	#define BIND(set, binding, texture, buffer) do { \
		spvc_msl_resource_binding_2 r; spvc_msl_resource_binding_init_2(&r); \
		r.stage = SpvExecutionModelGLCompute; r.desc_set = (set); r.binding = (binding); r.count = 1; \
		r.msl_texture = r.msl_sampler = (texture); r.msl_buffer = (buffer); \
		spvc_compiler_msl_add_resource_binding_2(comp, &r); \
	} while (0)
	BIND(0, 0, 0, 0);
	BIND(1, 0, 1, 0);
	BIND(2, 0, 0, 0);
	#undef BIND
	if (spvc_compiler_compile(comp, &source) == SPVC_SUCCESS) out = strdup(source);
done:
	if (!out) fprintf(stderr, "    fsr1: %s to MSL: %s\n", name, spvc_context_get_last_error_string(ctx));
	spvc_context_destroy(ctx);
	return out;
}
#endif

static SDL_GPUComputePipeline *pipelines[P_COUNT];
static int pipeline_tried[P_COUNT];
static SDL_GPUComputePipeline *pipeline(SDL_GPUDevice *dev, int pass) {
	if (pipeline_tried[pass]) return pipelines[pass];
	pipeline_tried[pass] = 1;
	const Pass *p = &passes[pass];
	SDL_GPUComputePipelineCreateInfo ci = {
		.num_samplers = 1,
		.num_readwrite_storage_textures = 1,
		.num_uniform_buffers = 1,
		.threadcount_x = 8,
		.threadcount_y = 8,
		.threadcount_z = 1,
	};
	char *msl = 0;
	if (SDL_GetGPUShaderFormats(dev) & SDL_GPU_SHADERFORMAT_SPIRV) {
		ci.code = (const Uint8 *)p->code;
		ci.code_size = p->size;
		ci.entrypoint = "main";
		ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
	}
#ifdef RECOMP_SPIRV_CROSS
	else if (SDL_GetGPUShaderFormats(dev) & SDL_GPU_SHADERFORMAT_MSL) {
		msl = msl_of(p->code, p->size / 4, p->name);
		if (!msl) return 0;
		ci.code = (const Uint8 *)msl;
		ci.code_size = strlen(msl) + 1;
		ci.entrypoint = "main0";
		ci.format = SDL_GPU_SHADERFORMAT_MSL;
	}
#endif
	else {
		fprintf(stderr, "    fsr1: no shader format this device accepts\n");
		return 0;
	}
	pipelines[pass] = SDL_CreateGPUComputePipeline(dev, &ci);
	if (!pipelines[pass]) fprintf(stderr, "    fsr1: %s pipeline: %s\n", p->name, SDL_GetError());
	free(msl);
	return pipelines[pass];
}

static struct {
	uint32_t w, h;
	SDL_GPUTexture *easu;
	SDL_GPUTexture *out;
	SDL_GPUSampler *sampler;
	int failed;
} state;

static void release_textures(SDL_GPUDevice *dev) {
	if (state.easu) SDL_ReleaseGPUTexture(dev, state.easu);
	if (state.out) SDL_ReleaseGPUTexture(dev, state.out);
	state.easu = state.out = 0;
	state.w = state.h = 0;
}

static int ensure_resources(SDL_GPUDevice *dev, uint32_t w, uint32_t h) {
	if (state.failed) return 0;
	if (!state.sampler) {
		SDL_GPUSamplerCreateInfo ci = {
			.min_filter = SDL_GPU_FILTER_LINEAR,
			.mag_filter = SDL_GPU_FILTER_LINEAR,
			.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
			.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
			.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
			.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
		};
		state.sampler = SDL_CreateGPUSampler(dev, &ci);
	}
	if (!state.sampler || !pipeline(dev, P_EASU) || !pipeline(dev, P_RCAS)) state.failed = 1;
	if (state.failed) {
		fprintf(stderr, "    fsr1: unavailable (%s)\n", SDL_GetError());
		return 0;
	}
	if (state.easu && state.w == w && state.h == h) return 1;
	release_textures(dev);
	const SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
	if (!SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, SDL_GPU_TEXTURETYPE_2D, usage) ||
	    !SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D, usage)) {
		fprintf(stderr, "    fsr1: required storage texture formats are unavailable\n");
		state.failed = 1;
		return 0;
	}
	SDL_GPUTextureCreateInfo ci = {
		.type = SDL_GPU_TEXTURETYPE_2D,
		.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
		.usage = usage,
		.width = w,
		.height = h,
		.layer_count_or_depth = 1,
		.num_levels = 1,
		.sample_count = SDL_GPU_SAMPLECOUNT_1,
	};
	state.easu = SDL_CreateGPUTexture(dev, &ci);
	ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	state.out = state.easu ? SDL_CreateGPUTexture(dev, &ci) : 0;
	if (!state.easu || !state.out) {
		fprintf(stderr, "    fsr1: cannot create %ux%u working textures (%s)\n", w, h, SDL_GetError());
		release_textures(dev);
		state.failed = 1;
		return 0;
	}
	state.w = w;
	state.h = h;
	return 1;
}

static uint32_t float_bits(float value) {
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	return bits;
}

static int dispatch(SDL_GPUCommandBuffer *cmd, int pass, SDL_GPUTexture *input, SDL_GPUTexture *output) {
	SDL_GPUStorageTextureReadWriteBinding image = {.texture = output};
	SDL_GPUComputePass *cp = SDL_BeginGPUComputePass(cmd, &image, 1, 0, 0);
	if (!cp) return 0;
	SDL_BindGPUComputePipeline(cp, pipelines[pass]);
	SDL_GPUTextureSamplerBinding source = {.texture = input, .sampler = state.sampler};
	SDL_BindGPUComputeSamplers(cp, 0, &source, 1);
	SDL_DispatchGPUCompute(cp, (state.w + 7) / 8, (state.h + 7) / 8, 1);
	SDL_EndGPUComputePass(cp);
	return 1;
}

SDL_GPUTexture *fsr1_dispatch(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, const Fsr1Frame *frame) {
	if (!dev || !cmd || !frame || !frame->color || !frame->input_w || !frame->input_h ||
	    !frame->output_w || !frame->output_h || frame->output_w > 16384 || frame->output_h > 16384) return 0;
	if (!ensure_resources(dev, frame->output_w, frame->output_h)) return 0;

	struct {
		uint32_t con[4][4];
		uint32_t output_size[2];
		uint32_t padding[2];
	} easu = {0};
	const float iw = (float)frame->input_w, ih = (float)frame->input_h;
	const float ow = (float)frame->output_w, oh = (float)frame->output_h;
	easu.con[0][0] = float_bits(iw / ow);
	easu.con[0][1] = float_bits(ih / oh);
	easu.con[0][2] = float_bits(0.5f * iw / ow - 0.5f);
	easu.con[0][3] = float_bits(0.5f * ih / oh - 0.5f);
	easu.con[1][0] = float_bits(1.0f / iw);
	easu.con[1][1] = float_bits(1.0f / ih);
	easu.con[1][2] = float_bits(1.0f / iw);
	easu.con[1][3] = float_bits(-1.0f / ih);
	easu.con[2][0] = float_bits(-1.0f / iw);
	easu.con[2][1] = float_bits(2.0f / ih);
	easu.con[2][2] = float_bits(1.0f / iw);
	easu.con[2][3] = float_bits(2.0f / ih);
	easu.con[3][0] = float_bits(0.0f);
	easu.con[3][1] = float_bits(4.0f / ih);
	easu.output_size[0] = frame->output_w;
	easu.output_size[1] = frame->output_h;
	SDL_PushGPUComputeUniformData(cmd, 0, &easu, sizeof easu);
	if (!dispatch(cmd, P_EASU, frame->color, state.easu)) {
		fprintf(stderr, "    fsr1: EASU dispatch: %s\n", SDL_GetError());
		state.failed = 1;
		return 0;
	}

	struct {
		uint32_t con[4];
		uint32_t output_size[2];
		uint32_t encode_srgb;
		uint32_t sharpen;
	} rcas = {0};
	float sharpness = frame->sharpness < 0 ? 0 : frame->sharpness > 1 ? 1 : frame->sharpness;
	const float linear_sharpness = exp2f(-(2.0f - 2.0f * sharpness));
	rcas.con[0] = float_bits(linear_sharpness);
	rcas.output_size[0] = frame->output_w;
	rcas.output_size[1] = frame->output_h;
	rcas.encode_srgb = frame->srgb != 0;
	rcas.sharpen = sharpness > 0;
	SDL_PushGPUComputeUniformData(cmd, 0, &rcas, sizeof rcas);
	if (!dispatch(cmd, P_RCAS, state.easu, state.out)) {
		fprintf(stderr, "    fsr1: RCAS dispatch: %s\n", SDL_GetError());
		state.failed = 1;
		return 0;
	}
	return state.out;
}
