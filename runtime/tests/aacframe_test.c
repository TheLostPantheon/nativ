/*
 * runtime/audio/aacframe.h: a raw AAC frame's length, read from its syntax, checked against ADTS files whose headers
 * state every frame's length. Needs nothing else (no libavcodec).
 *
 * audiodec/synth.aac is nine ADTS streams one after another, 0.45 s each of a synthetic signal (a chirp, noise,
 * clicks for short windows, silence, a vibrato tone with noise; different in each channel), made with:
 *
 *     ffmpeg -f lavfi -i "aevalsrc=<the signal>:s=48000:d=0.45" -ar <rate> -ac <ch> -c:a <enc> -b:a <rate> <opts> -f adts
 *
 * for FFmpeg's encoder (aac: 48000 2ch 96k twoloop; 44100 1ch 48k PNS; 32000 2ch 32k intensity + PNS; 16000 1ch
 * 20k PNS; 96000 2ch 160k twoloop) and Apple's (aac_at: 48000 2ch 128k CBR; 24000 2ch 32k CBR; 44100 2ch 48k HE-AAC
 * with SBR; 8000 1ch 12k CBR). Between them they have long and short windows, M/S, intensity and noise bands, TNS,
 * escapes, SBR fill elements, and eight sample rates (six of the long-window band tables). Neither encoder writes
 * pulse data (aacframe.h reads it as FFmpeg's decoder does) or the elements aacframe.h turns down.
 *
 * Each frame is measured with the next frames right after it (as a game's buffer has them), then with its last byte
 * cut off, which must fail.
 *
 *     cc -O1 -g -I ../audio aacframe_test.c -o aacframe_test && ./aacframe_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../audio/aacframe.h"

int main(void) {
	FILE *f = fopen("audiodec/synth.aac", "rb"); if (!f) f = fopen("runtime/tests/audiodec/synth.aac", "rb");
	if (!f) { printf("FAIL synth.aac readable\n"); return 1; }
	static uint8_t buf[1 << 20]; size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
	int frames = 0, right = 0, cut_fail = 0, sfis = 0; uint32_t seen = 0;
	for (size_t o = 0; o + 7 <= n;) {
		const uint8_t *h = buf + o;
		if (h[0] != 0xFF || (h[1] & 0xF0) != 0xF0) { printf("FAIL no ADTS sync at %zu\n", o); return 1; }
		int crc = !(h[1] & 1), sfi = h[2] >> 2 & 15, blocks = h[6] & 3;
		uint32_t len = (uint32_t)(h[3] & 3) << 11 | (uint32_t)h[4] << 3 | h[5] >> 5, hl = crc ? 9 : 7;
		if (blocks) { printf("FAIL a frame with %d raw blocks at %zu\n", blocks + 1, o); return 1; }
		const uint8_t *raw = h + hl; uint32_t rn = len - hl;
		uint32_t room = (uint32_t)(n - o - hl);   /* the frame and everything after it */
		int got = aac_raw_frame_len(raw, room, sfi);
		if (got == (int)rn) right++;
		else if (frames - right < 5) printf("     frame %d (sample rate index %d, %u bytes): measured %d\n", frames, sfi, rn, got);
		if (aac_raw_frame_len(raw, rn - 1, sfi) < 0) cut_fail++;
		if (!(seen >> sfi & 1)) { seen |= 1u << sfi; sfis++; }
		frames++; o += len;
	}
	int fails = 0;
	printf("%s %d of %d frames measured to their ADTS length\n", right == frames ? "ok  " : "FAIL", right, frames); fails += right != frames;
	printf("%s %d of %d frames cut short by a byte answer -1\n", cut_fail == frames ? "ok  " : "FAIL", cut_fail, frames); fails += cut_fail != frames;
	printf("%s %d sample rates\n", sfis >= 7 ? "ok  " : "FAIL", sfis); fails += sfis < 7;
	printf(fails ? "%d failed\n" : "all passed\n", fails);
	return fails != 0;
}
