/*
 * runtime/core/metrics.c's performance overlay, with no game and no window: game frames are fed at known
 * intervals (sleeps), then the full overlay's text and the frame-time graph's data are read back.
 *
 *   - the frame-time history (metrics_frame_times): one sample per frame, oldest first, the long frame where
 *     it was fed, and `max` keeping the newest ones;
 *   - the full level's CPU line: the process's resident memory (plat_rss_bytes) on every platform, and a real
 *     thread count where it is cheap (this test holds a second thread), else no thread count at all (Windows);
 *   - one stats refresh stays cheap: it runs on the game or window thread twice a second.
 *
 * Linux / macOS, from this folder:
 *     cc -O1 -g -Wall -Wno-unused-function -I ../core -I ../platform metrics_test.c ../platform/platform_posix.c -o /tmp/metrics_test -lpthread && /tmp/metrics_test
 * Windows (llvm-mingw):
 *     clang -O1 -g -Wall -Wno-unused-function -I ../core -I ../platform metrics_test.c ../platform/platform_win32.c -o metrics_test.exe -static -lpthread -lwinmm && ./metrics_test.exe
 * -DFAIL_BEFORE builds only the checks that also compile against a runtime without these additions.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/metrics.c"

/* the four symbols metrics.c takes from the rest of the runtime */
uint64_t guest_time_us_peek(void) { return now_us(); }
void gfx_gpu_get_counts(uint32_t *gpu_draws, uint32_t *fallback_draws) { *gpu_draws = 0; *fallback_draws = 0; }
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

	pthread_mutex_lock(&hold_mx); hold_done = 1; pthread_cond_signal(&hold_cv); pthread_mutex_unlock(&hold_mx);
	pthread_join(t, 0);
	if (fails) { printf("%d checks FAILED (%d ok)\n", fails, oks); return 1; }
	printf("all passed (%d ok)\n", oks);
	return 0;
}
