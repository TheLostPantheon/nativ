/*
 * SceCamera on the host. Uncharted: Golden Abyss has parchment puzzles that ask the player to hold the
 * Vita's rear camera up to a real light so the paper shows what is written through it: the game opens
 * the camera and reads its frames. With none of SceCamera implemented every call took the generic
 * stub's 0 and no frame was ever written -- the camera "worked" and saw nothing, forever.
 *
 * Frames come from one of three sources (settings "camera", RECOMP_CAMERA wins):
 *   light   (default) a synthetic picture: a dim room, or a bright lamp while the light is on --
 *           F7 or the F10 menu's Controls page toggles it (camera_light_toggle below). The lamp starts
 *           as the game's conf says (RECOMP_CAMERA_LAMP=1 in RUN_ENV: on -- Uncharted's, so its
 *           puzzles just work), and the first sceCameraStart says on screen what the camera sees
 *   webcam  the computer's own camera through SDL3's camera API (hold it up to a lamp, as on the Vita);
 *           falls back to `light` if no camera opens
 *   dark    always the dim picture
 * Whatever the source, the frame is written in the format, resolution and buffers the game asked for
 * in sceCameraOpen (SceCameraInfo) -- semantics as Vita3K's SceCamera module, read for reference only:
 * buffers given at open (SETBYOPEN) or per read (SETBYREAD), YUV planes / packed YUV / 32-bit RGB,
 * a new frame every read (WAIT_NEXTFRAME_ON) with its index and timestamp.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "hle.h"
#include "gfx.h"
#include "settings.h"
#include "overlay.h"

#define CAM_ERR_PARAM 0x802E0000u
#define CAM_ERR_ALREADY_OPEN 0x802E0003u
#define CAM_ERR_NOT_OPEN 0x802E0004u
#define CAM_ERR_ALREADY_START 0x802E0005u
#define CAM_ERR_NOT_START 0x802E0006u
#define CAM_ERR_FORMAT_UNKNOWN 0x802E0007u
#define CAM_ERR_RESOLUTION_UNKNOWN 0x802E0008u
enum { FMT_YUV422_PLANE = 1, FMT_YUV422_PACKED = 2, FMT_YUV420_PLANE = 3, FMT_ARGB = 4, FMT_ABGR = 5, FMT_RAW8 = 6 };
enum { BUF_SETBYOPEN = 0, BUF_SETBYREAD = 1 };

/* SceCameraInfo (0x30 bytes) and SceCameraRead (0x40) field offsets */
#define CI_SIZE 0x00
#define CI_FORMAT 0x06
#define CI_RESOLUTION 0x08
#define CI_FRAMERATE 0x0a
#define CI_WIDTH 0x0c
#define CI_HEIGHT 0x0e
#define CI_RANGE 0x10
#define CI_SIZE_I 0x14
#define CI_P_I 0x20
#define CI_PITCH 0x2c
#define CI_BUFFER 0x2e
#define CR_SIZE 0x00
#define CR_STATUS 0x0c
#define CR_FRAME 0x10
#define CR_TIMESTAMP 0x18
#define CR_SIZE_I 0x20
#define CR_P_I 0x2c

typedef struct { int open, started; uint32_t format, width, height, pitch, buffer, range; uint32_t base[3], size[3]; uint64_t frame; } Cam;
static Cam cams[2];   /* SCE_CAMERA_DEVICE_FRONT 0, BACK 1 */
static int light_on = -1;   /* the `light` source's lamp; -1: not yet set, the game's default (RECOMP_CAMERA_LAMP) */

int camera_light(void) { if (light_on < 0) { const char *e = getenv("RECOMP_CAMERA_LAMP"); light_on = e && *e == '1'; } return light_on; }
void camera_light_set(int on) { light_on = on != 0; }
void camera_light_toggle(void) {
	light_on = !camera_light();
	fprintf(stderr, "camera: lamp %s (F7)\n", light_on ? "on" : "off");
	overlay_toast_titled("Camera", light_on ? "Lamp on: the Vita's camera sees a bright light [F7]" : "Lamp off: the Vita's camera sees a dark room [F7]", 3.0);
}

static int source(void) {   /* 0 light, 1 webcam, 2 dark */
	char b[16]; const char *e = getenv("RECOMP_CAMERA"); const char *s = e ? e : settings_str("camera", "light", b, sizeof b);
	return !strcmp(s, "webcam") ? 1 : !strcmp(s, "dark") ? 2 : 0;
}
static int res_dims(uint32_t res, uint32_t *w, uint32_t *h) {
	static const uint16_t d[9][2] = {{0, 0}, {640, 480}, {320, 240}, {160, 120}, {352, 288}, {176, 144}, {480, 272}, {0, 0}, {640, 360}};
	if (res >= 9 || !d[res][0]) return 0;
	*w = d[res][0]; *h = d[res][1]; return 1;
}

