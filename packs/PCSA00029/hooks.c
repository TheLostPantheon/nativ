/*
 * Uncharted: Golden Abyss (PCSA00029): its hooks, built into this game alone (runtime/build.py RECOMP_PACK_HOOKS). packs/PCSA00029/pack.toml's
 * HOOKS= and IHOOKS= place them on the game's functions and instructions; what every game's hooks share is
 * runtime/core/hooks.h. Each one is this game's own feature, named for the guest function it hooks.
 */
#include "hooks.h"

/* Uncharted at 60 fps: characters ran in place at the end of a log (Dante on every flat log, Drake now
 * and then). Walking a log is a movement state (component +0xdc, update fn_81154ace) that leads the
 * character along a chain of segments; past the last one, fn_81152010 releases it only once the
 * character is more than 5 cm beyond that segment's end: |P - B|^2 > 0.0025 (0x81153350, the constant
 * built by 0x8115332c/0x81153334). The movement holds a character 4.4-4.8 cm past the end; at 30 fps one
 * step carries it to ~6 cm and it is released, at 60 the half step never does. The distance scales
 * with the frame time here (2.5 cm at 60). Found 2026-10-11: F6 captures (60 then 30 at the same
 * moment), a write watch on the state switch (fn_811c44ec), then a per-frame trace of the segment test
 * (along 0.430 m of a 0.386 m segment at 60, 0.445 the frame it let go at 30). */
/* (diagnostics, off unless the pack hooks them: HOOKS="0x8126072a:uc_shake" / "0x8125bc2c:uc_touch 0x813d62d8:uc_h88 ...") */
/* RECOMP_UC_SHAKE_TRACE=1: Uncharted's motion gesture detector (fn_8126072a, once a frame, dt in s0) as it
 * starts: the vector its "shake" test squares (the input singleton's +0x8b8, its last one at this+8), the
 * tilt vector (+0x8c8), its two-strike counter (this+0x4c) and hold-off timer (this+0x48) -- the machete
 * minigame's prompt was dismissed by a shake at 60 fps */
void recomp_hook_uc_shake(Cpu *c) {
	static int on = -1; if (on < 0) on = getenv("RECOMP_UC_SHAKE_TRACE") != 0;
	if (!on) return;
	extern uint64_t display_frames(void);
	uint32_t self = c->r[0], in = RD32(0x819053d8u);
	if (!in) return;
	float v[3], w[3], t[3]; for (int i = 0; i < 3; i++) { uint32_t a = RD32(in + 0x8b8 + 4 * i), b = RD32(self + 8 + 4 * i), d = RD32(in + 0x8c8 + 4 * i); memcpy(&v[i], &a, 4); memcpy(&w[i], &b, 4); memcpy(&t[i], &d, 4); }
	float l2 = v[0]*v[0] + v[1]*v[1] + v[2]*v[2], p2 = w[0]*w[0] + w[1]*w[1] + w[2]*w[2], timer; uint32_t tb = RD32(self + 0x48); memcpy(&timer, &tb, 4);
	fprintf(stderr, "uc shake: frame %llu dt %.4f v (%.3f,%.3f,%.3f) |v|^2 %.3f was %.3f jump %.3f%s tilt (%.3f,%.3f,%.3f) strikes %d timer %.2f\n", (unsigned long long)display_frames(), c->vfp.f[0],
	        v[0], v[1], v[2], l2, p2, l2 - p2, fabsf(l2 - p2) > 1.0f ? " STRIKE" : "", t[0], t[1], t[2], (int)(int16_t)RD16(self + 0x4c), timer);
}
/* RECOMP_UC_TOUCH_TRACE=1: Uncharted's UI touch routing. fn_8125bc2c routes one input action (*r2) of an event
 * (r3) to one subscriber (r1, of the table at this+0xc, 0x1c each: +0 its action, +4 its object, +8/+0xc its
 * member handler, +0x10 its zone -- flags +0x56: 4 touchable, 0x10 captured -- +0x14 a zone list); only a
 * subscriber whose action matches can get it, so only those are logged, with the touch event's kind (+0x16)
 * and the zone it is captured to (+0x88). uc_hNN: the touch widget's handlers (the vtable slot the
 * subscriber names: 0x88 onTouch, 0x8c onTap, 0x90 onTouchTrack, 0x94 onTouchTrackEnd; r0 the widget, r1 the
 * routed copy of the event). Found chasing the machete minigame's prompt at 60 fps (2026-10-13): the press
 * got through, its gesture's success was what got lost (uc_gesture_end below) */
/* Uncharted's drawn shapes (the machete minigames' Z, loops, arrows): fn_8126cacc is the "$1 unistroke
 * recognizer" -- the stroke resampled, rotated, scaled and compared with each active template of
 * customgestures.dat (golden-section search over the rotation, 0.618 / 2 degrees), score = 1 - distance /
 * half the template's diagonal, accepted at 0.8 (0x8126cca2/6 build it, 0x8126ccb8 compares). It is blind
 * to speed; it is strict about shape, and a stick-steered cursor rounds every corner and overshoots it:
 * traced exactly along the guide, its shapes scored under 0.8 where a finger's or a mouse's passed
 * (2026-10-13). For a stroke the mouse or the stick cursor drew, the threshold is
 * "touch_cursor_shape_threshold" (0.7); a real touch -- the pad's touchpad -- keeps the game's 0.8. Only
 * the templates the minigame has active are compared, so a looser bar accepts the shape it asks for.
 * uc_shape_threshold: after the movt of 0.8 (r1); uc_shape_score: after the score's vsub (s0), logged with
 * RECOMP_UC_SWIPE_TRACE=1. */
static int uc_cursor_stroke(void) { extern int window_last_touch_key(int port); int k = window_last_touch_key(0); return k == 1 || k == 2; }
void recomp_ihook_uc_shape_threshold(Cpu *c, int reg) {
	if (reg < 0 || reg > 14 || c->r[reg] != 0x3f4ccccdu || !uc_cursor_stroke()) return;
	float t = settings_float("touch_cursor_shape_threshold", 0.7f); if (!(t > 0 && t <= 1)) return;
	memcpy(&c->r[reg], &t, 4);
}
void recomp_ihook_uc_shape_score(Cpu *c, int reg) {
	(void)reg;
	static int on = -1; if (on < 0) on = getenv("RECOMP_UC_SWIPE_TRACE") != 0;
	if (!on) return;
	extern uint64_t display_frames(void); extern int window_last_touch_key(int port);
	float th; memcpy(&th, &c->r[1], 4);
	int k = window_last_touch_key(0);
	fprintf(stderr, "uc shape: frame %llu score %.3f threshold %.2f (%s)\n", (unsigned long long)display_frames(), c->vfp.f[0], th, k == 1 ? "mouse" : k == 2 ? "stick cursor" : k >= 10 ? "touchpad" : "?");
}
/* RECOMP_UC_SCENE_TRACE=1: Uncharted's scene controllers as the scripts ask whether they are done
 * (cPopIgoSceneController:IsDone, fn_8133c954: done when the controller's time, +0x1ac, has reached its
 * track's duration, [+0x158] -> +4). After a cutscene the sword's charcoal-rubbing puzzle waits on one
 * (t_guards) and at 60 fps it never reported done (2026-10-13). uc_scene_ctl: 0x8133c9de, the time just
 * loaded (s16), the controller in r5; uc_scene_dur: 0x8133c9f2, the duration just loaded (s0). One line a
 * frame per controller. */
static uint32_t uc_scene_obj;
void recomp_ihook_uc_scene_ctl(Cpu *c, int reg) { (void)reg; uc_scene_obj = c->r[5]; }
void recomp_ihook_uc_scene_dur(Cpu *c, int reg) {
	(void)reg;
	static int on = -1; if (on < 0) on = getenv("RECOMP_UC_SCENE_TRACE") != 0;
	if (!on) return;
	extern uint64_t display_frames(void);
	static uint64_t lf; static uint32_t seen[16]; static int ns; uint64_t f = display_frames();
	if (f != lf) { lf = f; ns = 0; }
	for (int i = 0; i < ns; i++) if (seen[i] == uc_scene_obj) return;
	if (ns < 16) seen[ns++] = uc_scene_obj;
	fprintf(stderr, "uc scene: frame %llu controller %#x time %.4f / duration %.4f%s\n", (unsigned long long)f, uc_scene_obj, c->vfp.f[16], c->vfp.f[0], c->vfp.f[16] >= c->vfp.f[0] ? "  DONE" : "");
}
/* The game-time warp the scene clock runs through (singleton [0x81905434], its state at +4: 0 off,
 * 1 pause next frame, 2 paused (time banked at +0xc, dt out 0), 3 pay the bank back, 4 half/1.5x speed
 * until +0xc is absorbed; +8 enabled, +9 dt at +0x10 already applied this frame). Set by a scene's
 * camera-cut alignment (fn_81602142: pause within k/30 s of a cut, resume once past it) and the
 * cinematic audio sync (fn_810cadd4: speed toward the audio's time). uc_warp_*: those setters' entries;
 * uc_scene_owner: the scene's owner update fn_8133770a (dt in s0), one line a frame. */
static int uc_scene_on(void) { static int on = -1; if (on < 0) on = getenv("RECOMP_UC_SCENE_TRACE") != 0; return on; }
static float uc_f32(uint32_t a) { uint32_t v = RD32(a); float f; memcpy(&f, &v, 4); return f; }
static void uc_warp_log(Cpu *c, const char *what) {
	if (!uc_scene_on()) return;
	extern uint64_t display_frames(void);
	uint32_t g = RD32(0x81905434u);
	fprintf(stderr, "uc warp: frame %llu %s s0 %.4f lr %#x | state %u enabled %u applied %u bank %.4f out %.4f\n", (unsigned long long)display_frames(), what, c->vfp.f[0], c->r[14],
	        g ? RD32(g + 4) : 0, g ? RD8(g + 8) : 0, g ? RD8(g + 9) : 0, g ? uc_f32(g + 0xc) : 0, g ? uc_f32(g + 0x10) : 0);
}
void recomp_hook_uc_warp_pause(Cpu *c) { uc_warp_log(c, "pause"); }
void recomp_hook_uc_warp_speed(Cpu *c) { uc_warp_log(c, "speed"); }
void recomp_hook_uc_warp_resume(Cpu *c) { uc_warp_log(c, "resume"); }
void recomp_hook_uc_audio_sync(Cpu *c) {
	if (!uc_scene_on()) return;
	extern uint64_t display_frames(void);
	uint32_t o = c->r[0], s = RD32(o + 0x38), ctl = s ? RD32(s + 0x1c4) : 0;
	fprintf(stderr, "uc audio sync: frame %llu audio %.4f scene %.4f active %u\n", (unsigned long long)display_frames(), c->vfp.f[0], ctl ? uc_f32(ctl + 0x1ac) : -1.0f, RD8(o + 0x74));
}
void recomp_hook_uc_scene_owner(Cpu *c) {
	if (!uc_scene_on()) return;
	uint32_t o = c->r[0], ctl = RD32(o + 0x1c4);
	if (!ctl || (uc_scene_obj && ctl != uc_scene_obj)) return;
	extern uint64_t display_frames(void); extern float display_frame_dt(void);
	uint32_t g = RD32(0x81905434u);
	fprintf(stderr, "uc owner: frame %llu owner %#x controller %#x dt %.4f frame_dt %.4f playing %u paused %u skip %u time %.4f | warp state %u applied %u bank %.4f out %.4f scale %.3f\n",
	        (unsigned long long)display_frames(), o, ctl, c->vfp.f[0], display_frame_dt(), RD8(o + 0x208), RD8(o + 0x209), (RD8(o + 0x21d) >> 2) & 1, uc_f32(ctl + 0x1ac),
	        g ? RD32(g + 4) : 0, g ? RD8(g + 9) : 0, g ? uc_f32(g + 0xc) : 0, g ? uc_f32(g + 0x10) : 0, uc_f32(RD32(0x819052ecu) + 0x98));
}
/* RECOMP_UC_PUZZLE_TRACE=1: Uncharted's puzzles (cPuzzle; the sword's charcoal rubbing never started at
 * 60 fps, 2026-10-13). Start fn_81081f8e (current puzzle [0x818ff904] = this, countdown +0x100), Begin
 * fn_81082178 (once the countdown is out: puzzle camera, GUI, HandleOnPuzzleStart; +0x108 = begun), End
 * fn_81081ab4, Finish fn_811292c2, the rub grid reset fn_8112854c, progress fn_81128a96, rub strokes
 * fn_8112945e, the load gate fn_81081c2a, the charcoal update fn_811289e4, camera transitions fn_8102d494;
 * uc_pz_active/uc_pz_gate: the base update fn_81081ba2's own IsActive (vtable +0x12c) and game-manager
 * (+0xa0, nonzero holds Begin back) results, logged when a puzzle's gate state changes. */
