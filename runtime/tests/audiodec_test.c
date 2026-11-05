/* audiodec_test.c -- the SceAudiodecUser ATRAC9 contract (runtime/audio/audiodec.c): InitLibrary for the
 * ATRAC9 type, CreateDecoder fills the info (channels, rate, superframe) and the ctrl's handle and
 * maximum sizes from the configuration word, Decode turns one frame into 16-bit PCM and reports the bytes
 * taken (a superframe's padding with its last frame) and given, ClearContext and DeleteDecoder end with 0,
 * and the error cases (other codec types, no InitLibrary, wrong ctrl size, a deleted handle) are errors.
 *
 * The configuration is synthetic (the ATRAC9 header layout: 0xFE, rate index 7 = 48 kHz, channel config
 * 0 = mono, 256-byte frames, superframe index 2 = 4 frames); the frames are built here bit by bit as the
 * smallest valid mono frame, so no title or encoder data is involved.
 *
 * -DFAIL_BEFORE runs the same checks against the behaviour without this file: every sceAudiodec* import
 * unresolved, which the runtime answers with 0 and nothing written (main.c's "not implemented -- returns 0").
 * Build and run: see README.md here.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../audio/audiodec.c"

uint8_t *recomp_mem;
uint32_t recomp_watch_lo = 0, recomp_watch_hi = 0;
void recomp_watch_write(uint32_t a, uint32_t v, int width) { (void)a; (void)v; (void)width; }

static int fails;
static void check(const char *what, int ok) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

#ifdef FAIL_BEFORE
static void unresolved(Cpu *c) { RET(0); }
#endif
static uint32_t call(const char *name, uint32_t a0, uint32_t a1) {
	Cpu cpu; memset(&cpu, 0, sizeof cpu); Cpu *c = &cpu;
	c->r[0] = a0; c->r[1] = a1;
#ifdef FAIL_BEFORE
	unresolved(c); (void)name;
#else
	HleFn f = 0;
	for (const HleEntry *e = hle_audiodec; e->name; e++) if (!strcmp(e->name, name)) f = e->fn;
	if (!f) { printf("FAIL no entry %s\n", name); fails++; return 0xffffffffu; }
	f(c);
#endif
	return c->r[0];
}
static uint32_t rd(uint32_t a) { uint32_t v; memcpy(&v, G(a), 4); return v; }
static void wr(uint32_t a, uint32_t v) { memcpy(G(a), &v, 4); }

/* a bit writer, MSB first, for the synthetic frame */
static void put(uint8_t *buf, int *pos, uint32_t v, int bits) {
	for (int i = bits - 1; i >= 0; i--, (*pos)++) if (v >> i & 1) buf[*pos >> 3] |= 0x80 >> (*pos & 7);
}

