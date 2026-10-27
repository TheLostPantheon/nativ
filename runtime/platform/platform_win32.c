/* platform.h on Windows (llvm-mingw, UCRT) */
#undef _WIN32_WINNT   /* (win32_compat.h, force-included, brought mingw's default: Windows 7) */
#define _WIN32_WINNT 0x0A00   /* Windows 10 and later (SetProcessInformation's power throttling) */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#include <string.h>
#include "platform.h"
#include <pthread.h>   /* (winpthreads: kernel.c names threads through it, so it has the names) */

/* The reservation: committed whole when the commit limit allows (pages still only become memory when touched),
 * else reserved and committed a page at a time from the exception handler below -- Windows has no overcommit,
 * and 4 GB of commit charge fails where the page file is small. */
typedef struct { uint8_t *base; size_t bytes; } Lazy;
static Lazy lazy[4]; static int nlazy;
static SRWLOCK lazy_lock = SRWLOCK_INIT;
void *plat_reserve(size_t bytes) {
	void *p = VirtualAlloc(0, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (p) return p;
	p = VirtualAlloc(0, bytes, MEM_RESERVE, PAGE_NOACCESS);
	if (!p) return 0;
	AcquireSRWLockExclusive(&lazy_lock);
	if (nlazy < 4) { lazy[nlazy].base = p; lazy[nlazy].bytes = bytes; nlazy++; }
	ReleaseSRWLockExclusive(&lazy_lock);
	return p;
}
static int lazy_commit(void *addr) {   /* a touch of a reserved, uncommitted page of a lazy reservation: commit it */
	uint8_t *a = addr; int ours = 0;
	AcquireSRWLockShared(&lazy_lock);
	for (int i = 0; i < nlazy; i++) if (a >= lazy[i].base && a < lazy[i].base + lazy[i].bytes) ours = 1;
	ReleaseSRWLockShared(&lazy_lock);
	if (!ours) return 0;
	MEMORY_BASIC_INFORMATION mi;
	if (!VirtualQuery(a, &mi, sizeof mi) || mi.State != MEM_RESERVE) return 0;   /* committed already: a protection fault, not ours */
	return VirtualAlloc((void *)((uintptr_t)a & ~(uintptr_t)(plat_page_size() - 1)), plat_page_size(), MEM_COMMIT, PAGE_READWRITE) != 0;
}
void plat_thread_name(char *buf, size_t n) { if (!n) return; buf[0] = 0; pthread_getname_np(pthread_self(), buf, n); }
int plat_protect(void *p, size_t bytes, int writable) { DWORD old; return VirtualProtect(p, bytes, writable < 0 ? PAGE_NOACCESS : writable ? PAGE_READWRITE : PAGE_READONLY, &old) ? 0 : -1; }
size_t plat_page_size(void) { static size_t ps; if (!ps) { SYSTEM_INFO si; GetSystemInfo(&si); ps = si.dwPageSize; } return ps; }

/* Faults. A vectored handler sees every exception first -- including the ones a driver or SDL raises and handles
 * itself -- so it only ever claims what a hook makes valid, and never reports. The crash report comes from the
 * unhandled-exception filter, which runs only when nothing else wanted the exception. */
#define MAX_HOOKS 4
static PlatFaultHook hooks[MAX_HOOKS]; static int nhooks;
static PlatCrashFn crash_fn;
static LONG CALLBACK on_vectored(EXCEPTION_POINTERS *ep) {
	if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || ep->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
	void *addr = (void *)ep->ExceptionRecord->ExceptionInformation[1];
	DWORD saved = GetLastError();
	for (int i = nhooks - 1; i >= 0; i--) if (hooks[i](addr)) { SetLastError(saved); return EXCEPTION_CONTINUE_EXECUTION; }
	if (lazy_commit(addr)) { SetLastError(saved); return EXCEPTION_CONTINUE_EXECUTION; }
	SetLastError(saved);
	return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI on_unhandled(EXCEPTION_POINTERS *ep) {
	DWORD code = ep->ExceptionRecord->ExceptionCode;
	void *addr = code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2 ? (void *)ep->ExceptionRecord->ExceptionInformation[1] : ep->ExceptionRecord->ExceptionAddress;
	const char *what = code == EXCEPTION_ACCESS_VIOLATION ? "access violation" : code == EXCEPTION_ILLEGAL_INSTRUCTION ? "illegal instruction" :
	                   code == EXCEPTION_STACK_OVERFLOW ? "stack overflow" : code == EXCEPTION_INT_DIVIDE_BY_ZERO ? "integer divide by zero" : "unhandled exception";
	if (crash_fn) crash_fn(what, addr);
	return EXCEPTION_CONTINUE_SEARCH;   /* then the default: Windows Error Reporting, or the debugger */
}
static void install(void) {
	static int done; if (done) return; done = 1;
	AddVectoredExceptionHandler(1, on_vectored);
	SetUnhandledExceptionFilter(on_unhandled);
	ULONG guarantee = 1 << 16; SetThreadStackGuarantee(&guarantee);   /* (room to report a stack overflow on the main thread) */
	timeBeginPeriod(1);   /* the system timer at 1 ms, not 15.6: every timed wait (the guest's semaphore and event timeouts through winpthreads) wakes on time */
	/* no power throttling (EcoQoS): Windows 11 runs a process it decides is in the background on the efficient cores at low
	 * clocks, and stops honouring its timer resolution -- on a Ryzen AI 9 HX 370 (4 Zen 5 + 8 Zen 5c cores) Killzone's render
	 * thread took 11 ms a frame there against 6 on the fast cores, the level at 27-39 fps against 43-54 (2026-10-23) */
	PROCESS_POWER_THROTTLING_STATE pt = {PROCESS_POWER_THROTTLING_CURRENT_VERSION,
	                                     PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0};
	SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
}
void plat_add_fault_hook(PlatFaultHook hook) { install(); if (nhooks < MAX_HOOKS) hooks[nhooks++] = hook; }
void plat_set_crash_handler(PlatCrashFn fn) { crash_fn = fn; install(); }
void plat_backtrace(FILE *f) {   /* addresses as module+offset: llvm-addr2line / llvm-symbolizer on the .exe (built with -g) names them */
	void *bt[48]; USHORT n = CaptureStackBackTrace(1, 48, bt, 0);
	for (USHORT i = 0; i < n; i++) {
		HMODULE m = 0; char name[MAX_PATH] = "?";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)bt[i], &m) && m) {
			GetModuleFileNameA(m, name, sizeof name);
			const char *b = strrchr(name, '\\'); fprintf(f, "  %2u %s+%#llx\n", i, b ? b + 1 : name, (unsigned long long)((uint8_t *)bt[i] - (uint8_t *)m));
		} else fprintf(f, "  %2u %p\n", i, bt[i]);
	}
}

