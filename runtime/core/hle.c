/*
 * The system-call registry: every subsystem file exports one table (name -> implementation);
 * hle_find searches them by import name. main.c resolves each import stub address once and caches
 * the answer, so the per-call cost is a lookup, not a search.
 */
#include <stdio.h>
#include <string.h>
#include "hle.h"
#include "net.h"

static const HleEntry *const modules[] = {
	hle_kernel, hle_display, hle_ngs, hle_libc, hle_io, hle_sysmem, hle_apputil, hle_fios, hle_gpu, hle_video, hle_gesture, hle_jpeg, hle_camera,
};
static const HleEntry *const online_modules[] = { hle_net, hle_np, hle_http, hle_ssl_local };   /* only for a game run online (net.h) */

static HleFn find_in(const HleEntry *const *mods, size_t n, const char *name) {
	for (size_t m = 0; m < n; m++)
		for (const HleEntry *e = mods[m]; e->name; e++)
			if (!strcmp(e->name, name)) return e->fn;
	return 0;
}
HleFn hle_find(const char *name) {
	HleFn f = find_in(modules, sizeof modules / sizeof *modules, name);
	if (!f && net_online()) f = find_in(online_modules, sizeof online_modules / sizeof *online_modules, name);
	return f;
}

void hle_init(Cpu *c) { (void)c; }

/* imported variables arrive one by one (main.c, from the image); some refer to each other (a C++
 * vtable's type_info), so they are filled once all are known */
static struct { char name[60]; uint32_t addr; } vars[256];
static int nvars;
void hle_import_var(const char *name, uint32_t addr) {
	if (nvars < 256) { strncpy(vars[nvars].name, name, 59); vars[nvars++].addr = addr; }
}
uint32_t hle_var_addr(const char *name) {
	for (int i = 0; i < nvars; i++) if (!strcmp(vars[i].name, name)) return vars[i].addr;
	return 0;
}
void hle_import_vars_done(void) {
	for (int i = 0; i < nvars; i++) {
		int ok = libc_import_var(vars[i].name, vars[i].addr) || cxx_import_var(vars[i].name, vars[i].addr);
		fprintf(stderr, "import variable %s at %#x%s\n", vars[i].name, vars[i].addr, ok ? "" : " (left zero)");
	}
}
