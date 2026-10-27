/*
 * The in-game overlay: the one C++ translation unit in this project, so Dear ImGui
 * (third_party/imgui/) stays out of every other file. overlay.h is the plain-C contract the rest of
 * the host (window.c, mainly) calls through; this file never reaches back into window.c's own
 * statics -- it calls the small getters/setters window.c, gfx_gpu.c, metrics.c and settings.c each
 * expose for it, declared locally below, the same way every other file in this project declares an
 * `extern` for a function it needs instead of sharing a header (see e.g. window.c's own use of
 * gfx_gpu_render_scale_cycle).
 *
 * Two states, one ImGui frame per present (window.c's present loop calls overlay_frame, then
 * overlay_marks; exactly one of them builds, keyed on overlay_is_open()):
 *   - the full menu (build_ui; F10, Escape, Back+Start or Guide in window.c): a tabbed window with
 *     every setting, the controls reference and the help page. The game is paused while it's up
 *     (display.c), every event is ImGui's (window.c), keyboard and gamepad navigation are on.
 *   - closed (build_closed): the HUD -- the performance overlay's lines and the gamepad-touch
 *     markers -- plus the MENU BAR: a strip that slides down from the window's top edge when the
 *     pointer rests within a few pixels of it (or the pull tab at the top centre is tapped: touch
 *     has no hover), stays while the pointer is over it or one of its menus is open, and slides
 *     away about a second after the pointer leaves. The game keeps running under it; only the full
 *     menu (which the bar's reference pages open) pauses. Its menus edit the same settings the
 *     full menu's pages do, through the same setters, and every item with a hotkey shows it, so
 *     the bar teaches the keys. Pointer-only by design (no keyboard/gamepad navigation while
 *     closed: a game's keys and pad are never ImGui's then); window.c asks overlay_mouse_captured()
 *     before it turns a mouse event into a touch, so a click or drag on the bar never reaches the
 *     game. Also here: a one-time hint toast (settings key menu_hint_shown), Game > Pause without
 *     the menu, and the OS cursor hidden after a few seconds still over the picture.
 *   - the touch card (build_intro): the first game a player starts opens it, as the menu opens (the game
 *     paused, input the overlay's): how to touch the Vita's screen with a keyboard and mouse, a controller or
 *     a touch screen, the device in use highlighted, closed by doing it once (or Escape / F10 / Guide). Once
 *     per player: shared settings key touch_intro_shown.
 *   - headless, one-shot (overlay_render_forced, RECOMP_OVERLAY_DUMP: window.c's own comment) --
 *     never touches overlay_is_open()/open/close, so it can never leave the real pause/input/audio
 *     state the rest of the host keys off that open flag for stuck on. No platform backend (no
 *     window to attach one to): the caller's own w/h stand in for a display size, there is no
 *     pointer, and RECOMP_OVERLAY_DUMP_UI picks what to force on screen (see overlay.h).
 */
#include "imgui.h"
#include "imgui_internal.h"   /* one call: ClosePopupsExceptModals (Escape with a bar menu open: overlay_bar_dismiss below) */
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"
#include "gpulock.h"   /* the SDL submit below is the locked one, and ImGui's own submits run under the same lock (render_frame): Homebrew SDL 3.4.16's Metal fence race, see that header */
#include "overlay.h"
#include "savedlg.h"
#include "ui_theme.h"
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <ctype.h>
#include <mutex>

extern "C" {
#include "online/m2.h"   /* (online_status: the Game page's Online now) */
int net_online(void);
/* display.c */
void display_set_paused(int on);
/* window.c */
int window_get_aspect(void); void window_set_aspect(int mode);
int window_get_fullscreen(void); void window_set_fullscreen(int on);
int window_raw_look(void); void window_set_raw_look(int on);
int window_controls_count(void);
int window_gyro_present(void);
void motion_recenter(void);
void window_controls_row(int i, const char **action, const char **key, const char **pad);
/* gfx_gpu.c: the GPU backend's internal resolution (1-gfx_gpu_render_scale_max) and F2's own cycle */
int gfx_gpu_render_scale(void); void gfx_gpu_render_scale_request(int n); void gfx_gpu_render_scale_cycle(void); int gfx_gpu_render_scale_max(void);
int gfx_gpu_hud_supported(void);   /* the game names its HUD draws (RECOMP_HUD_DRAWS): the HUD row applies */
int gfx_gpu_ssao_supported(void);   /* the game names its main 3D scene (RECOMP_SSAO): the ambient occlusion rows apply */
int gfx_gpu_target_scale_supported(void);   /* the game names buffers it draws below the screen's resolution (RECOMP_TARGET_SCALE) */
/* window.c: the swapchain's size in pixels (0 x 0 before the first frame); wider aspect support */
void window_pixel_size(uint32_t *w, uint32_t *h);
int window_aspect_supported(void);
float window_aspect_widen(void);
/* metrics.c: the F1 overlay's level, direct (not just cycling) and F1's cycle, and the live fps for the Performance page */
int metrics_osd_level(void); void metrics_osd_set(int level); void metrics_osd_cycle(void);
double metrics_fps(void);
int metrics_osd_lines(char (*out)[40], int max);   /* the F1 overlay's text, drawn here as the HUD (overlay_marks) */
/* settings.c */
int settings_has(const char *key);
int settings_int(const char *key, int def);
double settings_float(const char *key, double def);
void settings_set_int(const char *key, int v);
void settings_set_float(const char *key, double v);
void settings_set_str(const char *key, const char *v);
const char *settings_str(const char *key, const char *def, char *buf, int bufsize);
void settings_save(void);
}

static ImGuiContext *g_ctx;
static bool g_has_window;      /* overlay_init was given a real SDL_Window: the platform backend is live */
/* written by the window thread, read by the game thread (display.c's pause, window.c's audio): atomic,
 * like every other flag that crosses those threads (rscale_req, osd_level_v). g_paused: Game > Pause
 * in the menu bar -- the game held (display.c, as for the menu) with nothing but a banner on screen */
static std::atomic<bool> g_open, g_want_quit, g_want_capture, g_paused;
static std::atomic<bool> g_intro, g_intro_tap;   /* the touch card is what the open overlay shows; a pad's touchpad touched meanwhile */

int overlay_init(SDL_GPUDevice *device, SDL_Window *window, SDL_GPUTextureFormat color_fmt) {
	if (!device) return 0;
	if (g_ctx) return 1;   /* window.c calls this once at start-up; a repeat (shouldn't happen) is harmless */
	IMGUI_CHECKVERSION();
	g_ctx = ImGui::CreateContext();
	ImGui::SetCurrentContext(g_ctx);
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;   /* never write imgui.ini into the game's own run directory */
	/* (keyboard/gamepad navigation: set per frame in render_frame -- on with the full menu only) */
	ui::load_system_font(io);   /* an explicit face: without one, ImGui 1.92's own automatic choice (vector vs. bitmap)
		depends on a DPI scale the headless path never sets, and picks a degenerate (zero-size) one */
	ui::apply(15.0f, 1.0f);     /* the launcher's look (ui_theme.h), a point smaller: the menu sits over a 960x544 game */
	g_has_window = window != nullptr;
	if (g_has_window && !ImGui_ImplSDL3_InitForSDLGPU(window)) { ImGui::DestroyContext(g_ctx); g_ctx = nullptr; return 0; }
	ImGui_ImplSDLGPU3_InitInfo info = {};
	info.Device = device; info.ColorTargetFormat = color_fmt; info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
	if (!ImGui_ImplSDLGPU3_Init(&info)) {
		if (g_has_window) ImGui_ImplSDL3_Shutdown();
		ImGui::DestroyContext(g_ctx); g_ctx = nullptr;
		return 0;
	}
	return 1;
}
void overlay_shutdown(void) {
	if (!g_ctx) return;
	ImGui::SetCurrentContext(g_ctx);
	ImGui_ImplSDLGPU3_Shutdown();
	if (g_has_window) ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext(g_ctx);
	g_ctx = nullptr; g_has_window = false; g_open = false; g_want_quit = false; g_want_capture = false; g_paused = false;
}

int overlay_is_open(void) { return g_ctx && g_open; }
/* every pause goes through here: display.c freezes the game's clock for exactly the time the menu is up
 * or Game > Pause is on, whichever lasts longer (overlay_game_paused is what display.c and window.c's
 * audio actually poll) */
static void apply_pause(void) { display_set_paused(g_open || g_paused); }
void overlay_open(void) { if (g_ctx) { g_open = true; apply_pause(); } }
void overlay_close(void) {   /* closing the menu resumes: a Game > Pause doesn't outlive it */
	if (g_intro) { g_intro = false; settings_set_int("touch_intro_shown", 1); settings_save(); }   /* (the touch card, done or skipped: never again) */
	g_open = false; g_paused = false; apply_pause();
}
void overlay_toggle(void) { if (g_open) overlay_close(); else overlay_open(); }
void overlay_set_paused(int on) { if (g_ctx) { g_paused = on != 0; apply_pause(); } }
int overlay_game_paused(void) { return g_ctx && (g_open || g_paused); }
int overlay_wants_quit(void) { return g_want_quit.exchange(false); }
/* quitting a game the launcher started (ATIV_LAUNCHER, hub.cpp) returns to the launcher's library, which comes to the
 * front: the button says so */
static const char *quit_label(void) { return std::getenv("ATIV_LAUNCHER") ? "Back to the library" : "Quit game"; }
int overlay_wants_capture(void) { return g_want_capture.exchange(false); }

extern "C" { int window_kb_count(void); const char *window_kb_label(int i); const char *window_kb_key_name(int i);
             void window_kb_bind(int i, SDL_Keycode k); void window_kb_reset(void); int window_kbm_active(void); }
static int g_rebind = -1;   /* the Controls page's Rebind: the next key pressed goes to this action */
void overlay_process_event(const SDL_Event *event) {
	if (!g_ctx || !g_has_window) return;
	if (g_rebind >= 0 && event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat) { window_kb_bind(g_rebind, event->key.key); g_rebind = -1; return; }   /* (Escape and F10 close the menu first: window.c) */
	ImGui::SetCurrentContext(g_ctx);
	ImGui_ImplSDL3_ProcessEvent(event);
}

/* ---- the settings both the full menu's pages and the menu bar's menus edit: read once from settings.c
 * (it has no "changed since" signal, and a slider mid-drag must not snap back to the file's value on the
 * next frame), then this copy is the live value and every edit saves at once. One setter per key, so
 * the page and the menu can't drift apart. The values with a live owner elsewhere (render scale,
 * aspect, fullscreen, the overlay level) are read from that owner every frame instead. */
