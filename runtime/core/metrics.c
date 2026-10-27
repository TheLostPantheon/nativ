/*
 * Performance metrics for a play session: how fast the game is actually running, where the host's
 * own time goes, and whether the guest clock is keeping pace with real time. Three outputs share
 * one set of numbers, recomputed at most twice a second (everything else is a cheap running total
 * -- see "feeding" below):
 *
 *   - the window title (metrics_title): one line, refreshed ~2 Hz;
 *   - an on-screen overlay (metrics_osd_composite): F1 in window.c cycles off / basic (FPS and
 *     frame time only) / full; RECOMP_OSD=0|1|2 sets the start-up level. Default is 0: on the
 *     software path drawing it is nearly free (one blend into a CPU buffer already being built),
 *     but on the GPU path (gfx_gpu.c, this project's actual default) the only way to blend without
 *     adding a shader/pipeline to that file (not written yet; see window.c's comment on
 *     window_present_gpu) is a real download/composite/re-upload round trip with a GPU fence
 *     wait every frame. A performance overlay shouldn't be the thing that costs you the
 *     performance; F1 opts into that cost with eyes open instead. The overlay has no third-party
 *     font: the glyph table below is hand-drawn, 5x7 dots per character, upscaled and composited
 *     (real alpha, not a flat copy) directly into the CPU pixel buffer that's about to go to the
 *     swapchain -- never guest memory, never a RECOMP_SHOT image;
 *   - RECOMP_METRICS=1: one line to stderr a second, headless-safe (no window needed).
 *
 * Feeding: gfx_dispatch.c times its own four entry points (metrics_add_render) and counts scenes
 * and draws, into running totals metrics_game_frame() harvests once per real game frame
 * (sceDisplaySetFrameBuf, display.c) -- that's also where the frame-time ring buffer and the
 * GPU-vs-software-fallback draw split (gfx_gpu.c's own n_gpu_draws/n_fallback, read through its
 * one getter) are sampled. window.c separately reports its own loop (metrics_window_frame, for
 * window/present fps and blit cost -- a different cadence than game frames, e.g. under vsync) and
 * its audio path (metrics_set_audio). Game-frame delivery and the window loop are two independent
 * clocks on purpose: a hung game thread still leaves the window loop free to report 0 game fps
 * rather than the whole display going silent.
 *
 * The render-time/scene/draw accumulators are plain atomics (cheap on the hot path: thousands of
 * draws a frame on a busy scene, same reasoning as display.c's vblank counter); everything else
 * here runs at most a couple of times a second and sits behind one mutex.
 */
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "metrics.h"
#include "platform.h"
#ifdef __APPLE__
#include <mach/mach.h>
#endif

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }

/* whether anything will ever read these numbers: a window (title bar, F1 overlay), RECOMP_METRICS
 * or RECOMP_OSD. Resolved once; while it's 0 (headless runs: the gate, captures, the checker) the
 * hooks below cost one predictable branch and the game's frames see no extra work at all */
int metrics_on(void) {
	static int on = -1;
	if (on < 0) {
		const char *m = getenv("RECOMP_METRICS"), *o = getenv("RECOMP_OSD"), *h = getenv("RECOMP_HEADLESS");
		on = (m && *m && *m != '0') || (o && *o) || !(h && *h && *h != '0');
	}
	return on;
}

/* ---- gfx_dispatch.c's hot-path counters (§ see header): lock-free, harvested (read-and-zero) by
 * metrics_game_frame() once per real game frame */
static uint64_t acc_render_us;
static uint32_t acc_scenes, acc_draws;
static int backend_gpu;   /* 0 soft, 1 gpu; metrics_set_backend's only writer, set once early */
void metrics_add_render(uint64_t us) { __atomic_add_fetch(&acc_render_us, us, __ATOMIC_RELAXED); }
void metrics_add_scene(void) { __atomic_add_fetch(&acc_scenes, 1, __ATOMIC_RELAXED); }
void metrics_add_draw(void) { __atomic_add_fetch(&acc_draws, 1, __ATOMIC_RELAXED); }
void metrics_set_backend(int gpu) { __atomic_store_n(&backend_gpu, gpu, __ATOMIC_RELAXED); }
static int render_scale = 1;
void metrics_set_render_scale(int n) { __atomic_store_n(&render_scale, n, __ATOMIC_RELAXED); }

