/*
 * One host mutex around SDL_GPU's submit and fence calls, and a polling wait in place of the
 * blocking one. The Metal backend of the SDL3 the host links (Homebrew's 3.4.16) races against
 * itself: METAL_ReleaseFence nils fence->commandBuffer and releases the MTLCommandBuffer at the
 * FIRST release of any reference to the fence, outside the renderer's submitLock and before its
 * refcount decrement; METAL_Submit's cleanup loop (under submitLock) reads
 * submittedCommandBuffers[i]->fence->commandBuffer.status for every command buffer in flight,
 * other threads' included, and SDL_QueryGPUFence / SDL_WaitForGPUFences read that same pointer
 * for the caller's own fence, also unlocked. One thread's release between another thread's load of
 * the pointer and its objc_msgSend freed the command buffer under it: SIGSEGV in objc_msgSend <-
 * METAL_Submit, twice in one evening of Uncharted (2026-10-08 23:49 on the display thread,
 * 2026-10-09 00:53 on the window thread; each time the other thread of the pair was waiting for
 * submitLock at the top of METAL_Submit). Upstream fixed it after 3.4.16 (libsdl-org/SDL
 * d8451e52, "metal: Make the command buffer hold a reference to its fence", 2026-09-02); until
 * that SDL ships, every host call that reads or writes a fence's command buffer -- submit (its
 * cleanup loop reads every fence and, through METAL_INTERNAL_CleanCommandBuffer, writes the
 * completed ones), release, query -- runs under this one lock, and a wait is a poll of locked
 * queries, never SDL's blocking wait made with the pointer in hand. The lock is a leaf: nothing
 * runs under it but the one SDL call, so it orders after every other host lock (gfx_dispatch.c's
 * renderer lock, window.c's frame lock) and can't deadlock with them. The contention is what
 * SDL's own submitLock already imposed; a poll adds up to ~0.15 ms to a wait.
 *
 * The window thread's swapchain acquire (SDL_WaitAndAcquireGPUSwapchainTexture) waits on, queries
 * and releases SDL's own in-flight fence inside SDL, unlocked, and then blocks in nextDrawable for
 * vsync -- so it can't simply run under this lock (every other thread's submit would wait out the
 * vsync with it). window.c paces itself first instead: a locked poll of its own fence for the frame
 * two submits back, then gpu_sweep -- a locked empty submit, whose cleanup loop retires every
 * completed command buffer, SDL's in-flight one included (it is that frame or an older one; a
 * queue completes in submission order). The acquire then finds a fence no submitted command buffer
 * references any more (and whose command buffer pointer the retire already cleared, in 3.4.16),
 * so its unlocked reads and release race with nothing.
 *
 * Include after <SDL3/SDL.h> (or anything that includes it): the macros at the end rename the SDL
 * calls to the locked versions, so no call site needs to know. gpulock.c defines GPULOCK_IMPL
 * first and keeps the real names.
 */
#pragma once
#include <SDL3/SDL.h>
#ifdef __cplusplus
extern "C" {
#endif
void gpu_lock(void);
void gpu_unlock(void);
bool gpu_submit(SDL_GPUCommandBuffer *cmd);
SDL_GPUFence *gpu_submit_fence(SDL_GPUCommandBuffer *cmd);
void gpu_release_fence(SDL_GPUDevice *dev, SDL_GPUFence *f);
bool gpu_query_fence(SDL_GPUDevice *dev, SDL_GPUFence *f);
bool gpu_wait_fences(SDL_GPUDevice *dev, bool wait_all, SDL_GPUFence *const *fences, Uint32 n);
void gpu_sweep(SDL_GPUDevice *dev);
#ifdef __cplusplus
}
#endif
#ifndef GPULOCK_IMPL
#define SDL_SubmitGPUCommandBuffer gpu_submit
#define SDL_SubmitGPUCommandBufferAndAcquireFence gpu_submit_fence
#define SDL_ReleaseGPUFence gpu_release_fence
#define SDL_QueryGPUFence gpu_query_fence
#define SDL_WaitForGPUFences gpu_wait_fences
/* a blocking wait over SDL's whole submitted list, read without its lock: not with this SDL (and never
 * wanted -- gfx_gpu.c's ring fences explain the freeze it caused); the name fails to link on purpose */
#define SDL_WaitForGPUIdle gpu_wait_idle_is_unsafe_see_gpulock_h
#endif
