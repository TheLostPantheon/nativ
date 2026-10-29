/*
 * Killzone: Mercenary (PCSA00107): its hooks, built into this game alone (runtime/build.py RECOMP_PACK_HOOKS). packs/PCSA00107/pack.toml's
 * HOOKS= and IHOOKS= place them on the game's functions and instructions; what every game's hooks share is
 * runtime/core/hooks.h. Each one is this game's own feature, named for the guest function it hooks.
 */
#include "hooks.h"

/*
 * Killzone: Mercenary (PCSA00107) camera (2026-10-17). fn_815d9892, the render view's setup, builds the main camera's
 * projection through FUN_815d9130 from a fov multiplier ([view+0x8c0], loaded into s0 at 0x815d9bf0) and the screen aspect
 * (s1: the 960/544 getter at 0x815b1516, reached through the camera's vtable +0x58). The builder multiplies the camera's
 * horizontal fov in degrees (its vtable +0x74) by the multiplier (0x815d9164), then turns it into the vertical fov,
 * 2 atan(tan(h/2) / aspect), and builds the matrices and the culling frustum. kz_hfov widens h there, on the main call only,
 * by the aspect widening; kz_aspect_b (the getter's movt) widens the aspect. With both, a wider screen keeps the vertical
 * fov and shows more at the sides (Hor+). That camera turned out to be the first-person weapon's; the world's is
 * kz_world_cam's below, which also takes the fov setting. RECOMP_FOV_TRACE logs the fov now and then. kz_aspect_a
 * (0x8116be86: a 2D offset scaled by the aspect, not the projection) is left as it is unless RECOMP_KZ_ASPECT has 'a'.
 */
static double fov_factor(void) { const char *e = getenv("RECOMP_FOV"); double k = e && *e ? atof(e) : settings_float("fov", 1.0); return k > 0 ? k : 1.0; }
static __thread int kz_main_cam;   /* inside fn_815d9892's call of the builder: the main camera's projection */
void recomp_ihook_kz_cam_begin(Cpu *c, int reg) { (void)c; (void)reg; kz_main_cam = 1; }   /* 0x815d9bf0: the view's fov multiplier loaded for the call */
void recomp_ihook_kz_cam_end(Cpu *c, int reg) { (void)c; (void)reg; kz_main_cam = 0; }     /* 0x815d9c0e: just after the call */
void recomp_ihook_kz_hfov(Cpu *c, int reg) {   /* 0x815d9164, in the builder: s0 = the horizontal fov in degrees (the camera's, vtable +0x74, x the view's multiplier) */
	(void)reg;
	if (getenv("RECOMP_FOV_TRACE") && atoi(getenv("RECOMP_FOV_TRACE")) >= 2) {   /* =2: every call of the builder, by lr: left by the camera's fov getter just called, so one value per camera class, not the caller */
		static uint32_t seen[8]; static unsigned cnt[8]; uint32_t lr = c->r[14]; int i = 0;
		while (i < 8 && seen[i] && seen[i] != lr) i++;
		if (i < 8) { if (!seen[i]) seen[i] = lr; if (cnt[i]++ % 600 == 0) fprintf(stderr, "kz camera builder: call %u from lr %#x, horizontal fov %g deg%s\n", cnt[i], lr, c->vfp.f[0], kz_main_cam ? " (hooked)" : ""); }
	}
	if (!kz_main_cam) return;   /* (the builder's two other callers: other cameras, left as they are) */
	float h = c->vfp.f[0]; double k = aspect_widen();   /* (the weapon's view: the aspect only; the gun keeps the game's own look at any fov setting, the world's fov is kz_world_cam's) */
	if (getenv("RECOMP_FOV_TRACE")) { static int n; if (n++ % 300 == 0) fprintf(stderr, "kz weapon camera: horizontal fov %g deg, widened by %g\n", h, k); }
	if (k == 1.0 || !(h > 1.0f && h < 179.0f)) return;
	double w = 2.0 * atan(k * tan(h * M_PI / 360.0)) * 180.0 / M_PI;
	c->vfp.f[0] = (float)(w > 170.0 ? 170.0 : w);
}
/* The world's projection is not that one (2026-10-24). kz_hfov above only ever reached the first-person weapon's camera (60
 * degrees): the fov setting shrank the gun and left the world as it was (found in testing, 2026-10-23). The world is drawn with a
 * camera the view setup fn_816148c8 (1.12; fn_815d9892 in 1.00) builds on its stack each frame: the game camera's method
 * (vtable +0x44, 0x81236dd6) fills it from a horizontal fov in degrees (70) and the screen aspect, as a frustum at the near
 * plane (near 0.2, left/right +-0.14004, top/bottom +-0.07877), and fn_81236f8a then turns that into the projection, its
 * inverse and the culling frustum. kz_world_cam scales the four edges just before that call by the fov setting and the
 * aspect widening: the fov widens the world, and on a wider screen, where the game's aspect (kz_aspect_b) had narrowed the
 * vertical edges by the widening (the world zoomed in at 21:9), the same factor restores them and widens the horizontal ones:
 * Hor+. The camera is rebuilt every frame, so nothing compounds. Found by lldb write watchpoints from the shaders' constants
 * back, hop by hop (fovwatch scripts, 2026-10-24). */
void recomp_ihook_kz_world_cam(Cpu *c, int reg) {   /* 0x81614a2e, before the call of fn_81236f8a: r1 = the world camera (+0 type, 0 = perspective; +4 near; +0xc left, +0x10 right, +0x14 top, +0x18 bottom) */
	(void)reg;
	const uint32_t cam = c->r[1]; const double k = fov_factor() * aspect_widen();
	if (getenv("RECOMP_FOV_TRACE")) { static int n; if (n++ % 300 == 0) fprintf(stderr, "kz world camera: near %g, edges %g %g %g %g, widened by %g\n", rdf(cam + 4), rdf(cam + 0xc), rdf(cam + 0x10), rdf(cam + 0x14), rdf(cam + 0x18), k); }
	if (k == 1.0 || RD32(cam) != 0 || !(rdf(cam + 4) > 0.0f)) return;
	for (uint32_t off = 0xc; off <= 0x18; off += 4) { const float v = rdf(cam + off); if (isfinite(v)) { uint32_t w; float nv = (float)(v * k); memcpy(&w, &nv, 4); WR32(cam + off, w); } }
}
/* The static world vanished at the widest views (32:9, or 21:9 at a high fov) while dynamic objects stayed (2026-10-25).
 * The static scene's precomputed cells are classified each frame by fn_8180bcd0 (called from the job fn_8117a00a with
 * the view's vertical fov in s1 and its aspect in s2): it takes the horizontal fov as vfov x aspect, a linear estimate
 * (76 degrees at the native 70), and divides extents by the cosine of its half. Past 180 degrees that cosine is negative
 * and every cell is classified hidden: 61 x 3.59 = 219 at 32:9 and fov 1.5. kz_cell_fov leaves an estimate up to 150
 * degrees as it is (every native view: the cutscene camera's 80 estimates 90) and above that takes the true horizontal
 * fov, 2 atan(tan(vfov/2) aspect), with a 10% margin, between 150 and 170 degrees: never narrower than the view, never
 * near 180. Found by a verify-build count A/B (RECOMP_WATCH_WINDOWS) and write watchpoints back from the static draw
 * job fn_8115d650 to the cells' state bytes. */
void recomp_ihook_kz_cell_fov(Cpu *c, int reg) {   /* 1.12 0x8180bdb2 (1.00 0x817d024c), fn_8180bcd0's vmul s1 = s1 x s2 */
	(void)reg;
	const float est = c->vfp.f[1], a = c->vfp.f[2];
	if (!(a > 0 && est > 0)) return;
	const double lo = 150.0 * M_PI / 180.0, hi = 170.0 * M_PI / 180.0;
	const double v = est / a, t = 2.0 * atan(tan(0.5 * v) * a), cap = fmin(fmax(1.1 * t, lo), hi);
	if (est > lo) c->vfp.f[1] = (float)cap;
	if (getenv("RECOMP_FOV_TRACE")) { static int n; if (n++ % 600 == 0) fprintf(stderr, "kz cells: vfov %g deg, aspect %g: horizontal %g deg estimated, %g true%s\n", v * 180 / M_PI, a, est * 180 / M_PI, t * 180 / M_PI, est > lo ? ", capped" : ""); }
}
void recomp_ihook_kz_aspect_b(Cpu *c, int reg) { widen_aspect_reg(c, reg); }
void recomp_ihook_kz_aspect_a(Cpu *c, int reg) { const char *e = getenv("RECOMP_KZ_ASPECT"); if (e && strchr(e, 'a')) widen_aspect_reg(c, reg); }

/* Killzone at 60 fps (2026-10-18): see "only the player steps at 60" below; kz_pad_stamp fixes the input race 60 exposed. */
/* Killzone's pad thread (fn_810523b2) stamps each sample with the process time BEFORE its blocking sceCtrlReadBufferPositive
 * and files it in its 20-sample ring a vblank later; the main loop's reader (fn_817996e0) takes the samples stamped since its
 * own last call. At 30 it runs every other vblank and about half the samples got through; at 60 both wake on the same
 * vblank, the reader's call usually lands after the pad thread's stamp, and every sample is "older" than the window: no
 * input at all, or the last sample that got in held -- a turn running on for a second after the stick was let go (the
 * user's 60 fps "input stops"/"spins", 2026-10-18; the game never ran at 60 on the Vita). This restamps the sample under the
 * ring's lock (0x8105245e, just after the pad thread takes it), where the reader reads its time too: each sample lands in
 * exactly one reader window, at either rate. */
