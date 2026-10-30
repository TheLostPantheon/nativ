/*
 * The GPU driver's kernel side (SceGpuEs4ForUser), under Sony's own user-mode stack: the game's
 * GXM calls run libgxm, which calls libgpu_es4 (the PowerVR services client), which reaches the
 * kernel only through these system calls -- chiefly PVRSRV_BridgeDispatchKM(id, package), one
 * entry point multiplexing the driver's bridge functions by id.
 *
 * The bridge map (from SceGpuEs4.skprx's dispatch table, which its init fills in): ids are 0x1000 +
 * the services bridge index; 0x1000-0x1093, of which these have a handler on the console -- every
 * other id is the driver's "not supported" stub:
 *   0x1000-0x1007, 0x100c-0x100d, 0x101d-0x1022, 0x102a, 0x104d-0x1050, 0x1055-0x1059,
 *   0x105b-0x105c, 0x105e-0x1060, 0x1067, 0x106c, 0x1073, 0x1075, 0x1083-0x1093
 * A package is { id, size (0x1c), in, in size, out, out size, kernel services handle }; a handler's
 * output starts with its PVRSRV_ERROR (0 = PVRSRV_OK), copied out only when the handler succeeds.
 * The dispatcher returns the handler's result: 0, or -25 (the "not supported" stub's), or 0x18
 * (busy: the client sleeps and retries), or -14 (a bad package).
 *
 * This is the device side of the platform (hardware below the firmware's logic): it is emulated,
 * not ported. Each handler gives what the client's code reads back -- handles, device memory, sync
 * objects -- with the GPU itself being the host's renderer, reached at the GXM boundary.
 * RECOMP_LOG_GPU=1 logs every bridge call (id, sizes, the first input words).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"

static int log_gpu(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_LOG_GPU"); v = e && *e == '1'; } return v; }

typedef struct {
	uint32_t id, in, in_size, out, out_size;
} Bridge;

/* the services handles the driver hands out: opaque cookies the client passes back */
static uint32_t next_handle = 0x1000;
static uint32_t handle(void) { return next_handle += 4; }

/* ---- the bridge functions */

/* 0x100c CONNECT_SERVICES: out { error, kernel services handle } */
static void connect_services(Bridge *b) { WR32(b->out + 4, handle()); }

/* 0x1000 ENUM_DEVICES: out { error, device count, identifiers[...] (type, class, index, ...) } --
 * one SGX (PVRSRV_DEVICE_TYPE_SGX = 7, PVRSRV_DEVICE_CLASS_3D = 0) */
static void enum_devices(Bridge *b) {
	WR32(b->out + 4, 1);
	WR32(b->out + 8, 7); WR32(b->out + 12, 0); WR32(b->out + 16, 0);
}

/* SGX_GETMISCINFO: answer the request in an SGX_MISC_INFO (request at +0, its data from +8) */
static void misc_info(uint32_t mi) {
	uint32_t req = RD32(mi);
	switch (req) {
	case 2:   /* the driver's revision: libgpu_es4 accepts only the kernel it was built with --
	           * DDK version 0x10711 (+0x10), build 5029 (+0x14), build options 0x30322518 (+0x20) */
		WR32(mi + 0x10, 0x10711); WR32(mi + 0x14, 5029); WR32(mi + 0x20, 0x30322518);
		break;
	default:
		fprintf(stderr, "gpu misc info request %u (unimplemented)\n", req);
	}
}

/* 0x102a GET_MISC_INFO: in { flags, PVRSRV_MISC_INFO (0x98) }, out { error, the info }. The Vita's
 * kernel answers only requests 0x08, 0x10 and 0x80 (anything else: PVRSRV_ERROR_INVALID_PARAMS = 3);
 * of those only 0x10 fills anything -- the DDK version (+0x88: 1.7.17.5029) and, into the caller's
 * buffer (+0x8, size +0xc), its string */
