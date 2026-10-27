// view_msl's helpers, the same arithmetic (gfx_gpu.c "target views"): a destination texel -> the source
// pixel holding its bytes, and gfx_soft.c's small-float quantisers. SDL_GPU's SPIR-V layout: fragment
// samplers set 2, fragment uniform buffers set 3.
layout(set = 3, binding = 0, std140) uniform ViewU { uint off_bytes, tex_stride, tgt_stride, tex_bpp, tgt_bpp, mode, tgt_w, tgt_h; uvec4 swz; uint scale, scale_y, ms_x, ms_y; } u;   // ms_x, ms_y: the source's samples a pixel (0 = 1)
layout(location = 0) out vec4 o;
vec4 swizzle(vec4 c, uvec4 z) { vec4 r; for (int k = 0; k < 4; k++) r[k] = z[k] < 4u ? c[int(z[k])] : z[k] == 4u ? 0.0 : 1.0; return r; }
uint tsf(float f, uint mbits) {
	if (!(f > 0.0)) return 0u;
	if (f >= 65504.0) return (30u << mbits) | ((1u << mbits) - 1u);
	int e; float m = frexp(f, e); int be = e - 1 + 15;
	if (be <= 0) return uint(f * ldexp(1.0, 14 + int(mbits)) + 0.5) & ((1u << mbits) - 1u);
	uint mant = uint((m * 2.0 - 1.0) * float(1u << mbits) + 0.5);
	if ((mant >> mbits) != 0u) { mant = 0u; be++; }
	return (uint(be) << mbits) | mant;
}
float sfl(uint v, uint mbits) {
	uint e = v >> mbits, m = v & ((1u << mbits) - 1u);
	if (e == 0u) return ldexp(float(m), -14 - int(mbits));
	if (e == 31u) return m != 0u ? 0.0 : 65504.0;
	return ldexp(1.0 + float(m) / float(1u << mbits), int(e) - 15);
}
float q(float f, uint mbits) { return sfl(tsf(f, mbits), mbits); }
uvec2 srcpix(uvec2 P, out uint sub) {
	uvec2 sc = uvec2(u.scale, u.scale_y), p = P / sc, f = P % sc;
	uint b = u.off_bytes + (p.y * u.tex_stride + p.x) * u.tex_bpp; uint pix = b / u.tgt_bpp; sub = (b % u.tgt_bpp) / 4u;
	return min(uvec2(pix % u.tgt_stride, pix / u.tgt_stride) / max(uvec2(u.ms_x, u.ms_y), uvec2(1u)), uvec2(u.tgt_w - 1u, u.tgt_h - 1u)) * sc + f;
}
float s8(uint bb) { int v = int(bb); if (v >= 128) v -= 256; return v <= -127 ? -1.0 : float(v) / 127.0; }
