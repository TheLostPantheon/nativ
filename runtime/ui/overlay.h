/*
 * The in-game overlay menu: a C API in front of host/overlay.cpp, the one C++ file in this project
 * (Dear ImGui, third_party/imgui/). Everything else in host/ stays plain C and reaches
 * the overlay only through these calls, the same way it reaches window.c's other state.
 *
 * Ownership split with window.c (which actually owns the SDL window, the gamepad and the swapchain
 * present loop -- see its own header comment): window.c decides WHEN the overlay opens/closes (F10,
 * Escape, a gamepad combo, window_present/window_present_gpu's one-shot RECOMP_OVERLAY_DUMP path)
 * and WHERE it draws (the swapchain texture, after the frame blit); overlay.cpp owns everything
 * about WHAT it draws and reads/writes settings.c directly for persistence.
 */
#ifndef RECOMP_OVERLAY_H
#define RECOMP_OVERLAY_H
#include <stdint.h>
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* a badge at the top right for `seconds` (any thread): a title (or none) over the text; "[F10]" in the text draws
 * as a key cap, "\n" breaks the line. Up to three stack, newest on top; the same one again runs on instead */
void overlay_toast_titled(const char *title, const char *body, double seconds);
void overlay_toast(const char *msg, double seconds);   /* (no title) */
/* the save-data dialog (apputil.c, savedlg.h) is waiting on the player -- a list to pick from or a yes/no
 * question -- and the overlay is drawing it: window.c then routes the keyboard to the overlay instead of the
 * game, drops the mouse capture and lets go of what was held (the game is polling the dialog, not the pad) */
int overlay_dialog_active(void);

/* once, after the SDL_GPUDevice (and, in windowed mode, the SDL_Window) exist. `window` is the real
 * window in interactive mode, or NULL for a headless one-shot render (RECOMP_OVERLAY_DUMP below) --
 * ImGui's SDL3 platform backend (mouse/keyboard/IME/clipboard) only attaches when there is a window
 * to attach it to; the SDLGPU3 renderer backend (the only one that actually draws) always does.
 * `color_fmt` is the texture format the overlay will be drawn into: the swapchain's in windowed
 * mode, or the dump path's own offscreen texture headless. Returns 1 on success, 0 if ImGui or its
 * SDLGPU3 backend failed to initialise (no device, out of memory): every other call below is then a
 * safe no-op, so a failure here never takes the game down with it. */
int overlay_init(SDL_GPUDevice *device, SDL_Window *window, SDL_GPUTextureFormat color_fmt);
void overlay_shutdown(void);

/* open/close state of the FULL menu. While open, window.c must: route keyboard/mouse/gamepad to
 * overlay_process_event instead of the game, keep the game paused (display.c's vblank wait checks
 * overlay_game_paused -- see its own comment), and keep audio silent (window.c's audio_output checks
 * the same). */
int overlay_is_open(void);
void overlay_open(void);
void overlay_close(void);
void overlay_toggle(void);
/* Game > Pause in the menu bar: the game held without the menu (a banner says so). overlay_game_paused
 * is the one flag display.c and window.c's audio poll: the full menu open OR this pause. Closing the
 * full menu (Resume, Escape, F10) clears this pause too. */
void overlay_set_paused(int on);
int overlay_game_paused(void);

/* the menu bar (overlay.cpp's own header comment): drawn by overlay_marks while the full menu is closed,
 * from the mouse events window.c forwards to overlay_process_event at all times. overlay_mouse_captured:
 * ImGui wants the pointer (over the bar, one of its menus, or the pull tab) as of the last frame drawn --
 * window.c checks it before a mouse button or motion becomes a touch, so clicks and drags on the bar
 * never reach the game. overlay_bar_dismiss (Escape while the full menu is closed): closes any open bar
 * menu and hides the bar. */
int overlay_mouse_captured(void);
void overlay_bar_dismiss(void);

