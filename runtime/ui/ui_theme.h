/*
 * Project Nativ's look, shared by the launcher (launcher/hub.cpp) and the in-game overlay (host/overlay.cpp): one
 * palette (a dark slate ground, warm off-white text, the amber the in-game badges use as the accent), the
 * system's own UI face, the logo (drawn, so it's sharp at any size: assets/brand/make.py's geometry), and the
 * few controls both draw the same way -- the one primary button a view offers, a segmented choice, status
 * dots, and settings rows (a label with a line of explanation on the left, its control on the right).
 * Header-only C++ over Dear ImGui.
 */
#ifndef ATIV_UI_THEME_H
#define ATIV_UI_THEME_H
#include "imgui.h"
#include <cstdio>
#include <initializer_list>
#include <sys/stat.h>

namespace ui {
inline const ImVec4 bg(0.071f, 0.078f, 0.098f, 1), surface(0.102f, 0.114f, 0.141f, 1), card(0.125f, 0.141f, 0.173f, 1),
	card_hi(0.149f, 0.168f, 0.204f, 1), border(0.173f, 0.192f, 0.231f, 1), text(0.914f, 0.902f, 0.875f, 1), muted(0.565f, 0.592f, 0.639f, 1),
	accent(0.941f, 0.706f, 0.298f, 1), accent_hi(0.961f, 0.773f, 0.424f, 1), accent_ink(0.10f, 0.08f, 0.04f, 1),
	ready(0.345f, 0.761f, 0.545f, 1), warn(0.910f, 0.639f, 0.239f, 1), error(0.898f, 0.376f, 0.353f, 1);
inline ImVec4 with_alpha(const ImVec4 &c, float a) { return ImVec4(c.x, c.y, c.z, c.w * a); }
inline ImU32 u32(const ImVec4 &c, float a = 1) { return ImGui::GetColorU32(with_alpha(c, a)); }

/* the style: sizes in points at font_size, scaled by the display's content scale */
inline void apply(float font_size, float scale) {
	ImGuiStyle &st = ImGui::GetStyle();
	ImGui::StyleColorsDark();
	st.FontSizeBase = font_size; st.FontScaleDpi = scale;
	const float k = font_size / 16.0f;
	st.WindowPadding = ImVec2(22 * k, 18 * k); st.FramePadding = ImVec2(14 * k, 7 * k); st.ItemSpacing = ImVec2(12 * k, 9 * k);
	st.ItemInnerSpacing = ImVec2(8 * k, 6 * k); st.CellPadding = ImVec2(10 * k, 7 * k); st.SeparatorTextPadding = ImVec2(0, 6 * k);
	st.WindowRounding = 12 * k; st.ChildRounding = 10 * k; st.FrameRounding = 7 * k; st.PopupRounding = 10 * k; st.GrabRounding = 7 * k;
	st.ScrollbarRounding = 7 * k; st.TabRounding = 7 * k; st.GrabMinSize = 14 * k; st.ScrollbarSize = 11 * k;
	st.WindowBorderSize = 1; st.ChildBorderSize = 1; st.PopupBorderSize = 1; st.FrameBorderSize = 0; st.TabBorderSize = 0;
	ImVec4 *c = st.Colors;
	c[ImGuiCol_Text] = text; c[ImGuiCol_TextDisabled] = muted; c[ImGuiCol_WindowBg] = bg; c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_PopupBg] = surface; c[ImGuiCol_Border] = border; c[ImGuiCol_FrameBg] = card; c[ImGuiCol_FrameBgHovered] = card_hi;
	c[ImGuiCol_FrameBgActive] = card_hi; c[ImGuiCol_Button] = card_hi; c[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.22f, 0.27f, 1);
	c[ImGuiCol_ButtonActive] = ImVec4(0.24f, 0.26f, 0.32f, 1); c[ImGuiCol_Header] = card_hi; c[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.22f, 0.27f, 1);
	c[ImGuiCol_HeaderActive] = ImVec4(0.24f, 0.26f, 0.32f, 1); c[ImGuiCol_Separator] = border; c[ImGuiCol_SeparatorHovered] = border;
	c[ImGuiCol_CheckMark] = accent; c[ImGuiCol_SliderGrab] = accent; c[ImGuiCol_SliderGrabActive] = accent_hi; c[ImGuiCol_TextLink] = accent;
	c[ImGuiCol_TitleBg] = surface; c[ImGuiCol_TitleBgActive] = surface; c[ImGuiCol_MenuBarBg] = surface; c[ImGuiCol_Tab] = card;
	c[ImGuiCol_TabHovered] = card_hi; c[ImGuiCol_TabSelected] = card_hi; c[ImGuiCol_TabSelectedOverline] = accent;
	c[ImGuiCol_TableHeaderBg] = card; c[ImGuiCol_TableBorderLight] = border; c[ImGuiCol_TableBorderStrong] = border;
	c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
	c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.02f, 0.02f, 0.03f, 0.72f); c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_ScrollbarGrab] = card_hi; c[ImGuiCol_NavCursor] = accent; c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
	st.ScaleAllSizes(scale);
}
/* the system's own UI face, read at run time (nothing shipped): San Francisco, Segoe UI, a common Linux sans;
 * ImGui's built-in vector font when none is there. ImGui 1.92's fonts size themselves: one face, any size */