/* ---- everything below: guarded by mx, touched at most a couple of times a second */
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;

/* frame-time ring: the last RING_CAP real game frames' own timestamp and the gap since the one
 * before it; "last second or two" of stats scan back from `now` until a sample falls outside
 * FRAME_WINDOW_US. 512 covers a couple of seconds even at a few hundred fps. */
#define RING_CAP 512
#define FRAME_WINDOW_US 1500000ull
typedef struct { uint64_t t_us; uint32_t dur_us; } FrameSample;
static FrameSample gring[RING_CAP]; static int gring_n, gring_head; static uint64_t last_game_t;
static uint64_t wring[RING_CAP]; static int wring_n, wring_head;   /* window-loop timestamps only: no "duration" stat asked for there, just its own fps */

static void gring_push(uint64_t t, uint32_t dur) { gring[gring_head] = (FrameSample){t, dur}; gring_head = (gring_head + 1) % RING_CAP; if (gring_n < RING_CAP) gring_n++; }
static void wring_push(uint64_t t) { wring[wring_head] = t; wring_head = (wring_head + 1) % RING_CAP; if (wring_n < RING_CAP) wring_n++; }
static int dur_cmp(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }

/* game frame-time stats over the last ~1.5 s: fps (sample count / the span they actually cover),
 * the average, the worst (max), and the 1%-low -- the average of the slowest 1% of frames, which a
 * plain average hides (one stutter among 90 smooth frames barely moves the mean) */
static void game_frame_stats(uint64_t now, double *fps, double *avg_ms, double *max_ms, double *low1_ms) {
	static uint32_t durs[RING_CAP]; int n = 0; uint64_t sum = 0, mx = 0, newest = 0, oldest = 0;
	for (int i = 0; i < gring_n; i++) {
		FrameSample *s = &gring[(gring_head - 1 - i + RING_CAP) % RING_CAP];
		if (now - s->t_us > FRAME_WINDOW_US) break;
		if (!n) newest = s->t_us;
		oldest = s->t_us;
		durs[n++] = s->dur_us; sum += s->dur_us; if (s->dur_us > mx) mx = s->dur_us;
	}
	if (n < 2) { *fps = *avg_ms = *max_ms = *low1_ms = 0; return; }
	*avg_ms = sum / (double)n / 1000.0;
	*max_ms = mx / 1000.0;
	qsort(durs, (size_t)n, sizeof durs[0], dur_cmp);
	int k = n / 100; if (k < 1) k = 1;
	uint64_t slow = 0; for (int i = 0; i < k; i++) slow += durs[n - 1 - i];
	*low1_ms = slow / (double)k / 1000.0;
	double span = (newest - oldest) / 1e6;
	*fps = span > 0 ? (n - 1) / span : 0;
}
static double window_fps_stats(uint64_t now) {
	int n = 0; uint64_t newest = 0, oldest = 0;
	for (int i = 0; i < wring_n; i++) {
		uint64_t t = wring[(wring_head - 1 - i + RING_CAP) % RING_CAP];
		if (now - t > FRAME_WINDOW_US) break;
		if (!n) newest = t;
		oldest = t; n++;
	}
	double span = (newest - oldest) / 1e6;
	return n >= 2 && span > 0 ? (n - 1) / span : 0;
}

/* process CPU% (one core = 100%) between two getrusage samples, and the host's own thread count --
 * both "if cheap": one syscall and a fixed-size subtraction, called at most twice a second */
