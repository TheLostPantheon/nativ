/*
 * Page-granular write tracking of the guest arena (guestwatch.c): the GPU backend's texture cache
 * makes the pages under its cached textures read-only; the first write to one (recompiled game
 * code, host code -- any thread) faults, the handler stamps the page with a new value of a global
 * write clock and makes it writable again, and the cache re-hashes only textures with a page
 * stamped after their last check. Writes the kernel makes on our behalf (read(2)/pread(2) into
 * guest memory) don't fault -- they fail with EFAULT on a read-only page -- so every such host
 * write is bracketed by gwatch_write_begin/end. RECOMP_GPU_NO_TEX_WATCH=1 switches it all off.
 */
#ifndef RECOMP_GUESTWATCH_H
#define RECOMP_GUESTWATCH_H
#include <stdint.h>

int gwatch_on(void);              /* tracking available (not switched off, set up, no mprotect failure since) */
int gwatch_page_shift(void);      /* log2 of the host page size: the tracking granularity */
/* make the pages under [addr, addr+len) read-only (pages a host write has pinned stay writable: its
 * gwatch_write_end stamps them) and return the write clock: every page of the range written from
 * now on gets a stamp greater than the value returned. Take it BEFORE reading the bytes. */
uint32_t gwatch_protect(uint32_t addr, uint32_t len);
/* no page of [addr, addr+len) stamped after *since: 1, and *since advances to the current clock
 * (the pages are all still read-only); 0 when some page was written (gwatch_page_clock says which) */
int gwatch_unchanged(uint32_t addr, uint32_t len, uint32_t *since);
uint32_t gwatch_page_clock(uint32_t page);   /* the stamp of guest page (addr >> gwatch_page_shift()) */
/* a host write into guest memory that must not fault (a system call writing into it) or that is
 * known in advance (a bulk copy: one call instead of a fault per page): pages opened and pinned
 * writable before, stamped after */
void gwatch_write_begin(uint32_t addr, uint32_t len);
void gwatch_write_end(uint32_t addr, uint32_t len);
/* The read watch (the GPU backend's: does the game read a GPU-only render target on the CPU?): the
 * whole pages inside [addr, addr+len) become inaccessible; the first access to one (any thread, a load
 * or a store) opens that page, stamps it as written, and calls the hook with the guest address. A
 * system call can't fault on the guest's behalf -- write(2) from such a page fails with EFAULT -- so
 * host code handing guest memory to the kernel to read opens it first with gwatch_read_open. */
void gwatch_set_read_hook(void (*hook)(uint32_t addr));
void gwatch_read_arm(uint32_t addr, uint32_t len);
void gwatch_read_open(uint32_t addr, uint32_t len);
/* counters since the last call (RECOMP_GPU_STATS): write faults taken, bracketed host writes, pages made read-only */
void gwatch_stats(uint32_t *faults, uint32_t *marks, uint32_t *protects);
#endif
