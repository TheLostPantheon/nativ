/*
 * Unit harness for the message dialog in runtime/core/apputil.c: the parameter parsing
 * (guest SceMsgDialogParam bytes), the console's states (NONE, RUNNING, FINISHED), the results
 * a game reads, and the headless auto-answer. It includes apputil.c so the
 * h_* handlers are reachable and supplies the runtime symbols it takes from elsewhere (stubs/ holds a stand-in
 * for SDL's header). What the overlay draws and the keys it answers by are not covered: they need a window.
 * Build and run: see README.md in this directory.
 */
#include <stdarg.h>
#include <sys/mman.h>
#include "../core/apputil.c"

/* ---- the runtime apputil.c expects */
uint8_t *recomp_mem;
static uint32_t heap = 0x81000000;
uint32_t guest_alloc(uint32_t size, uint32_t align) { heap = (heap + align - 1) & ~(align - 1); uint32_t a = heap; heap += size; return a; }
void gwatch_read_open(uint32_t a, uint32_t n) { (void)a; (void)n; }
int net_online(void) { return 0; }
const char *net_online_needs(void) { return 0; }
void plat_random(void *buf, size_t n) { memset(buf, 7, n); }
static int hud;   /* overlay_draws_hud: a window to answer in */
int overlay_draws_hud(void) { return hud; }
void overlay_toast_titled(const char *t, const char *b, double s) { (void)t; (void)b; (void)s; }
const char *settings_str(const char *key, const char *def, char *buf, int n) { (void)key; snprintf(buf, (size_t)n, "%s", def); return buf; }
void settings_save(void) {}
void settings_set_str(const char *k, const char *v) { (void)k; (void)v; }
void settings_set_own(const char *k, const char *v) { (void)k; (void)v; }

