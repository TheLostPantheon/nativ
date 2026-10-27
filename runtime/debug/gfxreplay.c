/*
 * gfxreplay: plays a RECOMP_CAPTURE file (gfx_capture.c) back through the graphics layer --
 * the same gfx_dispatch.c, gfx_soft.c, gfx_gpu.c, usse*.c the game links -- with no game, so a
 * renderer or shader-translator change can be tried on a real frame in seconds.
 *
 *   gfxreplay <capture.vfc> [out.ppm]
 *
 * The backend and its diagnostics come from the usual environment: RECOMP_GFX=gpu for the GPU
 * path, RECOMP_GFX_CHECK=rgb (+ RECOMP_GFX_CHECK_FPROG) for the per-draw diff against the
 * software path, RECOMP_SHADERS=interp for the shader interpreter on the software path, and so on.
 * Writes the presented frame (default replay.ppm).
 *
 * RECOMP_REPLAY_DRAWS=k stops after the k-th draw instead and writes the render target that draw
 * went to, as either backend has it at that point (depth and stencil history intact) -- so
 * bisecting k between a software and a GPU replay finds the first draw where they part.
 *
 * RECOMP_REPLAY_KEEP_TARGETS=1: once a scene has rendered into a colour or depth/stencil surface, the
 * capture's memory records over it are not applied again -- every later pass reads what this replay
 * rendered, not what the run that made the capture left in guest memory. A capture from the GPU
 * backend holds stale bytes for surfaces that lived on the GPU (its G-buffer read back as noise),
 * and any capture restores the old renderer's output over a fix (see the README's replay limit).
 *
 * RECOMP_REPLAY_DUMP=1 renders nothing and prints every scene and draw as one text line instead --
 * the renderer's whole input -- so two captures of the same frame (two builds, two runs) can be
 * compared with diff, field by field.
 *
 * RECOMP_REPLAY_VERTS=k[,k2,...] prints draw k's index list, every vertex it references decoded by
 * its attribute formats, and the first words of its default uniform buffers, as the draw is
 * replayed -- where a HUD quad lands and the matrix that put it there, without a renderer trace.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include <SDL3/SDL.h>
#include "rt.h"
#include "hle.h"
#include "gfx.h"
#include "usse.h"
#include "platform.h"

uint8_t *recomp_mem;

/* what the graphics code expects from the rest of the host */
int guest_mem_ranges(uint32_t lo[], uint32_t hi[], int max) { (void)lo; (void)hi; (void)max; return 0; }   /* no capturing a replay */
/* metrics.c links in for gfx_dispatch.c's hooks (render time, scene/draw counts) and references
 * guest_time_us_peek for its game-speed stat; the per-frame/per-second machinery that would call it (display.c,
 * never linked here) never runs in a replay, so this is only here to satisfy the linker -- a
 * replay has no guest clock of its own, so the host's real one stands in */
