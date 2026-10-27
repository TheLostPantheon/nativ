/*
 * Unit 13 (PCSA00014): its hooks, built into this game alone (runtime/build.py RECOMP_PACK_HOOKS). packs/PCSA00014/pack.toml's
 * HOOKS= and IHOOKS= place them on the game's functions and instructions; what every game's hooks share is
 * runtime/core/hooks.h. Each one is this game's own feature, named for the guest function it hooks.
 */
#include "hooks.h"

/*
 * Unit 13 (PCSA00014): fn_81113916 is the per-frame camera update. From the camera controller's
 * vertical fov fields (+0xb8 and +0xbc, degrees) it builds the world camera (+0x30: its fov field
 * +0x48, then its projection through the builder fn_819f33ec), a copy of it with its own far plane
 * (+0x34, same fov), a third camera from the +0xbc fov (+0x38), and the 31 deg weapon camera (+0x3c),
 * and hands all four to the render system's camera slots (fn_813b8f88/94/a0/ac, last). Widening only
 * the world camera's projection matrix (the first version of this hook) widened the picture but not
 * the visibility culling, which works from the other cameras / the stored fov: objects at the new
 * edges popped in and out. So the fov is widened at its source instead, for exactly the length of
 * this update: the entry hook scales +0xb8/+0xbc, and the hook on the last slot setter (only when
 * called from here: lr 0x81113ea7) puts the controller's own values back -- every camera the update
 * derives agrees with the widened view, and the game's camera state (zoom, blends) never sees it.
 * The weapon camera has its own fixed fov and stays as it is.
 */
static __thread uint32_t u13_cam;        /* the controller being updated, while its fov is widened */
static __thread float u13_saved[2];      /* its own +0xb8/+0xbc */
void recomp_hook_u13_cam_begin(Cpu *c) {
	uint32_t p = c->r[0];
	u13_cam = 0;
	for (int i = 0; i < 2; i++) {
		float deg = rdf(p + 0xb8 + 4 * i);
		double scaled = fov_scale(deg * (M_PI / 180.0));
		u13_saved[i] = deg;
		if (scaled > 0.0) { wrf(p + 0xb8 + 4 * i, (float)(scaled * (180.0 / M_PI))); u13_cam = p; }
	}
	if (getenv("RECOMP_FOV_TRACE")) { static int n; if (n++ % 300 == 0) fprintf(stderr, "u13 camera fov %.2f / %.2f deg -> %.2f / %.2f\n", u13_saved[0], u13_saved[1], rdf(p + 0xb8), rdf(p + 0xbc)); }
}
void recomp_hook_u13_cam_end(Cpu *c) {
	if (c->r[14] != 0x81113ea7u || !u13_cam) return;   /* the slot setter called from anywhere else, or nothing widened */
	wrf(u13_cam + 0xb8, u13_saved[0]); wrf(u13_cam + 0xbc, u13_saved[1]);
	u13_cam = 0;
}


void recomp_ihook_u13_frame_dt(Cpu *c, int reg) { recomp_ihook_frame_dt(c, reg); }   /* (Unit 13's conf names it so) */

/* Unit 13 builds the same constant once a frame, at 0x81113bc6 in its camera update fn_81113916 (the one
 * u13_cam_begin/end wrap): dup'd to d2 and reloaded for all four of the update's projection builds (world
 * camera +0x30, its far-plane copy +0x34, the third camera +0x38, the 31-degree weapon camera +0x3c), from
 * which the render system's camera slots and its culling derive (2026-10-10). Independent of the fov hook:
 * that one scales the vertical fov fields, this the aspect -- separate inputs to the builder, so the two
 * settings compose. The other 960/544 builds in the binary are the PS TV output presets and a UI rect. */
void recomp_ihook_u13_aspect(Cpu *c, int reg) { widen_aspect_reg(c, reg); }

/* Unit 13's mouse look (F8 / window_raw_look), on the player character's aim -- the state the right stick
 * turns, which the body, the camera and its shake follow. fn_8131a730 (r0 the character; s0 a float) is
 * the aim's update: this frame's yaw target +0x1f0 (radians, 0..2pi, growing to the left; fn_81310e4c just
 * made it last frame's +0x780 plus the stick's rate) goes to fn_812e6a10, which writes the aim yaw on to
 * +0xa4 and +0x780; (+0x1f0 - +0x780) / +0x75c is the turn rate the body's turn animation plays (+0x78c);
 * the pitch is the aim controller's plus the persistent offset +0x77c, held to the game's limits by
 * fn_812e6b50 and left at +0xa8 (growing up, -60..+40 degrees). This hook, at fn_8131a730's entry, adds
 * the mouse's counts to +0x1f0 and +0x77c -- about 0.086 degrees a count at sensitivity 1, linear, any
 * speed -- so the body turns with its animation and the camera in the same frame. Found 2026-10-10 by
 * watching who writes the camera and the aim (lldb, the stick held, headless) after hooks on the stick's
 * channels (dead zones, clamps, a top speed) and on the camera's own angles (+0x90/+0x94 of its behaviour:
 * offsets on top -- the view turned, the character didn't) fell short. fn_8131a730 runs for every
 * character: only the player's -- the one the follow camera (the rig 0x820f99a0's behaviour +0x68, class
 * 0x81f35f58) targets at +0x274, remembered for the modes whose cameras keep something else there. */
