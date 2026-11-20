/*
 * framegen_test.c: frame generation (runtime/gfx/framegen) on a GPU without a window.
 *
 *   framegen_test                          a synthetic pair: a textured scene panned by (dx, dy) under a still HUD
 *                                          block; checks the frame made halfway against the scene panned by half,
 *                                          and the HUD against itself, and prints both errors beside a plain blend's;
 *                                          then --objects' checks
 *   framegen_test prev.png cur.png out.png [t]   the frame at t (default 0.5) between two real frames
 *   framegen_test --objects                a dark object over a bright background at 2880x1632, each moving its own way:
 *                                          the error around the object's edges and how much of it is a light outline
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

/* objects in front of a background at a frame size whose working size is a quarter of it (2880x1632, render scale 3):
 * a bright, softly textured background (a sky, a lit wall) and a dark object on it, each moving its own way over the
 * pair. Checked at t = 1/4 and 1/2 against the scene drawn at t: the error in a band around the object's edges (where a
 * wrong vector shows the background's colour around it, or a ghost of it) and how much of that band came out much
 * brighter than it should (a light outline). A thin object (narrower than its motion over the pair) is lost from the made
 * frames, the background shown in its place: the search sees the background in its blocks, and where two motions both fit
 * a pixel the still one is taken (what keeps a HUD sharp). Those two are printed, not checked. */
typedef struct { const char *name; float bx, by, ox, oy; int thin; } ObjCase;   /* background's motion, object's, a thin one */
enum { OW = 2880, OH = 1632 };
static float sky(float x, float y) { return 0.78f + 0.3f * (scene(x / 3, y / 3) - 0.5f); }
static int object(float x, float y, int thin) {
	float u = x - 1440, v = y - 816;
	if (thin) return fabsf(u) < 7 && v > -500 && v < 500;   /* a post 14 pixels wide */
	if (u * u + v * v < 260 * 260) return 1;   /* a disc and a slanted bar from it, like a gun */
	float a = u * 0.94f + v * 0.34f, b = -u * 0.34f + v * 0.94f;
	return a > 0 && a < 900 && fabsf(b) < 45;
}
static void paint_obj(uint8_t *px, uint8_t *mask, const ObjCase *c, float t) {
	for (uint32_t y = 0; y < OH; y++) for (uint32_t x = 0; x < OW; x++) {
		float X = (float)x - c->ox * t, Y = (float)y - c->oy * t; int in = object(X, Y, c->thin);
		float v = in ? 0.10f + 0.12f * scene(X / 2 + 300, Y / 2) : sky((float)x - c->bx * t, (float)y - c->by * t);
		uint8_t *p = px + 4 * (y * OW + x); p[0] = (uint8_t)(v * 255); p[1] = (uint8_t)(v * 235); p[2] = (uint8_t)(v * 215); p[3] = 255;
		if (mask) mask[y * OW + x] = (uint8_t)in;
	}
}
static int objects(void) {
	int fails = 0;
	static const ObjCase cs[] = {
		{ "still object, background panning", 36, 8, 0, 0, 0 },
		{ "still object, fast pan", 120, -30, 0, 0, 0 },
		{ "object moving, background still", 0, 0, 60, 12, 0 },
		{ "object and background opposite", 40, 0, -50, 0, 0 },
		{ "thin post, fast", 0, 0, 72, 0, 1 },
		{ "thin post, parallax", 24, 0, 72, 0, 1 },
	};
	uint8_t *a = malloc(4 * OW * OH), *b = malloc(4 * OW * OH), *want = malloc(4 * OW * OH), *mask = malloc(OW * OH);
	for (size_t i = 0; i < sizeof cs / sizeof *cs; i++) for (int k = 1; k <= 2; k++) {
		float t = 0.25f * (float)k;
		paint_obj(a, 0, &cs[i], 0); paint_obj(b, 0, &cs[i], 1); paint_obj(want, mask, &cs[i], t);
		double ms; uint8_t *g = generate(a, b, OW, OH, t, &ms);
		double band = 0, rest = 0; long nb = 0, nr = 0, light = 0;
		for (uint32_t y = 24; y + 24 < OH; y++) for (uint32_t x = 24; x + 24 < OW; x++) {
			uint8_t m = mask[y * OW + x]; int edge = 0;
			for (int dy = -16; dy <= 16 && !edge; dy += 8) for (int dx = -16; dx <= 16 && !edge; dx += 8) edge = mask[(y + dy) * OW + x + dx] != m;
			const uint8_t *p = g + 4 * (y * OW + x), *q = want + 4 * (y * OW + x);
			double e = (abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2])) / 3.0;
			if (edge || (cs[i].thin && m)) { band += e; nb++; if (p[0] + p[1] + p[2] > q[0] + q[1] + q[2] + 3 * 64) light++; } else { rest += e; nr++; }
		}
		double eb = band / (double)nb, lp = 100.0 * (double)light / (double)nb; int ok = cs[i].thin || (eb < 6.0 && lp < 1.0 && rest / (double)nr < 2.0);
		printf("%-34s t %.2f: edge band error %5.2f, %5.2f%% of it light, elsewhere %.2f, %.2f ms  %s\n", cs[i].name, t, eb, lp, rest / (double)nr, ms, cs[i].thin ? "(not checked)" : ok ? "ok" : "FAIL");
		if (!ok) fails++;
		if (getenv("FG_TEST_OUT")) { char p[512]; snprintf(p, sizeof p, "%s/obj%zu_%d_made.png", getenv("FG_TEST_OUT"), i, k); save(p, g, OW, OH);
			snprintf(p, sizeof p, "%s/obj%zu_%d_want.png", getenv("FG_TEST_OUT"), i, k); save(p, want, OW, OH); }
		free(g);
	}
	free(a); free(b); free(want); free(mask);
	return fails;
}

int main(int argc, char **argv) {
	if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 2; }
	dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, false, NULL);
	if (!dev) { fprintf(stderr, "no GPU device: %s\n", SDL_GetError()); return 2; }
	fprintf(stderr, "device: %s\n", SDL_GetGPUDeviceDriver(dev));
	if (argc >= 2 && !strcmp(argv[1], "--objects")) { int f = objects(); printf(f ? "%d failed\n" : "all passed\n", f); return f ? 1 : 0; }
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
	fails += objects();
	printf(fails ? "%d failed\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}