/* ---- the computer's camera (SDL3), opened on first use and kept open */
static SDL_Camera *webcam; static int webcam_tried;
static SDL_Surface *webcam_frame(void) {   /* the newest frame, or 0 (caller destroys it) */
	if (!webcam && !webcam_tried) {
		webcam_tried = 1;
		if (!SDL_WasInit(SDL_INIT_CAMERA) && !SDL_InitSubSystem(SDL_INIT_CAMERA)) { fprintf(stderr, "camera: no camera subsystem (%s): using the light\n", SDL_GetError()); return 0; }
		int n = 0; SDL_CameraID *ids = SDL_GetCameras(&n);
		if (ids && n > 0) webcam = SDL_OpenCamera(ids[0], 0);
		fprintf(stderr, "camera: %s\n", webcam ? "this computer's camera is the Vita's" : "no camera opened: using the light");
		SDL_free(ids);
	}
	if (!webcam) return 0;
	SDL_Surface *last = 0; Uint64 ts;
	for (int i = 0; i < 16; i++) {   /* drain to the newest */
		SDL_Surface *f = SDL_AcquireCameraFrame(webcam, &ts);
		if (!f) break;
		if (last) SDL_DestroySurface(last);
		last = SDL_DuplicateSurface(f); SDL_ReleaseCameraFrame(webcam, f);
	}
	static SDL_Surface *keep;   /* the camera delivers at its own rate: between frames, the previous one */
	if (last) { if (keep) SDL_DestroySurface(keep); keep = last; }
	return keep ? SDL_DuplicateSurface(keep) : 0;
}

/* this frame as w x h RGBA bytes (R, G, B, A) */
static void frame_rgba(uint8_t *rgba, uint32_t w, uint32_t h, uint64_t n) {
	int src = source();
	if (src == 1) {
		SDL_Surface *f = webcam_frame();
		if (f) {
			SDL_Surface *s = SDL_ScaleSurface(f, (int)w, (int)h, SDL_SCALEMODE_LINEAR);
			SDL_Surface *c = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888) : 0;   /* bytes R, G, B, A */
			if (c) for (uint32_t y = 0; y < h; y++) memcpy(rgba + 4u * w * y, (uint8_t *)c->pixels + (size_t)c->pitch * y, 4u * w);
			SDL_DestroySurface(c); SDL_DestroySurface(s); SDL_DestroySurface(f);
			if (c) return;
		}
	}
	/* the light: a lamp filling the view, brightest in the middle; off, a dim room. A little noise
	 * that changes every frame, as a sensor's (a game may look for a live picture) */
	int lit = src == 0 && camera_light();
	uint32_t seed = (uint32_t)n * 2654435761u + 1u;
	for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
		float dx = ((float)x + 0.5f) / (float)w - 0.5f, dy = ((float)y + 0.5f) / (float)h - 0.5f, r2 = dx * dx + dy * dy;
		seed = seed * 1664525u + 1013904223u;
		int v = lit ? 250 - (int)(r2 * 160.0f) : 22 + (int)(r2 * 10.0f);
		v += (int)(seed >> 29) - 4;
		v = v < 0 ? 0 : v > 255 ? 255 : v;
		uint8_t *p = rgba + 4u * (y * w + x);
		p[0] = (uint8_t)v; p[1] = (uint8_t)(lit ? v : v * 9 / 10); p[2] = (uint8_t)(lit ? v * 9 / 10 : v * 8 / 10); p[3] = 255;   /* a warm lamp, a slightly cool room */
	}
}

