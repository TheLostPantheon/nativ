/* savedlg_pad_test.c -- SAVE-DLG-01: the save dialog answers the controller's keys. The dialog's UI
 * (savedlg_ui.cpp, included here) reads ImGui's gamepad keys (Cross = choose, Circle = back, dpad/left
 * stick = move); the host feeds those keys from its own pad (savedlg_ui_gamepad_button/axis, which
 * overlay_process_event calls for the pad events routed to the overlay). The failure this pins: the
 * dialog was blind to the controller (the player could only answer with mouse/keyboard), because
 * ImGui's SDL3 backend polls just the gamepads it opened itself and its list went stale when the pad
 * connected after the overlay's first frame.
 *
 * Case 1 is the fail-before condition as observed: a LIST step that receives NO key events never gets
 * answered (the game waits forever -- the owner's report).
 * Cases 2-5 are the pass-after contract: keys delivered through the feed seam answer the selected row,
 * move the selection, back out, and respect the arm rule (a key already held when the step appeared
 * answers nothing).
 *
 * Includes apputil.c directly (override APPUTIL_UNDER_TEST to compile a different copy) and
 * savedlg_ui.cpp directly (override SAVEDLG_UI_UNDER_TEST), same fixture style as dlg_term_test.c.
 * Build and run: see README.md here.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef APPUTIL_UNDER_TEST
#define APPUTIL_UNDER_TEST "../core/apputil.c"
#endif
#ifndef SAVEDLG_UI_UNDER_TEST
#define SAVEDLG_UI_UNDER_TEST "../ui/savedlg_ui.cpp"
#endif
#include APPUTIL_UNDER_TEST
#include SAVEDLG_UI_UNDER_TEST
#include <SDL3/SDL_gamepad.h>

/* ---- apputil.c expects these (same shim style as dlg_term_test.c) ---- */
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

/* one dialog step (what apputil.c's sd_step leaves behind for the overlay): a LIST of three rows */
static void open_list(int seq) {
	memset(&sd, 0, sizeof sd);
	sd.open = 1; sd.status = ST_RUNNING; sd.sub = ST_RUNNING;
	sd.kind = SAVEDLG_LIST; sd.seq = seq; sd.focus = 0; sd.disp = 1; sd.percent = -1;
	strcpy(sd.title, "Save");
	static SaveDlgRow rows[3];
	memset(rows, 0, sizeof rows);
	rows[0].id = 7; strcpy(rows[0].title, "New Saved Data");
	rows[1].id = 8; rows[1].exists = 1; strcpy(rows[1].title, "Alpha"); strcpy(rows[1].date, "2026-11-05 20:00");
	rows[2].id = 9; rows[2].exists = 1; strcpy(rows[2].title, "Beta"); strcpy(rows[2].date, "2026-11-05 21:00");
	sd.nrows = 3; memcpy(sd.rows, rows, sizeof sd.rows[0] * 3);
}

static void frame(void) {
	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2(960, 544);
	io.DeltaTime = 1.0f / 60.0f;
	ImGui::NewFrame();
	savedlg_ui_build(960, 544);
	ImGui::Render();
}

/* one press+release of a pad button over two frames (an edge, as SDL delivers it) */
static void press(int sdl_button) {
	savedlg_ui_gamepad_button(sdl_button, 1);
	frame();
	savedlg_ui_gamepad_button(sdl_button, 0);
	frame();
}

int main(void) {
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;
	unsigned char *px; int fw, fh;
	io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);   /* build the atlas: no backend to do it */

	/* Case 1 (fail-before condition, the reported failure): a LIST step that receives no key
	 * events is never answered -- the game polls the dialog forever. */
	open_list(1);
	frame(); frame(); frame();
	check("no delivered keys -> the dialog stays unanswered", sd.sub == ST_RUNNING && sd.kind == SAVEDLG_LIST);

	/* Case 2 (fail-before, the root defect): a gamepad key fed straight into ImGui's key state is
	 * CLEARED again every frame while ImGuiBackendFlags_HasGamepad is unset (imgui.cpp
	 * UpdateKeyboardInputs: "Clear gamepad data if disabled") -- the SDL3 backend only sets the flag
	 * for gamepads it opened itself, so the dialog never saw the controller's keys. */
	open_list(2);
	frame();
	io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
	frame();
	io.AddKeyEvent(ImGuiKey_GamepadFaceDown, false);
	frame();
	check("raw AddKeyEvent without HasGamepad is wiped -> unanswered", sd.sub == ST_RUNNING && sd.kind == SAVEDLG_LIST);

	/* Case 3 (pass-after): Cross through the feed (which marks HasGamepad) answers the focused row. */
	open_list(3);
	frame();                        /* arms: no key was down as the step appeared */
	press(SDL_GAMEPAD_BUTTON_SOUTH);
	check("Cross (feed) answers the focused row (slot 7)", sd.sub == ST_FINISHED && sd.result == 0 && sd.slot == 7);

	/* Case 4: dpad down moves the selection, Cross answers the next row. */
	open_list(4);
	frame();
	savedlg_ui_gamepad_button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, 1);
	frame();
	savedlg_ui_gamepad_button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0);
	frame();
	press(SDL_GAMEPAD_BUTTON_SOUTH);
	check("dpad down then Cross answers row 2 (slot 8)", sd.sub == ST_FINISHED && sd.result == 0 && sd.slot == 8);

	/* Case 5: Circle backs out (USER_CANCELED). */
	open_list(5);
	frame();
	press(SDL_GAMEPAD_BUTTON_EAST);
	check("Circle (feed) cancels the step", sd.sub == ST_FINISHED && sd.result == 1);

	/* Case 6: the arm rule -- Cross held when the step appeared answers nothing until released
	 * and pressed again. */
	open_list(6);
	savedlg_ui_gamepad_button(SDL_GAMEPAD_BUTTON_SOUTH, 1);
	frame(); frame();
	check("Cross held at step start answers nothing", sd.sub == ST_RUNNING);
	savedlg_ui_gamepad_button(SDL_GAMEPAD_BUTTON_SOUTH, 0);
	frame();
	press(SDL_GAMEPAD_BUTTON_SOUTH);
	check("released and pressed again: the row is answered", sd.sub == ST_FINISHED && sd.result == 0 && sd.slot == 7);

	ImGui::DestroyContext();
	if (fails) { printf("%d checks FAILED\n", fails); return 1; }
	printf("all passed (7 ok)\n");
	return 0;
}
