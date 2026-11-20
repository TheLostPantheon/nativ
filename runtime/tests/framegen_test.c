/*
 * framegen_test.c: frame generation (runtime/gfx/framegen) on a GPU without a window.
 *
 *   framegen_test                          a synthetic pair: a textured scene panned by (dx, dy) under a still HUD
 *                                          block; checks the frame made halfway against the scene panned by half,
 *                                          and the HUD against itself, and prints both errors beside a plain blend's
 *   framegen_test prev.png cur.png out.png [t]   the frame at t (default 0.5) between two real frames
 *   framegen_test --time w h [w h ...]     GPU time per game frame and per made frame at those frame sizes
 *
 * Build: runtime/tests/framegen_test.sh. Exit 0 when the synthetic checks pass.
 */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "framegen/framegen.h"

int settings_int(const char *key, int def) { (void)key; return def; }

static SDL_GPUDevice *dev;
static SDL_GPUTexture *upload(const uint8_t *px, uint32_t w, uint32_t h) {
	SDL_GPUTextureCreateInfo ci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
		.width = w, .height = h, .layer_count_or_depth = 1, .num_levels = 1 };
	SDL_GPUTexture *t = SDL_CreateGPUTexture(dev, &ci);
	SDL_GPUTransferBufferCreateInfo bi = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = 4 * w * h };
	SDL_GPUTransferBuffer *b = SDL_CreateGPUTransferBuffer(dev, &bi);
	memcpy(SDL_MapGPUTransferBuffer(dev, b, false), px, 4 * w * h); SDL_UnmapGPUTransferBuffer(dev, b);
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev); SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureTransferInfo s = { .transfer_buffer = b, .pixels_per_row = w, .rows_per_layer = h }; SDL_GPUTextureRegion d = { .texture = t, .w = w, .h = h, .d = 1 };
	SDL_UploadToGPUTexture(cp, &s, &d, false); SDL_EndGPUCopyPass(cp); SDL_SubmitGPUCommandBuffer(cmd);
	SDL_ReleaseGPUTransferBuffer(dev, b);
	return t;
}
static uint8_t *download(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *t, uint32_t w, uint32_t h, double *ms) {
	SDL_GPUTransferBufferCreateInfo bi = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = 4 * w * h };
	SDL_GPUTransferBuffer *b = SDL_CreateGPUTransferBuffer(dev, &bi);
	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureRegion s = { .texture = t, .w = w, .h = h, .d = 1 }; SDL_GPUTextureTransferInfo d = { .transfer_buffer = b, .pixels_per_row = w, .rows_per_layer = h };
	SDL_DownloadFromGPUTexture(cp, &s, &d); SDL_EndGPUCopyPass(cp);
	Uint64 t0 = SDL_GetTicksNS();
	SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd); SDL_WaitForGPUFences(dev, true, &f, 1); SDL_ReleaseGPUFence(dev, f);
	if (ms) *ms = (double)(SDL_GetTicksNS() - t0) / 1e6;
	uint8_t *out = malloc(4 * w * h); memcpy(out, SDL_MapGPUTransferBuffer(dev, b, false), 4 * w * h); SDL_UnmapGPUTransferBuffer(dev, b);
	SDL_ReleaseGPUTransferBuffer(dev, b);
	return out;
}
/* the frame at t between a and b (frames w x h, RGBA8) */
static uint8_t *generate(const uint8_t *a, const uint8_t *b, uint32_t w, uint32_t h, float t, double *ms) {
	SDL_GPUTexture *ta = upload(a, w, h), *tb = upload(b, w, h);
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
	fg_forget(); fg_frame(dev, cmd, ta, w, h, 1); fg_frame(dev, cmd, tb, w, h, 1);
	SDL_GPUTexture *g = fg_make(dev, cmd, t);
	if (!g) { fprintf(stderr, "no frame made\n"); exit(2); }
	uint8_t *out = download(cmd, g, w, h, ms);
	SDL_ReleaseGPUTexture(dev, ta); SDL_ReleaseGPUTexture(dev, tb);
	return out;
}
static int save(const char *path, const uint8_t *px, uint32_t w, uint32_t h) {
	SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)px, (int)(4 * w));
	int ok = s && SDL_SavePNG(s, path); SDL_DestroySurface(s); return ok;
}
static uint8_t *load(const char *path, uint32_t *w, uint32_t *h) {
	SDL_Surface *s = SDL_LoadPNG(path); if (!s) { fprintf(stderr, "%s: %s\n", path, SDL_GetError()); exit(2); }
	SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888); SDL_DestroySurface(s);
	*w = (uint32_t)c->w; *h = (uint32_t)c->h; uint8_t *px = malloc(4 * *w * *h);
	for (uint32_t y = 0; y < *h; y++) memcpy(px + 4 * y * *w, (uint8_t *)c->pixels + y * (uint32_t)c->pitch, 4 * *w);
	SDL_DestroySurface(c); return px;
}