static int uc_pz_on(void) { static int on = -1; if (on < 0) on = getenv("RECOMP_UC_PUZZLE_TRACE") != 0; return on; }
static void uc_pz_log(Cpu *c, const char *what, uint32_t p) {
	extern uint64_t display_frames(void);
	uint32_t hud = RD32(0x819053a8u), gm = RD32(0x819052ecu);
	fprintf(stderr, "uc puzzle: frame %llu %s %#x lr %#x | current %#x countdown %u/%u begun %u load %u/%u hud %u scale %.2f\n", (unsigned long long)display_frames(), what, p, c->r[14],
	        RD32(0x818ff904u), p ? RD32(p + 0x100) : 0, p ? RD32(p + 0x104) : 0, p ? RD8(p + 0x108) : 0, p ? RD8(p + 0x110) : 0, p ? RD8(p + 0x111) : 0, hud ? RD8(hud + 8) : 0, gm ? uc_f32(gm + 0x98) : -1.0f);
}
void recomp_hook_uc_pz_start(Cpu *c) { if (uc_pz_on()) { uc_pz_log(c, "start", c->r[0]); fprintf(stderr, "  start args r1 %#x begin-now %u\n", c->r[1], c->r[2] & 0xff); } }
void recomp_hook_uc_pz_begin(Cpu *c) { if (uc_pz_on()) uc_pz_log(c, "begin", c->r[0]); }
void recomp_hook_uc_pz_end(Cpu *c) { if (uc_pz_on()) uc_pz_log(c, "end", c->r[0]); }
void recomp_hook_uc_pz_finish(Cpu *c) { if (uc_pz_on()) uc_pz_log(c, "finish", c->r[0]); }
void recomp_hook_uc_pz_rubreset(Cpu *c) { if (uc_pz_on()) uc_pz_log(c, "rub-grid reset", c->r[0]); }
void recomp_hook_uc_pz_progress(Cpu *c) {
	static float last = -1; if (!uc_pz_on() || c->vfp.f[0] == last) return;
	last = c->vfp.f[0]; extern uint64_t display_frames(void);
	fprintf(stderr, "uc puzzle: frame %llu progress %#x %.2f%%\n", (unsigned long long)display_frames(), c->r[0], last);
}
void recomp_hook_uc_pz_rub(Cpu *c) {
	static uint64_t lf; extern uint64_t display_frames(void); uint64_t f = display_frames();
	if (!uc_pz_on() || f < lf + 30) return;
	lf = f; fprintf(stderr, "uc puzzle: frame %llu rub stroke %#x on %#x (current %#x)\n", (unsigned long long)f, c->r[0], c->r[1], RD32(0x818ff904u));
}
void recomp_hook_uc_pz_load(Cpu *c) {
	static uint32_t lp, ls; uint32_t p = c->r[0], s = RD8(p + 0x110) | RD8(p + 0x111) << 8;
	if (!uc_pz_on() || (p == lp && s == ls)) return;
	lp = p; ls = s; uc_pz_log(c, "load gate", p);
}
void recomp_hook_uc_pz_charcoal(Cpu *c) {
	static uint32_t lp, ls; uint32_t p = c->r[0], s = RD8(p + 0x1f8) | RD8(p + 0x1f9) << 8 | RD8(p + 0x1bb) << 16;
	if (!uc_pz_on() || (p == lp && s == ls)) return;
	lp = p; ls = s; extern uint64_t display_frames(void);
	fprintf(stderr, "uc puzzle: frame %llu charcoal update %#x dt %.4f fading %u done %u complete %u fade %.2f/%.2f\n", (unsigned long long)display_frames(), p, c->vfp.f[0],
	        RD8(p + 0x1f8), RD8(p + 0x1f9), RD8(p + 0x1bb), uc_f32(p + 0x1f0), uc_f32(p + 0x1f4));
}
void recomp_hook_uc_transition(Cpu *c) {
	static uint64_t lf; static int n; extern uint64_t display_frames(void); uint64_t f = display_frames();
	if (!uc_pz_on()) return;
	if (f != lf) { lf = f; n = 0; }
	if (n++ >= 2) return;
	fprintf(stderr, "uc transition: frame %llu s0 %.3f r0 %#x r1 %#x r2 %#x r3 %#x lr %#x\n", (unsigned long long)f, c->vfp.f[0], c->r[0], c->r[1], c->r[2], c->r[3], c->r[14]);
}
static uint32_t uc_pz_cur_active;
void recomp_ihook_uc_pz_gate(Cpu *c, int reg);
void recomp_ihook_uc_pz_active(Cpu *c, int reg) {
	(void)reg; uc_pz_cur_active = c->r[0];
	if (uc_pz_on() && !c->r[0]) recomp_ihook_uc_pz_gate(c, -2);
}
void recomp_ihook_uc_pz_gate(Cpu *c, int reg) {
	if (!uc_pz_on()) return;
	uint32_t p = c->r[4], gm = reg == -2 ? 0xffffffffu : c->r[0];
	static uint32_t ps[16], st[16]; static int n;
	uint32_t s = (uc_pz_cur_active != 0) | (gm != 0) << 1 | (gm == 0xffffffffu) << 2 | (RD32(0x818ff904u) == p) << 3 | RD8(p + 0x108) << 4 | (RD32(p + 0x100) != 0) << 5;
	int i = 0; while (i < n && ps[i] != p) i++;
	if (i < n && st[i] == s) return;
	if (i == n) { if (n == 16) return; ps[n++] = p; }
	st[i] = s;
	extern uint64_t display_frames(void);
	fprintf(stderr, "uc puzzle: frame %llu update %#x active %u manager-busy %s current %u begun %u countdown %u\n", (unsigned long long)display_frames(), p, uc_pz_cur_active != 0,
	        gm == 0xffffffffu ? "-" : gm ? "yes" : "no", RD32(0x818ff904u) == p, RD8(p + 0x108), RD32(p + 0x100));
}
static int uc_touch_on(void) { static int on = -1; if (on < 0) on = getenv("RECOMP_UC_TOUCH_TRACE") != 0; return on; }
void recomp_hook_uc_touch(Cpu *c) {
	if (!uc_touch_on()) return;
	uint32_t self = c->r[0], idx = c->r[1], act = RD32(c->r[2]), ev = c->r[3];
	if (!ev || (int16_t)RD16(ev + 4) != 3) return;
	uint32_t tab = RD32(self + 0xc), n = tab ? (RD32(self + 0x10) - tab) / 0x1c : 0;
	if (idx >= n || RD32(tab + idx * 0x1c) != act) return;
	uint32_t ent = tab + idx * 0x1c, zone = RD32(ent + 0x10), list = RD32(ent + 0x14);
	extern uint64_t display_frames(void);
	fprintf(stderr, "uc route: frame %llu action %#x kind %u captured %#x obj %#x handler %#x zone %#x/%#x list %#x\n", (unsigned long long)display_frames(), act,
	        RD8(ev + 0x16), RD32(ev + 0x88), RD32(ent + 4), RD32(ent + 8), zone, zone ? RD16(zone + 0x56) : 0, list);
}
static uint64_t uc_lua_until;   /* the Lua trace below runs from a touch-widget press to three frames after it */
static void uc_widget_handler(Cpu *c, unsigned slot) {
	if (!uc_touch_on()) return;
	extern uint64_t display_frames(void);
	if (slot == 0x88) uc_lua_until = display_frames() + 3;
	uint32_t w = c->r[0], ev = c->r[1];
	fprintf(stderr, "uc widget: frame %llu handler %#x widget %#x flags %#x action %#x kind %u captured %#x\n", (unsigned long long)display_frames(), slot, w, RD32(w + 0x28),
	        ev ? RD32(ev + 0x10) : 0, ev ? RD8(ev + 0x16) : 0, ev ? RD32(ev + 0x88) : 0);
}
void recomp_hook_uc_h88(Cpu *c) { uc_widget_handler(c, 0x88); }   /* fn_813d62d8 */
void recomp_hook_uc_h8c(Cpu *c) { uc_widget_handler(c, 0x8c); }   /* fn_813d63fc */
void recomp_hook_uc_h90(Cpu *c) { uc_widget_handler(c, 0x90); }   /* fn_813d64d8 */
void recomp_hook_uc_h94(Cpu *c) { uc_widget_handler(c, 0x94); }   /* fn_813d65ac */
void recomp_hook_uc_h98(Cpu *c) { uc_widget_handler(c, 0x98); }   /* fn_813d6b4c */
/* The game's UI logic is Lua 5.1 (lua_Number float: a TValue is value + type, 8 bytes; Closure +6 isC, +0x10
 * the C function or the Proto; Proto +8 k, +0xc code, +0x14 lineinfo, +0x18 locvars, +0x1c upvalue names,
 * +0x20 source, +0x28/+0x2c/+0x30/+0x38 the sizes, +0x3c line; lua_State +8 top, +0x14 ci, +0x18 savedpc, +0x28
 * base_ci, CallInfo 0x18 bytes, +4 its function). uc_lua_call hooks luaD_precall (fn_8100e0ec: r0 L, r1 the
 * called function's stack slot), uc_lua_ret luaD_poscall (fn_8100df00: r1 the first result): each call, the
 * Lua caller and its pc, the arguments, and what came back -- every Lua function seen is dumped (code,
 * constants, names) to ~/uc_lua_protos.txt for dev/luadis.py */
static void uc_lua_val(uint32_t tv) {
	uint32_t v = RD32(tv), tt = RD32(tv + 4);
	switch (tt) {
	case 0: fprintf(stderr, " nil"); break;
	case 1: fprintf(stderr, " %s", v ? "true" : "false"); break;
	case 3: { float f; memcpy(&f, &v, 4); fprintf(stderr, " %g", f); break; }
	case 4: { uint32_t n = RD32(v + 0xc); fprintf(stderr, " \"%.*s\"", (int)(n < 48 ? n : 48), (const char *)(recomp_mem + v + 0x10)); break; }
	default: fprintf(stderr, " <%u %#x>", tt, v);
	}
}
static void uc_lua_proto(uint32_t p) {
	static uint32_t seen[4096]; static int nseen; static FILE *f;
	for (int i = 0; i < nseen; i++) if (seen[i] == p) return;
	if (nseen >= 4096) return;
	seen[nseen++] = p;
	if (!f) { char path[512]; snprintf(path, sizeof path, "%s/uc_lua_protos.txt", getenv("HOME") ? getenv("HOME") : "."); f = fopen(path, "w"); if (!f) return; }
	uint32_t src = RD32(p + 0x20), nk = RD32(p + 0x28), nc = RD32(p + 0x2c), nl = RD32(p + 0x30), nloc = RD32(p + 0x38), nup = RD32(p + 0x24);
	fprintf(f, "proto %#x source %.*s line %u params %u vararg %u stack %u\n", p, src ? (int)(RD32(src + 0xc) < 200 ? RD32(src + 0xc) : 200) : 1,
	        src ? (const char *)(recomp_mem + src + 0x10) : "?", RD32(p + 0x3c), RD8(p + 0x49), RD8(p + 0x4a), RD8(p + 0x4b));
	fprintf(f, "code"); for (uint32_t i = 0; i < nc && i < 20000; i++) fprintf(f, " %08x", RD32(RD32(p + 0xc) + 4 * i)); fprintf(f, "\n");
	fprintf(f, "lines"); for (uint32_t i = 0; i < nl && i < 20000; i++) fprintf(f, " %d", (int)RD32(RD32(p + 0x14) + 4 * i)); fprintf(f, "\n");
	for (uint32_t i = 0; i < nk && i < 4096; i++) {
		uint32_t tv = RD32(p + 8) + 8 * i, v = RD32(tv), tt = RD32(tv + 4);
		if (tt == 4) { uint32_t n = RD32(v + 0xc); fprintf(f, "k %u s ", i); fwrite(recomp_mem + v + 0x10, 1, n < 400 ? n : 400, f); fprintf(f, "\n"); }
		else if (tt == 3) { float x; memcpy(&x, &v, 4); fprintf(f, "k %u n %.9g\n", i, x); }
		else if (tt == 1) fprintf(f, "k %u b %u\n", i, v);
		else fprintf(f, "k %u nil\n", i);
	}
	for (uint32_t i = 0; i < nloc && i < 1024; i++) { uint32_t lv = RD32(p + 0x18) + 12 * i, nm = RD32(lv); if (nm) fprintf(f, "local %u %.*s %u %u\n", i, (int)RD32(nm + 0xc), (const char *)(recomp_mem + nm + 0x10), RD32(lv + 4), RD32(lv + 8)); }
	for (uint32_t i = 0; i < nup && i < 256; i++) { uint32_t nm = RD32(RD32(p + 0x1c) + 4 * i); if (nm) fprintf(f, "upval %u %.*s\n", i, (int)RD32(nm + 0xc), (const char *)(recomp_mem + nm + 0x10)); }
	fprintf(f, "end\n"); fflush(f);
}
/* the gesture object behind the prompt (BSGame::cGestureObject): fn_812029ce its press callback (r0 the object,
 * r1 the event; it starts the gesture when +0xe5 has 0x20, the state +0xd0 is 1, +0xe5 has 8 or 0x80, and the
 * event's action is the gesture's, [+0xb8]+0x44); fn_81202086 its end, below (r1 1 success / 0 fail) */
void recomp_hook_uc_gesture_press(Cpu *c) {
	if (!uc_touch_on()) return;
	extern uint64_t display_frames(void);
	uint32_t o = c->r[0], ev = c->r[1], cfg = RD32(o + 0xb8);
	fprintf(stderr, "uc gesture press: frame %llu obj %#x e5 %#x e6 %#x state %u action %#x want %#x active-fn %#x lr %#x\n", (unsigned long long)display_frames(), o,
	        RD8(o + 0xe5), RD8(o + 0xe6), RD32(o + 0xd0), ev ? RD32(ev + 0x10) : 0, cfg ? RD32(cfg + 0x44) : 0, RD32(RD32(o) + 0x12c), c->r[14]);
}
/* Uncharted's touch prompts (BSGame::cGestureObject: the machete's "touch to cut", ...). A press that meets
 * the prompt ends its gesture with success (fn_81202086: state +0xd0 = 2, the fade-out timer at +0xe8
 * restarted). The interact object's update (fn_81206606) then runs the gesture's update (fn_812021e6) and only
 * after it looks for state 2 to start the scene (fn_81337db0 -> the scene's HandleSceneBegin). But that
 * gesture update ends the fade once its timer -- process time, wall-clock seconds -- has passed the fade's
 * length, and ending it puts the state back to 0: when more real time than that has gone between the
 * press and the next update, the success is gone before anyone saw it, and the prompt just closes. At 30 fps
 * (and on a slower host) it hadn't; at a true 60 it had, every time (2026-10-13: a call-sequence diff of the
 * two from the success on parted exactly there). The fix: a gesture's first update after its success
 * starts the fade from that update -- the success is always seen, the fade is as long as ever. */