uint64_t guest_time_us(void);
void recomp_ihook_kz_pad_stamp(Cpu *c, int reg) {
	(void)reg;
	static int off = -1; if (off < 0) off = getenv("RECOMP_KZ_PADSTAMP") && *getenv("RECOMP_KZ_PADSTAMP") == '0';
	if (off) return;
	static uint32_t slot = 1; if (slot == 1) slot = game_addr("kz_pad_stamp_sp");   /* the sample's time on the pad thread's stack (1.00 sp+0x218) */
	if (!slot) return;
	const uint64_t t = guest_time_us(); const uint32_t sp = c->r[13];
	WR32(sp + slot, (uint32_t)t); WR32(sp + slot + 4, (uint32_t)(t >> 32));
}

/* Killzone at 60: only the player steps at 60. Entities update in groups by period -- the game's 33333 us (30 Hz) and
 * 66666 (15 Hz), copied into each source file's globals -- and their code is written for those rates: the 15 Hz AI steps
 * fixed 1/15 s (117 constants), cutscene and character logic counts updates. Moving whole groups to 60 (the first two
 * versions) ran NPCs, animations and cutscenes up to twice as fast (found in testing, 2026-10-18; the level intro 33 s -> 19 s). So:
 * - the scheduler (fn_8158bd0a) counts a frame as the vblanks that passed (0x81ab0b00 on), steps 1/60 (its s16) and reads
 *   its period as 16667 at its own four loads (kz_sched_period) -- the global stays 33333 for everything else;
 * - the player alone is filed under a 16667 group (kz_period at fn_8123a244's two reads of the entity's period, for the
 *   player only), re-filed through the game's own FUN_81244552 when the setting changes (kz_sched);
 * - the player is the entity whose update (fn_813d746e, kz_entity) runs the aim update (fn_8155b646, kz_aim).
 * Every other group keeps its period, its entities their dt. */
static int kz_60_on(void) { static int off = -1; if (off < 0) off = getenv("RECOMP_KZ_60_OFF") != 0; return display_frame_rate_60() && !off; }
static __thread uint32_t kz_cur_entity;   /* the entity whose update (fn_813d746e) this thread is in */
static uint32_t kz_player; static uint64_t kz_player_seen;   /* the player entity, and the frame its aim update last ran */
uint64_t guest_time_us_peek(void);
/* RECOMP_KZ_NPC_DIAG=<fiber name part>: per update of that entity (an NPC's fiber carries its name) and per world update, its position,
 * its locomotion state and its physics body (the character proxy the movement drives: movement = entity +0xfc, proxy = movement +0x198,
 * body = [[proxy+0x60]+0x10], position +0x120, velocity +0x1b0; fn_81571a28 copies the body's position back) -- an NPC's speed at 30 vs 60 */
static uint32_t kz_npc_ent;
static float kz_f(uint32_t a) { uint32_t b = RD32(a); float v; memcpy(&v, &b, 4); return v; }
static void kz_npc_line(uint32_t e, const char *what, float dt) {
	const uint32_t lo = RD32(e + 0x4cc), ls = lo && RD32(lo + 0xac) ? RD32(RD32(lo + 0xac) + 4) : 0xffffffffu;
	const uint32_t mv = RD32(e + 0xfc), px = mv ? RD32(mv + 0x198) : 0, pb = px && RD32(px + 0x60) ? RD32(RD32(px + 0x60) + 0x10) : 0;
	fprintf(stderr, "    kz npc: frame %llu t %.4f %s dt %.5f ent %#x pos %.3f %.3f %.3f state %d", (unsigned long long)display_frames(), guest_time_us_peek() / 1e6,
		what, dt, e, kz_f(e + 0xd0), kz_f(e + 0xd4), kz_f(e + 0xd8), (int)ls);
	if (mv) fprintf(stderr, " ground-ref z %.3f scripted %u/%u fall %.3f e108 %#x", kz_f(mv + 0x184), RD8(mv + 0x98), RD8(mv + 0x99), lo ? kz_f(lo + 0x104) : 0.f, RD32(e + 0x108));   /* (the movement's last-on-ground height, its scripted-move flags; the locomotion's fall height) */
	if (pb) fprintf(stderr, " body %.3f %.3f %.3f vel %.3f %.3f %.3f", kz_f(pb + 0x120), kz_f(pb + 0x124), kz_f(pb + 0x128), kz_f(pb + 0x1b0), kz_f(pb + 0x1b4), kz_f(pb + 0x1b8));
	fprintf(stderr, "\n");
}
void recomp_hook_kz_entity(Cpu *c) {
	kz_cur_entity = c->r[0];
	{	/* RECOMP_KZ_GESTURE_DIAG=1: the player's melee settings (entity +0x4bc's +0x10) -- the gesture windows at +0x10c easy, +0x110 medium, +0x114 hard, +0x118 multiplayer -- once */
		static int d = -1; if (d < 0) d = getenv("RECOMP_KZ_GESTURE_DIAG") != 0;
		const uint32_t mc = d == 1 && kz_player && c->r[0] == kz_player ? RD32(kz_player + 0x4bc) : 0, st = mc ? RD32(mc + 0x10) : 0;
		if (st) { d = 2; fprintf(stderr, "    kz gesture: settings %#x:", st); for (uint32_t o = 0xf0; o <= 0x124; o += 4) { uint32_t b = RD32(st + o); float f; memcpy(&f, &b, 4); fprintf(stderr, " +%#x %.3f", o, f); } fprintf(stderr, "\n"); }
	}
	static const char *npc = (const char *)-1; if (npc == (const char *)-1) npc = getenv("RECOMP_KZ_NPC_DIAG");
	char tn[32] = ""; if (npc) plat_thread_name(tn, sizeof tn);   /* (a fiber's host thread carries the fiber's name) */
	if (npc && strstr(tn, npc)) { kz_npc_ent = c->r[0]; kz_npc_line(c->r[0], "update", c->vfp.f[0]); }
	{	/* RECOMP_KZ_60_ON_FIBER=<name part>: switch to 60 fps when a fiber of that name first updates -- a recorded 30 fps session
		 * replayed into a scripted scene, which then plays at 60 (inputs after it no longer matter: the dropship's zipline) */
		static const char *f60 = (const char *)-1; if (f60 == (const char *)-1) f60 = getenv("RECOMP_KZ_60_ON_FIBER");
		static int done; char tn2[32] = "";
		if (f60 && !done) { plat_thread_name(tn2, sizeof tn2); if (strstr(tn2, f60)) { done = 1; extern void display_force_frame_rate(int fr); display_force_frame_rate(60); fprintf(stderr, "    kz: %s appeared (frame %llu): 60 fps from here\n", tn2, (unsigned long long)display_frames()); } }
	}
}
/* FUN_81568842 (r0 = an entity's melee component, owner at +0x1c): the brutal melee / interrogation gesture window counts
 * down at +0x348 (set from the difficulty's Easy/Medium/HardGestureWindow at 0x8156b654, decremented by the update's dt at
 * 0x815689b0). While the player's is open, the window lets a captured mouse drag the front panel (window_touch_gesture_hint). */
void recomp_hook_kz_gesture(Cpu *c) {
	const uint32_t m = c->r[0]; if (!m || (kz_player && RD32(m + 0x1c) != kz_player)) return;
	uint32_t b = RD32(m + 0x348); float w; memcpy(&w, &b, 4);
	static float last; if (w > 0 && !(last > 0)) fprintf(stderr, "    kz gesture: window open (%.2f s, frame %llu t %.3f)\n", w, (unsigned long long)display_frames(), guest_time_us_peek() / 1e6);
	if (!(w > 0) && last > 0) fprintf(stderr, "    kz gesture: window closed (frame %llu t %.3f)\n", (unsigned long long)display_frames(), guest_time_us_peek() / 1e6);
	last = w;
	if (w > 0) { extern void window_touch_gesture_hint(void); window_touch_gesture_hint(); }
}
/* Two-finger turns: Killzone's valves and explosives (planting a charge). Each has a touch controller (ValveController, vtable
 * 0x81a00cbc; ExplosiveLocationController, 0x819fdf48) whose update (+0x74: FUN_814347e6, FUN_8142d294) reads the first two
 * touches (ids +0x11c / +0x120, points +0x128..+0x12e, count +0x124) unless flag 8 (+0x24) is set, while attached to its object
 * (+0x20, resource +0xe4), and turns it by how much the line between them turns (atan of their slope):
 * - the valve (FUN_814346d0): times the resource's sensitivity +0x1d4, only the valve's way (+0x2b8: 1 up, 2 down, else both),
 *   clamped to +0x1c8..+0x1cc degrees; done at +0x1c0 (way 1) / +0x1bc (way 2) degrees (+0x2bc, radians);
 * - the explosive (FUN_8142d19c): only while its state (+0x2bc) is 1 or less, times 2 (state 0) or 4, either way, clamped to
 *   +0x1c0..+0x1c4 degrees.
 * The input so far is the controller's +0x108 (radians). While one reads touches, the pad's sticks and a captured mouse's drag
 * twist two fingers (window_touch_twist_hint). Logged: each start, both fingers reaching it, and the turn each second. */