static struct { bool loaded; float fov, view, scen, chr, tcs, gyro_sens, vol; int fr, gyro, gyro_flat; } P;
static void prefs_load(void) {
	if (P.loaded) return;
	char b[16];
	P.fov = (float)settings_float("fov", 1.0); P.fr = settings_int("frame_rate", 30);
	P.view = (float)settings_float("view_distance", 1.0);
	P.scen = (float)settings_float("scenery_detail", 1.0);
	P.chr = (float)settings_float("character_detail", 1.0);
	P.tcs = (float)settings_float("touch_cursor_speed", 1.0);
	P.gyro = settings_int("gyro", 1); P.gyro_flat = !strcmp(settings_str("gyro_pose", "upright", b, sizeof b), "flat");
	P.gyro_sens = (float)settings_float("gyro_sens", 1.0);
	P.vol = (float)settings_float("volume", 1.0);
	P.loaded = true;
}
static void set_fov(float v) { P.fov = v; settings_set_float("fov", v); settings_save(); }
static void set_view(float v) { P.view = v; settings_set_float("view_distance", v); settings_save(); }
static void set_scen(float v) { P.scen = v; settings_set_float("scenery_detail", v); settings_save(); }
static void set_chr(float v) { P.chr = v; settings_set_float("character_detail", v); settings_save(); }
/* the per-game options this game's hooks honour, from its conf (RECOMP_GAME_OPTIONS in RUN_ENV, e.g. "fov,view_distance"):
 * a row is shown only where it does something -- Uncharted's view distance and Killzone's scenery detail are different
 * hooks, and Shovel Knight has none */
static bool game_option(const char *name) {
	const char *e = std::getenv("RECOMP_GAME_OPTIONS"); if (!e) return false;
	const size_t n = std::strlen(name);
	for (const char *p = e; (p = std::strstr(p, name)); p += n)
		if ((p == e || p[-1] == ',' || p[-1] == ' ') && (p[n] == 0 || p[n] == ',' || p[n] == ' ')) return true;
	return false;
}
static void set_frame_rate(int fr) { P.fr = fr; settings_set_int("frame_rate", fr); settings_save(); }
static void set_tcs(float v) { P.tcs = v; settings_set_float("touch_cursor_speed", v); settings_save(); }
static void set_gyro(int on) { P.gyro = on != 0; settings_set_int("gyro", P.gyro); settings_save(); }
static void set_gyro_pose(int flat) { P.gyro_flat = flat != 0; settings_set_str("gyro_pose", flat ? "flat" : "upright"); settings_save(); }
static void set_gyro_sens(float v) { P.gyro_sens = v; settings_set_float("gyro_sens", v); settings_save(); }
static void set_volume(float v) { P.vol = v; settings_set_float("volume", v); settings_save(); }   /* window.c applies it to every output port */
static void set_osd(int level) { metrics_osd_set(level); settings_set_int("osd", level); settings_save(); }

/* ---- the host's own hotkeys, one table: the Help page's Shortcuts list and the Controls page's second
 * table both draw it (the game's controls are window.c's ctrl_rows; these are the keys window.c binds
 * beside them -- keep in sync with its event loop). The bar's own shortcut texts name the same keys. */
static const char *const g_shortcuts[][3] = {
	{"Menu bar", "Move the mouse to the top edge (Esc hides it)", "Tap the tab at the top centre (touch screen)"},
	{"This menu (pauses the game)", "F10 (Esc closes it)", "Guide, or Back + Start together"},
	{"On-screen stats: off, FPS, full", "F1", "\u2014"},
	{"Resolution: 1\u00d7 to 6\u00d7", "F2", "\u2014"},
	{"The rear camera's lamp", "F7", "\u2014"},
	{"Capture a frame (to captures/)", "F12", "\u2014"},
	{"Mark the log (for a bug report: \"that just happened\")", "F5", "\u2014"},
	{"Recentre the gyro", "\u2014", "R3"},
};
static void volume_slider(void) {   /* (the bar's Game menu) */
	prefs_load();
	float pct = P.vol * 100.0f;
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
	if (ImGui::SliderFloat("##vol", &pct, 0.0f, 100.0f, "%.0f%%")) set_volume(pct / 100.0f);
}
/* render scale labels: "3x  2880 x 1632" (the 960x544 frame's size at n), and the scale that matches the
 * window: the smallest whose frame is at least the window's pixel height (Vita frames are 16:9-ish, so
 * height decides in the fit aspect), capped at the backend's maximum */
static void scale_label(char *b, size_t n, int k) { std::snprintf(b, n, "%dx   %d x %d%s", k, 960 * k, 544 * k, k == 4 ? "  (4K)" : ""); }
static int scale_for_window(void) {
	uint32_t w, h; window_pixel_size(&w, &h);
	if (!h) return 0;
	int k = (int)((h + 543) / 544); if (k < 1) k = 1; if (k > gfx_gpu_render_scale_max()) k = gfx_gpu_render_scale_max();
	return k;
}
static const char *const g_aspect_names[3] = {"Fit", "Stretch", "Fill"};

/* ---- the full menu's pages: settings rows (ui_theme.h) -- what a setting is, a line on what it does, its
 * control on the right. Each reads its live value from its owner every frame (gfx_gpu.c, window.c,
 * metrics.c, settings.c, or P above) and calls that owner's setter the moment the control changes. */
