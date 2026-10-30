/*
 * The host window (SDL3): shows presented frames and turns keyboard and gamepad input into the
 * Vita's controller state. SDL wants the process's main thread, so in windowed mode the game runs
 * on its own thread (window_run) and hands each frame over through a shared buffer. Presentation
 * goes through SDL's GPU API (SDL_GPU): the software backend's frame is uploaded as a sampled
 * texture and blitted to the swapchain. This also stands up the SDL_GPUDevice/SDL_Window that a
 * future RECOMP_GFX=gpu backend (gfx_gpu.c) will render into directly instead of blitting.
 *
 * keys: arrows d-pad, Z cross, X circle, A square, S triangle, Enter start, Backspace select,
 *       Q/W L/R triggers; any SDL gamepad (A/B/X/Y as cross/circle/square/triangle); F1 cycles the
 *       performance overlay off / basic (FPS + frame time) / full (metrics.c); F10 (or a gamepad's
 *       Back+Start held together, or its Guide button) opens/closes the in-game menu (overlay.c*,
 *       Dear ImGui), Escape also closes it -- while it's open every key/mouse/gamepad event above
 *       goes to the menu instead, not the game (see the event loop's own comment on why). The mouse
 *       resting at the window's top edge slides the menu BAR down (overlay.cpp: the same settings,
 *       without pausing; every item shows its hotkey); mouse events always reach ImGui so it can see
 *       that hover, and one it captures (the bar, a menu, the pull tab) is never a touch. Escape
 *       hides the bar. The OS cursor hides after a few seconds still over the picture (overlay.cpp).
 * env:  RECOMP_HEADLESS=1   no window (tests, the checker); RECOMP_SCALE=n window scale (default 1)
 *       RECOMP_RENDER_SCALE=n the GPU backend's internal resolution (1-6, default 1; F2 cycles them: gfx_gpu.c)
 *       RECOMP_OSD=0|1|2    the overlay's level at start-up (default 0: see metrics.c and window_present_gpu's own comment on why the GPU path doesn't default it on)
 *       RECOMP_METRICS=1    one performance line to stderr a second (metrics.c), with or without a window
 *       RECOMP_OSD_DUMP=f   writes the overlay's own bitmap to f once, to check the font (see metrics.c)
 *       RECOMP_OVERLAY_DUMP=f.ppm  headless: renders the in-game menu once, open, over the next
 *                           presented frame, into an offscreen texture, and writes it to f -- so the
 *                           menu can be looked at without a screen (see overlay_dump_maybe below)
 *       RECOMP_OVERLAY_DUMP_UI=menu|menu:<tab>|bar|Game|View|Controls|Help|toast|intro[:pad|:touch][@<s>]|quit[,...]  what that dump
 *                           shows (overlay.h: overlay_render_forced); several, comma-separated, write
 *                           one file each, f_<name>.ppm
 */
#include <SDL3/SDL.h>
#include "gpulock.h"   /* every SDL submit/fence call below is the locked one: Homebrew SDL 3.4.16's Metal fence race, see that header */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "rt.h"
#include "hle.h"
#include "metrics.h"
#include "settings.h"
#include "overlay.h"
#include "controller_layout.h"

enum { SELECT = 0x1, START = 0x8, UP = 0x10, RIGHT = 0x20, DOWN = 0x40, LEFT = 0x80, LTRIG = 0x100, RTRIG = 0x200,
       TRIANGLE = 0x1000, CIRCLE = 0x2000, CROSS = 0x4000, SQUARE = 0x8000,
       /* a PS TV's pad only (window_input_pad2; the handheld's reads never see them): the stick clicks, and its L2/R2 here
        * (0x100/0x200 in its reads, where LTRIG/RTRIG become its L1/R1) */
       L3 = 0x2, R3 = 0x4, L1 = 0x400, R1 = 0x800, PAD_L2 = 0x10000, PAD_R2 = 0x20000 };

void (*window_f9)(void);   /* F9: set by a VERIFY build (verify.c's count window); none otherwise */
void (*window_f6)(void);   /* F6: set by a VERIFY build (verify.c's 60/30 A/B capture); none otherwise */
static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }   /* metrics.c: the window loop's own upload/blit cost */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *frame;                 /* RGBA, the latest presented frame */
static uint32_t fw, fh, new_frame;
static volatile uint32_t buttons;
static volatile uint8_t sticks[4] = {128, 128, 128, 128};
static int active;
static int self_paced; static uint64_t last_paced_us;   /* a present mode that doesn't wait for vsync: the present loop paces itself */
static volatile int touch_down, touch_x, touch_y;   /* the mouse as the front touch panel (1920x1088) */
/* Gamepad touch -- the agreed scheme (memory: input-touch-mapping-design): the triggers are L and R;
 * the bumpers are touch modifiers. A DS4/DualSense touchpad's fingers touch the front panel finger
 * for finger, or the rear one when RB is held as the finger lands (it stays on that panel until it
 * lifts). Holding LB (front) or RB (rear) on any pad turns the sticks into a cursor on that panel
 * (either one moves it) and Cross into the touch (hold to drag); the game sees both sticks centred meanwhile,
 * and a marker shows where the cursor is (overlay_marks). The right mouse button touches the rear
 * panel as the left one does the front. Panels: front 0..1919 x 0..1087, rear 0..1919 x 108..889
 * (display.c's sceTouchGetPanelInfo). Written by this thread, read by the game's through
 * window_touch_contacts. */
enum { TP_FINGERS = 2, REAR_Y0 = 108, REAR_Y1 = 889 };
static volatile int tp_down[TP_FINGERS], tp_port[TP_FINGERS], tp_x[TP_FINGERS], tp_y[TP_FINGERS];
/* a touchscreen's own fingers (screen_finger): the front panel where each touches the picture, the rear with RB held as it lands */
enum { ST_FINGERS = 6 };
static volatile int st_down[ST_FINGERS], st_port[ST_FINGERS], st_x[ST_FINGERS], st_y[ST_FINGERS]; static SDL_FingerID st_id[ST_FINGERS];
static volatile int cur_mode;              /* 0 off, 1 front (LB held), 2 rear (RB held) */
static volatile int cur_down, cur_x[2] = {960, 960}, cur_y[2] = {544, (REAR_Y0 + REAR_Y1) / 2};
/* Two cursors (two-finger gestures -- Killzone's valves): while a bumper is held, once BOTH sticks have moved the cursor
 * splits in two -- the left stick keeps the first, the right stick steers a second that starts where the first was -- and
 * the press (Cross, L2 or R2) puts both down. Until then either stick moves the one cursor, as before, so one-finger use
 * (menus, drawing) never grows a stray second touch. Reset when the bumper is let go. */
static volatile int cur2_on, cur2_x[2], cur2_y[2];
static int cur_used_l, cur_used_r;
static int cur_cross, cur_trig[2];   /* what holds the cursor touch down: Cross, L2, R2 (cur_down = any of them) */
static int trig_touch[2];            /* this trigger pressed a cursor touch: no L/R to the game until it is let go */
static volatile int mouse_rear_down, mouse_rear_x, mouse_rear_y;
static int lb_held, rb_held, ltrig_on, rtrig_on;   /* the pair that holds the touch cursor; the pair that is L and R (pad_second, pad_main) */
static int ptrig[2];                                /* the physical triggers past their press point (hysteresis) */
/* "shoulder_buttons" (shared): "triggers", the default: the triggers are the game's L and R and the bumpers the other pair
 * (the touch cursor; a PS TV's L2/R2); "bumpers": the other way round, as a PS TV's own pad has it. Read with the bindings
 * (kb_reload: at start and as the menu closes, when nothing is held) */
static int shoulders_swapped;
static volatile int pad_gyro;               /* the pad has a gyroscope and accelerometer streaming (motion.c) */
void motion_feed_accel(const float s[3], uint64_t t_ns); void motion_feed_gyro(const float s[3], uint64_t t_ns);
void motion_recenter(void); void motion_pause(void); void motion_lost(void); void motion_feed_virtual(float angle, uint64_t t_ns); int motion_use_virtual(int on, float *roll);
int window_gyro_present(void) { return pad_gyro; }
static SDL_Window *win;

/* ---- keyboard + mouse (settings "input_mode": auto | pad | kbm, default auto; "mouse_look" 1, "mouse_sens"
 * 1.0, "mouse_invert" 0, "bind_<action>" an SDL key name). auto: the scheme follows the device last used --
 * a bound key switches to keyboard+mouse, a pad button or stick back to the pad. With keyboard+mouse on
 * (and the menu closed, the window focused, Alt not held) the mouse is captured: its motion is the look,
 * the left button R (fire), the right L (aim); holding Alt frees the cursor, which is then the touch panel
 * as before (left button front, right rear). The keys are a PC layout by default (WASD the left stick,
 * Space Cross, ...), every one rebindable on the menu's Controls page.
 * The look: a game with a raw-look hook (hooks.c, e.g. uc_mouse_look) takes the motion each game frame
 * through window_mouse_delta and turns it into its camera directly, no stick curve in between; with no
 * such hook taking it (none for the game, or not in play) the motion is a right-stick stand-in instead.
 * The window thread writes, game threads read: the motion is two fixed-point accumulators, exchanged. */
static int kbm_auto;                       /* auto mode's current choice: 1 keyboard+mouse, 0 pad */
static volatile int kbm_on, rel_on;
static uint64_t rel_since_us;              /* when the capture last began (rel_on) */        /* keyboard+mouse in use; the mouse captured (looking) */
static int mouse_dx_q8, mouse_dy_q8;       /* motion since a raw-look hook last took it, x256 (atomics) */
static int look_dx_q8, look_dy_q8;         /* the same motion, for the right-stick stand-in (this thread) */
static volatile uint64_t raw_taken_us;     /* when a raw-look hook last took the motion */
static int kbm_lmb, kbm_rmb;               /* captured mouse buttons held (R, L) */
/* Tilt (motion.c's virtual pad): Uncharted's balance beams and river slide are steered by tilting the Vita, and
 * keyboard and mouse have no gyro. In a tilt section (window_tilt_tick: the section's per-frame update, hooked in
 * hooks.c) movement is restricted, so the move-left/right keys (A / D) tilt -- easing to a full tilt while held,
 * back to level on release -- and a pad without a gyro tilts with its left stick; that stick's left/right is held
 * back from the game there (window_input). The mouse stays the look. A gyro pad in hand tilts itself. */
static volatile uint64_t tilt_tick_ns;     /* the game's last tilt-section tick (window_tilt_tick, any thread; SDL_GetTicksNS) */
static int tilt_section;                   /* in a tilt section: a tick within the last 0.25 s (this thread, each frame) */
static volatile int tilt_kind;             /* what the section is (window_tilt_tick): 1 a balance beam, 2 a slide -- the toast's wording */
static float tilt;                         /* radians, positive = the right side down */
#define TILT_MAX 0.70f                     /* ~40 degrees either way */
/* the game is in a tilt section this frame (a hook on the section's per-frame update, hooks.c): the section lasts
 * while these keep coming and lapses 0.25 s after the last (a paused game stops them too) */
void window_tilt_tick(int kind) { tilt_kind = kind; __atomic_store_n(&tilt_tick_ns, SDL_GetTicksNS(), __ATOMIC_RELAXED); }
enum { KB_MOVE_UP, KB_MOVE_DOWN, KB_MOVE_LEFT, KB_MOVE_RIGHT };   /* the first four bindings: the left stick */
typedef struct { const char *key, *label, *def; uint32_t bit; SDL_Keycode kc; int held; } KbBind;
static KbBind kb_binds[] = {
	{"bind_move_up", "Move forward (left stick)", "W", 0}, {"bind_move_down", "Move back", "S", 0},
	{"bind_move_left", "Move left", "A", 0}, {"bind_move_right", "Move right", "D", 0},
	{"bind_cross", "Cross", "Space", CROSS}, {"bind_circle", "Circle", "C", CIRCLE},
	{"bind_square", "Square", "F", SQUARE}, {"bind_triangle", "Triangle", "E", TRIANGLE},
	{"bind_l", "L (also the right mouse button)", "Q", LTRIG}, {"bind_r", "R (also the left mouse button)", "R", RTRIG},
	{"bind_start", "Start", "Return", START}, {"bind_select", "Select", "Tab", SELECT},
	{"bind_up", "D-pad up", "Up", UP}, {"bind_down", "D-pad down", "Down", DOWN},
	{"bind_left", "D-pad left", "Left", LEFT}, {"bind_right", "D-pad right", "Right", RIGHT},
};
enum { KB_N = (int)(sizeof kb_binds / sizeof kb_binds[0]) };
static void tilt_section_update(void) {
	uint64_t t = __atomic_load_n(&tilt_tick_ns, __ATOMIC_RELAXED);
	int on = t && SDL_GetTicksNS() - t < 250000000ull;
	if (on && !tilt_section) {   /* what tilts, as a badge (overlay.cpp) */
		const char *title = tilt_kind == 2 ? "Slide" : "Balance", *verb = tilt_kind == 2 ? "steer" : "tilt";
		char msg[160];
		if (kbm_on) snprintf(msg, sizeof msg, "[%s] [%s] %s left / right", SDL_GetKeyName(kb_binds[KB_MOVE_LEFT].kc), SDL_GetKeyName(kb_binds[KB_MOVE_RIGHT].kc), verb);
		else if (!pad_gyro) snprintf(msg, sizeof msg, "Left stick: %s left / right", verb);
		else snprintf(msg, sizeof msg, "Tilt the controller to %s", verb);
		overlay_toast_titled(title, msg, 5.0);
	}
	tilt_section = on;
}
static void kb_reload(void) {   /* the bindings from settings (at start, and when the menu closes: a rebind may have changed them) */
	for (int i = 0; i < KB_N; i++) {
		char b[48]; const char *name = settings_str(kb_binds[i].key, kb_binds[i].def, b, sizeof b);
		SDL_Keycode k = SDL_GetKeyFromName(name);
		kb_binds[i].kc = k != SDLK_UNKNOWN ? k : SDL_GetKeyFromName(kb_binds[i].def);
	}
	char sb[16]; shoulders_swapped = !strcmp(settings_str("shoulder_buttons", "triggers", sb, sizeof sb), "bumpers");
}
int window_kb_count(void) { return KB_N; }
const char *window_kb_label(int i) { return i >= 0 && i < KB_N ? kb_binds[i].label : ""; }
const char *window_kb_key_name(int i) { return i >= 0 && i < KB_N ? SDL_GetKeyName(kb_binds[i].kc) : ""; }
void window_kb_bind(int i, SDL_Keycode k) {   /* the menu's Rebind: that action now on that key (a key another action had is taken from it) */
	if (i < 0 || i >= KB_N || k == SDLK_UNKNOWN) return;
	for (int j = 0; j < KB_N; j++) if (j != i && kb_binds[j].kc == k) { kb_binds[j].kc = SDLK_UNKNOWN; settings_set_str(kb_binds[j].key, ""); }
	kb_binds[i].kc = k; settings_set_str(kb_binds[i].key, SDL_GetKeyName(k)); settings_save();
}
void window_kb_reset(void) { for (int i = 0; i < KB_N; i++) settings_set_str(kb_binds[i].key, kb_binds[i].def); settings_save(); kb_reload(); }
int window_kbm_active(void) { return kbm_on; }
int window_controller_layout_auto(void) {
	char b[8]; const char *mode = settings_str("input_mode", "auto", b, sizeof b);
	const int headless = getenv("RECOMP_HEADLESS") != NULL;
	return controller_layout_auto_buttons(mode, kbm_on, headless ? 0 : SDL_HasGamepad(), headless);
}
/* a game's raw-look hook, once per game frame: the mouse's motion since its last call, in counts (y down),
 * and whether the mouse is looking at all (0: nothing to apply, the deltas are 0) */
