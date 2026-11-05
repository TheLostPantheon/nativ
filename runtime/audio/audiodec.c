/*
 * SceAudiodecUser: the system audio decoder a game feeds its own compressed streams to (music, movie
 * soundtracks) and whose PCM it then plays itself through SceAudioOut. Implemented here for ATRAC9 with
 * the LibAtrac9 already vendored for the NGS player (third_party/libatrac9, MIT), and for AAC (a movie
 * player's soundtrack) with FFmpeg's decoder where the build has it (RECOMP_RAWDEC: the static FFmpeg plus
 * runtime/platform/ffmpeg/rawdec.c's shim, which reports the bytes a raw frame consumed). Other codec types
 * (MP3, CELP; AAC without RECOMP_RAWDEC) fail with SCE_AUDIODEC_ERROR_INVALID_TYPE and a log line, so a
 * game's own error path runs; before this file every call was an unresolved import returning 0 with
 * nothing decoded, which a game reads as success and plays silence.
 *
 * The contract (the console's, as Vita3K's SceAudiodecUser implements it): SceAudiodecCtrl is 0x28 bytes
 * {size, handle, pEs, inputEsSize, maxEsSize, pPcm, outputPcmSize, maxPcmSize, wordLength, pInfo};
 * SceAudiodecInfo is {size, then the codec's union} -- for ATRAC9 {configData (4 bytes), ch, bitRate,
 * samplingRate, superFrameSize, framesInSuperFrame}. CreateDecoder reads the configuration word, fills
 * the rest of the info and the ctrl's maxEsSize/maxPcmSize and hands out a handle. Decode decodes one
 * ATRAC9 frame from pEs to pPcm (16-bit interleaved) and reports the bytes taken (inputEsSize, the
 * superframe's padding counted with its last frame) and the bytes written (outputPcmSize). For AAC the
 * info is {isAdts, ch, samplingRate, isSbr}; maxEsSize is 1536 (+0x100 with ADTS headers), maxPcmSize
 * ch x 2048 (twice that with SBR); Decode takes one raw (or ADTS) frame. GetContextSize answers the
 * memory a CreateDecoderExternal caller sets aside (AAC 0x18000); the host keeps its own, so the External
 * calls are the plain ones. ClearContext drops the decoder's history (a seek or loop restart).
 * RECOMP_AUDIODEC_LOG=1 logs the lifecycle.
 */
#include "hle.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "third_party/libatrac9/libatrac9.h"
#ifdef RECOMP_RAWDEC
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
int nativ_rawdec_frame(AVCodecContext *ctx, AVFrame *frame, int *got, AVPacket *pkt);   /* runtime/platform/ffmpeg/rawdec.c */
#endif

#define SCE_AUDIODEC_ERROR_API_FAIL 0x807F0000u
#define SCE_AUDIODEC_ERROR_INVALID_TYPE 0x807F0001u
#define SCE_AUDIODEC_ERROR_NOT_INITIALIZED 0x807F0005u
#define SCE_AUDIODEC_ERROR_ALL_HANDLES_IN_USE 0x807F0007u
#define SCE_AUDIODEC_ERROR_INVALID_PTR 0x807F0008u
#define SCE_AUDIODEC_ERROR_INVALID_HANDLE 0x807F0009u
#define SCE_AUDIODEC_ERROR_NOT_HANDLE_IN_USE 0x807F000Au
#define SCE_AUDIODEC_ERROR_INVALID_SIZE 0x807F000Du
#define SCE_AUDIODEC_AT9_ERROR_INVALID_CONFIG 0x807F2000u
#define SCE_AUDIODEC_TYPE_AT9 0x1003u
#define SCE_AUDIODEC_TYPE_AAC 0x1005u
#define SCE_AUDIODEC_AT9_MAX_ES_SIZE 1024u
#define SCE_AUDIODEC_AAC_MAX_ES_SIZE 1536u
#define SCE_AUDIODEC_AAC_CONTEXT_SIZE 0x18000u

