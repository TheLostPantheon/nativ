/* NVIDIA's management library (NVML), loaded at run time when the driver installed it -- nvml.dll on Windows,
 * libnvidia-ml.so.1 on Linux -- for the performance overlay's GPU numbers (platform.h's plat_gpu_*): load, memory,
 * temperature, power and clock of the device whose name matches the one the game renders on. Included by
 * platform_win32.c and platform_posix.c, which supply plat_lib_open/plat_lib_sym. The declarations below are the
 * documented NVML C API (nvml.h, NVML API Reference): the handful of calls this uses, written out here so no NVIDIA
 * header or library is needed to build. */
#ifndef RECOMP_GPUMON_NVML_H
#define RECOMP_GPUMON_NVML_H
#include <ctype.h>
#include <string.h>
#include "platform.h"

typedef struct NvmlDev_ *NvmlDev;
typedef struct { unsigned int gpu, memory; } NvmlUtilization;            /* percent over the last sample period */
typedef struct { unsigned long long total, free, used; } NvmlMemory;     /* bytes */
typedef struct {
	int tried, ok; NvmlDev dev;
	int (*util)(NvmlDev, NvmlUtilization *);
	int (*mem)(NvmlDev, NvmlMemory *);
	int (*temp)(NvmlDev, int, unsigned int *);       /* sensor 0: the GPU die */
	int (*power)(NvmlDev, unsigned int *);           /* milliwatts */
	int (*clock)(NvmlDev, int, unsigned int *);      /* clock 0: graphics, MHz */
} Nvml;

/* case-insensitive: does `hay` contain `needle`? */
static int nvml_icontains(const char *hay, const char *needle) {
	size_t n = strlen(needle); if (!n) return 0;
	for (; *hay; hay++) { size_t i = 0; while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++; if (i == n) return 1; }
	return 0;
}

/* finds the NVML device named like `want` (the renderer's device name; either may carry extra words), or the only
 * one when there is exactly one; leaves nv->ok 0 on any machine without NVML or without a match */
static void nvml_open(Nvml *nv, const char *want) {
	if (nv->tried) return;
	nv->tried = 1;
#ifdef _WIN32
	void *lib = plat_lib_open("nvml.dll");
#else
	void *lib = plat_lib_open("libnvidia-ml.so.1");
#endif
	if (!lib) return;
	int (*init)(void) = (int (*)(void))plat_lib_sym(lib, "nvmlInit_v2");
	int (*count)(unsigned int *) = (int (*)(unsigned int *))plat_lib_sym(lib, "nvmlDeviceGetCount_v2");
	int (*by_index)(unsigned int, NvmlDev *) = (int (*)(unsigned int, NvmlDev *))plat_lib_sym(lib, "nvmlDeviceGetHandleByIndex_v2");
	int (*name)(NvmlDev, char *, unsigned int) = (int (*)(NvmlDev, char *, unsigned int))plat_lib_sym(lib, "nvmlDeviceGetName");
	nv->util = (int (*)(NvmlDev, NvmlUtilization *))plat_lib_sym(lib, "nvmlDeviceGetUtilizationRates");
	nv->mem = (int (*)(NvmlDev, NvmlMemory *))plat_lib_sym(lib, "nvmlDeviceGetMemoryInfo");
	nv->temp = (int (*)(NvmlDev, int, unsigned int *))plat_lib_sym(lib, "nvmlDeviceGetTemperature");
	nv->power = (int (*)(NvmlDev, unsigned int *))plat_lib_sym(lib, "nvmlDeviceGetPowerUsage");
	nv->clock = (int (*)(NvmlDev, int, unsigned int *))plat_lib_sym(lib, "nvmlDeviceGetClockInfo");
	unsigned int n = 0;
	if (!init || !count || !by_index || !name || init() != 0 || count(&n) != 0 || !n) return;
	for (unsigned int i = 0; i < n; i++) {
		NvmlDev d; char nm[96] = "";
		if (by_index(i, &d) != 0) continue;
		name(d, nm, sizeof nm);
		if ((want && *want && *nm && (nvml_icontains(want, nm) || nvml_icontains(nm, want))) || (n == 1 && !(want && *want))) { nv->dev = d; nv->ok = 1; return; }
	}
}

/* fills what NVML gives into s (which the caller set to -1s); returns 1 when the device was found */
static int nvml_sample(Nvml *nv, PlatGpuSample *s) {
	if (!nv->ok) return 0;
	NvmlUtilization u; NvmlMemory m; unsigned int v;
	if (nv->util && nv->util(nv->dev, &u) == 0) s->load_pct = (float)u.gpu;
	if (nv->mem && nv->mem(nv->dev, &m) == 0) { s->vram_used_mb = (float)(m.used / 1048576.0); s->vram_total_mb = (float)(m.total / 1048576.0); }
	if (nv->temp && nv->temp(nv->dev, 0, &v) == 0) s->temp_c = (float)v;
	if (nv->power && nv->power(nv->dev, &v) == 0) s->power_w = v / 1000.0f;
	if (nv->clock && nv->clock(nv->dev, 0, &v) == 0) s->clock_mhz = (float)v;
	return 1;
}
#endif