static double cpu_percent(void) {
	static struct timespec pwall; static double pcpu; static int have;
	double cpu = plat_cpu_seconds();
	struct timespec wall; clock_gettime(CLOCK_MONOTONIC, &wall);
	double pct = 0;
	if (have) {
		double dwall = (wall.tv_sec - pwall.tv_sec) + (wall.tv_nsec - pwall.tv_nsec) / 1e9;
		if (dwall > 0) pct = 100.0 * (cpu - pcpu) / dwall;
	}
	pcpu = cpu; pwall = wall; have = 1;
	return pct;
}
#ifdef __APPLE__
static int host_thread_count(void) {
	thread_array_t threads; mach_msg_type_number_t n;
	if (task_threads(mach_task_self(), &threads, &n) != KERN_SUCCESS) return 0;
	vm_deallocate(mach_task_self(), (vm_address_t)threads, (vm_size_t)n * sizeof(thread_t));
	return (int)n;
}
#else
static int host_thread_count(void) {
	FILE *f = fopen("/proc/self/status", "r"); if (!f) return 0;
	char line[64]; int n = 0;
	while (fgets(line, sizeof line, f)) if (!strncmp(line, "Threads:", 8)) { n = atoi(line + 8); break; }
	fclose(f); return n;
}
#endif

/* guest time advanced per real second, sampled here (at most twice a second: nowhere near
 * guest_time_us's own "17 reads in a row with no vblank wait" polling threshold) -- 1.0 under
 * RECOMP_CLOCK=fixed means the host is keeping up with the vblank grid; under the real clock
 * guest_time_us() IS the host's clock, so this is always ~1.0 there, which is correct, just not
 * informative. gfxreplay.c (no display.c) stubs guest_time_us with the host's own real clock, for
 * exactly the same reason: this is still the only symbol metrics.c needs from outside gfx_dispatch.c. */
static uint64_t spd_last_real, spd_last_guest;
static double guest_speed(uint64_t now) {
	extern uint64_t guest_time_us_peek(void);   /* not guest_time_us: under the fixed clock its reads advance the clock */
	uint64_t g = guest_time_us_peek();
	double speed = 1.0;
	if (spd_last_real) { uint64_t dreal = now - spd_last_real, dguest = g - spd_last_guest; if (dreal) speed = (double)dguest / (double)dreal; }
	spd_last_real = now; spd_last_guest = g;
	return speed;
}

/* ---- the harvested "last frame" snapshot: written only by metrics_game_frame, read by maybe_tick */
static uint64_t lf_render_us; static uint32_t lf_scenes, lf_draws, lf_gpu_draws, lf_fallback;
static uint32_t cum_gpu_seen, cum_fb_seen;   /* gfx_gpu.c's n_gpu_draws/n_fallback last time we looked: cumulative counters, we take our own delta */
static uint64_t last_present_us;             /* window.c's most recent blit/upload cost (metrics_window_frame) */
static uint32_t audio_ms, audio_ports, audio_underruns;

void metrics_set_audio(uint32_t queued_ms, uint32_t ports, uint32_t underruns) {
	pthread_mutex_lock(&mx); audio_ms = queued_ms; audio_ports = ports; audio_underruns = underruns; pthread_mutex_unlock(&mx);
}

/* the overlay menu's Performance page ("show fps"): the same number the title bar and the F1
 * overlay already show, as a plain double instead of a drawn line */
double metrics_fps(void) {
	double fps, avg_ms, max_ms, low1_ms;
	pthread_mutex_lock(&mx); game_frame_stats(now_us(), &fps, &avg_ms, &max_ms, &low1_ms); pthread_mutex_unlock(&mx);
	return fps;
}

/* ---- the on-screen overlay: a 5x7-dot font, hand-drawn below (upper-case letters, digits, space,
 * and the handful of symbols the stats actually use: . : / - %); unrecognised characters draw
 * blank. Each glyph is 7 row-strings of 5 '#'/'.' characters -- authored as ASCII art so the shape
 * is visible in the source, not as opaque bit masks. */
