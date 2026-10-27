/* A stand-in for libFuzzer where the compiler has none (Apple's clang): each file named is run once, then (FUZZ_ITERS,
 * default 200000) random mutations of them -- enough to smoke-test a harness under AddressSanitizer, not to fuzz. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n);
int main(int argc, char **argv) {
	static uint8_t seeds[64][4096]; size_t sn[64]; int ns = 0;
	for (int i = 1; i < argc && ns < 64; i++) { FILE *f = fopen(argv[i], "rb"); if (!f) continue; sn[ns] = fread(seeds[ns], 1, sizeof seeds[ns], f); fclose(f); LLVMFuzzerTestOneInput(seeds[ns], sn[ns]); ns++; }
	if (!ns) { sn[0] = 64; memset(seeds[0], 0, 64); ns = 1; }
	const long iters = getenv("FUZZ_ITERS") ? atol(getenv("FUZZ_ITERS")) : 200000; uint8_t b[4096]; srand(1);
	for (long it = 0; it < iters; it++) {
		const int s = rand() % ns; size_t n = sn[s]; memcpy(b, seeds[s], n);
		for (int k = rand() % 8 + 1; k; k--) {
			const int op = rand() % 4;
			if (op == 0 && n) b[rand() % n] ^= (uint8_t)(1 << (rand() % 8));
			else if (op == 1 && n) b[rand() % n] = (uint8_t)rand();
			else if (op == 2 && n < sizeof b) n++;
			else if (op == 3 && n > 1) n--;
		}
		LLVMFuzzerTestOneInput(b, n);
	}
	printf("%ld inputs, no crash\n", iters + ns);
	return 0;
}