static void kz_twist(uint32_t vc, int bomb) {
	const uint32_t v = vc ? RD32(vc + 0x20) : 0;
	static uint32_t cur; static int fingers; static uint64_t t_log;
	const char *what = bomb ? "explosive" : "valve";
	if (!v || (RD32(vc + 0x24) & 8) || (bomb && (int32_t)RD32(v + 0x2bc) > 1)) {
		if (vc && vc == cur) { fprintf(stderr, "    kz %s: input off (frame %llu, input %.1f deg)\n", what, (unsigned long long)display_frames(), kz_f(vc + 0x108) * 57.29578f); cur = 0; }
		return;
	}
	extern void window_touch_twist_hint(void); window_touch_twist_hint();
	const uint64_t t = guest_time_us_peek();
	const uint32_t res = RD32(v + 0xe4);
	if (vc != cur) {
		cur = vc; fingers = 0; t_log = t;
		if (bomb) fprintf(stderr, "    kz explosive: in use (controller %#x, explosive %#x, frame %llu): state %d, input %.1f deg of %.1f..%.1f\n",
			vc, v, (unsigned long long)display_frames(), (int32_t)RD32(v + 0x2bc), kz_f(vc + 0x108) * 57.29578f, kz_f(res + 0x1c0), kz_f(res + 0x1c4));
		else fprintf(stderr, "    kz valve: in use (controller %#x, valve %#x, frame %llu): way %u, at %.1f deg, done at %.1f / %.1f deg, input %.1f..%.1f deg, sensitivity %.3f\n",
			vc, v, (unsigned long long)display_frames(), RD32(v + 0x2b8), kz_f(v + 0x2bc) * 57.29578f, kz_f(res + 0x1c0), kz_f(res + 0x1bc), kz_f(res + 0x1c8), kz_f(res + 0x1cc), kz_f(res + 0x1d4));
	}
	if (!fingers && RD32(vc + 0x124) == 2) { fingers = 1; fprintf(stderr, "    kz %s: both fingers on it (frame %llu)\n", what, (unsigned long long)display_frames()); }
	if (t - t_log >= 1000000) {
		t_log = t;
		if (bomb) fprintf(stderr, "    kz explosive: state %d, input %.1f deg, %u fingers\n", (int32_t)RD32(v + 0x2bc), kz_f(vc + 0x108) * 57.29578f, RD32(vc + 0x124));
		else fprintf(stderr, "    kz valve: at %.1f deg, input %.1f deg, %u fingers\n", kz_f(v + 0x2bc) * 57.29578f, kz_f(vc + 0x108) * 57.29578f, RD32(vc + 0x124));
	}
}
void recomp_hook_kz_valve(Cpu *c) { kz_twist(c->r[0], 0); }

/* ---- Killzone's video wrapper (its per-frame update FUN_8105b68c, its close FUN_8105b91e) holds its global lock (lwmutex
 * 0x81b351f0) across sceAvPlayerJumpToTime -- its own loop of the title video, FUN_8105baec -- and sceAvPlayerClose, both of
 * which wait for the player's controller thread; and that thread delivers each player event through the wrapper's callback
 * FUN_8105bbee, whose first act is taking the same lock. An event raised during the seek or the close was a deadlock: the
 * game's thread polling the player forever (sceKernelDelayThread from sceAvPlayerJumpToTime), the controller waiting for
 * the lock (a freeze at the title screen in testing, 2026-10-21: no present for 14 s and counting). A Vita runs these
 * threads by priority on shared cores, which leaves the race far narrower than threads running at once do. kz_avp_unlock
 * (after the instruction before each call) / kz_avp_relock (after the call): the lock let go of just around those two
 * calls, so the event is handled while the call waits, as it would have been a moment earlier. */
static __thread int kz_avp_count;
void recomp_ihook_kz_avp_unlock(Cpu *c, int reg) {
	(void)c; (void)reg; extern int kernel_lwmutex_release_all(uint32_t work);
	static uint32_t lk = 1; if (lk == 1) lk = game_addr("kz_avp_lock");   /* the wrapper's lwmutex (1.00: 0x81b351f0) */
	if (!lk) return;
	kz_avp_count = kernel_lwmutex_release_all(lk);
	static int said; if (!said++) fprintf(stderr, "    kz video: the wrapper's lock let go of around a player call that waits for its controller (count %d)\n", kz_avp_count);
}
void recomp_ihook_kz_avp_relock(Cpu *c, int reg) {
	(void)c; (void)reg; extern void kernel_lwmutex_retake(uint32_t work, int n);
	static uint32_t lk = 1; if (lk == 1) lk = game_addr("kz_avp_lock");
	if (lk) kernel_lwmutex_retake(lk, kz_avp_count);
	kz_avp_count = 0;
}
void recomp_hook_kz_bomb(Cpu *c) { kz_twist(c->r[0], 1); }
/* fn_81061da4(main, CDRAM, phycont): the game's own memory budgets, set once at start (FUN_8105f4cc: 0xe700000, 0x7000000,
 * 0x1a00000 -- a Vita's). Every block it allocates (FUN_81061f36) is counted against its type's budget first and refused
 * when over -- and a refused request still counts, and nobody checks for 0. In a level its CDRAM is exactly full by design
 * (render-data pool 78 MB, display 4, depth 0.5, render-target pools 2 + 15.5, GXM parameter buffer 12 = 112 MB), so any
 * overlap at a level change overflows it: a level 1 -> 2 transition on Linux (2026-10-21) had its volatile render-target
 * pool refused at the level load (budget 0x8b00000 of 0x7000000 afterwards), every volatile target placed at an offset from
 * 0, the frame drawn only into its top 192 rows. The host has the memory: the budgets 4x. Nothing sizes itself from them
 * (the limit is read only by that check), so this turns refusals into blocks and changes nothing else. */
void recomp_hook_kz_budget(Cpu *c) {
	for (int i = 0; i < 3; i++) c->r[i] *= 4;
	fprintf(stderr, "    kz memory budgets 4x: main %#x, CDRAM %#x, phycont %#x\n", c->r[0], c->r[1], c->r[2]);
}
/* FUN_81061f36 just after it has counted a request (0x81061f7c add.w r1, ip, sb: r1 the type's new total, r7 the block's
 * name, r8 its type, sb its size, [sp+0x10] the budget's address): every block that takes a type past the budget a Vita
 * had (a quarter of kz_budget's) logged -- what overlaps at a level change, where the console would have refused */
void recomp_ihook_kz_budget_over(Cpu *c, int reg) {
	(void)reg;
	const uint32_t total = c->r[1], vita = RD32(RD32(c->r[13] + 0x10)) / 4, type = c->r[8];
	if (total <= vita) return;
	static int n; if (n++ >= 50) return;
	const char *t = type == 0x09408060u ? "CDRAM" : type == 0x0c20d060u || type == 0x0c208060u ? "main" : "phycont";
	fprintf(stderr, "    kz memory: %s in use %#x, past a Vita's %#x: +%#x '%s' (frame %llu)\n", t, total, vita, c->r[9],
	        c->r[7] ? (const char *)G(c->r[7]) : "?", (unsigned long long)display_frames());
}
/* FUN_810044c6 (r0 = a string object, r1 = a C string): the game names its events through it (9122 call sites). With
 * RECOMP_KZ_EVENTS=1 the interrogation / brutal melee / zipline / knockdown ones are logged with the frame and the guest
 * time -- the timing of an interrogation's prompt, the player's swipe and the verdict (in testing: the swipe window felt too
 * short) and of dropship deployments (enemies downed at 60) from a real session */
static const char *const kz_event_names[] = {   /* matched by content: their addresses differ between builds */
	"Interrogate", "InterrogateSP", "InterrogateFailedVictimDied", "StartInterrogate", "InterrogateFailed", "Interrogation SP React",
	"GesturePassed", "InterrogateSuccess", "Interrogation SP Initiate", "Interrogation SP Breakout", "Brutal Melee", "StartZipLine",
	"Zipline Attach", "Knockdown", "Falling", "Stagger", "KnockedOutTime",
};
void recomp_hook_kz_event(Cpu *c) {
	static int on = -1; if (on < 0) on = getenv("RECOMP_KZ_EVENTS") != 0;
	if (!on || !c->r[1]) return;
	const char *a = (const char *)G(c->r[1]);   /* (the constructor's C string) */
	if (a[0] < 'B' || a[0] > 'Z') return;
	for (size_t i = 0; i < sizeof kz_event_names / sizeof kz_event_names[0]; i++) if (!strncmp(a, kz_event_names[i], 40)) {
		char tn[32] = "?"; plat_thread_name(tn, sizeof tn);
		fprintf(stderr, "    kz event: frame %llu t %.3f %s (%s, lr %#x)\n", (unsigned long long)display_frames(), guest_time_us_peek() / 1e6, kz_event_names[i], tn, c->r[14]);
		return;
	}
}
/* RECOMP_KZ_LOCO_DIAG=1: FUN_8148bf18 (the locomotion component's vtable +0x48: on-ground / falling, the fall timer +0xfc += dt,
 * the fall start height +0x104) -- its dt and how often it runs, per second */
void recomp_hook_kz_loco(Cpu *c) {
	static int on = -1; if (on < 0) on = getenv("RECOMP_KZ_LOCO_DIAG") != 0;
	if (!on) return;
	static uint64_t win_t0; static unsigned n; static double sum; static float mn = 1e9f, mx = 0;
	const uint64_t t = guest_time_us_peek(); const float dt = c->vfp.f[0];
	if (!win_t0) win_t0 = t;
	n++; sum += dt; if (dt < mn) mn = dt; if (dt > mx) mx = dt;
	if (t - win_t0 >= 1000000) {
		char tn[32] = "?"; plat_thread_name(tn, sizeof tn);
		fprintf(stderr, "    kz loco: frame %llu: %u calls/s, dt %.5f..%.5f (sum %.3f s/s), last thread %s\n", (unsigned long long)display_frames(), n, mn, mx, sum, tn);
		win_t0 = t; n = 0; sum = 0; mn = 1e9f; mx = 0;
	}
}
/* fall damage on landing (with RECOMP_KZ_EVENTS): 0x8148a858 / 0x814737b0 store (fall height - FallDamageRange) x
 * FallDamagePerExtraMeter to the stack -- logged when positive, with the fall height the locomotion component kept (+0x104) and
 * the fiber, to see whether the enemies a dropship drops at 60 are downed by a fall */
