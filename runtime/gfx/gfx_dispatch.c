/*
 * Picks the rendering backend: the SDL_GPU/SPIR-V backend (gfx_gpu.c) by default (since
 * 2026-10-06, when every test frame of the three games matched the interpreter on it; without a GPU
 * device it renders on the software path and says so); RECOMP_GFX=soft is the software backend
 * (gfx_soft.c), the reference the GPU backend is checked against -- the gate's tests set it, so
 * the references stay the interpreter's (see gfx_gpu.c's RECOMP_GFX_CHECK, the README's probes).
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rt.h"
#include "gfx.h"
#include "gfx_coverage.h"
#include "gfx_capture.h"
#include "metrics.h"
#include "platform.h"

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }   /* metrics.c: each entry point's own wall-clock cost */

/* one renderer lock around every entry point: the game's render thread records draws while Sony's
 * display-queue thread presents (its flip callback lands in sceDisplaySetFrameBuf), and since the
 * GPU backend batches a scene's draws into one command buffer (flushed at present, among other
 * points) the two threads were corrupting that batch -- three crashes in METAL_UploadToBuffer in
 * one play session. Recursive: a capture record inside gfx_draw syncs the GPU targets through
 * gfx_sync_to_guest. Uncontended it costs a few ns a draw. */
static pthread_mutex_t gfx_lock; static pthread_once_t gfx_lock_once = PTHREAD_ONCE_INIT;
static void gfx_lock_init(void) { pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&gfx_lock, &a); }
__thread int gfx_host_depth;   /* inside the lock below: guest memory this thread touches now is the graphics code's own access (gfx_gpu.c's read watch) */
int waitstats_on(void); uint64_t waitstats_now(void); void waitstats_begin(uint64_t t0); void waitstats_lock(uint64_t t0); void waitstats_held(uint64_t t0);
static __thread uint64_t gfx_hold_t0; static __thread int gfx_hold_depth;   /* (RECOMP_WAIT_STATS: this thread's outermost hold of the lock) */   /* (debug/waitstats.c: RECOMP_WAIT_STATS, the time threads queue for this lock) */
static void gfx_lock_take(void) {
	pthread_once(&gfx_lock_once, gfx_lock_init);
	if (!waitstats_on()) { pthread_mutex_lock(&gfx_lock); return; }
	if (pthread_mutex_trylock(&gfx_lock)) { const uint64_t t0 = waitstats_now(); waitstats_begin(t0); pthread_mutex_lock(&gfx_lock); waitstats_lock(t0); }
	if (!gfx_hold_depth++) gfx_hold_t0 = waitstats_now();
}
static void gfx_lock_give(void) { if (waitstats_on() && !--gfx_hold_depth) waitstats_held(gfx_hold_t0); pthread_mutex_unlock(&gfx_lock); }
#define LOCKED(...) do { gfx_lock_take(); gfx_host_depth++; __VA_ARGS__; gfx_host_depth--; gfx_lock_give(); } while (0)
/* the same lock for the window thread's own SDL_GPU calls (window.c's present loop, RECOMP_WINDOW_LOCK) */
void gfx_lock_enter(void) { gfx_lock_take(); }
void gfx_lock_leave(void) { gfx_lock_give(); }

static int gpu_backend(void) {
	static int b = -1;
	if (b < 0) { const char *e = getenv("RECOMP_GFX"); b = (e && !strcmp(e, "soft")) ? 0 : 1; metrics_set_backend(b); }
	return b;
}

/* RECOMP_GFX_READ_CHECK=1: does the renderer read a draw's inputs when the GPU would? The host
 * reads a draw's vertex streams, indices and default uniforms at sceGxmDraw; the console's GPU reads
 * them after sceGxmEndScene kicks the scene, so a game may legally finish writing a buffer between
 * the draw call and the end of the scene (a particle system filling its vertices on another thread,
 * synchronised before EndScene). Every draw's input ranges are hashed at the draw and again at
 * EndScene; a draw whose memory changed in between is reported -- the host drew data the console
 * never saw. */
