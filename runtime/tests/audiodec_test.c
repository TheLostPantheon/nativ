/*
 * runtime/audio/audiodec.c (SceAudiodecUser) driven the way a game drives it: SceAudiodecCtrl and SceAudiodecInfo
 * in guest memory, one Decode per frame. Needs no game and no firmware.
 *
 *  - API paths (always): InitLibrary / TermLibrary / CreateDecoder / DeleteDecoder / ClearContext errors for bad
 *    parameters, the ATRAC9 info the decoder fills in from a configuration word, and a hand-built silent ATRAC9
 *    frame run through Decode (the frame's bytes and PCM size, the superframe's padding on its last frame).
 *    LibAtrac9 ships no encoder or test stream, so ATRAC9 has no reference-audio check.
 *  - AAC and MP3 (only with -DRECOMP_AVCODEC and libavcodec's headers and libraries; without them the test says
 *    so and checks that those types fail with INVALID_TYPE): tone.aac and tone.mp3, 0.14 s sine tones made with
 *    the ffmpeg command line (-c:a aac, libmp3lame), are decoded through Decode frame by frame (AAC as ADTS and
 *    as raw frames) and the PCM compared with libavcodec decoding the same file directly.
 *
 *     cc -O1 -g -w -I ../core -I ../../recompiler -I ../.. audiodec_test.c ../../third_party/libatrac9/*.c -lm -lpthread \
 *        -o /tmp/audiodec_test [-DRECOMP_AVCODEC $(pkg-config --cflags --libs libavcodec libavutil)] && /tmp/audiodec_test
 */
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include "../audio/audiodec.c"

uint8_t *recomp_mem;

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); fails += !ok; }
static uint32_t call(const char *name, uint32_t a0, uint32_t a1) {
	Cpu c = {0}; c.r[0] = a0; c.r[1] = a1;
	for (const HleEntry *e = hle_audiodec; e->name; e++) if (!strcmp(e->name, name)) { e->fn(&c); return c.r[0]; }
	printf("FAIL no entry %s\n", name); fails++; return 0xffffffffu;
}
enum { CTRL = 0x1000, INFO = 0x1100, PARAM = 0x1200, ES = 0x4000, PCM = 0x20000, CTRL2 = 0x1300 };
static void ctrl_setup(uint32_t ctrl, uint32_t es, uint32_t pcm) {
	memset(G(ctrl), 0, CTRL_SIZE);
	WR32(ctrl + C_SIZE, CTRL_SIZE); WR32(ctrl + C_ES, es); WR32(ctrl + C_PCM, pcm); WR32(ctrl + C_WORD, 16); WR32(ctrl + C_INFO, INFO);
}
static void put(uint8_t *buf, int *pos, uint32_t v, int bits) {   /* MSB-first bit writer */
	for (int i = bits - 1; i >= 0; i--, (*pos)++) if (v >> i & 1) buf[*pos >> 3] |= 0x80 >> (*pos & 7);
}