/* SceAudiodecCtrl field offsets */
enum { C_SIZE = 0, C_HANDLE = 4, C_ES = 8, C_ES_USED = 12, C_ES_MAX = 16, C_PCM = 20, C_PCM_GIVEN = 24, C_PCM_MAX = 28, C_WORD = 32, C_INFO = 36, CTRL_SIZE = 0x28 };
/* SceAudiodecInfo (ATRAC9) field offsets */
enum { I_CONFIG = 4, I_CH = 8, I_BITRATE = 12, I_RATE = 16, I_SF_SIZE = 20, I_SF_FRAMES = 24 };
/* SceAudiodecInfo (AAC) field offsets */
enum { I_AAC_ADTS = 4, I_AAC_CH = 8, I_AAC_RATE = 12, I_AAC_SBR = 16 };
#ifdef RECOMP_RAWDEC
static int aac_supported(void) { return 1; }
#else
static int aac_supported(void) { return 0; }
#endif

#define MAX_DECODERS 32
typedef struct {
	int used;
	uint32_t type;
	void *at9;
	unsigned char config[4];
	Atrac9CodecInfo ci;
	int sf_frame, sf_left;   /* the frame within the superframe; its bytes not yet taken */
	short pcm[256 * 8];      /* one frame: at most 256 samples x 8 channels (LibAtrac9's limits) */
#ifdef RECOMP_RAWDEC
	AVCodecContext *aac; AVFrame *frame; AVPacket *pkt;
	uint8_t *in;             /* the frame's bytes, copied with FFmpeg's zero padding after them */
#endif
	uint32_t es_max, pcm_max;
} Decoder;
static Decoder decs[MAX_DECODERS];
static int at9_initialised, aac_initialised;
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;

