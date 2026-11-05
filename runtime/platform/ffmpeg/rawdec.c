/*
 * One frame through an FFmpeg audio decoder's own decode callback, with the bytes it consumed: what the
 * Vita's SceAudiodec reports for a raw (unframed) AAC stream, and what FFmpeg's send/receive API keeps to
 * itself (it holds the rest of a packet back). Built by the platform's setup-toolchain.sh against the
 * pinned FFmpeg source tree (the callback is internal, the header with it too) into libnativ_rawdec.a beside
 * libavcodec.a; runtime/audio/audiodec.c calls it when build.py finds that library (RECOMP_RAWDEC).
 */
#include "libavcodec/codec_internal.h"
#include "libavcodec/avcodec.h"

/* < 0: an FFmpeg error; else the bytes consumed, *got 1 when a frame came out */
int nativ_rawdec_frame(AVCodecContext *ctx, AVFrame *frame, int *got, AVPacket *pkt) {
	const FFCodec *c = ffcodec(ctx->codec);
	*got = 0;
	if (c->cb_type != FF_CODEC_CB_TYPE_DECODE) return AVERROR(ENOSYS);
	av_frame_unref(frame);
	return c->cb.decode(ctx, frame, got, pkt);
}
