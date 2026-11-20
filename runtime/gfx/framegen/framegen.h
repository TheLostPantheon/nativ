/* framegen.h: frame generation on the finished frame (framegen.c), for window.c's present loop */
#pragma once
#include <SDL3/SDL.h>

int fg_wanted(void);   /* the setting: settings "frame_gen" 0 | 1 (default 0; RECOMP_FRAME_GEN wins) */
/* a new game frame (level 0 of src, w x h, RGBA8): kept, with its luma pyramid; the pair it makes with the one before
 * is what fg_make blends. levels: the mip levels the generated frames get (as the present loop's own frame has) */
void fg_frame(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, uint32_t w, uint32_t h, uint32_t levels);
/* the frame at time t (0 the previous game frame, 1 the newest) between the last two, or 0 when there aren't two yet
 * (or it can't be made on this device); valid until the next call */
SDL_GPUTexture *fg_make(SDL_GPUDevice *dev, SDL_GPUCommandBuffer *cmd, float t);
void fg_forget(void);   /* drop the frames held (a pause, a jump): the next pair starts afresh */
