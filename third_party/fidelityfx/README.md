# AMD FidelityFX SDK: FSR 3.1's upscaler shaders

From [FidelityFX-SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) tag v1.1.4, the last with the
upscaler's GLSL (`sdk/include/FidelityFX/gpu/`), unchanged. MIT (`LICENSE.txt`).

`gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h` is the SDK's Vulkan binding layer, kept for reference: Nativ's own
(`runtime/gfx/fsr/`) binds the same resources the way SDL_GPU's compute pipelines expect. The SDK's host library isn't
here: what it does per frame is ported to C in `runtime/gfx/fsr/fsr.c`.