void recomp_ihook_kz_fall(Cpu *c, int reg) {
	(void)reg; static int on = -1; if (on < 0) on = getenv("RECOMP_KZ_EVENTS") != 0;
	if (!on || !(c->vfp.f[0] > 0)) return;
	char tn[32] = "?"; plat_thread_name(tn, sizeof tn);
	fprintf(stderr, "    kz event: frame %llu t %.3f fall damage %.2f (%s)\n", (unsigned long long)display_frames(), guest_time_us_peek() / 1e6, c->vfp.f[0], tn);
}
/* FUN_81573226 (the kinematic movement update; r5 = the movement component): airborne, it stores the drop to the ground below,
 * |ground height (+0x1d4) - z|, at +0x74 (0x8157344a) -- the fall height on landing takes the larger of that and last-on-ground z
 * minus z (FUN_815758da). The ground height comes from a ground probe (the probe ring +0x1f8/+0x1fc, or a cast at 0x8157472e); a
 * fresh component holds the constructor's 0 until one lands, and riders scripted down a dropship's zipline skip the probe. At 60
 * their first airborne update came before the first probe: the "drop" was their whole altitude (~820) and every one of them took
 * ~8150 fall damage on touchdown (test sessions, 2026-10-19/20: SequenceDude_0..4). A height of exactly 0.0 is never a real
 * one there (that level is at z ~800; no ground is -FLT_MAX), so: never measured -> no drop known, and the fall counts from where
 * they let go. */
void recomp_ihook_kz_drop(Cpu *c, int reg) {
	(void)reg; const uint32_t m = c->r[5];
	static uint32_t ground = 1; if (ground == 1) ground = game_addr("kz_move_ground");   /* the ground height's offset in the movement (1.00 +0x1d4; 1.12 +0x230, the struct grew) */
	if (!ground || RD32(m + ground) != 0 || RD32(m + 0x74) == 0) return;
	const float d = kz_f(m + 0x74); WR32(m + 0x74, 0);
	static int said; if (said < 20) { said++; char tn[32] = "?"; plat_thread_name(tn, sizeof tn); fprintf(stderr, "    kz: %s airborne before its first ground probe: a drop of %.1f (to height 0) ignored\n", tn, d); }
}
static void kz_player_mark(void) { if (kz_cur_entity) { kz_player = kz_cur_entity; kz_player_seen = display_frames(); } }
void recomp_ihook_kz_period(Cpu *c, int reg) {   /* fn_8123a244 (r6 = the entity it files): its period, for the player at 60 */
	if (reg < 0 || reg > 14 || !kz_60_on() || !kz_player || c->r[6] != kz_player) return;
	if (c->r[reg] == 33333u) c->r[reg] = 16667u;
}
/* the scheduler steps at 60 only while the player is in its 30 Hz (gameplay) mode: in its 15 Hz mode (66666: the game's cinematic
 * mode, the level intro's cutscene) it keeps the game's 30 Hz cadence -- something there steps per world update and the intro
 * played 1.6x fast with the scheduler at 60 (2026-10-18); cutscenes run at 30 then, as in many PC ports */
static int kz_sched_60;   /* (kz_sched, once a frame) */
static int kz_no(char k) { static const char *e = (const char *)-1; if (e == (const char *)-1) e = getenv("RECOMP_KZ_60_NO"); return e && strchr(e, k); }   /* (A/B: "w" world dt, "t" tick gate, "s" sub-step 1/60, "p" physics step off) */
void recomp_ihook_kz_sched_period(Cpu *c, int reg) { if (reg >= 0 && reg <= 14 && c->r[reg] == 33333u && kz_sched_60) c->r[reg] = 16667u; }
/* FUN_8112f5a4, the physics world step (the scheduler's sub-step loop at 0x8158c136): it ignores the dt it is passed and loads the
 * world's own fixed step from +0xc8, 1/30 from its constructor (0x8112dc96) -- so at 60 every body (NPCs' character proxies, whose
 * positions fn_81571a28 copies back each update) moved twice as far: the buddy walked 2x. Halved while the scheduler steps at 60. */
/* Level of detail distance: the "scenery_detail" setting (F10's Scenery detail, RECOMP_SCENERY_DETAIL; 1..8, 1 = the game's own).
 * The static scene's LOD update (the UpdateSceneLod job, FUN_817cc5d4 -> FUN_81174e66(scene, s0-s2 the camera, s3 the
 * camera's RenderLodScale, s4 its RenderLodBias)) compares each object's squared distance to the camera times s3 squared
 * with the object's LOD thresholds; s3 / k keeps every level of detail k times as far. 0x81174e6a: after the prologue,
 * before s3 is squared. In-level, standing still: x4 drew ~520 draws / 333K indices a frame against ~440 / 256K (2026-10-20).
 * (The ViewportSetup resource's LODDistanceMultiplier, 2.5 on the Vita, is never read in single player: watched from boot.)
 * RECOMP_LOD_FROM=<frame>: the game's own until that frame -- an A/B within one run. */
static double kz_lod_factor(void) {
	static long from = -2; if (from == -2) { const char *f = getenv("RECOMP_LOD_FROM"); from = f ? atol(f) : -1; }
	if (from >= 0 && (long)display_frames() < from) return 1.0;
	const char *e = getenv("RECOMP_SCENERY_DETAIL"); const double k = e && *e ? atof(e) : settings_float("scenery_detail", 1.0);
	return k > 1.0 ? (k < 8.0 ? k : 8.0) : 1.0;
}
/* Characters' and props' level of detail: the "character_detail" setting (F10's Character detail, RECOMP_CHARACTER_DETAIL; 1..8).
 * FUN_81172d68 builds a dynamic object's LOD distance factor from the view's base (renderer +0xd18, loaded into s0 at
 * 0x81172db8), times the camera's RenderLodScale (+0xd60) and plus its bias (+0xd64) when UseDynamicObjectRenderLodScaleBias
 * (+0xd68) is set, and the object's own scale and bias (+0x20, +0x24). s0 / k keeps each level k times as far. Dynamic objects
 * don't go through the static scene's precomputed per-area lists (see kz_lod). RECOMP_LOD_FROM applies to it too. */
static double kz_char_factor(void) {
	static long from = -2; if (from == -2) { const char *f = getenv("RECOMP_LOD_FROM"); from = f ? atol(f) : -1; }
	if (from >= 0 && (long)display_frames() < from) return 1.0;
	const char *e = getenv("RECOMP_CHARACTER_DETAIL"); const double k = e && *e ? atof(e) : settings_float("character_detail", 1.0);
	return k > 1.0 ? (k < 8.0 ? k : 8.0) : 1.0;
}
void recomp_ihook_kz_char_lod(Cpu *c, int reg) {
	(void)reg; const double k = kz_char_factor();
	{	/* RECOMP_KZ_LOD_DIAG=1: dynamic objects through here a frame, now and then */
		static int on = -1; if (on < 0) on = getenv("RECOMP_KZ_LOD_DIAG") != 0;
		if (on) { static uint64_t f; static unsigned n; const uint64_t fr = display_frames();
			if (fr != f) { if (fr % 150 == 0) fprintf(stderr, "    kz char lod: frame %llu: %u objects last frame, base %g, x%g\n", (unsigned long long)fr, n, c->vfp.f[0], k); f = fr; n = 0; }
			n++; }
	}
	if (k != 1.0) c->vfp.f[0] = (float)(c->vfp.f[0] / k);
}
/* Scenery detail without holes. The scene's LOD tree (FUN_81174e66's walk, redone here as kz_lod_true: nodes [S] with depth
 * [S+0x10][i], AABB and parent-distance range at [S+0xc]+0x20*i, own range and objects u16 first/end at [S+0x14]+0xc*i,
 * objects' [min, max) at [S+0x18]+8*o; it matches the game's flags object for object) holds groups of two kinds: details
 * that fade out ([0,15): nothing replaces them) and groups with a coarser stand-in ([0,10) x3 pieces, then one [10,200)
 * merged piece -- walls, structures; 1106 of 1397 groups in the first level). Scaled, a stand-in group kept its fine pieces
 * 3x as far and switched the stand-in off -- and a few steps back the fine pieces weren't drawn (their data streamed by the
 * game's own distances, as far as can be told), so walls vanished (captures at 3x, 1x clean, 2026-10-20). So
 * kz_sdl, at the static draw list's entry (FUN_81177972, ctx: S = [ctx+0x18]), puts the game's own pick back for every
 * group with a stand-in, leaving the fade-out details scaled; and for any group whose scaled pick isn't in the per-area lists
 * the draw list walks (one set per view at ctx+0..+0xc; entries [set+0x64] = {count, _, base}, u16 first/end at +2/+4 into
 * 16-byte records at [set+0x58] whose +4 is the LOD object; [set+0x80] entries decoded so far) while its own pick is.
 * x3 in-level: ~9% more geometry (+30% unrestricted). RECOMP_KZ_LOD_DIAG=1: counts and a check of the walk against the
 * game's flags; RECOMP_KZ_LOD_TREE=<frame>: the tree once; RECOMP_KZ_LOD_NOFALLBACK=1: the plain scaling (holes). */
