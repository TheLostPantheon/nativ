#version 450
#extension GL_GOOGLE_include_directive : require
// ssao_msl's blur_fs: the depth-aware 9-tap blur along (dirx, diry)
#include "ssao_common.glsl"
layout(set = 2, binding = 0) uniform sampler2D t;
void main() {
	ivec2 p = ivec2(gl_FragCoord.xy); vec2 c = texelFetch(t, p, 0).rg; float sum = c.r, ws = 1.0;
	for (int k = -4; k <= 4; k++) { if (k == 0) continue;
		ivec2 q = clamp(p + ivec2(int(float(k) * u.dirx), int(float(k) * u.diry)), ivec2(0), ivec2(int(u.hw) - 1, int(u.hh) - 1));
		vec2 sv = texelFetch(t, q, 0).rg; float w = exp(-0.18 * float(k * k)) * max(0.0, 1.0 - abs(sv.g - c.g) / (0.04 * c.g + 0.001));
		sum += sv.r * w; ws += w; }
	o = vec4(sum / ws, c.g, 0.0, 1.0);
}