static uint32_t uc_succeeded[8];
void recomp_hook_uc_gesture_update(Cpu *c) {   /* fn_812021e6, r0 the gesture */
	uint32_t o = c->r[0];
	for (int i = 0; i < 8; i++) if (uc_succeeded[i] == o) {
		uc_succeeded[i] = 0;
		if (RD32(o + 0xd0) != 2) return;
		extern uint64_t guest_time_us_peek(void);
		uint64_t now = guest_time_us_peek();   /* the timer (fn_81383688 & co): +0 start, +0x10 last read, +0x18 time paused, +0x28 paused */
		WR64(o + 0xe8, now); WR64(o + 0xf8, now); WR64(o + 0x100, 0);
		return;
	}
}
void recomp_hook_uc_gesture_end(Cpu *c) {   /* fn_81202086, r0 the gesture, r1 success */
	uint32_t o = c->r[0];
	if ((c->r[1] & 0xff) && RD32(o + 0xd0) == 1) {
		int k = 0; for (int i = 0; i < 8; i++) if (uc_succeeded[i] == o || !uc_succeeded[i]) { k = i; break; }
		uc_succeeded[k] = o;
	}
	if (!uc_touch_on()) return;
	extern uint64_t display_frames(void);
	fprintf(stderr, "uc gesture end: frame %llu obj %#x %s e5 %#x state %u lr %#x\n", (unsigned long long)display_frames(), o, c->r[1] & 0xff ? "SUCCESS" : "fail", RD8(o + 0xe5), RD32(o + 0xd0), c->r[14]);
	if ((c->r[1] & 0xff) && hook_seq_arm) hook_seq_arm("gesture success");   /* a verify build: the calls that follow, in order */
}
static int uc_lua_window(void) {   /* (RECOMP_UC_TOUCH_TRACE: from a widget press, or any front touch, to 4 s after it) */
	extern uint64_t display_frames(void); extern int window_touch_contacts(int port, int *keys, int *xs, int *ys, int max);
	if (!uc_touch_on()) return 0;
	static int all = -1; if (all < 0) all = getenv("RECOMP_UC_LUA_ALL") != 0;   /* (RECOMP_UC_LUA_ALL=1: every frame -- a short session) */
	if (all) return 1;
	int k[2], x[2], y[2]; uint64_t f = display_frames();
	if (window_touch_contacts(0, k, x, y, 2) && uc_lua_until < f + 240) uc_lua_until = f + 240;
	return uc_lua_until && f <= uc_lua_until;
}
/* RECOMP_UC_SCRIPT_TRACE=1: Uncharted's script system (the VM object at [0x81905348]: +4 the main lua_State,
 * +8 + 4*i one per game thread, the index from the input/game manager's vfunc 0x90) as a "HandleHotboxEnter"
 * handler is looked up (fn_81379aaa: r0 the VM, r1 the object, r2 the name, r3 1 = the calling thread's own
 * state), and any lua_pcall (fn_8100af5c, r0 L) on a state whose global state (L+0x10) has no allocator
 * (+0xc). Written for the crash on the rope swing at 60 fps (2026-10-13): the level script's
 * HandleHotboxEnter indexed an enemy group not there yet at 60 ("attempt to index a nil value"), and raising
 * that error crashed -- not a dead state: Sony's longjmp, recompiled, returned instead of unwinding (fixed at
 * the core: rt.h RecompJmp, main.c recomp_longjmp) */
static int uc_script_on(void) { static int on = -1; if (on < 0) on = getenv("RECOMP_UC_SCRIPT_TRACE") != 0; return on; }
void recomp_hook_uc_script_has(Cpu *c) {
	if (!uc_script_on() || !c->r[2] || strcmp((const char *)G(c->r[2]), "HandleHotboxEnter")) return;
	extern uint64_t display_frames(void); extern const char *recomp_cur_tname(void);
	uint32_t vm = c->r[0], o = c->r[1];
	fprintf(stderr, "uc script: frame %llu %s HandleHotboxEnter obj %#x (vt %#x flags19 %#x) own %u main L %#x (G %#x) Ls:", (unsigned long long)display_frames(), recomp_cur_tname(),
	        o, o ? RD32(o) : 0, o ? RD8(o + 0x19) : 0, c->r[3], RD32(vm + 4), RD32(vm + 4) ? RD32(RD32(vm + 4) + 0x10) : 0);
	for (int i = 0; i < 8; i++) { uint32_t L = RD32(vm + 8 + 4 * i); fprintf(stderr, " %#x(G %#x)", L, L ? RD32(L + 0x10) : 0); }
	fprintf(stderr, " lr %#x\n", c->r[14]);
}
void recomp_hook_uc_lua_pcall(Cpu *c) {
	if (!uc_script_on()) return;
	uint32_t L = c->r[0], g = L ? RD32(L + 0x10) : 0;
	if (L && g && RD32(g + 0xc)) return;
	extern uint64_t display_frames(void); extern const char *recomp_cur_tname(void);
	fprintf(stderr, "uc script: frame %llu %s lua_pcall on a dead state: L %#x G %#x frealloc %#x lr %#x\n", (unsigned long long)display_frames(), recomp_cur_tname(), L, g, g ? RD32(g + 0xc) : 0, c->r[14]);
}
/* RECOMP_UC_SCRIPT_TRACE: at every Lua call and return, the state's global state still has its allocator;
 * the first time it doesn't, which call or return found it so -- and the last calls before it (the rope
 * swing's HandleHotboxEnter at 60 fps found its state zeroed in mid-handler, 2026-10-13) */
static struct { uint32_t L, fn, from, depth; int ret; } uc_lua_ring[32]; static int uc_lua_ring_n;
static void uc_lua_check(Cpu *c, int ret) {
	if (!uc_script_on()) return;
	uint32_t L = c->r[0], g = L ? RD32(L + 0x10) : 0, ci = L ? RD32(L + 0x14) : 0;
	uint32_t fn = 0, from = 0;
	if (ci) { uint32_t f = RD32(ci + 4), cl = f ? RD32(f) : 0; if (f && RD32(f + 4) == 6 && cl) fn = RD8(cl + 6) ? RD32(cl + 0x10) : RD32(cl + 0x10) | 1u << 31; }
	if (!ret && c->r[1] && RD32(c->r[1] + 4) == 6) { uint32_t cl = RD32(c->r[1]); from = fn; fn = RD8(cl + 6) ? RD32(cl + 0x10) : RD32(cl + 0x10) | 1u << 31; }
	static int reported;
	if (g && RD32(g + 0xc)) {
		int k = uc_lua_ring_n++ & 31;
		uc_lua_ring[k].L = L; uc_lua_ring[k].fn = fn; uc_lua_ring[k].from = from; uc_lua_ring[k].ret = ret; uc_lua_ring[k].depth = ci && L ? (ci - RD32(L + 0x28)) / 0x18 : 0;
		return;
	}
	if (reported++) return;
	extern uint64_t display_frames(void); extern const char *recomp_cur_tname(void);
	fprintf(stderr, "uc script: frame %llu %s state ZEROED, found at a %s: L %#x G %#x; the last %d Lua calls/returns before (fn: C function, or 0x8... | Lua proto with the top bit):\n",
	        (unsigned long long)display_frames(), recomp_cur_tname(), ret ? "return" : "call", L, g, uc_lua_ring_n < 32 ? uc_lua_ring_n : 32);
	for (int i = uc_lua_ring_n < 32 ? 0 : uc_lua_ring_n - 32; i < uc_lua_ring_n; i++) {
		int k = i & 31;
		fprintf(stderr, "  %s L %#x depth %u fn %#x from %#x\n", uc_lua_ring[k].ret ? "ret " : "call", uc_lua_ring[k].L, uc_lua_ring[k].depth, uc_lua_ring[k].fn, uc_lua_ring[k].from);
		if (uc_lua_ring[k].fn >> 31) uc_lua_proto(uc_lua_ring[k].fn & 0x7fffffffu);
	}
	fprintf(stderr, "  (now) fn %#x from %#x\n", fn, from);
}
/* at a call to 0 (main.c's recomp_on_bad_call): the last Lua calls/returns, the state the last one ran on
 * -- its lua_State and global state as they are now, and the instruction it was at (savedpc) */
static void uc_bad_call(Cpu *c, uint32_t addr) {
	(void)c; (void)addr;
	if (!uc_script_on() || !uc_lua_ring_n) return;
	int last = (uc_lua_ring_n - 1) & 31;
	uint32_t L = uc_lua_ring[last].L, g = RD32(L + 0x10), ci = RD32(L + 0x14);
	fprintf(stderr, "uc script at the crash: the last %d Lua calls/returns (fn: C function, or Lua proto | 0x80000000):\n", uc_lua_ring_n < 32 ? uc_lua_ring_n : 32);
	for (int i = uc_lua_ring_n < 32 ? 0 : uc_lua_ring_n - 32; i < uc_lua_ring_n; i++) {
		int k = i & 31;
		fprintf(stderr, "  %s L %#x depth %u fn %#x from %#x\n", uc_lua_ring[k].ret ? "ret " : "call", uc_lua_ring[k].L, uc_lua_ring[k].depth, uc_lua_ring[k].fn, uc_lua_ring[k].from);
		if (uc_lua_ring[k].fn >> 31) uc_lua_proto(uc_lua_ring[k].fn & 0x7fffffffu);
		if (uc_lua_ring[k].from >> 31) uc_lua_proto(uc_lua_ring[k].from & 0x7fffffffu);
	}
	fprintf(stderr, "  L %#x now: G %#x ci %#x (base_ci %#x) top %#x base %#x savedpc %#x\n", L, g, ci, RD32(L + 0x28), RD32(L + 8), RD32(L + 0xc), RD32(L + 0x18));
	if (ci) {
		uint32_t f = RD32(ci + 4), cl = f ? RD32(f) : 0;
		if (f && RD32(f + 4) == 6 && cl && !RD8(cl + 6)) {
			uint32_t p = RD32(cl + 0x10);
			fprintf(stderr, "  running Lua proto %#x at instruction %d\n", p, (int)((RD32(L + 0x18) - RD32(p + 0xc)) / 4) - 1);
			uc_lua_proto(p);
		}
	}
	fprintf(stderr, "  lua_State words:"); for (int i = 0; i < 0x70; i += 4) fprintf(stderr, " %08x", RD32(L + i)); fprintf(stderr, "\n");
	if (g) { fprintf(stderr, "  global_State words:"); for (int i = 0; i < 0x80; i += 4) fprintf(stderr, " %08x", RD32(g + i)); fprintf(stderr, "\n"); }
}
__attribute__((constructor)) static void uc_bad_call_hook(void) { recomp_on_bad_call = uc_bad_call; }
void recomp_hook_uc_lua_call(Cpu *c) {
	uc_lua_check(c, 0);
	if (!uc_lua_window()) return;
	static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
	extern uint64_t display_frames(void); extern const char *recomp_cur_tname(void);
	uint32_t L = c->r[0], fn = c->r[1], ci = RD32(L + 0x14), depth = (ci - RD32(L + 0x28)) / 0x18;
	pthread_mutex_lock(&m);
	fprintf(stderr, "lua call: frame %llu %s L %#x depth %u", (unsigned long long)display_frames(), recomp_cur_tname(), L, depth);
	uint32_t cfn = RD32(ci + 4), ccl = RD32(cfn);   /* the caller: a Lua function (its pc) or C */
	if (RD32(cfn + 4) == 6 && !RD8(ccl + 6)) { uint32_t cp = RD32(ccl + 0x10); fprintf(stderr, " from %#x:%d", cp, (int)((RD32(L + 0x18) - RD32(cp + 0xc)) / 4) - 1); uc_lua_proto(cp); }
	if (RD32(fn + 4) != 6) { fprintf(stderr, " ->"); uc_lua_val(fn); }
	else {
		uint32_t cl = RD32(fn);
		if (RD8(cl + 6)) fprintf(stderr, " -> C %#x", RD32(cl + 0x10));
		else { uint32_t p = RD32(cl + 0x10); fprintf(stderr, " -> L %#x", p); uc_lua_proto(p); }
	}
	fprintf(stderr, " args");
	for (uint32_t a = fn + 8, n = 0; a < RD32(L + 8) && n < 6; a += 8, n++) uc_lua_val(a);
	fprintf(stderr, "\n");
	pthread_mutex_unlock(&m);
}
void recomp_hook_uc_lua_ret(Cpu *c) {
	uc_lua_check(c, 1);
	if (!uc_lua_window()) return;
	extern uint64_t display_frames(void); extern const char *recomp_cur_tname(void);
	uint32_t L = c->r[0], r = c->r[1], depth = (RD32(L + 0x14) - RD32(L + 0x28)) / 0x18;
	fprintf(stderr, "lua ret: frame %llu %s L %#x depth %u", (unsigned long long)display_frames(), recomp_cur_tname(), L, depth);
	for (uint32_t n = 0; r < RD32(L + 8) && n < 4; r += 8, n++) uc_lua_val(r);
	fprintf(stderr, "\n");
}
void recomp_ihook_uc_path_end(Cpu *c, int reg) {
	if (reg < 0 || reg > 14 || c->r[reg] != 0x3b23d70bu || !display_frame_rate_60()) return;
	float k = display_frame_dt() * 30.0f, d = 0.05f * (k < 1.0f ? k : 1.0f), d2 = d * d;
	memcpy(&c->r[reg], &d2, 4);
}
/* Uncharted at 60 fps: the sword's charcoal rubbing never started, now and then (2026-10-15). Tapping one
 * of the sword's sections (fn_81140ae2) starts a zoom camera and a 60-frame wait (+0x278); the sword
 * puzzle's update (fn_811402bc) counts the wait down while the zoom plays or the wait isn't over, and starts
 * that section's charcoal puzzle once the zoom is done and the wait is exactly 0. 60 frames were 2 s at 30
 * fps and the zoom always ended first; at 60 fps they are 1 s, and a zoom that outlasted them took the
 * count past 0 to -1, -2, ... -- the charcoal puzzle never started, and switching back to 30 didn't help.
 * frames30 on the movs of 60 (0x81140b78): 2 s at either rate, as on the console; uc_rub_wait_floor
 * (0x8114033c, the decrement): the count stops at 0, so a zoom that outlasts the wait starts the puzzle
 * as it ends. */
