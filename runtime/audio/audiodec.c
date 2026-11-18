/*
 * SceAudiodecUser: the codec engine's audio decoder. A game feeds it compressed frames it streams itself (music,
 * a movie's soundtrack) and plays the PCM it gets back through SceAudioOut. The runtime implements it at the
 * system-call boundary, as video.c does the video decoder: ATRAC9 through the LibAtrac9 already vendored for the
 * NGS player (third_party/libatrac9, MIT), AAC and MP3 through libavcodec where the build has it (RECOMP_AVCODEC
 * and the decoders compiled in: the toolchains' FFmpeg builds enable them). A codec the build lacks, and CELP,
 * fail with SCE_AUDIODEC_ERROR_INVALID_TYPE and a log line, so the game's own error path runs. (Before this file
 * every call fell to the not-implemented stub, which answers 0 with nothing written: a game reads that as
 * success and plays silence.)
 *
 * The contract (psp2/audiodec.h): SceAudiodecCtrl is 0x28 bytes {size, handle, pEs, inputEsSize, maxEsSize,
 * pPcm, outputPcmSize, maxPcmSize, wordLength, pInfo}. CreateDecoder reads the codec's SceAudiodecInfo (AT9:
 * configData; AAC: isAdts, ch, samplingRate, isSbr; MP3: ch, version), fills in the rest of it, and sets the
 * handle, maxEsSize and maxPcmSize. Decode takes ONE frame from pEs, writes 16-bit interleaved PCM to pPcm and
 * reports the bytes it took (inputEsSize) and wrote (outputPcmSize). ATRAC9 frames are variable length inside a
 * superframe, so a frame takes its own bytes and the superframe's last one also the padding after it. ADTS and
 * MP3 frames state their length in the header. A raw AAC frame does not, and libavcodec's public API does not
 * report how much of a packet the decoder used, so aacframe.h measures the frame from its own syntax: callers
 * don't say (Sony's AvPlayer leaves inputEsSize at the last call's answer and puts the next frame right after),
 * and a packet that runs past the frame is decoded by libavcodec as more frames. A frame aacframe.h can't
 * measure (a coupling element, another profile's tools) falls back to the caller's inputEsSize, then the buffer
 * up to its last nonzero byte. The host keeps the decoders' memory itself, so the *External calls are the plain
 * ones and GetContextSize only has to answer something the caller can allocate. RECOMP_AUDIODEC_LOG=1 logs the
 * lifecycle.
 */
#include "hle.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "third_party/libatrac9/libatrac9.h"
#include "aacframe.h"
#ifdef RECOMP_AVCODEC
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#endif

#define AD_ERR_API_FAIL 0x807F0000u
#define AD_ERR_INVALID_TYPE 0x807F0001u
#define AD_ERR_INVALID_INIT_PARAM 0x807F0002u
#define AD_ERR_ALREADY_INITIALIZED 0x807F0003u
#define AD_ERR_NOT_INITIALIZED 0x807F0005u
#define AD_ERR_A_HANDLE_IN_USE 0x807F0006u
#define AD_ERR_ALL_HANDLES_IN_USE 0x807F0007u
#define AD_ERR_INVALID_PTR 0x807F0008u
#define AD_ERR_NOT_HANDLE_IN_USE 0x807F000Au
#define AD_ERR_CH_SHORTAGE 0x807F000Bu
#define AD_ERR_INVALID_WORD_LENGTH 0x807F000Cu
#define AD_ERR_INVALID_SIZE 0x807F000Du
#define AD_ERR_INVALID_NFRAMES 0x807F000Fu
#define AD_ERR_INVALID_NSTREAMS 0x807F0010u
#define AD_ERR_AT9_INVALID_CONFIG 0x807F2000u
#define AD_ERR_MP3_INVALID_CH 0x807F2800u
#define AD_ERR_MP3_INVALID_MPEG_VERSION 0x807F2801u
#define AD_ERR_AAC_INVALID_CH 0x807F3000u
enum { T_AT9 = 0x1003, T_MP3 = 0x1004, T_AAC = 0x1005, T_CELP = 0x1006 };
enum { AT9_ES_MAX = 1024, MP3_ES_MAX = 1441, AAC_ES_MAX = 1536, AT9_NFRAMES = 8, AT9_MAX_CH = 2, MAX_STREAMS = 6 };

