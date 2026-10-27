/* mtl_archive.m (macOS, RECOMP_MTL_ARCHIVE builds): this machine's own archive of what Metal compiled for the game */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* a shader's library (AIR) to keep: key is its MSL cache file's (gfx_gpu.c), entry the function's name in air */
typedef void (*MtlAirStore)(uint64_t key, int is_fragment, const char *entry, const void *air, size_t n);
/* once, before the first pipeline: the archive in <cache_dir>/metal; 0 when off (RECOMP_MTL_ARCHIVE=0, or no Metal) */
int mtl_archive_open(const char *cache_dir, MtlAirStore store);
/* this thread's next pipeline: archive it or not, and whose libraries it should keep (0 none); a pipeline nothing was said of
 * is archived (the renderer's own: views, copies) */
void mtl_archive_want(int archive, uint64_t vkey, uint64_t fkey);
void mtl_archive_want_clear(void);   /* (after the pipeline: the next one is nothing said of) */
/* what was added since the last flush, written as a segment; how: 0 now and then, 1 the warm-up is done (this session has
 * made every pipeline the game's recipes name), 2 at exit (a compacting session that warmed up drops the segments it found) */
void mtl_archive_flush(int how);
void mtl_archive_flush_soon(int warm);   /* the same, on the archive's own thread (warm: 1's) */
/* pipelines from the archive, compiled and added, libraries kept since the start: to stderr */
void mtl_archive_report(void);
