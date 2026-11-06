/* savedlg_ui.cpp -- the save-data dialog's UI: the game's save / load / delete list, a yes/no question,
 * or a card the game closes itself (Saving..., a progress bar) -- over the picture while the game waits on it,
 * the game running underneath (it polls the dialog every frame; nothing pauses). The pad: up/down (left/right
 * on a question), Cross to choose, Circle to go back; the keyboard: arrows, Enter or Space, Escape or
 * Backspace; the mouse: hover and click. window.c hands this the keys and the pad while overlay_dialog_active.
 * SAVE-DLG-01: the pad's buttons and stick arrive as ImGui keys through savedlg_ui_gamepad_* (fed by
 * overlay_process_event from the pad events routed to the overlay), NOT through ImGui's SDL3 backend -- that
 * backend polls only the gamepads it opened itself, and its list goes stale when a controller connects after
 * the overlay's first frame (Nativ routes GAMEPAD_ADDED to the overlay only while the menu or a dialog is up),
 * so the dialog used to be blind to the controller while the game's own pad worked.
 */
#include "savedlg_ui.h"
#include "imgui.h"
#include "savedlg.h"
#include <SDL3/SDL_gamepad.h>
#include <cfloat>
#include <cmath>

static int g_sd_seq = -1, g_sd_sel; static bool g_sd_armed;

static const ImGuiKey K_UP[] = {ImGuiKey_UpArrow, ImGuiKey_W, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp};
static const ImGuiKey K_DOWN[] = {ImGuiKey_DownArrow, ImGuiKey_S, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown};
static const ImGuiKey K_SIDE[] = {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_A, ImGuiKey_D, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight};
static const ImGuiKey K_OK[] = {ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown};
static const ImGuiKey K_BACK[] = {ImGuiKey_Escape, ImGuiKey_Backspace, ImGuiKey_GamepadFaceRight};
template <size_t N> static bool keys_pressed(const ImGuiKey (&k)[N]) { for (ImGuiKey x : k) if (ImGui::IsKeyPressed(x, true)) return true; return false; }
template <size_t N> static bool keys_down(const ImGuiKey (&k)[N]) { for (ImGuiKey x : k) if (ImGui::IsKeyDown(x)) return true; return false; }
void savedlg_ui_build(float w, float hgt) {
	SaveDlgView v; const int kind = savedlg_view(&v);
	if (kind == SAVEDLG_NONE) { savedlg_release(); g_sd_seq = -1; return; }
	if (v.seq != g_sd_seq) { g_sd_seq = v.seq; g_sd_sel = kind == SAVEDLG_LIST ? v.focus : 0; g_sd_armed = false; }
	/* a key already down as the step appeared (the press that asked the game to save) answers nothing */
	if (!g_sd_armed && !keys_down(K_OK) && !keys_down(K_BACK)) g_sd_armed = true;
	const bool ok = g_sd_armed && keys_pressed(K_OK), back = g_sd_armed && keys_pressed(K_BACK);
	int answer = 0; bool answered = false, moved = false;
	const float s = w >= 1400 ? 2.0f : 1.0f;   /* (the bitmap font: integer multiples stay crisp) */
	ImGui::SetNextWindowPos(ImVec2(w * 0.5f, hgt * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(fminf(w - 32, 520 * s), 0), ImGuiCond_Always);   /* (height 0: fitted every frame) */
	ImGui::SetNextWindowBgAlpha(0.95f);
	ImGui::Begin("##savedlg", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
	ImGui::SetWindowFontScale(s);
	const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled), txt = ImGui::GetColorU32(ImGuiCol_Text);
	if (kind == SAVEDLG_LIST) {
		ImGui::TextUnformatted(v.title[0] ? v.title : "Saved data");
		ImGui::Separator();
		if (g_sd_armed && v.nrows) {
			if (keys_pressed(K_UP) && g_sd_sel > 0) { g_sd_sel--; moved = true; }
			if (keys_pressed(K_DOWN) && g_sd_sel < v.nrows - 1) { g_sd_sel++; moved = true; }
		}
		if (g_sd_sel >= v.nrows) g_sd_sel = v.nrows ? v.nrows - 1 : 0;
		if (!v.nrows) ImGui::TextDisabled("There is no saved data.");
		else {
			const float lh = ImGui::GetTextLineHeight(), rowh = 3 * lh + 12 * s;
			ImGui::BeginChild("rows", ImVec2(0, (rowh + ImGui::GetStyle().ItemSpacing.y) * (float)(v.nrows < 5 ? v.nrows : 5)));
			ImFont *font = ImGui::GetFont(); const float fs = ImGui::GetFontSize();
			for (int i = 0; i < v.nrows; i++) {
				const SaveDlgRow &r = v.rows[i];
				ImGui::PushID(i);
				const ImVec2 p = ImGui::GetCursorScreenPos(); const float rw = ImGui::GetContentRegionAvail().x;
				if (ImGui::Selectable("##row", i == g_sd_sel, 0, ImVec2(0, rowh))) { answer = i; answered = true; }
				if (ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x || ImGui::GetIO().MouseDelta.y)) g_sd_sel = i;
				if (moved && i == g_sd_sel) ImGui::SetScrollHereY(0.5f);
				ImDrawList *dl = ImGui::GetWindowDrawList();
				dl->AddText(font, fs, ImVec2(p.x + 6 * s, p.y + 4 * s), txt, r.title);
				if (r.exists) {
					const float dw = font->CalcTextSizeA(fs, FLT_MAX, 0, r.date).x;
					dl->AddText(font, fs, ImVec2(p.x + rw - dw - 6 * s, p.y + 4 * s), dim, r.date);
					if (r.sub[0]) dl->AddText(font, fs, ImVec2(p.x + 6 * s, p.y + 6 * s + lh), txt, r.sub);
					if (r.detail[0]) dl->AddText(font, fs, ImVec2(p.x + 6 * s, p.y + 8 * s + 2 * lh), dim, r.detail);
				} else dl->AddText(font, fs, ImVec2(p.x + 6 * s, p.y + 6 * s + lh), dim, "(empty)");
				ImGui::PopID();
			}
			ImGui::EndChild();
		}
		if (ok && v.nrows) { answer = g_sd_sel; answered = true; }
		ImGui::Separator();
		if (ImGui::Button("Back") || back) { answer = SAVEDLG_CANCEL; answered = true; }
		ImGui::SameLine(); ImGui::TextDisabled("  Cross / Enter: choose    Circle / Esc: back");
	} else if (kind == SAVEDLG_YESNO) {
		ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v.msg[0] ? v.msg : "Continue?"); ImGui::PopTextWrapPos();
		ImGui::Spacing();
		if (g_sd_armed && (keys_pressed(K_SIDE) || keys_pressed(K_UP) || keys_pressed(K_DOWN))) g_sd_sel ^= 1;
		static const char *const names[2] = {"Yes", "No"};
		for (int b = 0; b < 2; b++) {
			if (b) ImGui::SameLine();
			const bool sel = g_sd_sel == b;
			if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			if (ImGui::Button(names[b], ImVec2(90 * s, 0))) { answer = b ? SAVEDLG_NO : SAVEDLG_YES; answered = true; }
			if (sel) ImGui::PopStyleColor();
			if (ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x || ImGui::GetIO().MouseDelta.y)) g_sd_sel = b;
		}
		if (ok) { answer = g_sd_sel ? SAVEDLG_NO : SAVEDLG_YES; answered = true; }
		if (back) { answer = SAVEDLG_CANCEL; answered = true; }
		ImGui::SameLine(); ImGui::TextDisabled("  Cross / Enter    Circle / Esc: back");
	} else {   /* SAVEDLG_WAIT: the game closes it */
		ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v.msg[0] ? v.msg : "Please wait..."); ImGui::PopTextWrapPos();
		if (v.percent >= 0) ImGui::ProgressBar(v.percent / 100.0f, ImVec2(-1, 0));
	}
	ImGui::End();
	const int seq = v.seq;
	savedlg_release();   /* (before answering: savedlg_answer takes the same lock) */
	if (answered) savedlg_answer(seq, answer);
}

