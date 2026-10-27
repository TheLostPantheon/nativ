/*
 * Video playback. The demuxer (SceMp4) and the movie player (SceAvPlayer) are Sony's own user modules,
 * recompiled from the firmware like libgxm (packs/<title>/pack.toml MODULES: libscemp4, libsceavplayer --
 * libscemp4 does no file I/O of its own: the caller feeds it). What is left at the system boundary is
 * the hardware H.264 decoder, SceVideodecUser (sceVideodec* / sceAvcdec*), and the codec engine's
 * memory (SceCodecEngineUser), implemented here: H.264 through libavcodec (RECOMP_AVCODEC: build.sh
 * links it where pkg-config finds it -- macOS and Linux). Without it the decoder fails to initialise
 * the way the console's does when the codec can't be set up, so a game's own error path runs
 * (Uncharted skips its intro movie and goes to the menu); succeeding silently -- a stub's 0 -- would
 * leave it waiting forever for frames that never come.
 *
 * sceAvcdecDecode takes one access unit and gives back the pictures that are ready, in display order
 * (the decoder holds a reordered stream's pictures back, as the hardware's does); sceAvcdecDecodeStop
 * drains what is still held, a call at a time; sceAvcdecDecodeFlush drops it (a seek). Pictures are
 * written in the pixel format the caller's SceAvcdecPicture asks for: YUV420 packed raster (a Y plane,
 * then interleaved UV -- what Uncharted asks for, sampled as a YUV420P2 texture), YUV420 raster (Y, U, V)
 * or RGBA8888 (converted here). The SceMp4 entries below are for a game that imports SceMp4 without
 * loading the module: a loaded module's export wins over them.
 */
#include "hle.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef RECOMP_AVCODEC
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#endif

#define SCE_KERNEL_ERROR_ERROR 0x80020001u
#define SCE_VIDEODEC_ERROR_INITIALIZE 0x8062080Bu
#define SCE_AVCDEC_ERROR_INVALID_TYPE 0x80620001u
#define SCE_AVCDEC_ERROR_INVALID_PARAM 0x80620002u
#define SCE_AVCDEC_ERROR_OUT_OF_MEMORY 0x80620003u
#define SCE_AVCDEC_ERROR_INVALID_POINTER 0x80620009u
#define SCE_AVCDEC_ERROR_INVALID_STREAM 0x8062000Du
#define SCE_VIDEODEC_TYPE_HW_AVCDEC 0x1001u
enum { PIX_RGBA8888 = 0x00, PIX_RGBA565 = 0x01, PIX_RGBA5551 = 0x02, PIX_YUV420_RASTER = 0x10, PIX_YUV420_PACKED_RASTER = 0x20 };

