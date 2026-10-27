/*
 * psarc.h: PSARC archives (Sony's packed file format: a game's data, e.g. Killzone's game_data/data_ngp.psarc) read on the
 * host -- fios.c's mounts (the game's own file I/O) and gxp_index.c's program index (the shader recipes' loader).
 *
 * Header (big-endian): "PSAR", version, compression ("zlib"), TOC length, TOC entry size, entry count, block size, flags
 * (1 case-insensitive names, 2 absolute); entries {md5[16], first block u32, size u40, offset u40}; then each block's
 * compressed size (2, 3 or 4 bytes by block size; 0 = a full block stored). Entry 0 is the manifest: the names, one per
 * line. Each entry is its blocks in order, each deflated on its own (or stored), so a byte range inflates only the blocks
 * it covers. Reads use pread: one Psarc serves any number of threads.
 */
#ifndef PSARC_H
#define PSARC_H
#include <stdint.h>
#include <stddef.h>

typedef struct { char *name; uint32_t block; uint64_t size, off; } PsarcEntry;   /* name: lower-cased in a case-insensitive archive */
typedef struct { int fd; uint32_t block_size, nz, icase; uint32_t *zs; PsarcEntry *e; int n; } Psarc;   /* e: sorted by name (psarc_find) */

int psarc_open(Psarc *a, const char *path);   /* 0, or -1 (not a PSARC, or unreadable) */
void psarc_close(Psarc *a);
const PsarcEntry *psarc_find(const Psarc *a, const char *name);   /* by its manifest name (lower-cased first in a case-insensitive archive) */
uint8_t *psarc_read(const Psarc *a, const PsarcEntry *e);   /* the whole entry, malloc'd (its size bytes; 0 on failure) */
int psarc_read_range(const Psarc *a, const PsarcEntry *e, uint64_t off, void *out, size_t len);   /* bytes [off, off+len) of the entry: 0, or -1 */
/* block k of the entry inflated into out (a->block_size bytes), its length in *n; *foff: the file offset of block k, advanced
 * past it (start at e->off with k = 0, then call with k + 1): reading an entry in order without summing its block sizes */
int psarc_block(const Psarc *a, const PsarcEntry *e, uint32_t k, uint64_t *foff, uint8_t *out, uint32_t *n);

#endif