static int ad_log(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_AUDIODEC_LOG") != 0; return v; }

static int at9_open(Decoder *d) {
	d->at9 = Atrac9GetHandle();
	if (!d->at9) return -1;
	if (Atrac9InitDecoder(d->at9, d->config)) { Atrac9ReleaseHandle(d->at9); d->at9 = 0; return -1; }
	Atrac9GetCodecInfo(d->at9, &d->ci);
	d->sf_frame = 0; d->sf_left = d->ci.superframeSize;
	return 0;
}

static int *initialised(uint32_t type) {
	if (type == SCE_AUDIODEC_TYPE_AT9) return &at9_initialised;
	if (type == SCE_AUDIODEC_TYPE_AAC && aac_supported()) return &aac_initialised;
	return 0;
}
static void h_init(Cpu *c) {
	uint32_t type = A0;
	int *on = initialised(type);
	if (!on) {
		fprintf(stderr, "    audiodec: codec type %#x not implemented (ATRAC9%s) -- INVALID_TYPE\n", type, aac_supported() ? ", AAC" : " only");
		RET(SCE_AUDIODEC_ERROR_INVALID_TYPE); return;
	}
	pthread_mutex_lock(&mx); *on = 1; pthread_mutex_unlock(&mx);
	if (ad_log()) fprintf(stderr, "audiodec: InitLibrary %s\n", type == SCE_AUDIODEC_TYPE_AAC ? "AAC" : "ATRAC9");
	RET(0);
}
static void h_term(Cpu *c) {
	int *on = initialised(A0);
	if (!on) { RET(SCE_AUDIODEC_ERROR_INVALID_TYPE); return; }
	pthread_mutex_lock(&mx); *on = 0; pthread_mutex_unlock(&mx);
	RET(0);
}

/* the ctrl's checks shared by every call: a pointer, the right size */
static uint32_t ctrl_check(uint32_t ctrl) {
	if (!ctrl) return SCE_AUDIODEC_ERROR_INVALID_PTR;
	if (RD32(ctrl + C_SIZE) != CTRL_SIZE) return SCE_AUDIODEC_ERROR_INVALID_SIZE;
	return 0;
}
static Decoder *by_handle(uint32_t ctrl) {   /* (under mx) */
	uint32_t h = RD32(ctrl + C_HANDLE);
	return h >= 1 && h <= MAX_DECODERS && decs[h - 1].used ? &decs[h - 1] : 0;
}

static void dec_free(Decoder *d) {   /* (under mx) */
	if (d->at9) Atrac9ReleaseHandle(d->at9);
#ifdef RECOMP_RAWDEC
	avcodec_free_context(&d->aac); av_frame_free(&d->frame); av_packet_free(&d->pkt); free(d->in);
#endif
	memset(d, 0, sizeof *d);
}

#ifdef RECOMP_RAWDEC
static uint32_t create_aac(uint32_t ctrl, uint32_t info, int external) {
	uint32_t adts = RD32(info + I_AAC_ADTS), ch = RD32(info + I_AAC_CH), rate = RD32(info + I_AAC_RATE), sbr = RD32(info + I_AAC_SBR);
	if (!ch || ch > 8 || !rate) { fprintf(stderr, "    audiodec: AAC info rejected (%u ch, %u Hz)\n", ch, rate); return SCE_AUDIODEC_ERROR_API_FAIL; }
	pthread_mutex_lock(&mx);
	if (!external && !aac_initialised) { pthread_mutex_unlock(&mx); return SCE_AUDIODEC_ERROR_NOT_INITIALIZED; }
	int i = 0; while (i < MAX_DECODERS && decs[i].used) i++;
	if (i == MAX_DECODERS) { pthread_mutex_unlock(&mx); return SCE_AUDIODEC_ERROR_ALL_HANDLES_IN_USE; }
	Decoder *d = &decs[i];
	memset(d, 0, sizeof *d);
	d->type = SCE_AUDIODEC_TYPE_AAC;
	d->es_max = SCE_AUDIODEC_AAC_MAX_ES_SIZE + (adts ? 0x100 : 0);
	d->pcm_max = ch * 2048 * (sbr ? 2 : 1);
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
	d->aac = codec ? avcodec_alloc_context3(codec) : 0;
	d->frame = av_frame_alloc(); d->pkt = av_packet_alloc(); d->in = calloc(1, d->es_max + AV_INPUT_BUFFER_PADDING_SIZE);
	int bad = !d->aac || !d->frame || !d->pkt || !d->in;
	if (!bad) {   /* raw frames carry no configuration: AAC-LC for the channels and rate in the info (FFmpeg's default without extradata) */
		av_channel_layout_default(&d->aac->ch_layout, (int)ch);
		d->aac->sample_rate = (int)rate;
		bad = avcodec_open2(d->aac, codec, 0) < 0;
	}
	if (bad) { dec_free(d); pthread_mutex_unlock(&mx); fprintf(stderr, "    audiodec: AAC decoder could not be opened\n"); return SCE_AUDIODEC_ERROR_API_FAIL; }
	d->used = 1;
	pthread_mutex_unlock(&mx);
	WR32(ctrl + C_HANDLE, i + 1); WR32(ctrl + C_ES_MAX, d->es_max); WR32(ctrl + C_PCM_MAX, d->pcm_max);
	if (ad_log()) fprintf(stderr, "audiodec: decoder %d: AAC %u ch, %u Hz, adts %u, sbr %u, word length %u\n", i + 1, ch, rate, adts, sbr, RD32(ctrl + C_WORD));
	return 0;
}
#endif

static void create(Cpu *c, int external) {   /* External: the caller hands over the memory (GetContextSize) and needs no InitLibrary */
	uint32_t ctrl = A0, type = A1, e = ctrl_check(ctrl);
	if (e) { RET(e); return; }
#ifdef RECOMP_RAWDEC
	if (type == SCE_AUDIODEC_TYPE_AAC) {
		uint32_t info = RD32(ctrl + C_INFO);
		RET(info ? create_aac(ctrl, info, external) : SCE_AUDIODEC_ERROR_INVALID_PTR); return;
	}
#endif
	if (type != SCE_AUDIODEC_TYPE_AT9) {
		fprintf(stderr, "    audiodec: CreateDecoder for codec type %#x not implemented (ATRAC9%s) -- INVALID_TYPE\n", type, aac_supported() ? ", AAC" : " only");
		RET(SCE_AUDIODEC_ERROR_INVALID_TYPE); return;
	}
	uint32_t info = RD32(ctrl + C_INFO);
	if (!info) { RET(SCE_AUDIODEC_ERROR_INVALID_PTR); return; }
	pthread_mutex_lock(&mx);
	if (!external && !at9_initialised) { pthread_mutex_unlock(&mx); RET(SCE_AUDIODEC_ERROR_NOT_INITIALIZED); return; }
	int i = 0; while (i < MAX_DECODERS && decs[i].used) i++;
	if (i == MAX_DECODERS) { pthread_mutex_unlock(&mx); RET(SCE_AUDIODEC_ERROR_ALL_HANDLES_IN_USE); return; }
	Decoder *d = &decs[i];
	memset(d, 0, sizeof *d);
	d->type = SCE_AUDIODEC_TYPE_AT9;
	memcpy(d->config, G(info + I_CONFIG), 4);
	if (at9_open(d)) {
		pthread_mutex_unlock(&mx);
		fprintf(stderr, "    audiodec: ATRAC9 configuration %02x %02x %02x %02x rejected\n", d->config[0], d->config[1], d->config[2], d->config[3]);
		RET(SCE_AUDIODEC_AT9_ERROR_INVALID_CONFIG); return;
	}
	d->used = 1;
	Atrac9CodecInfo ci = d->ci;
	pthread_mutex_unlock(&mx);
	/* the bit rate: a superframe's bits, superframes per second */
	uint32_t bitrate = (uint32_t)((uint64_t)ci.superframeSize * 8u * (uint32_t)ci.samplingRate / ((uint32_t)ci.frameSamples * (uint32_t)ci.framesInSuperframe));
	WR32(info + I_CH, ci.channels); WR32(info + I_BITRATE, bitrate); WR32(info + I_RATE, ci.samplingRate);
	WR32(info + I_SF_SIZE, ci.superframeSize); WR32(info + I_SF_FRAMES, ci.framesInSuperframe);
	WR32(ctrl + C_HANDLE, i + 1);
	WR32(ctrl + C_ES_MAX, (uint32_t)ci.superframeSize < SCE_AUDIODEC_AT9_MAX_ES_SIZE ? (uint32_t)ci.superframeSize : SCE_AUDIODEC_AT9_MAX_ES_SIZE);
	WR32(ctrl + C_PCM_MAX, (uint32_t)(ci.frameSamples * ci.channels * 2));
	if (ad_log()) fprintf(stderr, "audiodec: decoder %d: %d ch, %d Hz, superframe %d bytes / %d frames of %d samples, word length %u\n",
		i + 1, ci.channels, ci.samplingRate, ci.superframeSize, ci.framesInSuperframe, ci.frameSamples, RD32(ctrl + C_WORD));
	RET(0);
}

static void h_delete(Cpu *c) {
	uint32_t ctrl = A0, e = ctrl_check(ctrl);
	if (e) { RET(e); return; }
	pthread_mutex_lock(&mx);
	Decoder *d = by_handle(ctrl);
	if (!d) { pthread_mutex_unlock(&mx); RET(SCE_AUDIODEC_ERROR_NOT_HANDLE_IN_USE); return; }
	dec_free(d);
	pthread_mutex_unlock(&mx);
	RET(0);
}

static void h_clear(Cpu *c) {
	uint32_t ctrl = A0, e = ctrl_check(ctrl);
	if (e) { RET(e); return; }
	if (!RD32(ctrl + C_HANDLE)) { RET(SCE_AUDIODEC_ERROR_INVALID_HANDLE); return; }
	pthread_mutex_lock(&mx);
	Decoder *d = by_handle(ctrl);
	if (!d) { pthread_mutex_unlock(&mx); RET(SCE_AUDIODEC_ERROR_NOT_HANDLE_IN_USE); return; }
	int bad = 0;
#ifdef RECOMP_RAWDEC
	if (d->type == SCE_AUDIODEC_TYPE_AAC) avcodec_flush_buffers(d->aac);   /* no overlap history */
	else
#endif
	{   /* a fresh decoder for the same configuration: no overlap history, at a superframe's start */
		Atrac9ReleaseHandle(d->at9); d->at9 = 0;
		bad = at9_open(d);
	}
	pthread_mutex_unlock(&mx);
	RET(bad ? SCE_AUDIODEC_ERROR_API_FAIL : 0);
}

#ifdef RECOMP_RAWDEC
static int16_t to_s16(float v) { v *= 32768.0f; return v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int16_t)v; }
/* nframes AAC frames from pEs: each one's bytes (up to maxEsSize) through the decoder's own callback,
 * which says how many it consumed; the PCM 16-bit interleaved (under mx) */
