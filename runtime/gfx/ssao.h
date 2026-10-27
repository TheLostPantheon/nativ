/* ssao.h: screen-space ambient occlusion for a game's main 3D scene (ssao.c; gfx_gpu.c calls it) */
#ifndef SSAO_H
#define SSAO_H
#include <SDL3/SDL.h>

/* the camera, from the game's projection matrix (column-major P[col][row]) and its viewport's z mapping:
 * p00 = P[0][0], p11 = P[1][1], p22 = P[2][2], p32 = P[3][2] (with P[2][3] = -1: w = -z), depth = zoff + zscale * z/w */
typedef struct { float p00, p11, p22, p32, zoff, zscale; } SsaoCamera;

int ssao_enabled(void);   /* settings "ssao" (RECOMP_SSAO_ON wins) */
/* multiplies the occlusion into `color` (w x h, the scene's colour target, its texture format color_fmt), from `depth` (its
 * depth texture, the same size): its own command buffer, submitted -- after the scene's. 1: done */
int ssao_apply(SDL_GPUDevice *gpu, SDL_GPUTexture *color, SDL_GPUTextureFormat color_fmt, SDL_GPUTexture *depth, uint32_t w, uint32_t h, const SsaoCamera *cam);

#endif
