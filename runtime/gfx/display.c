/*
 * SceDisplay, audio output and input: the system calls below the GXM library. GXM itself runs as
 * Sony's own code (libgxm_es4 and libgpu_es4 in MODULES; gxm_sony.c observes its rendering
 * boundary, gpu.c completes what it submits to the GPU driver), including the display queue, whose
 * thread calls the game's flip callback, which lands here in sceDisplaySetFrameBuf. Vblank is a
 * fixed 60 Hz grid, as the display hardware has; audio output is paced like the hardware's ports;
 * input is the host window's (window.c) or nothing pressed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>   /* sched_yield: vb_sleep_until */
#include <math.h>
#include "hle.h"
#include "gfx.h"
#include "metrics.h"
#include "settings.h"

static uint64_t raw_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
/* the in-game overlay's pause (display_set_paused, called by overlay.cpp as the menu opens and
 * closes): while it's open the host clock the game runs on (guest time on the real clock, vblank
 * edges, audio pacing) stands still at the instant it opened, and on resume carries on from there
 * with the paused span taken off for good -- a game left in the menu for a minute doesn't see a
 * minute pass (its timers, physics steps). Stays monotonic: readers get the frozen instant until
 * the offset has grown by the pause, then that instant plus the real time since. */
static uint64_t pause_off, pause_at;   /* total real time paused so far; nonzero while paused: the frozen instant */
void display_set_paused(int on) {
	uint64_t raw = raw_us(), at = __atomic_load_n(&pause_at, __ATOMIC_ACQUIRE);
	if (on && !at) __atomic_store_n(&pause_at, raw - __atomic_load_n(&pause_off, __ATOMIC_RELAXED), __ATOMIC_RELEASE);
	else if (!on && at) { __atomic_store_n(&pause_off, raw - at, __ATOMIC_RELEASE); __atomic_store_n(&pause_at, 0, __ATOMIC_RELEASE); }
}
static uint64_t now_us(void) { uint64_t at = __atomic_load_n(&pause_at, __ATOMIC_ACQUIRE); return at ? at : raw_us() - __atomic_load_n(&pause_off, __ATOMIC_ACQUIRE); }
/* RECOMP_CLOCK=fixed (vitarecomp test/capture set it): the time the GAME sees -- process time, RTC
 * ticks, libc time, input timestamps -- is deterministic: it starts at a fixed instant and advances
 * one vblank period per vblank waited and one microsecond per read, however long the host actually
 * took. A scripted test's frame then holds the same animation phase every run, loaded or not
 * (Uncharted's menu drifted 10-30% between correct runs on wall-clock time). The host's own pacing
 * (vblank sleeps, audio) stays real. Otherwise: the host's monotonic clock. */