int plat_cpu_count(void) { DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS); return n ? (int)n : 1; }
double plat_cpu_seconds(void) {
	FILETIME c, e, k, u;
	if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
	uint64_t kt = ((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime, ut = ((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime;
	return (kt + ut) / 1e7;   /* 100 ns units */
}
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
void plat_sleep_ns(uint64_t ns) {
	static __thread HANDLE timer; static __thread int tried;
	if (!tried) { tried = 1; timer = CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS); }   /* Windows 10 1803+: ~0.5 ms, not the 15.6 ms tick */
	if (timer) {
		LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((ns + 99) / 100);   /* relative, 100 ns units */
		if (SetWaitableTimer(timer, &due, 0, 0, 0, FALSE)) { WaitForSingleObject(timer, INFINITE); return; }
	}
	Sleep((DWORD)((ns + 999999) / 1000000));
}

void plat_random(void *buf, size_t n) {   /* the system's own generator (bcrypt.dll, loaded on first use: no import library needed) */
	typedef LONG (WINAPI *GenRandom)(void *, PUCHAR, ULONG, ULONG);
	static GenRandom gen; HMODULE m;
	if (!gen && (m = LoadLibraryA("bcrypt.dll"))) gen = (GenRandom)(void *)GetProcAddress(m, "BCryptGenRandom");
	if (gen && gen(NULL, (PUCHAR)buf, (ULONG)n, 0x00000002 /* BCRYPT_USE_SYSTEM_PREFERRED_RNG */) == 0) return;
	LARGE_INTEGER t; QueryPerformanceCounter(&t);
	for (size_t i = 0; i < n; i++) ((unsigned char *)buf)[i] = (unsigned char)(t.QuadPart >> (i % 56)) ^ (unsigned char)(GetTickCount() >> (i % 24));
}