typedef struct { uint32_t scene, nobj, nnode; uint8_t *tvis, *svis, *replaced; uint16_t *node_of; uint32_t *member; uint32_t stamp; } KzLodScene;   /* svis: RECOMP_KZ_LOD_DIAG's check -- kz_lod_true's walk at the scaled distance, against the game's own flags */
static KzLodScene kz_ls[4];
static KzLodScene *kz_lod_scene(uint32_t S, int make) {
	for (int i = 0; i < 4; i++) if (kz_ls[i].scene == S) return &kz_ls[i];
	if (!make) return 0;
	KzLodScene *x = 0; for (int i = 0; i < 4 && !x; i++) if (!kz_ls[i].scene) x = &kz_ls[i];
	if (!x) { x = &kz_ls[0]; }
	free(x->tvis); free(x->svis); free(x->replaced); free(x->node_of); free(x->member); memset(x, 0, sizeof *x); x->scene = S;
	return x;
}
static void kz_lod_true(uint32_t S, float cx, float cy, float cz, float scale, float bias, int into_svis) {
	const uint32_t nnode = RD32(S), nobj = RD32(S + 4), N = RD32(S + 0xc), D = RD32(S + 0x10), R = RD32(S + 0x14), O = RD32(S + 0x18);
	if (!nnode || !nobj || nobj > 0x10000 || !N || !D || !R || !O) return;
	KzLodScene *x = kz_lod_scene(S, 1);
	if (x->nobj != nobj || x->nnode != nnode) {   /* (re)built: the object -> node map */
		free(x->tvis); free(x->svis); free(x->replaced); free(x->node_of); free(x->member);
		x->tvis = calloc(nobj, 1); x->svis = calloc(nobj, 1); x->replaced = calloc(nnode, 1); x->node_of = malloc(nobj * sizeof *x->node_of); x->member = calloc(nobj, sizeof *x->member); x->nobj = nobj; x->nnode = nnode;
		for (uint32_t o = 0; o < nobj; o++) x->node_of[o] = 0xffff;
		for (uint32_t i = 0; i < nnode && i < 0xffff; i++) { const uint32_t f = RD16(R + i * 0xc), e = RD16(R + i * 0xc + 2); for (uint32_t o = f; o < e && o < nobj; o++) { x->node_of[o] = (uint16_t)i; if (kz_f(O + o * 8) > 0) x->replaced[i] = 1; } }   /* replaced: a level that starts past 0 -- the group has a coarser stand-in */
	}
	uint8_t *out = into_svis ? x->svis : x->tvis; memset(out, 0, nobj);
	{	/* RECOMP_KZ_LOD_TREE=<frame>: the tree once (the scenery fallback's design) */
		static long at = -2; if (at == -2) { const char *e = getenv("RECOMP_KZ_LOD_TREE"); at = e ? atol(e) : -1; }
		if (at >= 0 && (long)display_frames() >= at && !into_svis) { at = -1;
			unsigned hist[16] = {0}; for (uint32_t i = 0; i < nnode; i++) { uint32_t d = RD8(D + i); hist[d < 15 ? d : 15]++; }
			fprintf(stderr, "    kz lod tree: %u nodes, %u objects; depth:", nnode, nobj); for (int d = 0; d < 16; d++) if (hist[d]) fprintf(stderr, " %d:%u", d, hist[d]); fprintf(stderr, "\n");
			for (uint32_t i = 0; i < nnode && i < 60; i++) { const uint32_t n = N + i * 0x20, r = R + i * 0xc; const uint32_t f = RD16(r), e = RD16(r + 2);
				fprintf(stderr, "    kz lod node %u depth %u parent-range %.0f..%.0f own %.0f..%.0f objects %u..%u:", i, RD8(D + i), sqrtf(kz_f(n + 0x18)), sqrtf(fminf(kz_f(n + 0x1c), 1e12f)), sqrtf(kz_f(r + 4)), sqrtf(fminf(kz_f(r + 8), 1e12f)), f, e);
				for (uint32_t o = f; o < e && o < f + 8; o++) fprintf(stderr, " [%.0f,%.0f)", sqrtf(kz_f(O + o * 8)), sqrtf(fminf(kz_f(O + o * 8 + 4), 1e12f)));
				fprintf(stderr, "\n"); } } }
	float dist[66]; uint8_t vis[66]; dist[0] = 0; vis[0] = 1; const float s2 = scale * scale;
	for (uint32_t i = 0; i < nnode; i++) {
		const uint32_t depth = RD8(D + i); if (depth > 63) return;
		int ok = 0;
		if (vis[depth]) {
			const uint32_t n = N + i * 0x20; const float pd = dist[depth];
			if (pd >= kz_f(n + 0x18) && pd < kz_f(n + 0x1c)) {
				float px = cx > kz_f(n) ? cx : kz_f(n); if (!(px < kz_f(n + 4))) px = kz_f(n + 4);
				float py = cy > kz_f(n + 8) ? cy : kz_f(n + 8); if (!(py < kz_f(n + 0xc))) py = kz_f(n + 0xc);
				float pz = cz > kz_f(n + 0x10) ? cz : kz_f(n + 0x10); if (!(pz < kz_f(n + 0x14))) pz = kz_f(n + 0x14);
				float d = ((px - cx) * (px - cx) + (pz - cz) * (pz - cz) + (py - cy) * (py - cy)) * s2;
				if (bias != 0) { d = sqrtf(d) + bias; d = d > 0 ? d * d : 0; }
				dist[depth + 1] = d; ok = 1;
				const uint32_t r = R + i * 0xc;
				if (d >= kz_f(r + 4) && d < kz_f(r + 8))
					for (uint32_t o = RD16(r), e = RD16(r + 2); o < e && o < nobj; o++) if (d >= kz_f(O + o * 8) && d < kz_f(O + o * 8 + 4)) out[o] = 1;
			}
		}
		vis[depth + 1] = (uint8_t)ok;
	}
}
static int kz_lod_diag(void) { static int on = -1; if (on < 0) on = getenv("RECOMP_KZ_LOD_DIAG") != 0; return on; }
void recomp_ihook_kz_lod(Cpu *c, int reg) {
	(void)reg; const double k = kz_lod_factor();
	if (k == 1.0 && !kz_lod_diag()) return;
	if (!getenv("RECOMP_KZ_LOD_NOFALLBACK")) kz_lod_true(c->r[0], c->vfp.f[0], c->vfp.f[1], c->vfp.f[2], c->vfp.f[3], c->vfp.f[4], 0);
	c->vfp.f[3] = (float)(c->vfp.f[3] / k);
	if (kz_lod_diag()) kz_lod_true(c->r[0], c->vfp.f[0], c->vfp.f[1], c->vfp.f[2], c->vfp.f[3], c->vfp.f[4], 1);
}
/* FUN_81177972's entry (r0 = the draw list's context): see kz_lod_true */
void recomp_hook_kz_sdl(Cpu *c) {
	if ((kz_lod_factor() == 1.0 && !kz_lod_diag()) || getenv("RECOMP_KZ_LOD_NOFALLBACK")) return;
	const uint32_t ctx = c->r[0], S = RD32(ctx + 0x18); KzLodScene *x = kz_lod_scene(S, 0);
	if (!x || !x->tvis) return;
	const uint32_t stamp = ++x->stamp, flags = RD32(S + 0x1c), R = RD32(S + 0x14);
	unsigned listed = 0, undecoded = 0;
	for (int v = 0; v < 4; v++) {
		const uint32_t set = RD32(ctx + 4 * v); if (!set) continue;
		const uint32_t ent = RD32(set + 0x64); if (!ent) continue;
		const uint32_t count = RD32(ent), base = RD32(ent + 8), recs = RD32(set + 0x58), ready = RD32A(set + 0x80);
		const uint32_t m = ready < count ? ready : count; undecoded += count - m;
		for (uint32_t e = 0; e < m; e++) {
			const uint32_t f = RD16(base + e * 8 + 2), end = RD16(base + e * 8 + 4);
			for (uint32_t r = f; r < end; r++) { const uint32_t o = RD32(recs + r * 16 + 4); if (o < x->nobj && x->member[o] != stamp) { x->member[o] = stamp; listed++; } }
		}
	}
	unsigned fixed = 0, fixed_objs = 0, mismatch = 0;
	if (kz_lod_diag()) for (uint32_t o = 0; o < x->nobj; o++) if (x->node_of[o] == 0xffff || !x->replaced[x->node_of[o]]) mismatch += !!(RD8(flags + o) & 2) != x->svis[o];   /* (groups it leaves scaled: the draw list runs more than once a frame, and the stand-in groups were put back already) */
	for (uint32_t i = 0; i < x->nnode; i++) {
		const uint32_t r = R + i * 0xc, f = RD16(r), e = RD16(r + 2);
		int scaled_missing = 0, true_listed = 0;
		for (uint32_t o = f; o < e && o < x->nobj; o++) {
			const int sv = RD8(flags + o) & 2, ls = x->member[o] == stamp;
			if (sv && !ls) scaled_missing = 1;
			if (!sv && ls && x->tvis[o]) true_listed = 1;
		}
		if (!x->replaced[i] && (!scaled_missing || !true_listed)) continue;
		fixed++;
		for (uint32_t o = f; o < e && o < x->nobj; o++) { const uint8_t fl = RD8(flags + o); const uint8_t nf = (uint8_t)((fl & ~2u) | (x->tvis[o] ? 2u : 0u)); if (nf != fl) { WR8(flags + o, nf); fixed_objs++; } }
	}
	if (kz_lod_diag()) { static uint64_t last; const uint64_t fr = display_frames(); if (fr - last >= 150) { last = fr;
		fprintf(stderr, "    kz scenery: frame %llu: %u objects listed, %u entries not yet decoded, %u nodes back to the game's pick (%u objects); walk vs the game's flags: %u of %u differ\n", (unsigned long long)fr, listed, undecoded, fixed, fixed_objs, mismatch, x->nobj); } }
}
void recomp_ihook_kz_phys_dt(Cpu *c, int reg) { (void)reg; if (kz_sched_60 && !kz_no('p')) c->vfp.f[0] *= 0.5f; }
/* fn_81239b76 (r0 = the group table: count +0x14, record indices +0x18, 0x30-byte records +0xc: phase +0, period +4,
 * countdown +0x14): RECOMP_KZ_60_DIAG_FROM=frame logs the groups for 20 frames */
void recomp_hook_kz_groups(Cpu *c) {
	static long d_from = -2, d_to; if (d_from == -2) { const char *e = getenv("RECOMP_KZ_60_DIAG_FROM"); d_from = e ? atol(e) : -1; d_to = d_from + 20; }
	if (d_from < 0 || (long)display_frames() < d_from || (long)display_frames() > d_to) return;
	const uint32_t m = c->r[0], n = RD32(m + 0x14), idx = RD32(m + 0x18), recs = RD32(m + 0xc);
	fprintf(stderr, "    kz groups: frame %llu time %u player %#x period %u:", (unsigned long long)display_frames(), RD32(c->r[13]), kz_player, kz_player ? RD32(kz_player + 0x4c) : 0);
	for (uint32_t k = 0; k < n && k < 8; k++) { uint32_t rec = recs + RD32(idx + 4 * k) * 0x30; fprintf(stderr, " [phase %u period %u countdown %d]", RD32(rec), RD32(rec + 4), (int32_t)RD32(rec + 0x14)); }
	fprintf(stderr, "\n");
}
/* 0x8158bd2e (fn_8158bd0a building its 1/30 step, once a frame on the main thread, before any entity runs): at 60 the flag
 * on and the step 1/60 (the flag stays on back at 30: the game sets it itself in play; clearing it held the player at 15 Hz);
 * and the player moved to the setting's group -- 16667 at 60, 33333 at 30 -- when its period says otherwise (only from
 * 33333/16667: the 15 Hz mode of the level's intro is left alone), if its aim update ran in the last few frames */