typedef const char *Rows7[7];
static const Rows7 *glyph_for(char ch) {
	static const Rows7 blank = {".....",".....",".....",".....",".....",".....","....."};
	switch (toupper((unsigned char)ch)) {
	case '0': { static const Rows7 g = {".###.","#...#","#..##","#.#.#","##..#","#...#",".###."}; return &g; }
	case '1': { static const Rows7 g = {"..#..",".##..","..#..","..#..","..#..","..#..",".###."}; return &g; }
	case '2': { static const Rows7 g = {".###.","#...#","....#","..##.",".#...","#...#","#####"}; return &g; }
	case '3': { static const Rows7 g = {".###.","#...#","....#","..##.","....#","#...#",".###."}; return &g; }
	case '4': { static const Rows7 g = {"#...#","#...#","#...#","#####","....#","....#","....#"}; return &g; }
	case '5': { static const Rows7 g = {"#####","#....","####.","....#","....#","#...#",".###."}; return &g; }
	case '6': { static const Rows7 g = {".###.","#...#","#....","####.","#...#","#...#",".###."}; return &g; }
	case '7': { static const Rows7 g = {"#####","....#","...#.","..#..",".#...",".#...",".#..."}; return &g; }
	case '8': { static const Rows7 g = {".###.","#...#","#...#",".###.","#...#","#...#",".###."}; return &g; }
	case '9': { static const Rows7 g = {".###.","#...#","#...#",".####","....#","#...#",".###."}; return &g; }
	case 'A': { static const Rows7 g = {"..#..",".#.#.","#...#","#####","#...#","#...#","#...#"}; return &g; }
	case 'B': { static const Rows7 g = {"####.","#...#","#...#","####.","#...#","#...#","####."}; return &g; }
	case 'C': { static const Rows7 g = {".####","#....","#....","#....","#....","#....",".####"}; return &g; }
	case 'D': { static const Rows7 g = {"####.","#...#","#...#","#...#","#...#","#...#","####."}; return &g; }
	case 'E': { static const Rows7 g = {"#####","#....","#....","####.","#....","#....","#####"}; return &g; }
	case 'F': { static const Rows7 g = {"#####","#....","#....","####.","#....","#....","#...."}; return &g; }
	case 'G': { static const Rows7 g = {".####","#....","#....","#.###","#...#","#...#",".####"}; return &g; }
	case 'H': { static const Rows7 g = {"#...#","#...#","#...#","#####","#...#","#...#","#...#"}; return &g; }
	case 'I': { static const Rows7 g = {".###.","..#..","..#..","..#..","..#..","..#..",".###."}; return &g; }
	case 'J': { static const Rows7 g = {"..###","...#.","...#.","...#.","...#.","#..#.",".##.."}; return &g; }
	case 'K': { static const Rows7 g = {"#...#","#..#.","#.#..","##...","#.#..","#..#.","#...#"}; return &g; }
	case 'L': { static const Rows7 g = {"#....","#....","#....","#....","#....","#....","#####"}; return &g; }
	case 'M': { static const Rows7 g = {"#...#","##.##","#.#.#","#.#.#","#...#","#...#","#...#"}; return &g; }
	case 'N': { static const Rows7 g = {"#...#","##..#","#.#.#","#.#.#","#..##","#...#","#...#"}; return &g; }
	case 'O': { static const Rows7 g = {".###.","#...#","#...#","#...#","#...#","#...#",".###."}; return &g; }
	case 'P': { static const Rows7 g = {"####.","#...#","#...#","####.","#....","#....","#...."}; return &g; }
	case 'Q': { static const Rows7 g = {".###.","#...#","#...#","#...#","#.#.#","#..#.",".##.#"}; return &g; }
	case 'R': { static const Rows7 g = {"####.","#...#","#...#","####.","#.#..","#..#.","#...#"}; return &g; }
	case 'S': { static const Rows7 g = {".####","#....","#....",".###.","....#","....#","####."}; return &g; }
	case 'T': { static const Rows7 g = {"#####","..#..","..#..","..#..","..#..","..#..","..#.."}; return &g; }
	case 'U': { static const Rows7 g = {"#...#","#...#","#...#","#...#","#...#","#...#",".###."}; return &g; }
	case 'V': { static const Rows7 g = {"#...#","#...#","#...#","#...#","#...#",".#.#.","..#.."}; return &g; }
	case 'W': { static const Rows7 g = {"#...#","#...#","#...#","#.#.#","#.#.#","##.##","#...#"}; return &g; }
	case 'X': { static const Rows7 g = {"#...#","#...#",".#.#.","..#..",".#.#.","#...#","#...#"}; return &g; }
	case 'Y': { static const Rows7 g = {"#...#","#...#",".#.#.","..#..","..#..","..#..","..#.."}; return &g; }
	case 'Z': { static const Rows7 g = {"#####","....#","...#.","..#..",".#...","#....","#####"}; return &g; }
	case '.': { static const Rows7 g = {".....",".....",".....",".....",".....",".....","..#.."}; return &g; }
	case ':': { static const Rows7 g = {".....","..#..",".....",".....","..#..",".....","....."}; return &g; }
	case '/': { static const Rows7 g = {"....#","...#.","...#.","..#..",".#...",".#...","#...."}; return &g; }
	case '-': { static const Rows7 g = {".....",".....",".....","#####",".....",".....","....."}; return &g; }
	case '%': { static const Rows7 g = {"##..#","##.#.","...#.","..#..",".#...",".#.##","#..##"}; return &g; }
	default: return &blank;
	}
}
#define GLYPH_W 5
#define GLYPH_H 7
#define OSD_SCALE 2                              /* each dot becomes a 2x2 block: legible at the frame's native 960x544, and it scales up with the rest of the frame at RECOMP_SCALE>1 */
#define CHAR_W (GLYPH_W * OSD_SCALE + 2)         /* +2px gap between characters */
#define CHAR_H (GLYPH_H * OSD_SCALE + 3)         /* +3px between lines */
#define OSD_MARGIN 6
#define OSD_MAX_COLS 34
#define OSD_MAX_ROWS 6
#define OSD_MAX_W (OSD_MARGIN * 2 + OSD_MAX_COLS * CHAR_W)
#define OSD_MAX_H (OSD_MARGIN * 2 + OSD_MAX_ROWS * CHAR_H)

