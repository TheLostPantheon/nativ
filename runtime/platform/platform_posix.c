/* platform.h on POSIX: macOS, Linux (and Android) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* glibc: pthread_getname_np */
#endif
#include "platform.h"
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#if !defined(__ANDROID__)
#include <execinfo.h>
#endif

void *plat_reserve(size_t bytes) {
	void *p = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	return p == MAP_FAILED ? 0 : p;
}
void plat_thread_name(char *buf, size_t n) { if (!n) return; buf[0] = 0; pthread_getname_np(pthread_self(), buf, n); }
int plat_protect(void *p, size_t bytes, int writable) { return mprotect(p, bytes, writable < 0 ? PROT_NONE : writable ? PROT_READ | PROT_WRITE : PROT_READ); }
size_t plat_page_size(void) { long ps = sysconf(_SC_PAGESIZE); return ps > 0 ? (size_t)ps : 4096; }

#define MAX_HOOKS 4
static PlatFaultHook hooks[MAX_HOOKS]; static int nhooks;
static PlatCrashFn crash_fn;
/* one handler for SIGSEGV, SIGBUS (macOS reports a write to a read-only page as SIGBUS, Linux as SIGSEGV) and
 * SIGILL, on its own stack (a fault from a blown stack still reports) */
static void on_signal(int sig, siginfo_t *si, void *uc) {
	(void)uc;
	if (sig != SIGILL) {
		int saved = errno;
		for (int i = nhooks - 1; i >= 0; i--) if (hooks[i](si->si_addr)) { errno = saved; return; }   /* the access runs again */
		errno = saved;
	}
	if (crash_fn) crash_fn(sig == SIGILL ? "illegal instruction" : sig == SIGBUS ? "bus error" : "segmentation fault", si->si_addr);
	signal(sig, SIG_DFL);   /* the access re-runs on return and takes the default action */
}
static void install(void) {
	static int done; if (done) return; done = 1;
	static uint8_t altstack[1 << 16]; stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack };
	sigaltstack(&ss, 0);
	struct sigaction sa; memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = on_signal; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGILL, &sa, 0);
}
void plat_add_fault_hook(PlatFaultHook hook) { install(); if (nhooks < MAX_HOOKS) hooks[nhooks++] = hook; }
void plat_set_crash_handler(PlatCrashFn fn) { crash_fn = fn; install(); }
void plat_backtrace(FILE *f) {
#if !defined(__ANDROID__)
	void *bt[48]; int n = backtrace(bt, 48);
	fflush(f); backtrace_symbols_fd(bt, n, fileno(f));
#else
	(void)f;
#endif
}

int plat_cpu_count(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
double plat_cpu_seconds(void) {
	struct rusage ru; getrusage(RUSAGE_SELF, &ru);
	return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}
void plat_sleep_ns(uint64_t ns) {
	struct timespec ts = { (time_t)(ns / 1000000000u), (long)(ns % 1000000000u) };
	while (nanosleep(&ts, &ts) && errno == EINTR) ;
}

void plat_random(void *buf, size_t n) {
	FILE *f = fopen("/dev/urandom", "rb");
	size_t got = f ? fread(buf, 1, n, f) : 0;
	if (f) fclose(f);
	for (size_t i = got; i < n; i++) ((unsigned char *)buf)[i] = (unsigned char)(rand() ^ (time(0) >> (i % 24)));   /* (no urandom: weaker, still varied) */
}
