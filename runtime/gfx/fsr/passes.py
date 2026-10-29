#!/usr/bin/env python3
"""passes.py: writes passes/*.glsl, the FSR 3.1 passes Nativ runs (FidelityFX SDK v1.1.4's
sdk/src/backends/vk/shaders/fsr3upscaler/*_pass.glsl, MIT), each binding numbered from 0 within SDL_GPU's compute sets
in its order: set 0 the inputs (sampler2D) then read-only storage buffers, set 1 writable images then writable storage
buffers, set 2 the constant blocks (ffx_fsr3upscaler_callbacks_nativ.h). The host binds the same lists in the same order
(fsr.c's pass table): change one, change both."""
import os
D = os.path.dirname(os.path.abspath(__file__))
# pass: (algorithm header, entry call, thread group, set 0 inputs, set 0 read-only buffers, set 1 images, set 1 buffers, set 2 blocks)
PASSES = {
    "prepare_inputs": ("prepare_inputs", "PrepareInputs(FfxInt32x2(gl_GlobalInvocationID.xy));", (8, 8),
        ["INPUT_MOTION_VECTORS", "INPUT_DEPTH", "INPUT_COLOR"], [],
        ["DILATED_MOTION_VECTORS", "DILATED_DEPTH", "FARTHEST_DEPTH", "CURRENT_LUMA"], ["RECONSTRUCTED_PREV_NEAREST_DEPTH"], ["FSR3UPSCALER"]),
    "luma_pyramid": ("luma_pyramid", "ComputeAutoExposure(gl_WorkGroupID, gl_LocalInvocationIndex);", (256, 1),
        ["CURRENT_LUMA", "FARTHEST_DEPTH"], [],
        ["FRAME_INFO", "SPD_MIPS_LEVEL_0", "SPD_MIPS_LEVEL_1", "SPD_MIPS_LEVEL_2", "SPD_MIPS_LEVEL_3", "SPD_MIPS_LEVEL_4", "SPD_MIPS_LEVEL_5", "FARTHEST_DEPTH_MIP1"],
        ["SPD_GLOBAL_ATOMIC"], ["FSR3UPSCALER", "SPD"]),
    "shading_change_pyramid": ("shading_change_pyramid", "ComputeShadingChangePyramid(gl_WorkGroupID, gl_LocalInvocationIndex);", (256, 1),
        ["CURRENT_LUMA", "PREVIOUS_LUMA", "DILATED_MOTION_VECTORS", "INPUT_EXPOSURE"], [],
        ["SPD_MIPS_LEVEL_0", "SPD_MIPS_LEVEL_1", "SPD_MIPS_LEVEL_2", "SPD_MIPS_LEVEL_3", "SPD_MIPS_LEVEL_4", "SPD_MIPS_LEVEL_5"],
        ["SPD_GLOBAL_ATOMIC"], ["FSR3UPSCALER", "SPD"]),
    "shading_change": ("shading_change", "ShadingChange(FfxInt32x2(gl_GlobalInvocationID.xy));", (8, 8),
        ["SPD_MIPS"], [], ["SHADING_CHANGE"], [], ["FSR3UPSCALER"]),
    "prepare_reactivity": ("prepare_reactivity", "PrepareReactivity(FfxInt32x2(gl_GlobalInvocationID.xy));", (8, 8),
        ["DILATED_MOTION_VECTORS", "DILATED_DEPTH", "REACTIVE_MASK", "TRANSPARENCY_AND_COMPOSITION_MASK", "ACCUMULATION", "SHADING_CHANGE", "CURRENT_LUMA", "INPUT_EXPOSURE"],
        ["RECONSTRUCTED_PREV_NEAREST_DEPTH"], ["DILATED_REACTIVE_MASKS", "NEW_LOCKS", "ACCUMULATION"], [], ["FSR3UPSCALER"]),
    "luma_instability": ("luma_instability", "LumaInstability(FfxInt32x2(gl_GlobalInvocationID.xy));", (8, 8),
        ["INPUT_EXPOSURE", "DILATED_REACTIVE_MASKS", "DILATED_MOTION_VECTORS", "FRAME_INFO", "LUMA_HISTORY", "FARTHEST_DEPTH_MIP1", "CURRENT_LUMA"], [],
        ["LUMA_HISTORY", "LUMA_INSTABILITY"], [], ["FSR3UPSCALER"]),
    "accumulate": ("accumulate", "Accumulate(FfxInt32x2(gl_GlobalInvocationID.xy));", (8, 8),
        ["INPUT_EXPOSURE", "DILATED_REACTIVE_MASKS", "DILATED_MOTION_VECTORS", "INTERNAL_UPSCALED", "LANCZOS_LUT", "FARTHEST_DEPTH_MIP1", "CURRENT_LUMA", "LUMA_INSTABILITY", "INPUT_COLOR"], [],
        ["INTERNAL_UPSCALED", "UPSCALED_OUTPUT", "NEW_LOCKS"], [], ["FSR3UPSCALER"]),
    "accumulate_sharpen": ("accumulate", "Accumulate(FfxInt32x2(gl_GlobalInvocationID.xy));", (8, 8),   # (the output is RCAS's)
        ["INPUT_EXPOSURE", "DILATED_REACTIVE_MASKS", "DILATED_MOTION_VECTORS", "INTERNAL_UPSCALED", "LANCZOS_LUT", "FARTHEST_DEPTH_MIP1", "CURRENT_LUMA", "LUMA_INSTABILITY", "INPUT_COLOR"], [],
        ["INTERNAL_UPSCALED", "NEW_LOCKS"], [], ["FSR3UPSCALER"]),
    "autogen_reactive": (None, "AUTOGEN", (8, 8),   # (the SDK's own main, below: the reactive mask from the opaque-only and the final colour)
        ["INPUT_OPAQUE_ONLY", "INPUT_COLOR"], [], ["AUTOREACTIVE"], [], ["FSR3UPSCALER", "REACTIVE"]),
    "rcas": ("rcas", "RCAS(gl_LocalInvocationID.xyz, gl_WorkGroupID.xyz, gl_GlobalInvocationID.xyz);", (64, 1),
        ["INPUT_EXPOSURE", "RCAS_INPUT", "INPUT_COLOR"], [], ["UPSCALED_OUTPUT"], [], ["FSR3UPSCALER", "RCAS"]),   # (INPUT_COLOR: the game's alpha, see the binding layer)
}
EXTRA_INCLUDES = {"accumulate": ["sample", "upsample", "reproject"], "accumulate_sharpen": ["sample", "upsample", "reproject"]}   # (the SDK's accumulate pass includes these before its own)
SHARPEN = {"accumulate_sharpen": 1}
FP32_ONLY = ("luma_pyramid", "shading_change_pyramid")   # (their FFX_HALF paths are stubs in SDK v1.1.4 -- the source loads return 0, the stores
                                                         #  do nothing -- so FP16 left Killzone's exposure unset and its frame dark: FP32 always)
