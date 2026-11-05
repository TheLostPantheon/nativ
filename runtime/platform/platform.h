/*
 * The runtime's small interface to the operating system: what differs between POSIX (macOS, Linux, Android) and
 * Windows in more than spelling. Two implementations, platform_posix.c and platform_win32.c; build.sh compiles
 * the one for its target. (What only differs in spelling -- mkdir's mode argument, pread, localtime_r -- the
 * Windows build gets from win32_compat.h instead, force-included, so the POSIX call sites stay as they are.)
 *
 * Guest memory: the Vita's 4 GB address space is one host reservation (main.c, recomp_mem), read-write and
 * backed by memory only where touched. Texture write tracking (guestwatch.c) makes pages of it read-only and
 * catches the first write to each: a write fault the runtime handles and retries, where any other fault is a
 * crash to report.
 */
#ifndef ATIV_PLATFORM_H
#define ATIV_PLATFORM_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* memory */
void *plat_reserve(size_t bytes);                       /* zeroed, read-write, backed on first touch; 0 on failure */
void plat_thread_name(char *buf, size_t n);   /* this thread's name ("" when it has none): kernel.c names host threads after guest threads -- diagnostics say which */
int plat_protect(void *p, size_t bytes, int writable);  /* whole pages, read-only (0), read-write (1) or no access (-1); 0 on success */
size_t plat_page_size(void);

/* faults. A write-fault hook gets every access fault first (newest hook first) and returns 1 when it made the
 * access valid -- the access then runs again -- else 0. A fault no hook takes, or an illegal instruction, goes
 * to the crash handler, which reports; the process then dies the platform's default way (a core, the debugger,
 * Windows Error Reporting). */
typedef int (*PlatFaultHook)(void *addr);
void plat_add_fault_hook(PlatFaultHook hook);
typedef void (*PlatCrashFn)(const char *what, void *addr);
void plat_set_crash_handler(PlatCrashFn fn);
void plat_backtrace(FILE *f);                            /* this thread's native stack, best effort */

/* time and processors */
int plat_cpu_count(void);                                /* online logical processors */
double plat_cpu_seconds(void);                           /* this process's CPU time so far, user + system */
int plat_thread_count(void);                             /* this process's threads right now; 0 when unknown */
uint64_t plat_rss_bytes(void);                           /* this process's resident memory (Windows: its working set); 0 when unknown */

/* the machine, for the full performance overlay's sampler thread (metrics.c, twice a second while it shows): each
 * of these may take milliseconds, so nothing on the game's or the window's thread calls them */
int plat_system_cpu(uint64_t *busy, uint64_t *total);   /* all processors' busy and total time so far, one unit (ratios only); 0 unknown */
int plat_mem_info(uint64_t *used_bytes, uint64_t *total_bytes);   /* physical memory in use and installed; 0 unknown */
void plat_cpu_name(char *out, size_t n);                 /* the CPU's brand string ("AMD Ryzen 7 7800X3D 8-Core Processor"); "" unknown */
typedef struct { float load_pct, vram_used_mb, vram_total_mb, temp_c, power_w, clock_mhz; } PlatGpuSample;   /* -1: unknown */
typedef struct PlatGpuMon PlatGpuMon;
PlatGpuMon *plat_gpu_open(const char *device_name);      /* the GPU the game renders on, by the renderer's device name ("" or NULL: a guess) */
void plat_gpu_sample(PlatGpuMon *m, PlatGpuSample *s);   /* every field -1 that this machine does not give; m may be NULL */
void plat_gpu_close(PlatGpuMon *m);
void *plat_lib_open(const char *name);                   /* a shared library by file name (dlopen / LoadLibrary); NULL if absent */
void *plat_lib_sym(void *lib, const char *sym);
uint64_t plat_now_us(void);                              /* a monotonic microsecond clock for intervals, the platform's cheapest (not one to
                                                          * compare with another module's clock: macOS counts uptime, without sleep) */
void plat_random(void *buf, size_t n);                   /* n unpredictable bytes (an install's own ids: apputil.c's console id) */
void plat_sleep_ns(uint64_t ns);                         /* as precise as the platform allows (Windows: a high-resolution waitable timer, not Sleep's 1-16 ms) */

#endif