static int video_log(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_VIDEO_LOG") != 0; return v; }

/* ---- the codec engine's memory: the caller hands over a block it allocated (sceCodecEngineOpenUnmapMemBlock)
 * and takes the decoder's and the library's working memory from it; the host decoder keeps its own, so this
 * only has to hand out aligned, non-overlapping ranges of that block */
typedef struct { uint32_t base, size, used; int open; } UnmapBlock;
static UnmapBlock blocks[8];
static pthread_mutex_t blocks_mx = PTHREAD_MUTEX_INITIALIZER;
static void h_ce_open(Cpu *c) {
	uint32_t addr = A0, size = A1; int id = -1;
	pthread_mutex_lock(&blocks_mx);
	for (int i = 0; i < 8; i++) if (!blocks[i].open) { blocks[i] = (UnmapBlock){addr, size, 0, 1}; id = i; break; }
	pthread_mutex_unlock(&blocks_mx);
	if (video_log()) fprintf(stderr, "video: codec engine block %d at %#x, %#x bytes\n", id, addr, size);
	RET(id < 0 ? SCE_AVCDEC_ERROR_OUT_OF_MEMORY : (uint32_t)(id + 1));
}
static void h_ce_alloc(Cpu *c) {
	uint32_t id = A0 - 1, size = A1, align = A2 ? A2 : 1, a = 0;
	pthread_mutex_lock(&blocks_mx);
	if (id < 8 && blocks[id].open) {
		UnmapBlock *b = &blocks[id];
		uint32_t at = (b->base + b->used + align - 1) & ~(align - 1);
		if (at + size <= b->base + b->size) { a = at; b->used = at + size - b->base; }
	}
	pthread_mutex_unlock(&blocks_mx);
	RET(a);
}
static void h_ce_free(Cpu *c) { RET(0); }   /* (the ranges go with the block) */
static void h_ce_close(Cpu *c) {
	uint32_t id = A0 - 1;
	pthread_mutex_lock(&blocks_mx);
	if (id < 8) blocks[id].open = 0;
	pthread_mutex_unlock(&blocks_mx);
	RET(0);
}

/* ---- the library: nothing to set up on the host; its working memory is the caller's to have allocated */
static void h_videodec_init(Cpu *c) {
#ifdef RECOMP_AVCODEC
	RET(A0 == SCE_VIDEODEC_TYPE_HW_AVCDEC ? 0 : SCE_AVCDEC_ERROR_INVALID_TYPE);
#else
	RET(SCE_VIDEODEC_ERROR_INITIALIZE);
#endif
}
static void h_videodec_term(Cpu *c) { RET(0); }
static void h_videodec_query_mem(Cpu *c) {   /* (codec, const SceVideodecQueryInitInfo *, uint32_t *memSize) */
#ifdef RECOMP_AVCODEC
	if (A0 != SCE_VIDEODEC_TYPE_HW_AVCDEC) { RET(SCE_AVCDEC_ERROR_INVALID_TYPE); return; }
	if (A2) WR32(A2, 0x80000);
	RET(0);
#else
	RET(SCE_VIDEODEC_ERROR_INITIALIZE);
#endif
}
static void h_avcdec_query_mem(Cpu *c) {   /* (codec, const SceAvcdecQueryDecoderInfo *, SceAvcdecDecoderInfo *) */
	uint32_t q = A1, info = A2;
	if (A0 != SCE_VIDEODEC_TYPE_HW_AVCDEC) { RET(SCE_AVCDEC_ERROR_INVALID_TYPE); return; }
	if (!q || !info) { RET(SCE_AVCDEC_ERROR_INVALID_POINTER); return; }
	uint32_t w = RD32(q), h = RD32(q + 4), refs = RD32(q + 8);
	if (!w || !h || w > 1920 || h > 1088 || refs > 16) { RET(SCE_AVCDEC_ERROR_INVALID_PARAM); return; }
	uint32_t frame = ((w + 15) & ~15u) * ((h + 15) & ~15u) * 3 / 2;
	/* a frame per reference frame, 256 KiB aligned (Vita3K's figure). The host decoder keeps its own memory, so
	 * this only has to fit: Uncharted sizes its codec-engine block as this + the library's + 1 MiB of slack and
	 * then takes the two at 256 KiB and 1 MiB alignment -- a larger figure failed the second allocation */
	WR32(info, (frame * (refs ? refs : 1) + 0x3ffffu) & ~0x3ffffu);
	RET(0);
}

#ifdef RECOMP_AVCODEC
typedef struct {
	AVCodecContext *ctx; AVPacket *pkt; AVFrame *frm;
	uint8_t *buf; size_t cap;     /* the access unit, in Annex B with libavcodec's padding */
	int used, draining, eof;
	uint64_t aus, pics;
	pthread_mutex_t mx;
} Dec;
static Dec decs[8];
static pthread_mutex_t decs_mx = PTHREAD_MUTEX_INITIALIZER;
static Dec *dec_of(uint32_t ctrl) {   /* SceAvcdecCtrl.handle: index + 0x100 */
	if (!ctrl) return 0;
	uint32_t h = RD32(ctrl) - 0x100;
	return h < 8 && decs[h].used ? &decs[h] : 0;
}

/* the access unit into Annex B: as given when it already has start codes, else (an MP4 sample: 4-byte
 * big-endian NAL lengths) each length replaced by a start code */
static int au_annexb(Dec *d, const uint8_t *p, uint32_t n) {
	size_t need = (size_t)n * 2 + 64 + AV_INPUT_BUFFER_PADDING_SIZE;
	if (d->cap < need) { free(d->buf); d->buf = malloc(need); d->cap = d->buf ? need : 0; if (!d->buf) return -1; }
	size_t w = 0;
	if (n >= 4 && ((p[0] == 0 && p[1] == 0 && p[2] == 1) || (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1))) { memcpy(d->buf, p, n); w = n; }
	else {
		for (uint32_t o = 0; o + 4 <= n;) {
			uint32_t len = (uint32_t)p[o] << 24 | (uint32_t)p[o + 1] << 16 | (uint32_t)p[o + 2] << 8 | p[o + 3];
			o += 4;
			if (len > n - o) { memcpy(d->buf, p, n); w = n; break; }   /* not length-prefixed after all: as given */
			static const uint8_t sc[4] = {0, 0, 0, 1};
			memcpy(d->buf + w, sc, 4); memcpy(d->buf + w + 4, p + o, len); w += 4 + len; o += len;
		}
	}
	memset(d->buf + w, 0, AV_INPUT_BUFFER_PADDING_SIZE);
	return (int)w;
}

static inline uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
/* a decoded picture into the caller's SceAvcdecPicture (size, SceAvcdecFrame at +4, SceAvcdecInfo at +0x44) */
static void put_picture(Dec *d, uint32_t pic, const AVFrame *f) {
	uint32_t type = RD32(pic + 4), pitch = RD32(pic + 8), bufh = RD32(pic + 0x10), p0 = RD32(pic + 0x3c), p1 = RD32(pic + 0x40);
	int w = f->width, h = f->height;
	if (!pitch) pitch = (uint32_t)w;
	if (!bufh) bufh = (uint32_t)h;
	if ((uint32_t)w > pitch) w = (int)pitch;
	if ((uint32_t)h > bufh) h = (int)bufh;
	const uint8_t *Y = f->data[0], *U = f->data[1], *V = f->data[2];
	int ys = f->linesize[0], us = f->linesize[1], vs = f->linesize[2];
	int planar = f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P;
	if (!planar) { static int said; if (!said++) fprintf(stderr, "video: decoder gave pixel format %d, not 4:2:0 planar -- picture left as it was\n", f->format); }
	else if (p0 && (type == PIX_YUV420_PACKED_RASTER || type == PIX_YUV420_RASTER)) {
		uint8_t *dy = G(p0);
		for (int y = 0; y < h; y++) memcpy(dy + (size_t)y * pitch, Y + (size_t)y * ys, (size_t)w);
		uint8_t *dc = p1 ? G(p1) : dy + (size_t)pitch * bufh;   /* the chroma right after the luma unless a second plane is given */
		if (type == PIX_YUV420_PACKED_RASTER)
			for (int y = 0; y < h / 2; y++) {
				uint8_t *o = dc + (size_t)y * pitch; const uint8_t *u = U + (size_t)y * us, *v = V + (size_t)y * vs;
				for (int x = 0; x < w / 2; x++) { o[2 * x] = u[x]; o[2 * x + 1] = v[x]; }
			}
		else {
			uint8_t *dv = dc + (size_t)(pitch / 2) * (bufh / 2);
			for (int y = 0; y < h / 2; y++) { memcpy(dc + (size_t)y * (pitch / 2), U + (size_t)y * us, (size_t)w / 2); memcpy(dv + (size_t)y * (pitch / 2), V + (size_t)y * vs, (size_t)w / 2); }
		}
	} else if (p0 && type == PIX_RGBA8888) {   /* BT.601, or BT.709 when the stream says so; limited range unless full */
		int bt709 = f->colorspace == AVCOL_SPC_BT709, full = f->color_range == AVCOL_RANGE_JPEG;
		int ky = full ? 256 : 298, oy = full ? 0 : 16;
		int rv = bt709 ? (full ? 403 : 459) : (full ? 359 : 409), gu = bt709 ? (full ? 48 : 55) : (full ? 88 : 100);
		int gv = bt709 ? (full ? 120 : 136) : (full ? 183 : 208), bu = bt709 ? (full ? 475 : 541) : (full ? 454 : 516);
		uint8_t a = RD8(pic + 0x2c), *o = G(p0);
		for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
			int yy = (Y[(size_t)y * ys + x] - oy) * ky, u = U[(size_t)(y / 2) * us + x / 2] - 128, v = V[(size_t)(y / 2) * vs + x / 2] - 128;
			uint8_t *px = o + ((size_t)y * pitch + x) * 4;
			px[0] = clamp8((yy + rv * v + 128) >> 8); px[1] = clamp8((yy - gu * u - gv * v + 128) >> 8); px[2] = clamp8((yy + bu * u + 128) >> 8); px[3] = a;
		}
	} else { static int said; if (!said++) fprintf(stderr, "video: picture format %#x not written (picture %#x)\n", type, p0); }
	WR32(pic + 0x14, (uint32_t)f->width); WR32(pic + 0x18, (uint32_t)f->height);
	for (int i = 0; i < 4; i++) WR32(pic + 0x1c + 4 * i, 0);   /* (libavcodec has applied the stream's cropping) */
	/* SceAvcdecInfo: the frame rate as H.264 timing (time_scale / (2 num_units_in_tick)), aspect, colour, pts */
	AVRational fr = d->ctx->framerate;
	WR32(pic + 0x44, fr.num > 0 && fr.den > 0 ? (uint32_t)fr.den : 0); WR32(pic + 0x48, fr.num > 0 && fr.den > 0 ? (uint32_t)fr.num * 2 : 0);
	WR8(pic + 0x4c, fr.num > 0); WR8(pic + 0x4d, f->sample_aspect_ratio.num > 0 ? 255 : 0);
	WR16(pic + 0x4e, (uint16_t)(f->sample_aspect_ratio.num > 0 ? f->sample_aspect_ratio.num : 0)); WR16(pic + 0x50, (uint16_t)(f->sample_aspect_ratio.num > 0 ? f->sample_aspect_ratio.den : 0));
	WR8(pic + 0x52, (uint8_t)f->color_primaries); WR8(pic + 0x53, (uint8_t)f->color_trc); WR8(pic + 0x54, (uint8_t)f->colorspace); WR8(pic + 0x55, f->color_range == AVCOL_RANGE_JPEG);
	WR8(pic + 0x56, 0); WR8(pic + 0x57, 0); WR8(pic + 0x58, 0);
	int64_t pts = f->pts != AV_NOPTS_VALUE ? f->pts : f->best_effort_timestamp;
	uint64_t p = pts == AV_NOPTS_VALUE ? ~0ull : (uint64_t)pts;
	WR32(pic + 0x5c, (uint32_t)(p >> 32)); WR32(pic + 0x60, (uint32_t)p);
}