static void test_api_and_at9(void) {
	WR32(PARAM, 8); WR32(PARAM + 4, 2);
	ctrl_setup(CTRL, ES, PCM);
	memset(G(INFO), 0, 0x20); WR32(INFO, INFO_AT9); memcpy(G(INFO + I_AT9_CONFIG), "\xFE\x70\x1F\xF0", 4);   /* mono, 48 kHz, 256-byte frames, 4 per superframe */

	check(call("sceAudiodecCreateDecoder", CTRL, T_AT9) == AD_ERR_NOT_INITIALIZED, "CreateDecoder before InitLibrary: NOT_INITIALIZED");
	check(call("sceAudiodecTermLibrary", T_AT9, 0) == AD_ERR_NOT_INITIALIZED, "TermLibrary before InitLibrary: NOT_INITIALIZED");
	check(call("sceAudiodecInitLibrary", T_CELP, PARAM) == AD_ERR_INVALID_TYPE, "InitLibrary(CELP): INVALID_TYPE");
	check(call("sceAudiodecInitLibrary", T_AT9, 0) == AD_ERR_INVALID_PTR, "InitLibrary without a parameter: INVALID_PTR");
	WR32(PARAM + 4, 0);
	check(call("sceAudiodecInitLibrary", T_AT9, PARAM) == AD_ERR_INVALID_INIT_PARAM, "InitLibrary with 0 channels: INVALID_INIT_PARAM");
	WR32(PARAM + 4, 2);
	check(call("sceAudiodecInitLibrary", T_AT9, PARAM) == 0, "InitLibrary(AT9, 2 channels)");
	check(call("sceAudiodecInitLibrary", T_AT9, PARAM) == AD_ERR_ALREADY_INITIALIZED, "InitLibrary again: ALREADY_INITIALIZED");

	WR32(CTRL + C_SIZE, 0x24);
	check(call("sceAudiodecCreateDecoder", CTRL, T_AT9) == AD_ERR_INVALID_SIZE, "CreateDecoder with a short ctrl: INVALID_SIZE");
	WR32(CTRL + C_SIZE, CTRL_SIZE); WR32(CTRL + C_WORD, 24);
	check(call("sceAudiodecCreateDecoder", CTRL, T_AT9) == AD_ERR_INVALID_WORD_LENGTH, "CreateDecoder with 24-bit words: INVALID_WORD_LENGTH");
	WR32(CTRL + C_WORD, 16); WR32(INFO, 0x10);
	check(call("sceAudiodecCreateDecoder", CTRL, T_AT9) == AD_ERR_INVALID_SIZE, "CreateDecoder with a wrong info size: INVALID_SIZE");
	WR32(INFO, INFO_AT9); memcpy(G(INFO + I_AT9_CONFIG), "\x00\x00\x00\x00", 4);
	check(call("sceAudiodecCreateDecoder", CTRL, T_AT9) == AD_ERR_AT9_INVALID_CONFIG, "CreateDecoder with a bad configuration word: AT9 INVALID_CONFIG");
	check(call("sceAudiodecCreateDecoder", 0, T_AT9) == AD_ERR_INVALID_PTR, "CreateDecoder(NULL): INVALID_PTR");
	memcpy(G(INFO + I_AT9_CONFIG), "\xFE\x70\x1F\xF0", 4);
	check(call("sceAudiodecCreateDecoder", CTRL, T_AT9) == 0, "CreateDecoder(AT9)");
	check(RD32(CTRL + C_HANDLE) != 0, "  handle set");
	check(RD32(INFO + I_AT9_CH) == 1 && RD32(INFO + I_AT9_RATE) == 48000, "  info: 1 channel, 48000 Hz");
	check(RD32(INFO + I_AT9_SF_SIZE) == 1024 && RD32(INFO + I_AT9_SF_FRAMES) == 4, "  info: superframe 1024 bytes of 4 frames");
	check(RD32(INFO + I_AT9_BITRATE) == 384000, "  info: bit rate 384000");
	check(RD32(CTRL + C_ES_MAX) == 1024 && RD32(CTRL + C_PCM_MAX) == 512, "  ctrl: maxEsSize 1024, maxPcmSize 512");
	check(call("sceAudiodecTermLibrary", T_AT9, 0) == AD_ERR_A_HANDLE_IN_USE, "TermLibrary with a decoder alive: A_HANDLE_IN_USE");

	/* a second decoder, stereo: the 2-channel budget is spent (1 + 2 > 2) */
	ctrl_setup(CTRL2, ES, PCM); memcpy(G(INFO + I_AT9_CONFIG), "\xFE\x72\x1F\xF0", 4);
	check(call("sceAudiodecCreateDecoder", CTRL2, T_AT9) == AD_ERR_CH_SHORTAGE, "a stereo decoder over the channel budget: CH_SHORTAGE");
	check(call("sceAudiodecCreateDecoderExternal", CTRL2, T_AT9) == 0, "CreateDecoderExternal needs no budget");
	check(RD32(INFO + I_AT9_CH) == 2, "  stereo info: 2 channels");
	check(call("sceAudiodecDeleteDecoderExternal", CTRL2, 0) == 0, "DeleteDecoderExternal");
	check(call("sceAudiodecGetContextSize", CTRL2, T_AT9) > 0, "GetContextSize(AT9) answers a size");

	/* the smallest valid mono frame, as LibAtrac9 unpacks it: a first-in-superframe flag (set on frames 2-4),
	 * no bands extension, a zero gradient (35 bits), then fixed-length scale factors of 0 and zero spectra: silence.
	 * Frames are variable length and packed one after another, each written where the last one ended. */
	memset(G(ES), 0, 1024);
	uint32_t es_off = 0, pcm_off = 0, rc = 0, first = 0, last = 0;
	check(call("sceAudiodecDeleteDecoder", CTRL, 0) == 0, "DeleteDecoder (the first one)");
	ctrl_setup(CTRL, ES, PCM); memcpy(G(INFO + I_AT9_CONFIG), "\xFE\x70\x1F\xF0", 4);
	check(call("sceAudiodecDeleteDecoder", CTRL, 0) == AD_ERR_NOT_HANDLE_IN_USE, "DeleteDecoder with no handle: NOT_HANDLE_IN_USE");
	call("sceAudiodecCreateDecoder", CTRL, T_AT9);
	uint32_t h = RD32(CTRL + C_HANDLE);
	for (int i = 0; i < 4; i++) {
		uint8_t frame[64] = {0};
		int p = 0; put(frame, &p, i ? 1 : 0, 1); p = 36; put(frame, &p, 1, 2);
		memcpy(G(ES + es_off), frame, sizeof frame);
		WR32(CTRL + C_ES, ES + es_off); WR32(CTRL + C_PCM, PCM + pcm_off);
		memset(G(PCM + pcm_off), 0x5a, 512);
		rc |= call("sceAudiodecDecode", CTRL, 0);
		if (!i) first = RD32(CTRL + C_ES_USED);
		last = RD32(CTRL + C_ES_USED);
		check(RD32(CTRL + C_PCM_GIVEN) == 512, "  a frame writes 256 samples x 1 ch x 2 bytes");
		es_off += RD32(CTRL + C_ES_USED); pcm_off += RD32(CTRL + C_PCM_GIVEN);
	}
	int silent = 1; for (uint32_t k = 0; k < 2048; k++) if (RD8(PCM + k)) silent = 0;
	check(rc == 0, "Decode x4");
	check(first > 0 && first < 64, "  a frame takes its own bytes");
	check(last > first && es_off == 1024, "  the last frame also takes the superframe's padding: 4 frames = 1024 bytes");
	check(silent, "  the frames' PCM is silence");
	WR32(CTRL + C_ES, ES);
	check(call("sceAudiodecDecodeNFrames", CTRL, 0) == AD_ERR_INVALID_NFRAMES, "DecodeNFrames(0): INVALID_NFRAMES");
	check(call("sceAudiodecDecodeNFrames", CTRL, 9) == AD_ERR_INVALID_NFRAMES, "DecodeNFrames(9): INVALID_NFRAMES");
	memset(G(ES + 0x1000), 0xff, 256); WR32(CTRL + C_ES, ES + 0x1000);
	check(call("sceAudiodecDecode", CTRL, 0) == AD_ERR_API_FAIL, "Decode of a corrupt frame: API_FAIL");
	WR32(CTRL + C_ES, 0);
	check(call("sceAudiodecDecode", CTRL, 0) == AD_ERR_INVALID_PTR, "Decode without pEs: INVALID_PTR");
	check(call("sceAudiodecClearContext", CTRL, 0) == 0, "ClearContext");
	check(call("sceAudiodecGetInternalError", CTRL, 0x1400) == 0, "GetInternalError");
	check(call("sceAudiodecDeleteDecoder", CTRL, 0) == 0, "DeleteDecoder");
	WR32(CTRL + C_HANDLE, h);
	check(call("sceAudiodecDeleteDecoder", CTRL, 0) == AD_ERR_NOT_HANDLE_IN_USE, "DeleteDecoder again: NOT_HANDLE_IN_USE");
	WR32(CTRL + C_ES, ES); WR32(CTRL + C_PCM, PCM);
	check(call("sceAudiodecDecode", CTRL, 0) == AD_ERR_NOT_HANDLE_IN_USE, "Decode of a deleted handle: NOT_HANDLE_IN_USE");
	check(call("sceAudiodecTermLibrary", T_AT9, 0) == 0, "TermLibrary");
}

