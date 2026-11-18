/*
 * How many bytes a raw AAC frame (one raw_data_block, ISO/IEC 14496-3 4.4.2) takes, read from its own syntax.
 *
 * sceAudiodec takes raw AAC one frame at a time and reports the bytes it used, but a raw frame states no length
 * and libavcodec's public API doesn't say how much of a packet its decoder used. Callers don't say either: Sony's
 * AvPlayer puts each frame at the start of its own 512-byte slot and leaves inputEsSize at whatever the last call
 * reported, so a packet cut anywhere else runs into the next frames, which libavcodec then decodes as more frames
 * from the same packet (garbage, errors, and a decoder state the real frames have to recover from).
 *
 * This walks the frame's elements without decoding them: each channel's section data, scalefactors (Huffman
 * coded), pulse and TNS data, and spectral data (Huffman coded, with the sign bits and escapes the decoded values
 * call for), counting bits up to the END element. Only the lengths come out; the tables are aac_tables.h. It
 * covers AAC-LC and what rides on it (SBR and PS are fill elements): the coupling element, Main-profile
 * prediction and SSR gain control answer -1, and so does anything that runs past the buffer, so the caller can
 * fall back to another guess. sfi is the sampling-frequency index (0 = 96000 ... 12 = 7350).
 */
#pragma once
#include <stdint.h>
#include "aac_tables.h"

typedef struct { const uint8_t *p; uint32_t nbytes, pos; } AacBits;   /* pos in bits */

static inline uint32_t aacb_peek(const AacBits *b, int k) {   /* the next k bits (1..24); zeros past the end */
	uint32_t i = b->pos >> 3, w = 0;
	for (int j = 0; j < 4; j++) w = w << 8 | (i + j < b->nbytes ? b->p[i + j] : 0);
	return (w << (b->pos & 7)) >> (32 - k);
}
static inline uint32_t aacb_get(AacBits *b, int k) { uint32_t v = aacb_peek(b, k); b->pos += k; return v; }
static inline int aacb_over(const AacBits *b) { return b->pos > b->nbytes * 8; }

static int aacf_huff_sf(AacBits *b) {   /* a scalefactor codeword (up to 19 bits) */
	uint32_t w = aacb_peek(b, 19);
	for (int i = 0; i < 121; i++)
		if (w >> (19 - aac_sf_bits[i]) == aac_sf_code[i]) { b->pos += aac_sf_bits[i]; return i; }
	return -1;
}
static int aacf_huff_spec(AacBits *b, int cb) {   /* a spectral codeword of book cb (1..11, up to 16 bits): its index */
	const uint16_t *code = aac_codes[cb - 1]; const uint8_t *len = aac_bits[cb - 1];
	uint32_t w = aacb_peek(b, 16);
	for (int i = 0; i < aac_book_size[cb - 1]; i++)
		if (w >> (16 - len[i]) == code[i]) { b->pos += len[i]; return i; }
	return -1;
}
static int aacf_escape(AacBits *b) {   /* N ones, a zero, then N + 4 bits */
	int n = 0;
	while (aacb_get(b, 1)) if (++n > 8 || aacb_over(b)) return -1;
	b->pos += (uint32_t)n + 4;
	return 0;
}

typedef struct { int eight, max_sfb, ngroups, glen[8]; } AacIcs;

static int aacf_ics_info(AacBits *b, AacIcs *s, int sfi) {
	if (aacb_get(b, 1)) return -1;   /* ics_reserved_bit */
	s->eight = aacb_get(b, 2) == 2;   /* window_sequence: EIGHT_SHORT_SEQUENCE */
	b->pos += 1;                      /* window_shape */
	s->ngroups = 1; s->glen[0] = 1;
	if (s->eight) {
		s->max_sfb = (int)aacb_get(b, 4);
		uint32_t grouping = aacb_get(b, 7);
		for (int i = 6; i >= 0; i--) {
			if (grouping >> i & 1) s->glen[s->ngroups - 1]++;
			else s->glen[s->ngroups++] = 1;
		}
		return s->max_sfb <= aac_num_swb_short[sfi] ? 0 : -1;
	}
	s->max_sfb = (int)aacb_get(b, 6);
	if (aacb_get(b, 1)) return -1;   /* predictor_data_present: Main and LTP profiles, not LC */
	return s->max_sfb <= aac_num_swb_long[sfi] ? 0 : -1;
}

