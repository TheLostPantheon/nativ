/*
 * RECOMP_CAPTURE=<frame>: record that presented frame's graphics (RECOMP_CAPTURE_FRAMES=n: n frames
 * from there, for state carried from one frame to the next) as a self-contained file
 * (capture_<frame>.vfc, or RECOMP_CAPTURE_FILE; RECOMP_CAPTURE_EXIT=1 ends the run there) that runtime/debug/gfxreplay.c plays back
 * through either backend in seconds, without booting the game.
 *
 * What is recorded is the gfx.h stream itself -- every begin_scene/draw/end_scene/present, as the
 * plain structs they are (guest addresses, no host pointers) -- plus the guest memory they read.
 * Rather than working out what each draw reads (programs, uniform buffers, vertex and index data,
 * textures, render targets: each with its own size rules), the capture snapshots all guest memory
 * in use (the images' segments and the heap) when the frame starts, then before every call
 * records exactly the pages that changed since the last record, by comparing against a shadow
 * copy. So a replay sees, at every draw, the same bytes the real draw saw.
 *
 * Guest memory is authoritative on the software path; on the GPU path every render target is
 * written back to it first, so render targets that earlier frames left behind are in the snapshot
 * too. In a window, F12 captures the next RECOMP_CAPTURE_FRAMES frames (default 3) on demand, to
 * captures/frame_<n>.vfc (or the current directory when there's no captures/).
 *
 * File: "VFCP", version; then records {u32 tag, u32 length, payload}:
 *   MEMP  u32 address, u32 raw length, zlib data   a run of pages
 *   SCEN  GfxTarget        DRAW  GfxDraw        ENDS  (empty)
 *   PRES  u32 base, width, height, pitch, format    DONE  (empty)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <unistd.h>
#include <pthread.h>
#include "rt.h"
#include "hle.h"
#include "gfx.h"
#include "gfx_capture.h"

#define PAGE 4096u
static long want = -2;          /* the first frame to capture, -1 none */
static long nframes = 1;        /* how many */
static int requested;           /* by gfxcap_request, not the environment */
static long frame;              /* presents so far */
static FILE *out;
static int done;
typedef struct { uint32_t lo, hi; uint8_t *shadow; } Range;
static Range ranges[65]; static int nranges;
static uint64_t bytes_written;
/* the game presents from its display-queue thread and draws from its render thread: every entry
 * point holds this (a present closing the file under a draw being recorded crashed Unit 13) */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* F12 (window.c): the next frames, whatever the game is doing -- the way to capture a screen no
 * scripted test reaches */
void gfxcap_request(void) {
	pthread_mutex_lock(&lock);
	if (out) { pthread_mutex_unlock(&lock); return; }   /* one at a time */
	const char *n = getenv("RECOMP_CAPTURE_FRAMES");
	nframes = n && atol(n) > 0 ? atol(n) : 3;
	want = frame + 1; done = 0; requested = 1;
	pthread_mutex_unlock(&lock);
}
static int capturing(void);
int gfxcap_active(void) { return capturing(); }   /* (gfx_gpu.c, under the renderer's lock: a GXM transfer keeps the CPU path while frames are captured, since the capture records guest memory. No capture lock: a few ints read, and the renderer's lock must not wait on this one) */
static int capturing(void) {
	if (want == -2 && !requested) {
		const char *e = getenv("RECOMP_CAPTURE"), *n = getenv("RECOMP_CAPTURE_FRAMES");
		want = e ? atol(e) : -1; if (n && atol(n) > 0) nframes = atol(n);
	}
	return want >= 0 && !done && frame >= want && frame < want + nframes;
}
static void record(const char *tag, const void *p, uint32_t len) {
	fwrite(tag, 1, 4, out); fwrite(&len, 4, 1, out); if (len) fwrite(p, 1, len, out);
	bytes_written += 8 + len;
}
static void write_run(uint32_t addr, uint32_t len) {
	uLongf clen = compressBound(len);
	uint8_t *buf = malloc(8 + clen);
	memcpy(buf, &addr, 4); memcpy(buf + 4, &len, 4);
	compress2(buf + 8, &clen, G(addr), len, 1);
	record("MEMP", buf, (uint32_t)(8 + clen));
	free(buf);
}
/* every page that differs from the shadow (all of them, the first time: the shadow starts zeroed,
 * so all-zero pages cost nothing) is written as runs, and the shadow updated */
