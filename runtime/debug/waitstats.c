/*
 * RECOMP_WAIT_STATS=<presents> (1: every 300): where each thread's time goes, a report every that many presents. Blocked
 * in the kernel's waits (wait_on: semaphores, mutexes, condition variables, event flags, pipes), asleep in a delay, or
 * queued for the renderer's lock; and how late a woken thread ran after its waker signalled it. The thread nearest 100%
 * busy is what the frame waits on; the wake latency is what one hand-off between threads costs on this machine (a
 * pthread condition variable's: ~2 us on the M4 Pro and under winpthreads alike, 0.35 us with Windows' own, 2026-10-29).
 * Busy is the thread's own CPU time from the OS (a fiber parked in its switch, a vblank wait: whatever the blocking,
 * it isn't CPU); the waits and their latency are the kernel's alone. Two clock reads a wait, and only with it set.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

typedef struct { char name[32]; uint64_t cpu_at;   /* the CPU time at the window's start */
#ifdef _WIN32
	HANDLE h;
#elif defined(__APPLE__)
	mach_port_t port;
#else
	clockid_t cid;
#endif
	uint64_t born, waits, wait_ns, lat_n, lat_ns, lat_max, sleeps, sleep_ns, locks, lock_ns, idle_since, holds, hold_ns, hold_max; } WsThread;   /* holds: the renderer lock's outermost holds, and how long */   /* idle_since: a wait, sleep or lock queue under way (its time so far counts at a report) */
#define WS_MAX 256
static WsThread *ws_threads[WS_MAX]; static int ws_n; static pthread_mutex_t ws_m = PTHREAD_MUTEX_INITIALIZER;
static __thread WsThread *ws_me; static __thread char ws_name[32];
static uint64_t ws_start;   /* the report window's start */
static WsThread *me(void);
void waitstats_name(const char *n) { snprintf(ws_name, sizeof ws_name, "%s", n ? n : ""); if (ws_me) snprintf(ws_me->name, sizeof ws_me->name, "%s", ws_name); else me(); }   /* (and the thread counted from its start: a fiber may never wait in the kernel) */   /* (kernel.c: a guest thread's name, as it starts: winpthreads won't read a name back) */
static int ws_every = -1;