void recomp_ihook_uc_rub_wait_floor(Cpu *c, int reg) {
	if (reg >= 0 && reg <= 14 && (int32_t)c->r[reg] < 0) c->r[reg] = 0;
}
/* Uncharted (PCSA00029) at 60 fps: of its 43 sites building 1/30 s, three ran once per presented frame at
 * both rates in live play (RECOMP_WATCH_COUNT over 600 frames of walking, our test save, 2026-10-10) and get
 * the frame's real time by the same hook -- fn_8128924a (0x81289498, ~90 calls a frame: an object's velocity
 * from its per-frame movement, movement / (1/30): at 60 fps half the true speed, the likely cause of NPCs and
 * Drake sticking on surfaces there, per a player's report), fn_81290b92 (0x8129103e: a step held to at least 1/30
 * s) and fn_81293256 (0x812932c4: a timer += 1/30 a frame). The other sites didn't run in that play (menus,
 * cutscenes, minigames...); fn_813838f6's "30.0" is 2^32 (a 64-bit microsecond count to seconds). */

/*
 * Uncharted (PCSA00029) has the same builder, fn_816babb0 (d0/d2/d4/d6, d0's lanes duplicated by a
 * vdup just before the call). Its fov (2026-10-15): fn_81040eec, the camera manager's per-frame build,
 * takes the active camera object's full vertical fov (+0xc0, radians; near +0xb8, far +0xbc) into the
 * render camera (+0x48, beside the aspect +0x4c, near/far +0x50/+0x54) and its half into the projection
 * built by fn_816babb0 (return address 0x810412e3) and stored at +0x98; the view every pass draws and
 * culls with (fn_8136a80a) copies that camera and multiplies its view matrix (+0x58) by that projection
 * -- checked against frame_1233's render camera: fov 0.8727 (50 deg), near 0.04, far 512, projection
 * [1][1] 2.1445 = 1/tan(25 deg). Gameplay and cutscene cameras alike go through it. uc_proj scales that
 * call's half fov and uc_fov_copy (IHOOK on the vldr at 0x810410bc) the +0x48 copy, both by the "fov"
 * setting; the camera object itself is never touched. The culling follows: every view's frustum planes
 * are made lazily from its view x projection matrix (fn_81270130 -> fn_81712404) -- the objects', the
 * static world's, the lights' and the portals' tests -- and the one other reader of the projection,
 * fn_81370fa0, takes its angle back out of the matrix (+0x98). The PVS list is by position, not view. (An earlier test, 2026-10-10, saw nothing change through
 * New Game to frame 5700 when scaling this call -- a headless run of the intro, not the gameplay
 * camera.)
 *
 * Wider aspect ratios (2026-10-10): fn_816babb0 is every perspective camera's projection builder
 * (half fov d0, aspect d2 -- the literal 960/544 at each call site -- near d4, far d6; GL-style matrix
 * into a scratch buffer each caller copies). uc_proj, at its entry, logs its callers (RECOMP_PROJ_TRACE=1:
 * every caller with its fov / aspect / near / far once per 300 calls); the widening is uc_aspect's, below.
 * RECOMP_UC_ASPECT_CALLERS=lr,lr,... (return addresses) makes uc_proj scale those callers' aspect instead
 * (the first version; it left cutscene culling narrow).
 */
static int uc_aspect_caller(uint32_t lr) {
	static uint32_t list[16]; static int n = -1;
	if (n < 0) {
		n = 0; const char *e = getenv("RECOMP_UC_ASPECT_CALLERS");
		if (!e) e = "0x810412e3";   /* fn_81040eec's world camera; see the probe notes above */
		for (const char *p = e; *p && n < 16;) { char *end; unsigned long v = strtoul(p, &end, 0); if (end == p) break; list[n++] = (uint32_t)v | 1u; p = *end == ',' ? end + 1 : end; }
	}
	for (int i = 0; i < n; i++) if ((list[i] | 1u) == (lr | 1u)) return 1;
	return 0;
}
static volatile float uc_fov_ratio = 1.0f;   /* the render camera's tan(half fov) as drawn over the game's own (uc_proj): the cutscene band (uc_pvs_union) */
void recomp_hook_uc_proj(Cpu *c) {
	uint32_t lr = c->r[14];
	if (getenv("RECOMP_PROJ_TRACE")) {
		static uint32_t seen[64], cnt[64]; static int ns;
		int i = 0; while (i < ns && seen[i] != lr) i++;
		if (i == ns && ns < 64) { seen[ns] = lr; cnt[ns] = 0; ns++; }
		if (i < 64 && cnt[i]++ % 300 == 0)
			fprintf(stderr, "uc proj: lr %#x call %u: fov %.2f deg aspect %.4f near %.3f far %.1f\n", lr, cnt[i], 2.0 * c->vfp.f[0] * (180.0 / M_PI), c->vfp.f[4], c->vfp.f[8], c->vfp.f[12]);
	}
	/* the fov setting (see above) on the render camera's projection and on the four world-to-screen
	 * projections built from the same active camera object (fn_81040eda: +0xc0 fov, +0xb8/+0xbc near/far)
	 * -- fn_810b4354 (a point's screen position, clamped to 960x544: prompts and markers) and the builds at
	 * 0x8115b36c, 0x813f4548 and 0x813f52de -- whose aspect uc_aspect already widens, so what they place
	 * on screen stays on what it marks. The camera manager's one-time setup (0x810427c6) is left alone. */
	static const uint32_t fov_callers[] = {0x810412e3u, 0x810b4457u, 0x8115b371u, 0x813f454du, 0x813f52e3u};
	for (unsigned i = 0; i < sizeof fov_callers / sizeof *fov_callers; i++) if ((lr | 1u) == fov_callers[i]) {
		double f = fov_scale(2.0 * c->vfp.f[0]);
		if (i == 0) uc_fov_ratio = f > 0 ? (float)(tan(f * 0.5) / tan(c->vfp.f[0])) : 1.0f;
		if (f > 0) c->vfp.f[0] = c->vfp.f[1] = (float)(f * 0.5);   /* d0, both lanes */
		break;
	}
	/* the widening itself is uc_aspect's (below), on the constant at its source; scaling here as well would
	 * widen the world camera twice. RECOMP_UC_ASPECT_CALLERS set: this scales those callers instead (tests). */
	if (!getenv("RECOMP_UC_ASPECT_CALLERS")) return;
	double k = aspect_widen();
	if (k <= 1.0 || !uc_aspect_caller(lr)) return;
	float a = (float)(c->vfp.f[4] * k);
	c->vfp.f[4] = a; c->vfp.f[5] = a;   /* d2, both lanes (the caller duplicates its scalar) */
}
/* the render camera's fov copy (+0x48), just loaded into s2 from the camera object (see uc_proj) */
void recomp_ihook_uc_fov_copy(Cpu *c, int reg) {
	(void)reg;
	double f = fov_scale(c->vfp.f[2]);
	if (f > 0) c->vfp.f[2] = (float)f;
}
/* Uncharted builds its screen aspect, 960/544 (0x3fe1e1e2), in a register (movw/movt) at exactly ten
 * places and never keeps it as data (a scan of the whole ELF, 2026-10-10): the world / cutscene camera's
 * projection argument, the copies the camera builder fn_81040eec stores beside the matrix (+0x4c,
 * +0xa8/+0xac -- what the portal visibility reads: widening the projection alone drew a wider picture
 * whose edges cutscenes still culled), a camera-parameters constructor (fn_81234288) and the vector
 * math of several more. packs/PCSA00029/pack.toml's IHOOKS calls this right after each movt: the value
 * becomes 960/544 * aspect_widen(), so every consumer sees the same wider view. */
void recomp_ihook_uc_aspect(Cpu *c, int reg) { widen_aspect_reg(c, reg); }

/*
 * Uncharted: a longer view distance for what the game fades out (2026-10-15). Rocks and foliage on the
 * floor ahead of the player faded in and out a few metres away -- tuned for the Vita's 5-inch screen,
 * plain on a monitor (a player's report and three F12 captures of it, frame_1233/1595/1978). Two parts
 * of the game decide that distance:
 *
 * - the shaders: the static world's fading programs (a vertex program with a g_vFadeoutParams default
 *   uniform beside a fragment program sampling g_tFadeoutMask, a screen-space dither; 3 pairs in the
 *   jungle) take each vertex's distance d to g_vCameraPosition and dither the mesh away by
 *   fade = x*d + y, x and y per mesh: (0.5, -8.1) for the floor clutter, gone between 16.2 and 18.2 m;
 *   (0.5, -10) rocks, 20-22 m; others 32-130 m (traced in the interpreter, frame_1233 draw 754);
 * - the visibility list: the static world (fn_81255972) draws only the renderables listed for the PVS
 *   sample point nearest the camera (a kd-tree of points ~0.5 m apart along the walkable path; each
 *   point's list Huffman-coded and delta-summed, fn_8160924c), and a point's list leaves out whatever
 *   is past its fade end from that point -- baked in: every fading instance listed in the three
 *   captures was within its fade end (the largest overshoot 0.1 m), and the lists differ exactly there.
 *   A longer fade alone would only turn the fade into a pop at the list's edge.
 *
 * view_distance k (settings.ini / F10, 1..3; 1 = the game's own): the list becomes the game's own plus
 * renderables from the lists of the sample points within R = 18 m * sqrt(k^2 - 1) of the camera (50 m
 * and the nearest 512 points at most -- R then the 512th point's distance), and each fading draw's
 * distances are scaled by
 * s = min(k, sqrt(1 + (R/fe)^2), cut/fe), fe its fade end -- no further than the union is sure to reach,
 * even straight beside the path, where it reaches least, nor past the budget's cut (below). The union
 * also lists what the camera's own point had hidden behind walls; the depth test hides it again, at the
 * cost of the draws.
 *
 * The budget: the static world's render items come from the renderer's per-frame pools, bump-allocated
 * with no bound check -- 2048 items (+0xea4/+0xea8, +0xeb0; fn_81244476 resets them each frame). A frame
 * used 225 on the stairs of frame_1233 and 480-545 in the courtyard of frame_6044/6547, whose own list
 * is already 1137 renderables. The first version budgeted every listed item as if all were on screen
 * (1100): 2.0 at R 30 m before that budget had overrun the pools (the worker threads' uniform writer,
 * fn_8122df44, read a precomputed state from past their end and crashed in libgxm), and with it the
 * courtyard's own list alone was over budget, so nothing was added there (our test save's tower,
 * frame_6044/6547). The hook runs where the frame's culling planes are already in the static world
 * (+0x24c0, copied by fn_81255972 before the list), so it counts exactly what fn_81256ef0's jobs can
 * draw: the renderables whose bounding sphere passes those planes, one item per pass of each enabled
 * mesh part. Every frame the extras on screen join nearest first while that stays within the budget,
 * and the fades stop short of the first one left out (uc_view_cut). With the game's 2048 a 1500 budget
 * still cut the courtyard's path at ~30 m (frame_7396/7715: the same fading foliage drawn 3.9 m nearer)
 * -- a third of what it took was within 16 m, so hidden rather than far, since nothing fades that
 * close (now skipped) -- so the pools are made 4x larger (uc_item_pools, below) and the budget is
 * UC_VIEW_ITEMS (3500) or 70% of the pools, less whatever a frame's real total (read at fn_81244476's
 * reset) took past 90% of them: characters and effects use the rest.
 *
 * uc_pvs_union: IHOOK on fn_81255972's store of the camera's cell record to +0x26a8 (0x81255bd6, r9 =
 * the static world, r4 = the record), after the game has decoded that record's list into +0x64 and
 * before its PVSQueryRenderables jobs read it. The jobs take the list's length from the record at
 * +0x26a8 (+4 bits 22-31, +8 bits 30-31), so ours goes in a copy of the record at the list area's own
 * tail (+0x2054: entries 4088-4095, past the 4080 written at most). The game decodes a list only when
 * the camera's point changes (+0x2064 caches it): the union of the nearby points' lists is rebuilt
 * then, and the list written from it every frame.
 */
