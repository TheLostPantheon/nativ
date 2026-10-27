#version 450
#extension GL_GOOGLE_include_directive : require
// ssao_msl's apply_fs: the occlusion at full resolution (nearest-depth at silhouettes), multiplied in by the blend unit
#include "ssao_common.glsl"
layout(set = 2, binding = 0) uniform sampler2D t;      // the blurred occlusion and distance
layout(set = 2, binding = 1) uniform sampler2D dtex;   // the scene's depth texture
void main() {
	float d0 = lin(texelFetch(dtex, ivec2(clamp(gl_FragCoord.xy, vec2(0.0), vec2(u.fw - 1.0, u.fh - 1.0))), 0).r);
	vec2 b = texture(t, gl_FragCoord.xy / vec2(u.fw, u.fh)).rg; float ao = b.r;
	if (abs(b.g - d0) > 0.05 * d0) {
		ivec2 h = ivec2(floor(gl_FragCoord.xy * 0.5 - 0.5)); float best = 1e30;
		for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) { vec2 sv = texelFetch(t, clamp(h + ivec2(i, j), ivec2(0), ivec2(int(u.hw) - 1, int(u.hh) - 1)), 0).rg;
			float e = abs(sv.g - d0); if (e < best) { best = e; ao = sv.r; } }
	}
	o = vec4(vec3(mix(1.0, ao, clamp(u.strength, 0.0, 1.0)) * (u.strength > 1.0 ? mix(1.0, ao, u.strength - 1.0) : 1.0)), 1.0);
}
