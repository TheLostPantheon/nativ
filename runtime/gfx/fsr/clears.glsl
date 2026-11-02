// Nativ: FSR's per-frame clears in one pass (fsr.c): SPD's six mips to 0 (it reads pixels it never wrote otherwise),
// the reconstructed previous depth to "far", SPD's counter to 0 -- were six render passes and two fills
#version 450
layout (local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout (set = 1, binding = 0, rgba16f) writeonly uniform image2D spd0;
layout (set = 1, binding = 1, rgba16f) writeonly uniform image2D spd1;
layout (set = 1, binding = 2, rgba16f) writeonly uniform image2D spd2;
layout (set = 1, binding = 3, rgba16f) writeonly uniform image2D spd3;
layout (set = 1, binding = 4, rgba16f) writeonly uniform image2D spd4;
layout (set = 1, binding = 5, rgba16f) writeonly uniform image2D spd5;
layout (set = 1, binding = 6, std430) buffer Depth { uint v[]; } rpnd;
layout (set = 1, binding = 7, std430) buffer Counter { uint n; } counter;
layout (set = 2, binding = 0, std140) uniform Clear { uint far_bits; uint count; uint w2; uint h2; } cl;
void main()
{
    uint i = gl_GlobalInvocationID.x;
    if (i < cl.count) rpnd.v[i] = cl.far_bits;
    if (i == 0u) counter.n = 0u;
    if (i >= cl.w2 * cl.h2) return;
    ivec2 p = ivec2(i % cl.w2, i / cl.w2); vec4 z = vec4(0.0);
    imageStore(spd0, p, z);
    if (p.x < int(max(1u, cl.w2 >> 1)) && p.y < int(max(1u, cl.h2 >> 1))) imageStore(spd1, p, z);
    if (p.x < int(max(1u, cl.w2 >> 2)) && p.y < int(max(1u, cl.h2 >> 2))) imageStore(spd2, p, z);
    if (p.x < int(max(1u, cl.w2 >> 3)) && p.y < int(max(1u, cl.h2 >> 3))) imageStore(spd3, p, z);
    if (p.x < int(max(1u, cl.w2 >> 4)) && p.y < int(max(1u, cl.h2 >> 4))) imageStore(spd4, p, z);
    if (p.x < int(max(1u, cl.w2 >> 5)) && p.y < int(max(1u, cl.h2 >> 5))) imageStore(spd5, p, z);
}