/* RECOMP_INPUT_MOUSE="from-to:dx,dy;...": mouse counts per call over presented-frame ranges -- a headless
 * test's mouse (the raw-look hooks), as RECOMP_INPUT_STICKS is its sticks (display.c) */
extern uint64_t display_frames(void);
static int mouse_script(float *ix, float *iy) {
	static const char *s = (const char *)-1; if (s == (const char *)-1) s = getenv("RECOMP_INPUT_MOUSE");
	int hit = 0; uint64_t f = display_frames();
	for (const char *q = s; q && *q; ) {
		unsigned long a = strtoul(q, (char **)&q, 10), z = a; float vx = 0, vy = 0;
		if (*q == '-') z = strtoul(q + 1, (char **)&q, 10);
		if (*q == ':') { vx = strtof(q + 1, (char **)&q); if (*q == ',') vy = strtof(q + 1, (char **)&q); }
		if (f >= a && f <= z) { *ix += vx; *iy += vy; hit = 1; }
		if (*q == ';') q++; else break;
	}
	return hit;
}
int window_mouse_delta(float *dx, float *dy) {
	int x = __atomic_exchange_n(&mouse_dx_q8, 0, __ATOMIC_RELAXED), y = __atomic_exchange_n(&mouse_dy_q8, 0, __ATOMIC_RELAXED);
	raw_taken_us = now_us();
	float ix = 0, iy = 0; int scripted = mouse_script(&ix, &iy);
	if (!rel_on && !scripted) { *dx = *dy = 0; return 0; }
	*dx = (rel_on ? x / 256.0f : 0) + ix; *dy = (rel_on ? y / 256.0f : 0) + iy; return 1;
}
float window_mouse_sens(void) { return (float)settings_float("mouse_sens", 1.0); }
int window_mouse_invert(void) { return settings_int("mouse_invert", 0) != 0; }
/* direct mouse aim (raw look: a game's hook writing mouse counts into its camera, hooks.c), the menu's Mouse aim and F8
 * live (setting raw_look): on by default in a played run -- a game with no such hook, or a hook not in play, leaves the
 * motion to the stick stand-in above by itself, so it never takes the mouse look away. Off: always the stand-in, which
 * the stick's own curve and speed make stepped and late (a fresh install's Killzone, 2026-10-22). A scripted run
 * (RECOMP_HEADLESS) keeps the stand-in, as its recorded input was made with */
static int raw_look = -1;
int window_raw_look(void) {
	if (raw_look < 0) raw_look = getenv("RECOMP_RAW_LOOK") ? atoi(getenv("RECOMP_RAW_LOOK")) != 0 : settings_int("raw_look", getenv("RECOMP_HEADLESS") ? 0 : 1) != 0;   /* (RECOMP_RAW_LOOK: a test's) */
	return raw_look;
}
void window_set_raw_look(int on) { raw_look = on != 0; settings_set_int("raw_look", raw_look); settings_save(); }
static void raw_look_toggle(void) {
	window_set_raw_look(!window_raw_look());
	overlay_toast_titled("Mouse aim", raw_look ? "Direct: the game turns as far as the mouse moves [F8]" : "Stick: the mouse drives the right stick [F8]", 3.0);
}
static int kb_find(SDL_Keycode k) { for (int i = 0; i < KB_N; i++) if (kb_binds[i].kc == k && k != SDLK_UNKNOWN) return i; return -1; }

/* ---- aspect modes (the overlay's Display page, settings key "aspect"): how the Vita's 960x544
 * picture meets a window of any other shape. fit (today's default): letterbox, the whole picture
 * visible, bars where it doesn't reach. stretch: fills the window, non-uniform scale, no bars, the
 * picture distorted. fill: uniform scale to cover the window with no bars, cropping whatever
 * overhangs on one axis, centred. One function computes both rects (the window.c present loop's
 * blit source/destination AND mouse_to_touch's inverse of it) so a touch always lands under the
 * pixel it looks like it's under, in every mode -- see present_rect's own comment. */
enum { ASPECT_FIT, ASPECT_STRETCH, ASPECT_FILL };
static int aspect_mode = ASPECT_FIT;
static const char *aspect_name(int m) { return m == ASPECT_STRETCH ? "stretch" : m == ASPECT_FILL ? "fill" : "fit"; }
int window_get_aspect(void) { return aspect_mode; }
void window_set_aspect(int mode) {
	if (mode < ASPECT_FIT || mode > ASPECT_FILL) return;
	aspect_mode = mode;
	settings_set_str("aspect", aspect_name(mode)); settings_save();
}
int window_get_fullscreen(void) { return win && (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) != 0; }
void window_set_fullscreen(int on) {
	if (win) SDL_SetWindowFullscreen(win, on != 0);
	settings_set_int("fullscreen", on != 0); settings_save();
}
/* ---- wider aspect ratios (settings "aspect_ratio": native | 21:9 | 32:9 | window): the factor k the game's
 * horizontal view is widened by -- its camera hook (hooks.c, aspect_widen) builds the projection that
 * much wider into the same 960x544 frame, and the window shows that frame k times wider than native.
 * Only for a game whose conf declares such a hook (RECOMP_ASPECT_HOOK=1 in its RUN_ENV): without one the
 * picture would only be stretched. 1 = native (960:544, 1.765:1). */
