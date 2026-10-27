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
void plat_random(void *buf, size_t n);                   /* n unpredictable bytes (an install's own ids: apputil.c's console id) */
void plat_sleep_ns(uint64_t ns);                         /* as precise as the platform allows (Windows: a high-resolution waitable timer, not Sleep's 1-16 ms) */

#endif
