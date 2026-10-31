/*
 * User settings, key=value, in two files:
 *  - the shared one, every game's (the player's display, controls, sound, online ID): SDL's pref path
 *    ProjectNativ/Nativ/settings.ini, beside the launcher's own files (RECOMP_SETTINGS_SHARED=path another; "" none).
 *    The launcher's Settings page edits it, and every game's menu.
 *  - the game's own, in the directory it runs from (vitarecomp run: dump/decrypted/<title>/recomp/settings.ini --
 *    the player's, never committed): its own options (game_key in settings.c: fov, view_distance, scenery_detail,
 *    character_detail, private_server, net_redirect, menu_hint_shown, controller_layout), and any shared key it overrides.
 * A key reads from the game's file first, then the shared one, then its consumer's default. Setting a shared key
 * (any game's menu) writes the shared file and drops the game's override of it; a save merges with the file as it
 * is on disk. A scripted run (RECOMP_HEADLESS: test, capture, verify) or RECOMP_SETTINGS reads no shared file. An
 * environment variable named by a key's consumer (RECOMP_RENDER_SCALE, ...) still wins over both, so scripted runs
 * and the gate stay deterministic: a test runs in its own directory, with no settings.ini at all.
 *
 * Keys (consumer, values):
 *   render_scale  gfx_gpu.c   1..6                      internal resolution (5760x3264 at 6)
 *   aspect        window.c    fit | stretch | fill      how the 960x544 picture meets the window
 *   post_aa       post.c      0 | 1 (default 1)         FXAA on the finished frame (RECOMP_POST_AA wins)
 *   sharpen       post.c      0..1 (default 0.35)       CAS sharpening at output resolution, 0 off (RECOMP_SHARPEN wins)
 *   anisotropy    gfx_gpu.c   1 | 2 | 4 | 8 | 16 (default 16)   anisotropic texture filtering (RECOMP_ANISO wins)
 *   input_mode    window.c    auto | pad | kbm (default auto)   keyboard+mouse or the pad; auto: the device last used; pad ignores gameplay mouse events
 *   controller_layout display.c 0 | 1 | 2 (pack default, normally 1)  game-specific: automatic, handheld touch, or PS TV buttons; restart to apply
 *   mouse_look    window.c    0 | 1 (default 1)          keyboard+mouse: the mouse captured as the look (Alt frees it)
 *   mouse_sens    window.c    0.1..5 (default 1.0)       the look's sensitivity (a raw-look hook's, or the right-stick stand-in's)
 *   mouse_invert  window.c    0 | 1 (default 0)          the look's y inverted
 *   bind_<action> window.c    an SDL key name           keyboard+mouse bindings (kb_binds: move_up/down/left/right,
 *                                                       cross, circle, square, triangle, l, r, start, select, up/down/left/right)
 *   camera        camera.c    light | webcam | dark (default light)   what the Vita's camera sees: a lamp toggled with F7, the computer's camera, or a dark room (RECOMP_CAMERA wins)
 *   raw_look      window.c    0 | 1 (default 1; 0 in a scripted run)   Mouse aim: Direct, a game's raw mouse-look hook (hooks.c),
 *                             or Stick, the right-stick stand-in (a game without the hook gets it either way); F8 switches
 *   save_dialog   apputil.c   ask | auto (default ask)   the save-data dialog: the overlay asks (slot list, yes/no), or every question answers itself (the focused slot, YES); RECOMP_SAVE_DIALOG wins
 *   shadow_filter gfx_gpu.c   0 | 1 | 2 (default 1)     shadow-map samples averaged: 0 the game's one, 1 soft, 2 softer (RECOMP_SHADOW_FILTER wins)
 *   aspect_ratio  window.c    native | 21:9 | 32:9 | window   a wider view, for games with a camera hook (RECOMP_ASPECT_HOOK=1; hooks.c aspect_widen)
 *   hud_layout    gfx_gpu.c   edges | 16:9 | stretch (default edges)   with a wider view: the 2D elements at their own shape, anchored
 *                             to the screen edge of their third or all to the centre (a 16:9 frame), or stretched to the width; read at each present
 *   full_res_world gfx_gpu.c  0 | 1 (default 1)          the buffers a game draws below the screen's resolution at the full one (RECOMP_TARGET_SCALE; read at each scene)
 *   ssao          ssao.c      0 | 1 (default 1)          ambient occlusion on the main 3D scene, for a game that names it (RECOMP_SSAO; RECOMP_SSAO_ON wins)
 *   ssao_strength ssao.c      0..2 (default 1.0)         how dark (read every frame)
 *   open_psid     apputil.c   32 hex digits: this install's console id (sceKernelGetOpenPsId), random, made once; a game
 *                             with saves from before keeps the old fixed one in its own file
 *   online_id     np.c        the player's name online, after "ntv_" (at most 10 letters, digits, '-' or '_'; empty:
 *                             a tag of this install's own)
 *   fullscreen    window.c    0 | 1                     also kept when the window's own button (or the OS) changes it
 *   osd           metrics.c   0 | 1 | 2                 the F1 overlay's level
 *   view_distance hooks.c     1..3 (default 1, the game's own): how much further faded-out scenery reaches
 *                             (Uncharted: the floor's rocks and foliage; RECOMP_VIEW_DISTANCE wins; read live)
 *   fov           hooks.c     a multiplier, 1.0 = the game's own vertical field of view (Unit 13's
 *                             camera update, Uncharted's render camera; culling included; RECOMP_FOV wins; read live, so the
 *                             menu's slider applies at once)
 *   frame_rate    display.c   30 | 60                   60: the swap interval capped at one vblank
 *                             (experimental; RECOMP_FRAME_RATE wins; read every frame)
 *   touch_cursor_speed  window.c  a multiplier, 1.0 default   the bumper touch cursor's speed (either stick)
 *   gyro          motion.c    0 | 1 (default 1)         a DS4/DualSense gyro as the Vita's motion sensor
 *   gyro_pose     motion.c    upright | flat            which Vita pose a pad held level stands for
 *   gyro_sens     motion.c    a multiplier, 1.0 default on the gyro's rotation rate
 *   volume        window.c    0..1 (default 1)          master volume: every audio port's gain (RECOMP_VOLUME wins)
 *   audio_sync_trim_ms window.c  -100..100 (default 0)  added to the automatic audio delay (no menu: the A/V sync is measured; for a TV or receiver that adds latency of its own)
 *   audio_buffers window.c    1..4 (default 1)          buffers an audio port may queue ahead (latency vs. crackle; RECOMP_AUDIO_QUEUE wins)
 *   menu_hint_shown  overlay.cpp  1 once the first-launch hint ("move the mouse to the top for the
 *                             menu") has been shown; absent: show it on this game's next windowed run
 *
 * The in-game overlay (host/overlay.cpp: the full menu on F10, Escape, Back+Start or Guide in
 * window.c, and the menu bar at the top edge) writes all of these; F2 also writes render_scale.
 *
 * Thread-safe; values are read often (cheap: a short table under a mutex), written rarely.
 */
#ifndef SETTINGS_H
#define SETTINGS_H

void settings_load(const char *path);   /* once, early (main.c); NULL: "settings.ini" in the working directory */
void settings_load_shared(const char *path);   /* then the shared file (main.c); NULL or "": none */
int settings_int(const char *key, int def);
double settings_float(const char *key, double def);
const char *settings_str(const char *key, const char *def, char *buf, int bufsize);   /* copied into buf (the table may change) */
int settings_has(const char *key);
void settings_set_int(const char *key, int v);
void settings_set_float(const char *key, double v);
void settings_set_str(const char *key, const char *v);
void settings_set_own(const char *key, const char *v);   /* into the game's own file whatever the key: this game's value apart from the shared one */
void settings_save(void);   /* writes the changed files now, merged with them as they are on disk (the setters don't) */
int settings_shared_dir(char *buf, int n);   /* the shared file's folder, with its separator (0: none -- a test, or RECOMP_SETTINGS) */
int settings_game_option(const char *name);   /* this game's pack offers that option (RECOMP_GAME_OPTIONS, its RUN_ENV: "fov,online") */

#endif
