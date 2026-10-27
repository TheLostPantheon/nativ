/*
 * Force-included into every host file of a Windows build (build.sh TARGET=windows: -include win32_compat.h):
 * the POSIX calls the runtime makes that Windows has under another name or shape, so the call sites stay POSIX.
 * What differs in design rather than spelling is in platform.h instead. No <windows.h> here: it would bring its
 * macros (min, max, ERROR, near, ...) into every file; the few functions needing it live in win32_compat.c.
 * (Built with -D_FILE_OFFSET_BITS=64, off_t and lseek are 64-bit; -D_POSIX_THREAD_SAFE_FUNCTIONS gives
 * localtime_r/gmtime_r from mingw-w64's time.h.)
 */
#ifndef ATIV_WIN32_COMPAT_H
#define ATIV_WIN32_COMPAT_H
#ifdef _WIN32
#include <direct.h>
#include <intrin.h>   /* (declares _setjmp again -- SDL_assert.h includes it: first, so _setjmp's macro below meets no later declaration) */
#include <io.h>
#include <pthread.h>   /* (winpthreads' nanosleep, declared before the macro below) */
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>     /* (rename declared before its macro below) */
#include <sys/types.h>
#include <time.h>
#include <unistd.h>    /* (usleep, likewise) */

#define mkdir(path, mode) _mkdir(path)                       /* (no permission bits on Windows) */
#define timegm _mkgmtime
/* the guest's setjmp/longjmp (rt.h, main.c): POSIX _setjmp/_longjmp jump without unwinding anything; on x64
 * mingw-w64's _setjmp takes the frame to unwind to, and NULL means none -- the same plain jump (with a frame,
 * longjmp would walk the SEH unwind tables through every recompiled function in between) */
#undef _setjmp
#define _setjmp(buf) _setjmp((buf), NULL)
#define _longjmp(buf, v) longjmp((buf), (v))
#define sync() ((void)0)                                      /* (the Vita's flush-everything: nothing to do here) */
/* sleeps: mingw-w64's usleep/nanosleep are Sleep(), rounded up to the timer tick (1-15.6 ms) -- a guest's
 * sceKernelDelayThread(100) and the vblank wait (display.c) would oversleep by frames. platform.h's precise one. */
#define usleep(us) (plat_sleep_ns((uint64_t)(us) * 1000u), 0)
#define nanosleep(req, rem) win32_nanosleep((req), (rem))
#define fsync(fd) _commit(fd)
/* rename onto an existing file: POSIX replaces it (every "write a temporary, rename it over" save here relies on that);
 * Windows' own refuses -- the settings, shader caches and program index were written once and never updated again */
#define rename(from, to) win32_rename((from), (to))
#ifdef __cplusplus
extern "C" {
#endif
ssize_t pread(int fd, void *buf, size_t n, off_t off);      /* positional, the descriptor's own offset untouched (win32_compat.c) */
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);
char *realpath(const char *path, char *resolved);           /* an existing path, absolute, '/'-separated; 0 if it doesn't exist */
struct timeval;
int utimes(const char *path, const struct timeval tv[2]);   /* access and modification times, whole seconds */
void plat_sleep_ns(uint64_t ns);                             /* (platform.h) */
int win32_nanosleep(const struct timespec *req, struct timespec *rem);
int win32_rename(const char *from, const char *to);         /* MoveFileEx, replacing an existing file */
#ifdef __cplusplus
}
#endif
#endif
#endif