static volatile uint32_t win_pw, win_ph;   /* the swapchain's size in pixels, as the present loop last saw it */
int window_aspect_supported(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_ASPECT_HOOK"); v = e && *e == '1'; } return v; }
float window_aspect_widen(void) {
	if (!window_aspect_supported()) return 1.0f;
	char b[16]; if (getenv("RECOMP_ASPECT")) snprintf(b, sizeof b, "%s", getenv("RECOMP_ASPECT")); else settings_str("aspect_ratio", "native", b, sizeof b);   /* RECOMP_ASPECT wins (headless tests) */
	const float native = 960.0f / 544.0f;
	float a = !strcmp(b, "21:9") ? 21.0f / 9.0f : !strcmp(b, "32:9") ? 32.0f / 9.0f : 0;
	if (!strcmp(b, "window") && win_pw && win_ph) a = (float)win_pw / (float)win_ph;
	float k = a > 0 ? a / native : 1.0f;
	return k < 1.02f ? 1.0f : k > 2.5f ? 2.5f : k;   /* narrower than native: native (letterboxed as before) */
}
/* the visible source sub-rect (sx,sy,sw,sh: fit/stretch show it all; fill crops it, centred, to the
 * window's own aspect) and where it lands in the window (dx,dy,dw,dh) -- source and window sizes in
 * any consistent unit (window points for mouse_to_touch's 960x544; actual texture pixels, which may
 * be render-scaled, for the present loop's blit): the aspect ratio is the same either way (960:544),
 * so both callers get the same rects as fractions of their own inputs. */
static void present_rect(float ww, float wh, float sw, float sh,
                          float *sx, float *sy, float *sw_out, float *sh_out,
                          float *dx, float *dy, float *dw, float *dh) {
	switch (aspect_mode) {
	case ASPECT_STRETCH:
		*sx = 0; *sy = 0; *sw_out = sw; *sh_out = sh;
		*dx = 0; *dy = 0; *dw = ww; *dh = wh;
		break;
	case ASPECT_FILL: {
		float wa = ww / wh, sa = (960.0f / 544.0f) * window_aspect_widen();   /* the picture's shape, whatever the frame's pixel ratio (gfx_gpu.c may render it wider) */
		if (wa > sa) { *sw_out = sw; *sh_out = sh * sa / wa; *sx = 0; *sy = (sh - *sh_out) * 0.5f; }
		else { *sh_out = sh; *sw_out = sw * wa / sa; *sy = 0; *sx = (sw - *sw_out) * 0.5f; }
		*dx = 0; *dy = 0; *dw = ww; *dh = wh;
		break;
	}
	default: {   /* ASPECT_FIT: the whole frame, letterboxed, shown window_aspect_widen() times wider than native */
		float ew = sh * (960.0f / 544.0f) * window_aspect_widen();   /* the picture's width at this height: its shape, not the frame's pixel ratio (gfx_gpu.c may render it wider) */
		float s = ww / ew < wh / sh ? ww / ew : wh / sh;
		*sx = 0; *sy = 0; *sw_out = sw; *sh_out = sh;
		*dw = ew * s; *dh = sh * s; *dx = (ww - *dw) * 0.5f; *dy = (wh - *dh) * 0.5f;
	}
	}
}
/* the mouse (window points) as a front-panel position, through present_rect's own picture (fit,
 * stretch or filled and cropped). Outside the visible picture: 0, unless clamping a drag to its
 * edge. present_rect's source rect is in 960x544 "screen" points; the touch panel is exactly double
 * that in both axes (1920x1088), as the hardware's panel is to its screen. */
static int mouse_to_touch(float mx, float my, int clamp, int *tx, int *ty) {
	int w = 0, h = 0; SDL_GetWindowSize(win, &w, &h);
	if (w <= 0 || h <= 0) return 0;
	float sx, sy, sw, sh, dx, dy, dw, dh;
	present_rect((float)w, (float)h, 960.f, 544.f, &sx, &sy, &sw, &sh, &dx, &dy, &dw, &dh);
	float u = (mx - dx) / dw, v = (my - dy) / dh;
	if (!clamp && (u < 0 || u >= 1 || v < 0 || v >= 1)) return 0;
	u = u < 0 ? 0 : u > 0.9999f ? 0.9999f : u; v = v < 0 ? 0 : v > 0.9999f ? 0.9999f : v;
	float px = sx + u * sw, py = sy + v * sh;   /* a point in the 960x544 screen, inside the visible crop */
	*tx = (int)(px * 2.f); *ty = (int)(py * 2.f);
	return 1;
}

/* the inverse, for drawing a touch where it lands: a panel point (front coordinates; the rear panel's
 * are the same space) to window points */
static void touch_to_point(int tx, int ty, float *x, float *y) {
	int w = 0, h = 0; SDL_GetWindowSize(win, &w, &h);
	float sx, sy, sw, sh, dx, dy, dw, dh;
	present_rect((float)w, (float)h, 960.f, 544.f, &sx, &sy, &sw, &sh, &dx, &dy, &dw, &dh);
	*x = dx + (tx * 0.5f - sx) / sw * dw; *y = dy + (ty * 0.5f - sy) / sh * dh;
}

/* ---- the Controls page (read-only): the GAME's controls -- the mappings kb_binds (the defaults; rebindable)/pad_bit (below) and
 * mouse_to_touch (above) actually implement, kept here as one small table so the overlay can show them
 * without reaching into this file's own logic. Keep in sync with those if a binding changes. The host's
 * own hotkeys (F1, F2, F10, F12, Guide, R3, the menu bar) are overlay.cpp's shortcuts table: keep that in
 * sync with the event loop below. */
static const char *const ctrl_rows[][3] = {
	{"D-pad", "Arrow keys", "D-pad"},
	{"Cross / Circle / Square / Triangle", "Space / C / F / E", "Cross / Circle / Square / Triangle (Xbox: A / B / X / Y)"},
	{"Start / Select", "Enter / Tab", "Options / Share or Create (Xbox: Menu / View)"},
	{"L / R", "Right mouse / left mouse (or Q / R)", "L2 / R2 (Xbox: LT / RT)"},
	{"Left / right stick", "WASD / the mouse", "Left / right stick"},
	{"Front touch panel", "Left mouse button (click, drag); hold Alt first while the mouse looks; a swipe prompt: just drag; a two-finger turn: hold and drag down / up; a touchscreen: touch the picture", "Touchpad; or hold L1: a stick + Cross / L2 / R2 (both sticks: two fingers); a two-finger turn: both sticks out, rolled"},
	{"Rear touch panel", "Right mouse button (click, drag); hold Alt first while the mouse looks; a touchscreen: touch with R1 held", "Touchpad with R1 held; or hold R1: a stick + Cross / L2 / R2 (both sticks: two fingers)"},
	{"Motion sensor (tilt, aim)", "--", "Gyro (DS4/DualSense); R3 recentres"},
	{"A PS TV layout's L2 / R2 / L3 / R3 (controller only)", "--", "L1 / R1 / L3 / R3"},
};
/* the pad column with "shoulder_buttons" = bumpers (the bumpers are L and R, the triggers hold the touch cursor) */
static const char *const ctrl_rows_swapped[][2] = {
	{"L / R", "L1 / R1 (Xbox: LB / RB)"},
	{"Front touch panel", "Touchpad; or hold L2: a stick + Cross / L1 / R1 (both sticks: two fingers); a two-finger turn: both sticks out, rolled"},
	{"Rear touch panel", "Touchpad with R2 held; or hold R2: a stick + Cross / L1 / R1 (both sticks: two fingers)"},
	{"A PS TV layout's L2 / R2 / L3 / R3 (controller only)", "L2 / R2 / L3 / R3"},
};
extern int display_pstv_looked(void);
int window_controls_count(void) { return (int)(sizeof ctrl_rows / sizeof ctrl_rows[0]) - !display_pstv_looked(); }   /* (the last row: a PS TV layout's, only in a game that has one) */
void window_controls_row(int i, const char **action, const char **key, const char **pad) {
	if (i < 0 || i >= window_controls_count()) { *action = *key = *pad = ""; return; }
	*action = ctrl_rows[i][0]; *key = ctrl_rows[i][1]; *pad = ctrl_rows[i][2];
	if (shoulders_swapped) for (size_t j = 0; j < sizeof ctrl_rows_swapped / sizeof ctrl_rows_swapped[0]; j++) if (!strcmp(*action, ctrl_rows_swapped[j][0])) *pad = ctrl_rows_swapped[j][1];
}
int window_touch(int *x, int *y) { *x = touch_x; *y = touch_y; return touch_down; }
/* A game hook's word that the game wants a front-panel swipe now (Killzone: a brutal melee / interrogation gesture window):
 * with the mouse captured (keyboard + mouse), the left button then drags a touch instead of firing -- from the panel's
 * centre, moved by the mouse's motion -- so a quick drag lands inside the window. Without it a swipe meant Alt for the
 * cursor, finding the spot, then dragging, and the window was over (found in testing, 2026-10-19). Valid for 200 ms a call. */
static uint64_t gesture_hint_ns;
void window_touch_gesture_hint(void) { __atomic_store_n(&gesture_hint_ns, SDL_GetTicksNS(), __ATOMIC_RELAXED); }
/* (never in a PS TV layout: no touch screen there, and its prompts want the stick itself, which a swipe would have muted) */
extern int display_pstv_active(void);
static int gesture_wanted(void) { uint64_t t = __atomic_load_n(&gesture_hint_ns, __ATOMIC_RELAXED); return t && SDL_GetTicksNS() - t < 200000000ull && !display_pstv_active(); }
static int gesture_drag;   /* the left button is dragging a touch (gesture_wanted when it went down) */
/* ... and with a pad: while the window is open a flick of the right stick is a swipe that way -- a touch from the panel's centre
 * 450 points out over 80 ms (the stick muted for the game meanwhile). Pressing a bumper, moving the cursor and dragging with
 * Cross never fit a 0.8 s interrogation window (found in testing, 2026-10-20). Another flick needs the stick back near the centre. */
static volatile int flick_down, flick_x, flick_y;
static uint64_t gesture_t0;
/* A game hook's word that the game wants a two-finger twist now (Killzone: a valve or an explosive being planted -- two
 * touches anywhere on the front panel, turned by how much the line between them turns). The pad: each stick is a thumb
 * on the wheel -- its finger sits out from the panel's centre the way the stick points, so both sticks pushed apart and rolled
 * the same way turn the line between them (one stick rolled around the other at rest does too); the fingers are down while
 * either stick is out, and the game sees the sticks centred meanwhile. Keyboard + mouse: the left button grips (two fingers
 * either side of the centre) and dragging down turns them clockwise, up anticlockwise; the camera stays. Settled on this
 * design, 2026-10-20 (two cursors with a press held made rolling them around each other too fiddly). Valid for 200 ms a call. */
static uint64_t twist_hint_ns;
void window_touch_twist_hint(void) { __atomic_store_n(&twist_hint_ns, SDL_GetTicksNS(), __ATOMIC_RELAXED); }
static int twist_wanted(void) { uint64_t t = __atomic_load_n(&twist_hint_ns, __ATOMIC_RELAXED); return t && SDL_GetTicksNS() - t < 200000000ull && !display_pstv_active(); }   /* (nor this) */
static volatile int twist_down, twist_x[2], twist_y[2];
static int twist_drag, twist_pad;   /* the left button / the sticks hold the twist's fingers down */
static float twist_ang;             /* the left button's twist: radians, clockwise on the panel */
static void twist_place(float a) {  /* the left button's two fingers, 300 points either side of the centre at angle a */
	const int dx = (int)lroundf(cosf(a) * 300), dy = (int)lroundf(sinf(a) * 300);
	twist_x[0] = 960 - dx; twist_y[0] = 544 - dy; twist_x[1] = 960 + dx; twist_y[1] = 544 + dy;
}
/* every contact on a panel now (port 0 front, 1 rear), each with a key that stays the same while
 * it's held (display.c gives each new key a new report id): 1 the mouse, 2 / 3 the bumper cursors, 4 a flick,
 * 5 / 6 a twist, 10+n touchpad finger n, 20+n touchscreen finger n. Returns how many, at most `max`. */
static int last_touch_key[2];   /* the input behind each panel's latest contact (window_last_touch_key) */
int window_last_touch_key(int port) { return last_touch_key[port & 1]; }
int window_touch_contacts(int port, int *keys, int *xs, int *ys, int max) {
	int n = 0;
	#define ADD(k, x, y) do { if (n < max) { keys[n] = (k); xs[n] = (x); ys[n] = (y); n++; } } while (0)
	if (port == 0 && touch_down) ADD(1, touch_x, touch_y);
	if (port == 1 && mouse_rear_down) ADD(1, mouse_rear_x, mouse_rear_y);
	if (cur_down && cur_mode == port + 1) ADD(2, cur_x[port], cur_y[port]);
	if (cur_down && cur2_on && cur_mode == port + 1) ADD(3, cur2_x[port], cur2_y[port]);   /* (the second cursor: key 3) */
	if (port == 0 && flick_down) ADD(4, flick_x, flick_y);   /* (a right-stick flick's swipe: key 4) */
	if (port == 0 && twist_down) { ADD(5, twist_x[0], twist_y[0]); ADD(6, twist_x[1], twist_y[1]); }   /* (a twist's two fingers: keys 5, 6) */
	for (int i = 0; i < TP_FINGERS; i++) if (tp_down[i] && tp_port[i] == port) ADD(10 + i, tp_x[i], tp_y[i]);
	for (int i = 0; i < ST_FINGERS; i++) if (st_down[i] && st_port[i] == port) ADD(20 + i, st_x[i], st_y[i]);   /* (a touchscreen's fingers: keys 20+) */
	#undef ADD
	if (n && port >= 0 && port < 2) last_touch_key[port] = keys[0];
	return n;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int touchpad_active(void) { for (int i = 0; i < TP_FINGERS; i++) if (tp_down[i]) return 1; return 0; }
/* everything gamepad touch holds, let go: the menu opened, or the pad went away */
static void gamepad_touch_release(void) {
	motion_pause();   /* (and the gyro: the next sample starts a fresh interval) */
	for (int i = 0; i < TP_FINGERS; i++) tp_down[i] = 0;
	cur_down = 0; cur_mode = 0; lb_held = rb_held = 0; ltrig_on = rtrig_on = 0; ptrig[0] = ptrig[1] = 0;
	cur2_on = 0; cur_used_l = cur_used_r = 0; cur_cross = cur_trig[0] = cur_trig[1] = 0; trig_touch[0] = trig_touch[1] = 0;
	twist_pad = 0; twist_down = twist_drag;
	mouse_rear_down = 0;
	for (int i = 0; i < ST_FINGERS; i++) st_down[i] = 0;   /* (and the screen's fingers: the menu takes the screen) */
}
/* A touchscreen (SDL's direct touch devices: a laptop's or a handheld's screen; a trackpad's fingers stay the mouse, unless
 * RECOMP_TOUCH_ANY=1, which takes any touch device's, a Mac trackpad's included, as the screen's: a multi-touch test without a
 * touchscreen). Each finger is a contact of its own on the front panel where it lands on the picture (one on the bars touches
 * nothing), held as it moves (clamped to the picture's edge), the rear panel's instead when RB is held as it lands, as the
 * touchpad's are. Six at once, the front panel's own limit. */
static int touch_any(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_TOUCH_ANY") != 0; return v; }
#define SCRIPT_TOUCH_ID ((SDL_TouchID)0x6e746976)   /* finger_script's device */
/* RECOMP_FINGER_SCRIPT="frame:down|move|up:finger:x,y;..." (x, y 0-1 across the window): those finger events pushed into SDL's
 * queue at those frames, a touchscreen of their own -- screen_finger's test without one (the windowed loop: a window is needed) */
static void finger_script(void) {
	static const char *p = (const char *)1; if (p == (const char *)1) p = getenv("RECOMP_FINGER_SCRIPT");
	extern uint64_t display_frames(void);
	while (p && *p) {
		unsigned long long f; char kind[8]; unsigned id; float x = 0, y = 0;
		const int n = sscanf(p, "%llu:%7[a-z]:%u:%f,%f", &f, kind, &id, &x, &y);
		if (n < 3) { p = 0; break; }
		if (display_frames() < f) break;
		SDL_Event ev; SDL_zero(ev);
		ev.type = !strcmp(kind, "down") ? SDL_EVENT_FINGER_DOWN : !strcmp(kind, "up") ? SDL_EVENT_FINGER_UP : SDL_EVENT_FINGER_MOTION;
		ev.tfinger.touchID = SCRIPT_TOUCH_ID; ev.tfinger.fingerID = (SDL_FingerID)id + 1; ev.tfinger.x = x; ev.tfinger.y = y; ev.tfinger.pressure = 1;
		SDL_PushEvent(&ev);
		fprintf(stderr, "    finger script: frame %llu: %s finger %u at %.3f,%.3f\n", (unsigned long long)display_frames(), kind, id, x, y);
		p = strchr(p, ';'); if (p) p++;
	}
}
static void screen_finger(const SDL_Event *e) {
	if (!touch_any() && e->tfinger.touchID != SCRIPT_TOUCH_ID && SDL_GetTouchDeviceType(e->tfinger.touchID) != SDL_TOUCH_DEVICE_DIRECT) return;
	int w = 0, h = 0; SDL_GetWindowSize(win, &w, &h);
	const float mx = e->tfinger.x * (float)w, my = e->tfinger.y * (float)h;   /* (SDL's finger positions: 0-1 across the window) */
	int k = -1, free_k = -1;
	for (int i = 0; i < ST_FINGERS; i++) { if (st_down[i] && st_id[i] == e->tfinger.fingerID) k = i; else if (!st_down[i] && free_k < 0) free_k = i; }
	if (e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) { if (k >= 0) st_down[k] = 0; return; }
	int tx, ty;
	if (k < 0) {   /* a new finger (a motion of one that landed on the bars is ignored) */
		if (e->type != SDL_EVENT_FINGER_DOWN || free_k < 0 || !mouse_to_touch(mx, my, 0, &tx, &ty)) return;
		k = free_k; st_id[k] = e->tfinger.fingerID; st_port[k] = rb_held;
	} else if (!mouse_to_touch(mx, my, 1, &tx, &ty)) return;
	st_x[k] = tx; st_y[k] = st_port[k] ? clampi(ty, REAR_Y0, REAR_Y1) : ty;
	st_down[k] = 1;
}

int window_active(void) { return active; }

static SDL_GPUDevice *gpu;
void *window_gpu_device(void) { return gpu; }   /* for a future gfx_gpu.c */
void *window_handle(void) { return win; }

/* ---- audio: an SDL audio stream per Vita output port, bound to the default playback device.
 * Output queues the samples and blocks while more than a few buffers wait, so the game is paced
 * by the sound card as on the hardware */
static SDL_AudioStream *streams[8];
static uint32_t afreq[8], achan[8];
static int nstreams;
int audio_open(uint32_t freq, uint32_t channels, uint32_t samples) {
	(void)samples;
	if (!active || nstreams >= 8) return 0;
	if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return 0;
	SDL_AudioSpec want = {SDL_AUDIO_S16, (int)channels, (int)freq};
	/* the device's own buffer, in sample frames (before the first stream opens it): small, so what the
	 * game hears as "played" is close to what the speakers play (RECOMP_AUDIO_FRAMES overrides) */
	if (!nstreams) SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, getenv("RECOMP_AUDIO_FRAMES") ? getenv("RECOMP_AUDIO_FRAMES") : "256");
	SDL_AudioStream *s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, 0, 0);
	if (!s) return 0;
	SDL_ResumeAudioStreamDevice(s);
	streams[nstreams] = s; afreq[nstreams] = freq; achan[nstreams] = channels;
	return ++nstreams;   /* 1-based handle; 0 means "failed to open" to callers */
}
/* ---- automatic A/V sync: the audio held back so that sound and picture of one moment of game time come
 * out together, from measured terms (no per-game or per-frame-rate tuning):
 *   picture: the game's update -> its flip (about one flip interval, display.c) + the flip -> this
 *            window's submit (measured in the present loop) + the compositor and scan-out (1.5 display
 *            refreshes, the one estimated term)
 *   sound:   each port's own queue ahead of the new buffer + the device buffer (measured per buffer)
 * delay = picture - sound + a small bias toward late sound (people notice sound early ~3x sooner than
 * late; RECOMP_AV_BIAS_MS, default 10) + "audio_sync_trim_ms" (-100..100, settings.ini only, per game). Uncharted's cutscene dialogue ran ~47 ms ahead of the mouths with
 * none, and the right fixed value moved with the frame rate (2026-10-09). */
static volatile float av_video_ms = 25.0f;   /* flip -> submit + compositor, averaged (the present loop) */
static float av_audio_ms[8] = {10, 10, 10, 10, 10, 10, 10, 10};
static int av_applied[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
int window_audio_auto_delay_ms(void) { return av_applied[0] < 0 ? 0 : av_applied[0]; }
static int av_auto_delay(int port) {
	extern float display_frame_period_ms(void);
	static float bias = -1; if (bias < 0) bias = getenv("RECOMP_AV_BIAS_MS") ? (float)atof(getenv("RECOMP_AV_BIAS_MS")) : 10.0f;
	float trim = (float)settings_int("audio_sync_trim_ms", 0);   /* settings.ini's fine-tune: the constant part no measurement covers (compositor, speakers, a game's pipeline depth) */
	float target = display_frame_period_ms() + av_video_ms - av_audio_ms[port] + bias + trim;
	if (target < 0) target = 0; if (target > 300) target = 300;
	static float last_trim[8] = {-1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f};
	int moved = trim != last_trim[port]; last_trim[port] = trim;   /* the fine-tune moved: apply it at once, whatever the size */
	if (moved || av_applied[port] < 0 || target - (float)av_applied[port] > 5 || (float)av_applied[port] - target > 5) av_applied[port] = (int)(target + 0.5f);   /* else 5 ms hysteresis: each change is a small skip */
	return av_applied[port];
}
void audio_output(int dev, const void *buf, uint32_t bytes) {
	if (dev < 1 || dev > nstreams) return;
	SDL_AudioStream *s = streams[dev - 1];
	/* the overlay is open, or the menu bar's Game > Pause is on: the game is paused (display.c's vblank
	 * wait parks it; see that file's comment), but a buffer already queued before the pause would
	 * otherwise keep playing out -- drop this one and clear what's left so the pause is silent at
	 * once, not a fraction of a second late */
	if (overlay_game_paused()) {
		SDL_ClearAudioStream(s);
		/* and the game's audio thread waits the pause out with the rest of the game. Returned to at once, it ran its
		 * sound system flat out for as long as the menu stayed open -- every voice and stream played on to its end
		 * unheard (Killzone: three times real time over a session with menu visits; after them its streamed ambience
		 * and dialogue had run dry or out of step) */
		int b = sched_block_begin();
		while (overlay_game_paused()) SDL_Delay(5);
		if (b) sched_block_end();
		return;
	}
	/* metrics.c's audio stats: a port that already held less than one buffer's worth when this
	 * one arrives was about to run (or just ran) dry -- the best this path can tell about an
	 * underrun without deeper SDL hooks, counted cumulatively across the run */
	static uint32_t underruns;
	if (SDL_GetAudioStreamQueued(s) < (int)bytes) underruns++;
	/* the master volume (settings "volume" 0..1, the menu's slider; RECOMP_VOLUME wins): the port's
	 * gain, set when it changes */
	static float applied[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
	float vol = getenv("RECOMP_VOLUME") ? (float)atof(getenv("RECOMP_VOLUME")) : (float)settings_float("volume", 1.0);
	if (vol < 0) vol = 0; if (vol > 1) vol = 1;
	if (vol != applied[dev - 1]) { SDL_SetAudioStreamGain(s, vol); applied[dev - 1] = vol; }
	/* the audio delay: av_auto_delay's measured A/V sync (plus settings.ini's "audio_sync_trim_ms" fine-tune),
	 * that much audio held in the port's queue beyond the pacing depth, so everything the game hands over
	 * reaches the speakers that much later while the game's own timing (the buffers it has handed over)
	 * is unchanged. The queue is topped up with silence when it holds less (the delay rose, or the port
	 * ran dry); a lower delay drains through the pacing wait below. (A fixed delay, settings
	 * "audio_delay_ms" / RECOMP_AUDIO_DELAY_MS, was removed 2026-10-15: the dialogue running ahead of the
	 * lips it was set to hide was NGS reporting a stereo voice's samples at half the hardware's count --
	 * ngs.c h_voice_state -- and a fixed number went stale with every fix.) */
	int extra = 0;
	{	int frame = (int)achan[dev - 1] * 2, q = SDL_GetAudioStreamQueued(s);
		static int dev_frames = -1; if (dev_frames < 0) dev_frames = getenv("RECOMP_AUDIO_FRAMES") ? atoi(getenv("RECOMP_AUDIO_FRAMES")) : 256;
		{	/* this port's own latency for the new buffer, our delay excluded (av_auto_delay) */
			float bpms = (float)afreq[dev - 1] / 1000.0f * (float)frame, held = av_applied[dev - 1] > 0 ? (float)av_applied[dev - 1] * bpms : 0;
			float lat = ((float)q - held) / bpms; if (lat < 0) lat = 0;
			lat += (float)dev_frames * 1000.0f / (float)afreq[dev - 1];
			av_audio_ms[dev - 1] += (lat - av_audio_ms[dev - 1]) * 0.05f;
		}
		int dms = av_auto_delay(dev - 1);
		extra = (int)((uint64_t)dms * afreq[dev - 1] / 1000) * frame;
		if (extra && q < extra) { static uint8_t zero[8192]; for (int pad = (extra - q) / frame * frame; pad > 0;) { int n = pad < (int)sizeof zero ? pad : (int)sizeof zero; SDL_PutAudioStreamData(s, zero, n); pad -= n; } }
	}
	SDL_PutAudioStreamData(s, buf, (int)bytes);
	{	uint32_t ms = 0;
		for (int i = 0; i < nstreams; i++) { int q = SDL_GetAudioStreamQueued(streams[i]); uint32_t bps = achan[i] * 2 * (afreq[i] / 1000); if (bps) ms += (uint32_t)q / bps; }
		metrics_set_audio(ms, (uint32_t)nstreams, underruns);
	}
	/* pacing: the call returns once at most `depth` buffers wait behind the one playing -- the Vita's
	 * output is double-buffered (one pending), and a game times its cutscenes by the buffers it has
	 * handed over. Three (until 2026-10-09) put Uncharted's dialogue port (1024-sample buffers) up to
	 * ~85 ms behind the lips. settings "audio_buffers" 1..4 (default 1; RECOMP_AUDIO_QUEUE wins): more if
	 * a slow machine crackles. The pacing sleep gives the guest thread's run slot up (RECOMP_SCHED). */
	static int depth = -1;
	if (depth < 0) { depth = getenv("RECOMP_AUDIO_QUEUE") ? atoi(getenv("RECOMP_AUDIO_QUEUE")) : settings_int("audio_buffers", 1); if (depth < 1) depth = 1; if (depth > 4) depth = 4; }
	if (SDL_GetAudioStreamQueued(s) > depth * (int)bytes + extra) {
		int b = sched_block_begin();
		while (SDL_GetAudioStreamQueued(s) > depth * (int)bytes + extra) SDL_Delay(1);
		if (b) sched_block_end();
	}
}

/* the GPU backend's frame: blitted (on the game thread) into one of three RGBA8 textures here, the
 * present loop shows the latest; three so the one being written is never the one being shown */
static SDL_GPUTexture *gtex[3]; static uint32_t gtw[3], gth[3]; static int gidx = -1, gshow = -1, gnew;
static uint32_t gtl[3];   /* each one's mip levels (window_present_gpu: more than one when it is over twice the window's size) */
void window_pixel_size(uint32_t *w, uint32_t *h) { *w = win_pw; *h = win_ph; }
static uint64_t gready_us[3];   /* when each was handed over (the game's flip, as the GPU backend presents it): RECOMP_LATENCY_LOG */
static int frames_in_flight = 2;
/* the overlay needs real pixels to blend against, and the GPU-rendered path's frame never otherwise
 * touches the CPU; these two transfer buffers (one each direction -- SDL_GPU's usage hint may pick
 * different memory for them, so one buffer doesn't stand in for both) round-trip it through the
 * composite only while the overlay is actually on. ensure_osd_rt resizes both together. */
static SDL_GPUTransferBuffer *osd_dl, *osd_ul; static uint32_t osd_rtw, osd_rth;
static int ensure_osd_rt(uint32_t w, uint32_t h) {
	if (osd_dl && osd_rtw == w && osd_rth == h) return 1;
	if (osd_dl) SDL_ReleaseGPUTransferBuffer(gpu, osd_dl);
	if (osd_ul) SDL_ReleaseGPUTransferBuffer(gpu, osd_ul);
	SDL_GPUTransferBufferCreateInfo dci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = 4 * w * h };
	SDL_GPUTransferBufferCreateInfo uci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = 4 * w * h };
	osd_dl = SDL_CreateGPUTransferBuffer(gpu, &dci); osd_ul = SDL_CreateGPUTransferBuffer(gpu, &uci);
	osd_rtw = w; osd_rth = h;
	return osd_dl && osd_ul;
}
/* the overlay onto a GPU-rendered frame: download `src`, composite on the CPU, upload into `dst`
 * (1 done, 0 if any step failed: the caller blits the plain frame instead) */
static int osd_round_trip(SDL_GPUTexture *src, SDL_GPUTexture *dst, uint32_t w, uint32_t h) {
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
	if (!cmd) return 0;
	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureRegion sr = { .texture = src, .w = w, .h = h, .d = 1 };
	SDL_GPUTextureTransferInfo dt = { .transfer_buffer = osd_dl, .pixels_per_row = w, .rows_per_layer = h };
	SDL_DownloadFromGPUTexture(cp, &sr, &dt);
	SDL_EndGPUCopyPass(cp);
	SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
	if (fence) { SDL_WaitForGPUFences(gpu, true, &fence, 1); SDL_ReleaseGPUFence(gpu, fence); }
	void *dl = SDL_MapGPUTransferBuffer(gpu, osd_dl, false);
	void *ul = dl ? SDL_MapGPUTransferBuffer(gpu, osd_ul, true) : 0;
	if (ul) { memcpy(ul, dl, 4u * w * h); metrics_osd_composite((uint8_t *)ul, w, w, h); SDL_UnmapGPUTransferBuffer(gpu, osd_ul); }
	if (dl) SDL_UnmapGPUTransferBuffer(gpu, osd_dl);
	SDL_GPUCommandBuffer *cmd2 = ul ? SDL_AcquireGPUCommandBuffer(gpu) : 0;
	if (!cmd2) return 0;
	SDL_GPUCopyPass *up = SDL_BeginGPUCopyPass(cmd2);
	SDL_GPUTextureTransferInfo si = { .transfer_buffer = osd_ul, .pixels_per_row = w, .rows_per_layer = h };
	SDL_GPUTextureRegion dr = { .texture = dst, .w = w, .h = h, .d = 1 };
	SDL_UploadToGPUTexture(up, &si, &dr, true);
	SDL_EndGPUCopyPass(up);
	SDL_SubmitGPUCommandBuffer(cmd2);
	return 1;
}
static int osd_cpu(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_OSD_CPU") != 0; return v; }   /* RECOMP_OSD_CPU=1: the overlay composited into the frame's pixels on the CPU, as before the ImGui HUD */
void window_present_gpu(void *src, uint32_t w, uint32_t h, int raw) {
	if (!active || !gpu || !w || !h || w > 16384 || h > 16384) return;
	pthread_mutex_lock(&lock);
	int n = (gidx + 1) % 3; if (n == gshow) n = (n + 1) % 3;
	/* a frame over twice the window's size (render scale 4x and up on most displays) carries mip levels,
	 * so the present loop can blit from the level just above the window's size: a linear blit across a
	 * bigger ratio samples only some of the pixels -- the extra resolution came out as shimmer instead of
	 * supersampling */
	uint32_t levels = 1;
	if (win_pw && win_ph) while (levels < 6 && (w >> levels) >= win_pw && (h >> levels) >= win_ph) levels++;
	if (gtex[n] && (gtw[n] != w || gth[n] != h || gtl[n] != levels)) { SDL_ReleaseGPUTexture(gpu, gtex[n]); gtex[n] = 0; }
	if (!gtex[n]) {
		SDL_GPUTextureCreateInfo ci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
			.width = w, .height = h, .layer_count_or_depth = 1, .num_levels = levels, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
		gtex[n] = SDL_CreateGPUTexture(gpu, &ci); gtw[n] = w; gth[n] = h; gtl[n] = levels;
	}
	/* the overlay is off here by default (RECOMP_OSD=0, metrics.c's own comment says why): without
	 * a blending shader of its own in gfx_gpu.c (not written yet), the only way to blend a
	 * translucent overlay onto a frame that's already a GPU texture is to round-trip it through the
	 * CPU -- download `src`, composite, re-upload into gtex[n] below -- with a GPU fence wait to
	 * make the download safe to read. That's a real per-frame stall, only worth paying when someone
	 * actually asked to see the overlay (F1 or RECOMP_OSD=1|2); otherwise this falls through to the
	 * plain same-GPU blit at the bottom, unchanged from before this feature existed. */
	/* ... which is why, with the ImGui overlay up (overlay_draws_hud: every windowed run), the lines are
	 * now drawn by ImGui into the swapchain in the present loop below, beside the touch markers
	 * (overlay_marks), and this round trip isn't taken: the overlay on, the frame no longer costs a GPU
	 * sync. Our test setup had osd=2 on at render scale 2: 8 MB down, 8 MB up and the frame's whole
	 * GPU time waited for, every frame. RECOMP_OSD_CPU=1: the composite below, as before. */
	int osd = gtex[n] && metrics_osd_level() > 0 && !(overlay_draws_hud() && !osd_cpu()) && ensure_osd_rt(w, h);
	if (osd) {
		/* the round trip waits on the GPU: not under the lock the present loop takes every
		 * iteration. Only this thread (gfx_gpu_present, under the renderer's lock) uses the
		 * transfer buffers, and slot n can't be shown until it's published below. */
		SDL_GPUTexture *dst = gtex[n];
		pthread_mutex_unlock(&lock);
		int done = osd_round_trip(src, dst, w, h);
		pthread_mutex_lock(&lock);
		if (done) { gidx = n; gnew = 1; new_frame = 0; gready_us[n] = now_us(); } else osd = 0;
	}
	if (!osd) {
		SDL_GPUCommandBuffer *cmd = gtex[n] ? SDL_AcquireGPUCommandBuffer(gpu) : 0;
		if (cmd && raw) {
			/* raw: an sRGB frame (Killzone's). A blit would decode it on the way into this plain texture; a copy moves
			 * the bytes as they are, which is what the display scans out */
			SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
			SDL_GPUTextureLocation s = { .texture = src }, d = { .texture = gtex[n] };
			SDL_CopyGPUTextureToTexture(cp, &s, &d, w, h, 1, false);
			SDL_EndGPUCopyPass(cp);
		} else if (cmd) {
			SDL_GPUBlitInfo blit = { .source = { .texture = src, .w = w, .h = h }, .destination = { .texture = gtex[n], .w = w, .h = h }, .load_op = SDL_GPU_LOADOP_DONT_CARE, .filter = SDL_GPU_FILTER_NEAREST };
			SDL_BlitGPUTexture(cmd, &blit);
		}
		if (cmd) {
			if (gtl[n] > 1) SDL_GenerateMipmapsForGPUTexture(cmd, gtex[n]);
			SDL_SubmitGPUCommandBuffer(cmd);
			gidx = n; gnew = 1; new_frame = 0; gready_us[n] = now_us();
		}
	}
	pthread_mutex_unlock(&lock);
}
/* RECOMP_OVERLAY_DUMP=path.ppm (a verification aid, no window needed): the first time this frame
 * passes through here, render the in-game menu once, forced open, over a copy of it, and write the
 * result -- then let the run carry on exactly as before. It calls overlay_render_forced, never
 * overlay_open/overlay_is_open, so nothing else that keys off the overlay being open (display.c's
 * pause gate, this file's own input routing and audio mute above) is ever affected by it; a run with
 * this unset pays one cached getenv and a branch, same as metrics.c's RECOMP_OSD_DUMP. Works with
 * either graphics backend: by the time window_present is called, RECOMP_GFX=gpu has already synced
 * its rendered frame into this same guest memory (gfx_gpu.c's present, for exactly this reason). */
static void overlay_dump_maybe(uint32_t base, uint32_t w, uint32_t h, uint32_t pitch) {
	static int done = -1;
	if (done < 0) { const char *e = getenv("RECOMP_OVERLAY_DUMP"); done = e ? 0 : 1; }
	if (done) return;
	/* vitarecomp test sets RECOMP_SHOT_LAST to the frame it's capturing (TEST_FRAMES) and exits the
	 * process right after writing that shot -- one window_present call later than this one, which as
	 * a result never happens (gfx_soft.c's gfx_soft_present calls shot() then checks that exit
	 * before it ever reaches window_present). Wait for the LAST window_present call the run will
	 * ever make instead: display_frames() reaches RECOMP_SHOT_LAST exactly once, on the call right
	 * before that exit -- as close to the shot frame as this hook can structurally get, and the same
	 * scene. Standalone (no RECOMP_SHOT_LAST), fire on the first frame there is. */
	extern uint64_t display_frames(void);
	static long target = -2; if (target == -2) { const char *e = getenv("RECOMP_SHOT_LAST"); target = e ? atol(e) : -1; }
	if (target >= 0 && (long)display_frames() != target) return;
	done = 1;
	const char *path = getenv("RECOMP_OVERLAY_DUMP");
	if (!gpu) { fprintf(stderr, "RECOMP_OVERLAY_DUMP: no GPU device, skipping\n"); return; }
	if (!overlay_init(gpu, 0, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)) { fprintf(stderr, "RECOMP_OVERLAY_DUMP: overlay_init failed\n"); return; }
	uint8_t *tmp = malloc(4u * w * h);
	if (!tmp) return;
	for (uint32_t y = 0; y < h; y++) memcpy(tmp + 4u * w * y, G(base + 4 * pitch * y), 4u * w);
	SDL_GPUTextureCreateInfo ci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
		.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
		.width = w, .height = h, .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
	SDL_GPUTexture *tex = SDL_CreateGPUTexture(gpu, &ci);
	SDL_GPUTransferBufferCreateInfo uci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = 4u * w * h };
	SDL_GPUTransferBuffer *up = SDL_CreateGPUTransferBuffer(gpu, &uci);
	SDL_GPUTransferBufferCreateInfo dci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = 4u * w * h };
	SDL_GPUTransferBuffer *down = SDL_CreateGPUTransferBuffer(gpu, &dci);
	if (!tex || !up || !down) {
		fprintf(stderr, "RECOMP_OVERLAY_DUMP: out of GPU memory\n"); free(tmp);
		if (tex) SDL_ReleaseGPUTexture(gpu, tex);
		if (up) SDL_ReleaseGPUTransferBuffer(gpu, up);
		if (down) SDL_ReleaseGPUTransferBuffer(gpu, down);
		return;
	}
	/* RECOMP_OVERLAY_DUMP_UI: what to show (overlay.h's overlay_render_forced; default the full menu), one
	 * dump per comma-separated entry, each over a fresh copy of the same frame, named f_<entry>.ppm when
	 * there are several */
	char uis[256]; snprintf(uis, sizeof uis, "%s", getenv("RECOMP_OVERLAY_DUMP_UI") ? getenv("RECOMP_OVERLAY_DUMP_UI") : "menu");
	int several = strchr(uis, ',') != 0;
	for (char *ui = strtok(uis, ","); ui; ui = strtok(0, ",")) {
		void *dst = SDL_MapGPUTransferBuffer(gpu, up, true);
		if (dst) { memcpy(dst, tmp, 4u * w * h); SDL_UnmapGPUTransferBuffer(gpu, up); }
		/* three command buffers in order: the frame's upload; the overlay's own (overlay_render_forced
		 * submits one per frame it draws -- the ImGui backend's buffers are per frame; the closed-state
		 * modes draw the HUD, RECOMP_OSD's lines, under the bar in that same frame, as the live path does);
		 * the download. The device runs them in submission order. */
		SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
		SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
		SDL_GPUTextureTransferInfo si = { .transfer_buffer = up, .pixels_per_row = w, .rows_per_layer = h };
		SDL_GPUTextureRegion dr = { .texture = tex, .w = w, .h = h, .d = 1 };
		SDL_UploadToGPUTexture(cp, &si, &dr, true);
		SDL_EndGPUCopyPass(cp);
		SDL_SubmitGPUCommandBuffer(cmd);
		overlay_render_forced(gpu, tex, w, h, ui);   /* draws over the uploaded frame: LOAD, not CLEAR */
		cmd = SDL_AcquireGPUCommandBuffer(gpu);
		SDL_GPUCopyPass *cp2 = SDL_BeginGPUCopyPass(cmd);
		SDL_GPUTextureRegion sr = { .texture = tex, .w = w, .h = h, .d = 1 };
		SDL_GPUTextureTransferInfo dt = { .transfer_buffer = down, .pixels_per_row = w, .rows_per_layer = h };
		SDL_DownloadFromGPUTexture(cp2, &sr, &dt);
		SDL_EndGPUCopyPass(cp2);
		SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
		if (fence) { SDL_WaitForGPUFences(gpu, true, &fence, 1); SDL_ReleaseGPUFence(gpu, fence); }
		char out[600]; const char *dot = strrchr(path, '.');
		if (several) snprintf(out, sizeof out, "%.*s_%s%s", (int)(dot ? dot - path : (long)strlen(path)), path, ui, dot ? dot : "");
		else snprintf(out, sizeof out, "%s", path);
		const uint8_t *px = SDL_MapGPUTransferBuffer(gpu, down, false);
		FILE *f = px ? fopen(out, "wb") : 0;
		if (f) {
			fprintf(f, "P6\n%u %u\n255\n", w, h);
			for (uint32_t i = 0; i < w * h; i++) fwrite(px + 4 * i, 1, 3, f);
			fclose(f);
			fprintf(stderr, "    overlay: wrote the %ux%u dump (%s) to %s\n", w, h, ui, out);
		} else fprintf(stderr, "RECOMP_OVERLAY_DUMP: cannot write %s (%s)\n", out, SDL_GetError());
		if (px) SDL_UnmapGPUTransferBuffer(gpu, down);
	}
	free(tmp);
	SDL_ReleaseGPUTransferBuffer(gpu, down); SDL_ReleaseGPUTransferBuffer(gpu, up); SDL_ReleaseGPUTexture(gpu, tex);
}
void window_present(uint32_t base, uint32_t w, uint32_t h, uint32_t pitch) {
	if (!w || !h || w > 4096 || h > 4096) return;
	overlay_dump_maybe(base, w, h, pitch);
	if (!active) return;
	pthread_mutex_lock(&lock);
	gnew = 0; gshow = -1;   /* a software frame supersedes any GPU one */
	if (!frame || fw != w || fh != h) { free(frame); frame = malloc(4 * w * h); fw = w; fh = h; }
	for (uint32_t y = 0; y < h; y++) memcpy(frame + 4 * w * y, G(base + 4 * pitch * y), 4 * w);
	new_frame = 1;
	pthread_mutex_unlock(&lock);
}