/* RGB -> Y Cb Cr: BT.601 video range when the game asked for it (range 1), else full range (JFIF) */
static void ycc(const uint8_t *p, int bt601, int *Y, int *U, int *V) {
	float r = p[0], g = p[1], b = p[2];
	float y = 0.299f * r + 0.587f * g + 0.114f * b, u = (b - y) * 0.564f + 128.0f, v = (r - y) * 0.713f + 128.0f;
	if (bt601) { y = 16.0f + y * 219.0f / 255.0f; u = 128.0f + (u - 128.0f) * 224.0f / 255.0f; v = 128.0f + (v - 128.0f) * 224.0f / 255.0f; }
	*Y = (int)(y + 0.5f); *U = (int)(u + 0.5f); *V = (int)(v + 0.5f);
}
static void write_frame(Cam *k, const uint32_t base[3], const uint32_t size[3]) {
	uint32_t w = k->width, h = k->height, pitch = k->pitch ? k->pitch : w;
	uint8_t *rgba = malloc(4u * w * h); if (!rgba) return;
	frame_rgba(rgba, w, h, k->frame);
	int bt = k->range == 1;
	for (int i = 0; i < 3; i++) if (base[i] && size[i]) gfx_guest_writing(base[i], size[i]);
	switch (k->format) {
	case FMT_YUV420_PLANE: case FMT_YUV422_PLANE: {
		uint32_t cw = (w + 1) / 2, ch = k->format == FMT_YUV420_PLANE ? (h + 1) / 2 : h, cp = (pitch + 1) / 2;
		for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
			int Y, U, V; ycc(rgba + 4u * (y * w + x), bt, &Y, &U, &V);
			if (y * pitch + x < size[0]) WR8(base[0] + y * pitch + x, (uint8_t)Y);
			if (!(x & 1) && (k->format == FMT_YUV422_PLANE || !(y & 1))) {
				uint32_t o = (k->format == FMT_YUV420_PLANE ? y / 2 : y) * cp + x / 2;
				if (x / 2 < cw && (k->format == FMT_YUV420_PLANE ? y / 2 : y) < ch) {
					if (base[1] && o < size[1]) WR8(base[1] + o, (uint8_t)U);
					if (base[2] && o < size[2]) WR8(base[2] + o, (uint8_t)V);
				}
			}
		}
		break;
	}
	case FMT_YUV422_PACKED:   /* Y0 U Y1 V */
		for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x + 1 < w; x += 2) {
			int Y0, U, V, Y1, u1, v1; ycc(rgba + 4u * (y * w + x), bt, &Y0, &U, &V); ycc(rgba + 4u * (y * w + x + 1), bt, &Y1, &u1, &v1);
			uint32_t o = y * pitch * 2 + x * 2;
			if (o + 3 < size[0]) { WR8(base[0] + o, (uint8_t)Y0); WR8(base[0] + o + 1, (uint8_t)((U + u1) / 2)); WR8(base[0] + o + 2, (uint8_t)Y1); WR8(base[0] + o + 3, (uint8_t)((V + v1) / 2)); }
		}
		break;
	case FMT_ARGB: case FMT_ABGR: default:   /* a 32-bit word per pixel: A<<24 | B<<16 | G<<8 | R (ABGR), A<<24 | R<<16 | G<<8 | B (ARGB) */
		for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
			const uint8_t *p = rgba + 4u * (y * w + x); uint32_t o = 4u * (y * pitch + x);
			if (o + 3 >= size[0]) continue;
			if (k->format == FMT_RAW8) { WR8(base[0] + y * pitch + x, p[1]); continue; }
			uint32_t v = k->format == FMT_ARGB ? (0xFFu << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) : (0xFFu << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]);
			WR32(base[0] + o, v);
		}
		break;
	}
	for (int i = 0; i < 3; i++) if (base[i] && size[i]) gfx_guest_written(base[i], size[i]);
	free(rgba);
}

