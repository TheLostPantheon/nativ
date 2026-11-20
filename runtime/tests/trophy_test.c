/*
 * Unit harness for runtime/core/trophy.c, the game's trophies kept on this machine. It writes a small trophy pack (a
 * TROPHY.TRP with a platinum, a bronze and a hidden gold counting towards it, a trophy of an add-on group that doesn't,
 * the list in English and French, and an icon) into a temporary folder, then: the game's and a trophy's details, an
 * unlock and its toast, a second unlock of it, the platinum refused directly and unlocking itself with the last trophy
 * that counts towards it (the add-on's not needed), the unlock state's flags and count, the unlocks kept across contexts
 * (the file), the language's list, an icon by size then bytes, a copy without a pack (its context made all the same, unlocks
 * by id, kept), and the errors for a wrong size or trophy. It includes
 * trophy.c and maps app0: and ux0: to the folder. Build and run: see README.md in this directory.
 */
#include <stdarg.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../core/trophy.c"

/* ---- the runtime trophy.c expects */
uint8_t *recomp_mem;
static char root[512];
const char *host_path_str(const char *p, char *buf, size_t n) {   /* app0:x and ux0:x under the temporary folder */
	const char *colon = strchr(p, ':');
	snprintf(buf, n, "%s/%.*s/%s", root, (int)(colon - p), p, colon + 1);
	return buf;
}
static uint32_t lang = 1;
uint32_t game_system_language(void) { return lang; }
static int toasts; static char last_toast[256];
void overlay_toast_titled(const char *title, const char *body, double s) { (void)s; toasts++; snprintf(last_toast, sizeof last_toast, "%s: %s", title, body); }

