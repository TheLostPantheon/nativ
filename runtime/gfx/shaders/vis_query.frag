#version 450
// query_msl's main0: the visibility test's count -- one per sample that passed the draw's depth and stencil tests (they
// run first: early_fragment_tests), added to the query's counter. Needs fragmentStoresAndAtomics (gfx_gpu_create_device).
layout(early_fragment_tests) in;
layout(set = 2, binding = 0, std430) buffer Counts { uint counts[]; };
layout(set = 3, binding = 0, std140) uniform QU { uint slot; } q;
void main() { atomicAdd(counts[q.slot], 1u); }
