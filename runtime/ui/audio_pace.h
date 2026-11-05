/* audio_pace.h -- how far ahead of the device an output port's queue is kept (window.c audio_output).
 *
 * The pacing returns to the game once the port's queue holds at most `margin` bytes beyond the A/V-sync
 * delay it keeps queued; below that delay the queue is topped up with silence (a gap in the sound). The
 * device takes its buffer from the queue in one piece -- its real size, not the size asked for (Windows'
 * shared mode: 480 frames, 10 ms, for a request of 256) -- so a margin smaller than one device buffer lets
 * every pull drop the queue under the delay: with a game's 256-sample buffers (5.3 ms) that was ~50
 * silence inserts a second, a constant crackle, while 1024-sample buffers (21 ms) stayed clean.
 * The margin: `depth` of the game's buffers, and at least one device buffer plus one game buffer. */
#ifndef RECOMP_AUDIO_PACE_H
#define RECOMP_AUDIO_PACE_H
static inline int audio_pace_margin(int depth, int buffer_bytes, int device_bytes) {
	int m = depth * buffer_bytes;
	if (m < device_bytes + buffer_bytes) m = device_bytes + buffer_bytes;
	return m;
}
#endif