static void draw_glyph(uint8_t *buf, int stride_px, char ch, int x, int y) {
	const Rows7 *rows = glyph_for(ch);
	for (int gy = 0; gy < GLYPH_H; gy++) for (int gx = 0; gx < GLYPH_W; gx++) {
		if ((*rows)[gy][gx] != '#') continue;
		for (int sy = 0; sy < OSD_SCALE; sy++) for (int sx = 0; sx < OSD_SCALE; sx++) {
			uint8_t *p = buf + 4 * ((y + gy * OSD_SCALE + sy) * stride_px + (x + gx * OSD_SCALE + sx));
			p[0] = p[1] = p[2] = p[3] = 255;   /* solid white text, full alpha */
		}
	}
}

/* double-buffered so metrics_osd_composite never has to copy the bitmap out to read it safely --
 * both live behind mx anyway (a rebuild and a composite never run concurrently), the second buffer
 * is just so a composite that's mid-read when a tick starts doesn't matter; in practice mx already
 * rules that out, this is belt and suspenders for a buffer that's cheap to keep */
static uint8_t *osd_buf[2]; static int osd_live, osd_w, osd_h;
static int osd_level_v = -1;   /* -1: RECOMP_OSD not read yet */
static char line_full[6][40]; static int n_full;

int metrics_osd_level(void) {   /* F1 (the window's event thread) writes it, every present reads it: atomic */
	int v = __atomic_load_n(&osd_level_v, __ATOMIC_RELAXED);
	if (v < 0) {
		extern int settings_has(const char *key); extern int settings_int(const char *key, int def);   /* the environment over the game's settings.ini (overlay menu), as render_scale_init (gfx_gpu.c) does */
		const char *e = getenv("RECOMP_OSD");
		v = e ? atoi(e) : settings_has("osd") ? settings_int("osd", 0) : 0;   /* default 0: see this file's header comment */
		if (v < 0 || v > 2) v = 0;
		__atomic_store_n(&osd_level_v, v, __ATOMIC_RELAXED);
	}
	return v;
}
void metrics_osd_cycle(void) { pthread_mutex_lock(&mx); __atomic_store_n(&osd_level_v, (metrics_osd_level() + 1) % 3, __ATOMIC_RELAXED); pthread_mutex_unlock(&mx); }
void metrics_osd_set(int level) {
	if (level < 0) level = 0; if (level > 2) level = 2;
	pthread_mutex_lock(&mx); __atomic_store_n(&osd_level_v, level, __ATOMIC_RELAXED); pthread_mutex_unlock(&mx);
}