inline ImFont *load_system_font(ImGuiIO &io) {
	static const char *const faces[] = {"/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc", "C:\\Windows\\Fonts\\segoeui.ttf",
		"/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/cantarell/Cantarell-VF.otf", 0};
	for (int i = 0; faces[i]; i++) { struct stat st; if (stat(faces[i], &st) == 0) if (ImFont *f = io.Fonts->AddFontFromFileTTF(faces[i])) return f; }
	return io.Fonts->AddFontDefaultVector();
}

inline bool primary_button(const char *label, ImVec2 size = ImVec2(0, 0)) {   /* the one amber button a view offers */
	ImGui::PushStyleColor(ImGuiCol_Button, accent); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent_hi);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent); ImGui::PushStyleColor(ImGuiCol_Text, accent_ink);
	bool r = ImGui::Button(label, size);
	ImGui::PopStyleColor(4);
	return r;
}
/* a choice of n as one joined control; true when it changes. ids: labels may carry "##" suffixes */
inline bool segmented(const char *const *labels, int n, int *v) {
	bool ch = false;
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x * 0.25f, 0));
	for (int i = 0; i < n; i++) {
		if (i) ImGui::SameLine();
		bool on = *v == i;
		ImGui::PushStyleColor(ImGuiCol_Button, on ? with_alpha(accent, 0.18f) : card);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? with_alpha(accent, 0.24f) : card_hi);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, with_alpha(accent, 0.30f));
		ImGui::PushStyleColor(ImGuiCol_Text, on ? accent : muted);
		if (ImGui::Button(labels[i]) && !on) { *v = i; ch = true; }
		ImGui::PopStyleColor(4);
	}
	ImGui::PopStyleVar();
	return ch;
}
inline bool toggle(const char *id, bool *on) {   /* Off / On as a segmented pair */
	char a[64], b[64]; snprintf(a, sizeof a, "Off##%s", id); snprintf(b, sizeof b, "On##%s", id);
	const char *const ls[2] = {a, b};
	int v = *on ? 1 : 0;
	if (!segmented(ls, 2, &v)) return false;
	*on = v != 0;
	return true;
}
inline void heading(const char *s, float size) { ImGui::PushFont(nullptr, size); ImGui::TextUnformatted(s); ImGui::PopFont(); }
inline void muted_wrapped(const char *s) { ImGui::PushStyleColor(ImGuiCol_Text, muted); ImGui::TextWrapped("%s", s); ImGui::PopStyleColor(); }
inline void dot(const ImVec4 &c) {   /* a status dot, centred on the text line */
	ImVec2 p = ImGui::GetCursorScreenPos(); float h = ImGui::GetTextLineHeight(), r = h * 0.22f;
	ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + h * 0.5f), r, u32(c));
	ImGui::Dummy(ImVec2(r * 2, h)); ImGui::SameLine(0, h * 0.35f);
}
inline void spinner(const ImVec4 &c) {
	ImVec2 p = ImGui::GetCursorScreenPos(); float h = ImGui::GetTextLineHeight(), r = h * 0.36f, t = (float)ImGui::GetTime() * 6.0f;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->PathArcTo(ImVec2(p.x + r, p.y + h * 0.5f), r, t, t + 4.4f, 20); dl->PathStroke(u32(c), 0, r * 0.32f);
	ImGui::Dummy(ImVec2(r * 2, h)); ImGui::SameLine(0, h * 0.4f);
}
inline void chip(const char *s, const ImVec4 &c) {   /* a small rounded label, tinted */
	ImVec2 p = ImGui::GetCursorScreenPos(), ts = ImGui::CalcTextSize(s); float px = ImGui::GetFontSize() * 0.55f, py = ImGui::GetFontSize() * 0.18f;
	ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + ts.x + px * 2, p.y + ts.y + py * 2), u32(c, 0.16f), ts.y);
	ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + px, p.y + py), u32(c), s);
	ImGui::Dummy(ImVec2(ts.x + px * 2, ts.y + py * 2));
}