#ifdef RECOMP_AVCODEC
static uint8_t *slurp(const char *path, size_t *n) {
	char alt[256]; snprintf(alt, sizeof alt, "runtime/tests/%s", path);   /* from this folder or the repository root (run_all.sh) */
	FILE *f = fopen(path, "rb"); if (!f) f = fopen(alt, "rb");
	if (!f) return 0;
	fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc(*n + AV_INPUT_BUFFER_PADDING_SIZE); memset(b + *n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
	if (fread(b, 1, *n, f) != *n) { free(b); b = 0; }
	fclose(f); return b;
}
/* libavcodec decoding the frames of a file directly: the reference. frames[i] / sizes[i] are the packets. */
static int16_t *reference(int id, const uint8_t *asc, int asc_n, int ch, int rate, uint8_t **frames, uint32_t *sizes, int n, size_t *samples) {
	const AVCodec *codec = avcodec_find_decoder(id);
	AVCodecContext *x = avcodec_alloc_context3(codec);
	if (asc) { x->extradata = av_mallocz(asc_n + AV_INPUT_BUFFER_PADDING_SIZE); memcpy(x->extradata, asc, asc_n); x->extradata_size = asc_n; }
	if (avcodec_open2(x, codec, 0) < 0) return 0;
	AVPacket *pk = av_packet_alloc(); AVFrame *fr = av_frame_alloc();
	int16_t *out = malloc(1 << 20); *samples = 0;
	for (int i = 0; i < n; i++) {
		pk->data = frames[i]; pk->size = (int)sizes[i];
		if (avcodec_send_packet(x, pk) < 0) break;
		while (avcodec_receive_frame(x, fr) >= 0) {
			for (int s = 0; s < fr->nb_samples; s++) for (int k = 0; k < fr->ch_layout.nb_channels; k++)
				out[(*samples)++] = to_s16(((const float *)fr->extended_data[k])[s]);
			av_frame_unref(fr);
		}
	}
	avcodec_free_context(&x); av_packet_free(&pk); av_frame_free(&fr);
	(void)ch; (void)rate;
	return out;
}
/* a codec's frames through Decode, one call each, against the reference; returns the PCM sample count */
static void run_stream(const char *name, uint32_t type, uint32_t info_words, const uint32_t *info, uint8_t **frames, uint32_t *sizes, int n, int use_hint, const int16_t *ref, size_t ref_n) {
	char what[160];
	ctrl_setup(CTRL, ES, PCM); memset(G(INFO), 0, 0x20);
	for (uint32_t i = 0; i < info_words; i++) WR32(INFO + 4 * i, info[i]);
	snprintf(what, sizeof what, "%s: CreateDecoder", name);
	uint32_t r = call("sceAudiodecCreateDecoderExternal", CTRL, type);
	check(r == 0, what);
	if (r) return;
	snprintf(what, sizeof what, "%s: maxEsSize %u, maxPcmSize %u", name, RD32(CTRL + C_ES_MAX), RD32(CTRL + C_PCM_MAX)); check(RD32(CTRL + C_ES_MAX) > 0 && RD32(CTRL + C_PCM_MAX) > 0, what);
	size_t got = 0; int ok = 1, sizes_ok = 1;
	for (int i = 0; i < n; i++) {
		memset(G(ES), 0, 2048); memcpy(G(ES), frames[i], sizes[i]);   /* (zeros after the frame, as a game's buffer has) */
		WR32(CTRL + C_ES, ES); WR32(CTRL + C_PCM, PCM); WR32(CTRL + C_ES_USED, use_hint ? sizes[i] : 0);
		if (call("sceAudiodecDecode", CTRL, 0)) { ok = 0; break; }
		if (RD32(CTRL + C_ES_USED) != sizes[i]) sizes_ok = 0;
		uint32_t g = RD32(CTRL + C_PCM_GIVEN);
		if (got * 2 + g <= ref_n * 2) { if (memcmp(G(PCM), ref + got, g)) ok = 0; }
		else ok = 0;
		got += g / 2;
	}
	snprintf(what, sizeof what, "%s: every Decode returns 0 and its PCM equals libavcodec's (%zu samples)", name, got); check(ok && got == ref_n, what);
	snprintf(what, sizeof what, "%s: inputEsSize is each frame's length", name); check(sizes_ok, what);
	int peak = 0; for (size_t k = 0; k < got; k++) { int v = ref[k] < 0 ? -ref[k] : ref[k]; if (v > peak) peak = v; }
	snprintf(what, sizeof what, "%s: the tone is audible (peak %d)", name, peak); check(peak > 1000, what);
	check(call("sceAudiodecClearContext", CTRL, 0) == 0, "  ClearContext");
	check(call("sceAudiodecDeleteDecoderExternal", CTRL, 0) == 0, "  DeleteDecoderExternal");
}

static void test_aac(void) {
	size_t n; uint8_t *f = slurp("audiodec/tone.aac", &n);
	if (!f) { check(0, "tone.aac readable"); return; }
	if (!avc_has(T_AAC)) { printf("skip AAC: this libavcodec has no AAC decoder\n"); return; }
	uint8_t *adts[64], *raw[64]; uint32_t asz[64], rsz[64]; int cnt = 0;
	for (size_t o = 0; o + 7 <= n && cnt < 64;) {
		uint32_t len = (uint32_t)(f[o + 3] & 3) << 11 | (uint32_t)f[o + 4] << 3 | f[o + 5] >> 5;
		if (f[o] != 0xFF || len < 8 || o + len > n) break;
		adts[cnt] = f + o; asz[cnt] = len; raw[cnt] = f + o + 7; rsz[cnt] = len - 7; cnt++; o += len;
	}
	check(cnt >= 4, "tone.aac has several ADTS frames");
	const uint8_t asc[2] = {0x12, 0x08};   /* AAC-LC, 44.1 kHz, mono */
	size_t rn, an;
	int16_t *ref_raw = reference(AV_CODEC_ID_AAC, asc, 2, 1, 44100, raw, rsz, cnt, &rn);
	int16_t *ref_adts = reference(AV_CODEC_ID_AAC, 0, 0, 1, 44100, adts, asz, cnt, &an);
	WR32(PARAM, 8); WR32(PARAM + 4, 1);
	check(call("sceAudiodecInitLibrary", T_AAC, PARAM) == 0, "InitLibrary(AAC, 1 stream)");
	const uint32_t info_adts[5] = {INFO_AAC, 1, 1, 44100, 0}, info_raw[5] = {INFO_AAC, 0, 1, 44100, 0}, info_bad[5] = {INFO_AAC, 0, 6, 44100, 0};
	ctrl_setup(CTRL, ES, PCM); memset(G(INFO), 0, 0x20); for (int i = 0; i < 5; i++) WR32(INFO + 4 * i, info_bad[i]);
	check(call("sceAudiodecCreateDecoder", CTRL, T_AAC) == AD_ERR_AAC_INVALID_CH, "AAC with 6 channels: AAC INVALID_CH");
	check(call("sceAudiodecGetContextSize", CTRL, T_AAC) == 0x18000, "GetContextSize(AAC)");
	run_stream("AAC (ADTS)", T_AAC, 5, info_adts, adts, asz, cnt, 0, ref_adts, an);
	run_stream("AAC (raw frames, size from the caller)", T_AAC, 5, info_raw, raw, rsz, cnt, 1, ref_raw, rn);
	check(call("sceAudiodecTermLibrary", T_AAC, 0) == 0, "TermLibrary(AAC)");
	free(ref_raw); free(ref_adts); free(f);
}
static void test_mp3(void) {
	size_t n; uint8_t *f = slurp("audiodec/tone.mp3", &n);
	if (!f) { check(0, "tone.mp3 readable"); return; }
	if (!avc_has(T_MP3)) { printf("skip MP3: this libavcodec has no MP3 decoder\n"); return; }
	size_t o = 0; while (o + 4 < n && !mp3_len(f + o)) o++;   /* past the ID3 tag */
	uint8_t *fr[64]; uint32_t sz[64]; int cnt = 0;
	while (o + 4 <= n && cnt < 64) { uint32_t len = mp3_len(f + o); if (!len || o + len > n) break; fr[cnt] = f + o; sz[cnt++] = len; o += len; }
	check(cnt >= 4, "tone.mp3 has several frames");
	size_t rn; int16_t *ref = reference(AV_CODEC_ID_MP3, 0, 0, 1, 44100, fr, sz, cnt, &rn);
	const uint32_t info[3] = {INFO_MP3, 1, 3}, bad_ver[3] = {INFO_MP3, 1, 1}, bad_ch[3] = {INFO_MP3, 3, 3};
	ctrl_setup(CTRL, ES, PCM); memset(G(INFO), 0, 0x20);
	for (int i = 0; i < 3; i++) WR32(INFO + 4 * i, bad_ver[i]);
	check(call("sceAudiodecCreateDecoderExternal", CTRL, T_MP3) == AD_ERR_MP3_INVALID_MPEG_VERSION, "MP3 with the reserved version: INVALID_MPEG_VERSION");
	for (int i = 0; i < 3; i++) WR32(INFO + 4 * i, bad_ch[i]);
	check(call("sceAudiodecCreateDecoderExternal", CTRL, T_MP3) == AD_ERR_MP3_INVALID_CH, "MP3 with 3 channels: MP3 INVALID_CH");
	run_stream("MP3", T_MP3, 3, info, fr, sz, cnt, 0, ref, rn);
	free(ref); free(f);
}
#endif

int main(void) {
	recomp_mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	if (recomp_mem == MAP_FAILED) { perror("mmap"); return 1; }
	test_api_and_at9();
#ifdef RECOMP_AVCODEC
	test_aac();
	test_mp3();
#else
	printf("skip AAC and MP3: built without RECOMP_AVCODEC\n");
	WR32(PARAM, 8); WR32(PARAM + 4, 1); ctrl_setup(CTRL, ES, PCM);
	check(call("sceAudiodecInitLibrary", T_AAC, PARAM) == AD_ERR_INVALID_TYPE, "InitLibrary(AAC) without libavcodec: INVALID_TYPE");
	check(call("sceAudiodecCreateDecoder", CTRL, T_MP3) == AD_ERR_INVALID_TYPE, "CreateDecoder(MP3) without libavcodec: INVALID_TYPE");
#endif
	if (fails) { printf("%d checks FAILED\n", fails); return 1; }
	printf("all passed\n");
	return 0;
}