/* the overlay's lines as text, for overlay.cpp's HUD: with a window, the overlay is drawn by ImGui into the
 * swapchain over the frame (window.c's present loop, with the touch markers) instead of composited into
 * the frame's pixels -- on the GPU path that composite meant a fence wait for the whole frame's GPU work
 * plus a download and re-upload of the frame, every frame (window_present_gpu's round trip: at render
 * scale 2 an 8 MB frame each way, and the CPU/GPU overlap lost). Level 1: the first line; 2: all; 0:
 * none. Returns how many were copied, each NUL-terminated in 40 bytes. */
int metrics_osd_lines(char (*out)[40], int max) {
	int level = metrics_osd_level(), n = 0;
	pthread_mutex_lock(&mx);
	int want = level >= 2 ? n_full : level == 1 ? (n_full ? 1 : 0) : 0;
	for (; n < want && n < max; n++) snprintf(out[n], 40, "%s", line_full[n]);
	pthread_mutex_unlock(&mx);
	return n;
}
/* rebuilds the overlay bitmap for `level` (0 clears it, 1 just the FPS/frame-time line, 2 all of
 * line_full) into the off-screen buffer and publishes it; caller holds mx */
static void rebuild_osd(int level) {
	int nlines = level >= 2 ? n_full : (level == 1 ? 1 : 0);
	if (!nlines) { osd_w = osd_h = 0; return; }
	int maxlen = 0;
	for (int i = 0; i < nlines; i++) { int l = (int)strlen(line_full[i]); if (l > maxlen) maxlen = l; }
	if (maxlen > OSD_MAX_COLS) maxlen = OSD_MAX_COLS;
	int w = OSD_MARGIN * 2 + maxlen * CHAR_W, h = OSD_MARGIN * 2 + nlines * CHAR_H;
	if (!osd_buf[0]) { osd_buf[0] = malloc((size_t)4 * OSD_MAX_W * OSD_MAX_H); osd_buf[1] = malloc((size_t)4 * OSD_MAX_W * OSD_MAX_H); }
	if (!osd_buf[0] || !osd_buf[1]) { osd_w = osd_h = 0; return; }
	uint8_t *b = osd_buf[1 - osd_live];
	for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint8_t *p = b + 4 * (y * w + x); p[0] = p[1] = p[2] = 0; p[3] = 170; }   /* translucent dark panel */
	for (int i = 0; i < nlines; i++) {
		const char *s = line_full[i];
		for (int c = 0; s[c] && c < OSD_MAX_COLS; c++) draw_glyph(b, w, s[c], OSD_MARGIN + c * CHAR_W, OSD_MARGIN + i * CHAR_H);
	}
	osd_w = w; osd_h = h; osd_live = 1 - osd_live;
}

void metrics_osd_composite(uint8_t *rgba, uint32_t stride_px, uint32_t w, uint32_t h) {
	pthread_mutex_lock(&mx);
	int ow = osd_w, oh = osd_h; uint8_t *ov = ow && oh ? osd_buf[osd_live] : 0;
	if (ov) {
		const int ox = 8, oy = 8;
		for (int y = 0; y < oh && oy + y < (int)h; y++) {
			uint8_t *drow = rgba + 4 * ((oy + y) * stride_px + ox);
			const uint8_t *srow = ov + 4 * y * ow;
			for (int x = 0; x < ow && ox + x < (int)w; x++, drow += 4, srow += 4) {
				uint8_t a = srow[3]; if (!a) continue;
				if (a == 255) { drow[0] = srow[0]; drow[1] = srow[1]; drow[2] = srow[2]; continue; }
				drow[0] = (uint8_t)((srow[0] * a + drow[0] * (255 - a)) / 255);
				drow[1] = (uint8_t)((srow[1] * a + drow[1] * (255 - a)) / 255);
				drow[2] = (uint8_t)((srow[2] * a + drow[2] * (255 - a)) / 255);
			}
		}
	}
	pthread_mutex_unlock(&mx);
}

