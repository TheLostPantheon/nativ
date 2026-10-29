/*
 * SceNgs (the Vita's audio voice engine) on the host. On the console the kernel module drives the
 * audio DSP (the codec engine), so NGS can't be recompiled; this implements what the DSP does
 * for the voices games use: a system, racks of voices built from a voice definition, patches
 * routing a voice's output into another's input with a 2x2 volume matrix, per-module parameter
 * blocks, and SystemUpdate rendering one granule: every playing voice's player reads its PCM
 * buffers, mixes through its patches into the master bus, whose state data is the output.
 *
 * Objects live in guest memory where the game sees them (handles are guest addresses of small
 * records); the state behind them is host-side.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* (glibc's dladdr and Dl_info: vo_chain) */
#endif
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "hle.h"
#ifndef _WIN32
#include <dlfcn.h>
#include <execinfo.h>
#endif
#include "third_party/libatrac9/libatrac9.h"   /* MIT, vendored unmodified */


enum { DEF_MASTER = 1, DEF_SIMPLE, DEF_REVERB, DEF_OTHER, DEF_SCREAM, DEF_SCREAM_AT9, DEF_COMPRESSOR, DEF_SIDECHAIN };
/* a bus that passes what reaches it through (no dynamics yet): the compressor, and the side-chain
 * compressor -- the same with a second input (1) carrying the key signal that drives its gain
 * reduction, which is never part of its output */
#define PASS_BUS(d) ((d) == DEF_COMPRESSOR || (d) == DEF_SIDECHAIN)
/* voices whose module 0 is a PCM player (the simple voice FMOD uses; Sony's Scream library's) */
#define PCM_PLAYER(d) ((d) == DEF_SIMPLE || (d) == DEF_SCREAM)
#define PLAYER_VOICE(d) (PCM_PLAYER(d) || (d) == DEF_SCREAM_AT9)   /* a voice that plays data and can finish */
/* ATRAC9 player parameters: descriptor, four buffers of 16 bytes {data, bytes, s16 loops, s16 next,
 * s16 discard at start, s16 at end}, +72 float frequency, +76 float scalar, +88 s8 channels,
 * +92 the codec's configuration word */
#define A_BUF(i) (8 + 16 * (i))
#define A_FREQ 72
#define A_SCALAR 76
#define A_CHANNELS 88
#define A_CONFIG 92
#define MAX_MODULES 8
/* player parameter layout (see render_player) */
#define P_BUF(i) (8 + 12 * (i))
#define P_FREQ 56
#define P_SCALAR 60
#define P_CHANNELS 76
#define P_TYPE 79
#define P_START 80
#define PARAMS_SIZE 512
typedef struct Voice {
	uint32_t handle, def;
	int playing, paused;
	uint32_t params[MAX_MODULES];          /* guest memory: each module's parameter block */
	int bypass[MAX_MODULES];
	/* player state */
	uint32_t buffer, byte_pos, samples_gen, bytes_consumed, loops_done;
	uint32_t upd_active;                   /* system updates it has been active for since its play (SceNgsVoiceInfo.uUpdateCallsActive) */
	double frac;                           /* the read position past the resampler's centre, in source samples (rs_read) */
	float rs_h[2][32]; int rs_i;           /* the last 16 source samples per channel, each stored twice (rs_push) */
	int16_t *mix;                          /* a master bus's output granule (stereo) */
	float lim_red;                         /* a master bus's limiter: its gain reduction now (master_limit) */
	void *at9; uint32_t at9_cfg;           /* an ATRAC9 player's decoder, for its configuration word */
	int16_t *pcm; int pcm_len, pcm_pos, at9_ch, at9_samples;   /* the last decoded frame */
	/* an ADPCM player (type 1, HE-VAG): the decoded 16-byte frame per channel, which frame it is, the
	 * sample within it and each channel's four-sample history */
	int16_t ad_pcm[2][28]; uint32_t ad_frame_buf, ad_frame_pos; int ad_valid, ad_idx; float ad_hist[2][4];
	int at9_sf_bytes, at9_sf_frames, at9_frame;   /* frames are packed in fixed-size superframes */
	uint32_t at9_sf_start;
	uint8_t stage[1024]; int stage_on, stage_pos;   /* a superframe that straddles two buffers, joined */
	float *acc;                            /* its accumulator */
	struct Reverb *rv;                     /* a reverb bus's state (reverb_run) */
	uint32_t rack;                         /* the rack it belongs to (callback info) */
	/* callbacks (guest functions taking a SceNgsCallbackInfo *): the voice's "finished" one, and one
	 * per module (a player's END_OF_DATA / SWAPPED_BUFFER / LOOPED_BUFFER events) */
	uint32_t fin_cb, fin_ud, mod_cb[MAX_MODULES], mod_ud[MAX_MODULES];
	int finishing;                         /* stopped by itself or keyed off: the finished callback is due */
	/* the voice's modules after its player (voice_stages): the envelope's place (point, ms into the
	 * segment toward the next, level), its release after a key-off, and each biquad's two delays per
	 * channel (the equalizer's four filters, then the filter module's) */
	int env_pt, releasing, released_out; float env_ms, env_level, rel_from, rel_ms;
	float bq[5][2][2];
	uint64_t at9_waited, at9_decoded;   /* RECOMP_NGS_AT9_LOG: output samples spent waiting for data, source samples decoded */
} Voice;
typedef struct { uint32_t src, dst, input; float vol[2][2]; int active; } Patch;
#define ENV_ID 0x5ce3   /* the envelope module's parameter block id (voice_stages, below) */
static int stages_off(void); static int stage_module(const Voice *v, uint32_t id);

/* the reverb bus: a Freeverb-style room (four parallel feedback combs with damping into two
 * allpasses per channel, the right channel's delays offset for stereo spread; delays scaled from
 * 44.1 to 48 kHz) set from the game's SceNgsReverbParams (id 0x01015CE7, I3DL2's property set:
 * levels in millibels, times in seconds; the block is on one of the bus voice's modules, Unit 13's
 * module 1). The dry send passes at fDryMB (Unit 13: 0 mB, unity -- most of its voices reach the
 * mix only through its two reverb buses, so this dry path IS their level; it was a fixed quarter
 * before), the late reverb enters the combs at fRoom + fReverb (Unit 13: -1220 + -1780 mB = -30 dB,
 * the I3DL2 onset level of the late reverb; before, the combs ran unscaled, +2 dB of resonant wash
 * over a -12 dB dry: sounds "not coming through clearly") and the combs' feedback gives fDecayTime
 * as their RT60. Without a parameter block (a game that never sets one) the old room stands, at a
 * sane level: dry 0.25, late reverb -20 dB, RT60 1.1 s. */
#define RV_COMBS 4
static const int rv_comb_len[RV_COMBS] = { 1215, 1293, 1390, 1476 }, rv_ap_len[2] = { 605, 480 }, rv_spread = 23;
#define RV_ER_LEN 16384   /* the early reflections' delay line: I3DL2's ReflectionsDelay reaches 0.3 s, 14400 samples at 48 kHz */
typedef struct Reverb { float comb[2][RV_COMBS][1500]; float filt[2][RV_COMBS]; int ci[2][RV_COMBS]; float ap[2][2][640]; int ai[2][2]; float er[2][RV_ER_LEN]; int ei;
	float erap[2][2][160]; int erai[2][2]; } Reverb;   /* erap: the early reflections' two diffusing allpasses per channel */
typedef struct { float dry, wet_in, damp, fb[RV_COMBS], er_gain; int er_delay; } ReverbSet;   /* er_*: the early reflections (reverb_run) */   /* wet_in: the late reverb level, energy-normalised for the comb bank (reverb_settings) */
#define REVERB_PARAMS_ID 0x5ce7   /* the low half of 0x01015CE7 and of 0x01025CE7 (V2, the same leading fields) */
static float mb_gain(float mb) { if (mb <= -10000) return 0; if (mb > 2000) mb = 2000; return powf(10.0f, mb / 2000.0f); }
static void reverb_settings(const uint32_t params[MAX_MODULES], uint32_t sample_rate, ReverbSet *s) {
	float dry_mb = -1204.1f /* 0.25 */, level_mb = -2000, rt60 = 1.1f, hf_ratio = 1, er_mb = -10000, er_delay = 0;
	for (int m = 0; m < MAX_MODULES; m++) {
		uint32_t p = params[m];
		if (!p || (RD32(p) & 0xffff) != REVERB_PARAMS_ID) continue;
		float room, decay, reverb, ratio, dry, refl, refl_delay; memcpy(&room, G(p + 8), 4); memcpy(&decay, G(p + 16), 4); memcpy(&ratio, G(p + 20), 4); memcpy(&reverb, G(p + 32), 4); memcpy(&dry, G(p + 72), 4);
		memcpy(&refl, G(p + 24), 4); memcpy(&refl_delay, G(p + 28), 4);   /* Reflections (mB), Reflections_delay (s) */
		if (room == room && reverb == reverb) level_mb = room + reverb;
		/* the reflections no louder than the room and no later than 40 ms, where the ear fuses them with the sound
		 * (I3DL2 allows +1000 mB over the room and 0.3 s). Uncharted's jungle room asks for both limits (Room -952,
		 * Reflections +1000 mB at 0.3 s) and Killzone's second bus for 0.25 s: rendered as asked, every sound sent to
		 * them repeats a quarter of a second later at full level, a slapback no room of theirs has (2026-10-28).
		 * RECOMP_NGS_ER_LITERAL=1: as asked. */
		static int literal = -1; if (literal < 0) literal = getenv("RECOMP_NGS_ER_LITERAL") != 0;
		if (!literal && refl > 0) refl = 0;
		if (room == room && refl == refl) er_mb = room + refl;
		if (refl_delay >= 0 && refl_delay <= 0.3f) er_delay = literal || refl_delay < 0.04f ? refl_delay : 0.04f;
		if (decay > 0.1f && decay < 20) rt60 = decay;
		if (ratio > 0 && ratio < 10) hf_ratio = ratio;
		if (dry == dry && dry <= 2000) dry_mb = dry;
		break;
	}
	s->dry = mb_gain(dry_mb); s->wet_in = mb_gain(level_mb);
	/* the early reflections: I3DL2's Room + Reflections level, ReflectionsDelay after the sound. They were not modelled --
	 * only the dry send and the late reverb -- and Killzone's second reverb bus (Room 0, Reflections -1000 mB at 0.25 s,
	 * Reverb -5000 mB, its dry muted at -10000) is all reflections: everything the game sent to it, a share of its
	 * ambience and effects, came out silent (its input 0.0032 rms, its output 0; 2026-10-17). */
	s->er_gain = mb_gain(er_mb); if (s->er_gain > 1) s->er_gain = 1;
	s->er_delay = (int)(er_delay * (float)sample_rate);
	s->damp = hf_ratio < 1 ? 1 - hf_ratio : 0; if (s->damp > 0.9f) s->damp = 0.9f;
	for (int k = 0; k < RV_COMBS; k++) s->fb[k] = powf(10.0f, -3.0f * (float)rv_comb_len[k] / (rt60 * (float)sample_rate));   /* -60 dB after rt60 */
	/* the late reverb level is the tail's energy relative to its input, whatever the decay time (as
	 * I3DL2 renderers normalise it -- OpenAL Soft's density gain, sqrt(1 - g^2) per feedback line): the
	 * four combs in parallel have an energy gain of sum 1/(1 - g^2), divided out here. Unnormalised,
	 * Uncharted's jungle room (-952 + 285 mB, 2.9 s) came out at ~3x its input -- louder than the dry
	 * mix, a wash of ambience and effects over the dialogue (which sends nothing to it), 2026-10-09.
	 * RECOMP_NGS_REVERB_RAW=1: the unnormalised bank. */
	static int raw = -1; if (raw < 0) raw = getenv("RECOMP_NGS_REVERB_RAW") != 0;
	float e = 0; for (int k = 0; k < RV_COMBS; k++) e += 1.0f / (1.0f - s->fb[k] * s->fb[k]);
	if (!raw) s->wet_in /= sqrtf(e);
	/* ...and the tail never carries more energy than its input. The two allpasses after the combs (feedback
	 * 0.5: impulse response -1, 1, 1/2, 1/4, ...) add 1 + 1/(1 - 1/4) = 7/3 each, ~7.4 dB together, which the
	 * normalisation above leaves in; and Uncharted asks for presets I3DL2 rendered literally can't have
	 * meant: A5 Cliff Descent's bus is Room -200 + Reverb +2000 mB with its dry muted (DryMB -10000 -- the
	 * game's own property table names the fields: Room, Room_HF, Decay_time, Decay_HF_ratio, Reflections,
	 * Reflections_delay, Reverb, Reverb_delay, Diffusion, Density, HF_reference, EarlyReflectionPattern L/R,
	 * EarlyReflectionScalar, LF_reference, Room_LF, DryMB at +8..+72), +24.8 dB over its input here: every
	 * sound in the area ("the background", dogs, ambience) far too loud, the mix clipping (2026-10-13). On
	 * the console it plays at a sane level, so its reverb bounds the tail somehow; bounding it at unity
	 * leaves the presets that sounded right (the jungle's -667 mB: within 0.7 dB; Unit 13's -3000: as is)
	 * and brings that one to the level of what the game sends. RECOMP_NGS_REVERB_UNBOUNDED=1: no bound. */
	static int unb = -1; if (unb < 0) unb = getenv("RECOMP_NGS_REVERB_UNBOUNDED") != 0;
	if (!raw && !unb) { const float ap = 7.0f / 3.0f, energy = s->wet_in * s->wet_in * e * ap * ap; if (energy > 1) s->wet_in /= sqrtf(energy); }   /* (the tail's energy gain: into the combs, the bank, the allpasses) */
}
/* a reverb bus whose reverb module the game bypassed (sceNgsVoiceBypassModule, SCE_NGS_MODULE_FLAG_BYPASSED)
 * passes what reaches it through unaltered. Uncharted bypasses one of its two buses' reverb in places and
 * leaves an extreme preset on it (A5 Cliff Descent: room -200 + reverb +2000 mB); run anyway, that bus
 * came out +24.8 dB over its input -- every sound sent to it ("the background channel": ambience, dogs)
 * far over the rest, the mix clipping at 16x full scale (2026-10-13) */
