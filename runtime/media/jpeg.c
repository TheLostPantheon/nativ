/*
 * SceJpeg (the MJPEG decoder) and SceJpegEnc (the encoder) on the host. On the console both run on
 * the codec engine; here a JPEG is decoded and encoded with stb_image / stb_image_write (public
 * domain, third_party/stb), the YCbCr planes in between converted by hand (JFIF full range).
 *
 * Uncharted's photo minigame (BSGfx::cJpegEncoderDecoder, FUN_81239e42 and the wrappers after it):
 * a photo is the 960x544 screen through sceJpegEncoderCsc (BGRA -> YCbCr 4:2:0) and
 * sceJpegEncoderEncode; it shows photos (the target photo, the one just taken) through
 * sceJpegDecodeMJpegYCbCr at a quarter scale into a 240x136 YUV420P3 texture, and full size through
 * sceJpegDecodeMJpeg (RGBA). With none of these implemented every call returned 0 and wrote
 * nothing: the target photo was a blank box and a taken photo never showed.
 *
 * Output layout (the decoders' pitches) follows Vita3K's MJPEG module, read for semantics only:
 * the luma pitch is the image rounded up to the sampling's MCU and divided by the downscale, the
 * chroma planes half of it per subsampled direction, 16-aligned in the standard decode modes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "gfx.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG   /* (and PNG: the overlay's input icons, overlay.cpp) */
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "third_party/stb/stb_image_write.h"

#define SCE_JPEG_ERROR_DECODE_ERROR 0x80650009u        /* SceJpeg's codes as Vita3K lists them */
#define SCE_JPEG_ERROR_INVALID_POINTER 0x80650010u
#define SCE_JPEG_ERROR_INSUFFICIENT_BUFFER 0x80650066u   /* INVALID_DATA_SIZE: the output buffer is too small */
#define SCE_JPEGENC_ERROR_INSUFFICIENT_BUFFER 0x80650201u
#define SCE_JPEGENC_ERROR_INVALID_PIXELFORMAT 0x80650203u
#define SCE_JPEGENC_ERROR_INVALID_POINTER 0x80650205u

static int jdebug(void) { static int d = -1; if (d < 0) d = getenv("RECOMP_JPEG_DEBUG") != 0; return d; }
static int align_up(int v, int a) { return (v + a - 1) / a * a; }
static uint8_t clamp8(float v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5f); }

/* the CPU is about to read (or write) guest memory the GPU backend may hold newer contents of: its
 * overlapping render targets go back to guest memory first (the photo is the presented screen) */
static void sync_for_cpu(uint32_t addr, uint32_t len) { if (addr && len) gfx_guest_writing(addr, len); }
static void cpu_wrote(uint32_t addr, uint32_t len) { if (addr && len) gfx_guest_written(addr, len); }

/* ---- the decoder (SceJpegUser) */

/* decodeMode: bits 0-2 the DHT/sampling mode (0 WITH_DHT, 1 WITHOUT_DHT: standard 4:2:0 / 4:2:2;
 * 2, 3 any sampling), bits 4-6 the downscale (1/2, 1/4, 1/8) */
typedef struct { int w, h, comp; uint8_t *rgb; } Img;
static int decode(uint32_t jpeg, uint32_t size, Img *im) {
	if (!jpeg || !size) return 0;
	im->rgb = stbi_load_from_memory(G(jpeg), (int)size, &im->w, &im->h, &im->comp, 3);
	if (!im->rgb && jdebug()) fprintf(stderr, "    jpeg: decode of %u bytes at %#x failed: %s\n", size, jpeg, stbi_failure_reason());
	return im->rgb != 0;
}
/* the JPEG's chroma subsampling from its SOF0/SOF2 marker (component 1's sampling factors): 0x22 4:2:0,
 * 0x21 4:2:2, 0x11 4:4:4 */
