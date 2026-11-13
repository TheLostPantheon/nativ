/* fsr1.h: AMD FidelityFX FSR1 spatial upscaling for the renderer presentation path */
#pragma once
#include <SDL3/SDL_gpu.h>
#include <stdint.h>

typedef struct {
	SDL_GPUTexture *color;
	uint32_t input_w, input_h;
	uint32_t output_w, output_h;
	float sharpness;
	int srgb;
} Fsr1Frame;

/* Records EASU followed by RCAS into cmd and returns an RGBA8 output texture.
 * The returned texture remains owned by this module and is reused by the next call. */
SDL_GPUTexture *fsr1_dispatch(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, const Fsr1Frame *frame);