static int reverb_bypassed(const Voice *v) {
	for (int m = 0; m < MAX_MODULES; m++) if (v->params[m] && (RD32(v->params[m]) & 0xffff) == REVERB_PARAMS_ID) return v->bypass[m] != 0;
	return v->bypass[1] != 0;   /* no parameters yet: a reverb bus's reverb is its module 1 */
}
/* the early reflections as a dense cluster: twelve taps per channel over the 28 ms after the reflections delay, uneven,
 * signed and fading (the channels' patterns differ), through two short allpasses per channel that smear them; their
 * energy together is the input's at er_gain. Four close taps of one sign before (2026-10-17 .. 10-28) summed to a copy
 * of the input at up to twice its amplitude: Uncharted's jungle bus answered every sound with one clean repeat (its
 * response peaked at 309 ms, 10 dB over its own reverb tail). */
#define RV_ER_TAPS 12
static const int rv_er_tap[2][RV_ER_TAPS] = { { 0, 89, 197, 263, 379, 461, 587, 683, 811, 947, 1103, 1289 },
                                             { 41, 131, 229, 317, 421, 523, 641, 757, 877, 1009, 1171, 1361 } };
static const signed char rv_er_sign[2][RV_ER_TAPS] = { { 1, -1, 1, 1, -1, 1, -1, -1, 1, -1, 1, -1 }, { -1, 1, 1, -1, 1, -1, -1, 1, -1, 1, 1, -1 } };
static const int rv_er_ap[2][2] = { { 142, 107 }, { 151, 113 } };
static float rv_er_g[2][RV_ER_TAPS];
static void reverb_run(Reverb *r, const ReverbSet *s, const float *in, float *out, uint32_t gn) {
	const float damp = s->damp, dry = s->dry, win = s->wet_in;
	if (!rv_er_g[0][0]) for (int ch = 0; ch < 2; ch++) {   /* the taps' gains: fading 1.5 dB a tap, unit energy */
		float e = 0; for (int t = 0; t < RV_ER_TAPS; t++) { rv_er_g[ch][t] = rv_er_sign[ch][t] * powf(10.0f, -0.075f * (float)t); e += rv_er_g[ch][t] * rv_er_g[ch][t]; }
		for (int t = 0; t < RV_ER_TAPS; t++) rv_er_g[ch][t] /= sqrtf(e);
	}
	for (uint32_t f = 0; f < gn; f++) {
		float x = (in[2 * f] + in[2 * f + 1]) * 0.5f * win;
		float er[2] = { 0, 0 };
		if (s->er_gain > 0) {
			for (int ch = 0; ch < 2; ch++) {
				r->er[ch][r->ei] = in[2 * f + ch];
				float acc = 0;
				for (int t = 0; t < RV_ER_TAPS; t++) { int d = s->er_delay + rv_er_tap[ch][t]; if (d >= RV_ER_LEN) d = RV_ER_LEN - 1; acc += rv_er_g[ch][t] * r->er[ch][(r->ei - d + RV_ER_LEN) % RV_ER_LEN]; }
				for (int k = 0; k < 2; k++) {   /* (Schroeder allpass, g 0.6) */
					float *b = r->erap[ch][k]; int i = r->erai[ch][k], len = rv_er_ap[ch][k];
					float dl = b[i], w = acc + 0.6f * dl; acc = dl - 0.6f * w; b[i] = w;
					r->erai[ch][k] = i + 1 < len ? i + 1 : 0;
				}
				er[ch] = acc * s->er_gain;
			}
			r->ei = (r->ei + 1) % RV_ER_LEN;
		}
		for (int ch = 0; ch < 2; ch++) {
			float acc = 0;
			for (int k = 0; k < RV_COMBS; k++) {
				int len = rv_comb_len[k] + (ch ? rv_spread : 0); float *b = r->comb[ch][k]; int i = r->ci[ch][k];
				float y = b[i];
				r->filt[ch][k] = y * (1 - damp) + r->filt[ch][k] * damp;
				b[i] = x + r->filt[ch][k] * s->fb[k];
				r->ci[ch][k] = i + 1 < len ? i + 1 : 0;
				acc += y;
			}
			for (int k = 0; k < 2; k++) {
				int len = rv_ap_len[k] + (ch ? rv_spread : 0); float *b = r->ap[ch][k]; int i = r->ai[ch][k];
				float y = b[i], o = y - acc;
				b[i] = acc + y * 0.5f;
				r->ai[ch][k] = i + 1 < len ? i + 1 : 0;
				acc = o;
			}
			out[2 * f + ch] = acc + in[2 * f + ch] * dry + er[ch];
		}
	}
}
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Voice voices[512];
static int nvoices;
static Patch patches[1024];
static int npatches;
static uint32_t granularity = 512, sample_rate = 48000;
static int logged;

/* ---- RECOMP_NGS_WAV_DIR=<dir>: the audio taken apart for analysis -- every master bus's granule as
 * float (master<i>.wav, before the s16 clip), every player voice's rendered granules at the system
 * rate (v<i>_out.wav, s16 stereo, only the granules it played) and its decoded source samples before
 * resampling (v<i>_src.wav, s16 at the source channel count, the rate of its first play), with
 * voices.log listing each play's parameters and the events (swap, loop, end) against the sample
 * offsets in those files; at exit a report of the decoders' oddities per voice */