static int jpeg_sampling(uint32_t jpeg, uint32_t size) {
	const uint8_t *p = G(jpeg);
	for (uint32_t i = 2; i + 12 < size;) {
		if (p[i] != 0xFF) { i++; continue; }
		uint8_t m = p[i + 1];
		if (m == 0xC0 || m == 0xC1 || m == 0xC2) return p[i + 2 + 2 + 1 + 2 + 2 + 1 + 1];   /* length, precision, height, width, ncomp, comp id -> sampling */
		if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
		i += 2 + ((uint32_t)p[i + 2] << 8 | p[i + 3]);
	}
	return 0x22;
}
/* the decoded size and plane pitches for a decode mode */
typedef struct { int w, h, cw, ch, py, pyh, pc, pch, hs, vs; } Layout;
static void layout(int w, int h, int sampling, int mode, Layout *l) {
	int ratio = (mode & 0x70) == 0x10 ? 2 : (mode & 0x70) == 0x20 ? 4 : (mode & 0x70) == 0x40 ? 8 : 1;
	int standard = (mode & 7) < 2;
	l->hs = (sampling >> 4) >= 2 ? 2 : 1; l->vs = (sampling & 15) >= 2 ? 2 : 1;
	int bw = 8 * l->hs, bh = 8 * l->vs;
	l->w = (w + ratio - 1) / ratio; l->h = (h + ratio - 1) / ratio;
	l->py = align_up(align_up(w, bw) / ratio, bw); l->pyh = align_up(align_up(h, bh) / ratio, standard ? 8 : 1);
	l->pc = l->py / l->hs; l->pch = l->pyh / l->vs;
	if (standard && (l->hs == 2)) l->pc = align_up(l->pc, 16);
	l->cw = (l->w + l->hs - 1) / l->hs; l->ch = (l->h + l->vs - 1) / l->vs;
}
/* the RGB image box-filtered down to l->w x l->h */
static void sample_rgb(const Img *im, const Layout *l, int x, int y, float rgb[3]) {
	int r = (im->w + l->w - 1) / l->w; if (r < 1) r = 1;
	int x0 = x * im->w / l->w, y0 = y * im->h / l->h, n = 0; rgb[0] = rgb[1] = rgb[2] = 0;
	for (int yy = y0; yy < y0 + r && yy < im->h; yy++)
		for (int xx = x0; xx < x0 + r && xx < im->w; xx++, n++)
			for (int k = 0; k < 3; k++) rgb[k] += im->rgb[3 * (yy * im->w + xx) + k];
	if (n) for (int k = 0; k < 3; k++) rgb[k] /= (float)n;
}
static void rgb_to_ycc(const float c[3], float *y, float *cb, float *cr) {
	*y = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
	*cb = 128 - 0.168736f * c[0] - 0.331264f * c[1] + 0.5f * c[2];
	*cr = 128 + 0.5f * c[0] - 0.418688f * c[1] - 0.081312f * c[2];
}

static void h_jpeg_init(Cpu *c) { RET(0); }
static void h_jpeg_finish(Cpu *c) { RET(0); }
/* (jpeg, size, out, out size, decodeMode, coef buffer, coef size) -> pitch width << 16 | pitch height:
 * planar Y, Cb, Cr at the layout's pitches */
static void h_jpeg_decode_ycbcr(Cpu *c) {
	uint32_t jpeg = A0, size = A1, out = A2, osize = A3, mode = STK(0);
	Img im = {0};
	if (!out) { RET(SCE_JPEG_ERROR_INVALID_POINTER); return; }
	if (!decode(jpeg, size, &im)) { RET(SCE_JPEG_ERROR_DECODE_ERROR); return; }
	Layout l; layout(im.w, im.h, jpeg_sampling(jpeg, size), (int)mode, &l);
	uint32_t ysz = (uint32_t)(l.py * l.pyh), csz = (uint32_t)(l.pc * l.pch);
	if (ysz + 2 * csz > osize) { stbi_image_free(im.rgb); RET(SCE_JPEG_ERROR_INSUFFICIENT_BUFFER); return; }
	sync_for_cpu(out, ysz + 2 * csz);
	uint8_t *Y = G(out), *Cb = G(out + ysz), *Cr = G(out + ysz + csz);
	memset(Y, 0, ysz); memset(Cb, 128, 2 * csz);
	static float *cbs, *crs; static size_t cap;
	size_t need = (size_t)l.w * l.h; if (need > cap) { free(cbs); free(crs); cbs = malloc(need * sizeof(float)); crs = malloc(need * sizeof(float)); cap = need; }
	for (int y = 0; y < l.h; y++)
		for (int x = 0; x < l.w; x++) {
			float rgb[3], yy, cb, cr; sample_rgb(&im, &l, x, y, rgb); rgb_to_ycc(rgb, &yy, &cb, &cr);
			Y[y * l.py + x] = clamp8(yy); cbs[y * l.w + x] = cb; crs[y * l.w + x] = cr;
		}
	for (int y = 0; y < l.ch; y++)
		for (int x = 0; x < l.cw; x++) {
			float sb = 0, sr = 0; int n = 0;
			for (int dy = 0; dy < l.vs; dy++) for (int dx = 0; dx < l.hs; dx++) {
				int sx = x * l.hs + dx, sy = y * l.vs + dy;
				if (sx < l.w && sy < l.h) { sb += cbs[sy * l.w + sx]; sr += crs[sy * l.w + sx]; n++; }
			}
			if (n) { Cb[y * l.pc + x] = clamp8(sb / n); Cr[y * l.pc + x] = clamp8(sr / n); }
		}
	cpu_wrote(out, ysz + 2 * csz);
	if (jdebug()) fprintf(stderr, "    jpeg: YCbCr decode %ux%u (sampling %#x, mode %#x) -> %dx%d pitch %d/%d at %#x\n", im.w, im.h, jpeg_sampling(jpeg, size), mode, l.w, l.h, l.py, l.pc, out);
	stbi_image_free(im.rgb);
	RET((uint32_t)l.py << 16 | (uint32_t)l.pyh);
}
/* (jpeg, size, out RGBA, out size, decodeMode, temp buffer, temp size, coef buffer, coef size): RGBA8888
 * rows of the luma pitch */
