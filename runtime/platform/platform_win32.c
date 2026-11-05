/* platform.h on Windows (llvm-mingw, UCRT) */
#undef _WIN32_WINNT   /* (win32_compat.h, force-included, brought mingw's default: Windows 7) */
#define _WIN32_WINNT 0x0A00   /* Windows 10 and later (SetProcessInformation's power throttling) */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#define PSAPI_VERSION 2   /* K32GetProcessMemoryInfo: in kernel32, no psapi.lib */
#include <psapi.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <cpuid.h>
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
/* unknown: Windows has no cheap per-process count. A Toolhelp snapshot walks every thread on the system and took
 * 39-47 ms per call on a desktop with ~480 processes -- a stutter if the overlay asked twice a second. */
int plat_thread_count(void) { return 0; }
uint64_t plat_rss_bytes(void) {
	PROCESS_MEMORY_COUNTERS pmc; pmc.cb = sizeof pmc;
	return K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? (uint64_t)pmc.WorkingSetSize : 0;
}
uint64_t plat_now_us(void) {   /* (QueryPerformanceCounter: what winpthreads' clock_gettime reads, without its conversions) */
	static LARGE_INTEGER f; LARGE_INTEGER c;
	if (!f.QuadPart) QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000u + (uint64_t)(c.QuadPart % f.QuadPart) * 1000000u / (uint64_t)f.QuadPart;
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

/* ---- the machine, for the performance overlay's sampler thread (platform.h) */
void *plat_lib_open(const char *name) { return (void *)LoadLibraryA(name); }
void *plat_lib_sym(void *lib, const char *sym) { return lib ? (void *)GetProcAddress((HMODULE)lib, sym) : 0; }

static uint64_t ft64(FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }
int plat_system_cpu(uint64_t *busy, uint64_t *total) {   /* 100 ns units; kernel time includes idle time */
	FILETIME idle, kern, user;
	if (!GetSystemTimes(&idle, &kern, &user)) return 0;
	uint64_t i = ft64(idle), k = ft64(kern), u = ft64(user);
	*total = k + u; *busy = k + u - i; return 1;
}
int plat_mem_info(uint64_t *used, uint64_t *total) {
	MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
	if (!GlobalMemoryStatusEx(&ms) || !ms.ullTotalPhys) return 0;
	*total = ms.ullTotalPhys; *used = ms.ullTotalPhys - ms.ullAvailPhys; return 1;
}
void plat_cpu_name(char *out, size_t n) {
	if (!n) return;
	out[0] = 0;
	unsigned int r[12], a, b, c, d;
	if (!__get_cpuid(0x80000000u, &a, &b, &c, &d) || a < 0x80000004u) return;
	for (unsigned int i = 0; i < 3; i++) __get_cpuid(0x80000002u + i, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
	char s[49]; memcpy(s, r, 48); s[48] = 0;
	const char *p = s; while (*p == ' ') p++;
	snprintf(out, n, "%s", p);
}

/* the GPU. DXGI (dxgi.dll, loaded here) names the adapters: the one whose description matches the renderer's device
 * name -- else the one with the most dedicated memory -- gives the LUID the performance counters are keyed by, and
 * the VRAM size. The counters (pdh.dll) are the ones Task Manager shows: "GPU Engine" utilization per engine and
 * process, summed per engine type, the busiest type being the GPU's load; and "GPU Adapter Memory" dedicated usage.
 * NVML (gpumon_nvml.h) adds temperature, power and clock on NVIDIA. AMD's and Intel's equivalents are not read: -1. */
#include "gpumon_nvml.h"
typedef LONG (WINAPI *PdhOpenQueryW_t)(LPCWSTR, DWORD_PTR, HANDLE *);
typedef LONG (WINAPI *PdhAddEnglishCounterW_t)(HANDLE, LPCWSTR, DWORD_PTR, HANDLE *);
typedef LONG (WINAPI *PdhCollectQueryData_t)(HANDLE);
typedef LONG (WINAPI *PdhGetFormattedCounterArrayW_t)(HANDLE, DWORD, DWORD *, DWORD *, void *);
typedef LONG (WINAPI *PdhCloseQuery_t)(HANDLE);
typedef struct { LPWSTR name; struct { DWORD status; DWORD pad; union { LONG l; double d; LONGLONG ll; } v; } value; } PdhItem;   /* PDH_FMT_COUNTERVALUE_ITEM_W */
#define PDH_FMT_DOUBLE_ 0x00000200
#define PDH_FMT_LARGE_ 0x00000400
#define PDH_FMT_NOCAP100_ 0x00008000
#define PDH_MORE_DATA_ ((LONG)0x800007D2)
struct PlatGpuMon {
	Nvml nv; char name[96];
	wchar_t luid[40]; float vram_total_mb;   /* L"luid_0x%08X_0x%08X"; from DXGI */
	HANDLE q, c_util, c_mem; int samples;
	PdhCollectQueryData_t collect; PdhGetFormattedCounterArrayW_t array; PdhCloseQuery_t close;
};
/* DXGI's IDXGIFactory1 / IDXGIAdapter1, called through their vtables (only the slots used; the layout is the COM
 * interface's): EnumAdapters1 is the factory's 13th method, GetDesc1 the adapter's 11th, Release the 3rd */
typedef struct { WCHAR Description[128]; UINT VendorId, DeviceId, SubSysId, Revision; SIZE_T DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory; LUID AdapterLuid; UINT Flags; } DxgiDesc1;
typedef struct { void **vt; } ComObj;
static void dxgi_pick(PlatGpuMon *m) {
	HMODULE lib = LoadLibraryA("dxgi.dll"); if (!lib) return;
	HRESULT (WINAPI *create)(const GUID *, void **) = (HRESULT (WINAPI *)(const GUID *, void **))(void *)GetProcAddress(lib, "CreateDXGIFactory1");
	static const GUID iid_factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};   /* IID_IDXGIFactory1 */
	ComObj *f = 0;
	if (!create || create(&iid_factory1, (void **)&f) < 0 || !f) return;
	HRESULT (WINAPI *enum1)(ComObj *, UINT, ComObj **) = (HRESULT (WINAPI *)(ComObj *, UINT, ComObj **))f->vt[12];
	LUID best = {0, 0}; SIZE_T best_mem = 0; int matched = 0;
	for (UINT i = 0; i < 16 && !matched; i++) {
		ComObj *a = 0;
		if (enum1(f, i, &a) < 0 || !a) break;
		DxgiDesc1 d; memset(&d, 0, sizeof d);
		if (((HRESULT (WINAPI *)(ComObj *, DxgiDesc1 *))a->vt[10])(a, &d) >= 0 && !(d.Flags & 2 /* software */)) {
			char desc[128]; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, desc, sizeof desc, 0, 0);
			if (m->name[0] && desc[0] && (nvml_icontains(m->name, desc) || nvml_icontains(desc, m->name))) { best = d.AdapterLuid; best_mem = d.DedicatedVideoMemory; matched = 1; }
			else if (d.DedicatedVideoMemory > best_mem) { best = d.AdapterLuid; best_mem = d.DedicatedVideoMemory; }
		}
		((ULONG (WINAPI *)(ComObj *))a->vt[2])(a);
	}
	((ULONG (WINAPI *)(ComObj *))f->vt[2])(f);
	if (!best_mem) return;
	swprintf(m->luid, 40, L"luid_0x%08lX_0x%08lX", (unsigned long)best.HighPart, (unsigned long)best.LowPart);
	m->vram_total_mb = (float)(best_mem / 1048576.0);
}
PlatGpuMon *plat_gpu_open(const char *device_name) {
	PlatGpuMon *m = calloc(1, sizeof *m); if (!m) return 0;
	snprintf(m->name, sizeof m->name, "%s", device_name ? device_name : "");
	m->vram_total_mb = -1;
	dxgi_pick(m);
	HMODULE pdh = m->luid[0] ? LoadLibraryA("pdh.dll") : 0;
	PdhOpenQueryW_t open = pdh ? (PdhOpenQueryW_t)(void *)GetProcAddress(pdh, "PdhOpenQueryW") : 0;
	PdhAddEnglishCounterW_t add = pdh ? (PdhAddEnglishCounterW_t)(void *)GetProcAddress(pdh, "PdhAddEnglishCounterW") : 0;
	m->collect = pdh ? (PdhCollectQueryData_t)(void *)GetProcAddress(pdh, "PdhCollectQueryData") : 0;
	m->array = pdh ? (PdhGetFormattedCounterArrayW_t)(void *)GetProcAddress(pdh, "PdhGetFormattedCounterArrayW") : 0;
	m->close = pdh ? (PdhCloseQuery_t)(void *)GetProcAddress(pdh, "PdhCloseQuery") : 0;
	if (open && add && m->collect && m->array && m->close && open(0, 0, &m->q) == 0) {
		if (add(m->q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &m->c_util)) m->c_util = 0;
		if (add(m->q, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &m->c_mem)) m->c_mem = 0;
		m->collect(m->q);   /* (a rate counter needs two collections: the first sample reads -1) */
	}
	return m;
}
/* the counter's instances as an array (PdhItem), or NULL; the caller frees it */
static PdhItem *pdh_items(PlatGpuMon *m, HANDLE c, DWORD fmt, DWORD *count) {
	DWORD size = 0; *count = 0;
	if (m->array(c, fmt, &size, count, 0) != PDH_MORE_DATA_ || !size) return 0;
	PdhItem *it = malloc(size);
	if (it && m->array(c, fmt, &size, count, it) != 0) { free(it); it = 0; }
	return it;
}
void plat_gpu_sample(PlatGpuMon *m, PlatGpuSample *s) {
	s->load_pct = s->vram_used_mb = s->vram_total_mb = s->temp_c = s->power_w = s->clock_mhz = -1;
	if (!m) return;
	nvml_open(&m->nv, m->name); nvml_sample(&m->nv, s);   /* (the counters below win where both give a number) */
	if (m->vram_total_mb > 0) s->vram_total_mb = m->vram_total_mb;
	if (!m->q || m->collect(m->q) != 0) return;
	if (++m->samples < 2) return;
	DWORD n; PdhItem *it;
	if (m->c_util && (it = pdh_items(m, m->c_util, PDH_FMT_DOUBLE_ | PDH_FMT_NOCAP100_, &n))) {
		/* instances: pid_<p>_luid_<hi>_<lo>_phys_<k>_eng_<e>_engtype_<type>; summed per type on our adapter */
		struct { wchar_t type[32]; double sum; } t[16]; int nt = 0;
		for (DWORD i = 0; i < n; i++) {
			if (it[i].value.status > 1 || !wcsstr(it[i].name, m->luid)) continue;   /* (PDH_CSTATUS_VALID_DATA 0, NEW_DATA 1) */
			const wchar_t *ty = wcsstr(it[i].name, L"engtype_"); ty = ty ? ty + 8 : L"";
			int k = 0; while (k < nt && wcscmp(t[k].type, ty)) k++;
			if (k == nt) { if (nt == 16) continue; wcsncpy(t[nt].type, ty, 31); t[nt].type[31] = 0; t[nt].sum = 0; nt++; }
			t[k].sum += it[i].value.v.d;
		}
		double busiest = 0; for (int k = 0; k < nt; k++) if (t[k].sum > busiest) busiest = t[k].sum;
		if (nt) s->load_pct = (float)(busiest > 100 ? 100 : busiest);
		free(it);
	}
	if (m->c_mem && (it = pdh_items(m, m->c_mem, PDH_FMT_LARGE_, &n))) {
		double used = -1;
		for (DWORD i = 0; i < n; i++) if (it[i].value.status <= 1 && wcsstr(it[i].name, m->luid)) used = (used < 0 ? 0 : used) + (double)it[i].value.v.ll;
		if (used >= 0) s->vram_used_mb = (float)(used / 1048576.0);
		free(it);
	}
}
void plat_gpu_close(PlatGpuMon *m) { if (!m) return; if (m->q) m->close(m->q); free(m); }