int main(void) {
	recomp_mem = calloc(1, 0x20000);
	const uint32_t CTRL = 0x1000, INFO = 0x1100, ES = 0x2000, PCM = 0x8000, ERR = 0x1200;
	wr(CTRL + C_SIZE, CTRL_SIZE); wr(CTRL + C_INFO, INFO); wr(CTRL + C_ES, ES); wr(CTRL + C_PCM, PCM); wr(CTRL + C_WORD, 16);
	wr(INFO, 0x1c); memcpy(G(INFO + I_CONFIG), "\xFE\x70\x1F\xF0", 4);

	check("CreateDecoder before InitLibrary -> NOT_INITIALIZED", call("sceAudiodecCreateDecoder", CTRL, SCE_AUDIODEC_TYPE_AT9) == SCE_AUDIODEC_ERROR_NOT_INITIALIZED);
	check("InitLibrary(MP3) -> INVALID_TYPE (not implemented, fails loudly)", call("sceAudiodecInitLibrary", 0x1004, 0) == SCE_AUDIODEC_ERROR_INVALID_TYPE);
	check("InitLibrary(ATRAC9) -> 0", call("sceAudiodecInitLibrary", SCE_AUDIODEC_TYPE_AT9, 0) == 0);
	wr(CTRL + C_SIZE, 0x24);
	check("CreateDecoder with ctrl size 0x24 -> INVALID_SIZE", call("sceAudiodecCreateDecoder", CTRL, SCE_AUDIODEC_TYPE_AT9) == SCE_AUDIODEC_ERROR_INVALID_SIZE);
	wr(CTRL + C_SIZE, CTRL_SIZE);
	check("CreateDecoder(ATRAC9) -> 0", call("sceAudiodecCreateDecoder", CTRL, SCE_AUDIODEC_TYPE_AT9) == 0);
	check("  handle set", rd(CTRL + C_HANDLE) != 0);
	check("  info: 1 channel", rd(INFO + I_CH) == 1);
	check("  info: 48000 Hz", rd(INFO + I_RATE) == 48000);
	check("  info: superframe 1024 bytes", rd(INFO + I_SF_SIZE) == 1024);
	check("  info: 4 frames per superframe", rd(INFO + I_SF_FRAMES) == 4);
	check("  info: bit rate 384000 (1024 bytes per 1024 samples at 48 kHz)", rd(INFO + I_BITRATE) == 384000);
	check("  ctrl: maxEsSize 1024", rd(CTRL + C_ES_MAX) == 1024);
	check("  ctrl: maxPcmSize 512 (256 samples x 1 ch x 2 bytes)", rd(CTRL + C_PCM_MAX) == 512);

	/* the smallest valid mono frame, in LibAtrac9's unpack order: block header (first-in-superframe flag
	 * inverted, reuse 0), band count 0, no band extension, gradient mode 0 with zero units/values/boundary
	 * (35 bits), no extension data (1 bit), then scale-factor mode 1 (fixed-length, bit length 2, base 0: all scale factors 0), and
	 * zero spectra -- silence. Frames 2-4 set the first bit: not the first in the superframe. ATRAC9 frames
	 * are variable-length and packed back to back in the superframe, so each one is written where the
	 * previous Decode said it ended (the rest of the superframe stays zero: its padding). */
	memset(G(ES), 0, 1024);
	uint32_t total_es = 0, total_pcm = 0, rc_all = 0, first_used = 0, last_used = 0;
	for (int i = 0; i < 4; i++) {
		uint8_t frame[64]; memset(frame, 0, sizeof frame);
		int p = 0; put(frame, &p, i ? 1 : 0, 1);
		p = 36; put(frame, &p, 1, 2);
		memcpy(G(ES + total_es), frame, sizeof frame);
		wr(CTRL + C_ES, ES + total_es); wr(CTRL + C_PCM, PCM + total_pcm);
		memset(G(PCM + total_pcm), 0x5a, 512);
		rc_all |= call("sceAudiodecDecode", CTRL, 0);
		if (!i) first_used = rd(CTRL + C_ES_USED);
		last_used = rd(CTRL + C_ES_USED);
		total_es += rd(CTRL + C_ES_USED); total_pcm += rd(CTRL + C_PCM_GIVEN);
	}
	int silent = 1; for (uint32_t k = 0; k < 2048; k++) if (RD8(PCM + k)) silent = 0;
	check("Decode x4 -> 0", rc_all == 0);
	check("  a frame takes only its own bytes (variable length, < 64)", first_used > 0 && first_used < 64);
	check("  the last frame also takes the superframe's padding", last_used > first_used);
	check("  4 frames take exactly the 1024-byte superframe", total_es == 1024);
	check("  each frame gives 512 bytes of PCM", total_pcm == 2048);
	check("  the PCM written is the frames' silence", silent);
	wr(CTRL + C_PCM, PCM);

	/* a frame the decoder rejects (scale factors out of range: all-ones bits) is an error, not silence */
	memset(G(ES + 0x1000), 0xff, 256); wr(CTRL + C_ES, ES + 0x1000);
	check("Decode of a corrupt frame -> API_FAIL", call("sceAudiodecDecode", CTRL, 0) == SCE_AUDIODEC_ERROR_API_FAIL);
	check("ClearContext -> 0", call("sceAudiodecClearContext", CTRL, 0) == 0);
	check("GetInternalError -> 0", call("sceAudiodecGetInternalError", CTRL, ERR) == 0);
	check("DeleteDecoder -> 0", call("sceAudiodecDeleteDecoder", CTRL, 0) == 0);
	check("DeleteDecoder again -> NOT_HANDLE_IN_USE", call("sceAudiodecDeleteDecoder", CTRL, 0) == SCE_AUDIODEC_ERROR_NOT_HANDLE_IN_USE);
	check("CreateDecoder(MP3) -> INVALID_TYPE", call("sceAudiodecCreateDecoder", CTRL, 0x1004) == SCE_AUDIODEC_ERROR_INVALID_TYPE);
	check("TermLibrary(ATRAC9) -> 0", call("sceAudiodecTermLibrary", SCE_AUDIODEC_TYPE_AT9, 0) == 0);

#if defined(RECOMP_RAWDEC) || defined(AAC_EXPECTED)
	/* AAC (built with RECOMP_RAWDEC: FFmpeg + rawdec.c). The info: {size, isAdts 0, 1 channel, 48000 Hz, isSbr 0}.
	 * The frame: the smallest raw AAC-LC mono raw_data_block, from the bitstream syntax -- SCE (3 bits 0),
	 * tag 0 (4), global_gain 100 (8), ics_info: reserved 0, ONLY_LONG (2), shape 0, max_sfb 0 (6),
	 * no predictor (1); no sections or scale factors at max_sfb 0; no pulse/TNS/gain control (3); ID_END (3
	 * bits 111) = 32 bits, 00 C8 00 07: silence. Two of them back to back. */
	wr(CTRL + C_HANDLE, 0); wr(CTRL + C_ES, ES); wr(CTRL + C_PCM, PCM);
	wr(INFO, 0x14); wr(INFO + I_AAC_ADTS, 0); wr(INFO + I_AAC_CH, 1); wr(INFO + I_AAC_RATE, 48000); wr(INFO + I_AAC_SBR, 0);
	/* a movie player's sequence: GetContextSize, then CreateDecoderExternal with that memory -- no InitLibrary */
	check("AAC: GetContextSize -> 0x18000", call("sceAudiodecGetContextSize", CTRL, 0x1005) == 0x18000);
	check("AAC: CreateDecoder (not External) before InitLibrary -> NOT_INITIALIZED", call("sceAudiodecCreateDecoder", CTRL, 0x1005) == SCE_AUDIODEC_ERROR_NOT_INITIALIZED);
	check("AAC: CreateDecoderExternal without InitLibrary -> 0", call("sceAudiodecCreateDecoderExternal", CTRL, 0x1005) == 0);
	check("  handle set", rd(CTRL + C_HANDLE) != 0);
	check("  maxEsSize 1536", rd(CTRL + C_ES_MAX) == 1536);
	check("  maxPcmSize 2048 (1 ch)", rd(CTRL + C_PCM_MAX) == 2048);
	memset(G(ES), 0, 0x1000);
	memcpy(G(ES), "\x00\xC8\x00\x07\x00\xC8\x00\x07", 8);
	memset(G(PCM), 0x5a, 0x1000);
	uint32_t aac_rc = call("sceAudiodecDecode", CTRL, 0), aac_used = rd(CTRL + C_ES_USED), aac_given = rd(CTRL + C_PCM_GIVEN);
	check("AAC: Decode -> 0", aac_rc == 0);
	check("  the frame's 4 bytes taken", aac_used == 4);
	wr(CTRL + C_ES, ES + aac_used);
	aac_rc = call("sceAudiodecDecode", CTRL, 0);
	check("AAC: second Decode -> 0, 4 bytes", aac_rc == 0 && rd(CTRL + C_ES_USED) == 4);
	aac_given += rd(CTRL + C_PCM_GIVEN);
	check("  1024 samples x 1 ch x 2 bytes per frame (two frames, 4096 bytes)", aac_given == 4096);
	int aac_silent = 1; for (uint32_t k = 0; k < 2048; k++) if (RD8(PCM + k)) aac_silent = 0;
	check("  the PCM written is silence", aac_silent);
	check("AAC: ClearContext -> 0", call("sceAudiodecClearContext", CTRL, 0) == 0);
	check("AAC: DeleteDecoderExternal -> 0", call("sceAudiodecDeleteDecoderExternal", CTRL, 0) == 0);
	check("AAC: InitLibrary -> 0", call("sceAudiodecInitLibrary", 0x1005, 0) == 0);
	check("AAC: TermLibrary -> 0", call("sceAudiodecTermLibrary", 0x1005, 0) == 0);
#else
	check("AAC without RECOMP_RAWDEC: GetContextSize -> INVALID_TYPE", call("sceAudiodecGetContextSize", CTRL, 0x1005) == SCE_AUDIODEC_ERROR_INVALID_TYPE);
#endif

	if (fails) { printf("%d checks FAILED\n", fails); return 1; }
	printf("all passed\n");
	return 0;
}
