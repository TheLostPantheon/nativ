#version 450
#extension GL_GOOGLE_include_directive : require
#include "post_common.glsl"
// post.c's cas_fs (AMD FidelityFX CAS, the published algorithm): the same steps
void main() {
	vec2 r = u.rcp, uv = (gl_FragCoord.xy - u.origin) * r;
	vec3 a = tap(uv + vec2(-r.x, -r.y)), b = tap(uv + vec2(0.0, -r.y)), c = tap(uv + vec2(r.x, -r.y));
	vec3 d = tap(uv + vec2(-r.x, 0.0)), e = tap(uv), f = tap(uv + vec2(r.x, 0.0));
	vec3 g = tap(uv + vec2(-r.x, r.y)), h = tap(uv + vec2(0.0, r.y)), i = tap(uv + vec2(r.x, r.y));
	vec3 mn = min(min(min(d, e), min(f, b)), h), mx = max(max(max(d, e), max(f, b)), h);
	mn += min(mn, min(min(a, c), min(g, i))); mx += max(mx, max(max(a, c), max(g, i)));
	vec3 amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, vec3(1e-5)), 0.0, 1.0));
	vec3 w = amp * (-1.0 / mix(8.0, 5.0, clamp(u.k.x, 0.0, 1.0)));
	o = vec4(clamp((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w), 0.0, 1.0), 1.0);
}
