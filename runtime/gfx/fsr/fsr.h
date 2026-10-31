/* fsr.h: AMD FSR 3.1's upscaler as Nativ's temporal anti-aliasing (fsr.c) */
#pragma once
#include <SDL3/SDL_gpu.h>
#include <stdint.h>

enum {
	FSR_REACTIVE_APPLY_TONEMAP = 1u,
	FSR_REACTIVE_APPLY_INVERSE_TONEMAP = 2u,
	FSR_REACTIVE_APPLY_THRESHOLD = 4u,
	FSR_REACTIVE_USE_COMPONENTS_MAX = 8u,
};

typedef struct {
	float velocity_factor;                    /* [0, 1], SDK default 1 */
	float reactiveness_scale;                 /* >= 0, SDK default 1 */
	float shading_change_scale;               /* >= 0, SDK default 1 */
	float accumulation_added_per_frame;       /* [0, 1], SDK default 1/3 */
	float min_disocclusion_accumulation;       /* [-1, 1], SDK default -1/3 */
} FsrTuning;

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
	/* The optional textures below are the caller's to get right -- SDL_GPU cannot report a texture's size or format, so fsr_dispatch
	 * does not check them. Each needs SAMPLER usage; the passes texelFetch the named channel, so any float or unorm format with
	 * the value there reads the same (the masks are R8_UNORM in the SDK's own host). */
	SDL_GPUTexture *exposure; /* optional 1x1 exposure in .x; 0 uses FSR's auto exposure */
	float pre_exposure;       /* the input colour's pre-exposure, greater than 0 (normally 1) */
	SDL_GPUTexture *reactive; /* optional reactive mask in .r, at least the render size; takes precedence over opaque-generated reactivity */
	SDL_GPUTexture *transparency; /* optional transparency/composition mask in .r, at least the render size */
	SDL_GPUTexture *opaque;   /* the colour before the scene's transparent draws (render size), or 0: the reactive mask is their difference */
	float reactive_scale, reactive_threshold, reactive_value;
	uint32_t reactive_flags; /* FSR_REACTIVE_* flags for opaque-generated reactivity */
	int camera_valid;        /* device_to_view_depth, tan_half_fov and view_space_to_meters are valid */
	float device_to_view_depth[4]; /* SDK depth conversion factors: z = y / (device depth - x), then x/y projection scales */
	float tan_half_fov;      /* horizontal half-FOV tangent, as fsr.c's fallback camera computes it (v1.1.4's shaders define TanHalfFoV() but never call it) */
	float view_space_to_meters; /* metres per view-space unit: a projection cannot supply it, so the caller says (gfx_gpu.c assumes 1) */
	const FsrTuning *tuning; /* optional SDK development constants; 0 uses its defaults */
	int debug_view;          /* replace the output with the SDK's diagnostic grid */
} FsrFrame;

uint32_t fsr_jitter_phase_count(uint32_t render_width, uint32_t display_width);
void fsr_jitter_offset(uint64_t frame, uint32_t phase_count, float *x, float *y);

/* records FSR's passes for f into cmd (the caller submits it). 0: invalid input or a recorded failure (cancel *cmd, which may
 * be 0 after a failed profile submit). 1: recorded -- and if the caller then cannot submit it, fsr_dispatch_lost */
int fsr_dispatch(SDL_GPUDevice *dev, SDL_GPUCommandBuffer **cmd, const FsrFrame *f);   /* (*cmd: a profile run, RECOMP_FSR_TIME=2, submits it after each pass and goes on in a new one) */
void fsr_dispatch_lost(SDL_GPUDevice *dev);   /* a dispatch that returned 1 was not submitted: its history, clears and Lanczos table never reached the GPU, so the next dispatch starts over */
const char *fsr_profile(void);   /* RECOMP_FSR_TIME=2: each pass's average GPU time so far, a line (then reset) */
/* RECOMP_FSR_DUMP: fsr_dispatch with keep set keeps the frame's input colour; once that command buffer is done,
 * fsr_dump(prefix) writes FSR's view of it: <prefix>_in.ppm, _out.ppm, _acc.ppm (history weight), _reactive.ppm (dilated
 * reactive/composition/disocclusion masks), _mv.ppm (dilated vectors), _depth.ppm (dilated depth), _instability.ppm */
void fsr_keep_input(int keep);
void fsr_dump(SDL_GPUDevice *dev, const char *prefix);