AUTOGEN_MAIN = """    FfxUInt32x2 uDispatchThreadId = gl_GlobalInvocationID.xy;
    FfxFloat32x3 ColorPreAlpha  = LoadOpaqueOnly(FFX_MIN16_I2(uDispatchThreadId)).rgb;
    FfxFloat32x3 ColorPostAlpha = LoadInputColor(FfxInt32x2(uDispatchThreadId)).rgb;
    if ((GenReactiveFlags() & FFX_FSR3UPSCALER_AUTOREACTIVEFLAGS_APPLY_TONEMAP) != 0) { ColorPreAlpha = Tonemap(ColorPreAlpha); ColorPostAlpha = Tonemap(ColorPostAlpha); }
    if ((GenReactiveFlags() & FFX_FSR3UPSCALER_AUTOREACTIVEFLAGS_APPLY_INVERSETONEMAP) != 0) { ColorPreAlpha = InverseTonemap(ColorPreAlpha); ColorPostAlpha = InverseTonemap(ColorPostAlpha); }
    FfxFloat32x3 delta = abs(ColorPostAlpha - ColorPreAlpha);
    FfxFloat32 out_reactive_value = ((GenReactiveFlags() & FFX_FSR3UPSCALER_AUTOREACTIVEFLAGS_USE_COMPONENTS_MAX) != 0) ? max(delta.x, max(delta.y, delta.z)) : length(delta);
    out_reactive_value *= GenReactiveScale();
    out_reactive_value = ((GenReactiveFlags() & FFX_FSR3UPSCALER_AUTOREACTIVEFLAGS_APPLY_THRESHOLD) != 0) ? ((out_reactive_value < GenReactiveThreshold()) ? 0 : GenReactiveBinaryValue()) : out_reactive_value;
    imageStore(rw_output_autoreactive, FfxInt32x2(uDispatchThreadId), FfxFloat32x4(out_reactive_value, 0.0, 0.0, 0.0));"""   # (ffx_fsr3upscaler_autogen_reactive_pass.glsl's main, its group-id arithmetic as gl_GlobalInvocationID)   # FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING: the accumulate pass leaves the output to RCAS
for name, (hdr, call, (gx, gy), srv, robuf, uav, rwbuf, cb) in PASSES.items():
    lines = [f"// Nativ: FSR 3.1's {name} pass (FidelityFX SDK v1.1.4, MIT), bindings numbered for SDL_GPU -- generated by",
             "// runtime/gfx/fsr/passes.py, don't edit", "#version 450",
             "#extension GL_GOOGLE_include_directive : require", "#extension GL_EXT_samplerless_texture_functions : require", ""]
    if name in FP32_ONLY: lines += ["#undef FFX_HALF", "#define FFX_HALF 0"]
    lines.append(f"#define FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING {SHARPEN.get(name, 0)}")
    for i, r in enumerate(srv + robuf): lines.append(f"#define FSR3UPSCALER_BIND_SRV_{r} {i}")
    for i, r in enumerate(uav + rwbuf): lines.append(f"#define FSR3UPSCALER_BIND_UAV_{r} {i}")
    for i, r in enumerate(cb): lines.append(f"#define FSR3UPSCALER_BIND_CB_{r} {i}")
    lines += ["", '#include "ffx_fsr3upscaler_callbacks_nativ.h"', '#include "fsr3upscaler/ffx_fsr3upscaler_common.h"']
    lines += [f'#include "fsr3upscaler/ffx_fsr3upscaler_{x}.h"' for x in EXTRA_INCLUDES.get(name, [])]
    if hdr: lines.append(f'#include "fsr3upscaler/ffx_fsr3upscaler_{hdr}.h"')
    lines += ["", f"layout (local_size_x = {gx}, local_size_y = {gy}, local_size_z = 1) in;", "void main()", "{", AUTOGEN_MAIN if call == "AUTOGEN" else f"    {call}", "}", ""]
    open(os.path.join(D, "passes", f"{name}.glsl"), "w", newline="\n").write("\n".join(lines))
    print(f"{name}: {len(srv)} inputs + {len(robuf)} buffers, {len(uav)} images + {len(rwbuf)} buffers, {len(cb)} blocks, {gx}x{gy}")
