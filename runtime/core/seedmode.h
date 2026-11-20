/* seedmode.h: the instruction set of a missed function (code the analysis never saw, recorded for analyze.py --seeds when
 * a run calls it), 1 = Thumb, 0 = ARM. `before` is the instruction just before lr (the Thumb halfword at (lr & ~1) - 2 for
 * a Thumb caller, lr odd; the ARM word at lr - 4 otherwise; 0 when lr isn't in guest code), r the registers at the call.
 * A register BLX selects the state from the target's bit 0 (ARM ARM, BLX (register)): Thumb code's "blx r2" through a
 * vtable slot holding an even word enters ARM code. Phantasy Star Nova's 0x8204c890 was recorded as Thumb because lr was
 * odd; its Thumb decode broke r4 and the next virtual call jumped to junk (found in Luufen's fork, a810aac). It counts only
 * while that BLX's register still holds the target, as at the call itself (lr can be stale: a tail jump keeps the
 * caller's). Otherwise the rule before it: Thumb for an odd address, one ARM code can't start at (not 4-aligned), or one
 * Thumb code called or branched to directly (a bl/b target carries no Thumb bit; np_matching2's 0x8cee was recorded ARM) */
#pragma once
#include <stdint.h>
static inline int seed_thumb(uint32_t addr, uint32_t lr, uint32_t before, const uint32_t *r) {
	int m = -1;
	if (lr & 1) { if ((before & 0xff87u) == 0x4780u) m = (int)((before >> 3) & 15); }        /* Thumb BLX Rm: 0100 0111 1 Rm 000 */
	else if ((before & 0x0ffffff0u) == 0x012fff30u) m = (int)(before & 15);                 /* ARM BLX Rm: cond 0001 0010 1111 1111 1111 0011 Rm */
	if (m >= 0 && m != 14 && r[m] == addr) return (int)(addr & 1);
	return (addr & 3) || (lr & 1);
}
