/*
 * gxp_index.h: where a game's shader programs are in its own files (gxp_index.c) -- the shader recipes' loader
 * (gfx_gpu.c) builds a recipe's shader before the game has loaded its program.
 */
#ifndef GXP_INDEX_H
#define GXP_INDEX_H
#include <stdint.h>

/* in the background: the index of every GXP under app0 (cached in cache_dir, rebuilt when the files change), then the
 * bytes of the wanted programs (content hashes, usse_gxp_hash) read out of it */
void gxp_index_start(const char *app0, const char *cache_dir, const uint32_t *want, int nwant);
int gxp_index_ready(void);   /* 1 once the wanted programs are read (or the index failed: then none are found) */
const uint8_t *gxp_index_program(uint32_t hash, uint32_t *size);
/* want = 0 (gxp_index_start): every program the index finds is read; these list what was read, in hash order */
int gxp_index_count(void);
uint32_t gxp_index_hash_at(int i);   /* (once ready) a wanted program's bytes, 0 if the game's files don't have it */

#endif