/* settings rows: rows_begin, then per setting row(label, explanation) and its control(s), then rows_end */
inline bool rows_begin(const char *id) {
	if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) return false;
	ImGui::TableSetupColumn("setting", ImGuiTableColumnFlags_WidthStretch, 1.0f);
	ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch, 1.15f);
	return true;
}
inline void row(const char *label, const char *explanation = nullptr) {
	ImGui::TableNextRow(0, ImGui::GetFrameHeight() + ImGui::GetStyle().CellPadding.y * 2);
	ImGui::TableNextColumn();
	ImGui::TextUnformatted(label);
	if (explanation && *explanation) { ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.88f); muted_wrapped(explanation); ImGui::PopFont(); }
	ImGui::TableNextColumn();
}
inline void rows_end(void) { ImGui::EndTable(); }

/* the logo, drawn (assets/brand/make.py has the same geometry as SVG). wordmark: "nativ" in lowercase monoline, the n
 * in the text colour and "ativ" (Vita, backwards) in amber, `height` px tall (its units: x -6..282, y 14..110), 3x as
 * wide; returns the width */
inline float wordmark(ImDrawList *dl, ImVec2 at, float height, ImU32 n = 0, ImU32 ativ = 0) {
	if (!n) n = u32(text);
	if (!ativ) ativ = u32(accent);
	const float s = height / 96.0f, w = 14 * s;
	auto P = [&](float x, float y) { return ImVec2(at.x + (x + 6) * s, at.y + (y - 14) * s); };
	auto cap = [&](float x, float y, ImU32 c) { dl->AddCircleFilled(P(x, y), w * 0.5f, c); };
	auto line = [&](float x0, float y0, float x1, float y1, ImU32 c) { dl->AddLine(P(x0, y0), P(x1, y1), c, w); cap(x0, y0, c); cap(x1, y1, c); };
	line(4, 46, 4, 100, n);                                                                  /* n */
	dl->PathLineTo(P(4, 72)); dl->PathBezierCubicCurveTo(P(4, 54), P(14, 46), P(26, 46)); dl->PathBezierCubicCurveTo(P(38, 46), P(48, 54), P(48, 72));
	dl->PathLineTo(P(48, 100)); dl->PathStroke(n, 0, w); cap(48, 100, n);
	dl->AddCircle(P(101, 73), 27 * s, ativ, 0, w); line(128, 46, 128, 100, ativ);            /* a */
	dl->PathLineTo(P(161, 24)); dl->PathLineTo(P(161, 84)); dl->PathBezierQuadraticCurveTo(P(161, 100), P(177, 100));   /* t */
	dl->PathStroke(ativ, 0, w); cap(161, 24, ativ); cap(177, 100, ativ); line(149, 46, 175, 46, ativ);
	line(203, 46, 203, 100, ativ); dl->AddCircleFilled(P(203, 25), 8.4f * s, ativ);         /* i */
	dl->PathLineTo(P(225, 46)); dl->PathLineTo(P(242, 92)); dl->PathBezierQuadraticCurveTo(P(247, 104), P(252, 92)); dl->PathLineTo(P(269, 46));   /* v */
	dl->PathStroke(ativ, 0, w); cap(225, 46, ativ); cap(269, 46, ativ);
	return 288 * s;
}
/* the Vita in outline, `width` px wide (its units 600 x 280): the screen filled `screen`; controls 0 none, 1 the
 * sticks as dots (small sizes), 2 d-pad left, face buttons right, the two sticks; returns the height */
inline float vita(ImDrawList *dl, ImVec2 at, float width, ImU32 line, ImU32 screen, float body_w = 14, int controls = 2) {
	const float s = width / 600.0f;
	auto P = [&](float x, float y) { return ImVec2(at.x + x * s, at.y + y * s); };
	dl->AddRectFilled(P(125, 41), P(475, 239), screen, 12 * s);
	dl->AddRect(P(8, 8), P(592, 272), line, 132 * s, 0, body_w * s);
	if (controls == 1) { dl->AddCircleFilled(P(70, 140), 22 * s, line); dl->AddCircleFilled(P(530, 140), 22 * s, line); }
	if (controls == 2) {
		const float k = 12 * s;
		dl->AddLine(P(50, 108), P(82, 108), line, k); dl->AddLine(P(66, 92), P(66, 124), line, k);
		for (ImVec2 c : {P(50, 108), P(82, 108), P(66, 92), P(66, 124)}) dl->AddCircleFilled(c, k * 0.5f, line);
		dl->AddCircle(P(74, 190), 14 * s, line, 0, 10 * s); dl->AddCircle(P(526, 190), 14 * s, line, 0, 10 * s);
		for (ImVec2 c : {P(534, 90), P(516, 108), P(552, 108), P(534, 126)}) dl->AddCircleFilled(c, 8 * s, line);
	}
	return 280 * s;
}
/* the wordmark as a layout item, `height` px tall, on the current line */
inline void wordmark_item(float height) {
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(wordmark(ImGui::GetWindowDrawList(), p, height), height));
}
}   /* namespace ui */
#endif