extern "C" {

/* every feed sets ImGuiBackendFlags_HasGamepad: ImGui clears every gamepad key each frame while the flag is
 * unset (imgui.cpp UpdateKeyboardInputs: "Clear gamepad data if disabled"), and the SDL3 backend sets it only
 * when it has gamepads of its own open -- a controller that connected after the overlay's first frame never
 * gets there (its list goes stale), which is exactly how the save dialog came to ignore the pad (SAVE-DLG-01) */
static void sdlg_gamepad_mark(void) {
	ImGuiIO &io = ImGui::GetIO();
	io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
}

void savedlg_ui_gamepad_button(int sdl_button, int down) {
	if (!ImGui::GetCurrentContext()) return;
	sdlg_gamepad_mark();
	ImGuiKey k;
	switch (sdl_button) {
		case SDL_GAMEPAD_BUTTON_SOUTH: k = ImGuiKey_GamepadFaceDown; break;   /* Cross: the dialog's K_OK */
		case SDL_GAMEPAD_BUTTON_EAST: k = ImGuiKey_GamepadFaceRight; break;   /* Circle: K_BACK */
		case SDL_GAMEPAD_BUTTON_DPAD_UP: k = ImGuiKey_GamepadDpadUp; break;
		case SDL_GAMEPAD_BUTTON_DPAD_DOWN: k = ImGuiKey_GamepadDpadDown; break;
		case SDL_GAMEPAD_BUTTON_DPAD_LEFT: k = ImGuiKey_GamepadDpadLeft; break;
		case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: k = ImGuiKey_GamepadDpadRight; break;
		default: return;
	}
	ImGui::GetIO().AddKeyEvent(k, down != 0);
}

void savedlg_ui_gamepad_axis(int sdl_axis, int value) {
	if (!ImGui::GetCurrentContext()) return;
	ImGuiKey lo, hi;
	switch (sdl_axis) {
		case SDL_GAMEPAD_AXIS_LEFTX: lo = ImGuiKey_GamepadLStickLeft; hi = ImGuiKey_GamepadLStickRight; break;
		case SDL_GAMEPAD_AXIS_LEFTY: lo = ImGuiKey_GamepadLStickUp; hi = ImGuiKey_GamepadLStickDown; break;
		default: return;
	}
	const float dead = 8000.0f;   /* the SDL gamepad dead zone, as ImGui's SDL3 backend uses it */
	ImGui::GetIO().AddKeyAnalogEvent(lo, value < -dead, value < -dead ? (float)-value / 32768.0f : 0.0f);
	ImGui::GetIO().AddKeyAnalogEvent(hi, value > dead, value > dead ? (float)value / 32767.0f : 0.0f);
}

}   /* extern "C" */
