/* The locked SDL_GPU submit/fence calls: see gpulock.h for why they exist. */
#define GPULOCK_IMPL
#include "gpulock.h"
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

static pthread_mutex_t gpu_m = PTHREAD_MUTEX_INITIALIZER;
void gpu_lock(void) { pthread_mutex_lock(&gpu_m); }
void gpu_unlock(void) { pthread_mutex_unlock(&gpu_m); }
bool gpu_submit(SDL_GPUCommandBuffer *cmd) { gpu_lock(); bool r = SDL_SubmitGPUCommandBuffer(cmd); gpu_unlock(); return r; }
SDL_GPUFence *gpu_submit_fence(SDL_GPUCommandBuffer *cmd) { gpu_lock(); SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd); gpu_unlock(); return f; }
void gpu_release_fence(SDL_GPUDevice *dev, SDL_GPUFence *f) { if (!f) return; gpu_lock(); SDL_ReleaseGPUFence(dev, f); gpu_unlock(); }
bool gpu_query_fence(SDL_GPUDevice *dev, SDL_GPUFence *f) { gpu_lock(); bool r = SDL_QueryGPUFence(dev, f); gpu_unlock(); return r; }
/* SDL_WaitForGPUFences's contract (all of them, or any one) as a poll of locked queries: a few yields
 * for a wait that is about to end, then 100 us sleeps (macOS's timer leeway makes those ~150 us; a
 * 10 ms readback wait is ~70 wakes). A null fence counts as signaled, as the callers' checks assume. */
bool gpu_wait_fences(SDL_GPUDevice *dev, bool wait_all, SDL_GPUFence *const *fences, Uint32 n) {
	for (int k = 0;; k++) {
		int done = wait_all;
		gpu_lock();
		for (Uint32 i = 0; i < n; i++) { int q = !fences[i] || SDL_QueryGPUFence(dev, fences[i]); done = wait_all ? done && q : done || q; }
		gpu_unlock();
		if (done || !n) return true;
		if (k < 16) sched_yield(); else usleep(100);
	}
}
/* a locked empty submit: its cleanup loop retires every command buffer the GPU has finished, so a
 * fence another thread is about to look at or release outside this lock (SDL's own swapchain in-flight
 * fence: window.c's present loop) is referenced by nothing in SDL's submitted list any more */
void gpu_sweep(SDL_GPUDevice *dev) {
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
	if (cmd) gpu_submit(cmd);
}
