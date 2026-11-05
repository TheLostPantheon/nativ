/* codec_engine_test.c -- SceCodecEngine's unmap-memory ranges (runtime/media/video.c). A movie player
 * sizes the block it opens for the ranges it will take; on the console the block is the codec engine's
 * (mapped in its own address space), so a large alignment does not cost the caller block space. The
 * reference sequence, from a movie player's start-up: a 0x300000-byte block at a 16 KiB-aligned address,
 * then 0x80000 bytes aligned 0x40000 and 0x240000 bytes aligned 0x100000 -- 0x2c0000 bytes, inside the
 * budget. Aligning the second range by CPU address pushed it past the block's end, the allocation
 * returned 0 and the player never created its video decoder (a black movie).
 *
 * Checked: both ranges are nonzero, inside the block and disjoint; an aligned range that fits keeps its
 * alignment; a range larger than what is left still fails (0). VIDEO_UNDER_TEST compiles a different copy
 * of video.c (a fail-before run). Build and run: see README.md here.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef VIDEO_UNDER_TEST
#define VIDEO_UNDER_TEST "../media/video.c"
#endif
#include VIDEO_UNDER_TEST

uint8_t *recomp_mem;
uint32_t recomp_watch_lo = 0, recomp_watch_hi = 0;
void recomp_watch_write(uint32_t a, uint32_t v, int width) { (void)a; (void)v; (void)width; }

static int fails;
static void check(const char *what, int ok) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
static uint32_t call(HleFn f, uint32_t a0, uint32_t a1, uint32_t a2) {
	Cpu cpu; memset(&cpu, 0, sizeof cpu); cpu.r[0] = a0; cpu.r[1] = a1; cpu.r[2] = a2; f(&cpu); return cpu.r[0];
}

int main(void) {
	const uint32_t BASE = 0x10ffc000, SIZE = 0x300000, END = BASE + SIZE;
	uint32_t id = call(h_ce_open, BASE, SIZE, 0);
	check("OpenUnmapMemBlock -> a block id", id >= 1 && id <= 8);
	uint32_t a = call(h_ce_alloc, id, 0x80000, 0x40000);
	check("0x80000 aligned 0x40000 -> inside the block, aligned", a >= BASE && a + 0x80000 <= END && !(a & 0x3ffff));
	uint32_t b = call(h_ce_alloc, id, 0x240000, 0x100000);
	check("0x240000 aligned 0x100000 -> nonzero (the caller's budget fits)", b != 0);
	check("  inside the block", b >= BASE && b + 0x240000 <= END);
	check("  disjoint from the first range", b >= a + 0x80000 || b + 0x240000 <= a);
	check("0x100000 more -> 0 (the budget is spent)", call(h_ce_alloc, id, 0x100000, 0x10) == 0);
	check("CloseUnmapMemBlock -> 0", call(h_ce_close, id, 0, 0) == 0);

	/* a block where the alignment fits keeps it */
	id = call(h_ce_open, 0x20000000, 0x400000, 0);
	a = call(h_ce_alloc, id, 0x1000, 0x10);
	b = call(h_ce_alloc, id, 0x100000, 0x100000);
	check("an alignment that fits is kept", b == 0x20100000);
	call(h_ce_close, id, 0, 0);

	if (fails) { printf("%d checks FAILED\n", fails); return 1; }
	printf("all passed\n");
	return 0;
}
