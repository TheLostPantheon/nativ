/*
 * Hooks every game can use, and what the games' own hooks share (hooks.h). recomp.py's --hooks (a pack's HOOKS= list)
 * emits a call to recomp_hook_<name>(c) at the entry of one named guest function, before its body runs, so the hook sees
 * exactly the registers and VFP state the game set up for that call and can change them; --insn-hooks (IHOOKS=) calls
 * recomp_ihook_<name>(c, reg) right after one instruction. A game's own hooks are its pack's (packs/<title>/hooks.c,
 * built into that game alone: runtime/build.py RECOMP_PACK_HOOKS); here only what isn't one game's: the guest addresses
 * per build of a game (game_addr), the fov and aspect math, the 1/30 s and 30-fps-frame fixes any pack's IHOOKS name,
 * the callbacks a game's diagnostics plug into (hook_seq_arm, recomp_on_bad_call, hook_stall_long) and the hooks in
 * Sony's libgxm that vitarecomp adds to every game.
 */
#include "hooks.h"

/* the guest addresses a game's hook code uses that differ between builds of the game (Killzone 1.00 and its 1.12 update): the
 * pack's GAME_ADDRS = "name=0x... ...", built in (vitarecomp build -> RECOMP_GAME_ADDRS). 0 when this build has none -- the hook
 * that needs it then does nothing, and says so once, rather than reading or calling at another build's address */
uint32_t game_addr(const char *name) {
#ifdef RECOMP_GAME_ADDRS
	static const char list[] = RECOMP_GAME_ADDRS;
	size_t n = strlen(name);
	for (const char *p = list; (p = strstr(p, name)); p += n)
		if ((p == list || p[-1] == ' ') && p[n] == '=') return (uint32_t)strtoul(p + n + 1, 0, 16);
#endif
	static const char *said[32]; static int ns; int seen = 0;
	for (int i = 0; i < ns; i++) if (said[i] == name) seen = 1;
	if (!seen && ns < 32) { said[ns++] = name; fprintf(stderr, "    hooks: no address for %s in this build of the game (GAME_ADDRS): its hook is off\n", name); }
	return 0;
}

/* shared by every per-game fov hook below: the "fov" setting (RECOMP_FOV over settings.ini, as
 * RECOMP_RENDER_SCALE reads "render_scale") turned into a new vertical fov. d0' = 2*atan(k*tan(d0/2))
 * is the exact relation between two cameras with the same near-plane view width/tan(fov/2) ratio
 * (the standard "change the vertical fov, keep the same framing at the near plane" formula), and
 * is what a multiplier should mean: 1.35 widens the view the way a 35% wider frame would. Returns
 * the game's own fov unchanged (0, meaning "do nothing") for the default multiplier, a disabled/
 * bad one, or a fov value this math doesn't apply to -- so a hook only ever touches the register
 * when it is actually changing something. */
/* the factor a game's horizontal view is widened by for a wider aspect ratio (window.c: the "aspect_ratio"
 * setting, 1 = native) -- a camera hook scales its projection's horizontal extent (the aspect it passes,
 * or tan of its horizontal half-angle) by this, vertical untouched (Hor+) */
float window_aspect_widen(void);
double aspect_widen(void) { return (double)window_aspect_widen(); }
double fov_scale(double fov) {
	const char *e = getenv("RECOMP_FOV");
	double k = e && *e ? atof(e) : settings_float("fov", 1.0);
	if (k == 1.0 || k <= 0.0 || !(fov > 0.0 && fov < M_PI)) return 0.0;
	double scaled = 2.0 * atan(k * tan(fov * 0.5));
	const double limit = 150.0 * M_PI / 180.0;   /* clamp: the builder's cotangent terms degrade near 180 deg */
	if (scaled > limit) scaled = limit;
	else if (scaled < 0.01) scaled = 0.01;   /* guard a degenerate multiplier instead of handing the builder ~0 */
	return scaled;
}

/*
 * Unit 13 at 60 fps (the "frame_rate" setting, display.c): the game times most things by its measured
 * frame time, but some code builds the constant 1/30 s (movw/movt 0x3d088889) and uses it as "one
 * frame" -- particles' minimum step max(elapsed, 1/30), velocity x 1/30, a timer += 1/30 a frame, a
 * frame-time getter -- each run once per presented frame (counted with the verify build's
 * RECOMP_WATCH_COUNT at 30 and at 60), so at 60 fps they ran at double speed. packs/PCSA00014/pack.toml's
 * IHOOKS lists those sites; this replaces the constant, right after the movt that builds it, with the
 * time the frame on screen actually took (1/60..1/30 s) -- only while the 60 fps cap is on, so 30 fps
 * play and the gate run the game's own code untouched. Its fixed-step loops (fn_818ade80, fn_818af818:
 * step 1/30 until caught up with real time) are frame-rate independent already and stay as they are.
 */