typedef struct { uint32_t addr, len; uint64_t h; int draw, what; } ReadRange;
static ReadRange rc_r[8192]; static int rc_n, rc_draw, rc_on = -1; static uint64_t rc_scene, rc_reported;
static uint64_t rc_hash(uint32_t a, uint32_t n) { uint64_t h = 1469598103934665603ull; const uint8_t *p = G(a); for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull; return h; }
static void rc_add(uint32_t a, uint32_t n, int what) {
	if (!a || !n || n > (8u << 20) || rc_n >= 8192) return;
	ReadRange *r = &rc_r[rc_n++]; r->addr = a; r->len = n; r->h = rc_hash(a, n); r->draw = rc_draw; r->what = what;
}
static void rc_on_draw(const GfxDraw *d) {
	if (rc_on < 0) rc_on = getenv("RECOMP_GFX_READ_CHECK") != 0;
	if (!rc_on) return;
	uint32_t nv = d->count, isz = d->index_format ? 4 : 2;
	if (d->indices && d->count) {   /* indexed: the vertices it reaches */
		uint32_t mx = 0;
		for (uint32_t i = 0; i < d->count; i++) { uint32_t v = d->index_format ? RD32(d->indices + 4 * i) : RD16(d->indices + 2 * i); if (v > mx && v < 0x100000) mx = v; }
		nv = mx + 1;
		rc_add(d->indices, d->count * isz, 16);
	}
	for (int s = 0; s < d->nstream && s < 16; s++) if (d->stream[s]) rc_add(d->stream[s], nv * (d->stride[s] ? d->stride[s] : 16), s);
	/* (default uniform buffers are left out: their size isn't in the draw, and the next draw's
	 * reservation follows in the same ring, so a fixed window reports every draw) */
	rc_draw++;
}
static void rc_on_end_scene(void) {
	if (rc_on <= 0) return;
	static const char *what[] = { "stream", "indices", "vertex uniforms", "fragment uniforms" };
	int bad = 0;
	for (int i = 0; i < rc_n; i++) if (rc_hash(rc_r[i].addr, rc_r[i].len) != rc_r[i].h) {
		bad++;
		if (rc_reported < 400) {
			rc_reported++;
			int w = rc_r[i].what; const char *k = w < 16 ? what[0] : what[w - 15];
			fprintf(stderr, "gfx read check: scene %llu draw %d: %s%s%.0d %#x+%#x changed between the draw and EndScene\n", (unsigned long long)rc_scene,
			        rc_r[i].draw, k, w < 16 ? " " : "", w < 16 ? w : 0, rc_r[i].addr, rc_r[i].len);
		}
	}
	if (bad && rc_reported >= 400 && rc_reported++ == 400) fprintf(stderr, "gfx read check: (further reports suppressed)\n");
	rc_n = 0; rc_draw = 0; rc_scene++;
}
/* the scene's target, for gfx_sample_space: captures record the game's own draw state (viewport and
 * region clip in render-target pixels); the backends get each draw in the surface's pixels */
static GfxTarget scene_target;
static const GfxDraw *backend_draw(const GfxDraw *d, GfxDraw *tmp) {
	*tmp = *d;
	return gfx_sample_space(&scene_target, tmp) ? tmp : d;
}
/* a depth-only scene (no colour surface, a depth/stencil surface bound: Unit 13's cascaded shadow
 * atlas) carries no colour size, and the backends sized everything -- the depth buffer included --
 * from the colour surface: such a scene drew nothing and its shadow map stayed empty, so nothing
 * beyond the character's own shadow was ever shadowed. Its size is the render target's, in
 * samples, rows as the depth surface declares them; an old capture (no render target size)
 * takes the depth stride for both, its rows being whole */
static const GfxTarget *scene_dims(const GfxTarget *t, GfxTarget *tmp) {
	if (t->data || !t->depth || !t->ds_data || t->width) return t;
	*tmp = *t;
	uint32_t sx = t->ms_x && !t->scale_mode ? t->ms_x : 1, sy = t->ms_y && !t->scale_mode ? t->ms_y : 1;
	tmp->width = t->rt_width ? t->rt_width * sx : t->ds_stride;
	tmp->height = t->rt_height ? t->rt_height * sy : t->ds_stride;
	tmp->stride = t->ds_stride ? t->ds_stride : tmp->width;
	return tmp;
}
/* every entry point below times its own LOCKED section (metrics_add_render: metrics.c's "renderer
 * time per frame", harvested once per presented game frame) -- uncontended that's the real cost of
 * being here, lock wait included, same as the comment above says about the mutex itself. Only when
 * something reads the numbers (metrics_on: a window, RECOMP_METRICS, RECOMP_OSD) -- headless runs
 * don't pay two clock reads a draw */
