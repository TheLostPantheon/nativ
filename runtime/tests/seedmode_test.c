/* seedmode.h: a missed function's instruction set from the call that reached it (the cases are from Luufen's fork, a810aac).
 *   cc -O1 -I ../core seedmode_test.c -o /tmp/seedmode_test && /tmp/seedmode_test */
#include <stdio.h>
#include "seedmode.h"
static int checks, fails;
#define CHECK(what, got, want) do { checks++; if ((got) != (want)) { fails++; printf("FAIL %s: %d, want %d\n", what, (int)(got), (int)(want)); } } while (0)
int main(void) {
	uint32_t r[16] = {0};
	r[2] = 0x81002000u;   /* an even target in r2 */
	CHECK("Thumb blx r2 (0x4790) to an even target: ARM", seed_thumb(0x81002000u, 0x81001003u, 0x4790u, r), 0);
	r[2] = 0x81002001u;
	CHECK("Thumb blx r2 to an odd target: Thumb", seed_thumb(0x81002001u, 0x81001003u, 0x4790u, r), 1);
	r[3] = 0x81003000u;
	CHECK("ARM blx r3 (0xe12fff33) to an even target: ARM", seed_thumb(0x81003000u, 0x81001004u, 0xe12fff33u, r), 0);
	r[3] = 0x81003001u;
	CHECK("ARM blx r3 to an odd target: Thumb", seed_thumb(0x81003001u, 0x81001004u, 0xe12fff33u, r), 1);
	r[2] = 0x81005550u;   /* the register no longer holds the target (a stale lr after a tail jump): the old rule */
	CHECK("a stale blx: Thumb caller's lr says Thumb", seed_thumb(0x81002000u, 0x81001003u, 0x4790u, r), 1);
	CHECK("Thumb bl (no BLX before lr) to a 4-aligned target: Thumb", seed_thumb(0x81002000u, 0x81001003u, 0xf800u, r), 1);
	CHECK("ARM bl to a 4-aligned target: ARM", seed_thumb(0x81002000u, 0x81001004u, 0xebfffffeu, r), 0);
	CHECK("a 2-aligned address is never ARM", seed_thumb(0x81002002u, 0x81001004u, 0xebfffffeu, r), 1);
	CHECK("blx lr is no evidence", seed_thumb(0x81002000u, 0x81001003u, 0x47f0u, r), 1);
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
