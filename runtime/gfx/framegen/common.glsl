// Frame generation's shared declarations (framegen.c): one constant block for every pass, its fields named per pass
#extension GL_EXT_samplerless_texture_functions : enable
layout(set = 2, binding = 0) uniform U { ivec4 a; ivec4 b; vec4 c; vec4 d; } u;
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