/* a scene with no depth/stencil surface still depth-tests: the PowerVR keeps depth and stencil in tile
 * memory whichever way, and the surface only says whether that is loaded from / stored to memory (Vita3K
 * attaches a transient depth buffer the same way). So such a scene gets a depth/stencil buffer of its own
 * (keyed by its colour address, both backends' depth tables' fallback), cleared to far and never stored.
 * (Found on Killzone: Mercenary's world pass, which came without a surface and composited its 334
 * depth-tested draws in submission order -- but that surface was missing only because of the host's
 * FindMemBlockByAddr bug, 0ab6883; no game seen so far begins a depth-tested scene without one.)
 * RECOMP_NO_TRANSIENT_DEPTH=1: as before. */
static const GfxTarget *transient_depth(const GfxTarget *t, GfxTarget *out) {
	static int off = -1; if (off < 0) off = getenv("RECOMP_NO_TRANSIENT_DEPTH") != 0;
	if (off || t->depth || !t->data) return t;
	*out = *t;
	out->depth = 1; out->ds_data = out->ds_stencil_data = 0; out->ds_format = 0;
	out->ds_background_depth = 1.0f; out->ds_background_stencil = 0; out->ds_force_load = out->ds_force_store = 0;
	return out;
}
void gfx_begin_scene(const GfxTarget *t) {
	int m = metrics_on(); uint64_t t0 = m ? now_us() : 0;
	GfxTarget dims, tdepth;
	LOCKED(gfxcap_scene(t); gfxcov_scene(t); t = scene_dims(t, &dims); t = transient_depth(t, &tdepth); scene_target = *t; if (gpu_backend()) gfx_gpu_begin_scene(t); else gfx_soft_begin_scene(t));
	if (m) { metrics_add_render(now_us() - t0); metrics_add_scene(); }
}
static uint64_t presented;   /* frames presented so far (point_draw_seen's log line) */
/* SCE_GXM_PRIMITIVE_POINTS draws are rare -- Uncharted's swipe prompts, see gfx_soft.c's
 * render_points_interp -- so the first few of a run go to the log with their frame, and
 * RECOMP_CAPTURE_ON_POINTS=1 captures the frames after the first one (gfxcap_request, as F12 does):
 * the way to get a capture of a prompt no scripted run can be steered to on purpose */
static void point_draw_seen(const GfxDraw *d) {
	static int n, cap = -1;
	if (cap < 0) cap = getenv("RECOMP_CAPTURE_ON_POINTS") != 0;
	if (n < 8) { n++; fprintf(stderr, "    gfx: frame %llu: point sprite draw, %u points, polygon mode %u, width %u, vprog %#x fprog %#x\n", (unsigned long long)presented, d->count, d->polygon_mode, d->point_line_width, d->vprog, d->fprog); }
	if (cap == 1) { cap = 2; gfxcap_request(); fprintf(stderr, "capture requested (RECOMP_CAPTURE_ON_POINTS)\n"); }
}
void gfx_draw(const GfxDraw *d) {
	int m = metrics_on(); uint64_t t0 = m ? now_us() : 0;
	GfxDraw tmp;
	if (gfx_prim_kind(d->prim) == GFX_PRIM_POINTS) point_draw_seen(d);
	LOCKED(rc_on_draw(d); gfxcap_draw(d); gfxcov_draw(d); d = backend_draw(d, &tmp); if (gpu_backend()) gfx_gpu_draw(d); else gfx_soft_draw(d));
	if (m) { metrics_add_render(now_us() - t0); metrics_add_draw(); }
}
void gfx_sync_to_guest(void) { LOCKED(if (gpu_backend()) gfx_gpu_sync_all()); }
void gfx_guest_writing(uint32_t addr, uint32_t len) { LOCKED(if (gpu_backend()) gfx_gpu_guest_writing(addr, len)); }   /* a CPU write into guest memory: before and after (the software path reads memory directly: nothing to do) */
void gfx_guest_written(uint32_t addr, uint32_t len) { LOCKED(if (gpu_backend()) gfx_gpu_guest_written(addr, len)); }
void gfx_vis_write(uint32_t addr, uint32_t count) {   /* (inside a renderer entry point: the lock is recursive) */
	static int log = -1; if (log < 0) log = getenv("RECOMP_LOG_VIS") != 0;   /* RECOMP_LOG_VIS=1: every visibility result written */
	if (log) { static int n; if (n++ < 2000) fprintf(stderr, "    vis: %#x = %u\n", addr, count); }
	gfx_guest_writing(addr, 4); WR32(addr, count); gfx_guest_written(addr, 4);
}
void gfx_guest_reading(uint32_t addr, uint32_t len) { LOCKED(if (gpu_backend()) gfx_gpu_guest_reading(addr, len)); }
/* a scene's notification held back until its visibility counts land (gfx_gpu.c gfx_gpu_vis_defer): the kick hands its
 * fragment-phase writes here; the poller writes them once the GPU is done even if nothing on the game's side asks, and
 * gfx_vis_settle (sceGpuSignalWait, gpu.c) waits for them when the game waits for its notification */
