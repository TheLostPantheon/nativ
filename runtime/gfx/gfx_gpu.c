/*
 * SDL_GPU backend (RECOMP_GFX=gpu): render targets/textures/draws on the real GPU, translating
 * each draw's own GXP programs to SPIR-V (usse_spirv.c) instead of running them in the CPU
 * interpreter. A draw whose programs don't translate (see usse_spirv.h's coverage) falls back to
 * the software backend for that one draw -- gfx_soft_draw still writes straight into the target's
 * guest memory, so around a fallback this file downloads/uploads the GPU texture to keep guest
 * memory and the GPU texture in sync (see target_sync_for_cpu/target_sync_for_gpu).
 *
 * Scope (first real version, Shovel Knight-sized): one colour target at a time plus whatever
 * offscreen targets the game uses, a simple (address, format, width, height, content-hash)
 * texture cache, a pipeline cache keyed by (vprog, fprog, blend/depth/stencil/cull state), and a
 * readback path for presentation and RECOMP_SHOT. Vertex fetch reuses gfx_soft.c's
 * gfx_fetch_vertex_pa (same attribute packing as the CPU path) into a per-draw storage buffer
 * instead of a local UsseInst -- see usse_spirv.h for why the GPU vertex shader can just read
 * that directly. Every primitive (list/strip/fan) is expanded into a flat triangle list on the
 * CPU before upload (gfx.h's gfx_prim_tri), so the GPU draw is always a plain SDL_GPU_PRIMITIVETYPE_TRIANGLELIST
 * of 3*ntriangles sequential vertices -- simpler than threading index buffers and strip/fan
 * topology through the translator's gl_VertexIndex-only vertex-fetch model.
 */
#include <SDL3/SDL.h>
#include "gpulock.h"   /* every SDL submit/fence call below is the locked one: Homebrew SDL 3.4.16's Metal fence race, see that header */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "rt.h"
#include "gfx.h"
#include "usse.h"
#include "usse_spirv.h"
#include "guestwatch.h"
#include "metrics.h"
#include "settings.h"
#include "ssao.h"
#ifdef RECOMP_MTL_ARCHIVE
#include "mtl_archive.h"   /* this machine's archive of Metal's compiles (macOS) */
#endif
#include "vk_shaders.h"   /* the built-in shaders' SPIR-V twins (runtime/gfx/shaders/build.sh), for Vulkan */
/* RECOMP_GPU_STATS=1 timing: microseconds per frame in each stage of gfx_gpu_draw */
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifndef _WIN32
#include <sys/utsname.h>
#endif
#include "gxp_index.h"
#include "platform.h"   /* (plat_cpu_count) */
#include <pthread.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#endif
static int trace_sync(void);
/* The per-draw timers below (the t_* accumulators and the hitch thresholds) read the clock ~20 times a draw
 * whether or not anything prints them: clock_gettime/mach_absolute_time was 5.5-6% of the render thread's
 * on-CPU time at every render scale, ~1 ms per heavy frame. us_now() returns 0 unless something reads the
 * timers -- RECOMP_GPU_STATS, RECOMP_HITCH_LOG, RECOMP_GPU_FENCE_STATS, or RECOMP_GPU_TIMERS=1 to force them
 * on (=0 forces them off: the A/B of the clock reads) -- resolved once. Every use in this file is an
 * accumulator or a threshold test, so zeros are harmless; the metrics line's render time is gfx_dispatch.c's
 * own clock. */
static int timing_on(void) {
	static int v = -1;
	if (v < 0) {
		const char *e = getenv("RECOMP_GPU_TIMERS"), *h = getenv("RECOMP_HITCH_LOG");
		v = e && *e ? atoi(e) != 0 : getenv("RECOMP_GPU_STATS") != 0 || (h && *h && atoi(h) > 0) || getenv("RECOMP_GPU_FENCE_STATS") != 0;
	}
	return v;
}
static uint64_t us_now(void) { if (!timing_on()) return 0; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
static uint32_t n_shader_builds, n_vring_wraps;
/* RECOMP_HITCH_LOG=ms (display.c): a presented frame longer than that logs what the renderer did in it.
 * These count this frame's first-use work (a shader translated, cross-compiled and compiled; a pipeline
 * created; a texture uploaded) and its waits (a target download, a readback, a ring wrap), snapshotted
 * at present (gfx_gpu_last_frame_costs) */
static int hitch_log(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_HITCH_LOG"); v = e && *e && atoi(e) > 0; } return v; }
static uint32_t n_pipe_builds, n_tgt_downloads, n_readback_waits, n_ring_waits;
static uint64_t t_pipe_build, t_tgt_download, t_readback_wait, t_ring_wait, t_shader_xlate, t_shader_msl, t_shader_compile, t_vfetch_max;
static uint32_t n_shader_cached;   /* shaders whose MSL came from the cache this frame (msl_cache_get) */
static GfxGpuFrameCosts last_costs;
void gfx_gpu_last_frame_costs(GfxGpuFrameCosts *out) { *out = last_costs; }
#define VRING_WORDS (16u << 20)   /* the vertex ring: 64 MB of PA records, several frames' worth */
static SDL_GPUBuffer *vring; static SDL_GPUTransferBuffer *vring_xfer; static uint32_t vring_pos;   /* in words */
#define IRING_WORDS (4u << 20)    /* the index ring: 16 MB of 32-bit indices */
static SDL_GPUBuffer *iring; static SDL_GPUTransferBuffer *iring_xfer; static uint32_t iring_pos;
/* the LDST window ring: the 64 KB guest-memory windows a vertex program's LDSTs read through (storage buffer 1,
 * usse_spirv.h's USSE_LDST_WINDOW), placed at any word offset (the draw's window table carries it). It replaced a
 * GPU buffer and a transfer buffer created, filled and released for every such draw: free on Metal, 8 ms of
 * Uncharted's render thread a frame on Vulkan (an RTX 5070 Ti at 42-46 fps, CPU-bound) */
#define WRING_WORDS (8u << 20)    /* 32 MB: 512 windows, a few frames' worth after reuse */
static SDL_GPUBuffer *wring; static SDL_GPUTransferBuffer *wring_xfer; static uint32_t wring_pos;   /* in words */
static uint32_t bw_lo = ~0u, bw_hi, bwr_lo = ~0u, bwr_hi;   /* the pending batch's window words: written this batch (its one upload), and referenced (its fence) */
static uint32_t n_ldst_new, n_ldst_reused;   /* this frame's windows placed / reused */
static int no_indexed(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_GPU_NO_INDEXED") != 0; return v; }   /* RECOMP_GPU_NO_INDEXED=1: one record per index, as before */
static uint64_t t_up_decode, t_up_xfer, t_up_submit;   /* (RECOMP_GPU_STATS) the texture uploads' decode, transfer buffers (create, map, copy), command buffer (acquire, submit, release) */
static uint64_t t_vfetch, t_texhash, t_texup, t_tsync, t_view, t_submit, t_shader, t_pipe, t_bind, t_setup, t_vbuf, t_pass, t_total, t_map, t_acq, t_scene_sync;
static uint64_t write_clock;   /* bumps whenever a GPU target or depth surface gets new content: views (see target_view) follow it */
#ifdef RECOMP_SPIRV_CROSS
#include <spirv_cross/spirv_cross_c.h>
#endif
/* the scene's draws are recorded and submitted together (see "the draw batch" below); anything that
 * reads or rewrites what a recorded draw writes or samples flushes them first */
static void batch_flush(const char *why);
static void build_wait(const int *pending);   /* (the build workers, below: batch_flush waits for the pipelines its draws were recorded with) */
static void vis_complete(int wait); static void vis_download(void); static int vis_pending(void);   /* (the visibility test, with gfx_gpu_draw) */
static SDL_GPUBuffer *qbuf; static SDL_GPUTransferBuffer *qzero;   /* its counters, and zeros to reset them from */
static int batch_flushing;   /* inside batch_flush: it must not run again from anything it calls (view_pipeline_for's eviction) */
/* the fences of the scene command buffers still in flight (every flush submits with one): a ring
 * wrap waits for THESE, never for the whole device. SDL_WaitForGPUIdle froze Unit 13's armory
 * scene: it waits inside SDL's renderer lock while the window thread sits in
 * SDL_WaitAndAcquireGPUSwapchainTexture holding the same lock for a drawable that only the next
 * present can free -- and present was waiting on the renderer lock for this draw. */
static SDL_GPUDevice *gpu(void);
#define RING_FENCES 64
typedef struct { SDL_GPUFence *f; uint32_t vlo, vhi, ilo, ihi, wlo, whi; uint64_t vlap, ilap, wlap; } RingFence;   /* the words [lo, hi) of each ring its draws' records occupy, and in which lap of that ring */
static RingFence ring_fences[RING_FENCES]; static int n_ring_fences;
static uint64_t vring_lap, iring_lap, wring_lap;   /* each ring's lap: bumped at its wrap */
static void ring_fence_add(SDL_GPUFence *f, uint32_t vlo, uint32_t vhi, uint32_t ilo, uint32_t ihi, uint32_t wlo, uint32_t whi) {
	if (!f) return;
	int k = 0;   /* drop the completed ones first */
	for (int i = 0; i < n_ring_fences; i++) { if (SDL_QueryGPUFence(gpu(), ring_fences[i].f)) SDL_ReleaseGPUFence(gpu(), ring_fences[i].f); else ring_fences[k++] = ring_fences[i]; }
	n_ring_fences = k;
	if (n_ring_fences == RING_FENCES) { SDL_WaitForGPUFences(gpu(), true, &ring_fences[0].f, 1); SDL_ReleaseGPUFence(gpu(), ring_fences[0].f); memmove(ring_fences, ring_fences + 1, sizeof ring_fences[0] * (size_t)--n_ring_fences); }
	ring_fences[n_ring_fences++] = (RingFence){ f, vlo, vhi, ilo, ihi, wlo, whi, vring_lap, iring_lap, wring_lap };
}
/* Before the CPU writes words [pos, pos+n) of the vertex (ring 0), index (ring 1) or LDST window (ring 2) ring: wait for the scene
 * command buffers of an EARLIER lap whose records are still there -- the previous lap's draws over that
 * region, three or four frames old and long finished, so this almost never waits. The rule before waited at
 * every wrap for every fence in flight, i.e. for the GPU to finish everything submitted so far, the scenes
 * of the frame being recorded included: 1-37 ms of render-thread stall every few frames at 60 fps (the
 * hitch log's "ring waits"). RECOMP_GPU_RING_WAIT_ALL=1: that rule, for comparison. */
static void ring_wait_region(int ring, uint32_t pos, uint32_t n, int at_wrap) {
	static int all = -1; if (all < 0) all = getenv("RECOMP_GPU_RING_WAIT_ALL") != 0;
	if (all && !at_wrap) return;
	uint64_t lap = ring == 2 ? wring_lap : ring ? iring_lap : vring_lap;
	int k = 0;
	for (int i = 0; i < n_ring_fences; i++) {
		RingFence *r = &ring_fences[i];
		uint64_t rlap = ring == 2 ? r->wlap : ring ? r->ilap : r->vlap; uint32_t lo = ring == 2 ? r->wlo : ring ? r->ilo : r->vlo, hi = ring == 2 ? r->whi : ring ? r->ihi : r->vhi;
		if (all || (rlap < lap && lo < hi && lo < pos + n && hi > pos)) {
			uint64_t t0 = us_now(); SDL_WaitForGPUFences(gpu(), true, &r->f, 1); t_ring_wait += us_now() - t0; n_ring_waits++;
			SDL_ReleaseGPUFence(gpu(), r->f);
		} else ring_fences[k++] = *r;
	}
	n_ring_fences = k;
}
static void batch_retire(SDL_GPUTexture *tex, const char *why);   /* tex is about to be rewritten or released: pending draws that bind it go first */
static int batch_uses(SDL_GPUTexture *tex);

extern void *window_gpu_device(void);   /* window.c */
extern void *window_handle(void);
extern void window_present(uint32_t base, uint32_t w, uint32_t h, uint32_t pitch);

#ifdef RECOMP_SPIRV_CROSS
/* Metal: SDL_GPU doesn't take SPIR-V there, so cross-compile to MSL once per shader at creation
 * time (build.sh only links spirv-cross on Mac -- Vulkan on Linux/Windows takes our SPIR-V as-is,
 * see make_shader). Returns a malloc'd, NUL-terminated MSL source string (*out_len includes the
 * NUL, matching what SDL_CreateGPUShader's code_size wants for MSL source), or 0 on failure.
 *
 * SDL3's Metal backend binds resources by FIXED slot (see SDL_CreateGPUShader's own doc comment):
 * uniform buffers first, at [[buffer(0..)]], then storage buffers right after; sampled textures
 * at [[texture(0..)]] with samplers at the matching [[sampler(0..)]]. spirv-cross's *default* MSL
 * numbering instead renumbers resources in declaration order AFTER dead-code-eliminating anything
 * unused -- a vertex program that happens not to read its SA uniform buffer (common: plenty of
 * simple vertex shaders only touch PA) gets its uniform buffer dropped from the function signature
 * entirely, which shifts its storage buffer down to [[buffer(0)]] -- but SDL_GPU still binds the
 * storage buffer at [[buffer(1)]] (since it was TOLD this shader has 1 uniform buffer), so the
 * shader reads from the wrong slot. This produced a real, silent bug here: gl_Position computed
 * from garbage, no error anywhere, caught only by bisecting with a hand-written MSL shader (see
 * RECOMP_GPU_DEBUG_FLAT) down to inspecting the actual generated MSL's parameter list. Pinning
 * every resource's MSL index explicitly (ignoring whether spirv-cross would otherwise elide it)
 * fixes this regardless of which resources a given program happens to use. */
static int hitch_log(void);
static uint64_t us_now(void);
static void *spirv_cross_to_msl(const uint32_t *words, uint32_t nwords, int is_fragment, int nsmp, int nsbuf, size_t *out_len) {   /* nsbuf: vertex storage buffers (PA input, LDST windows) */
	uint64_t tq0 = us_now();
	spvc_context ctx; spvc_context_create(&ctx);
	spvc_parsed_ir ir;
	if (spvc_context_parse_spirv(ctx, words, nwords, &ir) != SPVC_SUCCESS) {
		fprintf(stderr, "    gpu: spirv-cross parse: %s\n", spvc_context_get_last_error_string(ctx)); spvc_context_destroy(ctx); return 0;
	}
	spvc_compiler comp;
	if (spvc_context_create_compiler(ctx, SPVC_BACKEND_MSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &comp) != SPVC_SUCCESS) {
		fprintf(stderr, "    gpu: spirv-cross create_compiler: %s\n", spvc_context_get_last_error_string(ctx)); spvc_context_destroy(ctx); return 0;
	}
	uint64_t tq1 = us_now();
	spvc_compiler_options opts; spvc_compiler_create_compiler_options(comp, &opts);
	/* MSL 2.3 (the platform stays macOS): framebuffer fetch -- a fragment module's input attachment (fetch mode 2,
	 * see fetch_mode) becomes a `float4 ... [[color(0)]]` input read from tile memory -- is refused by spirv-cross
	 * on macOS below it. The version is part of every shader's generated source: MSL_CACHE_MAGIC bumps with it */
	spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_VERSION, SPVC_MAKE_MSL_VERSION(2, 3, 0));
	spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_MSL_FRAMEBUFFER_FETCH_SUBPASS, SPVC_TRUE);
	spvc_compiler_install_compiler_options(comp, opts);
	SpvExecutionModel stage = is_fragment ? SpvExecutionModelFragment : SpvExecutionModelVertex;
	/* uniform buffer (SA): set 1 (vertex) or 3 (fragment), binding 0 -> MSL buffer 0, always */
	{ spvc_msl_resource_binding_2 b; spvc_msl_resource_binding_init_2(&b);
		b.stage = stage; b.desc_set = is_fragment ? 3 : 1; b.binding = 0; b.count = 1; b.msl_buffer = 0;
		spvc_compiler_msl_add_resource_binding_2(comp, &b); }
	for (int k = 0; !is_fragment && k < nsbuf; k++) {   /* vertex storage buffers: set 0, binding nsmp+k (after its textures) -> MSL buffer 1+k (right after the uniform buffer): the PA input, then the LDST windows */
		spvc_msl_resource_binding_2 b; spvc_msl_resource_binding_init_2(&b);
		b.stage = stage; b.desc_set = 0; b.binding = (unsigned)(nsmp + k); b.count = 1; b.msl_buffer = (unsigned)(1 + k);
		spvc_compiler_msl_add_resource_binding_2(comp, &b);
	}
	for (int i = 0; i < nsmp; i++) {   /* sampled textures: set 2 (vertex: 0), binding i -> MSL texture/sampler i (a fetch-mode-2 module's input attachment is not a resource: no binding) */
		spvc_msl_resource_binding_2 b; spvc_msl_resource_binding_init_2(&b);
		b.stage = stage; b.desc_set = is_fragment ? 2 : 0; b.binding = (unsigned)i; b.count = 1; b.msl_texture = (unsigned)i; b.msl_sampler = (unsigned)i;
		spvc_compiler_msl_add_resource_binding_2(comp, &b);
	}
	const char *src;
	if (spvc_compiler_compile(comp, &src) != SPVC_SUCCESS) {
		fprintf(stderr, "    gpu: spirv-cross compile: %s\n", spvc_context_get_last_error_string(ctx)); spvc_context_destroy(ctx); return 0;
	}
	size_t len = strlen(src);
	uint64_t tq2 = us_now();
	if (hitch_log()) fprintf(stderr, "    gpu hitch:   spirv-cross: %u SPIR-V words -> %zu bytes of MSL: parse+compiler %.2f ms, compile %.2f ms\n", nwords, len, (tq1 - tq0) / 1e3, (tq2 - tq1) / 1e3);
	char *out = malloc(len + 1); memcpy(out, src, len + 1);
	*out_len = len + 1;
	spvc_context_destroy(ctx);
	return out;
}
#endif

#ifdef RECOMP_SPIRV_CROSS
/* ---- the MSL cache (RECOMP_SHADER_CACHE=dir; default "shader_cache" in the working directory, next to
 * settings.ini; 0 = off). spirv-cross's SPIR-V -> MSL translation above costs 1-19 ms per program on this
 * Mac (median 1.4 ms; the game's big fragment programs are 30-57K SPIR-V words -> 100-200 KB of MSL) and
 * runs on the render thread at the program's first draw: entering a Unit 13 level built ~100 shaders (300
 * ms, a visible freeze) and a new effect or enemy mid-level a few more, 2-20 ms each -- a dropped frame or
 * two at 60 fps. Metal's own compile of the MSL is cached by the OS already (0.02-0.1 ms here on a repeat
 * run); this caches the step before it, so a repeat run builds a shader in ~0.1 ms. One file per shader,
 * named by a 64-bit hash of the SPIR-V words and the binding parameters, holding the SPIR-V itself
 * (compared whole on a hit: nothing rests on the hash) and the MSL; a miss translates and writes the file
 * whole, then renames it into place (two game processes may share the directory). The SPIR-V is the key,
 * so a translator change (usse_spirv.c) misses, never mis-hits; a change to the binding scheme in
 * spirv_cross_to_msl must bump MSL_CACHE_MAGIC. */
static uint64_t hash_bytes(const void *p, size_t n);   /* (textures, below) */
static const char *msl_salt(void) { const char *e = getenv("RECOMP_GPU_SALT_MSL"); return e && *e ? e : 0; }   /* (make_shader's measurement: no archive under it) */
#ifdef RECOMP_MTL_ARCHIVE
static int mtl_archive_on;   /* this machine's archive of Metal's compiles is open (mtl_archive.m) */
static uint32_t n_shader_libs, n_view_libs;   /* shaders and view variants made from their kept library */
#endif
static const char *msl_cache_dir(void) {
	static const char *d = (const char *)1;
	if (d == (const char *)1) {
		const char *e = getenv("RECOMP_SHADER_CACHE");
		d = e ? (*e && strcmp(e, "0") ? e : 0) : "shader_cache";
		if (d && mkdir(d, 0755) && errno != EEXIST) { fprintf(stderr, "    gpu: shader cache: cannot create %s (%s): translating every shader\n", d, strerror(errno)); d = 0; }
	}
	return d;
}
typedef struct { char magic[8]; uint32_t nwords, is_fragment, nsmp, nsbuf, msl_len, lib_len; } MslCacheHeader;   /* then the SPIR-V words, then msl_len bytes of MSL (its NUL included), then lib_len bytes of library block (0: none) */
#define MSL_CACHE_MAGIC "VRMSL002"   /* 002: MSL 2.3 + framebuffer-fetch subpasses (spirv_cross_to_msl) */
/* the library block (mtl_archive.m, RECOMP_MTL_ARCHIVE builds): Metal's own compile of the MSL above, from a pipeline this
 * machine made -- "LIB1", u32 entry length, the entry's name, u32 n, n bytes of library. A file without one (older, or
 * from a build without the archive) reads as before */
typedef struct { void *air; size_t n; char entry[64]; } MslLib;
static uint64_t msl_cache_key(const uint32_t *words, uint32_t nwords, int is_fragment, int nsmp, int nsbuf) {
	return hash_bytes(words, 4 * (size_t)nwords) ^ ((uint64_t)(uint32_t)is_fragment << 56) ^ ((uint64_t)(uint32_t)nsmp << 48) ^ ((uint64_t)(uint32_t)nsbuf << 40);
}
static void msl_cache_path(char *path, size_t n, uint64_t key) { snprintf(path, n, "%s/%016llx.msl", msl_cache_dir(), (unsigned long long)key); }
static int msl_lib_read(FILE *f, uint32_t len, MslLib *lib) {   /* (f just past the MSL) */
	uint8_t hd[8]; uint32_t el, n;
	if (len < 12 || fread(hd, 1, 8, f) != 8 || memcmp(hd, "LIB1", 4) || (memcpy(&el, hd + 4, 4), el == 0 || el >= sizeof lib->entry || 12 + el > len)) return 0;
	if (fread(lib->entry, 1, el, f) != el || fread(&n, 4, 1, f) != 1 || 12 + (size_t)el + n != len) return 0;
	lib->entry[el] = 0;
	if (!(lib->air = malloc(n)) || fread(lib->air, 1, n, f) != n) { free(lib->air); lib->air = 0; return 0; }
	lib->n = n;
	return 1;
}
static void *msl_cache_get(uint64_t key, const uint32_t *words, uint32_t nwords, int is_fragment, int nsmp, int nsbuf, size_t *out_len, MslLib *lib) {   /* lib: its library block too, if it has one */
	if (!msl_cache_dir()) return 0;
	char path[1024]; msl_cache_path(path, sizeof path, key);
	FILE *f = fopen(path, "rb"); if (!f) return 0;
	MslCacheHeader h; void *out = 0; uint32_t *w = 0;
	if (fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, MSL_CACHE_MAGIC, 8) && h.nwords == nwords && h.is_fragment == (uint32_t)is_fragment && h.nsmp == (uint32_t)nsmp && h.nsbuf == (uint32_t)nsbuf && h.msl_len > 0 && h.msl_len < (64u << 20)) {
		w = malloc(4 * (size_t)nwords); out = malloc(h.msl_len);
		if (w && out && fread(w, 4, nwords, f) == nwords && !memcmp(w, words, 4 * (size_t)nwords) && fread(out, 1, h.msl_len, f) == h.msl_len && ((char *)out)[h.msl_len - 1] == 0) {
			*out_len = h.msl_len;
			if (lib && h.lib_len && h.lib_len < (64u << 20)) msl_lib_read(f, h.lib_len, lib);
		}
		else { free(out); out = 0; }
	}
	free(w); fclose(f);
	return out;
}
static int msl_cache_put(uint64_t key, const uint32_t *words, uint32_t nwords, int is_fragment, int nsmp, int nsbuf, const void *msl, size_t len) {   /* 1: the file is there */
	if (!msl_cache_dir() || !len) return 0;
	char path[1024], tmp[1100]; msl_cache_path(path, sizeof path, key);
	snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, (int)getpid());
	FILE *f = fopen(tmp, "wb");
	if (!f) { static int said; if (!said++) fprintf(stderr, "    gpu: shader cache: cannot write %s (%s)\n", tmp, strerror(errno)); return 0; }
	MslCacheHeader h = { {0}, nwords, (uint32_t)is_fragment, (uint32_t)nsmp, (uint32_t)nsbuf, (uint32_t)len, 0 }; memcpy(h.magic, MSL_CACHE_MAGIC, 8);
	int ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(words, 4, nwords, f) == nwords && fwrite(msl, 1, len, f) == len;
	ok = !fclose(f) && ok;
	if (!ok || rename(tmp, path)) { unlink(tmp); return 0; }
	return 1;
}
#ifdef RECOMP_MTL_ARCHIVE
/* libraries of MSL the renderer writes itself (the view variants: hundreds, made as the game reads its targets in new
 * ways): <shader cache>/metal_text/<key>.lib, an .msl file's library block alone, by a hash of the text and its entry
 * (TEXT_KEY set, which mtl_archive.m hands back to msl_lib_store) */
#define TEXT_KEY (1ull << 63)
static uint64_t text_key(const char *msl, const char *entry) {
	uint64_t h = 1469598103934665603ULL;
	for (const char *p = msl; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
	h ^= 0xff; h *= 1099511628211ULL;
	for (const char *p = entry; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
	return h | TEXT_KEY;
}
static void text_lib_path(char *path, size_t n, uint64_t key) { snprintf(path, n, "%s/metal_text/%016llx.lib", msl_cache_dir(), (unsigned long long)key); }
static int text_lib_get(uint64_t key, MslLib *lib) {
	if (!msl_cache_dir()) return 0;
	char path[1024]; text_lib_path(path, sizeof path, key);
	FILE *f = fopen(path, "rb"); if (!f) return 0;
	fseek(f, 0, SEEK_END); const long len = ftell(f); fseek(f, 0, SEEK_SET);
	const int ok = len > 12 && len < (64L << 20) && msl_lib_read(f, (uint32_t)len, lib);
	fclose(f);
	return ok;
}
/* mtl_archive.m: a shader's library, from a pipeline this machine made, into its file (rewritten whole, then renamed into
 * place: another game process may be reading it) */
static void msl_lib_store(uint64_t key, int is_fragment, const char *entry, const void *air, size_t n) {
	char path[1024], tmp[1100];
	if (key & TEXT_KEY) {   /* (a view variant's: a file of its own) */
		char dir[1024]; snprintf(dir, sizeof dir, "%s/metal_text", msl_cache_dir()); mkdir(dir, 0755);
		text_lib_path(path, sizeof path, key); snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, (int)getpid());
		const uint32_t el = (uint32_t)strlen(entry), an = (uint32_t)n;
		FILE *f = el && el < 64 && n < (64u << 20) ? fopen(tmp, "wb") : 0; if (!f) return;
		int ok = fwrite("LIB1", 1, 4, f) == 4 && fwrite(&el, 4, 1, f) == 1 && fwrite(entry, 1, el, f) == el && fwrite(&an, 4, 1, f) == 1 && fwrite(air, 1, n, f) == n;
		ok = !fclose(f) && ok;
		if (!ok || rename(tmp, path)) unlink(tmp);
		(void)is_fragment; return;
	}
	msl_cache_path(path, sizeof path, key);
	FILE *f = fopen(path, "rb"); if (!f) return;
	MslCacheHeader h; uint8_t *body = 0; size_t bl = 0;
	if (fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, MSL_CACHE_MAGIC, 8) && h.is_fragment == (uint32_t)is_fragment && h.msl_len > 0 && h.msl_len < (64u << 20) && h.nwords < (16u << 20)) {
		bl = 4 * (size_t)h.nwords + h.msl_len;
		if ((body = malloc(bl)) && fread(body, 1, bl, f) != bl) { free(body); body = 0; }
	}
	fclose(f);
	const uint32_t el = (uint32_t)strlen(entry), an = (uint32_t)n;
	if (!body || !el || el >= 64 || n >= (64u << 20)) { free(body); return; }
	h.lib_len = 12 + el + an;
	snprintf(tmp, sizeof tmp, "%s.%d.lib.tmp", path, (int)getpid());
	if (!(f = fopen(tmp, "wb"))) { free(body); return; }
	int ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(body, 1, bl, f) == bl && fwrite("LIB1", 1, 4, f) == 4 && fwrite(&el, 4, 1, f) == 1 && fwrite(entry, 1, el, f) == el && fwrite(&an, 4, 1, f) == 1 && fwrite(air, 1, n, f) == n;
	ok = !fclose(f) && ok;
	if (!ok || rename(tmp, path)) unlink(tmp);
	free(body);
}
#endif
#endif
static SDL_GPUDevice *gpu(void) { return (SDL_GPUDevice *)window_gpu_device(); }
/* the GPU device (window.c, gfxreplay.c): SDL picks the backend. Vulkan is asked for fragmentStoresAndAtomics as well (the
 * visibility tests count in a buffer from a fragment shader, which SDL_GPU otherwise binds for reading only; Metal writes it as
 * it is), and for robustBufferAccess: a shader reading past the end of a buffer gets zeros instead of a GPU fault. Metal shrugs
 * such reads off; on the RTX laptop under Windows two sessions ended with the driver resetting the GPU (nvlddmkm event 153,
 * VK_ERROR_DEVICE_LOST: Killzone crashed, Unit 13 froze, 2026-10-23). A device without the features is made without them,
 * the tests then off. */
static int vk_fragment_stores;   /* a Vulkan device with fragmentStoresAndAtomics */
void *gfx_gpu_create_device(void) {
	SDL_GPUDevice *d = 0;
#ifndef __APPLE__   /* (on Apple, Metal: SDL could otherwise find MoltenVK) */
	if (!getenv("SDL_GPU_DRIVER") || !strcmp(getenv("SDL_GPU_DRIVER"), "vulkan")) {
		uint32_t vk10[55] = {0}; vk10[0] = 1; vk10[26] = 1;   /* VkPhysicalDeviceFeatures, 55 VkBool32 in the spec's order: [0] robustBufferAccess, [26] fragmentStoresAndAtomics */
		SDL_GPUVulkanOptions vo = {0}; vo.vulkan_api_version = 1u << 22; vo.vulkan_10_physical_device_features = vk10;   /* (VK_MAKE_VERSION(1, 0, 0): SDL's own default) */
		SDL_PropertiesID p = SDL_CreateProperties();
		SDL_SetBooleanProperty(p, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
		SDL_SetBooleanProperty(p, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, getenv("RECOMP_GPU_DEBUG_SDL") != 0);   /* (off, as SDL_CreateGPUDevice's was; RECOMP_GPU_DEBUG_SDL=1: SDL's checks -- they stop at an integer-format texture made for sampling, the stencil preload's) */
		SDL_SetStringProperty(p, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan");
		SDL_SetPointerProperty(p, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER, &vo);
		d = SDL_CreateGPUDeviceWithProperties(p);
		SDL_DestroyProperties(p);
		vk_fragment_stores = d != 0;
	}
#endif
#ifdef __APPLE__
	if (!d) d = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB, false, 0);   /* (METALLIB: a shader's library from this machine's archive, mtl_archive.m) */
#else
	if (!d) d = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, false, 0);
#endif
#ifdef RECOMP_MTL_ARCHIVE
	if (d && (SDL_GetGPUShaderFormats(d) & SDL_GPU_SHADERFORMAT_METALLIB) && msl_cache_dir() && !msl_salt()) mtl_archive_on = mtl_archive_open(msl_cache_dir(), msl_lib_store);
#endif
	if (d) fprintf(stderr, "    gpu: %s device %s%s\n", SDL_GetGPUDeviceDriver(d), SDL_GetStringProperty(SDL_GetGPUDeviceProperties(d), SDL_PROP_GPU_DEVICE_NAME_STRING, "?"), vk_fragment_stores ? " (robustBufferAccess, fragmentStoresAndAtomics on)" : "");
	return d;
}
/* D24_UNORM_S8_UINT isn't supported on Metal/Apple Silicon (no 24-bit depth format at all there);
 * query what the device actually supports instead of hardcoding one -- D32_FLOAT_S8_UINT is the
 * common combined depth-stencil fallback. */
static SDL_GPUTextureFormat depth_format(void) {
	static SDL_GPUTextureFormat f; static int have;
	if (!have) {
		have = 1;
		f = SDL_GPUTextureSupportsFormat(gpu(), SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
		    ? SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT : SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
	}
	return f;
}
static uint32_t index_at(const GfxDraw *d, uint32_t i) { return d->index_format ? RD32(d->indices + 4 * i) : RD16(d->indices + 2 * i); }

/* ---------------------------------------------------------------- shader modules, per translated program */
typedef struct { uint32_t addr, hash, hash2; int kind; uint32_t cube_mask, snap_mask; int pa_words; SDL_GPUShader *shader;   /* hash: the program's content; hash2: the vertex program's for a fragment shader (its varyings) */ int ok, pending; int smp_units[16]; int nsmp; int fetch; int ldst_sa[8], nldst;
	uint32_t layout_hash; int pcf_unit, pcf, points, fzero; int hnext; int low, prebuilt; uint64_t spv_hash, rid; int warm, wrefs, pred; uint64_t wkey; uint64_t lib_key; int on_disk; } ShaderEntry;   /* lib_key: its MSL cache file's, while that has no library yet (its first pipeline keeps one: mtl_archive.m); on_disk: that file is kept (not a predicted shader's) */   /* warm: a warm-up build's (outside the table: built, then released once wrefs reaches 0), wkey its key in warm.txt; low: queued behind every build a draw waits for (a shader recipe's prebuild; cleared when a draw wants it: bq_promote); prebuilt: made from a recipe, no draw has used it yet; spv_hash: its module's words (RECOMP_SHADER_RECIPES_VERIFY); rid: its recipe's id (0 none) */   /* pending: a build worker is compiling shader (gpu_async); shader and ok are final once it reads 0 */   /* fzero: a FRAGCOLOR program's blend-unit variant, its usse_fragment_fb_blend class (usse_spirv_set_fetch_zero; fetch 3 or 4), 0 none */   /* points: the point-sprite variant (usse_spirv_set_points) */   /* pcf: the filtered-shadow taps the module was built with (shadow_filter), on pcf_unit */   /* layout_hash: a vertex program's fetch plan (vf_plan: the draw's attribute layout the module decodes itself), 0 for the PA-record variant and for fragment programs */   /* ldst_sa: SA words holding the guest pointers the vertex program's LDSTs read through (SpirvModule.ldst_sa) */   /* fetch: SpirvModule.frag_fetch -- 1 samples the framebuffer copy at binding nsmp, 2 reads it as a framebuffer-fetch input */   /* kind: the colour target's, for fragment programs (-1 vertex); cube_mask: which units are cube maps (baked into the module) */
/* the shader table: never evicted (pipelines point at their shaders); looked up through a hash of the program's content
 * hash and its vertex program's -- a linear search over a long session's ~1500 variants ran for every draw. 2048 entries
 * filled in a 10-minute Killzone session with render-scale changes (each scale adds snap variants) */
#define MAXSHADER 8192
#define SHADER_HASH 4096
static ShaderEntry shaders[MAXSHADER]; static int nshaders; static int shader_head[SHADER_HASH];   /* bucket heads: index + 1 */

/* TEMP diagnostic (RECOMP_GPU_DEBUG_FLAT): a hand-written, known-good MSL pair bypassing the
 * translator entirely -- an oversized full-screen triangle (ignores the vertex buffer) and a flat
 * red fragment shader (ignores everything). If this renders, the pipeline/render-pass mechanics
 * are fine and the bug is in a translated shader's own math; if it doesn't, it's pipeline/pass
 * state. Declares none of the real resources (no storage buffer/samplers/uniforms). */
static const char *flat_msl =
	"#include <metal_stdlib>\nusing namespace metal;\n"
	"struct VOut { float4 position [[position]]; };\n"
	"vertex VOut main0(uint vid [[vertex_id]]) {\n"
	"  float2 p[3] = { float2(-1,1), float2(3,1), float2(-1,-3) };\n"
	"  VOut o; o.position = float4(p[vid], 0, 1); return o;\n"
	"}\n"
	"fragment float4 main0_frag(VOut in [[stage_in]]) { return float4(1,0,0,1); }\n"
	/* visualizes the real vertex shader's own location-0 varying directly, to tell an upstream
	 * (vertex-side) colour bug from a downstream (fragment-side) one */
	"struct VIn { float4 v0 [[user(locn0)]]; };\n"
	"fragment float4 main0_vis(VIn in [[stage_in]]) { return in.v0; }\n";

typedef struct { uint64_t msl_us, compile_us; int cached, no_disk; uint64_t lib_key; int on_disk, from_lib; } ShaderBuildTimes;   /* one make_shader call's (the t_shader_* totals are shared with the build workers); no_disk (in): the MSL isn't kept in the shader cache's files (a predicted shader's: the OS's cache is the point, and Killzone's ~10000 would be gigabytes) */
static SDL_GPUShader *make_shader(SpirvModule *m, int is_fragment, int nsmp, int nsbuf, uint32_t addr, ShaderBuildTimes *bt) {   /* nsbuf: vertex storage buffers (the PA input, plus the LDST windows); any thread (gpu_async) */
	SDL_GPUShaderFormat fmt = SDL_GetGPUShaderFormats(gpu());
	const void *code; size_t code_size; const char *entry = "main";
	const char *flatmode = getenv("RECOMP_GPU_DEBUG_FLAT");
	int flat = flatmode && (!strcmp(flatmode, "1") || !strcmp(flatmode, "both") || (is_fragment ? (!strcmp(flatmode, "frag") || !strcmp(flatmode, "vis")) : !strcmp(flatmode, "vert")));
	if (flat && (fmt & SDL_GPU_SHADERFORMAT_MSL)) {
		SDL_GPUShaderCreateInfo ci = {0};
		ci.code = (const Uint8 *)flat_msl; ci.code_size = strlen(flat_msl) + 1; ci.format = SDL_GPU_SHADERFORMAT_MSL;
		ci.entrypoint = !is_fragment ? "main0" : (flatmode && !strcmp(flatmode, "vis")) ? "main0_vis" : "main0_frag";
		ci.stage = is_fragment ? SDL_GPU_SHADERSTAGE_FRAGMENT : SDL_GPU_SHADERSTAGE_VERTEX;
		SDL_GPUShader *sh = SDL_CreateGPUShader(gpu(), &ci);
		if (!sh) fprintf(stderr, "    gpu: flat shader failed: %s\n", SDL_GetError());
		return sh;
	}
#ifdef RECOMP_SPIRV_CROSS
	size_t mlen = 0; void *msl = 0; MslLib lib = {0}; uint64_t key = 0;
	if (fmt & SDL_GPU_SHADERFORMAT_MSL) {
		uint64_t tx0 = us_now();
		key = msl_cache_key(m->words, m->nwords, is_fragment, nsmp, nsbuf);
#ifdef RECOMP_MTL_ARCHIVE
		MslLib *want_lib = mtl_archive_on ? &lib : 0;
#else
		MslLib *want_lib = 0;
#endif
		msl = msl_cache_get(key, m->words, m->nwords, is_fragment, nsmp, nsbuf, &mlen, want_lib);   /* the cache (above), else spirv-cross, and the cache remembers */
		if (msl) { bt->cached = 1; bt->on_disk = 1; __atomic_add_fetch(&n_shader_cached, 1, __ATOMIC_RELAXED); }
		else { msl = spirv_cross_to_msl(m->words, m->nwords, is_fragment, nsmp, nsbuf, &mlen); if (msl && !bt->no_disk) bt->on_disk = msl_cache_put(key, m->words, m->nwords, is_fragment, nsmp, nsbuf, msl, mlen); }
		bt->msl_us = us_now() - tx0; __atomic_add_fetch(&t_shader_msl, bt->msl_us, __ATOMIC_RELAXED);
		if (!msl) return 0;
		/* RECOMP_GPU_DEBUG_DUMP=1: save each shader's cross-compiled MSL to /tmp/gpudump_<addr>_<stage>.metal
		 * (keyed by guest address, not a bare counter -- that's what made the earlier version of
		 * this useful for bisecting a real bug down to one specific program's generated source) */
		if (getenv("RECOMP_GPU_DEBUG_DUMP")) { char path[64]; snprintf(path, sizeof path, "/tmp/gpudump_%08x_%s.metal", addr, is_fragment ? "frag" : "vert");
			FILE *f = fopen(path, "wb"); if (f) { fwrite(msl, 1, mlen, f); fclose(f); } }
		{ const char *salt_tag = msl_salt(); const int salt = salt_tag != 0;   /* RECOMP_GPU_SALT_MSL=1 (measurement): a unique comment per shader, so the OS's Metal compiler cache never hits -- a first-ever compile; =<tag>: the same comment on every shader, a machine whose cache has seen none of them until a run with that tag (the warm-up's test) */
		  if (salt) { static unsigned n; char tag[96]; int tl = strcmp(salt_tag, "1") ? snprintf(tag, sizeof tag, "\n// salt %.60s\n", salt_tag) : snprintf(tag, sizeof tag, "\n// salt %d %u\n", (int)getpid(), __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED)); char *m2 = realloc(msl, mlen + (size_t)tl); if (m2) { memcpy(m2 + mlen - 1, tag, (size_t)tl + 1); msl = m2; mlen += (size_t)tl; } } }
		code = msl; code_size = mlen; entry = "main0"; fmt = SDL_GPU_SHADERFORMAT_MSL;   /* spirv-cross's MSL entry point name; `fmt` must name exactly the one format being provided, not every format the device supports */
#ifdef RECOMP_MTL_ARCHIVE
		if (lib.air && !msl_salt()) { code = lib.air; code_size = lib.n; entry = lib.entry; fmt = SDL_GPU_SHADERFORMAT_METALLIB; bt->from_lib = 1; }   /* this machine compiled it before: its library, not the MSL (Metal's compile of it: ~70 ms cold) */
		else if (mtl_archive_on && bt->on_disk && !msl_salt()) bt->lib_key = key;   /* (its first pipeline keeps its library) */
#endif
	} else
#endif
	{ code = m->words; code_size = 4 * (size_t)m->nwords; fmt = SDL_GPU_SHADERFORMAT_SPIRV; }
	SDL_GPUShaderCreateInfo ci = {0};
	ci.code = code; ci.code_size = code_size; ci.entrypoint = entry; ci.format = fmt;
	ci.stage = is_fragment ? SDL_GPU_SHADERSTAGE_FRAGMENT : SDL_GPU_SHADERSTAGE_VERTEX;
	ci.num_samplers = !flat ? (uint32_t)nsmp : 0;   /* (a vertex program's own textures too: usse_spirv.c's vertex SMP) */
	ci.num_storage_buffers = (!is_fragment && !flat) ? (uint32_t)nsbuf : 0;
	ci.num_uniform_buffers = flat ? 0 : 1;   /* SA, both stages */
	uint64_t tc0 = us_now();
	SDL_GPUShader *sh = SDL_CreateGPUShader(gpu(), &ci);
#ifdef RECOMP_MTL_ARCHIVE
	if (bt->from_lib && sh) __atomic_add_fetch(&n_shader_libs, 1, __ATOMIC_RELAXED);
	if (bt->from_lib && !sh) {   /* a kept library Metal won't take (another OS's compiler's?): the MSL, and its library kept again */
		static int said; if (!said++) fprintf(stderr, "    gpu: shader archive: a kept library didn't load (%s): compiling the MSL\n", SDL_GetError());
		ci.code = msl; ci.code_size = mlen; ci.entrypoint = entry = "main0"; ci.format = fmt = SDL_GPU_SHADERFORMAT_MSL; bt->from_lib = 0; bt->lib_key = key;
		sh = SDL_CreateGPUShader(gpu(), &ci);
	}
#endif
	bt->compile_us = us_now() - tc0; __atomic_add_fetch(&t_shader_compile, bt->compile_us, __ATOMIC_RELAXED);
	if (!sh) fprintf(stderr, "    gpu: make_shader: fmt=%u stage=%d samplers=%u storage=%u uniforms=%u code_size=%zu entry=%s err=%s\n",
	                  (unsigned)fmt, ci.stage, ci.num_samplers, ci.num_storage_buffers, ci.num_uniform_buffers, code_size, entry, SDL_GetError());
#ifdef RECOMP_SPIRV_CROSS
	free(msl); free(lib.air);
#endif
	return sh;
}
/* FRAGCOLOR_USED on the GPU (the Vita's programmable blending: the fragment program starts from its pixel's
 * colour, see gfx_gpu_draw): how the translated program gets that colour. 1 = copy: the colour target is
 * copied before the draw (a copy pass between two render passes) and the copy sampled at gl_FragCoord --
 * works on every backend. 2 = fb: framebuffer fetch -- the program reads [[color(0)]] from tile memory, no
 * copy, no pass split, and in primitive order (a draw's later triangles see its earlier ones, as on the
 * hardware and in gfx_soft.c; the copy path read the pre-draw colour for the whole draw). Metal on Apple GPUs
 * only (spirv-cross emits it, Metal supports it on Apple silicon). The default is fb there, copy elsewhere;
 * RECOMP_GPU_FETCH=copy|fb overrides. Decided once, before the first shader is translated (the mode is
 * baked into the fragment module). */
static int fetch_mode(void) {
	static int m;
	if (!m) {
		int msl = (SDL_GetGPUShaderFormats(gpu()) & SDL_GPU_SHADERFORMAT_MSL) != 0;
#if defined(__APPLE__) && defined(__aarch64__)
		m = msl ? 2 : 1;
#else
		m = 1;
#endif
		const char *e = getenv("RECOMP_GPU_FETCH");
		if (e && !strcmp(e, "copy")) m = 1;
		else if (e && !strcmp(e, "fb")) { if (msl) m = 2; else fprintf(stderr, "    gpu: RECOMP_GPU_FETCH=fb needs the Metal backend: FRAGCOLOR draws copy the target\n"); }
		else if (e && *e) fprintf(stderr, "    gpu: RECOMP_GPU_FETCH=%s: not copy or fb, ignored\n", e);
		usse_spirv_set_fetch_mode(m);
		{	/* the lean F16 conversion: verified bit-exact on Apple silicon's GPU only (usse_spirv.c); RECOMP_GPU_F16_LEAN=0 for the old form */
			const char *l = getenv("RECOMP_GPU_F16_LEAN");
#if defined(__APPLE__) && defined(__aarch64__)
			usse_spirv_set_f16_lean(msl && !(l && *l == '0'));
#else
			usse_spirv_set_f16_lean(l && *l == '1');
#endif
		}
		if (trace_sync()) fprintf(stderr, "gpu trace: FRAGCOLOR fetch mode %s\n", m == 2 ? "fb (framebuffer fetch)" : "copy");
	}
	return m;
}
/* canonical varyings (usse_spirv_set_canonical_varyings): a fragment shader no longer depends on the vertex program it is
 * drawn with -- its key drops the pairing (ShaderEntry.hash2 0), the draw pushes the vertex program's component counts
 * (UsseDrawUniforms.vfetch[2..3]), and a fragment program's shader can be built from its own bytes alone (a recipe needs no
 * vertex program registered). RECOMP_GPU_CANON_VARY=0: the positional layout, a fragment shader per pairing, as before */
static int canon_vary_on(void) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_GPU_CANON_VARY"); on = !(e && *e == '0'); usse_spirv_set_canonical_varyings(on); }
	return on;
}
/* predicted shaders (RECOMP_SHADER_PREDICT=0: off): with canonical varyings a fragment shader's variant follows from its
 * program's bytes -- the kind only for a raw-word colour, cube units from its 3-component reads (usse_spirv_predict_cube),
 * its samplers from its own literals and dependent-sampler table, no render-scale snap: 2071 of Killzone's 2077 recorded
 * fragment shaders were exactly that (the 6 others 2x snap variants). So once the recipes' warm-up is done it builds every
 * other fragment program in the game's files in that variant and releases it (warm.txt keeps what was built: once per
 * machine): a level nobody recorded finds its shader libraries in the OS's cache. Pipelines aren't predicted: their state
 * is the draw's. Vertex programs neither: their fetch plans are the game's vertex formats */
/* predicted pipelines (RECOMP_PIPE_PREDICT=0|1; on by default on Vulkan with an NVIDIA device): on Vulkan a shader module
 * compiles nothing, the pipeline is the whole cost -- but NVIDIA's driver keeps a compiled fragment stage for any other
 * vertex shader and target format with the same blend and depth/stencil state (scratchpad pipereuse: 120 Killzone
 * pipelines 12.6 s cold, 0.86 s again with another vertex shader, 10.4 s with another blend; Mesa's RADV recompiles for
 * a new vertex shader instead). So each predicted fragment shader is also built into pipelines with a stand-in vertex
 * shader (any vertex shader fits any fragment shader with canonical varyings) in the recipes' most common states
 * (pred_states) -- Killzone: 5 (blend, depth/stencil) states hold 90% of its recorded pipelines */
/* one pipeline layout for every vertex shader (RECOMP_GPU_VS_LAYOUT=0|1; on by default on Vulkan): each declares two
 * storage buffers, the PA input and the LDST windows, whether it has windows or not (a window-less draw binds the PA
 * buffer twice). A Vulkan pipeline's layout is the whole pipeline's: NVIDIA recompiled a fragment stage it had for every
 * vertex shader whose layout differed -- a skinned one's second buffer recompiled all 120 a stand-in had built (12.6 s),
 * with one layout for all it took 1.3 s (scratchpad pipereuse). Off on Metal: its argument tables are per stage, and the
 * MSL would change for nothing */
static int vs_layout_uniform(void) {
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_GPU_VS_LAYOUT"); on = e && *e ? *e != '0' : !(SDL_GetGPUShaderFormats(gpu()) & SDL_GPU_SHADERFORMAT_MSL); }
	return on;
}
static int pipe_predict_on(void) {
	static int on = -1;
	if (on < 0) {
		const char *e = getenv("RECOMP_PIPE_PREDICT");
		if (e && *e) on = *e != '0';
		else { const char *name = SDL_GetStringProperty(SDL_GetGPUDeviceProperties(gpu()), SDL_PROP_GPU_DEVICE_NAME_STRING, "");
			on = canon_vary_on() && !(SDL_GetGPUShaderFormats(gpu()) & SDL_GPU_SHADERFORMAT_MSL) && name && strstr(name, "NVIDIA"); }
	}
	return on;
}
static int predict_on(void) {   /* (Metal by default, where the shader library is most of the cost; on Vulkan only for predicted pipelines -- a module alone compiles nothing; RECOMP_SHADER_PREDICT=1 forces it) */
	static int on = -1;
	if (on < 0) { const char *e = getenv("RECOMP_SHADER_PREDICT"); on = canon_vary_on() && (e && *e ? *e != '0' : (SDL_GetGPUShaderFormats(gpu()) & SDL_GPU_SHADERFORMAT_MSL) != 0 || pipe_predict_on()); }
	return on;
}
/* filtered shadows (settings "shadow_filter": 0 the game's own single sample, 1 soft -- the default -- 2 softer;
 * RECOMP_SHADOW_FILTER wins): a fragment program that names one of its samplers like a shadow map (Uncharted's
 * resolve: g_tShadowMap) is built with usse_spirv_set_pcf -- 16 taps on a 4x4 grid, one (soft) or two (softer)
 * shadow-map texels apart at 1x, whatever the render scale -- and averages its own compare over them. The Vita
 * takes one tap: at a PC resolution every sunlit leaf showed hard, texel-sized steps. */
static int g_points_draw;   /* gfx_gpu_draw: this draw is SCE_GXM_PRIMITIVE_POINTS sprites (get_shader picks the point-sprite variants) */
static int g_fzero_draw;    /* gfx_gpu_draw: this draw's fragment program's framebuffer blend goes to the blend unit -- its usse_fragment_fb_blend class (get_shader picks that variant) */
static int shadow_filter_want(void) {
	extern int settings_int(const char *, int);
	const char *e = getenv("RECOMP_SHADOW_FILTER"); int n = e ? atoi(e) : settings_int("shadow_filter", 1);
	return n < 0 ? 0 : n > 2 ? 2 : n;
}
/* the texture unit a fragment program samples its shadow map through: a sampler parameter (category 2) whose
 * name says "shadow", -1 for none. By content hash: the shader patcher reuses addresses for other programs. */
static int shadow_unit(uint32_t prog, uint32_t hash) {
	static struct { uint32_t hash; int unit; } memo[512]; static int nmemo;
	for (int i = 0; i < nmemo; i++) if (memo[i].hash == hash) return memo[i].unit;
	int unit = -1;
	uint32_t cnt = RD32(prog + 0x24), base = prog + 0x28 + RD32(prog + 0x28);
	for (uint32_t i = 0; i < cnt && i < 64 && unit < 0; i++) {
		uint32_t p = base + 16 * i;
		if ((RD16(p + 4) & 15) != 2) continue;
		const char *name = (const char *)G(p + (int32_t)RD32(p));
		char low[64]; size_t k = 0; for (; k < sizeof low - 1 && name[k]; k++) low[k] = (char)tolower((unsigned char)name[k]); low[k] = 0;
		if (strstr(low, "shadow")) { int r = (int32_t)RD32(p + 12); if (r >= 0 && r < 16) unit = r; }
	}
	if (nmemo < 512) { memo[nmemo].hash = hash; memo[nmemo].unit = unit; nmemo++; }
	return unit;
}
/* ---------------------------------------------------------------- asynchronous builds
 * A first-ever Metal compile of a translated program takes 10-180 ms: Killzone's frame_3408 replayed with the
 * OS compiler cache defeated (RECOMP_GPU_SALT_MSL) built 320 shaders in 21.4 s, 67 ms each. They ran on the
 * render thread at each program's first draw, so every new effect, enemy or room froze the game for a few
 * frames -- and a translator change (usse_spirv.c) makes every shader first-ever again. gpu_async (on with a
 * window, i.e. in play): the MSL step and the compile run on build workers, and so does a pipeline once both
 * its shaders exist (Vulkan's driver compiles there); a draw whose shader or pipeline is still being built is
 * skipped -- the object shows up a few frames late instead of the game stopping -- as emulators' asynchronous
 * shader modes do. Translation (0.06 ms a program) stays on the render thread: the translator's set_* switches
 * are global. Headless runs and replays build synchronously, so the gate's and the shots' output stays
 * deterministic; RECOMP_GPU_ASYNC=0|1 overrides, RECOMP_GPU_ASYNC_THREADS=n sets the workers (default two thirds
 * of the cores, 1-8; below the game's threads in priority). Creation is safe off the render thread: SDL's Metal backend calls only MTLDevice
 * methods there and its Vulkan backend locks its layout caches (SDL 3.4's SDL_gpu_metal.m / SDL_gpu_vulkan.c);
 * no command buffer is touched. */
static int gpu_async(void) {
	static int v = -1;
	if (v < 0) { extern int window_active(void); const char *e = getenv("RECOMP_GPU_ASYNC"); v = e && *e ? atoi(e) != 0 : window_active(); }
	return v;
}
/* gpu_overlap (with gpu_async; RECOMP_GPU_OVERLAP=0 off): a draw that must wait for its builds (not skippable: see
 * skip_lossless) no longer waits at the draw -- it goes into the batch with its pipeline still pending, and the batch's
 * flush waits for them all at once. A pipeline whose shaders are still compiling waits in bq_wait and is queued as they
 * finish (bq_release), so the next draws' shaders compile alongside instead of after. It was one build at a time while the
 * other workers idled: Killzone's first visit through a door waited for ~20 shaders and ~10 pipelines in turn (2026-10-20). */
static int gpu_overlap(void) {
	static int v = -1;
	if (v < 0) { const char *e = getenv("RECOMP_GPU_OVERLAP"); v = gpu_async() && !(e && *e == '0'); }
	return v;
}
typedef struct { ShaderEntry *e; SpirvModule m; int is_frag, nsmp, nsbuf; uint32_t addr; uint64_t t0, xlate_us; int async; } ShaderJob;   /* nsmp/nsbuf: make_shader's */
typedef struct { struct PipeEntry *p; SDL_GPUGraphicsPipelineCreateInfo ci; SDL_GPUColorTargetDescription ctd; uint32_t vprog, fprog; uint64_t t0; int async;
	ShaderEntry *vse, *fse; int null_fs; int warm; uint64_t wkey; } PipeJob;   /* warm: a warm-up build's (p outside the table: the pipeline is released when built) */   /* vse/fse: its shaders -- their objects go into ci when it's built (gpu_overlap: queued while they were still building) */   /* ci's colour target is ctd (pipeline_build points it there) */
typedef struct { struct ViewVariant *slot; char *src; const char *entry; SDL_GPUShader *vsh; int fmt; uint64_t t0; } ViewJob;   /* a baked view variant (view_pipeline_for): its MSL text, owned by the job */
typedef struct BuildJob { struct BuildJob *next; int is_pipe; ShaderJob sj; PipeJob pj; ViewJob vj; int low, prebuild; } BuildJob;   /* is_pipe: 0 a shader, 1 a pipeline, 2 a view variant; low: in bq_head[2] (bq_promote clears it); prebuild: a shader recipe's (counted in bq_low until done) */
static uint32_t n_async_skips;   /* draws skipped this frame: a build in flight */
/* the shader recipes' warm-up builds (below, with the recipes): done ones reported to the render thread, which releases them */
static pthread_mutex_t warm_m = PTHREAD_MUTEX_INITIALIZER;
static struct WarmDone { uint64_t key; int kind; } *warm_dones; static int nwarm_dones, warm_dones_cap;
static struct WarmRel { ShaderEntry *e; SDL_GPUGraphicsPipeline *pipe; } *warm_rels; static int nwarm_rels, warm_rels_cap;
static int warm_record;   /* warm.txt is open: every build that works goes into it (the OS's shader cache has it from then on) */
static void warm_done(uint64_t key, int kind, int ok) {   /* any thread: a build that worked, for warm.txt */
	if (!ok || !key || !__atomic_load_n(&warm_record, __ATOMIC_RELAXED)) return;
	pthread_mutex_lock(&warm_m);
	if (nwarm_dones == warm_dones_cap) { int c = warm_dones_cap ? 2 * warm_dones_cap : 1024; struct WarmDone *n = realloc(warm_dones, c * sizeof *n); if (n) { warm_dones = n; warm_dones_cap = c; } }
	if (nwarm_dones < warm_dones_cap) warm_dones[nwarm_dones++] = (struct WarmDone){ key, kind };
	pthread_mutex_unlock(&warm_m);
}
static void warm_release(ShaderEntry *e, SDL_GPUGraphicsPipeline *pipe) {   /* any thread: for the render thread to release */
	pthread_mutex_lock(&warm_m);
	if (nwarm_rels == warm_rels_cap) { int c = warm_rels_cap ? 2 * warm_rels_cap : 1024; struct WarmRel *n = realloc(warm_rels, c * sizeof *n); if (n) { warm_rels = n; warm_rels_cap = c; } }
	if (nwarm_rels < warm_rels_cap) warm_rels[nwarm_rels++] = (struct WarmRel){ e, pipe };
	pthread_mutex_unlock(&warm_m);
}
static void warm_unref(ShaderEntry *e) { if (e && !__atomic_sub_fetch(&e->wrefs, 1, __ATOMIC_ACQ_REL)) warm_release(e, 0); }
static void bq_release(void);   /* (the build queue, below) */
static void shader_build(ShaderJob *j) {   /* the MSL and the compile, on the render thread or a build worker; then the entry is final */
	ShaderEntry *e = j->e; ShaderBuildTimes bt = {0}; bt.no_disk = e->pred;
	SDL_GPUShader *sh = make_shader(&j->m, j->is_frag, j->nsmp, j->nsbuf, j->addr, &bt);
	usse_spirv_free(&j->m);
	if (hitch_log() && !e->prebuilt) fprintf(stderr, "    gpu hitch: %s shader %#x kind %d snap %#x layout %08x %s in %.2f ms (translate %.2f, msl %.2f%s, compile %.2f)\n", j->is_frag ? "fragment" : "vertex", j->addr, e->kind, e->snap_mask, e->layout_hash,
	                         j->async ? "ready (built by a worker)" : "built", (us_now() - j->t0) / 1e3, j->xlate_us / 1e3, bt.msl_us / 1e3, bt.cached ? " cached" : "", bt.compile_us / 1e3);
	if (!sh) fprintf(stderr, "    gpu: %#x: SDL_CreateGPUShader failed: %s\n", j->addr, SDL_GetError());
	e->shader = sh; e->ok = sh != 0; e->lib_key = bt.lib_key; e->on_disk = bt.on_disk;
	__atomic_store_n(&e->pending, 0, __ATOMIC_RELEASE);
	bq_release();
	warm_done(e->wkey, 's', e->ok);
	if (e->warm) warm_unref(e);
}
static void pipeline_build(PipeJob *j);   /* (with the pipelines, below) */
static void view_variant_build(ViewJob *j);   /* (with the views, below) */
static pthread_mutex_t bq_m = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t bq_c = PTHREAD_COND_INITIALIZER, bq_idle = PTHREAD_COND_INITIALIZER;
static int bq_inflight;   /* queued or building (or waiting for their shaders: bq_wait) */
static BuildJob *bq_wait;   /* pipeline jobs whose shaders are still building (gpu_overlap): queued by bq_release as each shader is done */
static BuildJob *bq_head[3], *bq_tail[3];   /* [0] pipelines (their shaders are done: nearest to drawing), [1] shaders, [2] the shader recipes' prebuilds (low) */
static int bq_low;   /* the recipes' jobs not done yet (BuildJob.prebuild): not the game waiting (gfx_gpu_build_status) */
static int bq_low_active, bq_low_cap;   /* workers on bq_head[2] jobs now, and at most: RECOMP_SHADER_PREBUILD_RESERVE (default 2) stay free for what a draw
                                         * wants -- with all 8 on prebuilds, a draw's promoted build waited for one of them to finish a cold compile of up to ~2 s */
static int bq_workers;   /* 0 not started, -1 none could start */
static void *build_worker(void *arg) {
	(void)arg;
#ifdef __APPLE__
	pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);   /* below the game's threads; the Metal compiler service runs at the requester's QoS. Frame_3408 cold: as fast as the default class (8 workers 4.0 s, 6 workers 4.7 vs 4.9) */
#elif defined(__linux__)
	setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);   /* (the Vulkan driver compiles on this thread) */
#endif
	for (;;) {
		pthread_mutex_lock(&bq_m);
		while (!bq_head[0] && !bq_head[1] && !(bq_head[2] && bq_low_active < bq_low_cap)) pthread_cond_wait(&bq_c, &bq_m);
		int q = bq_head[0] ? 0 : bq_head[1] ? 1 : 2; BuildJob *j = bq_head[q];
		if (!(bq_head[q] = j->next)) bq_tail[q] = 0;
		bq_low_active += q == 2;
		pthread_mutex_unlock(&bq_m);
		if (j->is_pipe == 2) view_variant_build(&j->vj); else if (j->is_pipe) pipeline_build(&j->pj); else shader_build(&j->sj);
		const int pre = j->prebuild; free(j);
		pthread_mutex_lock(&bq_m); if (pre) bq_low--; if (q == 2) { bq_low_active--; pthread_cond_signal(&bq_c); } if (!--bq_inflight) pthread_cond_broadcast(&bq_idle); pthread_mutex_unlock(&bq_m);
	}
	return 0;
}
void gfx_gpu_builds_wait(void) {   /* until every queued build is done (a replay's RECOMP_GPU_ASYNC=1 measurement: how long the workers take) */
	pthread_mutex_lock(&bq_m); while (bq_inflight) pthread_cond_wait(&bq_idle, &bq_m); pthread_mutex_unlock(&bq_m);
}
static void bq_release(void) {   /* the waiting pipelines whose shaders are both done, into the queue (counted in bq_inflight already) */
	pthread_mutex_lock(&bq_m);
	for (BuildJob **pj = &bq_wait; *pj; ) {
		BuildJob *j = *pj;
		if ((j->pj.vse && __atomic_load_n(&j->pj.vse->pending, __ATOMIC_ACQUIRE)) || (j->pj.fse && __atomic_load_n(&j->pj.fse->pending, __ATOMIC_ACQUIRE))) { pj = &j->next; continue; }
		*pj = j->next; j->next = 0;
		const int q = j->low ? 2 : 0;
		if (bq_tail[q]) bq_tail[q]->next = j; else bq_head[q] = j;
		bq_tail[q] = j; pthread_cond_signal(&bq_c);
	}
	pthread_mutex_unlock(&bq_m);
}
static int build_enqueue(BuildJob *src) {   /* 0: no worker -- the caller builds it */
	if (!bq_workers) {
		const char *e = getenv("RECOMP_GPU_ASYNC_THREADS"); long ncpu = plat_cpu_count();
		int n = e && *e ? atoi(e) : (int)(ncpu * 2 / 3); if (n < 1) n = 1; if (n > 8) n = 8;   /* frame_3408's 336 cold builds: 1 worker 20.8 s, 4 ~4.5 s, 8 ~2.3 s, 12 no faster (this 12-core Mac) */
		bq_workers = -1;
		for (int i = 0; i < n; i++) { pthread_t t; if (!pthread_create(&t, 0, build_worker, 0)) { pthread_detach(t); bq_workers = bq_workers < 0 ? 1 : bq_workers + 1; } }
		{ const char *r = getenv("RECOMP_SHADER_PREBUILD_RESERVE"); const int keep = r && *r ? atoi(r) : 2;
		  pthread_mutex_lock(&bq_m); bq_low_cap = bq_workers - keep < 1 ? 1 : bq_workers - keep; pthread_mutex_unlock(&bq_m); }
		fprintf(stderr, "    gpu: shaders and pipelines build asynchronously on %d worker thread%s (RECOMP_GPU_ASYNC=0: on the render thread)\n", bq_workers, bq_workers == 1 ? "" : "s");
	}
	if (bq_workers < 0) return 0;
	BuildJob *j = malloc(sizeof *j); if (!j) return 0;
	*j = *src; j->next = 0; j->sj.async = j->pj.async = 1; j->prebuild = j->low;
	int q = j->low ? 2 : j->is_pipe ? 0 : 1;
	pthread_mutex_lock(&bq_m);
	if (j->prebuild) bq_low++;
	if (j->is_pipe == 1 && ((j->pj.vse && __atomic_load_n(&j->pj.vse->pending, __ATOMIC_ACQUIRE)) || (j->pj.fse && __atomic_load_n(&j->pj.fse->pending, __ATOMIC_ACQUIRE)))) {
		j->next = bq_wait; bq_wait = j; bq_inflight++;   /* (checked under the lock shader_build's bq_release takes after clearing pending: none slips between) */
		pthread_mutex_unlock(&bq_m); return 1;
	}
	if (bq_tail[q]) bq_tail[q]->next = j; else bq_head[q] = j;
	bq_tail[q] = j; bq_inflight++;
	pthread_cond_signal(&bq_c);
	pthread_mutex_unlock(&bq_m);
	return 1;
}
static int shader_pending(const ShaderEntry *e) { return e && __atomic_load_n(&e->pending, __ATOMIC_ACQUIRE); }
/* a draw wants a build a shader recipe queued (low): to the front of its queue -- or, for a pipeline still waiting
 * in bq_wait, its own shaders, and it goes to the pipelines' queue when they're done (render thread) */
static void bq_promote_locked(ShaderEntry *e, struct PipeEntry *p) {
	BuildJob *prev = 0;
	for (BuildJob *j = bq_head[2]; j; prev = j, j = j->next) {
		if (e ? !(j->is_pipe == 0 && j->sj.e == e) : !(j->is_pipe == 1 && j->pj.p == p)) continue;
		if (prev) prev->next = j->next; else bq_head[2] = j->next;
		if (bq_tail[2] == j) bq_tail[2] = prev;
		const int q = e ? 1 : 0; j->low = 0;
		j->next = bq_head[q]; bq_head[q] = j; if (!bq_tail[q]) bq_tail[q] = j;
		pthread_cond_signal(&bq_c);   /* (a worker held back by bq_low_cap takes it) */
		return;
	}
	if (p) for (BuildJob *j = bq_wait; j; j = j->next) if (j->is_pipe == 1 && j->pj.p == p) {
		j->low = 0;
		if (j->pj.vse && j->pj.vse->low) { j->pj.vse->low = 0; bq_promote_locked(j->pj.vse, 0); }
		if (j->pj.fse && j->pj.fse->low) { j->pj.fse->low = 0; bq_promote_locked(j->pj.fse, 0); }
		return;
	}
}
static void bq_promote(ShaderEntry *e, struct PipeEntry *p) { pthread_mutex_lock(&bq_m); bq_promote_locked(e, p); pthread_mutex_unlock(&bq_m); }

/* ---------------------------------------------------------------- shader recipes
 * A first visit to an area builds its shaders and pipelines as the game first draws with them: seconds of "Compiling
 * shaders" at a Killzone door (2026-10-20), and each one is first-ever again after a translator change (usse_spirv.c) or on
 * a new machine. A recipe is what one of them is built from, less the program itself: the content hash of the game's GXP
 * (the program a draw uses is one the game registered with Sony's shader patcher, its bytes unchanged -- all 406 Killzone
 * drew with sat at registered addresses), for a fragment shader its vertex program's, the target's kind, the cube/snap/
 * point/framebuffer-blend variant, the units its dependent samplers read (usse_spirv_sa_read: the only SA entries a module
 * depends on), a vertex shader's fetch plan (UsseVfLayout); for a pipeline, the draw state get_pipeline reads. Not a byte
 * of the game, so a game's recipes ship with the port: packs/<title>/shaders.txt (RECOMP_SHADER_RECIPES_SHIPPED; `vitarecomp
 * recipes <title>` merges a player's into it).
 * Play records every shader and pipeline it creates (shader_cache/recipes.txt; RECOMP_SHADER_RECIPES=<file> elsewhere, =0
 * off). As the game registers each program -- at boot and on each level's load screen, before the level's first draws
 * (Killzone: frames 968-1018, draws from 1050) -- the recipes it completes are translated on the render thread, a slice of
 * each frame (RECOMP_SHADER_PREBUILD_MS, default 2), and compiled by the build workers behind every build a draw waits for;
 * a draw that reaches one still queued moves it to the front (bq_promote). In play only (gpu_async and a window, or
 * RECOMP_SHADER_RECIPES set): headless runs and replays are unchanged. RECOMP_SHADER_RECIPES_VERIFY=1: each prebuilt shader
 * a draw first uses is translated again from the draw's own inputs and the two modules compared; new records are logged.
 * The files: RECIPES_HEADER, then a line per record (the order doesn't matter; a merge is a sort -u):
 *   S <id> <v|f> <hash> <hash2> <kind> <cube_mask> <snap_mask> <points> <fzero> <SA unit entries: idx:unit,... | ->
 *     <fetch plan: n,npa,index_reg,instance_reg/stream,offset,stride,reg,fmt,comps,kind,dcomps,rawlen/... | ->
 *   P <vertex S id> <fragment S id> <get_pipeline's state: pipe_state's NPV fields>
 * where an S line's id is a hash of the rest of it (checked on load). */
static uint64_t hash_bytes(const void *p, size_t n);   /* (textures, below) */
#define RECIPES_HEADER "# ativ shader recipes 1"
#define NPV 26
typedef struct { uint64_t id; int is_frag, kind, points, fzero, nsa, has_vf, state; uint32_t hash, hash2, cube_mask, snap_mask; uint8_t sa_idx[32], sa_unit[32];
	UsseVfLayout vf; ShaderEntry *e; int pipes;
	uint64_t wkey; int wstate, wneed, wleft, wheld; ShaderEntry *we; } Recipe;   /* state: 0 waiting for its programs, 1 built (e), 2 passed over; pipes: its first PipeLink + 1; w*: the warm-up's (recipes_warm) */
typedef struct { int vs, fs, done; int32_t v[NPV]; uint64_t wkey; int wneed, wdone; } PipeRecipe;   /* vs/fs: Recipe indices */
typedef struct { int pipe, next; } PipeLink;
typedef struct { uint32_t hash, addr; int occ, nuse, use, fill; } RecipeProg;   /* a program the recipes name: where the game registered it (0 not yet), its recipes (rc.use[use .. use+nuse)) */
static struct {
	int state, fd;   /* state: 0 not loaded, 1 loaded (recording while fd >= 0), -1 off */
	Recipe *r; int nr; PipeRecipe *p; int np; PipeLink *l; int nl; int *use;
	RecipeProg *prog; uint32_t prog_cap;
	int *work; int nwork, work_at, work_cap, work_sorted;   /* recipes to try: their programs were just registered (tried in index order: the order play first used them) */
	uint32_t *reg; int nreg, reg_at, reg_cap;   /* registered programs not looked at yet (the render thread's copy of reg_q) */
	uint64_t burst_t0, t_render; int burst, regs, built_sh, built_pp, have, skipped, stale;   /* a burst: the work a level's registrations started */
	int used_sh, used_pp, verified, differ, recorded, too_many_sa, p_missed;
	int warm_phase, warm_at, warm_fd, w_keyed, w_missing, w_built, w_fail, w_sh, w_pp, w_need_sh, w_need_pp; uint32_t scratch[2]; uint64_t warm_t0, t_warm;   /* the warm-up (recipes_warm): phase 0 off, 1 waiting for the program index, 2 keys, 3 builds, 4 queued, 6 predicted shaders (predict_warm), 7 their builds queued, 5 done */
	int pred_at, p_frag, p_sh, p_warm, p_recorded, p_fail; uint64_t pred_t0;   /* the predicted shaders' pass */
	int32_t pst[8][26]; int npst, p_pp, p_pp_warm; ShaderEntry *stand_vs;   /* predicted pipelines (pipe_predict_on): their states (pred_states) and stand-in vertex shader */
} rc = { .fd = -1, .warm_fd = -1 };
static const char *recipes_file(void) {   /* this machine's recipes: what play recorded */
	static char path[1100]; static int done;
	if (!done) {
		done = 1;
		const char *e = getenv("RECOMP_SHADER_CACHE"), *m = getenv("RECOMP_SHADER_RECIPES");
		const char *d = e ? (*e && strcmp(e, "0") ? e : 0) : "shader_cache";   /* (msl_cache_dir's) */
		if (m && *m && strcmp(m, "0") && strcmp(m, "1")) snprintf(path, sizeof path, "%s", m);
		else if (d && !(m && *m == '0')) { if (mkdir(d, 0755) && errno != EEXIST) return 0; snprintf(path, sizeof path, "%s/recipes.txt", d); }
	}
	return path[0] ? path : 0;
}
static int recipes_on(void) {
	static int v = -1;
	if (v < 0) { extern int window_active(void); const char *m = getenv("RECOMP_SHADER_RECIPES");
		v = gpu_async() && (window_active() || (m && *m && *m != '0')) && recipes_file() && !getenv("RECOMP_PROBE") && !getenv("RECOMP_GPU_DEBUG_FLAT"); }   /* play, or asked for (a headless test); a probe or the flat shaders aren't what a draw would build */
	return v;
}
static int recipes_verify(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_SHADER_RECIPES_VERIFY") != 0; return v; }
#define RC_SET (1 << 17)
#define WTAG_S (0x5753ull << 48)   /* (warm.txt's keys in the set: a shader module, a pipeline) */
#define WTAG_P (0x5750ull << 48)
static uint64_t rc_set[RC_SET]; static int rc_nset;   /* every record's key, loaded or written (open addressing; 0 empty) */
static uint64_t *rc_slot(uint64_t k) {
	k |= 1;
	for (uint32_t i = (uint32_t)(k ^ k >> 32) & (RC_SET - 1); ; i = (i + 1) & (RC_SET - 1)) if (!rc_set[i] || rc_set[i] == k) return &rc_set[i];
}
static int rc_set_add(uint64_t k) {   /* 1: new (0 too once the set is full: recording stops) */
	uint64_t *s = rc_slot(k); if (*s) return 0;
	if (rc_nset >= RC_SET * 3 / 4) { static int said; if (!said++) fprintf(stderr, "    gpu: shader recipes: %d records, no room for more: recording stops\n", rc_nset); return 0; }
	*s = k | 1; rc_nset++; return 1;
}
/* an S line's text after its id (rc_shader_text) and its id; 0 if it can't be written */
static uint64_t rc_shader_text(char *o, size_t n, const ShaderEntry *e, int is_frag, const uint8_t *idx, const uint8_t *unit, int nsa, const UsseVfLayout *vf) {
	int k = snprintf(o, n, "%c %08x %08x %d %08x %08x %d %d ", is_frag ? 'f' : 'v', e->hash, e->hash2, e->kind, e->cube_mask, e->snap_mask, e->points, e->fzero);
	if (!nsa) k += snprintf(o + k, n - k, "-");
	for (int i = 0; i < nsa && k < (int)n; i++) k += snprintf(o + k, n - k, "%s%x:%x", i ? "," : "", idx[i], unit[i]);
	if (!vf && k < (int)n) k += snprintf(o + k, n - k, " -");
	else if (k < (int)n) {
		k += snprintf(o + k, n - k, " %d,%d,%d,%d", vf->n, vf->npa, vf->index_reg, vf->instance_reg);
		for (int j = 0; j < vf->n && j < 32 && k < (int)n; j++) { const UsseVfAttr *a = &vf->a[j];
			k += snprintf(o + k, n - k, "/%u,%u,%u,%u,%u,%u,%u,%u,%u", a->stream, a->offset, a->stride, a->reg, a->fmt, a->comps, a->kind, a->dcomps, a->rawlen); }
	}
	return k < (int)n ? hash_bytes(o, (size_t)k) : 0;
}
static void pipe_state(const GfxDraw *d, int has_depth, int ckind, int depth_ok, int stencil_ok, int cull_sdl, int null_fs, int32_t v[NPV]) {   /* what get_pipeline builds from */
	const int32_t t[NPV] = { d->cull, cull_sdl, has_depth, ckind, depth_ok, stencil_ok, null_fs, d->depth_write, (int32_t)d->depth_func,
		d->stencil_func, d->stencil_fail, d->depth_fail, d->depth_pass, d->stencil_cmask, d->stencil_wmask, d->fp_disabled,
		d->blend.present, d->blend.mask, d->blend.color_func, d->blend.alpha_func, d->blend.color_src, d->blend.color_dst, d->blend.alpha_src, d->blend.alpha_dst,
		d->depth_bias_factor, d->depth_bias_units };
	memcpy(v, t, sizeof t);
}
static void rc_write(const char *line, int n) {   /* one record, in one write (another game process may append too) */
	if (write(rc.fd, line, (size_t)n) != n) { fprintf(stderr, "    gpu: shader recipes: write failed (%s): recording stops\n", strerror(errno)); close(rc.fd); rc.fd = -1; return; }
	rc.recorded++;
}
static void recipes_load(void);   /* (below) */
/* get_shader made e (from these inputs): its recipe, recorded unless it's known */
static void recipe_note_shader(ShaderEntry *e, int is_frag, const uint32_t sa[256], const UsseVfLayout *vf) {
	if (!recipes_on()) return;
	recipes_load();
	uint32_t read[8]; usse_spirv_sa_read(read);   /* (the translation shader_new just did) */
	uint8_t idx[32], unit[32]; int nsa = 0;
	for (int i = 0; i < 256; i++) if (read[i >> 5] >> (i & 31) & 1) {
		if (nsa == 32) { rc.too_many_sa++; return; }   /* (never seen: a recipe would leave units out) */
		idx[nsa] = (uint8_t)i; unit[nsa++] = (uint8_t)sa[i];
	}
	char text[4096]; const uint64_t id = rc_shader_text(text, sizeof text, e, is_frag, idx, unit, nsa, vf);
	if (!id) return;
	e->rid = id;
	if (rc.fd >= 0 && rc_set_add(id)) {
		char line[4200]; const int n = snprintf(line, sizeof line, "S %016llx %s\n", (unsigned long long)id, text);
		rc_write(line, n);
		if (recipes_verify()) fprintf(stderr, "    gpu: recipes: new %s shader %#x hash %08x/%08x kind %d layout %08x\n", is_frag ? "fragment" : "vertex", e->addr, e->hash, e->hash2, e->kind, e->layout_hash);
	}
}
static void recipe_note_pipe(const GfxDraw *d, const ShaderEntry *vs, const ShaderEntry *fs, int has_depth, int ckind, int depth_ok, int stencil_ok, int cull_sdl, int null_fs) {
	if (!recipes_on() || rc.fd < 0 || !vs->rid || !fs->rid) return;   /* (a shader without a recipe) */
	int32_t v[NPV]; pipe_state(d, has_depth, ckind, depth_ok, stencil_ok, cull_sdl, null_fs, v);
	char line[512]; int n = snprintf(line, sizeof line, "P %016llx %016llx", (unsigned long long)vs->rid, (unsigned long long)fs->rid);
	for (int i = 0; i < NPV; i++) n += snprintf(line + n, sizeof line - n, " %d", v[i]);
	if (rc_set_add(hash_bytes(line, (size_t)n) ^ 0x5049ull << 48)) {
		n += snprintf(line + n, sizeof line - n, "\n"); rc_write(line, n);
		if (recipes_verify()) fprintf(stderr, "    gpu: recipes: new pipeline %08x/%08x kind %d\n", vs->hash, fs->hash, ckind);
	}
}

static uint32_t shader_bucket(uint32_t hash, uint32_t hash2, uint32_t layout_hash) { return (hash ^ hash2 * 0x9E3779B1u ^ layout_hash * 0x85EBCA6Bu) % SHADER_HASH; }
static ShaderEntry *shader_find(uint32_t hash, uint32_t hash2, int kind, uint32_t cube_mask, uint32_t snap_mask, uint32_t layout_hash, int pcf, int points, int fzero) {   /* pcf -1: the shadow filter setting's */
	for (int i = shader_head[shader_bucket(hash, hash2, layout_hash)]; i; i = shaders[i - 1].hnext) { ShaderEntry *x = &shaders[i - 1];
		if (x->hash == hash && x->hash2 == hash2 && x->kind == kind && x->cube_mask == cube_mask && x->snap_mask == snap_mask && x->layout_hash == layout_hash && x->points == points && x->fzero == fzero
		    && x->pcf == (pcf >= 0 ? pcf : x->pcf_unit >= 0 && shadow_filter_want() ? 16 : 0)) return x; }
	return 0;
}
/* prog translated into e (its key fields set): the job that builds it (shader_enqueue), 0 if the translation failed */
static int shader_translate(ShaderEntry *e, const UsseProgram *prog, const UsseVertexLayout *vl, const UsseFragLayout *fl, const uint32_t sa_tex_unit[256], const UsseVfLayout *vf, ShaderJob *sj) {
	int smp_units[16], nsmp = 0;
	fetch_mode(); canon_vary_on();   /* (sets the translator's FRAGCOLOR mode and varying layout before its first module) */
	uint64_t tb0 = us_now();
	usse_spirv_set_pcf(e->pcf ? e->pcf_unit : -1, e->pcf); usse_spirv_set_points(e->points); usse_spirv_set_fetch_zero(e->fzero >= USSE_FB_LERP_KEEP ? 2 : e->fzero ? 1 : 0);
	{ static long pf = -1; if (pf < 0) { const char *v = getenv("RECOMP_PROBE_FPROG"); pf = v ? (long)strtoul(v, 0, 16) : 0; } usse_spirv_set_probe_off(pf && (uint32_t)pf != e->addr); }   /* RECOMP_PROBE_FPROG=<hex addr>: probe that fragment program only, so the textures and framebuffer it reads stay real */
	SpirvModule m = usse_spirv_translate(prog, vl, fl, sa_tex_unit, 16, smp_units, &nsmp, e->kind < 0 ? 0 : e->kind, e->cube_mask, e->snap_mask, vf);
	usse_spirv_set_pcf(-1, 0); usse_spirv_set_points(0); usse_spirv_set_probe_off(0); usse_spirv_set_fetch_zero(0);
	uint64_t tb1 = us_now(); t_shader_xlate += tb1 - tb0;
	if (!m.ok) { fprintf(stderr, "    gpu: %#x: %s\n", e->addr, m.err); usse_spirv_free(&m); return 0; }
	int is_frag = usse_is_fragment(prog);
	e->fetch = m.frag_fetch; e->nldst = m.nldst_sa; e->pa_words = m.pa_words; memcpy(e->ldst_sa, m.ldst_sa, sizeof e->ldst_sa);
	e->nsmp = nsmp; memcpy(e->smp_units, smp_units, sizeof smp_units);
	e->spv_hash = hash_bytes(m.words, 4 * (size_t)m.nwords);
	*sj = (ShaderJob){ e, m, is_frag, nsmp + (m.frag_fetch == 1) + (is_frag ? m.nldst_sa : 0), is_frag ? 0 : (m.nldst_sa || vs_layout_uniform() ? 2 : 1), e->addr, tb0, tb1 - tb0, 0 };   /* the framebuffer copy is one more sampler, and a fragment program's LDST targets (usse_spirv.c's emit_ldst_target); a framebuffer-fetch input is none */
	{ const uint64_t w[4] = { e->spv_hash, (uint64_t)sj->is_frag, (uint64_t)sj->nsmp, (uint64_t)sj->nsbuf }; e->wkey = hash_bytes(w, sizeof w); }   /* (its key in the shader recipes' warm.txt: the MSL cache's -- the module and its bindings) */
	return 1;
}
static void shader_enqueue(const ShaderJob *sj, int low) {   /* (the worker owns the module now) */
	BuildJob bj = { .low = low, .sj = *sj };
	sj->e->pending = 1;
	if (!gpu_async() || !build_enqueue(&bj)) shader_build(&bj.sj);
}
/* a new entry and its build (queued with gpu_async); low: a shader recipe's prebuild (bq_head[2]) */
static ShaderEntry *shader_new(uint32_t addr, const UsseProgram *prog, uint32_t hash, uint32_t hash2, const UsseVertexLayout *vl, const UsseFragLayout *fl, const uint32_t sa_tex_unit[256], int kind, uint32_t cube_mask, uint32_t snap_mask,
                               const UsseVfLayout *vf, uint32_t layout_hash, int pcf_unit, int pcf, int points, int fzero, int low) {
	if (nshaders >= MAXSHADER) { static int said; if (!said++) fprintf(stderr, "    gpu: the shader table is full (%d): new programs go to the software path\n", MAXSHADER); return 0; }
	if (!low) n_shader_builds++;
	if (trace_sync()) fprintf(stderr, "gpu trace: shader built for %#x hash %08x/%08x kind %d (%d so far)%s\n", addr, hash, hash2, kind, nshaders + 1, low ? " (recipe)" : "");
	const uint32_t bucket = shader_bucket(hash, hash2, layout_hash);
	ShaderEntry *e = &shaders[nshaders++]; memset(e, 0, sizeof *e); e->hnext = shader_head[bucket]; shader_head[bucket] = nshaders; e->addr = addr; e->hash = hash; e->hash2 = hash2; e->kind = kind; e->cube_mask = cube_mask; e->snap_mask = snap_mask; e->layout_hash = layout_hash;
	e->pcf_unit = pcf_unit; e->pcf = pcf; e->points = points; e->fzero = fzero; e->low = e->prebuilt = low;
	ShaderJob sj;
	if (shader_translate(e, prog, vl, fl, sa_tex_unit, vf, &sj)) shader_enqueue(&sj, low);
	return e;
}
/* a draw's first use of a shader a recipe prebuilt (RECOMP_SHADER_RECIPES_VERIFY: the module its own inputs make, compared) */
static void recipe_used_shader(ShaderEntry *x, uint32_t addr, const UsseProgram *prog, const UsseVertexLayout *vl, const UsseFragLayout *fl, const uint32_t sa[256], const UsseVfLayout *vf) {
	x->prebuilt = 0; rc.used_sh++;
	if (!recipes_verify()) return;
	int smp_units[16], nsmp = 0;
	usse_spirv_set_pcf(x->pcf ? x->pcf_unit : -1, x->pcf); usse_spirv_set_points(x->points); usse_spirv_set_fetch_zero(x->fzero >= USSE_FB_LERP_KEEP ? 2 : x->fzero ? 1 : 0);
	SpirvModule m = usse_spirv_translate(prog, vl, fl, sa, 16, smp_units, &nsmp, x->kind < 0 ? 0 : x->kind, x->cube_mask, x->snap_mask, vf);
	usse_spirv_set_pcf(-1, 0); usse_spirv_set_points(0); usse_spirv_set_fetch_zero(0);
	const uint64_t h = m.ok ? hash_bytes(m.words, 4 * (size_t)m.nwords) : 0; usse_spirv_free(&m);
	rc.verified++; if (h != x->spv_hash) rc.differ++;
	fprintf(stderr, "    gpu: recipes: draw uses prebuilt %s shader %#x (%08x): %s module (%d of %d differ)\n", x->kind < 0 ? "vertex" : "fragment", addr, x->hash, h == x->spv_hash ? "the same" : "A DIFFERENT", rc.differ, rc.verified);
}

static ShaderEntry *get_shader(uint32_t addr, const UsseProgram *prog, const UsseProgram *vprog, const UsseVertexLayout *vl, const UsseFragLayout *fl, const uint32_t sa_tex_unit[256], int kind, uint32_t cube_mask, uint32_t snap_mask,
                               const UsseVfLayout *vf, uint32_t layout_hash) {   /* vf/layout_hash: a vertex program's GPU-side fetch plan (vf_plan), null/0 for the CPU packer's records (and fragment programs) */
	uint32_t hash = usse_program_hash(prog), hash2 = vprog && !canon_vary_on() ? usse_program_hash(vprog) : 0;
	usse_spirv_key(prog, fl, sa_tex_unit, &kind, &cube_mask, &snap_mask);   /* (only what changes the module: one shader for the variants that don't) */
	int pcf_unit = kind >= 0 ? shadow_unit(addr, hash) : -1, pcf = pcf_unit >= 0 && shadow_filter_want() ? 16 : 0;   /* (fragment programs: kind is the target's) */
	ShaderEntry *x = shader_find(hash, hash2, kind, cube_mask, snap_mask, layout_hash, pcf, g_points_draw, g_fzero_draw);
	if (x) {
		if (x->prebuilt) recipe_used_shader(x, addr, prog, vl, fl, sa_tex_unit, vf);
		if (x->low) { x->low = 0; if (shader_pending(x)) bq_promote(x, 0); }
		return x;
	}
	ShaderEntry *e = shader_new(addr, prog, hash, hash2, vl, fl, sa_tex_unit, kind, cube_mask, snap_mask, vf, layout_hash, pcf_unit, pcf, g_points_draw, g_fzero_draw, 0);
	if (e && (shader_pending(e) || e->ok)) recipe_note_shader(e, usse_is_fragment(prog), sa_tex_unit, vf);
	if (e && kind >= 0 && rc.warm_phase == 5 && rc.p_frag && e->wkey && !*rc_slot(e->wkey ^ WTAG_S)) {   /* a fragment shader neither predicted nor built before, after the predicted pass: say how it differs */
		static uint32_t told[256]; static int ntold; int seen = 0;
		for (int i = 0; i < ntold; i++) seen |= told[i] == hash;
		rc.p_missed++;
		if (!seen) {
			if (ntold < 256) told[ntold++] = hash;
			UsseDraw d; memset(&d, 0, sizeof d); usse_setup_registers(prog, &d, 0, 0);
			uint32_t sa[256]; for (int i = 0; i < 256; i++) sa[i] = d.sa_unit[i] ? d.sa_unit[i] - 1u : d.sa[i].u;
			const uint32_t pcube = usse_spirv_predict_cube(prog, fl, sa);
			uint32_t rd[8]; usse_spirv_sa_read(rd);   /* (the entries shader_new's translation read: its samplers) */
			int sa_differ = 0; for (int i = 0; i < 256; i++) sa_differ += (rd[i >> 5] >> (i & 31) & 1) && (sa[i] & 0xFF) != (sa_tex_unit[i] & 0xFF);
			fprintf(stderr, "    gpu: predicted shaders: fragment %#x (%08x) drawn in a variant not predicted -- cube %#x (predicted %#x), snap %#x, points %d, fzero %d, %d of its samplers not where its own tables put them; built now and recorded for next time\n",
			        addr, hash, cube_mask, pcube, snap_mask, g_points_draw, g_fzero_draw, sa_differ);
		}
	}
	return e;
}

/* ---------------------------------------------------------------- textures */
typedef struct { uint32_t addr, format, width, height; int layout; int stale, bc; uint64_t used, checked_frame; SDL_GPUTexture *tex; SDL_GPUSampler *samp[4][2][3][3];
	const void *view_src; uint64_t view_gen; int view_depth; SDL_GPUTextureFormat vfmt; int vfilt, vlinear;   /* a view (see target_view): built from this render target / depth entry at that generation; vfmt: its texture's format (RECOMP_GPU_VIEW_FAST's view_format), vfilt: linear-filtered; vlinear: of an sRGB target -- samples give decoded values, as the target itself does */
	/* change detection (see get_texture): the guest bytes the decode reads, [addr, addr+span); a
	 * content hash per host page of them; the guest write clock when the pages were last checked
	 * (guestwatch.h); hot: rewritten so much every frame that it is hashed once a frame instead of
	 * watched (streak/calm count the frames towards and back out of that); full: the next check
	 * re-hashes every page (and closes them again); nchk: re-checks in frame chk_frame */
	uint32_t span, wclock, nph; uint64_t *ph; int hot, streak, calm, full, nchk; uint64_t big_frame, chk_frame;
	uint64_t chash; int levels; uint64_t bytes;
	uint8_t sx, sy; } TexEntry;   /* sx, sy: a view's texels a guest pixel (its source's scale: target_scale) */   /* chash/levels: the uploaded bytes' hash over the whole span and its mip count (get_texture: a texture that moved) */   /* samp[min][mag][addrmode_u][addrmode_v]; layout: GfxTexture's (3 = cube: a 6-layer cube texture) */
/* The texture table: up to MAXTEX entries and tex_budget() bytes of GPU textures (an estimate per entry, TexEntry.bytes), the
 * least recently used one replaced past either. It was a fixed 512 entries: a Killzone area that samples more than that a
 * frame thrashed it -- least-recently-used order evicts exactly what the next frame needs first, so the same 277 textures
 * were decoded and uploaded as new again every frame (250-435 a frame, 47-84 ms; 20-44 fps for 40 s, 2026-10-20). A
 * session's textures came to ~850 MB at most if all were kept. tex_index: the slot last found for an (address, format,
 * size) hash, checked against the entry on use -- a bind no longer scans the table. */
#define MAXTEX 4096
static TexEntry textures[MAXTEX]; static int ntextures; static uint64_t tex_clock, tex_bytes;
#define TEX_INDEX 16384
static uint16_t tex_index[TEX_INDEX];   /* slot + 1, 0 none */
static unsigned tex_key(const GfxTexture *t) {
	uint32_t h = t->data * 0x9E3779B1u ^ t->format * 0x85EBCA77u ^ (t->width << 16 | t->height) * 0xC2B2AE3Du;
	return (h ^ h >> 15) & (TEX_INDEX - 1);
}
static uint64_t tex_budget(void) {   /* RECOMP_GPU_TEX_BUDGET_MB, default 1024 */
	static uint64_t b; if (!b) { const char *e = getenv("RECOMP_GPU_TEX_BUDGET_MB"); b = (uint64_t)(e && atoi(e) > 0 ? atoi(e) : 1024) << 20; }
	return b;
}
/* a content hash over eight independent 64-bit lanes, 64 bytes an iteration: 23 GB/s on this Mac
 * against 3.5 for the single-lane hash it replaces (each lane's multiply chain is the bound, so
 * more lanes run in parallel). Any difference in one word changes its lane's state for good (every
 * step after it is a bijection of the state); lanes fold together at the end. */
#define HASH_STEP(h, w) do { h = (h ^ (w)) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; } while (0)
static uint64_t hash_bytes(const void *p, size_t n) {
	const uint8_t *b = p; size_t i = 0;
	uint64_t h0 = 0x243F6A8885A308D3ull ^ n, h1 = 0x13198A2E03707344ull, h2 = 0xA4093822299F31D0ull, h3 = 0x082EFA98EC4E6C89ull,
	         h4 = 0x452821E638D01377ull, h5 = 0xBE5466CF34E90C6Cull, h6 = 0xC0AC29B7C97C50DDull, h7 = 0x3F84D5B5B5470917ull;
	for (; i + 64 <= n; i += 64) {
		uint64_t w[8]; memcpy(w, b + i, 64);
		HASH_STEP(h0, w[0]); HASH_STEP(h1, w[1]); HASH_STEP(h2, w[2]); HASH_STEP(h3, w[3]);
		HASH_STEP(h4, w[4]); HASH_STEP(h5, w[5]); HASH_STEP(h6, w[6]); HASH_STEP(h7, w[7]);
	}
	for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); HASH_STEP(h0, w); }
	for (; i < n; i++) HASH_STEP(h1, b[i]);
	uint64_t h = h0;
	HASH_STEP(h, h1); HASH_STEP(h, h2); HASH_STEP(h, h3); HASH_STEP(h, h4); HASH_STEP(h, h5); HASH_STEP(h, h6); HASH_STEP(h, h7);
	return h;
}
static uint64_t gpu_frame_no;   /* counts presents: an unwatched texture's content is checked once per frame (see get_texture) */
/* GXM's SceGxmTextureAddrMode: 0 repeat, 1 mirror, 2 clamp match SDL's REPEAT/MIRROR/CLAMP 1:1; the
 * rest (3 mirror-once, 4 repeat-ignore-border, 5-7 clamp-full/half-border) have no direct SDL
 * equivalent, so fold to the nearest: 3,5,6,7 -> clamp, 4 -> repeat. */
static SDL_GPUSamplerAddressMode gxm_addr_mode(int m) {
	switch (m & 15) {
	case 0: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
	case 1: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
	case 2: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	case 4: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
	default: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;   /* 3, 5, 6, 7 */
	}
}
/* anisotropic filtering (settings "anisotropy" 1 off / 2 / 4 / 8 / 16, default 16; RECOMP_ANISO wins): the
 * Vita samples with little or none, so surfaces at a glancing angle -- floors, walls, roads -- blur into
 * their small mip levels. (Measured 2026-10-10 on two Uncharted replays: no pixel changed, nor with every
 * sampler clamped to mip 0 -- its textures are so low-resolution that nothing on screen minifies at
 * 960x544 and up; it is here for games and scenes that do.)
 * Every linearly minified texture gets it; point-sampled ones (UI, lookup tables) stay exact. A change
 * of setting rebuilds the samplers between scenes (gfx_gpu_begin_scene). gfxreplay: off unless asked,
 * so replays compare against the software path exactly. */
static int aniso_cur = -1;
static int aniso_want(void) {
	extern int settings_int(const char *, int);
	const char *e = getenv("RECOMP_ANISO"); int n = e ? atoi(e) : settings_int("anisotropy", 16);
	return n >= 16 ? 16 : n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1;
}
static SDL_GPUSampler *tex_sampler(TexEntry *e, int minf, int magf, int addru, int addrv, int miplin) {
	int trilinear = (minf & 3) == 2 || miplin;   /* SCE_GXM_TEXTURE_FILTER_MIPMAP_LINEAR or the mip filter (gfx.h GFX_TEX_MIP_LINEAR): blend the two nearest levels */
	int mi = minf ? (trilinear ? 2 : 1) : trilinear ? 3 : 0;   /* the cache key keeps the mip mode too (it didn't: a texture sampled first with nearest mips kept that sampler for its trilinear draws) */
	minf = minf ? 1 : 0; magf = magf ? 1 : 0;
	SDL_GPUSamplerAddressMode mu = gxm_addr_mode(addru), mv = gxm_addr_mode(addrv);
	int iu = (int)mu, iv = (int)mv;
	if (e->samp[mi][magf][iu][iv]) return e->samp[mi][magf][iu][iv];
	SDL_GPUSamplerCreateInfo ci = {0};
	ci.min_filter = minf ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
	ci.mag_filter = magf ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
	ci.mipmap_mode = trilinear ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
	ci.address_mode_u = mu; ci.address_mode_v = mv; ci.address_mode_w = mu;
	ci.max_lod = 1000.0f;
	if (minf && aniso_cur > 1) {   /* with trilinear mips: Apple GPUs filter anisotropically only between levels (with nearest mips, 16x changed no pixel of a replay), and blending levels removes the mip bands the Vita's nearest-mip sampling shows on floors */
		ci.enable_anisotropy = true; ci.max_anisotropy = (float)aniso_cur; ci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
	}
	return e->samp[mi][magf][iu][iv] = SDL_CreateGPUSampler(gpu(), &ci);
}
static void views_release_samplers(void);   /* below, with the views */
static void tex_release_samplers(TexEntry *e) {
	for (int a = 0; a < 4; a++) for (int b = 0; b < 2; b++) for (int c = 0; c < 3; c++) for (int d = 0; d < 3; d++) if (e->samp[a][b][c][d]) { SDL_ReleaseGPUSampler(gpu(), e->samp[a][b][c][d]); e->samp[a][b][c][d] = 0; }
}
/* (address, format, width, height) cache: re-decodes/re-uploads when the guest overwrites a
 * texture's bytes in place (see get_texture's change detection) or its layout changes */
/* a texture decoded from memory a GPU target has since been downloaded over: decode it again */
static void textures_invalidate(uint32_t lo, uint32_t hi) {
	for (int i = 0; i < ntextures; i++) if (textures[i].addr >= lo && textures[i].addr < hi) { textures[i].stale = 1; textures[i].full = 1; textures[i].checked_frame = 0; }
}
/* a sampler unit the fragment program reads but the draw left unbound: sample transparent black
 * (SDL_GPU still requires every declared sampler to be bound to something) instead of the scene's
 * own colour target -- binding the render target currently being drawn into as one of its own
 * inputs is a read-after-write hazard within the same render pass, undefined on real hardware and
 * every modern API alike, not just "the wrong pixels". */
static SDL_GPUTexture *dummy_texture(void) {
	static SDL_GPUTexture *tex; static int tried;
	if (tried) return tex;
	tried = 1;
	SDL_GPUTextureCreateInfo tci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
		.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = 1, .height = 1, .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
	tex = SDL_CreateGPUTexture(gpu(), &tci);
	if (!tex) return 0;
	uint8_t px[4] = {0, 0, 0, 0};
	SDL_GPUTransferBufferCreateInfo xci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = 4 };
	SDL_GPUTransferBuffer *xb = SDL_CreateGPUTransferBuffer(gpu(), &xci);
	void *map = SDL_MapGPUTransferBuffer(gpu(), xb, true); memcpy(map, px, 4); SDL_UnmapGPUTransferBuffer(gpu(), xb);
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu()); SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureTransferInfo src = { .transfer_buffer = xb, .pixels_per_row = 1, .rows_per_layer = 1 };
	SDL_GPUTextureRegion dst = { .texture = tex, .w = 1, .h = 1, .d = 1 };
	SDL_UploadToGPUTexture(cp, &src, &dst, true);
	SDL_EndGPUCopyPass(cp); SDL_SubmitGPUCommandBuffer(cmd);
	SDL_ReleaseGPUTransferBuffer(gpu(), xb);
	return tex;
}
/* (re)create e->tex for this texture and upload its contents: a 2D texture with every mip level
 * it carries (gfx_soft.c's chain addressing), so the sampler can minify like the hardware
 * instead of aliasing level 0; a cube map (layout 3) as a 6-layer 2D array, one level, its faces
 * in GXM's +X -X +Y -Y +Z -Z order (the shader selects the face: usse_spirv.c's emit_smp) */
/* texture formats whose texels don't survive an 8-bit UNORM upload: signed, 16/32-bit integer,
 * float and depth families (SceGxmTextureBaseFormat S8 .. F32F32, the 16-bit x4 families) -- a
 * deferred renderer's G-buffers (world positions, normals, depth) are these. They upload as
 * RGBA32_FLOAT instead, exactly as the software sampler decodes them. Also the high-numbered
 * ones of the same kind: SBC4 / SBC5 (0x89 / 0x8b), S8S8S8 (0x99) and U2F10F10F10 (0x9a) --
 * Killzone: Mercenary's normalisation cube map is S8S8S8, and as UNORM every direction it
 * returned lost its sign: its lit floors and walls came out a sixth as bright as on the
 * interpreter, the reflections tinted blue */
static int tex_wide(uint32_t format) {
	uint32_t b = format >> 24;
	return b == 0x01 || b == 0x06 || (b >= 0x08 && b <= 0x1f && b != 0x0c) || b == 0x89 || b == 0x8b || b == 0x99 || b == 0x9a;
}
/* UBC1/2/3 (DXT1/3/5) go to the GPU as BC1/2/3 textures, their blocks copied straight from guest
 * memory (gfx_texture_blocks: the software decoder's own block addressing), instead of being
 * decoded here a texel() call per pixel -- that decode was the hottest host function of Unit 13's
 * boot and 725 ms of the first frame of its level (frame_757: ~210 BC textures, mostly 256-512
 * square, swizzled, with mips). Only swizzle 0 (ABGR, the natural order): an SDL GPU texture has
 * no component swizzle, so the others decode as before. UBC4 likewise goes up as BC4 only as
 * SWIZZLE1_R, which is exactly how a BC4_R texture samples, (r, 0, 0, 1); Killzone: Mercenary's
 * UBC4 UI textures are R111 (white, the channel as alpha) and take the decode path.
 * NOT bit-exact with the decode path, by the hardware's arithmetic: Apple's BC decoder (measured
 * on random blocks, every texel matched) expands the endpoints by bit replication as the
 * software does, but interpolates in 6-bit fixed point -- colour (43*e0 + 21*e1 + 32) >> 6, the
 * 3-colour average (e0 + e1 + 1) >> 1, BC3 alpha with weights like 55/64, 46/64 -- where
 * gfx_soft.c's dxt_rgb divides in float and rounds; a texel differs by up to 2/255 (about a third
 * of interpolated texels by 1, ~1.5% by 2). Downstream that is a 1-unit flip of a packed G-buffer
 * byte or of a 10-bit HDR float (4/255 in the 8-bit view) and, on Unit 13's menu, up to 4/255 on
 * 0.05% of the frame. RECOMP_GPU_NO_BC=1 is the exact path. Other GPUs' decoders differ again.
 * Returns the SDL format, or INVALID when this texture takes the decode path. */
static SDL_GPUTextureFormat tex_bc_format(const GfxTexture *t, int cube) {
	static int off = -1; if (off < 0) off = getenv("RECOMP_GPU_NO_BC") != 0;   /* RECOMP_GPU_NO_BC=1: decode every BC texture on the CPU, as before (an A/B check) */
	static int supported[4][2];   /* [format][cube]: 0 unknown, 1 yes, 2 no -- SDL_GPUTextureSupportsFormat once each */
	uint32_t base = t->format >> 24;
	if (off || base < 0x85 || base > 0x88 || ((t->format >> 12) & 7)) return SDL_GPU_TEXTUREFORMAT_INVALID;
	static const SDL_GPUTextureFormat f[4] = { SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM, SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM, SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM, SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM };
	int k = (int)base - 0x85;
	if (!supported[k][cube]) {
		supported[k][cube] = SDL_GPUTextureSupportsFormat(gpu(), f[k], cube ? SDL_GPU_TEXTURETYPE_2D_ARRAY : SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER) ? 1 : 2;
		if (supported[k][cube] == 2) fprintf(stderr, "    gpu: BC%d textures%s not supported by this device: decoded on the CPU\n", k + 1, cube ? " (array)" : "");
	}
	return supported[k][cube] == 1 ? f[k] : SDL_GPU_TEXTUREFORMAT_INVALID;
}
static uint32_t n_tex_uploads, n_tex_bc;   /* this frame's texture uploads, and how many of them went up as BC blocks */
/* the native BC upload: every level (or cube face) into ONE transfer buffer at its own offset,
 * mapped once (block-aligned offsets, as Metal's buffer-to-texture copy requires of compressed
 * formats). The region is the level's size in texels (max(1, w>>l)): a level smaller than a block
 * (the 2x2 and 1x1 mips) still carries a whole block, and Metal takes a region that reaches the
 * texture's edge without rounding it to the block size; pixels_per_row/rows_per_layer (Vulkan's
 * bufferRowLength/ImageHeight, in texels and a multiple of the block size) are the padded block
 * grid -- Metal derives the row pitch from the region's width itself. */
static uint8_t *tring_begin(uint32_t bytes, uint32_t *off); static void tring_end(SDL_GPUCommandBuffer *cmd, uint32_t lo, uint32_t hi); static SDL_GPUTransferBuffer *tring_map_buffer(void);   /* (the staging ring, below) */
static int upload_texture_bc(TexEntry *e, const GfxTexture *t, int cube, int levels, int layers) {
	size_t total = 0; uint32_t bsz = (t->format >> 24) == 0x85 || (t->format >> 24) == 0x88 ? 8 : 16;   /* UBC1/UBC4: 8-byte blocks */
	GfxTexture lv[16 * 6]; int n = 0;
	size_t off[16 * 6];
	for (int l = 0; l < levels && n < 16 * 6; l++) for (int f = 0; f < layers && n < 16 * 6; f++, n++) {
		if (cube) gfx_texture_face_level(t, f, l, &lv[n]); else gfx_texture_level(t, l, &lv[n]);
		off[n] = total; total += ((size_t)bsz * ((lv[n].width + 3u) / 4) * ((lv[n].height + 3u) / 4) + 15) & ~(size_t)15;
	}
	/* through the staging ring as the decoded uploads go (a transfer buffer created and released per upload was Killzone's
	 * streamed BC textures' 0.8 ms a frame on Vulkan, 2026-10-25); one too big for it takes a buffer of its own */
	uint32_t base = 0; uint8_t *map = total <= UINT32_MAX ? tring_begin((uint32_t)total, &base) : 0;
	SDL_GPUTransferBuffer *xb = 0, *src_buf;
	if (map) src_buf = tring_map_buffer();
	else {
		SDL_GPUTransferBufferCreateInfo xci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = (Uint32)total };
		if (!(xb = SDL_CreateGPUTransferBuffer(gpu(), &xci))) return 0;
		if (!(map = SDL_MapGPUTransferBuffer(gpu(), xb, false))) { SDL_ReleaseGPUTransferBuffer(gpu(), xb); return 0; }
		src_buf = xb;
	}
	for (int i = 0; i < n; i++) gfx_texture_blocks(&lv[i], map + base + off[i]);
	if (xb) SDL_UnmapGPUTransferBuffer(gpu(), xb);
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu()); SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	for (int i = 0; i < n; i++) {
		int l = i / layers, f = i % layers;
		SDL_GPUTextureTransferInfo src = { .transfer_buffer = src_buf, .offset = (Uint32)(base + off[i]), .pixels_per_row = (lv[i].width + 3u) & ~3u, .rows_per_layer = (lv[i].height + 3u) & ~3u };
		SDL_GPUTextureRegion dst = { .texture = e->tex, .mip_level = (Uint32)l, .layer = (Uint32)f, .w = lv[i].width, .h = lv[i].height, .d = 1 };
		SDL_UploadToGPUTexture(cp, &src, &dst, false);
	}
	SDL_EndGPUCopyPass(cp);
	if (xb) { SDL_SubmitGPUCommandBuffer(cmd); SDL_ReleaseGPUTransferBuffer(gpu(), xb); }
	else tring_end(cmd, base, base + (uint32_t)total);
	{	/* RECOMP_GPU_DUMP_TEX for a BC texture: the GPU gets blocks, not texels -- write the software decode of level 0 (what those blocks hold, to the hardware decoder's rounding), RGBA8 as for any texture */
		static const char *dt = 0; static int dti = 0; if (!dti) { dti = 1; dt = getenv("RECOMP_GPU_DUMP_TEX"); }
		const char *path = dt ? strchr(dt, ':') : 0;
		if (path && strtoul(dt, 0, 16) == t->data) {
			uint8_t *rgba = malloc(4u * lv[0].width * lv[0].height); gfx_decode_texture_rgba8(&lv[0], rgba);
			FILE *fo = fopen(path + 1, "wb"); if (fo) { fwrite(rgba, 4, (size_t)lv[0].width * lv[0].height, fo); fclose(fo); }
			free(rgba); fprintf(stderr, "gpu: texture %#x went up as BC blocks: dumped their software decode\n", t->data);
		}
	}
	return 1;
}
/* ---- the texture staging ring: one upload transfer buffer the decoded uploads take turns in (TRING_BYTES), each upload's
 * command buffer fenced, and a region written again only once the upload that last used it has finished. A transfer
 * buffer created and released per upload and per mip level cost Killzone 1.4 ms a frame on Vulkan for its ~6 uploads
 * (0.3 on Metal; RECOMP_GPU_STATS's "gpu uploads", 2026-10-21). An upload bigger than a quarter of the ring takes buffers
 * of its own, as before. */
#define TRING_BYTES (64u << 20)
static SDL_GPUTransferBuffer *tring; static uint32_t tring_pos; static uint64_t tring_lap;
typedef struct { SDL_GPUFence *f; uint32_t lo, hi; uint64_t lap; } TringUse;
#define NTRING_USE 256
static TringUse tring_use[NTRING_USE]; static int ntring_use;
static SDL_GPUTransferBuffer *tring_map_buffer(void) { return tring; }
static uint8_t *tring_begin(uint32_t bytes, uint32_t *off) {   /* the ring mapped, *off where this upload's bytes go; 0: not through the ring */
	if (bytes > TRING_BYTES / 4) return 0;
	if (!tring) { SDL_GPUTransferBufferCreateInfo xci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = TRING_BYTES }; if (!(tring = SDL_CreateGPUTransferBuffer(gpu(), &xci))) return 0; }
	if (tring_pos + bytes > TRING_BYTES) { tring_pos = 0; tring_lap++; }
	const uint32_t lo = tring_pos, hi = lo + bytes;
	int k = 0;
	for (int i = 0; i < ntring_use; i++) {   /* an earlier lap's uploads over this region: finished first (and the finished ones dropped) */
		TringUse *u = &tring_use[i];
		if (u->lap < tring_lap && u->lo < hi && u->hi > lo) { uint64_t t0 = us_now(); SDL_WaitForGPUFences(gpu(), true, &u->f, 1); t_ring_wait += us_now() - t0; n_ring_waits++; SDL_ReleaseGPUFence(gpu(), u->f); }
		else if (SDL_QueryGPUFence(gpu(), u->f)) SDL_ReleaseGPUFence(gpu(), u->f);
		else tring_use[k++] = *u;
	}
	ntring_use = k;
	tring_pos = (hi + 255) & ~255u; *off = lo;
	return SDL_MapGPUTransferBuffer(gpu(), tring, false);
}
static void tring_end(SDL_GPUCommandBuffer *cmd, uint32_t lo, uint32_t hi) {   /* the ring unmapped and the upload's command buffer submitted, fenced */
	SDL_UnmapGPUTransferBuffer(gpu(), tring);
	SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
	if (!f) return;
	if (ntring_use == NTRING_USE) { SDL_WaitForGPUFences(gpu(), true, &tring_use[0].f, 1); SDL_ReleaseGPUFence(gpu(), tring_use[0].f); memmove(tring_use, tring_use + 1, sizeof tring_use[0] * (size_t)--ntring_use); }
	tring_use[ntring_use++] = (TringUse){ f, lo, hi, tring_lap };
}
static void upload_texture(TexEntry *e, const GfxTexture *t) {
	int cube = ((t->u_mode >> 4) & 3) == 3, levels = cube ? gfx_texture_cube_levels(t) : gfx_texture_levels(t), layers = cube ? 6 : 1;
	e->chash = hash_bytes(G(t->data), e->span); e->levels = levels;   /* what the GPU texture holds (~10 us for 256 KB) */
	int wide = tex_wide(t->format); uint32_t bpp = wide ? 16 : 4;
	batch_retire(e->tex, "texture re-upload");   /* its own command buffer runs before the pending draws that sampled the old content */
	e->layout = (t->u_mode >> 4) & 3;
	n_tex_uploads++;
	SDL_GPUTextureFormat bc = tex_bc_format(t, cube);
	if (bc != SDL_GPU_TEXTUREFORMAT_INVALID) {
		/* the cache re-uploads in place only for the same address/format/size, which makes the same
		 * decision -- unless creating the BC object failed the first time and the entry went RGBA */
		if (e->tex && !e->bc) goto decode;
		if (!e->tex) {
			SDL_GPUTextureCreateInfo tci = { .type = cube ? SDL_GPU_TEXTURETYPE_2D_ARRAY : SDL_GPU_TEXTURETYPE_2D, .format = bc,
				.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = t->width, .height = t->height, .layer_count_or_depth = (Uint32)layers, .num_levels = (Uint32)levels, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
			e->tex = SDL_CreateGPUTexture(gpu(), &tci);
			if (!e->tex) fprintf(stderr, "    gpu: BC texture %ux%u fmt %#x layout %d: SDL_CreateGPUTexture failed (%s): decoding it instead\n", t->width, t->height, t->format, e->layout, SDL_GetError());
			e->bc = e->tex != 0;
		}
		if (e->tex) {   /* (a failed upload -- no transfer memory -- leaves it unfilled, as the decode path's would) */
			if (upload_texture_bc(e, t, cube, levels, layers)) n_tex_bc++;
			else fprintf(stderr, "    gpu: BC texture %#x: no transfer buffer (%s)\n", t->data, SDL_GetError());
			return;
		}
	}
decode:
	if (!e->tex) {
		SDL_GPUTextureCreateInfo tci = { .type = cube ? SDL_GPU_TEXTURETYPE_2D_ARRAY : SDL_GPU_TEXTURETYPE_2D, .format = wide ? SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
			.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = t->width, .height = t->height, .layer_count_or_depth = (Uint32)layers, .num_levels = (Uint32)levels, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
		e->tex = SDL_CreateGPUTexture(gpu(), &tci);
		if (!e->tex) { fprintf(stderr, "    gpu: texture %ux%u fmt %#x layout %d: SDL_CreateGPUTexture failed: %s\n", t->width, t->height, t->format, e->layout, SDL_GetError()); return; }
	}
	{	/* through the staging ring: each level decoded straight into it */
		uint32_t total = 0;
		for (int l = 0; l < levels; l++) for (int f = 0; f < layers; f++) { GfxTexture lv; if (cube) gfx_texture_face_level(t, f, l, &lv); else gfx_texture_level(t, l, &lv); total += (bpp * lv.width * lv.height + 15) & ~15u; }
		uint32_t base; uint64_t tx0 = us_now();
		uint8_t *map = tring_begin(total, &base);
		t_up_xfer += us_now() - tx0;
		if (map) {
			uint64_t tu0 = us_now();
			SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu()); SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
			t_up_submit += us_now() - tu0;
			uint32_t off = base;
			for (int l = 0; l < levels; l++) for (int f = 0; f < layers; f++) {
				GfxTexture lv; if (cube) gfx_texture_face_level(t, f, l, &lv); else gfx_texture_level(t, l, &lv);
				uint64_t td0 = us_now();
				if (wide) gfx_decode_texture_f32(&lv, (float *)(map + off)); else gfx_decode_texture_rgba8(&lv, map + off);
				t_up_decode += us_now() - td0;
				{	static const char *dt = 0; static int dti = 0; if (!dti) { dti = 1; dt = getenv("RECOMP_GPU_DUMP_TEX"); }   /* (as the path below: level 0 as the GPU receives it) */
					const char *path = dt ? strchr(dt, ':') : 0;
					if (path && l == 0 && f == 0 && strtoul(dt, 0, 16) == t->data) { FILE *o = fopen(path + 1, "wb"); if (o) { fwrite(map + off, bpp, (size_t)lv.width * lv.height, o); fclose(o); } }
				}
				SDL_GPUTextureTransferInfo src = { .transfer_buffer = tring, .offset = off, .pixels_per_row = lv.width, .rows_per_layer = lv.height };
				SDL_GPUTextureRegion dst = { .texture = e->tex, .mip_level = (Uint32)l, .layer = (Uint32)f, .w = lv.width, .h = lv.height, .d = 1 };
				SDL_UploadToGPUTexture(cp, &src, &dst, false);
				off += (bpp * lv.width * lv.height + 15) & ~15u;
			}
			uint64_t ts0 = us_now();
			SDL_EndGPUCopyPass(cp); tring_end(cmd, base, base + total);
			t_up_submit += us_now() - ts0;
			return;
		}
	}
	uint8_t *rgba = malloc((size_t)bpp * t->width * t->height);
	/* one transfer buffer per upload: re-mapping a single buffer with `cycle` inside one copy pass
	 * left every upload after the first reading uninitialised memory (the cube faces came out as
	 * Metal's magenta fill; mip levels past 0 the same way) -- the buffers are released once the
	 * command buffer is submitted */
	SDL_GPUTransferBuffer *xbs[6 * 16]; int nxb = 0;
	uint64_t tu0 = us_now();
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu()); SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	t_up_submit += us_now() - tu0;
	for (int l = 0; l < levels; l++) for (int f = 0; f < layers; f++) {
		GfxTexture lv; if (cube) gfx_texture_face_level(t, f, l, &lv); else gfx_texture_level(t, l, &lv);
		uint64_t td0 = us_now();
		if (wide) gfx_decode_texture_f32(&lv, (float *)rgba); else gfx_decode_texture_rgba8(&lv, rgba);
		t_up_decode += us_now() - td0;
		{	/* RECOMP_GPU_DUMP_TEX=hexaddr:path: that texture's level 0 as the GPU receives it (RGBA floats when wide, RGBA8 otherwise) -- gfxreplay's RECOMP_REPLAY_TEX writes the software sampler's view of the same texture, for a byte-level comparison */
			static const char *dt = 0; static int dti = 0; if (!dti) { dti = 1; dt = getenv("RECOMP_GPU_DUMP_TEX"); }
			const char *path = dt ? strchr(dt, ':') : 0;
			if (path && l == 0 && f == 0 && strtoul(dt, 0, 16) == t->data) { FILE *o = fopen(path + 1, "wb"); if (o) { fwrite(rgba, bpp, (size_t)lv.width * lv.height, o); fclose(o); } }
		}
		uint64_t tx0 = us_now();
		SDL_GPUTransferBufferCreateInfo xci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = bpp * lv.width * lv.height };
		SDL_GPUTransferBuffer *xb = SDL_CreateGPUTransferBuffer(gpu(), &xci);
		if (!xb || nxb >= 6 * 16) break;
		xbs[nxb++] = xb;
		void *map = SDL_MapGPUTransferBuffer(gpu(), xb, false); memcpy(map, rgba, (size_t)bpp * lv.width * lv.height); SDL_UnmapGPUTransferBuffer(gpu(), xb);
		t_up_xfer += us_now() - tx0;
		SDL_GPUTextureTransferInfo src = { .transfer_buffer = xb, .pixels_per_row = lv.width, .rows_per_layer = lv.height };
		SDL_GPUTextureRegion dst = { .texture = e->tex, .mip_level = (Uint32)l, .layer = (Uint32)f, .w = lv.width, .h = lv.height, .d = 1 };
		SDL_UploadToGPUTexture(cp, &src, &dst, false);
	}
	uint64_t ts0 = us_now();
	SDL_EndGPUCopyPass(cp); SDL_SubmitGPUCommandBuffer(cmd);
	for (int i = 0; i < nxb; i++) SDL_ReleaseGPUTransferBuffer(gpu(), xbs[i]);
	t_up_submit += us_now() - ts0;
	free(rgba);
}
/* TexEntry.bytes, and the table's total: its GPU texture's size, about (BC counted at a byte a texel, the most it takes; mips
 * a third more) */
static void tex_account(TexEntry *e) {
	const uint64_t px = (uint64_t)e->width * e->height * (e->layout == 3 ? 6 : 1);
	const uint64_t b = e->tex ? (e->bc ? px : px * (tex_wide(e->format) ? 16 : 4)) * 4 / 3 : 0;
	tex_bytes += b - e->bytes; e->bytes = b;
}
/* ---- change detection. The CPU rewrites textures in place -- Unit 13 draws its menu text into a
 * glyph atlas between frames (a 4 KB-prefix hash missed that: no menu text) -- and a texture the
 * CPU changed must be re-uploaded before the first draw that samples it. Hashing every bound
 * texture whole once a frame did that, at 27 ms of host time on the heavy level frame (frame_757).
 * Now the host pages under each cached texture are read-only (guestwatch.c): the first write to
 * one faults and stamps it with the guest write clock, and a bind costs a load of that clock while
 * nothing is written; after a write only the textures with a page stamped since their last check
 * re-hash, and only those pages (a content hash per page: a write to other data sharing a
 * texture's first or last page re-hashes one page and uploads nothing). The check runs at every
 * bind, not once a frame, so a CPU write between two draws is seen by the second -- what the
 * software path, reading memory at each draw, sees. RECOMP_GPU_NO_TEX_WATCH=1: every texture
 * hashed once a frame as before (still per page, on the faster hash);
 * RECOMP_GPU_TEX_WATCH_VERIFY=1: every watched texture is also hashed whole once a frame and a
 * change the watch didn't flag is reported -- the check that no host write path was missed. */
static int tex_verify(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_GPU_TEX_WATCH_VERIFY") != 0; return v; }
static uint32_t n_tex_binds, n_tex_checked, n_tex_changed, n_tex_new, n_tex_moved, n_tex_missed, n_tex_deferred; static uint64_t n_pages_hashed;
/* the guest bytes a texture's decode reads: every mip level (gfx_soft.c's chain addressing: the
 * level after the last starts where the texture ends), the six faces of a cube, the rows of a
 * strided one, block-compressed ones by whole blocks. (The 4 MB-capped w*h*4 bytes hashed before
 * missed the mip tail of RGBA8 textures and ran 2-8x past the end of the 1- and 2-byte and
 * block-compressed formats into unrelated memory, re-uploading on writes that weren't theirs.) */
static uint32_t tex_span(const GfxTexture *t) {
	GfxTexture x; int layout = (t->u_mode >> 4) & 3;
	if (layout == 3) gfx_texture_face(t, 6, &x);
	else if (t->format & 0xfff) { int b = gfx_texture_bpp(t); return (uint32_t)(t->format & 0xfff) * t->height * (uint32_t)(b ? b : 4); }   /* strided linear: level 0 only, stride x height texels. (Measured as one cube face it came out a whole power-of-two mip chain -- Killzone's 240x136 quarter-res depth, stride 256, as 0x55800 bytes: it "overlapped" the colour target after it, downloaded at each of its 5 binds a frame) */
	else gfx_texture_level(t, gfx_texture_levels(t), &x);
	return x.data - t->data;
}
static uint32_t tex_pages(uint32_t addr, uint32_t span) { int sh = gwatch_page_shift(); return span ? (uint32_t)((((uint64_t)addr + span - 1) >> sh) - (addr >> sh) + 1) : 0; }
/* re-hash e's pages -- every one (`all`), or those stamped after `since` -- against the stored
 * hashes; returns how many changed. With `watched`, a changed page that was not stamped is counted
 * in *missed (RECOMP_GPU_TEX_WATCH_VERIFY: a host write that went round the watch). */
static uint32_t tex_rehash(TexEntry *e, int all, int watched, uint32_t since, uint32_t *missed) {
	int sh = gwatch_page_shift(); uint32_t p0 = e->addr >> sh, changed = 0;
	uint64_t end = (uint64_t)e->addr + e->span;
	for (uint32_t i = 0; i < e->nph; i++) {
		int flagged = watched && gwatch_page_clock(p0 + i) > since;
		if (!all && !flagged) continue;
		uint64_t lo = (uint64_t)(p0 + i) << sh, hi = lo + ((uint64_t)1 << sh);
		if (lo < e->addr) lo = e->addr;
		if (hi > end) hi = end;
		uint64_t h = hash_bytes(G((uint32_t)lo), (size_t)(hi - lo)); n_pages_hashed++;
		if (h != e->ph[i]) { e->ph[i] = h; changed++; if (watched && !flagged) (*missed)++; }
	}
	return changed;
}
/* watched or hot: a write fault costs ~6 us a page, hashing a 16 KB page ~0.7 us -- so a texture
 * with more than a quarter of its pages rewritten in each of 3 consecutive frames (a movie frame, a
 * CPU-animated texture) stops being watched and is hashed whole at its first bind of each frame
 * instead (staleness: none across frames -- the first draw of a frame sees whatever the CPU wrote
 * before it -- but a write between two draws of one frame is seen only next frame, as before
 * this cache had a watch), until 30 frames in a row find it unchanged */
static void tex_policy(TexEntry *e, uint32_t changed) {
	if (!e->hot) {
		if (!changed || changed * 4 <= e->nph || e->big_frame == gpu_frame_no + 1) return;
		e->streak = e->big_frame == gpu_frame_no ? e->streak + 1 : 1; e->big_frame = gpu_frame_no + 1;
		if (e->streak >= 3) { e->hot = 1; e->calm = 0; if (trace_sync()) fprintf(stderr, "gpu trace: texture %#x %ux%u rewritten every frame: hashed each frame, not watched\n", e->addr, e->width, e->height); }
	} else {
		e->calm = changed ? 0 : e->calm + 1;
		if (e->calm >= 30) { e->hot = 0; e->streak = 0; e->full = 1; if (trace_sync()) fprintf(stderr, "gpu trace: texture %#x %ux%u stable again: watched\n", e->addr, e->width, e->height); }
	}
}
static TexEntry *get_texture(const GfxTexture *t) {
	uint64_t th0 = us_now();
	int layout = (t->u_mode >> 4) & 3, watch = gwatch_on();
	n_tex_binds++;
	TexEntry *e = 0;   /* one entry per (address, format, size): a layout change re-decodes it in place */
	const unsigned key = tex_key(t);
	#define TEX_MATCH(x) ((x)->addr == t->data && (x)->format == t->format && (x)->width == t->width && (x)->height == t->height)
	{ int hint = tex_index[key] - 1; if (hint >= 0 && hint < ntextures && TEX_MATCH(&textures[hint])) e = &textures[hint]; }
	if (!e) for (int i = 0; i < ntextures; i++) { TexEntry *x = &textures[i]; if (TEX_MATCH(x)) { e = x; tex_index[key] = (uint16_t)(i + 1); break; } }
	#undef TEX_MATCH
	if (e && e->ph && e->layout == layout && !e->full) {   /* the common case: nothing written over it since its last check */
		int fresh = watch && !e->hot ? gwatch_unchanged(e->addr, e->span, &e->wclock) && (!tex_verify() || e->checked_frame == gpu_frame_no + 1)
		                             : e->checked_frame == gpu_frame_no + 1;   /* hot, or no watch: once a frame */
		/* a texture whose pages keep being written within a frame (a page it shares with data the game
		 * rewrites between draws) re-checks at most 4 times a frame -- a fault and a page hash each;
		 * its later binds that frame use what was uploaded, and the stamps left open bring the first
		 * bind of the next frame back here (the once-a-frame guarantee) */
		if (!fresh && watch && !e->hot && !tex_verify() && e->chk_frame == gpu_frame_no + 1 && e->nchk >= 4) { fresh = 1; n_tex_deferred++; }
		if (fresh) { e->used = ++tex_clock; t_texhash += us_now() - th0; return e; }
	}
	uint32_t span = tex_span(t);
	if (e && e->ph && e->layout == layout && e->span == span) {
		/* a re-check: the pages stamped since the last one; every page when the texture isn't watched,
		 * after a target download over it (`stale`) or coming back from hot */
		int watched = watch && !e->hot && !e->full, all = !watched || tex_verify();
		uint32_t since = e->wclock, missed = 0;
		if (watch && !e->hot) e->wclock = gwatch_protect(e->addr, e->span);   /* closed again BEFORE the bytes are read: a write from here on gets a newer stamp */
		uint32_t changed = tex_rehash(e, all, watched, since, &missed);
		n_tex_checked++;
		if (e->chk_frame != gpu_frame_no + 1) { e->chk_frame = gpu_frame_no + 1; e->nchk = 0; }
		e->nchk++;
		if (missed) { n_tex_missed++; fprintf(stderr, "    gpu: texture watch MISSED a write: texture %#x fmt %#x %ux%u, %u of %u pages changed unflagged\n", t->data, t->format, t->width, t->height, missed, e->nph); }
		tex_policy(e, changed);
		e->full = 0; e->checked_frame = gpu_frame_no + 1; e->used = ++tex_clock;
		t_texhash += us_now() - th0;
		/* unchanged: written with the same bytes, or only the other data sharing its first/last
		 * page; `stale` likewise only forces this check -- a static render target viewed as a
		 * texture every frame was re-decoded and re-uploaded every frame */
		if (!changed) { e->stale = 0; return e; }
		if (trace_sync()) fprintf(stderr, "gpu trace: texture %#x fmt %#x %ux%u u%#x re-uploaded: %s (%u of %u pages)\n", t->data, t->format, t->width, t->height, t->u_mode, e->stale ? "a target came down over it and its bytes changed" : "its bytes changed", changed, e->nph);
		e->stale = 0; n_tex_changed++;
		uint64_t tu0 = us_now(); upload_texture(e, t); tex_account(e); t_texup += us_now() - tu0;
		if (hitch_log() && us_now() - tu0 > 1000) fprintf(stderr, "    gpu hitch: texture %#x fmt %#x %ux%u layout %d re-uploaded (%s) in %.2f ms\n", t->data, t->format, t->width, t->height, e->layout, e->bc ? "BC blocks" : "decoded", (us_now() - tu0) / 1e3);
		return e;
	}
	if (e) {   /* same slot, another layout or extent: re-decode in place */
		if (trace_sync()) fprintf(stderr, "gpu trace: texture %#x fmt %#x %ux%u u%#x re-uploaded: its layout or extent changed\n", t->data, t->format, t->width, t->height, t->u_mode);
		if (e->layout != layout && e->tex) { batch_retire(e->tex, "texture released"); SDL_ReleaseGPUTexture(gpu(), e->tex); e->tex = 0; }   /* a different layout needs a different texture object */
		free(e->ph); e->ph = 0; e->stale = 0; n_tex_changed++;
	}
	TexEntry *moved = 0; uint64_t ch = 0;
	if (!e) {
		if (!t->width || !t->height) return 0;
		/* a texture whose bytes moved: Killzone compacts its streaming pool with sceGxmTransferCopy every frame (a 2 MB
		 * block 43,712 bytes down), so a dozen BC textures a frame came back at new addresses with the same bytes and were
		 * decoded and uploaded again as new. An entry holding exactly these bytes -- format, size, layout, mip count, span
		 * and a hash of the whole span, as uploaded -- takes the new address instead: its GPU texture is already right. */
		ch = hash_bytes(G(t->data), span); int lv = layout == 3 ? gfx_texture_cube_levels(t) : gfx_texture_levels(t);
		for (int i = 1; i < ntextures && !moved; i++) { TexEntry *x = &textures[i];
			if (x->tex && x->chash == ch && x->span == span && x->format == t->format && x->width == t->width && x->height == t->height && x->layout == layout && x->levels == lv) moved = x; }
		if (moved) {
			if (trace_sync()) fprintf(stderr, "gpu trace: texture %#x fmt %#x %ux%u u%#x: the same bytes as the one at %#x -- moved, not uploaded\n", t->data, t->format, t->width, t->height, t->u_mode, moved->addr);
			e = moved; free(e->ph); e->ph = 0; e->addr = t->data; e->stale = 0; n_tex_moved++;
		}
	}
	if (!e) {
		if (ntextures < MAXTEX && (tex_bytes <= tex_budget() || ntextures < 3)) e = &textures[ntextures++];
		else {   /* full (MAXTEX, or the byte budget): the least recently used goes, with its GPU objects (textures[0] is the unbound-unit fallback: kept) */
			e = &textures[1];
			for (int i = 2; i < ntextures; i++) if (textures[i].used < e->used) e = &textures[i];
			if (e->checked_frame == gpu_frame_no + 1) {   /* this frame's own textures don't fit: it thrashes again */
				static int said; if (!said++) fprintf(stderr, "    gpu: the texture cache (%d entries, %llu MB) can't hold one frame's textures: re-uploads every frame from here (RECOMP_GPU_TEX_BUDGET_MB)\n", ntextures, (unsigned long long)(tex_bytes >> 20));
			}
			tex_bytes -= e->bytes;
			batch_retire(e->tex, "texture released");   /* its samplers go too: they are only bound with it */
			if (e->tex) SDL_ReleaseGPUTexture(gpu(), e->tex);
			for (int a = 0; a < 4; a++) for (int b = 0; b < 2; b++) for (int c = 0; c < 3; c++) for (int d = 0; d < 3; d++) if (e->samp[a][b][c][d]) SDL_ReleaseGPUSampler(gpu(), e->samp[a][b][c][d]);
			free(e->ph);
		}
		if (trace_sync()) fprintf(stderr, "gpu trace: texture %#x fmt %#x %ux%u u%#x uploaded: new\n", t->data, t->format, t->width, t->height, t->u_mode);
		memset(e, 0, sizeof *e); e->addr = t->data; e->format = t->format; e->width = t->width; e->height = t->height; n_tex_new++;
	}
	tex_index[key] = (uint16_t)(e - textures + 1);
	e->span = span; e->nph = tex_pages(t->data, span); e->ph = calloc(e->nph ? e->nph : 1, sizeof *e->ph);
	e->hot = e->streak = e->calm = e->full = 0; e->big_frame = 0;
	if (watch) e->wclock = gwatch_protect(e->addr, span);   /* before the bytes are hashed and decoded */
	{ uint32_t none = 0; tex_rehash(e, 1, 0, 0, &none); }
	e->checked_frame = gpu_frame_no + 1; e->used = ++tex_clock;
	t_texhash += us_now() - th0;
	if (moved && hash_bytes(G(t->data), span) == ch) return e;   /* (still those bytes now the pages are closed: a write in between uploads as usual) */
	{ uint64_t tu0 = us_now(); upload_texture(e, t); tex_account(e); t_texup += us_now() - tu0;
	  if (hitch_log() && us_now() - tu0 > 1000) fprintf(stderr, "    gpu hitch: texture %#x fmt %#x %ux%u layout %d uploaded new (%s) in %.2f ms\n", t->data, t->format, t->width, t->height, e->layout, e->bc ? "BC blocks" : "decoded", (us_now() - tu0) / 1e3); }
	return e;
}

/* ---------------------------------------------------------------- render targets */
typedef struct {
	uint32_t addr, width, height, stride, format;
	uint8_t type;    /* SceGxmColorSurfaceType (GfxTarget.type): the layout guest memory holds this surface in -- the download/upload encoders must use it (Unit 13's G-buffer is tiled; written linearly, the ambient pass read 32x32 blocks of garbage) */
	SDL_GPUTexture *color, *depth;
	SDL_GPUTexture *own_depth;    /* the depth texture get_target made for it, if any -- `depth` is usually the depth table's (a scene binds its surface's: gfx_gpu_begin_scene), which this entry must never release */
	SDL_GPUTexture *fetch_copy;   /* a copy of `color` taken before a FRAGCOLOR draw, for that draw to sample (made on first need) */
	SDL_GPUTexture *lo;           /* render scale > 1: the 1x copy guest-memory syncs go through (made on first need) */
	SDL_GPUTexture *snap;         /* its pixels as its current scene began, copied on the GPU (target_snapshot; made on first need) */
	uint64_t snap_gen;            /* which snapshot that is (a view of it is rebuilt for the next) */
	int sampled;                  /* a GPU draw has read it as a texture (target_view): not a CPU readback buffer (see readback_enqueue) */
	int self_read;                /* a draw of a scene on it sampled the region it was drawing (same_region_read): its scenes start with a download */
	int cpu_reads;                /* the game read its guest memory on the CPU (the read watch, see readback_enqueue): read back after every scene */
	uint64_t drawn_frame, prev_drawn_frame;   /* the last two frames (gpu_frame_no) a scene drew into it: drawn every frame, or now and then (skip_lossless) */
	int alias_in;                 /* this scene began with another target's bytes handed over (target_alias_sync): not its own last frame (skip_lossless) */
	int cpu_dirty;   /* guest memory was written since the GPU texture was last uploaded (a CPU-fallback draw happened) */
	int gpu_dirty;   /* the GPU texture has content guest memory doesn't (a GPU draw happened since the last download) */
	uint64_t used;   /* last use, for eviction */
	int kind;        /* target_kind: the GPU texture's format family */
	uint64_t gen;    /* write_clock when the GPU texture last got new content (a draw, an upload from guest) */
	uint8_t sx, sy;  /* its texels a guest pixel, across and down: the render scale, or a boosted target's (target_scale) */
} TargetEntry;
/* the GPU format a guest colour format renders into, with enough precision that a download encodes
 * the bits gfx_soft.c's target_write would: 8-bit families as RGBA8, the half-float families (F16,
 * U2F10F10F10, F11F11F10, SE5M9M9M9) as RGBA16F, F32/F32F32 as RGBA32F */
/* TK_U8S: an sRGB U8U8U8U8 colour surface (gfx.h GFX_TGT_GAMMA) -- an sRGB GPU texture, so the
 * shader's linear output is encoded on write and blending happens in linear, as on the hardware;
 * its bytes are the guest's bytes (the round trip is raw). Sampled directly the GPU decodes it, so a
 * non-gamma texture over it gets the shader's re-encode (UsseDrawUniforms.tex_srgb word 1). */
/* TK_HDR10 (opt-in, RECOMP_GPU_HDR_RG11B10=1): a U2F10F10F10 target as R11G11B10_UFLOAT, 4 bytes a
 * texel as on the Vita, instead of RGBA16F's 8. Its alpha reads as 1.0 everywhere (the view decode's
 * 2-bit alpha, a FRAGCOLOR fetch, a DST_ALPHA blend factor), where RGBA16F kept what the shader wrote;
 * HDR values differ by the 11/11/10-bit encoding before the view's own f10 quantisation. An A/B of
 * the lighting passes' bandwidth, off by default until that difference is measured as harmless. (The
 * translator sees it as its own colour kind: a raw-word colour program would lay its words out as
 * bytes there, as it would for RGBA8 -- no program writes raw words into an HDR target.) */
enum { TK_U8, TK_F16, TK_F32, TK_U8S, TK_HDR10 };
/* The F32 family (F32: one word a pixel, F32F32: two; the Vita has no wider float colour format)
 * renders as R32G32_FLOAT, 8 bytes a texel, not RGBA32F's 16: gfx_soft.c's target_write stores
 * exactly words 0 and 1 of the colour and its decode reads back (r, g, 0, 1), which is what an R32G32
 * texture samples as -- nothing downstream ever read channels b/a (the byte views pick word 0 or 1,
 * the translator's raw-word layout puts word k in channel k and the GPU drops k >= 2, the download
 * encodes two words). Unit 13's F32F32 G-buffer at 16 B/px was twice the write and twice the read of
 * its ~60 light volumes a frame, at every render scale. RECOMP_GPU_F32F32_RGBA=1: RGBA32F as before,
 * also the fallback where R32G32 is no render target format. Decided once, before the first target. */
static int f32_rg(void) {
	static int v = -1;
	if (v < 0) {
		const char *e = getenv("RECOMP_GPU_F32F32_RGBA");
		if (!gpu()) return 0;   /* (no device yet: nothing is created from the answer) */
		v = !(e && *e && *e != '0') && SDL_GPUTextureSupportsFormat(gpu(), SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
	}
	return v;
}
static int hdr_rg11b10(void) {
	static int v = -1;
	if (v < 0) {
		const char *e = getenv("RECOMP_GPU_HDR_RG11B10"); int want = e && *e && *e != '0';
		if (!gpu()) return 0;
		v = want && SDL_GPUTextureSupportsFormat(gpu(), SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
		if (want && !v) fprintf(stderr, "gpu: R11G11B10_UFLOAT is no render target format here: U2F10F10F10 targets stay RGBA16F\n");
	}
	return v;
}
static int target_kind(uint32_t fmt) {
	switch (fmt & 0xF1800000u) {
	case 0x41000000u: return hdr_rg11b10() ? TK_HDR10 : TK_F16;
	case 0x01000000u: case 0x00800000u: case 0xF0000000u: case 0x21000000u: case 0x31000000u: return TK_F16;
	case 0x10800000u: case 0x11000000u: return TK_F32;
	default: return GFX_TGT_GAMMA(fmt) && (fmt & 0xF1800000u) == 0 ? TK_U8S : TK_U8;
	}
}
static SDL_GPUTextureFormat kind_format(int k) {
	return k == TK_F32 ? (f32_rg() ? SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT : SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT) : k == TK_F16 ? SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT
	     : k == TK_HDR10 ? SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT : k == TK_U8S ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
}
static uint32_t kind_bytes(int k) { return k == TK_F32 ? (f32_rg() ? 8u : 16u) : k == TK_F16 ? 8 : 4; }   /* (TK_HDR10: 4) */
/* ---- internal resolution. RECOMP_RENDER_SCALE=n (1-RSCALE_MAX; F2 in the window cycles them): every GPU
 * render target, depth/stencil surface, view and FRAGCOLOR copy is n times its guest surface's size,
 * and every draw's viewport and scissor n times its own -- the game's geometry is rasterised at n
 * times the Vita's resolution. TargetEntry and DepthEntry keep the guest's sizes (the syncs, views
 * and address lookups all work in guest pixels); RSX()/RSY() are the GPU texture's. Guest memory stays 1x:
 * a download point-samples each pixel's n x n block down through a 1x copy (TargetEntry.lo), an
 * upload replicates each pixel up the same way. Textures from guest memory are unchanged (sampled
 * at the finer rate, they pick finer mip levels). RECOMP_GFX_CHECK compares per guest pixel: 1x. */
static int rscale = 1, rscale_req;
#define RSCALE_MAX 6   /* 5760x3264 for a 960x544 frame; at 8x a 2048-wide target would pass Metal's 16384-texel limit */
int gfx_gpu_render_scale_max(void) { return RSCALE_MAX; }
/* and horizontally: for a wider aspect ratio (window.c window_aspect_widen: the game's camera hook draws a
 * k-times wider view into the same guest frame) the x scale is rscale * k rounded up, so the extra view
 * gets its own pixels instead of being squeezed -- every target, depth surface, view, viewport, scissor
 * and copy below is RSX wide and RSY high; the texel-addressed shader paths (target views, snap_fetch)
 * take both factors. 1 and rscale at the native aspect. */
static int rscale_x = 1;
#define RSX(v) ((uint32_t)(v) * (uint32_t)rscale_x)
#define RSY(v) ((uint32_t)(v) * (uint32_t)rscale)
#define SCALED (rscale > 1 || rscale_x > 1)
/* ---- a target's own resolution (RECOMP_TARGET_SCALE, a game's RUN_ENV: "<w>x<h>:<factor>[,...]"): a render target of that
 * guest size draws at <factor> times the render scale, rounded to whole texels a guest pixel -- a game that renders its
 * world below the screen's resolution (Uncharted: 720x408, composited onto its 960x544 frame) gets the world at the
 * screen's texel density instead of three quarters of it. Every texel-addressed path takes the owner's scale (TargetEntry,
 * DepthEntry and view TexEntry sx, sy): the target's texture, its scenes' viewport, scissor, framebuffer copy and depth
 * surface, its views, uploads, downloads and readbacks, snap_fetch's block, the visibility counts' divisor. */
static struct { uint32_t w, h; float f; } tscale[8]; static int ntscale = -1;
static void tscale_init(void) {
	if (ntscale >= 0) return;
	ntscale = 0; const char *e = getenv("RECOMP_TARGET_SCALE");
	while (e && *e && ntscale < 8) {
		unsigned w, h; float f; int used = 0;
		if (sscanf(e, "%ux%u:%f%n", &w, &h, &f, &used) == 3 && used > 0 && f >= 1.0f && f <= 4.0f) { tscale[ntscale].w = w; tscale[ntscale].h = h; tscale[ntscale].f = f; ntscale++; }
		else break;
		e += used; if (*e == ',') e++;
	}
}
int gfx_gpu_target_scale_supported(void) { tscale_init(); return ntscale > 0; }   /* the game names targets to draw finer (the overlay's "Full-resolution world" row) */
static void target_scale(uint32_t w, uint32_t h, uint8_t *sx, uint8_t *sy) {
	tscale_init();
	int x = rscale_x, y = rscale;
	if (ntscale > 0 && settings_int("full_res_world", 1)) for (int k = 0; k < ntscale; k++) if (tscale[k].w == w && tscale[k].h == h) {   /* (settings "full_res_world" 0: the game's own resolution; read at each scene: a change applies at once -- targets at the other scale are new entries, the old ones age out) */
		int a = (int)((float)rscale_x * tscale[k].f + 0.5f), b = (int)((float)rscale * tscale[k].f + 0.5f);
		if (a > x) x = a; if (b > y) y = b; break;
	}
	*sx = (uint8_t)(x > 8 ? 8 : x); *sy = (uint8_t)(y > 8 ? 8 : y);   /* (8: a 2048-wide target within Metal's 16384 texels) */
}
static int any_scaled(void) { tscale_init(); return SCALED || ntscale > 0; }
#define TSX(e, v) ((uint32_t)(v) * (uint32_t)(e)->sx)
#define TSY(e, v) ((uint32_t)(v) * (uint32_t)(e)->sy)
#define TSCALED(e) ((e)->sx > 1 || (e)->sy > 1)
static int scene_sx = 1, scene_sy = 1;   /* the current scene's target's scale (gfx_gpu_begin_scene) */
float window_aspect_widen(void);   /* window.c (gfxreplay.c: 1, replays render at the captured aspect) */
static int want_rscale_x(int n) {
	float k = window_aspect_widen(), xf = (float)n * k;
	int x = (int)xf; if ((float)x < xf * 0.97f) x++;   /* rounded up, unless within 3%: 32:9 at 2x is 4.03 -- 5 drew 24% more pixels on every wide target for a 0.7% narrower picture (at 1x, 2.01 drew 3x) */
	return x < n ? n : x > 8 ? (8 > n ? 8 : n) : x;   /* 8: a 2048-wide target stays within Metal's 16384 */
}
/* RECOMP_GPU_PRESENT_NOSYNC=1 (headless measurement): a presented frame is NOT downloaded to guest memory
 * at present when nothing reads it there. With a window the frame goes to the swapchain from its GPU
 * texture and guest memory keeps the frame before it (gfx_gpu_present); headless, nothing shows the
 * frame, and the download (a fence wait on the whole frame's GPU work, then a float decode and encode
 * of 960x544 pixels) is 8-12 ms of renderer time a frame the windowed game never pays -- a headless
 * frame-time measurement with it on isn't the game the player plays. Shot frames still sync (RECOMP_SHOT
 * reads guest memory), as do CPU reads and writes of the frame (gfx_gpu_guest_writing, target_view). */
static int present_nosync(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_GPU_PRESENT_NOSYNC"); v = e && *e && *e != '0'; } return v; }
static void render_scale_init(void) {
	static int done; if (done) return; done = 1;
	const char *e = getenv("RECOMP_RENDER_SCALE"); int n = e ? atoi(e) : settings_int("render_scale", 1);   /* the environment over the game's settings.ini */
	if (n < 1) n = 1; if (n > RSCALE_MAX) n = RSCALE_MAX;
	if (getenv("RECOMP_GFX_CHECK")) n = 1;
	rscale = n; rscale_x = want_rscale_x(n); metrics_set_render_scale(n);
	if (n > 1 || rscale_x > 1) fprintf(stderr, "gpu: render scale %dx (%dx horizontally)\n", n, rscale_x);
}
/* F2 (window.c): the next scale, 1 -> 2 -> ... -> RSCALE_MAX -> 1, applied after the frame being presented (gfx_gpu_present) */
void gfx_gpu_render_scale_cycle(void) {
	int cur = __atomic_load_n(&rscale_req, __ATOMIC_RELAXED); if (!cur) cur = rscale;
	__atomic_store_n(&rscale_req, cur % RSCALE_MAX + 1, __ATOMIC_RELAXED);
}
/* the overlay menu's Display page: a direct pick instead of cycling, same clamp and timing as F2 */
void gfx_gpu_render_scale_request(int n) {
	if (n < 1) n = 1; if (n > RSCALE_MAX) n = RSCALE_MAX;
	__atomic_store_n(&rscale_req, n, __ATOMIC_RELAXED);
}
int gfx_gpu_render_scale(void) { return rscale; }
int gfx_gpu_render_scale_wanted(void) { const int r = __atomic_load_n(&rscale_req, __ATOMIC_RELAXED); return r ? r : rscale; }   /* (the menus' pick: a request waits for the next scene, and the game is paused under the menu) */
static uint16_t f2h(float f) {   /* round-to-nearest-even half, with the specials */
	uint32_t b; memcpy(&b, &f, 4);
	uint32_t s = (b >> 16) & 0x8000, e = (b >> 23) & 0xff, m = b & 0x7fffff;
	if (e == 0xff) return (uint16_t)(s | 0x7c00 | (m ? 0x200 : 0));
	int ne = (int)e - 127 + 15;
	if (ne >= 31) return (uint16_t)(s | 0x7c00);
	if (ne <= 0) { if (ne < -10) return (uint16_t)s; m |= 0x800000; uint32_t sh = (uint32_t)(14 - ne); uint32_t h = m >> sh, rem = m & ((1u << sh) - 1), half = 1u << (sh - 1); if (rem > half || (rem == half && (h & 1))) h++; return (uint16_t)(s | h); }
	uint32_t h = s | (uint32_t)ne << 10 | (m >> 13), rem = m & 0x1fff;
	if (rem > 0x1000 || (rem == 0x1000 && (h & 1))) h++;
	return (uint16_t)h;
}
static float h2f(uint16_t h) {
	uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 31, m = h & 0x3ff, b;
	if (e == 0) { if (!m) b = s; else { int sh = 0; while (!(m & 0x400)) { m <<= 1; sh++; } b = s | (uint32_t)(113 - sh) << 23 | (m & 0x3ff) << 13; } }
	else if (e == 31) b = s | 0x7f800000 | m << 13;
	else b = s | (e + 112) << 23 | m << 13;
	float f; memcpy(&f, &b, 4); return f;
}
/* a deferred renderer (Unit 13: G-buffer, HDR, shadow, UI passes) cycles through many surfaces;
 * when full, the least recently used one is synced back to guest memory and released */
#define MAXTARGET 32
static TargetEntry targets[MAXTARGET]; static int ntargets;
static uint64_t target_clock;
static void target_download_to_guest(TargetEntry *e);
static uint32_t n_fallback, n_gpu_draws; static uint64_t n_gpu_indices;   /* n_gpu_indices: this frame's GPU draws' index counts summed (RECOMP_GPU_STATS: how much geometry -- a level of detail setting's effect) */
/* metrics.c: how many draws this run has sent to the GPU vs. the software fallback path, so far --
 * cumulative, read-only; metrics.c takes its own deltas (RECOMP_GPU_STATS's report, above, is the
 * only thing that ever zeroes these, once a second while it's on) */
void gfx_gpu_get_counts(uint32_t *gpu_draws, uint32_t *fallback_draws) {   /* another thread reads them: atomic both ways */
	*gpu_draws = __atomic_load_n(&n_gpu_draws, __ATOMIC_RELAXED); *fallback_draws = __atomic_load_n(&n_fallback, __ATOMIC_RELAXED);
}
/* RECOMP_GPU_TRACE_SYNC=1: every target upload/download, depth preload, software fallback and sampler binding on stderr */
static int trace_sync(void) {   /* RECOMP_GPU_TRACE_SYNC=1: every sync, flush and view; =a-b: only while gpu_frame_no (presents) is in a..b */
	static int v = -1; static uint64_t lo, hi = ~0ull;
	if (v < 0) { const char *e = getenv("RECOMP_GPU_TRACE_SYNC"); v = e != 0; if (e && strchr(e, '-')) { lo = strtoull(e, 0, 10); hi = strtoull(strchr(e, '-') + 1, 0, 10); } }
	return v && gpu_frame_no >= lo && gpu_frame_no <= hi;
}
/* its GPU objects. Only the depth texture it made itself: `depth` may be a texture the depth table
 * owns -- or owned, and has since released (a surface evicted from the table while a target still
 * pointed at it). Asking the table whether it is "shared" released such a texture a second time
 * whenever the table had already let it go: an F2 render scale change, which drops the table before
 * the depth-only scratch target, aborted in SDL's pending-destroy list (pointer being freed was not
 * allocated) */
static void target_release(TargetEntry *e) {
	if (e->color) SDL_ReleaseGPUTexture(gpu(), e->color);
	if (e->fetch_copy) SDL_ReleaseGPUTexture(gpu(), e->fetch_copy);
	if (e->lo) SDL_ReleaseGPUTexture(gpu(), e->lo);
	if (e->snap) SDL_ReleaseGPUTexture(gpu(), e->snap);
	if (e->own_depth) SDL_ReleaseGPUTexture(gpu(), e->own_depth);
	e->color = e->fetch_copy = e->lo = e->snap = e->depth = e->own_depth = 0;
}
static TargetEntry *get_target(const GfxTarget *t) {
	uint8_t sx, sy; target_scale(t->width, t->height, &sx, &sy);
	for (int i = 0; i < ntargets; i++) if (targets[i].addr == t->data && targets[i].width == t->stride && targets[i].height == t->height && targets[i].kind == target_kind(t->format) && targets[i].type == t->type && targets[i].sx == sx && targets[i].sy == sy) {
		TargetEntry *e = &targets[i];
		e->used = ++target_clock;
		/* the same colour address can be used by one scene with a depth buffer and another
		 * without (e.g. a 3D world pass vs. a flat composite/UI pass onto the same surface) --
		 * a pipeline built for the depth case needs an actual depth attachment to hand the render
		 * pass, even if this target's very first use here didn't have one yet */
		if (t->depth && !e->own_depth) {
			SDL_GPUTextureCreateInfo dci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = depth_format(), .usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
				.width = TSX(e, e->width), .height = TSY(e, e->height), .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
			e->depth = e->own_depth = SDL_CreateGPUTexture(gpu(), &dci);
		}
		return e;
	}
	TargetEntry *e;
	if (ntargets < MAXTARGET) e = &targets[ntargets++];
	else {
		e = &targets[0];
		for (int i = 1; i < MAXTARGET; i++) if (targets[i].used < e->used) e = &targets[i];
		if (e->gpu_dirty) target_download_to_guest(e);   /* its pixels live on in guest memory */
		target_release(e);
	}
	memset(e, 0, sizeof *e); e->addr = t->data; e->width = t->stride; e->height = t->height; e->format = t->format; e->type = t->type; e->used = ++target_clock; e->sx = sx; e->sy = sy;
	e->kind = target_kind(t->format);
	e->cpu_dirty = 1; e->gen = ++write_clock;   /* starts as whatever guest memory holds (an earlier frame's render, a CPU write) */
	SDL_GPUTextureCreateInfo tci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = kind_format(e->kind),
		.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
		.width = TSX(e, e->width), .height = TSY(e, e->height), .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
	e->color = SDL_CreateGPUTexture(gpu(), &tci);
	if (t->depth) {
		SDL_GPUTextureCreateInfo dci = tci; dci.format = depth_format(); dci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
		e->depth = e->own_depth = SDL_CreateGPUTexture(gpu(), &dci);
	}
	if (!e->color || (t->depth && !e->depth)) {   /* the software path takes this scene instead */
		fprintf(stderr, "    gpu: render target %#x %ux%u fmt %#x: %s\n", t->data, e->width, e->height, t->format, SDL_GetError());
		target_release(e);
		memset(e, 0, sizeof *e);
		return 0;
	}
	return e;
}
/* guest memory -> the GPU colour texture (a CPU-fallback draw wrote guest memory; later GPU draws
 * in this scene need to see it) */
/* a TargetEntry as the GfxTarget gfx_soft.c's decode/encode need (only data/format/stride/height
 * matter to them; the GPU texture is always R8G8B8A8, so this entry's own "width" -- really its
 * stride, see get_target -- is both the GfxTarget stride and the decoded/encoded buffer's) */
static GfxTarget target_entry_as_gfx(const TargetEntry *e) { GfxTarget t = { e->addr, e->format, e->width, e->height, e->width, 0 }; t.type = e->type; return t; }

/* R11G11B10_UFLOAT's components: unsigned floats with a 5-bit exponent (bias 15) and 6 (R, G) or 5 (B)
 * mantissa bits -- gfx_soft.c's to_small_float/small_float rules (round half up, 65504 saturates,
 * nothing below zero), for the guest-memory round trip of a TK_HDR10 target */
#include <math.h>
static uint32_t to_ufloat(float f, int mbits) {
	if (!(f > 0)) return 0;
	if (f >= 65504.f) return (30u << mbits) | ((1u << mbits) - 1);
	int e; float m = frexpf(f, &e);   /* f = m * 2^e, m in [0.5, 1) */
	int be = e - 1 + 15;
	if (be <= 0) return (uint32_t)(f * ldexpf(1.f, 14 + mbits) + .5f) & ((1u << mbits) - 1);
	uint32_t mant = (uint32_t)((m * 2 - 1) * (1u << mbits) + .5f);
	if (mant >> mbits) { mant = 0; be++; }
	return ((uint32_t)be << mbits) | mant;
}
static float from_ufloat(uint32_t v, int mbits) {
	uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
	if (e == 0) return ldexpf((float)m, -14 - mbits);
	if (e == 31) return m ? 0.f : 65504.f;
	return ldexpf(1.f + (float)m / (1u << mbits), (int)e - 15);
}
/* RGBA floats <-> the GPU texture's own texel bytes for its kind (kind_bytes each) */
static void floats_to_texels(int kind, const float *in, uint8_t *out, uint32_t n) {
	uint32_t tb = kind_bytes(kind);
	for (uint32_t i = 0; i < n; i++) {
		const float *c = in + 4 * i;
		if (kind == TK_F32) memcpy(out + tb * i, c, tb);   /* R32G32: the first two words; RGBA32F: all four */
		else if (kind == TK_F16) { uint16_t h[4] = { f2h(c[0]), f2h(c[1]), f2h(c[2]), f2h(c[3]) }; memcpy(out + 8 * i, h, 8); }
		else if (kind == TK_HDR10) { uint32_t p = to_ufloat(c[0], 6) | to_ufloat(c[1], 6) << 11 | to_ufloat(c[2], 5) << 22; memcpy(out + 4 * i, &p, 4); }
		else for (int k = 0; k < 4; k++) { float v = c[k]; out[4 * i + k] = (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : (uint32_t)(v * 255 + .5f)); }
	}
}
static void texels_to_floats(int kind, const uint8_t *in, float *out, uint32_t n) {
	uint32_t tb = kind_bytes(kind);
	for (uint32_t i = 0; i < n; i++) {
		float *c = out + 4 * i;
		if (kind == TK_F32) { memcpy(c, in + tb * i, tb); if (tb == 8) { c[2] = 0; c[3] = 1; } }   /* R32G32 samples as (r, g, 0, 1): gfx_soft.c's F32F32 decode */
		else if (kind == TK_F16) { uint16_t h[4]; memcpy(h, in + 8 * i, 8); for (int k = 0; k < 4; k++) c[k] = h2f(h[k]); }
		else if (kind == TK_HDR10) { uint32_t p; memcpy(&p, in + 4 * i, 4); c[0] = from_ufloat(p & 0x7ff, 6); c[1] = from_ufloat((p >> 11) & 0x7ff, 6); c[2] = from_ufloat(p >> 22, 5); c[3] = 1; }
		else for (int k = 0; k < 4; k++) c[k] = in[4 * i + k] / 255.f;
	}
}
/* guest memory -> the GPU colour texture (a CPU-fallback draw wrote guest memory, or the target is
 * new and starts as whatever an earlier frame or the CPU left there) */
static TargetEntry *scene_entry;   /* the current scene's colour target (see gfx_gpu_begin_scene) */
static int scene_open;             /* between gfx_gpu_begin_scene and gfx_gpu_end_scene (scene_entry stays set after: the last scene's) */
static SDL_GPUTexture *target_lo(TargetEntry *e) {   /* render scale > 1: the 1x copy of e's texture guest-memory syncs go through */
	if (!e->lo) {
		SDL_GPUTextureCreateInfo ci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = kind_format(e->kind), .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
			.width = e->width, .height = e->height, .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
		e->lo = SDL_CreateGPUTexture(gpu(), &ci);
		if (!e->lo) fprintf(stderr, "    gpu: 1x copy of target %#x %ux%u: %s\n", e->addr, e->width, e->height, SDL_GetError());
	}
	return e->lo;
}
static void target_upload_from_guest(TargetEntry *e) {
	if (e == scene_entry || batch_uses(e->color)) batch_flush("target upload");   /* the upload's own command buffer would otherwise run before the pending draws (or view passes) it must follow */
	if (trace_sync()) fprintf(stderr, "gpu trace: upload target %#x from guest (after %u gpu + %u software draws)\n", e->addr, n_gpu_draws, n_fallback);
	uint32_t npx = e->width * e->height, n = kind_bytes(e->kind) * npx;
	float *f = malloc(16u * npx); uint8_t *raw = malloc(n);
	GfxTarget t = target_entry_as_gfx(e);
	gfx_decode_target_f32(&t, f);   /* the game's own declared colour format (565, F16, F32F32, ...), decoded as gfx_soft.c reads it */
	if (trace_sync() && npx > 10 * e->width + 10) fprintf(stderr, "gpu trace:   upload (10,10) = %g %g %g, guest word %#x\n", f[4 * (10 * e->width + 10)], f[4 * (10 * e->width + 10) + 1], f[4 * (10 * e->width + 10) + 2], RD32(e->addr + 4 * (10 * e->width + 10)));
	floats_to_texels(e->kind, f, raw, npx);
	SDL_GPUTransferBufferCreateInfo xci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = n };
	SDL_GPUTransferBuffer *xb = SDL_CreateGPUTransferBuffer(gpu(), &xci);
	void *map = SDL_MapGPUTransferBuffer(gpu(), xb, true); memcpy(map, raw, n); SDL_UnmapGPUTransferBuffer(gpu(), xb);
	free(f); free(raw);
	SDL_GPUTexture *up = TSCALED(e) && target_lo(e) ? e->lo : e->color;   /* scaled: the 1x copy, then each pixel replicated up */
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu()); SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureTransferInfo src = { .transfer_buffer = xb, .pixels_per_row = e->width, .rows_per_layer = e->height };
	SDL_GPUTextureRegion dst = { .texture = up, .w = e->width, .h = e->height, .d = 1 };
	SDL_UploadToGPUTexture(cp, &src, &dst, true);
	SDL_EndGPUCopyPass(cp);
	if (up != e->color) {
		SDL_GPUBlitInfo bl = { .source = { .texture = up, .w = e->width, .h = e->height }, .destination = { .texture = e->color, .w = TSX(e, e->width), .h = TSY(e, e->height) }, .load_op = SDL_GPU_LOADOP_DONT_CARE, .filter = SDL_GPU_FILTER_NEAREST };
		SDL_BlitGPUTexture(cmd, &bl);
	}
	SDL_SubmitGPUCommandBuffer(cmd);
	SDL_ReleaseGPUTransferBuffer(gpu(), xb);
	e->cpu_dirty = 0; e->gen = ++write_clock;
}
/* the GPU colour texture -> RGBA floats in a caller-owned buffer (not guest memory: RECOMP_GFX_CHECK
 * compares it against the software path without the two stepping on each other) */
static void target_download_texels(TargetEntry *e, float *out, uint8_t *raw);
static void target_download_floats(TargetEntry *e, float *out) { target_download_texels(e, out, 0); }
static void target_download_texels(TargetEntry *e, float *out, uint8_t *raw) {   /* raw: the texture's bytes as they are (kind_bytes a texel) instead of floats */
	if (e == scene_entry) batch_flush("target download");   /* the pending draws are what is being read */
	uint32_t npx = e->width * e->height, n = kind_bytes(e->kind) * npx;
	SDL_GPUTransferBufferCreateInfo xci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = n };
	SDL_GPUTransferBuffer *xb = SDL_CreateGPUTransferBuffer(gpu(), &xci);
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu());
	SDL_GPUTexture *down = e->color;
	if (TSCALED(e) && target_lo(e)) {   /* scaled: one sample of each pixel's n x n block, into the 1x copy (nearest: the guest's bytes are a sample, not a blend -- a packed G-buffer averaged is garbage) */
		SDL_GPUBlitInfo bl = { .source = { .texture = e->color, .w = TSX(e, e->width), .h = TSY(e, e->height) }, .destination = { .texture = e->lo, .w = e->width, .h = e->height }, .load_op = SDL_GPU_LOADOP_DONT_CARE, .filter = SDL_GPU_FILTER_NEAREST };
		SDL_BlitGPUTexture(cmd, &bl); down = e->lo;
	}
	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureRegion src = { .texture = down, .w = e->width, .h = e->height, .d = 1 };
	SDL_GPUTextureTransferInfo dst = { .transfer_buffer = xb, .pixels_per_row = e->width, .rows_per_layer = e->height };
	SDL_DownloadFromGPUTexture(cp, &src, &dst);
	SDL_EndGPUCopyPass(cp);
	SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
	SDL_WaitForGPUFences(gpu(), true, &fence, 1); SDL_ReleaseGPUFence(gpu(), fence);
	const uint8_t *map = SDL_MapGPUTransferBuffer(gpu(), xb, false);
	if (raw) memcpy(raw, map, n); else texels_to_floats(e->kind, map, out, npx);
	SDL_UnmapGPUTransferBuffer(gpu(), xb);
	SDL_ReleaseGPUTransferBuffer(gpu(), xb);
}
/* an 8-bit target whose guest format is the GPU texture's own byte layout (gfx_target_plain_rgba8): guest memory takes the
 * downloaded bytes as they are -- the float round trip (decode, then encode texel by texel) cost ~3 ms a 960x544 present */
static int target_raw_ok(const TargetEntry *e, const GfxTarget *t) { return (e->kind == TK_U8 || e->kind == TK_U8S) && t->stride == e->width && gfx_target_plain_rgba8(t); }
static void target_download_to_buffer(TargetEntry *e, uint8_t *out) {   /* as RGBA8, for RECOMP_GFX_CHECK's comparison */
	uint32_t npx = e->width * e->height;
	float *f = malloc(16u * npx); target_download_floats(e, f);
	floats_to_texels(TK_U8, f, out, npx); free(f);
}
static void textures_invalidate(uint32_t lo, uint32_t hi);
/* the GPU colour texture -> guest memory, in the game's declared format, exactly as gfx_soft.c's
 * target_write would have stored the same colours (so a CPU-fallback draw, a texture that reads
 * this memory in another format or layout, present and RECOMP_SHOT all see the real bytes) */
static const char *sync_why = "";   /* for the trace: what asked for this download */
static void target_download_to_guest(TargetEntry *e) {
	if (trace_sync()) fprintf(stderr, "gpu trace: download target %#x to guest %s (after %u gpu + %u software draws)\n", e->addr, sync_why, n_gpu_draws, n_fallback);
	uint64_t tdl0 = us_now(); n_tgt_downloads++;
	uint32_t npx = e->width * e->height;
	GfxTarget t = target_entry_as_gfx(e);
	uint32_t gbytes = (uint32_t)gfx_target_bpp(&t) * npx;
	if (target_raw_ok(e, &t)) {
		gwatch_write_begin(e->addr, gbytes); target_download_texels(e, 0, G(e->addr)); gwatch_write_end(e->addr, gbytes);
		e->gpu_dirty = 0;
		textures_invalidate(e->addr, e->addr + gbytes);
		t_tgt_download += us_now() - tdl0;
		if (hitch_log() && us_now() - tdl0 > 1000) fprintf(stderr, "    gpu hitch: target %#x %ux%u downloaded to guest memory %s in %.2f ms\n", e->addr, e->width, e->height, sync_why, (us_now() - tdl0) / 1e3);
		return;
	}
	float *f = malloc(16u * npx); target_download_floats(e, f);
	{	/* RECOMP_GPU_TRACE_ADDR=hexaddr: every download that writes that guest address (which target, why) -- what found a
		 * stale float target clobbering Killzone's glass strip (2026-10-17) */
		static uint32_t ta = 1; if (ta == 1) { const char *v = getenv("RECOMP_GPU_TRACE_ADDR"); ta = v ? (uint32_t)strtoul(v, 0, 16) : 0; }
		if (ta && ta >= e->addr && ta < e->addr + gbytes)
			fprintf(stderr, "gpu trace addr: download target %#x fmt %#x %ux%u kind %d (frame %llu) %s writes %#x\n", e->addr, t.format, e->width, e->height, e->kind, (unsigned long long)gpu_frame_no, sync_why, ta);
	}
	gwatch_write_begin(e->addr, gbytes);   /* one call opens the pages under a watched texture, instead of a write fault per page */
	gfx_encode_target_f32(&t, f);
	gwatch_write_end(e->addr, gbytes);
	{ static uint32_t ta = 1, last = 0xdeadbeef; if (ta == 1) { const char *v = getenv("RECOMP_GPU_TRACE_ADDR"); ta = v ? (uint32_t)strtoul(v, 0, 16) : 0; }
	  if (ta && ta >= e->addr && ta < e->addr + gbytes && RD32(ta) != last) { last = RD32(ta); fprintf(stderr, "gpu trace addr: frame %llu: %#x now holds %#x\n", (unsigned long long)gpu_frame_no, ta, last); } }
	if (trace_sync() && npx > 10 * e->width + 10) fprintf(stderr, "gpu trace:   download (10,10) = %g %g %g -> guest word %#x\n", f[4 * (10 * e->width + 10)], f[4 * (10 * e->width + 10) + 1], f[4 * (10 * e->width + 10) + 2], RD32(e->addr + 4 * (10 * e->width + 10)));
	free(f);
	e->gpu_dirty = 0;
	textures_invalidate(e->addr, e->addr + gbytes);
	t_tgt_download += us_now() - tdl0;
	if (hitch_log() && us_now() - tdl0 > 1000) fprintf(stderr, "    gpu hitch: target %#x %ux%u downloaded to guest memory %s in %.2f ms\n", e->addr, e->width, e->height, sync_why, (us_now() - tdl0) / 1e3);
}

/* ---------------------------------------------------------------- pipelines */
typedef struct PipeEntry {
	uint32_t vprog, fprog;
	GfxBlend blend; int cull; int cull_sdl; uint32_t depth_func; int depth_write;   /* cull_sdl: the SDL cull mode this draw's cull + viewport flip resolve to */
	uint8_t stencil_func, stencil_fail, depth_fail, depth_pass, stencil_cmask, stencil_wmask, stencil_ref;
	int has_depth, ckind;   /* ckind: the colour target's kind (its texture format) */
	int depth_ok, stencil_ok;   /* the bound depth/stencil surface's own format: an S8-only surface
	                             * must not depth-test, a depth-only one must not stencil-test */
	uint8_t fp_disabled;       /* sceGxmSetFrontFragmentProgramEnable(..., DISABLED): no colour write */
	int8_t dbf, dbu;           /* GfxDraw.depth_bias_factor / _units */
	uint8_t dbs;               /* the render scale the slope factor was multiplied by (bias_scale; 0 without a slope factor) */
	SDL_GPUGraphicsPipeline *pipe; int pending;   /* pending: a build worker is creating pipe (gpu_async) */
	const void *vse, *fse;            /* the ShaderEntries: a program's variants (cube_mask, snap_mask) share its hash */
	const int *smp_units; int nsmp;   /* points at the fragment ShaderEntry's own arrays */
	int fetch;                         /* the fragment shader samples the framebuffer copy at binding nsmp (fetch mode 1 only: a framebuffer-fetch program needs nothing bound or copied) */
	uint32_t hkey; int hnext; uint64_t used;   /* the table's hash index (pipe_key_hash, bucket chain: index + 1) and last use (LRU) */
	uint8_t low, prebuilt;   /* as ShaderEntry's */
} PipeEntry;
/* The pipeline table: a hash index, and when full the least recently used pipeline goes (after the batch's draws that
 * use it). It was a fixed 2048 with a linear search per draw and no eviction; a 10-minute Killzone session with
 * render-scale changes (the slope bias and the snap variants make pipelines per scale) filled it, and from then on
 * every draw wanting a new pipeline went to the software path -- each one a 6 ms download and re-upload of the HDR
 * target, ~200 ms frames whenever a new effect or area came into view (2026-10-17). */
#define MAXPIPE 4096
#define PIPE_HASH 4096
static void pipeline_build(PipeJob *j) {
	j->ci.target_info.color_target_descriptions = &j->ctd;
	if (!j->ci.vertex_shader && j->vse) j->ci.vertex_shader = j->vse->shader;   /* (queued while they compiled: gpu_overlap) */
	if (!j->ci.fragment_shader && j->fse) j->ci.fragment_shader = j->fse->shader;
	if (!j->ci.vertex_shader || !j->ci.fragment_shader) {   /* a shader that failed to compile: no pipeline -- its draws are dropped at the flush */
		fprintf(stderr, "    gpu: pipeline %#x/%#x: a shader failed to build\n", j->vprog, j->fprog);
		j->p->pipe = 0; __atomic_store_n(&j->p->pending, 0, __ATOMIC_RELEASE);
		if (j->warm) { warm_unref(j->vse); warm_unref(j->fse); free(j->p); }
		return;
	}
	uint64_t tpb0 = us_now();
#ifdef RECOMP_MTL_ARCHIVE
	if (mtl_archive_on) { ShaderEntry *v = j->vse, *f = j->fse;   /* this machine's archive: every pipeline a draw or a recipe makes (a predicted shader's too, once a draw uses it), not the predicted pass's own guesses; and its shaders' libraries while they have none */
		const int guess = j->warm && ((v && v->pred) || (f && f->pred));
		mtl_archive_want(!guess, v ? __atomic_exchange_n(&v->lib_key, 0, __ATOMIC_ACQ_REL) : 0, f ? __atomic_exchange_n(&f->lib_key, 0, __ATOMIC_ACQ_REL) : 0); }
#endif
	SDL_GPUGraphicsPipeline *pp = SDL_CreateGPUGraphicsPipeline(gpu(), &j->ci);
#ifdef RECOMP_MTL_ARCHIVE
	if (mtl_archive_on) mtl_archive_want_clear();   /* (if SDL made none) */
#endif
	uint64_t dt = us_now() - tpb0; __atomic_add_fetch(&t_pipe_build, dt, __ATOMIC_RELAXED); __atomic_add_fetch(&n_pipe_builds, 1, __ATOMIC_RELAXED);
	if (hitch_log() && (dt > 300 || j->async) && !j->p->prebuilt) fprintf(stderr, "    gpu hitch: pipeline %#x/%#x created in %.2f ms%s\n", j->vprog, j->fprog, dt / 1e3, j->async ? " (by a worker)" : "");
	if (!pp) fprintf(stderr, "    gpu: pipeline %#x/%#x: %s\n", j->vprog, j->fprog, SDL_GetError());
	j->p->pipe = pp;
	__atomic_store_n(&j->p->pending, 0, __ATOMIC_RELEASE);
	warm_done(j->wkey, 'p', pp != 0);
	if (j->warm) { if (pp) warm_release(0, pp); warm_unref(j->vse); warm_unref(j->fse); free(j->p); }   /* (its shaders' last reference releases them) */
}
static int pipeline_pending(const PipeEntry *p) { return p && __atomic_load_n(&p->pending, __ATOMIC_ACQUIRE); }
static PipeEntry pipelines[MAXPIPE]; static int npipelines; static int pipe_head[PIPE_HASH]; static uint64_t pipe_clock; static uint32_t n_pipe_evictions;
static uint32_t pipe_key_hash(const GfxDraw *d, const void *vs, const void *fs, int has_depth, int ckind, int depth_ok, int stencil_ok, int cull_sdl, int dbs) {
	uint64_t h = 1469598103934665603ULL;
#define PH(v) do { h ^= (uint64_t)(v); h *= 1099511628211ULL; } while (0)
	PH((uintptr_t)vs); PH((uintptr_t)fs); PH((uint32_t)d->cull); PH((uint32_t)cull_sdl); PH(d->depth_func); PH((uint32_t)d->depth_write);
	PH(d->stencil_func | (uint32_t)d->stencil_fail << 8 | (uint32_t)d->depth_fail << 16 | (uint32_t)d->depth_pass << 24); PH(d->stencil_cmask | (uint32_t)d->stencil_wmask << 8);
	{ const uint8_t *b = (const uint8_t *)&d->blend; for (size_t i = 0; i < sizeof d->blend; i++) PH(b[i]); }
	PH((uint32_t)has_depth | (uint32_t)ckind << 4 | (uint32_t)depth_ok << 8 | (uint32_t)stencil_ok << 9 | (uint32_t)d->fp_disabled << 10);
	PH((uint8_t)d->depth_bias_factor | (uint32_t)(uint8_t)d->depth_bias_units << 8 | (uint32_t)dbs << 16);
#undef PH
	return (uint32_t)(h ^ (h >> 32));
}
static int batch_uses_pipe(const PipeEntry *p);
/* the least recently used pipeline not being built, out of the table (0: every one is being built) */
static PipeEntry *pipe_evict(void) {
	PipeEntry *v = 0;
	for (int i = 0; i < npipelines; i++) { PipeEntry *p = &pipelines[i]; if (!pipeline_pending(p) && (!v || p->used < v->used)) v = p; }
	if (!v) return 0;
	if (batch_uses_pipe(v)) batch_flush("pipeline evicted");
	int idx = (int)(v - pipelines) + 1;
	for (int *link = &pipe_head[v->hkey % PIPE_HASH]; *link; link = &pipelines[*link - 1].hnext) if (*link == idx) { *link = v->hnext; break; }
	if (v->pipe) SDL_ReleaseGPUGraphicsPipeline(gpu(), v->pipe);
	if (!n_pipe_evictions++) fprintf(stderr, "    gpu: the pipeline table is full (%d): the least recently used ones are released from now on\n", npipelines);
	return v;
}
static SDL_GPUCompareOp conv_cmp(uint32_t f) { static const SDL_GPUCompareOp t[8] = {
	SDL_GPU_COMPAREOP_NEVER, SDL_GPU_COMPAREOP_LESS, SDL_GPU_COMPAREOP_EQUAL, SDL_GPU_COMPAREOP_LESS_OR_EQUAL,
	SDL_GPU_COMPAREOP_GREATER, SDL_GPU_COMPAREOP_NOT_EQUAL, SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, SDL_GPU_COMPAREOP_ALWAYS };
	return t[f & 7]; }
static SDL_GPUStencilOp conv_sop(uint8_t o) { static const SDL_GPUStencilOp t[8] = {
	SDL_GPU_STENCILOP_KEEP, SDL_GPU_STENCILOP_ZERO, SDL_GPU_STENCILOP_REPLACE, SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP,
	SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP, SDL_GPU_STENCILOP_INVERT, SDL_GPU_STENCILOP_INCREMENT_AND_WRAP, SDL_GPU_STENCILOP_DECREMENT_AND_WRAP };
	return t[o & 7]; }
static SDL_GPUBlendFactor conv_factor(uint8_t f) { static const SDL_GPUBlendFactor t[10] = {
	SDL_GPU_BLENDFACTOR_ZERO, SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_SRC_COLOR, SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR,
	SDL_GPU_BLENDFACTOR_SRC_ALPHA, SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_GPU_BLENDFACTOR_DST_COLOR, SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR,
	SDL_GPU_BLENDFACTOR_DST_ALPHA, SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA };
	return f < 10 ? t[f] : SDL_GPU_BLENDFACTOR_ONE; }
/* SceGxmBlendFunc: 0 NONE, 1 ADD, 2 SUBTRACT, 3 REVERSE_SUBTRACT, 4 MIN, 5 MAX -- MIN/MAX take the
 * raw source/destination values on real hardware (SDL_GPU's own doc comment agrees: no factors
 * mentioned), matching gfx_soft.c's blend_apply; the caller forces both factors to ONE for those
 * two so a factor that would otherwise scale them first can't. */
static SDL_GPUBlendOp conv_blendop(uint8_t f) { static const SDL_GPUBlendOp t[6] = {
	SDL_GPU_BLENDOP_ADD, SDL_GPU_BLENDOP_ADD, SDL_GPU_BLENDOP_SUBTRACT, SDL_GPU_BLENDOP_REVERSE_SUBTRACT,
	SDL_GPU_BLENDOP_MIN, SDL_GPU_BLENDOP_MAX };
	return f < 6 ? t[f] : SDL_GPU_BLENDOP_ADD; }

static SDL_GPUShader *null_fragment_shader(void);   /* (below, with the other built-in shaders) */
/* RECOMP_GPU_NULL_FS=0: a colour-masked draw runs its real fragment program behind a zero write mask, as before */
static int null_fs_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_GPU_NULL_FS"); v = !(e && *e && *e == '0'); } return v; }
/* a depth bias's slope term is the factor times the triangle's depth slope per pixel, and at render scale n the GPU's
 * pixels are n times smaller than the Vita's: the same surface has 1/n of the slope per pixel and got 1/n of the bias the
 * game asked for. Killzone's decals (factor -2) at 3x kept the constant term only, enough up close; on distant walls at
 * a glancing angle, where the slope term is most of it, they flickered in and out as the camera moved (2026-10-17). */
static int bias_scale(const GfxDraw *d) { return d->depth_bias_factor ? (scene_sx > scene_sy ? scene_sx : scene_sy) : 0; }
/* the pipeline get_pipeline builds for this draw state and these shaders (the shader recipes' warm-up builds the same outside
 * the table); *null_fs_io: a colour-masked draw's empty fragment shader (null_fs_on), decided here when < 0. The colour
 * target's description goes to *ctd_out: the caller points ci at its own copy (pipeline_build). */
static void pipe_create_info(const GfxDraw *d, const ShaderEntry *vs, const ShaderEntry *fs, int has_depth, int ckind, int depth_ok, int stencil_ok, int cull_sdl, int *null_fs_io,
                             SDL_GPUGraphicsPipelineCreateInfo *ci_out, SDL_GPUColorTargetDescription *ctd_out) {
	const int dbs = bias_scale(d);
	int dbg_noblend = getenv("RECOMP_GPU_DEBUG_NOBLEND") != 0, dbg_nodepth = getenv("RECOMP_GPU_DEBUG_NODEPTH") != 0;   /* TEMP diagnostics */
	SDL_GPUColorTargetBlendState bs = {0};
	int cf = d->blend.color_func, af = d->blend.alpha_func;
	/* SCE_GXM_BLEND_FUNC_NONE (0) on a channel: the source passes through unblended, i.e. ADD with
	 * factors ONE/ZERO -- not the ZERO/ONE factors the blend info happens to carry (gfx_soft.c's
	 * blend_apply has the same rule; Unit 13's SSAO pass is func NONE, mask R) */
	/* TEMP diagnostic (RECOMP_GPU_FETCH_NOBLEND=1): a program that blends itself through FRAGCOLOR gets no fixed-function blend on top */
	int fetch_noblend = fs->fetch && getenv("RECOMP_GPU_FETCH_NOBLEND");
	if (d->blend.present && !dbg_noblend && !fetch_noblend && (cf || af)) {
		bs.enable_blend = true;
		bs.src_color_blendfactor = cf == 0 || cf >= 4 ? SDL_GPU_BLENDFACTOR_ONE : conv_factor(d->blend.color_src);
		bs.dst_color_blendfactor = cf == 0 ? SDL_GPU_BLENDFACTOR_ZERO : cf >= 4 ? SDL_GPU_BLENDFACTOR_ONE : conv_factor(d->blend.color_dst);
		bs.color_blend_op = conv_blendop(cf);
		bs.src_alpha_blendfactor = af == 0 || af >= 4 ? SDL_GPU_BLENDFACTOR_ONE : conv_factor(d->blend.alpha_src);
		bs.dst_alpha_blendfactor = af == 0 ? SDL_GPU_BLENDFACTOR_ZERO : af >= 4 ? SDL_GPU_BLENDFACTOR_ONE : conv_factor(d->blend.alpha_dst);
		bs.alpha_blend_op = conv_blendop(af);
	}
	if (fs->fzero) {   /* a FRAGCOLOR program's blend-unit variant (gfx_gpu_draw: no GXM blend of its own): the module outputs X (and 1 - K), the target supplies the colour it read */
		int c = fs->fzero, lerp = c == USSE_FB_LERP_KEEP || c == USSE_FB_LERP_OVER;
		bs.enable_blend = true; bs.color_blend_op = bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		bs.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		bs.dst_color_blendfactor = lerp ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE;
		bs.src_alpha_blendfactor = c == USSE_FB_LERP_KEEP ? SDL_GPU_BLENDFACTOR_ZERO : SDL_GPU_BLENDFACTOR_ONE;   /* keep: the source alpha is 1 - K, not the target's */
		bs.dst_alpha_blendfactor = c == USSE_FB_ADD_REPLACE ? SDL_GPU_BLENDFACTOR_ZERO : c == USSE_FB_LERP_OVER ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA : SDL_GPU_BLENDFACTOR_ONE;
	}
	/* SceGxmColorMask (A=1 R=2 G=4 B=8) -> SDL_GPUColorComponentFlags (R=1 G=2 B=4 A=8); forced to
	 * nothing when the fragment program itself is disabled (depth/stencil still ran, above) */
	if (d->fp_disabled) { bs.enable_color_write_mask = true; bs.color_write_mask = 0; }
	else if (d->blend.present && (d->blend.mask & 0xf) != 0xf) {
		bs.enable_color_write_mask = true;
		bs.color_write_mask = (uint8_t)((d->blend.mask & 2 ? 1 : 0) | (d->blend.mask & 4 ? 2 : 0) | (d->blend.mask & 8 ? 4 : 0) | (d->blend.mask & 1 ? 8 : 0));
	}
	SDL_GPUColorTargetDescription ctd = { .format = kind_format(ckind), .blend_state = bs };
	SDL_GPUDepthStencilState dss = {0};
	has_depth = has_depth && !dbg_nodepth;
	int dtest = has_depth && depth_ok;
	dss.enable_depth_test = dtest; dss.enable_depth_write = dtest && d->depth_write; dss.compare_op = conv_cmp(d->depth_func);
	/* ALWAYS with keep/keep/keep is a no-op; ALWAYS with a write op (Unit 13 draws its clip masks
	 * that way: always, depth-pass INCR) is not -- skipping those dropped the mask entirely */
	dss.enable_stencil_test = has_depth && stencil_ok && (d->stencil_func != 7 || d->stencil_fail || d->depth_fail || d->depth_pass);
	SDL_GPUStencilOpState sos = { conv_sop(d->stencil_fail), conv_sop(d->depth_pass), conv_sop(d->depth_fail), conv_cmp(d->stencil_func) };
	dss.front_stencil_state = dss.back_stencil_state = sos; dss.compare_mask = d->stencil_cmask; dss.write_mask = d->stencil_wmask;

	/* A draw with its fragment program disabled (sceGxmSetFrontFragmentProgramEnable DISABLED: Uncharted's
	 * ~80 shadow casters and its stencil light volumes a frame, Unit 13's clip masks) writes depth and
	 * stencil only. Its pipeline gets a fragment shader that does nothing instead of the real program
	 * behind the zero colour mask above: no texture lookups or bindings (gfx_gpu_draw's nsmp/fetch gate),
	 * no FRAGCOLOR copy and pass split, and no fragment work on the GPU for pixels that keep nothing.
	 * Not for a program that KILLs or writes depth (DEPTHF): those decide what the depth/stencil pass
	 * does, and the software reference runs them (gfx_soft.c: its no-colour store comes after the shader). */
	int null_fs = *null_fs_io > 0;
	if (*null_fs_io < 0 && d->fp_disabled && null_fs_on()) {
		const UsseProgram *fp = usse_get(d->fprog); const UsseFragLayout *fl = usse_fragment_layout(fp);
		null_fs = !(fl && fl->discard_used) && !usse_program_uses_kill(fp) && !usse_program_uses_depthf(fp) && null_fragment_shader() != 0;
	}
	*null_fs_io = null_fs;
	SDL_GPUGraphicsPipelineCreateInfo ci = {0};   /* (filled here, copied out at the end) */
	ci.vertex_shader = vs->shader; ci.fragment_shader = null_fs ? null_fragment_shader() : fs->shader;
	ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
	/* SceGxmCullMode: gfx_soft.c's record_tri now culls (CULL_CW/CCW against the NDC winding,
	 * independent of the viewport), checked by diffing its own output on Unit 13's frame 596
	 * against itself with culling compiled out -- byte-identical, because an opaque 3D scene's
	 * back faces are already hidden behind its front faces by the depth test, so culling them
	 * doesn't move a pixel there; a real test of the winding *direction* needs a scene where it's
	 * visible. Mapping the same cull_mode here (either BACK-for-CW or the reverse) measurably
	 * changed Unit 13's 596 GPU replay against the software reference it's checked against (its sky
	 * gradient, specifically) -- so the winding convention this pipeline's CCW front_face implies
	 * isn't pinned down yet; left off (as before) rather than guessed at. */
	ci.rasterizer_state.cull_mode = (SDL_GPUCullMode)cull_sdl;   /* resolved by the caller from SceGxmCullMode and the viewport's y flip (see gfx_gpu_draw) */
	ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
	ci.rasterizer_state.enable_depth_clip = false;   /* clamp, not clip: gfx_soft.c (the reference) never discards a fragment for its z being outside [0,w], only for failing the depth test */
	if (d->depth_bias_factor || d->depth_bias_units) {   /* polygon offset (GfxDraw.depth_bias_*): units in the depth format's steps, factor on the slope */
		ci.rasterizer_state.enable_depth_bias = true; ci.rasterizer_state.depth_bias_constant_factor = (float)d->depth_bias_units;
		ci.rasterizer_state.depth_bias_slope_factor = (float)(d->depth_bias_factor * dbs); ci.rasterizer_state.depth_bias_clamp = 0;   /* (bias_scale) */
	}
	ci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
	ci.depth_stencil_state = dss;
	ci.target_info.num_color_targets = 1;   /* (the colour target: the job's copy of ctd, pipeline_build) */
	ci.target_info.has_depth_stencil_target = has_depth; ci.target_info.depth_stencil_format = depth_format();
	*ci_out = ci; *ctd_out = ctd;
}
static uint32_t n_pipes_made;   /* get_pipeline's new entries (a recipe's prebuild counts its own) */
static uint64_t pipe_warm_key(uint64_t vkey, uint64_t fkey, const int32_t v[NPV]) {   /* a pipeline's key in warm.txt: its shaders' and pipe_state's record */
	uint64_t w[NPV + 4]; w[0] = vkey; w[1] = fkey;
	for (int k = 0; k < NPV; k++) w[2 + k] = (uint32_t)v[k];
	w[NPV + 2] = (uint64_t)depth_format(); w[NPV + 3] = (uint64_t)(v[6] && null_fs_on());
	return hash_bytes(w, sizeof w);
}
/* null_fs: -1 decided here (a draw), else its recipe's; low: a shader recipe's prebuild (bq_head[2]) */
static PipeEntry *get_pipeline(const GfxDraw *d, const ShaderEntry *vs, const ShaderEntry *fs, int has_depth, int ckind, int depth_ok, int stencil_ok, int cull_sdl, int null_fs_rec, int low) {
	int dbs = bias_scale(d);
	uint32_t hk = pipe_key_hash(d, vs, fs, has_depth, ckind, depth_ok, stencil_ok, cull_sdl, dbs);
	for (int i = pipe_head[hk % PIPE_HASH]; i; i = pipelines[i - 1].hnext) {
		PipeEntry *p = &pipelines[i - 1];
		if (p->hkey == hk && p->vprog == vs->hash && p->fprog == fs->hash && p->vse == vs && p->fse == fs && p->cull == d->cull && p->cull_sdl == cull_sdl && p->depth_func == d->depth_func && p->depth_write == d->depth_write
		    && p->stencil_func == d->stencil_func && p->stencil_fail == d->stencil_fail && p->depth_fail == d->depth_fail && p->depth_pass == d->depth_pass
		    && p->stencil_cmask == d->stencil_cmask && p->stencil_wmask == d->stencil_wmask   /* the reference is dynamic state, not part of the pipeline */
		    && !memcmp(&p->blend, &d->blend, sizeof(GfxBlend)) && p->has_depth == has_depth && p->ckind == ckind
		    && p->depth_ok == depth_ok && p->stencil_ok == stencil_ok && p->fp_disabled == d->fp_disabled && p->dbf == d->depth_bias_factor && p->dbu == d->depth_bias_units && p->dbs == dbs)
			{
				if (!low) {
					p->used = ++pipe_clock;
					if (p->prebuilt) { p->prebuilt = 0; rc.used_pp++; }
					if (p->low) { p->low = 0; if (pipeline_pending(p)) bq_promote(0, p); }
				}
				return p;
			}
	}
	static int cap = -1; if (cap < 0) { const char *e = getenv("RECOMP_GPU_PIPE_CAP"); cap = e && atoi(e) > 0 && atoi(e) < MAXPIPE ? atoi(e) : MAXPIPE; }   /* (a smaller table to test eviction) */
	PipeEntry *p = npipelines < cap ? &pipelines[npipelines++] : pipe_evict();
	if (!p) return 0;   /* (every entry being built at once: this draw takes the software path) */
	memset(p, 0, sizeof *p); p->hkey = hk; p->used = ++pipe_clock; p->low = p->prebuilt = (uint8_t)low; n_pipes_made++; p->hnext = pipe_head[hk % PIPE_HASH]; pipe_head[hk % PIPE_HASH] = (int)(p - pipelines) + 1;
	p->vprog = vs->hash; p->fprog = fs->hash; p->blend = d->blend;   /* the programs by content: the patcher moves them between frames */ p->cull = d->cull; p->cull_sdl = cull_sdl; p->depth_func = d->depth_func; p->depth_write = d->depth_write;
	p->stencil_func = d->stencil_func; p->stencil_fail = d->stencil_fail; p->depth_fail = d->depth_fail; p->depth_pass = d->depth_pass;
	p->stencil_cmask = d->stencil_cmask; p->stencil_wmask = d->stencil_wmask; p->stencil_ref = d->stencil_ref; p->has_depth = has_depth; p->ckind = ckind;
	p->depth_ok = depth_ok; p->stencil_ok = stencil_ok; p->fp_disabled = d->fp_disabled; p->dbf = d->depth_bias_factor; p->dbu = d->depth_bias_units; p->dbs = (uint8_t)dbs;
	p->smp_units = fs->smp_units; p->nsmp = fs->nsmp; p->fetch = fs->fetch == 1; p->vse = vs; p->fse = fs;

	int null_fs = null_fs_rec; SDL_GPUGraphicsPipelineCreateInfo ci; SDL_GPUColorTargetDescription ctd;
	pipe_create_info(d, vs, fs, has_depth, ckind, depth_ok, stencil_ok, cull_sdl, &null_fs, &ci, &ctd);
	if (null_fs) { p->nsmp = 0; p->smp_units = 0; p->fetch = 0; }   /* gfx_gpu_draw binds nothing and takes no framebuffer copy for it */
	int32_t v[NPV]; pipe_state(d, has_depth, ckind, depth_ok, stencil_ok, cull_sdl, null_fs, v);
	BuildJob bj = { .is_pipe = 1, .low = low, .pj = { p, ci, ctd, d->vprog, d->fprog, us_now(), 0, (ShaderEntry *)vs, null_fs ? 0 : (ShaderEntry *)fs, null_fs, 0, vs->wkey && fs->wkey ? pipe_warm_key(vs->wkey, fs->wkey, v) : 0 } };
	p->pending = 1;
	if (!gpu_async() || !build_enqueue(&bj)) pipeline_build(&bj.pj);
	if (!low) recipe_note_pipe(d, vs, fs, has_depth, ckind, depth_ok, stencil_ok, cull_sdl, null_fs);   /* (its own pipe_state: before pipe_create_info's debug overrides) */
	return p;
}

/* ---------------------------------------------------------------- scene/draw */
static GfxTarget scene_target; static int scene_first_draw, scene_soft;

/* ---------------------------------------------------------------- depth/stencil buffers, per surface address
 * One depth texture per depth/stencil surface address (ds_data; the colour address for an old
 * capture), as gfx_soft.c keeps them: a scene that force-loads its depth gets the texture the
 * scene that stored it left behind, on the GPU, without a round trip through guest memory. When
 * the newest content IS in guest memory (nothing GPU-side has written this surface yet: a capture's
 * recorded buffer, a CPU-rendered scene), the preload passes below write it into the texture. */
typedef struct { uint32_t key, w, h, ds_stride, fmt; uint8_t type, sx, sy, msx, msy; SDL_GPUTexture *tex, *pre_z, *pre_s; int gpu_newer; uint64_t used, gen, zgen; } DepthEntry;   /* ds_stride: the surface's row stride in guest memory (GfxTarget.ds_stride; the texture is w = the colour stride wide); msx, msy: the surface's samples a texel of the texture across and down (depth_samples); gen: write_clock at its last change of any kind, zgen: at its last change of the depth plane (an X8U24 view reads only that); fmt, type: the surface's SceGxmDepthStencilFormat and layout (GfxTarget.ds_format/ds_type), what a view of it decodes */
#define MAXDEPTHG 32
static DepthEntry depthsg[MAXDEPTHG]; static uint64_t depthg_clock;
/* the depth surface's samples per pixel of the scene's depth texture: an MSAA_DOWNSCALE scene draws single-sampled at its colour
 * surface's size, while its depth surface keeps every sample (a 4X target's: 2x2 a pixel, its stride twice the width); a 1X target
 * downscaled into a surface half its size, the same. Killzone reads its 4X 384x192 pass's depth back at sample size, 768x384 */
static void depth_samples(const GfxTarget *t, uint8_t *mx, uint8_t *my) {
	*mx = t->scale_mode && t->width && (uint32_t)t->rt_width * (t->ms_x ? t->ms_x : 1u) >= 2u * t->width ? 2 : 1;
	*my = t->scale_mode && t->height && (uint32_t)t->rt_height * (t->ms_y ? t->ms_y : 1u) >= 2u * t->height ? 2 : 1;
}
static DepthEntry *depth_entry_for(const GfxTarget *t, int sx, int sy, int *fresh) {   /* sx, sy: the scene's target's scale */
	uint32_t key = t->ds_data ? t->ds_data : t->data;
	uint8_t mx, my; depth_samples(t, &mx, &my);
	DepthEntry *slot = 0, *oldest = &depthsg[0];
	for (int i = 0; i < MAXDEPTHG; i++) {
		DepthEntry *d = &depthsg[i];
		if (d->key == key && d->w == t->stride && d->h == t->height && d->sx == sx && d->sy == sy) { d->used = ++depthg_clock; d->fmt = t->ds_format; d->type = t->ds_type; d->msx = mx; d->msy = my; if (fresh) *fresh = 0; return d; }
		if (!d->key && !slot) slot = d;
		if (d->used < oldest->used) oldest = d;
	}
	DepthEntry *d = slot ? slot : oldest;
	if (d->tex) SDL_ReleaseGPUTexture(gpu(), d->tex); if (d->pre_z) SDL_ReleaseGPUTexture(gpu(), d->pre_z); if (d->pre_s) SDL_ReleaseGPUTexture(gpu(), d->pre_s);
	memset(d, 0, sizeof *d); d->key = key; d->w = t->stride; d->h = t->height; d->sx = (uint8_t)sx; d->sy = (uint8_t)sy; d->ds_stride = t->ds_stride ? t->ds_stride : t->stride; d->fmt = t->ds_format; d->type = t->ds_type; d->msx = mx; d->msy = my; d->used = ++depthg_clock; d->gen = d->zgen = ++write_clock;
	SDL_GPUTextureCreateInfo dci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = depth_format(), .usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,   /* SAMPLER: a depth buffer read back as an X8U24 texture is a view of this (target_view) */
		.width = TSX(d, d->w), .height = TSY(d, d->h), .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
	d->tex = SDL_CreateGPUTexture(gpu(), &dci);
	if (fresh) *fresh = 1;
	return d;
}
static SDL_GPUSampler *nearest_sampler(void) {
	static SDL_GPUSampler *smp;
	if (!smp) { SDL_GPUSamplerCreateInfo ci = {0}; ci.min_filter = ci.mag_filter = SDL_GPU_FILTER_NEAREST; ci.address_mode_u = ci.address_mode_v = ci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE; smp = SDL_CreateGPUSampler(gpu(), &ci); }
	return smp;
}
/* the preload: a full-screen triangle whose fragment writes the decoded depth (gl_FragDepth from an
 * R32F texture), then one pass per stencil bit that REPLACEs that bit where an R8 texture has it
 * set (a fragment can't write stencil; the reference value and write mask per pass can). MSL here, SPIR-V
 * from runtime/gfx/shaders/ for Vulkan (builtin_shader). */
static const char *preload_msl =
	"#include <metal_stdlib>\nusing namespace metal;\n"
	"struct VOut { float4 position [[position]]; };\n"
	"vertex VOut main0(uint vid [[vertex_id]]) { float2 p[3] = { float2(-1,1), float2(3,1), float2(-1,-3) }; VOut o; o.position = float4(p[vid], 0, 1); return o; }\n"
	"struct DOut { float d [[depth(any)]]; };\n"
	"fragment DOut main0_depth(VOut in [[stage_in]], texture2d<float> z [[texture(0)]], sampler s [[sampler(0)]]) { DOut o; o.d = z.read(uint2(in.position.xy)).r; return o; }\n"
	"fragment void main0_stencil(VOut in [[stage_in]], texture2d<float> st [[texture(0)]], sampler s [[sampler(0)]], constant uint &bit [[buffer(0)]]) { if ((uint(st.read(uint2(in.position.xy)).r * 255.0f + 0.5f) & bit) == 0u) discard_fragment(); }\n";   /* (the stencil bytes as R8_UNORM: SDL_GPU samples no integer format; x 255 back to the exact byte) */
/* a built-in shader (the preload, the target views): its MSL where the device takes Metal, else its SPIR-V
 * twin from runtime/gfx/shaders/ (vk_shaders.h: the same arithmetic in GLSL, compiled by vk/build.sh) -- Vulkan on
 * Linux, Windows and Android. A change to one side is made to the other. */
static SDL_GPUShader *builtin_shader(const char *msl, const char *msl_entry, const uint32_t *spv, size_t spv_bytes, SDL_GPUShaderStage stage, Uint32 samplers, Uint32 ubufs) {
	SDL_GPUShaderCreateInfo ci = {0};
	if (SDL_GetGPUShaderFormats(gpu()) & SDL_GPU_SHADERFORMAT_MSL) { ci.code = (const Uint8 *)msl; ci.code_size = strlen(msl) + 1; ci.format = SDL_GPU_SHADERFORMAT_MSL; ci.entrypoint = msl_entry; }
	else { ci.code = (const Uint8 *)spv; ci.code_size = spv_bytes; ci.format = SDL_GPU_SHADERFORMAT_SPIRV; ci.entrypoint = "main"; }
	ci.stage = stage; ci.num_samplers = samplers; ci.num_uniform_buffers = ubufs;
	return SDL_CreateGPUShader(gpu(), &ci);
}
static SDL_GPUShader *preload_shader(const char *entry, int fragment, int uniforms) {
	int depth = !strcmp(entry, "main0_depth");
	SDL_GPUShader *sh = builtin_shader(preload_msl, entry, !fragment ? vk_fullscreen_vert : depth ? vk_preload_depth_frag : vk_preload_stencil_frag,
		!fragment ? sizeof vk_fullscreen_vert : depth ? sizeof vk_preload_depth_frag : sizeof vk_preload_stencil_frag,
		fragment ? SDL_GPU_SHADERSTAGE_FRAGMENT : SDL_GPU_SHADERSTAGE_VERTEX, fragment ? 1 : 0, (Uint32)uniforms);
	if (!sh) fprintf(stderr, "    gpu: preload shader %s: %s\n", entry, SDL_GetError());
	return sh;
}
/* the fragment shader of a colour-masked draw (get_pipeline's null_fs): no inputs, no outputs, no
 * work. MSL where the device takes it (compiled once, as preload_msl is), else the translator's empty
 * SPIR-V module (usse_spirv_null_fragment). 0 when neither could be made: the real program is used. */
static SDL_GPUShader *null_fragment_shader(void) {
	static SDL_GPUShader *sh; static int tried;
	if (tried) return sh;
	tried = 1;
	SDL_GPUShaderCreateInfo ci = {0}; ci.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
	SpirvModule m; memset(&m, 0, sizeof m);
	if (SDL_GetGPUShaderFormats(gpu()) & SDL_GPU_SHADERFORMAT_MSL) {
		static const char *null_msl = "#include <metal_stdlib>\nusing namespace metal;\nfragment void main0() {}\n";
		ci.code = (const Uint8 *)null_msl; ci.code_size = strlen(null_msl) + 1; ci.format = SDL_GPU_SHADERFORMAT_MSL; ci.entrypoint = "main0";
	} else {
		m = usse_spirv_null_fragment();
		if (!m.ok) return 0;
		ci.code = (const Uint8 *)m.words; ci.code_size = 4 * (size_t)m.nwords; ci.format = SDL_GPU_SHADERFORMAT_SPIRV; ci.entrypoint = "main";
	}
	sh = SDL_CreateGPUShader(gpu(), &ci);
	if (!sh) fprintf(stderr, "    gpu: null fragment shader: %s (colour-masked draws run their real program)\n", SDL_GetError());
	usse_spirv_free(&m);
	return sh;
}
static SDL_GPUGraphicsPipeline *preload_pipeline(int bit) {   /* -1: the depth pass; 0..7: the stencil-bit pass */
	static SDL_GPUGraphicsPipeline *pipes[9]; static SDL_GPUShader *vs, *fs_d, *fs_s; static int tried;
	if (pipes[bit + 1]) return pipes[bit + 1];
	if (!tried) { tried = 1; vs = preload_shader("main0", 0, 0); fs_d = preload_shader("main0_depth", 1, 0); fs_s = preload_shader("main0_stencil", 1, 1); }
	if (!vs || !fs_d || !fs_s) return 0;
	SDL_GPUGraphicsPipelineCreateInfo ci = {0};
	ci.vertex_shader = vs; ci.fragment_shader = bit < 0 ? fs_d : fs_s;
	ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL; ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
	ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE; ci.rasterizer_state.enable_depth_clip = false;
	ci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
	SDL_GPUDepthStencilState dss = {0};
	if (bit < 0) { dss.enable_depth_test = true; dss.enable_depth_write = true; dss.compare_op = SDL_GPU_COMPAREOP_ALWAYS; }
	else {
		dss.enable_stencil_test = true;
		SDL_GPUStencilOpState sos = { SDL_GPU_STENCILOP_KEEP, SDL_GPU_STENCILOP_REPLACE, SDL_GPU_STENCILOP_KEEP, SDL_GPU_COMPAREOP_ALWAYS };
		dss.front_stencil_state = dss.back_stencil_state = sos; dss.compare_mask = 0xff; dss.write_mask = (Uint8)(1u << bit);
	}
	ci.depth_stencil_state = dss;
	ci.target_info.num_color_targets = 0; ci.target_info.has_depth_stencil_target = true; ci.target_info.depth_stencil_format = depth_format();
	pipes[bit + 1] = SDL_CreateGPUGraphicsPipeline(gpu(), &ci);
	if (!pipes[bit + 1]) fprintf(stderr, "    gpu: preload pipeline %d: %s\n", bit, SDL_GetError());
	return pipes[bit + 1];
}
static void depth_preload_from_guest(DepthEntry *de, const GfxTarget *t) {
	uint32_t n = de->w * de->h;
	float *z = malloc(sizeof(float) * n); uint8_t *st = malloc(n);
	gfx_decode_depth_stencil(t, de->w, de->h, z, st);
	int stbits = 0; for (uint32_t i = 0; i < n; i++) stbits |= st[i];
	if (trace_sync()) fprintf(stderr, "gpu trace: depth preload %#x %ux%u (stencil bits %#x)\n", t->ds_data, de->w, de->h, stbits);
	uint32_t W = TSX(de, de->w), H = TSY(de, de->h);
	if (TSCALED(de)) {   /* render scale: each sample replicated over its block */
		float *zs = malloc(sizeof(float) * W * H); uint8_t *ss = malloc((size_t)W * H);
		for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) { uint32_t i = (y / (uint32_t)de->sy) * de->w + x / (uint32_t)de->sx; zs[y * W + x] = z[i]; ss[y * W + x] = st[i]; }
		free(z); free(st); z = zs; st = ss; n = W * H;
	}
	if (!de->pre_z) {
		SDL_GPUTextureCreateInfo zi = { .type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT, .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = W, .height = H, .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
		de->pre_z = SDL_CreateGPUTexture(gpu(), &zi);
		SDL_GPUTextureCreateInfo si = zi; si.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM; de->pre_s = SDL_CreateGPUTexture(gpu(), &si);   /* (UNORM: SDL_GPU won't sample an integer format -- its debug mode stops there -- and the shaders scale it back) */
	}
	if (!de->pre_z || !de->pre_s || !preload_pipeline(-1)) { free(z); free(st); return; }
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu());
	SDL_GPUTransferBufferCreateInfo xzi = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = 4u * n }, xsi = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = n };
	SDL_GPUTransferBuffer *xz = SDL_CreateGPUTransferBuffer(gpu(), &xzi), *xs = SDL_CreateGPUTransferBuffer(gpu(), &xsi);
	void *mz = SDL_MapGPUTransferBuffer(gpu(), xz, false); memcpy(mz, z, 4u * n); SDL_UnmapGPUTransferBuffer(gpu(), xz);
	void *ms = SDL_MapGPUTransferBuffer(gpu(), xs, false); memcpy(ms, st, n); SDL_UnmapGPUTransferBuffer(gpu(), xs);
	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureTransferInfo srcz = { .transfer_buffer = xz, .pixels_per_row = W, .rows_per_layer = H }, srcs = { .transfer_buffer = xs, .pixels_per_row = W, .rows_per_layer = H };
	SDL_GPUTextureRegion dz = { .texture = de->pre_z, .w = W, .h = H, .d = 1 }, ds = { .texture = de->pre_s, .w = W, .h = H, .d = 1 };
	SDL_UploadToGPUTexture(cp, &srcz, &dz, false); SDL_UploadToGPUTexture(cp, &srcs, &ds, false);
	SDL_EndGPUCopyPass(cp);
	SDL_GPUViewport vp = { 0, 0, (float)W, (float)H, 0, 1 }; SDL_Rect sc = { 0, 0, (int)W, (int)H };
	/* pass 1: depth from the texture (the pass clears depth and stencil first: every pixel is rewritten) */
	SDL_GPUDepthStencilTargetInfo dti = { .texture = de->tex, .clear_depth = t->ds_background_depth, .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE,
		.stencil_load_op = SDL_GPU_LOADOP_CLEAR, .stencil_store_op = SDL_GPU_STOREOP_STORE, .clear_stencil = 0 };
	SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, 0, 0, &dti);
	SDL_BindGPUGraphicsPipeline(rp, preload_pipeline(-1)); SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
	SDL_GPUTextureSamplerBinding tb = { de->pre_z, nearest_sampler() }; SDL_BindGPUFragmentSamplers(rp, 0, &tb, 1);
	SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0); SDL_EndGPURenderPass(rp);
	/* passes 2..: each stencil bit any pixel has */
	for (int b = 0; b < 8; b++) {
		if (!(stbits & (1 << b)) || !preload_pipeline(b)) continue;
		SDL_GPUDepthStencilTargetInfo sti = { .texture = de->tex, .load_op = SDL_GPU_LOADOP_LOAD, .store_op = SDL_GPU_STOREOP_STORE, .stencil_load_op = SDL_GPU_LOADOP_LOAD, .stencil_store_op = SDL_GPU_STOREOP_STORE };
		rp = SDL_BeginGPURenderPass(cmd, 0, 0, &sti);
		SDL_BindGPUGraphicsPipeline(rp, preload_pipeline(b)); SDL_SetGPUViewport(rp, &vp); SDL_SetGPUScissor(rp, &sc);
		SDL_SetGPUStencilReference(rp, (Uint8)(1u << b));
		SDL_GPUTextureSamplerBinding sb = { de->pre_s, nearest_sampler() }; SDL_BindGPUFragmentSamplers(rp, 0, &sb, 1);
		uint32_t bit = 1u << b; SDL_PushGPUFragmentUniformData(cmd, 0, &bit, sizeof bit);
		SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0); SDL_EndGPURenderPass(rp);
	}
	SDL_SubmitGPUCommandBuffer(cmd);
	SDL_ReleaseGPUTransferBuffer(gpu(), xz); SDL_ReleaseGPUTransferBuffer(gpu(), xs);
	free(z); free(st);
}
static DepthEntry *scene_depth_entry;
static void apply_render_scale(int n);   /* below, with present */
/* a depth-only scene (no colour surface: Unit 13's cascaded shadow atlas, ~150 draws into a 1024x1024
 * D16 surface every frame) renders here like any other, into its depth texture, with this scratch
 * colour attachment for the render pass and pipelines. It is no guest surface (addr 0): never
 * uploaded, downloaded or viewed; outside targets[], so no lookup by address finds it. Handed to
 * the software path, the scene ran every shadow caster's vertex program on the CPU interpreter
 * (~70% of the render thread in level) and, once those scenes rasterised, its triangles too
 * (9 fps); the pass that samples the atlas reads the depth texture through a view (target_view). */
static TargetEntry scratch_entry;
static TargetEntry *scratch_target(const GfxTarget *t) {
	TargetEntry *e = &scratch_entry;
	uint8_t sx, sy; target_scale(t->width, t->height, &sx, &sy);
	if (!e->color || e->width != t->stride || e->height != t->height || e->sx != sx || e->sy != sy) {
		if (e->color) SDL_ReleaseGPUTexture(gpu(), e->color);
		memset(e, 0, sizeof *e); e->width = t->stride; e->height = t->height; e->kind = TK_U8; e->sx = sx; e->sy = sy;
		SDL_GPUTextureCreateInfo tci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = kind_format(e->kind), .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
			.width = TSX(e, e->width), .height = TSY(e, e->height), .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
		e->color = SDL_CreateGPUTexture(gpu(), &tci);
		if (!e->color) { fprintf(stderr, "    gpu: depth-only scene %ux%u: %s\n", e->width, e->height, SDL_GetError()); return 0; }
	}
	e->used = ++target_clock; e->depth = 0;
	return e;
}
/* Render targets that share memory under another shape or format: on the console it is one surface,
 * so a scene starting on one sees what the other drew last. Here each is a GPU texture of its own,
 * and a target this cache met for the first time started from guest memory, which the GPU had never
 * written back. Uncharted with gamma on: its tonemap pass writes the final 720x408 image as an sRGB
 * surface (format 0x1000) and its bullet-tracer / climb-highlight pass draws over the same memory as
 * a linear one (format 0) -- two textures, and the second still held an old frame (seen in testing: the
 * opening cutscene flashed up under every tracer and ledge highlight, 2026-10-09).
 *
 * At a scene's start, each overlapping target whose GPU content is newer than this one's hands it
 * over, in write order:
 *  - the same surface under another format with the same bytes per pixel (addr, stride, height,
 *    layout and texel size equal): a GPU texture-to-texture copy -- the bytes are the guest's either
 *    way (sRGB and linear RGBA8 store the same bytes). No CPU round trip, render scale kept.
 *  - a different pixel size (Uncharted's 64-bit F32F32 G-buffer and its 32-bit final image share
 *    memory; each scene begins with a full-screen opaque pass): nothing. The other's bits read as this
 *    format are meaningless, and the round trip they cost -- the CPU waiting for the GPU to finish the
 *    frame so far, twice a frame, plus per-pixel float conversion both ways -- took Uncharted from a
 *    locked 60 to ~40 fps (2026-10-09, the first version of this did it for every pair).
 *  - anything else with the same pixel size (another stride or layout over the same bytes): down to
 *    guest memory and this one reloads from there, as before.
 * RECOMP_GPU_NO_ALIAS_SYNC=1: none of it; RECOMP_GPU_ALIAS_LOG=1: each hand-over logged. */
static uint32_t target_bytes(const TargetEntry *e) { GfxTarget t = target_entry_as_gfx(e); return (uint32_t)gfx_target_bpp(&t) * e->width * e->height; }
static int target_bpp_entry(const TargetEntry *e) { GfxTarget t = target_entry_as_gfx(e); return gfx_target_bpp(&t); }
static void target_alias_sync(TargetEntry *e) {
	static int off = -1, log = -1; if (off < 0) { off = getenv("RECOMP_GPU_NO_ALIAS_SYNC") != 0; log = getenv("RECOMP_GPU_ALIAS_LOG") != 0; }
	if (off || !e || !e->addr) return;
	uint32_t lo = e->addr, hi = e->addr + target_bytes(e);
	int bpp = target_bpp_entry(e);
	TargetEntry *list[MAXTARGET]; int n = 0;
	for (int i = 0; i < ntargets; i++) {
		TargetEntry *o = &targets[i];
		if (o == e || !o->addr || !o->color || !o->gpu_dirty) continue;
		uint32_t olo = o->addr, ohi = o->addr + target_bytes(o);
		if (!(olo < hi && ohi > lo && (e->cpu_dirty || o->gen > e->gen))) continue;
		if (target_bpp_entry(o) != bpp) {
			if (log) fprintf(stderr, "    alias skip: scene %#x fmt %#x %ux%u, %#x fmt %#x %ux%u has another pixel size\n", e->addr, e->format, e->width, e->height, o->addr, o->format, o->width, o->height);
			continue;
		}
		list[n++] = o;
	}
	if (!n) return;
	for (int a = 1; a < n; a++) for (int b = a; b > 0 && list[b - 1]->gen > list[b]->gen; b--) { TargetEntry *x = list[b]; list[b] = list[b - 1]; list[b - 1] = x; }
	int via_guest = 0;
	for (int k = 0; k < n; k++) {
		TargetEntry *o = list[k];
		int same = o->addr == e->addr && o->width == e->width && o->height == e->height && o->type == e->type && kind_bytes(o->kind) == kind_bytes(e->kind) && e->color && o->sx == e->sx && o->sy == e->sy;
		if (log) fprintf(stderr, "    alias %s: scene %#x fmt %#x %ux%u <- %#x fmt %#x %ux%u (frame %llu)\n", same ? "copy" : "sync", e->addr, e->format, e->width, e->height,
		                 o->addr, o->format, o->width, o->height, (unsigned long long)gpu_frame_no);
		if (same) {   /* on the GPU: the whole texture, at the render scale */
			batch_flush("alias copy");   /* (returns at once when nothing is batched) */
			SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu()); SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
			SDL_GPUTextureLocation src = { .texture = o->color }, dst = { .texture = e->color };
			SDL_CopyGPUTextureToTexture(cp, &src, &dst, TSX(e, e->width), TSY(e, e->height), 1, false);
			SDL_EndGPUCopyPass(cp); SDL_SubmitGPUCommandBuffer(cmd);
			e->cpu_dirty = 0; e->gpu_dirty = 1; e->gen = ++write_clock; via_guest = 0;
		} else { sync_why = "(another target over the same memory begins a scene)"; target_download_to_guest(o); via_guest = 1; }
	}
	if (via_guest) e->cpu_dirty = 1;   /* the last hand-over went through guest memory: reload from there before the first draw */
	e->alias_in = 1;
}
/* A draw that samples the part of its own scene's target it is drawing reads, on the Vita, that target as the scene
 * began: the hardware renders a scene tile by tile after the game has submitted all of it, and a tile reaches memory
 * only when the tile is done -- the pixels under a draw are, as a texture, whatever memory held before the scene. Read
 * as rendered so far (a view of the GPU texture, or a download of it), they were the scene's earlier draws.
 * Killzone's reprojection strip (the bottom 384x64 of its 384x192 target: draw 1 fills the target with a uniform
 * colour, draw 2 copies the previous frame's strip back in, draw 3 blends the new low-res scene into it and positions
 * its mesh by sampling it) copied draw 1's fill instead of last frame's strip every frame -- a flat marker colour
 * (0, 0, 0.5, 0.25) everywhere the new scene didn't land, the dark blue that its guns, metal and glass reflect (they
 * sample the strip's downscaled chain), flickering as the two halves of each frame alternated (2026-10-17).
 * Such a read takes the guest-memory path without the scene's own target downloaded: at the start of each scene on a
 * target seen read that way (TargetEntry.self_read), the target goes to guest memory once, and memory holds the scene's
 * starting pixels for all its draws. Reads of other parts of the target keep the target as rendered so far, as the
 * tile order makes them on the console for regions finished earlier (Killzone's bloom atlas reads its earlier levels
 * in the same scene). "The part it is drawing": the texture's rectangle covers at least half of the draw's clip rect. */
static int scene_snap;                               /* the current scene's starting pixels: 1 in guest memory, 2 in its GPU snapshot (target_snapshot) */
static int cur_wx0, cur_wy0, cur_wx1, cur_wy1;       /* the draw being recorded: its clip rect, target pixels */
/* The starting pixels on the GPU instead (RECOMP_GPU_SNAP=1; off by default, with RECOMP_GPU_TRANSFER): a copy of the target taken as
 * the scene begins, which those reads sample (target_view: a view or mip_view of the snapshot). The download made Killzone's
 * strip scene a fence wait on the GPU every frame, 4.3 ms on the RTX laptop. Anything that can only read the starting pixels
 * from guest memory gets the snapshot downloaded there once (snap_to_guest). */
static void target_sync_for_gpu(TargetEntry *e);   /* (below) */
static int gpu_snap_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_GPU_SNAP"); v = e && *e && *e != '0'; } return v; }
static int target_snapshot(TargetEntry *e) {
	static uint64_t snaps;
	target_sync_for_gpu(e);   /* (guest memory newer after a CPU write: up first, as a scene starting on it would) */
	if (!e->snap) {
		SDL_GPUTextureCreateInfo tci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = kind_format(e->kind), .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
			.width = TSX(e, e->width), .height = TSY(e, e->height), .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
		if (!(e->snap = SDL_CreateGPUTexture(gpu(), &tci))) return 0;
	}
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu());
	if (!cmd) return 0;
	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureLocation from = { .texture = e->color }, to = { .texture = e->snap };
	SDL_CopyGPUTextureToTexture(cp, &from, &to, TSX(e, e->width), TSY(e, e->height), 1, false);
	SDL_EndGPUCopyPass(cp);
	SDL_SubmitGPUCommandBuffer(cmd);   /* (after the scenes that drew it, before this one's draws: both in submission order) */
	e->snap_gen = ++snaps | (1ull << 63);   /* (never a TargetEntry.gen: a view of the live target and one of a snapshot never pass for each other) */
	return 1;
}
static void snap_to_guest(TargetEntry *e) {   /* the guest-memory path needs the starting pixels there: the snapshot, downloaded (once a scene) */
	SDL_GPUTexture *c = e->color; e->color = e->snap; sync_why = "(scene start, for a read the GPU snapshot can't serve)"; target_download_to_guest(e); e->color = c;
	e->gpu_dirty = 1;   /* (the target itself has drawn on since) */
	scene_snap = 1;
}
static int same_region_read(const GfxTexture *tex) {
	if (!scene_entry || scene_entry == &scratch_entry || !tex || !tex->data) return 0;
	GfxTarget tg = target_entry_as_gfx(scene_entry);
	uint32_t bpp = (uint32_t)gfx_target_bpp(&tg), pitch = bpp * scene_entry->width;
	if (!bpp || !pitch || tex->data < scene_entry->addr || tex->data >= scene_entry->addr + pitch * scene_entry->height) return 0;
	uint32_t off = tex->data - scene_entry->addr;
	int ty0 = (int)(off / pitch), tx0 = (int)((off % pitch) / bpp), tx1 = tx0 + tex->width, ty1 = ty0 + tex->height;
	int ix0 = tx0 > cur_wx0 ? tx0 : cur_wx0, iy0 = ty0 > cur_wy0 ? ty0 : cur_wy0, ix1 = tx1 < cur_wx1 ? tx1 : cur_wx1, iy1 = ty1 < cur_wy1 ? ty1 : cur_wy1;
	if (ix1 <= ix0 || iy1 <= iy0) return 0;
	long inter = (long)(ix1 - ix0) * (iy1 - iy0), warea = (long)(cur_wx1 - cur_wx0) * (cur_wy1 - cur_wy0);
	return warea > 0 && 2 * inter >= warea;
}
/* ---- the HUD at its native shape on a wider aspect ratio (settings "hud_layout": edges | 16:9 | stretch). The game's camera
 * hook widens the 3D view k times (window_aspect_widen), but its 2D elements are still laid out for 960 x 544 and the wider
 * frame stretched them k times. A HUD draw -- in a scene whose target has been presented (the display buffer), of the kind
 * the game's RECOMP_HUD_DRAWS names (below), under 90% of the screen wide and tall (the full-screen composite and backgrounds stay as
 * they are: Uncharted's menus draw their backdrop as two full-height half-screen quads, which as HUD made one centred group
 * that took the whole menu with it, narrowed into the middle -- on an ultrawide, 2026-10-23; where it lands: its own
 * vertex program run on the CPU, gfx_draw_clip_bounds) -- gets a viewport k times
 * narrower, placed so one point keeps its place:
 * edges, the screen edge of its third (left/right elements keep their pixel distance from that edge, middle ones from the
 * centre: the minimap stays in its corner, the crosshair and subtitles centred); 16:9, the centre for all (the HUD as a
 * centred native-width frame). Its shape is then the game's own, in native pixels. A draw falling back to the software path
 * keeps the stretch. Off at the native aspect. */
static volatile uint32_t presented_ring[4], presented_n;
static int scene_hud;               /* this scene's target has been presented: its 2D draws are the HUD */
static int hud_mode_cur = 2;        /* 0 stretch, 1 16:9, 2 edges: settings "hud_layout", read at each present */
static uint32_t n_hud_draws; static uint64_t t_hud;
static void hud_note_present(uint32_t base) {
	for (int i = 0; i < 4; i++) if (presented_ring[i] == base) goto mode;
	presented_ring[presented_n++ & 3] = base;
mode:;
	char b[16]; settings_str("hud_layout", "edges", b, sizeof b);
	hud_mode_cur = !strcmp(b, "stretch") ? 0 : !strcmp(b, "16:9") ? 1 : 2;
}
void gfx_gpu_hud_note_present(uint32_t base) { hud_note_present(base); }   /* gfxreplay: a one-frame capture presents after its scenes, so they are noted up front */
static int hud_scene(uint32_t addr) { for (int i = 0; i < 4; i++) if (addr && presented_ring[i] == addr) return 1; return 0; }
/* what drew into each displayed buffer since its last present (F5's frame history, gfx_gpu_present): draws recorded, those the
 * HUD layout took, those skipped while their pipeline built, those on the software path */
enum { DISP_DRAWS, DISP_HUD, DISP_SKIPPED, DISP_SOFT };
static struct { uint32_t base, n[4]; } disp_count[4];
static void disp_note(int what) {
	if (!scene_hud) return;
	const uint32_t a = scene_target.data; int k = 0;
	for (int i = 0; i < 4; i++) { if (disp_count[i].base == a) { k = i; goto hit; } if (!disp_count[i].base) k = i; }
	memset(&disp_count[k], 0, sizeof disp_count[k]); disp_count[k].base = a;
hit:
	disp_count[k].n[what]++;
}
/* RECOMP_HUD_DRAWS (a game's RUN_ENV): which draws of the presented scene are its HUD -- "2d" those whose every vertex has
 * w = 1, "scene" every draw that isn't full-screen, 3D ones too (Killzone: its minimap is a small tilted 3D map; the scene
 * holds nothing else after the composite); unset, none: a game whose 3D world draws straight into the display buffer would
 * have its world squeezed */
static int hud_draws(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_HUD_DRAWS"); v = !e ? 0 : !strcmp(e, "scene") ? 2 : !strcmp(e, "2d") ? 1 : 0; } return v; }
int gfx_gpu_hud_supported(void) { return hud_draws() != 0; }
static int hud_classify(const GfxDraw *d, float out[4]) {   /* 1: a HUD draw, its NDC extent in out */
	if (window_aspect_widen() < 1.01f || !hud_mode_cur || !hud_draws()) return 0;
	uint64_t t0 = us_now(); float b[6];
	int ok = gfx_draw_clip_bounds(d, 16, b), hud = ok && (hud_draws() == 2 || (b[4] > 0.999f && b[5] < 1.001f)) && b[1] - b[0] < 1.8f && b[3] - b[2] < 1.8f;   /* (full height: a backdrop, see above) */
	t_hud += us_now() - t0;
	static int log = -1; if (log < 0) log = getenv("RECOMP_HUD_LOG") != 0;
	if (log) fprintf(stderr, "    hud: vprog %#x %u verts %s x %.3f..%.3f y %.3f..%.3f w %.3f..%.3f\n", d->vprog, d->count, !ok ? "no bounds" : hud ? "HUD" : "not HUD", b[0], b[1], b[2], b[3], b[4], b[5]);
	if (hud) memcpy(out, b, 4 * sizeof(float));
	return hud;
}
/* ---- screen-space ambient occlusion (ssao.c) on a game's main 3D scene, named by its RUN_ENV: RECOMP_SSAO=<colour
 * format>:<vertex uniform buffer>:<float offset> -- the scene whose colour surface has that SceGxmColorFormat (gamma bits
 * aside), with a depth surface, at least 30 draws; its camera's projection matrix (column-major) at that float offset of
 * that vertex uniform buffer of its draws (a perspective one: P[2][3] = -1, P[3][3] = 0), with its viewport's z mapping, the
 * one most of the scene's draws use (ao_note_draw) -- read from the game every frame, so a field-of-view or widescreen hook
 * moves it too.
 * Killzone: 0x1000000 (its F16 HDR scene), buffer 1 (_ngp_view) at float 24: an infinite far plane, near 0.02. Applied at
 * the scene's end, before anything samples it. Off unless named, and with settings "ssao" 0. */
static struct { int parsed, on, ub, off; uint32_t fmt; } ao_spec;
static int ao_scene, ao_draws; static uint32_t n_ao;
static struct { SsaoCamera cam; int n; } ao_cand[4]; static int ao_ncand;   /* the scene's cameras by viewport z mapping, with how many draws use each */
static int ao_spec_ok(void) {
	if (!ao_spec.parsed) {
		ao_spec.parsed = 1; const char *e = getenv("RECOMP_SSAO"); unsigned f; int ub, off;
		if (e && sscanf(e, "%x:%d:%d", &f, &ub, &off) == 3 && ub >= 0 && ub < 14 && off >= 0) { ao_spec.on = 1; ao_spec.fmt = f; ao_spec.ub = ub; ao_spec.off = off; }
	}
	return ao_spec.on;
}
int gfx_gpu_ssao_supported(void) { return ao_spec_ok(); }
/* each draw with a perspective projection in the named buffer votes for its camera -- the projection and its viewport's
 * z mapping. The one most draws use is the world's: Killzone draws its first-person weapon with the same projection
 * squeezed into the front tenth of the depth range (z mapping 0.05/0.05, 6 draws, against the world's 0.5/0.5, ~170), and
 * the weapon comes first */
static void ao_note_draw(const GfxDraw *d) {
	ao_draws++;
	if (!d->vubuf[ao_spec.ub]) return;
	float P[16]; const uint32_t a = d->vubuf[ao_spec.ub] + 4u * (uint32_t)ao_spec.off;
	for (int i = 0; i < 16; i++) { uint32_t w = RD32(a + 4u * (uint32_t)i); memcpy(&P[i], &w, 4); }
	if (!(fabsf(P[11] + 1.0f) < 1e-4f) || P[15] != 0.0f || !(P[0] > 0) || !(P[5] > 0) || !isfinite(P[10]) || !isfinite(P[14]) || P[14] == 0.0f) return;
	const int vz = d->viewport_set && !d->viewport_disabled;
	const SsaoCamera c = { P[0], P[5], P[10], P[14], vz ? d->vp_zoff : 0.0f, vz && d->vp_zscale != 0.0f ? d->vp_zscale : 1.0f };
	for (int k = 0; k < ao_ncand; k++) if (ao_cand[k].cam.zoff == c.zoff && ao_cand[k].cam.zscale == c.zscale) { ao_cand[k].n++; return; }
	if (ao_ncand < 4) { ao_cand[ao_ncand].cam = c; ao_cand[ao_ncand].n = 1; ao_ncand++; }
}
static const SsaoCamera *ao_camera(void) {   /* the scene's world camera: the most-used z mapping's */
	int best = -1; for (int k = 0; k < ao_ncand; k++) if (best < 0 || ao_cand[k].n > ao_cand[best].n) best = k;
	if (best < 0) return 0;
	static int logn; if (getenv("RECOMP_SSAO_LOG") && logn++ < 10) for (int k = 0; k < ao_ncand; k++) fprintf(stderr, "    ssao camera%s: %d draws: P00 %g P11 %g P22 %g P32 %g, z %g + %g * z/w\n", k == best ? " (used)" : "", ao_cand[k].n,
		ao_cand[k].cam.p00, ao_cand[k].cam.p11, ao_cand[k].cam.p22, ao_cand[k].cam.p32, ao_cand[k].cam.zoff, ao_cand[k].cam.zscale);
	return &ao_cand[best].cam;
}
void gfx_gpu_begin_scene(const GfxTarget *t) {
	scene_open = 1;
	gfx_soft_use_interp();   /* the CPU fallback and check reference run the real shaders */
	render_scale_init();
	/* a requested render scale (F2) applies here, between scenes: present comes from the display
	 * thread while the render thread may already have the next frame's first scene open (both games
	 * begin it before the flip), and dropping every surface there left that scene drawing into a
	 * released target */
	{ int req = __atomic_exchange_n(&rscale_req, 0, __ATOMIC_RELAXED); if (req && req != rscale && gpu()) { batch_flush("render scale change"); apply_render_scale(req); }
	  else if (gpu() && want_rscale_x(rscale) != rscale_x) { batch_flush("aspect change"); apply_render_scale(rscale); } }   /* the aspect ratio setting moved: the targets' width with it */
	{ int a = aniso_want(); if (a != aniso_cur && gpu()) {   /* the anisotropy setting (first scene, or changed): samplers rebuilt */
		if (aniso_cur >= 0) { batch_flush("anisotropy change"); for (int i = 0; i < ntextures; i++) tex_release_samplers(&textures[i]); views_release_samplers(); }
		aniso_cur = a; } }
	if (!gpu()) {   /* no device (creation failed, or several replays racing for one): every scene renders on the software path -- say so once, a silent switch mis-measured GPU parity for an afternoon */
		static int said; if (!said) { said = 1; fprintf(stderr, "gpu: no GPU device -- rendering on the software path\n"); }
		scene_soft = 1;   /* so gfx_gpu_end_scene ends it: left 0, the scene was never flushed and a RECOMP_REPLAY_DRAWS dump of it lacked every draw (gfxbisect.py's software side read 6.92% for Shovel Knight's 0.00) */
		gfx_soft_begin_scene(t); return;
	}
	batch_flush("scene begun without an end");   /* normally empty: gfx_gpu_end_scene flushed (targets and depth surfaces may be evicted below) */
	vis_complete(0);   /* visibility counters the GPU has finished: into guest memory */
	scene_target = *t;
	scene_hud = hud_scene(t->data);
	static int no_depth_only = -1; if (no_depth_only < 0) no_depth_only = getenv("RECOMP_GPU_NO_DEPTH_ONLY") != 0;   /* RECOMP_GPU_NO_DEPTH_ONLY=1: depth-only scenes on the software path, as before */
	scene_entry = t->data ? get_target(t) : t->depth && t->ds_data && t->width && !no_depth_only ? scratch_target(t) : 0;
	scene_first_draw = 1;
	scene_soft = !scene_entry;
	scene_depth_entry = 0;
	scene_sx = scene_entry ? scene_entry->sx : rscale_x; scene_sy = scene_entry ? scene_entry->sy : rscale;   /* (the target's own scale: target_scale) */
	if (scene_soft) { gfx_soft_begin_scene(t); return; }   /* gfx_gpu_draw hands every draw of this scene to the software path */
	scene_entry->alias_in = 0;
	if (scene_entry != &scratch_entry) target_alias_sync(scene_entry);
	if (scene_entry->drawn_frame != gpu_frame_no + 1) { scene_entry->prev_drawn_frame = scene_entry->drawn_frame; scene_entry->drawn_frame = gpu_frame_no + 1; }   /* (+1: 0 = never) */
	scene_snap = 0;
	if (scene_entry != &scratch_entry && scene_entry->self_read) {   /* (same_region_read) */
		if (gpu_snap_on() && scene_entry->gpu_dirty && target_snapshot(scene_entry)) scene_snap = 2;
		else { if (scene_entry->gpu_dirty) { sync_why = "(scene start: read in its own scene)"; target_download_to_guest(scene_entry); } scene_snap = 1; }
	}
	scene_entry->depth = scene_entry->own_depth;   /* (never a texture a previous scene borrowed from the depth table: it may be gone) */
	if (t->depth) {   /* the depth/stencil buffer by its own address, shared between the scenes that use it */
		int fresh = 0; DepthEntry *de = depth_entry_for(t, scene_entry->sx, scene_entry->sy, &fresh);
		if (de && de->tex) {
			scene_depth_entry = de; scene_entry->depth = de->tex;
			if (t->ds_data && t->ds_force_load) {
				static int no_preload = -1; if (no_preload < 0) no_preload = getenv("RECOMP_GPU_NO_DEPTH_PRELOAD") != 0;
				/* force-load: the surface as it stands -- on the GPU if a GPU scene wrote it last, else from guest memory */
				if ((fresh || !de->gpu_newer) && !no_preload) { depth_preload_from_guest(de, t); de->gen = de->zgen = ++write_clock; }
			}
			/* (no force-load: the first draw's render pass clears it to the background values, as before) */
		}
	}
	if (scene_entry == &scratch_entry && !scene_depth_entry) { scene_entry = 0; scene_soft = 1; gfx_soft_begin_scene(t); }   /* a depth-only scene is its depth texture */
	ao_draws = ao_ncand = 0;
	ao_scene = ao_spec_ok() && scene_entry && scene_entry != &scratch_entry && scene_depth_entry && (t->format & ~0x3000u) == ao_spec.fmt && ssao_enabled();   /* (ambient occlusion, above) */
}
/* guest memory [addr, addr+len) was written behind the GPU's back (gfxreplay restoring a capture's
 * memory records at a scene's start; a game writing into a surface with the CPU): the overlapping
 * colour targets re-upload from guest memory before their next GPU use, and the overlapping
 * depth/stencil surfaces preload from it again -- what gfx_soft.c sees by reading memory directly */
/* ---- CPU readback. A small render target no GPU draw ever samples is one the game reads with the
 * CPU: Unit 13 and Uncharted reduce the HDR frame to a 32x32 F32 luminance target (Unit 13 cycles
 * three, 4 KB apart, reading each a few frames later) and compute the tonemap exposure from it on the
 * CPU. The GPU backend leaves a target on the GPU until something on the host side reads it, so the
 * game read whatever guest memory last held there -- its exposure followed stale or never-written
 * luminance: Uncharted's surfaces far too dark, Unit 13 blown out with bloom at times (the software
 * path, writing guest memory itself, never had this). At the end of such a target's scene its texels
 * are copied down without waiting (its own command buffer and fence) and written to guest memory at
 * the next present, or before a guest write over them -- by then the GPU has long finished that scene.
 * A small FLOAT target is read back even when GPU draws sample it too: Uncharted's hold-it-to-the-light
 * parchments accumulate how much light each point of the paper got in a 160x120 F32 target the GPU
 * ping-pongs (so it is sampled) and the CPU counts its lit texels for the "% complete" (fn_813bdbfa) --
 * with it left out the symbols showed through the paper and the percentage never moved (2026-10-13). A
 * landed copy never marks the target CPU-written: the GPU's stays the one draws use.
 * RECOMP_GPU_READBACK_MAX=pixels (default 32768, 0 = off): the largest target treated this way. */
typedef struct { TargetEntry *e; GfxTarget t; int kind; uint64_t gen; SDL_GPUTransferBuffer *xb; SDL_GPUFence *fence; uint64_t frame; int pinned; } Readback;   /* frame: gpu_frame_no when enqueued; pinned: the presenting thread holds the fence for a wait outside the lock (gfx_gpu_readbacks_due) */
#define MAXREADBACK 16
static Readback readbacks[MAXREADBACK]; static int nreadbacks;
static SDL_GPUFence *parked[MAXREADBACK]; static int nparked;   /* fences of pinned readbacks another thread completed: the presenting thread releases them at its present, after its wait */
static uint32_t n_readbacks;   /* RECOMP_GPU_STATS: this frame's */
static void readback_complete(Readback *r) {
	{ uint64_t t0 = us_now(); SDL_WaitForGPUFences(gpu(), true, &r->fence, 1); t_readback_wait += us_now() - t0; n_readback_waits++; }
	if (r->pinned) { if (nparked < MAXREADBACK) parked[nparked++] = r->fence; }   /* (never freed under a lock-free waiter; at most MAXREADBACK can be pinned between presents) */
	else SDL_ReleaseGPUFence(gpu(), r->fence);
	uint32_t npx = r->t.stride * r->t.height;
	const int raw = (r->kind == TK_U8 || r->kind == TK_U8S) && r->t.stride == r->t.width && gfx_target_plain_rgba8(&r->t);   /* (target_raw_ok: the bytes as they are) */
	const uint32_t gbytes = (uint32_t)gfx_target_bpp(&r->t) * npx;
	float *f = raw ? 0 : malloc(16u * npx);
	const uint8_t *map = SDL_MapGPUTransferBuffer(gpu(), r->xb, false);
	if (map) {
		if (raw) { gwatch_write_begin(r->t.data, gbytes); memcpy(G(r->t.data), map, gbytes); gwatch_write_end(r->t.data, gbytes); }
		else texels_to_floats(r->kind, map, f, npx);
		SDL_UnmapGPUTransferBuffer(gpu(), r->xb);
	}
	SDL_ReleaseGPUTransferBuffer(gpu(), r->xb);
	if (map) {
		if (!raw) { gwatch_write_begin(r->t.data, gbytes); gfx_encode_target_f32(&r->t, f); gwatch_write_end(r->t.data, gbytes); }
		textures_invalidate(r->t.data, r->t.data + gbytes);
		if (r->e->addr == r->t.data && r->e->gen == r->gen) r->e->gpu_dirty = 0;   /* (still the same target, nothing drawn into it since) */
		if (trace_sync()) fprintf(stderr, "gpu trace: readback of target %#x %ux%u written to guest memory\n", r->t.data, r->t.stride, r->t.height);
	}
	free(f);
}
static void readbacks_complete(uint32_t lo, uint32_t hi) {   /* the pending ones over [lo, hi) (all: 0, ~0u) */
	int k = 0;
	for (int i = 0; i < nreadbacks; i++) {
		Readback *r = &readbacks[i]; uint32_t rhi = r->t.data + (uint32_t)gfx_target_bpp(&r->t) * r->t.stride * r->t.height;
		if (r->t.data < hi && rhi > lo) readback_complete(r); else readbacks[k++] = *r;
	}
	nreadbacks = k;
}
/* At present: the pending readbacks land if the GPU has finished them (a fence query, no wait), or are made
 * to land once they are RECOMP_GPU_READBACK_AGE presents old (default 1: the previous frame's; 2 was measured
 * too, see the end of this comment) -- never a wait on the current frame's GPU work. Waiting for them all here, as before, held the display thread (and through the renderer
 * lock the render thread, already recording the next frame) until the GPU had finished the scene that drew
 * them: 5-19 ms a frame at 60 fps on Unit 13's 32x32 exposure target (the hitch log's "readback waits").
 * Forcing those one present old (RECOMP_GPU_READBACK_AGE=1, the rule before) still waited 8.6-15.3 ms a
 * frame at render scale 3, where the GPU runs about a frame behind the CPU: the readback of frame N is
 * queued behind the rest of N's scenes, and present N+1 comes before the GPU reaches them. Two presents
 * later it has long finished. The exposure value reaches guest memory a present later than it did -- within
 * the latency the console's own GPU has, and the game smooths it over frames. A guest write over the
 * target (gfx_gpu_guest_writing) and a re-enqueue of the same target (readback_enqueue) still land the
 * pending one first, whatever its age. RECOMP_GPU_READBACK_WAIT=1: wait for all at present, as before.
 * Two presents of age (RECOMP_GPU_READBACK_AGE=2) let the CPU run two frames ahead of a GPU that is the
 * bottleneck (render scale 3), and the frames came out bimodal -- bursts of 16.7 ms frames, then 30-45 ms
 * stalls with nothing drawn: Unit 13 at 3x, 74 frames over 25 ms per 300 instead of 5-6, 1% low 66 ms instead
 * of 47-50, at the same fps (integration, 2026-10-09). One present of age keeps the queue one frame deep and
 * the frames even, and the wait for it happens off the renderer lock below, which is what mattered. */
static int readback_age(void) { static int v = -1; if (v < 0) { const char *a = getenv("RECOMP_GPU_READBACK_AGE"); v = a && *a && atoi(a) > 0 ? atoi(a) : 1; } return v; }
static int readback_wait_all(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_GPU_READBACK_WAIT") != 0; return v; }
static int readback_due(const Readback *r) { return readback_wait_all() || r->frame + (uint64_t)readback_age() <= gpu_frame_no; }   /* present makes it land whether or not the GPU is done */
/* The wait itself belongs outside the renderer lock. Even two presents old, a readback is still running at render
 * scale 3: the GPU is the bottleneck there (23 ms a frame against a 16.7 ms vblank) and the forced landing is what
 * paces the CPU to it, so the wait is as long as the GPU is behind, every frame -- 10-15 ms -- and under the lock
 * it held the render thread (10% of it in the mutex) and was counted twice in the "render" metric. gfx_dispatch.c's
 * gfx_present takes the lock just to collect the due fences here (_due), waits for them with the lock released
 * (_wait; the render thread keeps recording the next frame), then presents under the lock, where they land with no
 * wait left. A pinned readback -- one whose fence the presenting thread holds for that wait -- is never released by
 * anyone else: readback_complete parks the fence (parked[]) and the presenting thread releases it at its present,
 * after its wait on it is over. One presenting thread at a time (the display queue thread; gfxreplay's main thread).
 * The pre-wait's time is folded into the hitch log's readback waits at the present. Measured at render scale 3
 * (Unit 13, headless): the render thread's mutex wait 13.5% -> 2.4%, the "render" metric 34-40 -> 12-18 ms, fps
 * unchanged (GPU-bound). The price of two presents of allowed lag, in that GPU-bound regime: a synchronous
 * download (a shot, a CPU write over a GPU target) waits for two frames of queued GPU work instead of one -- the
 * headless run's shot frames went 30 -> 54 ms; RECOMP_GPU_READBACK_AGE=1 keeps the queue one frame deep. */
static uint64_t prewait_us; static uint32_t prewait_n;
int gfx_gpu_readbacks_due(void **fences, int max) {
	if (!gpu()) return 0;
	int n = 0;
	for (int i = 0; i < nreadbacks && n < max; i++) { Readback *r = &readbacks[i]; if (readback_due(r) && !SDL_QueryGPUFence(gpu(), r->fence)) { r->pinned = 1; fences[n++] = r->fence; } }
	return n;
}
void gfx_gpu_readbacks_wait(void **fences, int n) {
	if (!gpu() || n <= 0) return;
	uint64_t t0 = us_now(); SDL_WaitForGPUFences(gpu(), true, (SDL_GPUFence **)fences, (uint32_t)n); prewait_us += us_now() - t0; prewait_n++;
}
static void readbacks_complete_at_present(void) {
	for (int i = 0; i < nparked; i++) SDL_ReleaseGPUFence(gpu(), parked[i]);   /* this thread's wait on them is over */
	nparked = 0;
	t_readback_wait += prewait_us; n_readback_waits += prewait_n; prewait_us = 0; prewait_n = 0;
	int k = 0;
	for (int i = 0; i < nreadbacks; i++) {
		Readback *r = &readbacks[i]; int land = r->pinned || readback_due(r) || SDL_QueryGPUFence(gpu(), r->fence);
		r->pinned = 0;   /* (released by readback_complete below, not parked: the wait is over) */
		if (land) readback_complete(r); else readbacks[k++] = *r;
	}
	nreadbacks = k;
}
/* The read watch: which GPU-only targets does the game read on the CPU? Killzone measures its scene's brightness from a few
 * texels of its 384x512 post-processing atlas -- a target its own draws sample, which the rule below never read back: guest
 * memory held a stale image, the auto-exposure opened to its maximum (17.3, where the console's is ~1.5) and every scene came
 * out washed white (2026-10-19). So after each scene a target the rule leaves on the GPU has its whole guest pages made
 * inaccessible (gwatch_read_arm); the game's first access to one marks the target (cpu_reads), read back after every scene
 * from then on. The graphics code's own accesses (inside gfx_dispatch.c's lock) don't count. RECOMP_GPU_NO_READ_WATCH=1: off. */
extern __thread int gfx_host_depth;
static uint32_t rw_pending[16]; static int rw_npending;
static void read_hook(uint32_t addr) {   /* (from the fault handler, any thread: records only) */
	if (gfx_host_depth) return;
	const int i = __atomic_fetch_add(&rw_npending, 1, __ATOMIC_ACQ_REL);
	if (i < 16) __atomic_store_n(&rw_pending[i], addr, __ATOMIC_RELEASE);
}
static void read_watch_marks(void) {
	int n = __atomic_exchange_n(&rw_npending, 0, __ATOMIC_ACQ_REL); if (n > 16) n = 16;
	for (int k = 0; k < n; k++) {
		const uint32_t a = __atomic_exchange_n(&rw_pending[k], 0, __ATOMIC_ACQ_REL);
		for (int i = 0; a && i < ntargets; i++) {
			TargetEntry *t = &targets[i]; if (!t->color || !t->addr || t->cpu_reads) continue;
			GfxTarget g = target_entry_as_gfx(t); const uint32_t hi = t->addr + (uint32_t)gfx_target_bpp(&g) * t->width * t->height;
			if (a < t->addr || a >= hi) continue;
			t->cpu_reads = 1;
			fprintf(stderr, "    gpu: the game reads target %#x %ux%u on the CPU (at +%#x): read back after every scene from now on\n", t->addr, t->width, t->height, a - t->addr);
		}
	}
}
static void readback_enqueue(TargetEntry *e) {
	static int maxpx = -1, all, watch; if (maxpx < 0) { const char *v = getenv("RECOMP_GPU_READBACK_MAX"); maxpx = v ? atoi(v) : 32768; all = getenv("RECOMP_GPU_READBACK_ALL") != 0;
		watch = !getenv("RECOMP_GPU_NO_READ_WATCH") && gwatch_on(); if (watch) gwatch_set_read_hook(read_hook); }
	if (watch) read_watch_marks();
	if (!e || !e->addr || !e->gpu_dirty) return;
	if (!all && !e->cpu_reads && ((e->sampled && e->kind != TK_F32) || !maxpx || e->width * e->height > (uint32_t)maxpx)) {
		if (watch) { GfxTarget g = target_entry_as_gfx(e); gwatch_read_arm(e->addr, (uint32_t)gfx_target_bpp(&g) * e->width * e->height); }
		return;
	}   /* RECOMP_GPU_READBACK_ALL=1 (diagnosis): every scene's target, so guest memory holds what the console's would -- does the game read a target on the CPU the rule above misses? */
	readbacks_complete(e->addr, e->addr + 1);   /* an older copy of this one first */
	if (nreadbacks == MAXREADBACK) readbacks_complete(0, ~0u);
	uint32_t n = kind_bytes(e->kind) * e->width * e->height;
	SDL_GPUTransferBufferCreateInfo xci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = n };
	SDL_GPUTransferBuffer *xb = SDL_CreateGPUTransferBuffer(gpu(), &xci);
	if (!xb) return;
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu());
	SDL_GPUTexture *down = e->color;
	if (TSCALED(e) && target_lo(e)) {
		SDL_GPUBlitInfo bl = { .source = { .texture = e->color, .w = TSX(e, e->width), .h = TSY(e, e->height) }, .destination = { .texture = e->lo, .w = e->width, .h = e->height }, .load_op = SDL_GPU_LOADOP_DONT_CARE, .filter = SDL_GPU_FILTER_NEAREST };
		SDL_BlitGPUTexture(cmd, &bl); down = e->lo;
	}
	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUTextureRegion src = { .texture = down, .w = e->width, .h = e->height, .d = 1 };
	SDL_GPUTextureTransferInfo dst = { .transfer_buffer = xb, .pixels_per_row = e->width, .rows_per_layer = e->height };
	SDL_DownloadFromGPUTexture(cp, &src, &dst);
	SDL_EndGPUCopyPass(cp);
	Readback *r = &readbacks[nreadbacks++]; r->frame = gpu_frame_no; r->pinned = 0;   /* (the slot may hold a compacted-away entry's flag) */
	r->e = e; r->t = target_entry_as_gfx(e); r->kind = e->kind; r->gen = e->gen; r->xb = xb;
	r->fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
	if (!r->fence) { SDL_ReleaseGPUTransferBuffer(gpu(), xb); nreadbacks--; return; }
	n_readbacks++;
}
void gfx_gpu_guest_writing(uint32_t addr, uint32_t len) {
	/* BEFORE the bytes land: a target the GPU has drawn into since its last download goes to guest
	 * memory first, so the write replaces only the bytes it covers -- a capture's per-draw restore
	 * of a 2.7 KB texture window at the top of Unit 13's bloom pyramid marked the whole 512 KB
	 * target for re-upload, and the re-upload brought back whatever stale bytes the rest of guest
	 * memory held (every bloom level came out a flat red) */
	gwatch_write_begin(addr, len);   /* the pages open once for the whole copy, not a fault each (texture change detection: get_texture) */
	if (!gpu()) return;
	readbacks_complete(addr, addr + len);   /* a pending readback lands first: the write replaces it */
	/* the pending draws (see "the draw batch") read nothing from guest memory -- their vertices, LDST
	 * windows and textures were taken when recorded -- and their depth never comes down to it: only a
	 * download of the scene's target below has to wait for them (target_download_floats flushes) */
	for (int i = 0; i < ntargets; i++) {
		TargetEntry *e = &targets[i]; if (!e->color || !e->gpu_dirty) continue;
		GfxTarget t = target_entry_as_gfx(e); uint32_t hi = e->addr + (uint32_t)gfx_target_bpp(&t) * e->width * e->height;
		if (addr < hi && addr + len > e->addr) { sync_why = "(before a guest write)"; target_download_to_guest(e); }
	}
}
void gfx_gpu_guest_reading(uint32_t addr, uint32_t len) {   /* gfx_gpu_guest_writing's download without the write: a transfer's source stays the GPU's (marked written, the target re-uploaded before every next GPU use -- Killzone's streak strip each frame) */
	if (!gpu()) return;
	readbacks_complete(addr, addr + len);
	for (int i = 0; i < ntargets; i++) {
		TargetEntry *e = &targets[i]; if (!e->color || !e->gpu_dirty) continue;
		GfxTarget t = target_entry_as_gfx(e); uint32_t hi = e->addr + (uint32_t)gfx_target_bpp(&t) * e->width * e->height;
		if (addr < hi && addr + len > e->addr) { sync_why = "(before a guest read)"; target_download_to_guest(e); }
	}
}
void gfx_gpu_guest_written(uint32_t addr, uint32_t len) {
	gwatch_write_end(addr, len);
	if (!gpu()) return;
	for (int i = 0; i < ntargets; i++) {
		TargetEntry *e = &targets[i]; if (!e->color) continue;
		GfxTarget t = target_entry_as_gfx(e); uint32_t hi = e->addr + (uint32_t)gfx_target_bpp(&t) * e->width * e->height;
		if (addr < hi && addr + len > e->addr) { e->cpu_dirty = 1; e->gpu_dirty = 0; if (trace_sync()) fprintf(stderr, "gpu trace: guest wrote [%#x, +%u) over target %#x: re-upload before its next use\n", addr, len, e->addr); }
	}
	for (int i = 0; i < MAXDEPTHG; i++) {
		DepthEntry *de = &depthsg[i]; if (!de->tex) continue;
		uint32_t hi = de->key + 4u * de->w * de->h;   /* generous: every depth/stencil format is at most 4 bytes a sample */
		if (addr < hi && addr + len > de->key) de->gpu_newer = 0;
	}
}
/* every GPU target -> guest memory (gfxreplay's RECOMP_REPLAY_DRAWS dump: the state mid-frame) */
void gfx_gpu_sync_all(void) { if (!gpu()) return; batch_flush("sync to guest"); readbacks_complete(0, ~0u); for (int i = 0; i < ntargets; i++) if (targets[i].gpu_dirty) target_download_to_guest(&targets[i]); }
/* a scene's visibility results are in guest memory before it ends (RECOMP_VIS_SYNC=0: a frame or so later, as they come
 * back). The game reads them once the scene's notification says it is done, and the host writes notifications at the
 * kick, right after this: read a frame late, Killzone's occlusion culling hid what had just come into view -- objects and
 * NPCs flashing in and out, at every cutscene cut. Waiting costs the scene's GPU time, on scenes with queries only. */
static int vis_sync(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_VIS_SYNC"); v = !(e && *e == '0'); } return v; }
void gfx_gpu_end_scene(void) {
	scene_open = 0;
	if (scene_soft) { gfx_soft_end_scene(); scene_soft = 0; }
	if (gpu()) {
		batch_flush("end of scene");
		const SsaoCamera *cam = ao_scene && ao_draws >= 30 && scene_entry && scene_depth_entry && scene_depth_entry->tex ? ao_camera() : 0;
		if (cam && ssao_apply(gpu(), scene_entry->color, kind_format(scene_entry->kind), scene_depth_entry->tex, TSX(scene_entry, scene_entry->width), TSY(scene_entry, scene_entry->height), cam)) n_ao++;   /* before anything reads the scene */
		ao_scene = 0;
		if (scene_entry) readback_enqueue(scene_entry);
		if (vis_pending() && vis_sync()) vis_complete(1);
	}
}

static void target_sync_for_cpu(TargetEntry *e) { if (e && e->addr && e->gpu_dirty) { uint64_t t0 = us_now(); target_download_to_guest(e); t_tsync += us_now() - t0; } }
static void target_sync_for_gpu(TargetEntry *e) { if (e && e->addr && e->cpu_dirty) { uint64_t t0 = us_now(); target_upload_from_guest(e); t_tsync += us_now() - t0; } }

/* ---------------------------------------------------------------- target views
 * A texture over memory a GPU render target (or depth surface) owns, in another format, layout or
 * sub-rectangle: Unit 13's lighting reads its F32F32 G-buffer as 1440-wide U8U8U8U8 / S8S8S8S8
 * bytes, its depth buffer as X8U24, its HDR buffer as U2F10F10F10, and its bloom pyramid as strided
 * sub-rectangles of one F11F11F10 atlas -- every frame. Decoding those from guest memory meant a
 * download (fence wait + float encode), a per-texel decode and a 4-16 byte/texel upload per view per
 * frame: 130 of a 752 ms level frame. A view is instead rendered on the GPU: a full-screen pass
 * reads the source texture's pixels and writes what gfx_soft.c's texel() would decode from the
 * bytes the target would have stored (same quantisation, same component rules) into an RGBA32F
 * texture the draw then samples like any other. It is rebuilt only when the source has new content
 * (TargetEntry.gen; DepthEntry.zgen, its depth plane). Pairs this doesn't know fall back to the guest-memory path, traced. */
typedef struct { uint32_t off_bytes, tex_stride, tgt_stride, tex_bpp, tgt_bpp, mode, tgt_w, tgt_h, swz[4], scale, scale_y, ms_x, ms_y;
	uint32_t vfmt; } ViewU;   /* the shader's uniforms (the first 16 words, pushed to the GPU as-is); swz: per output component, the decoded component 0-3 or 4 = zero, 5 = one (gfx_soft.c's apply_swizzle); ms_x, ms_y: the source's guest memory holds that many samples a pixel across and down (an MSAA depth surface's, read at sample size), 0 = 1; vfmt (RECOMP_GPU_VIEW_FAST, appended: host-side only, never read by the shader): the view's own SDL_GPUTextureFormat, carried from view_format() at target_view() through batch_add_view/batch_flush's existing ViewU copy to view_pass, which picks the pipeline by it without either of those two functions' bodies changing */
static int batch_add_view(SDL_GPUTexture *dst, SDL_GPUTexture *src, int from_depth, const ViewU *u);   /* (the draw batch, below) */
static void view_pass(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *dst, SDL_GPUTexture *src, int from_depth, const ViewU *u);
static const char *view_msl =
	"#include <metal_stdlib>\nusing namespace metal;\n"
	"struct VOut { float4 position [[position]]; };\n"
	"vertex VOut main0(uint vid [[vertex_id]]) { float2 p[3] = { float2(-1,1), float2(3,1), float2(-1,-3) }; VOut o; o.position = float4(p[vid], 0, 1); return o; }\n"
	"struct ViewU { uint off_bytes, tex_stride, tgt_stride, tex_bpp, tgt_bpp, mode, tgt_w, tgt_h; uint4 swz; uint scale, scale_y, ms_x, ms_y; };\n"
	"static float4 swizzle(float4 c, uint4 z) { float4 o; for (uint k = 0u; k < 4u; k++) o[k] = z[k] < 4u ? c[z[k]] : z[k] == 4u ? 0.0 : 1.0; return o; }\n"
	/* gfx_soft.c's to_small_float / small_float: unsigned floats, 5-bit exponent (bias 15), mbits of mantissa, rounded half up */
	"static uint tsf(float f, uint mbits) { if (!(f > 0.0)) return 0u; if (f >= 65504.0) return (30u << mbits) | ((1u << mbits) - 1u); int e; float m = frexp(f, e); int be = e - 1 + 15;"
	" if (be <= 0) return uint(f * ldexp(1.0, 14 + int(mbits)) + .5) & ((1u << mbits) - 1u); uint mant = uint((m * 2.0 - 1.0) * float(1u << mbits) + .5); if (mant >> mbits) { mant = 0u; be++; } return (uint(be) << mbits) | mant; }\n"
	"static float sfl(uint v, uint mbits) { uint e = v >> mbits, m = v & ((1u << mbits) - 1u); if (e == 0u) return ldexp(float(m), -14 - int(mbits)); if (e == 31u) return m != 0u ? 0.0 : 65504.0; return ldexp(1.0 + float(m) / float(1u << mbits), int(e) - 15); }\n"
	"static float q(float f, uint mbits) { return sfl(tsf(f, mbits), mbits); }\n"
	/* destination texel -> the source pixel holding its bytes (linear addressing in both; identity for same-layout full views) and which 32-bit word of that pixel;
	 * at render scale n both are n times the guest's size: the guest pixel's mapping, the same sub-pixel of the source's block */
	"static uint2 srcpix(uint2 P, constant ViewU &u, thread uint &sub) { uint2 sc = uint2(u.scale, u.scale_y), p = P / sc, f = P % sc; uint b = u.off_bytes + (p.y * u.tex_stride + p.x) * u.tex_bpp; uint pix = b / u.tgt_bpp; sub = (b % u.tgt_bpp) / 4u;"
	" return min(uint2(pix % u.tgt_stride, pix / u.tgt_stride) / max(uint2(u.ms_x, u.ms_y), uint2(1u)), uint2(u.tgt_w - 1u, u.tgt_h - 1u)) * sc + f; }\n"
	"static float s8(uint bb) { int v = int(bb); if (v >= 128) v -= 256; return v <= -127 ? -1.0 : float(v) / 127.0; }\n"
	/* the sRGB transfer function both ways (an sRGB texture's decoded values <-> the bytes it stores) */
	"static float3 enc(float3 x) { return select(1.055 * pow(x, float3(1.0 / 2.4)) - 0.055, 12.92 * x, x <= 0.0031308); }\n"
	"static float3 dec(float3 x) { return select(pow((x + 0.055) / 1.055, float3(2.4)), x / 12.92, x <= 0.04045); }\n"
	"fragment float4 main0_view(VOut in [[stage_in]], texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]], constant ViewU &u [[buffer(0)]]) {\n"
	"  if (u.mode == 7u || u.mode == 8u) {\n"   /* a GXM transfer downscale (gfx_gpu_transfer_downscale): the 2x2 box of source guest pixels from off_bytes (rows of tgt_stride pixels) at the same sub-pixel, on the stored bytes (mode 8: an sRGB source and destination, whose bytes are its encoded values) */
	"    uint2 sc = uint2(u.scale, u.scale_y), P = uint2(in.position.xy), p = P / sc, f = P % sc; uint o0 = u.off_bytes / 4u;\n"
	"    uint2 org = uint2(o0 % u.tgt_stride, o0 / u.tgt_stride) + 2u * p, lim = uint2(u.tgt_w - 1u, u.tgt_h - 1u); uint4 s = uint4(0u);\n"
	"    for (uint d = 0u; d < 4u; d++) { float4 c = src.read(min(org + uint2(d & 1u, d >> 1u), lim) * sc + f); if (u.mode == 8u) c.rgb = enc(c.rgb); s += uint4(saturate(c) * 255.0 + 0.5); }\n"
	"    float4 r = float4((s + 2u) / 4u) / 255.0; if (u.mode == 8u) r.rgb = dec(r.rgb); return r; }\n"
	"  uint sub; uint2 sp = srcpix(uint2(in.position.xy), u, sub); float4 c = src.read(sp);\n"
	"  if (u.mode == 5u || u.mode == 6u) return swizzle(float4(c.r, 0.0, 0.0, 1.0), u.swz);\n"                   /* U8, F32 (one component) */
	"  if (u.mode == 1u) return float4(q(c.r, 6u), q(c.g, 6u), q(c.b, 5u), 1.0);\n"                          /* F11F11F10 */
	"  if (u.mode == 2u) { float al = c.a <= 0.0 ? 0.0 : c.a >= 1.0 ? 3.0 : floor(c.a * 3.0 + .5); return float4(q(c.r, 5u), q(c.g, 5u), q(c.b, 5u), al / 3.0); }\n"   /* U2F10F10F10 */
	"  if (u.mode == 3u || u.mode == 4u) { uint w = as_type<uint>(sub == 1u ? c.g : sub == 2u ? c.b : sub == 3u ? c.a : c.r); float4 o;\n"   /* the pixel's words as U8U8U8U8 / S8S8S8S8 bytes */
	"    for (uint k = 0u; k < 4u; k++) { uint bb = (w >> (8u * k)) & 255u; o[k] = u.mode == 3u ? float(bb) / 255.0 : s8(bb); } return swizzle(o, u.swz); }\n"
	"  return swizzle(c, u.swz); }\n"                                                                                           /* 0: the same values (F16x4, U8x4) */
	"fragment float4 main0_view_depth(VOut in [[stage_in]], depth2d<float> src [[texture(0)]], sampler s [[sampler(0)]], constant ViewU &u [[buffer(0)]]) {\n"
	"  uint sub; uint2 sp = srcpix(uint2(in.position.xy), u, sub); float d = src.read(sp); float m = u.mode == 1u ? 65535.0 : 16777215.0; float qd = floor(d * m + .5) / m; return float4(qd, qd, qd, 1.0); }\n";   /* X8U24: 24-bit depth, as the software path stores and reads it; mode 1, D16 read as U16: 16-bit */
/* ---------------------------------------------------------------- RECOMP_GPU_VIEW_FAST
 * Two independent changes to the view pass above, both off at RECOMP_GPU_VIEW_FAST=0 (the view_msl /
 * view_pipeline / RGBA32F-only pass just above, unchanged):
 * 1. Narrow view textures. view_entry's RGBA32F texture is wider than most decodes need: a byte view
 *    (mode 3/4) only ever writes n/255 or v/127 per channel, an identity view of an F16 target (mode 0)
 *    only ever holds f16-exact values, F11F11F10 tiles (mode 1) decode to exactly the f11/f10 grid. view_format
 *    picks the narrowest exact format for those (point-sampled only: a linear-filtered view keeps RGBA32F,
 *    since filtering a narrow format isn't the same arithmetic as filtering floats); the HDR view (mode 2,
 *    U2F10F10F10) stays RGBA32F -- its 2-bit alpha's 1/3, 2/3 aren't f16-exact. Byte views drop from 16 to 1
 *    byte a texel. Checked byte-identical against RGBA32F on 5 U13/Uncharted captures.
 * 2. Baked per-view shaders. srcpix's addressing (a division and a modulo by the uniform's tgt_stride, plus
 *    the mode dispatch) runs the same for every fragment of a view pass with values that are the same for
 *    every fragment of THAT view -- but they are still a per-draw uniform read to the general shader above,
 *    which cannot fold them. view_msl_variant re-renders the exact same shader text with one view's ViewU
 *    baked in as a MSL `constant` global instead of a push-constant parameter (a source-text transform of
 *    view_msl itself -- the three places that mention the parameter are removed by exact substring match,
 *    so a change to view_msl's shape that breaks one simply fails the match and view_pipeline_for falls
 *    back to the general pipeline, never emits a wrong shader): the Metal compiler then has a compile-time
 *    divisor for the division/modulo (its usual strength reduction, not row-alignment math) and a compile-
 *    time mode, so every other mode's branch -- and HDR's frexp/ldexp quantiser when the mode isn't 2 -- is
 *    dead code. Pipelines are cached per (from_depth, format, and every ViewU field the generated text
 *    depends on) in a 32-entry LRU (views themselves top out at MAXVIEW below, so 32 variants comfortably
 *    covers one scale's steady state); on a cache miss or a compile failure the general pipeline above
 *    runs instead, at the correct (possibly narrow) format either way -- never a format mismatch against
 *    the view's own texture. */
static int view_fast_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_GPU_VIEW_FAST"); v = !(e && *e && atoi(e) == 0); } return v; }   /* RECOMP_GPU_VIEW_FAST=0: today's views (RGBA32F, the general shader above, unchanged) */
/* every format view_format can return (kind_format's included: mode 0's), each with its own general pipeline. A format
 * missing here once shared slot 0 with RGBA32F: the slot's pipeline was built at whichever of the two asked first, and
 * the other's views were drawn by a pipeline of the wrong colour format -- an RGBA32F pipeline into an sRGB RGBA8 view
 * leaves each texel holding the float bits of its red channel. The sRGB views (564ef9f) went that way whenever the baked
 * cache was full during a flush: Killzone's store cards in rainbow noise after a while in play, every time on Vulkan,
 * which has only the general pipelines (2026-10-17) */
static const SDL_GPUTextureFormat view_fmts[] = { SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_SNORM, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT, SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT,
                                                  SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB };
#define NVIEWFMT ((int)(sizeof view_fmts / sizeof *view_fmts))
static int view_fmt_index(SDL_GPUTextureFormat f) { for (int i = 0; i < NVIEWFMT; i++) if (view_fmts[i] == f) return i; return -1; }
/* the view's own texture format: RGBA32F unless VIEW_FAST, a linear-filtered view (filtering a narrow
 * format isn't exact), a depth view, or an unsupported narrow format on this GPU */
static SDL_GPUTextureFormat view_format(int from_depth, int mode, int src_kind, int filtered) {
	SDL_GPUTextureFormat f = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
	if (!view_fast_on() || from_depth || filtered) return f;
	switch (mode) {
	case 3: case 5: f = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; break;
	case 4: f = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_SNORM; break;
	case 0: f = src_kind == TK_F32 ? SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT : kind_format(src_kind); break;
	case 1: f = SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT; break;
	/* mode 2 (HDR, U2F10F10F10): stays RGBA32F -- the 2-bit alpha is not f16-exact */
	}
	if (f != SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT && !SDL_GPUTextureSupportsFormat(gpu(), f, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER)) f = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
	return f;
}
/* the shared vertex shader (view_msl's main0: no ViewU, nothing to bake -- every pipeline below, general or baked, uses this one) */
static SDL_GPUShader *view_vertex_shader(void) {
	static SDL_GPUShader *vs; static int tried;
	if (tried) return vs;
	tried = 1;
	vs = builtin_shader(view_msl, "main0", vk_fullscreen_vert, sizeof vk_fullscreen_vert, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
	if (!vs) fprintf(stderr, "    gpu: view vertex shader: %s\n", SDL_GetError());
	return vs;
}
/* view_msl's own two fragment entrypoints, at a given output format (used for every view when VIEW_FAST
 * is off, on Vulkan (vk/view.frag, vk/view_depth.frag), and as the exact-format fallback when a baked
 * variant's compile fails): a cache keyed on the output format (view_fmts) */
static SDL_GPUGraphicsPipeline *view_pipeline_general(int from_depth, SDL_GPUTextureFormat fmt) {
	static SDL_GPUGraphicsPipeline *pipes[2][NVIEWFMT]; static SDL_GPUShader *fsg[2]; static int tried;
	int fi = view_fmt_index(fmt);
	if (fi < 0) {   /* no pipeline: view_render declines and the texture decodes from guest memory, slower but right */
		static int said; if (!said++) fprintf(stderr, "    gpu: no view pipeline for texture format %d (add it to view_fmts): those views go through guest memory\n", (int)fmt);
		return 0;
	}
	if (pipes[from_depth][fi]) return pipes[from_depth][fi];
	SDL_GPUShader *vsh = view_vertex_shader();
	if (!vsh) return 0;
	if (!tried) {
		tried = 1;
		for (int k = 0; k < 2; k++) {
			const char *entry = k == 0 ? "main0_view" : "main0_view_depth";
			fsg[k] = builtin_shader(view_msl, entry, k == 0 ? vk_view_frag : vk_view_depth_frag, k == 0 ? sizeof vk_view_frag : sizeof vk_view_depth_frag, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
			if (!fsg[k]) fprintf(stderr, "    gpu: view shader (general) %s: %s\n", entry, SDL_GetError());
		}
	}
	if (!fsg[from_depth]) return 0;
	SDL_GPUGraphicsPipelineCreateInfo ci = {0};
	ci.vertex_shader = vsh; ci.fragment_shader = fsg[from_depth];
	ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL; ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
	ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
	ci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
	SDL_GPUColorTargetDescription ctd = { .format = fmt };
	ci.target_info.num_color_targets = 1; ci.target_info.color_target_descriptions = &ctd;
	pipes[from_depth][fi] = SDL_CreateGPUGraphicsPipeline(gpu(), &ci);
	if (!pipes[from_depth][fi]) fprintf(stderr, "    gpu: view pipeline (general, fmt %d): %s\n", (int)fmt, SDL_GetError());
	return pipes[from_depth][fi];
}
/* one in-place substring replacement (0: `from` not found, buf unchanged); view_msl_variant's safety net --
 * every call site checks its expected match count and gives up on the baked shader, never a partial edit */
static int str_replace1(char *buf, size_t cap, const char *from, const char *to) {
	char *p = strstr(buf, from);
	if (!p) return 0;
	size_t flen = strlen(from), tlen = strlen(to), buflen = strlen(buf), taillen = buflen - (size_t)(p - buf) - flen;
	if (buflen - flen + tlen + 1 > cap) return 0;
	memmove(p + tlen, p + flen, taillen + 1);   /* +1: the NUL */
	memcpy(p, to, tlen);
	return 1;
}
static int str_replace_all(char *buf, size_t cap, const char *from, const char *to) { int n = 0; while (str_replace1(buf, cap, from, to)) n++; return n; }
/* view_msl's main0_view / main0_view_depth / srcpix, unchanged, with one view's ViewU baked in as a
 * `constant` global in place of the [[buffer(0)]] parameter (see the design comment above view_fast_on):
 * every occurrence of that parameter is removed from view_msl's own text by exact match (0 on any
 * mismatch: the caller falls back to the general, unbaked pipeline, byte-identical either way since it's
 * the same formula, just not constant-folded) */
static const char *view_msl_variant(const ViewU *u) {
	static char buf[8192];
	size_t n = strlen(view_msl);
	if (n + 1 > sizeof buf) return 0;
	memcpy(buf, view_msl, n + 1);
	if (!str_replace1(buf, sizeof buf, "constant ViewU &u, thread uint &sub", "thread uint &sub")) return 0;   /* srcpix's declaration (1) */
	if (str_replace_all(buf, sizeof buf, ", constant ViewU &u [[buffer(0)]]", "") != 2) return 0;               /* both entrypoints' parameter */
	if (str_replace_all(buf, sizeof buf, "(uint2(in.position.xy), u, sub)", "(uint2(in.position.xy), sub)") != 2) return 0;   /* both entrypoints' call into srcpix */
	/* one sample a pixel (every view but an MSAA depth surface's): srcpix and the struct as they were before ms_x/ms_y, so the
	 * text, and with it the library and pipeline this machine's archive keeps for it, is the same as before */
	const int ms = u->ms_x > 1 || u->ms_y > 1;
	if (!ms && !str_replace1(buf, sizeof buf, " / max(uint2(u.ms_x, u.ms_y), uint2(1u))", "")) return 0;
	char decl[384];
	if (ms) snprintf(decl, sizeof decl, "struct ViewU { uint off_bytes, tex_stride, tgt_stride, tex_bpp, tgt_bpp, mode, tgt_w, tgt_h; uint4 swz; uint scale, scale_y, ms_x, ms_y; };\n"
		"constant ViewU u = { %uu, %uu, %uu, %uu, %uu, %uu, %uu, %uu, uint4(%uu,%uu,%uu,%uu), %uu, %uu, %uu, %uu };\n",
		u->off_bytes, u->tex_stride, u->tgt_stride, u->tex_bpp, u->tgt_bpp, u->mode, u->tgt_w, u->tgt_h, u->swz[0], u->swz[1], u->swz[2], u->swz[3], u->scale, u->scale_y, u->ms_x, u->ms_y);
	else snprintf(decl, sizeof decl, "struct ViewU { uint off_bytes, tex_stride, tgt_stride, tex_bpp, tgt_bpp, mode, tgt_w, tgt_h; uint4 swz; uint scale, scale_y; };\n"
		"constant ViewU u = { %uu, %uu, %uu, %uu, %uu, %uu, %uu, %uu, uint4(%uu,%uu,%uu,%uu), %uu, %uu };\n",
		u->off_bytes, u->tex_stride, u->tgt_stride, u->tex_bpp, u->tgt_bpp, u->mode, u->tgt_w, u->tgt_h, u->swz[0], u->swz[1], u->swz[2], u->swz[3], u->scale, u->scale_y);
	if (!str_replace1(buf, sizeof buf, "struct ViewU { uint off_bytes, tex_stride, tgt_stride, tex_bpp, tgt_bpp, mode, tgt_w, tgt_h; uint4 swz; uint scale, scale_y, ms_x, ms_y; };\n", decl)) return 0;
	return buf;
}
static uint64_t view_variant_hash(int from_depth, const ViewU *u) {
	uint64_t h = 1469598103934665603ULL;   /* FNV-1a over every field view_msl_variant bakes, plus vfmt and from_depth (view_format/view_pipeline_general's key) */
	uint32_t vals[14] = { (uint32_t)from_depth, (uint32_t)u->vfmt, u->mode, u->scale, u->off_bytes, u->tex_stride, u->tgt_stride, u->tex_bpp, u->tgt_bpp, u->tgt_w, u->tgt_h, u->scale_y, u->ms_x, u->ms_y };
	for (int i = 0; i < 14; i++) { h ^= vals[i]; h *= 1099511628211ULL; }
	for (int i = 0; i < 4; i++) { h ^= u->swz[i]; h *= 1099511628211ULL; }
	return h;
}
typedef struct ViewVariant { uint64_t key, used; SDL_GPUShader *fs; SDL_GPUGraphicsPipeline *pipe; int pending; } ViewVariant;   /* pending: a build worker is making fs/pipe */
#define MAXVIEWVARIANT 256   /* Killzone's level uses ~36 a frame at one render scale; menus, other scales and other games' views on top */
static ViewVariant view_variants[MAXVIEWVARIANT]; static int nview_variants; static uint64_t view_variant_clock;
/* evicted variants' shaders and pipelines: released at the end of the batch_flush that may still bind them (batch_flush, below) */
static struct { SDL_GPUShader *fs; SDL_GPUGraphicsPipeline *pipe; } view_released[MAXVIEWVARIANT]; static int nview_released;
static void view_released_free(void) {
	for (int i = 0; i < nview_released; i++) { if (view_released[i].pipe) SDL_ReleaseGPUGraphicsPipeline(gpu(), view_released[i].pipe); if (view_released[i].fs) SDL_ReleaseGPUShader(gpu(), view_released[i].fs); }
	nview_released = 0;
}
static void view_variant_build(ViewJob *j) {   /* on a build worker or the render thread: the shader and pipeline, then the slot is final */
	SDL_GPUShaderCreateInfo ci = {0};
	ci.code = (const Uint8 *)j->src; ci.code_size = strlen(j->src) + 1; ci.format = SDL_GPU_SHADERFORMAT_MSL;
	ci.entrypoint = j->entry; ci.stage = SDL_GPU_SHADERSTAGE_FRAGMENT; ci.num_samplers = 1; ci.num_uniform_buffers = 0;
#ifdef RECOMP_MTL_ARCHIVE
	/* this machine's archive: the variant's library when it has kept one (no compile at all), else the MSL, its library kept
	 * by its first pipeline; and the pipeline archived. Cold, each was a compile on the OS's shared compiler, queued behind
	 * whatever it had: 557 a session, ready in 0.4 s at the median and 14 s at the worst (2026-10-25) */
	MslLib lib = {0}; uint64_t fkey = 0;
	if (mtl_archive_on && !getenv("RECOMP_GPU_VIEW_COLD")) {
		const uint64_t k = text_key(j->src, j->entry);
		if (text_lib_get(k, &lib)) { ci.code = lib.air; ci.code_size = lib.n; ci.entrypoint = lib.entry; ci.format = SDL_GPU_SHADERFORMAT_METALLIB; }
		else fkey = k;
	}
	SDL_GPUShader *fs = SDL_CreateGPUShader(gpu(), &ci);
	if (fs && lib.air) __atomic_add_fetch(&n_view_libs, 1, __ATOMIC_RELAXED);
	if (!fs && lib.air) { ci.code = (const Uint8 *)j->src; ci.code_size = strlen(j->src) + 1; ci.format = SDL_GPU_SHADERFORMAT_MSL; ci.entrypoint = j->entry; fs = SDL_CreateGPUShader(gpu(), &ci); fkey = text_key(j->src, j->entry); }
	free(lib.air);
#else
	SDL_GPUShader *fs = SDL_CreateGPUShader(gpu(), &ci);
#endif
	SDL_GPUGraphicsPipeline *pipe = 0;
	if (!fs) fprintf(stderr, "    gpu: view shader (baked): %s\n", SDL_GetError());
	else {
		SDL_GPUGraphicsPipelineCreateInfo ci2 = {0};
		ci2.vertex_shader = j->vsh; ci2.fragment_shader = fs;
		ci2.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		ci2.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL; ci2.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		ci2.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
		ci2.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
		SDL_GPUColorTargetDescription ctd = { .format = (SDL_GPUTextureFormat)j->fmt };
		ci2.target_info.num_color_targets = 1; ci2.target_info.color_target_descriptions = &ctd;
#ifdef RECOMP_MTL_ARCHIVE
		if (mtl_archive_on) mtl_archive_want(1, 0, fkey);
#endif
		pipe = SDL_CreateGPUGraphicsPipeline(gpu(), &ci2);
#ifdef RECOMP_MTL_ARCHIVE
		if (mtl_archive_on) mtl_archive_want_clear();
#endif
		if (!pipe) { fprintf(stderr, "    gpu: view pipeline (baked): %s\n", SDL_GetError()); SDL_ReleaseGPUShader(gpu(), fs); fs = 0; }
	}
	if (hitch_log() && us_now() - j->t0 > 2000) fprintf(stderr, "    gpu hitch: view variant ready in %.2f ms%s\n", (us_now() - j->t0) / 1e3, j->slot->pending ? " (built by a worker)" : "");
	free(j->src); j->src = 0;
	j->slot->fs = fs; j->slot->pipe = pipe;
	__atomic_store_n(&j->slot->pending, 0, __ATOMIC_RELEASE);
}
/* the pipeline view_pass binds: a cached baked variant (from_depth/vfmt/u's addressing all literal, *baked
 * set so the caller skips the uniform push -- the shader doesn't declare that buffer any more), or, with
 * VIEW_FAST off or any step of building a variant failing, view_pipeline_general at the correct format */
/* A variant missing from the cache was built here, on the render thread, inside the frame: a burst of new views (a menu
 * opening, a new area, a first run of a new binary -- Metal's shader cache cold) stalled the game for seconds, ~60 ms a cold
 * compile. And once the cache was full, a view asked for inside batch_flush (where view passes run) was compiled and thrown
 * away, every frame: Killzone's level rebuilt its ~36 views each frame, 1.4 ms warm and seconds cold (2026-10-19). Now a miss
 * takes a slot at once (evicting the least recently used built one; its pipeline is released after the flush that may still
 * bind it), the build goes to a worker (gpu_async), and the general pipeline -- the same formula, unbaked -- draws meanwhile. */
static SDL_GPUGraphicsPipeline *view_pipeline_for(int from_depth, const ViewU *u, int *baked) {
	*baked = 0;
	SDL_GPUTextureFormat fmt = (SDL_GPUTextureFormat)u->vfmt;
	if (!view_fast_on() || !(SDL_GetGPUShaderFormats(gpu()) & SDL_GPU_SHADERFORMAT_MSL)) return view_pipeline_general(from_depth, fmt);   /* baked variants are MSL text: Vulkan takes the general pipeline */
	uint64_t key = view_variant_hash(from_depth, u);
	for (int i = 0; i < nview_variants; i++) if (view_variants[i].key == key) {
		ViewVariant *v = &view_variants[i]; v->used = ++view_variant_clock;
		if (__atomic_load_n(&v->pending, __ATOMIC_ACQUIRE) || !v->pipe) return view_pipeline_general(from_depth, fmt);   /* building, or its build failed */
		*baked = 1; return v->pipe;
	}
	const char *src = view_msl_variant(u);
	if (!src) return view_pipeline_general(from_depth, fmt);
	SDL_GPUShader *vsh = view_vertex_shader();
	if (!vsh) return view_pipeline_general(from_depth, fmt);
	ViewVariant *slot = 0;
	static int cap = -1; if (cap < 0) { const char *e = getenv("RECOMP_GPU_VIEW_VARIANTS"); cap = e && atoi(e) > 0 && atoi(e) < MAXVIEWVARIANT ? atoi(e) : MAXVIEWVARIANT; }   /* (a smaller cache to test the eviction paths) */
	if (nview_variants < cap) slot = &view_variants[nview_variants++];
	else {
		for (int i = 0; i < nview_variants; i++) if (!__atomic_load_n(&view_variants[i].pending, __ATOMIC_ACQUIRE) && (!slot || view_variants[i].used < slot->used)) slot = &view_variants[i];
		if (!slot || nview_released == MAXVIEWVARIANT) return view_pipeline_general(from_depth, fmt);   /* (every slot building, or the release list full: next time) */
		view_released[nview_released].fs = slot->fs; view_released[nview_released].pipe = slot->pipe; nview_released++;
		slot->fs = 0; slot->pipe = 0;
		if (!batch_flushing) batch_flush("view variant released");   /* (inside a flush: released at its end) */
	}
	slot->key = key; slot->used = ++view_variant_clock; slot->fs = 0; slot->pipe = 0;
	ViewJob vj = { slot, 0, from_depth ? "main0_view_depth" : "main0_view", vsh, (int)fmt, us_now() };
	{	/* RECOMP_GPU_VIEW_COLD=1 (measurement): a unique comment on each variant's text, so Metal's shader cache never has it -- every build a cold compile */
		static int cold = -1; if (cold < 0) cold = getenv("RECOMP_GPU_VIEW_COLD") != 0;
		static unsigned nonce; size_t n = strlen(src) + 64; vj.src = malloc(n);
		if (!vj.src) return view_pipeline_general(from_depth, fmt);
		if (cold) snprintf(vj.src, n, "// cold %u %llu\n%s", ++nonce, (unsigned long long)us_now(), src); else memcpy(vj.src, src, strlen(src) + 1);
	}
	if (gpu_async()) {
		BuildJob bj; memset(&bj, 0, sizeof bj); bj.is_pipe = 2; bj.vj = vj;
		__atomic_store_n(&slot->pending, 1, __ATOMIC_RELEASE);
		if (build_enqueue(&bj)) return view_pipeline_general(from_depth, fmt);
		__atomic_store_n(&slot->pending, 0, __ATOMIC_RELEASE);   /* (no worker: built here) */
	}
	view_variant_build(&vj);
	if (!slot->pipe) return view_pipeline_general(from_depth, fmt);
	*baked = 1;
	return slot->pipe;
}
#define MAXVIEW 32
static TexEntry views[MAXVIEW]; static int nviews;
static void views_release_samplers(void) { for (int i = 0; i < nviews; i++) tex_release_samplers(&views[i]); }
static int no_views(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_GPU_NO_VIEWS") != 0; return v; }   /* RECOMP_GPU_NO_VIEWS=1: the guest-memory path for every render-target read (an A/B check) */
/* which decode the view shader runs for a target's format read by a texture's base format (-1: none known); tex_bpp: the texture's bytes per texel */
static int view_mode(uint32_t tgt_fmt, const GfxTexture *tex, uint32_t *tex_bpp, uint32_t swz[4]) {
	uint32_t tb = tgt_fmt & 0xF1800000u, xb = tex->format & 0xff000000u, sw = (tex->format >> 12) & 7;
	*tex_bpp = 4;
	/* gfx_soft.c's apply_swizzle, as component selectors: four-component formats permute, one-component
	 * formats (U8) place the one value; the HDR packed formats take none */
	static const uint8_t sw4[8][4] = { {0,1,2,3}, {2,1,0,3}, {3,2,1,0}, {1,2,3,0}, {0,1,2,5}, {2,1,0,5}, {3,2,1,5}, {1,2,3,5} };
	static const uint8_t sw1[8][4] = { {0,4,4,5}, {0,4,4,4}, {0,5,5,5}, {0,0,0,0}, {0,0,0,4}, {0,0,0,5}, {4,4,4,0}, {5,5,5,0} };
	int ncomp = xb == 0x0c000000u || xb == 0x0d000000u || xb == 0x1b000000u ? 4 : xb == 0x00000000u || xb == 0x12000000u ? 1 : 0;
	for (int k = 0; k < 4; k++) swz[k] = ncomp == 4 ? sw4[sw][k] : ncomp == 1 ? sw1[sw][k] : (uint32_t)k;
	switch (tb) {
	case 0x80800000u: *tex_bpp = 1; return xb == 0x00000000u ? 5 : -1;                          /* U8 as U8 */
	case 0xB0800000u: *tex_bpp = 2; if (xb != 0x07000000u) return -1;                         /* U8U8 as U8U8: the RGBA8 target's r, g (gfx_soft.c stores R in the
	                                                                                               * low byte and decodes it from there); Killzone's 384x384 depth-pair
	                                                                                               * target, sampled by ~40 draws a frame, came down every frame */
		swz[0] = 0; swz[1] = 1; swz[2] = 4; swz[3] = 5; return 0;
	case 0x10800000u: return xb == 0x12000000u ? 6 : -1;                                        /* F32 as F32 (mode 5's decode at full precision: Uncharted's
	                                                                                               * shadow mask went through guest memory, its lighting read
	                                                                                               * it at 1x and every lit silhouette got a 1x-pixel halo) */
	case 0x01000000u: *tex_bpp = 8; return xb == 0x1b000000u ? 0 : -1;                          /* F16F16F16F16 as F16F16F16F16 */
	case 0x21000000u: return xb == 0x1a000000u ? 1 : -1;                                        /* F11F11F10 as F11F11F10 */
	case 0x41000000u: return xb == 0x9a000000u ? 2 : -1;                                        /* U2F10F10F10 as U2F10F10F10 */
	case 0x11000000u: return xb == 0x0c000000u ? 3 : xb == 0x0d000000u ? 4 : -1;                /* F32F32 as U8U8U8U8 / S8S8S8S8 bytes */
	case 0x00000000u: return (tgt_fmt & 0x00300000u) == 0 && xb == 0x0c000000u ? 0 : -1;      /* U8U8U8U8 as U8U8U8U8 (a sub-rectangle; the exact case binds the target itself) */
	}
	return -1;
}
/* the texture's addressing of the target's memory as a source-pixel mapping (ViewU), or 0 when the layouts don't line up */
static int view_mapping(uint32_t tgt_addr, uint32_t tgt_stride, uint32_t tgt_w, uint32_t tgt_h, int tgt_type, uint32_t tgt_bpp, const GfxTexture *tex, uint32_t tex_bpp, int sx, int sy, ViewU *u) {   /* sx, sy: the source's scale */
	int layout = (tex->u_mode >> 4) & 3;
	if (gfx_texture_levels(tex) != 1) return 0;   /* a mip chain past the view's own level isn't the target's */
	uint32_t swz[4]; memcpy(swz, u->swz, sizeof swz);   /* set by view_mode before this */
	*u = (ViewU){ tex->data - tgt_addr, 0, tgt_stride, tex_bpp, tgt_bpp, 0, tgt_w, tgt_h, { swz[0], swz[1], swz[2], swz[3] }, (uint32_t)sx, (uint32_t)sy, 0, 0 };
	if (layout == 0 && tgt_type == 0) {   /* linear rows over linear rows, at any offset and stride */
		u->tex_stride = (tex->format & 0xfff) ? (tex->format & 0xfff) : ((tex->width + 7u) & ~7u);
		uint64_t end = (uint64_t)u->off_bytes + ((uint64_t)(tex->height - 1) * u->tex_stride + tex->width) * tex_bpp;
		return end <= (uint64_t)tgt_stride * tgt_h * tgt_bpp + tgt_bpp;   /* +1 pixel: a byte view at +4 of an F32F32 target ends one word past it (the shader clamps that read) */
	}
	if (u->off_bytes || tex->height > tgt_h || tex_bpp != tgt_bpp) return 0;
	if (layout == 2 && tgt_type == 1 && ((tex->width + 31u) & ~31u) == tgt_stride) { u->tex_stride = tgt_stride; return 1; }   /* the same 32x32 tiling (the target's width here is its stride, a whole number of tiles): identity */
	if (layout == 1 && tgt_type == 2 && tex->width == tgt_w) { u->tex_stride = tgt_stride; return 1; }   /* the same Morton order over the same square */
	return 0;
}
static TexEntry *view_entry(const GfxTexture *tex, SDL_GPUTextureFormat vfmt, int sx, int sy) {   /* sx, sy: its source's scale */   /* the view's own texture object (vfmt: RGBA32F unless RECOMP_GPU_VIEW_FAST narrowed it; the texture's size), LRU */
	int layout = (tex->u_mode >> 4) & 3;
	for (int i = 0; i < nviews; i++) { TexEntry *v = &views[i]; if (v->addr == tex->data && v->format == tex->format && v->width == tex->width && v->height == tex->height && v->layout == layout && v->sx == sx && v->sy == sy && v->levels <= 1) {
		v->used = ++tex_clock;
		if (v->tex && v->vfmt != vfmt) {   /* the same texture now decodes to another format (a depth <-> colour swap over the same memory, or narrow <-> RGBA32F as filtering turns on/off): recreate it */
			batch_retire(v->tex, "view format changed"); SDL_ReleaseGPUTexture(gpu(), v->tex); v->tex = 0; v->view_src = 0;
			SDL_GPUTextureCreateInfo tci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = vfmt, .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
				.width = TSX(v, tex->width), .height = TSY(v, tex->height), .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
			v->tex = SDL_CreateGPUTexture(gpu(), &tci); v->vfmt = vfmt;
		}
		return v; } }
	TexEntry *v;
	if (nviews < MAXVIEW) v = &views[nviews++];
	else { v = &views[0]; for (int i = 1; i < MAXVIEW; i++) if (views[i].used < v->used) v = &views[i];
		batch_retire(v->tex, "view released");
		if (v->tex) SDL_ReleaseGPUTexture(gpu(), v->tex);
		for (int a = 0; a < 4; a++) for (int b = 0; b < 2; b++) for (int c = 0; c < 3; c++) for (int d = 0; d < 3; d++) if (v->samp[a][b][c][d]) SDL_ReleaseGPUSampler(gpu(), v->samp[a][b][c][d]); }
	memset(v, 0, sizeof *v); v->addr = tex->data; v->format = tex->format; v->width = tex->width; v->height = tex->height; v->layout = layout; v->used = ++tex_clock; v->vfmt = vfmt; v->sx = (uint8_t)sx; v->sy = (uint8_t)sy;
	SDL_GPUTextureCreateInfo tci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = vfmt, .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
		.width = TSX(v, tex->width), .height = TSY(v, tex->height), .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };   /* the target's scale: a view is the target's pixels */
	v->tex = SDL_CreateGPUTexture(gpu(), &tci);
	return v;
}
static int view_render(TexEntry *v, SDL_GPUTexture *src, int from_depth, const ViewU *u) {
	SDL_GPUGraphicsPipeline *pipe = view_pipeline_general(from_depth, (SDL_GPUTextureFormat)u->vfmt);   /* the pass may bind a baked variant instead (view_pipeline_for); this one must exist on any backend */
	if (!pipe || !v->tex) return 0;
	uint64_t t0 = us_now();
	/* the pass goes into the consuming scene's command buffer (batch_add_view): at its head when its
	 * source is another scene's target (nothing pending writes it, nothing pending sampled this view),
	 * else in draw order, where it sees what the pending draws drew (the scene's own target or depth)
	 * and the draws before it still sample the view as it was -- each was a flush, 19 a frame on Unit
	 * 13's level (bloom pyramid, lighting). RECOMP_GPU_VIEW_HOIST=0: as before, its own command
	 * buffer when no draw is pending (8 a frame there), in draw order otherwise */
	if (!batch_add_view(v->tex, src, from_depth, u)) {
		SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu());
		view_pass(cmd, v->tex, src, from_depth, u);
		SDL_SubmitGPUCommandBuffer(cmd);
	}
	t_view += us_now() - t0;
	return 1;
}
/* A mipmapped texture whose every level is a GPU colour target: level 0 rows of one, each other level a whole one at the chain's
 * address. Killzone's streak strip is one: the bottom 384x64 of its sRGB 384x192 target, with the four levels its own
 * sceGxmTransferDownscale chain makes (GPU targets since gfx_gpu_transfer_downscale). The guest-memory path below downloaded all
 * five every frame; this copies them into one mipmapped texture on the GPU, again only when one of them has new content. At the
 * targets' render scale (a level is its target's own texels). 0: not that shape, so the paths below take it. */
static TexEntry *mip_view(const GfxTexture *tex, TargetEntry *best, SDL_GPUTexture *l0, uint64_t l0gen) {   /* l0: level 0 from this texture of best's instead (its snapshot), l0gen its generation */
	const int n = gfx_texture_levels(tex);
	if (n < 2 || n > 16 || ((tex->u_mode >> 4) & 3) != 0 || (tex->format & 0xfff) || (tex->format & 0x9F007000u) != 0x0c000000u) return 0;   /* a linear U8U8U8U8 (ABGR) chain at its natural stride, by the texel format's own bits (31, 24-28, 12-14): Killzone samples the strip with a LOD bias and linear mips too (bits 15-23) */
	if (best->kind != TK_U8 && best->kind != TK_U8S) return 0;
	TargetEntry *lv[16]; uint32_t x0 = 0, y0 = 0; uint64_t sig = 1469598103934665603ULL;
	for (int l = 0; l < n; l++) {
		GfxTexture x; gfx_texture_level(tex, l, &x);
		TargetEntry *t = l ? 0 : best;
		for (int i = 0; l && i < ntargets; i++) { TargetEntry *c = &targets[i]; if (c->color && c->addr == x.data && c->width == x.width && c->height == x.height && (!t || c->gen > t->gen)) t = c; }
		if (!t || t->kind != best->kind || t->sx != best->sx || t->sy != best->sy || t->type != 0 || (scene_open && t == scene_entry && !(l == 0 && l0))) return 0;
		if (l == 0) {   /* the texture's rows inside its target, at the target's pitch */
			const uint32_t pitch = 4u * t->width, off = x.data - t->addr;
			if (x.data < t->addr || off % 4u || ((x.width + 7u) & ~7u) != t->width) return 0;
			x0 = off % pitch / 4u; y0 = off / pitch;
			if (x0 + x.width > t->width || y0 + x.height > t->height) return 0;
		}
		lv[l] = t; sig = (sig ^ (l == 0 && l0 ? l0gen : t->gen) ^ (uint64_t)(uintptr_t)t) * 1099511628211ULL;
	}
	const SDL_GPUTextureFormat fmt = kind_format(best->kind);
	TexEntry *v = 0;
	for (int i = 0; i < nviews; i++) { TexEntry *e = &views[i]; if (e->levels == n && e->addr == tex->data && e->format == tex->format && e->width == tex->width && e->height == tex->height && e->sx == best->sx && e->sy == best->sy && e->vfmt == fmt) { v = e; break; } }
	if (!v) {
		if (nviews < MAXVIEW) v = &views[nviews++];
		else { v = &views[0]; for (int i = 1; i < MAXVIEW; i++) if (views[i].used < v->used) v = &views[i];
			batch_retire(v->tex, "view released");
			if (v->tex) SDL_ReleaseGPUTexture(gpu(), v->tex);
			for (int a = 0; a < 4; a++) for (int b = 0; b < 2; b++) for (int c = 0; c < 3; c++) for (int d = 0; d < 3; d++) if (v->samp[a][b][c][d]) SDL_ReleaseGPUSampler(gpu(), v->samp[a][b][c][d]); }
		memset(v, 0, sizeof *v); v->addr = tex->data; v->format = tex->format; v->width = tex->width; v->height = tex->height; v->levels = n; v->vfmt = fmt; v->sx = best->sx; v->sy = best->sy;
		SDL_GPUTextureCreateInfo tci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = fmt, .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
			.width = TSX(v, tex->width), .height = TSY(v, tex->height), .layer_count_or_depth = 1, .num_levels = (Uint32)n, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
		v->tex = SDL_CreateGPUTexture(gpu(), &tci);
		if (!v->tex) { memset(v, 0, sizeof *v); return 0; }   /* (an empty slot: never matched) */
	}
	v->used = ++tex_clock; v->vfilt = tex->min_filter || tex->mag_filter; v->vlinear = best->kind == TK_U8S;
	if (v->view_gen != sig || v->view_src != best) {
		for (int l = l0 ? 1 : 0; l < n; l++) target_sync_for_gpu(lv[l]);   /* (guest memory newer after a CPU write: up first) */
		SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu());
		if (!cmd) return 0;
		SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
		for (int l = 0; l < n; l++) {
			GfxTexture x; gfx_texture_level(tex, l, &x);
			SDL_GPUTextureLocation from = { .texture = l == 0 && l0 ? l0 : lv[l]->color, .x = l ? 0 : TSX(best, x0), .y = l ? 0 : TSY(best, y0) }, to = { .texture = v->tex, .mip_level = (Uint32)l };
			SDL_CopyGPUTextureToTexture(cp, &from, &to, TSX(v, x.width), TSY(v, x.height), 1, false);
		}
		SDL_EndGPUCopyPass(cp);
		SDL_SubmitGPUCommandBuffer(cmd);   /* (ahead of the pending batch, which samples it: its sources were all written by work already submitted) */
		v->view_gen = sig; v->view_src = best; v->view_depth = 0;
	}
	return v;
}
/* the texture's binding when it reads a render target's memory: a view (returned), the target's
 * own colour texture (*exact set, 0 returned: the exact U8U8U8U8 case), or 0 with every overlapping
 * target synced to guest memory for the ordinary decode (no view known for the pair, or disabled) */
static TexEntry *target_view(const GfxTexture *tex, TargetEntry **exact) {
	*exact = 0;
	int snap = 0;   /* the scene's own pixels under the draw: as the scene began (same_region_read) */
	if (same_region_read(tex)) {
		if (scene_snap) snap = 1;
		else if (!scene_entry->self_read) { scene_entry->self_read = 1; fprintf(stderr, "    gpu: target %#x %ux%u read under its own draw (texture %#x %ux%u, draw clip %d,%d-%d,%d): its scenes start from guest memory\n", scene_entry->addr, scene_entry->width, scene_entry->height, tex->data, tex->width, tex->height, cur_wx0, cur_wy0, cur_wx1, cur_wy1); }
	}
	uint32_t span = tex_span(tex), xhi = tex->data + (span ? span : 4u * tex->width * tex->height);   /* the bytes it reads (a generous 16 a texel, as before, reached past small textures into the next target) */
	TargetEntry *best = 0; DepthEntry *dbest = 0;
	for (int ti = 0; ti < ntargets; ti++) {   /* the newest target holding the texture's first texel */
		TargetEntry *te2 = &targets[ti]; if (!te2->color) continue;
		GfxTarget tg = target_entry_as_gfx(te2);
		uint32_t thi = te2->addr + (uint32_t)gfx_target_bpp(&tg) * te2->width * te2->height;
		if (tex->data < te2->addr || tex->data >= thi) continue;
		if (!best || te2->gen > best->gen) best = te2;
		te2->sampled = 1;
	}
	const int x8u24 = (tex->format & 0xff000000u) == 0x15000000u;
	for (int i = 0; i < MAXDEPTHG; i++) {   /* the newest depth surface starting at the texture (an X8U24 one: or in its first two sample rows, the next sample of an MSAA surface's pixel -- Killzone reads its 4X pass's depth at +0 and +4) */
		DepthEntry *de = &depthsg[i]; if (!de->tex || (de->key != tex->data && !(x8u24 && tex->data > de->key && tex->data - de->key < 8u * de->ds_stride))) continue;
		if (!dbest || de->gen > dbest->gen) dbest = de;
	}
	int d16 = dbest && dbest->fmt == 0x02444000u;   /* a D16 surface, read back as a U16 texture (Unit 13's shadow atlas) */
	if (dbest && (tex->format & 0xff000000u) != (d16 ? 0x09000000u : 0x15000000u)) dbest = 0;   /* only an X8U24 (or, of D16, a U16) texture reads a depth surface; a scratch depth over a colour target's memory (Unit 13's final pass, never stored) doesn't make that memory depth */
	if (!best && !dbest) return 0;   /* (after the filter: a depth surface it ruled out, with no colour target here, went on to read best->kind of a null best -- Uncharted, right after an F2 had dropped every target) */
	int ex = best && best->addr == tex->data && (best->kind == TK_U8 || best->kind == TK_U8S) && (best->format & 0xF1800000u) == 0 && ((best->format >> 20) & 3) == 0
	         && (tex->format & ~0x30000u) == 0x0c000000u && ((tex->u_mode >> 4) & 3) == 0 && tex->width == best->width && tex->height == best->height;   /* (the texture's gamma bits aside: the shader's sRGB masks handle them) */
	{	/* RECOMP_GPU_TRACE_ADDR=hexaddr: a texture over that address -- which target or depth surface it resolves to, every
		 * candidate with its format, size, kind and age (a stale entry over reused memory read in another format) */
		static uint32_t ta = 1; if (ta == 1) { const char *v = getenv("RECOMP_GPU_TRACE_ADDR"); ta = v ? (uint32_t)strtoul(v, 0, 16) : 0; }
		if (ta && ta >= tex->data && ta < xhi) {
			static unsigned said; if (said++ < 200) {
				fprintf(stderr, "gpu trace addr: frame %llu texture %#x fmt %#x %ux%u u%#x v%#x: best target %#x fmt %#x %ux%u kind %d gen %llu%s, depth %#x%s\n", (unsigned long long)gpu_frame_no,
				        tex->data, tex->format, tex->width, tex->height, tex->u_mode, tex->v_mode, best ? best->addr : 0, best ? best->format : 0, best ? best->width : 0, best ? best->height : 0,
				        best ? best->kind : -1, best ? (unsigned long long)best->gen : 0ull, best == scene_entry ? " (the scene's own)" : "", dbest ? dbest->key : 0, snap ? " snap" : "");
				for (int ti = 0; ti < ntargets; ti++) { TargetEntry *t2 = &targets[ti]; if (!t2->color || t2 == best) continue;
					GfxTarget tg = target_entry_as_gfx(t2); uint32_t thi = t2->addr + (uint32_t)gfx_target_bpp(&tg) * t2->width * t2->height;
					if (tex->data < thi && xhi > t2->addr) fprintf(stderr, "gpu trace addr:   also over it: target %#x fmt %#x %ux%u kind %d gen %llu dirty %d/%d\n", t2->addr, t2->format, t2->width, t2->height, t2->kind, (unsigned long long)t2->gen, t2->gpu_dirty, t2->cpu_dirty); }
			}
		}
	}
	const int snap2 = snap && scene_snap == 2 && best == scene_entry && best->snap && !dbest;   /* the starting pixels in the GPU snapshot */
	if (snap && !snap2) { if (scene_snap == 2) snap_to_guest(scene_entry); goto guest; }
	if (best && !dbest && gfx_texture_levels(tex) > 1 && !no_views()) { TexEntry *mv = mip_view(tex, best, snap2 ? best->snap : 0, snap2 ? best->snap_gen : 0); if (mv) return mv; }   /* every level a GPU target (the transfer chain's) */
	if (ex && !snap2 && (!dbest || best->gen > dbest->gen) && !getenv("RECOMP_GPU_DEBUG_NO_RTT")) {
		if (best == scene_entry) batch_flush("scene target sampled");   /* sampled while it is the attachment (undefined either way): at least after the draws before it */
		*exact = best; return 0;
	}
	if (!no_views()) {
		ViewU u; TexEntry *v = 0;
		if (dbest && (!best || dbest->gen > best->gen)) {   /* the depth surface's content is the newest over this memory */
			for (int k = 0; k < 4; k++) u.swz[k] = (uint32_t)k;
			const uint32_t msx = dbest->msx ? dbest->msx : 1u, msy = dbest->msy ? dbest->msy : 1u;
			if (dbest->gpu_newer && view_mapping(dbest->key, dbest->ds_stride, dbest->w * msx, dbest->h * msy, dbest->type, d16 ? 2 : 4, tex, d16 ? 2 : 4, dbest->sx, dbest->sy, &u)) {   /* (the surface's memory: its samples) */
				u.mode = d16 ? 1u : 0u;   /* the depth view's quantisation: 16 bits, else 24 */
				if (msx > 1 || msy > 1) { u.tgt_w = dbest->w; u.tgt_h = dbest->h; u.ms_x = msx; u.ms_y = msy; }   /* a sample -> the texture's pixel holding it (drawn single-sampled) */
				SDL_GPUTextureFormat vfmt = view_format(1, (int)u.mode, 0, tex->min_filter || tex->mag_filter); u.vfmt = (uint32_t)vfmt;
				v = view_entry(tex, vfmt, dbest->sx, dbest->sy); v->vlinear = 0;
				if (v->view_src != dbest || v->view_gen != dbest->zgen || !v->view_depth) {
					if (trace_sync()) fprintf(stderr, "gpu trace: view of depth %#x %ux%u for texture %#x fmt %#x %ux%u u%#x (rebuilt)\n", dbest->key, dbest->w, dbest->h, tex->data, tex->format, tex->width, tex->height, tex->u_mode);
					if (view_render(v, dbest->tex, 1, &u)) { v->view_src = dbest; v->view_gen = dbest->zgen; v->view_depth = 1; } else v = 0;
				}
				if (v) return v;
			}
			if (trace_sync()) fprintf(stderr, "gpu trace: no view of depth %#x %ux%u (gpu newer %d, gen %llu) for texture %#x fmt %#x %ux%u u%#x: decoded from guest memory\n", dbest->key, dbest->w, dbest->h, dbest->gpu_newer, (unsigned long long)dbest->gen, tex->data, tex->format, tex->width, tex->height, tex->u_mode);
		} else if (best) {
			uint32_t tex_bpp; int mode = view_mode(best->format, tex, &tex_bpp, u.swz);
			GfxTarget tg = target_entry_as_gfx(best);
			/* an sRGB target: the view pass reads decoded values. Mode 0 (U8U8U8U8 as U8U8U8U8, a sub-rectangle) passes them on
			 * -- into an sRGB view texture (re-encoded to the same bytes) or RGBA32F (kept decoded) -- and the draw's shader treats
			 * the view as it treats the target bound directly (bind_unit's sRGB masks). The other reinterpretations read raw bytes:
			 * guest memory. Killzone's streak strip (the bottom 384x64 of an sRGB 384x192 target) went down twice a frame. */
			if (best->kind == TK_U8S && mode != 0) mode = -1;
			if (mode >= 0 && view_mapping(best->addr, best->width, best->width, best->height, best->type, (uint32_t)gfx_target_bpp(&tg), tex, tex_bpp, best->sx, best->sy, &u)) {   /* a TargetEntry's width is the surface stride */
				u.mode = (uint32_t)mode;
				int vfilt = tex->min_filter || tex->mag_filter;
				SDL_GPUTextureFormat vfmt = view_format(0, mode, best->kind, vfilt); u.vfmt = (uint32_t)vfmt;
				if (!snap2) target_sync_for_gpu(best);   /* guest memory may hold the newest bytes (a software draw, a replay's restore) */
				v = view_entry(tex, vfmt, best->sx, best->sy); v->vfilt = vfilt; v->vlinear = best->kind == TK_U8S;
				const uint64_t want = snap2 ? best->snap_gen : best->gen;   /* (a view of the snapshot: its own generation) */
				if (v->view_src != best || v->view_gen != want || v->view_depth) {
					if (trace_sync()) fprintf(stderr, "gpu trace: view of target %#x fmt %#x %ux%u (mode %d) for texture %#x fmt %#x %ux%u u%#x (rebuilt)\n", best->addr, best->format, best->width, best->height, mode, tex->data, tex->format, tex->width, tex->height, tex->u_mode);
					if (view_render(v, snap2 ? best->snap : best->color, 0, &u)) { v->view_src = best; v->view_gen = want; v->view_depth = 0; } else v = 0;
				}
				if (v) return v;
			}
			if (trace_sync()) fprintf(stderr, "gpu trace: no view of target %#x fmt %#x %ux%u type %u for texture %#x fmt %#x %ux%u u%#x: decoded from guest memory\n", best->addr, best->format, best->width, best->height, best->type, tex->data, tex->format, tex->width, tex->height, tex->u_mode);
		}
	}
	if (snap2) snap_to_guest(scene_entry);   /* a snapshot read no view serves: memory as the scene began, below */
	/* the guest-memory path: every overlapping target's GPU content goes to guest memory first, oldest
	 * first (as target_alias_sync) so the newest bytes land last, as in the Vita's memory. In array
	 * order, Uncharted's last-frame composite (U8, 720x408, the first half of the same memory) came down
	 * after this frame's F32F32 G-buffer and its lighting read the old final image as albedo and normals:
	 * ghost copies over the top half of the screen on every frame but the first (Linux, before Vulkan
	 * had views) */
guest:;
	TargetEntry *list[MAXTARGET]; int n = 0;
	for (int ti = 0; ti < ntargets; ti++) {
		TargetEntry *te2 = &targets[ti]; if (!te2->color || !te2->gpu_dirty || (snap && te2 == scene_entry)) continue;   /* (snap: memory holds its starting pixels) */
		GfxTarget tg = target_entry_as_gfx(te2);
		uint32_t thi = te2->addr + (uint32_t)gfx_target_bpp(&tg) * te2->width * te2->height;
		if (tex->data >= thi || xhi <= te2->addr) continue;
		list[n++] = te2;
	}
	for (int a = 1; a < n; a++) for (int b = a; b > 0 && list[b - 1]->gen > list[b]->gen; b--) { TargetEntry *x = list[b]; list[b] = list[b - 1]; list[b - 1] = x; }
	for (int k = 0; k < n; k++) {
		TargetEntry *te2 = list[k];
		if (trace_sync()) fprintf(stderr, "gpu trace: texture %#x fmt %#x %ux%u u%#x is a view of target %#x fmt %#x %ux%u kind %d\n", tex->data, tex->format, tex->width, tex->height, tex->u_mode, te2->addr, te2->format, te2->width, te2->height, te2->kind);
		sync_why = "(texture view)"; target_sync_for_cpu(te2);
	}
	return 0;
}

/* a draw's texture `tex` on a stage's unit `unit` (sampler slot `slot` of program `prog`, for the trace) as a
 * binding: render-to-texture first (target_view), else the decoded texture, else a dummy; the units the shader
 * must sRGB-decode / re-encode and those bound at the render scale (snap_fetch) are or-ed into the masks */
static uint8_t unit_sx[16], unit_sy[16];   /* the scale of the target (or view) each of the draw's fragment units samples (0: none) -- snap_fetch's block, tex_srgb word 3 */
static uint32_t snap_scale(uint32_t snap_mask) {   /* the block snap_fetch steps in: its snapped units' target's scale (one word for the draw: they should agree), else any bound target's, else the render scale */
	int pick = -1;
	for (int u = 0; u < 16; u++) if ((snap_mask >> u & 1) && unit_sx[u]) {
		if (pick < 0) pick = u;
		else if (unit_sx[u] != unit_sx[pick] || unit_sy[u] != unit_sy[pick]) { static int said; if (!said++) fprintf(stderr, "    gpu: a draw's snapped reads are of targets at different scales (%dx%d, %dx%d): the first's block for both\n", unit_sx[pick], unit_sy[pick], unit_sx[u], unit_sy[u]); }
	}
	for (int u = 0; u < 16 && pick < 0; u++) if (unit_sx[u]) pick = u;
	return pick < 0 ? (uint32_t)rscale_x | (uint32_t)rscale << 16 : (uint32_t)unit_sx[pick] | (uint32_t)unit_sy[pick] << 16;
}
static void bind_unit(const GfxTexture *tex, int unit, SDL_GPUTextureSamplerBinding *b, uint32_t *srgb_dec, uint32_t *srgb_enc, uint32_t *scaled_units, const char *stage, uint32_t prog, int slot) {
	/* render-to-texture: a texture over memory a GPU scene has rendered into (offscreen
	 * then composite; or, as Unit 13 does, a surface read back in other formats and
	 * layouts -- its F32F32 target sampled as RGBA8 and as a double-width S8S8S8S8 view 4
	 * bytes in). Only when the texture describes exactly the target -- same address and
	 * size, and a format whose texels are the target's RGBA8 texels -- is the GPU's colour
	 * texture sampled directly; otherwise the target is synced to guest memory (in its
	 * declared format, as gfx_soft.c stores it) and the texture decoded from there like
	 * any other, so every reinterpretation reads the real bytes. */
	TargetEntry *src_target = 0; uint64_t tb0 = us_now();
	TexEntry *te = (tex && tex->data) ? target_view(tex, &src_target) : 0;   /* render-to-texture: a view, the target itself, or guest memory (see target_view) */
	if (src_target) target_sync_for_gpu(src_target);   /* its newest content may be in guest memory (a software-fallback draw, a replay's restored page): the texture must hold it before being sampled -- sampled stale it was Metal's uninitialised fill, a whole magenta frame */
	if (!te && !src_target && tex && tex->data) te = get_texture(tex);
	t_bind += us_now() - tb0;
	SDL_GPUTexture *dummy = dummy_texture();
	b->texture = src_target ? src_target->color : te ? te->tex : dummy ? dummy : (scene_entry ? scene_entry->color : 0);
	if ((src_target || (te && te->view_src)) && unit >= 0 && unit < 16) {
		*scaled_units |= 1u << unit;
		if (stage[0] == 'f') { unit_sx[unit] = src_target ? src_target->sx : te->sx; unit_sy[unit] = src_target ? src_target->sy : te->sy; }
	}
	if (tex && tex->data && unit >= 0 && unit < 16) {   /* sRGB: a gamma texture over plain data decodes in the shader; a non-gamma one over an sRGB target re-encodes what the GPU decoded */
		int tg = GFX_TEX_GAMMA(tex->format) && !tex_wide(tex->format), st = (src_target && src_target->kind == TK_U8S) || (te && te->view_src && te->vlinear);
		if (tg && !st) *srgb_dec |= 1u << unit;
		if (!tg && st) *srgb_enc |= 1u << unit;
	}
	if (trace_sync() && tex) fprintf(stderr, "gpu trace: draw %u %s %#x slot %d unit %d tex %#x fmt %#x %ux%u u%#x -> %s%s\n", n_gpu_draws, stage, prog, slot, unit, tex->data, tex->format, tex->width, tex->height, tex->u_mode, src_target ? "target" : te && te->view_src ? "view" : te ? "texture" : "dummy", te && !te->view_src && tex_wide(tex->format) ? " (wide)" : "");
	b->sampler = te ? tex_sampler(te, tex->min_filter, tex->mag_filter, tex->u_mode & 15, tex->v_mode & 15, (int)GFX_TEX_MIP_LINEAR(tex->format))
	                : tex_sampler(&textures[0], tex ? tex->min_filter : 0, tex ? tex->mag_filter : 0, tex ? (tex->u_mode & 15) : 2, tex ? (tex->v_mode & 15) : 2, tex ? (int)GFX_TEX_MIP_LINEAR(tex->format) : 0);
}
/* ---------------------------------------------------------------- the draw batch
 * A scene's draws go to the GPU together: gfx_gpu_draw packs each draw's vertex records and indices
 * into the rings as before but only RECORDS the rest (pipeline, viewport, scissor, stencil
 * reference, uniforms, sampler bindings, the draw call); a flush then makes one command buffer --
 * one copy pass (one upload per ring for the whole contiguous range the batch used, plus the LDST
 * windows), the render passes issuing every recorded draw -- and submits it. Acquiring a command
 * buffer, a copy pass, a LOAD/STORE render pass and a submit PER DRAW cost ~15 ms of host time on
 * Unit 13's 654-draw level frame 757 (vertex upload 5.1 + pass 6.5 + submit 2.7 + acquire 0.7);
 * batched, the same work is ~2 ms in 10 command buffers, one per scene.
 * Inside the command buffer, two things end the scene's render pass and start another with LOAD
 * ops, in draw order: a FRAGCOLOR draw (the copy of the colour target it samples is taken in a copy
 * pass between the two) and a view built from a texture a pending draw wrote (target_view: Unit 13's
 * bloom pyramid samples each level of its atlas while drawing the next; its lighting pass samples
 * its own depth as X8U24) -- the view's own pass goes between the two.
 * A flush comes at the end of a scene and before anything outside the command buffer that reads what
 * the pending draws write or rewrites what they sample, since a command buffer submitted later runs
 * later: a download or an upload of the scene's target (a software fallback, a guest write over it),
 * the scene's target sampled directly, a texture re-uploaded or released while a pending draw binds
 * it, a ring wrap (then the submitted scene buffers' fences are waited for: see ring_fences_wait), RECOMP_GFX_CHECK (it compares
 * every draw's own result), present and sync. RECOMP_GPU_STATS's "gpu batch" line counts each. */
typedef struct {
	PipeEntry *pipe; uint8_t stencil_ref; int indexed, nsbuf, ntsb;
	SDL_GPUViewport vp; SDL_Rect sc;
	uint32_t vring_off, vbytes, iring_off, nidx, first_vertex;   /* this draw's region of each ring (bytes) */
	uint32_t ibytes; int isize;   /* its index ring region's size (a multiple of 4) and the index element size (2 or 4; the GPU-side fetch draws the game's 16-bit indices as they are) */
	int wring;   /* binds the LDST window ring as storage buffer 1 (its windows: placed when the draw is recorded) */
	SDL_GPUTextureSamplerBinding tsb[25];   /* the program's samplers (16), the framebuffer copy, the fragment LDST targets (8) */
	SDL_GPUTextureSamplerBinding vtsb[16]; int nvtsb;   /* the vertex program's textures (GfxDraw.vtex) */
	UsseDrawUniforms vu, fu;
	int qslot;   /* a visibility query (vis_query_add): its counter in qbuf, -1 for a real draw */
	int hud; float hud_b[4];   /* a HUD draw (hud_classify) and where it lands (NDC: min x, max x, min y, max y): hud_layout narrows its vp at the flush */
} BatchDraw;
/* between draws pos-1 and pos: the FRAGCOLOR copy of src into dst (view 0; the region (x, y, w, h): the draw's
 * scissor rectangle, all its fragments can read), or a view pass of src into dst (hoisted: a view of another
 * scene's target, put at pos 0 ahead of the scene's first render pass -- see batch_add_view). Kept sorted by pos. */
typedef struct { int pos, view, from_depth, hoisted, job; SDL_GPUTexture *src, *dst; uint32_t x, y, w, h; ViewU u; } BatchEvent;   /* job: a FRAGCOLOR copy's extent still being measured (fjobs[job - 1], fetch_join) */
static struct {
	BatchDraw *d; int n, cap;
	BatchEvent *ev; int nev, evcap;
	SDL_GPUTexture *color, *depth;
	int clear_ds, debug_clear; float clear_z; uint8_t clear_s;
	int nq; uint32_t qaddr[256]; uint8_t qset[256];   /* the batch's visibility queries: slot k counts into guest word qaddr[k] */
} batch;
static uint64_t t_flush;   /* every flush's time, counted into t_total by batch_flush itself wherever it happens (gfx_gpu_draw leaves the nested ones out of its own) */
static uint32_t n_batch_additive;   /* this frame's FRAGCOLOR draws blended ONE/ONE instead of copied (gfx_gpu_draw) */
static uint32_t n_fetch_narrowed;   /* this frame's FRAGCOLOR copies narrowed to their draw's extent (fetch_narrow) */
static uint32_t n_batch_cmd, n_batch_pass, n_batch_draws, n_batch_fetch, n_batch_views, n_views_hoisted, n_views_inorder, n_views_alone;   /* the view passes: at the head of the scene's command buffer, in draw order (a render-pass split), in their own command buffer (RECOMP_GPU_VIEW_HOIST=0) */
static struct { const char *why; uint32_t n; } flush_why[16]; static int n_flush_why;
/* A FRAGCOLOR copy narrowed to the pixels its draw can cover. The copy's bytes were the cost, not the pass split: at 2x and
 * 32:9 Killzone's ~130 copies a frame took 5 ms of a 12.6 ms median frame (7.8 ms with 1x1 copies, the same 750 frames), and
 * its FRAGCOLOR draws cover 0.1-1.5% of the target (a decal, a lit sprite) while each copied its whole scissor. The extent
 * is the draw's own vertex program run on the CPU over every distinct vertex (gfx_bounds_prepare/run/result, cached),
 * mapped as gfx_soft.c maps it, plus a guest pixel each side. The runs go to RECOMP_FETCH_BBOX_THREADS workers (half the
 * logical processors, 1 to 4: a Steam Deck's 8 give 4; 0 on the render thread), at ~2.5 us a vertex too slow for it, and the flush that issues the copies waits for them
 * (fetch_join). Anything it can't be sure of keeps the whole scissor: more than RECOMP_FETCH_BBOX_MAX indices (384; 0
 * never narrows), a vertex behind the eye, a program beyond the interpreter, vertex textures (a GPU target's guest bytes
 * may be stale) and HUD draws (hud_layout moves them at the flush). */
#define FN_JOBS 1024
static struct FetchJob {
	GfxBoundsJob *j; int ready;   /* ready: gfx_bounds_run is done (or the result was cached) */
	int vset, vdis; float xoff, xscale, yoff, yscale; uint32_t vw, h; int sx0, sy0, sx1, sy1, kx, ky;   /* the draw's viewport and scissor (guest pixels), the scene's render scale */
} fjobs[FN_JOBS];
static int fn_n, fn_next, fn_done, fn_threads = -1;
static pthread_mutex_t fn_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fn_work = PTHREAD_COND_INITIALIZER, fn_fin = PTHREAD_COND_INITIALIZER;
static void *fn_worker(void *arg) {
	(void)arg;
	pthread_mutex_lock(&fn_m);
	for (;;) {
		while (fn_next >= fn_n) pthread_cond_wait(&fn_work, &fn_m);
		struct FetchJob *f = &fjobs[fn_next++];
		pthread_mutex_unlock(&fn_m);
		gfx_bounds_run(f->j);
		pthread_mutex_lock(&fn_m);
		f->ready = 1; fn_done++; pthread_cond_signal(&fn_fin);
	}
	return 0;
}
static void fetch_apply(struct FetchJob *f, BatchEvent *e) {
	float b[6];
	if (!gfx_bounds_result(f->j, b)) return;
	float x[2], y[2];
	for (int k = 0; k < 2; k++) {
		float nx = b[k], ny = b[2 + k];
		if (f->vset && f->vdis) { x[k] = nx; y[k] = ny; }
		else if (f->vset) { x[k] = f->xoff + f->xscale * nx; y[k] = f->yoff + f->yscale * ny; }
		else { x[k] = (nx + 1) * 0.5f * (float)f->vw; y[k] = (1 - ny) * 0.5f * (float)f->h; }
	}
	float fx0 = floorf(fminf(x[0], x[1])) - 1, fx1 = ceilf(fmaxf(x[0], x[1])) + 1, fy0 = floorf(fminf(y[0], y[1])) - 1, fy1 = ceilf(fmaxf(y[0], y[1])) + 1;
	if (!(fx0 < fx1 && fy0 < fy1)) return;   /* (a NaN) */
	int x0 = fx0 > f->sx0 ? (int)fx0 : f->sx0, x1 = fx1 < f->sx1 ? (int)fx1 : f->sx1, y0 = fy0 > f->sy0 ? (int)fy0 : f->sy0, y1 = fy1 < f->sy1 ? (int)fy1 : f->sy1;
	if (x1 <= x0 || y1 <= y0) { x0 = f->sx0; y0 = f->sy0; x1 = x0 + 1; y1 = y0 + 1; }   /* off screen: draws nothing, a token copy keeps the event */
	e->x = (uint32_t)(x0 * f->kx); e->y = (uint32_t)(y0 * f->ky); e->w = (uint32_t)((x1 - x0) * f->kx); e->h = (uint32_t)((y1 - y0) * f->ky);
	n_fetch_narrowed++;
}
static void fetch_narrow(const GfxDraw *d, const ShaderEntry *vs, BatchEvent *e, uint32_t vw, int sx0, int sy0, int sx1, int sy1) {
	static int lim = -1; if (lim < 0) { const char *v = getenv("RECOMP_FETCH_BBOX_MAX"); lim = v ? atoi(v) : 384; if (lim > GFX_BOUNDS_MAX) lim = GFX_BOUNDS_MAX; }
	if (fn_threads < 0) {
		const char *v = getenv("RECOMP_FETCH_BBOX_THREADS"); int nc = plat_cpu_count() / 2; fn_threads = v ? atoi(v) : nc < 1 ? 1 : nc > 4 ? 4 : nc;
		for (int i = 0; i < fn_threads; i++) { pthread_t t; if (pthread_create(&t, 0, fn_worker, 0)) { fn_threads = i; break; } pthread_detach(t); }
	}
	if (!d->count || d->count > (uint32_t)lim || vs->nsmp || fn_n >= FN_JOBS) return;
	struct FetchJob *f = &fjobs[fn_n];   /* (fn_n moves only here and in fetch_join, both the render thread's) */
	if (!f->j && !(f->j = gfx_bounds_job_new())) return;
	int r = gfx_bounds_prepare(f->j, d);
	if (!r) return;
	f->vset = d->viewport_set; f->vdis = d->viewport_disabled; f->xoff = d->vp_xoff; f->xscale = d->vp_xscale; f->yoff = d->vp_yoff; f->yscale = d->vp_yscale;
	f->vw = vw; f->h = scene_entry->height; f->sx0 = sx0; f->sy0 = sy0; f->sx1 = sx1; f->sy1 = sy1; f->kx = scene_sx; f->ky = scene_sy;
	if (r == 2 || !fn_threads) { if (r == 1) gfx_bounds_run(f->j); fetch_apply(f, e); return; }   /* cached, or no workers: now */
	f->ready = 0; e->job = fn_n + 1;
	pthread_mutex_lock(&fn_m); fn_n++; pthread_cond_signal(&fn_work); pthread_mutex_unlock(&fn_m);
}
static void fetch_join(void) {   /* (batch_flush, before it issues the copies) every queued run done, each event its extent */
	if (!fn_n) return;
	pthread_mutex_lock(&fn_m); while (fn_done < fn_n) pthread_cond_wait(&fn_fin, &fn_m); pthread_mutex_unlock(&fn_m);
	for (int i = 0; i < batch.nev; i++) if (batch.ev[i].job) { fetch_apply(&fjobs[batch.ev[i].job - 1], &batch.ev[i]); batch.ev[i].job = 0; }
	pthread_mutex_lock(&fn_m); fn_n = fn_next = fn_done = 0; pthread_mutex_unlock(&fn_m);
}
static int batch_uses(SDL_GPUTexture *tex) {
	if (!tex) return 0;
	for (int i = 0; i < batch.n; i++) for (int k = 0; k < batch.d[i].ntsb; k++) if (batch.d[i].tsb[k].texture == tex) return 1;
	for (int i = 0; i < batch.n; i++) for (int k = 0; k < batch.d[i].nvtsb; k++) if (batch.d[i].vtsb[k].texture == tex) return 1;
	for (int i = 0; i < batch.nev; i++) if (batch.ev[i].src == tex || batch.ev[i].dst == tex) return 1;
	return 0;
}
static int batch_uses_pipe(const PipeEntry *p) { for (int i = 0; i < batch.n; i++) if (batch.d[i].pipe == p) return 1; return 0; }
static void batch_retire(SDL_GPUTexture *tex, const char *why) { if (tex && (batch.n || batch.nev) && batch_uses(tex)) batch_flush(why); }
static BatchDraw *batch_add(void) {
	if (batch.n == batch.cap) { batch.cap = batch.cap ? 2 * batch.cap : 256; batch.d = realloc(batch.d, (size_t)batch.cap * sizeof *batch.d); }
	BatchDraw *b = &batch.d[batch.n++]; b->hud = 0;
	return b;
}
static BatchEvent *batch_event(void) {
	if (batch.nev == batch.evcap) { batch.evcap = batch.evcap ? 2 * batch.evcap : 64; batch.ev = realloc(batch.ev, (size_t)batch.evcap * sizeof *batch.ev); }
	BatchEvent *e = &batch.ev[batch.nev++]; memset(e, 0, sizeof *e); e->pos = batch.n;
	return e;
}
/* view_render's pass, into whichever command buffer: the full-screen triangle decoding src into dst */
static void view_pass(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *dst, SDL_GPUTexture *src, int from_depth, const ViewU *u) {
	SDL_GPUColorTargetInfo ct = { .texture = dst, .load_op = SDL_GPU_LOADOP_DONT_CARE, .store_op = SDL_GPU_STOREOP_STORE };
	SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, 0);
	int baked; SDL_BindGPUGraphicsPipeline(rp, view_pipeline_for(from_depth, u, &baked));   /* RECOMP_GPU_VIEW_FAST: a per-view baked pipeline (no uniform buffer -- u is baked into it) or the general one at u's format */
	SDL_GPUTextureSamplerBinding b = { src, nearest_sampler() }; SDL_BindGPUFragmentSamplers(rp, 0, &b, 1);
	if (!baked) SDL_PushGPUFragmentUniformData(cmd, 0, u, sizeof *u);
	SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
	SDL_EndGPURenderPass(rp);
}
/* ---------------------------------------------------------------- GXM transfers on the GPU
 * sceGxmTransferDownscale (gxm_sony.c) runs on the CPU over guest memory, so a source the GPU drew had to come down first (a
 * fence wait on all the GPU work queued so far, then a download), and the destination went back up as a texture at its next
 * use. Killzone downscales its streak strip (the bottom 384x64 of an sRGB 384x192 target) four times a frame, each level the
 * next one's source. On a discrete GPU (the RTX laptop, Windows) that first download was 4.3 ms a frame, 42 s of a 7-minute
 * session (2026-10-23). When the source is a plain U8U8U8U8 colour target holding the rows whole, the box filter runs on the
 * GPU instead (the view shader's modes 7 and 8, on the stored bytes as the hardware's is). The destination becomes a GPU target
 * over its memory, at the source's render scale: the next level's source, and what later draws sample (target_view).
 * Guest memory under it is behind until something reads it there: readback_enqueue's rule as for a scene's target (a small
 * one read back a frame later, a sampled one watched), a CPU write's download, a capture (which takes the CPU path).
 * Off by default (RECOMP_GPU_TRANSFER=1 on): byte-exact on Metal and Vulkan, but on the RTX laptop the frame rate didn't move
 * (the level is GPU-bound there; the GPU did the extra passes instead, 2026-10-23). Returns 0 for the CPU path to do it. */
int gfxcap_active(void);   /* gfx_capture.c */
static int gpu_transfer_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_GPU_TRANSFER"); v = e && *e && *e != '0'; } return v; }
int gfx_gpu_transfer_downscale(uint32_t src, uint32_t sw, uint32_t sh, uint32_t sstride, uint32_t dst, uint32_t dstride) {
	if (!gpu() || !gpu_transfer_on() || gfxcap_active()) return 0;
	static int trace = -1; if (trace < 0) trace = getenv("RECOMP_GPU_TRANSFER_TRACE") != 0;   /* RECOMP_GPU_TRANSFER_TRACE=1: why a transfer stayed on the CPU */
#define DECLINE(why) do { if (trace) fprintf(stderr, "gpu transfer: downscale %ux%u %#x -> %#x stays on the CPU: %s\n", sw, sh, src, dst, why); return 0; } while (0)
	const uint32_t dw = sw / 2, dh = sh / 2;
	if (!dw || !dh || sstride % 4 || dstride != 4 * dw) DECLINE("its strides");   /* (a wider destination row: the pass would write the bytes past the rectangle too) */
	TargetEntry *s = 0;   /* the newest plain U8U8U8U8 colour target holding the source rows whole, at its own pitch */
	for (int i = 0; i < ntargets; i++) {
		TargetEntry *t = &targets[i];
		if (!t->color || t->type != 0 || (t->kind != TK_U8 && t->kind != TK_U8S) || (t->format & 0xF1800000u) || ((t->format >> 20) & 3)) continue;
		const uint32_t pitch = 4u * t->width; if (sstride != pitch || src < t->addr) continue;
		const uint32_t off = src - t->addr, x0 = off % pitch / 4u, y0 = off / pitch;
		if (off % 4u || x0 + sw > t->width || y0 + sh > t->height) continue;
		if (!s || t->gen > s->gen) s = t;
	}
	if (!s) {
		if (trace) for (int i = 0; i < ntargets; i++) { TargetEntry *t = &targets[i]; if (t->color && src >= t->addr && src < t->addr + 4u * t->width * t->height) fprintf(stderr, "gpu transfer:   over target %#x fmt %#x %ux%u kind %d type %u\n", t->addr, t->format, t->width, t->height, t->kind, t->type); }
		DECLINE("no plain U8U8U8U8 target holds the source");
	}
	if (scene_open && s == scene_entry) DECLINE("its source is the scene being drawn");
	if (dst < s->addr + 4u * s->width * s->height && dst + dstride * dh > s->addr) DECLINE("in place");   /* (in place, or into the scene being drawn: the CPU path's order) */
	GfxTarget dt = { dst, s->format, dw, dh, dw, 0 };
	TargetEntry *d = get_target(&dt);
	if (!d || (scene_open && d == scene_entry) || d->sx != s->sx || d->sy != s->sy || d->kind != s->kind) DECLINE("its destination target (scale, kind)");
	SDL_GPUGraphicsPipeline *p = view_pipeline_general(0, kind_format(d->kind));
	if (!p) DECLINE("no view pipeline");
	batch_flush("transfer");   /* the pending draws (which may write the source, or sample the destination as it was) first */
	target_sync_for_gpu(s);
	ViewU u = {0}; u.off_bytes = src - s->addr; u.tgt_stride = u.tgt_w = s->width; u.tgt_h = s->height; u.mode = s->kind == TK_U8S ? 8u : 7u;
	u.scale = s->sx; u.scale_y = s->sy; u.vfmt = (uint32_t)kind_format(d->kind);
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu());
	if (!cmd) return 0;
	SDL_GPUColorTargetInfo ct = { .texture = d->color, .load_op = SDL_GPU_LOADOP_DONT_CARE, .store_op = SDL_GPU_STOREOP_STORE };
	SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, 0);
	SDL_BindGPUGraphicsPipeline(rp, p);
	SDL_GPUTextureSamplerBinding b = { s->color, nearest_sampler() }; SDL_BindGPUFragmentSamplers(rp, 0, &b, 1);
	SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof u);
	SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
	SDL_EndGPURenderPass(rp);
	SDL_SubmitGPUCommandBuffer(cmd);
	d->cpu_dirty = 0; d->gpu_dirty = 1; d->gen = ++write_clock; d->prev_drawn_frame = d->drawn_frame; d->drawn_frame = gpu_frame_no; d->used = ++target_clock;
	readback_enqueue(d);
	return 1;
#undef DECLINE
}
/* A view pass recorded into the scene's command buffer. One sourced from the scene's own colour target or
 * depth surface goes in draw order (after the pending draws, which write it, before the draw that reads
 * the view: a render-pass split). One sourced from another scene's target is HOISTED to the head of the
 * command buffer, before the scene's first render pass: nothing pending writes its source (a batch holds
 * one scene's draws, all into the scene's attachments; an upload of the source flushes first, as its
 * src is pending -- batch_uses; a guest write only marks the target cpu_dirty) and nothing pending
 * sampled this view with older content (a view is rebuilt only when its source's gen changed, and a
 * pending draw that bound it keeps it in draw order below) -- so the pass reads the same data, writes
 * the same texture and runs in the same order relative to the scene that produced the source (already
 * submitted) as a pass in its own command buffer or a split would: byte-identical, without the
 * standalone command buffer (8 a frame on Unit 13's level) or the split's store and load of the
 * attachments at the render scale (2 a frame, its lighting scene's shadow mask and SSAO views).
 * Returns 0 only with RECOMP_GPU_VIEW_HOIST=0 and no pending draw: view_render then submits its own. */
static int view_hoist(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_GPU_VIEW_HOIST"); v = !(e && *e == '0'); } return v; }
static int batch_add_view(SDL_GPUTexture *dst, SDL_GPUTexture *src, int from_depth, const ViewU *u) {
	int self = scene_entry && (src == scene_entry->color || (scene_target.depth && src == scene_entry->depth));   /* (scene_entry, not batch.color/depth: those are set by the first recorded draw) */
	int hoist = view_hoist() && !self && !batch_uses(dst);
	if (!view_hoist() && !batch.n) { n_views_alone++; n_batch_views++; return 0; }
	BatchEvent *e = batch_event(); e->view = 1; e->dst = dst; e->src = src; e->from_depth = from_depth; e->u = *u;
	if (hoist) {   /* to the front, after the pos-0 events already there (in-order views of a scene without a draw yet, an earlier hoisted view): the list stays sorted by pos */
		int k = 0; while (k < batch.nev - 1 && batch.ev[k].pos == 0) k++;
		if (k < batch.nev - 1) { BatchEvent tmp = *e; memmove(&batch.ev[k + 1], &batch.ev[k], (size_t)(batch.nev - 1 - k) * sizeof *e); batch.ev[k] = tmp; e = &batch.ev[k]; }
		e->pos = 0; e->hoisted = 1; n_views_hoisted++;
	} else n_views_inorder++;
	n_batch_views++;
	return 1;
}
static int fence_stats_on(void); static void fence_mark(int present);   /* RECOMP_GPU_FENCE_STATS (defined before apply_render_scale, below) */
static char fm_tag[320];   /* RECOMP_GPU_FENCE_TRACE: the tag of the next fence mark, what the command buffer held (batch_flush fills it) */
/* the batch's HUD draws, laid out (at its flush: all of a scene's are known by then). Each draw is narrowed k times about
 * an anchor its group decides: a draw mostly (60%) inside another's box joins that one (the minimap's compass letter, an
 * icon on a banner); a group reaching the outer 6% of the screen keeps its distance from that edge, and so does a group
 * next to one that does (the Valor icon, its rank, the balance's symbol and digits, the labels above them: separate
 * draws side by side); a small
 * group about the centre keeps the centre (the crosshair's ticks); any other keeps its own centre -- objective markers
 * are drawn where the game projects their target (with the widened camera), so they stay on it. 16:9: the edge groups
 * close in to a centred 16:9 frame (their anchor the centre), the rest as in edges. */
static void hud_layout(void) {
	const float k = window_aspect_widen();
	int idx[256], n = 0;
	for (int i = 0; i < batch.n && n < 256; i++) if (batch.d[i].hud) idx[n++] = i;
	if (!n || k < 1.01f) return;
	int par[256]; float g[256][4];
	for (int a = 0; a < n; a++) {   /* containment: the largest earlier draw holding 60% of this one's box -- a panel's contents
		 * come after it; what was drawn before is behind it (Uncharted's main-menu labels under a dialog, which went with it) */
		const float *A = batch.d[idx[a]].hud_b; float areaA = (A[1] - A[0]) * (A[3] - A[2]), best = 0; par[a] = a;
		for (int c = 0; c < a; c++) {
			const float *C = batch.d[idx[c]].hud_b; float areaC = (C[1] - C[0]) * (C[3] - C[2]);
			if (areaC <= areaA || areaC <= best) continue;
			float ix = fminf(A[1], C[1]) - fmaxf(A[0], C[0]), iy = fminf(A[3], C[3]) - fmaxf(A[2], C[2]);
			if (ix > 0 && iy > 0 && ix * iy >= 0.6f * areaA) { par[a] = c; best = areaC; }
			else if (areaA <= 0 && A[0] >= C[0] && A[1] <= C[1] && A[2] >= C[2] && A[3] <= C[3]) { par[a] = c; best = areaC; }   /* (a degenerate box: a point or a line inside) */
		}
	}
	for (int a = 0; a < n; a++) { int r = a, guard = 0; while (par[r] != r && guard++ < 256) r = par[r]; par[a] = r; }
	for (int a = 0; a < n; a++) if (par[a] == a) memcpy(g[a], batch.d[idx[a]].hud_b, sizeof g[a]);
	for (int a = 0; a < n; a++) if (par[a] != a) { const float *A = batch.d[idx[a]].hud_b; float *G = g[par[a]];
		G[0] = fminf(G[0], A[0]); G[1] = fmaxf(G[1], A[1]); G[2] = fminf(G[2], A[2]); G[3] = fmaxf(G[3], A[3]); }
	/* a panel drawn as two halves meeting at the centre line is one group (Uncharted's dialogs: two mirrored quads, each
	 * reaching past its screen edge with transparent margin -- anchored to their edges, the dialog split in two on an
	 * ultrawide, its buttons with it; captures frame_1074 and frame_1853, 2026-10-22) */
	for (int a = 0; a < n; a++) if (par[a] == a) for (int c = 0; c < n; c++) if (c != a && par[c] == c) {
		const float *A = g[a], *C = g[c], hmin = fminf(A[3] - A[2], C[3] - C[2]), oy = fminf(A[3], C[3]) - fmaxf(A[2], C[2]);
		if (fabsf(A[1] - C[0]) > 0.04f || fabsf(A[1]) > 0.06f || A[0] > -0.2f || C[1] < 0.2f || oy < 0.5f * hmin) continue;
		if (fabsf(A[2] - C[2]) > 0.03f || fabsf(A[3] - C[3]) > 0.03f) continue;   /* mirrored halves share their top and bottom: not a neighbour that happens to touch the centre (Uncharted's chapter cards under its dialog) */
		for (int x = 0; x < n; x++) if (par[x] == c) par[x] = a;
		g[a][0] = fminf(A[0], C[0]); g[a][1] = fmaxf(A[1], C[1]); g[a][2] = fminf(A[2], C[2]); g[a][3] = fmaxf(A[3], C[3]);
	}
	float anc[256]; int edge[256];   /* per group root: its anchor, and whether it's an edge group (one that crosses the centre never is) */
	for (int a = 0; a < n; a++) if (par[a] == a) { edge[a] = g[a][0] < -0.88f && g[a][1] < 0 ? -1 : g[a][1] > 0.88f && g[a][0] > 0 ? 1 : 0; anc[a] = 0; }
	for (int pass = 0, changed = 1; changed && pass < 16; pass++) {   /* a row beside an edge group: the same edge */
		changed = 0;
		for (int a = 0; a < n; a++) if (par[a] == a && !edge[a] && !(g[a][0] < 0 && g[a][1] > 0)) for (int c = 0; c < n; c++) if (par[c] == c && edge[c]) {
			const float *A = g[a], *C = g[c];
			/* neighbours: side by side (or one above the other) -- from touching, a sliver of overlap included, to a small
			 * gap -- and lined up over half the smaller one's height (width); a wide overlap (subtitles across the
			 * minimap's box) is no neighbour */
			float gx = fmaxf(A[0], C[0]) - fminf(A[1], C[1]), gy = fmaxf(A[2], C[2]) - fminf(A[3], C[3]);   /* the gap on each axis, negative where they overlap */
			float ox = -gx, oy = -gy, wmin = fminf(A[1] - A[0], C[1] - C[0]), hmin = fminf(A[3] - A[2], C[3] - C[2]);
			int beside = gx > -0.02f && gx < 0.08f && oy > 0.5f * hmin, above = gy > -0.02f && gy < 0.08f && ox > 0.5f * wmin;
			if (beside || above) { edge[a] = edge[c]; changed = 1; break; }
		}
	}
	for (int a = 0; a < n; a++) if (par[a] == a) {
		const float cx = 0.5f * (g[a][0] + g[a][1]), w = g[a][1] - g[a][0];
		anc[a] = edge[a] ? (hud_mode_cur == 1 ? 0.0f : (float)edge[a]) : (fabsf(cx) < 0.15f && w < 0.5f) ? 0.0f : cx;
	}
	{	static int log = -1; if (log < 0) { const char *e = getenv("RECOMP_HUD_LOG"); log = e ? atoi(e) : 0; }
		if (log >= 2) for (int a = 0; a < n; a++) if (par[a] == a) {   /* RECOMP_HUD_LOG=2: the groups */
			int m = 0; for (int x = 0; x < n; x++) m += par[x] == a;
			fprintf(stderr, "    hud group %d: %d draws, x %.3f..%.3f y %.3f..%.3f, edge %d, anchor %.3f\n", a, m, g[a][0], g[a][1], g[a][2], g[a][3], edge[a], anc[a]);
		}
	}
	for (int a = 0; a < n; a++) {
		SDL_GPUViewport *vp = &batch.d[idx[a]].vp; const float A = anc[par[a]];
		vp->x += 0.5f * (A + 1.0f) * vp->w * (1.0f - 1.0f / k); vp->w /= k;   /* NDC A lands where it did; everything about it 1/k as wide */
		n_hud_draws++;
	}
}
static void batch_flush_body(const char *why);
static void batch_flush(const char *why) {
	if (batch_flushing) { static int said; if (!said++) fprintf(stderr, "    gpu: batch_flush (%s) called from inside batch_flush: ignored (a bug: report it)\n", why); return; }
	batch_flushing = 1; batch_flush_body(why); batch_flushing = 0;
	if (nview_released) view_released_free();   /* evicted view variants: nothing unsubmitted binds them now */
}
static void batch_flush_body(const char *why) {
	fetch_join();
	if (!batch.n && !batch.nev) return;   /* (events alone -- a scene whose views were built but whose draws never reached the GPU -- still go: their consumer may be a later scene) */
	hud_layout();
	for (int i = 0; i < batch.n; i++) if (pipeline_pending(batch.d[i].pipe)) build_wait(&batch.d[i].pipe->pending);   /* (gpu_overlap: recorded while building, all in flight at once) */
	uint64_t t0 = us_now();
	{ int i; for (i = 0; i < n_flush_why; i++) if (flush_why[i].why == why) break;
	  if (i == n_flush_why && n_flush_why < 16) { flush_why[n_flush_why].why = why; flush_why[n_flush_why++].n = 0; }
	  if (i < 16) flush_why[i].n++; }
	if (trace_sync()) fprintf(stderr, "gpu trace: flush %d draws, %d passes between them (%s) after %u gpu draws\n", batch.n, batch.nev, why, n_gpu_draws);
	n_batch_cmd++; n_batch_draws += (uint32_t)batch.n;
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu());
	t_acq += us_now() - t0;
	uint64_t tc0 = us_now();
	uint32_t vlo = ~0u, vhi = 0, ilo = ~0u, ihi = 0;   /* bytes; the fence below remembers them (ring_fence_add) */
	if (batch.n) {   /* the rings: the batch's draws took consecutive regions (a wrap flushes first), so one upload each covers them all -- alignment gaps between records included, harmless */
		for (int i = 0; i < batch.n; i++) {
			BatchDraw *b = &batch.d[i];
			if (b->vring_off < vlo) vlo = b->vring_off; if (b->vring_off + b->vbytes > vhi) vhi = b->vring_off + b->vbytes;
			if (b->indexed) { if (b->iring_off < ilo) ilo = b->iring_off; if (b->iring_off + b->ibytes > ihi) ihi = b->iring_off + b->ibytes; }
		}
		SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
		{ SDL_GPUTransferBufferLocation s = { .transfer_buffer = vring_xfer, .offset = vlo }; SDL_GPUBufferRegion r = { .buffer = vring, .offset = vlo, .size = vhi - vlo }; SDL_UploadToGPUBuffer(cp, &s, &r, false); }   /* its own region of the ring: nothing else in flight touches it */
		if (ihi > ilo) { SDL_GPUTransferBufferLocation s = { .transfer_buffer = iring_xfer, .offset = ilo }; SDL_GPUBufferRegion r = { .buffer = iring, .offset = ilo, .size = ihi - ilo }; SDL_UploadToGPUBuffer(cp, &s, &r, false); }
		if (bw_hi > bw_lo) { SDL_GPUTransferBufferLocation s = { .transfer_buffer = wring_xfer, .offset = 4 * bw_lo }; SDL_GPUBufferRegion r = { .buffer = wring, .offset = 4 * bw_lo, .size = 4 * (bw_hi - bw_lo) }; SDL_UploadToGPUBuffer(cp, &s, &r, false); }   /* the windows this batch placed (a wrap flushes first: one region) */
		if (batch.nq) { SDL_GPUTransferBufferLocation s = { .transfer_buffer = qzero, .offset = 0 }; SDL_GPUBufferRegion r = { .buffer = qbuf, .offset = 0, .size = 4u * (uint32_t)batch.nq }; SDL_UploadToGPUBuffer(cp, &s, &r, false); }   /* its visibility counters from 0 */
		SDL_EndGPUCopyPass(cp);
	} else if (bw_hi > bw_lo) {   /* windows placed for draws that went to the software path after all: up anyway, a later draw may reuse them */
		SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
		SDL_GPUTransferBufferLocation s = { .transfer_buffer = wring_xfer, .offset = 4 * bw_lo }; SDL_GPUBufferRegion r = { .buffer = wring, .offset = 4 * bw_lo, .size = 4 * (bw_hi - bw_lo) }; SDL_UploadToGPUBuffer(cp, &s, &r, false);
		SDL_EndGPUCopyPass(cp);
	}
	t_vbuf += us_now() - tc0;

	uint64_t tp0 = us_now(); uint32_t pass0 = n_batch_pass;
	int pos = 0, first = 1;   /* (the hoisted view passes, pos 0, come out of the loop before the scene's first render pass; a batch of events alone begins no render pass and leaves batch.color/depth unread) */
	for (int e = 0; e <= batch.nev; e++) {
		int end = e < batch.nev ? batch.ev[e].pos : batch.n;
		if (end > pos) {   /* the scene's draws [pos, end) in one render pass */
			SDL_GPUColorTargetInfo cti = { .texture = batch.color, .load_op = batch.debug_clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD, .clear_color = (SDL_FColor){0, 1, 0, 1}, .store_op = SDL_GPU_STOREOP_STORE };
			/* depth/stencil: cleared to the surface's background values by the scene's first pass
			 * unless force-loaded, loaded otherwise; always stored -- later passes and later scenes
			 * sharing the surface load it (force_store's guest-memory round trip isn't done: the
			 * combined format's download layout is backend-defined) */
			int clear = first && batch.clear_ds;
			SDL_GPUDepthStencilTargetInfo dti = { .texture = batch.depth, .clear_depth = batch.clear_z, .clear_stencil = batch.clear_s,
				.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD, .stencil_load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD,
				.store_op = SDL_GPU_STOREOP_STORE, .stencil_store_op = SDL_GPU_STOREOP_STORE };
			SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &cti, 1, batch.depth ? &dti : 0);
			n_batch_pass++; first = 0;
			/* the dynamic state between draws of the pass, set only when it changes (uniform data pushed
			 * stays bound for the later draws of the command buffer, across pipeline binds -- but a view
			 * pass in between pushes its own, so every pass starts from nothing) */
			PipeEntry *lpipe = 0; const BatchDraw *last = 0; int ibound = 0;
			for (int i = pos; i < end && !batch.debug_clear; i++) {
				BatchDraw *b = &batch.d[i];
				if (!b->pipe->pipe) continue;   /* (gpu_overlap: its pipeline failed to build -- dropped, the state before it still current) */
				if (b->pipe != lpipe) { SDL_BindGPUGraphicsPipeline(rp, b->pipe->pipe); lpipe = b->pipe; }
				/* SDL_GPU's viewport/scissor/stencil reference are dynamic state, not baked into the pipeline:
				 * a pass that never sets a viewport rasterises nothing (a real bug here once: no errors, the
				 * target left as Metal's magenta fill); a stencil reference never set stays 0 */
				if (!last || last->stencil_ref != b->stencil_ref) SDL_SetGPUStencilReference(rp, b->stencil_ref);
				if (!last || memcmp(&last->vp, &b->vp, sizeof b->vp)) SDL_SetGPUViewport(rp, &b->vp);
				if (!last || memcmp(&last->sc, &b->sc, sizeof b->sc)) SDL_SetGPUScissor(rp, &b->sc);
				if (!last || last->wring != b->wring || last->nsbuf != b->nsbuf) {
					SDL_GPUBuffer *sbufs[2] = { vring, b->wring ? wring : vring };   /* the shader declared 2 when it has LDST windows; a window-less draw of such a program binds the PA buffer twice */
					SDL_BindGPUVertexStorageBuffers(rp, 0, sbufs, (uint32_t)b->nsbuf);
				}
				if (!last || memcmp(&last->vu, &b->vu, sizeof b->vu)) SDL_PushGPUVertexUniformData(cmd, 0, &b->vu, sizeof b->vu);
				if (!last || memcmp(&last->fu, &b->fu, sizeof b->fu)) SDL_PushGPUFragmentUniformData(cmd, 0, &b->fu, sizeof b->fu);
				if (b->ntsb && (!last || last->ntsb != b->ntsb || memcmp(last->tsb, b->tsb, (size_t)b->ntsb * sizeof b->tsb[0]))) SDL_BindGPUFragmentSamplers(rp, 0, b->tsb, (uint32_t)b->ntsb);
				if (b->nvtsb && (!last || last->nvtsb != b->nvtsb || memcmp(last->vtsb, b->vtsb, (size_t)b->nvtsb * sizeof b->vtsb[0]))) SDL_BindGPUVertexSamplers(rp, 0, b->vtsb, (uint32_t)b->nvtsb);
				if (b->qslot >= 0) SDL_BindGPUFragmentStorageBuffers(rp, 0, &qbuf, 1);   /* a visibility query: its counters (the slot is in its uniforms) */
				if (b->indexed) {
					if (ibound != b->isize) { SDL_GPUBufferBinding ib = { .buffer = iring }; SDL_BindGPUIndexBuffer(rp, &ib, b->isize == 2 ? SDL_GPU_INDEXELEMENTSIZE_16BIT : SDL_GPU_INDEXELEMENTSIZE_32BIT); ibound = b->isize; }   /* the whole ring, rebound when the element size changes: each draw starts at its own first index */
					SDL_DrawGPUIndexedPrimitives(rp, b->nidx, 1, b->iring_off / (uint32_t)b->isize, (int32_t)b->first_vertex, 0);   /* gl_VertexIndex = index + first_vertex: this draw's records in the ring (the GPU-side fetch: the game's own index, first_vertex 0) */
				} else SDL_DrawGPUPrimitives(rp, b->nidx, 1, b->first_vertex, 0);   /* gl_VertexIndex = first_vertex + i */
				last = b;
			}
			SDL_EndGPURenderPass(rp);
			pos = end;
		}
		if (e == batch.nev) break;
		BatchEvent *ev = &batch.ev[e];
		if (ev->view) view_pass(cmd, ev->dst, ev->src, ev->from_depth, &ev->u);
		else {   /* the framebuffer copy the next draw's FRAGCOLOR reads: the colour target as every earlier draw left it */
			SDL_GPUCopyPass *fcp = SDL_BeginGPUCopyPass(cmd);
			SDL_GPUTextureLocation fsrc = { .texture = ev->src, .x = ev->x, .y = ev->y }, fdst = { .texture = ev->dst, .x = ev->x, .y = ev->y };
			SDL_CopyGPUTextureToTexture(fcp, &fsrc, &fdst, ev->w, ev->h, 1, false);
			SDL_EndGPUCopyPass(fcp);
		}
	}
	t_pass += us_now() - tp0;
	{ uint64_t ts0 = us_now(); SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd); t_submit += us_now() - ts0; ring_fence_add(f, vlo / 4, vhi / 4, ilo == ~0u ? 0 : ilo / 4, ilo == ~0u ? 0 : ihi / 4, bwr_lo == ~0u ? 0 : bwr_lo, bwr_hi); }
	bw_lo = bwr_lo = ~0u; bw_hi = bwr_hi = 0;
	vis_download();
	if (fence_stats_on()) {   /* RECOMP_GPU_FENCE_STATS: a mark after every scene submit (see fence_mark); tagged with what this command buffer held */
		int nv = 0, nf = 0, nh = 0; for (int i = 0; i < batch.nev; i++) { if (batch.ev[i].view) { nv++; nh += batch.ev[i].hoisted; } else nf++; }
		snprintf(fm_tag, sizeof fm_tag, "tgt %#x fmt %#x %ux%u rt %ux%u ms%ux%u ds %#x fmt %#x ld%d st%d: %d draws %u passes %d views %d fetchcopies (%s)%s", scene_target.data, scene_target.format, scene_target.width, scene_target.height, scene_target.rt_width, scene_target.rt_height, scene_target.ms_x, scene_target.ms_y, scene_target.ds_data, scene_target.ds_format, scene_target.ds_force_load, scene_target.ds_force_store, batch.n, n_batch_pass - pass0, nv, nf, why, nh ? " hoisted" : "");
		fence_mark(0);
	}
	batch.n = batch.nev = 0; batch.nq = 0;
	t_flush += us_now() - t0; t_total += us_now() - t0;
}

/* RECOMP_GFX_CHECK=1: after each GPU draw, run the exact same draw through the software path on
 * its own copy of the pre-draw target (snapshotted by the caller, in the target's own raw format,
 * right after the two were last agreed to be in sync) and diff the two results -- the software
 * path stays the reference the GPU path is checked against everywhere else, so this is just that
 * same check applied mechanically, per draw, instead of only by eye on a whole presented frame.
 * Reports only the first divergent draw for the whole run (this is meant to localise the first
 * bug, not enumerate every pixel of every later one it causes). */
static int gfx_check_on(void) { static int c = -1; if (c < 0) c = getenv("RECOMP_GFX_CHECK") != 0; return c; }
static int gfx_check_reported, full_reported;
/* RECOMP_GFX_CHECK=rgb: ignore alpha (a target that is only ever presented never shows it, and an
 * alpha-only difference in a clear otherwise stops the check before any visible divergence) */
static int gfx_check_channels(void) { static int n = 0; if (!n) { const char *e = getenv("RECOMP_GFX_CHECK"); n = e && !strcmp(e, "rgb") ? 3 : 4; } return n; }
static void gfx_check_draw(const GfxDraw *d, TargetEntry *e, int has_depth, const uint8_t *pre_raw, uint32_t pre_bytes) {
	if (gfx_check_reported >= 400 && full_reported) return;
	uint32_t n4 = 4u * e->width * e->height;
	uint8_t *gpu_rgba = malloc(n4);
	target_download_to_buffer(e, gpu_rgba);   /* the GPU's own result for this draw, straight off the texture -- doesn't touch guest memory */

	uint8_t *post_raw = malloc(pre_bytes); memcpy(post_raw, G(e->addr), pre_bytes);   /* whatever the real run currently has there (likely still pre_raw: only GPU draws touch guest memory via explicit syncs) */
	memcpy(G(e->addr), pre_raw, pre_bytes);   /* the agreed-upon starting point */
	/* textures that are live GPU render targets: the software draw reads them from guest memory */
	for (int u = 0; u < 16; u++) if (d->tex[u].data) for (int ti = 0; ti < ntargets; ti++)
		if (targets[ti].addr == d->tex[u].data && &targets[ti] != e) target_sync_for_cpu(&targets[ti]);
	GfxTarget gt = target_entry_as_gfx(e); gt.depth = has_depth;
	/* gfx_soft_draw's render() reads file-scope scene state (the colour target, the depth/stencil
	 * buffer) that only gfx_soft_begin_scene sets up -- in RECOMP_GFX=gpu that call never otherwise
	 * happens, so without this the "software reference" draw was a silent no-op (record_tri bails
	 * on scene_target.data==0) and soft_rgba below decoded the PRE-draw state, not a real render.
	 * NOTE: this resets the depth/stencil buffer to its cleared state rather than replaying this
	 * scene's actual accumulated depth history, so a depth-tested (not ALWAYS) draw deep in a 3D
	 * scene can still read as a false divergence here; good enough for the opaque/ALWAYS UI draws
	 * this is first catching, revisit if later reports turn out to be artifacts of this gap. */
	gfx_soft_begin_scene(&gt);
	gfx_soft_draw(d);                           /* same draw, software path, writing on top of that same starting point */
	gfx_soft_end_scene();                       /* the software path only records triangles at draw time; its flush rasterises them */
	uint8_t *soft_rgba = malloc(n4);
	gfx_decode_target_rgba8(&gt, soft_rgba);
	memcpy(G(e->addr), post_raw, pre_bytes);   /* restore: the GPU path stays authoritative for the real run */
	free(post_raw);

	int x0 = -1, y0 = -1, x1 = -1, y1 = -1, ndiff = 0;
	const int tol = 12;   /* absorbs float-path rounding, not a real divergence (both sides already decode through the same gfx_decode_target_rgba8) */
	for (uint32_t y = 0; y < e->height; y++) for (uint32_t x = 0; x < e->width; x++) {
		const uint8_t *a = gpu_rgba + 4 * (y * e->width + x), *b = soft_rgba + 4 * (y * e->width + x);
		int diff = 0; for (int k = 0; k < gfx_check_channels(); k++) if (abs((int)a[k] - (int)b[k]) > tol) diff = 1;
		if (!diff) continue;
		ndiff++;
		if (x0 < 0 || (int)x < x0) x0 = (int)x; if ((int)x > x1) x1 = (int)x;
		if (y0 < 0 || (int)y < y0) y0 = (int)y; if ((int)y > y1) y1 = (int)y;
	}
	/* RECOMP_GFX_CHECK_FPROG=<addr>: the full report (with image dumps) goes to the first divergent
	 * draw of that fragment program instead of the first divergent draw overall */
	static long want_fp = -2; if (want_fp == -2) { const char *e = getenv("RECOMP_GFX_CHECK_FPROG"); want_fp = e ? strtol(e, 0, 16) : -1; }
	static int full_done; int full = ndiff && !full_done && (want_fp < 0 || (uint32_t)want_fp == d->fprog);
	if (full) full_done = full_reported = 1;
	if (ndiff && !full && gfx_check_reported++ >= 0) {   /* the rest: one line per divergent draw */
		const GfxTexture *t0 = d->tex[0].data ? &d->tex[0] : 0;
		fprintf(stderr, "RECOMP_GFX_CHECK: also divergent -- vprog=%#x fprog=%#x target=%#x fmt=%#x blend=%d depth=%u/%d  %d px, bbox (%d,%d)-(%d,%d)  tex0 %s%#x\n",
		        d->vprog, d->fprog, e->addr, e->format, d->blend.present, d->depth_func, d->depth_write, ndiff, x0, y0, x1, y1,
		        t0 ? "fmt " : "", t0 ? t0->format : 0);
		ndiff = 0;
	}
	if (ndiff) {
		fprintf(stderr, "RECOMP_GFX_CHECK: first divergent draw -- vprog=%#x fprog=%#x target=%#x %ux%u fmt=%#x\n"
		                "  blend=%d depth_func=%u depth_write=%d stencil_func=%u cull=%d\n"
		                "  %d/%u pixels differ (tol %d), bbox (%d,%d)-(%d,%d)\n",
		        d->vprog, d->fprog, e->addr, e->width, e->height, e->format,
		        d->blend.present, d->depth_func, d->depth_write, d->stencil_func, d->cull,
		        ndiff, e->width * e->height, tol, x0, y0, x1, y1);
		FILE *f = fopen("/tmp/gfxcheck_gpu.ppm", "wb");
		if (f) { fprintf(f, "P6\n%u %u\n255\n", e->width, e->height); for (uint32_t i = 0; i < e->width * e->height; i++) fwrite(gpu_rgba + 4 * i, 1, 3, f); fclose(f); }
		f = fopen("/tmp/gfxcheck_soft.ppm", "wb");
		if (f) { fprintf(f, "P6\n%u %u\n255\n", e->width, e->height); for (uint32_t i = 0; i < e->width * e->height; i++) fwrite(soft_rgba + 4 * i, 1, 3, f); fclose(f); }
		fprintf(stderr, "  saved /tmp/gfxcheck_gpu.ppm, /tmp/gfxcheck_soft.ppm\n");
		/* the textures it could read, decoded as the software path sees them (/tmp/gfxcheck_tex<unit>.ppm),
		 * and which one the GPU path would bind instead if the address is a live render target */
		for (int u = 0; u < 16; u++) {
			const GfxTexture *t = &d->tex[u];
			if (!t->data || !t->width || !t->height) continue;
			int live = 0; for (int ti = 0; ti < ntargets; ti++) if (targets[ti].addr == t->data) live = 1;
			fprintf(stderr, "  tex unit %d: data %#x fmt %#x %ux%u u_mode %#x v_mode %#x%s\n", u, t->data, t->format, t->width, t->height,
			        t->u_mode, t->v_mode, live ? "  [live GPU render target: GPU samples its colour texture]" : "");
			uint8_t *rgba = malloc(4u * t->width * t->height); gfx_decode_texture_rgba8(t, rgba);
			char path[64]; snprintf(path, sizeof path, "/tmp/gfxcheck_tex%d.ppm", u);
			FILE *tf = fopen(path, "wb");
			if (tf) { fprintf(tf, "P6\n%u %u\n255\n", t->width, t->height); for (uint32_t i = 0; i < (uint32_t)t->width * t->height; i++) fwrite(rgba + 4 * i, 1, 3, tf); fclose(tf); }
			free(rgba);
		}
		{   /* the interpreter's clip-space positions for the first vertices, to set against where the GPU drew */
			const UsseProgram *vp = usse_get(d->vprog);
			UsseDraw vd = {0}; UsseInst in;
			usse_setup_registers(vp, &vd, d->vuniforms, d->vubuf);
			memset(&in, 0, sizeof in); usse_run_secondary(vp, &in, &vd);
			fprintf(stderr, "  prim %u count %u; interpreter positions:", d->prim, d->count);
			for (uint32_t i = 0; i < d->count && i < 6; i++) {
				gfx_fetch_vertex_pa(d, usse_vertex_layout(vp), index_at(d, i), &in);
				usse_run_primary(vp, &in, &vd, 0, 0);
				fprintf(stderr, " (%g %g %g %g)", in.o[0].f, in.o[1].f, in.o[2].f, in.o[3].f);
			}
			fprintf(stderr, "\n");
		}
		{ const UsseFragLayout *fl = usse_fragment_layout(usse_get(d->fprog)); if (fl) for (int i = 0; i < fl->niters; i++) if (fl->iters[i].is_tex) fprintf(stderr, "  iterator %d samples unit %d into PA%d\n", i, fl->iters[i].tex_unit, fl->iters[i].pa); }
	}
	free(gpu_rgba); free(soft_rgba);
}

/* one draw on the CPU instead (a program the translator doesn't cover, a pipeline that couldn't be
 * made): the target synced down, the draw rasterised, the target synced back up. The software path
 * records triangles at draw time and rasterises them at its scene's flush -- a bare gfx_soft_draw
 * drew nothing (Unit 13's menu text went missing). Its depth buffer starts cleared: a depth-tested
 * fallback draw doesn't see this scene's GPU depth (fine for the overlays that need this so far) */
/* RECOMP_GPU_STATS=1: how many draws ran where, printed at present */
static struct { const char *why; uint32_t n; } fallback_why[8]; static int n_fallback_why;
/* the GPU targets holding bytes a draw reads from guest memory -- its uniform buffers and vertex streams (and,
 * with `textures`, its textures of both stages) starting inside one -- go down to it first, oldest first so the
 * newest land last (target_view's order). A software-path draw reads everything from guest memory: it sampled
 * Killzone's bloom atlas and quarter-res targets as whatever bytes were last downloaded there, and its
 * fragment programs that read the HDR scene through uniform buffers (LDST, draw 141 of frame 2400) read an
 * old frame. A GPU draw's SA words come from guest memory too (usse_setup_registers). The current scene's
 * target is the caller's (soft_fallback syncs it; a GPU draw reading its own target is undefined anyway). */
static void sync_draw_reads(const GfxDraw *d, int textures, const UsseProgram *fp) {   /* fp: a GPU draw's fragment program -- its uniform buffers it reaches only through LDST pointers are read from the GPU target itself (emit_ldst_target), no download */
	uint32_t a[80]; int n = 0;
	for (int i = 0; i < 14; i++) { if (d->vubuf[i]) a[n++] = d->vubuf[i]; if (d->fubuf[i] && (!fp || usse_buffer_words(fp, i))) a[n++] = d->fubuf[i]; }
	if (d->vuniforms) a[n++] = d->vuniforms;
	if (d->funiforms) a[n++] = d->funiforms;
	for (int i = 0; i < d->nstream && i < 16; i++) if (d->stream[i]) a[n++] = d->stream[i];
	if (textures) for (int u = 0; u < 16; u++) { if (d->tex[u].data) a[n++] = d->tex[u].data; if (d->vtex[u].data) a[n++] = d->vtex[u].data; }
	TargetEntry *list[MAXTARGET]; int nl = 0;
	for (int ti = 0; ti < ntargets; ti++) {
		TargetEntry *te = &targets[ti]; if (!te->color || !te->gpu_dirty || te == scene_entry) continue;
		GfxTarget tg = target_entry_as_gfx(te);
		uint32_t lo = te->addr, hi = lo + (uint32_t)gfx_target_bpp(&tg) * te->width * te->height;
		for (int k = 0; k < n; k++) if (a[k] >= lo && a[k] < hi) { list[nl++] = te; break; }
	}
	for (int x = 1; x < nl; x++) for (int y = x; y > 0 && list[y - 1]->gen > list[y]->gen; y--) { TargetEntry *t = list[y]; list[y] = list[y - 1]; list[y - 1] = t; }
	for (int k = 0; k < nl; k++) { sync_why = textures ? "(software draw input)" : "(draw uniforms/streams)"; target_sync_for_cpu(list[k]); }
}
/* the software path's stencil compare (gfx_soft.c's depth_pass on stencil values): ref OP stencil */
static int stencil_passes(uint32_t f, uint8_t ref, uint8_t st) {
	switch (f & 7) { case 0: return 0; case 1: return ref < st; case 2: return ref == st; case 3: return ref <= st; case 4: return ref > st; case 5: return ref != st; case 6: return ref >= st; default: return 1; }
}
static void soft_fallback(const GfxDraw *d, const char *why) {
	int skip = scene_entry == &scratch_entry;
	if (skip) why = "depth-only scene: draw skipped";   /* the software path's depth buffer isn't the GPU's: drawn there, it would be lost */
	/* a draw whose stencil test fails at every pixel on the software path does nothing there: each fallback is a software
	 * scene of its own whose stencil starts at the background value (no force-load) and is never stored (no force-store) --
	 * the marks the scene's earlier draws made are in the GPU's depth/stencil texture, which the CPU can't read. Skipped
	 * whole: no target download or upload. Killzone's edge mask (0x11337280) tests the marks 0x11337a20 made where the
	 * quarter-res effects buffer has edges and stores upsampling classes into the composite's stencil plane: ~8 ms a frame
	 * of round trips that never wrote a byte. (On the hardware it runs at the marked pixels; that needs it on the GPU.) */
	else if (scene_target.depth && (!scene_target.ds_data || (scene_target.ds_format & 0x00022000u))
	    && !(scene_target.ds_data && (scene_target.ds_force_load || scene_target.ds_force_store))
	    && !stencil_passes(d->stencil_func, d->stencil_ref & d->stencil_cmask, (scene_target.ds_data ? scene_target.ds_background_stencil : 0) & d->stencil_cmask))
		why = "stencil test can't pass on the software path: draw skipped", skip = 1;
	__atomic_add_fetch(&n_fallback, 1, __ATOMIC_RELAXED); disp_note(DISP_SOFT);
	if (trace_sync()) fprintf(stderr, "gpu trace: software fallback (%s) after %u gpu draws, fprog %#x\n", why, n_gpu_draws, d->fprog);
	{ int i; for (i = 0; i < n_fallback_why; i++) if (fallback_why[i].why == why) break;
	  if (i == n_fallback_why && n_fallback_why < 8) { fallback_why[n_fallback_why].why = why; fallback_why[n_fallback_why++].n = 0; }   /* (a reason's count carried over from an earlier frame before this reset) */
	  if (i < 8) fallback_why[i].n++; }
	if (skip) return;
	target_sync_for_cpu(scene_entry);
	sync_draw_reads(d, 1, 0);
	gfx_soft_begin_scene(&scene_target);
	gfx_soft_draw(d);
	gfx_soft_end_scene();
	target_upload_from_guest(scene_entry);   /* keep the GPU texture consistent for any later GPU draw this scene, or present */
	scene_first_draw = 0;
}
/* ---- a draw's vertex data path. Default: the GPU-side fetch -- the raw vertex streams of the draw's index range [lo, hi]
 * are copied as they are into the vertex ring (one memcpy per stream), its 16-bit indices verbatim into the index ring, and
 * the vertex shader, specialised on the draw's attribute layout (usse_spirv.c's emit_vertex_fetch, usse_spirv.h's
 * UsseVfLayout), decodes each attribute itself. Before, the render thread converted every vertex of every draw into a PA
 * record through gfx_vertex_pack (9-10 ms of a 14 ms gfx_gpu_draw on Unit 13's shooting stretch: 190-200K records a frame)
 * and re-emitted the triangles as rebased 32-bit indices. The plan is the packer's own (gfx_vertex_packer_init's
 * GfxPackAttr list, slot by slot), so the shader builds the same words; a layout the packer calls `slow`, a draw whose copies
 * would exceed VF_MAX_BYTES or run past guest memory, or a sparse index range (the records are the cheaper upload) keeps
 * the packer and the PA-record shader variant. RECOMP_GPU_CPU_VFETCH=1: every draw on the packer, as before. Decided before
 * the vertex shader is looked up: the plan's hash is part of its key (ShaderEntry.layout_hash). */
static int cpu_vfetch(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_GPU_CPU_VFETCH") != 0; return v; }
#define VF_MAX_BYTES (8u << 20)
typedef struct {
	int gpu;                        /* 1: the streams go up raw, the shader fetches (vf/hash name its variant) */
	UsseVfLayout vf; uint32_t hash;
	uint32_t lo, hi, ntri, nidx;    /* the draw's index range and its triangles (3 indices each) */
	int nst; struct { int s; uint32_t src, bytes; } st[16];   /* the stream copies: guest address, byte count */
	uint32_t words;                 /* ring words every copy takes together (each 4-byte aligned, one slack word after it) */
} VfPlan;
static uint32_t hash32(const void *p, size_t n) { const uint8_t *b = p; uint32_t h = 2166136261u; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; } return h; }
static uint32_t attr_bytes(uint8_t fmt) { switch (fmt) { case 9: case 10: return 4; case 6: case 3: case 7: case 2: case 8: return 2; default: return 1; } }   /* gfx_soft.c's attr_byte_size */
static void vf_plan(const GfxDraw *d, const UsseVertexLayout *vl, int npa, VfPlan *p) {
	memset(p, 0, sizeof *p);
	uint32_t n = d->count; p->ntri = gfx_prim_tris(d->prim, n); p->nidx = 3 * p->ntri;
	if (g_points_draw) { p->ntri = 2 * n; p->nidx = 6 * n; }   /* (a point is two triangles: the CPU packer, each point's record six times) */
	uint32_t lo = ~0u, hi = 0; const uint8_t *ib = G(d->indices);   /* the index range, both paths need it */
	if (d->index_format) for (uint32_t i = 0; i < n; i++) { uint32_t ix; memcpy(&ix, ib + 4 * i, 4); if (ix < lo) lo = ix; if (ix > hi) hi = ix; }
	else for (uint32_t i = 0; i < n; i++) { uint16_t ix; memcpy(&ix, ib + 2 * i, 2); if (ix < lo) lo = ix; if (ix > hi) hi = ix; }
	p->lo = lo; p->hi = hi;
	if (cpu_vfetch() || !p->ntri || g_points_draw) return;
	GfxVertexPacker pk; gfx_vertex_packer_init(&pk, d, vl, npa);
	if (pk.slow) return;
	UsseVfLayout *vf = &p->vf; int j = 0;   /* the packer's plan, attribute by attribute, plus the stream each one reads (its j-th slotted attribute) */
	for (int i = 0; i < d->nattr && j < pk.n; i++) {
		const GfxAttr *a = &d->attr[i];
		if (!usse_find_attr(vl, a->reg)) continue;
		const GfxPackAttr *q = &pk.a[j];
		if (a->stream >= 16 || q->base != d->stream[a->stream] + a->offset || q->stride != d->stride[a->stream]) return;   /* (never: the two walks agree) */
		vf->a[j] = (UsseVfAttr){ a->stream, a->offset, d->stride[a->stream], q->reg, q->fmt, q->comps, q->kind, q->dcomps, q->rawlen };
		j++;
	}
	if (j != pk.n) return;
	vf->n = pk.n; vf->npa = npa; vf->index_reg = pk.index_reg; vf->instance_reg = pk.instance_reg;
	for (int k = 0; k < vf->n; k++) {   /* each stream's record extent: the furthest byte an attribute reads */
		int st = vf->a[k].stream, m;
		for (m = 0; m < p->nst && p->st[m].s != st; m++);
		if (m == p->nst) { p->st[p->nst].s = st; p->st[p->nst].bytes = 0; p->nst++; }
		uint32_t ext = vf->a[k].offset + (vf->a[k].kind == 1 ? vf->a[k].rawlen : vf->a[k].comps * attr_bytes(vf->a[k].fmt));
		if (ext > p->st[m].bytes) p->st[m].bytes = ext;
	}
	uint64_t total = 0;
	for (int m = 0; m < p->nst; m++) {   /* the copy: records lo..hi of the stream, [lo*stride, hi*stride + extent) */
		int st = p->st[m].s; uint32_t stride = d->stride[st];
		uint64_t src = (uint64_t)d->stream[st] + (uint64_t)lo * stride, bytes = (uint64_t)(hi - lo) * stride + p->st[m].bytes;
		if (src + bytes > (1ull << 32)) return;   /* past the end of guest memory */
		p->st[m].src = (uint32_t)src; p->st[m].bytes = (uint32_t)bytes;
		total += bytes; p->words += (uint32_t)((bytes + 3) / 4) + 1;
	}
	if (total > VF_MAX_BYTES || hi - lo + 1 > 4u * p->nidx + 4096 || p->words > VRING_WORDS || p->nidx > IRING_WORDS) return;
	p->hash = hash32(vf, sizeof *vf) | 1;   /* (the struct was zeroed: its padding hashes alike) */
	p->gpu = 1;
}
/* the LDST ring's recent windows, by guest address: a draw whose window holds the same 64 KB as one already placed in
 * this lap of the ring reads that one (consecutive draws of one skinned mesh share their palette) -- a compare
 * against a host copy instead of a copy into write-combined memory and its upload */
#define NLDSTWIN 32
typedef struct { uint32_t addr, off; uint64_t lap; uint8_t *host; } LdstWin;
static LdstWin ldst_win[NLDSTWIN]; static int ldst_win_next;
static uint32_t ldst_window_reuse(uint32_t addr) {
	for (int i = 0; i < NLDSTWIN; i++) {
		LdstWin *c = &ldst_win[i];
		if (c->host && c->addr == addr && c->lap == wring_lap && !memcmp(c->host, G(addr), USSE_LDST_WINDOW)) { n_ldst_reused++; return c->off; }
	}
	return ~0u;
}
static void ldst_window_placed(uint32_t addr, uint32_t off, uint32_t *dst) {
	LdstWin *c = 0;
	for (int i = 0; i < NLDSTWIN && !c; i++) if (ldst_win[i].host && ldst_win[i].addr == addr) c = &ldst_win[i];   /* (the same address's older bytes: replaced) */
	if (!c) { c = &ldst_win[ldst_win_next]; ldst_win_next = (ldst_win_next + 1) % NLDSTWIN; }
	if (!c->host && !(c->host = malloc(USSE_LDST_WINDOW))) return;
	memcpy(c->host, G(addr), USSE_LDST_WINDOW); memcpy(dst, c->host, USSE_LDST_WINDOW);
	c->addr = addr; c->off = off; c->lap = wring_lap; n_ldst_new++;
}
/* ---- the visibility test on the GPU (GfxDraw.vis_addr). SDL_GPU has no occlusion queries, so a draw with the test on is
 * preceded by a query draw: the same geometry, the draw's own depth and stencil tests with every write off, and a fragment
 * shader that runs after them ([[early_fragment_tests]]) and adds one to its counter in qbuf. A batch's counters are
 * zeroed in its copy pass, copied back by a command buffer submitted after it, and written to guest memory
 * (gfx_vis_write) when that one's fence has passed -- a frame or so after the scene, where the hardware writes them as
 * the scene finishes; Killzone's flares read them in their vertex programs, a frame late. Counted at the render scale,
 * so divided back to the guest's pixels. The fragment shader writes a buffer SDL binds for reading: Metal takes that as
 * it is, Vulkan with fragmentStoresAndAtomics (gfx_gpu_create_device asks for it); without, the counters stay as the
 * game left them. */
#define QSLOTS 256
typedef struct { SDL_GPUFence *f; SDL_GPUTransferBuffer *xb; int n; uint32_t addr[QSLOTS]; uint32_t div; } VisPending;
#define NVISP 8
static VisPending visp[NVISP]; static int visp_r, visp_n;
static int vis_pending(void) { return visp_n; }
static SDL_GPUShader *query_fragment_shader(void) {
	static SDL_GPUShader *sh; static int tried;
	if (tried) return sh;
	tried = 1;
	const int msl = (SDL_GetGPUShaderFormats(gpu()) & SDL_GPU_SHADERFORMAT_MSL) != 0;
	if (!msl && !vk_fragment_stores) { fprintf(stderr, "    gpu: visibility tests need Metal or Vulkan's fragmentStoresAndAtomics: their counters are left as the game set them\n"); return 0; }
	static const char *query_msl = "#include <metal_stdlib>\nusing namespace metal;\nstruct QU { uint slot; };\n"
		"[[early_fragment_tests]] fragment void main0(constant QU &q [[buffer(0)]], device atomic_uint *counts [[buffer(1)]]) { atomic_fetch_add_explicit(&counts[q.slot], 1u, memory_order_relaxed); }\n";
	SDL_GPUShaderCreateInfo ci = {0};
	if (msl) { ci.code = (const Uint8 *)query_msl; ci.code_size = strlen(query_msl) + 1; ci.format = SDL_GPU_SHADERFORMAT_MSL; ci.entrypoint = "main0"; }
	else { ci.code = (const Uint8 *)vk_vis_query_frag; ci.code_size = sizeof vk_vis_query_frag; ci.format = SDL_GPU_SHADERFORMAT_SPIRV; ci.entrypoint = "main"; }   /* (runtime/gfx/shaders/vis_query.frag) */
	ci.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
	ci.num_uniform_buffers = 1; ci.num_storage_buffers = 1;   /* (Metal: the uniform at buffer 0, storage buffers after it; Vulkan: set 2 binding 0, set 3 binding 0) */
	sh = SDL_CreateGPUShader(gpu(), &ci);
	if (!sh) { fprintf(stderr, "    gpu: visibility query shader: %s (counters left as the game set them)\n", SDL_GetError()); return 0; }
	SDL_GPUBufferCreateInfo bci = { .usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, .size = 4 * QSLOTS };
	SDL_GPUTransferBufferCreateInfo zci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = 4 * QSLOTS };
	qbuf = SDL_CreateGPUBuffer(gpu(), &bci); qzero = SDL_CreateGPUTransferBuffer(gpu(), &zci);
	if (qzero) { void *z = SDL_MapGPUTransferBuffer(gpu(), qzero, false); if (z) { memset(z, 0, 4 * QSLOTS); SDL_UnmapGPUTransferBuffer(gpu(), qzero); } }
	if (!qbuf || !qzero) { SDL_ReleaseGPUShader(gpu(), sh); sh = 0; }
	return sh;
}
typedef struct { const void *vse; int cull_sdl, has_depth, ckind, depth_ok, stencil_ok; uint32_t depth_func; uint8_t stencil_func, stencil_cmask; PipeEntry e; } QueryPipe;
#define MAXQPIPE 1024
static QueryPipe qpipes[MAXQPIPE]; static int nqpipes;
static PipeEntry *query_pipeline(const GfxDraw *d, const ShaderEntry *vs, int has_depth, int ckind, int depth_ok, int stencil_ok, int cull_sdl) {
	SDL_GPUShader *fs = query_fragment_shader(); if (!fs) return 0;
	uint8_t sf = stencil_ok && has_depth ? d->stencil_func : 7;
	for (int i = 0; i < nqpipes; i++) { QueryPipe *q = &qpipes[i];
		if (q->vse == vs && q->cull_sdl == cull_sdl && q->has_depth == has_depth && q->ckind == ckind && q->depth_ok == depth_ok && q->stencil_ok == stencil_ok && q->depth_func == d->depth_func && q->stencil_func == sf && q->stencil_cmask == d->stencil_cmask)
			return !pipeline_pending(&q->e) && q->e.pipe ? &q->e : 0; }   /* (still being built: no query this time, the counter keeps its last value) */
	if (nqpipes >= MAXQPIPE) { static int said; if (!said++) fprintf(stderr, "    gpu: the visibility-query pipeline table is full (%d): new queries keep their counters' last values\n", MAXQPIPE); return 0; }
	QueryPipe *q = &qpipes[nqpipes++]; memset(q, 0, sizeof *q);
	q->vse = vs; q->cull_sdl = cull_sdl; q->has_depth = has_depth; q->ckind = ckind; q->depth_ok = depth_ok; q->stencil_ok = stencil_ok; q->depth_func = d->depth_func; q->stencil_func = sf; q->stencil_cmask = d->stencil_cmask;
	SDL_GPUColorTargetDescription ctd = { .format = kind_format(ckind) }; ctd.blend_state.enable_color_write_mask = true; ctd.blend_state.color_write_mask = 0;
	SDL_GPUGraphicsPipelineCreateInfo ci = {0};
	ci.vertex_shader = vs->shader; ci.fragment_shader = fs; ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL; ci.rasterizer_state.cull_mode = (SDL_GPUCullMode)cull_sdl; ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE; ci.rasterizer_state.enable_depth_clip = false;
	ci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
	ci.depth_stencil_state.enable_depth_test = has_depth && depth_ok; ci.depth_stencil_state.enable_depth_write = false; ci.depth_stencil_state.compare_op = conv_cmp(d->depth_func);
	ci.depth_stencil_state.enable_stencil_test = sf != 7;   /* the test only: KEEP everywhere, nothing written */
	SDL_GPUStencilOpState sos = { SDL_GPU_STENCILOP_KEEP, SDL_GPU_STENCILOP_KEEP, SDL_GPU_STENCILOP_KEEP, conv_cmp(sf) };
	ci.depth_stencil_state.front_stencil_state = ci.depth_stencil_state.back_stencil_state = sos; ci.depth_stencil_state.compare_mask = d->stencil_cmask; ci.depth_stencil_state.write_mask = 0;
	ci.target_info.color_target_descriptions = &ctd; ci.target_info.num_color_targets = 1; ci.target_info.has_depth_stencil_target = has_depth; ci.target_info.depth_stencil_format = depth_format();
	/* built like every other pipeline (gpu_async: on a worker): a first-ever one is tens of ms, and a level's flares need
	 * dozens at once -- built here, Killzone's first frames with flares took 2-3 s each */
	BuildJob bj = { .is_pipe = 1, .pj = { &q->e, ci, ctd, d->vprog, d->fprog, us_now(), 0, (ShaderEntry *)vs } };   /* vse: a vertex shader still compiling (gpu_async) is waited for -- the pipeline was built without one, failed, and its tests went uncounted for good (Killzone: ~67 occlusion tests a frame, 2026-10-21) */
	q->e.pending = 1;
	if (!gpu_async() || !build_enqueue(&bj)) pipeline_build(&bj.pj);
	return !pipeline_pending(&q->e) && q->e.pipe ? &q->e : 0;
}
static void vis_unknown(const GfxDraw *d);   /* (below) */
static uint32_t n_vis_queries;
static void vis_query_add(const GfxDraw *d, const ShaderEntry *vs, int has_depth, int ckind, int depth_ok, int stencil_ok, int cull_sdl) {   /* the draw just recorded (the batch's last) gets its query ahead of it */
	if (batch.nq >= QSLOTS) { vis_unknown(d); return; }
	PipeEntry *qp = query_pipeline(d, vs, has_depth, ckind, depth_ok, stencil_ok, cull_sdl); if (!qp) { vis_unknown(d); return; }   /* (its pipeline still being built, or none: no count this time) */
	batch_add(); BatchDraw *real = &batch.d[batch.n - 1], *q = &batch.d[batch.n - 2];   /* (batch_add may move the array) */
	*real = *q;   /* the draw moves up one; its copy in front becomes the query */
	int slot = batch.nq++; batch.qaddr[slot] = d->vis_addr; batch.qset[slot] = d->vis_set; n_vis_queries++;
	q->pipe = qp; q->ntsb = 0; q->qslot = slot; memset(&q->fu, 0, sizeof q->fu); memcpy(&q->fu, &slot, 4);
}
/* the counters copied back since: written to guest memory once their fence has passed (all of them with `wait`) */
static void vis_complete(int wait) {
	while (visp_n) {
		VisPending *p = &visp[visp_r];
		if (!wait && !SDL_QueryGPUFence(gpu(), p->f)) break;
		SDL_WaitForGPUFences(gpu(), true, &p->f, 1);
		const uint32_t *c = SDL_MapGPUTransferBuffer(gpu(), p->xb, false);
		if (c) {
			uint32_t addr[QSLOTS], sum[QSLOTS]; int n = 0;
			for (int k = 0; k < p->n; k++) { int j; for (j = 0; j < n && addr[j] != p->addr[k]; j++) {} if (j == n) { addr[n] = p->addr[k]; sum[n] = 0; n++; } sum[j] += c[k]; }
			SDL_UnmapGPUTransferBuffer(gpu(), p->xb);
			for (int j = 0; j < n; j++) gfx_vis_write(addr[j], sum[j] ? (sum[j] + p->div - 1) / p->div : 0);
		}
		SDL_ReleaseGPUFence(gpu(), p->f); p->f = 0;
		visp_r = (visp_r + 1) % NVISP; visp_n--;
	}
}
/* after a batch with queries is submitted: its counters into a download (its own command buffer, which runs after it) */
static void vis_download(void) {
	if (!batch.nq) return;
	if (visp_n == NVISP) vis_complete(1);   /* (the GPU NVISP batches behind: wait for them) */
	VisPending *p = &visp[(visp_r + visp_n) % NVISP];
	if (!p->xb) { SDL_GPUTransferBufferCreateInfo xci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = 4 * QSLOTS }; p->xb = SDL_CreateGPUTransferBuffer(gpu(), &xci); }
	if (!p->xb) { batch.nq = 0; return; }
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu());
	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	SDL_GPUBufferRegion r = { .buffer = qbuf, .offset = 0, .size = 4u * (uint32_t)batch.nq }; SDL_GPUTransferBufferLocation l = { .transfer_buffer = p->xb, .offset = 0 };
	SDL_DownloadFromGPUBuffer(cp, &r, &l);
	SDL_EndGPUCopyPass(cp);
	p->f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
	p->n = batch.nq; memcpy(p->addr, batch.qaddr, sizeof(uint32_t) * (size_t)batch.nq); p->div = (uint32_t)(scene_sx * scene_sy); if (!p->div) p->div = 1;
	if (p->f) visp_n++;
	batch.nq = 0;
}
static uint32_t n_vis_unknown;   /* (RECOMP_GPU_STATS) a frame's visibility queries drawn, and SET tests answered visible unseen */
static void vis_unknown(const GfxDraw *d) { if (d->vis_addr && d->vis_set) { gfx_vis_write(d->vis_addr, 1); n_vis_unknown++; } }   /* a SET test (occlusion culling) this frame can't count: visible -- a 0 deletes the object (the popping of 2026-10-16) */
static void async_skip(const GfxDraw *d, const char *why) {   /* gpu_async: the draw waits for its build by not happening */
	n_async_skips++; vis_unknown(d); disp_note(DISP_SKIPPED);
	/* ...but it still writes its target, as far as which surface over that memory is newest goes: the target keeps its last
	 * frame's picture and views must go on taking it, not another target over the same memory that is now newer by write
	 * order. Uncharted's tonemap target is the memory of the next frame's other-format pass too; with its draw skipped the
	 * composite took that one, read as colour -- purple static for a few frames during movement (capture
	 * frame_3029, 2026-10-22: the live composite sampled the other pass's bytes; the replay, built in advance, was clean). */
	if (scene_entry && scene_entry != &scratch_entry) { scene_entry->gpu_dirty = 1; scene_entry->gen = ++write_clock; }
	if (trace_sync()) fprintf(stderr, "gpu trace: draw %#x/%#x skipped: %s\n", d->vprog, d->fprog, why);
}
/* gpu_async skips a draw whose shader or pipeline is still being built only on a target a scene drew into in the frame
 * before too: it is drawn again next frame, and the object shows up a frame or two late. A target drawn into now and then
 * -- a glyph atlas, a picture cached once -- keeps what is drawn into it, and a draw skipped there was lost for good:
 * Killzone rasterises each glyph into its font atlas the first time a string needs it, and its first menu text lost the
 * first seven ("T h i s g a m"), every boot, to the build of the glyph shader. Those draws wait for their build.
 * And only while the target still holds its own last frame: a scene that began by taking over another target's bytes at the
 * same memory (target_alias_sync: the same surface under another format of the same pixel size) would show those as its
 * own picture. Such a scene's draws wait too. */
static int skip_lossless(void) { return scene_entry && !scene_entry->alias_in && scene_entry->prev_drawn_frame && scene_entry->prev_drawn_frame + 1 >= scene_entry->drawn_frame; }
static uint64_t build_wait_since;   /* (atomic) when the render thread began waiting for a build (SDL_GetTicksNS), 0 none: gfx_gpu_build_status */
static uint64_t build_wait_total;   /* (atomic) all the waits so far, ns: a burst of short ones (a level's first load) adds up too */
static void build_wait(const int *pending) {
	uint64_t t0 = us_now();
	if (__atomic_load_n(pending, __ATOMIC_ACQUIRE)) __atomic_store_n(&build_wait_since, SDL_GetTicksNS() | 1, __ATOMIC_RELAXED);
	while (__atomic_load_n(pending, __ATOMIC_ACQUIRE)) usleep(100);
	const uint64_t w0 = __atomic_exchange_n(&build_wait_since, 0, __ATOMIC_RELAXED);
	if (w0) __atomic_add_fetch(&build_wait_total, SDL_GetTicksNS() - w0, __ATOMIC_RELAXED);
	t_shader += us_now() - t0;
}
/* for the window's "Compiling shaders" notice: how long the game has waited on a build so far (ms, 0 not waiting), all the
 * waits ever (ms, the current one included), and the shaders and pipelines queued or building on the workers */
/* ---- the shader recipes' loading and builds (see shader recipes, with get_shader) */
static uint64_t rc_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }   /* (us_now is 0 unless the stats are on) */
static pthread_mutex_t reg_m = PTHREAD_MUTEX_INITIALIZER; static uint32_t reg_q[1 << 14]; static int reg_qn;
/* hooks.c gxm_register, on the game thread registering it: sceGxmShaderPatcherRegisterProgram's GXP */
void gfx_gpu_program_registered(uint32_t gxp) {
	pthread_mutex_lock(&reg_m);
	if (reg_qn < (int)(sizeof reg_q / sizeof *reg_q)) reg_q[reg_qn++] = gxp;
	else { static int said; if (!said++) fprintf(stderr, "    gpu: shader recipes: over %d programs registered between two frames: the rest have no prebuild\n", reg_qn); }
	pthread_mutex_unlock(&reg_m);
}
static int parse_sa(const char *s, Recipe *r) {
	r->nsa = 0; if (!strcmp(s, "-")) return 1;
	while (*s) {
		unsigned i, u; int k;
		if (sscanf(s, "%x:%x%n", &i, &u, &k) != 2 || i > 255 || u > 255 || r->nsa >= 32) return 0;
		r->sa_idx[r->nsa] = (uint8_t)i; r->sa_unit[r->nsa++] = (uint8_t)u; s += k;
		if (*s == ',') s++; else if (*s) return 0;
	}
	return 1;
}
static int parse_vf(const char *s, Recipe *r) {
	UsseVfLayout *vf = &r->vf; memset(vf, 0, sizeof *vf); r->has_vf = 0;
	if (!strcmp(s, "-")) return 1;
	int n, npa, ir, inr, k;
	if (sscanf(s, "%d,%d,%d,%d%n", &n, &npa, &ir, &inr, &k) != 4 || n < 0 || n > 32) return 0;
	s += k;
	for (int j = 0; j < n; j++) {
		unsigned a[9];
		if (sscanf(s, "/%u,%u,%u,%u,%u,%u,%u,%u,%u%n", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7], &a[8], &k) != 9) return 0;
		vf->a[j] = (UsseVfAttr){ (uint16_t)a[0], (uint16_t)a[1], (uint16_t)a[2], (uint16_t)a[3], (uint8_t)a[4], (uint8_t)a[5], (uint8_t)a[6], (uint8_t)a[7], (uint8_t)a[8] };   /* (as vf_plan fills it: the hash covers the struct) */
		s += k;
	}
	if (*s) return 0;
	vf->n = n; vf->npa = npa; vf->index_reg = ir; vf->instance_reg = inr; r->has_vf = 1;
	return 1;
}
static int parse_shader(char *line, Recipe *r) {   /* "S <id> <text>": 0 if malformed or its id isn't its text's */
	unsigned long long id; char c, sa[1024], vf[4096]; int kind, points, fzero; unsigned h, h2, cube, snap;
	if (strlen(line) < 20 || line[18] != ' ' || sscanf(line + 2, "%16llx", &id) != 1) return 0;
	const char *text = line + 19;
	if (hash_bytes(text, strlen(text)) != id) return 0;
	if (sscanf(text, "%c %x %x %d %x %x %d %d %1023s %4095s", &c, &h, &h2, &kind, &cube, &snap, &points, &fzero, sa, vf) != 10 || (c != 'v' && c != 'f')) return 0;
	memset(r, 0, sizeof *r);
	r->id = id; r->is_frag = c == 'f'; r->hash = h; r->hash2 = c == 'f' && canon_vary_on() ? 0 : h2; r->kind = kind;   /* (a recipe from before canonical varyings: its pairing no longer matters) */ r->cube_mask = cube; r->snap_mask = snap; r->points = points; r->fzero = fzero;
	return parse_sa(sa, r) && parse_vf(vf, r);
}
static RecipeProg *rc_prog(uint32_t hash, int add) {   /* the programs the recipes name */
	if (!rc.prog) return 0;
	for (uint32_t i = (hash * 0x9E3779B1u) & (rc.prog_cap - 1); ; i = (i + 1) & (rc.prog_cap - 1)) {
		RecipeProg *p = &rc.prog[i];
		if (p->occ && p->hash == hash) return p;
		if (!p->occ) { if (!add) return 0; p->occ = 1; p->hash = hash; return p; }
	}
}
static char *rc_read(const char *path, size_t *len) {
	FILE *f = fopen(path, "rb"); if (!f) return 0;
	fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
	char *b = n >= 0 && n < (1L << 30) ? malloc((size_t)n + 1) : 0;
	if (b && fread(b, 1, (size_t)n, f) == (size_t)n) { b[n] = 0; *len = (size_t)n; } else { free(b); b = 0; }
	fclose(f); return b;
}
static const char *shader_cache_dir(void); static void warm_open(const char *dir);   /* (the warm-up, below) */
static void recipes_load(void) {   /* once, on the render thread: the shipped recipes and this machine's, and the file open for this session's */
	if (rc.state) return;
	rc.state = -1;
	const char *local = recipes_file(); if (!local) return;
	const char *shipped = getenv("RECOMP_SHADER_RECIPES_SHIPPED");
	const char *paths[2] = { shipped && *shipped ? shipped : 0, local };
	char *buf[2] = { 0, 0 }; size_t len[2] = { 0, 0 }; int local_ok = 0, nship = 0;
	const size_t hl = strlen(RECIPES_HEADER);
	for (int f = 0; f < 2; f++) {
		if (!paths[f] || !(buf[f] = rc_read(paths[f], &len[f]))) continue;
		if (len[f] > hl && !strncmp(buf[f], RECIPES_HEADER, hl) && buf[f][hl] == '\n') { if (f) local_ok = 1; continue; }
		if (f && len[f]) { char old[1200]; snprintf(old, sizeof old, "%s.old", local); rename(local, old); fprintf(stderr, "    gpu: shader recipes: %s is another version's (moved to %s): starting a new one\n", local, old); }
		else if (!f) fprintf(stderr, "    gpu: shader recipes: %s isn't this version's recipes: not used\n", paths[f]);
		free(buf[f]); buf[f] = 0; len[f] = 0;
	}
	char **lines = 0; int nl = 0, ns = 0, cap = 0, nship_lines = 0;   /* every line, NUL-terminated in place */
	for (int f = 0; f < 2; f++) {
		if (!buf[f]) continue;
		for (char *p = buf[f] + hl + 1, *end = buf[f] + len[f]; p < end; ) {
			char *nlp = memchr(p, '\n', (size_t)(end - p)); if (!nlp) break;   /* (a last line cut short: a session that ended mid-write) */
			*nlp = 0;
			if (nl == cap) { cap = cap ? 2 * cap : 4096; char **n2 = realloc(lines, cap * sizeof *n2); if (!n2) break; lines = n2; }
			lines[nl++] = p; ns += p[0] == 'S'; p = nlp + 1;
		}
		if (!f) nship_lines = nl;
	}
	uint32_t idcap = 64; while (idcap < 2u * (uint32_t)ns + 64) idcap *= 2;
	struct { uint64_t id; int ix; } *ids = calloc(idcap, sizeof *ids);
	rc.r = calloc((size_t)ns + 1, sizeof *rc.r);
	if (!ids || !rc.r) { free(ids); free(lines); free(buf[0]); free(buf[1]); return; }
	for (int i = 0; i < nl; i++) {   /* the shaders, then the pipelines between them */
		if (lines[i][0] != 'S' || lines[i][1] != ' ') continue;
		Recipe *r = &rc.r[rc.nr];
		if (!parse_shader(lines[i], r) || !rc_set_add(r->id)) continue;
		uint32_t k = (uint32_t)(r->id ^ r->id >> 32) & (idcap - 1); while (ids[k].id) k = (k + 1) & (idcap - 1);
		ids[k].id = r->id; ids[k].ix = rc.nr++; nship += i < nship_lines;
	}
	for (int i = 0; i < nl; i++) {
		if (lines[i][0] != 'P' || lines[i][1] != ' ') continue;
		unsigned long long a, b; int32_t v[NPV]; int k = 0, n;
		if (sscanf(lines[i], "P %16llx %16llx%n", &a, &b, &n) != 2) continue;
		const char *s = lines[i] + n;
		for (; k < NPV; k++) { int m; if (sscanf(s, " %d%n", &v[k], &m) != 1) break; s += m; }
		if (k < NPV || *s || !rc_set_add(hash_bytes(lines[i], strlen(lines[i])) ^ 0x5049ull << 48)) continue;
		int ia = -1, ib = -1;
		for (uint32_t j = (uint32_t)(a ^ a >> 32) & (idcap - 1); ids[j].id; j = (j + 1) & (idcap - 1)) if (ids[j].id == a) { ia = ids[j].ix; break; }
		for (uint32_t j = (uint32_t)(b ^ b >> 32) & (idcap - 1); ids[j].id; j = (j + 1) & (idcap - 1)) if (ids[j].id == b) { ib = ids[j].ix; break; }
		if (ia < 0 || ib < 0 || rc.r[ia].is_frag || !rc.r[ib].is_frag) continue;
		if (!(rc.np & 1023)) { PipeRecipe *p2 = realloc(rc.p, (rc.np + 1024) * sizeof *p2); PipeLink *l2 = realloc(rc.l, (2 * rc.np + 2048) * sizeof *l2); if (p2) rc.p = p2; if (l2) rc.l = l2; if (!p2 || !l2) break; }
		PipeRecipe *q = &rc.p[rc.np]; *q = (PipeRecipe){ .vs = ia, .fs = ib }; memcpy(q->v, v, sizeof v);   /* (all of it: rc.p grows by realloc, and a warm-up flag left from the heap -- wdone -- skipped up to 2 of 3 pipelines, a number that changed run to run) */
		rc.l[rc.nl] = (PipeLink){ rc.np, rc.r[ia].pipes }; rc.r[ia].pipes = ++rc.nl;
		rc.l[rc.nl] = (PipeLink){ rc.np, rc.r[ib].pipes }; rc.r[ib].pipes = ++rc.nl;
		rc.np++;
	}
	free(ids); free(lines); free(buf[0]); free(buf[1]);
	/* the programs they name: each one's recipes, found when the game registers it */
	for (rc.prog_cap = 64; rc.prog_cap < 4u * (uint32_t)rc.nr + 64; rc.prog_cap *= 2) {}
	rc.prog = calloc(rc.prog_cap, sizeof *rc.prog); rc.use = calloc(2 * (size_t)rc.nr + 1, sizeof *rc.use);
	if (!rc.prog || !rc.use) { free(rc.prog); rc.prog = 0; rc.nr = rc.np = 0; }
	for (int i = 0; i < rc.nr; i++) { rc_prog(rc.r[i].hash, 1)->nuse++; if (rc.r[i].is_frag && rc.r[i].hash2) rc_prog(rc.r[i].hash2, 1)->nuse++; }
	int at = 0;
	for (uint32_t i = 0; rc.prog && i < rc.prog_cap; i++) if (rc.prog[i].occ) { rc.prog[i].use = at; at += rc.prog[i].nuse; }
	for (int i = 0; i < rc.nr; i++) {
		RecipeProg *p = rc_prog(rc.r[i].hash, 0); rc.use[p->use + p->fill++] = i;
		if (rc.r[i].is_frag && rc.r[i].hash2) { p = rc_prog(rc.r[i].hash2, 0); rc.use[p->use + p->fill++] = i; }
	}
	/* this session's records go on the end of the local file (a new one if it was missing or another version's) */
	rc.fd = open(local, O_WRONLY | O_CREAT | O_APPEND | (local_ok ? 0 : O_TRUNC), 0644);
	if (rc.fd < 0) fprintf(stderr, "    gpu: shader recipes: cannot open %s (%s): not recording\n", local, strerror(errno));
	else if (!local_ok) { char h[64]; const int n = snprintf(h, sizeof h, "%s\n", RECIPES_HEADER); if (write(rc.fd, h, (size_t)n) != n) { close(rc.fd); rc.fd = -1; } }
	else { struct stat st; char last = '\n'; if (!fstat(rc.fd, &st) && st.st_size > 0) { int r2 = open(local, O_RDONLY); if (r2 >= 0) { if (pread(r2, &last, 1, st.st_size - 1) != 1) last = '\n'; close(r2); } }
		if (last != '\n' && write(rc.fd, "\n", 1) != 1) { close(rc.fd); rc.fd = -1; } }   /* (after a line cut short) */
	rc.state = 1;
	fprintf(stderr, "    gpu: shader recipes: %d shaders and %d pipelines (%d shaders from %s)%s\n", rc.nr, rc.np, nship, paths[0] ? paths[0] : "no shipped set", rc.fd < 0 ? ", not recording" : "");
	/* the loader: the recipes' programs out of the game's own files (gxp_index.c), for the warm-up (recipes_warm) */
	const char *off = getenv("RECOMP_SHADER_LOADER"), *cache = shader_cache_dir();
	char app0[1024], p[1100]; snprintf(p, sizeof p, "%s/app0", getenv("RECOMP_FS") ? getenv("RECOMP_FS") : "fs");
	uint32_t *want = malloc((2 * (size_t)rc.nr + 1) * sizeof *want); int nwant = 0;
	if (want && (rc.nr || predict_on()) && cache && !(off && *off == '0') && realpath(p, app0) && !(mkdir(cache, 0755) && errno != EEXIST)) {
		for (int i = 0; i < rc.nr; i++) { want[nwant++] = rc.r[i].hash; if (rc.r[i].is_frag && rc.r[i].hash2) want[nwant++] = rc.r[i].hash2; }
		warm_open(cache);
		gxp_index_start(app0, cache, predict_on() ? 0 : want, nwant);   /* (predicting: every program in the files) */
		rc.warm_phase = 1;
	}
	free(want);
}
static void rc_registered(uint32_t addr) {   /* a program the game registered: the recipes that name it, to try */
	if (!addr || memcmp(G(addr), "GXP", 4)) return;
	uint32_t size; RecipeProg *p = rc_prog(usse_content_hash(addr, &size), 0);
	if (!p) return;
	p->addr = addr; rc.regs++;
	if (rc.nwork + p->nuse > rc.work_cap) { int c = rc.work_cap ? rc.work_cap : 4096; while (c < rc.nwork + p->nuse) c *= 2; int *w = realloc(rc.work, c * sizeof *w); if (!w) return; rc.work = w; rc.work_cap = c; }
	for (int k = 0; k < p->nuse; k++) rc.work[rc.nwork++] = rc.use[p->use + k];
}
static void draw_of_state(const int32_t v[NPV], GfxDraw *d) {   /* the GfxDraw fields of pipe_state's record */
	memset(d, 0, sizeof *d);
	d->cull = v[0]; d->depth_write = v[7]; d->depth_func = (uint32_t)v[8];
	d->stencil_func = (uint8_t)v[9]; d->stencil_fail = (uint8_t)v[10]; d->depth_fail = (uint8_t)v[11]; d->depth_pass = (uint8_t)v[12]; d->stencil_cmask = (uint8_t)v[13]; d->stencil_wmask = (uint8_t)v[14]; d->fp_disabled = (uint8_t)v[15];
	d->blend.present = v[16]; d->blend.mask = (uint8_t)v[17]; d->blend.color_func = (uint8_t)v[18]; d->blend.alpha_func = (uint8_t)v[19];
	d->blend.color_src = (uint8_t)v[20]; d->blend.color_dst = (uint8_t)v[21]; d->blend.alpha_src = (uint8_t)v[22]; d->blend.alpha_dst = (uint8_t)v[23];
	d->depth_bias_factor = (int8_t)v[24]; d->depth_bias_units = (int8_t)v[25];
}
static void recipe_try_pipe(int i) {
	PipeRecipe *q = &rc.p[i]; const Recipe *a = &rc.r[q->vs], *b = &rc.r[q->fs];
	if (q->done || a->state != 1 || b->state != 1) return;
	q->done = 1;
	if (npipelines >= MAXPIPE * 3 / 4) { rc.skipped++; return; }
	const int32_t *v = q->v; GfxDraw d; draw_of_state(v, &d);
	const uint32_t made = n_pipes_made;
	get_pipeline(&d, a->e, b->e, v[2], v[3], v[4], v[5], v[1], v[6] && null_fs_on() && null_fragment_shader() != 0, 1);
	if (n_pipes_made != made) rc.built_pp++; else rc.have++;
}
static void recipe_try(int i) {   /* built once its programs are registered (a fragment shader's vertex program too) */
	Recipe *r = &rc.r[i];
	if (r->state) return;
	const RecipeProg *b = rc_prog(r->hash, 0), *pr = r->is_frag && r->hash2 ? rc_prog(r->hash2, 0) : 0;
	if (!b || !b->addr || (r->is_frag && r->hash2 && (!pr || !pr->addr))) return;
	if ((r->snap_mask && !any_scaled()) || nshaders >= MAXSHADER * 3 / 4) { r->state = 2; rc.skipped++; return; }   /* (snap variants: a draw at 1x never wants them) */
	uint32_t size;
	if (usse_content_hash(b->addr, &size) != r->hash || (pr && usse_content_hash(pr->addr, &size) != r->hash2)) { rc.stale++; return; }   /* (unregistered since, its memory reused: it may be registered again) */
	const UsseProgram *prog = usse_get(b->addr), *vprog = pr ? usse_get(pr->addr) : 0;
	const UsseVertexLayout *vl = r->is_frag ? (vprog ? usse_vertex_layout(vprog) : 0) : usse_vertex_layout(prog); const UsseFragLayout *fl = r->is_frag ? usse_fragment_layout(prog) : 0;
	uint32_t sa[256]; memset(sa, 0, sizeof sa); for (int k = 0; k < r->nsa; k++) sa[r->sa_idx[k]] = r->sa_unit[k];
	usse_spirv_key(prog, fl, sa, &r->kind, &r->cube_mask, &r->snap_mask);   /* (get_shader's key) */
	const uint32_t layout_hash = r->has_vf ? hash32(&r->vf, sizeof r->vf) | 1 : 0;   /* (vf_plan's) */
	const int pcf_unit = r->is_frag ? shadow_unit(b->addr, r->hash) : -1, pcf = pcf_unit >= 0 && shadow_filter_want() ? 16 : 0;
	ShaderEntry *e = shader_find(r->hash, r->hash2, r->kind, r->cube_mask, r->snap_mask, layout_hash, pcf, r->points, r->fzero);
	if (e) rc.have++;
	else if ((e = shader_new(b->addr, prog, r->hash, r->hash2, vl, fl, sa, r->kind, r->cube_mask, r->snap_mask, r->has_vf ? &r->vf : 0, layout_hash, pcf_unit, pcf, r->points, r->fzero, 1))) rc.built_sh++;
	if (!e || (!shader_pending(e) && !e->ok)) { r->state = 2; rc.skipped++; return; }
	if (!e->rid) e->rid = r->id;
	r->e = e; r->state = 1;
	for (int k = r->pipes; k; k = rc.l[k - 1].next) recipe_try_pipe(rc.l[k - 1].pipe);
}
/* ---- the warm-up: every recipe built once from the game's own files (gxp_index.c) and released, so that the OS's shader
 * cache holds it before the game first loads its programs -- a level's registration then builds at warm speed (Killzone's
 * level 1: ~950 builds in 0.4 s warm against ~7 s cold, on a load screen of 1-2 s). Nothing stays built: memory doesn't grow
 * with the number of levels the recipes cover. What it built is remembered (warm.txt in the shader cache: a key per shader
 * module and per pipeline), but every start builds them all again (warm_open: the OS's cache is shared and size-limited,
 * and lets builds go): a few seconds of the workers at OS-cache speed. RECOMP_SHADER_LOADER=0: off. */
#define WARM_HEADER "# ativ warm 1"
#define WARM_SCRATCH (1u << 20)
static const char *shader_cache_dir(void) { const char *e = getenv("RECOMP_SHADER_CACHE"); return e ? (*e && strcmp(e, "0") ? e : 0) : "shader_cache"; }
static void warm_fingerprint(char *out, size_t n) {
#ifdef _WIN32
	const char *rel = "windows";
#else
	struct utsname u; const char *rel = uname(&u) ? "?" : u.release;   /* (an OS update clears its shader cache) */
#endif
#ifdef RECOMP_SPIRV_CROSS
	snprintf(out, n, "%s %s %s", WARM_HEADER, MSL_CACHE_MAGIC, rel);
#else
	snprintf(out, n, "%s spirv %s", WARM_HEADER, rel);
#endif
}
static void warm_open(const char *dir) {   /* warm.txt: what earlier starts built (into rc_set), and the file open for this one's */
	char path[1100], fp[160], line[256]; snprintf(path, sizeof path, "%s/warm.txt", dir); warm_fingerprint(fp, sizeof fp);
	FILE *f = fopen(path, "r"); int same = 0, n = 0;
	if (f) {
		same = fgets(line, sizeof line, f) && !strncmp(line, fp, strlen(fp)) && line[strlen(fp)] == '\n';
		unsigned long long k; char t;
		/* counted, no longer trusted (2026-10-25): the OS's shader cache is shared by every program and size-limited (macOS:
		 * ~0.5 GB of rotating files), so builds a start made can be gone by the next -- a night of GPU test runs pushed out a
		 * Killzone level's, the warm-up skipped 8031 builds as warm, and ~270 shaders compiled cold mid-play (70-85 ms each).
		 * Every start builds every recipe again: at OS-cache speed when it still has them (well under a millisecond each), at
		 * boot rather than mid-play when it doesn't */
		while (same && fgets(line, sizeof line, f)) if (sscanf(line, "%c %16llx", &t, &k) == 2 && (t == 's' || t == 'p')) n++;
		fclose(f);
	}
	rc.warm_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);   /* (this start's builds, written afresh) */
	if (rc.warm_fd >= 0) { char h[200]; const int k = snprintf(h, sizeof h, "%s\n", fp); if (write(rc.warm_fd, h, (size_t)k) != k) { close(rc.warm_fd); rc.warm_fd = -1; } }
	if (rc.warm_fd >= 0) __atomic_store_n(&warm_record, 1, __ATOMIC_RELAXED);
	if (n) fprintf(stderr, "    gpu: shader recipes: %d builds made last start (%s): all made again, the OS's cache may have let some go\n", n, path);
}
static void warm_drain(void) {   /* the warm-up builds the workers finished: into warm.txt, and their objects released */
	pthread_mutex_lock(&warm_m);
	struct WarmDone *d = warm_dones; const int nd = nwarm_dones; struct WarmRel *r = warm_rels; const int nr = nwarm_rels;
	warm_dones = 0; nwarm_dones = warm_dones_cap = 0; warm_rels = 0; nwarm_rels = warm_rels_cap = 0;
	pthread_mutex_unlock(&warm_m);
	for (int i = 0; i < nd; i++) {
		if (!rc_set_add(d[i].key ^ (d[i].kind == 's' ? WTAG_S : WTAG_P)) || rc.warm_fd < 0) continue;
		char l[48]; const int k = snprintf(l, sizeof l, "%c %016llx\n", d[i].kind, (unsigned long long)d[i].key);
		if (write(rc.warm_fd, l, (size_t)k) != k) { close(rc.warm_fd); rc.warm_fd = -1; }
	}
	for (int i = 0; i < nr; i++) {
		if (r[i].pipe) SDL_ReleaseGPUGraphicsPipeline(gpu(), r[i].pipe);
		if (r[i].e) { if (r[i].e->shader) SDL_ReleaseGPUShader(gpu(), r[i].e->shader); free(r[i].e); }
	}
	free(d); free(r);
}
static int warm_scratch(void) {   /* guest memory the warm-up decodes programs in (usse_decode_uncached reads guest addresses) */
	if (!rc.scratch[1]) { extern uint32_t guest_alloc(uint32_t size, uint32_t align); rc.scratch[0] = guest_alloc(WARM_SCRATCH, 64); rc.scratch[1] = rc.scratch[0] ? guest_alloc(WARM_SCRATCH, 64) : 0; }
	return rc.scratch[1] != 0;
}
static int warm_translate(int i, ShaderEntry *e, ShaderJob *sj) {   /* recipe i translated from the game's files: 1, 0 failed, -1 its programs aren't there */
	const Recipe *r = &rc.r[i]; uint32_t sz, sz2 = 0;
	const uint8_t *b = gxp_index_program(r->hash, &sz), *b2 = r->is_frag && r->hash2 ? gxp_index_program(r->hash2, &sz2) : 0;
	if (!b || (r->is_frag && r->hash2 && !b2) || sz > WARM_SCRATCH || sz2 > WARM_SCRATCH || !warm_scratch()) return -1;
	memcpy(G(rc.scratch[0]), b, sz < 128 ? 128 : sz); if (b2) memcpy(G(rc.scratch[1]), b2, sz2 < 128 ? 128 : sz2);   /* (gxp_index keeps at least the 128 bytes the hash reads) */
	UsseProgram *prog = usse_decode_uncached(rc.scratch[0]), *vprog = b2 ? usse_decode_uncached(rc.scratch[1]) : 0;
	int ok = prog && usse_program_hash(prog) == r->hash && (!b2 || (vprog && usse_program_hash(vprog) == r->hash2));
	if (ok) {
		memset(e, 0, sizeof *e);
		e->addr = rc.scratch[0]; e->hash = r->hash; e->hash2 = r->hash2; e->kind = r->kind; e->cube_mask = r->cube_mask; e->snap_mask = r->snap_mask;
		e->layout_hash = r->has_vf ? hash32(&r->vf, sizeof r->vf) | 1 : 0; e->points = r->points; e->fzero = r->fzero; e->prebuilt = 1;   /* (prebuilt: no hitch log) */
		e->pcf_unit = r->is_frag ? shadow_unit(rc.scratch[0], r->hash) : -1; e->pcf = e->pcf_unit >= 0 && shadow_filter_want() ? 16 : 0;
		uint32_t sa[256]; memset(sa, 0, sizeof sa); for (int k = 0; k < r->nsa; k++) sa[r->sa_idx[k]] = r->sa_unit[k];
		const UsseFragLayout *fl = r->is_frag ? usse_fragment_layout(prog) : 0;
		usse_spirv_key(prog, fl, sa, &e->kind, &e->cube_mask, &e->snap_mask);   /* (get_shader's key) */
		ok = shader_translate(e, prog, r->is_frag ? (vprog ? usse_vertex_layout(vprog) : 0) : usse_vertex_layout(prog), fl, sa, r->has_vf ? &r->vf : 0, sj);
	}
	usse_program_free(prog); usse_program_free(vprog);
	return ok;
}
static uint64_t warm_shader_key(const ShaderJob *sj) { return sj->e->wkey; }   /* (shader_translate's) */
static uint64_t warm_pipe_key(const PipeRecipe *q) { return pipe_warm_key(rc.r[q->vs].wkey, rc.r[q->fs].wkey, q->v); }
static void warm_hold_done(Recipe *r) { if (r->wheld) { r->wheld = 0; warm_unref(r->we); } }   /* the warm-up has made every pipeline it will with r's shader */
static void warm_pipe(PipeRecipe *q) {   /* a pipeline of two warm-up shaders, built and released */
	Recipe *a = &rc.r[q->vs], *b = &rc.r[q->fs];
	q->wdone = 1;
	GfxDraw d; draw_of_state(q->v, &d);
	int null_fs = q->v[6] && null_fs_on() && null_fragment_shader() != 0;
	SDL_GPUGraphicsPipelineCreateInfo ci; SDL_GPUColorTargetDescription ctd;
	PipeEntry *p = calloc(1, sizeof *p);
	if (p) {   /* (its references to the shaders first: the warm-up's own may be the last ones, let go of below) */
		pipe_create_info(&d, a->we, b->we, q->v[2], q->v[3], q->v[4], q->v[5], q->v[1], &null_fs, &ci, &ctd);
		p->pending = 1; p->prebuilt = 1;
		__atomic_add_fetch(&a->we->wrefs, 1, __ATOMIC_ACQ_REL); if (!null_fs) __atomic_add_fetch(&b->we->wrefs, 1, __ATOMIC_ACQ_REL);
		BuildJob bj = { .is_pipe = 1, .low = 1, .pj = { p, ci, ctd, a->hash, b->hash, us_now(), 0, a->we, null_fs ? 0 : b->we, null_fs, 1, q->wkey } };
		if (!build_enqueue(&bj)) pipeline_build(&bj.pj);
		rc.w_pp++;
	}
	if (--a->wleft == 0) warm_hold_done(a);
	if (b != a && --b->wleft == 0) warm_hold_done(b);
}
static void recipes_warm_slice(uint64_t t0, uint64_t budget);
static void recipes_warm(uint64_t t0, uint64_t budget) {   /* (recipes_step, when no registration is being handled) a slice of the warm-up */
	if (rc.warm_phase == 1) { if (!gxp_index_ready()) return; rc.warm_phase = 2; rc.warm_at = 0; rc.warm_t0 = rc_us(); }
	recipes_warm_slice(t0, budget);
	rc.t_warm += rc_us() - t0;
}
static int pst_group(const int32_t *v, const int32_t *w) {   /* the same (blend, depth/stencil) state: what NVIDIA's fragment stage depends on (pipe_state: 2 has_depth, 4-5, 7-14 depth/stencil, 16-23 blend) */
	if (v[2] != w[2] || v[4] != w[4] || v[5] != w[5]) return 0;
	for (int k = 7; k <= 14; k++) if (v[k] != w[k]) return 0;
	for (int k = 16; k <= 23; k++) if (v[k] != w[k]) return 0;
	return 1;
}
static void pred_states(void) {   /* predicted pipelines' states: RECOMP_PIPE_PREDICT_STATES=<file> (a pipe_state record a line, or whole P lines), else the recipes' most common (blend, depth/stencil) states */
	rc.npst = 0;
	int want = 5; { const char *n = getenv("RECOMP_PIPE_PREDICT_N"); if (n && atoi(n) > 0) want = atoi(n) < 8 ? atoi(n) : 8; }
	const char *path = getenv("RECOMP_PIPE_PREDICT_STATES");
	FILE *f = path && *path ? fopen(path, "r") : 0;
	if (f) {
		char line[1024];
		while (rc.npst < want && fgets(line, sizeof line, f)) {
			char *q = line; if (*q == 'P') for (int skip = 0; skip < 3 && *q; skip++) { while (*q && *q != ' ') q++; while (*q == ' ') q++; }   /* (a P line: its ids skipped) */
			int32_t v[NPV]; int k = 0; char *end;
			for (; k < NPV; k++) { long x = strtol(q, &end, 10); if (end == q) break; v[k] = (int32_t)x; q = end; }
			if (k == NPV) memcpy(rc.pst[rc.npst++], v, sizeof v);
		}
		fclose(f);
		return;
	}
	enum { G = 128 }; static struct { int32_t v[NPV]; int n; } g[G]; int ng = 0;   /* (the groups, each with its first record as its state) */
	for (int i = 0; i < rc.np; i++) {
		const int32_t *v = rc.p[i].v;
		if (v[6] || v[15]) continue;   /* (no fragment shader of its own: the null one, or the program disabled) */
		int k = 0; while (k < ng && !pst_group(g[k].v, v)) k++;
		if (k == ng) { if (ng == G) continue; memcpy(g[ng].v, v, sizeof g[ng].v); g[ng].n = 0; ng++; }
		g[k].n++;
	}
	for (int n = 0; n < want && n < ng; n++) {   /* (the largest groups, in order) */
		int best = -1; for (int k = 0; k < ng; k++) if (g[k].n > 0 && (best < 0 || g[k].n > g[best].n)) best = k;
		if (best < 0) break;
		memcpy(rc.pst[rc.npst++], g[best].v, sizeof g[best].v); g[best].n = -1;
	}
}
static ShaderEntry *stand_in_vs(void) {   /* a vertex shader for predicted pipelines: the first program in the files that translates (its PA-record variant), built once and held */
	for (int i = 0; i < gxp_index_count(); i++) {
		uint32_t h = gxp_index_hash_at(i), sz; const uint8_t *b = gxp_index_program(h, &sz);
		if (!b || sz > WARM_SCRATCH || !warm_scratch()) continue;
		memcpy(G(rc.scratch[1]), b, sz < 128 ? 128 : sz);
		UsseProgram *prog = usse_decode_uncached(rc.scratch[1]);
		if (!prog || usse_program_hash(prog) != h || usse_is_fragment(prog)) { usse_program_free(prog); continue; }
		UsseDraw d; memset(&d, 0, sizeof d); usse_setup_registers(prog, &d, 0, 0);
		uint32_t sa[256]; for (int k = 0; k < 256; k++) sa[k] = d.sa_unit[k] ? d.sa_unit[k] - 1u : d.sa[k].u;
		ShaderEntry *e = calloc(1, sizeof *e); ShaderJob sj;
		if (!e) { usse_program_free(prog); return 0; }
		e->addr = rc.scratch[1]; e->hash = h; e->kind = -1; e->pcf_unit = -1; e->prebuilt = 1;
		const int ok = shader_translate(e, prog, usse_vertex_layout(prog), 0, sa, 0, &sj);
		usse_program_free(prog);
		if (!ok || e->nldst) { if (ok) usse_spirv_free(&sj.m); free(e); continue; }   /* (no LDST windows: nothing for them to point at) */
		e->warm = 1; e->wrefs = 2; e->pred = 1;   /* (its build's reference and the pass's, let go of when the pass is done) */
		shader_enqueue(&sj, 1);
		return e;
	}
	return 0;
}
static void predict_pipe(ShaderEntry *vs, ShaderEntry *fs, const int32_t v[NPV]) {   /* a predicted pipeline built and released (warm_pipe's), unless warm already */
	const uint64_t key = pipe_warm_key(vs->wkey, fs->wkey, v);
	if (*rc_slot(key ^ WTAG_P)) { rc.p_pp_warm++; return; }
	GfxDraw d; draw_of_state(v, &d);
	int null_fs = 0; SDL_GPUGraphicsPipelineCreateInfo ci; SDL_GPUColorTargetDescription ctd;
	PipeEntry *p = calloc(1, sizeof *p); if (!p) return;
	pipe_create_info(&d, vs, fs, v[2], v[3], v[4], v[5], v[1], &null_fs, &ci, &ctd);
	p->pending = 1; p->prebuilt = 1;
	__atomic_add_fetch(&vs->wrefs, 1, __ATOMIC_ACQ_REL); __atomic_add_fetch(&fs->wrefs, 1, __ATOMIC_ACQ_REL);
	BuildJob bj = { .is_pipe = 1, .low = 1, .pj = { p, ci, ctd, vs->hash, fs->hash, us_now(), 0, vs, fs, 0, 1, key } };
	if (!build_enqueue(&bj)) pipeline_build(&bj.pj);
	rc.p_pp++;
}
static void predict_warm(uint32_t hash) {   /* a program in the game's files: its predicted fragment shader (predict_on) built and released, unless warm already */
	if (rc_prog(hash, 0)) { rc.p_recorded++; return; }   /* (a recipe names it: the warm-up built it as recorded) */
	uint32_t sz; const uint8_t *b = gxp_index_program(hash, &sz);
	if (!b || sz > WARM_SCRATCH || !warm_scratch()) return;
	memcpy(G(rc.scratch[0]), b, sz < 128 ? 128 : sz);
	UsseProgram *prog = usse_decode_uncached(rc.scratch[0]);
	if (!prog || usse_program_hash(prog) != hash || !usse_is_fragment(prog)) { usse_program_free(prog); return; }
	rc.p_frag++;
	const UsseFragLayout *fl = usse_fragment_layout(prog);
	UsseDraw d; memset(&d, 0, sizeof d); usse_setup_registers(prog, &d, 0, 0);   /* (its literals and dependent-sampler table: no uniform buffers) */
	uint32_t sa[256]; for (int i = 0; i < 256; i++) sa[i] = d.sa_unit[i] ? d.sa_unit[i] - 1u : d.sa[i].u;   /* (gfx_gpu_draw's sa_for_smp) */
	const uint32_t cube = usse_spirv_predict_cube(prog, fl, sa);
	for (int k = 0; k < 3; k++) {   /* kind 0; and 1, 2 for a raw-word colour (usse_spirv_key keeps those) */
		int kind = k; uint32_t c = cube; usse_spirv_key(prog, fl, sa, &kind, &c, 0);
		if (kind != k) break;
		ShaderEntry *e = calloc(1, sizeof *e); ShaderJob sj;
		if (!e) break;
		e->addr = rc.scratch[0]; e->hash = hash; e->kind = kind; e->cube_mask = c; e->prebuilt = 1;   /* (prebuilt: no hitch log) */
		e->pcf_unit = shadow_unit(rc.scratch[0], hash); e->pcf = e->pcf_unit >= 0 && shadow_filter_want() ? 16 : 0;
		if (!shader_translate(e, prog, 0, fl, sa, 0, &sj)) { free(e); rc.p_fail++; continue; }
		const int warm = *rc_slot(e->wkey ^ WTAG_S) != 0;
		int need = !warm;   /* (or one of its predicted pipelines isn't warm) */
		for (int st = 0; !need && rc.stand_vs && st < rc.npst; st++) need = !*rc_slot(pipe_warm_key(rc.stand_vs->wkey, e->wkey, rc.pst[st]) ^ WTAG_P);
		if (!need) { usse_spirv_free(&sj.m); free(e); rc.p_warm++; rc.p_pp_warm += rc.stand_vs ? rc.npst : 0; continue; }
		e->warm = 1; e->wrefs = 2; e->pred = 1;   /* (references: its build's, and this pass's while it queues pipelines; pred: no MSL file) */
		if (warm) rc.p_warm++; else rc.p_sh++;
		shader_enqueue(&sj, 1);   /* (warm already but with pipelines to make: the module again -- on Vulkan it costs nothing) */
		for (int st = 0; rc.stand_vs && st < rc.npst; st++) predict_pipe(rc.stand_vs, e, rc.pst[st]);
		warm_unref(e);
	}
	usse_program_free(prog);
}
static int pred_low_cap = -1;   /* the low-priority workers' cap before the predicted pass lowered it (-1: not lowered) */
static void pred_throttle(int on) {   /* the predicted pass builds on at most RECOMP_SHADER_PREDICT_WORKERS (default 2) workers: a cold first
                                       * session's own builds queue behind whatever the OS's shader compiler already has (Killzone, every
                                       * shader cold: draws waited 9-17 s with the pass on 6 workers against 4.4 s without it) */
	pthread_mutex_lock(&bq_m);
	if (on && pred_low_cap < 0) { static int n = -1; if (n < 0) { const char *e = getenv("RECOMP_SHADER_PREDICT_WORKERS"); n = e && atoi(e) > 0 ? atoi(e) : 2; }
		pred_low_cap = bq_low_cap; if (bq_low_cap > n) bq_low_cap = n; }
	else if (!on && pred_low_cap >= 0) { bq_low_cap = pred_low_cap; pred_low_cap = -1; pthread_cond_broadcast(&bq_c); }
	pthread_mutex_unlock(&bq_m);
}
static void warm_archive(void) {   /* the recipes' warm-up is done (what follows, the predicted shaders, isn't archived): this machine's archive written (mtl_archive.m) */
#ifdef RECOMP_MTL_ARCHIVE
	if (!mtl_archive_on) return;
	fprintf(stderr, "    gpu: shader archive: so far %u shaders and %u view variants made from their kept libraries\n", __atomic_load_n(&n_shader_libs, __ATOMIC_RELAXED), __atomic_load_n(&n_view_libs, __ATOMIC_RELAXED));
	mtl_archive_report();
	mtl_archive_flush_soon(1);
#endif
}
void gfx_gpu_quit(void) {   /* the window closes (window.c: _exit, no atexit handlers): this machine's archive written first */
#ifdef RECOMP_MTL_ARCHIVE
	if (mtl_archive_on) mtl_archive_flush(2);
#endif
}
static void warm_recipes_done(void) {   /* the recipes' warm-up is queued: the predicted shaders next, or done */
	warm_archive();
	if (predict_on() && gxp_index_count()) {
		rc.warm_phase = 6; rc.pred_at = 0; rc.pred_t0 = rc_us(); pred_throttle(1);
		if (pipe_predict_on()) { pred_states(); if (rc.npst) rc.stand_vs = stand_in_vs(); }
	}
	else rc.warm_phase = 5;
}
static void recipes_warm_slice(uint64_t t0, uint64_t budget) {
	while (rc.warm_phase == 2 && rc_us() - t0 < budget) {   /* each recipe's keys */
		if (rc.warm_at == rc.nr) {   /* which builds aren't warm yet */
			for (int i = 0; i < rc.np; i++) {
				PipeRecipe *q = &rc.p[i];
				if (rc.r[q->vs].wstate != 1 || rc.r[q->fs].wstate != 1) continue;
				q->wkey = warm_pipe_key(q); q->wneed = !*rc_slot(q->wkey ^ WTAG_P);
				if (q->wneed) { rc.r[q->vs].wleft++; if (q->fs != q->vs) rc.r[q->fs].wleft++; rc.w_need_pp++; }
			}
			for (int i = 0; i < rc.nr; i++) { Recipe *r = &rc.r[i]; r->wneed = r->wstate == 1 && (r->wleft || !*rc_slot(r->wkey ^ WTAG_S)); rc.w_need_sh += r->wneed; }
			fprintf(stderr, "    gpu: shader recipes: warm-up: %d of %d shaders translated from the game's files (%d built for this session already or not at this render scale, %d not in the files); %d shaders and %d pipelines aren't warm yet (%.0f ms of the render thread over %.1f s)\n",
			        rc.w_keyed, rc.nr, rc.w_built, rc.w_missing, rc.w_need_sh, rc.w_need_pp, (rc.t_warm + rc_us() - t0) / 1e3, (rc_us() - rc.warm_t0) / 1e6);
			rc.warm_at = 0; if (rc.w_need_sh) rc.warm_phase = 3; else warm_recipes_done();
			break;
		}
		Recipe *r = &rc.r[rc.warm_at++];
		if (r->state || (r->snap_mask && !any_scaled())) { r->wstate = 2; rc.w_built++; continue; }   /* (built for this session already; snap variants at 1x) */
		ShaderEntry tmp; ShaderJob sj; const int k = warm_translate((int)(r - rc.r), &tmp, &sj);
		if (k == 1) { r->wkey = warm_shader_key(&sj); usse_spirv_free(&sj.m); r->wstate = 1; rc.w_keyed++; }
		else { r->wstate = 2; rc.w_missing += k < 0; }
	}
	while (rc.warm_phase == 3 && rc_us() - t0 < budget) {   /* the builds, in the order play first used them */
		pthread_mutex_lock(&bq_m); const int queued = bq_low; pthread_mutex_unlock(&bq_m);
		if (queued > 48) break;   /* (a few dozen modules waiting at a time, not thousands) */
		if (rc.warm_at == rc.nr) {
			for (int i = 0; i < rc.nr; i++) warm_hold_done(&rc.r[i]);   /* (pipelines whose other shader failed) */
			rc.warm_phase = 4;
			break;
		}
		const int i = rc.warm_at++; Recipe *r = &rc.r[i];
		if (!r->wneed) continue;
		ShaderEntry *e = calloc(1, sizeof *e); ShaderJob sj;
		const int k = e ? warm_translate(i, e, &sj) : -1;
		if (k != 1 || warm_shader_key(&sj) != r->wkey) {   /* (not as it was keyed: its pipelines go unbuilt) */
			rc.w_fail++;
			if (k == 1) usse_spirv_free(&sj.m);
			free(e); r->wneed = 0;
			for (int l = r->pipes; l; l = rc.l[l - 1].next) { PipeRecipe *q = &rc.p[rc.l[l - 1].pipe];
				if (!q->wneed || q->wdone) continue;
				q->wdone = 1; Recipe *o = &rc.r[q->vs == i ? q->fs : q->vs]; if (o != r && --o->wleft == 0) warm_hold_done(o); }
			continue;
		}
		e->warm = 1; e->wrefs = 2; e->wkey = r->wkey; r->we = e; r->wheld = 1;   /* (two references: its build's and the warm-up's, until its pipelines are made) */
		shader_enqueue(&sj, 1); rc.w_sh++;
		for (int l = r->pipes; l; l = rc.l[l - 1].next) { PipeRecipe *q = &rc.p[rc.l[l - 1].pipe];
			if (q->wneed && !q->wdone && rc.r[q->vs].we && rc.r[q->fs].we) warm_pipe(q); }
		if (!r->wleft) warm_hold_done(r);
	}
	if (rc.warm_phase == 4) {
		pthread_mutex_lock(&bq_m); const int left = bq_low; pthread_mutex_unlock(&bq_m);
		if (!left) { fprintf(stderr, "    gpu: shader recipes: warm-up done: %d shaders and %d pipelines built from the game's files and released (%d shaders not as keyed), %.1f s after the index was ready (%.0f ms of the render thread)\n", rc.w_sh, rc.w_pp, rc.w_fail, (rc_us() - rc.warm_t0) / 1e6, rc.t_warm / 1e3); warm_recipes_done(); }
	}
	while (rc.warm_phase == 6 && rc_us() - t0 < budget) {   /* every other fragment program in the game's files, in its predicted variant */
		pthread_mutex_lock(&bq_m); const int queued = bq_low, game = bq_inflight - bq_low; pthread_mutex_unlock(&bq_m);
		if (queued > 16 || game > 0) break;   /* (a few modules waiting at a time; none added while the game's own builds are in flight) */
		if (rc.pred_at >= gxp_index_count()) { rc.warm_phase = 7; break; }
		predict_warm(gxp_index_hash_at(rc.pred_at++));
	}
	if (rc.warm_phase == 7) {
		pthread_mutex_lock(&bq_m); const int left = bq_low; pthread_mutex_unlock(&bq_m);
		if (!left) {
			rc.warm_phase = 5;
			fprintf(stderr, "    gpu: shader recipes: predicted shaders: %d fragment programs in the game's files without a recipe -- %d shaders built and released, %d warm already, %d not translated (%d programs had recipes); %d pipelines in %d states built, %d warm already; %.1f s\n",
			        rc.p_frag, rc.p_sh, rc.p_warm, rc.p_fail, rc.p_recorded, rc.p_pp, rc.npst, rc.p_pp_warm, (rc_us() - rc.pred_t0) / 1e6);
			if (rc.stand_vs) { warm_unref(rc.stand_vs); rc.stand_vs = 0; }
			pred_throttle(0);
		}
	}
}
static int cmp_int(const void *a, const void *b) { const int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
static void recipes_step(void) {   /* each present: the programs registered since, and a slice of the builds they allow */
	if (!recipes_on() || (recipes_load(), rc.state <= 0) || bq_workers < 0) { pthread_mutex_lock(&reg_m); reg_qn = 0; pthread_mutex_unlock(&reg_m); return; }   /* (no build workers: it would all be built on the render thread) */
	if (recipes_verify() && gpu_frame_no % 300 == 0)
		fprintf(stderr, "    gpu: recipes: frame %llu: draws waited %.0f ms for builds so far; %d prebuilt shaders and %d pipelines used; %d records new this session; %d fragment shaders the prediction missed\n",
		        (unsigned long long)gpu_frame_no, __atomic_load_n(&build_wait_total, __ATOMIC_RELAXED) / 1e6, rc.used_sh, rc.used_pp, rc.recorded, rc.p_missed);
	warm_drain();
	static uint64_t budget; if (!budget) { const char *e = getenv("RECOMP_SHADER_PREBUILD_MS"); budget = (uint64_t)((e && atof(e) > 0 ? atof(e) : 2.0) * 1000); }
	pthread_mutex_lock(&reg_m);
	if (reg_qn && rc.nreg + reg_qn > rc.reg_cap) { int c = rc.reg_cap ? rc.reg_cap : 8192; while (c < rc.nreg + reg_qn) c *= 2; uint32_t *g = realloc(rc.reg, c * sizeof *g); if (g) { rc.reg = g; rc.reg_cap = c; } }
	if (reg_qn && rc.nreg + reg_qn <= rc.reg_cap) { memcpy(rc.reg + rc.nreg, reg_q, reg_qn * sizeof *reg_q); rc.nreg += reg_qn; }
	reg_qn = 0;
	pthread_mutex_unlock(&reg_m);
	if (rc.reg_at == rc.nreg && rc.work_at == rc.nwork) {   /* nothing to do: once its builds are done, the burst's summary */
		rc.nreg = rc.reg_at = rc.nwork = rc.work_at = 0;
		if (rc.burst) {
			pthread_mutex_lock(&bq_m); const int left = bq_low; pthread_mutex_unlock(&bq_m);
			if (!left) {
				rc.burst = 0;
				if (rc.built_sh || rc.built_pp) fprintf(stderr, "    gpu: shader recipes: %d registered programs had recipes: %d shaders and %d pipelines built in %.1f s (%.0f ms of it on the render thread), %d built already, %d passed over, %d stale; draws have used %d prebuilt shaders and %d pipelines so far\n",
				        rc.regs, rc.built_sh, rc.built_pp, (rc_us() - rc.burst_t0) / 1e6, rc.t_render / 1e3, rc.have, rc.skipped, rc.stale, rc.used_sh, rc.used_pp);
			}
		}
		if (rc.warm_phase && rc.warm_phase != 5) recipes_warm(rc_us(), budget);   /* (between levels' registrations: the warm-up) */
		return;
	}
	if (!rc.burst) { rc.burst_t0 = rc_us(); rc.t_render = 0; rc.regs = rc.built_sh = rc.built_pp = rc.have = rc.skipped = rc.stale = 0; }
	rc.burst = 1;
	const uint64_t t0 = rc_us();
	while (rc_us() - t0 < budget) {
		if (rc.reg_at < rc.nreg) { rc_registered(rc.reg[rc.reg_at++]); rc.work_sorted = 0; }
		else if (rc.work_at < rc.nwork) {
			if (!rc.work_sorted) { qsort(rc.work + rc.work_at, (size_t)(rc.nwork - rc.work_at), sizeof *rc.work, cmp_int); rc.work_sorted = 1; }   /* (a recipe file is in the order play first used each: the build queue gets them so) */
			recipe_try(rc.work[rc.work_at++]);
		} else break;
	}
	rc.t_render += rc_us() - t0;
}
void gfx_gpu_build_status(double *wait_ms, double *total_ms, int *inflight) {
	const uint64_t t = __atomic_load_n(&build_wait_since, __ATOMIC_RELAXED), now = SDL_GetTicksNS();
	*wait_ms = t ? (now - t) / 1e6 : 0;
	*total_ms = __atomic_load_n(&build_wait_total, __ATOMIC_RELAXED) / 1e6 + *wait_ms;
	pthread_mutex_lock(&bq_m); *inflight = bq_inflight - bq_low; pthread_mutex_unlock(&bq_m);   /* (the recipes' prebuilds aren't the game waiting) */
}
void gfx_gpu_draw(const GfxDraw *d) {
	if (!gpu() || !scene_entry) { if (!gpu() || scene_soft) gfx_soft_draw(d); return; }   /* (no target and no software scene: nothing begun to draw into -- dropped) */
	disp_note(DISP_DRAWS);
	if (ao_scene) ao_note_draw(d);   /* the main scene's camera, for its ambient occlusion */
	if (d->clip_mode == 0x40000000u) return;   /* SCE_GXM_REGION_CLIP_ALL: nothing drawn (gfx_soft.c's record_tri agrees); scene_first_draw untouched so the next real draw still clears if it's the scene's first */
	/* SCE_GXM_PRIMITIVE_POINTS: point sprites, expanded to quads on the CPU by gfx_soft.c's
	 * render_points_interp (the pipelines here are triangle lists; a few dozen sprites per prompt).
	 * Uncharted's swipe prompts draw this way into a widget-sized target (see gfx_soft.c) */
	g_points_draw = 0;
	if (gfx_prim_kind(d->prim) == GFX_PRIM_POINTS) {   /* RECOMP_GPU_NO_POINTS=1 drops them instead: the A/B of what the fallback costs (the target comes down and goes back up around each point draw) */
		static int no_points = -1, soft_points = -1; if (no_points < 0) { no_points = getenv("RECOMP_GPU_NO_POINTS") != 0; soft_points = getenv("RECOMP_GPU_SOFT_POINTS") != 0; }
		if (no_points) return;
		if (soft_points) { soft_fallback(d, "point sprites"); return; }   /* RECOMP_GPU_SOFT_POINTS=1: the software path's sprites, as before */
		g_points_draw = 1;   /* on the GPU: six vertices a point, the corner offsets in the vertex shader (usse_spirv_set_points) */
	}
	uint64_t td0 = us_now(), tf0 = t_flush;
	const UsseProgram *vp = usse_get(d->vprog), *fp = usse_get(d->fprog);
	const UsseVertexLayout *vl = usse_vertex_layout(vp);
	const UsseFragLayout *fl = usse_fragment_layout(fp);

	/* secondary programs run on the CPU regardless of backend (see usse_spirv.h): this resolves
	 * dependent-sampler texture-unit assignment too, which the translator needs up front */
	UsseDraw vdraw = {0}, fdraw = {0};
	sync_draw_reads(d, 0, fp);   /* (the SA words below, the vertex streams the rings copy) */
	usse_setup_registers(vp, &vdraw, d->vuniforms, d->vubuf);
	usse_setup_registers(fp, &fdraw, d->funiforms, d->fubuf);
	UsseInst scratch;
	memset(&scratch, 0, sizeof scratch); usse_run_secondary(vp, &scratch, &vdraw);
	memset(&scratch, 0, sizeof scratch); usse_run_secondary(fp, &scratch, &fdraw);

	uint32_t sa_for_smp[256]; for (int i = 0; i < 256; i++) sa_for_smp[i] = fdraw.sa_unit[i] ? fdraw.sa_unit[i] - 1u : fdraw.sa[i].u;   /* a dependent sampler's unit (UsseDraw.sa_unit; its SA words stay 0) */
	uint32_t cube_mask = 0; for (int i = 0; i < 16; i++) if (d->tex[i].data && ((d->tex[i].u_mode >> 4) & 3) == 3) cube_mask |= 1u << i;
	/* the vertex program's own (GfxDraw.vtex; its dependent units from vdraw's table, as the fragment's) */
	uint32_t vsa_for_smp[256]; for (int i = 0; i < 256; i++) vsa_for_smp[i] = vdraw.sa_unit[i] ? vdraw.sa_unit[i] - 1u : vdraw.sa[i].u;
	uint32_t vcube_mask = 0; for (int i = 0; i < 16; i++) if (d->vtex[i].data && ((d->vtex[i].u_mode >> 4) & 3) == 3) vcube_mask |= 1u << i;
	/* cube maps: uploaded as 6-layer 2D arrays and face-selected in the shader (a real cube texture
	 * sampled as unbound -- solid magenta -- on Metal); RECOMP_GPU_NO_CUBE=1 sends such draws to the
	 * software path instead */
	static int gpu_cube = -1; if (gpu_cube < 0) gpu_cube = getenv("RECOMP_GPU_NO_CUBE") == 0;
	if ((cube_mask || vcube_mask) && !gpu_cube) { soft_fallback(d, "cube map texture"); return; }
	/* FRAGCOLOR_USED: the program starts from the framebuffer's colour at its pixel (the hardware's
	 * programmable blending): the translator reads it from tile memory (fetch mode 2, Metal: nothing
	 * to do here) or fetches it from a copy of the colour target taken just before the draw (mode 1,
	 * below; see fetch_mode); RECOMP_GPU_NO_FETCH=1 sends such draws to the software path instead.
	 * (In mode 1 two triangles of one draw covering a pixel both read the pre-draw colour; the hardware
	 * and mode 2 read the first's result -- convex light volumes and full-screen passes don't overlap
	 * themselves.) */
	static int gpu_fetch = -1; if (gpu_fetch < 0) gpu_fetch = getenv("RECOMP_GPU_NO_FETCH") == 0;
	if (fl && fl->frag_color_used && !gpu_fetch) { soft_fallback(d, "framebuffer colour (FRAGCOLOR)"); return; }
	__atomic_add_fetch(&n_gpu_draws, 1, __ATOMIC_RELAXED); n_gpu_indices += d->count;
	t_setup += us_now() - td0;
	uint64_t tsh0 = us_now();
	/* render scale > 1: the units that read a GPU render target's bytes at another texel size,
	 * point-sampled (usse_spirv.c's snap_fetch: their reads at the 1x pixel centre) */
	uint32_t snap_mask = 0;
	if (any_scaled()) for (int i = 0; i < 16; i++) {
		const GfxTexture *tx = &d->tex[i]; uint32_t tb;
		if (!tx->data || (cube_mask >> i & 1) || tx->min_filter || tx->mag_filter || !(tb = (uint32_t)gfx_texture_bpp(tx))) continue;
		for (int k = 0; k < ntargets; k++) {
			TargetEntry *te = &targets[k]; if (!te->color || !TSCALED(te)) continue;   /* (a target at 1x: its texels are the bytes' size) */
			GfxTarget tg = target_entry_as_gfx(te); uint32_t bpp = (uint32_t)gfx_target_bpp(&tg);
			if (tx->data >= te->addr && tx->data < te->addr + bpp * te->width * te->height && bpp != tb) { snap_mask |= 1u << i; break; }
		}
	}
	VfPlan plan; vf_plan(d, vl, usse_program_pa_words(vp), &plan);   /* the vertex data path (above) picks the vertex shader variant */
	ShaderEntry *vs = get_shader(d->vprog, vp, 0, vl, 0, vsa_for_smp, -1, vcube_mask, 0, plan.gpu ? &plan.vf : 0, plan.hash);
	if (plan.gpu && (!vs || (!shader_pending(vs) && !vs->ok))) { plan.gpu = 0; vs = get_shader(d->vprog, vp, 0, vl, 0, vsa_for_smp, -1, vcube_mask, 0, 0, 0); }   /* (a layout the fetch prologue refused: the packer's variant, as before) */
	/* fetch mode 1 (no framebuffer read: Vulkan) and a FRAGCOLOR program whose use of the colour it reads is a
	 * blend the blend unit can do (usse_fragment_fb_blend -- Uncharted's deferred lights add into the HDR target,
	 * its particles alpha-blend into it in the shader): that variant and blend, instead of a copy of the whole
	 * target before the draw. A float target only (an 8-bit one clamps the source before blending, the Vita
	 * clamps the result) and no GXM blend of the draw's own. The Vita rounds each result to the target's format
	 * (U2F10F10F10's 5-bit mantissas), the blend unit keeps F16 between draws: a little more precise.
	 * RECOMP_GPU_NO_ADDITIVE=1: the copies. */
	static int no_additive = -1; if (no_additive < 0) no_additive = getenv("RECOMP_GPU_NO_ADDITIVE") != 0;
	g_fzero_draw = fl && fl->frag_color_used && fetch_mode() == 1 && !no_additive && (scene_entry->kind == TK_F16 || scene_entry->kind == TK_HDR10)
	               && !(d->blend.present && (d->blend.color_func || d->blend.alpha_func)) ? usse_fragment_fb_blend(fp) : 0;
	if (g_fzero_draw) {
		static uint32_t told[64]; static int ntold; uint32_t h = usse_program_hash(fp); int seen = 0;
		static const char *what[5] = { "", "adds to it: ONE/ONE", "adds to it, alpha replaced: ONE/ONE, alpha ONE/ZERO", "blends over it, alpha kept: ONE/ONE_MINUS_SRC_ALPHA", "blends over it: ONE/ONE_MINUS_SRC_ALPHA" };
		for (int i = 0; i < ntold; i++) if (told[i] == h) seen = 1;
		if (!seen && ntold < 64) { told[ntold++] = h; fprintf(stderr, "    gpu: fragment %#x reads the framebuffer and %s blending, no copy\n", d->fprog, what[g_fzero_draw]); }
	}
	ShaderEntry *fs = get_shader(d->fprog, fp, vp, vl, fl, sa_for_smp, scene_entry->kind, cube_mask, snap_mask, 0, 0);
	{	/* RECOMP_GXM_PATCHER_LOG=1: each fragment program's first draw, against when the game's shader patcher made it (hooks.c gxm_fp_create) */
		static int on = -1; if (on < 0) on = getenv("RECOMP_GXM_PATCHER_LOG") != 0;
		if (on) { static uint32_t seen[4096]; static int ns; int k = 0; for (; k < ns && seen[k] != d->fprog; k++) {}
			if (k == ns) { extern uint64_t display_frames(void); if (ns < 4096) seen[ns++] = d->fprog; fprintf(stderr, "    gpu: first draw of fragment gxp %#x frame %llu\n", d->fprog, (unsigned long long)display_frames()); } }
	}
	if (g_fzero_draw) n_batch_additive++;
	g_fzero_draw = 0;
	t_shader += us_now() - tsh0;
	{	/* RECOMP_GPU_FORCE_SKIP=n[,n...]: those draws (1-based over every draw this backend is handed: a replay's numbering)
		 * skipped as if their shader or pipeline were still being built, where skip_lossless allows it -- the async skip on
		 * demand, for a replay (whose builds are synchronous) of what only a live session's timing does */
		static uint32_t nd; nd++;
		static const char *fsk = (const char *)1; if (fsk == (const char *)1) fsk = getenv("RECOMP_GPU_FORCE_SKIP");
		if (fsk) for (const char *p = fsk; *p; ) { char *e; unsigned long v = strtoul(p, &e, 10); if (e == p) break;
			if (v == nd && skip_lossless()) { async_skip(d, "RECOMP_GPU_FORCE_SKIP"); fprintf(stderr, "    gpu: draw %u skipped (forced)\n", nd); return; }
			p = *e == ',' ? e + 1 : e; }
	}
	if (shader_pending(vs) || shader_pending(fs)) {   /* (gpu_async) */
		if (skip_lossless()) { async_skip(d, "a shader is being built"); return; }
		if (!gpu_overlap()) { if (vs) build_wait(&vs->pending); if (fs) build_wait(&fs->pending); }   /* (gpu_overlap: the flush waits) */
	}
	if (!vs || !fs || (!shader_pending(vs) && !vs->ok) || (!shader_pending(fs) && !fs->ok)) { soft_fallback(d, "program not translated"); return; }   /* translator doesn't cover this pair yet */
	/* the pipeline, before anything below touches the rings or the targets: a draw whose pipeline is still being built is skipped whole */
	/* ds_format 0 (an old capture, or a surface we never saw ourselves): both planes present, as before */
	int ds_depth_ok = !scene_target.ds_data || (scene_target.ds_format & 0x00044000u) != 0;
	int ds_stencil_ok = !scene_target.ds_data || (scene_target.ds_format & 0x00022000u) != 0;
	/* SceGxmCullMode against the NDC winding (gfx_soft.c's record_tri: CW culls a negative NDC
	 * area, CCW a positive one). SDL_GPU's front face is in its NDC too (y up, as Metal evaluates
	 * it; its Vulkan backend flips the viewport and the winding itself), and the game's clip
	 * position goes to gl_Position untouched, so CW is simply the back face of a CCW-front
	 * pipeline, whatever the viewport does with y. Checked on Unit 13's frame 1200: the other
	 * convention culls the full-screen ambient pass entirely. RECOMP_GPU_CULL_SWAP=1: that other
	 * convention, for diagnosis. */
	static int swap = -1; if (swap < 0) swap = getenv("RECOMP_GPU_CULL_SWAP") != 0;
	int cull_sdl = g_points_draw ? SDL_GPU_CULLMODE_NONE : d->cull == 1 ? (swap ? SDL_GPU_CULLMODE_FRONT : SDL_GPU_CULLMODE_BACK)
	             : d->cull == 2 ? (swap ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_FRONT) : SDL_GPU_CULLMODE_NONE;
	uint64_t tp0 = us_now();
	PipeEntry *pipe = get_pipeline(d, vs, fs, scene_target.depth != 0, scene_entry->kind, ds_depth_ok, ds_stencil_ok, cull_sdl, -1, 0);
	t_pipe += us_now() - tp0;
	if (pipeline_pending(pipe)) {
		if (skip_lossless()) { async_skip(d, "its pipeline is being built"); return; }
		if (!gpu_overlap()) build_wait(&pipe->pending);   /* (gpu_overlap: recorded pending, the flush waits) */
	}
	if (!pipe || (!pipeline_pending(pipe) && !pipe->pipe)) { soft_fallback(d, "no pipeline"); return; }
	/* a fragment program's LDST windows (usse_spirv.c's emit_ldst_target): each pointer (+4: the load's own offset) inside
	 * a GPU colour target -- F16F16F16F16 or 8-bit RGBA, never the scene's own -- whose texture the shader fetches from.
	 * Killzone's quarter-res pass reads its HDR scene so: through guest memory that was a 4 MB download a frame. */
	TargetEntry *ldst_te[8] = {0}; uint32_t ldst_win[8][4]; memset(ldst_win, 0, sizeof ldst_win);
	for (int k = 0; k < fs->nldst && k < 8; k++) {
		uint32_t ptr = fdraw.sa[fs->ldst_sa[k] & 255].u + 4; TargetEntry *te = 0; uint32_t bpp = 0;
		for (int ti = 0; ti < ntargets; ti++) {
			TargetEntry *x = &targets[ti]; if (!x->color || (x->kind != TK_F16 && x->kind != TK_U8)) continue;
			GfxTarget tg = target_entry_as_gfx(x); uint32_t b = (uint32_t)gfx_target_bpp(&tg);
			if (ptr >= x->addr && ptr < x->addr + b * x->width * x->height && (!te || x->gen > te->gen)) { te = x; bpp = b; }
		}
		if (!te || te == scene_entry || !((te->kind == TK_F16 && bpp == 8) || (te->kind == TK_U8 && bpp == 4))) { soft_fallback(d, "fragment LDST outside a GPU colour target"); return; }
		ldst_te[k] = te;
		ldst_win[k][0] = te->addr; ldst_win[k][1] = bpp * te->width * te->height; ldst_win[k][2] = bpp * te->width; ldst_win[k][3] = (te->kind == TK_F16 ? 1u : 0u) | bpp << 8;
	}
	for (int k = 0; k < fs->nldst && k < 8; k++) target_sync_for_gpu(ldst_te[k]);   /* (its newest bytes may be in guest memory: a software draw) */
	/* guest memory newer than the scene's target (a software draw; in a replay, a memory record
	 * restored over it -- ~20 ms of Unit 13's frame 757 replay, which the live game doesn't have) */
	{ uint64_t ts0 = us_now(), tf1 = t_flush; target_sync_for_gpu(scene_entry); t_scene_sync += us_now() - ts0 - (t_flush - tf1); }

	/* RECOMP_GFX_CHECK=1: snapshot guest memory (the agreed-upon pre-draw state, just synced
	 * above) in the target's own raw format, so the comparison below can run the software path
	 * on an exact copy of what the GPU is about to draw onto, then diff the two results. */
	uint8_t *check_pre = 0; uint32_t check_pre_bytes = 0;
	if (gfx_check_on() && scene_entry->addr) {
		GfxTarget gt = target_entry_as_gfx(scene_entry);
		check_pre_bytes = (uint32_t)gfx_target_bpp(&gt) * scene_entry->width * scene_entry->height;
		check_pre = malloc(check_pre_bytes);
		target_sync_for_cpu(scene_entry);   /* the GPU texture is the current state; guest memory may be draws behind */
		memcpy(check_pre, G(scene_entry->addr), check_pre_bytes);
	}

	/* the draw's vertices and indices into the rings (one persistent storage buffer and its transfer buffer
	 * each; a fresh pair per draw cost 49 ms of a 206 ms frame). GPU-side fetch (vf_plan, the default): the
	 * raw streams of the index range [lo, hi], one memcpy each, and the game's own indices verbatim
	 * (16-bit as 16-bit; a strip or fan expanded into a 32-bit list by gfx.h's gfx_prim_tri, in the order of
	 * gfx_soft.c's render_interp) -- gl_VertexIndex is the vertex's own index, first_vertex 0, the shader
	 * subtracts lo (UsseDrawUniforms.vfetch) to find its record in each stream's copy. CPU packer (the plan
	 * declined, or RECOMP_GPU_CPU_VFETCH=1): one PA record per vertex of [lo, hi] packed exactly like
	 * gfx_soft.c's fetch_interp (gfx_vertex_pack), pa_words each, the triangles as 32-bit indices rebased by
	 * lo, the draw starting at its first record (gl_VertexIndex counts from first_vertex); a sparse range
	 * (more records than indices) packs one record per index instead. */
	uint32_t ntri = plan.ntri, pos[3];
	if (!ntri) { free(check_pre); return; }
	uint64_t tv0 = us_now();
	uint32_t npa = (uint32_t)vs->pa_words, nverts, need_words;
	if (!vring) {
		SDL_GPUBufferCreateInfo rci = { .usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, .size = VRING_WORDS * 4 };
		vring = SDL_CreateGPUBuffer(gpu(), &rci);
		SDL_GPUTransferBufferCreateInfo rxi = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = VRING_WORDS * 4 };
		vring_xfer = SDL_CreateGPUTransferBuffer(gpu(), &rxi);
		if (!vring || !vring_xfer) { free(check_pre); soft_fallback(d, "no vertex ring"); return; }
	}
	uint32_t lo = plan.lo, hi = plan.hi, nuniq = hi - lo + 1, nidx = plan.nidx, nrec = 0;
	int kind = gfx_prim_kind(d->prim), indexed, isize = 4;   /* isize: the index ring's element size for this draw */
	if (plan.gpu) { indexed = 1; need_words = plan.words; if (!d->index_format && (kind == GFX_PRIM_TRIANGLES || kind == GFX_PRIM_EDGES)) isize = 2; }
	else { indexed = !g_points_draw && nuniq <= nidx && nidx <= IRING_WORDS && !no_indexed(); nrec = indexed ? nuniq : nidx; need_words = nrec * npa; }
	if (need_words > VRING_WORDS) { free(check_pre); soft_fallback(d, "draw larger than the vertex ring"); return; }
	if (indexed && !iring) {
		SDL_GPUBufferCreateInfo ici = { .usage = SDL_GPU_BUFFERUSAGE_INDEX, .size = IRING_WORDS * 4 };
		iring = SDL_CreateGPUBuffer(gpu(), &ici);
		SDL_GPUTransferBufferCreateInfo ixi = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = IRING_WORDS * 4 };
		iring_xfer = SDL_CreateGPUTransferBuffer(gpu(), &ixi);
		if (!iring || !iring_xfer) { if (plan.gpu) { free(check_pre); soft_fallback(d, "no index ring"); return; } indexed = 0, nrec = nidx, need_words = nrec * npa; }
	}
	uint32_t iwords = indexed ? (isize == 2 ? (nidx + 1) / 2 : nidx) : 0;   /* the index ring words this draw takes: 16-bit indices two to a word, every draw's region word-aligned */
	if (!plan.gpu) { uint32_t al = g_points_draw ? 6 * npa : npa; vring_pos = (vring_pos + al - 1) / al * al; }   /* records of this width start at a multiple of it (point sprites: of six of them, gl_VertexIndex % 6 is the corner) */
	/* the GPU may still read the ring's start: wait (once per wrap; the ring holds several frames) --
	 * after submitting the pending draws, whose records are where the ring ends (an idle wait doesn't
	 * cover work not yet submitted, and their batch's one upload must not span the wrap) */
	if (vring_pos + need_words > VRING_WORDS) { batch_flush("ring wrap"); vring_pos = 0; vring_lap++; n_vring_wraps++; ring_wait_region(0, 0, need_words, 1); }
	else ring_wait_region(0, vring_pos, need_words, 0);
	if (indexed) {
		if (iring_pos + iwords > IRING_WORDS) { batch_flush("ring wrap"); iring_pos = 0; iring_lap++; n_vring_wraps++; ring_wait_region(1, 0, iwords, 1); }
		else ring_wait_region(1, iring_pos, iwords, 0);
	}
	uint64_t tm0 = us_now();
	uint32_t first_vertex = plan.gpu ? 0 : vring_pos / npa, *vmap = (uint32_t *)SDL_MapGPUTransferBuffer(gpu(), vring_xfer, false) + vring_pos, *vp_out = vmap;
	t_map += us_now() - tm0;
	uint32_t iring_off = 0, vf_stream_w[16] = {0};   /* vf_stream_w: each stream copy's ring word offset (UsseDrawUniforms.stream) */
	if (plan.gpu) {
		uint32_t w = vring_pos;
		for (int m = 0; m < plan.nst; m++) { memcpy(vmap + (w - vring_pos), G(plan.st[m].src), plan.st[m].bytes); vf_stream_w[plan.st[m].s] = w; w += (plan.st[m].bytes + 3) / 4 + 1; }
		uint32_t *imap = (uint32_t *)SDL_MapGPUTransferBuffer(gpu(), iring_xfer, false) + iring_pos;
		if (kind == GFX_PRIM_TRIANGLES || kind == GFX_PRIM_EDGES) memcpy(imap, G(d->indices), (size_t)isize * nidx);
		else { uint32_t *ip = imap; for (uint32_t t = 0; t < ntri; t++) { gfx_prim_tri(d->prim, t, pos); for (int k = 0; k < 3; k++) *ip++ = index_at(d, pos[k]); } }
		SDL_UnmapGPUTransferBuffer(gpu(), iring_xfer);
		iring_off = iring_pos * 4; iring_pos += iwords;
	} else {
		GfxVertexPacker pk; gfx_vertex_packer_init(&pk, d, vl, (int)npa);
		if (indexed) {
			for (uint32_t v = lo; v <= hi; v++, vp_out += npa) gfx_vertex_pack(&pk, v, vp_out);
			uint32_t *imap = (uint32_t *)SDL_MapGPUTransferBuffer(gpu(), iring_xfer, false) + iring_pos, *ip = imap;
			for (uint32_t t = 0; t < ntri; t++) { gfx_prim_tri(d->prim, t, pos); for (int k = 0; k < 3; k++) *ip++ = index_at(d, pos[k]) - lo; }
			SDL_UnmapGPUTransferBuffer(gpu(), iring_xfer);
			iring_off = iring_pos * 4; iring_pos += iwords;
		} else if (g_points_draw) {   /* each point's record six times: the vertex shader takes its corner from gl_VertexIndex % 6 */
			for (uint32_t i = 0; i < d->count; i++) { gfx_vertex_pack(&pk, index_at(d, i), vp_out); for (int k = 1; k < 6; k++) memcpy(vp_out + (size_t)k * npa, vp_out, 4u * npa); vp_out += 6 * npa; }
		} else {
			for (uint32_t t = 0; t < ntri; t++) { gfx_prim_tri(d->prim, t, pos); for (int k = 0; k < 3; k++, vp_out += npa) gfx_vertex_pack(&pk, index_at(d, pos[k]), vp_out); }
		}
	}
	SDL_UnmapGPUTransferBuffer(gpu(), vring_xfer);
	uint32_t vring_off = vring_pos * 4; vring_pos += need_words;
	nverts = nidx;
	t_vfetch += us_now() - tv0; if (us_now() - tv0 > t_vfetch_max) t_vfetch_max = us_now() - tv0;


	/* the vertex records' bytes (160 words/vertex at most, matching usse_spirv.c's StorageBuffer layout) go up with the batch */
	uint64_t tvb0 = us_now();
	uint32_t vbytes = need_words * 4;

	/* the LDST windows (see usse_spirv.c's emit_ldst): USSE_LDST_WINDOW bytes of guest memory from
	 * each distinct pointer the vertex program's SA words hold, as storage buffer 1 */
	uint32_t win[8][4]; int nwin = 0;
	for (int k = 0; k < vs->nldst; k++) {
		uint32_t b = vdraw.sa[vs->ldst_sa[k] & 255].u, j;
		for (j = 0; j < (uint32_t)nwin && win[j][0] != b; j++);
		if (j < (uint32_t)nwin || !b || b > 0xFFFFFFFFu - USSE_LDST_WINDOW) continue;
		win[nwin][0] = b; win[nwin][1] = USSE_LDST_WINDOW; win[nwin][2] = (uint32_t)nwin * (USSE_LDST_WINDOW / 4); win[nwin][3] = 0; nwin++;
		if (trace_sync()) fprintf(stderr, "gpu trace: draw %u LDST window %d: SA%d = %#x, first words %#x %#x %#x\n", n_gpu_draws, nwin - 1, vs->ldst_sa[k], b, RD32(b), RD32(b + 4), RD32(b + 8));
	}
	if (nwin) {   /* each window into the LDST ring (guest memory as it is now: the batch's copy pass uploads it), or the one already there */
		if (!wring) {
			SDL_GPUBufferCreateInfo wci = { .usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, .size = WRING_WORDS * 4 };
			wring = SDL_CreateGPUBuffer(gpu(), &wci);
			SDL_GPUTransferBufferCreateInfo wxci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = WRING_WORDS * 4 };
			wring_xfer = SDL_CreateGPUTransferBuffer(gpu(), &wxci);
			if (!wring || !wring_xfer) { free(check_pre); soft_fallback(d, "no LDST window ring"); return; }
		}
		const uint32_t ww = USSE_LDST_WINDOW / 4;
		if (wring_pos + (uint32_t)nwin * ww > WRING_WORDS) {   /* (an empty batch doesn't flush: its ranges are of windows no recorded draw uses, and the lap change retires them from the reuse cache) */
			batch_flush("LDST ring wrap"); wring_pos = 0; wring_lap++; bw_lo = bwr_lo = ~0u; bw_hi = bwr_hi = 0; ring_wait_region(2, 0, (uint32_t)nwin * ww, 1);
		}
		uint32_t *wmap = 0;
		for (int k = 0; k < nwin; k++) {
			uint32_t off = ldst_window_reuse(win[k][0]);
			if (off == ~0u) {
				ring_wait_region(2, wring_pos, ww, 0);
				if (!wmap) wmap = (uint32_t *)SDL_MapGPUTransferBuffer(gpu(), wring_xfer, false);
				off = wring_pos; wring_pos += ww;
				ldst_window_placed(win[k][0], off, wmap + off);
				if (off < bw_lo) bw_lo = off; if (off + ww > bw_hi) bw_hi = off + ww;
			}
			if (off < bwr_lo) bwr_lo = off; if (off + ww > bwr_hi) bwr_hi = off + ww;
			win[k][2] = off;   /* the window's word offset in storage buffer 1 */
		}
		if (wmap) SDL_UnmapGPUTransferBuffer(gpu(), wring_xfer);
	}
	t_vbuf += us_now() - tvb0;

	int debug_clear = 0; { static int dc = -1; if (dc < 0) dc = getenv("RECOMP_GPU_DEBUG_CLEAR") != 0; debug_clear = dc; }   /* TEMP diagnostic: clear to green, skip the draws */
	uint64_t tr0 = us_now();
	/* the surface's logical size, not its stride: a 960-wide display laid out 1024 wide spread every
	 * full-screen draw over 1024 columns (Shovel Knight's composite came out stretched by 1024/960) */
	uint32_t vw = scene_target.width && scene_target.width <= scene_entry->width ? scene_target.width : scene_entry->width;
	SDL_GPUViewport vp2;
	if (d->viewport_set && !d->viewport_disabled) {   /* sceGxmSetViewport: screen = offset + scale * ndc */
		vp2.w = 2.0f * d->vp_xscale; vp2.x = d->vp_xoff - d->vp_xscale;
		vp2.h = -2.0f * d->vp_yscale; vp2.y = d->vp_yoff + d->vp_yscale;
		vp2.min_depth = 0.0f; vp2.max_depth = 1.0f;   /* z is mapped in the vertex shader (zOffset + zScale * z/w): the depth range stays the full [0,1] */
	} else if (d->viewport_set) {   /* SCE_GXM_VIEWPORT_DISABLED: clip position IS the device position, 1:1 */
		vp2 = (SDL_GPUViewport){ -1.0f, -1.0f, 2.0f, 2.0f, 0.0f, 1.0f };
	} else vp2 = (SDL_GPUViewport){ 0, 0, (float)vw, (float)scene_entry->height, 0.0f, 1.0f };   /* never set: the old implicit full-target mapping */
	{ float kx = (float)scene_sx, ky = (float)scene_sy; vp2.x *= kx; vp2.y *= ky; vp2.w *= kx; vp2.h *= ky; }   /* render scale: the target's pixels are n x n (DISABLED's 1:1 device mapping too: (-n,-n,2n,2n) puts clip x at n*x) */
	/* sceGxmSetRegionClip: OUTSIDE scissors to the box (xMin/yMin/xMax/yMax inclusive, pixels);
	 * ALL already returned above; NONE (default, 0 -- an old capture too) leaves the full target */
	int sx0 = 0, sy0 = 0, sx1 = (int)vw, sy1 = (int)scene_entry->height;
	if (d->clip_mode == 0x80000000u) {
		if ((int)d->clip_xmin > sx0) sx0 = (int)d->clip_xmin;
		if ((int)d->clip_ymin > sy0) sy0 = (int)d->clip_ymin;
		if ((int)d->clip_xmax + 1 < sx1) sx1 = (int)d->clip_xmax + 1;
		if ((int)d->clip_ymax + 1 < sy1) sy1 = (int)d->clip_ymax + 1;
		if (sx1 < sx0) sx1 = sx0; if (sy1 < sy0) sy1 = sy0;
	}
	cur_wx0 = sx0; cur_wy0 = sy0; cur_wx1 = sx1; cur_wy1 = sy1;   /* (same_region_read, for the binds below) */
	SDL_GPUTextureSamplerBinding tsb[25]; int ntsb = 0;
	if (pipe->fetch && !scene_entry->fetch_copy) {   /* the framebuffer copy this draw's FRAGCOLOR reads (taken in its batch's copy pass) */
		SDL_GPUTextureCreateInfo fci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = kind_format(scene_entry->kind), .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
			.width = TSX(scene_entry, scene_entry->width), .height = TSY(scene_entry, scene_entry->height), .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
		scene_entry->fetch_copy = SDL_CreateGPUTexture(gpu(), &fci);
	}
	t_setup += us_now() - tr0;
	uint32_t srgb_dec = 0, srgb_enc = 0;   /* UsseDrawUniforms.tex_srgb: units the shader decodes / re-encodes (usse_spirv.c srgb_fixup) */
	memset(unit_sx, 0, sizeof unit_sx); memset(unit_sy, 0, sizeof unit_sy);
	uint32_t scaled_units = 0;             /* tex_srgb word 2: units bound to a texture at the render scale (a view, the target itself) -- snap_fetch */
	if (pipe->nsmp || pipe->fetch) {
		for (int i = 0; i < pipe->nsmp; i++) {
			int unit = pipe->smp_units[i];
			bind_unit((unit >= 0 && unit < 16) ? &d->tex[unit] : 0, unit, &tsb[i], &srgb_dec, &srgb_enc, &scaled_units, "fprog", d->fprog, i);
		}
		ntsb = pipe->nsmp;
		if (pipe->fetch && scene_entry->fetch_copy) {   /* the framebuffer copy, point-sampled, after the program's own textures */
			static SDL_GPUSampler *fetch_sampler;
			if (!fetch_sampler) { SDL_GPUSamplerCreateInfo ci = {0}; ci.min_filter = ci.mag_filter = SDL_GPU_FILTER_NEAREST; ci.address_mode_u = ci.address_mode_v = ci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE; fetch_sampler = SDL_CreateGPUSampler(gpu(), &ci); }
			tsb[ntsb].texture = scene_entry->fetch_copy; tsb[ntsb].sampler = fetch_sampler; ntsb++;
		}
	}

	SDL_GPUTextureSamplerBinding vtsb[16]; uint32_t v_srgb_dec = 0, v_srgb_enc = 0, v_scaled = 0;
	for (int i = 0; i < vs->nsmp && i < 16; i++) {
		int unit = vs->smp_units[i];
		bind_unit((unit >= 0 && unit < 16) ? &d->vtex[unit] : 0, unit, &vtsb[i], &v_srgb_dec, &v_srgb_enc, &v_scaled, "vprog", d->vprog, i);
	}

	for (int k = 0; k < fs->nldst && k < 8 && ntsb < 25; k++) { tsb[ntsb].texture = ldst_te[k]->color; tsb[ntsb].sampler = nearest_sampler(); ntsb++; }   /* the LDST targets, after the samplers and the framebuffer copy (emit_ldst_target's bindings) */

	/* record the draw (see "the draw batch"): binding its textures above may have flushed the draws
	 * before it (a re-upload) or put a view pass after them; a FRAGCOLOR draw gets its framebuffer
	 * copy, taken between the render pass of the draws before it and its own */
	uint64_t trc0 = us_now();
	int ds_clear = scene_target.depth && scene_first_draw && !(scene_target.ds_data && scene_target.ds_force_load);
	if (batch.n && (batch.color != scene_entry->color || batch.depth != (scene_target.depth ? scene_entry->depth : 0))) batch_flush("attachments changed");   /* not expected inside a scene; kept so a batch never mixes them */
	int fetch_ev = -1;   /* this draw's copy event (narrowed to its extent below, once b->hud is known) */
	if (pipe->fetch && scene_entry->fetch_copy) {   /* (fetch mode 1) only the draw's scissor rectangle can be read: Unit 13's half-res particle draws copy a quarter of the target */
		int cw = (sx1 - sx0) * scene_sx, ch = (sy1 - sy0) * scene_sy;
		if (cw > 0 && ch > 0) {
			BatchEvent *e = batch_event(); e->src = scene_entry->color; e->dst = scene_entry->fetch_copy; e->x = (uint32_t)(sx0 * scene_sx); e->y = (uint32_t)(sy0 * scene_sy); e->w = (uint32_t)cw; e->h = (uint32_t)ch;
			n_batch_fetch++; fetch_ev = batch.nev - 1;
		}
	}
	BatchDraw *b = batch_add();
	if (batch.n == 1) {
		batch.color = scene_entry->color; batch.depth = scene_target.depth ? scene_entry->depth : 0; batch.debug_clear = debug_clear;
		/* an old capture (ds_data 0) clears to 1.0/0, as before */
		batch.clear_ds = ds_clear; batch.clear_z = scene_target.ds_data ? scene_target.ds_background_depth : 1.0f; batch.clear_s = scene_target.ds_data ? scene_target.ds_background_stencil : 0;
	}
	b->pipe = pipe; b->stencil_ref = d->stencil_ref; b->indexed = indexed; b->nsbuf = vs->nldst || vs_layout_uniform() ? 2 : 1; b->ntsb = ntsb; b->qslot = -1;
	b->hud = scene_hud && d->viewport_set && !d->viewport_disabled && vp2.w > 0 && hud_classify(d, b->hud_b);   /* the HUD at its native shape (hud_layout, at the flush) */
	if (b->hud) disp_note(DISP_HUD);
	if (fetch_ev >= 0 && !b->hud) fetch_narrow(d, vs, &batch.ev[fetch_ev], vw, sx0, sy0, sx1, sy1);
	b->vp = vp2; b->sc = (SDL_Rect){ sx0 * scene_sx, sy0 * scene_sy, (sx1 - sx0) * scene_sx, (sy1 - sy0) * scene_sy };
	b->vring_off = vring_off; b->vbytes = vbytes; b->iring_off = iring_off; b->nidx = nverts; b->first_vertex = first_vertex; b->isize = isize; b->ibytes = 4 * iwords;
	b->wring = nwin > 0;
	memcpy(b->tsb, tsb, (size_t)ntsb * sizeof tsb[0]);
	b->nvtsb = vs->nsmp < 16 ? vs->nsmp : 16; memcpy(b->vtsb, vtsb, (size_t)b->nvtsb * sizeof vtsb[0]);
	{	/* the SA words plus the viewport z mapping (see emit_vert_epilogue); the fragment stage's block has the same shape */
		memcpy(b->vu.sa, vdraw.sa, sizeof b->vu.sa); memcpy(b->fu.sa, fdraw.sa, sizeof b->fu.sa);
		memset(b->vu.vfetch, 0, sizeof b->vu.vfetch); memset(b->vu.stream, 0, sizeof b->vu.stream); memset(b->fu.vfetch, 0, sizeof b->fu.vfetch); memset(b->fu.stream, 0, sizeof b->fu.stream);   /* (before the point sprites' size and flip below: cleared after them, both were zeroed since 7b823b5) */
		b->vu.vfetch[3] = 0x3f800000u;   /* the fetch prologue's opaque 0 and 1.0f in .z/.w (emit_vertex_fetch; every draw, harmless for the rest) */
		int vz_on = d->viewport_set && !d->viewport_disabled;
		b->vu.viewport_z[0] = b->fu.viewport_z[0] = vz_on ? d->vp_zoff : 0.0f; b->vu.viewport_z[1] = b->fu.viewport_z[1] = vz_on ? d->vp_zscale : 1.0f;
		b->vu.viewport_z[2] = b->vu.viewport_z[3] = b->fu.viewport_z[2] = b->fu.viewport_z[3] = 0;
		if (g_points_draw) {   /* point sprites: half a target pixel in NDC per axis, signed for the screen's directions (gfx_soft.c's render_points_interp) */
			float kx, ky; int ydown, xleft = 0;
			if (d->viewport_set && d->viewport_disabled) { kx = ky = 1; ydown = 1; }
			else if (d->viewport_set) { kx = fabsf(d->vp_xscale); ky = fabsf(d->vp_yscale); ydown = d->vp_yscale > 0; xleft = d->vp_xscale < 0; }
			else { kx = scene_target.width * 0.5f; ky = scene_target.height * 0.5f; ydown = 0; }
			if (!(kx > 0)) kx = 1;
			if (!(ky > 0)) ky = 1;
			b->vu.viewport_z[2] = 0.5f * (xleft ? -1.0f : 1.0f) / kx; b->vu.viewport_z[3] = 0.5f * (ydown ? 1.0f : -1.0f) / ky;
			float size = d->point_line_width ? (float)d->point_line_width : 1.0f, flip = d->polygon_mode == 2 ? 1.0f : 0.0f;   /* POINT_10UV */
			memcpy(&b->vu.vfetch[0], &size, 4); memcpy(&b->vu.vfetch[1], &flip, 4);
		}
		memset(b->vu.win, 0, sizeof b->vu.win); memset(b->fu.win, 0, sizeof b->fu.win); memcpy(b->vu.win, win, (size_t)nwin * sizeof win[0]);
		if (fs->nldst) memcpy(b->fu.win, ldst_win, (size_t)(fs->nldst < 8 ? fs->nldst : 8) * sizeof ldst_win[0]);   /* the fragment LDST targets (emit_ldst_target) */
		b->fu.tex_srgb[0] = srgb_dec; b->fu.tex_srgb[1] = srgb_enc; b->fu.tex_srgb[2] = scaled_units;
		for (int u = 0; u < 16; u++) {   /* each unit's texture LOD bias (usse_spirv.c sample_lod) */
			b->fu.lod_bias[u] = d->tex[u].data ? GFX_TEX_LOD_BIAS(d->tex[u].format) : 0.0f;
			b->vu.lod_bias[u] = d->vtex[u].data ? GFX_TEX_LOD_BIAS(d->vtex[u].format) : 0.0f;
		}
		b->vu.tex_srgb[0] = v_srgb_dec; b->vu.tex_srgb[1] = v_srgb_enc; b->vu.tex_srgb[2] = v_scaled;   /* the vertex stage's own units */ b->vu.tex_srgb[3] = b->fu.tex_srgb[3] = snap_scale(snap_mask);   /* snap_fetch: x | y << 16, the snapped target's scale */
		/* the GPU-side fetch's lowest index and each stream copy's ring word offset (emit_vertex_fetch); zeros for the packer's records (cleared above) */
		if (canon_vary_on()) { b->fu.vfetch[2] = usse_spirv_vary_ncomp(vl, 0); b->fu.vfetch[3] = usse_spirv_vary_ncomp(vl, 1); }   /* (the vertex program's component counts: canonical varyings) */
		if (fs->pcf && d->tex[fs->pcf_unit].width && d->tex[fs->pcf_unit].height) {   /* filtered shadows: the grid step in UV (usse_spirv_set_pcf), in the shadow map's 1x texels */
			float texels = shadow_filter_want() >= 2 ? 2.0f : 1.0f, su = texels / d->tex[fs->pcf_unit].width, sv = texels / d->tex[fs->pcf_unit].height;
			memcpy(&b->fu.vfetch[0], &su, 4); memcpy(&b->fu.vfetch[1], &sv, 4);
		}
		if (plan.gpu) { b->vu.vfetch[0] = lo; b->vu.vfetch[1] = (uint32_t)plan.nst;   /* (point sprites never fetch on the GPU: vf_plan) */ for (int m = 0; m < plan.nst; m++) { b->vu.stream[plan.st[m].s][0] = vf_stream_w[plan.st[m].s]; b->vu.stream[plan.st[m].s][1] = d->stride[plan.st[m].s]; } }
	}
	if (d->vis_addr) vis_query_add(d, vs, scene_target.depth != 0, scene_entry->kind, ds_depth_ok, ds_stencil_ok, cull_sdl);
	/* what the draw writes, as views and the guest-memory syncs see it: the colour target always;
	 * the depth/stencil surface only when the pipeline can change it (a depth or stencil write, or
	 * the scene's first batch clearing it), and its depth plane (what its X8U24 view reads) only on
	 * a depth write or clear -- a bump per draw made Unit 13's lighting pass, which samples its own
	 * depth while each light's stencil volume writes stencil with depth writes off, rebuild that view,
	 * and so flush the batch, before every draw */
	scene_entry->gpu_dirty = 1; scene_entry->gen = ++write_clock; scene_first_draw = 0;
	if (scene_depth_entry && scene_target.depth) {
		int st_on = ds_stencil_ok && (d->stencil_func != 7 || d->stencil_fail || d->depth_fail || d->depth_pass);   /* get_pipeline's enable_stencil_test */
		int zwrites = ds_clear || (ds_depth_ok && d->depth_write);
		int writes = zwrites || (st_on && d->stencil_wmask && (d->stencil_fail || d->depth_fail || d->depth_pass));
		scene_depth_entry->gpu_newer = 1;
		if (writes) scene_depth_entry->gen = ++write_clock;
		if (zwrites) scene_depth_entry->zgen = scene_depth_entry->gen;
	}
	t_pass += us_now() - trc0;
	if (check_pre) batch_flush("RECOMP_GFX_CHECK");   /* the check reads this draw's own result */
	t_total += us_now() - td0 - (t_flush - tf0);   /* the flushes inside this draw counted themselves */

	if (check_pre) { gfx_check_draw(d, scene_entry, scene_target.depth != 0, check_pre, check_pre_bytes); free(check_pre); }
}

/* ---------------------------------------------------------------- GPU timing (RECOMP_GPU_FENCE_STATS=1)
 * How long the GPU itself takes per presented frame. The host never waits for the GPU at present (the
 * game sees its kick complete at once), so frame times and the render thread's host time say nothing
 * about GPU execution. On: after every scene submit (batch_flush) and at every present an empty
 * command buffer goes in with a fence, "a mark". The queue runs in order, so a mark's completion is
 * the completion of everything submitted before it. A polling thread (100 us) stamps each mark's
 * completion (SDL_QueryGPUFence is an atomic read on the Metal backend); the GPU's own time between
 * marks is busy_i = done_i - max(done_{i-1}, submit_i) (nothing of mark i's work can start before it
 * was submitted), summed per presented frame. One stderr line a second: busy ms/frame, the busiest
 * frame, the present mark's submit->done latency (how far the GPU runs behind the game) and how many
 * presents were still in flight at each present. RECOMP_GPU_FENCE_TRACE=1 prints every mark: in a
 * replay (gfxreplay calls gfx_gpu_fence_stats_finish) mark k is scene k, so the per-scene GPU cost of
 * a captured frame. Off, nothing here runs and no command buffer is added. */
#include <pthread.h>
typedef struct { SDL_GPUFence *f; uint64_t submit, done; int state, present; uint64_t frame; char tag[320]; } FenceMark;   /* state 0 free, 1 pending, 2 done (fences released on the render thread); tag: what the command buffer before this mark held (batch_flush), frame: gpu_frame_no at submit -- both for RECOMP_GPU_FENCE_TRACE */
#define NFM 1024
static FenceMark fm[NFM]; static int fm_w, fm_r;   /* ring: written at fm_w, folded from fm_r, both in submit order */
static pthread_mutex_t fm_m = PTHREAD_MUTEX_INITIALIZER;
static int fence_stats_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_GPU_FENCE_STATS"); v = e && *e && *e != '0'; } return v; }
static void *fm_poll(void *arg) {
	(void)arg;
	for (;;) {
		usleep(100);
		pthread_mutex_lock(&fm_m);
		for (int i = fm_r; i != fm_w; i = (i + 1) % NFM) if (fm[i].state == 1 && fm[i].f && SDL_QueryGPUFence(gpu(), fm[i].f)) { fm[i].done = us_now(); fm[i].state = 2; }
		pthread_mutex_unlock(&fm_m);
	}
	return 0;
}
static struct { uint64_t last_done, last_report, acc_busy, frame_acc, max_frame, acc_lat, max_lat, acc_inflight, max_inflight; uint32_t nframes, nmarks, npresent; } fmst;
static void fence_mark(int present) {
	static int started; if (!started) { started = 1; pthread_t t; pthread_create(&t, 0, fm_poll, 0); pthread_detach(t); }
	pthread_mutex_lock(&fm_m);
	/* fold the completed marks, in submit order, stopping at the first still pending (completion order is submission order) */
	while (fm_r != fm_w && fm[fm_r].state == 2) {
		FenceMark *e = &fm[fm_r];
		uint64_t start = fmst.last_done > e->submit ? fmst.last_done : e->submit, busy = e->done > start ? e->done - start : 0;
		uint64_t idle = fmst.last_done && e->submit > fmst.last_done ? e->submit - fmst.last_done : 0;   /* the GPU had nothing queued between the previous mark's completion and this one's submit */
		if (e->done > fmst.last_done) fmst.last_done = e->done;
		fmst.acc_busy += busy; fmst.frame_acc += busy; fmst.nmarks++;
		{ static int tr = -1; if (tr < 0) { const char *te = getenv("RECOMP_GPU_FENCE_TRACE"); tr = te && *te && *te != '0'; }
		  if (tr) fprintf(stderr, "    gpu fence mark %u%s: busy %.2f ms, submit->done %.2f ms, idle before %.2f ms [f%llu %s]\n", fmst.nmarks, e->present ? " (present)" : "", busy / 1e3, (e->done - e->submit) / 1e3, idle / 1e3, (unsigned long long)e->frame, e->tag); }
		if (e->present) { uint64_t lat = e->done - e->submit; fmst.acc_lat += lat; if (lat > fmst.max_lat) fmst.max_lat = lat; if (fmst.frame_acc > fmst.max_frame) fmst.max_frame = fmst.frame_acc; fmst.frame_acc = 0; fmst.nframes++; }
		SDL_ReleaseGPUFence(gpu(), e->f); e->f = 0; e->state = 0;
		fm_r = (fm_r + 1) % NFM;
	}
	if (present) { uint32_t inflight = 0; for (int i = fm_r; i != fm_w; i = (i + 1) % NFM) if (fm[i].present) inflight++; fmst.acc_inflight += inflight; if (inflight > fmst.max_inflight) fmst.max_inflight = inflight; fmst.npresent++; }
	if ((fm_w + 1) % NFM != fm_r) {
		FenceMark *e = &fm[fm_w];
		SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu());
		if (cmd) { e->f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd); e->submit = us_now(); e->present = present; e->state = e->f ? 1 : 0; e->frame = gpu_frame_no; memcpy(e->tag, present ? "present" : fm_tag, sizeof e->tag); if (e->f) fm_w = (fm_w + 1) % NFM; }
	}
	pthread_mutex_unlock(&fm_m);
}
static void fence_stats_report(const char *why) {
	uint64_t now = us_now(); if (!fmst.last_report) fmst.last_report = now;
	fprintf(stderr, "    gpu fence (%s): %u frames done, gpu busy %.1f ms/frame (max %.1f), %.0f%% of %.2f s busy (%u marks), present submit->done latency avg %.1f ms (max %.1f), frames in flight at present avg %.2f (max %llu)\n",
	        why, fmst.nframes, fmst.nframes ? fmst.acc_busy / 1e3 / fmst.nframes : fmst.acc_busy / 1e3, fmst.max_frame / 1e3, now > fmst.last_report ? 100.0 * fmst.acc_busy / (double)(now - fmst.last_report) : 0.0, (now - fmst.last_report) / 1e6, fmst.nmarks,
	        fmst.nframes ? fmst.acc_lat / 1e3 / fmst.nframes : 0.0, fmst.max_lat / 1e3, fmst.npresent ? (double)fmst.acc_inflight / fmst.npresent : 0.0, (unsigned long long)fmst.max_inflight);
	fmst.last_report = now; fmst.acc_busy = fmst.max_frame = fmst.acc_lat = fmst.max_lat = fmst.acc_inflight = fmst.max_inflight = 0; fmst.nframes = fmst.nmarks = fmst.npresent = 0;
}
static void fence_stats_present(void) {
	fence_mark(1);
	uint64_t now = us_now(); if (!fmst.last_report) fmst.last_report = now;
	if (now - fmst.last_report >= 1000000) fence_stats_report("1 s");
}
/* a replay (gfxreplay.c): wait for every mark, then report what the GPU took for the frame(s) drawn */
void gfx_gpu_fence_stats_finish(void) {
	if (!fence_stats_on() || !gpu()) return;
	fence_mark(1);   /* (a present mark after everything: the last frame's total lands) */
	for (int k = 0; k < 20000; k++) { int pending = 0; pthread_mutex_lock(&fm_m); for (int i = fm_r; i != fm_w; i = (i + 1) % NFM) if (fm[i].state == 1) pending = 1; pthread_mutex_unlock(&fm_m); if (!pending) break; usleep(500); }
	fence_mark(0); fence_stats_report("finish");
}

/* a render scale change (F2), between frames: every target goes to guest memory at the old scale and
 * every GPU surface is dropped -- targets come back from guest memory at the new one; depth/stencil
 * surfaces start over (a scene that force-loads one this frame preloads it from guest memory, which
 * the GPU never wrote: one frame's depth may be wrong) */
static void apply_render_scale(int n) {
	batch_flush("render scale change"); readbacks_complete(0, ~0u);
	for (int i = 0; i < ntargets; i++) { TargetEntry *e = &targets[i]; if (e->gpu_dirty) { sync_why = "(render scale change)"; target_download_to_guest(e); } target_release(e); }
	ntargets = 0;
	target_release(&scratch_entry); memset(&scratch_entry, 0, sizeof scratch_entry);
	for (int i = 0; i < MAXDEPTHG; i++) {
		DepthEntry *d = &depthsg[i];
		if (d->tex) SDL_ReleaseGPUTexture(gpu(), d->tex); if (d->pre_z) SDL_ReleaseGPUTexture(gpu(), d->pre_z); if (d->pre_s) SDL_ReleaseGPUTexture(gpu(), d->pre_s);
		memset(d, 0, sizeof *d);
	}
	for (int i = 0; i < nviews; i++) {
		TexEntry *v = &views[i];
		if (v->tex) SDL_ReleaseGPUTexture(gpu(), v->tex);
		for (int a = 0; a < 4; a++) for (int b = 0; b < 2; b++) for (int c = 0; c < 3; c++) for (int d = 0; d < 3; d++) if (v->samp[a][b][c][d]) SDL_ReleaseGPUSampler(gpu(), v->samp[a][b][c][d]);
		memset(v, 0, sizeof *v);
	}
	nviews = 0;
	scene_entry = 0; scene_depth_entry = 0;
	rscale = n; rscale_x = want_rscale_x(n); metrics_set_render_scale(n);
	settings_set_int("render_scale", n); settings_save();   /* F2 / the menu: kept for the next run */
	fprintf(stderr, "gpu: render scale %dx (%dx horizontally)\n", n, rscale_x);
}
/* ---- F5's frame history (RECOMP_FRAME_HISTORY=n presents, default 180 with a window: 3 s at 60 fps, 0 off). A flicker of a frame or two
 * (Killzone's HUD, 2026-10-25) is over before F12's capture starts: the capture caught the frame after it. So the last n
 * presented frames are kept, small, on the GPU (one blit each, 480 pixels wide), with what drew into each displayed buffer
 * (disp_note), and F5 (window.c) writes them out: a line a frame to the log, the pictures to captures/f5_<frame>/. Few draws in
 * a flickered frame = the game didn't draw it; the same count = this renderer lost it. */
#define HIST_MAX 256
#define HIST_W 480u
typedef struct { uint64_t frame; uint32_t base, n[4]; uint8_t from_gpu, img; } HistRec;
static HistRec hist[HIST_MAX]; static uint32_t hist_head;   /* presents recorded; the newest in slot (hist_head - 1) % hist_len() */
static SDL_GPUTexture *hist_tex[HIST_MAX]; static uint32_t hist_w, hist_h;
static int hist_save_mark;   /* an F5 waiting for the next present (its mark number) */
static uint32_t hist_len(void) {
	static int v = -1;
	if (v < 0) { extern int window_active(void); const char *e = getenv("RECOMP_FRAME_HISTORY"); v = e ? atoi(e) : window_active() ? 180 : 0; v = v < 0 ? 0 : v > HIST_MAX ? HIST_MAX : v; }
	return (uint32_t)v;
}
void gfx_gpu_history_save(int mark) { __atomic_store_n(&hist_save_mark, mark, __ATOMIC_RELEASE); }   /* (window.c's F5: any thread) */
static void hist_record(uint32_t base, TargetEntry *e, uint32_t w, uint32_t h, int from_gpu) {
	extern uint64_t display_frames(void);
	uint32_t n[4] = {0};
	for (int i = 0; i < 4; i++) if (disp_count[i].base == base) { memcpy(n, disp_count[i].n, sizeof n); memset(disp_count[i].n, 0, sizeof disp_count[i].n); }
	{	/* RECOMP_FRAME_HISTORY_LOG=1: every present's line as it happens (a headless run's whole record, no F5 needed) */
		static int log = -1; if (log < 0) log = getenv("RECOMP_FRAME_HISTORY_LOG") != 0;
		if (log) fprintf(stderr, "  present frame %llu: buffer %#x, %u draws into it (%u HUD, %u skipped while building, %u on the software path)\n", (unsigned long long)display_frames(), base, n[DISP_DRAWS], n[DISP_HUD], n[DISP_SKIPPED], n[DISP_SOFT]);
	}
	const uint32_t len = hist_len(); if (!len) return;
	const uint32_t slot = hist_head++ % len; HistRec *r = &hist[slot];
	memset(r, 0, sizeof *r); r->frame = display_frames(); r->base = base; r->from_gpu = (uint8_t)from_gpu; memcpy(r->n, n, sizeof r->n);
	if (!e || !e->color || e->cpu_dirty || !w || !h) return;   /* (guest memory newer than the texture: no picture) */
	const uint32_t hw = HIST_W, hh = (uint32_t)((uint64_t)HIST_W * h / w) ? (uint32_t)((uint64_t)HIST_W * h / w) : 1u;
	if (hw != hist_w || hh != hist_h) {   /* the frame's shape changed (aspect ratio, render scale): a new ring */
		for (uint32_t i = 0; i < HIST_MAX; i++) { if (hist_tex[i]) { batch_retire(hist_tex[i], "frame history resized"); SDL_ReleaseGPUTexture(gpu(), hist_tex[i]); hist_tex[i] = 0; } hist[i].img = 0; }
		hist_w = hw; hist_h = hh;
	}
	if (!hist_tex[slot]) {
		SDL_GPUTextureCreateInfo ci = { .type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
			.width = hw, .height = hh, .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1 };
		hist_tex[slot] = SDL_CreateGPUTexture(gpu(), &ci);
	}
	SDL_GPUCommandBuffer *cmd = hist_tex[slot] ? SDL_AcquireGPUCommandBuffer(gpu()) : 0;
	if (!cmd) return;
	SDL_GPUBlitInfo b = { .source = { .texture = e->color, .w = w, .h = h }, .destination = { .texture = hist_tex[slot], .w = hw, .h = hh }, .load_op = SDL_GPU_LOADOP_DONT_CARE, .filter = SDL_GPU_FILTER_LINEAR };
	SDL_BlitGPUTexture(cmd, &b); SDL_SubmitGPUCommandBuffer(cmd);
	r->img = 1;
}
typedef struct { char dir[64]; uint32_t w, h, n; uint64_t frame[HIST_MAX]; uint8_t *px; } HistWrite;
static void *hist_write(void *arg) {   /* (a thread of its own: 180 PNGs, the size a bug report can carry) */
	HistWrite *j = arg; const size_t one = 4u * j->w * j->h;
	for (uint32_t k = 0; k < j->n; k++) {
		char path[128]; snprintf(path, sizeof path, "%s/%llu.png", j->dir, (unsigned long long)j->frame[k]);
		/* bytes R, G, B, A; the target's alpha isn't the picture's (XBGR: ignored, so no frame saves see-through) */
		SDL_Surface *s = SDL_CreateSurfaceFrom((int)j->w, (int)j->h, SDL_PIXELFORMAT_XBGR8888, j->px + one * k, (int)(4 * j->w));
		if (s) { SDL_SavePNG(s, path); SDL_DestroySurface(s); }
	}
	free(j->px); free(j);
	return 0;
}
static void hist_save(int mark) {
	const uint32_t len = hist_len(); if (!len || !hist_head) return;
	const uint32_t n = hist_head < len ? hist_head : len, first = hist_head - n;
	extern uint64_t display_frames(void);
	HistWrite *j = calloc(1, sizeof *j); if (!j) return;
	snprintf(j->dir, sizeof j->dir, "captures/f5_%llu", (unsigned long long)display_frames());
	mkdir("captures", 0755); mkdir(j->dir, 0755);
	fprintf(stderr, "frame history at F5 mark %d: the last %u presents (pictures in %s/):\n", mark, n, j->dir);
	for (uint32_t k = first; k < hist_head; k++) {
		const HistRec *r = &hist[k % len];
		fprintf(stderr, "  frame %llu: buffer %#x, %u draws into it (%u HUD, %u skipped while building, %u on the software path), shown from %s%s\n", (unsigned long long)r->frame, r->base,
		        r->n[DISP_DRAWS], r->n[DISP_HUD], r->n[DISP_SKIPPED], r->n[DISP_SOFT], r->from_gpu ? "the GPU" : "guest memory", r->img ? "" : ", no picture");
	}
	for (uint32_t k = first; k < hist_head; k++) if (hist[k % len].img && hist_tex[k % len]) j->n++;
	const size_t one = 4u * hist_w * hist_h;
	SDL_GPUTransferBuffer *xb = 0;
	if (j->n) { SDL_GPUTransferBufferCreateInfo xci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = (Uint32)(one * j->n) }; xb = SDL_CreateGPUTransferBuffer(gpu(), &xci); }
	SDL_GPUCommandBuffer *cmd = xb ? SDL_AcquireGPUCommandBuffer(gpu()) : 0;
	if (!cmd) { if (xb) SDL_ReleaseGPUTransferBuffer(gpu(), xb); free(j); return; }
	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd); uint32_t i = 0;
	for (uint32_t k = first; k < hist_head; k++) {
		const HistRec *r = &hist[k % len]; if (!r->img || !hist_tex[k % len]) continue;
		SDL_GPUTextureRegion src = { .texture = hist_tex[k % len], .w = hist_w, .h = hist_h, .d = 1 };
		SDL_GPUTextureTransferInfo dst = { .transfer_buffer = xb, .offset = (Uint32)(one * i), .pixels_per_row = hist_w, .rows_per_layer = hist_h };
		SDL_DownloadFromGPUTexture(cp, &src, &dst); j->frame[i++] = r->frame;
	}
	SDL_EndGPUCopyPass(cp);
	SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
	if (f) { SDL_WaitForGPUFences(gpu(), true, &f, 1); SDL_ReleaseGPUFence(gpu(), f); }
	const uint8_t *px = SDL_MapGPUTransferBuffer(gpu(), xb, false);
	j->w = hist_w; j->h = hist_h; j->px = px ? malloc(one * j->n) : 0;
	if (j->px) memcpy(j->px, px, one * j->n);
	if (px) SDL_UnmapGPUTransferBuffer(gpu(), xb);
	SDL_ReleaseGPUTransferBuffer(gpu(), xb);
	pthread_t t;
	if (!j->px || pthread_create(&t, 0, hist_write, j)) { free(j->px); free(j); return; }
	pthread_detach(t);
}
void gfx_gpu_present(uint32_t base, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format) {
	static int stats = -1; if (stats < 0) stats = getenv("RECOMP_GPU_STATS") != 0;
	render_scale_init();
	hud_note_present(base);
	if (gpu()) { batch_flush("present"); readbacks_complete_at_present(); vis_complete(0); }   /* normally empty: the scene's end flushed; the CPU reads its readbacks from the next frame on */
	if (gpu() && fence_stats_on()) fence_stats_present();
	if (stats) {
		fprintf(stderr, "    gpu stats: frame %llu: %u draws on the GPU (%llu indices), %u on the software path this frame, %u skipped (building), %u readbacks, tables %d shaders %d pipelines", (unsigned long long)gpu_frame_no, n_gpu_draws, (unsigned long long)n_gpu_indices, n_fallback, n_async_skips, n_readbacks, nshaders, npipelines); n_readbacks = 0; n_gpu_indices = 0; if (!hitch_log()) n_async_skips = 0;
		for (int i = 0; i < n_fallback_why; i++) fprintf(stderr, "%s %u %s", i ? "," : ":", fallback_why[i].n, fallback_why[i].why);
		fprintf(stderr, "\n    gpu time: %.1f ms = setup %.1f + vertex fetch %.1f (%u ring wraps) + shaders %.1f (%u built) + pipelines %.1f + vertex upload %.1f + texture binding %.1f (hash %.1f, upload %.1f [%u textures, %u as BC blocks], target sync %.1f, views %.1f) + scene target upload %.1f + pass %.1f + submit %.1f + other %.1f [map %.1f acquire %.1f]\n",
		        t_total / 1e3, t_setup / 1e3, t_vfetch / 1e3, n_vring_wraps, t_shader / 1e3, n_shader_builds, t_pipe / 1e3, t_vbuf / 1e3, t_bind / 1e3, t_texhash / 1e3, t_texup / 1e3, n_tex_uploads, n_tex_bc, t_tsync / 1e3, t_view / 1e3, t_scene_sync / 1e3, t_pass / 1e3, t_submit / 1e3, ((double)t_total - (double)(t_setup + t_vfetch + t_shader + t_pipe + t_vbuf + t_bind + t_scene_sync + t_pass + t_submit)) / 1e3, t_map / 1e3, t_acq / 1e3);   /* signed: the syncs at present/guest-write time are outside t_total */
		{	uint32_t wf = 0, wm = 0, wp = 0; gwatch_stats(&wf, &wm, &wp);   /* change detection (get_texture): its time is the "hash" bucket above */
			fprintf(stderr, "    gpu visibility: %u queries, %u SET tests answered visible unseen\n", n_vis_queries, n_vis_unknown); n_vis_queries = n_vis_unknown = 0;
			if (n_ao) { fprintf(stderr, "    gpu ssao: %u scenes\n", n_ao); n_ao = 0; }
			if (n_hud_draws || t_hud) { fprintf(stderr, "    gpu hud: %u draws at their native shape (%.2f ms finding them)\n", n_hud_draws, t_hud / 1e3); n_hud_draws = 0; t_hud = 0; }
			fprintf(stderr, "    gpu uploads: decode %.2f ms, transfer buffers %.2f ms, command buffers %.2f ms\n", t_up_decode / 1e3, t_up_xfer / 1e3, t_up_submit / 1e3); t_up_decode = t_up_xfer = t_up_submit = 0;
			fprintf(stderr, "    gpu textures: %u binds, %u re-checked (%llu pages hashed), %u changed, %u new, %u moved, %u deferred to the next frame%s; watch %s: %u write faults, %u bracketed host writes, %u pages closed\n",
			        n_tex_binds, n_tex_checked, (unsigned long long)n_pages_hashed, n_tex_changed, n_tex_new, n_tex_moved, n_tex_deferred, n_tex_missed ? " -- WATCH MISSED WRITES" : "", gwatch_on() ? "on" : "off", wf, wm, wp);
			n_tex_binds = n_tex_checked = n_tex_changed = n_tex_new = n_tex_moved = n_tex_missed = n_tex_deferred = 0; n_pages_hashed = 0; }
		/* the scene command buffers (one copy pass + one render pass each) and why each batch was submitted */
		fprintf(stderr, "    gpu batch: %u draws in %u scene command buffers, %u render passes (%u FRAGCOLOR copies, %u narrowed, %u additive, and %u view passes: %u hoisted to the head, %u in draw order, %u in their own command buffer), %u LDST windows placed, %u reused, flushed at", n_batch_draws, n_batch_cmd, n_batch_pass, n_batch_fetch, n_fetch_narrowed, n_batch_additive, n_batch_views, n_views_hoisted, n_views_inorder, n_views_alone, n_ldst_new, n_ldst_reused);
		for (int i = 0; i < n_flush_why; i++) fprintf(stderr, "%s %s %u", i ? "," : "", flush_why[i].why, flush_why[i].n);
		fprintf(stderr, "\n");
		n_batch_additive = n_ldst_new = n_ldst_reused = 0;
		n_batch_draws = n_batch_cmd = n_batch_pass = n_batch_fetch = n_fetch_narrowed = n_batch_views = n_views_hoisted = n_views_inorder = n_views_alone = 0; n_flush_why = 0; t_flush = 0;
		__atomic_store_n(&n_gpu_draws, 0, __ATOMIC_RELAXED); __atomic_store_n(&n_fallback, 0, __ATOMIC_RELAXED); n_fallback_why = 0; n_shader_builds = n_vring_wraps = 0; n_tex_uploads = n_tex_bc = 0; t_total = t_vfetch = t_texhash = t_texup = t_tsync = t_view = t_submit = t_shader = t_pipe = t_bind = t_setup = t_vbuf = t_pass = t_map = t_acq = t_scene_sync = 0;
	}
	gpu_frame_no++;   /* textures are re-checked against guest memory from here on */
	if (gpu()) recipes_step();
	if (gpu()) {
		GfxTarget t = { base, 0, width, height, pitch, 0 };
		/* the display buffer as the game last rendered it: a U8U8U8U8 surface, plain or sRGB (the display scans out
		 * the bytes either way). get_target with the display's format 0 found only a plain one: Killzone renders its
		 * front end into an sRGB surface (RECOMP_SRGB=1), so present made a new, empty plain target over the same
		 * memory and every frame from the first logo to the level came out black */
		TargetEntry *e = 0;
		for (int i = 0; i < ntargets; i++) {
			TargetEntry *x = &targets[i];
			if (x->addr == base && x->color && x->width == pitch && x->height == height && x->type == 0 && (x->kind == TK_U8 || x->kind == TK_U8S) && (!e || x->gen > e->gen)) e = x;
		}
		if (e) e->used = ++target_clock; else e = get_target(&t);
		static const char *dump = (const char *)1; if (dump == (const char *)1) dump = getenv("RECOMP_GPU_DUMP_PRESENT");
		static int shots = -1; if (shots < 0) shots = getenv("RECOMP_SHOT") != 0;
		if (dump && e->color && (e->kind == TK_U8 || e->kind == TK_U8S) && (!shots || gfx_soft_shot_due())) {   /* (what the GPU drew last: a replay may have restored guest memory over it since); with RECOMP_SHOT, on the shot frames, "%u" in the path the frame */   /* RECOMP_GPU_DUMP_PRESENT=path: the frame at the render scale (guest memory and RECOMP_SHOT hold it at 1x), overwritten each present */
			target_sync_for_gpu(e);   /* (a frame rendered before a render scale change is only in guest memory) */
			uint32_t w = TSX(e, width), h = TSY(e, height), n = 4 * w * h;
			SDL_GPUTransferBufferCreateInfo xci = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = n };
			SDL_GPUTransferBuffer *xb = SDL_CreateGPUTransferBuffer(gpu(), &xci);
			SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu()); SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
			SDL_GPUTextureRegion src = { .texture = e->color, .w = w, .h = h, .d = 1 };
			SDL_GPUTextureTransferInfo dst = { .transfer_buffer = xb, .pixels_per_row = w, .rows_per_layer = h };
			SDL_DownloadFromGPUTexture(cp, &src, &dst); SDL_EndGPUCopyPass(cp);
			SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd); SDL_WaitForGPUFences(gpu(), true, &f, 1); SDL_ReleaseGPUFence(gpu(), f);
			const uint8_t *px = SDL_MapGPUTransferBuffer(gpu(), xb, false);
			char path[512]; snprintf(path, sizeof path, dump, (unsigned)gpu_frame_no);
			FILE *fo = fopen(path, "wb");
			if (fo && px) { fprintf(fo, "P6\n%u %u\n255\n", w, h); for (uint32_t i = 0; i < w * h; i++) fwrite(px + 4 * i, 1, 3, fo); }
			if (fo) fclose(fo);
			SDL_UnmapGPUTransferBuffer(gpu(), xb); SDL_ReleaseGPUTransferBuffer(gpu(), xb);
		}
		extern int window_active(void); extern void window_present_gpu(void *tex, uint32_t w, uint32_t h);
		static int no_gpu_present = -1; if (no_gpu_present < 0) no_gpu_present = getenv("RECOMP_GPU_NO_PRESENT") != 0;   /* RECOMP_GPU_NO_PRESENT=1: present through guest memory as before */
		const int via_gpu = e->gpu_dirty && !gfx_soft_shot_due() && format == 0 && window_active() && !no_gpu_present && e->kind != TK_U8S;
		hist_record(base, e, TSX(e, width), TSY(e, height), via_gpu);
		{ const int m = __atomic_exchange_n(&hist_save_mark, 0, __ATOMIC_ACQ_REL); if (m) hist_save(m); }
		{ static long at = -2; if (at == -2) { const char *v = getenv("RECOMP_FRAME_HISTORY_SAVE_AT"); at = v ? atol(v) : -1; }   /* RECOMP_FRAME_HISTORY_SAVE_AT=frame: F5's save without a window (tests) */
		  extern uint64_t display_frames(void); if (at >= 0 && (long)display_frames() >= at) { at = -1; hist_save(0); } }
		if (via_gpu) {   /* (an sRGB-format frame would decode in the blit: it presents from its raw bytes below) */
			/* the frame goes to the window from the GPU texture: no fence wait, encode and re-upload
			 * per frame. Guest memory keeps the frame before this one until something reads it (a
			 * shot, a texture over it, a CPU write: each syncs first) */
			window_present_gpu(e->color, TSX(e, width), TSY(e, height)); gfx_soft_skip_window = 1;
		} else if (!window_active() && !gfx_soft_shot_due() && format == 0 && present_nosync()) { /* RECOMP_GPU_PRESENT_NOSYNC=1, headless: the frame stays on the GPU, as it does with a window (see present_nosync); window_present below is a no-op without a window */ }
		else { sync_why = "(present)"; target_sync_for_cpu(e); }   /* guest memory must hold what the GPU rendered before anything reads it below */
	}
	if (hitch_log()) {   /* this frame's first-use work and waits, for display.c's hitch line; then the counters start the next frame (RECOMP_GPU_STATS, if also on, zeroed them above already: run one or the other) */
		GfxGpuFrameCosts c = { .draws = __atomic_load_n(&n_gpu_draws, __ATOMIC_RELAXED), .fallback = __atomic_load_n(&n_fallback, __ATOMIC_RELAXED), .shaders = n_shader_builds, .pipes = n_pipe_builds, .tex_uploads = n_tex_uploads, .tex_new = n_tex_new,
			.tgt_downloads = n_tgt_downloads, .readback_waits = n_readback_waits, .ring_waits = n_ring_waits, .flushes = n_batch_cmd,
			.t_total = t_total, .t_shader = t_shader, .t_xlate = t_shader_xlate, .t_msl = t_shader_msl, .t_compile = t_shader_compile, .t_pipe = t_pipe_build, .t_texup = t_texup, .t_tgt_download = t_tgt_download,
			.t_readback_wait = t_readback_wait, .t_ring_wait = t_ring_wait, .t_vfetch = t_vfetch, .t_tsync = t_tsync, .t_scene_sync = t_scene_sync, .t_vfetch_max = t_vfetch_max, .t_map = t_map, .shaders_cached = n_shader_cached, .skipped = n_async_skips };
		for (int i = 0; i < n_fallback_why && i < 4; i++) { c.fb_why[i] = fallback_why[i].why; c.fb_n[i] = fallback_why[i].n; }
		c.nshaders = (uint32_t)nshaders; c.npipes = (uint32_t)npipelines;
		last_costs = c;
		__atomic_store_n(&n_gpu_draws, 0, __ATOMIC_RELAXED); __atomic_store_n(&n_fallback, 0, __ATOMIC_RELAXED); n_fallback_why = 0; n_shader_builds = n_vring_wraps = 0; n_tex_uploads = n_tex_bc = 0; n_tex_new = 0; n_batch_cmd = 0;
		n_pipe_builds = n_tgt_downloads = n_readback_waits = n_ring_waits = 0; n_async_skips = 0;
		t_total = t_vfetch = t_texhash = t_texup = t_tsync = t_view = t_submit = t_shader = t_pipe = t_bind = t_setup = t_vbuf = t_pass = t_map = t_acq = t_scene_sync = 0;
		t_pipe_build = t_tgt_download = t_readback_wait = t_ring_wait = t_shader_xlate = t_shader_msl = t_shader_compile = t_vfetch_max = 0; n_shader_cached = 0;
	}
	/* RECOMP_SHOT, RECOMP_USSE_PROFILE and (unless done above) window_present all live in gfx_soft_present */
	gfx_soft_present(base, width, height, pitch, format);
	{	/* RECOMP_RENDER_SCALE_SWITCH=frame:n,...: request scale n after that present -- F2 without a window, for tests */
		static const char *sw = (const char *)1; if (sw == (const char *)1) sw = getenv("RECOMP_RENDER_SCALE_SWITCH");
		for (const char *p = sw; p && *p; p += strcspn(p, ",") + (p[strcspn(p, ",")] != 0)) {
			unsigned long f = strtoul(p, 0, 10); const char *c = strchr(p, ':');
			if (c && f == gpu_frame_no) __atomic_store_n(&rscale_req, atoi(c + 1), __ATOMIC_RELAXED);
		}
	}
	{	/* RECOMP_SETTINGS_SWITCH=frame:key=value,...: a setting changed after that present, as the menu would: an A/B inside one
		 * scripted run (fov 1 then 1.5 at the same spot) instead of two runs to the same frame */
		static const char *sw = (const char *)1; if (sw == (const char *)1) sw = getenv("RECOMP_SETTINGS_SWITCH");
		for (const char *p = sw; p && *p; p += strcspn(p, ",") + (p[strcspn(p, ",")] != 0)) {
			const unsigned long f = strtoul(p, 0, 10); const char *c = strchr(p, ':'), *eq = c ? strchr(c, '=') : 0; const size_t n = strcspn(p, ",");
			if (!c || !eq || eq >= p + n || f != gpu_frame_no) continue;
			char k[48], v[48]; snprintf(k, sizeof k, "%.*s", (int)(eq - c - 1), c + 1); snprintf(v, sizeof v, "%.*s", (int)(p + n - eq - 1), eq + 1);
			settings_set_str(k, v); fprintf(stderr, "    settings switch: frame %lu: %s = %s\n", f, k, v);
		}
	}
}
