/*
 * Synthetic Metal smoke scene: test-only SPIR-V -> MSL, shader creation on worker threads,
 * framebuffer fetch, half packing, a render pipeline, a locked fence, and readback. No window,
 * game, capture, or proprietary shader is involved. Build and run: see README.md.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include <spirv_cross/spirv_cross_c.h>
#include "../gfx/gpulock.h"
#include "metal_smoke_spirv.h"

static int checks, fails;
static void check(int ok, const char *what) {
	checks++;
	if (!ok) { fails++; fprintf(stderr, "FAIL: %s%s%s\n", what, *SDL_GetError() ? ": " : "", SDL_GetError()); }
}

static char *to_msl(const uint32_t *words, size_t nwords, int fragment, char error[256]) {
	spvc_context context = 0;
	spvc_parsed_ir ir;
	spvc_compiler compiler;
	spvc_compiler_options options;
	const char *source = 0;
	if (spvc_context_create(&context) != SPVC_SUCCESS ||
	    spvc_context_parse_spirv(context, words, nwords, &ir) != SPVC_SUCCESS ||
	    spvc_context_create_compiler(context, SPVC_BACKEND_MSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) != SPVC_SUCCESS ||
	    spvc_compiler_create_compiler_options(compiler, &options) != SPVC_SUCCESS) goto failed;
	spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_MSL_VERSION, SPVC_MAKE_MSL_VERSION(2, 3, 0));
	spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_MSL_FRAMEBUFFER_FETCH_SUBPASS, SPVC_TRUE);
	spvc_compiler_install_compiler_options(compiler, options);
	if (spvc_compiler_compile(compiler, &source) != SPVC_SUCCESS) goto failed;
	size_t n = strlen(source) + 1;
	char *copy = malloc(n);
	if (copy) memcpy(copy, source, n);
	else snprintf(error, 256, "out of memory");
	spvc_context_destroy(context);
	return copy;
failed:
	snprintf(error, 256, "%s SPIR-V to MSL: %s", fragment ? "fragment" : "vertex",
	         context ? spvc_context_get_last_error_string(context) : "context creation failed");
	if (context) spvc_context_destroy(context);
	return 0;
}

typedef struct {
	SDL_GPUDevice *device;
	const uint32_t *words;
	size_t nwords;
	SDL_GPUShaderStage stage;
	SDL_GPUShader *shader;
	char *msl;
	char error[256];
} ShaderJob;

static void *make_shader(void *arg) {
	ShaderJob *job = arg;
	job->msl = to_msl(job->words, job->nwords, job->stage == SDL_GPU_SHADERSTAGE_FRAGMENT, job->error);
	if (!job->msl) return 0;
	SDL_GPUShaderCreateInfo info = {0};
	info.code = (const Uint8 *)job->msl;
	info.code_size = strlen(job->msl) + 1;
	info.entrypoint = "main0";
	info.format = SDL_GPU_SHADERFORMAT_MSL;
	info.stage = job->stage;
	job->shader = SDL_CreateGPUShader(job->device, &info);
	if (!job->shader) snprintf(job->error, sizeof job->error, "%s", SDL_GetError());
	return 0;
}

static SDL_GPUGraphicsPipeline *pipeline(SDL_GPUDevice *device, SDL_GPUShader *vertex, SDL_GPUShader *fragment) {
	SDL_GPUColorTargetDescription target = { .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM };
	SDL_GPUGraphicsPipelineCreateInfo info = {0};
	info.vertex_shader = vertex;
	info.fragment_shader = fragment;
	info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
	info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
	info.target_info.color_target_descriptions = &target;
	info.target_info.num_color_targets = 1;
	return SDL_CreateGPUGraphicsPipeline(device, &info);
}

int main(void) {
	check(SDL_Init(SDL_INIT_VIDEO), "SDL video initialization");
	SDL_GPUDevice *device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, true, "metal");
	check(device != 0, "Metal device creation");
	if (!device) { SDL_Quit(); return 1; }
	check(!strcmp(SDL_GetGPUDeviceDriver(device), "metal"), "SDL selected Metal");
	fprintf(stderr, "Metal device: %s\n",
	        SDL_GetStringProperty(SDL_GetGPUDeviceProperties(device), SDL_PROP_GPU_DEVICE_NAME_STRING, "?"));

	ShaderJob jobs[] = {
		{
			.device = device,
			.words = metal_smoke_vert,
			.nwords = sizeof metal_smoke_vert / sizeof metal_smoke_vert[0],
			.stage = SDL_GPU_SHADERSTAGE_VERTEX,
		},
		{
			.device = device,
			.words = metal_base_frag,
			.nwords = sizeof metal_base_frag / sizeof metal_base_frag[0],
			.stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
		},
		{
			.device = device,
			.words = metal_fetch_frag,
			.nwords = sizeof metal_fetch_frag / sizeof metal_fetch_frag[0],
			.stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
		},
	};
	pthread_t threads[3];
	int started[3] = {0};
	for (int i = 0; i < 3; i++) {
		int error = pthread_create(&threads[i], 0, make_shader, &jobs[i]);
		started[i] = !error;
		if (error) snprintf(jobs[i].error, sizeof jobs[i].error, "start asynchronous shader build: %s", strerror(error));
		check(started[i], jobs[i].error[0] ? jobs[i].error : "start asynchronous shader build");
	}
	for (int i = 0; i < 3; i++) {
		if (!started[i]) continue;
		int error = pthread_join(threads[i], 0);
		if (error) {
			fprintf(stderr, "FAIL: join asynchronous shader build: %s\n", strerror(error));
			return 1;
		}
	}
	for (int i = 0; i < 3; i++) {
		check(jobs[i].msl != 0, jobs[i].error[0] ? jobs[i].error : "SPIR-V translated to MSL");
		check(jobs[i].shader != 0, jobs[i].error[0] ? jobs[i].error : "MSL shader created");
	}
	check(jobs[2].msl && strstr(jobs[2].msl, "[[color(0)]]"), "input attachment became Metal framebuffer fetch");

	SDL_GPUGraphicsPipeline *base = jobs[0].shader && jobs[1].shader ? pipeline(device, jobs[0].shader, jobs[1].shader) : 0;
	SDL_GPUGraphicsPipeline *fetch = jobs[0].shader && jobs[2].shader ? pipeline(device, jobs[0].shader, jobs[2].shader) : 0;
	check(base != 0, "base render pipeline");
	check(fetch != 0, "framebuffer-fetch render pipeline");
	enum { WIDTH = 8, HEIGHT = 8, BYTES = WIDTH * HEIGHT * 4 };
	SDL_GPUTextureCreateInfo texture_info = {
		.type = SDL_GPU_TEXTURETYPE_2D,
		.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
		.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
		.width = WIDTH, .height = HEIGHT, .layer_count_or_depth = 1,
		.num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1,
	};
	SDL_GPUTexture *texture = SDL_CreateGPUTexture(device, &texture_info);
	SDL_GPUTransferBufferCreateInfo transfer_info = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = BYTES };
	SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
	check(texture != 0, "offscreen color target");
	check(transfer != 0, "download buffer");

	SDL_GPUCommandBuffer *command = texture && transfer && base && fetch ? SDL_AcquireGPUCommandBuffer(device) : 0;
	check(command != 0, "command buffer");
	SDL_GPUFence *fence = 0;
	if (command) {
		SDL_GPUColorTargetInfo color = {
			.texture = texture, .clear_color = {0, 0, 0, 1},
			.load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE,
		};
		SDL_GPURenderPass *render = SDL_BeginGPURenderPass(command, &color, 1, 0);
		check(render != 0, "render pass");
		if (render) {
			SDL_BindGPUGraphicsPipeline(render, base);
			SDL_DrawGPUPrimitives(render, 3, 1, 0, 0);
			SDL_BindGPUGraphicsPipeline(render, fetch);
			SDL_DrawGPUPrimitives(render, 3, 1, 0, 0);
			SDL_EndGPURenderPass(render);
			SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);
			check(copy != 0, "copy pass");
			if (copy) {
				SDL_GPUTextureRegion source = { .texture = texture, .w = WIDTH, .h = HEIGHT, .d = 1 };
				SDL_GPUTextureTransferInfo destination = { .transfer_buffer = transfer, .pixels_per_row = WIDTH, .rows_per_layer = HEIGHT };
				SDL_DownloadFromGPUTexture(copy, &source, &destination);
				SDL_EndGPUCopyPass(copy);
				fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
				command = 0;
			}
		}
		if (command) check(SDL_CancelGPUCommandBuffer(command), "command buffer cancellation");
	}
	check(fence != 0, "locked command submission returned a fence");
	check(fence && SDL_WaitForGPUFences(device, true, &fence, 1), "locked fence wait");
	if (fence) SDL_ReleaseGPUFence(device, fence);

	const uint8_t *pixels = transfer ? SDL_MapGPUTransferBuffer(device, transfer, false) : 0;
	check(pixels != 0, "download mapping");
	const uint8_t expected[] = {96, 128, 191, 255};
	int different = 0;
	if (pixels) for (int i = 0; i < WIDTH * HEIGHT; i++) for (int c = 0; c < 4; c++)
		if (abs((int)pixels[4 * i + c] - expected[c]) > 1) different++;
	check(!different, "all rendered pixels match half conversion plus framebuffer fetch");
	if (pixels) {
		fprintf(stderr, "pixel: %u %u %u %u; mismatched channels: %d\n", pixels[0], pixels[1], pixels[2], pixels[3], different);
		SDL_UnmapGPUTransferBuffer(device, transfer);
	}

	if (transfer) SDL_ReleaseGPUTransferBuffer(device, transfer);
	if (texture) SDL_ReleaseGPUTexture(device, texture);
	if (fetch) SDL_ReleaseGPUGraphicsPipeline(device, fetch);
	if (base) SDL_ReleaseGPUGraphicsPipeline(device, base);
	for (int i = 0; i < 3; i++) {
		if (jobs[i].shader) SDL_ReleaseGPUShader(device, jobs[i].shader);
		free(jobs[i].msl);
	}
	SDL_DestroyGPUDevice(device);
	SDL_Quit();
	fprintf(stderr, "%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