/* RECOMP_OSD_DUMP=path.ppm: writes the overlay bitmap once (forcing a full-level rebuild if the
 * overlay itself is off or basic, so the dump always shows the whole font), composited onto a
 * mid-grey backdrop so translucency is visible, for `magick` + a look rather than guessing at
 * legibility. Caller holds mx. */
static void maybe_dump_osd(void) {
	static int done = -1;
	if (done < 0) { const char *e = getenv("RECOMP_OSD_DUMP"); done = e ? 0 : 1; }
	if (done) return;
	done = 1;
	const char *path = getenv("RECOMP_OSD_DUMP");
	int level = metrics_osd_level();
	if (level < 2) rebuild_osd(2);
	int w = osd_w, h = osd_h;
	if (w && h) {
		uint8_t *canvas = malloc((size_t)3 * w * h);
		if (canvas) {
			memset(canvas, 128, (size_t)3 * w * h);   /* mid-grey backdrop */
			const uint8_t *ov = osd_buf[osd_live];
			for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
				const uint8_t *s = ov + 4 * (y * w + x); uint8_t a = s[3]; uint8_t *d = canvas + 3 * (y * w + x);
				if (!a) continue;
				if (a == 255) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; continue; }
				d[0] = (uint8_t)((s[0] * a + d[0] * (255 - a)) / 255); d[1] = (uint8_t)((s[1] * a + d[1] * (255 - a)) / 255); d[2] = (uint8_t)((s[2] * a + d[2] * (255 - a)) / 255);
			}
			FILE *f = fopen(path, "wb");
			if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h); fwrite(canvas, 1, (size_t)3 * w * h, f); fclose(f); fprintf(stderr, "    osd: wrote the %dx%d overlay bitmap to %s\n", w, h, path); }
			else fprintf(stderr, "RECOMP_OSD_DUMP: cannot write %s\n", path);
			free(canvas);
		}
	}
	if (level < 2) rebuild_osd(level);   /* restore what's actually meant to be on screen */
}

/* ---- the shared ~2 Hz recompute (title + overlay content) and ~1 Hz stderr report; either driver
 * (a game frame or a window-loop iteration) can trigger it, whichever crosses the gate first --
 * see this file's header comment for why there are two drivers */
#define TICK_US 500000ull
#define REPORT_US 1000000ull
static uint64_t last_tick_us, last_report_us;
static char line_title[160];
static uint64_t last_title_emit_us;