static void slider_row(const char *id, float *v, float lo, float hi, const char *fmt, ImGuiSliderFlags fl = 0) {
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::SliderFloat(id, v, lo, hi, fmt, fl);
}
static void page_display(void) {
	if (!ui::rows_begin("display")) return;
	ui::row("Resolution", "The game drawn at a multiple of the Vita's 960 × 544. Higher is sharper; 4× and up want a fast GPU.");
	{	int rs = gfx_gpu_render_scale(), mx = gfx_gpu_render_scale_max(), v = rs - 1;
		static char names[8][16]; const char *labels[8];
		for (int n = 1; n <= mx && n <= 8; n++) { std::snprintf(names[n - 1], sizeof names[0], "%d×##rs", n); labels[n - 1] = names[n - 1]; }
		if (ui::segmented(labels, mx < 8 ? mx : 8, &v)) gfx_gpu_render_scale_request(v + 1);
		char b[48]; scale_label(b, sizeof b, rs);
		ImGui::TextDisabled("%s", b + 5);
		int mw = scale_for_window();
		if (mw && mw != rs) { ImGui::SameLine(); char l[40]; std::snprintf(l, sizeof l, "Match the window (%d×)", mw); if (ImGui::TextLink(l)) gfx_gpu_render_scale_request(mw); }
	}
	ui::row("Picture fit", "How the picture fills the window.");
	{ int am = window_get_aspect(); if (ui::segmented(g_aspect_names, 3, &am)) window_set_aspect(am); }
	ui::row("Widescreen", window_aspect_supported() ? "Shows more of the game at the sides." : "This game has no widescreen support yet.");
	{	static const char *const ids[4] = {"native", "21:9", "32:9", "window"}, *const names[4] = {"Off##ar", "21:9##ar", "32:9##ar", "Window##ar"};
		char cur[16]; settings_str("aspect_ratio", "native", cur, sizeof cur);
		int v = 0; for (int i = 0; i < 4; i++) if (!strcmp(cur, ids[i])) v = i;
		ImGui::BeginDisabled(!window_aspect_supported());
		if (ui::segmented(names, 4, &v)) { settings_set_str("aspect_ratio", ids[v]); settings_save(); }
		ImGui::EndDisabled();
	}
	if (gfx_gpu_hud_supported()) {
		ui::row("HUD", "With widescreen on: the HUD at its own shape, at the screen's edges or in a 16:9 frame in the middle; or stretched to the width.");
		static const char *const ids[3] = {"edges", "16:9", "stretch"}, *const names[3] = {"Edges##hud", "16:9##hud", "Stretched##hud"};
		char cur[16]; settings_str("hud_layout", "edges", cur, sizeof cur);
		int v = 0; for (int i = 0; i < 3; i++) if (!strcmp(cur, ids[i])) v = i;
		ImGui::BeginDisabled(!window_aspect_supported() || window_aspect_widen() < 1.01f);
		if (ui::segmented(names, 3, &v)) { settings_set_str("hud_layout", ids[v]); settings_save(); }
		ImGui::EndDisabled();
	}
	if (gfx_gpu_target_scale_supported()) {
		ui::row("Full-resolution world", "The game draws its 3D world below the screen's resolution; on, at the full resolution. Sharper, at a GPU cost.");
		{ bool on = settings_int("full_res_world", 1) != 0; if (ui::toggle("frw", &on)) { settings_set_int("full_res_world", on ? 1 : 0); settings_save(); } }
	}
	if (gfx_gpu_ssao_supported()) {
		ui::row("Ambient occlusion", "Soft shadows where surfaces meet: corners, crevices, under objects.");
		{ bool on = settings_int("ssao", 1) != 0; if (ui::toggle("ssao", &on)) { settings_set_int("ssao", on ? 1 : 0); settings_save(); } }
		ui::row("Occlusion strength");
		{	float st = (float)settings_float("ssao_strength", 1.0) * 100.0f;
			ImGui::BeginDisabled(settings_int("ssao", 1) == 0);
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::SliderFloat("##ssaost", &st, 0.0f, 200.0f, "%.0f%%")) { settings_set_float("ssao_strength", st / 100.0f); settings_save(); }
			ImGui::EndDisabled();
		}
	}
	ui::row("Fullscreen");
	{ bool fs = window_get_fullscreen() != 0; if (ui::toggle("fs", &fs)) window_set_fullscreen(fs ? 1 : 0); }
	ui::row("Anti-aliasing", "Smooths jagged edges (FXAA).");
	{ bool aa = settings_int("post_aa", 1) != 0; if (ui::toggle("aa", &aa)) { settings_set_int("post_aa", aa ? 1 : 0); settings_save(); } }
	ui::row("Sharpening", "Brings back detail lost to scaling and anti-aliasing.");
	{	float sh = (float)settings_float("sharpen", 0.35) * 100.0f;
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::SliderFloat("##sharp", &sh, 0.0f, 100.0f, sh <= 0.0f ? "Off" : "%.0f%%")) { settings_set_float("sharpen", sh / 100.0f); settings_save(); }
	}
	ui::row("Texture filtering", "Keeps textures crisp when seen at an angle.");
	{	static const int an[5] = {1, 2, 4, 8, 16}; static const char *const names[5] = {"Off##af", "2×##af", "4×##af", "8×##af", "16×##af"};
		int cur = settings_int("anisotropy", 16), v = 4; for (int i = 0; i < 5; i++) if (an[i] == cur) v = i;
		if (ui::segmented(names, 5, &v)) { settings_set_int("anisotropy", an[v]); settings_save(); }
	}
	ui::row("Shadow edges", "How soft the edges of shadows are.");
	{	static const char *const names[3] = {"Original##sf", "Soft##sf", "Softer##sf"}; int v = settings_int("shadow_filter", 1);
		if (ui::segmented(names, 3, &v)) { settings_set_int("shadow_filter", v); settings_save(); }
	}
	ui::rows_end();
}
static void page_performance(void) {
	if (!ui::rows_begin("perf")) return;
	ui::row("On-screen stats", "Frame rate and timings drawn over the game. F1 cycles them.");
	{ static const char *const names[3] = {"Off##osd", "FPS##osd", "Full##osd"}; int lvl = metrics_osd_level(); if (ui::segmented(names, 3, &lvl)) set_osd(lvl); }
	ui::row("Frame rate now");
	{ char b[32]; std::snprintf(b, sizeof b, "%.0f fps", metrics_fps()); ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.6f); ImGui::TextUnformatted(b); ImGui::PopFont(); }
	ui::rows_end();
}
static void page_game(void) {
	prefs_load();
	if (!ui::rows_begin("game")) return;
	ui::row("Frame rate", "60 is experimental: parts of a game timed in fixed 1/30 s steps can run fast.");
	{ static const char *const names[2] = {"30##fr", "60##fr"}; int v = P.fr >= 60; if (ui::segmented(names, 2, &v)) set_frame_rate(v ? 60 : 30); }
	if (game_option("fov")) {
		ui::row("Field of view", "A wider view. 1.00× is the game's own.");
		ImGui::SetNextItemWidth(-FLT_MIN); if (ImGui::SliderFloat("##fov", &P.fov, 0.75f, 1.5f, "%.2f×")) set_fov(P.fov);
	}
	if (game_option("view_distance")) {
		ui::row("View distance", "How far fading scenery reaches. 1.00× is the game's own.");
		ImGui::SetNextItemWidth(-FLT_MIN); if (ImGui::SliderFloat("##view", &P.view, 1.0f, 3.0f, "%.2f×")) set_view(P.view);
	}
	if (game_option("scenery_detail")) {
		ui::row("Scenery detail", "How far small scenery details stay visible. 1.00× is the game's own; walls and buildings keep the game's own detail switching.");
		ImGui::SetNextItemWidth(-FLT_MIN); if (ImGui::SliderFloat("##scen", &P.scen, 1.0f, 8.0f, "%.1f×")) set_scen(P.scen);
	}
	if (game_option("character_detail")) {
		ui::row("Character detail", "How far characters and props keep their full detail. 1.00× is the game's own.");
		ImGui::SetNextItemWidth(-FLT_MIN); if (ImGui::SliderFloat("##chr", &P.chr, 1.0f, 8.0f, "%.1f×")) set_chr(P.chr);
	}
	if (game_option("online")) {
		ui::row("Play online", "The game's online modes with other Project Nativ players, on your network and over the internet. They see your online name and your computer's IP address, as in any peer-to-peer game. Applies the next time the game starts.");
		{ bool on = settings_int("online", 1) != 0; if (ui::toggle("online", &on)) { settings_set_int("online", on ? 1 : 0); settings_save(); } }
		if (net_online()) {   /* how it stands (status.c), every half second */
			static OnlineStatus st; static double at = -1; const double now = ImGui::GetTime();
			if (at < 0 || now - at > 0.5) { online_status(&st); at = now; }
			char a[160], b[160], c2[160];
			if (!st.started) { snprintf(a, sizeof a, "Starts when the game goes online"); b[0] = c2[0] = 0; }
			else {
				if (st.linked) snprintf(a, sizeof a, "Linked with %d other %s", st.linked, st.linked == 1 ? "copy" : "copies"); else snprintf(a, sizeof a, "Looking for other players");
				snprintf(b, sizeof b, "%d %s open%s", st.rooms_heard + st.rooms_mine, st.rooms_heard + st.rooms_mine == 1 ? "room" : "rooms", st.rooms_mine ? " (yours among them)" : "");
				if (!st.port) snprintf(c2, sizeof c2, "Your network only (online_overlay is off)");
				else if (st.pub[0]) snprintf(c2, sizeof c2, "On the internet as %s (%d DHT nodes)", st.pub, st.dht_nodes);
				else snprintf(c2, sizeof c2, st.dht_nodes ? "Reaching the internet (%d DHT nodes)" : "Reaching the internet", st.dht_nodes);
			}
			ui::row("Online now", st.clash ? "Another player in this game has your online name: you get two digits after it from the next start." : nullptr);
			ImGui::TextUnformatted(a); if (b[0]) { ImGui::PushStyleColor(ImGuiCol_Text, ui::muted); ImGui::TextUnformatted(b); ImGui::TextUnformatted(c2); ImGui::PopStyleColor(); }
		}
		static char oid[32], srv[128]; static int loaded;
		if (!loaded) { loaded = 1; settings_str("online_id", "", oid, sizeof oid); for (const char *tag : {"ntv_", "nativ_", "ativ_"}) if (!strncasecmp(oid, tag, strlen(tag))) { memmove(oid, oid + strlen(tag), strlen(oid) - strlen(tag) + 1); break; } oid[10] = 0; settings_str("private_server", "", srv, sizeof srv); }
		ui::row("Online name", "Others see ntv_ and your name: up to 10 letters, digits, '-' or '_'. Empty: a tag of your own. Applies the next time the game starts.");
		{	ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("ntv_"); ImGui::SameLine(0, 0);
			ImGui::SetNextItemWidth(-FLT_MIN); ImGui::InputText("##oid", oid, 11);
			int ok = 1, n = (int)strlen(oid);
			for (int i = 0; i < n; i++) if (!isalnum((unsigned char)oid[i]) && oid[i] != '-' && oid[i] != '_') ok = 0;
			if (ImGui::IsItemDeactivatedAfterEdit() && ok) { settings_set_str("online_id", oid); settings_save(); }
			if (!ok) { ImGui::PushStyleColor(ImGuiCol_Text, ui::muted); ImGui::TextUnformatted("Not saved: letters, digits, '-' or '_' only"); ImGui::PopStyleColor(); }
		}
		if (game_option("private_server")) {   /* (a game whose online needs its makers' servers) */
			ui::row("Private server", "The community server that stands in for the game's own (host, or host:port). Empty: the game's own, long offline.");
			ImGui::SetNextItemWidth(-FLT_MIN); ImGui::InputText("##srv", srv, sizeof srv); if (ImGui::IsItemDeactivatedAfterEdit()) { settings_set_str("private_server", srv); settings_save(); }
		}
	}
	ui::row("Volume");
	{ float pct = P.vol * 100.0f; ImGui::SetNextItemWidth(-FLT_MIN); if (ImGui::SliderFloat("##vol", &pct, 0.0f, 100.0f, "%.0f%%")) set_volume(pct / 100.0f); }
	ui::rows_end();
}
extern "C" int camera_light(void); extern "C" void camera_light_set(int on);
static void section_label(const char *s) { ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.4f)); ImGui::PushStyleColor(ImGuiCol_Text, ui::accent); ImGui::TextUnformatted(s); ImGui::PopStyleColor(); }
static void reference_table(const char *id, int (*count)(void), void (*row)(int, const char **, const char **, const char **)) {
	if (!ImGui::BeginTable(id, 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
	ImGui::TableSetupColumn("Action"); ImGui::TableSetupColumn("Keyboard and mouse"); ImGui::TableSetupColumn("Controller");
	ImGui::PushStyleColor(ImGuiCol_Text, ui::muted); ImGui::TableHeadersRow(); ImGui::PopStyleColor();
	for (int i = 0, n = count(); i < n; i++) {
		const char *a, *k, *p; row(i, &a, &k, &p);
		ImGui::TableNextRow();
		ImGui::TableNextColumn(); ImGui::TextWrapped("%s", a);
		ImGui::TableNextColumn(); ImGui::TextWrapped("%s", k);
		ImGui::TableNextColumn(); ImGui::TextWrapped("%s", p);
	}
	ImGui::EndTable();
}
static int shortcuts_count(void) { return (int)(sizeof g_shortcuts / sizeof g_shortcuts[0]); }
static void shortcuts_row(int i, const char **a, const char **k, const char **p) { *a = g_shortcuts[i][0]; *k = g_shortcuts[i][1]; *p = g_shortcuts[i][2]; }
static void page_controls(void) {
	prefs_load();
	section_label("Input");
	if (ui::rows_begin("input")) {
		ui::row("Play with", window_kbm_active() ? "Keyboard and mouse in use. Hold Alt (Option) to free the cursor -- it is then the touch screen."
		                                          : "Controller in use. Automatic follows the last device you touch.");
		{	static const char *const ids[3] = {"auto", "pad", "kbm"}, *const names[3] = {"Automatic##in", "Controller##in", "Keyboard & mouse##in"};
			char cur[8]; settings_str("input_mode", "auto", cur, sizeof cur);
			int v = 0; for (int i = 0; i < 3; i++) if (!strcmp(cur, ids[i])) v = i;
			if (ui::segmented(names, 3, &v)) { settings_set_str("input_mode", ids[v]); settings_save(); }
		}
		ui::row("Mouse look", "The mouse turns the camera and aims; left button R, right button L.");
		{ bool look = settings_int("mouse_look", 1) != 0; if (ui::toggle("ml", &look)) { settings_set_int("mouse_look", look ? 1 : 0); settings_save(); } }
		ui::row("Mouse aim", "Direct: the game turns exactly as far as the mouse moves (games with direct aim; the others use Stick by themselves). Stick: the mouse moves the right stick. F8 switches.");
		{ static const char *const names[2] = {"Direct##ma", "Stick##ma"}; int v = window_raw_look() ? 0 : 1; if (ui::segmented(names, 2, &v)) window_set_raw_look(v == 0); }
		ui::row("Mouse sensitivity");
		{ static float sens = -1; if (sens < 0) sens = (float)settings_float("mouse_sens", 1.0); ImGui::SetNextItemWidth(-FLT_MIN);
		  if (ImGui::SliderFloat("##ms", &sens, 0.1f, 5.0f, "%.2f×", ImGuiSliderFlags_Logarithmic)) { settings_set_float("mouse_sens", sens); settings_save(); } }
		ui::row("Invert vertical look");
		{ bool inv = settings_int("mouse_invert", 0) != 0; if (ui::toggle("mi", &inv)) { settings_set_int("mouse_invert", inv ? 1 : 0); settings_save(); } }
		ui::row("Touch cursor speed", "Hold a bumper and move a stick: the cursor is a finger on the touch screen (Cross, L2 or R2 presses). Move both sticks for two fingers.");
		ImGui::SetNextItemWidth(-FLT_MIN); if (ImGui::SliderFloat("##tcs", &P.tcs, 0.25f, 3.0f, "%.2f×")) set_tcs(P.tcs);
		ui::rows_end();
	}
	section_label("Motion");
	if (ui::rows_begin("motion")) {
		ui::row("Gyro", window_gyro_present() ? "This controller has one: it stands in for the Vita's motion sensor." : "This controller has no gyro. Tilt sections use the keys or the left stick.");
		{ bool on = P.gyro != 0; if (ui::toggle("gy", &on)) set_gyro(on); }
		ui::row("A level controller is a Vita held", "Upright, as in your hands, or flat on a table.");
		{ static const char *const names[2] = {"Upright##gp", "Flat##gp"}; int v = P.gyro_flat ? 1 : 0; if (ui::segmented(names, 2, &v)) set_gyro_pose(v); }
		ui::row("Gyro sensitivity");
		ImGui::SetNextItemWidth(-FLT_MIN); if (ImGui::SliderFloat("##gs", &P.gyro_sens, 0.25f, 3.0f, "%.2f×")) set_gyro_sens(P.gyro_sens);
		ui::row("Recentre", "Makes the way the controller points now the centre. R3 does the same.");
		if (ImGui::Button("Recentre the gyro")) motion_recenter();
		ui::row("Rear camera", "What the Vita's camera sees -- Uncharted's parchment puzzles want a light. F7 switches the lamp.");
		{	static const char *const ids[3] = {"light", "webcam", "dark"}, *const names[3] = {"A lamp##cam", "This computer's camera##cam", "Darkness##cam"};
			char cur[16]; settings_str("camera", "light", cur, sizeof cur);
			int v = 0; for (int i = 0; i < 3; i++) if (!strcmp(cur, ids[i])) v = i;
			if (ui::segmented(names, 3, &v)) { settings_set_str("camera", ids[v]); settings_save(); }
			if (v == 0) { bool lit = camera_light() != 0; ImGui::SameLine(); ImGui::TextDisabled("lamp"); ImGui::SameLine(); if (ui::toggle("lamp", &lit)) camera_light_set(lit ? 1 : 0); }
		}
		ui::rows_end();
	}
	section_label("Keys");
	if (ImGui::BeginTable("kbbinds", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Action"); ImGui::TableSetupColumn("Key"); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
		ImGui::PushStyleColor(ImGuiCol_Text, ui::muted); ImGui::TableHeadersRow(); ImGui::PopStyleColor();
		for (int i = 0, n = window_kb_count(); i < n; i++) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(window_kb_label(i));
			ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
			const char *k = window_kb_key_name(i);
			if (g_rebind == i) ImGui::TextColored(ui::accent, "Press a key…"); else ui::chip(k && *k ? k : "none", ui::text);
			ImGui::TableNextColumn(); ImGui::PushID(i); if (ImGui::Button("Change")) g_rebind = i; ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (ImGui::Button("Reset keys to the defaults")) { window_kb_reset(); g_rebind = -1; }
	section_label("The game's controls");
	reference_table("controls", window_controls_count, window_controls_row);
	section_label("Shortcuts");
	reference_table("shortcuts", shortcuts_count, shortcuts_row);
}
static void page_help(void) {
	section_label("Shortcuts");
	reference_table("shortcuts", shortcuts_count, shortcuts_row);
	section_label("About");
	{	ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.2f));
		ImVec2 p = ImGui::GetCursorScreenPos(); const float h = ImGui::GetFontSize() * 1.5f;
		ImGui::Dummy(ImVec2(ui::wordmark(ImGui::GetWindowDrawList(), p, h), h));
	}
	ImGui::TextWrapped("Project Nativ: this PS Vita game, statically recompiled to native code, running on this computer's own CPU and GPU.");
	const char *title = std::getenv("RECOMP_TITLE");
	int v = SDL_GetVersion();
	ImGui::TextDisabled("%s  ·  Dear ImGui %s  ·  SDL %d.%d.%d", title && *title ? title : "unknown title", IMGUI_VERSION, SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
	ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.3f));
	ui::muted_wrapped("The menu bar: move the mouse to the top edge of the window (or tap the tab there). This menu: F10, or the PS button; the game pauses while it's up.");
}

/* ---- the full menu (build_ui): a panel over the paused, dimmed game -- the game's name and a Paused chip, the
 * sections down the side, the page, and Resume / Capture / Quit at the foot */
enum { TAB_DISPLAY, TAB_PERF, TAB_GAME, TAB_CONTROLS, TAB_HELP, TAB_N };
static const char *const g_tab_names[TAB_N] = {"Display", "Performance", "Game", "Controls", "Help"};
static const char *const g_tab_blurbs[TAB_N] = {"Resolution, picture and image quality", "Frame rate and on-screen stats",
	"Frame rate, view and detail options, volume", "Keyboard, mouse, controller, gyro and camera", "Shortcuts and about"};
static int g_select_tab = -1;   /* the bar asked for this page (open_page): selected on the menu's next frame, then -1 */
static int g_section = TAB_DISPLAY;
static void open_page(int tab) { g_select_tab = tab; overlay_open(); }
static void build_ui(void) {
	ImGuiIO &io = ImGui::GetIO();
	const float em = ImGui::GetFontSize();
	if (g_select_tab >= 0) { g_section = g_select_tab; g_select_tab = -1; }
	ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, ui::u32(ui::bg, 0.62f));   /* the paused game, dimmed behind */
	const float W = io.DisplaySize.x - 2 * em < 58 * em ? io.DisplaySize.x - 2 * em : 58 * em;
	const float H = io.DisplaySize.y - 2 * em < 38 * em ? io.DisplaySize.y - 2 * em : 38 * em;
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_Always);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::surface);
	ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
	ImGui::PopStyleColor();
	/* header: the game, Paused, Resume */
	{	const char *name = std::getenv("RECOMP_GAME_NAME"); if (!name || !*name) name = std::getenv("RECOMP_TITLE"); if (!name || !*name) name = "Project Nativ";
		ui::heading(name, em * 1.35f);
		ImGui::SameLine(0, em * 0.7f); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + em * 0.18f); ui::chip("Paused", ui::accent);
		float rw = ImGui::CalcTextSize("Resume").x + ImGui::GetStyle().FramePadding.x * 2;
		ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - rw);
		if (ui::primary_button("Resume")) overlay_close();
	}
	ImGui::Dummy(ImVec2(0, em * 0.2f));
	const float foot = ImGui::GetFrameHeightWithSpacing() + em * 0.9f;
	/* the sections; a controller's L1 / R1 (or Ctrl+Tab, Ctrl+Shift+Tab) step them in the sidebar's order -- the sidebar and
	 * the page are separate panels the D-pad doesn't cross -- and the new page's first row takes the focus */
	static const int order[TAB_N] = {TAB_DISPLAY, TAB_GAME, TAB_CONTROLS, TAB_PERF, TAB_HELP};
	static bool focus_page;
	if (!ImGui::IsAnyItemActive()) {
		const bool ctrl = io.KeyCtrl, shift = io.KeyShift; int step = 0;
		if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || (ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_Tab, false))) step = -1;
		if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_Tab, false))) step = 1;
		if (step) { int k = 0; while (k < TAB_N - 1 && order[k] != g_section) k++; g_section = order[(k + step + TAB_N) % TAB_N]; focus_page = true; }
	}
	ImGui::BeginChild("nav", ImVec2(em * 11.5f, -foot), 0);
	for (int k = 0; k < TAB_N; k++) {
		int i = order[k]; bool on = g_section == i;
		ImVec2 p = ImGui::GetCursorScreenPos(); float h = ImGui::GetFrameHeight() * 1.15f, w = ImGui::GetContentRegionAvail().x;
		ImGui::PushID(i);
		if (ImGui::InvisibleButton("sec", ImVec2(w, h))) g_section = i;
		bool hov = ImGui::IsItemHovered() || ImGui::IsItemFocused();
		ImDrawList *dl = ImGui::GetWindowDrawList();
		if (on || hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ui::u32(on ? ui::card_hi : ui::card), ImGui::GetStyle().FrameRounding);
		if (on) dl->AddRectFilled(ImVec2(p.x, p.y + h * 0.22f), ImVec2(p.x + 3, p.y + h * 0.78f), ui::u32(ui::accent), 2);
		dl->AddText(ImVec2(p.x + em * 0.9f, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), ui::u32(on ? ui::text : ui::muted), g_tab_names[i]);
		ImGui::PopID();
	}
	ImGui::EndChild();
	ImGui::SameLine(0, em * 0.8f);
	/* the page */
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::bg);
	ImGui::BeginChild("page", ImVec2(0, -foot), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
	ImGui::PopStyleColor();
	ui::heading(g_tab_names[g_section], em * 1.25f);
	ImGui::TextDisabled("%s", g_tab_blurbs[g_section]);
	if (focus_page) { ImGui::SetKeyboardFocusHere(); focus_page = false; }   /* (the page's first control) */
	ImGui::Dummy(ImVec2(0, em * 0.2f));
	static void (*const pages[TAB_N])(void) = {page_display, page_performance, page_game, page_controls, page_help};
	pages[g_section]();
	ImGui::EndChild();
	/* the foot */
	ImGui::Dummy(ImVec2(0, em * 0.15f));
	if (ImGui::Button("Capture frame (F12)")) g_want_capture = true;
	ImGui::SameLine(); ImGui::TextDisabled(SDL_HasGamepad() ? "L1 / R1: sections   Esc, F10 or the PS button closes this menu" : "Esc, F10 or the PS button closes this menu");
	const char *quit = quit_label();
	float qw = ImGui::CalcTextSize(quit).x + ImGui::GetStyle().FramePadding.x * 2;
	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - qw);
	ImGui::PushStyleColor(ImGuiCol_Text, ui::error);
	if (ImGui::Button(quit)) g_want_quit = true;
	ImGui::PopStyleColor();
	ImGui::End();
}

