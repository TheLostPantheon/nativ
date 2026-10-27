// post.c's PostU (k.x: CAS sharpness) and helpers; fragment sampler set 2, uniform buffer set 3
layout(set = 2, binding = 0) uniform sampler2D t;
layout(set = 3, binding = 0, std140) uniform PostU { vec2 rcp; vec2 origin; vec4 k; } u;
layout(location = 0) out vec4 o;
float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 tap(vec2 uv) { return textureLod(t, uv, 0.0).rgb; }