void window_input(uint32_t *b, uint8_t s[4]) {
	*b = buttons & ~(L3 | R3 | PAD_L2 | PAD_R2);
	for (int i = 0; i < 4; i++) s[i] = sticks[i];
	/* the sticks are the touch cursor while a bumper is held -- unless the touchpad is doing the touching
	 * (RB held only to put its fingers on the rear panel: the sticks stay the game's) */
	if (cur_mode && !touchpad_active()) s[0] = s[1] = s[2] = s[3] = 128;
	if (gesture_wanted()) s[2] = s[3] = 128;   /* a gesture window: the right stick flicks a swipe (window_touch_gesture_hint) */
	if (twist_wanted() && !kbm_on) s[0] = s[1] = s[2] = s[3] = 128;   /* a twist: the sticks are its fingers (window_touch_twist_hint) */
	if (tilt_section && (kbm_on || !pad_gyro)) s[0] = 128;   /* a tilt section: left/right is the tilt (A / D, a gyro-less pad's stick), not a step or a steer */
}
/* a PS TV's pad (display.c: the "2" reads of port 1): the handheld's L and R are its L1 and R1, as a PS TV gives them to a
 * game that never asks; the other shoulder pair is its L2 and R2, and the stick clicks are there */
void window_input_pad2(uint32_t *b, uint8_t s[4]) {
	const uint32_t v = buttons;
	window_input(b, s);
	*b = (*b & ~(LTRIG | RTRIG)) | (v & LTRIG ? L1 : 0) | (v & RTRIG ? R1 : 0) | (v & PAD_L2 ? LTRIG : 0) | (v & PAD_R2 ? RTRIG : 0) | (v & (L3 | R3));
}