/* the synthetic scene: smooth noise with edges, sampled at (x - ox, y - oy) */
static float scene(float x, float y) {
	float v = 0.5f + 0.25f * sinf(x * 0.031f + 1.3f * sinf(y * 0.017f)) + 0.15f * sinf(y * 0.053f - x * 0.011f);
	int cx = (int)floorf(x / 40.0f), cy = (int)floorf(y / 40.0f);
	if (((cx * 7 + cy * 13) & 3) == 0) v = 0.15f + 0.1f * sinf(x * 0.2f);   /* blocks with hard edges */
	return v < 0 ? 0 : v > 1 ? 1 : v;
}
static int in_hud(uint32_t x, uint32_t y) { return x >= 40 && x < 240 && y >= 30 && y < 80; }
static float hud(uint32_t x, uint32_t y) {   /* a still, high-contrast pattern like text: 2x3 cells, on or off at random (a pattern that repeats along the
                                               * motion matches the moving vector as well as zero: no picture tells those apart) */
	uint32_t h = (x / 2) * 73856093u ^ (y / 3) * 19349663u; h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
	return (h & 1) ? 0.95f : 0.05f;
}
static void paint(uint8_t *px, uint32_t w, uint32_t h, float ox, float oy) {
	for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
		float v = in_hud(x, y) ? hud(x, y) : scene((float)x - ox, (float)y - oy);
		uint8_t *p = px + 4 * (y * w + x); p[0] = (uint8_t)(v * 255); p[1] = (uint8_t)(v * 230); p[2] = (uint8_t)(v * 200); p[3] = 255;
	}
}
static double err(const uint8_t *a, const uint8_t *b, uint32_t w, uint32_t h, int which) {   /* mean |a-b| (0..255) over the scene (which 0, away from the edges) or the HUD (1) */
	double s = 0; long n = 0;
	for (uint32_t y = 24; y + 24 < h; y++) for (uint32_t x = 24; x + 24 < w; x++) {
		int hh = in_hud(x, y), near_hud = x >= 24 && x < 256 && y >= 14 && y < 96;
		if (which ? !hh : near_hud) continue;
		for (int c = 0; c < 3; c++) s += abs((int)a[4 * (y * w + x) + c] - (int)b[4 * (y * w + x) + c]);
		n += 3;
	}
	return n ? s / (double)n : 0;
}

/* GPU time at a frame size: a new frame with its pair's motion and one made frame (what a 30 fps game on 60 Hz costs per
 * game frame), and a made frame alone (each further one on a faster display), averaged over 30 rounds after a warm-up */