/* ---- the menu bar and everything else the closed state shows (build_closed). All of it runs on the
 * window thread (window.c's present loop), like the full menu. Times are ImGui's own clock (GetTime:
 * io.DeltaTime summed), so the headless path's fixed 1/60 s steps work the same way. */
enum {
	HOT_PX = 4,          /* the top-edge zone: the pointer resting within this many points of the edge opens the bar */
	TAB_NEAR_PX = 48,    /* the pull tab shows while the pointer is within this of the top */
};
static const float HOT_DWELL_S = 0.18f, HIDE_S = 1.0f, SLIDE_S = 0.16f, PIN_S = 4.0f, TOAST_S = 4.5f, CURSOR_HIDE_S = 3.0f;
static float g_bar_vis;                 /* 0 hidden (parked above the window's top edge) .. 1 fully down */
static bool g_bar_want;                 /* where it's heading */
static double g_hot_since = -1;         /* when the pointer entered the top-edge zone (-1: it isn't there) */
static bool g_hot_block;                /* Escape hid the bar: the pointer must leave the zone before it can re-open it */
static double g_bar_active_t = -1e9;    /* the last moment the pointer was over the bar, a menu was open, or the tab pinned it */
static double g_bar_pin_until = -1e9;   /* the pull tab was tapped: the bar stays at least until then */
static float g_tab_a;                   /* the pull tab's alpha */
static double g_toast_until = -1e9; static bool g_toast_checked;   /* the first-launch menu hint's end (the pull tab shows with it) */
/* the toasts: badges at the top right, newest on top (overlay_toast_titled: posted by any thread, taken in by the next frame) */
struct Toast { char title[48]; char body[224]; double start, until; };
static Toast g_toasts[3]; static int g_ntoasts;
static Toast g_toast_q[8]; static int g_toast_qn; static std::mutex g_toast_mx;
static double g_idle_since; static ImVec2 g_last_mouse(-1, -1);
static bool g_dismiss_req;              /* overlay_bar_dismiss (Escape in window.c): close the menus, hide the bar */
static bool g_mouse_captured;           /* io.WantCaptureMouse as of this thread's last ImGui frame (overlay_mouse_captured) */
static const char *g_force_menu; static bool g_force_bar, g_force_toast;   /* RECOMP_OVERLAY_DUMP_UI (overlay_render_forced) */
static bool g_touch_cursor;             /* overlay_touch_cursor: the pointer is a touch mark this frame -- no arrow */
static bool g_skip_draw;                /* overlay_render_forced's warm-up frames: built (ImGui sizes its windows) but not drawn */
static float g_bar_h;                   /* the bar's height this frame, in points (0: none drawn) -- the HUD moves down by it */

