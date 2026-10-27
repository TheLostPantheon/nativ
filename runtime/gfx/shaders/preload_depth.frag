#version 450
layout(set = 2, binding = 0) uniform sampler2D z;
// preload_msl's main0_depth: the decoded depth (an R32F texture) written as the fragment's depth
void main() { gl_FragDepth = texelFetch(z, ivec2(gl_FragCoord.xy), 0).r; }