static volatile float uc_view_k = 1.0f, uc_view_r, uc_view_cut;   /* this frame's view distance, union radius and budget cut, for the fade filter */
static volatile uint64_t uc_view_frame;               /* the frame the list was last written: the filter follows it */
static volatile uint32_t uc_items_peak;               /* the renderer's render items at its resets since the last list */
static double view_distance(void) {
	const char *e = getenv("RECOMP_VIEW_DISTANCE");
	double k = e && *e ? atof(e) : settings_float("view_distance", 1.0);
	return k > 1.0 ? (k < 3.0 ? k : 3.0) : 1.0;
}
/* fn_81244476: the renderer's per-frame reset of its item pools (r0 = the renderer) -- what the frame used */
/* Uncharted's balance beams, steered by tilting the Vita (keyboard + mouse: window.c's tilt): cBalanceMoveController
 * (RTTI N6BSGame22cBalanceMoveControllerE, vtable 0x818c89c0) -- its slot 3, fn_81154ace, runs every frame Drake
 * balances and not otherwise (F9 count windows on the Mac, 2026-10-16: 34 of 34 frames on a beam, 29 of 291 across
 * stepping off, where slot 6 ran once). The game makes no motion call of its own at a beam (no sceMotionReset; its
 * sampling is on from boot), so this is the trigger: each call keeps the tilt section on for a moment -- the mouse
 * or trackpad tilts, a toast says how -- and it lapses a quarter second after the last. */
void recomp_hook_uc_balance_tick(Cpu *c) { (void)c; extern void window_tilt_tick(int kind); window_tilt_tick(1); }
/* ... and the river slide, steered the same way: cMudLugeController (RTTI N6BSGame18cMudLugeControllerE, vtable
 * 0x818c9cd0), the same move-controller base -- its slot 3, fn_811e1016, ran on every frame of a 3218-frame count
 * window down the slide (the canoe's and the balance's slot 3: none) */
void recomp_hook_uc_slide_tick(Cpu *c) { (void)c; extern void window_tilt_tick(int kind); window_tilt_tick(2); }
void recomp_hook_uc_items_reset(Cpu *c) {
	uint32_t n = RD32(c->r[0] + 0xeac);
	if (n > uc_items_peak) uc_items_peak = n;
}
/* The pools themselves, made 4x larger (8192 items): fn_812419d0, the renderer's setup (once, from
 * fn_8124570e), allocates the item storage (+0xea4, 64 bytes an item), the array of pointers to them
 * (+0xea8, filled in pairs by a 0x400-pass loop) and the frame's list (+0xeb0) from the game's heap;
 * uc_item_pools, an IHOOK on the store of the last (0x8124233e, r4 = the renderer), puts larger ones from
 * the host's guest allocator in their place, so the game's own heap stays exactly as it was. Nothing
 * else is sized to 2048: the frame's sort is a qsort of the list (fn_81254684) into at most 16 key
 * buckets, and the objects' matrix buffers (+0xedc, FUN_812445f6) are separate and untouched. */
static volatile uint32_t uc_item_cap = 2048;
void recomp_ihook_uc_item_pools(Cpu *c, int reg) {
	(void)reg;
	enum { K = 4 };
	extern uint32_t guest_alloc(uint32_t size, uint32_t align);
	uint32_t r = c->r[4], n = 2048 * K;
	uint32_t store = guest_alloc(64 * n, 64), ptrs = guest_alloc(4 * n, 16), list = guest_alloc(4 * n, 16);
	memset(G(store), 0, 64 * n); memset(G(list), 0, 4 * n);
	for (uint32_t i = 0; i < n; i++) WR32(ptrs + 4 * i, store + 64 * i);
	WR32(r + 0xea4, store); WR32(r + 0xea8, ptrs); WR32(r + 0xeb0, list);
	uc_item_cap = n;
	fprintf(stderr, "uc view distance: the renderer's item pools hold %u (the game's 2048)\n", n);
}
/* one sample point's list (fn_8160924c's decode and the prefix sum after it), as bits in a 64K-entry set;
 * its length, -1 on data that isn't a list */
static int uc_pvs_decode(uint32_t sw, uint32_t rec, uint8_t *set) {
	uint32_t cnt = (RD32(rec + 8) >> 30) << 10 | RD32(rec + 4) >> 22;
	uint32_t p = RD32(sw + 0x50) + (RD32(rec) & 0xffffff), root = RD32(RD32(sw + 0x54) + 4), v = 0;
	unsigned bit = 0; uint8_t byte = RD8(p);
	for (uint32_t i = 0; i < cnt; i++) {
		uint32_t node = root; int32_t val = (int32_t)RD32(node + 8);
		for (int depth = 0; val < 0; depth++) {
			int b = byte >> (7 - bit) & 1;
			bit = (bit + 1) & 7; if (!bit) byte = RD8(++p);
			node = RD32(node + (b ? 4 : 0));
			if (!node || depth > 32) return -1;
			val = (int32_t)RD32(node + 8);
		}
		v = (v + (uint32_t)val) & 0xffff;
		set[v >> 3] |= (uint8_t)(1u << (v & 7));
	}
	return (int)cnt;
}
/* a list entry (a renderable: +0x4c's 8-byte record, its instance in the high half of word 1, its passes in
 * the low 12 bits) as fn_81256ef0 draws it: its instance's bounding sphere, and one render item per pass
 * each enabled mesh part's material has; 0 items: not loaded (the job skips it) */
typedef struct { float x, y, z, r, near; uint16_t j, items; uint8_t nearby; } UcPvsEnt;
static int uc_pvs_ent(uint32_t sw, uint32_t j, UcPvsEnt *o) {
	uint32_t tb = RD32(sw + 0x24a0), ntab = (RD32(sw + 0x24a4) - tb) / 4, w1 = RD32(RD32(sw + 0x4c) + 8 * j + 4);
	uint32_t e = (w1 >> 16) < ntab ? RD32(tb + 4 * (w1 >> 16)) : 0, hdr = e ? RD32(e + 4) : 0, grp = e ? RD32(e + 8) : 0;
	memset(o, 0, sizeof *o); o->j = (uint16_t)j;
	if (!hdr || !grp) return 0;
	memcpy(&o->x, G(hdr + 0x60), 4); memcpy(&o->y, G(hdr + 0x64), 4); memcpy(&o->z, G(hdr + 0x68), 4); memcpy(&o->r, G(hdr + 0x6c), 4);
	uint32_t np = RD32(grp + 0x10), pmask = RD32(hdr + 8), parts = RD32(grp + 0xc); int n = 0;
	for (uint32_t k = 0; parts && k < np && k < 32; k++) {
		if (!(pmask >> k & 1)) continue;
		uint32_t mat = RD32(parts + 0x28 * k + 0x18);
		if (mat) n += __builtin_popcount(RD32(mat + 0x20) & (w1 & 0xfff));
	}
	o->items = (uint16_t)n;
	return n;
}
static int uc_pvs_near_cmp(const void *a, const void *b) { float x = (*(UcPvsEnt *const *)a)->near, y = (*(UcPvsEnt *const *)b)->near; return x < y ? -1 : x > y; }
static int uc_pvs_j_cmp(const void *a, const void *b) { return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b; }
typedef struct { float d2; uint32_t rec; } UcPvsPoint;
static int uc_pvs_point_cmp(const void *a, const void *b) { float x = ((const UcPvsPoint *)a)->d2, y = ((const UcPvsPoint *)b)->d2; return x < y ? -1 : x > y; }
/* The PVS data is the level's and doesn't change while it is loaded, so each sample point's decoded list
 * is kept (by its cell record), and each renderable's sphere and items (by its list index, checked against
 * the game's table entry it came from -- streaming loads and unloads renderables); a different static
 * world or PVS data drops both. A rebuild then merges cached lists: hundreds of points' lists of ~1200
 * entries each in the courtyard of frame_7396, too many to decode at every step of the camera. */
typedef struct { uint32_t rec, n; uint16_t *v; } UcPvsList;
static UcPvsList uc_lists[8192];
static UcPvsEnt uc_ents[65536]; static uint32_t uc_ent_from[65536];
static uint32_t uc_cache_sw, uc_cache_data;
/* A cutscene's PVS data (2026-10-16): its sample points are its camera's path, one a frame (~0.06 m apart in
 * frame_1963/3587/3734), each listing only what that frame's own view shows -- clipped to the shot. The
 * gameplay data samples the walkable path 0.7-1.1 m apart (frame_1233/7180) and lists what a point can see,
 * so a wider fov there finds what it needs. The median spacing of 64 points tells the two apart. */
