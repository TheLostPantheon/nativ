/* metrics.c's public edge: who feeds it data and who reads it back. See metrics.c's own header
 * comment for the design (what each stat means, why it's computed where it is, the overlay font). */
#ifndef RECOMP_METRICS_H
#define RECOMP_METRICS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* gfx_dispatch.c: each gfx_begin_scene/draw/end_scene/present call's own wall-clock cost (its
 * LOCKED section) -- folded into the render-time total metrics_game_frame() harvests at the next
 * presented frame. metrics_add_scene/draw also count that call. metrics_set_backend records which
 * backend gfx_dispatch.c picked (0 soft, 1 gpu) -- fixed for the run, set once, read every tick. */
int metrics_on(void);   /* anything reading the numbers (a window, RECOMP_METRICS, RECOMP_OSD): the hooks' own gate */
void metrics_add_render(uint64_t us);
void metrics_add_scene(void);
void metrics_add_draw(void);
void metrics_set_backend(int gpu);
void metrics_set_render_scale(int n);   /* the GPU backend's internal resolution multiplier (gfx_gpu.c RECOMP_RENDER_SCALE / F2), shown with the backend */

/* display.c: once per sceDisplaySetFrameBuf, i.e. once per real game frame -- rolls the renderer
 * accumulators above plus gfx_gpu.c's own GPU/software-fallback draw counts into "last frame"
 * stats, pushes a frame-time sample, and (so RECOMP_METRICS works with no window at all) may print
 * the stderr line. */
void metrics_game_frame(void);

/* window.c: once per window-loop iteration that reached the swapchain -- window/present fps and
 * how long that iteration's own upload+blit took (excluding the vsync wait inside
 * SDL_WaitAndAcquireGPUSwapchainTexture, which isn't a cost); also pokes the same stderr gate as
 * metrics_game_frame, so a hung game thread still reports 0 fps instead of going silent. */
void metrics_window_frame(uint64_t blit_us);
/* fills buf (>=128 bytes recommended) with the title-bar line when it's next due (~2 Hz); returns
 * 0 (buf untouched) otherwise, so window.c only calls SDL_SetWindowTitle when there's something new */
int metrics_title(char *buf, int bufsz);

/* window.c's audio path: queued milliseconds summed over open ports, how many ports are open, and
 * a best-effort cumulative underrun count (a port whose stream already held less than one
 * buffer's worth when the next buffer arrived) */
void metrics_set_audio(uint32_t queued_ms, uint32_t ports, uint32_t underruns);

/* the on-screen overlay: F1's level (0 off, 1 basic FPS+frame time, 2 full) and its cycle;
 * RECOMP_OSD sets the start-up value. metrics_osd_composite alpha-blends the current overlay
 * bitmap onto `rgba` (tightly packed RGBA8, `stride_px` pixels per row, visible area w x h) at a
 * fixed corner margin -- real translucency against whatever is already there, drawn with this
 * module's own hand-authored bitmap font. Call it on a CPU buffer about to be uploaded to the
 * swapchain, for both present paths, AFTER the game frame is in it; never on guest memory. A cheap
 * no-op while the level is 0. */
int metrics_osd_level(void);
void metrics_osd_cycle(void);
void metrics_osd_set(int level);   /* the overlay menu's Performance page: a direct pick (0/1/2), same effect as cycling to it */
void metrics_osd_composite(uint8_t *rgba, uint32_t stride_px, uint32_t w, uint32_t h);
int metrics_osd_lines(char (*out)[40], int max);   /* the same overlay as text lines, for overlay.cpp's ImGui HUD (windowed: no composite, no GPU round trip) */

/* the last ~1.5s of game frames' fps (game_frame_stats' own number): for the overlay menu's
 * Performance page ("show fps"), which wants the figure as a plain double, not drawn into a bitmap */
double metrics_fps(void);

/* the full overlay's frame-time graph: the last ~1.5 s of game-frame durations in ms, oldest first, at most
 * `max`; returns how many were written */
int metrics_frame_times(float *ms, int max);

/* the full overlay's numbers in one copy, for overlay.cpp's HUD (read every present while the full level shows;
 * recomputed at most twice a second). A value this machine or backend does not give is negative: drawn as "-".
 * The machine's numbers (CPU, RAM, GPU) come from a sampler thread that runs only while the full level shows, at
 * 2 Hz: their OS calls take milliseconds (a GPU monitor's first open hundreds), so they never run on the game's or
 * the window's thread. */
#define METRICS_GRAPH 240
typedef struct {
	float fps, frame_ms_avg, frame_ms_max;   /* the game's frames over the last ~1.5 s */
	float low1_fps, low01_fps;               /* the fps of the slowest 1% / 0.1% of frames over the last 10 s; -1: under 100 frames */
	int nframes; float frame_ms[METRICS_GRAPH];   /* the graph: the newest game frames' durations, oldest first */
	float speed, window_fps;
	float render_ms, present_ms;             /* the renderer's CPU time in the last frame; the window's blit */
	float builds, build_ms, tex_uploads, waits_ms, skipped;   /* per frame over the last 0.5 s: shader + pipeline builds and their time, texture uploads, waits (target downloads, readbacks, ring wraps), draws skipped for an unfinished build; -1 on the software backend */
	int gpu_backend, render_scale; unsigned scenes, draws, fallback;
	int machine;                             /* 1 once the sampler has delivered */
	float cpu_total_pct, cpu_game_pct;       /* the whole machine and this game, percent of all logical processors */
	int threads;                             /* this process's threads; 0 unknown (Windows) */
	float ram_used_mb, ram_total_mb, ram_game_mb;
	float gpu_pct, vram_used_mb, vram_total_mb, gpu_temp_c, gpu_power_w, gpu_clock_mhz;
	char cpu_name[64], gpu_name[96];
	unsigned audio_ms, audio_underruns;
} MetricsSnapshot;
void metrics_snapshot(MetricsSnapshot *out);
int metrics_sampler_running(void);   /* (tests) the machine sampler is alive */
void metrics_set_gpu_name(const char *name);   /* gfx_gpu.c, when it creates its device: the name the GPU monitor matches */
/* the 1% and 0.1% lows of n frame times (ms, any order; sorted in place) as fps: the fps of the fastest frame among
 * the slowest 1% (0.1%); -1 both under 100 frames */
void metrics_lows(float *ms, int n, float *low1_fps, float *low01_fps);

#ifdef __cplusplus
}
#endif
#endif
