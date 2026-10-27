// ssao.c's shared declarations (ssao_msl's AoU, lin, vpos): a change here is made there too
layout(set = 3, binding = 0, std140) uniform AoU { float p00, p11, p22, p32, zoff, zscale, radius, intensity, fw, fh, hw, hh, bias, maxpx, dirx, diry, strength, pad0, pad1, pad2; } u;
layout(location = 0) out vec4 o;
float lin(float z) { return u.p32 / ((z - u.zoff) / u.zscale + u.p22); }
vec3 vpos(vec2 px, float d) { vec2 ndc = vec2(px.x / u.fw * 2.0 - 1.0, 1.0 - px.y / u.fh * 2.0); return vec3(ndc.x * d / u.p00, ndc.y * d / u.p11, -d); }
