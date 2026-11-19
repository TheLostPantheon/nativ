/* Small rules of the NGS voices, apart from ngs.c so runtime/tests/ngs_rules_test.c can check them. */
#ifndef NGS_RULES_H
#define NGS_RULES_H
#include <stdint.h>

/* SceNgsPlayerParams (0x54 bytes): descriptor +0, four buffers of 12 bytes {data, bytes, s16 loops, s16 next} from +8,
 * float frequency +56, float scalar +60, lead-in +64, sample limit +68, start bytes +72, s8 channels +76, channel map
 * +77..78, s8 type +79, a reserved byte +80, and the s8 start buffer at +81 (Vita3K's ngs/modules/player.h has the same
 * layout). The start buffer was read at +80, which holds nothing, so a voice whose first buffer is not 0 started on
 * buffer 0 (empty, or another sound's). */
#define NGS_PLAYER_START_BUFFER 81
/* the buffer a player starts on: 0..3 as given, else 0 */
static inline uint32_t ngs_player_start(int8_t start) { return start >= 0 && start < 4 ? (uint32_t)start : 0; }

#define NGS_ERROR 0x804a0001u
/* sceNgsVoicePlay starts a voice that is at rest and does nothing to one that is not (paused, or in its key-off release
 * too): it returns SCE_NGS_ERROR, as Vita3K's VoiceScheduler::play does for a voice that is not AVAILABLE. A game that
 * calls Play on its music voice every update would otherwise restart it each time and play the first buffer for ever.
 * A player voice is at rest unless it plays. The master, reverb and bus voices the runtime runs from their creation, so
 * that they mix, are at rest for the game until its own first Play: they check `started` instead, which Play sets and
 * Kill and KeyOff clear. 0 when the voice starts (the flag set), NGS_ERROR when it is left as it is. */
static inline uint32_t ngs_voice_play(int player_voice, int *playing, int *started) {
	int *flag = player_voice ? playing : started;
	if (*flag) return NGS_ERROR;
	*flag = 1; *playing = 1;
	return 0;
}
#endif
