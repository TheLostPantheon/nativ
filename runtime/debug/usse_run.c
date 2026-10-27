/*
 * usse_run: run one fragment GXP through the shader interpreter with chosen inputs, no game -- the
 * harness that pins down what a small program computes (iterators, framebuffer colour, uniforms).
 *
 *   usse_run <program.gxp> [pa=<iter index>:r,g,b,a ...] [o=r,g,b,a] [sa<n>=f | sa<n>=0xHEX ...] [nosec]
 *
 * pa=k:... feeds iterator k (its 4 components), o=... preloads the framebuffer colour (FRAGCOLOR_USED),
 * sa<n>=f sets secondary-attribute register n (a float). Prints the colour the program wrote.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "usse.h"
uint8_t *recomp_mem;
int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: usse_run <program.gxp> [pa=k:r,g,b,a] [o=r,g,b,a] [sa<n>=f]\n"); return 1; }
	FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
	fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *buf = malloc(size + 16); if (fread(buf, 1, size, f) != (size_t)size) return 1; fclose(f);
	uint32_t addr = 0x10000; recomp_mem = buf - addr;
	const UsseProgram *p = usse_get(addr);
	if (!usse_is_fragment(p)) { fprintf(stderr, "not a fragment program\n"); return 1; }
	const UsseFragLayout *fl = usse_fragment_layout(p);
	UsseInst inst; memset(&inst, 0, sizeof inst);
	UsseDraw draw; memset(&draw, 0, sizeof draw); int nosec = 0;
	for (int i = 2; i < argc; i++) {
		float c[4] = {0, 0, 0, 1}; int k, n; unsigned c_u;
		if (sscanf(argv[i], "pa=%d:%f,%f,%f,%f", &k, &c[0], &c[1], &c[2], &c[3]) >= 2 && k >= 0 && k < fl->niters) usse_pack_iter(&inst, &fl->iters[k], c);
		else if (sscanf(argv[i], "o=%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3]) >= 1) usse_preload_color(&inst, fl, c);
		else if (sscanf(argv[i], "sa%d=0x%x", &n, &c_u) == 2 && n >= 0 && n < 256) draw.sa[n].u = c_u;   /* a raw word (F16 pairs, packed bytes) */
		else if (sscanf(argv[i], "sa%d=%f", &n, &c[0]) == 2 && n >= 0 && n < 256) memcpy(&draw.sa[n], &c[0], 4);
		else if (!strcmp(argv[i], "nosec")) nosec = 1;   /* skip the secondary program: the SA words given are the post-secondary state (e.g. from RECOMP_DEBUG_DRAW's dump) */
		else fprintf(stderr, "ignored: %s\n", argv[i]);
	}
	if (getenv("RECOMP_USSE_TRACE")) {   /* the inputs as laid out in the registers */
		fprintf(stderr, "  pa:"); for (int i = 0; i < 8; i++) fprintf(stderr, " %08x", inst.pa[i].u);
		fprintf(stderr, "\n  o: "); for (int i = 0; i < 4; i++) fprintf(stderr, " %08x", inst.o[i].u);
		fprintf(stderr, "\n");
	}
	if (!nosec) { UsseInst scratch; memset(&scratch, 0, sizeof scratch); usse_run_secondary(p, &scratch, &draw); }   /* the per-draw SA preparation, as the renderer runs it */
	if (getenv("RECOMP_USSE_TRACE")) { fprintf(stderr, "  sa after secondary:"); for (int i = 0; i < 20; i++) fprintf(stderr, "%s%d=%08x", i % 10 ? " " : "\n   ", i, draw.sa[i].u); fprintf(stderr, "\n"); }
	usse_run_primary(p, &inst, &draw, 0, 0);
	float out[4]; usse_read_color(&inst, fl, out);
	printf("colour %g %g %g %g%s\n", out[0], out[1], out[2], out[3], inst.killed ? "  (killed)" : "");
	return 0;
}
