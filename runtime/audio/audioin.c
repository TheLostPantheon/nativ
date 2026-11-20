/*
 * audioin.c: SceAudioIn, the microphone -- a silent one. A port opens (one VOICE, type 0, and one RAW, type 2: the Vita's
 * two), each input waits one buffer's time (grain / frequency) and hands back silence, and the mute status says not
 * muted. On the guest clock under RECOMP_CLOCK=fixed (display.c's audio pacing, so a headless run stays the same run to
 * run), on the host's clock otherwise, paced against the port's own timeline so the long-run rate is exact.
 *
 * Missing, LittleBigPlanet's mic thread called OpenPort, GetStatus and Input 24 million times each in a few minutes, each
 * returning at once: a core spent on nothing. Ten games of the library import it (voice chat, mic gameplay). A real
 * capture would need the player's microphone permission and a setting to give it, so it is silence for now.
 *
 * The errors are the open SDK's audioin.h values (0x8026010x: 1 invalid port, 2 invalid size, 4 invalid port type,
 * 5 invalid pointer, 7 port full, 9 not opened). Tested by runtime/tests/audioin_test.c.
 */
#include "hle.h"
#include "platform.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void audio_wait_guest_at(uint64_t *vnext, uint32_t i, uint64_t len);   /* display.c: a wait of len us on the guest clock, from *vnext */

#define AUDIO_IN_ERROR_INVALID_PORT      0x80260101u
#define AUDIO_IN_ERROR_INVALID_SIZE      0x80260102u
#define AUDIO_IN_ERROR_INVALID_PORT_TYPE 0x80260104u
#define AUDIO_IN_ERROR_INVALID_POINTER   0x80260105u
#define AUDIO_IN_ERROR_PORT_FULL         0x80260107u
#define AUDIO_IN_ERROR_NOT_OPENED        0x80260109u

static uint32_t in_len[2], in_freq[2];   /* per port (VOICE, RAW): samples per input, 0 while closed; samples a second */
static uint64_t in_next[2], in_vnext[2];   /* when its next input is due: host microseconds, guest microseconds */

static void h_audio_in_open(Cpu *c) {   /* (port type, grain in samples, frequency, format) -> the port */
	const uint32_t k = A0 == 0 ? 0 : A0 == 2 ? 1 : 2;
	if (k > 1) { RET(AUDIO_IN_ERROR_INVALID_PORT_TYPE); return; }
	if (in_len[k]) { RET(AUDIO_IN_ERROR_PORT_FULL); return; }   /* (that type's port is open already) */
	if (!A1 || A1 > 4096) { RET(AUDIO_IN_ERROR_INVALID_SIZE); return; }
	in_freq[k] = A2 ? A2 : 16000; in_next[k] = in_vnext[k] = 0;
	in_len[k] = A1;   /* (last: the port is open from here) */
	fprintf(stderr, "    audio in port %u (type %u): %u samples at %u Hz, silent\n", k + 1, A0, A1, in_freq[k]);
	RET(k + 1);
}
static void h_audio_in_input(Cpu *c) {   /* (port, buffer of grain 16-bit mono samples) */
	const uint32_t k = A0 - 1;
	if (k > 1) { RET(AUDIO_IN_ERROR_INVALID_PORT); return; }
	if (!in_len[k]) { RET(AUDIO_IN_ERROR_NOT_OPENED); return; }
	if (!A1) { RET(AUDIO_IN_ERROR_INVALID_POINTER); return; }
	memset(G(A1), 0, in_len[k] * 2u);
	const uint64_t len = (uint64_t)in_len[k] * 1000000 / in_freq[k];
	if (guest_clock_fixed()) { audio_wait_guest_at(&in_vnext[k], 8 + k, len); RET(0); return; }
	const uint64_t t = plat_now_us();
	if (!in_next[k] || in_next[k] + len < t) in_next[k] = t;   /* (the first input, or one long after the last: from now) */
	if (in_next[k] > t) { int b = sched_block_begin(); usleep((useconds_t)(in_next[k] - t)); if (b) sched_block_end(); }
	in_next[k] += len;
	RET(0);
}
static void h_audio_in_release(Cpu *c) {
	const uint32_t k = A0 - 1;
	if (k > 1) { RET(AUDIO_IN_ERROR_INVALID_PORT); return; }
	if (!in_len[k]) { RET(AUDIO_IN_ERROR_NOT_OPENED); return; }
	in_len[k] = 0;
	RET(0);
}
static void h_audio_in_status(Cpu *c) { RET(0); }   /* (select): SCE_AUDIO_IN_GETSTATUS_MUTE -- 0, not muted */
static void h_audio_in_adopt(Cpu *c) { RET(A0 == 0 || A0 == 2 ? 1 : 0); }   /* (port type): 1, that type can be opened */

const HleEntry hle_audioin[] = {
	{"sceAudioInOpenPort", h_audio_in_open}, {"sceAudioInInput", h_audio_in_input}, {"sceAudioInReleasePort", h_audio_in_release},
	{"sceAudioInGetStatus", h_audio_in_status}, {"sceAudioInGetAdopt", h_audio_in_adopt},
	{0, 0}
};
