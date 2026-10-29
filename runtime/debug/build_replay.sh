#!/bin/zsh
# Build gfxreplay (the graphics layer alone, for RECOMP_CAPTURE files): build_replay.sh [out]
set -e
RT=${0:A:h:h}; ROOT=${RT:h}; G=$RT/gfx; D=$RT/debug; C=$RT/core
OUT=${1:-$ROOT/build/gfxreplay}; mkdir -p ${OUT:h}
SRCS=($D/gfxreplay.c $RT/platform/platform_posix.c $G/gfx_dispatch.c $G/gfx_soft.c $G/gfx_gpu.c $G/gpulock.c $D/gfx_coverage.c $D/gfx_capture.c $G/guestwatch.c $C/metrics.c $C/settings.c $G/usse.c $G/usse_spirv.c $C/psarc.c $G/gxp_index.c $G/ssao.c $G/fsr/fsr.c)
INC=(); for d in core gfx audio media input net ui platform debug; do INC+=(-I $RT/$d); done; INC+=(-I $ROOT/recompiler -I $ROOT)
for s in $SRCS $ROOT/recompiler/rt.h $RT/*/*.h; do [[ ! -f $OUT || $s -nt $OUT ]] && { stale=1; break; }; done
[[ -z $stale ]] && exit 0
SPIRV_CROSS=()   # as build.sh: only Metal needs SPIR-V cross-compiled
if [[ "$(uname)" == Darwin ]] && pkg-config --exists spirv-cross-c 2>/dev/null; then
  SCDIR=$(pkg-config --variable=libdir spirv-cross-c)
  SPIRV_CROSS=($(pkg-config --cflags spirv-cross-c) -DRECOMP_SPIRV_CROSS -L$SCDIR -lspirv-cross-c -lspirv-cross-msl -lspirv-cross-glsl -lspirv-cross-hlsl -lspirv-cross-reflect -lspirv-cross-cpp -lspirv-cross-core -lspirv-cross-util -lc++)
fi
cc -O2 -ffp-contract=off -Wall $INC $(pkg-config --cflags sdl3) $SRCS $(pkg-config --libs sdl3) $SPIRV_CROSS -lm -lpthread -lz -o $OUT
echo "built $OUT"