int waitstats_on(void) {
	if (ws_every < 0) { const char *e = getenv("RECOMP_WAIT_STATS"); ws_every = e && *e && *e != '0' ? (atoi(e) > 1 ? atoi(e) : 300) : 0; }
	return ws_every;
}
uint64_t waitstats_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static uint64_t cpu_ns(WsThread *t) {   /* the thread's CPU time, user and system, from any thread */
#ifdef _WIN32
	FILETIME c, e, k, u; if (!t->h || !GetThreadTimes(t->h, &c, &e, &k, &u)) return 0;
	return ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100;
#elif defined(__APPLE__)
	thread_basic_info_data_t i; mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
	if (thread_info(t->port, THREAD_BASIC_INFO, (thread_info_t)&i, &n) != KERN_SUCCESS) return 0;
	return ((uint64_t)i.user_time.seconds + (uint64_t)i.system_time.seconds) * 1000000000ull + ((uint64_t)i.user_time.microseconds + (uint64_t)i.system_time.microseconds) * 1000ull;
#else
	struct timespec ts; if (clock_gettime(t->cid, &ts)) return 0; return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
static WsThread *me(void) {
	if (!ws_me) {
		WsThread *t = calloc(1, sizeof *t); if (!t) return 0;
#ifdef _WIN32
		t->h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
#elif defined(__APPLE__)
		t->port = mach_thread_self();
#else
		pthread_getcpuclockid(pthread_self(), &t->cid);
#endif
		t->cpu_at = cpu_ns(t);
		char n[32] = ""; if (!ws_name[0]) pthread_getname_np(pthread_self(), n, sizeof n);
		snprintf(t->name, sizeof t->name, "%s", ws_name[0] ? ws_name : n[0] ? n : "(host)"); t->born = waitstats_now();
		pthread_mutex_lock(&ws_m); if (ws_n < WS_MAX) ws_threads[ws_n++] = t; pthread_mutex_unlock(&ws_m);
		ws_me = t;
	}
	return ws_me;
}
#define ADD(f, v) __atomic_add_fetch(&m->f, (v), __ATOMIC_RELAXED)
void waitstats_begin(uint64_t t0) { WsThread *m = me(); if (m) __atomic_store_n(&m->idle_since, t0, __ATOMIC_RELAXED); }   /* a wait, sleep or lock queue starts */
static uint64_t counted(WsThread *m, uint64_t t0, uint64_t now) {   /* its time inside this report window (an earlier window counted the rest), and it's over */
	__atomic_store_n(&m->idle_since, 0, __ATOMIC_RELAXED);
	const uint64_t s = __atomic_load_n(&ws_start, __ATOMIC_RELAXED); return now - (t0 > s ? t0 : s);
}
void waitstats_wait(uint64_t t0, uint64_t woken_at) {   /* a kernel wait that began at t0; woken_at: when its waker signalled (0: a timeout, a deletion) */
	WsThread *m = me(); if (!m) return; const uint64_t now = waitstats_now();
	ADD(waits, 1); ADD(wait_ns, counted(m, t0, now));
	if (woken_at) { const uint64_t l = now > woken_at ? now - woken_at : 0; ADD(lat_n, 1); ADD(lat_ns, l); if (l > m->lat_max) m->lat_max = l; }
}
void waitstats_sleep(uint64_t t0) { WsThread *m = me(); if (!m) return; ADD(sleeps, 1); ADD(sleep_ns, counted(m, t0, waitstats_now())); }
void waitstats_held(uint64_t t0) { WsThread *m = me(); if (!m) return; const uint64_t d = waitstats_now() - t0; ADD(holds, 1); ADD(hold_ns, d); if (d > m->hold_max) m->hold_max = d; }   /* (gfx_dispatch.c: a hold of the renderer's lock ends) */
void waitstats_lock(uint64_t t0) { WsThread *m = me(); if (!m) return; ADD(locks, 1); ADD(lock_ns, counted(m, t0, waitstats_now())); }
static int by_busy(const void *a, const void *b) { const double x = *(const double *)a, y = *(const double *)b; return (x < y) - (x > y); }
void waitstats_present(void) {   /* display.c, each present: the report every ws_every */
	if (!waitstats_on()) return;
	static uint64_t frames;
	const uint64_t now = waitstats_now(), start = __atomic_load_n(&ws_start, __ATOMIC_RELAXED);
	if (!start) { __atomic_store_n(&ws_start, now, __ATOMIC_RELAXED); return; }
	if (++frames % (uint64_t)ws_every) return;
	const double win = (double)(now - start), nf = (double)ws_every;
	struct { double busy; int i; } rows[WS_MAX]; int nr = 0;
	pthread_mutex_lock(&ws_m);
	for (int i = 0; i < ws_n; i++) {
		WsThread *t = ws_threads[i]; const double span = (double)(now - (t->born > start ? t->born : start));
		if (span <= 0) continue;
		const uint64_t c = cpu_ns(t), used = c > t->cpu_at ? c - t->cpu_at : 0; t->cpu_at = c;
		rows[nr].busy = 100.0 * (used < span ? used : span) / span; rows[nr].i = i; nr++;
	}
	qsort(rows, (size_t)nr, sizeof rows[0], by_busy);
	fprintf(stderr, "    waits: %.0f presents in %.2f s (%.1f fps); per thread, busiest first: CPU busy%% | waits/frame, blocked ms/frame | sleeps ms/frame | renderer lock: queued/frame ms/frame, held/frame ms/frame max ms | wake latency avg/max us\n",
	        nf, win / 1e9, nf / (win / 1e9));
	for (int k = 0; k < nr && k < 14; k++) {
		WsThread *t = ws_threads[rows[k].i];
		fprintf(stderr, "    waits:   %-24s %5.1f%% | %6.1f %6.2f | %6.2f | %5.1f %6.2f, %6.1f %6.2f %6.2f | %6.1f %7.1f\n", t->name, rows[k].busy,
		        t->waits / nf, t->wait_ns / nf / 1e6, t->sleep_ns / nf / 1e6, t->locks / nf, t->lock_ns / nf / 1e6, t->holds / nf, t->hold_ns / nf / 1e6, t->hold_max / 1e6,
		        t->lat_n ? t->lat_ns / (double)t->lat_n / 1e3 : 0.0, t->lat_max / 1e3);
	}
	for (int i = 0; i < ws_n; i++) { WsThread *t = ws_threads[i]; t->waits = t->wait_ns = t->lat_n = t->lat_ns = t->lat_max = t->sleeps = t->sleep_ns = t->locks = t->lock_ns = t->holds = t->hold_ns = t->hold_max = 0; }
	pthread_mutex_unlock(&ws_m);
	__atomic_store_n(&ws_start, now, __ATOMIC_RELAXED);
}
