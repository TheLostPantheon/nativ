#version 450
#extension GL_GOOGLE_include_directive : require
#include "view_common.glsl"
layout(set = 2, binding = 0) uniform sampler2D src;   // a depth texture: its depth in .r
// view_msl's main0_view_depth: X8U24 (24-bit depth, as the software path stores and reads it); mode 1, D16 read as U16: 16-bit
void main() {
	uint sub; uvec2 sp = srcpix(uvec2(gl_FragCoord.xy), sub); float d = texelFetch(src, ivec2(sp), 0).r;
	float m = u.mode == 1u ? 65535.0 : 16777215.0; float qd = floor(d * m + 0.5) / m; o = vec4(qd, qd, qd, 1.0);
}