static void get_misc_info(Bridge *b) {
	uint32_t mi = b->out + 4;
	memcpy(G(mi), G(b->in + 4), 0x98);
	uint32_t req = RD32(mi);
	WR32(mi + 4, 0);
	if (req & ~0x98u) { WR32(b->out, 3); return; }
	if (req & 0x10 && RD32(mi + 8)) {
		WR32(mi + 4, 0x10);
		WR32(mi + 0x88, 1); WR32(mi + 0x8c, 7); WR32(mi + 0x90, 17); WR32(mi + 0x94, 5029);
		if (RD32(mi + 0xc) < 12) { WR32(b->out, 3); return; }
		snprintf((char *)G(RD32(mi + 8)), RD32(mi + 0xc), "1.7.17.5029");
	}
}

/* ---- GPU memory: the device's heaps and the device addresses the client is given.
 * The GPU's view of memory is the CPU's (device address = CPU address) for plain mappings and
 * device-memory allocations; the shader-code heaps' mappings (vertex/fragment USSE) get device
 * addresses in their own ranges, which libgxm turns into heap-relative USSE offsets. The
 * host records every such range so the rendering boundary can find the CPU memory behind a device
 * address (gpu_dev_to_cpu). */
typedef struct { uint32_t id, base, size, next; } Heap;
/* the heaps libgxm needs (it fails Initialize without all seven): the ids, and device ranges that
 * keep its USSE offset arithmetic valid -- the fragment USSE ring's offset must land in bank 4..7 or
 * 9 (sceGxmCreateContext checks), so fragment code is placed from +16 MB of its heap */
static Heap heaps[] = {
	{0x00, 0x00000000, 0xf0000000, 0},            /* general (device = CPU addresses) */
	{0x04, 0x20000000, 0x08000000, 0x01000000},   /* fragment USSE */
	{0x05, 0x10000000, 0x08000000, 0},            /* vertex USSE */
	{0x08, 0x30000000, 0x01000000, 0},            /* sync info */
	{0x0e, 0x31000000, 0x01000000, 0},
	{0x12, 0x32000000, 0x01000000, 0},
	{0x13, 0x33000000, 0x01000000, 0},
};
#define NHEAPS (int)(sizeof heaps / sizeof *heaps)
static uint32_t heap_handle[NHEAPS];
static Heap *heap_by_handle(uint32_t h) { for (int i = 0; i < NHEAPS; i++) if (h && heap_handle[i] == h) return &heaps[i]; return 0; }

/* device range -> CPU memory, for the ranges whose device addresses differ from their CPU ones */
static struct { uint32_t dev, cpu, size; } ranges[1024];
static int nranges;
static void add_range(uint32_t dev, uint32_t cpu, uint32_t size) {
	for (int i = 0; i < nranges; i++) if (ranges[i].dev == dev) { ranges[i].cpu = cpu; ranges[i].size = size; return; }
	if (nranges < 1024) { ranges[nranges].dev = dev; ranges[nranges].cpu = cpu; ranges[nranges++].size = size; }
	else fprintf(stderr, "gpu: device range table full\n");
}
static void remove_range(uint32_t dev) { for (int i = 0; i < nranges; i++) if (ranges[i].dev == dev) { ranges[i] = ranges[--nranges]; return; } }
uint32_t gpu_dev_to_cpu(uint32_t dev) {
	for (int i = 0; i < nranges; i++) if (dev - ranges[i].dev < ranges[i].size) return ranges[i].cpu + (dev - ranges[i].dev);
	return dev;
}
/* a device range in a heap: page granular, bump allocated (the client unmaps rarely; freed ranges aren't reused) */
static uint32_t heap_carve(Heap *h, uint32_t size) {
	size = (size + 0xfff) & ~0xfffu;
	if (h->next + size > h->size) { fprintf(stderr, "gpu: heap %#x exhausted\n", h->id); return 0; }
	uint32_t d = h->base + h->next; h->next += size;
	return d;
}

/* 0x1003 CREATE_DEVMEMCONTEXT (_, device cookie): out { error, context handle, heap count,
 * heaps[] of { id, heap handle, device base, size, attributes, 1 } } */
static void create_devmem_context(Bridge *b) {
	WR32(b->out + 4, handle()); WR32(b->out + 8, NHEAPS);
	for (int i = 0; i < NHEAPS; i++) {
		uint32_t e = b->out + 12 + 0x18 * i;
		if (!heap_handle[i]) heap_handle[i] = handle();
		WR32(e, heaps[i].id); WR32(e + 4, heap_handle[i]); WR32(e + 8, heaps[i].base);
		WR32(e + 12, heaps[i].size); WR32(e + 16, 0); WR32(e + 20, 1);
	}
}