/* the world update's fixed 1/15 s step for its round-robin systems (0x8158b9d4: FUN_81184442 and FUN_81616ea8, called once a
 * world update with s0 = 1/15): the world update comes every frame at 60, twice as often, so the step is 1/30 */
void recomp_ihook_kz_world_dt(Cpu *c, int reg) {
	if (reg < 0 || reg > 14 || c->r[reg] != 0x3d888889u || !kz_sched_60 || kz_no('w')) return;
	const float dt = 1.0f / 30.0f; memcpy(&c->r[reg], &dt, 4);
}
/* the scheduler's 15 Hz tick (0x8158c00a reads its flag +0x154; the block at 0x8158c01a calls 0x81a9e2ec's object's vtable +0x48
 * with the simulation time and a fixed 1/15 s, then clears the flag): the flag is set every other world update, and at 60 the
 * world updates every frame -- the tick came at ~30 Hz and the level intro's cutscene played 1.6x fast (2026-10-18). At 60
 * every second tick is dropped (its flag cleared, unseen): 15 Hz again. */
void recomp_ihook_kz_tick_gate(Cpu *c, int reg) {
	if (reg < 0 || reg > 14 || !c->r[reg] || !kz_sched_60 || kz_no('t')) return;
	static int odd; odd ^= 1;
	if (!odd) { c->r[reg] = 0; WR8(c->r[6] + 0x154, 0); }
}
/* RECOMP_KZ_60_DIAG_FROM (20 frames): the scheduler's sub-steps (fn_8112f5a4, s0 = its step) and its 15 Hz tick (0x8158c09a) */
static int kz_diag_window(void) { static long d_from = -2; if (d_from == -2) { const char *e = getenv("RECOMP_KZ_60_DIAG_FROM"); d_from = e ? atol(e) : -1; } return d_from >= 0 && (long)display_frames() >= d_from && (long)display_frames() <= d_from + 20; }
void recomp_hook_kz_substep(Cpu *c) { if (kz_diag_window()) fprintf(stderr, "    kz substep: frame %llu dt %.5f last %u\n", (unsigned long long)display_frames(), c->vfp.f[0], c->r[1]); }
void recomp_ihook_kz_tick_log(Cpu *c, int reg) { (void)reg; if (kz_diag_window()) fprintf(stderr, "    kz tick: frame %llu\n", (unsigned long long)display_frames()); }
/* fn_8158b886 (the world update: r0 the scheduler, r2:r3 its deadline, [sp] the frame's time in us): logged with the groups */
/* RECOMP_KZ_FIND_VS=<frame>: once, at that frame, guest memory searched for ViewportSetup structs (the reflection's fields:
 * +0 Name, +4 FOVMultiplier, +8 FPFOVMultiplier, +0x24 AspectRatioMultiplier, +0x28 LODFadingEnabled, +0x2c
 * LODDistanceMultiplier, +0x30 LowLODDistanceMultiplier, +0x34 TextureMipMapBias, +0x38/+0x3c LightingFOVScale/Bias, +0x40
 * ShadowMapSizeThresholdMultiplier, +0x44 MotionVectorScale) -- where the LOD distance setting lives */
void recomp_hook_kz_world(Cpu *c) {
	if (kz_npc_ent) kz_npc_line(kz_npc_ent, "world", 0);
	static long d_from = -2, d_to; if (d_from == -2) { const char *e = getenv("RECOMP_KZ_60_DIAG_FROM"); d_from = e ? atol(e) : -1; d_to = d_from + 20; }
	if (d_from < 0 || (long)display_frames() < d_from || (long)display_frames() > d_to) return;
	fprintf(stderr, "    kz world: frame %llu time %u deadline %llu\n", (unsigned long long)display_frames(), RD32(c->r[13]), (unsigned long long)c->r[2] | (unsigned long long)c->r[3] << 32);
}
uint32_t guest_call(Cpu *parent, uint32_t fn, uint32_t a0, uint32_t a1);
void recomp_ihook_kz_sched(Cpu *c, int reg) {
	if (reg < 0 || reg > 14 || c->r[reg] != 0x3d088889u) return;
	const int on = kz_60_on();
	const int live = kz_player && display_frames() - kz_player_seen < 10;
	const uint32_t pp = live ? RD32(kz_player + 0x4c) : 0;
	kz_sched_60 = on && live && (pp == 33333u || pp == 16667u);
	static uint32_t vflag = 1, setp = 1;   /* the scheduler's frames-by-vblank flag; FUN_81244552(entity, period), Thumb (1.00 addresses) */
	if (vflag == 1) { vflag = game_addr("kz_vblank_flag"); setp = game_addr("kz_set_period"); }
	if (on && vflag && RD8(vflag) != 1) WR8(vflag, 1);
	if (kz_sched_60 && !kz_no('s')) { const float dt = 1.0f / 60.0f; memcpy(&c->r[reg], &dt, 4); }
	if (live) {
		const uint32_t p = pp, want = on ? 16667u : 33333u;
		if ((p == 33333u || p == 16667u) && p != want && setp) guest_call(c, setp, kz_player, want);   /* FUN_81244552(entity, period): sets +0x4c and re-files */
	}
	static int diag = -1; if (diag < 0) diag = getenv("RECOMP_KZ_60_DIAG") != 0;
	if (diag) { uint32_t s = c->r[6]; static unsigned n; if (n++ % 30 == 0) fprintf(stderr, "    kz 60: frame %llu on %d flag %u player %#x period %u deadline %llu now %llu\n", (unsigned long long)display_frames(), on, vflag ? RD8(vflag) : 0, kz_player, kz_player ? RD32(kz_player + 0x4c) : 0, (unsigned long long)RD32(s + 0x120) | (unsigned long long)RD32(s + 0x124) << 32, (unsigned long long)guest_time_us_peek()); }
}

/* Killzone's mouse look (F8 / window_raw_look), on the player's view rotation. fn_8155b646, the player's aim update (on the
 * <Player> fiber), ends by turning the view through fn_813d4634(r0 = the player r5, s0 = this frame's yaw change in radians --
 * the right stick's ramped term times the turn speed ([r8+0x2c8] degrees a second, 240 at full tilt) times dt, to the right
 * negative -- s1 = the new pitch: the current one (+0xa0) plus the stick's change, held to the game's limits [sp+0x20]..
 * [sp+0x24]). Two call sites: plain aiming (0x8155c786) and a mode with its own yaw store (+0x664: 0x8155c6b2); the hooks
 * run on the instruction before each call. The mouse's counts join both arguments -- about 0.086 degrees a count at
 * sensitivity 1, linear, no ramp -- and the pitch is held to the same limits. Found 2026-10-18: RECOMP_MEMSCAN for the
 * stick's yaw rate (0 at rest, -c right, +c left) and an lldb hardware watchpoint on it (memscan_best) led to this function;
 * its rate feeds the weapon sway (fn_81501506), its delta the view. RECOMP_KZ_AIM_DIAG=1: the arguments each frame. */
static __thread float kz_mouse_yaw;   /* this frame's mouse yaw, radians (kz_aim -> kz_aim_term) */
static float kz_rdf(uint32_t a) { uint32_t b = RD32(a); float f; memcpy(&f, &b, 4); return f; }
void recomp_ihook_kz_aim(Cpu *c, int reg) {
	(void)reg;
	kz_mouse_yaw = 0;
	kz_player_mark();
	const uint32_t sp = c->r[13];
	const float lo = kz_rdf(sp + 0x20), hi = kz_rdf(sp + 0x24);
	static int diag = -1; if (diag < 0) diag = getenv("RECOMP_KZ_AIM_DIAG") != 0;
	if (diag) {   /* (+ the body's heading, from its matrix -- FUN_813d4634 reads it at [[r0+0x20]+0xa0]: rows of 4 floats) */
		const uint32_t e = RD32(c->r[0] + 0x20); float h0 = 0, h1 = 0;
		if (e) { h0 = atan2f(kz_rdf(e + 0xa8), kz_rdf(e + 0xa0)); h1 = atan2f(kz_rdf(e + 0xc0), kz_rdf(e + 0xa0)); }
		fprintf(stderr, "    kz aim: frame %llu yaw change %.5f pitch %.5f (limits %.3f..%.3f) dt %.5f heading %.4f %.4f m", (unsigned long long)display_frames(), c->vfp.f[0], c->vfp.f[1], lo, hi, c->vfp.f[30], h0, h1);
		for (int q = 0; q < 16 && e; q++) fprintf(stderr, " %.3f", kz_rdf(e + 0xa0 + 4 * q));   /* (rows +0xa0..+0xcc, position +0xd0) */
		fprintf(stderr, "\n");
	}
	if (!window_raw_look()) return;
	float dx = 0, dy = 0; window_mouse_delta(&dx, &dy);
	if (dx == 0 && dy == 0) return;
	const float k = window_mouse_sens() * 0.0015f, inv = window_mouse_invert() ? -1.0f : 1.0f;
	float p = c->vfp.f[1] - dy * inv * k;   /* (counts: y down; the pitch grows up) */
	if (lo < hi) { if (p > hi) p = hi; if (p < lo) p = lo; }
	c->vfp.f[0] -= dx * k;
	c->vfp.f[1] = p;
	kz_mouse_yaw = -dx * k;   /* (the same turn through the stick's other path: kz_aim_term, just after) */
	if (diag) fprintf(stderr, "    kz aim: mouse %g %g -> yaw change %.5f pitch %.5f\n", dx, dy, c->vfp.f[0], c->vfp.f[1]);
}
/* 0x8155c79a, just before fn_8155b646 hands the player controller its turn input (vtable +0x94, r1 = &[sp+0x10]: the right
 * stick's ramped yaw term, s0 = the turn speed in radians a second; the controller keeps it at +0x10c): the yaw change
 * above drives the view only in some states (the tutorial's look-around); in plain play the turn is this term times the
 * turn speed times dt. The mouse's yaw joins it as the term that turns exactly that far this frame. */