static void h_jpeg_decode_rgba(Cpu *c) {
	uint32_t jpeg = A0, size = A1, out = A2, osize = A3, mode = STK(0);
	Img im = {0};
	if (!out) { RET(SCE_JPEG_ERROR_INVALID_POINTER); return; }
	if (!decode(jpeg, size, &im)) { RET(SCE_JPEG_ERROR_DECODE_ERROR); return; }
	Layout l; layout(im.w, im.h, jpeg_sampling(jpeg, size), (int)mode, &l);
	uint32_t bytes = (uint32_t)(4 * l.py * l.pyh);
	if (bytes > osize) { stbi_image_free(im.rgb); RET(SCE_JPEG_ERROR_INSUFFICIENT_BUFFER); return; }
	sync_for_cpu(out, bytes);
	uint8_t *o = G(out); memset(o, 0, bytes);
	for (int y = 0; y < l.h; y++)
		for (int x = 0; x < l.w; x++) {
			float rgb[3]; sample_rgb(&im, &l, x, y, rgb);
			uint8_t *p = o + 4 * (y * l.py + x); p[0] = clamp8(rgb[0]); p[1] = clamp8(rgb[1]); p[2] = clamp8(rgb[2]); p[3] = 255;
		}
	cpu_wrote(out, bytes);
	if (jdebug()) fprintf(stderr, "    jpeg: RGBA decode %ux%u (mode %#x) -> %dx%d pitch %d at %#x\n", im.w, im.h, mode, l.w, l.h, l.py, out);
	stbi_image_free(im.rgb);
	RET((uint32_t)l.py << 16 | (uint32_t)l.pyh);
}

/* ---- the encoder (SceJpegEncUser). The context is the game's memory (GetContextSize bytes):
 * {in width, in height, pixel format, out buffer, out size, compression ratio, valid width, valid height} */
