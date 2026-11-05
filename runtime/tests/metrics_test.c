/*
 * runtime/core/metrics.c's performance overlay, with no game and no window: game frames are fed at known
 * intervals (sleeps), then the full overlay's text and the frame-time graph's data are read back.
 *
 *   - the frame-time history (metrics_frame_times): one sample per frame, oldest first, the long frame where
 *     it was fed, and `max` keeping the newest ones;
 *   - the full level's CPU line: the process's resident memory (plat_rss_bytes) on every platform, and a real
 *     thread count where it is cheap (this test holds a second thread), else no thread count at all (Windows);
 *   - one stats refresh stays cheap: it runs on the game or window thread twice a second.
 *   - the full overlay's snapshot (metrics_snapshot): 1%/0.1% lows from known frame times (metrics_lows), the
 *     renderer's per-frame costs averaged, and the machine sampler -- started by the full level, its numbers sane
 *     (CPU within 0..100%, RAM used within installed, GPU values a number or -1), stopped when the level drops.
 *
 * Linux / macOS, from this folder:
 *     cc -O1 -g -Wall -Wno-unused-function -I ../core -I ../platform metrics_test.c ../platform/platform_posix.c -o /tmp/metrics_test -lpthread -ldl && /tmp/metrics_test
 * Windows (llvm-mingw):
 *     clang -O1 -g -Wall -Wno-unused-function -I ../core -I ../platform metrics_test.c ../platform/platform_win32.c -o metrics_test.exe -static -lpthread -lwinmm && ./metrics_test.exe
 * -DFAIL_BEFORE builds only the checks that also compile against a runtime without these additions.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/metrics.c"

/* the symbols metrics.c takes from the rest of the runtime */
uint64_t guest_time_us_peek(void) { return now_us(); }
void gfx_gpu_get_counts(uint32_t *gpu_draws, uint32_t *fallback_draws) { *gpu_draws = 0; *fallback_draws = 0; }
static int costs_on;   /* 0: the software backend (no costs); 1: fixed costs every frame */
int gfx_gpu_overlay_costs(uint32_t *builds, uint32_t *uploads, uint32_t *skipped, uint64_t *build_us, uint64_t *wait_us) {
	if (!costs_on) return 0;
	*builds = 2; *uploads = 3; *skipped = 1; *build_us = 500; *wait_us = 250; return 1;
}
int settings_has(const char *key) { (void)key; return 0; }
int settings_int(const char *key, int def) { (void)key; return def; }

static int fails, oks;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (ok) oks++; else fails++; }
static void sleep_ms(int ms) { plat_sleep_ns((uint64_t)ms * 1000000u); }

static pthread_mutex_t hold_mx = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t hold_cv = PTHREAD_COND_INITIALIZER; static int hold_done;
static void *holder(void *arg) {   /* a second thread that just waits: the count must see it */
	(void)arg; pthread_mutex_lock(&hold_mx); while (!hold_done) pthread_cond_wait(&hold_cv, &hold_mx); pthread_mutex_unlock(&hold_mx); return 0;
}

