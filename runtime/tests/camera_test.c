/*
 * runtime/media/camera.c (SceCamera) the way Uncharted's parchment puzzles drive it: open the rear camera with
 * buffers given at open (YUV420 planes, 640x480), start, read frames with the lamp off and on (F7),
 * and check the planes are written, dark then bright, with the frame counter advancing; then the
 * per-read buffers and a 32-bit ABGR format.
 *
 *     cc -O1 -g -Wall -Wno-unused-function -I ../.. -I .. $(pkg-config --cflags --libs sdl3) camera_test.c -o /tmp/camera_test && /tmp/camera_test
 */
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include "../media/camera.c"

uint8_t *recomp_mem;
void gfx_guest_writing(uint32_t addr, uint32_t len) { (void)addr; (void)len; }
void gfx_guest_written(uint32_t addr, uint32_t len) { (void)addr; (void)len; }
uint64_t guest_time_us(void) { static uint64_t t; return t += 33333; }
int settings_int(const char *k, int d) { (void)k; return d; }
const char *settings_str(const char *k, const char *d, char *b, int n) { (void)k; snprintf(b, (size_t)n, "%s", d); return b; }
void overlay_toast(const char *msg, double s) { printf("     toast (%.0f s): %s\n", s, msg); }
void overlay_toast_titled(const char *title, const char *body, double s) { printf("     toast (%.0f s): %s: %s\n", s, title, body); }

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); fails += !ok; }
static uint32_t call(void (*fn)(Cpu *), uint32_t a0, uint32_t a1) { Cpu c = {0}; c.r[0] = a0; c.r[1] = a1; c.r[13] = 0x100000; fn(&c); return c.r[0]; }
static double mean(uint32_t a, uint32_t n) { double s = 0; for (uint32_t i = 0; i < n; i++) s += RD8(a + i); return s / n; }

int main(void) {
	recomp_mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	if (recomp_mem == MAP_FAILED) { perror("mmap"); return 1; }
	const uint32_t info = 0x200000, rd = 0x200100, Y = 0x300000, U = 0x400000, V = 0x500000;
	const uint32_t W = 640, H = 480;
	WR32(info + CI_SIZE, 0x30); WR16(info + CI_FORMAT, FMT_YUV420_PLANE); WR16(info + CI_RESOLUTION, 1); WR16(info + CI_FRAMERATE, 30);
	WR32(info + CI_SIZE_I, W * H); WR32(info + CI_SIZE_I + 4, W * H / 4); WR32(info + CI_SIZE_I + 8, W * H / 4);
	WR32(info + CI_P_I, Y); WR32(info + CI_P_I + 4, U); WR32(info + CI_P_I + 8, V); WR16(info + CI_PITCH, 0); WR16(info + CI_BUFFER, BUF_SETBYOPEN);
	check(call(h_read, 1, rd) == CAM_ERR_PARAM, "read with a zero-size SceCameraRead fails");
	check(call(h_open, 1, info) == 0, "open the rear camera");
	check(RD16(info + CI_WIDTH) == W && RD16(info + CI_HEIGHT) == H, "open reports 640x480");
	check(call(h_open, 1, info) == CAM_ERR_ALREADY_OPEN, "a second open fails");
	WR32(rd + CR_SIZE, 0x40);
	check(call(h_read, 1, rd) == CAM_ERR_NOT_START, "read before start fails");
	check(call(h_start, 1, 0) == 0, "start");
	check(call(h_read, 1, rd) == 0, "read (lamp off)");
	double dark = mean(Y, W * H), du = mean(U, W * H / 4);
	printf("     lamp off: Y %.1f, U %.1f, frame %u\n", dark, du, RD32(rd + CR_FRAME));
	camera_light_toggle();
	check(call(h_read, 1, rd) == 0, "read (lamp on)");
	double bright = mean(Y, W * H);
	printf("     lamp on:  Y %.1f, frame %u, timestamp %u\n", bright, RD32(rd + CR_FRAME), RD32(rd + CR_TIMESTAMP));
	check(dark < 40 && bright > 180, "the lamp turns a dark frame bright");
	check(RD32(rd + CR_FRAME) == 1 && RD32(rd + CR_STATUS) == 0, "frame index 1, status active");
	check(du > 110 && du < 146, "chroma near neutral");
	check(call(h_stop, 1, 0) == 0 && call(h_close, 1, 0) == 0, "stop and close");

	/* buffers per read, 32-bit ABGR at 320x240 */
	WR16(info + CI_FORMAT, FMT_ABGR); WR16(info + CI_RESOLUTION, 2); WR16(info + CI_BUFFER, BUF_SETBYREAD); WR32(info + CI_P_I, 0);
	check(call(h_open, 1, info) == 0 && call(h_start, 1, 0) == 0, "reopen as ABGR 320x240, buffers per read");
	WR32(rd + CR_SIZE_I, 320 * 240 * 4); WR32(rd + CR_P_I, 0x600000);
	check(call(h_read, 1, rd) == 0, "read into the read's own buffer");
	uint32_t px = RD32(0x600000 + 4u * (120 * 320 + 160));
	printf("     centre pixel %08x\n", px);
	check((px >> 24) == 0xFF && (px & 0xFF) > 200, "an opaque bright centre (R in the low byte)");
	WR32(rd + CR_P_I, 0);
	check(call(h_read, 1, rd) == CAM_ERR_PARAM, "a per-read camera with no buffer fails");
	printf("%s\n", fails ? "FAILED" : "all passed");
	return fails != 0;
}