enum { E_W, E_H, E_FMT, E_OUT, E_OSIZE, E_RATIO, E_VW, E_VH, E_WORDS };
#define CTX(c_, k) ((c_) + 4u * (k))
static void enc_init(uint32_t ctx, uint32_t w, uint32_t h, uint32_t fmt, uint32_t out, uint32_t osize) {
	WR32(CTX(ctx, E_W), w); WR32(CTX(ctx, E_H), h); WR32(CTX(ctx, E_FMT), fmt); WR32(CTX(ctx, E_OUT), out); WR32(CTX(ctx, E_OSIZE), osize);
	WR32(CTX(ctx, E_RATIO), 64); WR32(CTX(ctx, E_VW), w); WR32(CTX(ctx, E_VH), h);
}
static void h_enc_context_size(Cpu *c) { RET(4 * E_WORDS); }
static void h_enc_init(Cpu *c) {   /* (context, in width, in height, pixel format, out buffer, out size) */
	if (!A0) { RET(SCE_JPEGENC_ERROR_INVALID_POINTER); return; }
	enc_init(A0, A1, A2, A3, STK(0), STK(1)); RET(0);
}
static void h_enc_init_param(Cpu *c) {   /* (context, SceJpegEncoderInitParam {size, inWidth, inHeight, pixelFormat, outBuffer, outSize, option}) */
	if (!A0 || !A1) { RET(SCE_JPEGENC_ERROR_INVALID_POINTER); return; }
	enc_init(A0, RD32(A1 + 4), RD32(A1 + 8), RD32(A1 + 12), RD32(A1 + 16), RD32(A1 + 20)); RET(0);
}
static void h_enc_end(Cpu *c) { RET(0); }
static void h_enc_valid_region(Cpu *c) { if (A0) { WR32(CTX(A0, E_VW), A1); WR32(CTX(A0, E_VH), A2); } RET(0); }
static void h_enc_ratio(Cpu *c) { if (A0) WR32(CTX(A0, E_RATIO), A1); RET(A1 > 255 ? 0x80650202u : 0); }
static void h_enc_output(Cpu *c) { if (A0) { WR32(CTX(A0, E_OUT), A1); WR32(CTX(A0, E_OSIZE), A2); } RET(0); }
/* (context, out YCbCr, in ARGB, in pitch in pixels, in pixel format): the context's in width x height
 * to planar Y, Cb, Cr (4:2:0 for pixel format 8, 4:2:2 for 9, the context's low nibble). In pixel
 * format 0 is RGBA in memory (R first), 4 BGRA (Vita3K's naming); Uncharted passes 4. */
static void h_enc_csc(Cpu *c) {
	uint32_t ctx = A0, out = A1, in = A2, pitch = A3, infmt = STK(0);
	if (!ctx || !out || !in) { RET(SCE_JPEGENC_ERROR_INVALID_POINTER); return; }
	int w = (int)RD32(CTX(ctx, E_W)), h = (int)RD32(CTX(ctx, E_H)), f = (int)(RD32(CTX(ctx, E_FMT)) & 15);
	if (f != 8 && f != 9) { RET(SCE_JPEGENC_ERROR_INVALID_PIXELFORMAT); return; }
	int vs = f == 8 ? 2 : 1, bgra = (infmt & 15) == 4;
	uint32_t ysz = (uint32_t)(w * h), csz = (uint32_t)((w / 2) * (h / vs));
	sync_for_cpu(in, 4 * pitch * (uint32_t)h); sync_for_cpu(out, ysz + 2 * csz);
	const uint8_t *src = G(in); uint8_t *Y = G(out), *Cb = G(out + ysz), *Cr = G(out + ysz + csz);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			const uint8_t *p = src + 4 * ((uint32_t)y * pitch + (uint32_t)x);
			float rgb[3] = {p[bgra ? 2 : 0], p[1], p[bgra ? 0 : 2]}, yy, cb, cr; rgb_to_ycc(rgb, &yy, &cb, &cr);
			Y[y * w + x] = clamp8(yy);
		}
	for (int y = 0; y < h / vs; y++)
		for (int x = 0; x < w / 2; x++) {
			float sb = 0, sr = 0;
			for (int dy = 0; dy < vs; dy++) for (int dx = 0; dx < 2; dx++) {
				const uint8_t *p = src + 4 * ((uint32_t)(y * vs + dy) * pitch + (uint32_t)(x * 2 + dx));
				float rgb[3] = {p[bgra ? 2 : 0], p[1], p[bgra ? 0 : 2]}, yy, cb, cr; rgb_to_ycc(rgb, &yy, &cb, &cr); sb += cb; sr += cr;
			}
			Cb[y * (w / 2) + x] = clamp8(sb / (2 * vs)); Cr[y * (w / 2) + x] = clamp8(sr / (2 * vs));
		}
	cpu_wrote(out, ysz + 2 * csz);
	if (jdebug()) fprintf(stderr, "    jpeg: csc %dx%d %s pitch %u %#x -> YCbCr 4:2:%d at %#x\n", w, h, bgra ? "BGRA" : "RGBA", pitch, in, vs == 2 ? 0 : 2, out);
	RET(0);
}
typedef struct { uint8_t *dst; uint32_t cap, n; int over; } Sink;
static void sink_put(void *ctx, void *data, int size) {
	Sink *s = ctx;
	if (s->n + (uint32_t)size > s->cap) { s->over = 1; return; }
	memcpy(s->dst + s->n, data, (size_t)size); s->n += (uint32_t)size;
}
/* (context, in YCbCr): the valid region of the planes as a JPEG into the context's out buffer; returns
 * its size. The compression ratio (1-255, the game's 64) maps to stb's quality, higher = smaller. */