static int uc_shot_data;            /* this data is a cutscene's (uc_pvs_is_shot) */
static uint8_t uc_any[8192]; static int uc_any_ok;   /* every renderable any of its points lists, built once */
static int uc_f_cmp(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
static int uc_pvs_is_shot(uint32_t sw) {
	uint32_t n = RD32(sw + 0x3c), nodes = RD32(sw + 0x40);
	if (n < 3) return 0;
	float sp[64]; int ns = 0;
	for (int s = 0; s < 64; s++) {
		uint32_t i = 1 + (uint32_t)((uint64_t)s * (n - 1) / 64);
		float x, y, z, best = INFINITY; memcpy(&x, G(nodes + 16 * i), 4); memcpy(&y, G(nodes + 16 * i + 4), 4); memcpy(&z, G(nodes + 16 * i + 8), 4);
		for (uint32_t j = 1; j < n; j++) {
			if (j == i) continue;
			float px, py, pz; memcpy(&px, G(nodes + 16 * j), 4); memcpy(&py, G(nodes + 16 * j + 4), 4); memcpy(&pz, G(nodes + 16 * j + 8), 4);
			float d2 = (px - x) * (px - x) + (py - y) * (py - y) + (pz - z) * (pz - z);
			if (d2 < best) best = d2;
		}
		sp[ns++] = best;
	}
	qsort(sp, (size_t)ns, sizeof *sp, uc_f_cmp);
	return sqrtf(sp[ns / 2]) < 0.25f;
}
static int uc_table_is_shot(uint32_t sw) {   /* uc_pvs_is_shot, remembered per data set (several tables are live at once) */
	static struct { uint32_t data, n; int shot; } memo[16]; static int nm;
	uint32_t data = RD32(sw + 0x50), n = RD32(sw + 0x3c);
	if (n < 3 || n > 1u << 20 || !RD32(sw + 0x40) || !data) return 0;
	for (int i = 0; i < nm && i < 16; i++) if (memo[i].data == data && memo[i].n == n) return memo[i].shot;
	int shot = uc_pvs_is_shot(sw);
	memo[nm++ % 16] = (typeof(memo[0])){data, n, shot};
	return shot;
}
static void uc_pvs_cache_check(uint32_t sw) {
	uint32_t data = RD32(sw + 0x50);
	if (sw == uc_cache_sw && data == uc_cache_data) return;
	for (int i = 0; i < 8192; i++) { free(uc_lists[i].v); uc_lists[i] = (UcPvsList){0, 0, 0}; }
	memset(uc_ent_from, 0, sizeof uc_ent_from);
	uc_cache_sw = sw; uc_cache_data = data;
	uc_shot_data = uc_table_is_shot(sw); uc_any_ok = 0;
	if (getenv("RECOMP_VIEW_TRACE")) fprintf(stderr, "uc pvs data %#x: %u points, %s\n", data, RD32(sw + 0x3c), uc_shot_data ? "a cutscene's (its camera path)" : "the walkable path's");
}
/* the game's own view from the drawn one (the cutscene band, the strip): the drawn side planes (2-5 of +0x24c0's six)
 * turned back in about the camera (cx, cy, cz) to the game's own fov and aspect -- the wider pair (the
 * horizontal) by kh, the other by kv; near and far as they are */
static void uc_game_view(const float pl[6][4], float cx, float cy, float cz, float kh, float kv, float gv[6][4]) {
	memcpy(gv, pl, sizeof(float) * 24);
	float f0 = sqrtf(pl[0][0] * pl[0][0] + pl[0][1] * pl[0][1] + pl[0][2] * pl[0][2]);
	if (!(f0 > 0)) return;
	float fw[3] = {pl[0][0] / f0, pl[0][1] / f0, pl[0][2] / f0};   /* the near plane faces forward */
	float s2 = pl[2][0] * fw[0] + pl[2][1] * fw[1] + pl[2][2] * fw[2], s4 = pl[4][0] * fw[0] + pl[4][1] * fw[1] + pl[4][2] * fw[2];
	for (int i = 2; i < 6; i++) {
		float kk = (i < 4) == (s2 >= s4) ? kh : kv;
		float s = pl[i][0] * fw[0] + pl[i][1] * fw[1] + pl[i][2] * fw[2], sd[3] = {pl[i][0] - s * fw[0], pl[i][1] - s * fw[1], pl[i][2] - s * fw[2]};
		float sl = sqrtf(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
		if (!(sl > 1e-6f) || s <= 0) continue;   /* (not a side plane: left as it is) */
		float h0 = atanf(tanf(atan2f(s, sl)) / kk), a = sinf(h0), b = cosf(h0) / sl;   /* the half-angle that plane makes with forward, the game's own */
		for (int j = 0; j < 3; j++) gv[i][j] = a * fw[j] + b * sd[j];
		gv[i][3] = -(gv[i][0] * cx + gv[i][1] * cy + gv[i][2] * cz);   /* through the camera, as the drawn one */
	}
}
/* a sphere wholly inside the side planes (2-5) of a view: what the game's own view decides alone. A piece that
 * crosses its edge is the band's or the strip's -- the game's list, made for its own frame, may leave out a piece
 * whose part inside was hidden there although its part past the edge is in plain sight now (RECOMP_POP_TRACE,
 * 2026-10-16: all 579 pops of a camera move were pieces touching that edge -- they vanished as they slid onto it) */
static int uc_sphere_within(const float (*p)[4], const UcPvsEnt *e) {
	for (int i = 2; i < 6; i++) if (p[i][0] * e->x + p[i][1] * e->y + p[i][2] * e->z + p[i][3] < e->r) return 0;
	return 1;
}
/* a sphere against planes (inside or crossing every one) */
static int uc_sphere_in(const float (*p)[4], int np, const UcPvsEnt *e) {
	for (int i = 0; i < np; i++) if (p[i][0] * e->x + p[i][1] * e->y + p[i][2] * e->z + p[i][3] < -e->r) return 0;
	return 1;
}
static const UcPvsList *uc_pvs_list(uint32_t sw, uint32_t rec) {   /* NULL: not a list */
	unsigned h = (rec >> 4) * 2654435761u >> 19;
	for (int probe = 0; probe < 8192; probe++, h = (h + 1) & 8191) {
		if (uc_lists[h].rec == rec) return &uc_lists[h];
		if (uc_lists[h].rec) continue;
		static uint8_t one[8192]; memset(one, 0, sizeof one);
		int n = uc_pvs_decode(sw, rec, one);
		if (n < 0) return NULL;
		uint16_t *v = malloc(sizeof *v * (size_t)(n ? n : 1)); uint32_t w = 0;
		for (uint32_t x = 0; x < 65536 && w < (uint32_t)n; x++) if (one[x >> 3] >> (x & 7) & 1) v[w++] = (uint16_t)x;
		uc_lists[h] = (UcPvsList){rec, w, v};
		return &uc_lists[h];
	}
	return NULL;   /* full: a level with this many points near the camera doesn't exist; the game's list */
}
static const UcPvsEnt *uc_pvs_ent_cached(uint32_t sw, uint32_t j) {
	uint32_t tb = RD32(sw + 0x24a0), ntab = (RD32(sw + 0x24a4) - tb) / 4, w1 = RD32(RD32(sw + 0x4c) + 8 * j + 4);
	uint32_t e = (w1 >> 16) < ntab ? RD32(tb + 4 * (w1 >> 16)) : 0;
	if (!e || uc_ent_from[j] != e) { uc_pvs_ent(sw, j, &uc_ents[j]); uc_ent_from[j] = e; }
	return &uc_ents[j];
}
static void uc_fade_filter(const UsseProgram *p, uint32_t prog, UsseDraw *d, int base14, int size14) {
	extern uint64_t display_frames(void);
	float k = uc_view_k;
	if (k <= 1.0f || display_frames() > uc_view_frame + 2) return;   /* no union this frame: the game's fade */
	static uint64_t cache[256];   /* program content hash << 32 | the parameter's resource + 1 (0: none), one word so threads can't mix two */
	uint32_t key = usse_program_hash(p) | 1u; uint64_t e = __atomic_load_n(&cache[key >> 24], __ATOMIC_RELAXED);
	if ((uint32_t)(e >> 32) != key) {
		int ct = -1, r = usse_param_resource(prog, "g_vFadeoutParams", &ct);
		e = (uint64_t)key << 32 | (uint32_t)(ct == 14 && r >= 0 ? r + 1 : 0);
		__atomic_store_n(&cache[key >> 24], e, __ATOMIC_RELAXED);
	}
	int res = (int)(uint32_t)e - 1;
	if (res < 0 || res + 1 >= size14 || base14 + res + 1 >= 256) return;
	UWord *x = &d->sa[base14 + res], *y = &d->sa[base14 + res + 1];
	float fe = x->f > 0 ? (1.0f - y->f) / x->f : 0, r = uc_view_r;
	if (!(fe > 0)) return;
	float s = sqrtf(1.0f + (r / fe) * (r / fe));
	if (s > k) s = k;
	if (s * fe > uc_view_cut) s = uc_view_cut / fe;   /* not past the budget's cut: nothing beyond it is listed */
	if (s > 1.0f) x->f /= s;
}
/*
 * The level's scenery past a cutscene's frame (2026-10-16). A cutscene doesn't draw the level's own static
 * world: the cutscene's zone, with its own smaller set (frame_1987: 496 instances against the level's 1765,
 * 483 of them the very same pieces), draws instead, the level's zone isn't visited at all (its table has no
 * query point or planes during the cutscene; the layer check in its update, FUN_8134f6d4 -> FUN_8123e440,
 * would pass: frame_2103's layer slots 0/1/1). That set holds only what its shots see, so past the game's
 * own frame a wider view found nothing to draw there -- even with the band above, the tree backdrop showed
 * through a crack at the right edge (138 level instances in that strip, all loaded, none in the set).
 *
 * uc_zone_done (IHOOKS, in a zone's per-view update FUN_8134f6d4 after its call of FUN_812563d0, the table's
 * lights + scenery update: r4 = the view, r5 = the table's handle) runs, after a cutscene table's update for
 * the main view, the scenery update fn_81255972 (table, view + 0x10 = its position, view + 0xf0 = its
 * frustum) of each live level table (uc_pvs_new) that shares the set's pieces and that the game didn't
 * update itself, the cutscene table's lights copied in first (+0x246c..+0x247c, +0x2494 -- each table
 * lights its own scenery with up to four lights; the level's own light update, FUN_812560ae, is not run: it
 * would add the level's lights to the cutscene's). Its list (uc_pvs_strip, from uc_pvs_union) is only the
 * level's instances past the game's own view and inside the drawn one that the set doesn't have (the same
 * sphere), so everything inside the game's frame stays the cutscene's.
 */
static __thread uint32_t uc_strip_tab, uc_strip_shot;   /* the level table being run for the strip, and the cutscene's */
static void uc_table_drawn(uint32_t tab);
static uint64_t uc_sphere_key(const float *s) {   /* an instance's bounding sphere to 1 cm */
	uint64_t k = 1469598103934665603ull;
	for (int i = 0; i < 4; i++) { int32_t q = (int32_t)lrintf(s[i] * 100.0f); k = (k ^ (uint32_t)q) * 1099511628211ull; }
	return k;
}
static int uc_u64_cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static const uint64_t *uc_shot_keys(uint32_t shot, uint32_t *n) {   /* the cutscene set's pieces by sphere, sorted (once per set; one thread) */
	static uint64_t keys[16384]; static uint32_t nkeys, keys_shot, keys_data;
	if (shot != keys_shot || RD32(shot + 0x50) != keys_data) {
		uint32_t tb = RD32(shot + 0x24a0), nt = (RD32(shot + 0x24a4) - tb) / 4; nkeys = 0;
		for (uint32_t t = 0; t < nt && nkeys < 16384; t++) {
			uint32_t e = RD32(tb + 4 * t), h = e ? RD32(e + 4) : 0; float sp[4];
			if (h) { memcpy(sp, G(h + 0x60), 16); keys[nkeys++] = uc_sphere_key(sp); }
		}
		qsort(keys, nkeys, sizeof *keys, uc_u64_cmp);
		keys_shot = shot; keys_data = RD32(shot + 0x50);
	}
	*n = nkeys; return keys;
}
/* RECOMP_POP_TRACE=1 (diagnostic): per scenery table, a piece listed in its last update that is still in this
 * update's drawn view but no longer listed -- a pop -- with where it is (inside the game's own view, past it)
 * and whether it is loaded; and a table updated more than once in a frame (several views sharing its one list) */
static void uc_pop_check(uint32_t sw, const uint16_t *list, uint32_t n, const char *who) {
	static int on = -1; if (on < 0) on = getenv("RECOMP_POP_TRACE") != 0;
	if (!on) return;
	extern uint64_t display_frames(void);
	static struct { uint32_t sw; uint64_t frame; uint8_t set[8192]; } st[4]; static int next;
	int k = -1;
	for (int i = 0; i < 4; i++) if (st[i].sw == sw) k = i;
	if (k < 0) { k = next++ & 3; st[k].sw = sw; st[k].frame = 0; memset(st[k].set, 0, sizeof st[k].set); }
	uint64_t f = display_frames();
	static uint8_t now[8192]; memset(now, 0, sizeof now);
	for (uint32_t i = 0; i < n; i++) now[list[i] >> 3] |= (uint8_t)(1u << (list[i] & 7));
	if (st[k].frame == f) fprintf(stderr, "uc pop: frame %llu table %#x (%s) updated again in the same frame (another view)\n", (unsigned long long)f, sw, who);
	float pl[6][4], gv[6][4]; memcpy(pl, G(sw + 0x24c0), sizeof pl);
	float cx, cy, cz; memcpy(&cx, G(sw + 0x24b0), 4); memcpy(&cy, G(sw + 0x24b4), 4); memcpy(&cz, G(sw + 0x24b8), 4);
	float kv = uc_fov_ratio, kh = kv * (float)aspect_widen();
	uc_game_view((const float (*)[4])pl, cx, cy, cz, kh, kv, gv);
	int logged = 0, drops = 0;
	if (st[k].frame + 1 == f || st[k].frame == f)
		for (uint32_t v = 0; v < 65536; v++) {
			if (!(st[k].set[v >> 3] >> (v & 7) & 1) || now[v >> 3] >> (v & 7) & 1) continue;
			UcPvsEnt e; int items = uc_pvs_ent(sw, v, &e);
			if (!uc_sphere_in((const float (*)[4])pl, 6, &e)) continue;   /* left the view: not a pop */
			drops++;
			if (logged++ < 6) fprintf(stderr, "uc pop: frame %llu table %#x (%s) dropped piece %u at %.1f m (r %.1f), %s, %s\n", (unsigned long long)f, sw, who, v,
			                          sqrtf((e.x - cx) * (e.x - cx) + (e.y - cy) * (e.y - cy) + (e.z - cz) * (e.z - cz)), e.r,
			                          uc_sphere_within((const float (*)[4])gv, &e) ? "wholly inside the game's own view" : uc_sphere_in((const float (*)[4])gv, 6, &e) ? "across the game's own view's edge" : "past the game's own view", items ? "loaded" : "NOT loaded");
		}
	if (drops > 6) fprintf(stderr, "uc pop: frame %llu table %#x (%s) ... %d dropped in all (list %u)\n", (unsigned long long)f, sw, who, drops, n);
	memcpy(st[k].set, now, sizeof now); st[k].frame = f;
}
static void uc_pvs_strip(Cpu *c, uint32_t sw, uint32_t shot) {
	enum { MAXREC = 65536, MAXLIST = 4080 };
	uint32_t rec = c->r[4], list = sw + 0x64, fake = sw + 0x2054;
	uint32_t nkeys; const uint64_t *keys = uc_shot_keys(shot, &nkeys);
	/* the level's records: sphere, and whether the cutscene set has that piece (once per pair) */
	typedef struct { float x, y, z, r; uint8_t dup; } LRec;
	static LRec lr[MAXREC]; static uint32_t nlr, lr_tab, lr_data, lr_keys;
	if (sw != lr_tab || RD32(sw + 0x50) != lr_data || shot != lr_keys) {
		uint32_t tb = RD32(sw + 0x24a0), nt = (RD32(sw + 0x24a4) - tb) / 4, recs = RD32(sw + 0x4c), n = RD32(sw + 0x48);
		nlr = n < MAXREC ? n : MAXREC;
		for (uint32_t j = 0; j < nlr; j++) {
			uint32_t t = RD32(recs + 8 * j + 4) >> 16, e = t < nt ? RD32(tb + 4 * t) : 0, h = e ? RD32(e + 4) : 0;
			lr[j].r = -1; lr[j].dup = 1;
			if (!h) continue;
			float sp[4]; memcpy(sp, G(h + 0x60), 16);
			uint64_t k = uc_sphere_key(sp);
			lr[j] = (LRec){sp[0], sp[1], sp[2], sp[3], bsearch(&k, keys, nkeys, sizeof *keys, uc_u64_cmp) != NULL};
		}
		lr_tab = sw; lr_data = RD32(sw + 0x50); lr_keys = shot;
	}
	float pl[6][4], gv[6][4]; memcpy(pl, G(sw + 0x24c0), sizeof pl);
	float cx, cy, cz; memcpy(&cx, G(sw + 0x24b0), 4); memcpy(&cy, G(sw + 0x24b4), 4); memcpy(&cz, G(sw + 0x24b8), 4);
	float kv = uc_fov_ratio, kh = kv * (float)aspect_widen();
	uc_game_view((const float (*)[4])pl, cx, cy, cz, kh, kv, gv);
	static UcPvsEnt ents[MAXLIST]; static UcPvsEnt *pick[MAXLIST]; uint32_t ne = 0, cands = 0;
	for (uint32_t j = 0; j < nlr && ne < MAXLIST; j++) {
		if (lr[j].dup || lr[j].r < 0) continue;
		UcPvsEnt t = {lr[j].x, lr[j].y, lr[j].z, lr[j].r, 0, 0, 0, 0};
		if (!uc_sphere_in((const float (*)[4])pl, 6, &t) || uc_sphere_within((const float (*)[4])gv, &t)) continue;   /* in the drawn view, past or across the game's own view's edge */
		cands++;
		if (!uc_pvs_ent(sw, j, &ents[ne])) continue;   /* not loaded */
		ents[ne].near = sqrtf((t.x - cx) * (t.x - cx) + (t.y - cy) * (t.y - cy) + (t.z - cz) * (t.z - cz)) - t.r;
		pick[ne] = &ents[ne]; ne++;
	}
	qsort(pick, ne, sizeof *pick, uc_pvs_near_cmp);
	int budget = (int)(uc_item_cap / 5), items = 0; uint32_t took = 0;   /* a fifth of the pools: the cutscene's own budget keeps the rest */
	static uint16_t take[MAXLIST];
	for (; took < ne && items + pick[took]->items <= budget; took++) { items += pick[took]->items; take[took] = pick[took]->j; }
	qsort(take, took, sizeof *take, uc_pvs_j_cmp);
	memcpy(G(list), take, 2 * took);
	WR32(fake, RD32(rec)); WR32(fake + 12, RD32(rec + 12));
	WR32(fake + 4, (RD32(rec + 4) & 0x3fffffu) | (took & 0x3ffu) << 22);
	WR32(fake + 8, (RD32(rec + 8) & 0x3fffffffu) | (took >> 10) << 30);
	WR32(sw + 0x26a8, fake);
	WR32(sw + 0x2064, 0);   /* the list there is ours: the game decodes its own again on its next real update */
	uc_pop_check(sw, take, took, "level strip");
	static uint64_t last_trace; extern uint64_t display_frames(void); uint64_t now = display_frames();
	if (getenv("RECOMP_VIEW_TRACE") && now >= last_trace + 120) {
		last_trace = now;
		fprintf(stderr, "uc cutscene strip: level table %#x (%u pieces, the cutscene's %u): +%u of %u past the game's own view (%u loaded), %d items\n", sw, nlr, nkeys, took, cands, ne, items);
	}
}
void recomp_ihook_uc_pvs_union(Cpu *c, int reg) {
	(void)reg;
	extern uint64_t display_frames(void);
	enum { UC_VIEW_ITEMS = 3500, UC_MAX_POINTS = 512, UC_MAX_LIST = 4080, UC_MAX_EXT = 16384 };
	const float UC_NEAR_MIN = 15.0f;   /* nothing fades this close (the shortest fade ends at 16 m): an extra
	                                    * nearer than this was left out for being hidden, not far -- skip it */
	uint32_t cap = uc_item_cap, pool_safe = cap - cap / 10;   /* the pools' size (4x since the setup hook) and a frame's safe use of it */
	static int registered; if (!registered) { registered = 1; usse_set_uniform_filter(uc_fade_filter); }
	if (uc_strip_tab && c->r[9] == uc_strip_tab) { uc_pvs_strip(c, uc_strip_tab, uc_strip_shot); return; }   /* uc_zone_done's run of a hidden level table */
	uc_table_drawn(c->r[9]);   /* (the game's own update of it: uc_zone_done leaves a table the game draws alone) */
	uint32_t sw = c->r[9], rec = c->r[4], list = sw + 0x64, fake = sw + 0x2054;
	static uint32_t last_sw, last_rec, live_n, n_own, n_ext; static uint16_t live_list[4096]; static double last_k; static int live, budget_cut, last_band;
	static UcPvsEnt own[4096], ext[UC_MAX_EXT]; static float rad; static int pts;
	uint32_t items_last = uc_items_peak; uc_items_peak = 0;
	double k = view_distance();
	/* our last list still in place: the game decodes over it only when the camera's point changes */
	int in_place = live && sw == last_sw && memcmp(G(list), live_list, 2 * live_n) == 0;
	live = 0; uc_view_k = 1.0f;
	/* off, or no list of ours this time: one in place still serves this frame (with its own length), and
	 * the game decodes its own list again on the next */
#define UC_PVS_GAME_LIST() do { if (in_place) { WR32(sw + 0x26a8, fake); WR32(sw + 0x2064, 0); } return; } while (0)
	/* The cutscene band (2026-10-16): a cutscene's lists stop at its own frame's edges (uc_pvs_is_shot), so a
	 * wider fov or aspect ratio showed its static world cut off down both sides (frame_1963/3587/3734: of the
	 * renderables in the added strip, 0 of 25, 7 of 80 and 0 of 18 listed; inside the game's own view, half
	 * or more). There the strip is filled from everything the cutscene's points list anywhere (so it is all
	 * the cutscene's own, loaded geometry), the depth test hiding what is behind something; inside the
	 * game's own view, its list stands as it is. */
	uint32_t n = RD32(sw + 0x3c), nodes = RD32(sw + 0x40), recs = RD32(sw + 0x44);
	int valid = n >= 2 && n <= 1u << 20 && nodes && recs && RD32(sw + 0x50) && RD32(sw + 0x54);   /* sample points */
	if (valid) uc_pvs_cache_check(sw);
	float kv = uc_fov_ratio, kh = kv * (float)aspect_widen();   /* how much wider the drawn view is than the game's, vertically and across */
	int band = valid && uc_shot_data && (kv > 1.001f || kh > 1.001f);
	if (k <= 1.0 && !band) UC_PVS_GAME_LIST();
	float cx, cy, cz; memcpy(&cx, G(sw + 0x24b0), 4); memcpy(&cy, G(sw + 0x24b4), 4); memcpy(&cz, G(sw + 0x24b8), 4);   /* fn_81255808's query point: the camera */
	int rebuilt = 0;
	if (!(in_place && rec == last_rec && k == last_k && band == last_band)) {   /* the union of the nearby points' lists, again */
		if (!valid) UC_PVS_GAME_LIST();
		rad = k > 1.0 ? (float)(18.0 * sqrt(k * k - 1.0)) : 0.0f; if (rad > 50.0f) rad = 50.0f;
		static uint8_t mine[8192], set[8192]; memset(mine, 0, sizeof mine); memset(set, 0, sizeof set);
		if (band && !uc_any_ok) {   /* the cutscene's every list, once */
			memset(uc_any, 0, sizeof uc_any);
			for (uint32_t i = 1; i < n; i++) {
				const UcPvsList *l = uc_pvs_list(sw, recs + ((RD32(nodes + 16 * i + 12) & 0x3fffffff) >> 2) * 16);
				if (l) for (uint32_t x = 0; x < l->n; x++) uc_any[l->v[x] >> 3] |= (uint8_t)(1u << (l->v[x] & 7));
			}
			uc_any_ok = 1;
		}
		const UcPvsList *ol = uc_pvs_list(sw, rec);
		if (!ol) UC_PVS_GAME_LIST();
		for (uint32_t i = 0; i < ol->n; i++) mine[ol->v[i] >> 3] |= (uint8_t)(1u << (ol->v[i] & 7));
		static UcPvsPoint npts[65536]; uint32_t nn = 0;
		for (uint32_t i = 1; i < n && nn < 65536; i++) {
			uint32_t nd = nodes + 16 * i; float px, py, pz;
			memcpy(&px, G(nd), 4); memcpy(&py, G(nd + 4), 4); memcpy(&pz, G(nd + 8), 4);
			float d2 = (px - cx) * (px - cx) + (py - cy) * (py - cy) + (pz - cz) * (pz - cz);
			uint32_t r2 = recs + ((RD32(nd + 12) & 0x3fffffff) >> 2) * 16;
			if (d2 <= rad * rad && r2 != rec) npts[nn++] = (UcPvsPoint){d2, r2};
		}
		qsort(npts, nn, sizeof *npts, uc_pvs_point_cmp);
		pts = 1;
		for (uint32_t i = 0; i < nn && i < UC_MAX_POINTS; i++, pts++) {
			const UcPvsList *l = uc_pvs_list(sw, npts[i].rec);
			if (!l) UC_PVS_GAME_LIST();
			for (uint32_t x = 0; x < l->n; x++) set[l->v[x] >> 3] |= (uint8_t)(1u << (l->v[x] & 7));
		}
		if (nn > UC_MAX_POINTS) rad = sqrtf(npts[UC_MAX_POINTS - 1].d2);   /* how far the points decoded reach */
		n_own = n_ext = 0;
		for (uint32_t v = 0; v < 65536; v++) {
			if (mine[v >> 3] >> (v & 7) & 1) { if (n_own < 4096) own[n_own++] = *uc_pvs_ent_cached(sw, v); continue; }
			int near_pt = set[v >> 3] >> (v & 7) & 1;
			if ((near_pt || (band && uc_any[v >> 3] >> (v & 7) & 1)) && n_ext < UC_MAX_EXT) {
				const UcPvsEnt *e = uc_pvs_ent_cached(sw, v);
				if (e->items) { ext[n_ext] = *e; ext[n_ext++].nearby = (uint8_t)near_pt; }
			}
		}
		if (n_own > UC_MAX_LIST) UC_PVS_GAME_LIST();
		last_sw = sw; last_rec = rec; last_k = k; last_band = band; rebuilt = 1;
	}
	/* this frame: the renderables on screen (the planes the jobs test with), the game's own first */
	if (items_last > pool_safe) budget_cut += (int)(items_last - pool_safe);
	else if (budget_cut > 0) budget_cut -= budget_cut < 4 ? budget_cut : 4;
	int budget = (int)(UC_VIEW_ITEMS < cap * 7 / 10 ? UC_VIEW_ITEMS : cap * 7 / 10) - budget_cut;
	float pl[6][4]; memcpy(pl, G(sw + 0x24c0), sizeof pl);
#define UC_IN_VIEW(e) (pl[0][0] * (e)->x + pl[0][1] * (e)->y + pl[0][2] * (e)->z + pl[0][3] >= -(e)->r && pl[1][0] * (e)->x + pl[1][1] * (e)->y + pl[1][2] * (e)->z + pl[1][3] >= -(e)->r && \
	pl[2][0] * (e)->x + pl[2][1] * (e)->y + pl[2][2] * (e)->z + pl[2][3] >= -(e)->r && pl[3][0] * (e)->x + pl[3][1] * (e)->y + pl[3][2] * (e)->z + pl[3][3] >= -(e)->r && \
	pl[4][0] * (e)->x + pl[4][1] * (e)->y + pl[4][2] * (e)->z + pl[4][3] >= -(e)->r && pl[5][0] * (e)->x + pl[5][1] * (e)->y + pl[5][2] * (e)->z + pl[5][3] >= -(e)->r)
	int items = 0;
	for (uint32_t i = 0; i < n_own; i++) if (own[i].items && UC_IN_VIEW(&own[i])) items += own[i].items;
	int own_items = items;
	float gv[6][4];   /* the game's own view (band) */
	if (band) uc_game_view((const float (*)[4])pl, cx, cy, cz, kh, kv, gv); else memcpy(gv, pl, sizeof gv);
	static UcPvsEnt *cand[UC_MAX_EXT], *edge[UC_MAX_EXT]; uint32_t nc = 0, ne = 0;
	for (uint32_t i = 0; i < n_ext; i++) {
		UcPvsEnt *e = &ext[i];
		if (!UC_IN_VIEW(e)) continue;
		e->near = sqrtf((e->x - cx) * (e->x - cx) + (e->y - cy) * (e->y - cy) + (e->z - cz) * (e->z - cz)) - e->r;
		if (band && !uc_sphere_within((const float (*)[4])gv, e)) { edge[ne++] = e; continue; }   /* past or across the game's own view's edge: the strip */
		if (k <= 1.0 || !e->nearby || e->near < UC_NEAR_MIN) continue;
		cand[nc++] = e;
	}
#undef UC_IN_VIEW
	static uint16_t take[UC_MAX_EXT]; uint32_t took = 0, edged = 0; float cut = INFINITY;
	qsort(edge, ne, sizeof *edge, uc_pvs_near_cmp);   /* the strip first, nearest first: it is in plain view */
	for (; edged < ne && items + edge[edged]->items <= budget && n_own + edged < UC_MAX_LIST; edged++) { items += edge[edged]->items; take[edged] = edge[edged]->j; }
	took = edged;
	qsort(cand, nc, sizeof *cand, uc_pvs_near_cmp);
	for (uint32_t ci = 0; ci < nc; ci++, took++) {
		if (items + cand[ci]->items > budget || n_own + took >= UC_MAX_LIST) { cut = cand[ci]->near > 0 ? cand[ci]->near : 0; break; }
		items += cand[ci]->items; take[took] = cand[ci]->j;
	}
	qsort(take, took, sizeof *take, uc_pvs_j_cmp);
	uint32_t w = 0, a = 0, b = 0;   /* the game's own list and ours, merged in order */
	while (a < n_own || b < took) {
		uint16_t v = b >= took || (a < n_own && own[a].j < take[b]) ? own[a++].j : take[b++];
		live_list[w++] = v;
	}
#undef UC_PVS_GAME_LIST
	if (!in_place || memcmp(G(list), live_list, 2 * w) != 0 || w != live_n) memcpy(G(list), live_list, 2 * w);
	WR32(fake, RD32(rec)); WR32(fake + 12, RD32(rec + 12));
	WR32(fake + 4, (RD32(rec + 4) & 0x3fffffu) | (w & 0x3ffu) << 22);
	WR32(fake + 8, (RD32(rec + 8) & 0x3fffffffu) | (w >> 10) << 30);
	WR32(sw + 0x26a8, fake);
	live = 1; live_n = w;
	uc_pop_check(sw, live_list, w, band ? "cutscene set" : "level");
	uc_view_r = rad; uc_view_cut = cut; uc_view_k = (float)k; uc_view_frame = display_frames();
	static int told; static uint64_t last_trace; uint64_t now = display_frames();
	if (!told++ || (getenv("RECOMP_VIEW_TRACE") && (rebuilt || now >= last_trace + 120))) {
		last_trace = now;
		fprintf(stderr, "uc view distance %.2f: %s%d points within %.1f m; own %u renderables (%d items on screen), +%u of %u extra on screen -> %d items (budget %d, last frame used %u of %u), cut %.1f m\n",
		        k, rebuilt ? "rebuilt, " : "", pts, rad, n_own, own_items, took - edged, nc, items, budget, items_last, cap, cut);
		if (band) fprintf(stderr, "uc cutscene band (view %.2fx across, %.2fx up): +%u of %u renderables past the game's own view\n", kh, kv, edged, ne);
	}
}
/* the scenery sets alive (the strip, above): BSGfx::cPVSTable's constructor FUN_81255338 and its two destructors
 * FUN_81255498 / FUN_812555b8 (HOOKS, r0 = the table) keep this list -- a cutscene can begin straight after a load,
 * the level's zone never drawn and so never seen any other way (our test save, 2026-10-16: frame_2103) */
static uint32_t uc_tables[32]; static pthread_mutex_t uc_tables_mx = PTHREAD_MUTEX_INITIALIZER;
void recomp_hook_uc_pvs_new(Cpu *c) {
	pthread_mutex_lock(&uc_tables_mx);
	for (int i = 0; i < 32; i++) if (!uc_tables[i] || uc_tables[i] == c->r[0]) { uc_tables[i] = c->r[0]; break; }
	pthread_mutex_unlock(&uc_tables_mx);
}
void recomp_hook_uc_pvs_free(Cpu *c) {
	pthread_mutex_lock(&uc_tables_mx);
	for (int i = 0; i < 32; i++) if (uc_tables[i] == c->r[0]) uc_tables[i] = 0;
	pthread_mutex_unlock(&uc_tables_mx);
}
static uint32_t uc_handle_obj(uint32_t h) { return h ? RD32(h + 0x28) : 0; }   /* FUN_8165d494's handle -> object (+0x28) */
static int uc_is_table(uint32_t t) { return t && RD32(t) == 0x818cdc18u; }   /* BSGfx::cPVSTable's vtable */
/* the share of a cutscene set's pieces a table also has (the same sphere), once per pair: the level the set was cut
 * from has nearly all of them (479 of 496, frame_2103) */
static uint32_t uc_overlap(uint32_t tab, uint32_t shot) {
	static struct { uint32_t tab, data, shot, sdata, n; } memo[16]; static int nm;
	uint32_t data = RD32(tab + 0x50), sdata = RD32(shot + 0x50);
	for (int i = 0; i < nm && i < 16; i++) if (memo[i].tab == tab && memo[i].data == data && memo[i].shot == shot && memo[i].sdata == sdata) return memo[i].n;
	uint32_t nk, n = 0; const uint64_t *keys = uc_shot_keys(shot, &nk);
	uint32_t tb = RD32(tab + 0x24a0), nt = tb ? (RD32(tab + 0x24a4) - tb) / 4 : 0;
	for (uint32_t t = 0; t < nt && t < 65536; t++) {
		uint32_t e = RD32(tb + 4 * t), h = e ? RD32(e + 4) : 0; float sp[4];
		if (!h) continue;
		memcpy(sp, G(h + 0x60), 16); uint64_t k = uc_sphere_key(sp);
		if (bsearch(&k, keys, nk, sizeof *keys, uc_u64_cmp)) n++;
	}
	memo[nm++ % 16] = (typeof(memo[0])){tab, data, shot, sdata, n};
	return n;
}
static struct { uint32_t tab; uint64_t frame; } uc_drawn[16];   /* tables the game updated itself, and when */
static void uc_table_drawn(uint32_t tab) {
	extern uint64_t display_frames(void); uint64_t f = display_frames(); int k = 0;
	for (int i = 0; i < 16; i++) { if (uc_drawn[i].tab == tab) { k = i; break; } if (uc_drawn[i].frame < uc_drawn[k].frame) k = i; }
	uc_drawn[k].tab = tab; uc_drawn[k].frame = f;
}
void recomp_ihook_uc_zone_done(Cpu *c, int reg) {
	(void)reg;
	extern uint64_t display_frames(void); extern GuestFn guest_lookup(uint32_t a);
	uint32_t view = c->r[4], shot = uc_handle_obj(c->r[5]);
	if (!uc_is_table(shot) || !uc_table_is_shot(shot)) return;
	uint64_t f = display_frames();
	float kv = uc_fov_ratio, kh = kv * (float)aspect_widen();
	if (!(kv > 1.001f || kh > 1.001f) || uc_strip_tab) return;
	if (getenv("RECOMP_NO_CUTSCENE_STRIP")) return;
	/* the main view only: its side planes' tangents in the drawn picture's proportion (a shadow view isn't) */
	float pl[6][4]; memcpy(pl, G(shot + 0x24c0), sizeof pl);
	float f0 = sqrtf(pl[0][0] * pl[0][0] + pl[0][1] * pl[0][1] + pl[0][2] * pl[0][2]);
	if (!(f0 > 0)) return;
	float t[2];
	for (int i = 0; i < 2; i++) {
		const float *p = pl[2 + 2 * i]; float sn = (p[0] * pl[0][0] + p[1] * pl[0][1] + p[2] * pl[0][2]) / f0, nn = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
		float cs2 = nn * nn - sn * sn; if (!(sn > 0 && cs2 > 0)) return;
		t[i] = sn / sqrtf(cs2);   /* tan of that side's half-angle */
	}
	float want = 960.0f / 544.0f * (float)aspect_widen(), got = t[0] > t[1] ? t[0] / t[1] : t[1] / t[0];
	if (fabsf(got / want - 1.0f) > 0.03f) return;
	GuestFn fn = guest_lookup(0x81255972u);
	if (!fn) return;
	uint32_t tabs[32]; pthread_mutex_lock(&uc_tables_mx); memcpy(tabs, uc_tables, sizeof tabs); pthread_mutex_unlock(&uc_tables_mx);
	uint32_t nk; uc_shot_keys(shot, &nk);
	for (int i = 0; i < 32; i++) {
		uint32_t lvl = tabs[i];
		if (!lvl || lvl == shot || !uc_is_table(lvl) || uc_table_is_shot(lvl)) continue;
		uint32_t pn = RD32(lvl + 0x3c);
		if (pn < 2 || pn > 1u << 20 || !RD32(lvl + 0x40) || !RD32(lvl + 0x44) || !RD32(lvl + 0x50) || !RD32(lvl + 0x54) || !RD32(lvl + 0x24a0)) continue;   /* not loaded */
		int drawn = 0;
		for (int k = 0; k < 16; k++) if (uc_drawn[k].tab == lvl && uc_drawn[k].frame + 1 >= f) drawn = 1;
		if (drawn) continue;   /* the game draws it itself this time */
		uint32_t ov = uc_overlap(lvl, shot);
		if (getenv("RECOMP_VIEW_TRACE")) {
			static uint64_t lt; if (f >= lt + 120) { lt = f; fprintf(stderr, "uc cutscene strip: level table %#x shares %u of the cutscene set's %u pieces%s\n", lvl, ov, nk, ov * 10 < nk && ov < 64 ? " -- not its level, skipped" : ""); }
		}
		if (ov * 10 < nk && ov < 64) continue;   /* not the level this cutscene's set was cut from */
		for (int k = 0; k < 5; k++) WR32(lvl + 0x246c + 4 * k, RD32(shot + 0x246c + 4 * k));   /* the cutscene's lights */
		WR32(lvl + 0x2494, RD32(shot + 0x2494));
		uc_strip_tab = lvl; uc_strip_shot = shot;
		Cpu n = *c;
		n.r[0] = lvl; n.r[1] = view + 0x10; n.r[2] = view + 0xf0;
		n.r[13] = (c->r[13] - 512) & ~7u; n.r[14] = 0xfffffffe;
		fn(&n);
		uc_strip_tab = uc_strip_shot = 0;
	}
}

/* Uncharted's mouse look (keyboard + mouse, window.c; F8 / window_raw_look, off by default -- while off this
 * never takes the mouse, which then drives window.c's right-stick stand-in).
 * fn_81269416 (r6: the pad block, input object +0x1c, the object at *0x819053d8) reads the pad and, per
 * stick, turns the bytes into floats, curves them through its dead zone (fn_81269228: the right stick's
 * result at r6+0x354/+0x358), sets the stick's "deflected" flag, the four direction "buttons", and POSTS
 * INPUT EVENTS carrying the curved value (FUN_81260a22(DAT_819053e0, ...)) -- the camera reads those
 * events, not the fields, so everything written after this function returned (the input object's
 * +0x370/+0x378 and their flag at the old 0x8126645a site; fn_810ee968, which is the player's movement
 * controller) never reached it (found in testing, 2026-10-10). This hook is the instruction after the
 * right stick's curve: the mouse's counts this frame join the curved value there, so the flag, the
 * direction buttons and the events all carry it. Linear in counts (no dead zone, no curve, no easing of
 * ours); the camera still turns at its own rate for a deflection. */
int window_mouse_delta(float *dx, float *dy); float window_mouse_sens(void); int window_mouse_invert(void); int window_raw_look(void);
/* RECOMP_YAW_DIAG=1: twice a second, the active camera (DAT_81905310's camera manager: **(mgr+0x5c)+8 when
 * mgr+0x60, the camera FUN_81040eda returns) and every float of its first 0x400 bytes that changed since
 * the last look -- turning the view shows which fields are its yaw/pitch */
static void uc_camera_diag(void) {
	static uint32_t last_cam; static uint32_t snap[256]; static int tick;
	if (tick++ % 30) return;
	uint32_t mgr = RD32(0x81905310u), cam = 0;
	if (mgr && RD32(mgr + 0x60)) { uint32_t p = RD32(RD32(mgr + 0x5c)); if (p) cam = RD32(p + 8); }
	if (!cam) return;
	char line[1024]; int o = snprintf(line, sizeof line, "uc_cam %#x vt %#x:", cam, RD32(cam)), shown = 0;
	for (int i = 0; i < 256; i++) {
		uint32_t b = RD32(cam + 4u * i); float v, w; memcpy(&v, &b, 4); memcpy(&w, &snap[i], 4);
		if (cam == last_cam && b != snap[i] && isfinite(v) && isfinite(w) && fabsf(v) < 1e6f && fabsf(v - w) > 1e-4f && shown < 24 && o < (int)sizeof line - 40) {
			o += snprintf(line + o, sizeof line - (size_t)o, " +%#x %.4f>%.4f", 4 * i, w, v); shown++;
		}
		snap[i] = b;
	}
	last_cam = cam;
	fprintf(stderr, "%s\n", line);
}
static float uc_mx, uc_my; static int uc_age;   /* the mouse counts not yet turned (uc_mouse_look adds, uc_look_velocity spends; dropped if no camera takes them within a few frames: a cutscene) */
void recomp_ihook_uc_mouse_look(Cpu *c, int reg) {
	(void)reg;
	static int diag = -1; if (diag < 0) diag = getenv("RECOMP_YAW_DIAG") != 0;
	if (diag) uc_camera_diag();
	if (uc_age++ > 3) uc_mx = uc_my = 0;
	if (!window_raw_look()) return;
	float dx = 0, dy = 0;
	window_mouse_delta(&dx, &dy);
	if (dx == 0 && dy == 0) return;
	uint32_t pad = c->r[6];
	if (!pad) return;
	/* the camera's turn: its look message (built from this stick by the action layer, which keeps only the
	 * direction -- one speed for any deflection, found in testing 2026-10-10) sets its turn velocity
	 * (FUN_8106677e, uc_look_velocity below), which then gets the mouse's exact amount. Here the stick only
	 * has to point the mouse's way, past any dead zone, for the message to be sent at all. */
	const float inv = window_mouse_invert() ? -1.0f : 1.0f;
	uc_mx += dx; uc_my += dy * inv;
	float x, y; uint32_t bx = RD32(pad + 0x354), by = RD32(pad + 0x358); memcpy(&x, &bx, 4); memcpy(&y, &by, 4);
	if (fabsf(dx) >= fabsf(dy)) x = dx > 0 ? 0.6f : -0.6f; else y = dy * inv > 0 ? 0.6f : -0.6f;
	memcpy(&bx, &x, 4); memcpy(&by, &y, 4); WR32(pad + 0x354, bx); WR32(pad + 0x358, by);
	if (diag) { static int n; if (n++ % 20 == 0) fprintf(stderr, "uc_mouse_look: dx=%g dy=%g\n", dx, dy); }
}

/* Uncharted's camera, just after its "set look velocity" (FUN_8106677e, the camera's vtable +0x18c; r0 the
 * camera) stored the message's direction times its turn speeds at +0x160/+0x164 (radians a frame, about
 * 0.075 at full tilt): on a frame the mouse moved, the turn is the mouse's instead -- linear in the counts,
 * about 0.0015 rad (0.086 degrees) a count at sensitivity 1, any speed */
void recomp_ihook_uc_look_velocity(Cpu *c, int reg) {
	(void)reg;
	if (uc_mx == 0 && uc_my == 0) return;
	const float k = window_mouse_sens() * 0.0015f;
	float vx = uc_mx * k, vy = uc_my * k; uint32_t bx, by; memcpy(&bx, &vx, 4); memcpy(&by, &vy, 4);
	uint32_t cam = c->r[0];
	WR32(cam + 0x160, bx); WR32(cam + 0x164, by);
	uc_mx = uc_my = 0; uc_age = 0;
	if (getenv("RECOMP_YAW_DIAG")) { static int n; if (n++ % 20 == 0) fprintf(stderr, "uc_look_velocity: cam %#x v %.4f %.4f\n", cam, vx, vy); }
}
