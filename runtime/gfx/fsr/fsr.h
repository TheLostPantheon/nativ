/* fsr.h: AMD FSR 3.1's upscaler as Nativ's temporal anti-aliasing (fsr.c) */
#pragma once
#include <SDL3/SDL_gpu.h>
#include <stdint.h>

typedef struct {
	SDL_GPUTexture *color;    /* the main scene's colour, drawn at this frame's jitter */
	SDL_GPUTexture *depth;    /* its depth buffer (sampled: the depth in .r) */
	SDL_GPUTexture *motion;   /* RG16F vectors, (current - previous) / 2 in clip units, y up (gfx_gpu.c's motion pass) */
	uint32_t rw, rh;          /* the render size: colour, depth and motion */
	SDL_GPUTexture *dest;     /* where the frame FSR makes goes (a blit: any colour format) */
	uint32_t dw, dh;          /* its size, the display size: rw, rh for anti-aliasing alone, larger to upscale too */
	float jitter_x, jitter_y; /* the offset the frame was drawn at, in pixels, x right and y down */
	float frame_ms;           /* since the last frame */
	int hdr;                  /* the colour is a float target: FSR's HDR path */
	int inverted;             /* the scene's depth test is GREATER (near = 1) */
	int reset;                /* no history to blend: a cut, a first frame */
	float sharpness;          /* RCAS after it, 0 (soft) to 1 (sharpest); below 0 none */
	SDL_GPUTexture *opaque;   /* the colour before the scene's transparent draws (render size), or 0: the reactive mask is their difference */
	float reactive_threshold, reactive_value;   /* a pixel the transparent draws changed by more than the threshold gets this reactivity (0-1) */
} FsrFrame;

/* records FSR's passes for f into cmd (the caller submits it). 0: not run (a pipeline failed: said once) */
int fsr_dispatch(SDL_GPUDevice *dev, SDL_GPUCommandBuffer **cmd, const FsrFrame *f);   /* (*cmd: a profile run, RECOMP_FSR_TIME=2, submits it after each pass and goes on in a new one) */
const char *fsr_profile(void);   /* RECOMP_FSR_TIME=2: each pass's average GPU time so far, a line (then reset) */
/* RECOMP_FSR_DUMP: fsr_dispatch with keep set keeps the frame's input colour; once that command buffer is done,
 * fsr_dump(prefix) writes FSR's view of it: <prefix>_in.ppm, _out.ppm, _acc.ppm (history weight), _reactive.ppm (dilated
 * reactive/composition/disocclusion masks), _mv.ppm (dilated vectors), _depth.ppm (dilated depth), _instability.ppm */
void fsr_keep_input(int keep);
void fsr_dump(SDL_GPUDevice *dev, const char *prefix);
