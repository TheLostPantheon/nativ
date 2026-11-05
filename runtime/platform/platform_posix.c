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
#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>

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
#ifdef __APPLE__
int plat_thread_count(void) {
	thread_array_t threads; mach_msg_type_number_t n;
	if (task_threads(mach_task_self(), &threads, &n) != KERN_SUCCESS) return 0;
	for (mach_msg_type_number_t i = 0; i < n; i++) mach_port_deallocate(mach_task_self(), threads[i]);
	vm_deallocate(mach_task_self(), (vm_address_t)threads, (vm_size_t)n * sizeof(thread_t));
	return (int)n;
}
uint64_t plat_rss_bytes(void) {
	mach_task_basic_info_data_t info; mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
	return task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &n) == KERN_SUCCESS ? (uint64_t)info.resident_size : 0;
}
#else   /* Linux and Android: procfs */
int plat_thread_count(void) {
	FILE *f = fopen("/proc/self/status", "r"); if (!f) return 0;
	char line[64]; int n = 0;
	while (fgets(line, sizeof line, f)) if (!strncmp(line, "Threads:", 8)) { n = atoi(line + 8); break; }
	fclose(f); return n;
}
uint64_t plat_rss_bytes(void) {   /* statm's second field: resident pages */
	FILE *f = fopen("/proc/self/statm", "r"); if (!f) return 0;
	unsigned long long size = 0, resident = 0; int ok = fscanf(f, "%llu %llu", &size, &resident) == 2;
	fclose(f); return ok ? (uint64_t)resident * plat_page_size() : 0;
}
#endif
/* macOS: the uptime counter (a register read). Its clock_gettime(CLOCK_MONOTONIC) also reads the boot time through
 * gettimeofday on every call: the renderer's per-draw timers were 9% of Uncharted's main thread in Low Power Mode
 * (2026-11-01). Linux's CLOCK_MONOTONIC is already a vDSO read. */
