/* ctrl_v2.h -- the shoulder buttons in the "2" pad reads (sceCtrlPeekBufferPositive2 / sceCtrlReadBufferPositive2).
 *
 * The "2" reads report the handheld's L and R as SCE_CTRL_L1 / SCE_CTRL_R1 (0x400 / 0x800); the other reads report them as
 * SCE_CTRL_LTRIGGER / SCE_CTRL_RTRIGGER (0x100 / 0x200). Vita3K does the same: every is_v2 read takes its button set from
 * the "ext" bindings, where the shoulders are L1 / R1 and the trigger bits are never set from them (ctrl/src/ctrl.cpp,
 * get_controller_bindings_ext and the keyboard's buttons_ext). A game that only uses the "2" reads tests L1 / R1; given
 * the trigger bits on port 0, it never sees L or R. Port 1 (a PS TV's paired pad) has its own layout (window_input_pad2). */
#pragma once
#include <stdint.h>

static inline uint32_t ctrl_v2_buttons(uint32_t b) {
	return (b & ~0x300u) | (b & 0x100u ? 0x400u : 0) | (b & 0x200u ? 0x800u : 0);
}
