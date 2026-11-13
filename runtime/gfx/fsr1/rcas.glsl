#version 450
#extension GL_GOOGLE_include_directive : require

#define FFX_GPU 1
#define FFX_GLSL 1
#define FFX_HALF 0
#define FSR_RCAS_F 1
#define FSR_RCAS_DENOISE 1
#include "ffx_core.h"

layout(set = 0, binding = 0) uniform sampler2D fsrInput;
layout(set = 1, binding = 0, rgba8) writeonly uniform image2D fsrOutput;
layout(set = 2, binding = 0, std140) uniform RcasConstants {
	uvec4 con;
	uvec2 outputSize;
	uint encodeSrgb;
	uint sharpen;
} u;

FfxFloat32x4 FsrRcasLoadF(FfxInt32x2 p)
{
	return texelFetch(fsrInput, clamp(p, ivec2(0), ivec2(u.outputSize) - 1), 0);
}
void FsrRcasInputF(inout FfxFloat32 r, inout FfxFloat32 g, inout FfxFloat32 b) {}

#include "fsr1/ffx_fsr1.h"

vec3 linearToSrgb(vec3 color)
{
	bvec3 low = lessThanEqual(color, vec3(0.0031308));
	return mix(1.055 * pow(max(color, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055, 12.92 * color, low);
}

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main()
{
	uvec2 p = gl_GlobalInvocationID.xy;
	if (any(greaterThanEqual(p, u.outputSize))) return;
	vec3 color;
	if (u.sharpen != 0) FsrRcasF(color.r, color.g, color.b, p, u.con);
	else color = FsrRcasLoadF(ivec2(p)).rgb;
	if (u.encodeSrgb != 0) color = linearToSrgb(color);
	imageStore(fsrOutput, ivec2(p), vec4(clamp(color, 0.0, 1.0), 1.0));
}
