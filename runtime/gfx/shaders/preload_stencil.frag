#version 450
layout(set = 2, binding = 0) uniform sampler2D st;
layout(set = 3, binding = 0, std140) uniform Bit { uint bit; } b;
// preload_msl's main0_stencil: survive (and get the pass's stencil REPLACE) where the R8 texture has the bit -- the bytes as
// R8_UNORM (SDL_GPU samples no integer format), times 255 back to the exact byte
void main() { if ((uint(texelFetch(st, ivec2(gl_FragCoord.xy), 0).r * 255.0 + 0.5) & b.bit) == 0u) discard; }