/* the pictures that are ready, up to the caller's numOfElm, into its array; how many */
static uint32_t take_pictures(Dec *d, uint32_t arr) {
	uint32_t max = RD32(arr + 4), list = RD32(arr + 8), n = 0;
	while (n < max) {
		int r = avcodec_receive_frame(d->ctx, d->frm);
		if (r == AVERROR_EOF) { d->eof = 1; break; }
		if (r < 0) break;
		uint32_t pic = list ? RD32(list + 4 * n) : 0;
		if (pic) put_picture(d, pic, d->frm);
		av_frame_unref(d->frm);
		n++; d->pics++;
	}
	WR32(arr, n);
	return n;
}
#endif

static void h_avcdec_create(Cpu *c) {   /* (codec, SceAvcdecCtrl *, const SceAvcdecQueryDecoderInfo *) */
	if (A0 != SCE_VIDEODEC_TYPE_HW_AVCDEC) { RET(SCE_AVCDEC_ERROR_INVALID_TYPE); return; }
	if (!A1 || !A2) { RET(SCE_AVCDEC_ERROR_INVALID_POINTER); return; }
#ifdef RECOMP_AVCODEC
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec) { RET(SCE_VIDEODEC_ERROR_INITIALIZE); return; }
	pthread_mutex_lock(&decs_mx);
	int k = -1;
	for (int i = 0; i < 8; i++) if (!decs[i].used) { k = i; break; }
	if (k < 0) { pthread_mutex_unlock(&decs_mx); RET(SCE_AVCDEC_ERROR_OUT_OF_MEMORY); return; }
	Dec *d = &decs[k];
	memset(d, 0, sizeof *d);
	d->ctx = avcodec_alloc_context3(codec);
	if (d->ctx) {
		d->ctx->thread_count = 2; d->ctx->thread_type = FF_THREAD_SLICE;   /* slices only: frame threading would hold pictures back further than the hardware does */
		if (avcodec_open2(d->ctx, codec, NULL) < 0) avcodec_free_context(&d->ctx);
	}
	d->pkt = av_packet_alloc(); d->frm = av_frame_alloc();
	if (!d->ctx || !d->pkt || !d->frm) {
		avcodec_free_context(&d->ctx); av_packet_free(&d->pkt); av_frame_free(&d->frm);
		pthread_mutex_unlock(&decs_mx); RET(SCE_AVCDEC_ERROR_OUT_OF_MEMORY); return;
	}
	pthread_mutex_init(&d->mx, NULL);
	d->used = 1;
	pthread_mutex_unlock(&decs_mx);
	WR32(A1, (uint32_t)k + 0x100);
	if (video_log()) fprintf(stderr, "video: decoder %d for %ux%u, %u reference frames\n", k, RD32(A2), RD32(A2 + 4), RD32(A2 + 8));
	RET(0);