#define U13_AIM_CAM_SIGN 1.0f   /* the sign of the aim/camera dot in plain play (measured headless) */
uint64_t display_frames(void);
/* the frame's mouse counts, read once (window_mouse_delta drains) and spent once per axis: the aim update's
 * hook below in plain play, the aim setters' (fn_812e6a10 yaw, fn_812e6b50 pitch) on the paths that skip
 * that update -- in cover fn_8134b05e doesn't call fn_8131a730, and another state (fn_81328c5c) sets the
 * aim through the same two setters (found in testing, 2026-10-10: there the mouse had fallen back to
 * window.c's stick stand-in -- stick steps, the game's own invert) */
static uint64_t u13_frame = ~0ull; static float u13_dx, u13_dy; static int u13_yaw_done, u13_pitch_done; static uint32_t u13_player;
static void u13_frame_input(void) {
	uint64_t f = display_frames();
	if (f == u13_frame) return;
	float dx = 0, dy = 0; window_mouse_delta(&dx, &dy);
	if (u13_frame == ~0ull || f - u13_frame > 15) dx = dy = 0;   /* (the aim wasn't running -- the first frame, a menu, a cutscene: what the mouse did meanwhile isn't a turn) */
	u13_frame = f; u13_dx = dx; u13_dy = dy; u13_yaw_done = u13_pitch_done = 0;
}
void recomp_hook_u13_aim(Cpu *c) {
	/* the player: the character the follow camera's behaviour targets (+0x274), remembered -- in cover and the
	 * other modes the camera switches to behaviours whose +0x274 is something else, and the hook then let the
	 * mouse fall back to window.c's right-stick stand-in there (stick steps, the game's own invert: found in
	 * testing, 2026-10-10) */
	uint32_t ch = c->r[0], beh = RD32(0x820f99a0u + 0x68);
	if (beh && RD32(beh) == 0x81f35f58u && RD32(beh + 0x274) == ch) u13_player = ch;
	if (!ch || ch != u13_player) return;
	{	/* RECOMP_YAW_DIAG=3: the look's state four times a second, raw look or not (the stick's path too):
		 * the camera's class, its look's vertical (+0x84), its own pitch/yaw offsets (+0x90/+0x94), the
		 * character's aim pitch (+0xa8) and its stored pitch (+0x77c) */
		static int d = -1; if (d < 0) d = getenv("RECOMP_YAW_DIAG") ? atoi(getenv("RECOMP_YAW_DIAG")) : 0;
		static uint64_t last;
		if (d == 3 && display_frames() - last >= 15) {
			last = display_frames();
			float v84, v90, v94, va8, v77c; uint32_t w;
			w = RD32(beh + 0x84); memcpy(&v84, &w, 4); w = RD32(beh + 0x90); memcpy(&v90, &w, 4); w = RD32(beh + 0x94); memcpy(&v94, &w, 4);
			w = RD32(ch + 0xa8); memcpy(&va8, &w, 4); w = RD32(ch + 0x77c); memcpy(&v77c, &w, 4);
			fprintf(stderr, "u13_look f%llu: camera vt %#x look-y %.3f offsets %.3f %.3f | aim pitch %.3f stored %.3f | raw %d\n",
				(unsigned long long)display_frames(), RD32(beh), v84, v90, v94, va8, v77c, window_raw_look());
		}
	}
	if (!window_raw_look()) return;
	u13_frame_input();
	if (u13_yaw_done && u13_pitch_done) return;
	const float dx = u13_yaw_done ? 0 : u13_dx, dy = u13_pitch_done ? 0 : u13_dy;
	u13_yaw_done = u13_pitch_done = 1;
	if (dx == 0 && dy == 0) return;
	const float k = window_mouse_sens() * 0.0015f, inv = window_mouse_invert() ? -1.0f : 1.0f;
	float yaw, pitch, off; uint32_t wy = RD32(ch + 0x1f0), wp = RD32(ch + 0xa8), wo = RD32(ch + 0x77c);
	memcpy(&yaw, &wy, 4); memcpy(&pitch, &wp, 4); memcpy(&off, &wo, 4);
	yaw -= dx * k;
	const float two_pi = 6.2831853f;
	while (yaw >= two_pi) yaw -= two_pi;
	while (yaw < 0) yaw += two_pi;
	/* mirrored when the camera looks at the character's aim from the front: in cover he faces the camera, and
	 * his "up" is the camera's "down" (found in testing, 2026-10-10: only there up and down swapped). His aim's
	 * direction (+0x350, x y z) against the camera's look (its behaviour's +0x80, x y z), across the ground */
	float fs = 1.0f;
	{
		uint32_t cb = RD32(0x820f99a0u + 0x68); float ax, az, cx, cz; uint32_t w;
		w = RD32(ch + 0x350); memcpy(&ax, &w, 4); w = RD32(ch + 0x358); memcpy(&az, &w, 4);
		w = RD32(cb + 0x80); memcpy(&cx, &w, 4); w = RD32(cb + 0x88); memcpy(&cz, &w, 4);
		float dot = ax * cx + az * cz;
		if (dot * U13_AIM_CAM_SIGN < -0.2f) fs = -1.0f;
		if (getenv("RECOMP_YAW_DIAG")) { static int n; if (n++ % 20 == 0) fprintf(stderr, "u13_aim facing: aim %.2f,%.2f camera %.2f,%.2f dot %.2f -> %s\n", ax, az, cx, cz, dot, fs < 0 ? "mirrored" : "as is"); }
	}
	/* pitch: only as far as the limit, so moving past it doesn't wind up a debt the other way */
	float dp = -dy * k * inv * fs, lo = -1.047198f, hi = 0.698132f;
	if (pitch + dp > hi) dp = hi - pitch;
	if (pitch + dp < lo) dp = lo - pitch;
	if ((-dy * inv * fs < 0) != (dp < 0) && dp != 0) dp = 0;   /* (already beyond the limit the mouse pushes toward: nothing) */
	off += dp;
	memcpy(&wy, &yaw, 4); memcpy(&wo, &off, 4); WR32(ch + 0x1f0, wy); WR32(ch + 0x77c, wo);
	if (getenv("RECOMP_YAW_DIAG")) { static int n; if (n++ % 20 == 0) fprintf(stderr, "u13_aim: character %#x dx=%g dy=%g -> yaw %.3f pitch %.3f (offset %.3f)\n", ch, dx, dy, yaw, pitch, off); }
}
/* the aim setters, for the paths that skip fn_8131a730 (cover): this frame's counts not yet spent go into the
 * angle being set (s0; r0 the character) ahead of the setter's own limits -- the cover arc for the yaw
 * (fn_812e6a10: +0x81c/+0x820), +-s1/s2 for the pitch (fn_812e6b50), which also leaves the result in +0x77c,
 * the next frame's base, so nothing winds up */