/* the driver's object tables grow as the client makes objects (a freed slot is reused): these were
 * fixed arrays, and a full one dropped the object silently -- Killzone's campaign start makes more
 * than 64 live render targets, the 65th was never recorded, RT_GET_MEMBLOCK on it failed, and the
 * game (which doesn't check that result) freed the stale UID left on its stack: a std::out_of_range
 * throw from its allocator's map. An object lookup that fails says so. */
#define TABLE_SLOT(tab, n, cap) ({ int _i; for (_i = 0; _i < n && (tab)[_i].h; _i++) ; \
	if (_i == n) { if (n == cap) { cap = cap ? 2 * cap : 256; tab = realloc(tab, cap * sizeof *(tab)); } n++; } \
	memset(&(tab)[_i], 0, sizeof *(tab)); &(tab)[_i]; })

/* device-memory allocations by their kernel meminfo handle (a kick names the words it writes by
 * meminfo + offset) */
static struct MemInfo { uint32_t h, cpu, size; } *meminfos;
static int nmeminfos, cmeminfos;
static uint32_t meminfo_cpu(uint32_t h) {
	for (int i = 0; i < nmeminfos; i++) if (meminfos[i].h && meminfos[i].h == h) return meminfos[i].cpu;
	return 0;
}

/* 0x1006 ALLOC_DEVICEMEM (_, device cookie, heap handle, attributes, size, alignment, _): out { error,
 * meminfo handle, CPU address, device address, flags, size, 0, client meminfo, meminfo handle } */
static void alloc_devicemem(Bridge *b) {
	Heap *h = heap_by_handle(RD32(b->in + 8));
	uint32_t attr = RD32(b->in + 12), size = RD32(b->in + 16), align = RD32(b->in + 20);
	if (!h || !size) { WR32(b->out, 3); return; }
	/* the device address is the CPU address: libgxm uses these allocations (the GPU read pointers,
	 * notification and sync words) through plain CPU pointers, as the GPU MMU maps user memory at the
	 * same addresses on the console. Only the USSE code heaps' mappings (MAP_MEMORY) differ. */
	uint32_t cpu = guest_alloc(size, align > 16 ? align : 16), dev = cpu;
	memset(G(cpu), 0, size);
	uint32_t mh = handle();
	struct MemInfo *m = TABLE_SLOT(meminfos, nmeminfos, cmeminfos);
	m->h = mh; m->cpu = cpu; m->size = size;
	WR32(b->out + 4, mh); WR32(b->out + 8, cpu); WR32(b->out + 12, dev); WR32(b->out + 16, attr);
	WR32(b->out + 20, size); WR32(b->out + 28, mh); WR32(b->out + 32, mh);
}

/* 0x101d MAP_MEMORY (_, device cookie, context, heap handle or 0, base, size, max size, attributes):
 * out { error, device address } -- the CPU address itself without a heap, else a range of the heap */
static void map_memory(Bridge *b) {
	uint32_t base = RD32(b->in + 16), size = RD32(b->in + 20), max = RD32(b->in + 24);
	Heap *h = heap_by_handle(RD32(b->in + 12));
	if ((base | size) & 0xfff || !size || (max && size > max)) { WR32(b->out, 3); return; }
	uint32_t dev = base;
	if (h && h->base) { dev = heap_carve(h, size); if (!dev) { WR32(b->out, 3); return; } add_range(dev, base, size); }
	WR32(b->out + 4, dev);
}

/* render targets: what CREATE_RENDER_TARGET's descriptor said about its driver memory block, for
 * RT_GET_MEMBLOCK */
