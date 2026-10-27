/* What a game's own hooks (packs/<title>/hooks.c) share with the runtime: runtime/core/hooks.c, and the runtime calls they
 * reach for. */
#pragma once
#include "platform.h"
#include "rt.h"
#include "settings.h"
#include "usse.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t game_addr(const char *name);   /* a guest address this build of the game names in GAME_ADDRS; 0: none */
double aspect_widen(void);              /* the horizontal widening for the aspect ratio setting (1 = native) */
double fov_scale(double fov);           /* a vertical fov (radians) under the fov setting; 0: leave it */
void widen_aspect_reg(Cpu *c, int reg); /* a register holding 960/544, widened by aspect_widen() */
void recomp_ihook_frame_dt(Cpu *c, int reg);
uint64_t display_frames(void);
int display_frame_rate_60(void);
float display_frame_dt(void);
int window_mouse_delta(float *dx, float *dy); float window_mouse_sens(void); int window_mouse_invert(void); int window_raw_look(void);   /* the mouse look (window.c) */

/* the callbacks a game's diagnostics plug into (hooks.c) */
extern void (*hook_seq_arm)(const char *);
extern void (*recomp_on_bad_call)(Cpu *, uint32_t);
extern void (*hook_stall_long)(uint64_t stall_us);

static inline float rdf(uint32_t a) { float f; memcpy(&f, G(a), 4); return f; }   /* a guest float */
static inline void wrf(uint32_t a, float f) { memcpy(G(a), &f, 4); }
