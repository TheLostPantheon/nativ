/*
 * Offline USSE disassembler / coverage checker: loads a .gxp file (as dumped by
 * RECOMP_DUMP_GXP, see gxm_sony.c) straight into a synthetic "guest arena" at address 0 and runs it
 * through the same decoder the interpreter uses, so this is an exact coverage check of what
 * usse.c currently understands. Run over a whole directory to total up unsupported encodings
 * across every program a game registered.
 *
 * Usage: usse_dis [-q] file.gxp [file.gxp ...]
 *   -q   quiet: print only a one-line summary per file (for scanning many programs at once)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usse.h"
#include "usse_internal.h"   /* UOp, OP_*, BK_*: the VPCK constant-ROM scan below */

uint8_t *recomp_mem;   /* rt.h's arena pointer; here it just points at one loaded file at a time */

static uint8_t *load_file(const char *path, long *size) {
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "usse_dis: cannot open %s\n", path); return 0; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *buf = malloc((size_t)n + 4096);   /* pad: decode may read a few bytes past a truncated table */
	memset(buf, 0, (size_t)n + 4096);
	fread(buf, 1, (size_t)n, f); fclose(f);
	*size = n;
	return buf;
}

int main(int argc, char **argv) {
	int quiet = 0, argi = 1;
	long total_insns = 0, total_unsupported = 0, total_files = 0, files_unsupported = 0;
	if (argc > 1 && !strcmp(argv[1], "-q")) { quiet = 1; argi = 2; }
	for (; argi < argc; argi++) {
		long size;
		uint8_t *buf = load_file(argv[argi], &size);
		if (!buf) continue;
		/* usse_get() caches decoded programs by address, so each file needs its own synthetic
		 * guest address -- otherwise the second file would just hit the first file's cache entry. */
		uint32_t addr = (uint32_t)argi * 0x10000u;
		recomp_mem = buf - addr;
		if (size < 16 || memcmp(buf, "GXP", 3)) { fprintf(stderr, "usse_dis: %s: not a GXP file\n", argv[argi]); free(buf); continue; }
		const UsseProgram *p = usse_get(addr);
		total_files++;
		if (!quiet) {
			usse_debug_header(addr, stdout); usse_disassemble(p, stdout);
			{   /* the GXP's parameter table: name, category/type/components/container, array size, resource index */
				uint32_t cnt = *(uint32_t *)(buf + 0x24), off = 0x28 + *(uint32_t *)(buf + 0x28);
				printf(" parameters (%u):\n", cnt);
				for (uint32_t i = 0; i < cnt && i < 64; i++) {
					uint8_t *pp = buf + off + 16 * i; uint16_t ct = *(uint16_t *)(pp + 4);
					printf("   %-28s cat %u type %u comps %u container %u  array %u  resource %u\n", (char *)(pp + *(int32_t *)pp), ct & 15, (ct >> 4) & 15, (ct >> 8) & 15, (ct >> 12) & 15,
					       *(uint32_t *)(pp + 8), *(uint32_t *)(pp + 12));
				}
			}
			if (!usse_is_fragment(p)) {   /* the vertex layout: which O registers carry each texcoord, how wide */
				const UsseVertexLayout *vl = usse_vertex_layout(p);
				printf(" outputs:");
				if (vl->o_color0 >= 0) printf("  color0 o%d", vl->o_color0);
				if (vl->o_color1 >= 0) printf("  color1 o%d", vl->o_color1);
				if (vl->o_fog >= 0) printf("  fog o%d", vl->o_fog);
				for (int k = 0; k < 10; k++) if (vl->o_tex[k] >= 0) printf("  texcoord%d o%d x%d", k, vl->o_tex[k], vl->ncomp_tex[k]);
				if (vl->o_psize >= 0) printf("  psize o%d", vl->o_psize);
				printf("\n");
			}
			if (usse_is_fragment(p)) {   /* the iterator table: what arrives in PA before the program runs */
				const UsseFragLayout *fl = usse_fragment_layout(p);
				printf(" iterators (%d): colour type %d comps %d %s at %d%s\n", fl->niters, fl->color_type, fl->color_comps, fl->native_color ? "O" : "PA", fl->color_pa_start,
				       fl->frag_color_used ? "  FRAGCOLOR_USED (O0 preloaded with the framebuffer colour)" : "");
				for (int i = 0; i < fl->niters; i++) {
					const UsseIter *it = &fl->iters[i];
					if (it->is_tex) printf("   pa%-3d texture unit %d coord src %d proj %d result_fmt %d%s\n", it->pa, it->tex_unit, it->tex_coord_src, it->tex_proj, it->result_fmt, it->point_coord ? "  (point-sprite coordinate)" : "");
					else printf("   pa%-3d semantic %d comps %d storage %d%s\n", it->pa, it->semantic, it->comps, it->storage, it->point_coord ? "  (point-sprite coordinate)" : "");
				}
			}
		}
		{   /* VPCK sources in the constant ROM or an immediate: both executors read these as 0 until 2026-10-09
			 * (Uncharted's swipe prompt loads its threshold selectors that way) -- count them, to know which programs a
			 * fix like that reaches */
			int nc[2] = {0, 0}, n = 0; const UOp *code;
			for (int sec = 0; sec < 2; sec++) { code = usse_program_code(p, sec, &n); for (int i = 0; i < n; i++) {
				const UOp *op = &code[i]; if (op->op != OP_VPCK) continue;
				/* exec_vpck's source choice per written lane: src2 only carries z,w of a two-unit F32 source (src2's
				 * bank is decoded for every VPCK and means nothing otherwise) */
				int f32 = op->s1.type == TY_F32, wide = f32 && (op->s1.bank == BK_FPI || (op->s1.bank == op->s2.bank && op->s1.num == op->s2.num &&
				          op->s1.swiz[0] == 0 && op->s1.swiz[1] == 1 && op->s1.swiz[2] == 2 && op->s1.swiz[3] == 3 && (op->mask == 1 || op->mask == 3 || op->mask == 7 || op->mask == 15)));
				for (int c = 0; c < 4; c++) if (op->mask & (1 << c)) {
					const UOperand *src = (!f32 || wide || (op->s1.swiz[c] & 3) < 2) ? &op->s1 : &op->s2;
					if (src->bank == BK_FPCONST || src->bank == BK_IMM) { nc[sec]++; break; }
				}
			} }
			if (nc[0] || nc[1]) printf(" %s: VPCK(s) reading the constant ROM or an immediate: %d primary, %d secondary\n", argv[argi], nc[0], nc[1]);
		}
		int unsupported_here = !usse_supported(p);
		if (unsupported_here) files_unsupported++;
		int tot = 0, bad = usse_count_unsupported(p, &tot);
		total_insns += tot; total_unsupported += bad;
		if (quiet) printf("%-40s %s  %4d instr  %d unsupported%s\n", argv[argi], usse_is_fragment(p) ? "frag" : "vert", tot, bad, bad ? "  <--" : "");
		free(buf);
		recomp_mem = 0;
	}
	fprintf(stderr, "\n%ld file(s), %ld instruction(s) total, %ld unsupported encoding(s) in %ld file(s)\n",
	        total_files, total_insns, total_unsupported, files_unsupported);
	return total_unsupported ? 1 : 0;
}