#else
	RET(SCE_VIDEODEC_ERROR_INITIALIZE);
#endif
}
static void h_avcdec_delete(Cpu *c) {
#ifdef RECOMP_AVCODEC
	Dec *d = dec_of(A0);
	if (!d) { RET(SCE_AVCDEC_ERROR_INVALID_PARAM); return; }
	pthread_mutex_lock(&decs_mx);
	if (video_log()) fprintf(stderr, "video: decoder %d deleted after %llu access units, %llu pictures\n", (int)(d - decs), (unsigned long long)d->aus, (unsigned long long)d->pics);
	avcodec_free_context(&d->ctx); av_packet_free(&d->pkt); av_frame_free(&d->frm); free(d->buf);
	pthread_mutex_destroy(&d->mx);
	memset(d, 0, sizeof *d);
	pthread_mutex_unlock(&decs_mx);
	WR32(A0, 0);
	RET(0);
#else
	RET(SCE_AVCDEC_ERROR_INVALID_PARAM);
#endif
}
static void h_avcdec_decode(Cpu *c) {   /* (const SceAvcdecCtrl *, const SceAvcdecAu *, SceAvcdecArrayPicture *) */
#ifdef RECOMP_AVCODEC
	Dec *d = dec_of(A0); uint32_t au = A1, arr = A2;
	if (!au || !arr) { RET(SCE_AVCDEC_ERROR_INVALID_POINTER); return; }
	if (!d) { RET(SCE_AVCDEC_ERROR_INVALID_PARAM); return; }
	pthread_mutex_lock(&d->mx);
	if (d->draining || d->eof) { avcodec_flush_buffers(d->ctx); d->draining = d->eof = 0; }   /* a stream after a stop: starts afresh */
	uint32_t es = RD32(au + 0x10), size = RD32(au + 0x14);
	uint64_t pts = (uint64_t)RD32(au) << 32 | RD32(au + 4), dts = (uint64_t)RD32(au + 8) << 32 | RD32(au + 12);
	int sent = 0, r = 0;
	if (es && size) {
		int n = au_annexb(d, G(es), size);
		if (n > 0) {
			d->pkt->data = d->buf; d->pkt->size = n;
			d->pkt->pts = pts == ~0ull ? AV_NOPTS_VALUE : (int64_t)pts; d->pkt->dts = dts == ~0ull ? AV_NOPTS_VALUE : (int64_t)dts;
			r = avcodec_send_packet(d->ctx, d->pkt);
			if (r == AVERROR(EAGAIN)) {   /* pictures waiting: the caller takes them first, then the unit goes in */
				uint32_t got = take_pictures(d, arr);
				r = avcodec_send_packet(d->ctx, d->pkt);
				sent = r >= 0;
				if (video_log() && d->aus < 8) fprintf(stderr, "video: AU %llu (%u bytes) after %u pictures: %d\n", (unsigned long long)d->aus, size, got, r);
				d->aus += sent;
				av_packet_unref(d->pkt);
				pthread_mutex_unlock(&d->mx);
				RET(r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_INVALIDDATA ? SCE_AVCDEC_ERROR_INVALID_STREAM : 0);
				return;
			}
			sent = r >= 0;
			av_packet_unref(d->pkt);
		}
	}
	uint32_t got = take_pictures(d, arr);
	if (video_log() && (d->aus < 8 || r < 0)) {
		const uint8_t *p = size ? G(es) : 0;
		fprintf(stderr, "video: AU %llu: %u bytes [%02x %02x %02x %02x %02x] pts %#llx dts %#llx -> send %d, %u picture(s)\n", (unsigned long long)d->aus, size,
		        p ? p[0] : 0, p ? p[1] : 0, p ? p[2] : 0, p ? p[3] : 0, p ? p[4] : 0, (unsigned long long)pts, (unsigned long long)dts, r, got);
	}
	d->aus += sent;
	pthread_mutex_unlock(&d->mx);
	RET(r < 0 && r != AVERROR_INVALIDDATA ? SCE_AVCDEC_ERROR_INVALID_STREAM : 0);   /* (a damaged unit is skipped, as the hardware conceals it) */
#else
	RET(SCE_AVCDEC_ERROR_INVALID_PARAM);
#endif
}
static void h_avcdec_stop(Cpu *c) {   /* (const SceAvcdecCtrl *, SceAvcdecArrayPicture *): the held pictures, a call at a time */
#ifdef RECOMP_AVCODEC
	Dec *d = dec_of(A0); uint32_t arr = A1;
	if (!arr) { RET(SCE_AVCDEC_ERROR_INVALID_POINTER); return; }
	if (!d) { RET(SCE_AVCDEC_ERROR_INVALID_PARAM); return; }
	pthread_mutex_lock(&d->mx);
	if (!d->draining && !d->eof) { avcodec_send_packet(d->ctx, NULL); d->draining = 1; }
	uint32_t got = d->eof ? (WR32(arr, 0), 0) : take_pictures(d, arr);
	if (video_log()) fprintf(stderr, "video: stop -> %u picture(s)%s\n", got, d->eof ? " (drained)" : "");
	pthread_mutex_unlock(&d->mx);
	RET(0);
#else
	RET(SCE_AVCDEC_ERROR_INVALID_PARAM);
#endif
}
static void h_avcdec_flush(Cpu *c) {   /* drop everything held (a seek) */
#ifdef RECOMP_AVCODEC
	Dec *d = dec_of(A0);
	if (!d) { RET(SCE_AVCDEC_ERROR_INVALID_PARAM); return; }
	pthread_mutex_lock(&d->mx);
	avcodec_flush_buffers(d->ctx); d->draining = d->eof = 0;
	pthread_mutex_unlock(&d->mx);
	RET(0);
#else
	RET(SCE_AVCDEC_ERROR_INVALID_PARAM);
#endif
}
static void h_avcdec_available(Cpu *c) { RET(0x100000); }   /* room for an access unit: the host's input is never full */

