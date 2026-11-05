/* sysmodule_test.c -- SceSysmodule's IsLoaded contract: a module answers SCE_SYSMODULE_LOADED (0) only after the game
 * loaded it, SCE_SYSMODULE_ERROR_UNLOADED (0x805a1001) before that and after an unload. IsLoaded used to answer 0 for
 * every module: a game asking about the IME module (0x11) it never loads before calling sceImeUpdate read "loaded" plus
 * the update stub's 0 as an open on-screen keyboard and never opened its save-data dialog.
 *
 * Case 1 is the fail-before / pass-after case. Calls go through the hle_apputil table by name, so the same fixture
 * compiles against an older apputil.c (-DAPPUTIL_UNDER_TEST) for the fail-before run.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef APPUTIL_UNDER_TEST
#define APPUTIL_UNDER_TEST "../core/apputil.c"
#endif
#include APPUTIL_UNDER_TEST

/* ---- apputil.c expects these (same shim style as monitor_test.c / kernel_test.c) */
uint8_t *recomp_mem;
uint32_t recomp_watch_lo = 0, recomp_watch_hi = 0;
void recomp_watch_write(uint32_t a, uint32_t v, int width) { (void)a; (void)v; (void)width; }
uint32_t guest_alloc(uint32_t size, uint32_t align) { (void)align; static uint32_t heap = 0x81000000; uint32_t a = heap; heap += size; return a; }
void gwatch_read_open(uint32_t addr, uint32_t len) { (void)addr; (void)len; }
int net_online(void) { return 0; }
const char *net_online_needs(void) { return ""; }
int overlay_draws_hud(void) { return 0; }
void overlay_toast_titled(const char *title, const char *body, double seconds) { (void)title; (void)body; (void)seconds; }
void plat_random(void *buf, size_t n) { memset(buf, 0x5a, n); }
const char *settings_str(const char *key, const char *def, char *buf, int bufsize) { (void)key; (void)buf; (void)bufsize; return def; }
void settings_set_str(const char *key, const char *v) { (void)key; (void)v; }
void settings_set_own(const char *key, const char *v) { (void)key; (void)v; }
void settings_save(void) {}

static int fails;
static void check(const char *what, int ok) {
	printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
	if (!ok) fails++;
}

static uint32_t call(const char *name, uint32_t a0) {
	for (const HleEntry *e = hle_apputil; e->name; e++)
		if (!strcmp(e->name, name)) { Cpu cpu = {0}; cpu.r[0] = a0; e->fn(&cpu); return cpu.r[0]; }
	return 0xdeadbeefu;   /* not in the table: the generic stub would answer (0 for sceSysmodule) */
}

int main(void) {
	/* Case 1 (fail-before / pass-after): a module the game never loaded is not loaded */
	check("IsLoaded(IME 0x11) before any load is UNLOADED", call("sceSysmoduleIsLoaded", 0x11) == 0x805a1001u);
	/* Case 2: loading makes it loaded; other modules stay unloaded */
	check("LoadModule(0x11) returns 0", call("sceSysmoduleLoadModule", 0x11) == 0);
	check("IsLoaded(0x11) after LoadModule is LOADED (0)", call("sceSysmoduleIsLoaded", 0x11) == 0);
	check("IsLoaded(0x4) stays UNLOADED", call("sceSysmoduleIsLoaded", 0x4) == 0x805a1001u);
	/* Case 3: unloading makes it unloaded again */
	check("UnloadModule(0x11) returns 0", call("sceSysmoduleUnloadModule", 0x11) == 0);
	check("IsLoaded(0x11) after UnloadModule is UNLOADED", call("sceSysmoduleIsLoaded", 0x11) == 0x805a1001u);
	/* Case 4: an id outside the table is never loaded (and does not crash) */
	call("sceSysmoduleLoadModule", 0x12345);
	check("IsLoaded(out-of-range id) is UNLOADED", call("sceSysmoduleIsLoaded", 0x12345) == 0x805a1001u);

	if (fails) { printf("%d checks FAILED\n", fails); return 1; }
	printf("all passed\n");
	return 0;
}