static uint32_t pad_bit(int b) {
	switch (b) {
	case SDL_GAMEPAD_BUTTON_SOUTH: return CROSS; case SDL_GAMEPAD_BUTTON_EAST: return CIRCLE;
	case SDL_GAMEPAD_BUTTON_WEST: return SQUARE; case SDL_GAMEPAD_BUTTON_NORTH: return TRIANGLE;
	case SDL_GAMEPAD_BUTTON_START: return START; case SDL_GAMEPAD_BUTTON_BACK: return SELECT;
	case SDL_GAMEPAD_BUTTON_DPAD_UP: return UP; case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return DOWN;
	case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return LEFT; case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return RIGHT;
	case SDL_GAMEPAD_BUTTON_LEFT_STICK: return L3; case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return R3;
	default: return 0;
	}
}
/* The two shoulder pairs, whichever physical pair each is ("shoulder_buttons": the triggers by default are pad_main).
 * pad_main: the game's L and R; with the touch cursor up (the other pair held) a press is the cursor's touch, like
 * Cross (two cursors: both go down), and a press that touched sends no L/R until it is let go, whatever the other pair
 * does meanwhile. pad_second: held, the touch cursor (left: the front panel, right: the rear; right wins when both are
 * held); a game playing a PS TV has no touch, and there it is the pad's L2 and R2. k: 0 left, 1 right. */
static void pad_main(SDL_Gamepad *pad, int k, int dn) {
	(void)pad;
	int *on = k ? &rtrig_on : &ltrig_on; const uint32_t bit = k ? RTRIG : LTRIG;
	if (dn && !*on) { *on = 1; if (cur_mode) { cur_trig[k] = trig_touch[k] = 1; } else buttons |= bit; }
	else if (!dn && *on) { *on = 0; buttons &= ~bit; cur_trig[k] = trig_touch[k] = 0; }
	cur_down = cur_mode && (cur_cross || cur_trig[0] || cur_trig[1]);
}
static void pad_second(SDL_Gamepad *pad, int k, int dn) {
	if (display_pstv_active()) { const uint32_t bit = k ? PAD_R2 : PAD_L2; if (dn) buttons |= bit; else buttons &= ~bit; return; }
	if (k) rb_held = dn; else lb_held = dn;
	int m = rb_held ? 2 : lb_held ? 1 : 0;
	if (m != cur_mode) { cur_cross = cur_trig[0] = cur_trig[1] = 0; cur2_on = 0; cur_used_l = cur_used_r = 0; }   /* a cursor touch (and the split) ends with its mode */
	if (m && !cur_mode && pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH)) { cur_cross = 1; buttons &= ~CROSS; }   /* Cross already held: it's the touch now, not Cross */
	if (m && !cur_mode) for (int j = 0; j < 2; j++) if (j ? rtrig_on : ltrig_on) { cur_trig[j] = trig_touch[j] = 1; buttons &= ~(j ? RTRIG : LTRIG); }   /* L or R already held: the touch too */
	cur_mode = m;
	cur_down = m && (cur_cross || cur_trig[0] || cur_trig[1]);
}
/* keyboard or mouse tried to play the game (a bound key, the mouse moved or clicked) in a PS TV layout, where they don't:
 * the notice why, once a run */
static int kbm_pstv_said;
static void kbm_used(void) {
	if (kbm_pstv_said || !display_pstv_active()) return;
	kbm_pstv_said = 1;
	overlay_toast_titled("Controller layout", "PS TV buttons are controller only: keyboard and mouse don't play the game in them. To play with keyboard and mouse: [F10] > Game > Controller layout > Touch screen (from the next start)", 10.0);
}

typedef struct { void *(*fn)(void *); void *arg; } Game;
static void *game_main(void *p) { Game *g = p; g->fn(g->arg); return 0; }

/* (re)create the GPU-side texture + transfer buffer that the CPU frame is uploaded into, when its
 * size changes; returns 0 if the GPU texture isn't ready (e.g. device/window not yet claimed) */
static SDL_GPUTexture *src_tex;
static SDL_GPUTransferBuffer *xfer;
static uint32_t stw, sth;
static int ensure_src_texture(uint32_t w, uint32_t h) {
	if (src_tex && stw == w && sth == h) return 1;
	if (src_tex) SDL_ReleaseGPUTexture(gpu, src_tex);
	if (xfer) SDL_ReleaseGPUTransferBuffer(gpu, xfer);
	SDL_GPUTextureCreateInfo tci = {
		.type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
		.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = w, .height = h, .layer_count_or_depth = 1, .num_levels = 1,
		.sample_count = SDL_GPU_SAMPLECOUNT_1,
	};
	src_tex = SDL_CreateGPUTexture(gpu, &tci);
	SDL_GPUTransferBufferCreateInfo xci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = 4 * w * h };
	xfer = src_tex ? SDL_CreateGPUTransferBuffer(gpu, &xci) : 0;
	stw = w; sth = h;
	return src_tex && xfer;
}