static void h_mp4_fail(Cpu *c) { RET(SCE_KERNEL_ERROR_ERROR); }

const HleEntry hle_video[] = {
	{"SceMp4_05474AF0", h_mp4_fail}, {"SceMp4_20A01112", h_mp4_fail}, {"SceMp4_30E49E4D", h_mp4_fail}, {"SceMp4_32A15788", h_mp4_fail},
	{"SceMp4_40351E1A", h_mp4_fail}, {"SceMp4_40C64DC0", h_mp4_fail}, {"SceMp4_609E57AD", h_mp4_fail}, {"SceMp4_6B69F900", h_mp4_fail},
	{"SceMp4_7B4832FE", h_mp4_fail}, {"SceMp4_8BE0E3D3", h_mp4_fail}, {"SceMp4_920623C8", h_mp4_fail}, {"SceMp4_94E1305D", h_mp4_fail},
	{"SceMp4_96676BA0", h_mp4_fail}, {"SceMp4_A7C09746", h_mp4_fail}, {"SceMp4_A870587F", h_mp4_fail}, {"SceMp4_CB91CEBC", h_mp4_fail},
	{"SceMp4_D092A066", h_mp4_fail}, {"SceMp4_D2871F1C", h_mp4_fail}, {"SceMp4_EFE299C9", h_mp4_fail},
	{"sceVideodecInitLibraryWithUnmapMem", h_videodec_init}, {"sceVideodecInitLibrary", h_videodec_init}, {"sceVideodecTermLibrary", h_videodec_term},
	{"sceVideodecQueryMemSize", h_videodec_query_mem}, {"sceAvcdecQueryDecoderMemSize", h_avcdec_query_mem},
	{"sceAvcdecCreateDecoder", h_avcdec_create}, {"sceAvcdecDeleteDecoder", h_avcdec_delete}, {"sceAvcdecDecode", h_avcdec_decode},
	{"sceAvcdecDecodeStop", h_avcdec_stop}, {"sceAvcdecDecodeFlush", h_avcdec_flush}, {"sceAvcdecDecodeAvailableSize", h_avcdec_available},
	{"sceCodecEngineOpenUnmapMemBlock", h_ce_open}, {"sceCodecEngineAllocMemoryFromUnmapMemBlock", h_ce_alloc},
	{"sceCodecEngineFreeMemoryFromUnmapMemBlock", h_ce_free}, {"sceCodecEngineCloseUnmapMemBlock", h_ce_close},
	{0, 0},
};
