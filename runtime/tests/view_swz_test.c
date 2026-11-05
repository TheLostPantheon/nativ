/* view_swz_test.c -- a GPU view of a U8U8U8U8 render target read by a U8U8U8U8 texture, for every
 * target swizzle (ABGR, ARGB, RGBA, BGRA) and every four-component texture swizzle: the view's
 * component selectors (runtime/gfx/view_swz.h) applied to the target's colour must give what the
 * texture decodes from the bytes the target stores in memory. The oracle below restates
 * gfx_soft.c's target_write_of (the byte layout per target swizzle) and apply_swizzle's sw4 table
 * (the texture decode) independently. Before view_swz.h only ABGR targets had a view: every other
 * swizzle took the guest-memory path, a synchronous target download per read (-DVIEW_SWZ_UPSTREAM_RULE
 * builds that rule instead: the control). Pure logic:
 *   cc -O1 -g -Wall -I../gfx view_swz_test.c -o /tmp/view_swz_test && /tmp/view_swz_test */
#ifdef VIEW_SWZ_UPSTREAM_RULE
/* the rule before view_swz.h (gfx_gpu.c view_mode, U8U8U8U8 case): a view only for an ABGR target, selectors untouched */
#include <stdint.h>
static int view_swz_u8x4(uint32_t tgt_fmt, uint32_t swz[4]) { (void)swz; return (tgt_fmt & 0xF1800000u) == 0 && (tgt_fmt & 0x00300000u) == 0; }
#else
#include "view_swz.h"
#endif
#include <stdio.h>
#include <string.h>

static int checks, failed;
#define CK(c) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* gfx_soft.c target_write_of, U8U8U8U8: the word stored for colour c under target swizzle s */
static uint32_t store(int s, const uint8_t c[4]) {
	uint32_t r = c[0], g = c[1], b = c[2], a = c[3];
	switch (s) {
	case 0: return r | g << 8 | b << 16 | a << 24;
	case 1: return b | g << 8 | r << 16 | a << 24;
	case 2: return a | b << 8 | g << 16 | r << 24;
	default: return a | r << 8 | g << 16 | b << 24;
	}
}
/* gfx_soft.c apply_swizzle for four-component textures: selector k picks byte sw4[sw][k] (4 = 0, 5 = 1) */
static const uint8_t sw4[8][4] = { {0,1,2,3}, {2,1,0,3}, {3,2,1,0}, {1,2,3,0}, {0,1,2,5}, {2,1,0,5}, {3,2,1,5}, {1,2,3,5} };
static uint8_t pick(const uint8_t v[4], uint32_t sel) { return sel < 4 ? v[sel] : sel == 4 ? 0 : 255; }

int main(void)
{
	static const uint8_t colours[3][4] = { {0x11, 0x22, 0x33, 0x44}, {0xff, 0x00, 0x80, 0x01}, {0x0a, 0xb0, 0xc0, 0xd0} };
	for (int s = 0; s < 4; s++) for (int sw = 0; sw < 8; sw++) {
		uint32_t swz[4]; for (int k = 0; k < 4; k++) swz[k] = sw4[sw][k];
		CK(view_swz_u8x4(0x00000000u | (uint32_t)s << 20, swz) == 1);
		for (int c = 0; c < 3; c++) {
			uint32_t w = store(s, colours[c]);
			uint8_t bytes[4] = { (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)(w >> 24) };
			for (int k = 0; k < 4; k++) {
				uint8_t want = pick(bytes, sw4[sw][k]);      /* what the texture decodes from memory */
				uint8_t got = pick(colours[c], swz[k]);      /* what the view samples from the target */
				if (want != got) printf("  target swizzle %d texture swizzle %d colour %d comp %d: want %#x got %#x\n", s, sw, c, k, want, got);
				CK(want == got);
			}
		}
	}
	/* an A8R8G8B8 target read as U8U8U8U8_ARGB tiles (a texture atlas drawn by the GPU): the identity */
	{ uint32_t swz[4] = { 2, 1, 0, 3 }; CK(view_swz_u8x4(0x00100000u, swz) == 1); CK(swz[0] == 0 && swz[1] == 1 && swz[2] == 2 && swz[3] == 3); }
	/* not a U8U8U8U8 target: refused, selectors untouched */
	{ uint32_t swz[4] = { 0, 1, 2, 3 }; CK(view_swz_u8x4(0x80800000u, swz) == 0); CK(swz[0] == 0 && swz[3] == 3); }
	printf("%d checks, %d failed\n", checks, failed);
	return failed ? 1 : 0;
}