void recomp_ihook_frame_dt(Cpu *c, int reg) {   /* any game's: packs/<title>/pack.toml IHOOKS="addr:frame_dt" on the movt */
	if (reg < 0 || reg > 14 || c->r[reg] != 0x3d088889u || !display_frame_rate_60()) return;
	float dt = display_frame_dt(); memcpy(&c->r[reg], &dt, 4);
	if (getenv("RECOMP_FRAME_DT_TRACE")) { static unsigned n; if (n++ % 100 == 0) fprintf(stderr, "frame dt: 1/30 -> %.5f at %#x (%u so far)\n", dt, c->r[14], n); }
}
/* any game's: IHOOKS="addr:frames30" on the instruction that sets a count of frames written for 30 fps
 * (a wait, a window): doubled at 60 fps, so it lasts as long as on the console */
void recomp_ihook_frames30(Cpu *c, int reg) {
	if (reg < 0 || reg > 14 || !display_frame_rate_60()) return;
	c->r[reg] *= 2;
}

/* a register holding the native aspect, 960/544 (0x3fe1e1e2, built by a movw/movt): widened by aspect_widen(), for a
 * pack's IHOOKS right after the movt (Uncharted's ten sites, Unit 13's camera update, Killzone's aspect getter) */
void widen_aspect_reg(Cpu *c, int reg) {   /* a register holding the 960/544 constant: widened by aspect_widen() */
	if (reg < 0 || reg > 14 || c->r[reg] != 0x3fe1e1e2u) return;
	double k = aspect_widen();
	if (k <= 1.0) return;
	float a = (float)(960.0 / 544.0 * k); memcpy(&c->r[reg], &a, 4);
}

/* the callbacks a game's diagnostics plug into (a pack's hooks.c sets them from a constructor): a call-sequence capture
 * to arm (verify.c), a call to address 0 (main.c), a stall of the presented frames (stallprof.c) */
void (*hook_seq_arm)(const char *);
void (*recomp_on_bad_call)(Cpu *, uint32_t);
void (*hook_stall_long)(uint64_t stall_us);

/* ---- Sony's shader patcher (libgxm, a recompiled module: MOD_HOOKS). RECOMP_GXM_PATCHER_LOG=1: each fragment / vertex
 * program variant the game creates, with the frame -- whether a game creates them at load time (the shader prebuild's
 * premise) or at first draw. sceGxmShaderPatcherCreateFragmentProgram(patcher, id, output format, multisample, blend,
 * vertex program, out); sceGxmShaderPatcherCreateVertexProgram(patcher, id, attributes, n, streams, n, out). */
static int gxm_patcher_log(void) { static int on = -1; if (on < 0) on = getenv("RECOMP_GXM_PATCHER_LOG") != 0; return on; }
void recomp_hook_gxm_fp_create(Cpu *c) {
	if (!gxm_patcher_log()) return;
	static unsigned n; n++;
	fprintf(stderr, "    gxm patcher: frame %llu fragment program #%u: id %#x format %u msaa %u blend %#x vertex %#x\n", (unsigned long long)display_frames(), n, c->r[1], c->r[2], c->r[3], RD32(c->r[13]), RD32(c->r[13] + 4));
}
void recomp_hook_gxm_vp_create(Cpu *c) {
	if (!gxm_patcher_log()) return;
	static unsigned n; n++;
	fprintf(stderr, "    gxm patcher: frame %llu vertex program #%u: id %#x attributes %u streams %u\n", (unsigned long long)display_frames(), n, c->r[1], c->r[3], RD32(c->r[13] + 4));
}
/* 0x89017034, CreateFragmentProgram's common tail just after *out = r4 (the variant; +0x70 its reference count, 1 when made
 * now; +0 the GXP a draw uses: the registered program itself, not a copy): RECOMP_GXM_PATCHER_LOG's creation frame per variant */
void recomp_ihook_gxm_fp_made(Cpu *c, int reg) {
	(void)reg; if (!gxm_patcher_log()) return;
	const uint32_t fp = c->r[4];
	if (RD32(fp + 0x70) == 1) fprintf(stderr, "    gxm patcher: new fragment gxp %#x created frame %llu\n", RD32(fp), (unsigned long long)display_frames());
}
/* sceGxmShaderPatcherRegisterProgram(patcher, GXP, out id): the program to gfx_gpu.c's shader recipes (a game draws with the
 * programs it registers, bytes unchanged: their recipes can be built before the first draw), and RECOMP_GXM_PATCHER_LOG's
 * registration frames. vitarecomp hooks it in every game's libgxm. */
void recomp_hook_gxm_register(Cpu *c) {
	extern void gfx_gpu_program_registered(uint32_t gxp);
	gfx_gpu_program_registered(c->r[1]);
	if (!gxm_patcher_log()) return;
	const uint32_t g = c->r[1];
	fprintf(stderr, "    gxm patcher: register gxp %#x type %u frame %llu\n", g, g ? (RD8(g + 0x8) & 0xf) : 0, (unsigned long long)display_frames());
}
