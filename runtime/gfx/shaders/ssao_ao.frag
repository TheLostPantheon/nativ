#version 450
#extension GL_GOOGLE_include_directive : require
// ssao_msl's ao_fs: half-resolution occlusion and distance
#include "ssao_common.glsl"
layout(set = 2, binding = 0) uniform sampler2D dtex;   // the scene's depth texture: its depth in .r
float dep(vec2 px) { return lin(texelFetch(dtex, ivec2(clamp(px, vec2(0.0), vec2(u.fw - 1.0, u.fh - 1.0))), 0).r); }
void main() {
	vec2 fp = floor(gl_FragCoord.xy) * 2.0 + 0.5;
	float d = dep(fp);
	if (!(d > 0.3) || d > 1000.0) { o = vec4(1.0, d, 0.0, 1.0); return; }
	vec3 P = vpos(fp, d);
	vec3 pr = vpos(fp + vec2(2.0, 0.0), dep(fp + vec2(2.0, 0.0))), pl = vpos(fp - vec2(2.0, 0.0), dep(fp - vec2(2.0, 0.0)));
	vec3 pd = vpos(fp + vec2(0.0, 2.0), dep(fp + vec2(0.0, 2.0))), pu = vpos(fp - vec2(0.0, 2.0), dep(fp - vec2(0.0, 2.0)));
	vec3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl, dy = abs(pd.z - P.z) < abs(P.z - pu.z) ? pd - P : P - pu;
	vec3 n = normalize(cross(dx, dy)); if (dot(n, P) > 0.0) n = -n;
	float rpx = min(u.radius * u.p11 * 0.5 * u.fh / d, u.maxpx);
	if (rpx < 2.0) { o = vec4(1.0, d, 0.0, 1.0); return; }
	float spin = fract(52.9829189 * fract(dot(floor(gl_FragCoord.xy), vec2(0.06711056, 0.00583715)))) * 6.2831853;
	float r2 = u.radius * u.radius, sum = 0.0;
	for (int i = 0; i < 12; i++) {
		float a = (float(i) + 0.5) / 12.0, ang = a * 31.4159265 + spin;
		vec2 q = fp + vec2(cos(ang), sin(ang)) * (a * rpx);
		if (q.x < 0.0 || q.y < 0.0 || q.x >= u.fw || q.y >= u.fh) continue;   // off screen: nothing counted
		vec3 v = vpos(q, dep(q)) - P; float vv = dot(v, v), vn = dot(v, n);
		float f = max(r2 - vv, 0.0); sum += f * f * f * max((vn - 0.25 * sqrt(vv) - u.bias * max(1.0, 0.1 * d)) / (vv + 0.01 * r2), 0.0);
	}
	o = vec4(max(0.0, 1.0 - sum * u.intensity / (r2 * r2 * r2) * (5.0 / 12.0)), d, 0.0, 1.0);
}