static uint32_t decode_aac(Decoder *d, uint32_t ctrl, uint32_t es, uint32_t pcm, uint32_t nframes) {
	uint32_t used_total = 0, given_total = 0, room = d->pcm_max * nframes;
	for (uint32_t f = 0; f < nframes; f++) {
		memcpy(d->in, G(es + used_total), d->es_max);
		memset(d->in + d->es_max, 0, AV_INPUT_BUFFER_PADDING_SIZE);
		d->pkt->data = d->in; d->pkt->size = (int)d->es_max;
		int got = 0, used = nativ_rawdec_frame(d->aac, d->frame, &got, d->pkt);
		if (used < 0) {
			static int said; if (said++ < 4) fprintf(stderr, "    audiodec: AAC frame rejected (%d) at es %#x\n", used, es + used_total);
			WR32(ctrl + C_ES_USED, used_total); WR32(ctrl + C_PCM_GIVEN, given_total);
			return SCE_AUDIODEC_ERROR_API_FAIL;
		}
		/* FFmpeg counts zero bytes after the frame as consumed too (it skips padding): a raw_data_block
		 * ends with ID_END's 1-bits, so the frame ends at its last nonzero byte */
		if ((uint32_t)used == d->es_max) while (used > 0 && !d->in[used - 1]) used--;
		used_total += (uint32_t)used;
		if (!got) continue;
		AVFrame *fr = d->frame;
		int ch = fr->ch_layout.nb_channels, n = fr->nb_samples;
		if (ch < 1) continue;
		if (given_total + (uint32_t)(n * ch * 2) > room) {
			static int said; if (!said++) fprintf(stderr, "    audiodec: AAC frame of %d x %d samples over maxPcmSize -- cut\n", n, ch);
			n = (int)((room - given_total) / (uint32_t)(2 * ch));
		}
		uint8_t *out = G(pcm + given_total);
		for (int s = 0; s < n; s++) for (int k = 0; k < ch; k++) {
			int16_t v;
			switch (fr->format) {
			case AV_SAMPLE_FMT_FLTP: v = to_s16(((const float *)fr->extended_data[k])[s]); break;
			case AV_SAMPLE_FMT_FLT: v = to_s16(((const float *)fr->data[0])[s * ch + k]); break;
			case AV_SAMPLE_FMT_S16P: v = ((const int16_t *)fr->extended_data[k])[s]; break;
			case AV_SAMPLE_FMT_S16: v = ((const int16_t *)fr->data[0])[s * ch + k]; break;
			default: v = 0; break;
			}
			memcpy(out + 2 * (s * ch + k), &v, 2);
		}
		given_total += (uint32_t)(n * ch * 2);
	}
	WR32(ctrl + C_ES_USED, used_total); WR32(ctrl + C_PCM_GIVEN, given_total);
	return 0;
}
#endif