static Cam *dev(Cpu *c) { return A0 < 2 ? &cams[A0] : 0; }
static void h_open(Cpu *c) {   /* (devnum, SceCameraInfo *) */
	Cam *k = dev(c); uint32_t ci = A1;
	if (!k || !ci) { RET(CAM_ERR_PARAM); return; }
	if (k->open) { RET(CAM_ERR_ALREADY_OPEN); return; }
	uint32_t fmt = RD16(ci + CI_FORMAT), w, h;
	if (fmt < 1 || fmt > 6) { RET(CAM_ERR_FORMAT_UNKNOWN); return; }
	if (!res_dims(RD16(ci + CI_RESOLUTION), &w, &h)) { RET(CAM_ERR_RESOLUTION_UNKNOWN); return; }
	memset(k, 0, sizeof *k);
	k->open = 1; k->format = fmt; k->width = w; k->height = h; k->pitch = RD16(ci + CI_PITCH); k->buffer = RD16(ci + CI_BUFFER); k->range = RD16(ci + CI_RANGE);
	for (int i = 0; i < 3; i++) { k->size[i] = RD32(ci + CI_SIZE_I + 4 * i); k->base[i] = RD32(ci + CI_P_I + 4 * i); }
	WR16(ci + CI_WIDTH, (uint16_t)w); WR16(ci + CI_HEIGHT, (uint16_t)h);
	fprintf(stderr, "camera %u: open, format %u, %ux%u (pitch %u) at %u fps, buffers %s, source %s\n", A0, fmt, w, h, k->pitch, RD16(ci + CI_FRAMERATE),
	        k->buffer == BUF_SETBYREAD ? "per read" : "from open", source() == 1 ? "webcam" : source() == 2 ? "dark" : "light (F7)");
	RET(0);
}
static void h_close(Cpu *c) { Cam *k = dev(c); if (!k) { RET(CAM_ERR_PARAM); return; } if (!k->open) { RET(CAM_ERR_NOT_OPEN); return; } k->open = k->started = 0; RET(0); }
static void h_start(Cpu *c) {
	Cam *k = dev(c); if (!k) { RET(CAM_ERR_PARAM); return; }
	if (!k->open) { RET(CAM_ERR_NOT_OPEN); return; }
	if (k->started) { RET(CAM_ERR_ALREADY_START); return; }
	k->started = 1; k->frame = 0;
	static int said;   /* what the camera sees, once a session: a puzzle may be waiting on it */
	if (!said) {
		said = 1; int src = source();
		overlay_toast_titled("Camera", src == 1 ? "The game is using the Vita's camera: your Mac's camera stands in for it\nChange it: [F10] > Controls"
		              : src == 0 && camera_light() ? "The game is using the Vita's camera: it sees a bright lamp\n[F7] turns it off"
		              : "The game is using the Vita's camera: it sees a dark room\n[F7] shines a lamp into it", 6.0);
	}
	RET(0);
}
static void h_stop(Cpu *c) { Cam *k = dev(c); if (!k) { RET(CAM_ERR_PARAM); return; } if (!k->open) { RET(CAM_ERR_NOT_OPEN); return; } if (!k->started) { RET(CAM_ERR_NOT_START); return; } k->started = 0; RET(0); }
static void h_read(Cpu *c) {   /* (devnum, SceCameraRead *) */
	Cam *k = dev(c); uint32_t rd = A1;
	if (!k || !rd) { RET(CAM_ERR_PARAM); return; }
	uint32_t sz = RD32(rd + CR_SIZE); if (sz <= 24 || sz > 0x40) { RET(CAM_ERR_PARAM); return; }
	if (!k->open) { RET(CAM_ERR_NOT_OPEN); return; }
	if (!k->started) { RET(CAM_ERR_NOT_START); return; }
	uint32_t base[3], size[3];
	for (int i = 0; i < 3; i++) {
		int have = k->buffer == BUF_SETBYREAD && sz >= CR_P_I + 4u * (uint32_t)i + 4u;
		base[i] = have ? RD32(rd + CR_P_I + 4 * i) : k->base[i]; size[i] = have ? RD32(rd + CR_SIZE_I + 4 * i) : k->size[i];
	}
	if (!base[0] || !size[0]) { RET(CAM_ERR_PARAM); return; }
	write_frame(k, base, size);
	WR32(rd + CR_STATUS, 0);   /* SCE_CAMERA_STATUS_IS_ACTIVE */
	if (sz >= CR_FRAME + 8) { uint64_t f = k->frame; WR32(rd + CR_FRAME, (uint32_t)f); WR32(rd + CR_FRAME + 4, (uint32_t)(f >> 32)); }
	if (sz >= CR_TIMESTAMP + 8) { uint64_t t = guest_time_us(); WR32(rd + CR_TIMESTAMP, (uint32_t)t); WR32(rd + CR_TIMESTAMP + 4, (uint32_t)(t >> 32)); }
	k->frame++;
	RET(0);
}
static void h_is_active(Cpu *c) { Cam *k = dev(c); if (!k) { RET(CAM_ERR_PARAM); return; } if (!k->open) { RET(CAM_ERR_NOT_OPEN); return; } if (!k->started) { RET(CAM_ERR_NOT_START); return; } RET(1); }
static void h_setting(Cpu *c) { Cam *k = dev(c); if (!k) { RET(CAM_ERR_PARAM); return; } if (!k->open) { RET(CAM_ERR_NOT_OPEN); return; } RET(0); }   /* EV, white balance, auto control hold: the picture is ours */

const HleEntry hle_camera[] = {
	{"sceCameraOpen", h_open}, {"sceCameraClose", h_close}, {"sceCameraStart", h_start}, {"sceCameraStop", h_stop},
	{"sceCameraRead", h_read}, {"sceCameraIsActive", h_is_active},
	{"sceCameraSetEV", h_setting}, {"sceCameraSetWhiteBalance", h_setting}, {"sceCameraSetAutoControlHold", h_setting},
	{0, 0},
};