static void sync_memory(void) {
	gfx_sync_to_guest();   /* the GPU path keeps render targets on the GPU between reads (views, present): the record must hold their bytes */
	uint32_t lo[65], hi[65];
	int n = guest_mem_ranges(lo, hi, 65);
	for (int i = 0; i < n; i++) {
		uint32_t l = lo[i] & ~(PAGE - 1), h = (hi[i] + PAGE - 1) & ~(PAGE - 1);
		Range *r = 0;
		for (int k = 0; k < nranges; k++) if (ranges[k].lo == l) r = &ranges[k];
		if (!r) { if (nranges >= 65) continue; r = &ranges[nranges++]; r->lo = l; r->hi = l; r->shadow = 0; }
		if (h > r->hi) {   /* the heap grew: new pages start as zero in the shadow */
			r->shadow = realloc(r->shadow, h - r->lo);
			memset(r->shadow + (r->hi - r->lo), 0, h - r->hi);
			r->hi = h;
		}
		uint32_t run = 0, run_start = 0;
		for (uint32_t a = r->lo; a < r->hi; a += PAGE) {
			uint8_t *sh = r->shadow + (a - r->lo);
			int changed = memcmp(sh, G(a), PAGE) != 0;
			if (changed) { memcpy(sh, G(a), PAGE); if (!run) run_start = a; run += PAGE; }
			if ((!changed || run >= (16u << 20)) && run) { write_run(run_start, run); run = 0; }
		}
		if (run) write_run(run_start, run);
	}
}
static void begin_if_needed(void) {
	if (out) return;
	char path[256]; const char *e = getenv("RECOMP_CAPTURE_FILE");
	if (e && !requested) snprintf(path, sizeof path, "%s", e);
	else if (!access("captures", F_OK)) snprintf(path, sizeof path, "captures/frame_%ld.vfc", want);
	else snprintf(path, sizeof path, "capture_%ld.vfc", want);
	gfx_sync_to_guest();   /* the GPU path's render targets into guest memory: the snapshot's starting state */
	out = fopen(path, "wb");
	if (!out) { perror(path); done = 1; return; }
	uint32_t version = 1;
	fwrite("VFCP", 1, 4, out); fwrite(&version, 4, 1, out);
	fprintf(stderr, "capture: frame %ld -> %s\n", want, path);
}

void gfxcap_scene(const GfxTarget *t) {
	if (want == -1) return;   /* the common case, without the lock */
	pthread_mutex_lock(&lock);
	if (capturing()) { begin_if_needed(); if (out) { sync_memory(); record("SCEN", t, sizeof *t); } }
	pthread_mutex_unlock(&lock);
}
void gfxcap_draw(const GfxDraw *d) {
	if (want == -1) return;
	pthread_mutex_lock(&lock);
	if (capturing() && out) { sync_memory(); record("DRAW", d, sizeof *d); }
	pthread_mutex_unlock(&lock);
}
void gfxcap_end_scene(void) {
	if (want == -1) return;
	pthread_mutex_lock(&lock);
	if (capturing() && out) record("ENDS", 0, 0);
	pthread_mutex_unlock(&lock);
}
void gfxcap_present(uint32_t base, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format) {
	pthread_mutex_lock(&lock);
	if (capturing()) {
		begin_if_needed();
		if (out) {
			sync_memory();
			uint32_t p[5] = {base, width, height, pitch, format};
			record("PRES", p, sizeof p);
		}
		if (out && frame + 1 < want + nframes) { frame++; pthread_mutex_unlock(&lock); return; }   /* more frames to go */
		if (out) {
			record("DONE", 0, 0);
			fclose(out); out = 0;
			fprintf(stderr, "capture: done, %.1f MB\n", bytes_written / 1048576.0);
			if (getenv("RECOMP_CAPTURE_EXIT")) _exit(0);   /* a capture-only run (vitarecomp capture) */
			for (int i = 0; i < nranges; i++) free(ranges[i].shadow);
			nranges = 0;
		}
		done = 1;
	}
	frame++;
	pthread_mutex_unlock(&lock);
}