/* nframes ATRAC9 frames from pEs, one after another, their PCM one after another at pPcm */
static uint32_t decode_frames(uint32_t ctrl, uint32_t nframes) {
	uint32_t e = ctrl_check(ctrl);
	if (e) return e;
	uint32_t es = RD32(ctrl + C_ES), pcm = RD32(ctrl + C_PCM);
	if (!es || !pcm) return SCE_AUDIODEC_ERROR_INVALID_PTR;
	pthread_mutex_lock(&mx);
	Decoder *d = by_handle(ctrl);
	if (!d) { pthread_mutex_unlock(&mx); return SCE_AUDIODEC_ERROR_NOT_HANDLE_IN_USE; }
#ifdef RECOMP_RAWDEC
	if (d->type == SCE_AUDIODEC_TYPE_AAC) { uint32_t r = decode_aac(d, ctrl, es, pcm, nframes); pthread_mutex_unlock(&mx); return r; }
#endif
	uint32_t used_total = 0, given_total = 0, frame_bytes = (uint32_t)(d->ci.frameSamples * d->ci.channels * 2);
	for (uint32_t f = 0; f < nframes; f++) {
		int used = 0;
		int r = Atrac9Decode(d->at9, G(es + used_total), d->pcm, &used);
		if (r) {
			pthread_mutex_unlock(&mx);
			static int said; if (said++ < 4) fprintf(stderr, "    audiodec: ATRAC9 frame rejected (%d) at es %#x\n", r, es + used_total);
			WR32(ctrl + C_ES_USED, used_total); WR32(ctrl + C_PCM_GIVEN, given_total);
			return SCE_AUDIODEC_ERROR_API_FAIL;
		}
		uint32_t take = (uint32_t)used;
		d->sf_left -= used;
		if (++d->sf_frame == d->ci.framesInSuperframe) {   /* the superframe's padding goes with its last frame */
			if (d->sf_left > 0) take += (uint32_t)d->sf_left;
			d->sf_frame = 0; d->sf_left = d->ci.superframeSize;
		}
		memcpy(G(pcm + given_total), d->pcm, frame_bytes);
		used_total += take; given_total += frame_bytes;
	}
	pthread_mutex_unlock(&mx);
	WR32(ctrl + C_ES_USED, used_total); WR32(ctrl + C_PCM_GIVEN, given_total);
	return 0;
}
static void h_decode(Cpu *c) { RET(decode_frames(A0, 1)); }
static void h_decode_n(Cpu *c) { RET(decode_frames(A0, A1)); }