typedef struct { FILE *f; uint32_t bytes, rate, ch, fmt; } Wav;
static const char *dump_dir;
static int dump_init;
static FILE *dump_log;
static uint64_t updates;
typedef struct { Wav out, src; uint32_t plays, src_frames; uint32_t ad_frames, ad_shift13, ad_he, ad_idx_bad, ad_flag7, ad_flag_other, ad_clip, clipped_out; uint32_t last_type, last_ch; float last_freq, last_scal; } VStat;
static VStat vstat[512];
static Wav master_wav[512];
static uint32_t master_clips[512], master_limited[512];   /* clips: samples over full scale before the limiter; limited: frames it took over 0.5 dB off (master_limit) */
static void wav_put(Wav *w, const char *path, uint32_t rate, uint32_t ch, uint32_t fmt /* 1 s16, 3 f32 */, const void *data, uint32_t bytes) {
	if (!w->f) {
		w->f = fopen(path, "wb"); if (!w->f) return;
		w->rate = rate; w->ch = ch; w->fmt = fmt;
		uint32_t bps = fmt == 3 ? 4 : 2, br = rate * ch * bps;
		uint8_t h[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0";
		h[20] = (uint8_t)fmt; h[22] = (uint8_t)ch; memcpy(h + 24, &rate, 4); memcpy(h + 28, &br, 4);
		h[32] = (uint8_t)(ch * bps); h[34] = (uint8_t)(bps * 8); memcpy(h + 36, "data", 4);
		fwrite(h, 1, 44, w->f);
	}
	fwrite(data, 1, bytes, w->f); w->bytes += bytes;
	uint32_t riff = w->bytes + 36;
	off_t at = ftello(w->f); fseeko(w->f, 4, SEEK_SET); fwrite(&riff, 4, 1, w->f); fseeko(w->f, 40, SEEK_SET); fwrite(&w->bytes, 4, 1, w->f); fseeko(w->f, at, SEEK_SET);   /* (64-bit offsets: Windows' long ftell stops at 2 GB) */
}
static void dump_report(void) {
	if (!dump_dir) return;
	for (int i = 0; i < 512; i++) if (master_wav[i].f) fprintf(stderr, "    ngs dump: master voice %d: %u granule samples over full scale before the limiter, %u frames limited\n", i, master_clips[i], master_limited[i]);
	for (int i = 0; i < 512; i++) {
		VStat *st = &vstat[i]; if (!st->plays) continue;
		fprintf(stderr, "    ngs dump: voice %d def %u: %u plays, last type %u ch %u freq %.0f scal %.3f, src frames %u, out clipped %u",
		        i, voices[i].def, st->plays, st->last_type, st->last_ch, st->last_freq, st->last_scal, st->src_frames, st->clipped_out);
		if (st->ad_frames) fprintf(stderr, ", adpcm frames %u shift>12 %u he-filters %u bad-index %u flag7 %u other-flags %u clipped %u", st->ad_frames, st->ad_shift13, st->ad_he, st->ad_idx_bad, st->ad_flag7, st->ad_flag_other, st->ad_clip);
		fprintf(stderr, "\n");
	}
	for (int i = 0; i < 512; i++) { if (vstat[i].out.f) fclose(vstat[i].out.f); if (vstat[i].src.f) fclose(vstat[i].src.f); if (master_wav[i].f) fclose(master_wav[i].f); }
	if (dump_log) fclose(dump_log);
}
static void dump_setup(void) {
	if (dump_init) return; dump_init = 1;
	dump_dir = getenv("RECOMP_NGS_WAV_DIR");
	if (!dump_dir) return;
	char path[1024]; snprintf(path, sizeof path, "%s/voices.log", dump_dir); dump_log = fopen(path, "w");
	atexit(dump_report);
}
#define VIDX(v) ((int)((v) - voices))
static void dump_log_line(Voice *v, const char *what, const char *extra) {
	if (!dump_log) return;
	VStat *st = &vstat[VIDX(v)]; uint32_t pp = v->params[0];
	int at9 = v->def == DEF_SCREAM_AT9;
	float freq, scal; memcpy(&freq, G(pp + (at9 ? A_FREQ : P_FREQ)), 4); memcpy(&scal, G(pp + (at9 ? A_SCALAR : P_SCALAR)), 4);
	fprintf(dump_log, "upd %llu v %d def %u %s buf %u pos %u freq %.1f scal %.4f ch %d type %d start %u lead %u limit %u startbytes %u map %u %u src_off %u out_off %u%s\n",
	        (unsigned long long)updates, VIDX(v), v->def, what, v->buffer, v->byte_pos, freq, scal,
	        (int8_t)RD8(pp + (at9 ? A_CHANNELS : P_CHANNELS)), at9 ? 9 : (int8_t)RD8(pp + P_TYPE), at9 ? 0 : RD8(pp + P_START),
	        at9 ? 0 : RD32(pp + 64), at9 ? 0 : RD32(pp + 68), at9 ? 0 : RD32(pp + 72), at9 ? 0 : RD8(pp + 77), at9 ? 0 : RD8(pp + 78),
	        st->src.ch ? st->src.bytes / (2 * st->src.ch) : 0, st->out.bytes / 4, extra ? extra : "");
	if (at9) for (int i = 0; i < 4; i++) fprintf(dump_log, "    at9 buf %d: data %#x bytes %u loops %d next %d skip %d/%d\n", i, RD32(pp + A_BUF(i)), RD32(pp + A_BUF(i) + 4), (int16_t)RD16(pp + A_BUF(i) + 8), (int16_t)RD16(pp + A_BUF(i) + 10), (int16_t)RD16(pp + A_BUF(i) + 12), (int16_t)RD16(pp + A_BUF(i) + 14));
	else for (int i = 0; i < 4; i++) fprintf(dump_log, "    buf %d: data %#x bytes %u loops %d next %d\n", i, RD32(pp + P_BUF(i)), RD32(pp + P_BUF(i) + 4), (int16_t)RD16(pp + P_BUF(i) + 8), (int16_t)RD16(pp + P_BUF(i) + 10));
	fflush(dump_log);
}
static void dump_src(Voice *v, const int16_t *pcm, uint32_t frames, uint32_t ch, float rate) {
	if (!dump_dir) return;
	VStat *st = &vstat[VIDX(v)]; char path[1024]; snprintf(path, sizeof path, "%s/v%d_src.wav", dump_dir, VIDX(v));
	wav_put(&st->src, path, (uint32_t)rate, ch, 1, pcm, frames * ch * 2); st->src_frames += frames;
}

static uint32_t gobj(uint32_t size) { uint32_t a = guest_alloc(size, 16); memset(G(a), 0, size); return a; }
static Voice *voice_of(uint32_t h) {
	for (int i = 0; i < nvoices; i++) if (voices[i].handle == h) return &voices[i];
	return 0;
}

/* ---- voice definitions: a guest record whose first word says which */
static uint32_t def_obj[16];
static uint32_t defn(int kind) { if (!def_obj[kind]) { def_obj[kind] = gobj(16); WR32(def_obj[kind], kind); } return def_obj[kind]; }
static void h_def_master(Cpu *c) { RET(defn(DEF_MASTER)); }
static void h_def_simple(Cpu *c) { RET(defn(DEF_SIMPLE)); }
static void h_def_reverb(Cpu *c) { RET(defn(DEF_REVERB)); }
static void h_def_scream(Cpu *c) { RET(defn(DEF_SCREAM)); }
static void h_def_scream_at9(Cpu *c) { RET(defn(DEF_SCREAM_AT9)); }
static void h_def_compressor(Cpu *c) { RET(defn(DEF_COMPRESSOR)); }
static void h_def_sidechain(Cpu *c) { RET(defn(DEF_SIDECHAIN)); }

/* ---- system and racks */
static void h_sys_mem(Cpu *c) { WR32(A1, 0x10000); RET(0); }   /* (params, *size) */
static void h_sys_init(Cpu *c) {   /* (memory, ?, SceNgsSystemInitParams*, *handle): maxRacks, maxVoices, granularity, sampleRate, maxModules */
	if (A2) { granularity = RD32(A2 + 8) ? RD32(A2 + 8) : 512; sample_rate = RD32(A2 + 12) ? RD32(A2 + 12) : 48000; }
	fprintf(stderr, "    ngs: system, granularity %u at %u Hz\n", granularity, sample_rate);
	WR32(A3, gobj(16)); RET(0);
}
static void h_rack_mem(Cpu *c) { WR32(A2, 0x1000); RET(0); }   /* (system, description, *size) */
/* (system, buffer info, SceNgsRackDescription*: definition, voices, channels per voice, ..., *rack) */
static void h_rack_init(Cpu *c) {
	uint32_t desc = A2, def = RD32(desc), n = RD32(desc + 4), kind = def ? RD32(def) : DEF_OTHER;
	uint32_t rack = gobj(8 + 4 * n);
	WR32(rack, n);
	pthread_mutex_lock(&lock);
	for (uint32_t i = 0; i < n && nvoices < 512; i++) {
		Voice *v = &voices[nvoices++];
		memset(v, 0, sizeof *v);
		v->handle = gobj(16); v->def = kind; v->rack = rack;
		for (int m = 0; m < MAX_MODULES; m++) v->params[m] = gobj(PARAMS_SIZE);
		if (kind == DEF_MASTER) { v->mix = calloc(2 * 4096, 2); v->acc = calloc(2 * 4096, sizeof(float)); v->playing = 1; }
		else if (kind == DEF_REVERB || PASS_BUS(kind)) { v->acc = calloc(2 * 4096, sizeof(float)); v->playing = 1; if (kind == DEF_REVERB && !v->rv) v->rv = calloc(1, sizeof *v->rv); }
		WR32(rack + 8 + 4 * i, v->handle);
	}
	pthread_mutex_unlock(&lock);
	fprintf(stderr, "    ngs: rack of %u voices, definition %u\n", n, kind);
	WR32(A3, rack); RET(0);
}
static void h_rack_voice(Cpu *c) { uint32_t n = RD32(A0); WR32(A2, A1 < n ? RD32(A0 + 8 + 4 * A1) : 0); RET(A1 < n ? 0 : 0x804a0005u); }

/* ---- patches: (SceNgsPatchSetupInfo*: source voice, output index, sub index, destination, input index, *patch) */
static void h_patch_create(Cpu *c) {
	pthread_mutex_lock(&lock);
	Patch *p = &patches[npatches < 1024 ? npatches++ : 1023];
	p->src = RD32(A0); p->dst = RD32(A0 + 12); p->input = RD32(A0 + 16);
	p->vol[0][0] = p->vol[1][1] = 1; p->vol[0][1] = p->vol[1][0] = 0; p->active = 1;
	pthread_mutex_unlock(&lock);
	dump_setup();
	if (dump_log) { Voice *s = voice_of(p->src), *d = voice_of(p->dst); fprintf(dump_log, "upd %llu patch %d: v %d def %u -> v %d def %u input %u\n", (unsigned long long)updates, (int)(p - patches), s ? VIDX(s) : -1, s ? s->def : 0, d ? VIDX(d) : -1, d ? d->def : 0, p->input); fflush(dump_log); }
	WR32(A1, 0x60000000u + (uint32_t)(p - patches)); RET(0);
}
static Patch *patch_of(uint32_t h) { uint32_t i = h - 0x60000000u; return i < (uint32_t)npatches ? &patches[i] : 0; }
static void h_patch_remove(Cpu *c) { Patch *p = patch_of(A0); if (p) p->active = 0; RET(0); }
static void h_patch_volumes(Cpu *c) {   /* (patch, SceNgsVolumeMatrix*: float m[2][2]) */
	Patch *p = patch_of(A0);
	if (p && A1) for (int i = 0; i < 4; i++) { float f; memcpy(&f, G(A1 + 4 * i), 4); p->vol[i / 2][i % 2] = f; }
	if (p && dump_log) { Voice *s = voice_of(p->src); fprintf(dump_log, "upd %llu patch %d vol: v %d -> v %d  %.3f %.3f / %.3f %.3f\n", (unsigned long long)updates, (int)(p - patches), s ? VIDX(s) : -1, voice_of(p->dst) ? VIDX(voice_of(p->dst)) : -1, p->vol[0][0], p->vol[0][1], p->vol[1][0], p->vol[1][1]); }
	RET(0);
}

/* RECOMP_NGS_VO_LOG=1: a timeline of dialogue voices (DEF_SCREAM_AT9) for lip-sync root-causing --
 * play/keyoff/kill, the first granule that produced audible samples, and every GetStateData poll
 * with its caller (c->r[14]) and the SceNgsPlayerStates fields the game reads. GetStateData can be
 * polled every frame by several callers; past VO_BURST calls from the same lr inside a 5 s window
 * this stops printing a line per call for that caller and relies on the periodic summary instead. */
static int vo_log_level(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_NGS_VO_LOG"); v = e && *e ? atoi(e) : 0; } return v; }   /* 1: everything; 2: starts, ends and kills only (no state polls) */
static int vo_log_on(void) { return vo_log_level() != 0; }
/* the guest functions on this thread's stack, innermost first ("fn_" names of the recompiled code, as the stall profiler
 * reads them): who asked for a sound -- a line said twice shows whether the second request came from the game's own logic
 * or from inside its sound engine */
static void vo_chain(char *out, size_t n) {
	out[0] = 0;
#ifndef _WIN32
	void *pc[96]; const int k = backtrace(pc, 96); size_t at = 0; int m = 0;
	for (int i = 0; i < k && m < 24 && at < n; i++) {
		Dl_info di; if (!dladdr(pc[i], &di) || !di.dli_sname || strncmp(di.dli_sname, "fn_", 3)) continue;
		at += (size_t)snprintf(out + at, n - at, "%s%s", m++ ? " < " : "", di.dli_sname + 3);
	}
#endif
}
/* for a pack's hooks (Killzone's kz_strm_api): the game's call chain as the "vo:" lines give it */
void ngs_chain(char *out, size_t n) { vo_chain(out, n); }
static int at9_log_on(void);   /* RECOMP_NGS_AT9_LOG (render_at9, below) */
static uint8_t at9_polls[512];   /* (RECOMP_NGS_AT9_LOG) GetInfo/GetStateData polls logged since the voice's last play */
static uint64_t vo_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000; }
static uint8_t vo_audible[512];   /* cleared on play, set on the first granule with samples_gen > 0 */
#define VO_BURST 20
typedef struct { uint32_t lr, count; } VoCaller;
static VoCaller vo_callers[32]; static int nvo_callers;
static uint64_t vo_summary_due;
static VoCaller *vo_caller_of(uint32_t lr) {
	for (int i = 0; i < nvo_callers; i++) if (vo_callers[i].lr == lr) return &vo_callers[i];
	if (nvo_callers < 32) { vo_callers[nvo_callers] = (VoCaller){lr, 0}; return &vo_callers[nvo_callers++]; }
	return 0;
}
static void vo_state_log(Voice *v, uint32_t lr, uint64_t us) {
	if (vo_log_level() == 2) return;
	if (!vo_summary_due) vo_summary_due = us + 5000000;
	VoCaller *vc = vo_caller_of(lr);
	uint32_t burst = vc && ++vc->count > VO_BURST;
	if (!burst) fprintf(stderr, "    vo: v%d state us %llu upd %llu lr %#x: bytepos %u buf %u samples_gen %u bytes_consumed %u\n",
	                     VIDX(v), (unsigned long long)us, (unsigned long long)updates, lr, v->byte_pos, v->buffer, v->samples_gen, v->bytes_consumed);
	if (us >= vo_summary_due) {
		for (int i = 0; i < nvo_callers; i++) if (vo_callers[i].count) { fprintf(stderr, "    vo: GetStateData caller %#x: %u calls/5s\n", vo_callers[i].lr, vo_callers[i].count); vo_callers[i].count = 0; }
		vo_summary_due = us + 5000000;
	}
}

/* ---- voices */
static void h_voice_play(Cpu *c) {
	Voice *v = voice_of(A0);
	if (v) { v->playing = 1; v->paused = 0; v->buffer = 0; v->byte_pos = 0; v->samples_gen = v->bytes_consumed = 0; v->loops_done = 0; v->upd_active = 0;
		v->frac = 1; memset(v->rs_h, 0, sizeof v->rs_h); v->rs_i = 0;   /* (1: the first output fetches the first source sample) */
		v->pcm_len = v->pcm_pos = 0; v->at9_cfg = 0; v->stage_on = 0;   /* (a fresh decoder: ATRAC9 frames carry state) */
		v->ad_valid = 0; v->ad_idx = 0; memset(v->ad_hist, 0, sizeof v->ad_hist);
		v->at9_waited = v->at9_decoded = 0; v->env_pt = 0; v->env_ms = 0; v->env_level = 0; v->releasing = v->released_out = 0; v->rel_ms = 0; memset(v->bq, 0, sizeof v->bq);
		uint32_t pp = v->params[0]; if (pp && PCM_PLAYER(v->def)) { v->buffer = (uint8_t)RD8(pp + P_START); if (v->buffer > 3) v->buffer = 0; } }
	dump_setup();
	if (v && dump_dir && PLAYER_VOICE(v->def)) {
		VStat *st = &vstat[VIDX(v)]; uint32_t pp = v->params[0]; int at9 = v->def == DEF_SCREAM_AT9;
		st->plays++; st->last_type = at9 ? 9 : RD8(pp + P_TYPE); st->last_ch = RD8(pp + (at9 ? A_CHANNELS : P_CHANNELS));
		memcpy(&st->last_freq, G(pp + (at9 ? A_FREQ : P_FREQ)), 4); memcpy(&st->last_scal, G(pp + (at9 ? A_SCALAR : P_SCALAR)), 4);
		dump_log_line(v, "play", 0);
	}
	if (v && getenv("RECOMP_NGS_DEBUG")) {
		uint32_t pp = v->params[0];
		fprintf(stderr, "    ngs: play voice %#x def %u params:", A0, v->def);
		for (int i = 0; i < 24; i++) fprintf(stderr, " %08x", RD32(pp + 4 * i));
		fprintf(stderr, "\n");
	}
	if (v && v->def == DEF_SCREAM_AT9) at9_polls[VIDX(v)] = 0;
	if (v && PCM_PLAYER(v->def) && vo_log_on()) vo_audible[VIDX(v)] = 0;
	if (v && v->def == DEF_SCREAM_AT9 && vo_log_on()) {
		vo_audible[VIDX(v)] = 0;
		char chain[400]; vo_chain(chain, sizeof chain);
		fprintf(stderr, "    vo: v%d play handle %#x us %llu upd %llu lr %#x from %s\n", VIDX(v), A0, (unsigned long long)vo_us(), (unsigned long long)updates, c->r[14], chain);
	}
	RET(0);
}
static void h_voice_kill(Cpu *c) {   /* stopped dead: no callback */
	Voice *v = voice_of(A0);
	if (v && v->def == DEF_SCREAM_AT9 && vo_log_on()) fprintf(stderr, "    vo: v%d kill us %llu upd %llu lr %#x\n", VIDX(v), (unsigned long long)vo_us(), (unsigned long long)updates, c->r[14]);
	if (v) { v->playing = 0; v->finishing = 0; v->releasing = v->released_out = 0; }
	RET(0);
}
/* key-off = release: a voice with an envelope module plays on through its release (voice_stages) and
 * finishes when it has run out; without one it stops at once. Either way its finished callback is due
 * at the SystemUpdate where it ends, where the console's NGS calls it (on the thread that updates the
 * system) */
static void h_voice_keyoff(Cpu *c) {
	pthread_mutex_lock(&lock);
	Voice *v = voice_of(A0);
	if (v && v->def == DEF_SCREAM_AT9 && vo_log_on()) fprintf(stderr, "    vo: v%d keyoff us %llu upd %llu lr %#x\n", VIDX(v), (unsigned long long)vo_us(), (unsigned long long)updates, c->r[14]);
	if (v && v->playing && !v->paused && PLAYER_VOICE(v->def) && !stages_off() && stage_module(v, ENV_ID) >= 0) {
		if (!v->releasing) { v->releasing = 1; v->rel_ms = 0; v->rel_from = v->env_level; }
	} else if (v) { if (v->playing && PLAYER_VOICE(v->def)) v->finishing = 1; v->playing = 0; v->releasing = 0; }
	pthread_mutex_unlock(&lock);
	RET(v ? 0 : 0x804a0005u);
}
/* (voice, callback, user data) / (voice, module, callback, user data): kept per voice and module */
static void h_voice_finished_cb(Cpu *c) {
	pthread_mutex_lock(&lock);
	Voice *v = voice_of(A0); if (v) { v->fin_cb = A1; v->fin_ud = A2; }
	pthread_mutex_unlock(&lock);
	RET(v ? 0 : 0x804a0005u);
}
static void h_voice_module_cb(Cpu *c) {
	pthread_mutex_lock(&lock);
	Voice *v = voice_of(A0); int ok = v && A1 < MAX_MODULES;
	if (ok) { v->mod_cb[A1] = A2; v->mod_ud[A1] = A3; }
	pthread_mutex_unlock(&lock);
	RET(ok ? 0 : 0x804a0005u);
}
/* (voice, module, char message[]): the console names a parameter outside its range; the host
 * clamps nothing and reports none -- an empty message */
static void h_params_out_of_range(Cpu *c) { if (A2) WR8(A2, 0); RET(0); }
static void h_voice_pause(Cpu *c) { Voice *v = voice_of(A0); if (v) v->paused = 1; RET(0); }
static void h_voice_resume(Cpu *c) { Voice *v = voice_of(A0); if (v) v->paused = 0; RET(0); }
static void h_voice_bypass(Cpu *c) { Voice *v = voice_of(A0); if (v && A1 < MAX_MODULES) v->bypass[A1] = A2; if (v && getenv("RECOMP_NGS_DEBUG")) fprintf(stderr, "    ngs: bypass voice %#x def %u module %u = %u\n", A0, v->def, A1, A2); RET(0); }
/* (voice, SceNgsVoiceInfo*): state, modules, inputs, outputs, patches per output, and update calls active -- the system
 * updates the voice has played through since it started. Killzone's sound engine (fn 0x81759340) reads a streaming
 * voice's player state only once that count is non-zero, and queues its next chunk from it; the field was never
 * written, the count was whatever its stack held -- mostly 0: its streamed music and ambience ran out after the first
 * 1.3 s chunk and restarted, over and over, the dropship's roar cut off (2026-10-17) */
static void h_voice_info(Cpu *c) {
	Voice *v = voice_of(A0);
	memset(G(A1), 0, 24);
	if (v) { WR32(A1, v->playing ? 1u | (v->paused ? 0x20u : 0) | (v->releasing ? 0x40u : 0) : 0); WR32(A1 + 4, 4);   /* SceNgsVoiceState bits: ACTIVE 1, PAUSED 0x20, KEY_OFF 0x40 (4, written for paused before, is FINALIZE) */ WR32(A1 + 8, 1); WR32(A1 + 12, 1); WR32(A1 + 16, 4); WR32(A1 + 20, v->playing ? v->upd_active : 0); }
	if (v && v->def == DEF_SCREAM_AT9 && at9_log_on() && at9_polls[VIDX(v)] < 12) { at9_polls[VIDX(v)]++; fprintf(stderr, "    at9 v%d upd %llu GetInfo (lr %#x) -> state %u\n", VIDX(v), (unsigned long long)updates, c->r[14], RD32(A1)); }
	RET(0);
}
/* (voice, module, params interface id, SceNgsBufferInfo* out): the module's parameter block */
static void h_voice_lock(Cpu *c) {
	Voice *v = voice_of(A0);
	if (!v || A1 >= MAX_MODULES) { RET(0x804a0005u); return; }
	WR32(A3, v->params[A1]); WR32(A3 + 4, PARAMS_SIZE); RET(0);
}
static void h_voice_unlock(Cpu *c) {
	Voice *v = voice_of(A0);
	if (v && A1 < MAX_MODULES && !PLAYER_VOICE(v->def) && getenv("RECOMP_NGS_DEBUG") && getenv("RECOMP_NGS_DEBUG")[0] == '2') {   /* a bus's parameters, as set through Lock/Unlock */
		uint32_t p = v->params[A1]; fprintf(stderr, "    ngs: unlock voice %d def %u module %u id %#x:", VIDX(v), v->def, A1, RD32(p));
		for (uint32_t k = 8; k < 48; k += 4) { float f; uint32_t w = RD32(p + k); memcpy(&f, &w, 4); fprintf(stderr, " %g/%#x", f, w); }
		fprintf(stderr, "\n");
	}
	if (v && dump_log && v->def == DEF_REVERB && A1 < MAX_MODULES) {   /* the reverb settings set through Lock/Unlock */
		uint32_t p = v->params[A1];
		fprintf(dump_log, "upd %llu v %d reverb unlock module %u id %#x:", (unsigned long long)updates, VIDX(v), A1, RD32(p));
		for (uint32_t k = 8; k < 80; k += 4) { float f; memcpy(&f, G(p + k), 4); fprintf(dump_log, " %g", f); }
		fprintf(dump_log, "\n"); fflush(dump_log);
	}
	if (v && A1 == 0 && v->def == DEF_SIMPLE && logged < 6) {
		logged++;
		uint32_t p = v->params[0];
		fprintf(stderr, "    ngs: player params:");
		for (int i = 0; i < 28; i++) fprintf(stderr, " %08x", RD32(p + 4 * i));
		fprintf(stderr, "\n");
	}
	RET(0);
}
/* (voice, SceNgsModuleParamHeader* block, size, *errors): a run of {module index, channel} headers
 * each followed by a parameter block {id, size, ...} */
static void h_voice_set_block(Cpu *c) {
	Voice *v = voice_of(A0);
	uint32_t at = A1, end = A1 + A2;
	static int dbg = -1; if (dbg < 0) dbg = getenv("RECOMP_NGS_DEBUG") != 0;
	if (dbg && v) fprintf(stderr, "    ngs: set params block voice %#x def %u: %u bytes at %#x\n", A0, v->def, A2, A1);
	while (v && at + 16 <= end) {
		int32_t module = (int32_t)RD32(at);
		uint32_t psize = RD32(at + 12);
		if (dbg) fprintf(stderr, "      entry at +%u: module %d chan %u id %#x size %u%s\n", at - A1, module, RD32(at + 4), RD32(at + 8), psize,
		                 (module < 0 || module >= MAX_MODULES || psize < 8 || psize > PARAMS_SIZE || at + 8 + psize > end) ? "  -- REJECTED, block abandoned" : "");
		if (module < 0 || module >= MAX_MODULES || psize < 8 || psize > PARAMS_SIZE || at + 8 + psize > end) break;
		if (dbg && getenv("RECOMP_NGS_DEBUG")[0] == '2') {   /* =2: every block's words, as float and hex */
			fprintf(stderr, "      words:");
			for (uint32_t k = 8; k < psize; k += 4) { float f; uint32_t w = RD32(at + 8 + k); memcpy(&f, &w, 4); fprintf(stderr, " %g/%#x", f, w); }
			fprintf(stderr, "\n");
		}
		if (module == 0 && v->def == DEF_SCREAM_AT9 && at9_log_on()) {   /* the buffer table as the game changes it: when each chunk is queued and chained */
			uint32_t op = v->params[0], np = at + 8; int changed = 0;
			for (int i = 0; i < 4 && 8 + 16 * (uint32_t)(i + 1) <= psize; i++) if (RD32(op + A_BUF(i)) != RD32(np + A_BUF(i)) || RD32(op + A_BUF(i) + 4) != RD32(np + A_BUF(i) + 4) || RD16(op + A_BUF(i) + 10) != RD16(np + A_BUF(i) + 10)) changed = 1;
			if (changed) {
				fprintf(stderr, "    at9 v%d upd %llu set (lr %#x): playing %d buf %u pos %u |", VIDX(v), (unsigned long long)updates, c->r[14], v->playing, v->buffer, v->byte_pos);
				for (int i = 0; i < 4 && 8 + 16 * (uint32_t)(i + 1) <= psize; i++) fprintf(stderr, " %d:%#x/%u>%d", i, RD32(np + A_BUF(i)), RD32(np + A_BUF(i) + 4), (int16_t)RD16(np + A_BUF(i) + 10));
				fprintf(stderr, "\n");
			}
		}
		memcpy(G(v->params[module]), G(at + 8), psize);
		if (dump_log && v->def == DEF_REVERB) {   /* the game's reverb settings (SceNgsReverbParams: I3DL2 room, levels in millibels, decay in seconds) */
			fprintf(dump_log, "upd %llu v %d reverb params module %d id %#x:", (unsigned long long)updates, VIDX(v), module, RD32(at + 8));
			for (uint32_t k = 8; k < psize && k < 80; k += 4) { float f; memcpy(&f, G(at + 8 + k), 4); fprintf(dump_log, " %g", f); }
			fprintf(dump_log, "\n"); fflush(dump_log);
		}
		at += 8 + psize;
	}
	if (A3) WR32(A3, 0);   /* the error count (SceInt32 *, the 4th argument). Written through a 5th, stack-passed one that doesn't
	                        * exist, it stored 0 wherever the caller's stack word pointed: Killzone's sound engine once in a while
	                        * had half of two links of a playing handle zeroed (prev 0x81c25680 -> 0x5680, next -> 0x81c20000), its
	                        * unlink asserted ("Corrupt handler list"), returned holding ScreamSynth, and the main thread waited on
	                        * that lock for good -- the in-level freeze */
	RET(0);
}

/* ---- callbacks due from an update: queued while rendering (under the lock) and called after it,
 * on the thread that called SystemUpdate -- where the console's NGS calls them -- with a
 * SceNgsCallbackInfo {voice, rack, module id, reason, reason 2, pointer, user data} on its stack.
 * Unlocked, since a callback may call NGS (replay the voice, queue the next buffer). */
typedef struct { uint32_t fn, voice, rack, module, reason, reason2, ptr, ud; } Due;
static Due due[256];
static int ndue;
/* a module's id as the callback reports it: its parameter descriptor's low half (the player's
 * 0x01015ce6 -> 0x5ce6), or the player's own id while no parameters are set */
static uint32_t module_id(Voice *v, int m) {
	uint32_t id = RD32(v->params[m]) & 0xffff;
	return id ? id : m ? 0 : v->def == DEF_SCREAM_AT9 ? 0x5caa : 0x5ce6;
}
/* the player module's events: END_OF_DATA 0, SWAPPED_BUFFER 1 (reason 2 the previous buffer, the
 * pointer the new one's data), LOOPED_BUFFER (2 for PCM, 4 for ATRAC9; reason 2 the loop count),
 * ATRAC9's DECODE_ERROR 3 (reason 2 the byte position) */
enum { EV_END = 0, EV_SWAPPED = 1, EV_DECODE_ERROR = 3 };
#define EV_LOOPED(v) ((v)->def == DEF_SCREAM_AT9 ? 4u : 2u)
static void player_event(Voice *v, uint32_t reason, uint32_t reason2, uint32_t ptr) {
	if (dump_log) { char ex[64]; snprintf(ex, sizeof ex, " reason2 %u ptr %#x", reason2, ptr); dump_log_line(v, reason == EV_END ? "END" : reason == EV_SWAPPED ? "SWAP" : reason == EV_DECODE_ERROR ? "DECODE_ERROR" : "LOOP", ex); }
	if (!v->mod_cb[0] || ndue >= 256) return;
	due[ndue++] = (Due){v->mod_cb[0], v->handle, v->rack, module_id(v, 0), reason, reason2, ptr, v->mod_ud[0]};
}
static void voice_finished(Voice *v) {
	if (!v->fin_cb || ndue >= 256) return;
	due[ndue++] = (Due){v->fin_cb, v->handle, v->rack, module_id(v, 0), 0, 0, 0, v->fin_ud};
}

/* ---- the player (module 0 of a simple voice), layout as FMOD fills it (84 bytes, id 0x01015ce6):
 *   +0 descriptor {id, size}; +8 four buffers {data, bytes, s16 loops (-1 forever), s16 next (-1 none)};
 *   +56 float playback frequency; +60 float scalar; +64 lead-in; +68 sample limit; +72 start bytes;
 *   +76 s8 channels; +77 channel map[2]; +79 s8 type (0 PCM s16); +80 s8 start buffer */
/* one HE-VAG frame (16 bytes, third_party/hevag/hevag_coefs.h) into 28 samples, carrying the
 * channel's history */
#include "third_party/hevag/hevag_coefs.h"
static void hevag_frame(const uint8_t *fr, float hist[4], int16_t out[28]) {
	int index = ((fr[0] >> 4) & 0xf) | (fr[1] & 0xf0), shift = fr[0] & 0xf, flag = fr[1] & 0xf;
	if (index > 127) index = 0;
	for (int i = 0; i < 28; i++) {
		float sample = 0;
		if (flag < 7) {
			int code = (fr[2 + i / 2] >> (i & 1 ? 4 : 0)) & 0xf; if (code & 8) code -= 16;
			sample = (float)((code << 12) >> shift) + hist[0] * hevag_coefs[index][0] + hist[1] * hevag_coefs[index][1] + hist[2] * hevag_coefs[index][2] + hist[3] * hevag_coefs[index][3];
		}
		out[i] = sample > 32767 ? 32767 : sample < -32768 ? -32768 : (int16_t)sample;
		hist[3] = hist[2]; hist[2] = hist[1]; hist[1] = hist[0]; hist[0] = sample;
	}
}
/* ---- resampling: every player's source samples reach the system rate through a 16-tap windowed sinc (Kaiser, beta 7)
 * read at the voice's fractional position, 8 source samples behind the newest one fetched (0.2-0.4 ms). Before
 * 2026-10-28 each output sample was the nearest source sample held, no interpolation: every sound not at the system
 * rate or not at pitch 1 (most effects: 22-32 kHz sources, pitch-varied gunshots) carried its spectrum's images above
 * its Nyquist rate, a gritty, metallic top. Pitched up (step > 1) the cutoff drops with the output's Nyquist rate, in
 * five bands, so what would fold back is filtered too. RECOMP_NGS_RESAMPLE=hold: the old way. */
#define RS_TAPS 16
#define RS_PHASES 128
static const float rs_band_div[5] = { 1.0f, 1.25f, 1.6f, 2.2f, 3.2f };
static float rs_tab[5][RS_PHASES + 1][RS_TAPS];
static int rs_mode = -1;   /* 1: the sinc; 0: hold */
static double bessel_i0(double x) { double s = 1, t = 1; for (int k = 1; k < 32; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; }
static void rs_init(void) {
	if (rs_mode >= 0) return;
	const char *e = getenv("RECOMP_NGS_RESAMPLE"); rs_mode = !(e && !strcmp(e, "hold"));
	const double pi = 3.14159265358979323846, beta = 7.0, i0b = bessel_i0(beta), half = RS_TAPS / 2.0;
	for (int b = 0; b < 5; b++) for (int p = 0; p <= RS_PHASES; p++) {
		double fc = 0.92 / rs_band_div[b], w[RS_TAPS], sum = 0;
		for (int k = 0; k < RS_TAPS; k++) {
			double d = k - (half - 1) - (double)p / RS_PHASES;   /* the tap's distance from the read point, between taps 7 and 8 */
			double u = d / half, win = fabs(u) < 1 ? bessel_i0(beta * sqrt(1 - u * u)) / i0b : 0;
			w[k] = (d == 0 ? fc : sin(pi * fc * d) / (pi * d)) * win; sum += w[k];
		}
		for (int k = 0; k < RS_TAPS; k++) rs_tab[b][p][k] = (float)(w[k] / sum);   /* unity at DC in every phase */
	}
}
static void rs_push(Voice *v, float l, float r) {
	v->rs_i = (v->rs_i + 1) & (RS_TAPS - 1);
	v->rs_h[0][v->rs_i] = v->rs_h[0][v->rs_i + RS_TAPS] = l;   /* (twice: the last 16 are always contiguous) */
	v->rs_h[1][v->rs_i] = v->rs_h[1][v->rs_i + RS_TAPS] = r;
}
static void rs_read(const Voice *v, double step, float *l, float *r) {
	if (!rs_mode) { *l = v->rs_h[0][v->rs_i]; *r = v->rs_h[1][v->rs_i]; return; }   /* hold: the newest sample */
	int b = 0; while (b < 4 && step > rs_band_div[b]) b++;
	double pf = v->frac * RS_PHASES; int p = (int)pf; if (p > RS_PHASES - 1) p = RS_PHASES - 1; if (p < 0) p = 0;
	const float a = (float)(pf - p), *w0 = rs_tab[b][p], *w1 = rs_tab[b][p + 1];
	const float *hl = &v->rs_h[0][v->rs_i + 1], *hr = &v->rs_h[1][v->rs_i + 1];   /* oldest to newest */
	float sl = 0, sr = 0;
	for (int k = 0; k < RS_TAPS; k++) { float w = w0[k] + a * (w1[k] - w0[k]); sl += w * hl[k]; sr += w * hr[k]; }
	*l = sl; *r = sr;
}

/* the Scream/simple player's ADPCM type (SCE_NGS_PLAYER_TYPE_ADPCM, 1): HE-VAG frames of 16 bytes
 * per channel (stereo: a left frame then a right frame), 28 samples each, through the same buffer
 * chain and resampling as PCM. Unit 13's weapon and world effects are all ADPCM -- 563 of its 565
 * effect voices in one session; only PCM was rendered, so its pistol never made a sound and the
 * rifle (PCM) did. */
/* the next source sample of an ADPCM player into *l, *r: 1, or 0 when its data ends */
static int fetch_adpcm(Voice *v, uint32_t pp, int ch, float rate, float *l, float *r) {
	uint32_t frame_bytes = 16u * (uint32_t)ch;
	for (;;) {
		if (v->buffer > 3) return 0;
		uint32_t data = RD32(pp + P_BUF(v->buffer)), bytes = RD32(pp + P_BUF(v->buffer) + 4);
		if (!data || bytes < frame_bytes) return 0;
		if (v->byte_pos + frame_bytes > bytes) {   /* this buffer is done: repeat it, or move to the next (as the PCM path) */
			int16_t loops = (int16_t)RD16(pp + P_BUF(v->buffer) + 8), next = (int16_t)RD16(pp + P_BUF(v->buffer) + 10);
			v->byte_pos = 0; v->ad_idx = 0; v->ad_valid = 0;
			if (loops != 0 && (loops < 0 || v->loops_done < (uint32_t)loops)) { v->loops_done++; player_event(v, EV_LOOPED(v), v->loops_done, data); }
			else {
				uint32_t prev = v->buffer;
				v->loops_done = 0; if (next < 0 || next > 3) return 0; v->buffer = next;
				player_event(v, EV_SWAPPED, prev, RD32(pp + P_BUF(next)));
			}
			continue;
		}
		if (!v->ad_valid || v->ad_frame_buf != v->buffer || v->ad_frame_pos != v->byte_pos) {
			for (int c = 0; c < ch; c++) hevag_frame(G(data + v->byte_pos + 16u * (uint32_t)c), v->ad_hist[c], v->ad_pcm[c]);
			if (dump_dir) {
				VStat *st = &vstat[VIDX(v)];
				for (int c = 0; c < ch; c++) {
					const uint8_t *fr = G(data + v->byte_pos + 16u * (uint32_t)c);
					int index = ((fr[0] >> 4) & 0xf) | (fr[1] & 0xf0), shift = fr[0] & 0xf, flag = fr[1] & 0xf;
					st->ad_frames++; st->ad_shift13 += shift > 12; st->ad_he += index >= 5 && index <= 127; st->ad_idx_bad += index > 127; st->ad_flag7 += flag == 7; st->ad_flag_other += flag != 0 && flag != 7;
					for (int i = 0; i < 28; i++) st->ad_clip += v->ad_pcm[c][i] == 32767 || v->ad_pcm[c][i] == -32768;
				}
				int16_t il[56]; for (int i = 0; i < 28; i++) for (int c = 0; c < ch; c++) il[i * ch + c] = v->ad_pcm[c][i];
				dump_src(v, il, 28, (uint32_t)ch, rate);
			}
			v->ad_valid = 1; v->ad_frame_buf = v->buffer; v->ad_frame_pos = v->byte_pos;
			if (v->ad_idx > 27) v->ad_idx = 0;
		}
		int16_t a = v->ad_pcm[0][v->ad_idx], b = ch == 2 ? v->ad_pcm[1][v->ad_idx] : a;
		*l = a / 32768.0f; *r = b / 32768.0f;
		if (++v->ad_idx >= 28) { v->ad_idx = 0; v->byte_pos += frame_bytes; v->bytes_consumed += frame_bytes; }
		v->samples_gen += (uint32_t)ch;   /* source samples, all channels (h_voice_state) */
		return 1;
	}
}
/* the next source sample of a PCM player: 1, or 0 when its data ends */
static int fetch_pcm(Voice *v, uint32_t pp, int ch, float rate, float *l, float *r) {
	uint32_t frame_bytes = 2u * (uint32_t)ch;
	for (;;) {
		if (v->buffer > 3) return 0;
		uint32_t data = RD32(pp + P_BUF(v->buffer)), bytes = RD32(pp + P_BUF(v->buffer) + 4);
		if (!data || bytes < frame_bytes) return 0;
		if (v->byte_pos + frame_bytes > bytes) {   /* this buffer is done: repeat it, or move to the next */
			int16_t loops = (int16_t)RD16(pp + P_BUF(v->buffer) + 8), next = (int16_t)RD16(pp + P_BUF(v->buffer) + 10);
			v->byte_pos = 0;
			if (loops != 0 && (loops < 0 || v->loops_done < (uint32_t)loops)) { v->loops_done++; player_event(v, EV_LOOPED(v), v->loops_done, data); }
			else {
				uint32_t prev = v->buffer;
				v->loops_done = 0; if (next < 0 || next > 3) return 0; v->buffer = next;
				player_event(v, EV_SWAPPED, prev, RD32(pp + P_BUF(next)));
			}
			continue;
		}
		int16_t a = (int16_t)RD16(data + v->byte_pos), b = ch == 2 ? (int16_t)RD16(data + v->byte_pos + 2) : a;
		*l = a / 32768.0f; *r = b / 32768.0f;
		if (dump_dir) dump_src(v, (const int16_t *)G(data + v->byte_pos), 1, (uint32_t)ch, rate);
		v->byte_pos += frame_bytes; v->bytes_consumed += frame_bytes;
		v->samples_gen += (uint32_t)ch;   /* source samples, all channels (h_voice_state) */
		return 1;
	}
}
static void render_player(Voice *v, float *out /* stereo, granularity frames */) {
	uint32_t pp = v->params[0];
	int ch = (int8_t)RD8(pp + P_CHANNELS);
	if (ch < 1 || ch > 2) ch = 1;
	int type = (int8_t)RD8(pp + P_TYPE);
	if (type != 0 && type != 1) { static int said; if (!said++) fprintf(stderr, "    ngs: player type %d not rendered\n", type); return; }
	float freq; memcpy(&freq, G(pp + P_FREQ), 4);
	float scal; memcpy(&scal, G(pp + P_SCALAR), 4);
	if (!(freq > 0)) freq = (float)sample_rate;
	if (!(scal > 0)) scal = 1;
	double step = freq * scal / sample_rate;
	rs_init();
	for (uint32_t f = 0; f < granularity; f++) {
		while (v->frac >= 1.0) {   /* past the newest source sample: the next one */
			float l, r;
			if (!(type == 1 ? fetch_adpcm(v, pp, ch, freq * scal, &l, &r) : fetch_pcm(v, pp, ch, freq * scal, &l, &r))) { v->playing = 0; return; }
			rs_push(v, l, r); v->frac -= 1.0;
		}
		float l, r; rs_read(v, step, &l, &r);
		out[2 * f] += l; out[2 * f + 1] += r;
		v->frac += step;
	}
}

/* an ATRAC9 player: frames decoded from the game's buffers (LibAtrac9), following their loop /
 * next chaining like the PCM player, resampled to the system rate. The configuration word holds the
 * codec's four header bytes (0xFE, sample rate and channels, frame size, superframe) */
static int at9_log_on(void) { static int on = -1; if (on < 0) on = getenv("RECOMP_NGS_AT9_LOG") != 0; return on; }
static void at9_log(Voice *v, uint32_t pp, uint32_t prev, const char *what) {
	if (!at9_log_on()) return;
	fprintf(stderr, "    at9 v%d upd %llu %s %u->%u: bytes %u discard %d/%d next-bytes %u | out %u decoded %llu waited %llu\n", VIDX(v), (unsigned long long)updates, what, prev, v->buffer,
	        RD32(pp + A_BUF(prev) + 4), (int16_t)RD16(pp + A_BUF(prev) + 12), (int16_t)RD16(pp + A_BUF(prev) + 14), RD32(pp + A_BUF(v->buffer) + 4),
	        v->samples_gen, (unsigned long long)v->at9_decoded, (unsigned long long)v->at9_waited);
}
/* the next source sample of an ATRAC9 player: 1, 0 when its data ends (or fails), 2 while it waits for the game to
 * queue more */
static int fetch_at9(Voice *v, uint32_t pp, float rate, float *l, float *r) {
	while (v->pcm_pos >= v->pcm_len) {   /* the next frame */
		int used = 0, e;
		if (v->stage_on) {   /* inside a superframe joined from two buffers */
			e = Atrac9Decode(v->at9, v->stage + v->stage_pos, v->pcm, &used);
			if (e) { fprintf(stderr, "    ngs: ATRAC9 frame (joined superframe) failed (%d)\n", e); player_event(v, EV_DECODE_ERROR, v->byte_pos, 0); return 0; }
			v->stage_pos += used;
			if (++v->at9_frame >= v->at9_sf_frames) { v->at9_frame = 0; v->stage_on = 0; }
			v->pcm_len = v->at9_samples; v->pcm_pos = 0;
			if (dump_dir) dump_src(v, v->pcm, (uint32_t)v->at9_samples, (uint32_t)v->at9_ch, rate);
			continue;
		}
		if (v->buffer > 3) return 0;
		uint32_t data = RD32(pp + A_BUF(v->buffer)), bytes = RD32(pp + A_BUF(v->buffer) + 4);
		if (!data || !bytes) return 2;   /* waiting for the game to queue data */
		if (v->byte_pos >= bytes) {   /* this buffer is done: repeat it, or move to the next */
			int16_t loops = (int16_t)RD16(pp + A_BUF(v->buffer) + 8), next = (int16_t)RD16(pp + A_BUF(v->buffer) + 10);
			v->byte_pos = 0; v->at9_frame = 0;
			if (loops != 0 && (loops < 0 || v->loops_done < (uint32_t)loops)) { v->loops_done++; player_event(v, EV_LOOPED(v), v->loops_done, data); }
			else {
				uint32_t prev = v->buffer;
				v->loops_done = 0; if (next < 0 || next > 3) return 0; v->buffer = next;
				player_event(v, EV_SWAPPED, prev, RD32(pp + A_BUF(next)));
				at9_log(v, pp, prev, "swap");
			}
			continue;
		}
		if (v->at9_frame == 0 && v->byte_pos + (uint32_t)v->at9_sf_bytes > bytes && v->at9_sf_bytes <= (int)sizeof v->stage) {
			/* the superframe continues in the next buffer: join its two parts, then decode from the join */
			uint32_t part = bytes - v->byte_pos;
			memcpy(v->stage, G(data + v->byte_pos), part);
			int16_t loops = (int16_t)RD16(pp + A_BUF(v->buffer) + 8), next = (int16_t)RD16(pp + A_BUF(v->buffer) + 10);
			if (loops != 0 && (loops < 0 || v->loops_done < (uint32_t)loops)) { v->loops_done++; player_event(v, EV_LOOPED(v), v->loops_done, data); }
			else {
				uint32_t prev = v->buffer;
				v->loops_done = 0; if (next < 0 || next > 3) return 0; v->buffer = next;
				player_event(v, EV_SWAPPED, prev, RD32(pp + A_BUF(next)));
				at9_log(v, pp, prev, "swap (joined)");
			}
			uint32_t ndata = RD32(pp + A_BUF(v->buffer));
			if (!ndata) return 2;
			memcpy(v->stage + part, G(ndata), (size_t)v->at9_sf_bytes - part);
			v->byte_pos = (uint32_t)v->at9_sf_bytes - part; v->bytes_consumed += (uint32_t)v->at9_sf_bytes;
			v->stage_on = 1; v->stage_pos = 0;
			continue;
		}
		if (v->at9_frame == 0) v->at9_sf_start = v->byte_pos;
		e = Atrac9Decode(v->at9, G(data + v->byte_pos), v->pcm, &used);
		if (e || used <= 0) {
			fprintf(stderr, "    ngs: ATRAC9 frame at %#x+%#x (buffer %u of %u bytes) failed (%d)\n", data, v->byte_pos, v->buffer, bytes, e);
			player_event(v, EV_DECODE_ERROR, v->byte_pos, data); return 0;
		}
		/* within a superframe the next frame follows this one; after its last, the next superframe starts */
		uint32_t was = v->byte_pos;
		if (++v->at9_frame >= v->at9_sf_frames) { v->at9_frame = 0; v->byte_pos = v->at9_sf_start + v->at9_sf_bytes; }
		else v->byte_pos += used;
		v->bytes_consumed += v->byte_pos - was;
		v->pcm_len = v->at9_samples; v->pcm_pos = 0; v->at9_decoded += (uint64_t)v->at9_samples;
		if (dump_dir) dump_src(v, v->pcm, (uint32_t)v->at9_samples, (uint32_t)v->at9_ch, rate);
	
	}
	int16_t a = v->pcm[v->pcm_pos * v->at9_ch], b = v->at9_ch > 1 ? v->pcm[v->pcm_pos * v->at9_ch + 1] : a;
	*l = a / 32768.0f; *r = b / 32768.0f;
	v->pcm_pos++; v->samples_gen += (uint32_t)v->at9_ch;   /* source samples, all channels (h_voice_state) */
	return 1;
}
static void render_at9(Voice *v, float *out) {
	uint32_t pp = v->params[0], cfg = RD32(pp + A_CONFIG);
	uint8_t b[4] = {cfg & 0xff, cfg >> 8 & 0xff, cfg >> 16 & 0xff, cfg >> 24};
	if (b[0] != 0xfe) return;   /* no parameters yet: a voice may be played before they're set */
	if (!v->at9 || v->at9_cfg != cfg) {
		if (v->at9) Atrac9ReleaseHandle(v->at9);
		v->at9 = Atrac9GetHandle();
		int e = Atrac9InitDecoder(v->at9, b);
		if (e) { fprintf(stderr, "    ngs: ATRAC9 configuration %02x %02x %02x %02x rejected (%d)\n", b[0], b[1], b[2], b[3], e); Atrac9ReleaseHandle(v->at9); v->at9 = 0; v->playing = 0; return; }
		Atrac9CodecInfo ci; Atrac9GetCodecInfo(v->at9, &ci);
		v->at9_ch = ci.channels; v->at9_samples = ci.frameSamples; v->at9_cfg = cfg;
		v->at9_sf_bytes = ci.superframeSize; v->at9_sf_frames = ci.framesInSuperframe; v->at9_frame = 0;
		v->pcm = realloc(v->pcm, sizeof(int16_t) * ci.frameSamples * ci.channels);
		v->pcm_len = v->pcm_pos = 0;
	}
	float freq; memcpy(&freq, G(pp + A_FREQ), 4);
	float scal; memcpy(&scal, G(pp + A_SCALAR), 4);
	if (!(freq > 0)) freq = (float)sample_rate;
	if (!(scal > 0)) scal = 1;
	double step = freq * scal / sample_rate;
	rs_init();
	for (uint32_t f = 0; f < granularity; f++) {
		while (v->frac >= 1.0) {   /* past the newest source sample: the next one */
			float l, r; int st = fetch_at9(v, pp, freq * scal, &l, &r);
			if (st == 0) { v->playing = 0; return; }
			if (st == 2) { v->at9_waited += granularity - f; return; }
			rs_push(v, l, r); v->frac -= 1.0;
		}
		float l, r; rs_read(v, step, &l, &r);
		out[2 * f] += l; out[2 * f + 1] += r;
		v->frac += step;
	}
}

/* ---- a player voice's modules after the player, found by their parameter block ids (Uncharted's
 * Scream voices: 1 envelope 0x5CE3, 2 distortion 0x5CE2, 3 equalizer 0x5CEC, 4 filter 0x5CE4, 5 pauser
 * 0x5CE5), each skipped while bypassed. None ran before 2026-10-09: every sound played at its
 * patch level alone -- ambience loops at full volume from their first sample (their envelopes fade
 * them in over 0.5-2 s) and at full bandwidth (distance and occlusion are the equalizer's low-pass,
 * down to 3 kHz), cut dead at key-off (the envelope's release fades them over ~1.7 s): background
 * effects over the dialogue. Layouts as Vita3K's headers give them (its modules don't run these
 * either); the curve shapes and the filters' resonance-to-Q mapping are this file's choices.
 *   envelope {desc; 4 x {u32 ms to the next point, f32 amplitude, u32 curve (0 linear, 1 curved)};
 *             u32 release ms; u32 points; u32 loop start; s32 loop end (-1 none)}
 *   distortion {desc; f32 a, b, clip, gate, wet gain, dry gain}   (wet: not modelled, dry passes)
 *   equalizer {desc; 4 x {u32 mode, f32 frequency, f32 resonance, f32 gain}}, filter {desc; one of them}
 *   pauser {desc; u32 lead-in, lead-out}   (pause/resume ramps: not modelled) */
#define DIST_ID 0x5ce2
#define EQ_ID 0x5cec
#define FILT_ID 0x5ce4
typedef struct { float b0, b1, b2, a1, a2; int on; } Biquad;
/* SceNgsParamFilterMode -> RBJ cookbook coefficients at the system rate; resonance is taken as Q, gain
 * (peak / shelves) in dB. A low-pass at or above 0.45 of the rate, or a mode it can't be, is off. */
static Biquad biquad_of(uint32_t mode, float freq, float res, float gain_db) {
	Biquad q = {0};
	float fs = (float)sample_rate;
	if (!mode || !(freq > 0) || freq >= 0.49f * fs) return q;
	if ((mode == 1 || mode == 12 || mode == 9) && freq >= 0.45f * fs) return q;
	float Q = res > 0.1f ? (res < 30 ? res : 30) : 0.1f, w0 = 2 * (float)M_PI * freq / fs, cw = cosf(w0), sw = sinf(w0), al = sw / (2 * Q);
	float A = powf(10.0f, (gain_db > -60 && gain_db < 60 ? gain_db : 0) / 40.0f);
	float b0, b1, b2, a0, a1, a2;
	switch (mode) {
	case 1: case 12: b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;   /* LOWPASS_RESONANT (_NORMALIZED) */
	case 2: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;       /* HIGHPASS_RESONANT */
	case 3: b0 = Q * al; b1 = 0; b2 = -Q * al; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;               /* BANDPASS_PEAK (peak gain Q) */
	case 4: b0 = al; b1 = 0; b2 = -al; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;                       /* BANDPASS_ZERO (0 dB peak) */
	case 5: b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;                    /* NOTCH */
	case 6: b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A; break;   /* PEAK */
	case 7: case 8: {   /* HIGHSHELF / LOWSHELF */
		float sq = 2 * sqrtf(A) * al, s = mode == 7 ? 1.f : -1.f;
		b0 = A * ((A + 1) + s * (A - 1) * cw + sq); b1 = -2 * s * A * ((A - 1) + s * (A + 1) * cw); b2 = A * ((A + 1) + s * (A - 1) * cw - sq);
		a0 = (A + 1) - s * (A - 1) * cw + sq; a1 = 2 * s * ((A - 1) - s * (A + 1) * cw); a2 = (A + 1) - s * (A - 1) * cw - sq; break; }
	case 9: case 10: {   /* LOWPASS / HIGHPASS_ONEPOLE (as a biquad with b2 = a2 = 0) */
		float k = expf(-w0); a0 = 1; a1 = -k; a2 = 0;
		if (mode == 9) { b0 = 1 - k; b1 = 0; } else { b0 = (1 + k) / 2; b1 = -(1 + k) / 2; }
		b2 = 0; break; }
	case 11: b0 = 1 - al; b1 = -2 * cw; b2 = 1 + al; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;         /* ALLPASS */
	default: return q;
	}
	q.b0 = b0 / a0; q.b1 = b1 / a0; q.b2 = b2 / a0; q.a1 = a1 / a0; q.a2 = a2 / a0; q.on = 1;
	return q;
}
static void biquad_run(const Biquad *q, float st[2][2], float *buf, uint32_t gn) {
	for (int ch = 0; ch < 2; ch++) {
		float z1 = st[ch][0], z2 = st[ch][1];
		for (uint32_t f = 0; f < gn; f++) {   /* transposed direct form II */
			float x = buf[2 * f + ch], y = q->b0 * x + z1;
			z1 = q->b1 * x - q->a1 * y + z2; z2 = q->b2 * x - q->a2 * y;
			buf[2 * f + ch] = y;
		}
		st[ch][0] = fabsf(z1) < 1e-20f ? 0 : z1; st[ch][1] = fabsf(z2) < 1e-20f ? 0 : z2;   /* no denormal tails */
	}
}
static int stage_module(const Voice *v, uint32_t id) {   /* the module index carrying parameter block `id`, not bypassed; -1 if none */
	for (int m = 1; m < MAX_MODULES; m++) if (v->params[m] && (RD32(v->params[m]) & 0xffff) == id && !v->bypass[m]) return m;
	return -1;
}
static float curve(float t, uint32_t type) { return type == 1 ? 0.5f - 0.5f * cosf((float)M_PI * t) : t; }   /* curved: an S ease (the console's shape unknown) */
/* the envelope's level after `ms` more milliseconds (its own state in v); the release when keyed off */
static float env_advance(Voice *v, uint32_t pp, float ms) {
	uint32_t n = RD32(pp + 8 + 48 + 4); if (n < 1) n = 1; if (n > 4) n = 4;
	int32_t loop_start = (int32_t)RD32(pp + 8 + 48 + 8), loop_end = (int32_t)RD32(pp + 8 + 48 + 12);
	#define EP_MS(i) RD32(pp + 8 + 12 * (i))
	#define EP_AMP(i) ({ float a_; memcpy(&a_, G(pp + 8 + 12 * (i) + 4), 4); a_; })
	#define EP_CURVE(i) RD32(pp + 8 + 12 * (i) + 8)
	if (v->releasing) {
		float rel = (float)RD32(pp + 8 + 48);
		v->rel_ms += ms;
		if (rel <= 0 || v->rel_ms >= rel) { v->env_level = 0; v->released_out = 1; return 0; }
		return v->env_level = v->rel_from * (1 - curve(v->rel_ms / rel, EP_CURVE(n - 1)));
	}
	v->env_ms += ms;
	for (int guard = 0; guard < 8; guard++) {
		int i = v->env_pt;
		if (i >= (int)n - 1) {   /* the last point: hold, or loop back */
			if (loop_end >= 0 && loop_start >= 0 && loop_start < loop_end && loop_end < (int)n) { v->env_pt = loop_start; continue; }
			return v->env_level = EP_AMP(n - 1);
		}
		float seg = (float)EP_MS(i);
		if (v->env_ms < seg) return v->env_level = EP_AMP(i) + (EP_AMP(i + 1) - EP_AMP(i)) * curve(v->env_ms / seg, EP_CURVE(i));
		v->env_ms -= seg; v->env_pt = i + 1;
		if (loop_end >= 0 && v->env_pt > loop_end && loop_start >= 0 && loop_start < loop_end) v->env_pt = loop_start;
	}
	return v->env_level = EP_AMP(v->env_pt < (int)n ? v->env_pt : (int)n - 1);
	#undef EP_MS
	#undef EP_AMP
	#undef EP_CURVE
}
static int stages_off(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_NGS_NO_STAGES") != 0; return v; }
static void voice_stages(Voice *v, float *buf, uint32_t gn) {
	if (stages_off()) return;
	int m = stage_module(v, ENV_ID);
	if (m >= 0) {
		uint32_t pp = v->params[m];
		const float step = 1000.0f / (float)sample_rate;
		for (uint32_t f = 0; f < gn; f++) {
			float g = env_advance(v, pp, step);
			buf[2 * f] *= g; buf[2 * f + 1] *= g;
			if (v->released_out) { for (uint32_t k = f + 1; k < gn; k++) buf[2 * k] = buf[2 * k + 1] = 0; break; }
		}
	} else if (v->releasing) v->released_out = 1;   /* no envelope: the release is instant */
	if ((m = stage_module(v, DIST_ID)) >= 0) {
		float wet, dry; memcpy(&wet, G(v->params[m] + 8 + 16), 4); memcpy(&dry, G(v->params[m] + 8 + 20), 4);
		if (wet != 0) { static int said; if (!said++) fprintf(stderr, "    ngs: distortion wet gain %g not modelled (dry only)\n", wet); }
		if (dry == dry && dry != 1) for (uint32_t k = 0; k < 2 * gn; k++) buf[k] *= dry;
	}
	if ((m = stage_module(v, EQ_ID)) >= 0)
		for (int k = 0; k < 4; k++) {
			uint32_t f = v->params[m] + 8 + 16 * (uint32_t)k; float fr, re, ga; memcpy(&fr, G(f + 4), 4); memcpy(&re, G(f + 8), 4); memcpy(&ga, G(f + 12), 4);
			Biquad q = biquad_of(RD32(f), fr, re, ga);
			if (q.on) biquad_run(&q, v->bq[k], buf, gn);
		}
	if ((m = stage_module(v, FILT_ID)) >= 0) {
		uint32_t f = v->params[m] + 8; float fr, re, ga; memcpy(&fr, G(f + 4), 4); memcpy(&re, G(f + 8), 4); memcpy(&ga, G(f + 12), 4);
		Biquad q = biquad_of(RD32(f), fr, re, ga);
		if (q.on) biquad_run(&q, v->bq[4], buf, gn);
	}
}

/* a voice's (stereo) output through every active patch out of it, into the destinations' inputs */
static void route(Voice *v, const float *out, uint32_t gn) {
	for (int p = 0; p < npatches; p++) {
		if (!patches[p].active || patches[p].src != v->handle) continue;
		Voice *d = voice_of(patches[p].dst);
		if (!d || !d->acc) continue;
		if (d->def == DEF_SIDECHAIN && patches[p].input > 0) continue;   /* the key input: it only steers the (unmodelled) gain */
		for (uint32_t f = 0; f < gn; f++) {
			float l = out[2 * f], r = out[2 * f + 1];
			d->acc[2 * f] += l * patches[p].vol[0][0] + r * patches[p].vol[1][0];
			d->acc[2 * f + 1] += l * patches[p].vol[0][1] + r * patches[p].vol[1][1];
		}
	}
}

/* the master's limiter, before the s16 output: peaks over -1 dBFS are turned down instead of clipped. The gain follows
 * what each sample needs over the next 1 ms of the granule (look-ahead inside it, no added latency), falls with a fast
 * attack and recovers over 80 ms; anything still over the ceiling (a peak at a granule's very start) bends into it
 * softly. Before 2026-10-28 the mix was cut at full scale: loud scenes crackled (Uncharted's mix once reached 16x full
 * scale, 2026-10-13). RECOMP_NGS_LIMITER=0: the hard clip. */
static void master_limit(Voice *v, float *acc, uint32_t gn) {
	static int off = -1; if (off < 0) { const char *e = getenv("RECOMP_NGS_LIMITER"); off = e && !strcmp(e, "0"); }
	if (off || gn > 4096) return;
	const float ceil = 0.891f;   /* -1 dBFS */
	const int la = (int)(sample_rate / 1000);
	const float att = 1 - expf(-4.0f / (float)la), rel = 1 - expf(-1.0f / (0.08f * (float)sample_rate));
	static float need[4096];
	for (uint32_t k = 0; k < gn; k++) { float p = fmaxf(fabsf(acc[2 * k]), fabsf(acc[2 * k + 1])); need[k] = p > ceil ? ceil / p : 1; }
	float g = 1 - v->lim_red;
	for (uint32_t k = 0; k < gn; k++) {
		float t = need[k]; for (uint32_t j = k + 1; j < gn && j < k + (uint32_t)la; j++) if (need[j] < t) t = need[j];
		g += (t < g ? att : rel) * (t - g);
		if (g < 0.944f) master_limited[VIDX(v)]++;   /* (more than 0.5 dB down) */
		for (int c = 0; c < 2; c++) {
			float y = acc[2 * k + c] * g, m = fabsf(y);
			if (m > ceil) y = copysignf(ceil + (1 - ceil) * tanhf((m - ceil) / (1 - ceil)), y);
			acc[2 * k + c] = y;
		}
	}
	v->lim_red = 1 - g;
}

static void h_system_update(Cpu *c) {
	/* the mixing is the DSP's work on the hardware, not the calling thread's: under the scheduler
	 * (RECOMP_SCHED=vita) the thread gives its run slot up for it, and takes one back before the
	 * guest callbacks below */
	int b = sched_block_begin();
	pthread_mutex_lock(&lock);
	dump_setup(); updates++;
	static float tmp[2 * 4096];
	uint32_t gn = granularity > 4096 ? 4096 : granularity;
	for (int i = 0; i < nvoices; i++) if (voices[i].acc) memset(voices[i].acc, 0, sizeof(float) * 2 * gn);
	for (int i = 0; i < nvoices; i++) if (voices[i].playing && !voices[i].paused) voices[i].upd_active++;   /* (h_voice_info) */
	for (int i = 0; i < nvoices; i++) {
		Voice *v = &voices[i];
		if (!v->playing || v->paused) continue;
		if (!PCM_PLAYER(v->def) && v->def != DEF_SCREAM_AT9) continue;
		memset(tmp, 0, sizeof(float) * 2 * gn);
		int had = ndue;
		if (v->def == DEF_SCREAM_AT9) render_at9(v, tmp); else render_player(v, tmp);
		if ((v->def == DEF_SCREAM_AT9 || PCM_PLAYER(v->def)) && vo_log_on() && !vo_audible[i] && v->samples_gen) {
			vo_audible[i] = 1;
			const int at9 = v->def == DEF_SCREAM_AT9;
			const uint32_t pp = v->params[0], b0 = RD32(pp + (at9 ? A_BUF(0) : P_BUF(0))), n0 = RD32(pp + (at9 ? A_BUF(0) : P_BUF(0)) + 4); uint32_t h = 2166136261u;   /* (FNV-1a of its first 256 bytes: the same sound started twice hashes the same) */
			for (uint32_t k = 0; b0 && k < 256 && k < n0; k++) h = (h ^ RD8(b0 + k)) * 16777619u;
			extern uint64_t display_frames(void);
			fprintf(stderr, "    vo: v%d def %u first audible us %llu upd %llu frame %llu samples_gen %u data %#x/%u hash %08x\n", i, v->def, (unsigned long long)vo_us(), (unsigned long long)updates, (unsigned long long)display_frames(), v->samples_gen, b0, n0, h);
		}
		voice_stages(v, tmp, gn);
		if (v->released_out) {   /* keyed off and its release has run out: finished, no END_OF_DATA */
			route(v, tmp, gn);
			v->playing = 0; v->releasing = v->released_out = 0; v->finishing = 1;
			continue;
		}
		if (dump_dir) {
			VStat *st = &vstat[i]; static int16_t o16[2 * 4096]; char path[1024]; snprintf(path, sizeof path, "%s/v%d_out.wav", dump_dir, i);
			for (uint32_t k = 0; k < 2 * gn; k++) { float s = tmp[k] * 32767.0f; st->clipped_out += s > 32767 || s < -32768; o16[k] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s); }
			wav_put(&st->out, path, sample_rate, 2, 1, o16, 2 * gn * 2);
			/* which update each granule of v<i>_out.wav came from (u32 each): the per-voice files hold only the granules played */
			static FILE *uf[512]; if (!uf[i]) { snprintf(path, sizeof path, "%s/v%d_upd.u32", dump_dir, i); uf[i] = fopen(path, "wb"); }
			if (uf[i]) { uint32_t u = (uint32_t)updates; fwrite(&u, 4, 1, uf[i]); fflush(uf[i]); }
		}
		route(v, tmp, gn);
		if (!v->playing && v->def == DEF_SCREAM_AT9) at9_log(v, v->params[0], v->buffer, "stopped");
		if (!v->playing) {   /* it ran out of data (or failed) during this granule: END_OF_DATA unless the decoder already said why */
			int failed = 0; for (int k = had; k < ndue; k++) failed |= due[k].voice == v->handle && due[k].reason == EV_DECODE_ERROR && v->def == DEF_SCREAM_AT9;
			if (!failed) player_event(v, EV_END, 0, 0);
			if (vo_log_on() && v->def == DEF_SCREAM_AT9) fprintf(stderr, "    vo: v%d callbacks at its end: module %#x finished %#x (queued %d)\n", i, v->mod_cb[0], v->fin_cb, ndue);
			if (v->def == DEF_SCREAM_AT9) at9_polls[i] = 0;   /* (RECOMP_NGS_AT9_LOG: the engine's next polls -- how it takes the end, e.g. a line it then plays again) */
			if (vo_log_on() && (v->def == DEF_SCREAM_AT9 || PCM_PLAYER(v->def))) { extern uint64_t display_frames(void); fprintf(stderr, "    vo: v%d def %u ended (out of data%s) frame %llu us %llu samples_gen %u\n", i, v->def, failed ? ", a decode error" : "", (unsigned long long)display_frames(), (unsigned long long)vo_us(), v->samples_gen); }
			v->finishing = 1;
		}
	}
	for (int i = 0; i < nvoices; i++) if (voices[i].finishing) { voices[i].finishing = 0; voice_finished(&voices[i]); }
	/* buses forward what reached them (voice -> compressor -> master; voice -> reverb -> compressor):
	 * the compressor passes through, the reverb bus runs its room (reverb_run) over what it received.
	 * Each bus runs ONCE per update, after every bus that feeds it (a voice's send is already in the
	 * accumulator): this was a three-pass forwarding loop that ran the reverb's room in every pass, so
	 * three granules of its tail -- at t, t+512 and t+1024 -- were summed into every output granule
	 * and the tail was spent three times too fast. Under every reverberant sound in Unit 13 the mix
	 * jumped at every granule boundary (clicks of 0.6-0.75 full scale, 93.75 Hz): the "staticy
	 * channel". A feedback loop between buses, should a game patch one, is cut in array order. */
	static float rvout[2 * 4096];
	int bus[64], nb = 0, done[64] = {0}, feeds[64][64] = {{0}};
	for (int i = 0; i < nvoices && nb < 64; i++) if (PASS_BUS(voices[i].def) || voices[i].def == DEF_REVERB) bus[nb++] = i;
	for (int a = 0; a < nb; a++)
		for (int p = 0; p < npatches; p++) {
			if (!patches[p].active || patches[p].src != voices[bus[a]].handle) continue;
			Voice *d = voice_of(patches[p].dst);
			if (d && d != &voices[bus[a]] && d->def == DEF_SIDECHAIN && patches[p].input > 0) continue;   /* the key input steers no audio */
			if (d) for (int b = 0; b < nb; b++) if (bus[b] == VIDX(d) && a != b) feeds[a][b] = 1;
		}
	for (int left = nb, cycle = 0; left > 0;) {
		int progressed = 0;
		for (int b = 0; b < nb; b++) {
			if (done[b]) continue;
			int wait = 0;
			for (int a = 0; a < nb && !wait; a++) wait = feeds[a][b] && !done[a];
			if (wait && !cycle) continue;
			Voice *v = &voices[bus[b]];
			if (PASS_BUS(v->def)) route(v, v->acc, gn);
			else if (v->rv && reverb_bypassed(v)) { memset(v->rv, 0, sizeof *v->rv); route(v, v->acc, gn); }   /* (its tail cleared: nothing stale when the game turns it back on) */
			else if (v->rv) {
				if (dump_dir) { char path[1024]; snprintf(path, sizeof path, "%s/rv%d_in_pass0.wav", dump_dir, bus[b]); static Wav rin[512]; wav_put(&rin[bus[b]], path, sample_rate, 2, 3, v->acc, 2 * gn * 4); }
				ReverbSet rs; reverb_settings(v->params, sample_rate, &rs);
				reverb_run(v->rv, &rs, v->acc, rvout, gn); route(v, rvout, gn);
				if (dump_dir) { char path[1024]; snprintf(path, sizeof path, "%s/rv%d_out_pass0.wav", dump_dir, bus[b]); static Wav rout[512]; wav_put(&rout[bus[b]], path, sample_rate, 2, 3, rvout, 2 * gn * 4); }
			}
			memset(v->acc, 0, sizeof(float) * 2 * gn);
			done[b] = 1; left--; progressed = 1;
		}
		if (!progressed) cycle = 1;   /* nothing could run: buses feed each other in a loop; the next round runs them all */
	}
	if (getenv("RECOMP_NGS_DEBUG")) {
		static uint64_t nu; if (nu++ % 300 == 0) {
			for (int i = 0; i < nvoices; i++) {
				Voice *v = &voices[i];
				if (!v->playing || (!v->acc && !PCM_PLAYER(v->def) && v->def != DEF_SCREAM_AT9)) continue;
				uint32_t pp = v->params[0], data = RD32(pp + P_BUF(0)), bytes = RD32(pp + P_BUF(0) + 4), nz = 0;
				for (uint32_t k = 0; data && k < bytes && k < 8192; k += 2) nz += RD16(data + k) != 0;
				double e = 0; if (v->acc) for (uint32_t k = 0; k < 2 * gn; k++) e += v->acc[k] * v->acc[k];
				int np = 0; for (int p = 0; p < npatches; p++) np += patches[p].active && patches[p].src == v->handle;
				fprintf(stderr, "    ngs: update %llu voice %d def %u buf %u pos %u data %#x/%u nonzero %u patches %d acc %.4f ch %d type %d\n",
				        (unsigned long long)nu, i, v->def, v->buffer, v->byte_pos, data, bytes, nz, np, e,
				        (int8_t)RD8(pp + P_CHANNELS), (int8_t)RD8(pp + P_TYPE));
				for (int p = 0; p < npatches; p++) if (patches[p].active && patches[p].src == v->handle)
					fprintf(stderr, "    ngs:   patch -> %#x vol %.2f %.2f / %.2f %.2f\n", patches[p].dst,
					        patches[p].vol[0][0], patches[p].vol[0][1], patches[p].vol[1][0], patches[p].vol[1][1]);
			}
		}
	}
	for (int i = 0; i < nvoices; i++) {
		Voice *v = &voices[i];
		if (!v->mix) continue;   /* master buses: the output granule */
		if (dump_dir) for (uint32_t k = 0; k < 2 * gn; k++) master_clips[i] += v->acc[k] > 1 || v->acc[k] < -1;   /* (what a hard clip would cut) */
		master_limit(v, v->acc, gn);
		if (dump_dir) { char path[1024]; snprintf(path, sizeof path, "%s/master%d.wav", dump_dir, i); wav_put(&master_wav[i], path, sample_rate, 2, 3, v->acc, 2 * gn * 4); }
		for (uint32_t k = 0; k < 2 * gn; k++) {
			float s = v->acc[k] * 32767.0f;
			v->mix[k] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
		}
	}
	Due now[256]; int n = ndue;
	memcpy(now, due, sizeof(Due) * (size_t)n); ndue = 0;
	pthread_mutex_unlock(&lock);
	if (b) sched_block_end();
	if (n) {
		uint32_t info = (c->r[13] - 64) & ~7u;   /* below the caller's stack pointer, above guest_call's frame (sp - 512) */
		for (int k = 0; k < n; k++) {
			Due *d = &now[k];
			WR32(info, d->voice); WR32(info + 4, d->rack); WR32(info + 8, d->module); WR32(info + 12, d->reason);
			WR32(info + 16, d->reason2); WR32(info + 20, d->ptr); WR32(info + 24, d->ud);
			if (getenv("RECOMP_NGS_DEBUG")) fprintf(stderr, "    ngs: callback %#x voice %#x module %#x reason %u/%u\n", d->fn, d->voice, d->module, d->reason, d->reason2);
			/* (RECOMP_NGS_VO_LOG: a dialogue voice's END and finished callbacks as delivered -- Killzone's doubled lines, 2026-10-25:
			 * after a healthy line's end the engine polls the voice in that same update; before a doubled one it never polls and
			 * plays the line again 0.7 s later. Was the callback delivered, and when?) */
			Voice *dv = vo_log_on() && d->reason == EV_END ? voice_of(d->voice) : 0;
			if (dv && dv->def == DEF_SCREAM_AT9) fprintf(stderr, "    vo: v%d callback %s %#x delivered upd %llu\n", (int)VIDX(dv), d->fn == dv->fin_cb ? "finished" : "END", d->fn, (unsigned long long)updates);
			guest_call(c, d->fn, info, 0);
		}
	}
	RET(0);
}

/* (voice, module, memory, size): a master bus's output granule, or a player's state */
static void h_voice_state(Cpu *c) {
	Voice *v = voice_of(A0);
	if (!v) { memset(G(A2), 0, A3); RET(0x804a0005u); return; }
	if (v->mix) {
		uint32_t n = 4 * granularity; if (n > A3) n = A3;
		memcpy(G(A2), v->mix, n);
	} else if (A3 >= 24) {   /* SceNgsPlayerStates; samples generated = source samples consumed x channels, as the hardware
		 * (Vita3K: decoded size * channels) -- counted as output frames at 48 kHz until 2026-10-15, a stereo
		 * stream's clock ran at half speed in every game that halves it back to frames: Uncharted's cutscene
		 * sync (fn_815adf6c, voice types 3-5) then slowed the picture to that clock for its whole window and
		 * left the dialogue running ahead of the lips by however long the window was */
		WR32(A2, v->byte_pos); WR32(A2 + 4, v->buffer); WR32(A2 + 8, v->samples_gen);
		WR32(A2 + 12, v->bytes_consumed); WR32(A2 + 16, v->samples_gen); WR32(A2 + 20, v->bytes_consumed);
		if (v->def == DEF_SCREAM_AT9 && vo_log_on()) vo_state_log(v, c->r[14], vo_us());
		if (v->def == DEF_SCREAM_AT9 && at9_log_on() && at9_polls[VIDX(v)] < 12) { at9_polls[VIDX(v)]++; fprintf(stderr, "    at9 v%d upd %llu GetStateData (lr %#x) -> bytepos %u buf %u samples %u consumed %u (playing %d)\n", VIDX(v), (unsigned long long)updates, c->r[14], v->byte_pos, v->buffer, v->samples_gen, v->bytes_consumed, v->playing); }
	} else memset(G(A2), 0, A3);
	RET(0);
}
static void h_ok(Cpu *c) { RET(0); }
static void h_patch_info(Cpu *c) {   /* (patch, SceNgsPatchRouteInfo*: output channels, input channels, float matrix[2][2]) */
	Patch *p = patch_of(A0);
	if (!p) { RET(0x804a0005u); return; }
	if (A1) { WR32(A1, 2); WR32(A1 + 4, 2); memcpy(G(A1 + 8), p->vol, 16); }
	RET(0);
}
/* (start sample, samples, config, SceNgsAT9SkipBufferInfo*: start byte, bytes, samples to skip at
 * the start, at the end): the whole superframes covering the interval */
static void h_at9_section(Cpu *c) {
	uint32_t cfg = A2;
	uint8_t b[4] = {cfg & 0xff, cfg >> 8 & 0xff, cfg >> 16 & 0xff, cfg >> 24};
	uint32_t frame_bytes = ((uint32_t)b[2] << 3 | b[3] >> 5) + 1, frames = 1u << (b[3] >> 3 & 3);
	uint32_t sf_bytes = frame_bytes * frames, sf_samples = ((b[1] >> 4) < 4 ? 128 : 256) * frames;
	uint32_t first = A0 / sf_samples, last = (A0 + A1 + sf_samples - 1) / sf_samples;
	memset(G(A3), 0, 16);
	WR32(A3, first * sf_bytes); WR32(A3 + 4, (last - first) * sf_bytes);
	WR16(A3 + 8, (uint16_t)(A0 - first * sf_samples)); WR16(A3 + 10, (uint16_t)(last * sf_samples - (A0 + A1))); WR32(A3 + 12, frames == 4);
	RET(b[0] == 0xfe ? 0 : 0x804a0005u);
}

const HleEntry hle_ngs[] = {
	{"sceNgsVoiceDefGetMasterBuss", h_def_master}, {"sceNgsVoiceDefGetSimpleVoice", h_def_simple},
	{"sceNgsVoiceDefGetReverbBuss", h_def_reverb}, {"sceNgsVoiceDefGetScreamVoice", h_def_scream},
	{"sceNgsVoiceDefGetScreamAtrac9Voice", h_def_scream_at9}, {"sceNgsVoiceDefGetCompressorBuss", h_def_compressor},
	{"sceNgsVoiceDefGetCompressorSideChainBuss", h_def_sidechain},
	{"sceNgsVoiceSetFinishedCallback", h_voice_finished_cb}, {"sceNgsVoiceSetModuleCallback", h_voice_module_cb},
	{"sceNgsVoiceGetParamsOutOfRange", h_params_out_of_range}, {"sceNgsVoiceInit", h_ok}, {"sceNgsPatchGetInfo", h_patch_info},
	{"sceNgsAT9GetSectionDetails", h_at9_section},
	{"sceNgsSystemGetRequiredMemorySize", h_sys_mem}, {"sceNgsSystemInit", h_sys_init}, {"sceNgsSystemSetFlags", h_ok},
	{"sceNgsSystemRelease", h_ok}, {"sceNgsSystemUpdate", h_system_update},
	{"sceNgsRackGetRequiredMemorySize", h_rack_mem}, {"sceNgsRackInit", h_rack_init}, {"sceNgsRackRelease", h_ok},
	{"sceNgsRackGetVoiceHandle", h_rack_voice},
	{"sceNgsPatchCreateRouting", h_patch_create}, {"sceNgsPatchRemoveRouting", h_patch_remove},
	{"sceNgsVoicePatchSetVolumesMatrix", h_patch_volumes},
	{"sceNgsVoicePlay", h_voice_play}, {"sceNgsVoiceKill", h_voice_kill}, {"sceNgsVoicePause", h_voice_pause},
	{"sceNgsVoiceKeyOff", h_voice_keyoff},   /* key-off = release and stop, then the finished callback: stubbed, a keyed-off voice stayed "playing" and a sound system that waits for a voice to stop before reusing it (Unit 13 polls GetStateData 100K times a session; Killzone frees its voice in the finished callback) never got a free voice -- a silent gun */
	{"sceNgsVoiceResume", h_voice_resume}, {"sceNgsVoiceBypassModule", h_voice_bypass}, {"sceNgsVoiceGetInfo", h_voice_info},
	{"sceNgsVoiceLockParams", h_voice_lock}, {"sceNgsVoiceUnlockParams", h_voice_unlock},
	{"sceNgsVoiceSetParamsBlock", h_voice_set_block}, {"sceNgsVoiceGetStateData", h_voice_state},
	{0, 0},
};
