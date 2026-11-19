/* The small rules of the NGS voices in runtime/audio/ngs_rules.h: where a player's start buffer is in its parameters, and
 * which voices sceNgsVoicePlay starts. */
#include <stdio.h>
#include <string.h>
#include "../audio/ngs_rules.h"

static int fails, checks;
static void check(const char *what, int ok) { checks++; if (!ok) { fails++; printf("FAIL %s\n", what); } }

int main(void) {
	/* the parameter block as a game fills it: type ADPCM, start buffer 1 */
	uint8_t pb[0x54]; memset(pb, 0, sizeof pb); pb[79] = 1; pb[80] = 0; pb[NGS_PLAYER_START_BUFFER] = 1;
	check("the start buffer is the s8 at +81", NGS_PLAYER_START_BUFFER == 81 && ngs_player_start((int8_t)pb[NGS_PLAYER_START_BUFFER]) == 1);
	check("the byte at +80 is not it", ngs_player_start((int8_t)pb[80]) == 0);
	check("a start buffer outside 0..3 starts at 0", ngs_player_start(-1) == 0 && ngs_player_start(4) == 0 && ngs_player_start(3) == 3);

	int playing = 0, started = 0;
	check("Play on a player at rest starts it", ngs_voice_play(1, &playing, &started) == 0 && playing == 1);
	int restarted = 0;
	for (int u = 0; u < 100; u++) restarted += ngs_voice_play(1, &playing, &started) == 0;
	check("Play on a playing player, 100 updates: never restarted", restarted == 0 && ngs_voice_play(1, &playing, &started) == NGS_ERROR && playing == 1);
	playing = 0;   /* its data ended, or it was keyed off or killed */
	check("once it stopped, Play starts it again", ngs_voice_play(1, &playing, &started) == 0 && playing == 1);

	/* a master, reverb or bus voice: running from its creation, at rest for the game until its own first Play */
	playing = 1; started = 0;
	check("the game's first Play on a bus the runtime runs succeeds", ngs_voice_play(0, &playing, &started) == 0 && started == 1);
	check("its second Play is an error", ngs_voice_play(0, &playing, &started) == NGS_ERROR);
	started = 0;   /* Kill or KeyOff */
	check("after Kill it can be played again", ngs_voice_play(0, &playing, &started) == 0);
	printf(fails ? "%d of %d FAILED\n" : "all %d passed\n", fails ? fails : checks, checks);
	return fails != 0;
}