static void h_internal_error(Cpu *c) {
	uint32_t ctrl = A0, out = A1, e = ctrl_check(ctrl);
	if (e) { RET(e); return; }
	if (!out) { RET(SCE_AUDIODEC_ERROR_INVALID_PTR); return; }
	WR32(out, 0);
	RET(0);
}

/* (ctrl, codec type): the memory a CreateDecoderExternal caller sets aside for the decoder */
static void h_context_size(Cpu *c) {
	uint32_t ctrl = A0, type = A1, e = ctrl_check(ctrl);
	if (e) { RET(e); return; }
	if (type == SCE_AUDIODEC_TYPE_AAC && aac_supported()) { RET(SCE_AUDIODEC_AAC_CONTEXT_SIZE); return; }
	fprintf(stderr, "    audiodec: GetContextSize for codec type %#x not implemented -- INVALID_TYPE\n", type);
	RET(SCE_AUDIODEC_ERROR_INVALID_TYPE);
}
/* (ctrl, type, context, size) / (ctrl, context): the host keeps the decoder's memory itself */
static void h_create(Cpu *c) { create(c, 0); }
static void h_create_external(Cpu *c) { create(c, 1); }
static void h_delete_external(Cpu *c) { h_delete(c); }

const HleEntry hle_audiodec[] = {
	{"sceAudiodecInitLibrary", h_init}, {"sceAudiodecTermLibrary", h_term},
	{"sceAudiodecCreateDecoder", h_create}, {"sceAudiodecDeleteDecoder", h_delete},
	{"sceAudiodecDecode", h_decode}, {"sceAudiodecDecodeNFrames", h_decode_n},
	{"sceAudiodecClearContext", h_clear}, {"sceAudiodecGetInternalError", h_internal_error},
	{"sceAudiodecGetContextSize", h_context_size},
	{"sceAudiodecCreateDecoderExternal", h_create_external}, {"sceAudiodecDeleteDecoderExternal", h_delete_external},
	{0, 0},
};