static int aacf_ics(AacBits *b, const AacIcs *shared, int sfi) {   /* individual_channel_stream */
	AacIcs own;
	b->pos += 8;   /* global_gain */
	const AacIcs *s = shared;
	if (!s) { if (aacf_ics_info(b, &own, sfi)) return -1; s = &own; }
	uint8_t cbs[8][64];
	/* section_data: runs of bands sharing a codebook */
	int sbits = s->eight ? 3 : 5, sesc = (1 << sbits) - 1;
	for (int g = 0; g < s->ngroups; g++) {
		int k = 0;
		while (k < s->max_sfb) {
			int cb = (int)aacb_get(b, 4), n = 0, inc;
			if (cb == 12) return -1;
			do { inc = (int)aacb_get(b, sbits); n += inc; } while (inc == sesc && !aacb_over(b));
			if (aacb_over(b) || !n || k + n > s->max_sfb) return -1;
			while (n--) cbs[g][k++] = (uint8_t)cb;
		}
	}
	/* scale_factor_data: one codeword a coded band; the first noise band's energy is 9 plain bits */
	int noise_first = 1;
	for (int g = 0; g < s->ngroups; g++)
		for (int sfb = 0; sfb < s->max_sfb; sfb++) {
			int cb = cbs[g][sfb];
			if (!cb) continue;
			if (cb == 13 && noise_first) { noise_first = 0; b->pos += 9; }
			else if (aacf_huff_sf(b) < 0) return -1;
		}
	if (aacb_get(b, 1)) {   /* pulse_data */
		int n = (int)aacb_get(b, 2);
		b->pos += 6 + 9 * (uint32_t)(n + 1);
	}
	if (aacb_get(b, 1)) {   /* tns_data */
		for (int w = 0; w < (s->eight ? 8 : 1); w++) {
			int nf = (int)aacb_get(b, s->eight ? 1 : 2);
			if (!nf) continue;
			int res = (int)aacb_get(b, 1);
			for (int f = 0; f < nf; f++) {
				b->pos += s->eight ? 4 : 6;   /* length */
				int order = (int)aacb_get(b, s->eight ? 3 : 5);
				if (!order) continue;
				b->pos += 1;   /* direction */
				int compress = (int)aacb_get(b, 1);
				b->pos += (uint32_t)(order * (3 + res - compress));
			}
		}
	}
	if (aacb_get(b, 1)) return -1;   /* gain_control_data: the SSR profile */
	/* spectral_data: tuples of 4 (books 1-4) or 2 (5-11); unsigned books add a sign bit per nonzero value,
	 * book 11 an escape per value of 16 */
	const uint16_t *off = s->eight ? aac_swb_short[sfi] : aac_swb_long[sfi];
	for (int g = 0; g < s->ngroups; g++)
		for (int sfb = 0; sfb < s->max_sfb; sfb++) {
			int cb = cbs[g][sfb];
			if (!cb || cb >= 13) continue;   /* zero, noise and intensity bands carry no spectral data */
			int ncoef = (off[sfb + 1] - off[sfb]) * s->glen[g], step = cb < 5 ? 4 : 2;
			for (int k = 0; k < ncoef; k += step) {
				int i = aacf_huff_spec(b, cb);
				if (i < 0) return -1;
				switch (cb) {
				case 3: case 4: b->pos += (i / 27 != 0) + (i / 9 % 3 != 0) + (i / 3 % 3 != 0) + (i % 3 != 0); break;
				case 7: case 8: b->pos += (i / 8 != 0) + (i % 8 != 0); break;
				case 9: case 10: b->pos += (i / 13 != 0) + (i % 13 != 0); break;
				case 11:
					b->pos += (i / 17 != 0) + (i % 17 != 0);
					if (i / 17 == 16 && aacf_escape(b)) return -1;
					if (i % 17 == 16 && aacf_escape(b)) return -1;
					break;
				default: break;   /* 1, 2, 5, 6: signed values, no sign bits */
				}
				if (aacb_over(b)) return -1;
			}
		}
	return aacb_over(b) ? -1 : 0;
}

static int aacf_pce(AacBits *b) {   /* program_config_element */
	b->pos += 4 + 2 + 4;   /* tag, object type, sampling-frequency index */
	int front = (int)aacb_get(b, 4), side = (int)aacb_get(b, 4), back = (int)aacb_get(b, 4);
	int lfe = (int)aacb_get(b, 2), assoc = (int)aacb_get(b, 3), cc = (int)aacb_get(b, 4);
	if (aacb_get(b, 1)) b->pos += 4;   /* mono mixdown */
	if (aacb_get(b, 1)) b->pos += 4;   /* stereo mixdown */
	if (aacb_get(b, 1)) b->pos += 3;   /* matrix mixdown */
	b->pos += (uint32_t)(5 * (front + side + back) + 4 * (lfe + assoc) + 5 * cc);
	b->pos = (b->pos + 7) & ~7u;
	b->pos += 8 * aacb_get(b, 8);   /* comment bytes */
	return aacb_over(b) ? -1 : 0;
}

/* the frame's length in bytes, or -1 */
static int aac_raw_frame_len(const uint8_t *p, uint32_t n, int sfi) {
	if (sfi < 0 || sfi > 12) return -1;
	AacBits b = {p, n, 0};
	for (int elements = 0; elements < 64; elements++) {
		uint32_t id = aacb_get(&b, 3);
		switch (id) {
		case 0: case 3:   /* single channel, LFE */
			b.pos += 4;
			if (aacf_ics(&b, 0, sfi)) return -1;
			break;
		case 1: {   /* channel pair */
			b.pos += 4;
			AacIcs shared;
			int common = (int)aacb_get(&b, 1);
			if (common) {
				if (aacf_ics_info(&b, &shared, sfi)) return -1;
				uint32_t ms = aacb_get(&b, 2);
				if (ms == 3) return -1;
				if (ms == 1) b.pos += (uint32_t)(shared.ngroups * shared.max_sfb);
			}
			if (aacf_ics(&b, common ? &shared : 0, sfi) || aacf_ics(&b, common ? &shared : 0, sfi)) return -1;
			break;
		}
		case 2: return -1;   /* coupling channel */
		case 4: {   /* data stream: byte-aligned or not, then its bytes */
			b.pos += 4;
			int align = (int)aacb_get(&b, 1);
			uint32_t cnt = aacb_get(&b, 8);
			if (cnt == 255) cnt += aacb_get(&b, 8);
			if (align) b.pos = (b.pos + 7) & ~7u;
			b.pos += 8 * cnt;
			break;
		}
		case 5: if (aacf_pce(&b)) return -1; break;
		case 6: {   /* fill (SBR and PS ride here) */
			uint32_t cnt = aacb_get(&b, 4);
			if (cnt == 15) cnt += aacb_get(&b, 8) - 1;
			b.pos += 8 * cnt;
			break;
		}
		case 7:   /* END, then byte alignment */
			b.pos = (b.pos + 7) & ~7u;
			return aacb_over(&b) ? -1 : (int)(b.pos >> 3);
		}
		if (aacb_over(&b)) return -1;
	}
	return -1;
}