void recomp_hook_u13_set_yaw(Cpu *c) {
	if (!window_raw_look() || !u13_player || c->r[0] != u13_player) return;
	u13_frame_input();
	if (u13_yaw_done) return;
	u13_yaw_done = 1;
	if (u13_dx != 0) c->vfp.f[0] -= u13_dx * window_mouse_sens() * 0.0015f;
}
void recomp_hook_u13_set_pitch(Cpu *c) {
	if (!window_raw_look() || !u13_player || c->r[0] != u13_player) return;
	u13_frame_input();
	if (u13_pitch_done) return;
	u13_pitch_done = 1;
	if (u13_dy != 0) c->vfp.f[0] -= u13_dy * window_mouse_sens() * 0.0015f * (window_mouse_invert() ? -1.0f : 1.0f);
	if (getenv("RECOMP_YAW_DIAG")) { static int n; if (n++ % 20 == 0) fprintf(stderr, "u13_set_pitch (another aim path): dx=%g dy=%g\n", u13_dx, u13_dy); }
}

/* Unit 13 in cover: the cover state (r0; its character at +4) aims on its own. fn_81341db0 adds the
 * stick's pitch change (s0) to the character's stored pitch +0x77c and holds it to cover's limits (directly
 * or through fn_812e6b50); fn_813414ae turns the stored yaw +0x780 toward the cover's target +0x1b4 at most
 * +0x214 a second and sets it through fn_8130a858 (the cover arc +0x81c). The frame's counts go to the
 * pitch at fn_81341db0's entry, and to both the yaw and its target at fn_813414ae's -- the gap the game
 * closes stays the stick's, so the mouse turns 1:1, not at cover's turn rate (found 2026-10-10: lldb on the
 * aim's fields while turning in cover with the stick) */