static double wait_ms(SDL_GPUCommandBuffer *cmd) {
	Uint64 t0 = SDL_GetTicksNS();
	SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd); SDL_WaitForGPUFences(dev, true, &f, 1); SDL_ReleaseGPUFence(dev, f);
	return (double)(SDL_GetTicksNS() - t0) / 1e6;
}
static void bench(uint32_t w, uint32_t h) {
	uint8_t *a = malloc(4 * w * h), *b = malloc(4 * w * h);
	for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
		float va = scene((float)x * 960 / (float)w, (float)y * 544 / (float)h), vb = scene((float)x * 960 / (float)w - 9, (float)y * 544 / (float)h - 4);
		for (int c = 0; c < 3; c++) { a[4 * (y * w + x) + c] = (uint8_t)(va * 255); b[4 * (y * w + x) + c] = (uint8_t)(vb * 255); }
		a[4 * (y * w + x) + 3] = b[4 * (y * w + x) + 3] = 255;
	}
	SDL_GPUTexture *ta = upload(a, w, h), *tb = upload(b, w, h);
	double pair = 0, made = 0; const int N = 30;
	for (int i = -3; i < N; i++) {
		SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
		fg_frame(dev, cmd, (i & 1) ? ta : tb, w, h, 1); fg_make(dev, cmd, 0.5f);
		double t = wait_ms(cmd); if (i >= 0) pair += t;
		cmd = SDL_AcquireGPUCommandBuffer(dev); fg_make(dev, cmd, 0.25f);
		t = wait_ms(cmd); if (i >= 0) made += t;
	}
	printf("%ux%u: a game frame (pyramid, motion, one made frame) %.2f ms; a further made frame %.2f ms\n", w, h, pair / N, made / N);
	SDL_ReleaseGPUTexture(dev, ta); SDL_ReleaseGPUTexture(dev, tb); free(a); free(b);
}

int main(int argc, char **argv) {
	if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 2; }
	dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, false, NULL);
	if (!dev) { fprintf(stderr, "no GPU device: %s\n", SDL_GetError()); return 2; }
	fprintf(stderr, "device: %s\n", SDL_GetGPUDeviceDriver(dev));
	if (argc >= 2 && !strcmp(argv[1], "--time")) { for (int i = 2; i + 1 < argc; i += 2) bench((uint32_t)atoi(argv[i]), (uint32_t)atoi(argv[i + 1])); return 0; }
	if (argc >= 4) {
		uint32_t w, h, w2, h2; uint8_t *a = load(argv[1], &w, &h), *b = load(argv[2], &w2, &h2);
		if (w != w2 || h != h2) { fprintf(stderr, "the frames differ in size\n"); return 2; }
		double ms; uint8_t *g = generate(a, b, w, h, argc > 4 ? (float)atof(argv[4]) : 0.5f, &ms);
		if (!save(argv[3], g, w, h)) { fprintf(stderr, "%s: %s\n", argv[3], SDL_GetError()); return 2; }
		printf("%ux%u -> %s (GPU %.2f ms, one submit with uploads)\n", w, h, argv[3], ms);
		return 0;
	}
	const uint32_t w = 960, h = 544; int fails = 0;
	const float pans[][2] = { { 0, 0 }, { 6, 0 }, { 18, -8 }, { -31, 12 }, { 64, 24 } };
	uint8_t *a = malloc(4 * w * h), *b = malloc(4 * w * h), *mid = malloc(4 * w * h), *blend = malloc(4 * w * h);
	for (size_t i = 0; i < sizeof pans / sizeof *pans; i++) {
		float dx = pans[i][0], dy = pans[i][1];
		paint(a, w, h, 0, 0); paint(b, w, h, dx, dy); paint(mid, w, h, dx / 2, dy / 2);
		for (uint32_t k = 0; k < 4 * w * h; k++) blend[k] = (uint8_t)((a[k] + b[k] + 1) / 2);
		double ms; uint8_t *g = generate(a, b, w, h, 0.5f, &ms);
		double es = err(g, mid, w, h, 0), eh = err(g, mid, w, h, 1), bs = err(blend, mid, w, h, 0);
		int ok = es < 6.0 && eh < 2.0;
		printf("pan (%+.0f, %+.0f): scene error %.2f (a plain blend: %.2f), HUD error %.2f, %.2f ms  %s\n", dx, dy, es, bs, eh, ms, ok ? "ok" : "FAIL");
		if (!ok) fails++;
		if (getenv("FG_TEST_OUT")) { char p[512]; snprintf(p, sizeof p, "%s/pan%zu_mid.png", getenv("FG_TEST_OUT"), i); save(p, g, w, h);
			snprintf(p, sizeof p, "%s/pan%zu_want.png", getenv("FG_TEST_OUT"), i); save(p, mid, w, h); }
		free(g);
	}
	printf(fails ? "%d failed\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}