static void h_enc_encode(Cpu *c) {
	uint32_t ctx = A0, in = A1;
	if (!ctx || !in) { RET(SCE_JPEGENC_ERROR_INVALID_POINTER); return; }
	int w = (int)RD32(CTX(ctx, E_W)), h = (int)RD32(CTX(ctx, E_H)), f = (int)(RD32(CTX(ctx, E_FMT)) & 15);
	int vw = (int)RD32(CTX(ctx, E_VW)), vh = (int)RD32(CTX(ctx, E_VH)), ratio = (int)RD32(CTX(ctx, E_RATIO));
	uint32_t out = RD32(CTX(ctx, E_OUT)), osize = RD32(CTX(ctx, E_OSIZE));
	if (f != 8 && f != 9) { RET(SCE_JPEGENC_ERROR_INVALID_PIXELFORMAT); return; }
	if (vw <= 0 || vw > w) vw = w;
	if (vh <= 0 || vh > h) vh = h;
	int vs = f == 8 ? 2 : 1;
	uint32_t ysz = (uint32_t)(w * h), csz = (uint32_t)((w / 2) * (h / vs));
	sync_for_cpu(in, ysz + 2 * csz);
	const uint8_t *Y = G(in), *Cb = G(in + ysz), *Cr = G(in + ysz + csz);
	uint8_t *rgb = malloc((size_t)vw * vh * 3);
	for (int y = 0; y < vh; y++)
		for (int x = 0; x < vw; x++) {
			float yy = Y[y * w + x], cb = Cb[(y / vs) * (w / 2) + x / 2] - 128.f, cr = Cr[(y / vs) * (w / 2) + x / 2] - 128.f;
			uint8_t *p = rgb + 3 * (y * vw + x);
			p[0] = clamp8(yy + 1.402f * cr); p[1] = clamp8(yy - 0.344136f * cb - 0.714136f * cr); p[2] = clamp8(yy + 1.772f * cb);
		}
	int q = 100 - ratio / 4; if (q < 30) q = 30; if (q > 95) q = 95;
	sync_for_cpu(out, osize);
	Sink s = {G(out), osize, 0, 0};
	int ok = stbi_write_jpg_to_func(sink_put, &s, vw, vh, 3, rgb, q);
	free(rgb);
	cpu_wrote(out, s.n);
	if (jdebug()) fprintf(stderr, "    jpeg: encode %dx%d (valid %dx%d) quality %d -> %u bytes at %#x%s\n", w, h, vw, vh, q, s.n, out, s.over ? " (did not fit)" : "");
	if (!ok || s.over) { RET(SCE_JPEGENC_ERROR_INSUFFICIENT_BUFFER); return; }
	RET(s.n);
}

const HleEntry hle_jpeg[] = {
	{"sceJpegInitMJpeg", h_jpeg_init}, {"sceJpegInitMJpegWithParam", h_jpeg_init}, {"sceJpegFinishMJpeg", h_jpeg_finish},
	{"sceJpegDecodeMJpegYCbCr", h_jpeg_decode_ycbcr}, {"sceJpegDecodeMJpeg", h_jpeg_decode_rgba},
	{"sceJpegEncoderGetContextSize", h_enc_context_size}, {"sceJpegEncoderInit", h_enc_init}, {"sceJpegEncoderInitWithParam", h_enc_init_param},
	{"sceJpegEncoderEnd", h_enc_end}, {"sceJpegEncoderSetValidRegion", h_enc_valid_region}, {"sceJpegEncoderSetCompressionRatio", h_enc_ratio},
	{"sceJpegEncoderSetOutputAddr", h_enc_output}, {"sceJpegEncoderSetHeaderMode", h_enc_end}, {"sceJpegEncoderCsc", h_enc_csc},
	{"sceJpegEncoderEncode", h_enc_encode},
	{0, 0},
};