static int checks, fails;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKEQ(a, b) do { uint32_t _a = (a), _b = (b); checks++; if (_a != _b) { fails++; fprintf(stderr, "FAIL %s:%d: %s = %#x, expected %s = %#x\n", __FILE__, __LINE__, #a, _a, #b, _b); } } while (0)

static uint32_t call(HleFn f, int n, ...) {   /* r0-r3, then the stack words from 0x80f00000 */
	static Cpu c; memset(&c, 0, sizeof c); c.r[13] = 0x80f00000;
	va_list ap; va_start(ap, n);
	for (int i = 0; i < n; i++) { uint32_t v = va_arg(ap, uint32_t); if (i < 4) c.r[i] = v; else WR32(c.r[13] + 4u * (uint32_t)(i - 4), v); }
	va_end(ap);
	f(&c);
	return c.r[0];
}
static uint32_t heap = 0x81000000;
static uint32_t galloc(uint32_t n) { heap = (heap + 7) & ~7u; uint32_t a = heap; heap += n; memset(G(a), 0, n); return a; }

/* ---- the pack */
static const char *const EN =
	"<!--Sce-Np-Trophy-Signature: 00-->\n<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<trophyconf version=\"1.1\" platform=\"psp2\">\n"
	"<npcommid>NPWR99999_00</npcommid>\n<title-name>Test &amp; Game</title-name>\n<title-detail>All of them</title-detail>\n"
	"<group id=\"001\"><name>Add-on</name><detail>Extra</detail></group>\n"
	"<trophy id=\"000\" hidden=\"no\" ttype=\"P\" pid=\"-1\"><name>Everything</name><detail>Every trophy</detail></trophy>\n"
	"<trophy id=\"001\" hidden=\"no\" ttype=\"B\" pid=\"000\"><name>First</name><detail>A start</detail></trophy>\n"
	"<trophy id=\"002\" hidden=\"yes\" ttype=\"G\" pid=\"000\"><name>Secret</name><detail>Found it</detail></trophy>\n"
	"<trophy id=\"003\" hidden=\"no\" ttype=\"S\" pid=\"-1\" gid=\"001\"><name>Extra</name><detail>Add-on</detail></trophy>\n"
	"</trophyconf>\n";
static const char *const FR =
	"<trophyconf><title-name>Jeu de test</title-name><title-detail>Tous</title-detail>\n"
	"<trophy id=\"000\" ttype=\"P\" pid=\"-1\"><name>Tout</name><detail>-</detail></trophy>\n</trophyconf>\n";
static void be32w(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void write_pack(const char *path) {
	const char *names[3] = { "TROP.SFM", "TROP_02.SFM", "ICON0.PNG" }; const char *data[3] = { EN, FR, "PNGDATA" };
	uint8_t h[0x40] = {0}, e[3][0x40]; memset(e, 0, sizeof e);
	be32w(h, 0xdca24d00u); be32w(h + 4, 2); be32w(h + 0x10, 3); be32w(h + 0x14, 0x40);
	uint32_t off = 0x40 + 3 * 0x40;
	for (int i = 0; i < 3; i++) { strcpy((char *)e[i], names[i]); be32w(e[i] + 36, off); be32w(e[i] + 44, (uint32_t)strlen(data[i])); off += (uint32_t)strlen(data[i]); }
	char dir[600]; snprintf(dir, sizeof dir, "%s", path); *strrchr(dir, '/') = 0;
	for (char *q = dir + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(dir, 0755); *q = '/'; } mkdir(dir, 0755);
	FILE *f = fopen(path, "wb"); fwrite(h, 1, sizeof h, f); fwrite(e, 1, sizeof e, f);
	for (int i = 0; i < 3; i++) fwrite(data[i], 1, strlen(data[i]), f);
	fclose(f);
}

static uint32_t comm_id(void) { uint32_t a = galloc(12); memcpy(G(a), "NPWR99999", 9); return a; }   /* {data[9], term, num 0, dummy} */
static uint32_t context(void) { uint32_t p = galloc(4); CHECKEQ(call(h_create_context, 4, p, comm_id(), 0, 0), 0); return RD32(p); }

int main(void) {
	recomp_mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (recomp_mem == MAP_FAILED) { perror("mmap"); return 2; }
	snprintf(root, sizeof root, "/tmp/trophy_test.%d", (int)getpid());
	char trp[700]; snprintf(trp, sizeof trp, "%s/app0/sce_sys/trophy/NPWR99999_00/TROPHY.TRP", root); write_pack(trp);

	CHECKEQ(call(h_init, 1, 0), 0); CHECKEQ(call(h_init, 1, 0), TR_ALREADY_INITIALIZED);
	const uint32_t ctx = context(), hp = galloc(4);
	CHECKEQ(call(h_create_handle, 1, hp), 0);
	const uint32_t h = RD32(hp);
	CHECKEQ(call(h_register, 3, ctx, h, 0), 0);
	CHECKEQ(call(h_create_context, 4, galloc(4), comm_id(), 0, 0), TR_CONTEXT_ALREADY_EXISTS);

	/* the game's details: four trophies, one of each grade, one group; the title unescaped */
	const uint32_t gd = galloc(0x49c), gdat = galloc(0x1c); WR32(gd, 0x49c); WR32(gdat, 0x1c);
	CHECKEQ(call(h_game_info, 4, ctx, h, gd, gdat), 0);
	CHECKEQ(RD32(gd + 4), 1); CHECKEQ(RD32(gd + 8), 4); CHECKEQ(RD32(gd + 12), 1); CHECKEQ(RD32(gd + 16), 1); CHECKEQ(RD32(gd + 20), 1); CHECKEQ(RD32(gd + 24), 1);
	CHECK(!strcmp((const char *)G(gd + 28), "Test & Game"));
	CHECKEQ(RD32(gdat + 4), 0);
	/* a trophy: the hidden gold */
	const uint32_t td = galloc(0x494), tdat = galloc(0x18); WR32(td, 0x494); WR32(tdat, 0x18);
	CHECKEQ(call(h_trophy_info, 5, ctx, h, 2, td, tdat), 0);
	CHECKEQ(RD32(td + 4), 2); CHECKEQ(RD32(td + 8), GRADE_GOLD); CHECKEQ(RD32(td + 16), 1);
	CHECK(!strcmp((const char *)G(td + 20), "Secret")); CHECK(!strcmp((const char *)G(td + 148), "Found it"));
	CHECKEQ(RD32(tdat + 8), 0);

	/* unlocks: the platinum only by itself, the bronze, then the gold, which brings the platinum */
	const uint32_t plat = galloc(4);
	CHECKEQ(call(h_unlock, 4, ctx, h, 0, plat), TR_PLATINUM_CANNOT_UNLOCK);
	CHECKEQ(call(h_unlock, 4, ctx, h, 1, plat), 0); CHECKEQ(RD32(plat), (uint32_t)-1); CHECKEQ(toasts, 1);
	CHECK(strstr(last_toast, "Bronze") && strstr(last_toast, "First"));
	CHECKEQ(call(h_unlock, 4, ctx, h, 1, plat), TR_ALREADY_UNLOCKED);
	CHECKEQ(call(h_unlock, 4, ctx, h, 2, plat), 0); CHECKEQ(RD32(plat), 0); CHECKEQ(toasts, 3);   /* the gold, then the platinum */
	CHECKEQ(call(h_unlock, 4, ctx, h, 9, plat), TR_INVALID_TROPHY_ID);
	CHECKEQ(call(h_trophy_info, 5, ctx, h, 2, td, tdat), 0);
	CHECKEQ(RD32(tdat + 8), 1); CHECK(RD32(tdat + 16) || RD32(tdat + 20));   /* unlocked, with a time */
	const uint32_t flags = galloc(16), count = galloc(4);
	CHECKEQ(call(h_unlock_state, 4, ctx, h, flags, count), 0);
	CHECKEQ(RD32(flags), 0x7); CHECKEQ(RD32(count), 4);
	CHECKEQ(call(h_game_info, 4, ctx, h, gd, gdat), 0);
	CHECKEQ(RD32(gdat + 4), 3); CHECKEQ(RD32(gdat + 24), 75);
	/* the add-on group's own counts */
	const uint32_t grd = galloc(0x49c), grdat = galloc(0x20); WR32(grd, 0x49c); WR32(grdat, 0x20);
	CHECKEQ(call(h_group_info, 5, ctx, h, 1, grd, grdat), 0);
	CHECKEQ(RD32(grd + 8), 1); CHECK(!strcmp((const char *)G(grd + 28), "Add-on")); CHECKEQ(RD32(grdat + 8), 0);

	/* kept: a new context reads them back */
	CHECKEQ(call(h_destroy_context, 1, ctx), 0);
	const uint32_t ctx2 = context();
	CHECKEQ(call(h_unlock_state, 4, ctx2, h, flags, count), 0); CHECKEQ(RD32(flags), 0x7);
	CHECKEQ(call(h_destroy_context, 1, ctx2), 0);
	/* the game's language: French's list */
	lang = 2;
	const uint32_t ctx3 = context();
	CHECKEQ(call(h_game_info, 4, ctx3, h, gd, 0), 0); CHECK(!strcmp((const char *)G(gd + 28), "Jeu de test"));
	/* the icon: its size, then its bytes */
	const uint32_t sz = galloc(4), buf = galloc(64);
	CHECKEQ(call(h_game_icon, 4, ctx3, h, 0, sz), 0); CHECKEQ(RD32(sz), 7);
	CHECKEQ(call(h_game_icon, 4, ctx3, h, buf, sz), 0); CHECK(!memcmp(G(buf), "PNGDATA", 7));
	/* errors */
	WR32(td, 0x400);
	CHECKEQ(call(h_trophy_info, 5, ctx3, h, 1, td, 0), TR_INVALID_ARGUMENT);
	CHECKEQ(call(h_unlock_state, 4, 99, h, flags, count), TR_INVALID_CONTEXT);
	/* a copy without its trophy pack: the context is made all the same, any id unlocks (and is kept) */
	const uint32_t other = galloc(12); memcpy(G(other), "NPWR11111", 9);
	const uint32_t cp = galloc(4); CHECKEQ(call(h_create_context, 4, cp, other, 0, 0), 0);
	const uint32_t ctx4 = RD32(cp);
	CHECKEQ(call(h_unlock_state, 4, ctx4, h, flags, count), 0); CHECKEQ(RD32(flags), 0); CHECKEQ(RD32(count), 0);
	CHECKEQ(call(h_unlock, 4, ctx4, h, 5, plat), 0); CHECKEQ(RD32(plat), (uint32_t)-1); CHECK(strstr(last_toast, "Trophy 5") != 0);
	CHECKEQ(call(h_unlock, 4, ctx4, h, 5, plat), TR_ALREADY_UNLOCKED);
	CHECKEQ(call(h_destroy_context, 1, ctx4), 0);
	CHECKEQ(call(h_create_context, 4, cp, other, 0, 0), 0);
	CHECKEQ(call(h_unlock_state, 4, RD32(cp), h, flags, count), 0); CHECKEQ(RD32(flags), 1u << 5);   /* kept */
	CHECKEQ(call(h_term, 0), 0); CHECKEQ(call(h_term, 0), TR_NOT_INITIALIZED);

	char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", root); if (system(cmd)) {}
	fprintf(stderr, "%d checks, %d failed\n", checks, fails);
	if (!fails) fprintf(stderr, "all passed\n");
	return fails ? 1 : 0;
}