void overlay_bar_dismiss(void) { if (g_ctx) g_dismiss_req = true; }
int overlay_mouse_captured(void) { return g_ctx && g_has_window && g_mouse_captured; }

static void menu_game(void) {
	if (ImGui::MenuItem(g_paused ? "Resume" : "Pause")) overlay_set_paused(!g_paused);
	ImGui::TextUnformatted("Volume"); ImGui::SameLine(); volume_slider();
	if (ImGui::MenuItem("Capture frame", "F12")) g_want_capture = true;
	if (ImGui::MenuItem("Full menu…", "F10")) overlay_open();
	ImGui::Separator();
	if (ImGui::MenuItem(quit_label())) g_want_quit = true;
}
static void menu_view(void) {
	if (ImGui::BeginMenu("Resolution")) {
		int rs = gfx_gpu_render_scale();
		for (int n = 1; n <= gfx_gpu_render_scale_max(); n++) { char l[48]; scale_label(l, sizeof l, n); if (ImGui::MenuItem(l, nullptr, rs == n)) gfx_gpu_render_scale_request(n); }
		ImGui::Separator();
		int mw = scale_for_window();
		if (mw) { char l[48]; std::snprintf(l, sizeof l, "Match window (%dx)", mw); if (ImGui::MenuItem(l)) gfx_gpu_render_scale_request(mw); }
		if (ImGui::MenuItem("Cycle", "F2")) gfx_gpu_render_scale_cycle();
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Picture fit")) {
		int am = window_get_aspect();
		for (int i = 0; i < 3; i++) if (ImGui::MenuItem(g_aspect_names[i], nullptr, am == i)) window_set_aspect(i);
		ImGui::EndMenu();
	}
	bool fs = window_get_fullscreen() != 0;
	if (ImGui::MenuItem("Fullscreen", nullptr, &fs)) window_set_fullscreen(fs ? 1 : 0);
	ImGui::Separator();
	prefs_load();
	if (game_option("fov")) { ImGui::TextDisabled("Field of view"); ImGui::SetNextItemWidth(170); if (ImGui::SliderFloat("##fov", &P.fov, 0.75f, 1.5f, "%.2fx")) set_fov(P.fov); }
	if (game_option("view_distance")) { ImGui::TextDisabled("View distance"); ImGui::SetNextItemWidth(170); if (ImGui::SliderFloat("##view", &P.view, 1.0f, 3.0f, "%.2fx")) set_view(P.view); }
	if (game_option("scenery_detail")) { ImGui::TextDisabled("Scenery detail"); ImGui::SetNextItemWidth(170); if (ImGui::SliderFloat("##scen", &P.scen, 1.0f, 8.0f, "%.1fx")) set_scen(P.scen); }
	if (game_option("character_detail")) { ImGui::TextDisabled("Character detail"); ImGui::SetNextItemWidth(170); if (ImGui::SliderFloat("##chr", &P.chr, 1.0f, 8.0f, "%.1fx")) set_chr(P.chr); }
	if (ImGui::BeginMenu("Frame rate")) {
		if (ImGui::MenuItem("30", nullptr, P.fr < 60)) set_frame_rate(30);
		if (ImGui::MenuItem("60 (experimental)", nullptr, P.fr >= 60)) set_frame_rate(60);
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("On-screen stats")) {
		static const char *const names[3] = {"Off", "FPS", "Full"};
		int lvl = metrics_osd_level();
		for (int i = 0; i < 3; i++) if (ImGui::MenuItem(names[i], nullptr, lvl == i)) set_osd(i);
		ImGui::Separator();
		if (ImGui::MenuItem("Cycle", "F1")) metrics_osd_cycle();
		ImGui::EndMenu();
	}
}
static void menu_controls(void) {
	prefs_load();
	ImGui::TextDisabled("Touch cursor speed (hold L1/R1, either stick)");
	ImGui::SetNextItemWidth(170);
	if (ImGui::SliderFloat("##tcs", &P.tcs, 0.25f, 3.0f, "%.2fx")) set_tcs(P.tcs);
	ImGui::Separator();
	bool gy = P.gyro != 0;
	if (ImGui::MenuItem("Gyro as the Vita's motion sensor", nullptr, &gy)) set_gyro(gy);
	if (ImGui::BeginMenu("A level pad is a Vita held")) {
		if (ImGui::MenuItem("upright", nullptr, !P.gyro_flat)) set_gyro_pose(0);
		if (ImGui::MenuItem("flat", nullptr, P.gyro_flat != 0)) set_gyro_pose(1);
		ImGui::EndMenu();
	}
	ImGui::TextDisabled("Gyro sensitivity");
	ImGui::SetNextItemWidth(170);
	if (ImGui::SliderFloat("##gs", &P.gyro_sens, 0.25f, 3.0f, "%.2fx")) set_gyro_sens(P.gyro_sens);
	if (ImGui::MenuItem("Recentre the gyro", "R3")) motion_recenter();
	ImGui::Separator();
	if (ImGui::MenuItem("Controls reference…")) open_page(TAB_CONTROLS);   /* (the full menu: pauses) */
}
static void menu_help(void) {
	if (ImGui::MenuItem("Shortcuts…")) open_page(TAB_HELP);
	if (ImGui::MenuItem("About…")) open_page(TAB_HELP);
}
/* the bar: a window the width of the display, exactly one menu-bar high, parked above the top edge by
 * (1 - vis) of its height -- the slide. Its menus are ImGui popups below it. */
static void build_bar(float w) {
	const float h = ImGui::GetFrameHeight();
	g_bar_h = h;
	const float e = g_bar_vis * g_bar_vis * (3.0f - 2.0f * g_bar_vis);   /* smoothstep: eases in and out */
	ImGui::SetNextWindowPos(ImVec2(0, -(1.0f - e) * (h + 1)));
	ImGui::SetNextWindowSize(ImVec2(w, h));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	const ImVec4 bg = ui::with_alpha(ui::surface, 0.96f);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
	ImGui::PushStyleColor(ImGuiCol_MenuBarBg, bg);
	const ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
	const bool open = ImGui::Begin("##menubar", nullptr, fl);
	ImGui::PopStyleVar(4);   /* (the bar window's own: the menus it opens get the theme's padding, a little tighter) */
	if (open && ImGui::BeginMenuBar()) {
		const float em = ImGui::GetFontSize();
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 0.7f, em * 0.55f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(em * 0.6f, em * 0.45f));
		/* (headless dump: a menu forced open -- OpenPopup here shares BeginMenu's ID scope, so the ids match) */
		if (g_force_menu) { ImGui::OpenPopup(g_force_menu); g_force_menu = nullptr; }
		if (ImGui::BeginMenu("Game")) { menu_game(); ImGui::EndMenu(); }
		if (ImGui::BeginMenu("View")) { menu_view(); ImGui::EndMenu(); }
		if (ImGui::BeginMenu("Controls")) { menu_controls(); ImGui::EndMenu(); }
		if (ImGui::BeginMenu("Help")) { menu_help(); ImGui::EndMenu(); }
		ImGui::PopStyleVar(2);
		ImGui::EndMenuBar();
	}
	ImGui::End();
	ImGui::PopStyleColor(2);
}
/* the pull tab: a small rounded tongue at the top centre with a chevron, in a window of its own size so
 * it's hoverable and tappable (touch has no top-edge hover); a tap brings the bar down and pins it a while */
static void build_tab(float w, double t) {
	const float tw = 72, th = 14;
	ImGui::SetNextWindowPos(ImVec2(w * 0.5f - tw * 0.5f, 0));
	ImGui::SetNextWindowSize(ImVec2(tw, th));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
		ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse;
	if (ImGui::Begin("##pulltab", nullptr, fl)) {
		if (ImGui::InvisibleButton("tab", ImVec2(tw, th))) { g_bar_want = true; g_bar_pin_until = t + PIN_S; g_bar_active_t = t; }
		const bool hov = ImGui::IsItemHovered();
		/* the window is the hit area; the picture goes on the foreground list, over anything else (the HUD
		 * panel sits at the top-left on the background list). NB ImGui 1.92.8+ takes AddRect(..., rounding,
		 * THICKNESS, flags): flags passed in the thickness slot draw a 192-px-thick outline (a star burst). */
		ImDrawList *dl = ImGui::GetForegroundDrawList();
		const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
		dl->AddRectFilled(p0, p1, IM_COL32(30, 30, 36, (int)(g_tab_a * (hov ? 240 : 190))), 6.0f, ImDrawFlags_RoundCornersBottom);
		dl->AddRect(p0, p1, IM_COL32(255, 255, 255, (int)(g_tab_a * 60)), 6.0f, 1.0f, ImDrawFlags_RoundCornersBottom);
		const ImU32 c = IM_COL32(230, 230, 240, (int)(g_tab_a * 235));
		const float cx = (p0.x + p1.x) * 0.5f, cy = p0.y + th * 0.3f;
		dl->AddLine(ImVec2(cx - 6, cy), ImVec2(cx, cy + 5), c, 2.0f);
		dl->AddLine(ImVec2(cx, cy + 5), ImVec2(cx + 6, cy), c, 2.0f);
	}
	ImGui::End();
	ImGui::PopStyleVar(3);
}
extern "C" void overlay_touch_cursor(int on) { g_touch_cursor = on != 0; }
extern "C" void overlay_toast_titled(const char *title, const char *body, double seconds) {
	std::lock_guard<std::mutex> l(g_toast_mx);
	if (g_toast_qn == 8) { std::memmove(g_toast_q, g_toast_q + 1, sizeof g_toast_q[0] * 7); g_toast_qn--; }   /* (a burst: the oldest unseen goes) */
	Toast &q = g_toast_q[g_toast_qn++];
	std::snprintf(q.title, sizeof q.title, "%s", title ? title : ""); std::snprintf(q.body, sizeof q.body, "%s", body ? body : "");
	q.start = 0; q.until = seconds;   /* (a duration until taken in) */
}
extern "C" void overlay_toast(const char *msg, double seconds) { overlay_toast_titled(nullptr, msg, seconds); }
/* the posted toasts into the stack: one already showing (same title and text) runs on instead of stacking twice;
 * three at most, the oldest dropped; each gone when its time is up */