void recomp_ihook_kz_aim_term(Cpu *c, int reg) {
	(void)reg;
	const float y = kz_mouse_yaw; kz_mouse_yaw = 0;
	const float speed = c->vfp.f[0], dt = c->vfp.f[30];
	if (y == 0 || !(speed > 0) || !(dt > 0)) return;
	const uint32_t a = c->r[13] + 0x10; float t; uint32_t b = RD32(a); memcpy(&t, &b, 4);
	t += y / (speed * dt); memcpy(&b, &t, 4); WR32(a, b);
	static int diag = -1; if (diag < 0) diag = getenv("RECOMP_KZ_AIM_DIAG") != 0;
	if (diag) fprintf(stderr, "    kz aim: turn term %.4f (speed %.3f dt %.4f)\n", t, speed, dt);
}

/*
 * Killzone job system trace (RECOMP_KZ_JOBTRACE=1), for the freeze where a worker hands work to itself (2026-10-17: lldb
 * caught JobSched_002 spinning in the hand-off fn_81088772 on its own record, waiting for itself to go to sleep). A
 * worker going idle (fn_81089fce) lowers the active count, pushes its record onto the idle queue (fn_8179d6c0) and sleeps
 * (fn_8108855c: its record again, from TLS; +1 on the record's waiting count; WaitEventFlag); a dispatcher (fn_81089432)
 * pops a record and wakes it through fn_81088772. Each of those is an event in a ring; a pop of one's own record or a
 * sleep on someone else's record prints the ring. Records here are the inner one ([+0] thread uid, [+0xc] waiting count,
 * [+0x14] event flag); the idle queue holds the worker struct 4 bytes before it.
 */