/* SceAudiodecCtrl */
enum { C_SIZE = 0, C_HANDLE = 4, C_ES = 8, C_ES_USED = 12, C_ES_MAX = 16, C_PCM = 20, C_PCM_GIVEN = 24, C_PCM_MAX = 28, C_WORD = 32, C_INFO = 36, CTRL_SIZE = 0x28 };
/* SceAudiodecInfo: the codec's struct after the size word */
enum { I_AT9_CONFIG = 4, I_AT9_CH = 8, I_AT9_BITRATE = 12, I_AT9_RATE = 16, I_AT9_SF_SIZE = 20, I_AT9_SF_FRAMES = 24, INFO_AT9 = 0x1C,
       I_MP3_CH = 4, I_MP3_VERSION = 8, INFO_MP3 = 0xC,
       I_AAC_ADTS = 4, I_AAC_CH = 8, I_AAC_RATE = 12, I_AAC_SBR = 16, INFO_AAC = 0x14 };

#ifdef RECOMP_AVCODEC
static int avc_has(uint32_t type) {   /* the build's libavcodec has this decoder */
	return avcodec_find_decoder(type == T_AAC ? AV_CODEC_ID_AAC : AV_CODEC_ID_MP3) != 0;
}
#else
static int avc_has(uint32_t type) { (void)type; return 0; }
#endif
static int type_ok(uint32_t type) { return type == T_AT9 || ((type == T_AAC || type == T_MP3) && avc_has(type)); }
static const char *type_name(uint32_t type) { return type == T_AT9 ? "ATRAC9" : type == T_AAC ? "AAC" : type == T_MP3 ? "MP3" : type == T_CELP ? "CELP" : "unknown"; }

#define MAX_DECODERS 32
typedef struct {
	int used; uint32_t type;
	uint32_t es_max, pcm_max, pcm_cap, ch, adts;   /* pcm_max is what the API reports, pcm_cap what a frame may write (implicit SBR doubles AAC) */
	int sfi;                    /* raw AAC: the sampling-frequency index, for aacframe.h */
	int32_t last_err;
	void *at9; unsigned char config[4]; Atrac9CodecInfo ci;
	int sf_frame, sf_left;      /* the frame within the superframe; its bytes not yet taken */
	short *pcm;                 /* one ATRAC9 frame, at most 2048 samples x 2 channels */
#ifdef RECOMP_AVCODEC
	AVCodecContext *av; AVFrame *frm; AVPacket *pkt;
	uint8_t *in;                /* the frame with libavcodec's zero padding after it */
#endif
} Dec;
static Dec decs[MAX_DECODERS];
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
/* what InitLibrary set aside per codec: the channels (ATRAC9) or streams (AAC, MP3) the library may serve */
static struct { int on; uint32_t cap; } lib[3];   /* T_AT9, T_MP3, T_AAC */
static int lib_ix(uint32_t type) { return type == T_AT9 ? 0 : type == T_MP3 ? 1 : 2; }