static void maybe_tick(uint64_t now) {
	pthread_mutex_lock(&mx);
	if (last_tick_us && now - last_tick_us < TICK_US) { pthread_mutex_unlock(&mx); return; }
	last_tick_us = now;

	double fps, avg_ms, max_ms, low1_ms; game_frame_stats(now, &fps, &avg_ms, &max_ms, &low1_ms);
	double wfps = window_fps_stats(now);
	double speed = guest_speed(now);
	double cpu = cpu_percent();
	int threads = host_thread_count();
	int gpu = __atomic_load_n(&backend_gpu, __ATOMIC_RELAXED);
	int rs = __atomic_load_n(&render_scale, __ATOMIC_RELAXED);
	char backend[16]; if (gpu && rs > 1) snprintf(backend, sizeof backend, "gpu %dx", rs); else snprintf(backend, sizeof backend, "%s", gpu ? "gpu" : "soft");
	uint32_t scenes = lf_scenes, draws = lf_draws, gd = lf_gpu_draws, fd = lf_fallback;
	double render_ms = lf_render_us / 1000.0, present_ms = last_present_us / 1000.0;
	uint32_t am = audio_ms, ap = audio_ports, au = audio_underruns;

	snprintf(line_title, sizeof line_title,
	        "vitarecomp - %.1f fps  frame %.1f/%.1f/%.1f ms  speed %.2fx  %s %u scn %u dr%s  cpu %.0f%%  win %.1f fps",
	        fps, avg_ms, max_ms, low1_ms, speed, backend, scenes, draws, (gpu && fd) ? " (sw fallback)" : "", cpu, wfps);

	/* the overlay's own lines, in its font's upper-case + digits + ". : / - %" charset only */
	snprintf(line_full[0], sizeof line_full[0], "FPS %.1f FRAME %.1f/%.1f/%.1f MS", fps, avg_ms, max_ms, low1_ms);
	snprintf(line_full[1], sizeof line_full[1], "SPEED %.2fX WINDOW %.1f FPS", speed, wfps);
	snprintf(line_full[2], sizeof line_full[2], "%s%s %uSCN %uDR %uSW", gpu ? "GPU" : "SOFT", gpu && rs == 2 ? " 2X" : gpu && rs == 3 ? " 3X" : gpu && rs == 4 ? " 4X" : "", scenes, draws, fd);
	snprintf(line_full[3], sizeof line_full[3], "RENDER %.1fMS PRESENT %.1fMS", render_ms, present_ms);
	snprintf(line_full[4], sizeof line_full[4], "CPU %.0f%% THREADS %d", cpu, threads);
	snprintf(line_full[5], sizeof line_full[5], "AUDIO %uMS Q %u UNDERRUN", am, au);
	n_full = 6;

	rebuild_osd(metrics_osd_level());
	maybe_dump_osd();

	static int metrics_env = -1; if (metrics_env < 0) { const char *e = getenv("RECOMP_METRICS"); metrics_env = e && *e && *e != '0'; }
	if (metrics_env && (!last_report_us || now - last_report_us >= REPORT_US)) {
		last_report_us = now;
		fprintf(stderr, "    metrics: %.1f fps (frame %.1f/%.1f/%.1f ms avg/max/1%%low)  speed %.2fx  window %.1f fps  %s: %u scenes %u draws (%u gpu %u sw)  render %.1f ms present %.1f ms  cpu %.0f%% (%d threads)  audio %u ms queued (%u ports), %u underruns\n",
		        fps, avg_ms, max_ms, low1_ms, speed, wfps, backend, scenes, draws, gd, fd, render_ms, present_ms, cpu, threads, am, ap, au);
	}
	pthread_mutex_unlock(&mx);
}

void metrics_game_frame(void) {
	if (!metrics_on()) return;
	uint64_t now = now_us();
	uint64_t render_us = __atomic_exchange_n(&acc_render_us, 0, __ATOMIC_RELAXED);
	uint32_t scenes = __atomic_exchange_n(&acc_scenes, 0, __ATOMIC_RELAXED);
	uint32_t draws = __atomic_exchange_n(&acc_draws, 0, __ATOMIC_RELAXED);
	uint32_t cum_gpu = 0, cum_fb = 0;
	extern void gfx_gpu_get_counts(uint32_t *gpu_draws, uint32_t *fallback_draws);
	gfx_gpu_get_counts(&cum_gpu, &cum_fb);

	pthread_mutex_lock(&mx);
	lf_render_us = render_us; lf_scenes = scenes; lf_draws = draws;
	lf_gpu_draws = cum_gpu >= cum_gpu_seen ? cum_gpu - cum_gpu_seen : cum_gpu;       /* RECOMP_GPU_STATS resets these to 0 once a second when it's on; a drop means that happened */
	lf_fallback  = cum_fb  >= cum_fb_seen  ? cum_fb  - cum_fb_seen  : cum_fb;
	cum_gpu_seen = cum_gpu; cum_fb_seen = cum_fb;
	if (last_game_t) gring_push(now, (uint32_t)(now - last_game_t));
	last_game_t = now;
	pthread_mutex_unlock(&mx);

	maybe_tick(now);
}

void metrics_window_frame(uint64_t blit_us) {
	if (!metrics_on()) return;
	uint64_t now = now_us();
	pthread_mutex_lock(&mx);
	wring_push(now);
	last_present_us = blit_us;
	pthread_mutex_unlock(&mx);
	maybe_tick(now);
}

int metrics_title(char *buf, int bufsz) {
	uint64_t now = now_us();
	pthread_mutex_lock(&mx);
	int due = !last_title_emit_us || now - last_title_emit_us >= 500000ull;
	if (due) { last_title_emit_us = now; snprintf(buf, (size_t)bufsz, "%s", line_title); }
	pthread_mutex_unlock(&mx);
	return due;
}
