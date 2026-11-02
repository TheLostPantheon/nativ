# AMD FidelityFX SDK: FSR 3.1's upscaler shaders

From [FidelityFX-SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) tag v1.1.4, the last with the
upscaler's GLSL (`sdk/include/FidelityFX/gpu/`), unchanged. MIT (`LICENSE.txt`).

`gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h` is the SDK's Vulkan binding layer, kept for reference: Nativ's own
(`runtime/gfx/fsr/`) binds the same resources the way SDL_GPU's compute pipelines expect. The SDK's host library isn't
here: what it does per frame is ported to C in `runtime/gfx/fsr/fsr.c`.

Nativ declares the luma-history storage image as `rgba16f`, matching the SDK host's
`R16G16B16A16_FLOAT` allocation. The v1.1.4 GLSL callback says `rgba8`; using that qualifier with the allocated format
is undefined on Vulkan and is tracked upstream in
[FidelityFX-SDK#161](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/issues/161).

Run `runtime/gfx/fsr/build.sh` after changing the callback or pass wrappers and commit the regenerated
`runtime/gfx/fsr_shaders.h`. Only the upscaler is vendored; FidelityFX frame interpolation/frame generation and its
presentation backend are not included.
