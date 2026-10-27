/* RECOMP_CAPTURE: one frame's graphics stream plus the guest memory it reads (gfx_capture.c) */
#ifndef RECOMP_GFX_CAPTURE_H
#define RECOMP_GFX_CAPTURE_H
#include "gfx.h"

void gfxcap_scene(const GfxTarget *t);   /* gfx_dispatch.c, before the backend sees each call */
void gfxcap_draw(const GfxDraw *d);
void gfxcap_end_scene(void);
void gfxcap_request(void);               /* window.c: F12 -- capture the next frames */
void gfxcap_present(uint32_t base, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format);
#endif