static void toasts_take(double t) {
	{
		std::lock_guard<std::mutex> l(g_toast_mx);
		for (int i = 0; i < g_toast_qn; i++) {
			Toast q = g_toast_q[i]; double dur = q.until;
			int k = 0; while (k < g_ntoasts && (std::strcmp(g_toasts[k].title, q.title) || std::strcmp(g_toasts[k].body, q.body))) k++;
			if (k < g_ntoasts) { g_toasts[k].until = t + dur; continue; }
			if (g_ntoasts == 3) g_ntoasts--;
			std::memmove(g_toasts + 1, g_toasts, sizeof g_toasts[0] * g_ntoasts); g_ntoasts++;
			q.start = t; q.until = t + dur; g_toasts[0] = q;
		}
		g_toast_qn = 0;
	}
	int n = 0; for (int i = 0; i < g_ntoasts; i++) if (t < g_toasts[i].until) g_toasts[n++] = g_toasts[i];
	g_ntoasts = n;
}
/* A toast as a badge at the top right (below the menu bar when it's down): a dark card with an amber edge, the
 * title in amber, the text under it, wrapped to a third of the window; "[F10]" in the text draws as a key cap.
 * It slides in, and fades over its last 0.4 s. */
struct ToastTok { const char *s; int n; bool key, brk; float w; };
static int toast_tokens(const char *body, ImFont *font, float fs, float cap_pad, ToastTok *out, int max) {
	int n = 0;
	for (const char *p = body; *p && n < max; ) {
		if (*p == '\n') { out[n++] = {p, 0, false, true, 0}; p++; continue; }
		if (*p == ' ') { p++; continue; }
		const char *e = p; bool key = *p == '[' && std::strchr(p, ']');
		if (key) { e = std::strchr(p, ']') + 1; out[n] = {p + 1, (int)(e - p - 2), true, false, 0}; }
		else { while (*e && *e != ' ' && *e != '\n' && !(*e == '[' && e > p)) e++; out[n] = {p, (int)(e - p), false, false, 0}; }
		out[n].w = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, out[n].s, out[n].s + out[n].n).x + (key ? 2 * cap_pad : 0);
		n++; p = e;
	}
	return n;
}
static void build_toasts(float w, float hgt, double t) {
	(void)hgt;
	ImFont *font = ImGui::GetFont(); const float fs = ImGui::GetFontSize() * (w >= 1000 ? 2.0f : 1.0f);   /* (the bitmap font: integer multiples stay crisp) */
	const float pad = fs * 0.75f, edge = fs * 0.3f, cap_pad = fs * 0.3f, space = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, " ").x, lh = fs * 1.35f;
	const float maxw = fmaxf(w / 3.0f, fs * 18.0f);
	float y = fs * 0.75f + g_bar_h * g_bar_vis;
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	for (int i = 0; i < g_ntoasts; i++) {
		const Toast &q = g_toasts[i];
		const float in = g_has_window ? (float)fmin(1.0, (t - q.start) / 0.15) : 1.0f, a = (float)fmin(1.0, (q.until - t) / 0.4) * in;   /* (a headless dump: as it settles) */
		ToastTok tok[64]; const int nt = toast_tokens(q.body, font, fs, cap_pad, tok, 64);
		/* the lines: tokens laid left to right, a new line at a "\n" or where the next would pass maxw */
		float lx = 0, widest = q.title[0] ? font->CalcTextSizeA(fs, FLT_MAX, 0.0f, q.title).x : 0; int lines = nt ? 1 : 0;
		float tx[64], ty_line[64];
		for (int k = 0; k < nt; k++) {
			if (tok[k].brk) { lines++; lx = 0; tx[k] = 0; ty_line[k] = (float)(lines - 1); continue; }
			if (lx > 0 && lx + space + tok[k].w > maxw) { lines++; lx = 0; }
			if (lx > 0) lx += space;
			tx[k] = lx; ty_line[k] = (float)(lines - 1); lx += tok[k].w; if (lx > widest) widest = lx;
		}
		const float cw = widest + 2 * pad + edge, ch = (q.title[0] ? lh : 0) + lines * lh + 2 * pad - (lh - fs);
		const float x0 = w - fs * 0.75f - cw + (1 - in) * fs * 1.5f;
		const ImVec2 p0(x0, y), p1(x0 + cw, y + ch);
		dl->AddRectFilled(p0, p1, IM_COL32(18, 20, 26, (int)(a * 235)), fs * 0.4f);
		dl->AddRectFilled(p0, ImVec2(p0.x + edge, p1.y), IM_COL32(255, 186, 72, (int)(a * 255)), fs * 0.4f, ImDrawFlags_RoundCornersLeft);
		dl->AddRect(p0, p1, IM_COL32(255, 255, 255, (int)(a * 40)), fs * 0.4f);
		float cy = y + pad, cx = x0 + edge + pad;
		if (q.title[0]) { dl->AddText(font, fs, ImVec2(cx, cy), IM_COL32(255, 196, 92, (int)(a * 255)), q.title); cy += lh; }
		for (int k = 0; k < nt; k++) {
			if (tok[k].brk) continue;
			const ImVec2 at(cx + tx[k], cy + ty_line[k] * lh);
			if (tok[k].key) {   /* a key cap */
				dl->AddRectFilled(ImVec2(at.x, at.y - fs * 0.12f), ImVec2(at.x + tok[k].w, at.y + fs * 1.12f), IM_COL32(58, 62, 74, (int)(a * 255)), fs * 0.2f);
				dl->AddRect(ImVec2(at.x, at.y - fs * 0.12f), ImVec2(at.x + tok[k].w, at.y + fs * 1.12f), IM_COL32(150, 156, 172, (int)(a * 255)), fs * 0.2f);
				dl->AddText(font, fs, ImVec2(at.x + cap_pad, at.y), IM_COL32(255, 255, 255, (int)(a * 255)), tok[k].s, tok[k].s + tok[k].n);
			} else dl->AddText(font, fs, at, IM_COL32(232, 234, 240, (int)(a * 255)), tok[k].s, tok[k].s + tok[k].n);
		}
		y = p1.y + fs * 0.5f;
	}
}
/* Game > Pause (not the menu): the picture stays, with this over it */
static void build_pause_banner(float w, float hgt) {
	ImFont *font = ImGui::GetFont(); const float fs = ImGui::GetFontSize() * 2.0f;
	static const char l1[] = "PAUSED", l2[] = "Game > Resume in the menu bar (mouse to the top), or F10";
	const ImVec2 s1 = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, l1), s2 = font->CalcTextSizeA(fs * 0.5f, FLT_MAX, 0.0f, l2);
	const float pad = 12, bw = fmaxf(s1.x, s2.x) + 2 * pad, bh = s1.y + s2.y + 3 * pad;
	const ImVec2 p0(w * 0.5f - bw * 0.5f, hgt * 0.5f - bh * 0.5f), p1(p0.x + bw, p0.y + bh);
	ImDrawList *dl = ImGui::GetBackgroundDrawList();   /* (under the bar's menus, like the HUD) */
	dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 170), 8.0f);
	dl->AddText(font, fs, ImVec2(w * 0.5f - s1.x * 0.5f, p0.y + pad), IM_COL32(255, 255, 255, 255), l1);
	dl->AddText(font, fs * 0.5f, ImVec2(w * 0.5f - s2.x * 0.5f, p0.y + 2 * pad + s1.y), IM_COL32(205, 205, 215, 255), l2);
}
/* the HUD while the menu is closed: the touch markers (window.c's gamepad touch) and the performance overlay
 * (metrics.c's F1 lines) -- no window, no input, the foreground draw list only */
static const OverlayMark *g_marks; static int g_nmarks;
static float g_rear[4];   /* the rear panel's rows on the picture, in window points (x, y, w, h); w 0: none */
static char g_osd[6][40]; static int g_nosd;   /* the overlay's lines this frame (metrics_osd_lines) */
int overlay_draws_hud(void) { return g_ctx && g_has_window; }
static void build_marks(void) {
	ImDrawList *dl = ImGui::GetBackgroundDrawList();   /* over the game's frame (LOAD), under the bar and its menus */
	if (g_nosd) {   /* the overlay: a translucent dark panel at the top-left corner with the lines, as the bitmap version drew it (metrics.c); below the bar while that's down */
		ImFont *font = ImGui::GetFont(); float fs = ImGui::GetFontSize() * 2.0f;   /* the embedded bitmap font at an integer multiple: crisp */
		const float x = 8, y = 8 + g_bar_h * g_bar_vis, pad = 6, lh = fs + 2; float wmax = 0;
		for (int i = 0; i < g_nosd; i++) { ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, g_osd[i]); if (sz.x > wmax) wmax = sz.x; }
		dl->AddRectFilled(ImVec2(x, y), ImVec2(x + wmax + 2 * pad, y + g_nosd * lh + 2 * pad), IM_COL32(0, 0, 0, 170));
		for (int i = 0; i < g_nosd; i++) dl->AddText(font, fs, ImVec2(x + pad, y + pad + i * lh), IM_COL32(255, 255, 255, 255), g_osd[i]);
	}
	if (g_rear[2] > 0) dl->AddRect(ImVec2(g_rear[0], g_rear[1]), ImVec2(g_rear[0] + g_rear[2], g_rear[1] + g_rear[3]), IM_COL32(255, 170, 60, 140), 0.0f, 2.0f);   /* (rounding 0, thickness 2: ImGui 1.92.8+ argument order -- the old (0, 0, 2.0f) put 2 in `flags`, an ImGui user-error assert) */
	for (int i = 0; i < g_nmarks; i++) {
		const OverlayMark &m = g_marks[i];
		ImU32 col = m.rear ? IM_COL32(255, 170, 60, 230) : IM_COL32(80, 200, 255, 230);   /* rear orange, front blue */
		float r = m.cursor ? 14.f : 10.f;
		if (m.pressed) dl->AddCircleFilled(ImVec2(m.x, m.y), r, (col & 0x00ffffff) | IM_COL32(0, 0, 0, 120));
		dl->AddCircle(ImVec2(m.x, m.y), r, col, 0, 2.5f);
		if (m.cursor) { dl->AddLine(ImVec2(m.x - r - 4, m.y), ImVec2(m.x + r + 4, m.y), col, 1.5f); dl->AddLine(ImVec2(m.x, m.y - r - 4), ImVec2(m.x, m.y + r + 4), col, 1.5f); }
	}
}
/* the save-data dialog (apputil.c through savedlg.h): the game's save / load / delete list, a yes/no question,
 * or a card the game closes itself (Saving..., a progress bar) -- over the picture while the game waits on it,
 * the game running underneath (it polls the dialog every frame; nothing pauses). The pad: up/down (left/right
 * on a question), Cross to choose, Circle to go back; the keyboard: arrows, Enter or Space, Escape or
 * Backspace; the mouse: hover and click. window.c hands this the keys and the pad while overlay_dialog_active
 * (ImGui's SDL backend polls the pad itself, so its buttons arrive whatever the routing). */