static int ad_log(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_AUDIODEC_LOG") != 0; return v; }

static void dec_free(Dec *d) {   /* (under mx) */
	if (d->at9) Atrac9ReleaseHandle(d->at9);
	free(d->pcm);
#ifdef RECOMP_AVCODEC
	avcodec_free_context(&d->av); av_frame_free(&d->frm); av_packet_free(&d->pkt); free(d->in);
#endif
	memset(d, 0, sizeof *d);
}
static uint32_t units_used(uint32_t type) {   /* channels or streams in use for a codec (under mx) */
	uint32_t n = 0;
	for (int i = 0; i < MAX_DECODERS; i++) if (decs[i].used && decs[i].type == type) n += type == T_AT9 ? decs[i].ch : 1;
	return n;
}

static void h_init(Cpu *c) {   /* (codec type, SceAudiodecInitParam *): {size, totalCh | totalStreams} */
	uint32_t type = A0, p = A1;
	if (!type_ok(type)) {
		fprintf(stderr, "    audiodec: InitLibrary for %s (%#x) not available in this build -- INVALID_TYPE\n", type_name(type), type);
		RET(AD_ERR_INVALID_TYPE); return;
	}
	if (!p) { RET(AD_ERR_INVALID_PTR); return; }
	uint32_t n = RD32(p + 4), max = type == T_AT9 ? 16 : 8;
	if (!n || n > max) { RET(AD_ERR_INVALID_INIT_PARAM); return; }
	pthread_mutex_lock(&mx);
	int k = lib_ix(type), already = lib[k].on;
	if (!already) { lib[k].on = 1; lib[k].cap = n; }
	pthread_mutex_unlock(&mx);
	if (ad_log()) fprintf(stderr, "audiodec: InitLibrary %s, %u %s\n", type_name(type), n, type == T_AT9 ? "channels" : "streams");
	RET(already ? AD_ERR_ALREADY_INITIALIZED : 0);
}
static void h_term(Cpu *c) {
	uint32_t type = A0;
	if (type != T_AT9 && type != T_MP3 && type != T_AAC) { RET(AD_ERR_INVALID_TYPE); return; }
	pthread_mutex_lock(&mx);
	int k = lib_ix(type);
	uint32_t r = !lib[k].on ? AD_ERR_NOT_INITIALIZED : units_used(type) ? AD_ERR_A_HANDLE_IN_USE : 0;
	if (!r) lib[k].on = 0;
	pthread_mutex_unlock(&mx);
	RET(r);
}

static uint32_t ctrl_check(uint32_t ctrl) {   /* what every call checks first: a pointer, the right size */
	if (!ctrl) return AD_ERR_INVALID_PTR;
	return RD32(ctrl + C_SIZE) == CTRL_SIZE ? 0 : AD_ERR_INVALID_SIZE;
}
static Dec *by_handle(uint32_t ctrl) {   /* (under mx) */
	uint32_t h = RD32(ctrl + C_HANDLE);
	return h >= 1 && h <= MAX_DECODERS && decs[h - 1].used ? &decs[h - 1] : 0;
}

#ifdef RECOMP_AVCODEC
/* the decoder; AAC gets an AudioSpecificConfig for AAC-LC at this rate and channel count, since raw frames carry
 * none and libavcodec needs it (an ADTS header states all of it itself; implicit SBR is found by the decoder) */
static int av_create(Dec *d, uint32_t type, uint32_t ch, uint32_t rate, uint32_t adts) {
	static const uint32_t rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
	const AVCodec *codec = avcodec_find_decoder(type == T_AAC ? AV_CODEC_ID_AAC : AV_CODEC_ID_MP3);
	int sr = -1;
	for (int i = 0; i < 13; i++) if (rates[i] == rate) sr = i;
	if (type == T_AAC && !adts && sr < 0) return -1;
	d->sfi = sr;
	d->av = codec ? avcodec_alloc_context3(codec) : 0;
	d->frm = av_frame_alloc(); d->pkt = av_packet_alloc(); d->in = calloc(1, d->es_max + AV_INPUT_BUFFER_PADDING_SIZE);
	if (!d->av || !d->frm || !d->pkt || !d->in) return -1;
	if (type == T_AAC && !adts) {
		d->av->extradata = av_mallocz(2 + AV_INPUT_BUFFER_PADDING_SIZE);
		if (!d->av->extradata) return -1;
		d->av->extradata[0] = (uint8_t)(2 << 3 | sr >> 1); d->av->extradata[1] = (uint8_t)((sr & 1) << 7 | ch << 3);
		d->av->extradata_size = 2;
	}
	return avcodec_open2(d->av, codec, 0) < 0 ? -1 : 0;
}
#endif

static int at9_open(Dec *d) {
	d->at9 = Atrac9GetHandle();
	if (!d->at9) return -1;
	if (Atrac9InitDecoder(d->at9, d->config)) { Atrac9ReleaseHandle(d->at9); d->at9 = 0; return -1; }
	Atrac9GetCodecInfo(d->at9, &d->ci);
	d->sf_frame = 0; d->sf_left = d->ci.superframeSize;
	return 0;
}

static void create(Cpu *c, int external) {   /* External: the caller hands over the memory and needs no InitLibrary */
	uint32_t ctrl = A0, type = A1, e = ctrl_check(ctrl);
	if (e) { RET(e); return; }
	if (!type_ok(type)) {
		fprintf(stderr, "    audiodec: CreateDecoder for %s (%#x) not available in this build -- INVALID_TYPE\n", type_name(type), type);
		RET(AD_ERR_INVALID_TYPE); return;
	}
	uint32_t info = RD32(ctrl + C_INFO);
	if (!info) { RET(AD_ERR_INVALID_PTR); return; }
	if (RD32(ctrl + C_WORD) != 16) { RET(AD_ERR_INVALID_WORD_LENGTH); return; }
	if (RD32(info) != (type == T_AT9 ? INFO_AT9 : type == T_AAC ? INFO_AAC : INFO_MP3)) { RET(AD_ERR_INVALID_SIZE); return; }
	/* the codec's parameters, checked before a slot is taken */
	uint32_t ch = 0, rate = 0, adts = 0, sbr = 0, ver = 0;
	if (type == T_AAC) {
		adts = RD32(info + I_AAC_ADTS); ch = RD32(info + I_AAC_CH); rate = RD32(info + I_AAC_RATE); sbr = RD32(info + I_AAC_SBR);
		if (ch < 1 || ch > 2) { RET(AD_ERR_AAC_INVALID_CH); return; }
	} else if (type == T_MP3) {
		ch = RD32(info + I_MP3_CH); ver = RD32(info + I_MP3_VERSION);
		if (ch < 1 || ch > 2) { RET(AD_ERR_MP3_INVALID_CH); return; }
		if (ver != 0 && ver != 2 && ver != 3) { RET(AD_ERR_MP3_INVALID_MPEG_VERSION); return; }
	}
	pthread_mutex_lock(&mx);
	int k = lib_ix(type);
	if (!external && !lib[k].on) { pthread_mutex_unlock(&mx); RET(AD_ERR_NOT_INITIALIZED); return; }
	int i = 0; while (i < MAX_DECODERS && decs[i].used) i++;
	if (i == MAX_DECODERS) { pthread_mutex_unlock(&mx); RET(AD_ERR_ALL_HANDLES_IN_USE); return; }
	Dec *d = &decs[i];
	memset(d, 0, sizeof *d);
	d->type = type;
	if (type == T_AT9) {
		memcpy(d->config, G(info + I_AT9_CONFIG), 4);
		if (at9_open(d) || d->ci.channels > AT9_MAX_CH || d->ci.frameSamples > 2048) {
			fprintf(stderr, "    audiodec: ATRAC9 configuration %02x %02x %02x %02x rejected\n", d->config[0], d->config[1], d->config[2], d->config[3]);
			dec_free(d); pthread_mutex_unlock(&mx); RET(AD_ERR_AT9_INVALID_CONFIG); return;
		}
		ch = (uint32_t)d->ci.channels;
		d->pcm = malloc(2048 * AT9_MAX_CH * sizeof(short));
		if (!d->pcm) { dec_free(d); pthread_mutex_unlock(&mx); RET(AD_ERR_API_FAIL); return; }
		d->es_max = (uint32_t)d->ci.superframeSize < AT9_ES_MAX ? (uint32_t)d->ci.superframeSize : AT9_ES_MAX;
		d->pcm_max = d->pcm_cap = (uint32_t)d->ci.frameSamples * ch * 2;
	} else if (type == T_AAC) {   /* 1024 samples a frame, 2048 with SBR */
		d->es_max = AAC_ES_MAX + (adts ? 0x100 : 0);
		d->pcm_max = ch * 2048 * (sbr ? 2 : 1); d->pcm_cap = ch * 4096;
	} else {
		d->es_max = MP3_ES_MAX; d->pcm_max = d->pcm_cap = ch * 1152 * 2;
	}
	d->ch = ch; d->adts = adts;
	if (!external && lib[k].cap && units_used(type) + (type == T_AT9 ? ch : 1) > lib[k].cap) { dec_free(d); pthread_mutex_unlock(&mx); RET(AD_ERR_CH_SHORTAGE); return; }
#ifdef RECOMP_AVCODEC
	if (type != T_AT9 && av_create(d, type, ch, rate, adts)) {
		dec_free(d); pthread_mutex_unlock(&mx);
		fprintf(stderr, "    audiodec: %s decoder could not be opened (%u ch, %u Hz)\n", type_name(type), ch, rate);
		RET(AD_ERR_API_FAIL); return;
	}
#endif
	d->used = 1;
	Atrac9CodecInfo ci = d->ci;
	uint32_t es_max = d->es_max, pcm_max = d->pcm_max;
	pthread_mutex_unlock(&mx);
	if (type == T_AT9) {   /* the bit rate: a superframe's bits, superframes per second */
		uint32_t bps = (uint32_t)((uint64_t)ci.superframeSize * 8u * (uint32_t)ci.samplingRate / ((uint32_t)ci.frameSamples * (uint32_t)ci.framesInSuperframe));
		WR32(info + I_AT9_CH, ci.channels); WR32(info + I_AT9_BITRATE, bps); WR32(info + I_AT9_RATE, ci.samplingRate);
		WR32(info + I_AT9_SF_SIZE, ci.superframeSize); WR32(info + I_AT9_SF_FRAMES, ci.framesInSuperframe);
	}
	WR32(ctrl + C_HANDLE, i + 1); WR32(ctrl + C_ES_MAX, es_max); WR32(ctrl + C_PCM_MAX, pcm_max);
	if (ad_log()) fprintf(stderr, "audiodec: decoder %d: %s, %u ch, %u Hz, adts %u, sbr %u, max es %u, max pcm %u\n", i + 1, type_name(type), ch, type == T_AT9 ? (uint32_t)ci.samplingRate : rate, adts, sbr, es_max, pcm_max);
	RET(0);
}
static void h_create(Cpu *c) { create(c, 0); }
static void h_create_external(Cpu *c) { create(c, 1); }   /* (ctrl, type, context, size) */

static void h_delete(Cpu *c) {   /* also DeleteDecoderExternal (ctrl, &context) */
	uint32_t ctrl = A0, e = ctrl_check(ctrl);
	if (e) { RET(e); return; }
	pthread_mutex_lock(&mx);
	Dec *d = by_handle(ctrl);
	if (d) { dec_free(d); WR32(ctrl + C_HANDLE, 0); }
	pthread_mutex_unlock(&mx);
	RET(d ? 0 : AD_ERR_NOT_HANDLE_IN_USE);
}

static void h_clear(Cpu *c) {   /* a seek or a loop: no overlap history, the next frame starts a superframe */
	uint32_t ctrl = A0, e = ctrl_check(ctrl);
	if (e) { RET(e); return; }
	pthread_mutex_lock(&mx);
	Dec *d = by_handle(ctrl);
	int bad = 0;
	if (d && d->type == T_AT9) { Atrac9ReleaseHandle(d->at9); d->at9 = 0; bad = at9_open(d); }
#ifdef RECOMP_AVCODEC
	else if (d) avcodec_flush_buffers(d->av);
#endif
	pthread_mutex_unlock(&mx);
	RET(!d ? AD_ERR_NOT_HANDLE_IN_USE : bad ? AD_ERR_API_FAIL : 0);
}

static void h_internal_error(Cpu *c) {   /* (ctrl, int32_t *): what the last failed decode said */
	uint32_t ctrl = A0, out = A1, e = ctrl_check(ctrl);
	if (e) { RET(e); return; }
	if (!out) { RET(AD_ERR_INVALID_PTR); return; }
	pthread_mutex_lock(&mx);
	Dec *d = by_handle(ctrl);
	if (d) WR32(out, (uint32_t)d->last_err);
	pthread_mutex_unlock(&mx);
	RET(d ? 0 : AD_ERR_NOT_HANDLE_IN_USE);
}
static void h_context_size(Cpu *c) {   /* (ctrl, type): what a CreateDecoderExternal caller sets aside; the host keeps its own, so only the size matters */
	uint32_t e = ctrl_check(A0), type = A1;
	if (e) { RET(e); return; }
	RET(type_ok(type) ? (type == T_AAC ? 0x18000 : type == T_MP3 ? 0x10000 : 0x8000) : AD_ERR_INVALID_TYPE);
}

#ifdef RECOMP_AVCODEC
static int16_t to_s16(float v) { v *= 32768.0f; return v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int16_t)v; }
static const int mp3_kbps[2][15] = {   /* layer III: MPEG-1, then MPEG-2 and 2.5 */
	{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}, {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}};
