#version 450
#extension GL_GOOGLE_include_directive : require

#define FFX_GPU 1
#define FFX_GLSL 1
#define FFX_HALF 0
#define FFX_FSR_EASU_FLOAT 1
#include "ffx_core.h"

layout(set = 0, binding = 0) uniform sampler2D fsrInput;
layout(set = 1, binding = 0, rgba16f) writeonly uniform image2D fsrOutput;
layout(set = 2, binding = 0, std140) uniform EasuConstants {
	uvec4 con0;
	uvec4 con1;
	uvec4 con2;
	uvec4 con3;
	uvec2 outputSize;
	uvec2 padding;
} u;

FfxFloat32x4 FsrEasuRF(FfxFloat32x2 p) { return textureGather(fsrInput, p, 0); }
FfxFloat32x4 FsrEasuGF(FfxFloat32x2 p) { return textureGather(fsrInput, p, 1); }
FfxFloat32x4 FsrEasuBF(FfxFloat32x2 p) { return textureGather(fsrInput, p, 2); }

#include "fsr1/ffx_fsr1.h"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main()
{
	uvec2 p = gl_GlobalInvocationID.xy;
	if (any(greaterThanEqual(p, u.outputSize))) return;
	FfxFloat32x3 color;
	ffxFsrEasuFloat(color, p, u.con0, u.con1, u.con2, u.con3);
	imageStore(fsrOutput, ivec2(p), vec4(color, 1.0));
}
