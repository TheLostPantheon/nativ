/*
 * psarc.c: PSARC archives on the host (see psarc.h). Moved out of fios.c, with byte-range and block reads added for
 * gxp_index.c.
 */
#include "psarc.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

static uint64_t be(const uint8_t *b, int n) { uint64_t v = 0; while (n--) v = v << 8 | *b++; return v; }
static int entry_cmp(const void *a, const void *b) { return strcmp(((const PsarcEntry *)a)->name, ((const PsarcEntry *)b)->name); }
static uint32_t zsize(const Psarc *a, uint32_t blk) { return a->zs[blk] ? a->zs[blk] : a->block_size; }

int psarc_block(const Psarc *a, const PsarcEntry *e, uint32_t k, uint64_t *foff, uint8_t *out, uint32_t *n) {
	const uint64_t at = (uint64_t)k * a->block_size;
	if (at >= e->size || e->block + k >= a->nz) return -1;
	const uint32_t zsz = zsize(a, e->block + k), want = e->size - at < a->block_size ? (uint32_t)(e->size - at) : a->block_size;
	uint8_t *z = malloc(zsz);   /* (not on the stack: fios.c reads on the game's threads) */
	int ok = z && pread(a->fd, z, zsz, (off_t)*foff) == (ssize_t)zsz;
	if (ok) {
		uLongf got = want;
		if (zsz == want || z[0] != 0x78 || uncompress(out, &got, z, zsz) != Z_OK || got != want) memcpy(out, z, zsz < want ? zsz : want);   /* stored */
		*n = want; *foff += zsz;
	}
	free(z);
	return ok ? 0 : -1;
}
uint8_t *psarc_read(const Psarc *a, const PsarcEntry *e) {
	uint8_t *out = malloc(e->size ? e->size + a->block_size : 1), *blk = malloc(a->block_size);
	uint64_t got = 0, foff = e->off;
	for (uint32_t k = 0; out && blk && got < e->size; k++) {
		uint32_t n;
		if (psarc_block(a, e, k, &foff, blk, &n)) { free(out); out = 0; break; }
		memcpy(out + got, blk, n); got += n;
	}
	free(blk);
	return out;
}
int psarc_read_range(const Psarc *a, const PsarcEntry *e, uint64_t off, void *out, size_t len) {
	if (off > e->size || len > e->size - off) return -1;
	if (!len) return 0;
	const uint32_t k0 = (uint32_t)(off / a->block_size), k1 = (uint32_t)((off + len - 1) / a->block_size);
	uint64_t foff = e->off;
	for (uint32_t k = 0; k < k0; k++) { if (e->block + k >= a->nz) return -1; foff += zsize(a, e->block + k); }
	uint8_t *blk = malloc(a->block_size); if (!blk) return -1;
	size_t done = 0;
	for (uint32_t k = k0; k <= k1; k++) {
		uint32_t n;
		if (psarc_block(a, e, k, &foff, blk, &n)) { free(blk); return -1; }
		const uint64_t bstart = (uint64_t)k * a->block_size, from = off + done - bstart;
		size_t take = n - from < len - done ? n - (size_t)from : len - done;
		memcpy((uint8_t *)out + done, blk + from, take); done += take;
	}
	free(blk);
	return done == len ? 0 : -1;
}
int psarc_open(Psarc *a, const char *path) {
	memset(a, 0, sizeof *a); a->fd = -1;
	const int fd = open(path, O_RDONLY);
	uint8_t h[32];
	if (fd < 0 || pread(fd, h, 32, 0) != 32 || memcmp(h, "PSAR", 4)) { if (fd >= 0) close(fd); return -1; }
	const uint32_t toc = (uint32_t)be(h + 12, 4), esz = (uint32_t)be(h + 16, 4), n = (uint32_t)be(h + 20, 4);
	a->block_size = (uint32_t)be(h + 24, 4); a->icase = be(h + 28, 4) & 1; a->fd = fd;
	if (toc < 32 + (uint64_t)n * esz || esz < 30 || !n || !a->block_size) { close(fd); a->fd = -1; return -1; }
	uint8_t *t = malloc(toc);
	if (!t || pread(fd, t, toc, 0) != (ssize_t)toc) { free(t); close(fd); a->fd = -1; return -1; }
	const int zb = a->block_size <= 0x10000 ? 2 : a->block_size <= 0x1000000 ? 3 : 4;
	a->nz = (toc - 32 - n * esz) / zb; a->zs = malloc(4 * ((size_t)a->nz + 1));
	for (uint32_t i = 0; i < a->nz; i++) a->zs[i] = (uint32_t)be(t + 32 + n * esz + i * zb, zb);
	PsarcEntry *all = calloc(n, sizeof *all);
	for (uint32_t i = 0; i < n; i++) {
		const uint8_t *e = t + 32 + i * esz;
		all[i].block = (uint32_t)be(e + 16, 4); all[i].size = be(e + 20, 5); all[i].off = be(e + 25, 5);
	}
	free(t);
	char *man = (char *)psarc_read(a, &all[0]);
	if (!man) { free(all); psarc_close(a); return -1; }
	man[all[0].size] = 0;   /* (psarc_read leaves room) */
	a->e = calloc(n, sizeof *a->e);
	char *line = man;
	for (uint32_t i = 1; i < n && line; i++) {   /* the manifest names entries 1.. in order */
		char *nl = strchr(line, '\n'); if (nl) *nl = 0;
		size_t l = strlen(line); if (l && line[l - 1] == '\r') line[l - 1] = 0;
		while (*line == '/') line++;
		a->e[a->n] = all[i]; a->e[a->n].name = strdup(line);
		if (a->icase) for (char *q = a->e[a->n].name; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
		a->n++;
		line = nl ? nl + 1 : 0;
	}
	free(man); free(all);
	qsort(a->e, a->n, sizeof *a->e, entry_cmp);
	return 0;
}
void psarc_close(Psarc *a) {
	if (a->fd >= 0) close(a->fd);
	for (int i = 0; i < a->n; i++) free(a->e[i].name);
	free(a->e); free(a->zs);
	memset(a, 0, sizeof *a); a->fd = -1;
}
const PsarcEntry *psarc_find(const Psarc *a, const char *name) {
	const PsarcEntry probe = { (char *)name };
	return bsearch(&probe, a->e, a->n, sizeof *a->e, entry_cmp);
}
