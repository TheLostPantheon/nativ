/* audio_pace_test.c -- the output pacing margin (runtime/ui/audio_pace.h) against a simulated device.
 *
 * A port's queue at 48 kHz stereo 16-bit, an A/V-sync delay of 65 ms kept in it: the device takes its buffer
 * (480 frames, 10 ms -- what Windows' shared mode gave for a request of 256) out of the queue in one piece
 * each period; the game's audio thread polls every 1 ms (window.c's SDL_Delay(1)) and hands over its next
 * buffer once the queue holds at most margin + delay -- topping the queue up with silence first if it holds
 * less than the delay (window.c audio_output). Every top-up is an audible gap.
 *
 * Checked over 10 simulated seconds: 256-sample buffers (a menu's music port) against a 480-frame device: no
 * silence inserted; 1024-sample buffers (a movie's port): none either; 256 against a 256-frame device: none;
 * an unknown device size (0) keeps the old margin. -DOLD_RULE uses the previous margin (depth x buffer): the
 * 256/480 case inserts silence ~50 times a second, the measured crackle. Header-only, no shims.
 */
#include <stdio.h>
#include "../ui/audio_pace.h"

static int fails;
static void check(const char *what, int ok) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

static int margin_rule(int depth, int buf, int dev) {
#ifdef OLD_RULE
	(void)dev; return depth * buf;
#else
	return audio_pace_margin(depth, buf, dev);
#endif
}

/* the number of silence top-ups in `seconds` after the start (time in 1/48000 s steps) */
static long simulate(int buf_frames, int dev_frames, int seconds) {
	const int frame = 4, rate = 48000, extra = 65 * rate / 1000 * frame;
	int buf = buf_frames * frame, dev = dev_frames * frame, margin = margin_rule(1, buf, dev);
	long q = extra + buf, pads = 0;
	int poll = rate / 1000;   /* the producer looks every 1 ms */
	for (long t = 0; t < (long)seconds * rate; t++) {
		if (t % dev_frames == 0) { q -= dev; if (q < 0) q = 0; }   /* the device's pull */
		if (t % poll == 0) {
			while (q <= margin + extra) {   /* the game hands over buffers until the pacing holds it */
				if (q < extra) { if (t >= rate / 10) pads++; q = extra; }   /* (counted after the first 100 ms: the start pads up to the delay by design) */
				q += buf;
			}
		}
	}
	return pads;
}

int main(void) {
	long p;
	p = simulate(256, 480, 10);
	printf("     (256-sample buffers, 480-frame device: %ld silence inserts in 10 s)\n", p);
	check("256-sample port on a 480-frame device: no silence inserted", p == 0);
	check("1024-sample port on a 480-frame device: no silence inserted", simulate(1024, 480, 10) == 0);
	check("256-sample port on a 256-frame device: no silence inserted", simulate(256, 256, 10) == 0);
	check("margin with an unknown device size (0) is depth x buffer", audio_pace_margin(1, 1024, 0) == 1024 && audio_pace_margin(2, 1024, 0) == 2048);
	check("margin covers one device buffer plus one game buffer", audio_pace_margin(1, 1024, 1920) == 2944);
	check("a deeper queue setting still wins when larger", audio_pace_margin(4, 4096, 1920) == 16384);
	if (fails) { printf("%d checks FAILED\n", fails); return 1; }
	printf("all passed\n");
	return 0;
}