uint64_t plat_now_us(void) {
#ifdef __APPLE__
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000u;
#else
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
#endif
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

/* ---- the machine, for the performance overlay's sampler thread (platform.h) */
void *plat_lib_open(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void *plat_lib_sym(void *lib, const char *sym) { return lib ? dlsym(lib, sym) : 0; }

#ifdef __APPLE__
int plat_system_cpu(uint64_t *busy, uint64_t *total) {   /* ticks */
	host_cpu_load_info_data_t li; mach_msg_type_number_t n = HOST_CPU_LOAD_INFO_COUNT;
	if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&li, &n) != KERN_SUCCESS) return 0;
	uint64_t u = li.cpu_ticks[CPU_STATE_USER], s = li.cpu_ticks[CPU_STATE_SYSTEM], ni = li.cpu_ticks[CPU_STATE_NICE], id = li.cpu_ticks[CPU_STATE_IDLE];
	*busy = u + s + ni; *total = u + s + ni + id; return 1;
}
int plat_mem_info(uint64_t *used, uint64_t *total) {   /* used: active + wired + compressed, as Activity Monitor's "Memory Used" roughly */
	uint64_t mem = 0; size_t len = sizeof mem;
	if (sysctlbyname("hw.memsize", &mem, &len, 0, 0) || !mem) return 0;
	vm_statistics64_data_t vs; mach_msg_type_number_t n = HOST_VM_INFO64_COUNT;
	if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &n) != KERN_SUCCESS) return 0;
	*used = ((uint64_t)vs.active_count + vs.wire_count + vs.compressor_page_count) * plat_page_size(); *total = mem; return 1;
}
void plat_cpu_name(char *out, size_t n) {
	if (!n) return;
	out[0] = 0; size_t len = n;
	if (sysctlbyname("machdep.cpu.brand_string", out, &len, 0, 0)) out[0] = 0;
}
#else   /* Linux and Android: procfs */
int plat_system_cpu(uint64_t *busy, uint64_t *total) {   /* USER_HZ ticks, /proc/stat's first line */
	FILE *f = fopen("/proc/stat", "r"); if (!f) return 0;
	unsigned long long v[8] = {0}; int got = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
	fclose(f);
	if (got < 4) return 0;
	uint64_t t = 0; for (int i = 0; i < 8; i++) t += v[i];
	*total = t; *busy = t - v[3] - v[4]; return 1;   /* (idle and iowait are not busy) */
}
int plat_mem_info(uint64_t *used, uint64_t *total) {   /* used: MemTotal - MemAvailable, as free(1)'s "used" */
	FILE *f = fopen("/proc/meminfo", "r"); if (!f) return 0;
	char line[128]; unsigned long long tot = 0, avail = 0; int have = 0;
	while (fgets(line, sizeof line, f)) {
		if (!strncmp(line, "MemTotal:", 9)) { tot = strtoull(line + 9, 0, 10); have |= 1; }
		else if (!strncmp(line, "MemAvailable:", 13)) { avail = strtoull(line + 13, 0, 10); have |= 2; }
	}
	fclose(f);
	if (have != 3 || !tot) return 0;
	*total = tot * 1024u; *used = (tot - (avail < tot ? avail : tot)) * 1024u; return 1;
}
void plat_cpu_name(char *out, size_t n) {
	if (!n) return;
	out[0] = 0;
#if defined(__x86_64__) || defined(__i386__)
	unsigned int r[12], a, b, c, d;
	if (__get_cpuid(0x80000000u, &a, &b, &c, &d) && a >= 0x80000004u) {
		for (unsigned int i = 0; i < 3; i++) __get_cpuid(0x80000002u + i, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
		char s[49]; memcpy(s, r, 48); s[48] = 0;
		const char *p = s; while (*p == ' ') p++;
		snprintf(out, n, "%s", p); return;
	}
#endif
	FILE *f = fopen("/proc/cpuinfo", "r"); if (!f) return;   /* ARM: "model name" on some kernels, "Hardware" on others */
	char line[256];
	while (fgets(line, sizeof line, f)) {
		if (strncmp(line, "model name", 10) && strncmp(line, "Hardware", 8)) continue;
		char *c = strchr(line, ':'); if (!c) continue;
		c++; while (*c == ' ' || *c == '\t') c++;
		size_t l = strlen(c); while (l && (c[l - 1] == '\n' || c[l - 1] == '\r')) c[--l] = 0;
		snprintf(out, n, "%s", c); break;
	}
	fclose(f);
}
#endif

/* the GPU: NVML on NVIDIA (gpumon_nvml.h); on Linux, amdgpu's sysfs files for AMD -- the card's busy percent, its VRAM,
 * and its hwmon temperature, power and clock. Intel and macOS give nothing here: -1. */
#include "gpumon_nvml.h"
struct PlatGpuMon { Nvml nv; char name[96]; char amd[64]; char hwmon[96]; };
static int read_ull(const char *path, unsigned long long *v) {
	FILE *f = fopen(path, "r"); if (!f) return 0;
	int ok = fscanf(f, "%llu", v) == 1; fclose(f); return ok;
}
PlatGpuMon *plat_gpu_open(const char *device_name) {
	PlatGpuMon *m = calloc(1, sizeof *m); if (!m) return 0;
	snprintf(m->name, sizeof m->name, "%s", device_name ? device_name : "");
#if !defined(__APPLE__)
	int want_amd = !m->name[0] || nvml_icontains(m->name, "AMD") || nvml_icontains(m->name, "Radeon");
	for (int i = 0; want_amd && i < 16 && !m->amd[0]; i++) {
		char p[96]; unsigned long long v;
		snprintf(p, sizeof p, "/sys/class/drm/card%d/device/gpu_busy_percent", i);
		if (!read_ull(p, &v)) continue;
		snprintf(m->amd, sizeof m->amd, "/sys/class/drm/card%d/device", i);
		snprintf(p, sizeof p, "%s/hwmon", m->amd);
		DIR *d = opendir(p); struct dirent *e;
		while (d && (e = readdir(d))) if (!strncmp(e->d_name, "hwmon", 5)) { snprintf(m->hwmon, sizeof m->hwmon, "%s/hwmon/%.16s", m->amd, e->d_name); break; }
		if (d) closedir(d);
	}
#endif
	return m;
}
void plat_gpu_sample(PlatGpuMon *m, PlatGpuSample *s) {
	s->load_pct = s->vram_used_mb = s->vram_total_mb = s->temp_c = s->power_w = s->clock_mhz = -1;
	if (!m) return;
	if (!m->amd[0]) { nvml_open(&m->nv, m->name); if (nvml_sample(&m->nv, s)) return; }
	if (!m->amd[0]) return;
	char p[160]; unsigned long long v;
	snprintf(p, sizeof p, "%s/gpu_busy_percent", m->amd); if (read_ull(p, &v)) s->load_pct = (float)v;
	snprintf(p, sizeof p, "%s/mem_info_vram_used", m->amd); if (read_ull(p, &v)) s->vram_used_mb = (float)(v / 1048576.0);
	snprintf(p, sizeof p, "%s/mem_info_vram_total", m->amd); if (read_ull(p, &v)) s->vram_total_mb = (float)(v / 1048576.0);
	if (!m->hwmon[0]) return;
	snprintf(p, sizeof p, "%s/temp1_input", m->hwmon); if (read_ull(p, &v)) s->temp_c = v / 1000.0f;   /* millidegrees */
	snprintf(p, sizeof p, "%s/power1_average", m->hwmon);
	if (read_ull(p, &v)) s->power_w = v / 1e6f;   /* microwatts */
	else { snprintf(p, sizeof p, "%s/power1_input", m->hwmon); if (read_ull(p, &v)) s->power_w = v / 1e6f; }
	snprintf(p, sizeof p, "%s/freq1_input", m->hwmon); if (read_ull(p, &v)) s->clock_mhz = v / 1e6f;   /* Hz */
}
void plat_gpu_close(PlatGpuMon *m) { free(m); }   /* (NVML's library stays loaded for the process) */