extern uint32_t recomp_cur_tid(void);
extern const char *recomp_cur_tname(void);
typedef struct { uint64_t seq; const char *what; uint32_t tid, rec, uid, tok, x; char tn[16]; } KzEv;
#define KZ_EV 1024
static KzEv kz_ring[KZ_EV]; static uint64_t kz_seq; static pthread_mutex_t kz_m = PTHREAD_MUTEX_INITIALIZER; static int kz_dumps;
static uint32_t kz_recs[8]; static int kz_nrecs;   /* every record seen sleeping (kz_sleep, always): dumped with the ring and the queue */
static uint32_t kz_sched;   /* the job scheduler (the dispatcher's r0): its idle queue at +0x680 */
static int kz_jobtrace(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_KZ_JOBTRACE") != 0; return v; }
static void kz_ev_dump(const char *why) {   /* lock-free recording: a dump reads what the ring holds now */
	pthread_mutex_lock(&kz_m);
	if (kz_dumps++ < 3) {
		uint64_t end = __atomic_load_n(&kz_seq, __ATOMIC_ACQUIRE);
		fprintf(stderr, "kz jobs: %s (%s) -- the last %d events (seq thread: event record uid waiting extra):\n", why, recomp_cur_tname(), KZ_EV);
		for (uint64_t s2 = end > KZ_EV ? end - KZ_EV : 0; s2 < end; s2++) {
			KzEv *e = &kz_ring[s2 % KZ_EV];
			fprintf(stderr, "kz jobs:   %llu %s(%#x): %s rec %#x uid %#x waiting %u x %#x\n", (unsigned long long)e->seq, e->tn, e->tid, e->what, e->rec, e->uid, e->tok, e->x);
		}
		for (int k = 0; k < kz_nrecs; k++) { uint32_t r = kz_recs[k]; fprintf(stderr, "kz jobs:   record %#x: uid %#x waiting %u evf %#x popped-flag %u\n", r, RD32(r), RD32(r + 0xc), RD32(r + 0x14), RD8(r - 4 + 0x9c)); }
	}
	pthread_mutex_unlock(&kz_m);
}
static void kz_ev2(const char *what, uint32_t rec, uint32_t x) {
	if (!kz_jobtrace()) return;
	uint64_t n = __atomic_fetch_add(&kz_seq, 1, __ATOMIC_ACQ_REL);
	KzEv *e = &kz_ring[n % KZ_EV]; e->seq = n; e->what = what; e->tid = recomp_cur_tid(); e->rec = rec; e->x = x;
	e->uid = rec ? RD32(rec) : 0; e->tok = rec ? RD32(rec + 0xc) : 0; snprintf(e->tn, sizeof e->tn, "%s", recomp_cur_tname());
	if (!strcmp(what, "pop") && e->uid == e->tid) kz_ev_dump("a dispatcher popped its own record");
	if (!strcmp(what, "push") && e->uid != e->tid) kz_ev_dump("a worker pushed another thread's record");
	if (!strcmp(what, "sleep") && e->uid != e->tid) kz_ev_dump("a worker sleeps on another thread's record");
	if (!strcmp(what, "woke") && RD8(rec - 4 + 0x9c)) kz_ev_dump("a worker woke that no dispatcher popped");
	if (!strcmp(what, "woke") && getenv("RECOMP_KZ_JOBTRACE")[0] == '2' && n > 200) kz_ev_dump("sanity: a wake, 200 events in");
}
static void kz_ev(const char *what, uint32_t rec) { kz_ev2(what, rec, 0); }
void recomp_hook_kz_q_push(Cpu *c) {   /* (the job system's own code: kz_jobs_lo..kz_jobs_hi; 1.00 0x81089000..0x8108b000) */
	static uint32_t lo = 1, hi; if (lo == 1) { lo = game_addr("kz_jobs_lo"); hi = game_addr("kz_jobs_hi"); }
	if (c->r[14] >= lo && c->r[14] < hi) kz_ev("push", c->r[1] + 4);
}   /* fn_8179d6c0(queue, worker): the idle queue's push (from the job system only) */
/* fn_81088772(record): the hand-off. A dispatcher handed its OWN record waits there for itself to go to sleep -- forever:
 * the freeze at the M02 dropship and deeper in play (2026-10-17, lldb: JobSched_002 spinning in it on its record, waiting
 * count 0, the other workers asleep, the main thread waiting on the job group). The pop read a slot before the commit
 * index that published it and got its own record from the lap before (see kz_pop; 2026-10-18, kz_queue_dump: the worker
 * whose entry it consumed asleep and missing from the queue) -- fixed by recomp.py's ordered loads in lock-free code.
 * Pointing the call elsewhere does not save the run (that worker sleeps on, and the active count is one too high), so
 * this only reports it. */
void recomp_hook_kz_wake(Cpu *c) {
	kz_ev("wake", c->r[0]);
	if (!c->r[0] || RD32(c->r[0]) != recomp_cur_tid()) return;
	static unsigned n; if (n++ < 20) fprintf(stderr, "    kz jobs: %s was handed its own record %#x to wake (waiting count %u): it will wait for itself (%u so far)\n", recomp_cur_tname(), c->r[0], RD32(c->r[0] + 0xc), n);
}
/* 0x81089456: r1 = the worker the dispatcher popped. The pop loads the queue's commit index (+0x6a0) and then the slot
 * with no barrier between them; an ARM core may satisfy the slot load first, and gets the slot's value from the lap
 * before. RECOMP_KZ_POPCHECK=1: after a fence the slot is re-read, and a different value while the read index (+0x6c0)
 * has not moved is that stale load, reported (not corrected). Off by default: the fence alone changed the timing enough
 * that six runs in a row stopped freezing. */
void recomp_ihook_kz_pop(Cpu *c, int reg) {
	(void)reg;
	kz_sched = c->r[0];
	static int chk = -1; if (chk < 0) chk = getenv("RECOMP_KZ_POPCHECK") != 0;
	if (chk) {
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		uint32_t s = c->r[0], idx = c->r[4], fresh = RD32(c->r[3] + ((idx & (RD32(s + 0x6cc) - 1)) << 2));
		if (fresh != c->r[1] && RD32(s + 0x6c0) == idx) {
			static unsigned n; if (n++ < 20) fprintf(stderr, "    kz jobs: %s popped a stale slot %u: worker %#x, the slot now holds %#x (commit %u) (%u so far)\n", recomp_cur_tname(), idx, c->r[1], fresh, RD32(s + 0x6a0), n);
		}
	}
	kz_ev("pop", c->r[1] + 4);
}
void recomp_ihook_kz_sleep(Cpu *c, int reg) {   /* 0x8108858a: r4 = the sleeper's record, from TLS */
	(void)reg; uint32_t rec = c->r[4]; int k, n = __atomic_load_n(&kz_nrecs, __ATOMIC_ACQUIRE);
	for (k = 0; k < n && kz_recs[k] != rec; k++) {}
	if (k == n) { pthread_mutex_lock(&kz_m); for (k = 0; k < kz_nrecs && kz_recs[k] != rec; k++) {} if (k == kz_nrecs && k < 8) { kz_recs[k] = rec; __atomic_store_n(&kz_nrecs, k + 1, __ATOMIC_RELEASE); } pthread_mutex_unlock(&kz_m); }
	kz_ev("sleep", rec);
}
void recomp_ihook_kz_woke(Cpu *c, int reg) { (void)reg; kz_ev2("woke", c->r[4], c->r[0]); }   /* 0x810885d4: WaitEventFlag returned (x: its result) */

/* stallprof.c, after each report of a stall (RECOMP_STALL_PROF): past 5 s, once per run, the idle queue as it stands --
 * which workers sleep, which records the queue holds between its read and commit indices, the active count. A sleeper
 * missing from the queue was lost to a stale pop; a read index past the commit index is a pop that read the indices out
 * of order. With RECOMP_KZ_JOBTRACE, the trace's ring as well. */
static void kz_queue_dump(void) {
	uint32_t s = kz_sched; if (!s) return;
	uint32_t q = s + 0x680, head = RD32(q), commit = RD32(q + 0x20), rd = RD32(q + 0x40), slots = RD32(q + 0x44), cap = RD32(q + 0x4c);
	fprintf(stderr, "kz jobs: idle queue at %#x: reserved %u, committed %u, read %u (capacity %u); active %u of %u\n", q, head, commit, rd, cap, RD32(s + 0x6e0), RD32(s + 0x640));
	for (uint32_t i = rd; (int32_t)(commit - i) > 0 && i - rd < 16 && cap; i++) { uint32_t w = RD32(slots + ((i & (cap - 1)) << 2)); fprintf(stderr, "kz jobs:   slot %u: worker %#x (record %#x, uid %#x)\n", i, w, w + 4, RD32(w + 4)); }
	for (int k = 0; k < kz_nrecs; k++) {
		uint32_t r = kz_recs[k], inq = 0;
		for (uint32_t i = rd; (int32_t)(commit - i) > 0 && i - rd < 64 && cap; i++) if (RD32(slots + ((i & (cap - 1)) << 2)) + 4 == r) inq++;
		fprintf(stderr, "kz jobs:   record %#x: uid %#x waiting %u popped-flag %u, in the queue %u time(s)\n", r, RD32(r), RD32(r + 0xc), RD8(r - 4 + 0x9c), inq);
	}
}
static void kz_stall_long(uint64_t stall_us) {   /* stallprof.c's hook_stall_long */
	if (stall_us <= 5000000u) return;
	static int once; if (!once++) kz_queue_dump();
	if (kz_jobtrace()) kz_ev_dump("a stall of over 5 s");
}
__attribute__((constructor)) static void kz_stall_hook(void) { hook_stall_long = kz_stall_long; }

/*
 * Killzone: Mercenary (PCSA00107, 1.12) said a line twice (2026-10-25..28): at 60 fps the same line again ~0.7 s after it
 * ended, several times a conversation, now and then. How a line is said: the AI sets "say" in its state; each character's
 * apply (fn_814b075a, or fn_8142363e for another controller class; job threads, every frame) reads the replicated copy of
 * that state and, while its say flag (+0xf0) is set, either sends a type 0x45 speech message, which the main thread turns
 * into a queued request (QueueSpeech fn_8148a912), or queues the request directly. The character says its queue's head
 * (fn_8147ff5c -> SayLine fn_8147f886) and the next one after the line's length plus a pause. At 60 fps one snapshot's say
 * can be read by two applies (every double seen: exactly 2 frames apart), so the line went into the queue twice. Fix: a say
 * is queued once per snapshot (kz_speech_once on the message route, kz_say_send / kz_say_direct on the direct ones).
 * RECOMP_KZ_SND_REQ=1 logs stream voice starts (kz_strm_voice), the game's stream API calls (kz_strm_api), every SayLine
 * with its behaviour's call chain (kz_say) and every say the fix turns away.
 */
void ngs_chain(char *out, size_t n);
static int kz_snd_req(void) { static int on = -1; if (on < 0) { const char *e = getenv("RECOMP_KZ_SND_REQ"); on = e && atoi(e) > 0; } return on; }
static void kz_guest_str(uint32_t a, char *out, size_t n) {   /* a guest C string, printable characters only ("?" when unreadable) */
	size_t k = 0;
	if (a >= 0x10000u) for (; k + 1 < n; k++)   /* (the guest's 4 GB are all mapped; watched pages fault into their handler) */ { const uint8_t ch = RD8(a + (uint32_t)k); if (!ch) break; out[k] = ch >= 32 && ch < 127 ? (char)ch : '?'; }
	if (!k && n > 1) out[k++] = '?';
	out[k] = 0;
}
void recomp_hook_kz_strm_voice(Cpu *c) {
	if (!kz_snd_req()) return;
	const uint32_t st = c->r[1], t = st ? RD32(st + 0x1b74) : 0; char name[96]; kz_guest_str(t ? RD32(t + 0x198) : 0, name, sizeof name);
	/* the line: t's +0x20, +0x68 (its size, again at +0x7c) and +0x70 differ line to line inside one speech lump; r0's +0xc is
	 * when it was asked for (a tick count, shared by streams asked for in one tick): a repeat with the first start's time is the
	 * same request started again, a later time a new request */
	fprintf(stderr, "    kz snd: stream voice start '%s' line %#x/%#x/%#x asked %#x stream %#x frame %llu\n", name, t ? RD32(t + 0x20) : 0, t ? RD32(t + 0x68) : 0,
	        t ? RD32(t + 0x70) : 0, RD32(c->r[0] + 0xc), c->r[0], (unsigned long long)display_frames());
	if (getenv("RECOMP_KZ_SND_DUMP")) {   /* RECOMP_KZ_SND_DUMP=1: both records' words (where the fields above came from) */
		fprintf(stderr, "    kz snd dump t %#x:", t); for (int i = 0; i < 0x1a0 / 4; i++) fprintf(stderr, " %08x", t ? RD32(t + 4 * i) : 0);
		fprintf(stderr, "\n    kz snd dump r0:"); for (int i = 0; i < 64; i++) fprintf(stderr, " %08x", RD32(c->r[0] + 4 * i));
		fprintf(stderr, "\n");
	}
}
static void kz_strm_api(Cpu *c, const char *which) {
	if (!kz_snd_req()) return;
	static __thread uint64_t sec; static __thread int n; const uint64_t now = display_frames() / 60;   /* (at most 20 a second a thread: a per-frame caller would flood the log) */
	if (now != sec) { sec = now; n = 0; }
	if (++n > 20) return;
	char chain[400]; ngs_chain(chain, sizeof chain);
	fprintf(stderr, "    kz snd: stream api %s(%#x, %#x, %#x, %#x) frame %llu from %s\n", which, c->r[0], c->r[1], c->r[2], c->r[3], (unsigned long long)display_frames(), chain);
}
void recomp_hook_kz_strm_api_a(Cpu *c) { kz_strm_api(c, "8179a282"); }
void recomp_hook_kz_strm_api_b(Cpu *c) { kz_strm_api(c, "8179a134"); }
/* a character's SayLine (fn_8147f886, via fn_8147fbfc from the behaviours fn_8147ff5c and fn_81480378): schedules Say
 * (fn_814a2c10: stops the character's current speech, plays the line) and returns the line's length plus a pause */
void recomp_hook_kz_say(Cpu *c) {
	if (!kz_snd_req()) return;
	char chain[600]; ngs_chain(chain, sizeof chain);
	fprintf(stderr, "    kz snd: say char %#x line %#x r2 %#x r3 %#x s0 %.3f frame %llu from %s\n", c->r[0], c->r[1], c->r[2], c->r[3], c->vfp.f[0],
	        (unsigned long long)display_frames(), chain);
}
static int kz_say_twice(uint32_t ctl, uint32_t src, const char *where) {
	const uint64_t now = display_frames();
	static struct { uint32_t ctl, src; uint64_t at; } last[64]; static int n;
	int i; for (i = 0; i < n && last[i].ctl != ctl; i++) {}
	if (i == n) { if (n == 64) i = (int)(now % 64); else n++; last[i].ctl = ctl; last[i].src = 0; last[i].at = 0; }
	const int again = last[i].src == src && src && now - last[i].at <= 3;
	if (kz_snd_req()) fprintf(stderr, "    kz snd: say %s %s controller %#x src %#x frame %llu\n", where, again ? "DROPPED (same snapshot again)" : "queued directly", ctl, src, (unsigned long long)now);
	if (!again) { last[i].src = src; last[i].at = now; }
	return again;
}
void recomp_ihook_kz_say_send(Cpu *c, int reg) {
	(void)reg;
	if (!c->r[0]) return;
	const uint32_t sl = c->r[10], fp = c->r[11], ent = RD32(sl + 0x20);
	if (ent && RD32(ent + 0x110)) { if (kz_snd_req()) fprintf(stderr, "    kz snd: say send copy %#x frame %llu\n", fp, (unsigned long long)display_frames()); return; }
	if (kz_say_twice(sl, RD32(fp + 0xf4), "(814b075a)")) c->r[0] = 0;
}
void recomp_ihook_kz_say_direct(Cpu *c, int reg) {
	(void)reg;
	if (!c->r[0]) return;
	if (kz_say_twice(c->r[4], RD32(c->r[5] + 0xf4), "(8142363e)")) c->r[0] = 0;
}
/* the message route: QueueSpeech (fn_8148a912, the main thread) just after it loads the character's speech state
 * (0x8148a91a: r4 = [r7 + 0x164], 0 = nothing to queue; r7 = the character, r1 = the message, its line id at +0xc): the same
 * line for the same character again within half a second is the same snapshot sent twice, turned away the function's own
 * way (r4 = 0). A line said again on purpose comes much later. */
void recomp_ihook_kz_speech_once(Cpu *c, int reg) {
	(void)reg;
	if (!c->r[4]) return;
	const uint32_t ch = c->r[7], id = RD32(c->r[1] + 0xc);
	const uint64_t now = display_frames();
	static struct { uint32_t ch, id; uint64_t at; } last[64]; static int n;
	int i; for (i = 0; i < n && last[i].ch != ch; i++) {}
	if (i == n) { if (n == 64) i = (int)(now % 64); else n++; last[i].ch = ch; last[i].id = 0; last[i].at = 0; }
	if (last[i].id == id && now - last[i].at <= 30) {
		if (kz_snd_req()) fprintf(stderr, "    kz snd: speech %#x for char %#x again %llu frames later: not queued twice (frame %llu)\n", id, ch, (unsigned long long)(now - last[i].at), (unsigned long long)now);
		c->r[4] = 0; return;
	}
	last[i].id = id; last[i].at = now;
}