int main(void) {
	static char env_m[] = "RECOMP_METRICS=0", env_o[] = "RECOMP_OSD=2", env_h[] = "RECOMP_HEADLESS=0";
	putenv(env_m); putenv(env_o); putenv(env_h);   /* a window would read these numbers: metrics_on, full level */
	check(metrics_on() == 1, "metrics are on (as with a window)");
	check(metrics_osd_level() == 2, "RECOMP_OSD=2 selects the full level");

	pthread_t t; pthread_create(&t, 0, holder, 0);

	/* frame 0, then ten frames: 3 ms apart, except the sixth, 40 ms after the fifth. Frame 0 refreshes the stats
	 * (the first tick), and its cost lands in frame 1's duration: it is timed on its own first */
	sleep_ms(1);
	uint64_t t0 = now_us(); metrics_game_frame(); uint64_t tick_us = now_us() - t0;   /* the first frame always refreshes the stats */
	printf("     first frame with a stats refresh: %llu us\n", (unsigned long long)tick_us);
	check(tick_us < 5000, "a stats refresh costs under 5 ms (it runs on the game or window thread twice a second)");
	for (int i = 1; i <= 10; i++) { sleep_ms(i == 6 ? 40 : 3); metrics_game_frame(); }

#ifndef FAIL_BEFORE
	float ms[64]; int n = metrics_frame_times(ms, 64);
	printf("     frame times (%d):", n); for (int i = 0; i < n; i++) printf(" %.1f", ms[i]); printf("\n");
	check(n == 10, "ten frames after the first give ten samples");
	int worst = 0; for (int i = 1; i < n; i++) if (ms[i] > ms[worst]) worst = i;
	check(n == 10 && worst == 5, "the long frame is the sixth sample: oldest first");
	check(n == 10 && ms[5] >= 39.0f, "the long frame measures at least its 40 ms sleep");
	int short_ok = 1; for (int i = 0; i < n; i++) if (i != 5 && ms[i] < 2.9f) short_ok = 0;
	check(n == 10 && short_ok, "every other frame measures at least its 3 ms sleep");
	float last4[4]; int n4 = metrics_frame_times(last4, 4);
	check(n4 == 4 && n == 10 && !memcmp(last4, ms + 6, sizeof last4), "max 4 returns the newest four, oldest first");
	check(metrics_frame_times(ms, 0) == 0 && metrics_frame_times(0, 8) == 0, "no room: nothing written");
#endif

	/* the overlay's text is rebuilt at most twice a second: wait past that, then one more frame */
	sleep_ms(550); metrics_game_frame();
	char lines[6][40]; int nl = metrics_osd_lines(lines, 6);
	for (int i = 0; i < nl; i++) printf("     osd: %s\n", lines[i]);
	check(nl == 6, "the full level has six lines");
	int cpu = -1, threads = -1; unsigned mem = 0;
#ifdef FAIL_BEFORE
	const int known = 1;   /* (no plat_thread_count to ask: the count is expected) */
#else
	const int known = plat_thread_count() > 0;
#endif
	int got = 0;
	if (nl == 6 && known) got = sscanf(lines[4], "CPU %d%% THREADS %d MEM %uMB", &cpu, &threads, &mem) == 3;
	else if (nl == 6) got = sscanf(lines[4], "CPU %d%% MEM %uMB", &cpu, &mem) == 2 && !strstr(lines[4], "THREADS");
	if (known) check(got && threads >= 2, "the CPU line counts this process's threads (main + holder)");
	else check(got, "no cheap thread count here: the CPU line leaves it out instead of showing 0");
	check(got && mem > 0, "the CPU line shows the process's resident memory in MB");

#ifndef FAIL_BEFORE
	/* ---- the full overlay's snapshot (metrics_snapshot) */
	{	/* 1% and 0.1% lows from known frame times, given out of order: 979 at 10 ms, 20 at 40 ms, one at 100 ms */
		static float ft[1000]; float l1, l01;
		for (int i = 0; i < 1000; i++) ft[i] = i == 0 ? 100.0f : i <= 20 ? 40.0f : 10.0f;
		metrics_lows(ft, 1000, &l1, &l01);
		printf("     lows of 1000 frames: 1%% %.2f fps, 0.1%% %.2f fps\n", l1, l01);
		check(l1 > 24.99f && l1 < 25.01f, "1% low: the slowest 10 frames are 40 ms ones -> 25 fps");
		check(l01 > 9.99f && l01 < 10.01f, "0.1% low: the slowest frame is the 100 ms one -> 10 fps");
		for (int i = 0; i < 200; i++) ft[i] = i < 2 ? 50.0f : 10.0f;
		metrics_lows(ft, 200, &l1, &l01);
		check(l1 > 19.99f && l1 < 20.01f, "200 frames, exactly 1% at 50 ms: 1% low 20 fps");
		metrics_lows(ft, 99, &l1, &l01);
		check(l1 < 0 && l01 < 0, "under 100 frames: no lows (-1)");
	}
	MetricsSnapshot sn; metrics_snapshot(&sn);
	check(sn.builds < 0 && sn.tex_uploads < 0, "no renderer costs given (software backend): -1, drawn as -");

	/* the renderer's costs: 2 builds, 3 uploads, 1 skipped draw, 0.5 ms building, 0.25 ms waiting, every frame */
	costs_on = 1;
	for (int i = 0; i < 20; i++) { sleep_ms(5); metrics_game_frame(); }
	sleep_ms(550); metrics_game_frame(); metrics_snapshot(&sn);
	printf("     renderer per frame: %.2f builds %.3f ms, %.2f uploads, %.3f ms waits, %.2f skipped\n", sn.builds, sn.build_ms, sn.tex_uploads, sn.waits_ms, sn.skipped);
	check(sn.builds > 1.99f && sn.builds < 2.01f && sn.tex_uploads > 2.99f && sn.tex_uploads < 3.01f && sn.skipped > 0.99f && sn.skipped < 1.01f, "renderer counts averaged per frame");
	check(sn.build_ms > 0.499f && sn.build_ms < 0.501f && sn.waits_ms > 0.249f && sn.waits_ms < 0.251f, "renderer times averaged per frame, in ms");

	/* the machine sampler: started by the full level, delivers within a couple of seconds (the GPU monitor's first
	 * open can take hundreds of ms); two samples make the CPU percentages */
	metrics_set_gpu_name("no such test GPU"); metrics_snapshot(&sn);
	for (int i = 0; i < 60 && !(sn.machine && sn.cpu_total_pct >= 0); i++) { sleep_ms(50); metrics_game_frame(); metrics_snapshot(&sn); }
	printf("     machine: cpu '%s' %.1f%% (game %.1f%%), ram %.0f/%.0f MB (game %.0f), threads %d\n", sn.cpu_name, sn.cpu_total_pct, sn.cpu_game_pct, sn.ram_used_mb, sn.ram_total_mb, sn.ram_game_mb, sn.threads);
	printf("     gpu '%s': %.1f%%, vram %.0f/%.0f MB, %.1f C, %.1f W, %.0f MHz\n", sn.gpu_name, sn.gpu_pct, sn.vram_used_mb, sn.vram_total_mb, sn.gpu_temp_c, sn.gpu_power_w, sn.gpu_clock_mhz);
	check(sn.machine == 1 && metrics_sampler_running(), "the full level runs the machine sampler");
	check(sn.cpu_total_pct >= 0 && sn.cpu_total_pct <= 100.5f, "system CPU in 0..100%");
	check(sn.cpu_game_pct >= 0 && sn.cpu_game_pct <= 100.5f, "this game's CPU in 0..100% of all processors");
	check(sn.ram_total_mb > 0 && sn.ram_used_mb > 0 && sn.ram_used_mb <= sn.ram_total_mb, "system RAM: used within installed");
	check(sn.ram_game_mb > 0 && sn.ram_game_mb <= sn.ram_total_mb, "this game's RAM within installed");
#if defined(__x86_64__) || defined(__i386__)
	check(sn.cpu_name[0] != 0, "the CPU names itself (x86 brand string)");
#endif
	int gpu_sane = 1; const float gv[6] = {sn.gpu_pct, sn.vram_used_mb, sn.vram_total_mb, sn.gpu_temp_c, sn.gpu_power_w, sn.gpu_clock_mhz};
	for (int i = 0; i < 6; i++) if (!(gv[i] == -1.0f || gv[i] >= 0)) gpu_sane = 0;
	if (sn.gpu_pct > 100.5f || (sn.vram_used_mb >= 0 && sn.vram_total_mb > 0 && sn.vram_used_mb > sn.vram_total_mb)) gpu_sane = 0;
	check(gpu_sane, "GPU numbers are each a value or -1 (unknown), load within 100%, VRAM used within total");
	check(!strcmp(sn.gpu_name, "no such test GPU"), "the snapshot carries the renderer's GPU name");
	check(sn.nframes > 0 && sn.nframes <= METRICS_GRAPH, "the graph holds the newest frames");

	/* leaving the full level ends the sampler at its next wake-up (twice a second) */
	metrics_osd_set(1);
	for (int i = 0; i < 30 && metrics_sampler_running(); i++) sleep_ms(50);
	check(!metrics_sampler_running(), "leaving the full level stops the machine sampler");
	metrics_snapshot(&sn);
	check(sn.machine == 0, "...and the snapshot says its machine numbers are stale");
#endif

	pthread_mutex_lock(&hold_mx); hold_done = 1; pthread_cond_signal(&hold_cv); pthread_mutex_unlock(&hold_mx);
	pthread_join(t, 0);
	if (fails) { printf("%d checks FAILED (%d ok)\n", fails, oks); return 1; }
	printf("all passed (%d ok)\n", oks);
	return 0;
}