static struct RenderTarget { uint32_t h, uid, owned, width, height, msaa, flags; } *rts;
static int nrts, crts;
static struct RenderTarget *rt_find(uint32_t h, const char *what) {
	for (int i = 0; i < nrts; i++) if (h && rts[i].h == h) return &rts[i];
	fprintf(stderr, "gpu: %s: render target handle %#x unknown\n", what, h);
	return 0;
}
static void create_render_target(Bridge *b) {   /* 0x1086 (_, device cookie, descriptor 0x44): out { error, handle } --
                                                 * descriptor: +0x14 width, +0x18 height, +0x1c multisample x:16 y:16,
                                                 * +0x34 driver memblock UID, +0x38 libgxm allocated it */
	uint32_t d = b->in + 8, h = handle();
	struct RenderTarget *r = TABLE_SLOT(rts, nrts, crts);
	r->h = h; r->uid = RD32(d + 0x34); r->owned = RD32(d + 0x38);
	r->width = RD32(d + 0x14); r->height = RD32(d + 0x18); r->msaa = RD32(d + 0x1c); r->flags = RD32(d);
	WR32(b->out + 4, h);
}
static void render_target_info(Bridge *b) {   /* 0x108b (_, handle): out { error, width, height, multisample x:16 y:16, flags bit 17 } (BeginScene) */
	struct RenderTarget *r = rt_find(RD32(b->in + 4), "RT_GET_INFO");
	if (!r) { WR32(b->out, 3); return; }
	WR32(b->out + 4, r->width); WR32(b->out + 8, r->height); WR32(b->out + 12, r->msaa); WR32(b->out + 16, (r->flags >> 17) & 1);
}
/* a render target's size and samples per pixel (multisample x:16 y:16 as CREATE_RENDER_TARGET's
 * descriptor gave them: NONE 1x1, 2X 1x2, 4X 2x2), 0 for an unknown handle -- for gxm_sony.c's
 * BeginScene decode */
uint32_t gpu_render_target_info(uint32_t handle, uint32_t *width, uint32_t *height) {
	for (int i = 0; i < nrts; i++) if (rts[i].h && rts[i].h == handle) { *width = rts[i].width; *height = rts[i].height; return rts[i].msaa; }
	return 0;
}
static void render_target_memblock(Bridge *b) {   /* 0x1089 (_, handle): out { error, memblock UID, owned by libgxm } */
	struct RenderTarget *r = rt_find(RD32(b->in + 4), "RT_GET_MEMBLOCK");
	if (!r) { WR32(b->out, 3); return; }
	WR32(b->out + 4, r->uid); WR32(b->out + 8, r->owned);
}
static void destroy_render_target(Bridge *b) {   /* 0x1087 (_, device cookie, render context, handle) */
	struct RenderTarget *r = rt_find(RD32(b->in + 12), "DESTROY_RENDER_TARGET");
	if (r) r->h = 0;
}

/* 0x108c KICK (in 0x2fc): a scene's (or a mid-scene flush's) GPU work. The host drew it already (gxm_sony.c's
 * observers), so it completes at once: the words the GPU writes when it finishes -- libgxm's ring status words
 * (which sceGxmFinish and the ring refill wait on) and the scene's notifications -- are listed in the kick as
 * records { control, value, meminfo }, control = 0x400000 | (offset + 0xf): the vertex phase's (count at
 * +0x104, up to 32, records from +0x140), then the fragment phase's (count at +0x108, up to 4, from +0x2c0) */
static uint32_t signal_counter;
static void kick_writes(uint32_t recs, uint32_t n) {
	for (uint32_t k = 0; k < n; k++) {
		uint32_t r = recs + 12 * k, ctl = RD32(r), cpu = meminfo_cpu(RD32(r + 8));
		if (cpu && (ctl & 0xfffff) >= 0xf) WR32(cpu + (ctl & 0xfffff) - 0xf, RD32(r + 4));
		else fprintf(stderr, "gpu kick: write record %#x %#x %#x not understood\n", ctl, RD32(r + 4), RD32(r + 8));
	}
}
static void signal_bump(void) { __atomic_add_fetch(&signal_counter, 1, __ATOMIC_SEQ_CST); }
static void kick(Bridge *b) {
	uint32_t na = RD32(b->in + 0x104), nb = RD32(b->in + 0x108);
	kick_writes(b->in + 0x140, na < 32 ? na : 32);
	/* the fragment phase's writes (the scene's notification) wait for its visibility counts when those are still on the
	 * GPU: the renderer writes them right after the counts land (gfx_dispatch.c gfx_kick_defer) */
	uint32_t addr[4], val[4]; int n = 0, ok = 1; if (nb > 4) nb = 4;
	for (uint32_t k = 0; k < nb; k++) {
		uint32_t r = b->in + 0x2c0 + 12 * k, ctl = RD32(r), cpu = meminfo_cpu(RD32(r + 8));
		if (!cpu || (ctl & 0xfffff) < 0xf) { ok = 0; break; }
		addr[n] = cpu + (ctl & 0xfffff) - 0xf; val[n] = RD32(r + 4); n++;
	}
	extern int gfx_kick_defer(const uint32_t *addr, const uint32_t *val, int n); extern void (*gfx_kick_done_hook)(void);
	gfx_kick_done_hook = signal_bump;
	if (!(ok && n && gfx_kick_defer(addr, val, n))) kick_writes(b->in + 0x2c0, nb);
	signal_bump();
}

