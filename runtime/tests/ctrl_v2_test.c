/* ctrl_v2_test.c -- the "2" pad reads report the shoulders as L1 / R1 (runtime/input/ctrl_v2.h).
 * cc -O1 -Wall -I ../input ctrl_v2_test.c -o ctrl_v2_test && ./ctrl_v2_test
 * -DCTRL_V2_IDENTITY builds it against the old behaviour (port 0 of a "2" read got the trigger bits unchanged). */
#include <stdio.h>
#include "ctrl_v2.h"
#ifdef CTRL_V2_IDENTITY
#define V2(b) (b)
#else
#define V2(b) ctrl_v2_buttons(b)
#endif
static int fails, n;
static void check(const char *what, int ok) { n++; if (!ok) { fails++; printf("FAIL %s\n", what); } }
int main(void) {
	check("L (0x100) is L1 (0x400)", V2(0x100u) == 0x400u);
	check("R (0x200) is R1 (0x800)", V2(0x200u) == 0x800u);
	check("both shoulders and cross", V2(0x4300u) == 0x4c00u);
	check("no trigger bit left", (V2(0x300u) & 0x300u) == 0);
	check("every other button unchanged", V2(0xf0f9u) == 0xf0f9u);
	check("L1 / R1 already set stay set", V2(0xc00u) == 0xc00u);
	check("nothing pressed", V2(0u) == 0u);
	printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", n, fails);
	return fails != 0;
}
