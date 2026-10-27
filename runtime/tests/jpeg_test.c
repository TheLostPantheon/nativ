/*
 * runtime/media/jpeg.c (SceJpeg / SceJpegEnc) round trip, the way Uncharted's photo minigame drives it
 * (BSGfx::cJpegEncoderDecoder): a 960x544 BGRA picture through sceJpegEncoderCsc (4:2:0) and
 * sceJpegEncoderEncode, then back through sceJpegDecodeMJpegYCbCr at a quarter scale (mode 0x23: the
 * 240x136 thumbnail the game shows as a YUV420P3 texture) and sceJpegDecodeMJpeg (RGBA, full size and
 * a quarter). Checks the returned pitches and the colours of four flat quadrants after the trip.
 *
 *     cc -O1 -g -Wall -Wno-unused-function -I ../.. -I .. jpeg_test.c -o /tmp/jpeg_test && /tmp/jpeg_test
 */
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include "../media/jpeg.c"

uint8_t *recomp_mem;
void gfx_guest_writing(uint32_t addr, uint32_t len) { (void)addr; (void)len; }
void gfx_guest_written(uint32_t addr, uint32_t len) { (void)addr; (void)len; }

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); fails += !ok; }
static uint32_t call(void (*fn)(Cpu *), uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t s0, uint32_t s1, uint32_t s2, uint32_t s3, uint32_t s4) {
	Cpu c = {0}; c.r[0] = a0; c.r[1] = a1; c.r[2] = a2; c.r[3] = a3; c.r[13] = 0x100000;
	WR32(0x100000, s0); WR32(0x100004, s1); WR32(0x100008, s2); WR32(0x10000c, s3); WR32(0x100010, s4);
	fn(&c); return c.r[0];
}
/* the four quadrant colours (R, G, B) */
static const uint8_t quad[4][3] = {{200, 40, 40}, {40, 180, 60}, {50, 70, 220}, {230, 230, 230}};
static int near(int a, int b, int tol) { return a - b <= tol && b - a <= tol; }

int main(void) {
	recomp_mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	if (recomp_mem == MAP_FAILED) { perror("mmap"); return 1; }
	const uint32_t ctx = 0x200000, src = 0x300000, ycc = 0x800000, jpg = 0xa00000, rgba = 0xc00000, param = 0x110000;
	const int W = 960, H = 544;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			const uint8_t *q = quad[(y >= H / 2) * 2 + (x >= W / 2)];
			uint8_t *p = G(src + 4u * (uint32_t)(y * W + x)); p[0] = q[2]; p[1] = q[1]; p[2] = q[0]; p[3] = 255;   /* BGRA */
		}
	/* the game's setup: 960x544, pixel format 0x18 (YCbCr 4:2:0 | CSC), out 0x7f800 bytes */
	uint32_t csz = call(h_enc_context_size, 0, 0, 0, 0, 0, 0, 0, 0, 0);
	check(csz >= 32, "context size");
	WR32(param, 0x1c); WR32(param + 4, W); WR32(param + 8, H); WR32(param + 12, 0x18); WR32(param + 16, jpg); WR32(param + 20, 0x7f800); WR32(param + 24, 1);
	check(call(h_enc_init_param, ctx, param, 0, 0, 0, 0, 0, 0, 0) == 0, "InitWithParam");
	check(call(h_enc_valid_region, ctx, W, H, 0, 0, 0, 0, 0, 0) == 0, "SetValidRegion");
	check(call(h_enc_ratio, ctx, 0x40, 0, 0, 0, 0, 0, 0, 0) == 0, "SetCompressionRatio");
	check(call(h_enc_csc, ctx, ycc, src, W, 4, 0, 0, 0, 0) == 0, "Csc BGRA -> YCbCr");
	int32_t n = (int32_t)call(h_enc_encode, ctx, ycc, 0, 0, 0, 0, 0, 0, 0);
	printf("     encoded %d bytes\n", n);
	check(n > 100 && n < 0x7f800 && RD8(jpg) == 0xFF && RD8(jpg + 1) == 0xD8, "Encode -> a JPEG");
	if (n <= 0) return 1;

	/* the thumbnail: YCbCr at a quarter scale, any-sampling mode */
	uint32_t r = call(h_jpeg_decode_ycbcr, jpg, (uint32_t)n, ycc, 0xbf400, 0x23, 0, 0, 0, 0);
	printf("     YCbCr 1/4 returned %#x\n", r);
	check(r == (240u << 16 | 136u), "YCbCr 1/4: pitch 240 x 136");
	for (int k = 0; k < 4; k++) {   /* each quadrant's centre, converted back with the texture's CSC1 maths */
		int x = (k & 1) ? 180 : 60, y = (k & 2) ? 102 : 34;
		float Y = RD8(ycc + (uint32_t)(y * 240 + x)), cb = RD8(ycc + 240 * 136 + (uint32_t)((y / 2) * 120 + x / 2)) - 128.f,
		      cr = RD8(ycc + 240 * 136 + 120 * 68 + (uint32_t)((y / 2) * 120 + x / 2)) - 128.f;
		int R = (int)(Y + 1.402f * cr), Gc = (int)(Y - 0.344136f * cb - 0.714136f * cr), B = (int)(Y + 1.772f * cb);
		char what[96]; snprintf(what, sizeof what, "YCbCr quadrant %d: %d %d %d (want %d %d %d)", k, R, Gc, B, quad[k][0], quad[k][1], quad[k][2]);
		check(near(R, quad[k][0], 12) && near(Gc, quad[k][1], 12) && near(B, quad[k][2], 12), what);
	}
	/* RGBA, full size (mode 1) and a quarter (0x20) */
	r = call(h_jpeg_decode_rgba, jpg, (uint32_t)n, rgba, 0x1fe000, 1, ycc, 0xbf400, 0, 0);
	check(r == (960u << 16 | 544u), "RGBA full: pitch 960 x 544");
	r = call(h_jpeg_decode_rgba, jpg, (uint32_t)n, rgba, 0x1fe000, 0x20, ycc, 0xbf400, 0, 0);
	check(r == (240u << 16 | 136u), "RGBA 1/4: pitch 240 x 136");
	for (int k = 0; k < 4; k++) {
		int x = (k & 1) ? 180 : 60, y = (k & 2) ? 102 : 34;
		const uint8_t *p = G(rgba + 4u * (uint32_t)(y * 240 + x));
		char what[96]; snprintf(what, sizeof what, "RGBA quadrant %d: %d %d %d", k, p[0], p[1], p[2]);
		check(near(p[0], quad[k][0], 12) && near(p[1], quad[k][1], 12) && near(p[2], quad[k][2], 12), what);
	}
	check(call(h_jpeg_decode_ycbcr, jpg, (uint32_t)n, ycc, 100, 0x23, 0, 0, 0, 0) >= 0x80000000u, "a too-small output buffer fails");
	printf("%s\n", fails ? "FAILED" : "all passed");
	return fails != 0;
}