/* 0x108f SUBMIT_TRANSFER (in 0x94): sceGxmTransferCopy/Downscale/Fill. The blit itself already
 * happened at the GXM boundary (gxm_sony.c's observers, which see the typed surfaces this bridge
 * call's opaque 2D-blit command words don't); this only signals completion -- the same single
 * notification record { ctl, value, meminfo } kick_writes already knows how to apply, present only
 * when in[0x90] is set (+0x84 the record, +0x90 the flag) */
static void transfer_submit(Bridge *b) {
	if (RD32(b->in + 0x90)) kick_writes(b->in + 0x84, 1);
	__atomic_add_fetch(&signal_counter, 1, __ATOMIC_SEQ_CST);
}

static int bridge(Bridge *b) {
	switch (b->id) {
	case 0x1000: enum_devices(b); return 1;
	case 0x1001: WR32(b->out + 4, handle()); return 1;   /* ACQUIRE_DEVICEINFO (_, index, type): out { error, device cookie } */
	case 0x1003: create_devmem_context(b); return 1;
	case 0x1004: return 1;                                /* DESTROY_DEVMEMCONTEXT */
	case 0x1006: alloc_devicemem(b); return 1;
	case 0x1007:   /* FREE_DEVICEMEM (_, device cookie, meminfo): the handle retires (guest memory isn't reclaimed) */
		for (int i = 0; i < nmeminfos; i++) if (meminfos[i].h && meminfos[i].h == RD32(b->in + 8)) meminfos[i].h = 0;
		return 1;
	case 0x100c: connect_services(b); return 1;
	case 0x100d: return 1;                                /* DISCONNECT_SERVICES */
	case 0x101d: map_memory(b); return 1;
	case 0x101e: remove_range(RD32(b->in + 4)); return 1;   /* UNMAP_MEMORY (_, device address, heap, _) */
	case 0x102a: get_misc_info(b); return 1;
	case 0x104d: WR32(b->out + 4, handle()); return 1;   /* DISPLAY_CREATE: out { error, display handle } */
	case 0x104e: return 1;                                /* DISPLAY_DESTROY */
	case 0x1055: WR32(b->out + 4, handle()); return 1;   /* SYNC_OBJECT_CREATE (_, device cookie): out { error, sync info handle } -- only a handle */
	case 0x1056: return 1;                                /* SYNC_OBJECT_DESTROY */
	case 0x1058: {   /* REGISTER_MEMBLOCK (_, memblock UID, id, flags): out { error, id } -- the id given, else a new one
	                  * (from 2: libgxm passes it on as ALLOC_DEVICEMEM's group, which the kernel requires > 1). The
	                  * kernel checks the block is a user one of memtype 0x8000 and lists it */
		static uint32_t next_group = 2;
		WR32(b->out + 4, RD32(b->in + 8) ? RD32(b->in + 8) : next_group++);
		return 1;
	}
	case 0x1059: return 1;                                /* UNREGISTER_MEMBLOCK */
	case 0x105e:   /* SGX_GETCLIENTINFO (_, device cookie): out SGX_CLIENT_INFO { process id, ..., +0xa0: 0x60 bytes of
	                * the kernel's per-device client block }, then the error at +0x100 (libgxm reads none of the block) */
		WR32(b->out, 1);
		return 1;
	case 0x105f: return 1;                                /* SGX_RELEASECLIENTINFO */
	case 0x106c:   /* SGX_GETMISCINFO (_, device cookie, SGX_MISC_INFO *): the kernel copies 0x4c bytes in, answers the request (its first word), copies them back */
		misc_info(RD32(b->in + 8));
		return 1;
	case 0x1084: WR32(b->out + 4, handle()); return 1;   /* REGISTER_RENDER_CONTEXT (_, device cookie, descriptor): out { error, handle } */
	case 0x1085: return 1;                                /* UNREGISTER_RENDER_CONTEXT */
	case 0x1086: create_render_target(b); return 1;
	case 0x1087: destroy_render_target(b); return 1;
	case 0x1089: render_target_memblock(b); return 1;
	case 0x108b: render_target_info(b); return 1;
	case 0x108c: kick(b); return 1;
	/* the display queue: libgxm's own thread issues the flip (0x104f) per entry, polls its status
	 * (0x1057) until the GPU work the syncs guard is done, calls the game's display callback, then
	 * releases it (0x1050). The host's GPU work is done when the scene ends, so every flip is complete
	 * at once: status 0 (out { status, event flag, mask } all zero) */
	case 0x104f: return 1;   /* FLIP (_, display, sync handles[4], flags, count) */
	case 0x1057: return 1;   /* FLIP_STATUS (_, display): 0 = complete */
	case 0x1050: return 1;   /* FLIP_RELEASE (_, display) */
	case 0x108d: WR32(b->out + 4, handle()); return 1;   /* REGISTER_TRANSFER_CONTEXT: out { error, handle } */
	case 0x108e: return 1;                                /* UNREGISTER_TRANSFER_CONTEXT */
	case 0x108f: transfer_submit(b); return 1;
	case 0x1090: return 1;                                /* TRANSFER_FINISH: the host's transfer work (gxm_sony.c) is already done */
	case 0x1093: return 1;                                /* WAIT_IDLE (sceGxmDestroyContext): the host's GPU work is already done */
	}
	return 0;
}

