/* RECOMP_GFX_COVERAGE: what a game's graphics ask for vs. what the backends honour (gfx_coverage.c) */
#ifndef RECOMP_GFX_COVERAGE_H
#define RECOMP_GFX_COVERAGE_H
#include "rt.h"
#include "hle.h"
#include "gfx.h"

int gfxcov_on(void);
void gfxcov_api(const char *name, HleFn fn, Cpu *c, int module);   /* main.c: every import call (filters to GXM/display); module: a loaded module's export handles it (fn = the boundary observer, if any) */
void gfxcov_scene(const GfxTarget *t);                  /* gfx_dispatch.c */
void gfxcov_draw(const GfxDraw *d);
void gfxcov_present(uint32_t format);
void gfxcov_write(void);
/* gxm_sony.c: libgxm state the decode does not hand to the backends (or hands over approximated),
 * seen set to something other than its default -- a gap no backend can report, never seeing it.
 * partial: 1 approximated, 0 ignored */
void gfxcov_gap(const char *key, const char *value, int partial, const char *note);
#endif