static int g_sd_seq = -1, g_sd_sel; static bool g_sd_armed;
int overlay_dialog_active(void) {
	if (!g_ctx || !g_has_window) return 0;
	SaveDlgView v; const int k = savedlg_view(&v); savedlg_release();
	return k == SAVEDLG_LIST || k == SAVEDLG_YESNO;
}
static const ImGuiKey K_UP[] = {ImGuiKey_UpArrow, ImGuiKey_W, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp};
static const ImGuiKey K_DOWN[] = {ImGuiKey_DownArrow, ImGuiKey_S, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown};
static const ImGuiKey K_SIDE[] = {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_A, ImGuiKey_D, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight};
static const ImGuiKey K_OK[] = {ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown};
static const ImGuiKey K_BACK[] = {ImGuiKey_Escape, ImGuiKey_Backspace, ImGuiKey_GamepadFaceRight};
template <size_t N> static bool keys_pressed(const ImGuiKey (&k)[N]) { for (ImGuiKey x : k) if (ImGui::IsKeyPressed(x, true)) return true; return false; }
template <size_t N> static bool keys_down(const ImGuiKey (&k)[N]) { for (ImGuiKey x : k) if (ImGui::IsKeyDown(x)) return true; return false; }
static void build_save_dialog(float w, float hgt) {
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
/* everything the closed state shows, and the bar's state machine (see the file's header comment) */
static void build_closed(void) {
	ImGuiIO &io = ImGui::GetIO();
	const float w = io.DisplaySize.x, hgt = io.DisplaySize.y, dt = io.DeltaTime;
	const double t = ImGui::GetTime();
	g_bar_h = 0;
	const bool popup_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
	if (g_dismiss_req) {   /* Escape: the menus close, the bar leaves, and the pointer sitting in the top zone doesn't bring it straight back */
		g_dismiss_req = false;
		if (popup_open) ImGui::ClosePopupsExceptModals();
		g_bar_want = false; g_bar_pin_until = -1e9; g_bar_active_t = -1e9; g_hot_since = -1; g_hot_block = true;
	}
	if (g_has_window) {
		if (!g_toast_checked) {   /* the hint, once per game, on this overlay's first windowed frame */
			g_toast_checked = true;
			if (!settings_has("touch_intro_shown")) {   /* the player's first game: the touch card first (it says where the menu is too) */
				g_intro = true; g_intro_tap = false; settings_set_int("menu_hint_shown", 1); settings_save(); overlay_open();
			} else if (!settings_has("menu_hint_shown")) { g_toast_until = t + TOAST_S; overlay_toast_titled("Menu", "Move the mouse to the top, or press [F10] or the PS button", TOAST_S); settings_set_int("menu_hint_shown", 1); settings_save(); }
		}
		const ImVec2 m = io.MousePos;
		const bool inside = ImGui::IsMousePosValid(&m) && m.x >= 0 && m.x < w && m.y >= 0 && m.y < hgt;   /* (focused but outside the window, e.g. on the title bar: the backend reports a position there too) */
		const float bar_h = ImGui::GetFrameHeight();
		const bool in_hot = inside && m.y <= HOT_PX;
		if (!in_hot) { g_hot_since = -1; g_hot_block = false; }
		else if (g_hot_since < 0) g_hot_since = t;
		if (in_hot && !g_hot_block && t - g_hot_since >= HOT_DWELL_S) { g_bar_want = true; g_bar_active_t = t; }
		const bool over_bar = inside && g_bar_vis > 0 && m.y <= bar_h * g_bar_vis + 2;
		if (over_bar || popup_open || io.WantCaptureMouse || t < g_bar_pin_until) g_bar_active_t = t;
		if (g_bar_want && t - g_bar_active_t > HIDE_S) g_bar_want = false;
		/* the pull tab: with the pointer near the top while the bar is away, and with the hint */
		const bool near_top = inside && m.y < TAB_NEAR_PX;
		const float tab_target = (near_top || t < g_toast_until) && g_bar_vis <= 0.01f ? 1.0f : 0.0f;
		g_tab_a += (tab_target - g_tab_a) * (dt > 0.1f ? 1.0f : dt * 8.0f);
		if ((tab_target == 0.0f && g_tab_a < 0.02f) || g_bar_vis > 0.01f) g_tab_a = 0;   /* (gone at once as the bar arrives: the two never overlap) */
		/* the OS cursor: hidden after a few seconds still over the picture, back the moment it moves */
		const bool moved = m.x != g_last_mouse.x || m.y != g_last_mouse.y; g_last_mouse = m;
		if (moved || ImGui::IsAnyMouseDown()) g_idle_since = t;
		if (g_bar_vis <= 0 && !popup_open && !io.WantCaptureMouse && !g_paused && t - g_idle_since > CURSOR_HIDE_S) ImGui::SetMouseCursor(ImGuiMouseCursor_None);
		if (g_touch_cursor && g_bar_vis <= 0 && !popup_open && !io.WantCaptureMouse) ImGui::SetMouseCursor(ImGuiMouseCursor_None);   /* (the touch mark stands in for it) */
	} else {   /* headless (the dump): no pointer; RECOMP_OVERLAY_DUMP_UI says what to show */
		if (g_force_bar) { g_bar_vis = 1; g_bar_want = true; }
		if (g_force_toast) { g_toast_until = t + TOAST_S; g_tab_a = 1; g_force_toast = false; overlay_toast_titled("Menu", "Move the mouse to the top, or press [F10] or the PS button", TOAST_S); overlay_toast_titled("Balance", "[A] [D] tilt left / right", TOAST_S); }
	}
	{	/* the slide, at a fixed rate either way */
		const float target = g_bar_want ? 1.0f : 0.0f, step = dt > 0.1f ? 1.0f : dt / SLIDE_S;
		g_bar_vis += target > g_bar_vis ? fminf(step, target - g_bar_vis) : -fminf(step, g_bar_vis - target);
	}
	if (g_bar_vis > 0) build_bar(w);
	if (g_tab_a > 0) build_tab(w, t);
	build_marks();
	if (g_has_window) build_save_dialog(w, hgt);
	if (g_paused) build_pause_banner(w, hgt);
	toasts_take(t);
	if (g_ntoasts) build_toasts(w, hgt, t);
}

/* shared by overlay_frame, overlay_marks and overlay_render_forced: builds one ImGui frame and renders
 * it into `target` with LOAD (never CLEAR -- the game's own frame, or the dump path's copy of it, is
 * already there and must show through everywhere the overlay doesn't draw). `have_window` picks
 * whether the SDL3 platform backend supplies DisplaySize/DeltaTime/mouse (interactive) or this
 * function sets them itself from the caller's w/h/dt_s (headless: see this file's own header
 * comment on the render paths). A frame that drew nothing (the bar away, no HUD) costs no render pass. */
static void render_frame(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, uint32_t w, uint32_t h, double dt_s, bool have_window, void (*build)(void) = build_ui) {
	if (!g_ctx || !w || !h) return;
	ImGui::SetCurrentContext(g_ctx);
	ImGuiIO &io = ImGui::GetIO();
	/* keyboard and gamepad navigation only while the full menu is up: the bar is pointer-only, and while
	 * it's closed a game's keys and pad are the game's (window.c forwards only mouse events then) */
	io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
	if (g_open) io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
	ImGui_ImplSDLGPU3_NewFrame();
	if (have_window) {
		ImGui_ImplSDL3_NewFrame();
	} else {
		io.DisplaySize = ImVec2((float)w, (float)h);
		io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
		io.DeltaTime = dt_s > 0 ? (float)dt_s : 1.0f / 60.0f;
	}
	ImGui::NewFrame();
	g_mouse_captured = io.WantCaptureMouse;   /* (computed by NewFrame from where the pointer was: the bar, a menu, the tab) */
	build();
	ImGui::Render();
	ImDrawData *dd = ImGui::GetDrawData();
	if (g_skip_draw || dd->DisplaySize.x <= 0 || dd->DisplaySize.y <= 0 || dd->TotalVtxCount == 0) return;
	/* RECOMP_OVERLAY_DEBUG=1: what ImGui handed the backend this frame -- per draw command, the vertices it
	 * indexes: their position box, how many are not finite, the first one's colour (a verification aid
	 * for the headless dump, where the picture can't be watched being drawn) */
	static int dbg = -1; if (dbg < 0) dbg = std::getenv("RECOMP_OVERLAY_DEBUG") != nullptr;
	if (dbg) {
		std::fprintf(stderr, "    overlay frame: %d lists, %d vtx, %d idx\n", dd->CmdListsCount, dd->TotalVtxCount, dd->TotalIdxCount);
		for (int l = 0; l < dd->CmdListsCount; l++) {
			const ImDrawList *dl = dd->CmdLists[l];
			for (int c = 0; c < dl->CmdBuffer.Size; c++) {
				const ImDrawCmd &pc = dl->CmdBuffer[c];
				if (!pc.ElemCount) continue;
				float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX; int bad = 0;
				for (unsigned i = 0; i < pc.ElemCount; i++) {
					const ImDrawVert &v = dl->VtxBuffer[dl->IdxBuffer[pc.IdxOffset + i] + pc.VtxOffset];
					if (!std::isfinite(v.pos.x) || !std::isfinite(v.pos.y) || !std::isfinite(v.uv.x) || !std::isfinite(v.uv.y)) { bad++; continue; }
					x0 = fminf(x0, v.pos.x); y0 = fminf(y0, v.pos.y); x1 = fmaxf(x1, v.pos.x); y1 = fmaxf(y1, v.pos.y);
				}
				const ImDrawVert &v0 = dl->VtxBuffer[dl->IdxBuffer[pc.IdxOffset] + pc.VtxOffset];
				std::fprintf(stderr, "      list %d '%s' cmd %d: %u idx (vtx off %u, idx off %u) clip (%.0f,%.0f)-(%.0f,%.0f) pos box (%.1f,%.1f)-(%.1f,%.1f) nonfinite %d first col %08x uv (%.4f,%.4f) texdata %p (status %d, %dx%d)\n",
					l, dl->_OwnerName ? dl->_OwnerName : "?", c, pc.ElemCount, pc.VtxOffset, pc.IdxOffset, pc.ClipRect.x, pc.ClipRect.y, pc.ClipRect.z, pc.ClipRect.w, x0, y0, x1, y1, bad, v0.col, v0.uv.x, v0.uv.y,
					(void *)pc.TexRef._TexData, pc.TexRef._TexData ? (int)pc.TexRef._TexData->Status : -1, pc.TexRef._TexData ? pc.TexRef._TexData->Width : 0, pc.TexRef._TexData ? pc.TexRef._TexData->Height : 0);
			}
		}
	}
	/* mandatory before the render pass below: uploads the vertex/index buffers. It also acquires and
	 * submits a command buffer of its own whenever ImGui has a texture to create or update (the font
	 * atlas: the first frame, and any frame a new glyph is rasterised), with SDL's plain
	 * SDL_SubmitGPUCommandBuffer -- the backend is third_party code built without gpulock.h's renames
	 * -- so the whole call runs under the host's submit lock, as every submit must (gpulock.h) */
	gpu_lock(); ImGui_ImplSDLGPU3_PrepareDrawData(dd, cmd); gpu_unlock();
	SDL_GPUColorTargetInfo ti = {};
	ti.texture = target; ti.load_op = SDL_GPU_LOADOP_LOAD; ti.store_op = SDL_GPU_STOREOP_STORE; ti.cycle = false;
	SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ti, 1, nullptr);
	ImGui_ImplSDLGPU3_RenderDrawData(dd, cmd, rp);
	SDL_EndGPURenderPass(rp);
}
/* ---- the touch card (this file's header): the three ways to touch, the device last used highlighted */
enum { DEV_KBM, DEV_PAD, DEV_TOUCH };
static int g_intro_dev = -1; static double g_intro_done = -1;   /* the device highlighted (-1: not yet chosen); when the touch was made */
extern "C" void overlay_intro_touchpad(void) { if (g_intro) g_intro_tap = true; }   /* (window.c: a gamepad's touchpad, while the overlay has input) */
static void pad_names(const char **l, const char **r, const char **a, bool *touchpad) {   /* the first pad's own button names */
	*l = "L1"; *r = "R1"; *a = "Cross"; *touchpad = true;   /* (the Vita's own names, and a DualShock 4's or DualSense's: unless the pad says otherwise) */
	int n = 0; SDL_JoystickID *ids = SDL_GetGamepads(&n);
	if (ids && n > 0) {
		const SDL_GamepadType t = SDL_GetGamepadTypeForID(ids[0]);
		if (t == SDL_GAMEPAD_TYPE_XBOX360 || t == SDL_GAMEPAD_TYPE_XBOXONE) { *l = "LB"; *r = "RB"; *a = "A"; *touchpad = false; }
		else if (t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR) { *l = "L"; *r = "R"; *a = "B"; *touchpad = false; }
		else if (t == SDL_GAMEPAD_TYPE_PS3) *touchpad = false;
	}
	SDL_free(ids);
}
static void build_intro(void) {
	ImGuiIO &io = ImGui::GetIO(); const double t = ImGui::GetTime();
	const bool touch = io.MouseSource == ImGuiMouseSource_TouchScreen;
	if (g_intro_dev < 0) g_intro_dev = window_kbm_active() ? DEV_KBM : DEV_PAD;
	/* the device in use: whatever the player last moved, pressed or touched */
	if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || io.KeyAlt) g_intro_dev = touch ? DEV_TOUCH : DEV_KBM;
	for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadR3; k++) if (ImGui::IsKeyDown((ImGuiKey)k)) { g_intro_dev = DEV_PAD; break; }
	if (g_intro_done < 0) {   /* a touch, made the way the card says: done */
		const bool click = ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1);
		const bool pad = (ImGui::IsKeyDown(ImGuiKey_GamepadL1) || ImGui::IsKeyDown(ImGuiKey_GamepadR1)) && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
		if (click && touch) { g_intro_dev = DEV_TOUCH; g_intro_done = t; }
		else if (click && io.KeyAlt) { g_intro_dev = DEV_KBM; g_intro_done = t; }
		else if (pad || g_intro_tap.exchange(false)) { g_intro_dev = DEV_PAD; g_intro_done = t; }
	} else if (g_has_window && t - g_intro_done > 1.2) overlay_close();   /* (a moment to see it worked, then the game) */
	const char *pl, *pr, *pa; bool tp; pad_names(&pl, &pr, &pa, &tp);