static int u13_cover_player(Cpu *c) {
	if (!window_raw_look() || !u13_player) return 0;
	return RD32(c->r[0] + 4) == u13_player;
}
void recomp_hook_u13_cover_pitch(Cpu *c) {
	if (!u13_cover_player(c)) return;
	u13_frame_input();
	if (u13_pitch_done) return;
	u13_pitch_done = 1;
	if (u13_dy == 0) return;
	float off; uint32_t w = RD32(u13_player + 0x77c); memcpy(&off, &w, 4);
	off -= u13_dy * window_mouse_sens() * 0.0015f * (window_mouse_invert() ? -1.0f : 1.0f);
	memcpy(&w, &off, 4); WR32(u13_player + 0x77c, w);
	if (getenv("RECOMP_YAW_DIAG")) { static int n; if (n++ % 20 == 0) fprintf(stderr, "u13_cover_pitch: dy=%g -> %.3f\n", u13_dy, off); }
}
void recomp_hook_u13_cover_yaw(Cpu *c) {
	if (!u13_cover_player(c)) return;
	u13_frame_input();
	if (u13_yaw_done) return;
	u13_yaw_done = 1;
	if (u13_dx == 0) return;
	const float d = -u13_dx * window_mouse_sens() * 0.0015f, two_pi = 6.2831853f;
	const uint32_t at[2] = {c->r[0] + 0x1b4, u13_player + 0x780};
	for (int i = 0; i < 2; i++) {
		float v; uint32_t w = RD32(at[i]); memcpy(&v, &w, 4);
		v += d; while (v >= two_pi) v -= two_pi; while (v < 0) v += two_pi;
		memcpy(&w, &v, 4); WR32(at[i], w);
	}
	if (getenv("RECOMP_YAW_DIAG")) { static int n; if (n++ % 20 == 0) fprintf(stderr, "u13_cover_yaw: dx=%g\n", u13_dx); }
}

/* Online co-op diagnostics (the pack's U13_NET_DIAG): the game's peer connection asks np_matching2's signaling for the
 * peer's status twice (FUN_810cc3xx). It keeps the address and port from the first ([r4+0x40], [r4+0x44]) and fails with
 * state 0xc, "A protocol mismatch occurred" on screen, when the second answer differs. These log both answers */
void recomp_ihook_u13_sig_first(Cpu *c, int reg) {
	(void)reg; const uint32_t r4 = c->r[4];
	fprintf(stderr, "    u13: signaling status (kept): %#x, peer %08x port %u\n", c->r[0], RD32(r4 + 0x40), RD32(r4 + 0x44) & 0xffff);
}
void recomp_ihook_u13_sig_check(Cpu *c, int reg) {
	(void)reg; const uint32_t sp = c->r[13], r4 = c->r[4];
	fprintf(stderr, "    u13: signaling status (checked): %#x, state %u, peer %08x port %u; kept %08x port %u\n", c->r[0], RD32(sp + 0x10),
	        RD32(sp + 0x14), RD32(sp + 0x1c) & 0xffff, RD32(r4 + 0x40), RD32(r4 + 0x44) & 0xffff);
}
/* the same connection's state machine (FUN_810cc398, state at [r0], 0..12; 12 = failed): each change logged */
void recomp_hook_u13_conn_state(Cpu *c) {
	static uint32_t last_obj, last_state = 0xffffffffu;
	const uint32_t obj = c->r[0], st = RD32(obj);
	if (obj == last_obj && st == last_state) return;
	last_obj = obj; last_state = st;
	fprintf(stderr, "    u13: connection %#x state %u (elapsed %u ms, limit %u)\n", obj, st, RD32(obj + 0xbc), RD32(obj + 4) ? RD32(RD32(obj + 4) + 0x48) : 0);
}
/* what np_matching2 tells the game: its room-event callback (FUN_810bf1c4: ctx, room, event, event key, error, data size)
 * and its signaling callback (FUN_810bf298: ctx, room, peer member, event, error), at their entries (stack arguments at sp) */
void recomp_hook_u13_room_event(Cpu *c) {
	const uint32_t sp = c->r[13];
	fprintf(stderr, "    u13: room event %#x (key %#x, error %#x, %u bytes) room %#x%08x\n", RD32(sp) & 0xffff, RD32(sp + 4), RD32(sp + 8), RD32(sp + 12), c->r[3], c->r[2]);
}
void recomp_hook_u13_signaling_event(Cpu *c) {
	const uint32_t sp = c->r[13];
	fprintf(stderr, "    u13: signaling event %#x for member %#x (error %#x) room %#x%08x\n", RD32(sp + 4) & 0xffff, RD32(sp) & 0xffff, RD32(sp + 8), c->r[3], c->r[2]);
}