uint64_t display_frames(void) { return 0; }   /* (gfx_gpu.c's RECOMP_GXM_PATCHER_LOG: no presented frames to count here) */
uint32_t guest_alloc(uint32_t size, uint32_t align) { (void)size; (void)align; return 0; }   /* (gfx_gpu.c's shader warm-up, never started here: no app0) */
uint64_t guest_time_us_peek(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
void window_present(uint32_t base, uint32_t w, uint32_t h, uint32_t pitch) { (void)base; (void)w; (void)h; (void)pitch; }
int window_active(void) { return 0; }   /* no window: gfx_gpu.c presents through guest memory, as the shots need */
void window_present_gpu(void *tex, uint32_t w, uint32_t h) { (void)tex; (void)w; (void)h; }
void *window_gpu_device(void) {
	static SDL_GPUDevice *dev; static int tried;
	if (!tried) {
		tried = 1;
		const char *g = getenv("RECOMP_GFX");
		if (g && !strcmp(g, "gpu") && SDL_Init(SDL_INIT_VIDEO))
			{ extern void *gfx_gpu_create_device(void); dev = gfx_gpu_create_device(); }
	}
	return dev;
}

static void write_frame(const char *path, uint32_t base, uint32_t w, uint32_t h, uint32_t pitch) {
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); return; }
	fprintf(f, "P6\n%u %u\n255\n", w, h);
	for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
		uint32_t p = RD32(base + 4 * (y * pitch + x)); uint8_t rgb[3] = {p, p >> 8, p >> 16};
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: gfxreplay <capture.vfc> [out.ppm]\n"); return 1; }
	setenv("RECOMP_SHADER_CACHE", "0", 0);   /* gfx_gpu.c's MSL cache is opt-in here (RECOMP_SHADER_CACHE=dir): a replay run from any directory leaves no shader_cache/ behind, and RECOMP_GPU_STATS times real translations */
	const char *outp = argc > 2 ? argv[2] : "replay.ppm";
	recomp_mem = plat_reserve(1ull << 32);
	if (!recomp_mem) { fprintf(stderr, "can't reserve the 4 GiB guest address space\n"); return 1; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }
	char magic[4]; uint32_t version;
	if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "VFCP", 4) || fread(&version, 4, 1, f) != 1 || version != 1) { fprintf(stderr, "%s: not a capture\n", argv[1]); return 1; }
	int scenes = 0, draws = 0; uint64_t membytes = 0;
	if (getenv("RECOMP_REPLAY_WIDEN")) {   /* the HUD layout runs on presented surfaces: a capture's presents noted first (one frame presents after its scenes) */
		extern void gfx_gpu_hud_note_present(uint32_t base);
		long start = ftell(f); char t[4]; uint32_t l, p[5];
		while (fread(t, 1, 4, f) == 4 && fread(&l, 4, 1, f) == 1) {
			if (!memcmp(t, "PRES", 4) && l >= sizeof p && fread(p, 4, 5, f) == 5) { gfx_gpu_hud_note_present(p[0]); if (fseek(f, (long)l - (long)sizeof p, SEEK_CUR)) break; }
			else if (fseek(f, (long)l, SEEK_CUR)) break;
		}
		fseek(f, start, SEEK_SET);
	}
	long stop = getenv("RECOMP_REPLAY_DRAWS") ? atol(getenv("RECOMP_REPLAY_DRAWS")) : -1;
	GfxTarget cur = {0};
	int dump = getenv("RECOMP_REPLAY_DUMP") != 0;
	uint8_t *buf = 0; uint32_t cap = 0;
	int keep = getenv("RECOMP_REPLAY_KEEP_TARGETS") != 0;
	static uint32_t kept[512][2]; int nkept = 0;   /* [lo, hi) of every surface a scene has rendered into */
	for (;;) {
		static uint8_t *scratch; static uint32_t scratch_cap;
		char tag[4]; uint32_t len;
		if (fread(tag, 1, 4, f) != 4 || fread(&len, 4, 1, f) != 1) { fprintf(stderr, "truncated capture\n"); return 1; }
		if (len > cap) { cap = len; buf = realloc(buf, cap); }
		if (len && fread(buf, 1, len, f) != len) { fprintf(stderr, "truncated capture\n"); return 1; }
		if (!memcmp(tag, "MEMP", 4)) {
			uint32_t addr, raw; memcpy(&addr, buf, 4); memcpy(&raw, buf + 4, 4);
			uLongf dl = raw;
			/* a record whose bytes guest memory already holds is not written at all: writing them
			 * would be a no-op for the software path but makes the GPU backend download the target
			 * underneath and re-upload it afterwards -- a replay then measures those syncs, which
			 * the live run never does (9 of this level frame's 16 downloads) */
			if (scratch_cap < raw) { scratch_cap = raw; scratch = realloc(scratch, raw); }
			if (uncompress(scratch, &dl, buf + 8, len - 8) != Z_OK || dl != raw) { fprintf(stderr, "bad memory record at %#x\n", addr); return 1; }
			/* RECOMP_REPLAY_KEEP_TARGETS: the record's bytes outside every rendered surface, in pieces */
			uint32_t seg[64][2]; int nseg = 1; seg[0][0] = addr; seg[0][1] = addr + raw;
			for (int k = 0; keep && k < nkept && nseg; k++) {
				int n2 = 0; uint32_t out[64][2];
				for (int j = 0; j < nseg; j++) {
					uint32_t a = seg[j][0], b = seg[j][1], lo = kept[k][0], hi = kept[k][1];
					if (b <= lo || a >= hi) { if (n2 < 64) { out[n2][0] = a; out[n2][1] = b; n2++; } continue; }
					if (a < lo && n2 < 64) { out[n2][0] = a; out[n2][1] = lo; n2++; }
					if (b > hi && n2 < 64) { out[n2][0] = hi; out[n2][1] = b; n2++; }
				}
				memcpy(seg, out, sizeof out); nseg = n2;
			}
			for (int j = 0; j < nseg; j++) {
				uint32_t a = seg[j][0], n = seg[j][1] - seg[j][0]; const uint8_t *src = scratch + (a - addr);
				if (!n || !memcmp(G(a), src, n)) continue;
				if (!dump) gfx_guest_writing(a, n);   /* a GPU backend's copy of a target here comes down first, so the record replaces only its own bytes */
				memcpy(G(a), src, n);
				if (!dump) gfx_guest_written(a, n);   /* ... and is re-uploaded before its next GPU use */
			}
			membytes += raw;
		} else if (!memcmp(tag, "SCEN", 4)) {
			memset(&cur, 0, sizeof cur); memcpy(&cur, buf, len < sizeof cur ? len : sizeof cur);   /* older captures: a shorter GfxTarget */
			const char *dt = getenv("RECOMP_REPLAY_DS_TILED");   /* hex addresses: depth/stencil surfaces a capture made before GfxTarget.ds_type should treat as tiled */
			for (const char *p = dt; p && *p; p += strcspn(p, ",") + (p[strcspn(p, ",")] != 0))
				if (cur.ds_data && strtoul(p, 0, 16) == cur.ds_data) cur.ds_type = 1;
			scenes++;
			if (dump) { printf("scene %d: colour %#x fmt %#x %ux%u stride %u type %u  depth %d ds fmt %#x %#x/%#x stride %u bg %g/%u load %d store %d ds type %u rt %ux%u ms %ux%u scale %u\n", scenes,
			                   cur.data, cur.format, cur.width, cur.height, cur.stride, cur.type, cur.depth, cur.ds_format, cur.ds_data, cur.ds_stencil_data, cur.ds_stride,
			                   cur.ds_background_depth, cur.ds_background_stencil, cur.ds_force_load, cur.ds_force_store, cur.ds_type, cur.rt_width, cur.rt_height, cur.ms_x, cur.ms_y, cur.scale_mode); continue; }
			if (keep && nkept < 512) {   /* the surfaces this scene renders into */
				uint32_t h = cur.height ? cur.height : (uint32_t)cur.rt_height * (cur.ms_y ? cur.ms_y : 1);
				if (cur.data) { uint32_t bpp = (uint32_t)gfx_target_bpp(&cur); kept[nkept][0] = cur.data; kept[nkept][1] = cur.data + bpp * cur.stride * cur.height; nkept++; }
				if (cur.depth && cur.ds_data && nkept < 512) {
					uint32_t bpp = cur.ds_format == 0x02444000u ? 2 : 4, st = cur.ds_stride ? cur.ds_stride : cur.stride;
					if (!h) h = st;   /* (an old depth-only capture: square, as scene_dims assumes) */
					kept[nkept][0] = cur.ds_data; kept[nkept][1] = cur.ds_data + bpp * st * h; nkept++;
				}
			}
			gfx_begin_scene(&cur);
		}
		else if (!memcmp(tag, "DRAW", 4)) {
			GfxDraw d; memset(&d, 0, sizeof d); memcpy(&d, buf, len < sizeof d ? len : sizeof d);   /* older captures: a shorter GfxDraw */
			if (dump) {
				printf("draw %d: v %#x f %#x prim %#x ix %#x/%#x n %u  attr", ++draws, d.vprog, d.fprog, d.prim, d.index_format, d.indices, d.count);
				for (int k = 0; k < d.nattr; k++) printf(" %u:%u+%u f%#x c%u r%u", k, d.attr[k].stream, d.attr[k].offset, d.attr[k].format, d.attr[k].comps, d.attr[k].reg);
				printf("  streams");
				for (int k = 0; k < d.nstream; k++) printf(" %#x/%u", d.stream[k], d.stride[k]);
				printf("  uni %#x %#x  vub", d.vuniforms, d.funiforms);
				for (int k = 0; k < 14; k++) if (d.vubuf[k]) printf(" %d=%#x", k, d.vubuf[k]);
				printf(" fub");
				for (int k = 0; k < 14; k++) if (d.fubuf[k]) printf(" %d=%#x", k, d.fubuf[k]);
				printf("  tex");
				for (int k = 0; k < 16; k++) if (d.tex[k].data) printf(" %d=%#x f%#x %ux%u m%u/%u u%#x v%#x", k, d.tex[k].data, d.tex[k].format, d.tex[k].width, d.tex[k].height,
				                                                   d.tex[k].min_filter, d.tex[k].mag_filter, d.tex[k].u_mode, d.tex[k].v_mode);
				printf("  blend %d %#x %x%x %x%x %x%x", d.blend.present, d.blend.mask, d.blend.color_func, d.blend.alpha_func, d.blend.color_src, d.blend.color_dst, d.blend.alpha_src, d.blend.alpha_dst);
				if (d.vis_addr) printf("  vis %#x%s", d.vis_addr, d.vis_set ? " set" : "");
				printf("  cull %d depth %u/%d st %u %u%u%u %#x/%#x ref %u  vp %d/%d %g %g %g %g %g %g  clip %#x %u-%u %u-%u  fpoff %d  pm %u pw %u\n",
				       d.cull, d.depth_func, d.depth_write, d.stencil_func, d.stencil_fail, d.depth_fail, d.depth_pass, d.stencil_cmask, d.stencil_wmask, d.stencil_ref,
				       d.viewport_set, d.viewport_disabled, d.vp_xoff, d.vp_xscale, d.vp_yoff, d.vp_yscale, d.vp_zoff, d.vp_zscale,
				       d.clip_mode, d.clip_xmin, d.clip_xmax, d.clip_ymin, d.clip_ymax, d.fp_disabled, d.polygon_mode, d.point_line_width);
				continue;
			}
			if (getenv("RECOMP_REPLAY_INFO") && atol(getenv("RECOMP_REPLAY_INFO")) == draws + 1) {   /* what draw k's programs declare */
				const UsseProgram *fp = usse_get(d.fprog);
				const UsseFragLayout *fl = fp ? usse_fragment_layout(fp) : 0;
				fprintf(stderr, "draw %d: target %#x fmt %#x  vprog %#x fprog %#x", draws + 1, cur.data, cur.format, d.vprog, d.fprog);
				if (fl) fprintf(stderr, "  colour: type %d comps %d %s at %d", fl->color_type, fl->color_comps, fl->native_color ? "O" : "PA", fl->color_pa_start);
				fprintf(stderr, "  blend %d\n", d.blend.present);
			}
			{	/* RECOMP_REPLAY_SAVE_PROG=hexaddr:path -- a program's GXP bytes out of the capture's memory, for usse_dis / usse_run (see README) */
				static const char *sp = 0; static int sp_init = 0, sp_done = 0;
				if (!sp_init) { sp_init = 1; sp = getenv("RECOMP_REPLAY_SAVE_PROG"); }
				if (sp && !sp_done) { uint32_t want = (uint32_t)strtoul(sp, 0, 16); const char *path = strchr(sp, ':');
					if (path && (want == d.vprog || want == d.fprog)) { uint32_t sz; memcpy(&sz, G(want + 8), 4); FILE *pf = fopen(path + 1, "wb");
						if (pf) { fwrite(G(want), 1, sz, pf); fclose(pf); fprintf(stderr, "saved program %#x (%u bytes) -> %s\n", want, sz, path + 1); } sp_done = 1; } }
			}
			{	/* RECOMP_REPLAY_VERTS=k[,k2...]: draw k's vertices as the vertex program receives them (its index list, then each
				 * referenced vertex's attributes decoded by SceGxmAttributeFormat) and the first 32 words of its vertex and fragment
				 * default uniform buffers -- where a HUD quad lands, and the matrix that put it there, without a renderer trace */
				static const char *vk = 0; static int vki = 0; if (!vki) { vki = 1; vk = getenv("RECOMP_REPLAY_VERTS"); }
				int want = 0; for (const char *q = vk; q && *q; q += strcspn(q, ",") + (q[strcspn(q, ",")] != 0)) if (atol(q) == draws + 1) want = 1;
				if (want) {
					fprintf(stderr, "verts draw %d: vprog %#x fprog %#x prim %u count %u index%s %#x\n   indices:", draws + 1, d.vprog, d.fprog, d.prim, d.count, d.index_format ? "32" : "16", d.indices);
					uint32_t seen[64]; int nseen = 0;
					for (uint32_t i = 0; i < d.count && i < 4096; i++) {
						uint32_t ix = d.index_format ? RD32(d.indices + 4 * i) : RD16(d.indices + 2 * i);
						if (i < 64) fprintf(stderr, " %u", ix);
						int dup = 0; for (int j = 0; j < nseen; j++) if (seen[j] == ix) dup = 1;
						if (!dup && nseen < 64) seen[nseen++] = ix;
					}
					fprintf(stderr, "%s\n", d.count > 64 ? " ..." : "");
					for (int j = 0; j < nseen; j++) {
						fprintf(stderr, "   v%-4u", seen[j]);
						for (int k = 0; k < d.nattr; k++) {
							const GfxAttr *a = &d.attr[k]; uint32_t base = d.stream[a->stream] + (uint32_t)d.stride[a->stream] * seen[j] + a->offset;
							fprintf(stderr, "  a%d(f%u)=", k, a->format);
							for (int c = 0; c < a->comps; c++) {
								float v; uint32_t raw = 0;
								switch (a->format) {
								case 9: case 10: memcpy(&v, G(base + 4 * c), 4); memcpy(&raw, G(base + 4 * c), 4); break;
								case 4: v = RD8(base + c) / 255.f; raw = RD8(base + c); break;
								case 0: v = RD8(base + c); raw = RD8(base + c); break;
								case 1: v = (int8_t)RD8(base + c); raw = RD8(base + c); break;
								case 5: v = (int8_t)RD8(base + c) / 127.f; raw = RD8(base + c); break;
								case 2: v = RD16(base + 2 * c); raw = RD16(base + 2 * c); break;
								case 6: v = RD16(base + 2 * c) / 65535.f; raw = RD16(base + 2 * c); break;
								case 3: v = (int16_t)RD16(base + 2 * c); raw = RD16(base + 2 * c); break;
								case 7: v = (int16_t)RD16(base + 2 * c) / 32767.f; raw = RD16(base + 2 * c); break;
								case 8: { uint16_t h = RD16(base + 2 * c); raw = h; uint32_t sgn = (h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, bits;   /* F16 */
									if (!e) bits = sgn; else if (e == 31) bits = sgn | 0x7f800000 | m << 13; else bits = sgn | (e + 112) << 23 | m << 13;
									memcpy(&v, &bits, 4); break; }
								default: v = -999; break;
								}
								fprintf(stderr, "%s%g", c ? "," : "", v); (void)raw;
							}
						}
						fprintf(stderr, "\n");
					}
					if (d.vuniforms) { fprintf(stderr, "   vuniforms %#x:", d.vuniforms); for (int i = 0; i < 32; i++) { float v; memcpy(&v, G(d.vuniforms + 4 * i), 4); fprintf(stderr, "%s%08x(%g)", i % 8 ? " " : "\n     ", RD32(d.vuniforms + 4 * i), v); } fprintf(stderr, "\n"); }
					if (d.funiforms) { fprintf(stderr, "   funiforms %#x:", d.funiforms); for (int i = 0; i < 16; i++) { float v; memcpy(&v, G(d.funiforms + 4 * i), 4); fprintf(stderr, "%s%08x(%g)", i % 8 ? " " : "\n     ", RD32(d.funiforms + 4 * i), v); } fprintf(stderr, "\n"); }
				}
			}
			{	/* RECOMP_REPLAY_TEX=hexaddr:path: at the stop draw, that texture's level 0 as the software sampler decodes it (RGBA floats; see gfx_gpu.c's RECOMP_GPU_DUMP_TEX) */
				static const char *dt = 0; static int dti = 0; if (!dti) { dti = 1; dt = getenv("RECOMP_REPLAY_TEX"); }
				const char *path = dt ? strchr(dt, ':') : 0;
				if (path && draws + 1 == stop) for (int k = 0; k < 16; k++) if (d.tex[k].data && d.tex[k].data == strtoul(dt, 0, 16)) {
					GfxTexture lv; gfx_texture_level(&d.tex[k], 0, &lv);
					float *fb = malloc(16u * lv.width * lv.height); gfx_decode_texture_f32(&lv, fb);
					FILE *o = fopen(path + 1, "wb"); if (o) { fwrite(fb, 16, (size_t)lv.width * lv.height, o); fclose(o); }
					free(fb); break;
				}
			}
			gfx_draw(&d);
			if (++draws == stop) {   /* the target as it stands after this draw */
				gfx_end_scene();
				const char *g = getenv("RECOMP_GFX"); if (g && !strcmp(g, "gpu")) gfx_gpu_sync_all();
				if (getenv("RECOMP_REPLAY_MEM")) {   /* RECOMP_REPLAY_MEM=hexaddr:len:path -- any guest memory as it stands after the draw (a program's LDST stores) */
					const char *m = getenv("RECOMP_REPLAY_MEM"), *c1 = strchr(m, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : 0;
					uint32_t ma = (uint32_t)strtoul(m, 0, 16), ml = c1 ? (uint32_t)strtoul(c1 + 1, 0, 0) : 0;
					FILE *mf = c2 ? fopen(c2 + 1, "wb") : 0; if (mf) { fwrite(G(ma), 1, ml, mf); fclose(mf); }
				}
				if (getenv("RECOMP_REPLAY_RAW")) {   /* the target's bytes as the game sees them, for comparing HDR targets the 8-bit image clamps */
					FILE *rf = fopen(getenv("RECOMP_REPLAY_RAW"), "wb");
					if (rf) { fwrite(G(cur.data), 1, (size_t)gfx_target_bpp(&cur) * cur.stride * cur.height, rf); fclose(rf); }
				}
				uint8_t *rgba = malloc(4u * cur.stride * cur.height); gfx_decode_target_rgba8(&cur, rgba);
				FILE *o = fopen(outp, "wb");
				if (o) { fprintf(o, "P6\n%u %u\n255\n", cur.stride, cur.height); for (uint32_t i = 0; i < cur.stride * cur.height; i++) fwrite(rgba + 4 * i, 1, 3, o); fclose(o); }
				fprintf(stderr, "draw %d (scene %d, target %#x %ux%u fmt %#x: vprog %#x fprog %#x) -> %s\n", draws, scenes, cur.data, cur.stride, cur.height, cur.format, d.vprog, d.fprog, outp);
				return 0;
			}
		}
		else if (!memcmp(tag, "ENDS", 4)) { if (!dump) gfx_end_scene(); }
		else if (!memcmp(tag, "PRES", 4)) {
			uint32_t p[5]; memcpy(p, buf, sizeof p);
			if (dump) { printf("present %#x %ux%u pitch %u fmt %#x\n", p[0], p[1], p[2], p[3], p[4]); continue; }
			gfx_present(p[0], p[1], p[2], p[3], p[4]);
			write_frame(outp, p[0], p[1], p[2], p[3]);
		} else if (!memcmp(tag, "DONE", 4)) break;
		else { fprintf(stderr, "unknown record %.4s\n", tag); return 1; }
	}
	fprintf(stderr, "replayed %d scenes, %d draws (%.1f MB of memory records) -> %s\n", scenes, draws, membytes / 1048576.0, outp);
	{ const char *g = getenv("RECOMP_GFX"); if (g && !strcmp(g, "gpu") && getenv("RECOMP_GPU_ASYNC")) {   /* RECOMP_GPU_ASYNC=1: how long the build workers take after the replay */
		struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a); gfx_gpu_builds_wait(); clock_gettime(CLOCK_MONOTONIC, &b);
		fprintf(stderr, "asynchronous builds done %.1f ms after the replay\n", (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6); } }
	{ extern void gfx_gpu_fence_stats_finish(void); gfx_gpu_fence_stats_finish(); }   /* RECOMP_GPU_FENCE_STATS=1: what the GPU took for the frame (every mark waited for, then the report) */
	/* _exit, as the game's window close does (window.c): exit()'s library destructors ran while the NVIDIA
	 * driver's own shader-compile threads were still working -- a SIGSEGV in libnvidia-glvkspirv (Mesa's
	 * destructors running on the main thread, both Vulkan drivers loaded), or a hang, after the frame was out */
	fflush(stdout); fflush(stderr); _exit(0);
}
/* gfx_gpu.c asks the window how much wider than native to render (window.c); a replay renders native */
/* replays compare against the software path: no anisotropic filtering unless RECOMP_ANISO asks (gfx_gpu.c aniso_want) */
__attribute__((constructor)) static void replay_aniso_default(void) { setenv("RECOMP_ANISO", "1", 0); setenv("RECOMP_SHADOW_FILTER", "0", 0); }   /* (replays compare against the software path: no anisotropy, the game's own one-tap shadows, unless asked) */
float window_aspect_widen(void) { const char *e = getenv("RECOMP_REPLAY_WIDEN"); return e ? (float)atof(e) : 1.0f; }   /* RECOMP_REPLAY_WIDEN=k: test the wider-x paths */