static int clock_fixed(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_CLOCK"); v = e && !strcmp(e, "fixed"); } return v; }
static uint64_t vblanks, vreads;
static uint64_t frame_edges_g = 2, vb_setbuf_g;   /* h_set_frame_buf: the fixed clock's vblanks between the last two SetFrameBufs, and that clock at the last one */
/* threads waiting for guest time to pass (the fixed clock's audio pacing, below): woken whenever
 * the vblank count moves */
static pthread_mutex_t vclock_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vclock_cv = PTHREAD_COND_INITIALIZER;
/* Each thread counts the vblanks IT waited; the clock is the furthest any thread has got. Summed, two
 * threads waiting on the same edge (Uncharted's display and main threads, every frame) moved the
 * guest's time two periods an edge: the game ran at double speed under the fixed clock, live play and
 * a recorded session's replay both (2026-10-10). A thread joins at the clock's present on its first
 * wait; one waiting thread gives exactly the old count. */
static __thread uint64_t vb_mine; static __thread int vb_joined;
/* A SetFrameBuf wait's vblanks for the fixed clock: up to the n-th after the set (vb_setbuf_g), from
 * where this thread already is -- none when it got there waiting for vblank start. Counted n either
 * way, Uncharted at 60 fps (WaitVblankStart, then WaitSetFrameBufMulti(1) already met) took two a
 * frame: double speed (2026-10-10). */
static uint64_t setbuf_guest_count(uint64_t n) {
	pthread_mutex_lock(&vclock_m);
	if (!vb_joined) { vb_joined = 1; vb_mine = __atomic_load_n(&vblanks, __ATOMIC_RELAXED); }
	uint64_t want = vb_setbuf_g + n, c = want > vb_mine ? want - vb_mine : 0;
	pthread_mutex_unlock(&vclock_m);
	return c;
}
/* RECOMP_CLOCK_IDLE=1 (with RECOMP_CLOCK=fixed): while no thread waits for a vblank -- Uncharted's
 * loading screens flip nothing for seconds -- the clock follows the wall's vblank edges (vidle, kept
 * apart from the threads' own counts so they don't fall behind it), credited whenever the time is
 * read or awaited. Without it guest time moved only a microsecond per polling read there (0.06x)
 * and every load took ~16 times as long (2026-10-10). Off by default: how much passes then depends
 * on how fast the host loads, which a gate frame mustn't. */
static uint64_t vidle, vb_active_edge, vb_idle_edge;
static uint64_t vb_now(void), wall_edges(void);
static int clock_idle(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_CLOCK_IDLE"); v = e && *e == '1'; } return v; }
enum { VB_IDLE_EDGES = 15 };   /* no vblank wait for this long (250 ms) is a load, not a slow frame (a VERIFY build's count window runs frames at a few fps) */
static int overlay_paused(void);
static void vclock_idle_locked(void) {   /* (vclock_m held) */
	uint64_t e = wall_edges(), a = __atomic_load_n(&vb_active_edge, __ATOMIC_RELAXED);
	if (overlay_paused()) { __atomic_store_n(&vb_active_edge, e, __ATOMIC_RELAXED); return; }   /* the menu holds the game and its clock: a pause is not a load */
	if (!a || e <= a + VB_IDLE_EDGES) return;
	uint64_t from = vb_idle_edge > a + VB_IDLE_EDGES ? vb_idle_edge : a + VB_IDLE_EDGES;
	if (e > from) { __atomic_add_fetch(&vidle, e - from, __ATOMIC_RELAXED); vb_idle_edge = e; pthread_cond_broadcast(&vclock_cv); }
}
static void vclock_idle(void) {
	if (!clock_idle()) return;
	pthread_mutex_lock(&vclock_m); vclock_idle_locked(); pthread_mutex_unlock(&vclock_m);
}
static uint64_t vclock_edges(void) { return __atomic_load_n(&vblanks, __ATOMIC_RELAXED) + __atomic_load_n(&vidle, __ATOMIC_RELAXED); }   /* the fixed clock, in vblank periods */
static void vblanks_add(uint64_t n) {
	if (n) __atomic_store_n(&vb_active_edge, wall_edges(), __ATOMIC_RELAXED);
	pthread_mutex_lock(&vclock_m);
	if (!vb_joined) { vb_joined = 1; vb_mine = __atomic_load_n(&vblanks, __ATOMIC_RELAXED); }
	vb_mine += n;
	if (vb_mine > __atomic_load_n(&vblanks, __ATOMIC_RELAXED)) __atomic_store_n(&vblanks, vb_mine, __ATOMIC_RELAXED);
	pthread_cond_broadcast(&vclock_cv);
	pthread_mutex_unlock(&vclock_m);
}
/* the fixed clock: vblanks plus a microsecond per time read -- but only once a thread is POLLING
 * (its 17th consecutive read with no vblank wait or sleep between). A microsecond per read made the
 * clock depend on how many reads every thread made per frame, and under load (agents replaying,
 * builds) Unit 13's time-driven menu ticker landed on a different step: the gate's frame flaked.
 * Ordinary per-frame reads now leave the clock on the vblank grid; a loop that spins on the time
 * still advances it and terminates. */
static __thread uint32_t reads_in_row;
uint64_t guest_time_us(void) {
	if (!clock_fixed()) return now_us();
	if (++reads_in_row > 16) __atomic_add_fetch(&vreads, 1, __ATOMIC_RELAXED);
	vclock_idle();
	return (1ull << 32) + vclock_edges() * 16667ull + __atomic_load_n(&vreads, __ATOMIC_RELAXED);
}
int guest_clock_fixed(void) { return clock_fixed(); }
/* the same time as guest_time_us without its side effects (the fixed clock advances a microsecond
 * per read past 16 in a row): for an observer that must not perturb what the game sees -- metrics.c */
uint64_t guest_time_us_peek(void) {
	if (!clock_fixed()) return now_us();
	return (1ull << 32) + vclock_edges() * 16667ull + __atomic_load_n(&vreads, __ATOMIC_RELAXED);
}
/* a thread's own sleep (sceKernelDelayThread) also moves the fixed clock by what it asked for:
 * loading code that polls the time between sleeps, with no vblank wait in the loop, would never
 * see it pass otherwise (Uncharted's boot stalled at frame 3 the first time) */
/* a thread's sleep does NOT advance the fixed clock any more: every sleeping thread's DelayThread
 * calls added their microseconds to one global clock, so its rate depended on how many threads
 * were polling and how fast the host ran them -- under load (agents replaying on the same machine)
 * Unit 13's menu animation landed on a different phase every gate run (2.8% between two runs).
 * Guest time is vblanks plus a microsecond per read; a loop that sleeps while waiting for guest
 * time still progresses because frames keep flipping. (The advance had been added for Uncharted's
 * boot "stall", which turned out to be the software renderer, not the clock.) */
void guest_time_slept(uint32_t us) { (void)us; reads_in_row = 0; }
static uint64_t t0, frames;
static uint64_t frame_edges = 2;   /* the edges between the last two SetFrameBufs: how long the frame now on screen took */
uint64_t display_frames(void) { return frames; }   /* presented frames so far (main.c's RECOMP_LOG_FROM_FRAME) */
static uint64_t vb_now(void); static uint64_t vb_setbuf;   /* below: the vblank grid */
static void vb_sleep_to_edge(uint64_t edge);   /* (below) */
static uint32_t swap_seen;      /* the largest swap interval a SetFrameBuf wait asked for since the last SetFrameBuf: how long that frame stays up */
static uint64_t present_edge;   /* the edge the last frame went to the window at (h_set_frame_buf) */
/* SceDisplayFrameBuf: size, base, pitch, pixel format, width, height */
/* RECOMP_HITCH_LOG=ms: every presented frame that took longer than that (SetFrameBuf to SetFrameBuf, real time)
 * logs what the host did in it -- the GPU backend's first-use work and waits (gfx_gpu_last_frame_costs) and
 * the guest's file reads (io.c) -- so a stutter can be put down to a cause, not guessed at */
static int hitch_ms(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_HITCH_LOG"); v = e && *e ? atoi(e) : 0; if (v < 0) v = 0; } return v; }
void stallprof_present(void);   /* stallprof.c: RECOMP_STALL_PROF */
static void hitch_note(uint64_t frame_us, uint64_t edges) {
	static uint32_t nhitch; int th = hitch_ms();
	uint64_t rb, ru, ab, au; uint32_t rc, ac;
	extern void io_read_stats_take(uint64_t *, uint32_t *, uint64_t *, uint64_t *, uint32_t *, uint64_t *);
	io_read_stats_take(&rb, &rc, &ru, &ab, &ac, &au);   /* this frame's, whether or not it's logged */
	if (!th || frame_us < (uint64_t)th * 1000) return;
	GfxGpuFrameCosts g; gfx_gpu_last_frame_costs(&g);
	fprintf(stderr, "    hitch %u: frame %llu took %.1f ms (%llu edges): gpu %u draws %.1f ms [shaders %u (%u cached) %.1f ms (translate %.1f msl %.1f compile %.1f), pipelines %u %.1f ms, %u skipped (building), textures %u up (%u new) %.1f ms, target downloads %u %.1f ms (sync %.1f scene %.1f), readback waits %u %.1f ms, ring waits %u %.1f ms, vfetch %.1f ms (slowest draw %.1f, map %.1f), %u flushes, %u sw]  io: %u reads %llu KB %.1f ms, %u archive entries %llu KB %.1f ms\n",
	        ++nhitch, (unsigned long long)frames, frame_us / 1e3, (unsigned long long)edges, g.draws, g.t_total / 1e3, g.shaders, g.shaders_cached, g.t_shader / 1e3, g.t_xlate / 1e3, g.t_msl / 1e3, g.t_compile / 1e3, g.pipes, g.t_pipe / 1e3, g.skipped,
	        g.tex_uploads, g.tex_new, g.t_texup / 1e3, g.tgt_downloads, g.t_tgt_download / 1e3, g.t_tsync / 1e3, g.t_scene_sync / 1e3, g.readback_waits, g.t_readback_wait / 1e3, g.ring_waits, g.t_ring_wait / 1e3, g.t_vfetch / 1e3, g.t_vfetch_max / 1e3, g.t_map / 1e3, g.flushes, g.fallback,
	        rc, (unsigned long long)(rb >> 10), ru / 1e3, ac, (unsigned long long)(ab >> 10), au / 1e3);
	if (g.fallback) {   /* why those draws went to the software path, and how full the tables are that send them there when full */
		fprintf(stderr, "    hitch %u:   software path:", nhitch);
		for (int i = 0; i < 4 && g.fb_why[i]; i++) fprintf(stderr, "%s %u %s", i ? "," : "", g.fb_n[i], g.fb_why[i]);
		fprintf(stderr, " (tables: %u shaders, %u pipelines)\n", g.nshaders, g.npipes);
	}
}
/* the game's flip interval, averaged: how long a frame takes from the game's update to its flip, about
 * (window.c's automatic audio delay: the game's own part of a picture's way to the screen) */
static volatile float frame_period_ms = 33.3f;
float display_frame_period_ms(void) { return frame_period_ms; }
static void load_mark(void);   /* below, with the input replay */
static void (*frame_hook)(uint64_t);   /* display_set_frame_hook: called once per presented frame (verify.c's F6 capture steps its phases on it) */
void display_set_frame_hook(void (*f)(uint64_t)) { frame_hook = f; }
static void h_set_frame_buf(Cpu *c) {
	metrics_game_frame();   /* once per presented game frame: frame-time stats, last frame's renderer/GPU-draw counts, the ~1 Hz RECOMP_METRICS line (metrics.c) */
	frames++;
	static uint64_t hitch_prev; uint64_t hitch_now = raw_us(), hitch_dur = hitch_prev ? hitch_now - hitch_prev : 0; hitch_prev = hitch_now;
	{ uint64_t e = vb_now(); __atomic_store_n(&frame_edges, e - vb_setbuf, __ATOMIC_RELAXED); vb_setbuf = e; }
	{ uint64_t g = __atomic_load_n(&vblanks, __ATOMIC_RELAXED); __atomic_store_n(&frame_edges_g, g - vb_setbuf_g, __ATOMIC_RELAXED); vb_setbuf_g = g; }
	load_mark();
	if (frame_hook) frame_hook(frames);
	{ extern void memscan_frame(uint64_t); memscan_frame(frames); }   /* RECOMP_MEMSCAN (memscan.c) */
	if (hitch_dur > 0 && hitch_dur < 200000) { float ms = hitch_dur / 1e3f; frame_period_ms += (ms - frame_period_ms) * 0.05f; }   /* the flip interval (display_frame_period_ms) */
	{ extern void sysmem_dump_maybe(uint64_t); sysmem_dump_maybe(frames); }
	if (!t0) t0 = now_us();
	if (frames <= 3 || frames % 300 == 0 || (getenv("RECOMP_VBLANK_TRACE") && frames < 60))
		fprintf(stderr, "    frame %llu: %ux%u at %#x, %.3f s\n", (unsigned long long)frames, RD32(A0 + 16), RD32(A0 + 20), RD32(A0 + 4), (now_us() - t0) / 1e6);
	/* the flip lands at the next vblank: the display hardware latches the new buffer there, so a game's frames reach the
	 * screen on the vblank grid however unevenly its frames finish. Presented the moment SetFrameBuf ran, they reached the
	 * window that unevenly: Killzone's 30 fps flips came 25-45 ms apart (each frame finishing at its own point between two
	 * edges) and the window loop, showing the newest frame at each refresh, held them for 1-3 refreshes -- a steady 30 on
	 * the counter, juddering motion on screen. And as the display's queue does, a frame stays up for the swap interval
	 * the game's flip waits asked for after it (WaitSetFrameBufMulti(2) at 30 fps): the next one goes at the later of its
	 * own next edge and that slot. The next edge alone still juddered in Killzone's levels and cutscenes -- a frame that
	 * finished just past an edge went one edge late, the next one on time: up for 1, 2 or 3 edges. The sleep never
	 * delays the game: the slot is at most the edge its flip callback waits for next anyway. With a window only
	 * (headless runs and the gate pace as before); RECOMP_PRESENT_AT_VBLANK=0|1 overrides. */
	{ static int at_vb = -1; if (at_vb < 0) { extern int window_active(void); const char *e = getenv("RECOMP_PRESENT_AT_VBLANK"); at_vb = e && *e ? atoi(e) != 0 : window_active(); }
	  uint32_t hold = swap_seen ? swap_seen : 1; swap_seen = 0;
	  if (at_vb) { uint64_t edge = vb_setbuf + 1; if (present_edge && present_edge + hold > edge) edge = present_edge + hold; vb_sleep_to_edge(edge); present_edge = edge; }
	  static int ptrace = -1; if (ptrace < 0) ptrace = getenv("RECOMP_PRESENT_TRACE") != 0;   /* RECOMP_PRESENT_TRACE=1: each present's distance from the one before (the cadence the window gets) */
	  static uint64_t plast; uint64_t pnow = raw_us(); if (ptrace && plast) fprintf(stderr, "    present: +%.1f ms (set +%.1f ms)\n", (pnow - plast) / 1e3, hitch_dur / 1e3); plast = pnow; }
	int b = sched_block_begin();   /* presenting is the display hardware's work: no run slot meanwhile (RECOMP_SCHED) */
	gfx_present(RD32(A0 + 4), RD32(A0 + 16), RD32(A0 + 20), RD32(A0 + 8), RD32(A0 + 12));
	if (b) sched_block_end();
	stallprof_present();
	{ extern void waitstats_present(void); waitstats_present(); }   /* (RECOMP_WAIT_STATS) */
	if (hitch_ms()) hitch_note(hitch_dur, __atomic_load_n(&frame_edges, __ATOMIC_RELAXED));
	{	/* RECOMP_TEST_STALL=frame:ms -- a deliberate stall of the flipping thread at that frame (does a
		 * game's clock carry a long frame through, or drop it?) */
		static int f = -2, ms; if (f == -2) { const char *e = getenv("RECOMP_TEST_STALL"); f = -1; if (e) { f = atoi(e); const char *c2 = strchr(e, ':'); ms = c2 ? atoi(c2 + 1) : 1000; } }
		if (f >= 0 && frames == (uint64_t)f) { fprintf(stderr, "    test stall: %d ms at frame %llu\n", ms, (unsigned long long)frames); usleep((useconds_t)ms * 1000); }
	}
	RET(0);
}
/* vblank: a fixed 60 Hz grid from the first wait, as the display hardware has; a wait sleeps to
 * the next tick after now (restarting the period from "now" would add a whole period to every
 * late frame) */
static uint64_t vblank0;
static const uint64_t vb_period = 16667;
static uint64_t wall_edges(void) { uint64_t t = now_us(); if (!vblank0) vblank0 = t; return (t - vblank0) / vb_period; }   /* edges since the first wait */
/* RECOMP_UNPACED=1 (with RECOMP_CLOCK=fixed): a throughput benchmark. The grid is the fixed clock's own vblank count instead of
 * the wall's: a wait that counts vblanks moves the clock and returns, one that counts none (a polling pad thread) waits for
 * another thread to move it, and nothing sleeps to an edge. The game sees the same clock as in a paced run (the fixed clock
 * never showed it the wall), so it does the same work, as fast as the host can: the wall time to a frame is then what a
 * CPU-side change buys, where a paced run sits at 60 whatever the host does. Loading works in real time (Killzone's loader
 * threads sleep and read), so a loading screen unpaced flips a thousand frames a second and frame-keyed taps land before the
 * menu they were for: RECOMP_UNPACED=<frame> runs paced up to that presented frame (1: unpaced from the start). From there
 * only the waits that count no vblank still look at the grid, and they settle on the new one at their next wait. */
static int unpaced(void) {
	static long from = -2; if (from == -2) { const char *e = getenv("RECOMP_UNPACED"); from = clock_fixed() && e && *e ? atol(e) : -1; if (from == 0) from = -1; }
	return from >= 0 && frames >= (uint64_t)from;
}
static uint64_t vb_now(void) { return unpaced() ? vclock_edges() : wall_edges(); }
static uint64_t edge_us(uint64_t e) { return unpaced() ? 0 : vblank0 + e * vb_period; }   /* when edge e is, for a sleep to it (0: at once) */
/* vb_setbuf (above): the edge count when the frame buffer was last set: the flip lands at the next edge */
static __thread uint64_t vb_last_target;    /* the edge this thread last waited for */
/* sleep until edge `target` (at least the next one), counting it for the fixed guest clock */
/* the overlay (window.c: F10, Escape, a gamepad combo) pauses the game by holding every thread
 * right here, where essentially all of a title's own per-frame pacing already rendezvouses every
 * frame -- and, under RECOMP_CLOCK=fixed, by holding off vblanks_add() until it's done, which holds
 * the fixed guest clock still too; on the real clock display_set_paused (above) does the same. A
 * plain sleep-poll, no lock: the window thread that flips the flag must never wait on anything this
 * thread holds. */
static int overlay_paused(void) { extern int overlay_game_paused(void); return overlay_game_paused(); }   /* the menu open, or the menu bar's Game > Pause (overlay.h) */
/* The sleep to an edge. One usleep to the exact time wakes late by a fixed share of the sleep on macOS
 * -- the kernel's timer leeway for an ordinary thread is about half of what was asked: usleep(1000)
 * returns after 1.5 ms, usleep(5000) after 7.2, usleep(15000) after 20 (measured here; a SCHED_RR or
 * UI-QoS thread gets a quarter, capped near 2 ms) -- so the display thread woke 2-5 ms after the edge
 * it had waited for, and the frame after a light one started that late: at 60 fps about one presented
 * frame in 25 logged 20-24 ms with one edge. Sleeping half of what remains, over and over, never
 * passes the edge (half plus its leeway is at most ~0.8 of the remainder) and converges in ~7 wakes
 * from a whole period; the last RECOMP_VBLANK_SPIN us (default 200) are sched_yield polls, which land
 * within ~20 us. Costs the display thread ~0.2 ms of polling a frame; a fixed lead before the edge did
 * not work (the first sleep's leeway alone overshot it). RECOMP_VBLANK_SPIN=0: the single sleep, as
 * before. */
static uint64_t vb_spin_us(void) {
	static long v = -1;
	if (v < 0) { const char *e = getenv("RECOMP_VBLANK_SPIN"); v = e && *e ? atol(e) : 200; if (v == 1) v = 200; if (v < 0) v = 0; }
	return (uint64_t)v;
}
static void vb_sleep_until(uint64_t at) {   /* at: now_us() time; the thread is blocked for RECOMP_SCHED the whole way (a vblank wait gives the core up) */
	uint64_t t = now_us(), spin = vb_spin_us();
	if (at <= t || unpaced()) return;
	int b = sched_block_begin();
	if (!spin) usleep((useconds_t)(at - t));
	else for (;;) {
		t = now_us(); if (t >= at) break;
		uint64_t left = at - t;
		if (left > spin) usleep((useconds_t)(left / 2)); else sched_yield();
	}
	if (b) sched_block_end();
}
static void vb_sleep_to_edge(uint64_t edge) { (void)vb_now(); vb_sleep_until(vblank0 + edge * vb_period); }   /* (vb_now starts the grid if nothing waited yet) */
static void vb_wait_until(uint64_t target, uint32_t count) {
	reads_in_row = 0;
	uint64_t now = vb_now(); if (target <= now) target = now + 1;
	uint64_t t = now_us(), at = vblank0 + target * vb_period;
	if (getenv("RECOMP_VBLANK_TRACE")) { static int k; if (k++ < 60) fprintf(stderr, "    vblank wait: edge %llu (now %llu), sleeping %lld us (frame %llu)\n", (unsigned long long)target, (unsigned long long)now, (long long)(at > t ? at - t : 0), (unsigned long long)frames); }
	if (at > t) vb_sleep_until(at);
	if (unpaced() && !count) {   /* (a wait that moves no clock: until another thread's does, or a while) */
		pthread_mutex_lock(&vclock_m);
		for (int k = 0; k < 20 && vclock_edges() < target; k++) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 5000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; } pthread_cond_timedwait(&vclock_cv, &vclock_m, &ts); }
		pthread_mutex_unlock(&vclock_m);
	}
	if (overlay_paused()) { int b = sched_block_begin(); while (overlay_paused()) usleep(2000); if (b) sched_block_end(); }
	vb_last_target = target;
	vblanks_add(count);   /* the fixed guest clock's unit: one per vblank asked for, as before */
}
/* sceDisplayWaitVblankStart: the next edge. sceDisplayWaitVblankStartMulti(n): n edges counted from
 * the edge this thread last waited for (the hardware counts vblanks; the time the thread spent
 * between waits is part of the n) -- counted from "now" instead, Unit 13's 30 fps flip callback
 * (Multi(2) after SetFrameBuf) took three or four edges a frame: 50-68 ms. sceDisplayWaitSetFrameBuf
 * / Multi(n): n edges after the one the last SetFrameBuf lands on. */
static void h_wait_vblank(Cpu *c) { vb_wait_until(vb_now() + 1, 1); RET(0); }
static void h_wait_vblank_multi(Cpu *c) {
	uint32_t n = A0 ? A0 : 1; if (n > 8) n = 8;
	uint64_t now = vb_now(), base = vb_last_target && vb_last_target + n > now ? vb_last_target : now;
	if (getenv("RECOMP_VBLANK_TRACE")) { static int k; if (k++ < 40) fprintf(stderr, "    vblank wait multi(%u): last %llu now %llu\n", n, (unsigned long long)vb_last_target, (unsigned long long)now); }
	vb_wait_until(base + n, n); RET(0);
}
/* The SetFrameBuf waits are a condition, not a fresh wait: until the frame buffer set last has been
 * up for n edges. When that edge has already passed -- the frame was late, or the flip's own
 * WaitSetFrameBuf after SetFrameBuf already reached it -- they return at once. vb_wait_until's "at
 * least the next edge" is the VblankStart waits' rule; applied here it cost every late frame an extra
 * edge, and made a swap interval of 1 unreachable: both Unit 13 and Uncharted flip with Multi(n),
 * SetFrameBuf, WaitSetFrameBuf, so Multi(1)'s edge (setbuf + 1) was always just reached and got
 * bumped to the one after -- 30 fps whatever n said. The fixed clock counts the n vblanks either way. */
static void vb_wait_setbuf(uint64_t target, uint32_t count) {
	if (clock_fixed()) count = (uint32_t)setbuf_guest_count(count);
	if (target > vb_now()) { vb_wait_until(target, count); return; }
	reads_in_row = 0;
	if (overlay_paused()) { int b = sched_block_begin(); while (overlay_paused()) usleep(2000); if (b) sched_block_end(); }
	vb_last_target = vb_now();
	vblanks_add(count);
}
/* the "frame_rate" setting (settings.h; RECOMP_FRAME_RATE wins): 60 caps the swap interval a game asks
 * WaitSetFrameBufMulti for at one edge -- it presents every vblank it has a frame ready. Experimental:
 * a game's own fixed 1/30 s steps then run at its frame rate. Read every call (once a frame), so the
 * menu's choice applies at once. */
static int forced_rate = -1;   /* display_force_frame_rate: a capture's switch (verify.c's F6), over everything else */
void display_force_frame_rate(int fr) { forced_rate = fr; }
static int frame_rate_60(void) {
	if (forced_rate >= 0) return forced_rate >= 60;
	static int env = -2; static long from = -2;
	if (env == -2) { const char *e = getenv("RECOMP_FRAME_RATE"); env = e && *e ? atoi(e) : -1; }
	/* RECOMP_FRAME_RATE_AT="frame:rate,...": the rate from each presented frame on (the last entry at or before the frame
	 * wins) -- a headless run that switches back and forth, as the menu's setting does mid-game */
	static const char *at = (const char *)-1; if (at == (const char *)-1) at = getenv("RECOMP_FRAME_RATE_AT");
	if (at) {
		int r = 30;
		for (const char *q = at; *q; ) { unsigned long f = strtoul(q, (char **)&q, 10); int v = 30; if (*q == ':') v = (int)strtol(q + 1, (char **)&q, 10); if (frames >= f) r = v; if (*q == ',') q++; else break; }
		return r >= 60;
	}
	/* RECOMP_FRAME_RATE_FROM=n: 60 from presented frame n on (a recorded session plays its frame-keyed
	 * input at 30 into a level, then the measurement runs at 60) */
	if (from == -2) { const char *e = getenv("RECOMP_FRAME_RATE_FROM"); from = e && *e ? atol(e) : -1; }
	if (from >= 0) return frames >= (uint64_t)from;
	/* RECOMP_FRAME_RATE_UNTIL=n: 60 up to presented frame n, 30 from there (a recorded 60 fps session
	 * replayed to a frame-rate bug, then the same moment at 30: Uncharted's companion stuck on a log) */
	static long until = -2;
	if (until == -2) { const char *e = getenv("RECOMP_FRAME_RATE_UNTIL"); until = e && *e ? atol(e) : -1; }
	if (until >= 0) return frames < (uint64_t)until;
	return (env >= 0 ? env : settings_int("frame_rate", 30)) >= 60;
}
int display_frame_rate_60(void) { return frame_rate_60(); }
float display_frame_dt(void) { uint64_t e = __atomic_load_n(clock_fixed() ? &frame_edges_g : &frame_edges, __ATOMIC_RELAXED); return (float)(e < 1 ? 1 : e > 2 ? 2 : e) / 60.0f; }   /* 1/60..1/30 s (hooks.c: a game's fixed 1/30 steps at 60 fps) */
static uint32_t swap_interval(uint32_t n) { if (!n) n = 1; if (n > 8) n = 8; return n > 1 && frame_rate_60() ? 1 : n; }
static void h_wait_setbuf(Cpu *c) { if (!swap_seen) swap_seen = 1; vb_wait_setbuf(vb_setbuf + 1, 1); RET(0); }
static void h_wait_setbuf_multi(Cpu *c) { uint32_t n = swap_interval(A0); if (n > swap_seen) swap_seen = n; vb_wait_setbuf(vb_setbuf + n, n); RET(0); }

/* ---- the vblank callback and the CB waits. sceDisplayRegisterVblankStartCallback(cb): a kernel
 * callback (kernel.c) the hardware notifies at every vblank; here nothing runs at the edge, so a
 * registered callback is brought up to date -- notified with the edges since its last notification
 * -- whenever a thread delivers callbacks (kernel.c calls display_vblank_callbacks from
 * CheckCallback and every CB wait), and the owner runs it then. The display's CB waits sleep edge by
 * edge, delivering at each one, so a vblank callback owned by the waiting thread runs once per
 * vblank during the wait, as on the hardware. */
static struct { uint32_t uid; uint64_t last; } vbcbs[8];
static pthread_mutex_t vbcb_lock = PTHREAD_MUTEX_INITIALIZER;
/* Under RECOMP_CLOCK=fixed the edges a callback is told about are the guest's vblanks (the count
 * the fixed clock is made of), not wall-clock edges: counted from the host's real time, two runs of
 * the same recorded session delivered different numbers of vblank notifications, and whatever a
 * game drives from its vblank callback ran a different number of times (the replay drift). */
static uint64_t vb_edges_seen(void) { return clock_fixed() ? vclock_edges() : vb_now(); }
void display_vblank_callbacks(void) {
	uint64_t now = vb_edges_seen();
	pthread_mutex_lock(&vbcb_lock);
	for (int i = 0; i < 8; i++) if (vbcbs[i].uid && now > vbcbs[i].last) { kernel_notify_callback(vbcbs[i].uid, (uint32_t)(now - vbcbs[i].last), 0); vbcbs[i].last = now; }
	pthread_mutex_unlock(&vbcb_lock);
}
static void h_register_vblank_cb(Cpu *c) {   /* (callback uid) */
	if (!kernel_callback_valid(A0)) { RET(0x80290001u); return; }   /* SCE_DISPLAY_ERROR_INVALID_VALUE */
	pthread_mutex_lock(&vbcb_lock);
	int at = -1;
	for (int i = 0; i < 8; i++) if (vbcbs[i].uid == A0) at = i;
	for (int i = 0; at < 0 && i < 8; i++) if (!vbcbs[i].uid) at = i;
	if (at >= 0) { vbcbs[at].uid = A0; vbcbs[at].last = vb_edges_seen(); }
	pthread_mutex_unlock(&vbcb_lock);
	RET(at >= 0 ? 0 : 0x80290001u);
}
static void h_unregister_vblank_cb(Cpu *c) {
	pthread_mutex_lock(&vbcb_lock);
	int found = 0;
	for (int i = 0; i < 8; i++) if (vbcbs[i].uid == A0) { vbcbs[i].uid = 0; found = 1; }
	pthread_mutex_unlock(&vbcb_lock);
	RET(found ? 0 : 0x80290001u);
}
/* vb_wait_until's CB form: the pending callbacks before, at each edge and after; the sleep itself
 * runs a callback notified meanwhile (kernel_cb_sleep) */
static void vb_wait_until_cb(Cpu *c, uint64_t target, uint32_t count) {
	uint64_t now = vb_now(); if (target <= now) target = now + 1;
	kernel_check_callbacks(c);
	if (overlay_paused()) { int b = sched_block_begin(); while (overlay_paused()) usleep(2000); if (b) sched_block_end(); }   /* the overlay: see vb_wait_until's own comment */
	if (clock_fixed()) {
		/* the guest's edges, one per vblank asked for, each followed by the callbacks it notifies;
		 * the real sleep only paces the host (as many callback rounds every run, however late) */
		for (uint32_t k = 0; k < count; k++) {
			uint64_t e = now + 1 + (uint64_t)k * (target - now) / count;
			kernel_cb_sleep(c, edge_us(e));
			vblanks_add(1);
			kernel_check_callbacks(c);
		}
		vb_last_target = target;
		return;
	}
	for (uint64_t e = now + 1; e <= target; e++) { kernel_cb_sleep(c, edge_us(e)); kernel_check_callbacks(c); }
	vb_last_target = target;
	vblanks_add(count);
}
static void h_wait_vblank_cb(Cpu *c) { vb_wait_until_cb(c, vb_now() + 1, 1); RET(0); }
static void h_wait_vblank_multi_cb(Cpu *c) {
	uint32_t n = A0 ? A0 : 1; if (n > 8) n = 8;
	uint64_t now = vb_now(), base = vb_last_target && vb_last_target + n > now ? vb_last_target : now;
	vb_wait_until_cb(c, base + n, n); RET(0);
}
/* the CB forms of the SetFrameBuf waits: the same "already up: return" rule (vb_wait_setbuf), with the
 * pending callbacks delivered; under the fixed clock they keep their edge-by-edge callback rounds */
static void vb_wait_setbuf_cb(Cpu *c, uint64_t target, uint32_t count) {
	if (clock_fixed()) {   /* the guest's vblanks still to go, edge by edge with their callback rounds; none: the callbacks only */
		count = (uint32_t)setbuf_guest_count(count);
		if (count) vb_wait_until_cb(c, target, count); else { kernel_check_callbacks(c); vb_last_target = vb_now(); }
		return;
	}
	if (target > vb_now()) { vb_wait_until_cb(c, target, count); return; }
	kernel_check_callbacks(c);
	if (overlay_paused()) { int b = sched_block_begin(); while (overlay_paused()) usleep(2000); if (b) sched_block_end(); }
	vb_last_target = vb_now();
	vblanks_add(count);
}
static void h_wait_setbuf_cb(Cpu *c) { if (!swap_seen) swap_seen = 1; vb_wait_setbuf_cb(c, vb_setbuf + 1, 1); RET(0); }
static void h_wait_setbuf_multi_cb(Cpu *c) { uint32_t n = swap_interval(A0); if (n > swap_seen) swap_seen = n; vb_wait_setbuf_cb(c, vb_setbuf + n, n); RET(0); }

/* ---- audio output: paced like the hardware (Output blocks until the previous buffer has
 * played), the samples not played yet */
static uint32_t port_len[8], port_freq[8], port_ch[8];
static int port_dev[8];
int audio_open(uint32_t freq, uint32_t channels, uint32_t samples);
void audio_output(int dev, const void *buf, uint32_t bytes);
static uint64_t port_next[8];
static uint32_t port_vol[8][2];   /* sceAudioOutSetVolume: 0..0x8000 (SCE_AUDIO_VOLUME_0DB) per channel */
static void h_audio_open(Cpu *c) {   /* (type, samples per buffer, frequency, mode) */
	for (int i = 0; i < 8; i++) if (!port_len[i]) {
		port_len[i] = A1; port_freq[i] = A2 ? A2 : 48000; port_ch[i] = A3 == 0 ? 1 : 2;   /* mode 0 mono, 1 stereo */
		port_vol[i][0] = port_vol[i][1] = 0x8000;
		port_dev[i] = audio_open(port_freq[i], port_ch[i], A1);
		fprintf(stderr, "    audio port %d: %u samples at %u Hz, mode %u\n", i, A1, A2, A3);
		RET(i + 1); return;
	}
	RET(0x80260005u);
}
/* RECOMP_AUDIO_WAV=<file>: port 1's output as a WAV file (the header's sizes are patched as it grows);
 * RECOMP_AUDIO_WAV_DIR=<dir>: every port's buffers, each to <dir>/port<i>_<rate>Hz_<ch>ch.wav in its
 * own format, as the game hands them over (before the port's volume) */
static void wav_write(uint32_t i, const void *pcm) {
	static FILE *f[8]; static int tried; static uint32_t bytes[8]; static const char *dir;
	if (!tried) { tried = 1; if (getenv("RECOMP_AUDIO_WAV")) f[0] = fopen(getenv("RECOMP_AUDIO_WAV"), "wb"); dir = getenv("RECOMP_AUDIO_WAV_DIR"); }
	if (dir && !f[i]) { char path[1024]; snprintf(path, sizeof path, "%s/port%u_%uHz_%uch.wav", dir, i, port_freq[i], port_ch[i]); f[i] = fopen(path, "wb"); if (!f[i]) dir = 0; }
	if (!f[i]) return;
	uint32_t n = port_len[i] * port_ch[i] * 2, rate = port_freq[i], ch = port_ch[i];
	if (!bytes[i]) {
		uint8_t h[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0";
		h[22] = (uint8_t)ch; memcpy(h + 24, &rate, 4); uint32_t br = rate * ch * 2; memcpy(h + 28, &br, 4);
		h[32] = (uint8_t)(ch * 2); h[34] = 16; memcpy(h + 36, "data", 4);
		fwrite(h, 1, 44, f[i]);
	}
	fwrite(pcm, 1, n, f[i]); bytes[i] += n;
	uint32_t riff = bytes[i] + 36;
	off_t at = ftello(f[i]); fseeko(f[i], 4, SEEK_SET); fwrite(&riff, 4, 1, f[i]); fseeko(f[i], 40, SEEK_SET); fwrite(&bytes[i], 4, 1, f[i]); fseeko(f[i], at, SEEK_SET);   /* (64-bit offsets: Windows' long ftell stops at 2 GB) */
	fflush(f[i]);
}
/* (port, SCE_AUDIO_VOLUME_FLAG_L_CH 1 | R_CH 2, const int vol[2]): Killzone sets its ports' volume
 * at start-up and from the options menu */
static void h_audio_set_volume(Cpu *c) {
	uint32_t i = A0 - 1;
	if (i >= 8 || !port_len[i]) { RET(0x80260003u); return; }
	if (!A2) { RET(0x80260008u); return; }   /* SCE_AUDIO_OUT_ERROR_INVALID_POINTER */
	for (int ch = 0; ch < 2; ch++) if (A1 & (1u << ch)) { uint32_t v = RD32(A2 + 4 * ch); if (v > 0x8000) { RET(0x80260009u); return; } port_vol[i][ch] = v; }   /* INVALID_VOLUME */
	if (getenv("RECOMP_AUDIO_WAV_DIR")) fprintf(stderr, "    audio port %u: volume %#x %#x\n", i, port_vol[i][0], port_vol[i][1]);
	RET(0);
}
/* RECOMP_CLOCK=fixed: an output port consumes its buffers in guest time, as the audio hardware
 * does in the console's one clock -- an output call returns once the previous buffer has played
 * (the thread runs at most one buffer ahead), and "played" is counted on the guest clock. Paced by
 * the host's wall clock instead, voice lines ended a different number of frames into the run every
 * time, and Unit 13's mission scripts (which wait for them) drifted 1-7 frames per event between
 * two replays of one recorded session. A thread waiting here gives its run slot up (RECOMP_SCHED);
 * if guest time stops for two seconds of real time (every other thread blocked on this one) it
 * goes on rather than deadlock, and says so. */
static uint64_t port_vnext[8];
static uint64_t guest_vblank_us(void) { return (1ull << 32) + vclock_edges() * 16667ull; }
static void audio_wait_guest(uint32_t i, uint64_t len) {
	vclock_idle();
	uint64_t now = guest_vblank_us();
	if (port_vnext[i] < now) port_vnext[i] = now;
	uint64_t start = port_vnext[i];   /* this buffer starts playing when the previous one ends */
	port_vnext[i] += len;
	if (start <= now) return;
	int b = sched_block_begin();
	pthread_mutex_lock(&vclock_m);
	int stalls = 0;
	while ((clock_idle() ? vclock_idle_locked() : (void)0, guest_vblank_us() < start)) {
		struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 100 * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
		if (pthread_cond_timedwait(&vclock_cv, &vclock_m, &ts) && ++stalls >= 20) {
			static int said; if (!said++) fprintf(stderr, "    audio: guest time stood still for 2 s while port %u waited (the fixed clock's audio pacing gave way)\n", i);
			break;
		}
	}
	pthread_mutex_unlock(&vclock_m);
	if (b) sched_block_end();
}
static void h_audio_output(Cpu *c) {   /* (port, buffer) */
	uint32_t i = A0 - 1;
	if (i >= 8 || !port_len[i]) { RET(0x80260003u); return; }
	if (A1) wav_write(i, G(A1));
	if (port_dev[i]) {
		if (A1) {
			uint32_t n = port_len[i] * port_ch[i];
			if (port_vol[i][0] == 0x8000 && (port_ch[i] == 1 || port_vol[i][1] == 0x8000)) audio_output(port_dev[i], G(A1), n * 2);
			else {   /* scaled by the port's volume (the hardware's attenuation happens after the game's mix) */
				static __thread int16_t *tmp; static __thread uint32_t tmpn;
				if (tmpn < n) { tmp = realloc(tmp, n * 2); tmpn = n; }
				const int16_t *pcm = (const int16_t *)G(A1);
				for (uint32_t k = 0; k < n; k++) tmp[k] = (int16_t)(((int32_t)pcm[k] * (int32_t)port_vol[i][port_ch[i] == 2 ? (k & 1) : 0]) >> 15);
				audio_output(port_dev[i], tmp, n * 2);
			}
		}
		if (clock_fixed()) audio_wait_guest(i, (uint64_t)port_len[i] * 1000000 / port_freq[i]);
		RET(0); return;
	}
	if (getenv("RECOMP_AUDIO_STATS") && A1) {   /* how loud the buffers are (silent while NGS is missing?) */
		static uint64_t nb; int16_t *pcm = (int16_t *)G(A1); double sum = 0; uint32_t n = port_len[i] * port_ch[i];
		for (uint32_t k = 0; k < n; k++) sum += (double)pcm[k] * pcm[k];
		if (nb++ % 200 == 0) fprintf(stderr, "    audio port %u: buffer %llu rms %.1f\n", i, (unsigned long long)nb, sqrt(sum / n));
	}
	uint64_t len = (uint64_t)port_len[i] * 1000000 / port_freq[i];
	if (clock_fixed()) { audio_wait_guest(i, len); RET(0); return; }
	uint64_t t = now_us();
	if (!port_next[i] || port_next[i] + len < t) port_next[i] = t;
	if (port_next[i] > t) { int b = sched_block_begin(); usleep((useconds_t)(port_next[i] - t)); if (b) sched_block_end(); }
	port_next[i] += len;
	RET(0);
}

/* ---- input: nothing pressed yet */
/* ---- input recording and replay. RECOMP_INPUT_RECORD=path writes every controller and touch
 * sample the game reads, with the presented-frame number it was read at ("C frame buttons lx ly
 * rx ry", "T frame port down id x y slot" -- one line per contact on the panel, slot its place in
 * the report list; a recording from before multi-touch has no slot: 0); RECOMP_INPUT_PLAY=path serves those samples back by frame
 * instead of the window, the scripts and the mouse (the latest record at or before the frame).
 * Why: Unit 13's enemy that turns but never walks is a room into the first mission -- a path no
 * scripted tap sequence reaches -- so we play there once with recording on and the
 * headless replay (deterministic under RECOMP_CLOCK=fixed) puts the probes on it. */
typedef struct { uint64_t frame; uint32_t buttons; uint8_t st[4]; } CtrlRec;
typedef struct { uint64_t frame; uint8_t port, down, id, slot; uint16_t x, y; } TouchRec;
typedef struct { uint64_t frame; float a[3], g[3], q[4]; } MotionRec;   /* "M frame ax ay az gx gy gz qx qy qz qw": a live gyro's state as the game read it */
enum { TOUCH_SLOTS = 6 };   /* contacts a panel reports at once here (the front panel's own limit is 6, the rear's 4) */
static FILE *rec_file; static int rec_init, play_loaded;
static CtrlRec *ctrl_recs; static size_t n_ctrl_recs, ctrl_cur;
static TouchRec *touch_recs; static size_t n_touch_recs, touch_cur[2][TOUCH_SLOTS];
static uint64_t *load_recs; static size_t n_load_recs, load_cur;   /* "L frame": the recorded session's load ends (load_mark) */
static MotionRec *motion_recs; static size_t n_motion_recs, motion_cur;
static void input_rec_init(void) {
	if (rec_init) return; rec_init = 1;
	const char *r = getenv("RECOMP_INPUT_RECORD");
	if (r) { rec_file = fopen(r, "w"); if (!rec_file) fprintf(stderr, "RECOMP_INPUT_RECORD: cannot write %s\n", r); else fprintf(stderr, "recording input to %s\n", r); }
	const char *pl = getenv("RECOMP_INPUT_PLAY");
	if (!pl) return;
	FILE *f = fopen(pl, "r");
	if (!f) { fprintf(stderr, "RECOMP_INPUT_PLAY: cannot open %s\n", pl); return; }
	char line[256]; size_t cc = 0, tc = 0, mc = 0;
	while (fgets(line, sizeof line, f)) {
		if (line[0] == 'C') {
			CtrlRec r2 = {0}; unsigned s0, s1, s2, s3;
			if (sscanf(line + 1, "%llu %x %u %u %u %u", (unsigned long long *)&r2.frame, &r2.buttons, &s0, &s1, &s2, &s3) == 6) {
				r2.st[0] = (uint8_t)s0; r2.st[1] = (uint8_t)s1; r2.st[2] = (uint8_t)s2; r2.st[3] = (uint8_t)s3;
				if (n_ctrl_recs == cc) { cc = cc ? cc * 2 : 4096; ctrl_recs = realloc(ctrl_recs, cc * sizeof *ctrl_recs); }
				ctrl_recs[n_ctrl_recs++] = r2;
			}
		} else if (line[0] == 'L') {
			unsigned long long fr;
			if (sscanf(line + 1, "%llu", &fr) == 1) { load_recs = realloc(load_recs, (n_load_recs + 1) * sizeof *load_recs); load_recs[n_load_recs++] = fr; }
		} else if (line[0] == 'M') {
			MotionRec r2 = {0};
			if (sscanf(line + 1, "%llu %f %f %f %f %f %f %f %f %f %f", (unsigned long long *)&r2.frame, &r2.a[0], &r2.a[1], &r2.a[2], &r2.g[0], &r2.g[1], &r2.g[2], &r2.q[0], &r2.q[1], &r2.q[2], &r2.q[3]) == 11) {
				if (n_motion_recs == mc) { mc = mc ? mc * 2 : 4096; motion_recs = realloc(motion_recs, mc * sizeof *motion_recs); }
				motion_recs[n_motion_recs++] = r2;
			}
		} else if (line[0] == 'T') {
			TouchRec r2 = {0}; unsigned port, down, id, x, y, slot = 0;
			if (sscanf(line + 1, "%llu %u %u %u %u %u %u", (unsigned long long *)&r2.frame, &port, &down, &id, &x, &y, &slot) >= 6 && slot < TOUCH_SLOTS) {
				r2.port = (uint8_t)port; r2.down = (uint8_t)down; r2.id = (uint8_t)id; r2.x = (uint16_t)x; r2.y = (uint16_t)y; r2.slot = (uint8_t)slot;
				if (n_touch_recs == tc) { tc = tc ? tc * 2 : 4096; touch_recs = realloc(touch_recs, tc * sizeof *touch_recs); }
				touch_recs[n_touch_recs++] = r2;
			}
		}
	}
	fclose(f); play_loaded = 1;
	fprintf(stderr, "replaying input from %s: %zu controller, %zu touch and %zu motion samples, %zu load ends\n", pl, n_ctrl_recs, n_touch_recs, n_motion_recs, n_load_recs);
}
/* the replay's position in the recording: the presented frame plus an offset that verify.c's Lua
 * tracer re-anchors at every mission-script call it finds in the recorded session's log
 * (RECOMP_INPUT_ANCHORS) -- loading and streaming take a few frames more or less from run to run,
 * so a purely frame-keyed replay of a long session drifts until the player misses a ledge or a
 * guard spots him (Unit 13's training: 2 frames by the mission start, diverged at a melee) */
static int64_t play_offset;
void input_play_set_offset(int64_t o) { play_offset = o; }
/* A load's end under RECOMP_CLOCK_IDLE: the 10th frame in a row with no idle time after frames that
 * ran idle for half a second or more between them (Uncharted's loading screen still flips every few
 * hundred ms, so a load is a run of slow frames, not one gap). A recording marks it ("L frame"); a
 * replay re-aligns there to the recorded mark nearest its own position (within 20 s, in order). An
 * idle clock lets the host's real loading time into guest time, so live play and its replay passed
 * the same menus on different frames: Uncharted's replay took the player's swipe to Chapters as a
 * tap on Continue (2026-10-11). It re-aligns at loads only: assets a menu loads in the background
 * still arrive on whatever frame the host's disk gets them there, which a replay can't reproduce. */
static uint64_t vidle_seen, load_idle, load_clean;
static void load_mark(void) {
	uint64_t vi = __atomic_load_n(&vidle, __ATOMIC_RELAXED), d = vi - vidle_seen; vidle_seen = vi;
	if (d && getenv("RECOMP_LOAD_TRACE")) fprintf(stderr, "    idle clock: +%llu vblanks before frame %llu\n", (unsigned long long)d, (unsigned long long)frames);
	if (d) { load_idle += d; load_clean = 0; return; }
	if (!load_idle || ++load_clean < 10) return;
	uint64_t was = load_idle; load_idle = 0;
	if (was < 30) return;
	input_rec_init();
	if (rec_file) fprintf(rec_file, "L %llu\n", (unsigned long long)frames);
	if (!play_loaded || load_cur >= n_load_recs) return;
	int64_t at = (int64_t)frames + play_offset; size_t best = (size_t)-1; int64_t bd = 1200;
	for (size_t k = load_cur; k < n_load_recs; k++) { int64_t dd = (int64_t)load_recs[k] - at; if (dd < 0) dd = -dd; if (dd <= bd) { bd = dd; best = k; } if ((int64_t)load_recs[k] > at + 1200) break; }
	if (best == (size_t)-1) { fprintf(stderr, "input anchor: a load ended at frame %llu with no recorded load end near it\n", (unsigned long long)frames); return; }
	play_offset = (int64_t)load_recs[best] - (int64_t)frames; load_cur = best + 1;
	fprintf(stderr, "input anchor (load end): frame %llu = recorded %llu (offset %lld)\n", (unsigned long long)frames, (unsigned long long)load_recs[best], (long long)play_offset);
}
/* RECOMP_INPUT_SHIFT=from:delta[,from:delta...]: from replay frame `from` on, serve the recording `delta` frames later
 * (negative: earlier), deltas adding up -- a hand alignment where a load took a different number of frames than in
 * the recorded session and no load-end anchor was recorded (Killzone's loading screens keep flipping, so the idle
 * detector behind load_mark never fires): its brutal-melee swipes arrived before the replay's prompt */
static int64_t input_shift(uint64_t at) {
	static int n = -1; static uint64_t from[16]; static int64_t delta[16];
	if (n < 0) { n = 0; const char *e = getenv("RECOMP_INPUT_SHIFT");
		for (const char *p = e; p && *p && n < 16; ) { char *end; from[n] = strtoull(p, &end, 10); if (*end != ':') break; delta[n] = strtoll(end + 1, &end, 10); n++; p = *end == ',' ? end + 1 : end; } }
	int64_t d = 0; for (int i = 0; i < n; i++) if (at >= from[i]) d += delta[i];
	return d;
}
static uint64_t play_frame(void) { int64_t f = (int64_t)frames + play_offset + input_shift(frames); return f < 0 ? 0 : (uint64_t)f; }
/* the latest recorded sample at or before the replay frame (records are in frame order) */
static int play_ctrl(uint32_t *b, uint8_t st[4]) {
	uint64_t pf = play_frame();
	if (!n_ctrl_recs || ctrl_recs[0].frame > pf) return 0;
	if (ctrl_recs[ctrl_cur].frame > pf) ctrl_cur = 0;   /* the anchor moved the replay back */
	while (ctrl_cur + 1 < n_ctrl_recs && ctrl_recs[ctrl_cur + 1].frame <= pf) ctrl_cur++;
	*b = ctrl_recs[ctrl_cur].buttons; memcpy(st, ctrl_recs[ctrl_cur].st, 4); return 1;
}
static int play_touch(uint32_t port, uint32_t slot, int *down, uint8_t *id, int *x, int *y) {   /* one slot of a panel */
	uint64_t pf = play_frame();
	size_t *cur = &touch_cur[port & 1][slot]; size_t i = *cur, best = (size_t)-1;
	#define MATCH(r) ((r).port == (port & 1) && (r).slot == slot)
	if (*cur < n_touch_recs && touch_recs[*cur].frame > pf) *cur = i = 0;
	for (; i < n_touch_recs && touch_recs[i].frame <= pf; i++) if (MATCH(touch_recs[i])) best = i;
	if (best == (size_t)-1) { for (i = 0; i < *cur; i++) if (MATCH(touch_recs[i]) && touch_recs[i].frame <= pf) best = i; }
	#undef MATCH
	if (best == (size_t)-1) return 0;
	if (best > *cur) *cur = best;
	*down = touch_recs[best].down; *id = touch_recs[best].id; *x = touch_recs[best].x; *y = touch_recs[best].y; return 1;
}
static int play_motion(float a[3], float g[3], float q[4]) {   /* the latest recorded motion state at or before the replay frame */
	uint64_t pf = play_frame();
	if (!n_motion_recs || motion_recs[0].frame > pf) return 0;
	if (motion_recs[motion_cur].frame > pf) motion_cur = 0;
	while (motion_cur + 1 < n_motion_recs && motion_recs[motion_cur + 1].frame <= pf) motion_cur++;
	memcpy(a, motion_recs[motion_cur].a, 12); memcpy(g, motion_recs[motion_cur].g, 12); memcpy(q, motion_recs[motion_cur].q, 16); return 1;
}
static void h_ctrl_peek(Cpu *c) {   /* (port, SceCtrlData *, count): timestamp, buttons, sticks centred */
	input_rec_init();
	for (uint32_t i = 0; i < A2; i++) {
		uint32_t p = A1 + 64 * i;
		memset(G(p), 0, 64);
		WR64(p, guest_time_us());
		extern void window_input(uint32_t *b, uint8_t s[4]);
		uint32_t b; uint8_t st[4];
		int played = play_loaded && play_ctrl(&b, st);
		if (!played) window_input(&b, st);
		WR32(p + 8, b);
		WR8(p + 12, st[0]); WR8(p + 13, st[1]); WR8(p + 14, st[2]); WR8(p + 15, st[3]);
		/* RECOMP_INPUT="from-to:mask,...": buttons held over frame ranges (hex Vita masks: 4000
		 * cross, 8 start, 20 right, ...), for scripted headless runs */
		static char *script; static int script_read;
		if (!script_read) { script_read = 1; script = getenv("RECOMP_INPUT"); }
		for (const char *q = played ? 0 : script; q && *q; ) {
			unsigned long a = strtoul(q, (char **)&q, 10), z = a, m = 0;
			if (*q == '-') z = strtoul(q + 1, (char **)&q, 10);
			if (*q == ':') m = strtoul(q + 1, (char **)&q, 16);
			if (frames >= a && frames <= z) {
				WR32(p + 8, RD32(p + 8) | (uint32_t)m);
				/* RECOMP_LOG_CTRL=1: the scripted presses as the game reads them (port, buffers asked,
				 * the mask) -- Uncharted's menu ignored a scripted cross and this showed why */
				static int logn = -1; if (logn < 0) logn = getenv("RECOMP_LOG_CTRL") ? 20 : 0;
				if (logn > 0) { logn--; fprintf(stderr, "    ctrl: frame %llu port %u count %u buffer %u/%u: scripted %#lx -> %#x (lr %#x)\n", (unsigned long long)frames, A0, A2, i, A2, m, RD32(p + 8), c->r[14]); }
			}
			if (*q == ',') q++; else break;
		}
		/* RECOMP_INPUT_STICKS="from-to:lx,ly,rx,ry;...": sticks held at fixed byte values (0-255,
		 * 128 centred) over frame ranges, for headless runs that need the right stick (or left) held
		 * steady -- e.g. driving a raw-look hook's camera deterministically without a window. Several
		 * ranges, semicolon-separated; a byte left out of a quad keeps that axis centred for the range. */
		static char *sticks_script; static int sticks_script_read;
		if (!sticks_script_read) { sticks_script_read = 1; sticks_script = getenv("RECOMP_INPUT_STICKS"); }
		for (const char *q = played ? 0 : sticks_script; q && *q; ) {
			unsigned long a = strtoul(q, (char **)&q, 10), z = a;
			long v[4] = {128, 128, 128, 128};
			if (*q == '-') z = strtoul(q + 1, (char **)&q, 10);
			if (*q == ':') { q++; for (int k = 0; k < 4; k++) { v[k] = strtol(q, (char **)&q, 10); if (*q == ',') q++; else break; } }
			if (frames >= a && frames <= z) for (int k = 0; k < 4; k++) WR8(p + 12 + k, (uint8_t)v[k]);
			if (*q == ';') q++; else break;
		}
		if (rec_file && i == 0) fprintf(rec_file, "C %llu %x %u %u %u %u\n", (unsigned long long)frames, RD32(p + 8), RD8(p + 12), RD8(p + 13), RD8(p + 14), RD8(p + 15));
		/* RECOMP_LOG_CTRL_FROM=n (RECOMP_LOG_CTRL_TO, default n+60): every read over those presented frames -- the
		 * vblank it saw, the thread, the caller, the sticks it got (which frames a pad thread gets a sample in) */
		{ static long lf = -2, lt; if (lf == -2) { const char *e = getenv("RECOMP_LOG_CTRL_FROM"), *t = getenv("RECOMP_LOG_CTRL_TO"); lf = e ? atol(e) : -1; lt = t ? atol(t) : lf + 60; }
		  if (lf >= 0 && i == 0 && (long)frames >= lf && (long)frames <= lt) { extern const char *recomp_cur_tname(void);
			fprintf(stderr, "    ctrl read: frame %llu vblank %llu port %u count %u %s lr %#x sticks %u %u %u %u\n", (unsigned long long)frames, (unsigned long long)vb_now(), A0, A2, recomp_cur_tname(), c->r[14], RD8(p + 12), RD8(p + 13), RD8(p + 14), RD8(p + 15)); } }
	}
	RET(A2);
}
/* the Read forms block until a sample this thread hasn't seen exists -- the hardware samples the
 * controller and the panels once per vblank edge and Read waits for the next one -- while Peek
 * returns the latest sample at once. A thread that waits for vblank then reads once a frame never
 * stalls here (the edge is new); a thread that only reads blocks once per edge. Killzone's PadThread
 * is such a loop: with Read returning at once it made ten billion import calls in seven minutes
 * and ate a core. Under the fixed clock this is deterministic (edges are the clock). */
/* per thread AND per port: a sample edge is new for each device separately. Uncharted reads
 * both touch panels every frame (port 0 then 1); with one edge per thread the second read blocked
 * for a whole vblank, the main loop ran at half rate and its menu froze. RECOMP_READ_NOWAIT=1:
 * the old behaviour (every Read returns at once) for comparison. */
static __thread uint64_t ctrl_read_edge[4], touch_read_edge[2];
static void read_wait(uint64_t *last) {
	static int nowait = -1; if (nowait < 0) nowait = getenv("RECOMP_READ_NOWAIT") != 0;
	if (nowait) return;
	uint64_t now = vb_now();
	if (*last == now) { vb_wait_until(now + 1, 0); now = vb_now(); }   /* count 0: paces the thread, doesn't advance the fixed clock (a polling pad thread made guest time run 8x fast) */
	*last = now;
}
static void h_ctrl_read(Cpu *c) { read_wait(&ctrl_read_edge[A0 & 3]); h_ctrl_peek(c); }
static void h_audio_adopt(Cpu *c) { RET(1); }
#define SCE_CTRL_ERROR_INVALID_ARG 0x80340001u
/* PS TV mode. A game that reads pad port 1 and gets an answer takes itself for a PS TV with a paired pad: no touch screen
 * at all, and the touch screen's actions on buttons, its own PS TV layout (Killzone 1.12 looks once at boot; 1.00 never
 * does). A handheld's port 1 fails. Settings "controller_layout" (the game's own; the menu's Game page offers it once the
 * game has looked): 0 auto, the default: a PS TV when a gamepad is connected as the game looks, so a controller player
 * gets buttons and a keyboard-and-mouse player keeps clicking; 1 touch: a handheld, as before; 2 buttons: a PS TV.
 * Either way keyboard, mouse aim and controller go on feeding the same pad: only the mouse's touches are gone in PS TV
 * mode (menus by d-pad or keys). Decided at the game's first look: a change applies on its next start. Port 1 reads
 * the same input as port 0 (h_ctrl_peek reads every port alike); ports 2-4 stay unpaired. */
static int pstv_looked;
int display_pstv_looked(void) { return __atomic_load_n(&pstv_looked, __ATOMIC_RELAXED); }
static int pstv_mode(void) {
	static int v = -1;
	if (v < 0) {
		extern int settings_int(const char *, int); extern int window_gamepad_connected(void);
		const int s = settings_int("controller_layout", 0); v = s == 2 || (s == 0 && window_gamepad_connected());
		fprintf(stderr, "    ctrl: the game asks for a PS TV's pad: %s (controller_layout %d)\n", v ? "answered, a PS TV: its button layout, no touch" : "refused, a handheld: touch", s);
	}
	return v;
}
static int pstv_port_ok(uint32_t port) { if (!port) return 1; __atomic_store_n(&pstv_looked, 1, __ATOMIC_RELAXED); return port == 1 && pstv_mode(); }
static void h_ctrl_read2(Cpu *c) { if (!pstv_port_ok(A0)) { RET(SCE_CTRL_ERROR_INVALID_ARG); return; } h_ctrl_read(c); }
static void h_ctrl_peek2(Cpu *c) { if (!pstv_port_ok(A0)) { RET(SCE_CTRL_ERROR_INVALID_ARG); return; } h_ctrl_peek(c); }
/* SceTouchData (0x90 bytes, VitaSDK psp2/touch.h): u64 timestamp, u32 status, u32 report count, 8
 * reports {u8 id, u8 force, s16 x, s16 y, u8 reserved[8], u16 info}. RECOMP_TOUCH="from-to:x,y;..."
 * (frame ranges, front panel coordinates 1920x1088) touches the front panel in scripted runs.
 * Each new contact gets a new report id (counting up modulo 128, the id space SceSystemGesture
 * tracks), as the panels give each touch its own; not a fixed one: gesture.c follows a touch by its id and,
 * like the firmware, won't restart a DRAG for an id whose drag already ended -- with every touch
 * reported as id 1, Killzone: Mercenary's second drag would never have begun. */
static void h_touch_peek(Cpu *c) {   /* (port, SceTouchData *, count) */
	/* the contacts on this panel now: the replay's, else the window's (mouse, gamepad touchpad, the
	 * bumper cursor: window_touch_contacts), else the script's (front only, one). A contact keeps its
	 * report id while held; each new one takes the next (one contact at a time -- every recorded and
	 * scripted session so far -- numbers exactly as before multi-touch) */
	static char *script; static int script_read;
	static uint8_t contact[2];
	static struct { int key; uint8_t id; } live[2][TOUCH_SLOTS]; static int nlive[2], nrec[2];
	if (!script_read) { script_read = 1; script = getenv("RECOMP_TOUCH"); }
	uint32_t port = A0 & 1;
	int keys[TOUCH_SLOTS], xs[TOUCH_SLOTS], ys[TOUCH_SLOTS], n = 0; uint8_t ids[TOUCH_SLOTS];
	extern int window_touch_contacts(int port, int *keys, int *xs, int *ys, int max);
	n = window_touch_contacts((int)port, keys, xs, ys, TOUCH_SLOTS);
	for (const char *q = script; port == 0 && !n && q && *q; ) {
		unsigned long a = strtoul(q, (char **)&q, 10), z = a, x = 0, y = 0;
		if (*q == '-') z = strtoul(q + 1, (char **)&q, 10);
		if (*q == ':') x = strtoul(q + 1, (char **)&q, 10);
		if (*q == ',') y = strtoul(q + 1, (char **)&q, 10);
		if (frames >= a && frames <= z) { keys[0] = 0; xs[0] = (int)x; ys[0] = (int)y; n = 1; break; }
		if (*q == ';') q++; else break;
	}
	input_rec_init();
	int played = 0;
	if (play_loaded) {   /* the recording's contacts, slot by slot, with its ids */
		int m = 0;
		for (uint32_t sl = 0; sl < TOUCH_SLOTS; sl++) {
			int pdown = 0, px = 0, py = 0; uint8_t pid = 0;
			if (!play_touch(port, sl, &pdown, &pid, &px, &py)) continue;
			played = 1;
			if (pdown) { keys[m] = 1000 + pid; xs[m] = px; ys[m] = py; ids[m] = pid; m++; }
		}
		if (played) n = m;
	}
	if (!played)
		for (int i = 0; i < n; i++) {
			int k = -1; for (int j = 0; j < nlive[port]; j++) if (live[port][j].key == keys[i]) k = j;
			ids[i] = k >= 0 ? live[port][k].id : (contact[port] = (contact[port] + 1) & 0x7f);
		}
	/* RECOMP_SEQ_ON_RELEASE=1 (a VERIFY build): a front-panel contact lifting arms the call-sequence record
	 * (verify.c) -- what the game does with a finished stroke, for one that it recognises and one that it
	 * doesn't (Uncharted's traced shapes failed from the mouse and the stick cursor, 2026-10-13); the key
	 * says which input it was (1 the mouse, 2 the stick cursor, 10+ a touchpad finger) */
	if (port == 0 && !n && nlive[0]) {
		static int on = -1; if (on < 0) on = getenv("RECOMP_SEQ_ON_RELEASE") != 0;
		extern void (*hook_seq_arm)(const char *);
		if (on && hook_seq_arm) { static char why[64]; snprintf(why, sizeof why, "touch release, key %d, frame %llu", live[0][0].key, (unsigned long long)frames); hook_seq_arm(why); }
	}
	for (int i = 0; i < n; i++) { live[port][i].key = keys[i]; live[port][i].id = ids[i]; }
	nlive[port] = n;
	if (rec_file) {   /* a line per contact; a slot that just emptied says so; an empty panel, one "up" line as before */
		for (int i = 0; i < n; i++) fprintf(rec_file, "T %llu %u 1 %u %d %d %d\n", (unsigned long long)frames, port, ids[i], xs[i], ys[i], i);
		for (int i = n; i < nrec[port]; i++) fprintf(rec_file, "T %llu %u 0 %u 0 0 %d\n", (unsigned long long)frames, port, contact[port], i);
		if (!n && !nrec[port]) fprintf(rec_file, "T %llu %u 0 %u 0 0 0\n", (unsigned long long)frames, port, contact[port]);
		nrec[port] = n;
	}
	/* RECOMP_LOG_TOUCH=1: the reports a panel gives the game, whenever they change (port, count, id@x,y ...) */
	static int logt = -1; if (logt < 0) logt = getenv("RECOMP_LOG_TOUCH") != 0;
	if (logt) {
		static char last[2][160]; char cur[160]; int o = snprintf(cur, sizeof cur, "%d", n);
		for (int i = 0; i < n && o < (int)sizeof cur; i++) o += snprintf(cur + o, sizeof cur - (size_t)o, " %u@%d,%d", ids[i], xs[i], ys[i]);
		if (strcmp(cur, last[port])) { fprintf(stderr, "    touch: frame %llu port %u: %s%s\n", (unsigned long long)frames, port, cur, played ? " (replay)" : ""); snprintf(last[port], sizeof last[port], "%s", cur); }
	}
	for (uint32_t i = 0; i < A2; i++) {
		uint32_t p = A1 + 0x90 * i;
		memset(G(p), 0, 0x90); WR64(p, guest_time_us());
		WR32(p + 12, (uint32_t)n);
		for (int k = 0; k < n; k++) {
			uint32_t r = p + 16 + 0x10 * (uint32_t)k;
			WR8(r, ids[k]); WR8(r + 1, 128); WR16(r + 2, (uint16_t)xs[k]); WR16(r + 4, (uint16_t)ys[k]);
		}
	}
	RET(A2);
}
static void h_touch_read(Cpu *c) { read_wait(&touch_read_edge[A0 & 1]); h_touch_peek(c); }
static void h_touch_panel_info(Cpu *c) {   /* front panel 0..1919 x 0..1087; back 0..1919 x 108..889 (its 1920x782 rows sit inside the front's range) */
	memset(G(A1), 0, 0x30);
	WR16(A1, 0); WR16(A1 + 2, A0 ? 108 : 0); WR16(A1 + 4, 1919); WR16(A1 + 6, A0 ? 889 : 1087);
	WR16(A1 + 8, 0); WR16(A1 + 10, A0 ? 108 : 0); WR16(A1 + 12, 1919); WR16(A1 + 14, A0 ? 889 : 1087);
	RET(0);
}
/* sceDisplayGetRefreshRate(float *): the panel's nominal rate, 59.94 Hz, as the SDK reports it (a
 * theoretical value; the host's vblank grid above runs at 60) */
static void h_refresh_rate(Cpu *c) { if (!A0) { RET(0x80290002u); return; } float hz = 59.94f; memcpy(G(A0), &hz, 4); RET(0); }   /* no pointer: SCE_DISPLAY_ERROR_INVALID_ADDR */
/* sceCtrlSetSamplingMode(mode): 0 digital, 1 analog, 2 analog wide; returns the PREVIOUS mode (the
 * console starts digital). The sticks are reported either way -- a game that left the pad digital
 * doesn't read them */
static uint32_t ctrl_mode;
static void h_ctrl_mode(Cpu *c) { if (A0 > 2) { RET(0x80340001u); return; } RET(__atomic_exchange_n(&ctrl_mode, A0, __ATOMIC_RELAXED)); }   /* SCE_CTRL_ERROR_INVALID_ARG */
/* sceTouchSetSamplingState(port, state): the host samples the panels whenever asked */
static void h_touch_sampling(Cpu *c) { RET(A0 > 1 ? 0x80350001u : 0); }   /* SCE_TOUCH_ERROR_INVALID_ARG */

/* ---- SceMotion. With a gamepad's gyro live (motion.c: a DS4/DualSense's gyroscope and accelerometer,
 * fused), its state; else -- no pad, no gyro, headless runs, the gate -- a console held still, lying
 * flat and face up: the accelerometer reads gravity (0, 0, -1 g), no rotation, identity orientation
 * (quaternion x, y, z, w = 0, 0, 0, 1; both matrices identity), basic orientation "screen up" (0, 0, 1).
 * A recording's M lines replay a session's motion. GetState before StartSampling is the console's
 * NOT_SAMPLING error. SceMotionState (0xf8 bytes): u32 timestamp, acceleration +4, angular velocity
 * +16, quaternion +40, rotation matrix +56, NED matrix +120 (no magnetometer: the rotation matrix
 * again), basic orientation +188, u64 host timestamp +200, u8 magnetic field stability +244 (0
 * unstable). */
static int motion_on;
static int motion_log(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_LOG_MOTION") != 0; return v; }   /* RECOMP_LOG_MOTION=1: sampling on/off and resets too */
static void h_motion_start(Cpu *c) { motion_on = 1; if (motion_log()) fprintf(stderr, "    motion: sampling started at frame %llu (lr %#x)\n", (unsigned long long)frames, c->r[14]); RET(0); }
static void h_motion_stop(Cpu *c) { motion_on = 0; if (motion_log()) fprintf(stderr, "    motion: sampling stopped at frame %llu (lr %#x)\n", (unsigned long long)frames, c->r[14]); RET(0); }
static void put_f(uint32_t a, float f) { memcpy(G(a), &f, 4); }
extern int motion_live(float acc[3], float gyr[3], float quat[4]);
extern void motion_matrix(const float quat[4], float m[16]);
extern void motion_recenter(void);
/* the motion state the game sees now: 1 with a live pad or a replayed one (written to a recording) */
static int motion_now(float a[3], float g[3], float q[4]) {
	input_rec_init();
	static int logm = -1; if (logm < 0) logm = getenv("RECOMP_LOG_MOTION") != 0;   /* RECOMP_LOG_MOTION=1: every 60th state the game reads */
	if (play_loaded && play_motion(a, g, q)) {
		if (logm) { static unsigned n; if (n++ % 60 == 0) fprintf(stderr, "    motion: frame %llu (replay) acc %.3f %.3f %.3f gyro %.3f %.3f %.3f quat %.3f %.3f %.3f %.3f\n", (unsigned long long)frames, a[0], a[1], a[2], g[0], g[1], g[2], q[0], q[1], q[2], q[3]); }
		return 1;
	}
	if (!motion_live(a, g, q)) return 0;
	if (logm) { static unsigned n; if (n++ % 60 == 0) fprintf(stderr, "    motion: frame %llu (live) acc %.3f %.3f %.3f gyro %.3f %.3f %.3f quat %.3f %.3f %.3f %.3f\n", (unsigned long long)frames, a[0], a[1], a[2], g[0], g[1], g[2], q[0], q[1], q[2], q[3]); }
	if (rec_file) fprintf(rec_file, "M %llu %g %g %g %g %g %g %g %g %g %g\n", (unsigned long long)frames, a[0], a[1], a[2], g[0], g[1], g[2], q[0], q[1], q[2], q[3]);
	return 1;
}
/* basic orientation (sceMotionGetBasicOrientation): the device axis gravity lies along, once it's
 * within 20 degrees of one (the SDK's default angle threshold), signed so that lying flat face up
 * is (0, 0, 1); in between, the last one */
static float basic_or[3] = {0, 0, 1};
static void basic_update(const float a[3]) {
	float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); if (n < 0.1f) return;
	int k = 0; for (int i = 1; i < 3; i++) if (fabsf(a[i]) > fabsf(a[k])) k = i;
	if (fabsf(a[k]) / n < 0.9397f) return;   /* cos 20 deg */
	basic_or[0] = basic_or[1] = basic_or[2] = 0; basic_or[k] = a[k] > 0 ? -1.0f : 1.0f;
}
/* SceMotionState is built here and copied into guest memory in one go: zeroed in place, then filled, it
 * read as all zeros for a moment -- and Uncharted reads it from another thread while its double-buffered
 * "MotionSensorQuery" job may still be writing (more often at 60 fps): a zero quaternion made its motion
 * vectors' length jump 1 -> 0 -> 1, twice in a row: its "shake" gesture, which can dismiss a touch prompt
 * (the machete minigame's, 2026-10-12) */
static void ms_put_f(uint8_t *b, uint32_t off, float v) { memcpy(b + off, &v, 4); }
static void h_motion_state(Cpu *c) {
	if (!motion_on) { RET(0x80360206u); return; }   /* SCE_MOTION_ERROR_NOT_SAMPLING */
	if (!A0) { RET(0x80360200u); return; }   /* SCE_MOTION_ERROR_DATA_INVALID */
	uint32_t p = A0; uint64_t t = guest_time_us();
	uint8_t b[0xf8]; memset(b, 0, sizeof b);
	uint32_t t32 = (uint32_t)t; memcpy(b, &t32, 4); memcpy(b + 200, &t, 8);
	float a[3], g[3], q[4];
	if (!motion_now(a, g, q)) {
		ms_put_f(b, 12, -1.0f);   /* acceleration z */
		ms_put_f(b, 52, 1.0f);    /* quaternion w */
		for (int i = 0; i < 4; i++) { ms_put_f(b, 56 + 20 * i, 1.0f); ms_put_f(b, 120 + 20 * i, 1.0f); }
		ms_put_f(b, 196, 1.0f);   /* basic orientation z */
	} else {
		float m[16]; motion_matrix(q, m); basic_update(a);
		for (int i = 0; i < 3; i++) { ms_put_f(b, 4 + 4 * i, a[i]); ms_put_f(b, 16 + 4 * i, g[i]); ms_put_f(b, 188 + 4 * i, basic_or[i]); }
		for (int i = 0; i < 4; i++) ms_put_f(b, 40 + 4 * i, q[i]);
		for (int i = 0; i < 16; i++) { ms_put_f(b, 56 + 4 * i, m[i]); ms_put_f(b, 120 + 4 * i, m[i]); }
	}
	memcpy(G(p), b, sizeof b);
	RET(0);
}
static void h_motion_basic(Cpu *c) {   /* (SceFVector3 *) */
	if (!motion_on) { RET(0x80360206u); return; }
	float a[3], g[3], q[4];
	int live = motion_now(a, g, q);
	if (live) basic_update(a);
	if (A0) { float v[3] = {0, 0, 1.0f}; if (live) for (int i = 0; i < 3; i++) v[i] = basic_or[i]; memcpy(G(A0), v, 12); }   /* (one copy: see h_motion_state) */
	RET(0);
}
/* sceMotionReset: the console restarts its orientation estimate; here, the heading recentres */
static void h_motion_reset(Cpu *c) { if (motion_log()) fprintf(stderr, "    motion: reset at frame %llu (lr %#x)\n", (unsigned long long)frames, c->r[14]); motion_recenter(); RET(0); }

/* set-up calls with nothing to set up (motion tuning). SceSystemGesture is gesture.c */
static void h_input_ok(Cpu *c) { RET(0); }

const HleEntry hle_display[] = {
	{"sceDisplaySetFrameBuf", h_set_frame_buf}, {"sceDisplayWaitVblankStart", h_wait_vblank},
	{"sceDisplayWaitSetFrameBuf", h_wait_setbuf}, {"sceDisplayWaitSetFrameBufMulti", h_wait_setbuf_multi},
	{"sceDisplayWaitVblankStartMulti", h_wait_vblank_multi},
	{"sceDisplayWaitVblankStartCB", h_wait_vblank_cb}, {"sceDisplayWaitVblankStartMultiCB", h_wait_vblank_multi_cb},
	{"sceDisplayWaitSetFrameBufCB", h_wait_setbuf_cb}, {"sceDisplayWaitSetFrameBufMultiCB", h_wait_setbuf_multi_cb},
	{"sceDisplayRegisterVblankStartCallback", h_register_vblank_cb}, {"sceDisplayUnregisterVblankStartCallback", h_unregister_vblank_cb},
	{"sceAudioOutOpenPort", h_audio_open}, {"sceAudioOutOutput", h_audio_output}, {"sceAudioOutSetVolume", h_audio_set_volume},
	{"sceCtrlPeekBufferPositive", h_ctrl_peek}, {"sceTouchPeek", h_touch_peek},
	{"sceCtrlReadBufferPositive", h_ctrl_read}, {"sceTouchRead", h_touch_read}, {"sceTouchGetPanelInfo", h_touch_panel_info},
	/* the "2" reads (Killzone 1.12): the same SceCtrlData, on a handheld only from port 0 -- the PS TV's paired pads are ports 1-4,
	 * and on a handheld those fail. Killzone 1.12 reads port 1 once at boot to tell the two apart: a success there made it a
	 * PS TV, which has no touch screen, and it never created its touch recognizers (no menu answered a tap) */
	{"sceCtrlReadBufferPositive2", h_ctrl_read2}, {"sceCtrlPeekBufferPositive2", h_ctrl_peek2},
	{"sceAudioOutGetAdopt", h_audio_adopt},   /* (port type): 1, the type can be opened -- only voice chat or the system's music takes one */
	{"sceDisplayGetRefreshRate", h_refresh_rate},
	{"sceCtrlSetSamplingMode", h_ctrl_mode}, {"sceCtrlSetSamplingModeExt", h_ctrl_mode}, {"sceTouchSetSamplingState", h_touch_sampling},
	{"sceMotionStartSampling", h_motion_start}, {"sceMotionStopSampling", h_motion_stop}, {"sceMotionGetState", h_motion_state},
	{"sceMotionGetBasicOrientation", h_motion_basic}, {"sceMotionReset", h_motion_reset}, {"sceMotionSetDeadband", h_input_ok}, {"sceMotionSetTiltCorrection", h_input_ok},
	{0, 0},
};
