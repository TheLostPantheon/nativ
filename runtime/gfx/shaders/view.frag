#version 450
#extension GL_GOOGLE_include_directive : require
#include "view_common.glsl"
layout(set = 2, binding = 0) uniform sampler2D src;
// the sRGB transfer function both ways (an sRGB texture's decoded values <-> the bytes it stores)
vec3 enc(vec3 x) { return mix(1.055 * pow(x, vec3(1.0 / 2.4)) - 0.055, 12.92 * x, lessThanEqual(x, vec3(0.0031308))); }
vec3 dec(vec3 x) { return mix(pow((x + 0.055) / 1.055, vec3(2.4)), x / 12.92, lessThanEqual(x, vec3(0.04045))); }
// view_msl's main0_view: one texel of a colour target re-read in another format
void main() {
	if (u.mode == 7u || u.mode == 8u) {                                                            // a GXM transfer downscale: the 2x2 box of source guest pixels, on the stored bytes (8: sRGB)
		uvec2 sc = uvec2(u.scale, u.scale_y), P = uvec2(gl_FragCoord.xy), p = P / sc, f = P % sc; uint o0 = u.off_bytes / 4u;
		uvec2 org = uvec2(o0 % u.tgt_stride, o0 / u.tgt_stride) + 2u * p, lim = uvec2(u.tgt_w - 1u, u.tgt_h - 1u); uvec4 s = uvec4(0u);
		for (uint d = 0u; d < 4u; d++) { vec4 c = texelFetch(src, ivec2(min(org + uvec2(d & 1u, d >> 1u), lim) * sc + f), 0); if (u.mode == 8u) c.rgb = enc(c.rgb); s += uvec4(clamp(c, 0.0, 1.0) * 255.0 + 0.5); }
		vec4 r = vec4((s + 2u) / 4u) / 255.0; if (u.mode == 8u) r.rgb = dec(r.rgb); o = r; return;
	}
	uint sub; uvec2 sp = srcpix(uvec2(gl_FragCoord.xy), sub); vec4 c = texelFetch(src, ivec2(sp), 0);
	if (u.mode == 5u || u.mode == 6u) { o = swizzle(vec4(c.r, 0.0, 0.0, 1.0), u.swz); return; }   // U8, F32 (one component)
	if (u.mode == 1u) { o = vec4(q(c.r, 6u), q(c.g, 6u), q(c.b, 5u), 1.0); return; }               // F11F11F10
	if (u.mode == 2u) {                                                                            // U2F10F10F10
		float al = c.a <= 0.0 ? 0.0 : c.a >= 1.0 ? 3.0 : floor(c.a * 3.0 + 0.5);
		o = vec4(q(c.r, 5u), q(c.g, 5u), q(c.b, 5u), al / 3.0); return;
	}
	if (u.mode == 3u || u.mode == 4u) {                                                            // the pixel's words as U8U8U8U8 / S8S8S8S8 bytes
		uint w = floatBitsToUint(sub == 1u ? c.g : sub == 2u ? c.b : sub == 3u ? c.a : c.r); vec4 r;
		for (int k = 0; k < 4; k++) { uint bb = (w >> (8u * uint(k))) & 255u; r[k] = u.mode == 3u ? float(bb) / 255.0 : s8(bb); }
		o = swizzle(r, u.swz); return;
	}
	o = swizzle(c, u.swz);                                                                         // 0: the same values (F16x4, U8x4)
}