static const int mp3_hz[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
/* an MP3 frame's length from its header (0: not a layer III header) */
static uint32_t mp3_len(const uint8_t *p) {
	if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0 || (p[1] >> 1 & 3) != 1) return 0;
	int v = p[1] >> 3 & 3, br = p[2] >> 4, sr = p[2] >> 2 & 3, pad = p[2] >> 1 & 1;
	if (v == 1 || br == 0 || br == 15 || sr == 3) return 0;
	int mpeg1 = v == 3, hz = mp3_hz[mpeg1 ? 0 : v == 2 ? 1 : 2][sr];
	return (uint32_t)((mpeg1 ? 144 : 72) * mp3_kbps[!mpeg1][br] * 1000 / hz + pad);
}
/* the frame's byte count: ADTS and MP3 headers say it; a raw AAC frame's is read from its syntax (aacframe.h),
 * else it's what the caller set in inputEsSize, else the buffer up to its last nonzero byte (a raw_data_block ends
 * in ID_END's 1-bits) */
static uint32_t frame_len(Dec *d, const uint8_t *es, uint32_t hint) {
	if (d->type == T_MP3) return mp3_len(es);
	if (d->adts) return es[0] == 0xFF && (es[1] & 0xF0) == 0xF0 ? (uint32_t)(es[3] & 3) << 11 | (uint32_t)es[4] << 3 | es[5] >> 5 : 0;
	int m = aac_raw_frame_len(es, d->es_max, d->sfi);
	if (m > 0) return (uint32_t)m;
	if (ad_log()) { static int said; if (said++ < 4) fprintf(stderr, "audiodec: a raw AAC frame aacframe.h can't measure; using %s\n", hint ? "inputEsSize" : "the buffer's last nonzero byte"); }
	if (hint && hint <= d->es_max) return hint;
	uint32_t n = d->es_max; while (n && !es[n - 1]) n--;
	return n;
}
/* one frame through libavcodec: the PCM 16-bit interleaved at pcm (under mx) */
static uint32_t av_decode(Dec *d, uint32_t hint, uint32_t es, uint32_t pcm, uint32_t *used, uint32_t *given) {
	uint32_t n = frame_len(d, G(es), hint);
	if (!n || n > d->es_max) { d->last_err = -1; return AD_ERR_API_FAIL; }
	memcpy(d->in, G(es), n); memset(d->in + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
	d->pkt->data = d->in; d->pkt->size = (int)n;
	*used = n; *given = 0;
	int r = avcodec_send_packet(d->av, d->pkt);
	if (r < 0) { d->last_err = r; static int said; if (said++ < 4) fprintf(stderr, "    audiodec: %s frame rejected (%d)\n", type_name(d->type), r); return AD_ERR_API_FAIL; }
	while (avcodec_receive_frame(d->av, d->frm) >= 0) {   /* the packet's first frame; a raw packet that ran past its frame can yield more, dropped */
		AVFrame *f = d->frm;
		int ch = f->ch_layout.nb_channels, ns = f->nb_samples;
		if (!*given && ch >= 1) {
			if ((uint32_t)(ns * ch * 2) > d->pcm_cap) ns = (int)(d->pcm_cap / (uint32_t)(2 * ch));
			uint8_t *out = G(pcm);
			for (int s = 0; s < ns; s++) for (int k = 0; k < ch; k++) {
				int16_t v;
				switch (f->format) {
				case AV_SAMPLE_FMT_FLTP: v = to_s16(((const float *)f->extended_data[k])[s]); break;
				case AV_SAMPLE_FMT_FLT: v = to_s16(((const float *)f->data[0])[s * ch + k]); break;
				case AV_SAMPLE_FMT_S16P: v = ((const int16_t *)f->extended_data[k])[s]; break;
				case AV_SAMPLE_FMT_S16: v = ((const int16_t *)f->data[0])[s * ch + k]; break;
				default: v = 0; break;
				}
				memcpy(out + 2 * (s * ch + k), &v, 2);
			}
			*given = (uint32_t)(ns * ch * 2);
		}
		av_frame_unref(f);
	}
	return 0;
}
#endif

/* one ATRAC9 frame from es (under mx) */
static uint32_t at9_decode(Dec *d, uint32_t es, uint32_t pcm, uint32_t *used, uint32_t *given) {
	int u = 0, r = Atrac9Decode(d->at9, G(es), d->pcm, &u);
	if (r) { d->last_err = r; static int said; if (said++ < 4) fprintf(stderr, "    audiodec: ATRAC9 frame rejected (%d) at es %#x\n", r, es); return AD_ERR_API_FAIL; }
	uint32_t take = (uint32_t)u;
	d->sf_left -= u;
	if (++d->sf_frame == d->ci.framesInSuperframe) {   /* the superframe's padding goes with its last frame */
		if (d->sf_left > 0) take += (uint32_t)d->sf_left;
		d->sf_frame = 0; d->sf_left = d->ci.superframeSize;
	}
	*given = (uint32_t)(d->ci.frameSamples * d->ci.channels * 2);
	memcpy(G(pcm), d->pcm, *given);
	*used = take;
	return 0;
}

/* nframes frames one after another from pEs to pPcm; the totals are reported in the ctrl */
static uint32_t decode_ctrl(uint32_t ctrl, uint32_t nframes) {
	uint32_t e = ctrl_check(ctrl);
	if (e) return e;
	uint32_t es = RD32(ctrl + C_ES), pcm = RD32(ctrl + C_PCM), hint = RD32(ctrl + C_ES_USED);
	if (!es || !pcm) return AD_ERR_INVALID_PTR;
	pthread_mutex_lock(&mx);
	Dec *d = by_handle(ctrl);
	if (!d) { pthread_mutex_unlock(&mx); return AD_ERR_NOT_HANDLE_IN_USE; }
	if (nframes < 1 || nframes > (d->type == T_AT9 ? AT9_NFRAMES : 1)) { pthread_mutex_unlock(&mx); return AD_ERR_INVALID_NFRAMES; }
	uint32_t used_total = 0, given_total = 0, r = 0;
	for (uint32_t f = 0; f < nframes && !r; f++) {
		uint32_t used = 0, given = 0;
#ifdef RECOMP_AVCODEC
		if (d->type != T_AT9) r = av_decode(d, hint, es + used_total, pcm + given_total, &used, &given);
		else
#endif
		r = at9_decode(d, es + used_total, pcm + given_total, &used, &given);
		if (!r) { used_total += used; given_total += given; }
	}
	pthread_mutex_unlock(&mx);
	WR32(ctrl + C_ES_USED, used_total); WR32(ctrl + C_PCM_GIVEN, given_total);
	return r;
}
static void h_decode(Cpu *c) { RET(decode_ctrl(A0, 1)); }
static void h_decode_n(Cpu *c) { RET(decode_ctrl(A0, A1)); }   /* (ctrl, nFrames) */
static void h_decode_streams(Cpu *c) {   /* (SceAudiodecCtrl *ctrls[], nStreams): one frame for each */
	uint32_t arr = A0, n = A1;
	if (!arr) { RET(AD_ERR_INVALID_PTR); return; }
	if (n < 1 || n > MAX_STREAMS) { RET(AD_ERR_INVALID_NSTREAMS); return; }
	uint32_t r = 0;
	for (uint32_t i = 0; i < n && !r; i++) r = decode_ctrl(RD32(arr + 4 * i), 1);
	RET(r);
}

const HleEntry hle_audiodec[] = {
	{"sceAudiodecInitLibrary", h_init}, {"sceAudiodecTermLibrary", h_term},
	{"sceAudiodecCreateDecoder", h_create}, {"sceAudiodecCreateDecoderExternal", h_create_external},
	{"sceAudiodecDeleteDecoder", h_delete}, {"sceAudiodecDeleteDecoderExternal", h_delete},
	{"sceAudiodecDecode", h_decode}, {"sceAudiodecDecodeNFrames", h_decode_n}, {"sceAudiodecDecodeNStreams", h_decode_streams},
	{"sceAudiodecClearContext", h_clear}, {"sceAudiodecGetInternalError", h_internal_error}, {"sceAudiodecGetContextSize", h_context_size},
	{0, 0},
};