static void h_bridge(Cpu *c) {   /* PVRSRV_BridgeDispatchKM(id, package) */
	uint32_t p = A1;
	Bridge b = { RD32(p), RD32(p + 8), RD32(p + 12), RD32(p + 16), RD32(p + 20) };
	int known = b.id >= 0x1000 && b.id <= 0x1093;
	if (known) {
		if (b.out && b.out_size) memset(G(b.out), 0, b.out_size);   /* error 0 (PVRSRV_OK) unless the handler says otherwise */
		known = bridge(&b);
	}
	if (log_gpu() || !known) {
		fprintf(stderr, "gpu bridge %#x in %#x[%#x] out %#x[%#x]%s:", b.id, b.in, b.in_size, b.out, b.out_size, known ? "" : " (unimplemented)");
		for (uint32_t i = 0; i < b.in_size && i < 0x30; i += 4) fprintf(stderr, " %08x", RD32(b.in + i));
		fprintf(stderr, "\n");
	}
	RET(known ? 0 : -25);
}
static void h_open(Cpu *c) { RET(1); }      /* PVRSRVOpen: the services connection (a descriptor >= 0) */
static void h_release(Cpu *c) { RET(0); }   /* PVRSRVRelease */
/* sceGpuSignalWait(uint32 *last seen, timeout): the kernel sleeps while *last seen == its completion counter,
 * then stores the counter. The host's work completes at the kick, except a scene's notification held back for its
 * visibility counts (kick): those the wait completes first, waiting for the GPU as the console's would */
static void h_signal_wait(Cpu *c) {
	extern void gfx_vis_settle(void); gfx_vis_settle();
	if (A0) WR32(A0, __atomic_load_n(&signal_counter, __ATOMIC_SEQ_CST));
	RET(0);
}

const HleEntry hle_gpu[] = {
	{"PVRSRV_BridgeDispatchKM", h_bridge}, {"PVRSRVOpen", h_open}, {"PVRSRVRelease", h_release},
	{"sceGpuSignalWait", h_signal_wait},
	{0, 0},
};