void (*gfx_kick_done_hook)(void);   /* gpu.c's completion counter bump, after held-back writes land */
int gfx_gpu_vis_defer(const uint32_t *addr, const uint32_t *val, int n); int gfx_gpu_vis_deferred(void); void gfx_gpu_vis_flush(int wait);
static void *vis_poller(void *arg) {
	(void)arg;
	for (;;) { plat_sleep_ns(250000); if (gfx_gpu_vis_deferred()) LOCKED(gfx_gpu_vis_flush(0)); }
	return 0;
}
int gfx_kick_defer(const uint32_t *addr, const uint32_t *val, int n) {
	int r = 0; LOCKED(if (gpu_backend()) r = gfx_gpu_vis_defer(addr, val, n));
	static int started; if (r && !started) { started = 1; pthread_t t; if (!pthread_create(&t, 0, vis_poller, 0)) pthread_detach(t); }
	return r;
}
void gfx_vis_settle(void) { if (gfx_gpu_vis_deferred()) LOCKED(gfx_gpu_vis_flush(1)); }
int gfx_transfer_downscale(uint32_t src, uint32_t sw, uint32_t sh, uint32_t sstride, uint32_t dst, uint32_t dstride) { int r = 0; LOCKED(if (gpu_backend()) r = gfx_gpu_transfer_downscale(src, sw, sh, sstride, dst, dstride)); return r; }   /* a CPU read of guest memory a GPU target may hold newer: the download only */
void gfx_end_scene(void) {
	int m = metrics_on(); uint64_t t0 = m ? now_us() : 0;
	LOCKED(rc_on_end_scene(); gfxcap_end_scene(); if (gpu_backend()) gfx_gpu_end_scene(); else gfx_soft_end_scene());
	if (m) metrics_add_render(now_us() - t0);
}
void gfx_frame_queued(void) {   /* (the game's thread: under the lock, as its draws; only the motion vectors use it: RECOMP_MV, RECOMP_FSR) */
	if (gpu_backend() && gfx_gpu_mv_on()) LOCKED(gfx_gpu_frame_queued());
}
void gfx_shader_prep_tick(void) {   /* (the window's loop while the game is paused: gfx_gpu.c's shader prep) */
	if (gpu_backend()) LOCKED(gfx_gpu_shader_prep_tick());
}
/* present: a readback the GPU has not finished and that present would wait for (gfx_gpu.c readbacks_complete_at_present:
 * RECOMP_GPU_READBACK_AGE presents old) is waited for BEFORE the lock is taken, so the render thread keeps recording the
 * next frame; under the lock the wait held it for as long as the GPU was behind (10-15 ms a frame at render scale 3) and
 * the "render" metric counted that time on both threads. The wait is the display thread pacing itself to the GPU, not
 * renderer work, so it stays outside the metric too. RECOMP_GPU_READBACK_PREWAIT=0: wait under the lock, as before. */
static int readback_prewait(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_GPU_READBACK_PREWAIT"); v = !(e && *e) || atoi(e) != 0; } return v; }
void gfx_present(uint32_t base, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format) {
	if (gpu_backend() && readback_prewait()) {
		void *due[16]; int n = 0;
		LOCKED(n = gfx_gpu_readbacks_due(due, 16));
		if (n) gfx_gpu_readbacks_wait(due, n);
	}
	int m = metrics_on(); uint64_t t0 = m ? now_us() : 0;
	presented++;
	LOCKED(
		gfxcap_present(base, width, height, pitch, format);
		gfxcov_present(format);
		if (gpu_backend()) gfx_gpu_present(base, width, height, pitch, format);
		else gfx_soft_present(base, width, height, pitch, format));
	if (m) metrics_add_render(now_us() - t0);
}
