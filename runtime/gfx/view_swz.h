/* view_swz.h -- the component selectors of a GPU view that reads a U8U8U8U8 render target the way a
 * U8U8U8U8 texture over the same memory would (gfx_gpu.c view_mode, mode 0). The target's GPU texture
 * holds its colour as rendered (r, g, b, a); guest memory holds the bytes gfx_soft.c's target_write_of
 * stores for the target's swizzle (format bits 0x00300000: ABGR, ARGB, RGBA, BGRA); the texture's
 * selectors (apply_swizzle) index those bytes. The view's selectors are the two composed: byte i of the
 * stored word is colour component enc[s][i]. Only ABGR (enc = identity) had a view before; every other
 * swizzle took the guest-memory path, a synchronous target download per read.
 * Pure, header-only: runtime/tests/view_swz_test.c checks it against both rules restated. */
#ifndef VIEW_SWZ_H
#define VIEW_SWZ_H
#include <stdint.h>

/* tgt_fmt: the target's colour format; swz: the texture's selectors (0-3 a byte, 4 zero, 5 one), rewritten
 * in place to index the target's colour. 1 = a view can serve it, 0 = not a U8U8U8U8 target (swz untouched) */
static inline int view_swz_u8x4(uint32_t tgt_fmt, uint32_t swz[4]) {
	static const uint8_t enc[4][4] = { {0,1,2,3}, {2,1,0,3}, {3,2,1,0}, {3,0,1,2} };   /* ABGR, ARGB, RGBA, BGRA: byte i = colour enc[i] */
	if ((tgt_fmt & 0xF1800000u) != 0) return 0;
	const uint8_t *e = enc[(tgt_fmt >> 20) & 3];
	for (int k = 0; k < 4; k++) if (swz[k] < 4) swz[k] = e[swz[k]];
	return 1;
}
#endif