/* run fn(arg) as the game; with a window, on its own thread while this one shows frames */
void window_run(void *(*fn)(void *), void *arg) {
	const char *hl = getenv("RECOMP_HEADLESS");
	if ((hl && *hl && *hl != '0') || !SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
		if (!(hl && *hl && *hl != '0')) fprintf(stderr, "no window (%s): running headless\n", SDL_GetError());
		/* RECOMP_GFX=gpu needs an SDL_GPUDevice even here: gfx_gpu.c never presents to a window
		 * swapchain (window.c's own present loop below does that, for the software backend's
		 * frame, and isn't reached in headless mode) -- it only renders into its own offscreen
		 * colour/depth textures and reads them back for RECOMP_SHOT/window_present, neither of
		 * which needs a window. SDL_GPU itself still needs SDL_INIT_VIDEO initialised. */
		extern void *gfx_gpu_create_device(void);
		if (SDL_Init(SDL_INIT_VIDEO)) gpu = gfx_gpu_create_device();
		fn(arg);
		return;
	}
	int scale = getenv("RECOMP_SCALE") ? atoi(getenv("RECOMP_SCALE")) : 1;
	if (scale < 1) scale = 1;
	{ const char *name = getenv("RECOMP_GAME_NAME"); win = SDL_CreateWindow(name && *name ? name : "Project Nativ", 960 * scale, 544 * scale, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY); }
	{	/* the app icon (assets/brand): the install's, as vitarecomp run names it (an AppImage's moves on every start), else
		 * the one the build saw */
		const char *ip = getenv("RECOMP_ICON");
#ifdef RECOMP_ICON
		if (!ip) ip = RECOMP_ICON;
#endif
		SDL_Surface *icon = ip ? SDL_LoadPNG(ip) : NULL; if (icon) { SDL_SetWindowIcon(win, icon); SDL_DestroySurface(icon); }
	}
	SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	{ extern void *gfx_gpu_create_device(void); gpu = gfx_gpu_create_device(); }
	if (gpu) SDL_ClaimWindowForGPUDevice(gpu, win);
	if (gpu) {
		/* the swapchain's present mode. On Vulkan, mailbox where the display has it: with vsync (fifo) this loop's submit
		 * waits for the swapchain image on the GPU's one queue, and every scene the game submits after it waits out the
		 * refresh too. Killzone's CPU reads of its exposure target waited ~6 ms each: 40 fps windowed against 66 headless
		 * (RTX laptop, 2026-10-26), 47 with mailbox. Mailbox doesn't block, so this loop paces itself (self_paced, below).
		 * Metal blocks the CPU in nextDrawable instead, not the queue: vsync there. RECOMP_PRESENT_MODE=fifo|mailbox|immediate */
		const char *pe = getenv("RECOMP_PRESENT_MODE"), *dv = SDL_GetGPUDeviceDriver(gpu);
		/* (with mailbox, Killzone at 3x and 21:9 lost the device in about half its runs: the FRAGCOLOR copy, a blit since,
		 * gfx_gpu.c fetch_blit) */
		SDL_GPUPresentMode pm = pe ? (!strcmp(pe, "immediate") ? SDL_GPU_PRESENTMODE_IMMEDIATE : !strcmp(pe, "mailbox") ? SDL_GPU_PRESENTMODE_MAILBOX : SDL_GPU_PRESENTMODE_VSYNC)
		                           : dv && !strcmp(dv, "vulkan") ? SDL_GPU_PRESENTMODE_MAILBOX : SDL_GPU_PRESENTMODE_VSYNC;
		if (pm != SDL_GPU_PRESENTMODE_VSYNC && SDL_WindowSupportsGPUPresentMode(gpu, win, pm) && SDL_SetGPUSwapchainParameters(gpu, win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, pm)) self_paced = 1;
		fprintf(stderr, "    window: present mode %s\n", !self_paced ? "vsync" : pm == SDL_GPU_PRESENTMODE_MAILBOX ? "mailbox" : "immediate");
	}
	/* frames the swapchain may have queued for presentation (SDL's default 2): each one is a refresh
	 * between a game's flip and the screen. 1 here -- with the frame picked after the drawable wait
	 * (the present loop) -- is the Vita's single flip; games time their audio against that (Uncharted's
	 * cutscene dialogue ran ~47 ms ahead of the mouths with 2 and the frame picked first, 2026-10-09).
	 * settings "frames_in_flight" 1..3 (RECOMP_FRAMES_IN_FLIGHT wins) */
	if (gpu) {
		frames_in_flight = getenv("RECOMP_FRAMES_IN_FLIGHT") ? atoi(getenv("RECOMP_FRAMES_IN_FLIGHT")) : settings_int("frames_in_flight", 1);
		if (frames_in_flight < 1) frames_in_flight = 1; if (frames_in_flight > 3) frames_in_flight = 3;
		if (!SDL_SetGPUAllowedFramesInFlight(gpu, (Uint32)frames_in_flight)) { fprintf(stderr, "frames in flight %d refused (%s): SDL's 2\n", frames_in_flight, SDL_GetError()); frames_in_flight = 2; }
	}
	if (!gpu) fprintf(stderr, "no GPU device (%s): presenting nothing\n", SDL_GetError());
	/* the overlay's own settings (its Display page), read once here the same way render_scale_init
	 * (gfx_gpu.c) reads "render_scale": the environment doesn't override these two, there being no
	 * scripted test that cares what the window looks like */
	{ char b[16]; settings_str("aspect", "fit", b, sizeof b);
	  aspect_mode = !strcmp(b, "stretch") ? ASPECT_STRETCH : !strcmp(b, "fill") ? ASPECT_FILL : ASPECT_FIT; }   /* window_set_aspect also saves: read it in directly instead, nothing changed yet */
	if (settings_int("fullscreen", 0)) SDL_SetWindowFullscreen(win, true);
	if (gpu) overlay_init(gpu, win, SDL_GetGPUSwapchainTextureFormat(gpu, win));
	SDL_Gamepad *pad = 0;
	int pad_back = 0, pad_start = 0, pad_combo_latch = 0;
	uint64_t last_overlay_t = 0;
	active = 1;
	Game g = {fn, arg};
	pthread_t t;
	pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
	pthread_create(&t, &at, game_main, &g);
	for (;;) {
		SDL_Event e;
		finger_script();
		while (SDL_PollEvent(&e)) {
			/* _exit, not SDL_Quit()+exit(): the game thread is still running and may be mid-call
			 * into SDL (audio_output, window_present) or guest code; tearing down SDL subsystems
			 * out from under it raced into a SIGSEGV at a null host function pointer. The OS
			 * reclaims everything on process exit; skip the unsafe graceful-shutdown step. */
			/* (a close request asks first: the quit card, overlay.cpp; a second one while it's up quits) */
			if (e.type == SDL_EVENT_QUIT && !overlay_quit_confirm()) { extern void host_summary(const char *why); extern void gfx_gpu_quit(void); host_summary("window closed"); gfx_gpu_quit(); _exit(0); }   /* the import histogram and the stubbed-import list land in the log */
			if (e.type == SDL_EVENT_QUIT) { buttons = 0; touch_down = 0; gamepad_touch_release(); continue; }
			/* fullscreen however it was entered or left -- the window's own button (macOS's green one), the OS's
			 * shortcut, the menu -- is the setting: the next game starts the same way */
			if ((e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN || e.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) && settings_int("fullscreen", 0) != (e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN)) {
				settings_set_int("fullscreen", e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN); settings_save();
			}
			/* the overlay itself: always live, open or not, so it can always be reached and always
			 * closed. Opening (or re-opening by the same gamepad combo) clears whatever buttons/touch
			 * were down so the game never sees them as still held on resume. */
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F10) { overlay_toggle(); if (overlay_is_open()) { buttons = 0; touch_down = 0; gamepad_touch_release(); } }
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_ESCAPE) { if (overlay_is_open()) overlay_close(); else overlay_bar_dismiss(); }   /* (closed: Escape hides the menu bar and its menus) */
			if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) { overlay_toggle(); if (overlay_is_open()) { buttons = 0; touch_down = 0; gamepad_touch_release(); } }
			if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) pad_back = 1;
			if (e.type == SDL_EVENT_GAMEPAD_BUTTON_UP && e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) { pad_back = 0; pad_combo_latch = 0; }
			if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_START) pad_start = 1;
			if (e.type == SDL_EVENT_GAMEPAD_BUTTON_UP && e.gbutton.button == SDL_GAMEPAD_BUTTON_START) { pad_start = 0; pad_combo_latch = 0; }
			if (pad_back && pad_start && !pad_combo_latch) { pad_combo_latch = 1; overlay_toggle(); if (overlay_is_open()) { buttons = 0; touch_down = 0; gamepad_touch_release(); } }
			/* exclusive while open: the menu gets every key/mouse/gamepad event instead of the game
			 * (which is paused anyway -- display.c) */
			if (overlay_is_open()) { overlay_process_event(&e); continue; }
			/* the save-data dialog waiting on the player (overlay_dialog_active): it has the keys and the mouse;
			 * the game is polling the dialog, not the pad */
			if (overlay_dialog_active()) { overlay_process_event(&e); continue; }
			/* closed: ImGui still sees the mouse (and the window's focus/enter/leave), so the menu bar can
			 * show on a hover at the top edge; a press or a move it has claimed (the pointer over the bar,
			 * a menu or the pull tab, as of the last frame drawn) never becomes a touch below. A release
			 * always falls through: letting a touch go is never wrong, and a click that began on the
			 * picture then dragged onto the bar ends as the game's (ImGui doesn't claim a drag that began
			 * outside it: its MouseDownOwned). Keys and the pad stay the game's -- the bar is pointer-only. */
			if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_WHEEL ||
			    e.type == SDL_EVENT_WINDOW_MOUSE_ENTER || e.type == SDL_EVENT_WINDOW_MOUSE_LEAVE || e.type == SDL_EVENT_WINDOW_FOCUS_GAINED || e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
				overlay_process_event(&e);
				if (overlay_mouse_captured() && e.type != SDL_EVENT_MOUSE_BUTTON_UP) continue;
			}
			/* a touchscreen's fingers (screen_finger). SDL also makes the first finger the mouse (which == SDL_TOUCH_MOUSEID):
			 * that stops here, after the menu bar has seen it, so a tap never fires, looks, captures the mouse or switches
			 * the input to keyboard + mouse */
			if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_MOTION || e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED) { screen_finger(&e); continue; }
			if ((e.type == SDL_EVENT_MOUSE_MOTION && e.motion.which == SDL_TOUCH_MOUSEID) || ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) && e.button.which == SDL_TOUCH_MOUSEID)) continue;
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F12) { extern void gfxcap_request(void); gfxcap_request(); fprintf(stderr, "capture requested (F12)\n"); }
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F1) metrics_osd_cycle();
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F5) {   /* a marker in the log: "that just happened" for a bug report (a doubled voice line, a glitch) */
				static int n; n++; extern uint64_t display_frames(void);
				fprintf(stderr, "user mark %d at frame %llu\n", n, (unsigned long long)display_frames());
				{ extern void gfx_gpu_history_save(int mark); gfx_gpu_history_save(n); }   /* and the frames before it (gfx_gpu.c: F5's frame history) */
				char b[48]; snprintf(b, sizeof b, "Mark %d at frame %llu", n, (unsigned long long)display_frames()); overlay_toast_titled("Marked in the log", b, 2.0);
			}
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F6) {   /* a VERIFY build's 60/30 A/B capture (verify.c sets window_f6) */
				if (window_f6) window_f6(); else fprintf(stderr, "F6: the A/B capture needs the verify build (vitarecomp build <title> verify)\n");
			}
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F9) {   /* a VERIFY build's hand-held count window (verify.c sets window_f9) */
				if (window_f9) window_f9(); else fprintf(stderr, "F9: count windows need the verify build (vitarecomp build <title> verify)\n");
			}
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F8) raw_look_toggle();   /* the experimental raw mouse look (hooks.c) */
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F4) { extern void gfx_gpu_fsr_toggle(void); gfx_gpu_fsr_toggle(); }   /* FSR 3.1 anti-aliasing off and on, live (RECOMP_FSR: gfx_gpu.c) */
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F7) { extern void camera_light_toggle(void); camera_light_toggle(); }   /* the rear camera's lamp (camera.c: Uncharted's hold-it-to-the-light puzzles) */
			if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F2) { extern void gfx_gpu_render_scale_cycle(void); gfx_gpu_render_scale_cycle(); }   /* the GPU backend's internal resolution, 1x -> 2x -> ... -> 6x, from the next frame */
			if ((e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) || e.type == SDL_EVENT_KEY_UP) {   /* the bound keys (kb_binds) */
				int i = kb_find(e.key.key), dn = e.type == SDL_EVENT_KEY_DOWN;
				if (i >= 0) {
					kb_binds[i].held = dn;
					if (kb_binds[i].bit && !display_pstv_active()) { if (dn) buttons |= kb_binds[i].bit; else buttons &= ~kb_binds[i].bit; }   /* (a PS TV layout: a controller's only, kbm_used) */
					if (dn) { kbm_auto = 1; kbm_used(); }
				}
			}
			if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad && (pad = SDL_OpenGamepad(e.gdevice.which)) != 0) {
				/* a DS4/DualSense's gyroscope and accelerometer stream only once asked for (SDL's HIDAPI driver) */
				int g = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
				int a = SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) && SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
				pad_gyro = g && a;
				fprintf(stderr, "gamepad: %s%s%s\n", SDL_GetGamepadName(pad) ? SDL_GetGamepadName(pad) : "?", SDL_GetNumGamepadTouchpads(pad) > 0 ? ", touchpad" : "", pad_gyro ? ", gyro" : "");
			}
			if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && SDL_GetGamepadID(pad) == e.gdevice.which) { SDL_CloseGamepad(pad); pad = 0; pad_gyro = 0; motion_lost(); gamepad_touch_release(); buttons = 0; for (int i = 0; i < 4; i++) sticks[i] = 128; }
			if (e.type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE) {
				if (e.gsensor.sensor == SDL_SENSOR_ACCEL) motion_feed_accel(e.gsensor.data, e.gsensor.sensor_timestamp);
				else if (e.gsensor.sensor == SDL_SENSOR_GYRO) motion_feed_gyro(e.gsensor.data, e.gsensor.sensor_timestamp);
			}
			if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (e.gaxis.value > 16000 || e.gaxis.value < -16000))) kbm_auto = 0;   /* the pad in hand: auto's choice */
			if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) motion_recenter();   /* R3 (the Vita has no stick clicks): the gyro's heading becomes straight ahead */
			if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
				int dn = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN, b = e.gbutton.button;
				if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER || b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
					const int k = b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
					if (shoulders_swapped) pad_main(pad, k, dn); else pad_second(pad, k, dn);
				} else if (b == SDL_GAMEPAD_BUTTON_SOUTH && (cur_mode || cur_down || cur_cross)) {
					cur_cross = dn && cur_mode;            /* Cross is the touch while a bumper is held */
					cur_down = cur_mode && (cur_cross || cur_trig[0] || cur_trig[1]);
					if (!dn) buttons &= ~CROSS;            /* (a Cross pressed before the bumper, let go after) */
				} else if (dn) buttons |= pad_bit(b);
				else buttons &= ~pad_bit(b);
			}
			if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
				/* a trigger is pressed past ~37%, released under ~24% (no chatter at the edge) */
				const int k = e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, was = ptrig[k];
				if (!was && e.gaxis.value > 12000) ptrig[k] = 1;
				else if (was && e.gaxis.value < 8000) ptrig[k] = 0;
				if (ptrig[k] != was) { if (shoulders_swapped) pad_second(pad, k, ptrig[k]); else pad_main(pad, k, ptrig[k]); }
			}
			if (e.type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN || e.type == SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION || e.type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP) {
				int f = e.gtouchpad.finger;
				if (e.gtouchpad.touchpad == 0 && f >= 0 && f < TP_FINGERS) {
					if (e.type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP) tp_down[f] = 0;
					else {
						if (e.type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN || !tp_down[f]) tp_port[f] = rb_held;   /* the panel is chosen as the finger lands */
						float u = e.gtouchpad.x < 0 ? 0 : e.gtouchpad.x > 1 ? 1 : e.gtouchpad.x, v = e.gtouchpad.y < 0 ? 0 : e.gtouchpad.y > 1 ? 1 : e.gtouchpad.y;
						tp_x[f] = clampi((int)(u * 1920.f), 0, 1919);
						tp_y[f] = tp_port[f] ? clampi(REAR_Y0 + (int)(v * (REAR_Y1 - REAR_Y0 + 1)), REAR_Y0, REAR_Y1) : clampi((int)(v * 1088.f), 0, 1087);
						tp_down[f] = 1;
					}
				}
			}
			/* the mouse moved for real over the picture while the pad had the input (auto mode): back to keyboard +
			 * mouse, so the capture returns at once -- it used to wait for a key, the pointer loose and stopping at
			 * the window's edges after any touch of the pad (Alt still frees it; a nudge under a few points doesn't count) */
			if (!rel_on && e.type == SDL_EVENT_MOUSE_MOTION && !overlay_mouse_captured() && settings_int("mouse_look", 1) &&
			    fabsf(e.motion.xrel) + fabsf(e.motion.yrel) > 3.0f && !(SDL_GetModState() & SDL_KMOD_ALT)) kbm_auto = 1;
			if (rel_on && (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP)) {   /* captured: the look, and R / L */
				if (e.type == SDL_EVENT_MOUSE_MOTION && now_us() - rel_since_us < 60000) continue;
				if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && gesture_wanted()) {   /* a swipe: see window_touch_gesture_hint */
					gesture_drag = 1; touch_x = 960; touch_y = 544; touch_down = 1; gesture_t0 = now_us();
					fprintf(stderr, "    touch swipe: down (mouse, a gesture window)\n"); continue;
				}
				if (gesture_drag && e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
					fprintf(stderr, "    touch swipe: up after %.0f ms, moved %d,%d\n", (now_us() - gesture_t0) / 1e3, touch_x - 960, touch_y - 544);
					gesture_drag = 0; touch_down = 0; continue;
				}
				if (gesture_drag && e.type == SDL_EVENT_MOUSE_MOTION) {   /* the touch follows the motion, 2.5 panel pixels a mouse unit (a short flick crosses a good part of the panel); the camera stays */
					touch_x = clampi(touch_x + (int)lroundf(e.motion.xrel * 2.5f), 0, 1919); touch_y = clampi(touch_y + (int)lroundf(e.motion.yrel * 2.5f), 0, 1087);
					continue;
				}
				if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && twist_wanted() && !twist_pad) {   /* a twist: see window_touch_twist_hint */
					twist_drag = 1; twist_ang = 0; twist_place(0); twist_down = 1;
					fprintf(stderr, "    touch twist: grip (mouse)\n"); continue;
				}
				if (twist_drag && e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
					fprintf(stderr, "    touch twist: let go (mouse), turned %.0f deg\n", twist_ang * 57.29578f);
					twist_drag = 0; twist_down = 0; continue;
				}
				if (twist_drag && e.type == SDL_EVENT_MOUSE_MOTION) {   /* down turns clockwise: 0.4 degrees a mouse unit x "touch_twist_speed" */
					twist_ang += e.motion.yrel * 0.4f * (float)(M_PI / 180) * (float)settings_float("touch_twist_speed", 1.0);
					twist_place(twist_ang); continue;
				}
				if (e.type == SDL_EVENT_MOUSE_MOTION) {
					int qx = (int)lroundf(e.motion.xrel * 256.0f), qy = (int)lroundf(e.motion.yrel * 256.0f);
					__atomic_fetch_add(&mouse_dx_q8, qx, __ATOMIC_RELAXED); __atomic_fetch_add(&mouse_dy_q8, qy, __ATOMIC_RELAXED);
					look_dx_q8 += qx; look_dy_q8 += qy;
				} else {
					int dn = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
					if (e.button.button == SDL_BUTTON_LEFT) { kbm_lmb = dn; if (dn) buttons |= RTRIG; else buttons &= ~RTRIG; }
					if (e.button.button == SDL_BUTTON_RIGHT) { kbm_rmb = dn; if (dn) buttons |= LTRIG; else buttons &= ~LTRIG; }
				}
				continue;
			}
			int tx, ty;
			if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && mouse_to_touch(e.button.x, e.button.y, 0, &tx, &ty)) { touch_x = tx; touch_y = ty; touch_down = 1; }   /* (a click on the bars touches nothing) */
			if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) touch_down = 0;
			if (e.type == SDL_EVENT_MOUSE_MOTION && touch_down && mouse_to_touch(e.motion.x, e.motion.y, 1, &tx, &ty)) { touch_x = tx; touch_y = ty; }
			/* the right button: the rear panel, at the same place on the picture (the rear panel's
			 * coordinates face the same way as the front's; it's shorter, so y clamps to its rows) */
			if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_RIGHT && mouse_to_touch(e.button.x, e.button.y, 0, &tx, &ty)) { mouse_rear_x = tx; mouse_rear_y = clampi(ty, REAR_Y0, REAR_Y1); mouse_rear_down = 1; }
			if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_RIGHT) mouse_rear_down = 0;
			if (e.type == SDL_EVENT_MOUSE_MOTION && mouse_rear_down && mouse_to_touch(e.motion.x, e.motion.y, 1, &tx, &ty)) { mouse_rear_x = tx; mouse_rear_y = clampi(ty, REAR_Y0, REAR_Y1); }
			if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e.gaxis.axis < 4)
				sticks[e.gaxis.axis] = (uint8_t)((e.gaxis.value + 32768) >> 8);
		}
		{	/* keyboard + mouse, once a loop: which scheme, the capture, WASD as the left stick, the look's stand-in */
			static uint64_t last; uint64_t t = now_us(); double dt = last ? (t - last) / 1e6 : 0; last = t;
			if (dt > 0.1) dt = 0.1;
			static int loaded; if (!loaded) { loaded = 1; kb_reload(); }
			char mb[8]; const char *mode = settings_str("input_mode", "auto", mb, sizeof mb);
			int on = !strcmp(mode, "kbm") ? 1 : !strcmp(mode, "pad") ? 0 : kbm_auto;
			const int pstv = display_pstv_active();   /* a PS TV layout is a controller's only: no keyboard-and-mouse scheme, no capture */
			if (on && pstv) { kbm_used(); on = 0; }
			static int said;
			if (on && !kbm_on && !said) { said = 1; overlay_toast_titled("Keyboard & mouse", "The mouse looks and aims -- hold [Alt] for the cursor\nControls: [F10]", 6.0); }
			kbm_on = on;
			int want = (on || (window_raw_look() && !pstv)) && settings_int("mouse_look", 1) && !overlay_is_open() && !overlay_dialog_active() && win && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) && !(SDL_GetModState() & SDL_KMOD_ALT);
			/* SDL's own state, not just ours: the system can drop the capture behind our back (a focus change as
			 * the window opens, say) and rel_on then claimed a capture that wasn't there -- the pointer showed,
			 * left the window and stopped tracking at its edge. Re-applied whenever the two disagree. */
			if (win && want && rel_on && !SDL_GetWindowRelativeMouseMode(win)) {
				static int said; if (!said++) fprintf(stderr, "mouse: the capture was lost -- re-applied\n");
				SDL_SetWindowRelativeMouseMode(win, true); rel_since_us = now_us();
			}
			/* RECOMP_MOUSE_DIAG=1: once a second, the capture as we, SDL and the system see it -- for a capture
			 * that holds in one game and not another (the pointer stopping at the window's edge) */
			{
				static int diag = -1; if (diag < 0) diag = getenv("RECOMP_MOUSE_DIAG") != 0;
				static uint64_t last;
				if (diag && win && now_us() - last > 1000000) {
					last = now_us();
					float gx, gy, wx, wy; SDL_GetGlobalMouseState(&gx, &gy); SDL_GetMouseState(&wx, &wy);
					int px, py, ww, wh; SDL_GetWindowPosition(win, &px, &py); SDL_GetWindowSize(win, &ww, &wh);
					fprintf(stderr, "mouse: want %d rel_on %d sdl_rel %d kbm %d raw %d focus %d fullscreen %d | global %.0f,%.0f in-window %.0f,%.0f | window %d,%d %dx%d | pending %d,%d\n",
						want, rel_on, SDL_GetWindowRelativeMouseMode(win), on, window_raw_look(), !!(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS),
						!!(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN), gx, gy, wx, wy, px, py, ww, wh,
						__atomic_load_n(&mouse_dx_q8, __ATOMIC_RELAXED) / 256, __atomic_load_n(&mouse_dy_q8, __ATOMIC_RELAXED) / 256);
				}
			}
			if (want != rel_on && win) {
				if (!SDL_SetWindowRelativeMouseMode(win, want) && want) fprintf(stderr, "mouse: capture refused: %s\n", SDL_GetError());
				if (want) rel_since_us = now_us();   /* (the capture's first motion can carry the pointer's jump to the centre: dropped) */
				if (!want) { if (gesture_drag) { gesture_drag = 0; touch_down = 0; } if (twist_drag) { twist_drag = 0; twist_down = twist_pad; } if (kbm_lmb) buttons &= ~RTRIG; if (kbm_rmb) buttons &= ~LTRIG; kbm_lmb = kbm_rmb = 0; __atomic_store_n(&mouse_dx_q8, 0, __ATOMIC_RELAXED); __atomic_store_n(&mouse_dy_q8, 0, __ATOMIC_RELAXED); }
				rel_on = want;
			}
			tilt_section_update();
			if (dt > 0) {   /* the motion sensor's source, and the virtual pad's tilt */
				/* keyboard + mouse, or a pad without a gyro: the virtual pad; a pad with one in hand: its gyro. A change
				 * mid-section glides (motion.c) and the virtual tilt carries on from the angle the game was reading. */
				float roll;
				if (motion_use_virtual(on || !pad_gyro, &roll)) tilt = roll > TILT_MAX ? TILT_MAX : roll < -TILT_MAX ? -TILT_MAX : roll;
				float want = 0;
				if (tilt_section && !overlay_is_open()) {
					if (on) want = (float)(kb_binds[KB_MOVE_RIGHT].held - kb_binds[KB_MOVE_LEFT].held) * TILT_MAX;   /* A / D: eased toward a full tilt while held */
					else if (!pad_gyro) { float sx = (sticks[0] - 127.5f) / 127.5f; if (fabsf(sx) < 0.12f) sx = 0; want = sx * TILT_MAX; }   /* the left stick, analog */
				}
				tilt += (want - tilt) * (float)(1.0 - exp(-dt / (on ? 0.12 : 0.06)));   /* (out of a section: back to level) */
				motion_feed_virtual(tilt, SDL_GetTicksNS());   /* (dropped unless the virtual pad is the source) */
			}
			if (on && !overlay_is_open()) {
				int x = kb_binds[KB_MOVE_RIGHT].held - kb_binds[KB_MOVE_LEFT].held, y = kb_binds[KB_MOVE_DOWN].held - kb_binds[KB_MOVE_UP].held;
				float k = x && y ? 0.7071f : 1.0f;
				sticks[0] = (uint8_t)lroundf(127.5f + x * k * 127.5f); sticks[1] = (uint8_t)lroundf(127.5f + y * k * 127.5f);
				/* the look's stand-in: no raw-look hook has taken the motion lately -- counts per second
				 * as a right-stick deflection (about 900 counts/s full tilt at sensitivity 1), eased over ~8 ms */
				static float vx, vy;
				int raw = t - raw_taken_us < 300000;
				float tx = 0, ty = 0;
				if (rel_on && !raw && dt > 0) {
					float g = window_mouse_sens() / 900.0f / (float)dt;
					tx = look_dx_q8 / 256.0f * g; ty = look_dy_q8 / 256.0f * g * (window_mouse_invert() ? -1.0f : 1.0f);
					tx = tx > 1 ? 1 : tx < -1 ? -1 : tx; ty = ty > 1 ? 1 : ty < -1 ? -1 : ty;
				} else if (rel_on && raw) {
					/* a raw-look hook has the motion: only a nudge just past the game's dead zone on a frame the
					 * mouse moved, so a camera update the game runs only while the stick is deflected still runs
					 * (Uncharted's uc_camera_yaw, hooks.c) -- the turn itself is the hook's. Here, before the
					 * game's pad read: a nudge written after it (an IHOOK past the read) is overwritten unseen. */
					const float n = 0.0f;   /* (off: the hook sets the game's own "stick deflected" flag itself -- hooks.c uc_pad_nudge) */
					tx = look_dx_q8 > 0 ? n : look_dx_q8 < 0 ? -n : 0;
					ty = (look_dy_q8 > 0 ? n : look_dy_q8 < 0 ? -n : 0) * (window_mouse_invert() ? -1.0f : 1.0f);
				}
				look_dx_q8 = look_dy_q8 = 0;
				float a = dt > 0 ? (float)(dt / 0.008) : 1; if (a > 1) a = 1;   /* (eased over ~8 ms: one display frame's jitter, no more) */
				vx += (tx - vx) * a; vy += (ty - vy) * a;
				/* past the game's own radial dead zone: a slow mouse is a small deflection, which the stick
				 * curve would swallow -- start the motion at the dead zone's edge ("mouse_deadzone", the
				 * fraction of full tilt the game ignores; RECOMP_MOUSE_DEADZONE from a game's conf) */
				float ox = vx, oy = vy, m = sqrtf(vx * vx + vy * vy);
				if (m > 0.002f) {
					static float dz = -1; if (dz < 0) { const char *e = getenv("RECOMP_MOUSE_DEADZONE"); dz = (float)settings_float("mouse_deadzone", e ? atof(e) : 0.2); }
					float m2 = dz + m * (1 - dz); if (m2 > 1) m2 = 1;
					ox = vx * m2 / m; oy = vy * m2 / m;
				} else ox = oy = 0;
				sticks[2] = (uint8_t)lroundf(127.5f + ox * 127.5f); sticks[3] = (uint8_t)lroundf(127.5f + oy * 127.5f);
			}
		}
		{	/* a gesture window: a right-stick flick swipes (see flick_down) */
			static int armed = 1; static uint64_t t0; static float fx, fy;
			if (pad && gesture_wanted() && !cur_mode && !touchpad_active() && !overlay_is_open()) {
				float ax = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.f, ay = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.f, mag = sqrtf(ax * ax + ay * ay);
				if (!flick_down && armed && mag > 0.6f) { flick_down = 1; armed = 0; t0 = now_us(); fx = ax / mag; fy = ay / mag; flick_x = 960; flick_y = 544; fprintf(stderr, "    touch swipe: right-stick flick %.2f,%.2f\n", fx, fy); }
				if (mag < 0.3f) armed = 1;
			}
			if (flick_down) {
				double q = (now_us() - t0) / 80000.0;
				if (q >= 1.25) flick_down = 0;   /* (held at the end a moment, then lifted) */
				else { if (q > 1) q = 1; flick_x = clampi(960 + (int)lround(fx * 450 * q), 0, 1919); flick_y = clampi(544 + (int)lround(fy * 450 * q), 0, 1087); }
			}
		}
		{	/* a twist (see window_touch_twist_hint): the sticks are two thumbs on the wheel -- a finger 400 points out from the
			 * centre at full tilt, at the centre inside a 0.15 deadzone (a stick at rest jittering the line would creep the
			 * valve: it takes the turns its way and drops the others); down past 0.35 on either stick, up below 0.2 on both */
			const int w = twist_wanted() && !overlay_is_open();
			if (!w) { twist_drag = 0; twist_pad = 0; }
			else if (pad && !cur_mode && !touchpad_active() && !twist_drag) {
				float v[2][2], mx = 0;
				for (int st = 0; st < 2; st++) {
					float ax = SDL_GetGamepadAxis(pad, st ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX) / 32767.f;
					float ay = SDL_GetGamepadAxis(pad, st ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY) / 32767.f;
					float m = sqrtf(ax * ax + ay * ay); if (m > mx) mx = m;
					if (m < 0.15f) ax = ay = 0; else if (m > 1) { ax /= m; ay /= m; }
					v[st][0] = ax; v[st][1] = ay;
				}
				if (!twist_pad && mx > 0.35f) { twist_pad = 1; fprintf(stderr, "    touch twist: grip (sticks)\n"); }
				else if (twist_pad && mx < 0.2f) { twist_pad = 0; fprintf(stderr, "    touch twist: let go (sticks)\n"); }
				if (twist_pad) for (int i = 0; i < 2; i++) { twist_x[i] = clampi(960 + (int)lroundf(v[i][0] * 400), 0, 1919); twist_y[i] = clampi(544 + (int)lroundf(v[i][1] * 400), 0, 1087); }
			} else twist_pad = 0;
			twist_down = twist_pad || twist_drag;
			static int was, shown;   /* the prompt, the first few valves of a session */
			if (w && !was && shown < 3) { shown++; overlay_toast_titled("Two-finger turn", kbm_on ? "Hold the left button and drag down / up" : "Push both sticks out and roll them", 4.0); }
			if (!w && was) fprintf(stderr, "    touch twist: the game stopped asking for it\n");
			was = w;
		}
		{	/* "Compiling shaders": the game waiting on shader or pipeline builds -- one wait past 300 ms, or over 150 ms of the
			 * last half second in waits (a level's first load: hundreds of ~50 ms ones) -- or a big batch building in the
			 * background for over a second: a badge instead of an unexplained freeze or pop-in */
			extern void gfx_gpu_build_status(double *wait_ms, double *total_ms, int *inflight);
			double wait_ms, total_ms; int inflight; gfx_gpu_build_status(&wait_ms, &total_ms, &inflight);
			static uint64_t busy_since, win_t; static double win_total; const uint64_t t = now_us();
			if (inflight < 16) busy_since = 0; else if (!busy_since) busy_since = t;
			static int burst; if (t - win_t >= 500000) { burst = total_ms - win_total > 150; win_t = t; win_total = total_ms; }
			if (wait_ms > 300 || burst || (busy_since && t - busy_since > 1000000))
				overlay_toast_titled("Compiling shaders", "Building this area's graphics for the first time", 0.6);
		}
		{	/* the bumper cursor: either stick moves it (both add up), full deflection crossing the panel's
			 * width in about 1.3 s times the "touch_cursor_speed" setting; a small deadzone each keeps it still at rest */
			static uint64_t last; uint64_t t = now_us(); double dt = last ? (t - last) / 1e6 : 0; last = t;
			if (dt > 0.1) dt = 0.1;
			if (cur_mode && pad && !overlay_is_open()) {
				int port = cur_mode - 1;
				float vx = 0, vy = 0, sv[2][2] = {{0, 0}, {0, 0}};   /* sv: each stick's own vector, past its deadzone */
				for (int st = 0; st < 2; st++) {   /* left stick, then right */
					float ax = SDL_GetGamepadAxis(pad, st ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX) / 32767.f;
					float ay = SDL_GetGamepadAxis(pad, st ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY) / 32767.f;
					float mag = sqrtf(ax * ax + ay * ay);
					if (mag > 0.12f) { float k = (mag - 0.12f) / (0.88f * mag); sv[st][0] = ax * k; sv[st][1] = ay * k; vx += ax * k; vy += ay * k; if (st) cur_used_r = 1; else cur_used_l = 1; }   /* rescaled past the deadzone */
				}
				if (!cur2_on && cur_used_l && cur_used_r && !touchpad_active()) {   /* both sticks in this hold: the second cursor splits off the first */
					cur2_on = 1; cur2_x[port] = cur_x[port]; cur2_y[port] = cur_y[port];
				}
				if (cur2_on) {   /* two cursors: the left stick steers the first, the right the second, free steering (no straight-line drawing) */
					if (!touchpad_active()) {
						const double v = 1500.0 * settings_float("touch_cursor_speed", 1.0) * dt;
						cur_x[port] = clampi(cur_x[port] + (int)lround(sv[0][0] * v), 0, 1919); cur_y[port] = clampi(cur_y[port] + (int)lround(sv[0][1] * v), port ? REAR_Y0 : 0, port ? REAR_Y1 : 1087);
						cur2_x[port] = clampi(cur2_x[port] + (int)lround(sv[1][0] * v), 0, 1919); cur2_y[port] = clampi(cur2_y[port] + (int)lround(sv[1][1] * v), port ? REAR_Y0 : 0, port ? REAR_Y1 : 1087);
					}
					vx = vy = 0;   /* (the one-cursor path below moves nothing) */
				}
				float vm = sqrtf(vx * vx + vy * vy); if (vm > 1) { vx /= vm; vy /= vm; }   /* both sticks together: no faster than one at full tilt */
				/* drawing (Cross held: the touch is down), the cursor goes in straight lines along
				 * "touch_cursor_draw_directions" (16) directions: it turns when the stick has pointed into another
				 * sector for 40 ms, so a swing through the directions between is a clean corner, and a stick let
				 * back to centre turns at once. A stick steers a cursor in curves -- rounded corners, overshoot,
				 * wobble -- and drawn shapes are judged on shape alone (Uncharted's "$1" matcher: stick-traced
				 * Z's and loops scored 0.48-0.62 where a mouse's scored 0.76, 2026-10-13); straight strokes with
				 * sharp corners are what a finger draws. 0: free steering while drawing too. Aiming (Cross up)
				 * stays free either way. Fractions of a point carry over between frames, so a line at 22.5
				 * degrees stays one at any speed. */
				static int draw_dir = -1, pend_dir = -1; static double pend_t, carry_x, carry_y;
				int ndir = settings_int("touch_cursor_draw_directions", 16);
				if (!cur_down || !(vx || vy)) { draw_dir = pend_dir = -1; }
				if (!cur_down) carry_x = carry_y = 0;
				if ((vx || vy) && !touchpad_active()) {
					double v = 1500.0 * settings_float("touch_cursor_speed", 1.0) * dt, mx = vx, my = vy;
					if (cur_down && ndir >= 4) {
						double step = 2 * M_PI / ndir, mag = sqrt((double)vx * vx + (double)vy * vy);
						int sec = (int)lround(atan2(vy, vx) / step); sec = ((sec % ndir) + ndir) % ndir;
						if (draw_dir < 0) draw_dir = sec;
						if (sec == draw_dir) pend_dir = -1;
						else { if (sec != pend_dir) { pend_dir = sec; pend_t = 0; } pend_t += dt; if (pend_t >= 0.04) { draw_dir = sec; pend_dir = -1; } }
						mx = cos(draw_dir * step) * mag; my = sin(draw_dir * step) * mag;
					}
					double nx = mx * v + carry_x, ny = my * v + carry_y;
					int ix = (int)lround(nx), iy = (int)lround(ny);
					if (cur_down) { carry_x = nx - ix; carry_y = ny - iy; }
					cur_x[port] = clampi(cur_x[port] + ix, 0, 1919);
					cur_y[port] = clampi(cur_y[port] + iy, port ? REAR_Y0 : 0, port ? REAR_Y1 : 1087);
				}
			}
		}
		{	/* the menu just closed (any way: F10, Escape, a pad combo, its Resume button): the sticks moved
			 * while it had the events -- a stick let go with the menu up would otherwise stay deflected */
			static int was_open; int open = overlay_is_open() || overlay_dialog_active();   /* (the save dialog too) */
			if (open && !was_open) { buttons = 0; touch_down = 0; gamepad_touch_release(); for (int i = 0; i < KB_N; i++) kb_binds[i].held = 0; }   /* let go of what was held as it opened */
			if (was_open && !open) {
				kb_reload();   /* (the Controls page may have rebound keys) */
				for (int i = 0; i < 4; i++) sticks[i] = pad ? (uint8_t)((SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)i) + 32768) >> 8) : 128;
				/* and L and R: one held through the menu sends no new event (the other pair, held through, waits for a new press) */
				for (int k = 0; k < 2; k++) {
					ptrig[k] = pad && SDL_GetGamepadAxis(pad, k ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000;
					if (pad) pad_main(pad, k, shoulders_swapped ? SDL_GetGamepadButton(pad, k ? SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER : SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) : ptrig[k]);
				}
			}
			was_open = open;
		}
		if (overlay_wants_quit()) { extern void host_summary(const char *why); extern void gfx_gpu_quit(void); host_summary("window closed"); gfx_gpu_quit(); _exit(0); }
		if (overlay_wants_capture()) { extern void gfxcap_request(void); gfxcap_request(); fprintf(stderr, "capture requested (overlay)\n"); }


		if (!gpu) { SDL_Delay(1); continue; }
		if (overlay_game_paused()) { extern void gfx_shader_prep_tick(void); gfx_shader_prep_tick(); }   /* (a paused game doesn't present: the shader warm-up steps on from here, its prep card's too) */
		if (self_paced) {   /* no vsync wait paces this loop: a present once the game has a new frame, else once a refresh (menus, the overlay); spinning presented ~1200 times a second and slowed the game */
			const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
			const uint64_t period = (uint64_t)(1e6 / (dm && dm->refresh_rate > 1 ? dm->refresh_rate : 60.0f));
			while (!__atomic_load_n(&gnew, __ATOMIC_ACQUIRE) && !__atomic_load_n(&new_frame, __ATOMIC_ACQUIRE) && now_us() - last_paced_us < period) SDL_DelayPrecise(250000);
			last_paced_us = now_us();
		}
		uint64_t t_blit0 = now_us();
		SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
		if (!cmd) continue;
		uint64_t t_blit1 = now_us();   /* the swapchain wait below is vsync, not a cost metrics.c should count */
		/* this loop's frame two submits back is finished before SDL's acquire below looks at its own
		 * in-flight fence (SDL allows two frames in flight: that fence is this frame's or an older one):
		 * a locked poll of this loop's own fence for it, then the sweep that retires it from SDL's
		 * submitted list -- in SDL 3.4.16 the acquire reads, waits on and releases that fence unlocked,
		 * racing every other thread's submit, and then blocks for vsync, so it can't run under the
		 * host's lock itself; see gpulock.h. The pacing is what it was: the acquire's wait finds the
		 * fence done and blocks only in nextDrawable, for vsync, as before. */
		static SDL_GPUFence *frame_fence[3]; static int frame_fi;
		{ int k = (frame_fi + 3 - frames_in_flight) % 3;   /* the submit SDL's acquire will look at: frames_in_flight back */
		  if (frame_fence[k]) { SDL_WaitForGPUFences(gpu, true, &frame_fence[k], 1); SDL_ReleaseGPUFence(gpu, frame_fence[k]); frame_fence[k] = 0; } }
		/* this loop's SDL_GPU calls under the renderer's lock (gfx_dispatch.c), with Vulkan: SDL's Vulkan backend (3.4.16) isn't
		 * safe against this thread recording and submitting while the renderer creates, records and submits on others. With
		 * GPU transfers and snapshots on (gfx_gpu.c), which make and copy more each frame, Killzone crashed inside SDL in this
		 * thread's submit, or jumped into garbage, in 3 of 4 windowed runs; under the lock, none in 7+ (2026-10-26). Mailbox
		 * doesn't wait in the acquire, so it goes under the lock too; with vsync the acquire waits a refresh, outside it.
		 * RECOMP_WINDOW_LOCK=0|1 picks */
		extern void gfx_lock_enter(void), gfx_lock_leave(void);
		static int wlock = -1; if (wlock < 0) { const char *e = getenv("RECOMP_WINDOW_LOCK"), *dv = SDL_GetGPUDeviceDriver(gpu); wlock = e && *e ? *e != '0' : dv && !strcmp(dv, "vulkan"); }
		if (wlock) gfx_lock_enter();
		gpu_sweep(gpu);
		if (wlock && !self_paced) gfx_lock_leave();   /* (vsync: the acquire waits a refresh, outside the lock) */
		SDL_GPUTexture *swap = 0;
		uint32_t ww = 0, wh = 0;
		int got_swap = SDL_WaitAndAcquireGPUSwapchainTexture(cmd, win, &swap, &ww, &wh);
		uint64_t t_blit2 = now_us();
		if (wlock && !self_paced) gfx_lock_enter();
		if (got_swap && ww && wh) { win_pw = ww; win_ph = wh; }
		/* the frame to show, picked after the drawable wait: whatever the game flipped while this loop
		 * waited for vsync goes out now, not a refresh later */
		pthread_mutex_lock(&lock);
		int have_frame = new_frame;
		uint32_t pw = fw, ph = fh;
		SDL_GPUTransferBuffer *up_xfer = 0;
		if (gnew) { gshow = gidx; gnew = 0; }
		SDL_GPUTexture *gsrc = gshow >= 0 ? gtex[gshow] : 0; uint32_t gw = gshow >= 0 ? gtw[gshow] : 0, gh = gshow >= 0 ? gth[gshow] : 0, gl = gshow >= 0 ? gtl[gshow] : 1;
		if (have_frame && gpu && ensure_src_texture(pw, ph)) {
			void *dst = SDL_MapGPUTransferBuffer(gpu, xfer, true);
			/* the overlay goes on AFTER the game frame, into this upload copy -- never into
			 * `frame` itself (the software path's next guest-memory read overwrites it fresh
			 * anyway) and nowhere near guest memory, so RECOMP_SHOT (read straight from guest
			 * memory, elsewhere) never sees it */
			if (dst) { memcpy(dst, frame, 4 * pw * ph); if (!(overlay_draws_hud() && !osd_cpu())) metrics_osd_composite((uint8_t *)dst, pw, pw, ph); SDL_UnmapGPUTransferBuffer(gpu, xfer); up_xfer = xfer; }   /* (the ImGui HUD draws the overlay below otherwise) */
			new_frame = 0;
		}
		pthread_mutex_unlock(&lock);
		if (up_xfer) {
			SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
			SDL_GPUTextureTransferInfo src = { .transfer_buffer = up_xfer, .pixels_per_row = pw, .rows_per_layer = ph };
			SDL_GPUTextureRegion dst = { .texture = src_tex, .w = pw, .h = ph, .d = 1 };
			SDL_UploadToGPUTexture(cp, &src, &dst, true);
			SDL_EndGPUCopyPass(cp);
		}
		if (got_swap && swap && (gsrc || src_tex)) {
			/* the 960x544-aspect frame inside the (resizable) window, in whichever aspect mode the
			 * overlay's Display page picked (present_rect, above: fit/stretch/fill) */
			uint32_t sw = gsrc ? gw : stw, sh = gsrc ? gh : sth;
			float px, py, pw2, ph2, dx, dy, dw, dh;
			present_rect((float)ww, (float)wh, (float)sw, (float)sh, &px, &py, &pw2, &ph2, &dx, &dy, &dw, &dh);
			/* a frame with mip levels (window_present_gpu): the smallest level still at least the size it is
			 * drawn at, so the last linear step shrinks by under 2x */
			uint32_t lv = 0;
			if (gsrc) while (lv + 1 < gl && ph2 / (float)(2u << lv) >= dh && pw2 / (float)(2u << lv) >= dw) lv++;
			/* FXAA and sharpening (post.c), or else the plain scaled blit */
			extern int post_present(SDL_GPUDevice *, SDL_GPUCommandBuffer *, SDL_GPUTexture *, uint32_t, uint32_t, uint32_t, float, float, float, float, SDL_GPUTexture *, SDL_GPUTextureFormat, float, float, float, float);
			if (!post_present(gpu, cmd, gsrc ? gsrc : src_tex, sw, sh, gsrc ? gl : 1, px, py, pw2, ph2, swap, SDL_GetGPUSwapchainTextureFormat(gpu, win), dx, dy, dw, dh)) {
				SDL_GPUBlitInfo blit = {
					.source = { .texture = gsrc ? gsrc : src_tex, .mip_level = lv, .x = (Uint32)px >> lv, .y = (Uint32)py >> lv, .w = (Uint32)pw2 >> lv, .h = (Uint32)ph2 >> lv },
					.destination = { .texture = swap, .x = (Uint32)dx, .y = (Uint32)dy, .w = (Uint32)dw, .h = (Uint32)dh },
					.load_op = SDL_GPU_LOADOP_CLEAR, .clear_color = { 0, 0, 0, 1 }, .filter = SDL_GPU_FILTER_LINEAR,
				};
				SDL_BlitGPUTexture(cmd, &blit);
			}
			/* the overlay, if open, on top of the frame just blitted -- after it, on the same
			 * swapchain texture, for both the GPU-presented path (gsrc) and the software path
			 * (src_tex): see overlay_frame's own comment on why LOAD, not CLEAR */
			double dt = last_overlay_t ? (double)(t_blit0 - last_overlay_t) / 1e6 : 1.0 / 60.0;
			last_overlay_t = t_blit0;
			/* the free pointer as a touch cursor -- Alt / Option held, or a click under way -- over the picture: the
			 * bumper cursor's ring in its place (the OS arrow hidden, overlay_touch_cursor), blue on the front panel,
			 * orange on the rear (the right button), filled while pressed; over the bars or a menu, the arrow */
			int mt = 0, mtx = 0, mty = 0;
			if (!rel_on && !overlay_is_open() && !overlay_mouse_captured() && ((SDL_GetModState() & SDL_KMOD_ALT) || touch_down || mouse_rear_down)) {
				float px, py; SDL_GetMouseState(&px, &py);
				mt = mouse_to_touch(px, py, 0, &mtx, &mty);
			}
			overlay_touch_cursor(mt);
			overlay_frame(cmd, swap, ww, wh, dt);
			/* the gamepad-touch markers: the bumper cursor, the touchpad's fingers, a right-button rear
			 * touch; the rear panel's rows outlined while rear touch is in play */
			OverlayMark mk[TP_FINGERS + ST_FINGERS + 6]; int nm = 0; float rear[4] = {0, 0, 0, 0};
			if (flick_down) { touch_to_point(flick_x, flick_y, &mk[nm].x, &mk[nm].y); mk[nm].rear = 0; mk[nm].pressed = 1; mk[nm].cursor = 0; nm++; }
			if (twist_down) for (int i = 0; i < 2; i++) { touch_to_point(twist_x[i], twist_y[i], &mk[nm].x, &mk[nm].y); mk[nm].rear = 0; mk[nm].pressed = 1; mk[nm].cursor = 0; nm++; }
			if (cur_mode) { touch_to_point(cur_x[cur_mode - 1], cur_y[cur_mode - 1], &mk[nm].x, &mk[nm].y); mk[nm].rear = cur_mode == 2; mk[nm].pressed = cur_down; mk[nm].cursor = 1; nm++; }
			if (cur_mode && cur2_on) { touch_to_point(cur2_x[cur_mode - 1], cur2_y[cur_mode - 1], &mk[nm].x, &mk[nm].y); mk[nm].rear = cur_mode == 2; mk[nm].pressed = cur_down; mk[nm].cursor = 1; nm++; }
			for (int i = 0; i < TP_FINGERS; i++) if (tp_down[i]) { touch_to_point(tp_x[i], tp_y[i], &mk[nm].x, &mk[nm].y); mk[nm].rear = tp_port[i]; mk[nm].pressed = 1; mk[nm].cursor = 0; nm++; }
			for (int i = 0; i < ST_FINGERS; i++) if (st_down[i] && st_port[i]) { touch_to_point(st_x[i], st_y[i], &mk[nm].x, &mk[nm].y); mk[nm].rear = 1; mk[nm].pressed = 1; mk[nm].cursor = 0; nm++; }   /* (a rear touch from the screen: the finger isn't where it lands) */
			if (mt) { touch_to_point(mtx, mouse_rear_down ? clampi(mty, REAR_Y0, REAR_Y1) : mty, &mk[nm].x, &mk[nm].y); mk[nm].rear = mouse_rear_down; mk[nm].pressed = touch_down || mouse_rear_down; mk[nm].cursor = 1; nm++; }
			else if (mouse_rear_down) { touch_to_point(mouse_rear_x, mouse_rear_y, &mk[nm].x, &mk[nm].y); mk[nm].rear = 1; mk[nm].pressed = 1; mk[nm].cursor = 0; nm++; }
			int rear_in_play = cur_mode == 2 || rb_held || mouse_rear_down;
			for (int i = 0; i < TP_FINGERS; i++) rear_in_play |= tp_down[i] && tp_port[i];
			for (int i = 0; i < ST_FINGERS; i++) rear_in_play |= st_down[i] && st_port[i];
			if (rear_in_play) { float x0, y0, x1, y1; touch_to_point(0, REAR_Y0, &x0, &y0); touch_to_point(1920, REAR_Y1 + 1, &x1, &y1); rear[0] = x0; rear[1] = y0; rear[2] = x1 - x0; rear[3] = y1 - y0; }
			overlay_marks(cmd, swap, ww, wh, dt, mk, nm, rear);
		}
		frame_fence[frame_fi] = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd); frame_fi = (frame_fi + 1) % 3;   /* vsync via the swapchain paces this loop; the fence is for the poll above, frames_in_flight on */
		if (wlock) gfx_lock_leave();
		uint64_t t_blit3 = now_us();
		{	/* each newly shown GPU frame: the flip -> submit time, plus the compositor and scan-out estimate,
			 * into the automatic A/V sync's picture term (av_video_ms); RECOMP_LATENCY_LOG=1 prints the
			 * terms once a second */
			static int on = -1; if (on < 0) on = getenv("RECOMP_LATENCY_LOG") != 0;
			static int last_shown = -1; static uint64_t sum, n, mx, t_rep; static float refresh_ms = 16.7f; static uint64_t t_mode;
			if (t_blit3 - t_mode > 2000000) {   /* the display's refresh, re-read now and then (the window may move) */
				const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
				if (dm && dm->refresh_rate > 1) refresh_ms = 1000.0f / dm->refresh_rate;
				t_mode = t_blit3;
			}
			if (gsrc && gshow >= 0 && gshow != last_shown && gready_us[gshow]) {
				uint64_t d = t_blit3 - gready_us[gshow];
				if (d < 200000) av_video_ms += ((float)d / 1e3f + 1.5f * refresh_ms - av_video_ms) * 0.05f;
				sum += d; n++; if (d > mx) mx = d;
				if (on && t_blit3 - t_rep > 1000000) {
					extern float display_frame_period_ms(void);
					fprintf(stderr, "    latency: flip -> submit avg %.1f ms max %.1f ms over %llu frames (frames in flight %d); A/V: flip interval %.1f + after flip %.1f (refresh %.1f) - audio %.1f/%.1f -> delay %d/%d ms\n",
					        sum / 1e3 / (double)n, mx / 1e3, (unsigned long long)n, frames_in_flight, display_frame_period_ms(), av_video_ms, refresh_ms, av_audio_ms[0], av_audio_ms[1], av_applied[0], av_applied[1]);
					sum = n = mx = 0; t_rep = t_blit3;
				}
			}
			last_shown = gshow;
		}
		metrics_window_frame((t_blit1 - t_blit0) + (t_blit3 - t_blit2));   /* upload + blit/submit; (t_blit2 - t_blit1) is the vsync wait, excluded */

		char title[160];
		if (metrics_title(title, sizeof title)) SDL_SetWindowTitle(win, title);
	}
}
