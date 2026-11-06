/* volatile_access_test.c -- RD32/WR32-family must be typed volatile accesses (recompiler/rt.h).
 *
 * Guest memory is shared between host threads and fibers: a spin-wait through a plain (memcpy)
 * accessor may be hoisted out of its loop at -O2, and the waiter then never observes the other
 * thread's store; stores may be dropped or merged the same way. The same requirement
 * vita-recomp-kit documents for its armrecomp.h rdv/wrv accessors.
 *
 * The case: a second host thread spins on RD32(flag) while this thread stores to it. The spin is
 * bounded (no hang: the bound hit reports "never observed"). On the old accessors the result
 * CHANGES with optimisation -- the observer never sees the store at -O2 but sees it at -O0 -- and
 * on the volatile accessors it passes at every level.
 *
 * RT_UNDER_TEST='<path>' compiles the same source against a copy of rt.h (the fail-before side).
 * Build and run: see README.md in this directory.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#ifdef RT_UNDER_TEST
#include RT_UNDER_TEST
#else
#include "../../recompiler/rt.h"
#endif

uint8_t *recomp_mem;

static int checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { failed++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

#define FLAG 0x2000u
static volatile uint32_t seen; static volatile int observer_done;
#define SPIN_BOUND 300000000u
static void *observer(void *p) {
	(void)p;
	uint32_t spins = 0, v;
	for (;;) { v = RD32(FLAG); if (v) break; if (++spins >= SPIN_BOUND) break; }
	seen = v;
	observer_done = 1;
	return 0;
}

int main(void) {
	recomp_mem = calloc((size_t)4 << 30, 1);   /* guest arena, zeroed like a fresh boot */
	if (!recomp_mem) { perror("calloc"); return 2; }

	/* round trip through every accessor width (the volatile forms must still read back) */
	WR64(0x1000, 0x0123456789abcdefull); WR32(0x1010, 0xa5a5a5a5u); WR16(0x1020, 0x5aa5u); WR8(0x1030, 0xc3u);
	CHECK(RD64(0x1000) == 0x0123456789abcdefull);
	CHECK(RD32(0x1010) == 0xa5a5a5a5u);
	CHECK(RD16(0x1020) == 0x5aa5u);
	CHECK(RD8(0x1030) == 0xc3u);

	/* the cross-thread observation: a store through WR32 must be seen by a spinning RD32 */
	pthread_t th;
	if (pthread_create(&th, 0, observer, 0)) { perror("pthread_create"); return 2; }
	usleep(20000);                    /* the observer is spinning on a zero flag */
	WR32(FLAG, 0x11111111u);
	for (int i = 0; i < 5000 && !observer_done; i++) usleep(1000);   /* bounded: never join a spinner */
	CHECK(observer_done);             /* the spin loop ended (flag seen or bound hit) */
	CHECK(seen == 0x11111111u);       /* THE check: the observer saw the other thread's store */
	if (observer_done) pthread_join(th, 0);

	if (failed) { printf("%d checks FAILED (store never observed)\n", failed); return 1; }
	printf("all passed (%d ok)\n", checks);
	return 0;
}