static int checks, fails;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKEQ(a, b) do { uint32_t _a = (a), _b = (b); checks++; if (_a != _b) { fails++; fprintf(stderr, "FAIL %s:%d: %s = %#x, expected %s = %#x\n", __FILE__, __LINE__, #a, _a, #b, _b); } } while (0)

static uint32_t call(HleFn f, int n, ...) {
	Cpu c = { 0 };
	va_list ap; va_start(ap, n);
	for (int i = 0; i < n && i < 4; i++) c.r[i] = va_arg(ap, uint32_t);
	va_end(ap);
	f(&c);
	return c.r[0];
}
/* guest strings and structs */
static uint32_t gstr(const char *s) { uint32_t a = guest_alloc((uint32_t)strlen(s) + 1, 4); strcpy((char *)G(a), s); return a; }
static uint32_t gzero(uint32_t n) { uint32_t a = guest_alloc(n, 8); memset(G(a), 0, n); return a; }

/* SceMsgDialogParam: mode +0x50, userMsgParam +0x54, sysMsgParam +0x58, errorCodeParam +0x5c, progBarParam +0x60 */
static uint32_t msg_user(int buttons, const char *text, const char *b1, const char *b2, const char *b3) {
	uint32_t prm = gzero(0x88), up = gzero(0x28);
	WR32(prm + 0x50, 1); WR32(prm + 0x54, up); WR32(up, (uint32_t)buttons); WR32(up + 4, gstr(text));
	if (b1) { uint32_t bp = gzero(0x38); WR32(bp, gstr(b1)); WR32(bp + 8, gstr(b2)); WR32(bp + 0x10, gstr(b3)); WR32(up + 8, bp); }
	return prm;
}
static int msg_status(void) { return (int)call(h_msg_dlg_status, 0); }
static void test_msg(void) {
	hud = 1;   /* a window to answer in: nothing answers by itself */
	uint32_t res = gzero(0x2c), prm;
	MsgDlgView v;
	CHECKEQ(call(h_msg_dlg_init, 1, 0), 0x80020402u);
	CHECKEQ(msg_status(), ST_NONE);
	CHECKEQ(call(h_msg_dlg_result, 1, res), 0x80020404u);   /* never opened */

	/* USER_MSG with Yes / No: RUNNING until the player answers; No is id 2 and what Circle presses */
	prm = msg_user(1, "Quit the game?", 0, 0, 0);
	CHECKEQ(call(h_msg_dlg_init, 1, prm), 0);
	for (int i = 0; i < 10; i++) CHECKEQ(msg_status(), ST_RUNNING);   /* polled: no window answer, no auto answer */
	CHECKEQ(call(h_msg_dlg_result, 1, res), 0x80020410u);
	CHECKEQ(msgdlg_view(&v), 1); msgdlg_release();
	CHECKEQ(v.nbuttons, 2); CHECK(!strcmp(v.label[0], "Yes") && !strcmp(v.label[1], "No") && !strcmp(v.msg, "Quit the game?")); CHECKEQ(v.back, 1);
	msgdlg_answer(v.seq - 1, 0); CHECKEQ(msg_status(), ST_RUNNING);   /* an answer to a dialog that has gone is dropped */
	msgdlg_answer(v.seq, 1);
	CHECKEQ(msg_status(), ST_FINISHED);
	CHECKEQ(call(h_msg_dlg_result, 1, res), 0); CHECKEQ(RD32(res), 1); CHECKEQ(RD32(res + 4), 0); CHECKEQ(RD32(res + 8), 2);   /* mode, OK, button NO */
	CHECKEQ(msgdlg_view(&v), 0); msgdlg_release();
	CHECKEQ(call(h_msg_dlg_term, 0), 0); CHECKEQ(msg_status(), ST_NONE);

	/* OK alone; and OK / Cancel; and Cancel alone (id 2); and three buttons with the game's own texts */
	CHECKEQ(call(h_msg_dlg_init, 1, msg_user(0, "Saved.", 0, 0, 0)), 0);
	msgdlg_view(&v); msgdlg_release(); CHECKEQ(v.nbuttons, 1); CHECK(!strcmp(v.label[0], "OK")); CHECKEQ(v.back, -1);
	msgdlg_answer(v.seq, 0); call(h_msg_dlg_result, 1, res); CHECKEQ(RD32(res + 8), 1); call(h_msg_dlg_term, 0);
	CHECKEQ(call(h_msg_dlg_init, 1, msg_user(3, "Delete?", 0, 0, 0)), 0);
	msgdlg_view(&v); msgdlg_release(); CHECKEQ(v.nbuttons, 2); CHECK(!strcmp(v.label[1], "Cancel")); call(h_msg_dlg_term, 0);
	CHECKEQ(call(h_msg_dlg_init, 1, msg_user(4, "Wait", 0, 0, 0)), 0);
	msgdlg_view(&v); msgdlg_release(); CHECKEQ(v.nbuttons, 1); CHECK(!strcmp(v.label[0], "Cancel"));
	msgdlg_answer(v.seq, 0); call(h_msg_dlg_result, 1, res); CHECKEQ(RD32(res + 8), 2); call(h_msg_dlg_term, 0);
	CHECKEQ(call(h_msg_dlg_init, 1, msg_user(5, "Pick", "Alpha", "Beta", "Gamma")), 0);
	msgdlg_view(&v); msgdlg_release(); CHECKEQ(v.nbuttons, 3); CHECK(!strcmp(v.label[0], "Alpha") && !strcmp(v.label[2], "Gamma")); CHECKEQ(v.back, -1);
	msgdlg_answer(v.seq, 2); call(h_msg_dlg_result, 1, res); CHECKEQ(RD32(res + 8), 3); call(h_msg_dlg_term, 0);

	/* NONE: a card with no buttons stays until the game closes it; Close finishes it with OK */
	CHECKEQ(call(h_msg_dlg_init, 1, msg_user(2, "Please wait", 0, 0, 0)), 0);
	CHECKEQ(msgdlg_view(&v), 2); msgdlg_release(); CHECK(!cdlg_active());
	for (int i = 0; i < 10; i++) CHECKEQ(msg_status(), ST_RUNNING);
	CHECKEQ(call(h_msg_dlg_close, 0), 0); CHECKEQ(msg_status(), ST_FINISHED);
	CHECKEQ(call(h_msg_dlg_result, 1, res), 0); CHECKEQ(RD32(res + 4), 0); CHECKEQ(RD32(res + 8), 0);
	CHECKEQ(call(h_msg_dlg_close, 0), 0x80020404u);   /* already finished */
	call(h_msg_dlg_term, 0);

	/* SYSTEM_MSG, ERROR_CODE and PROGRESS_BAR (a bar's value is clamped; SetMsg changes its text) */
	prm = gzero(0x88); uint32_t sp = gzero(0x28); WR32(prm + 0x50, 2); WR32(prm + 0x58, sp); WR32(sp, 2);   /* NOSPACE */
	CHECKEQ(call(h_msg_dlg_init, 1, prm), 0); msgdlg_view(&v); msgdlg_release();
	CHECK(!strcmp(v.msg, "There is not enough free space on the memory card.")); CHECKEQ(v.nbuttons, 1); call(h_msg_dlg_term, 0);
	WR32(sp, 6);   /* WAIT_CANCEL */
	CHECKEQ(call(h_msg_dlg_init, 1, prm), 0); msgdlg_view(&v); msgdlg_release(); CHECKEQ(v.nbuttons, 1); CHECK(!strcmp(v.label[0], "Cancel")); CHECKEQ(v.back, 0); call(h_msg_dlg_term, 0);
	prm = gzero(0x88); uint32_t ep = gzero(0x24); WR32(prm + 0x50, 3); WR32(prm + 0x5c, ep); WR32(ep, 0x80010001u);
	CHECKEQ(call(h_msg_dlg_init, 1, prm), 0); msgdlg_view(&v); msgdlg_release(); CHECK(!strcmp(v.msg, "An error occurred. (80010001)")); call(h_msg_dlg_term, 0);
	prm = gzero(0x88); uint32_t pp = gzero(0x50); WR32(prm + 0x50, 4); WR32(prm + 0x60, pp); WR32(pp + 0x2c, gstr("Installing"));
	CHECKEQ(call(h_msg_dlg_init, 1, prm), 0); CHECKEQ(msgdlg_view(&v), 2); msgdlg_release(); CHECKEQ(v.percent, 0); CHECK(!strcmp(v.msg, "Installing"));
	call(h_msg_bar_inc, 2, 0, 30); call(h_msg_bar_set, 2, 0, 0); call(h_msg_bar_inc, 2, 0, 30);
	msgdlg_view(&v); msgdlg_release(); CHECKEQ(v.percent, 30);
	call(h_msg_bar_set, 2, 0, 500); msgdlg_view(&v); msgdlg_release(); CHECKEQ(v.percent, 100);
	call(h_msg_bar_msg, 2, 0, gstr("Almost done")); msgdlg_view(&v); msgdlg_release(); CHECK(!strcmp(v.msg, "Almost done"));
	CHECKEQ(call(h_msg_dlg_abort, 0), 0); call(h_msg_dlg_result, 1, res); CHECKEQ(RD32(res), 4); CHECKEQ(RD32(res + 4), 2);   /* ABORTED */
	call(h_msg_dlg_term, 0);

	/* what the console refuses: a mode it doesn't have, a missing sub-parameter, a button type past the list */
	prm = gzero(0x88); WR32(prm + 0x50, 9); CHECKEQ(call(h_msg_dlg_init, 1, prm), 0x80100a01u); CHECKEQ(msg_status(), ST_NONE);
	prm = gzero(0x88); WR32(prm + 0x50, 1); CHECKEQ(call(h_msg_dlg_init, 1, prm), 0x80100a01u);
	CHECKEQ(call(h_msg_dlg_init, 1, msg_user(9, "x", 0, 0, 0)), 0x80100a01u);

	/* headless: the first button answers after a few polls, the dialog opened with Yes / No answers Yes */
	hud = 0;
	CHECKEQ(call(h_msg_dlg_init, 1, msg_user(1, "Continue?", 0, 0, 0)), 0);
	CHECKEQ(msg_status(), ST_RUNNING); CHECKEQ(msg_status(), ST_RUNNING);
	CHECKEQ(msg_status(), ST_FINISHED);
	call(h_msg_dlg_result, 1, res); CHECKEQ(RD32(res + 4), 0); CHECKEQ(RD32(res + 8), 1); call(h_msg_dlg_term, 0);
	CHECKEQ(call(h_msg_dlg_init, 1, msg_user(2, "Loading", 0, 0, 0)), 0);   /* no buttons: the game closes it even headless */
	for (int i = 0; i < 10; i++) CHECKEQ(msg_status(), ST_RUNNING);
	call(h_msg_dlg_term, 0);
}
int main(void) {
	recomp_mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (recomp_mem == MAP_FAILED) { perror("mmap"); return 2; }
	test_msg();
	fprintf(stderr, "%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