#ifdef __APPLE__
	const char *alt = "Option";
#else
	const char *alt = "Alt";
#endif
	char kbm[200], padb[240];
	snprintf(kbm, sizeof kbm, "Hold %s and click to touch the screen. Right-click instead to touch the rear pad.", alt);
	snprintf(padb, sizeof padb, "Hold %s and steer the cursor with a stick, then press %s to touch. %s does the same on the rear pad.%s", pl, pa, pr, tp ? " The touchpad works too." : "");
	const struct { const char *name, *how; } devs[3] = {{"Keyboard and mouse", kbm}, {"Controller", padb}, {"Touch screen", "Touch the picture, as you would the Vita's own screen."}};
	const ImGuiViewport *vp = ImGui::GetMainViewport(); const float em = ImGui::GetFontSize();
	ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), ui::u32(ui::bg, 0.55f));   /* (the paused game, dimmed behind it) */
	const float cw = fminf(vp->Size.x - 2 * em, 32 * em);
	ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(cw, 0), ImVec2(cw, vp->Size.y - 2 * em));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.4f, em * 1.2f));
	ImGui::Begin("##touchcard", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav);
	ui::heading(g_intro_done < 0 ? "Touching the screen" : "That's it", em * 1.35f);
	ui::muted_wrapped("Vita games are played with the touch screen as well as buttons. Here's how to touch it:");
	ImGui::Dummy(ImVec2(0, em * 0.2f));
	for (int d = 0; d < 3; d++) {
		const bool on = d == g_intro_dev;
		ImGui::PushStyleColor(ImGuiCol_ChildBg, on ? ui::with_alpha(ui::accent, 0.13f) : ui::card);
		ImGui::PushStyleColor(ImGuiCol_Border, on ? ui::with_alpha(ui::accent, 0.55f) : ui::border);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, em * 0.4f);
		char id[16]; snprintf(id, sizeof id, "##dev%d", d);
		ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
		ImGui::PushStyleColor(ImGuiCol_Text, on ? ui::accent : ui::text); ImGui::TextUnformatted(devs[d].name); ImGui::PopStyleColor();
		ImGui::PushStyleColor(ImGuiCol_Text, on ? ui::text : ui::muted); ImGui::TextWrapped("%s", devs[d].how); ImGui::PopStyleColor();
		ImGui::EndChild();
		ImGui::PopStyleVar(); ImGui::PopStyleColor(2);
	}
	ImGui::Dummy(ImVec2(0, em * 0.2f));
	if (g_intro_done < 0) { ImGui::PushStyleColor(ImGuiCol_Text, ui::accent); ImGui::TextUnformatted("Try it now to continue."); ImGui::PopStyleColor(); }
	else ImGui::TextUnformatted("Back to the game.");
	ui::muted_wrapped("F10 opens the menu, with every control. Esc skips this.");
	ImGui::End();
	ImGui::PopStyleVar();
}
void overlay_frame(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, uint32_t w, uint32_t h, double dt_s) {
	if (!overlay_is_open()) return;
	render_frame(cmd, target, w, h, dt_s, g_has_window, g_intro ? build_intro : build_ui);
}
void overlay_marks(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, uint32_t w, uint32_t h, double dt_s, const OverlayMark *marks, int n, const float rear_rect[4]) {
	if (overlay_is_open()) return;   /* the menu has its own fps line (page_performance) */
	g_nosd = metrics_osd_lines(g_osd, 6);
	/* headless (the dump's HUD check): only with something to show. Windowed: every present -- the bar's
	 * hover and timers, the cursor and the capture flag live in this frame (a frame that draws nothing is
	 * a few microseconds of CPU and no render pass: render_frame) */
	if (!g_has_window && !n && !g_nosd && !(rear_rect && rear_rect[2] > 0)) return;
	g_marks = marks; g_nmarks = n;
	for (int i = 0; i < 4; i++) g_rear[i] = rear_rect ? rear_rect[i] : 0;
	render_frame(cmd, target, w, h, dt_s, g_has_window, build_closed);
}
/* RECOMP_OVERLAY_DUMP (window.c): deliberately bypasses overlay_is_open() -- this must never be the
 * thing that makes the rest of the host think the menu is open (display.c's pause gate, window.c's
 * input routing and audio mute all key off that flag, not off anything here). `ui` (overlay.h): NULL
 * or "menu" the full menu; "bar" the menu bar down; "Game"/"View"/"Controls"/"Help" the bar with that
 * menu open; "toast" the first-launch hint and the pull tab. */
/* one headless frame of `build`, on a command buffer of its own when drawn (the SDLGPU3 backend keeps one
 * vertex/index buffer set and one texture-upload path per frame: one drawn frame per command buffer, as
 * the interactive present loop does, keeps a later frame's uploads out of an earlier frame's draws) */
static void forced_frame(SDL_GPUDevice *device, SDL_GPUTexture *target, uint32_t w, uint32_t h, bool draw, void (*build)(void)) {
	g_skip_draw = !draw;
	SDL_GPUCommandBuffer *cmd = draw ? SDL_AcquireGPUCommandBuffer(device) : nullptr;
	if (draw && !cmd) { g_skip_draw = false; return; }
	render_frame(cmd, target, w, h, 0, false, build);
	if (cmd) SDL_SubmitGPUCommandBuffer(cmd);
	g_skip_draw = false;
}
void overlay_render_forced(SDL_GPUDevice *device, SDL_GPUTexture *target, uint32_t w, uint32_t h, const char *ui) {
	if (!g_ctx || !device) return;
	g_nmarks = 0; g_rear[2] = 0; g_nosd = 0;
	if (!ui || !*ui || !strncmp(ui, "menu", 4)) {
		/* "menu:<tab>" opens the full menu on that page (the tab bar applies a selection on its next layout) */
		if (ui && ui[4] == ':') for (int i = 0; i < TAB_N; i++) if (!strcmp(ui + 5, g_tab_names[i])) g_select_tab = i;
		/* three frames, the last one drawn: a window ImGui has never seen before (every window here, this
		 * context's first and only frame otherwise) renders nothing on its own first Begin/End -- auto-fit
		 * sizing and the font atlas's GPU texture both need one full frame to settle before anything is
		 * visible -- and a tab selection takes one more. The interactive path (overlay_frame) never needs
		 * this: it runs every loop iteration while the menu is open, so the same one-frame blank
		 * self-corrects at ~60 Hz, imperceptibly, on its own. */
		for (int i = 0; i < 3; i++) forced_frame(device, target, w, h, i == 2, build_ui);
		return;
	}
	if (!strncmp(ui, "intro", 5)) {   /* "intro", "intro:pad", "intro:touch": the touch card with that device highlighted */
		g_intro_dev = !strcmp(ui, "intro:pad") ? DEV_PAD : !strcmp(ui, "intro:touch") ? DEV_TOUCH : DEV_KBM; g_intro_done = -1;
		for (int i = 0; i < 3; i++) forced_frame(device, target, w, h, i == 2, build_intro);
		g_intro_dev = -1;
		return;
	}
	if (!strcmp(ui, "toast")) g_force_toast = true;
	else { g_force_bar = true; if (strcmp(ui, "bar")) g_force_menu = ui; }
	g_nosd = metrics_osd_lines(g_osd, 6);   /* the HUD too (RECOMP_OSD), in the same frame as the bar so it layers as it does live: under it */
	/* three frames: the bar's first, its menu popup's auto-fit on the second, and only the settled third
	 * drawn (the texture is LOADed into, so drawing all three would stack the bar's translucency) */
	for (int i = 0; i < 3; i++) forced_frame(device, target, w, h, i == 2, build_closed);
	g_nosd = 0;
	/* then put the closed state back the way a run finds it, inside one more (undrawn) frame so an open
	 * menu popup is closed through ImGui itself, not left in its popup stack */
	g_force_bar = false; g_force_menu = nullptr; g_bar_vis = 0; g_bar_want = false; g_toast_until = -1e9; g_tab_a = 0; g_dismiss_req = true;
	forced_frame(device, target, w, h, false, build_closed);
}