/* one-shot flags the Quit and Capture frame buttons set; window.c polls and clears them once a loop
 * iteration, the same way it reacts to SDL_EVENT_QUIT and F12 */
int overlay_wants_quit(void);
int overlay_wants_capture(void);

/* interactive mode only (window != NULL at overlay_init): forwards one SDL event to ImGui. A no-op,
 * cheaply, when the overlay wasn't initialised with a window (headless) or ImGui isn't capturing.
 * window.c forwards every event while the full menu is open, and only mouse and window events while
 * it's closed (the bar is pointer-only; the keys and the pad stay the game's). */
void overlay_process_event(const SDL_Event *event);

/* builds one ImGui frame (the pages below) and renders it into `target`, sized w x h pixels, with
 * SDL_GPU_LOADOP_LOAD -- drawn over whatever is already there, never clearing it. `dt_s`: seconds
 * since the overlay last built a frame (0 is fine for the one-shot headless dump). Call only while
 * overlay_is_open(); a no-op otherwise. Must be called with `cmd` mid-recording (after the frame's
 * own blit, before SDL_SubmitGPUCommandBuffer) -- see window.c's present loop and its own comment on
 * why both the GPU-presented and the software path's blit land in the same swapchain texture first. */
void overlay_frame(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, uint32_t w, uint32_t h, double dt_s);

/* the gamepad-touch markers (window.c): where the bumper cursor and the touchpad's fingers touch, on
 * the picture, while the menu is closed -- x, y in window points; rear (orange) or front (blue);
 * pressed fills it; cursor adds a crosshair. rear_rect (x, y, w, h in window points, or w 0) outlines
 * the rear panel's rows while rear touch is in play. Drawn like overlay_frame (LOAD, after the blit);
 * a no-op with nothing to show or the menu open. */
typedef struct { float x, y; int rear, pressed, cursor; } OverlayMark;
/* window.c: the free pointer is a touch cursor this frame (drawn as a mark): the OS arrow is hidden over the picture */
void overlay_touch_cursor(int on);
void overlay_marks(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, uint32_t w, uint32_t h, double dt_s, const OverlayMark *marks, int n, const float rear_rect[4]);
/* overlay_marks also draws the performance overlay (metrics.c's F1 lines, metrics_osd_lines) when its level is
 * on: the HUD. overlay_draws_hud says whether it will (ImGui up, with a window), so window.c can skip the
 * CPU composite of the same overlay into the frame -- on the GPU path a fence wait + download + re-upload
 * of every frame (window_present_gpu's own comment). RECOMP_OSD_CPU=1 (window.c) keeps that composite.
 * Windowed, overlay_marks builds a frame every present whether or not anything shows: the menu bar's
 * hover and timers, the idle cursor and overlay_mouse_captured live in it (a frame that draws nothing
 * costs no render pass). */
int overlay_draws_hud(void);

/* RECOMP_OVERLAY_DUMP's own entry (window.c): renders one frame into `target` (LOAD, as above) with
 * no platform backend and no regard for overlay_is_open() -- see this call's own comment in
 * overlay.cpp for why it must never touch open/close state. Acquires and submits its own command
 * buffer(s) on `device` (one per drawn frame: the backend's buffers are per frame), so the caller
 * submits whatever wrote `target` before this and records anything that reads it after. `ui` is one
 * entry of RECOMP_OVERLAY_DUMP_UI (window.c): NULL or "menu" the full menu (the default), "menu:<tab>"
 * the full menu on that page (Display, Performance, Game, Controls, Help); "bar" the menu bar slid
 * down; "Game", "View", "Controls" or "Help" the bar with that menu open; "toast" the first-launch
 * hint with the pull tab. */
void overlay_render_forced(SDL_GPUDevice *device, SDL_GPUTexture *target, uint32_t w, uint32_t h, const char *ui);

#ifdef __cplusplus
}
#endif
#endif
