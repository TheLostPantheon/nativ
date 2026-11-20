#!/bin/bash
# Build and run framegen_test (frame generation on a GPU without a window): framegen_test.sh [args...]
set -e
RT=$(cd "$(dirname "$0")/.." && pwd); OUT=${FG_TEST_BIN:-$RT/../build/framegen_test}; mkdir -p "$(dirname "$OUT")"
SC=()
if [ "$(uname)" = Darwin ] && pkg-config --exists spirv-cross-c 2>/dev/null; then
  D=$(pkg-config --variable=libdir spirv-cross-c)
  SC=($(pkg-config --cflags spirv-cross-c) -DRECOMP_SPIRV_CROSS -L"$D" -lspirv-cross-c -lspirv-cross-msl -lspirv-cross-glsl -lspirv-cross-hlsl -lspirv-cross-reflect -lspirv-cross-cpp -lspirv-cross-core -lspirv-cross-util -lc++)
fi
cc -O2 -Wall -I"$RT/gfx" $(pkg-config --cflags sdl3) "$RT/tests/framegen_test.c" "$RT/gfx/framegen/framegen.c" $(pkg-config --libs sdl3) "${SC[@]}" -lm -o "$OUT"
"$OUT" "$@"
