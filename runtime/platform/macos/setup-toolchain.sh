#!/bin/bash
# The Mac toolchain, pinned: the libraries a game build links, built from source so a player's Mac needs nothing but
# Apple's Command Line Tools (`xcode-select --install`: the compiler and the macOS SDK, which the Metal code needs and
# which can't be redistributed). SDL3 as a shared library that ships beside each program; SPIRV-Cross's C API, static
# (the SPIR-V to Metal translator, the same release Homebrew has, so the Metal source it writes, and the shader caches
# keyed by it, stay the same); FFmpeg's H.264 decoder, static (runtime/media/video.c: the movies). Everything targets
# macOS $MIN and arm64 (Apple silicon).
# Into $ATIV_MAC_TOOLCHAIN (default ~/toolchains/ativ-mac); an install keeps it at <root>/toolchain, where runtime/build.py
# and launcher/build.sh look first.
#   setup-toolchain.sh            on a Mac with the Command Line Tools, cmake and ninja (Homebrew's); built here once and
#                                 copied into installs, nothing of this is needed where they run
# Each installed component carries a .nativ-manifest. A changed pin, target, compiler, SDK, flag or build recipe rebuilds
# that component into a temporary directory; the previous install remains in place until an atomic validated swap.
set -euo pipefail

SDL=3.4.16
SDL_SHA=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
SDL_URL="https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL3-$SDL.tar.gz"
SPIRV_CROSS=vulkan-sdk-1.4.363.0
SPIRV_CROSS_SHA=f708982e88b763ef5b0394ed468fc2c3628f68cdab89f2e7a946cd628c04e721
SPIRV_CROSS_URL="https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/$SPIRV_CROSS.tar.gz"
FFMPEG=7.1.1
FFMPEG_SHA=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-$FFMPEG.tar.xz"
MIN=11.0
ARCH=arm64
D=${ATIV_MAC_TOOLCHAIN:-$HOME/toolchains/ativ-mac}
HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPT=$HERE/setup-toolchain.sh

die() { echo "setup-toolchain.sh: $*" >&2; exit 1; }
tool() { command -v "$1" || die "missing required tool: $1"; }
sha256() { shasum -a 256 "$1" | awk '{print $1}'; }
verify() {
  local got
  got=$(sha256 "$1")
  [[ $got == "$2" ]] && return
  echo "checksum mismatch: $1 (expected $2, got $got)" >&2
  rm -f "$1"
  return 1
}
fetch() {
  local url=$1 dst=$2 expected=$3
  if [[ -f $dst ]] && verify "$dst" "$expected"; then return; fi
  curl -fL --retry 3 -o "$dst.part" "$url"
  verify "$dst.part" "$expected" || die "download verification failed: $url"
  mv -f "$dst.part" "$dst"
}
quote() { printf '%q' "$1"; }
quote_args() {
  local arg
  for arg in "$@"; do printf '%q ' "$arg"; done
}
recipe_sha() {
  local block
  block=$(sed -n "/^# $1_RECIPE_BEGIN$/,/^# $1_RECIPE_END$/p" "$SCRIPT")
  [[ $block == *"# $1_RECIPE_BEGIN"* && $block == *"# $1_RECIPE_END"* ]] ||
    die "missing $1 recipe markers"
  printf '%s\n' "$block" | shasum -a 256 | awk '{print $1}'
}

[[ $(uname -s) == Darwin ]] || die "the Mac toolchain is built on macOS"
mkdir -p "$D/dl"
CMAKE=$(tool cmake)
MAKE=$(tool make)
AR=$(tool ar)
LIPO=$(tool lipo)
VTOOL=$(tool vtool)
XCRUN=$(tool xcrun)
CC=$(tool "${CC:-clang}")
CXX=$(tool "${CXX:-clang++}")
SDK=$("$XCRUN" --sdk macosx --show-sdk-path)
SDK_VERSION=$("$XCRUN" --sdk macosx --show-sdk-version)
CC_VERSION=$("$CC" --version | head -1)
CXX_VERSION=$("$CXX" --version | head -1)
CC_TARGET=$("$CC" -print-target-triple)
CMAKE_VERSION=$("$CMAKE" --version | head -1)
MAKE_VERSION=$("$MAKE" --version | head -1)
JOBS=$(sysctl -n hw.ncpu)
GEN=()
if NINJA=$(command -v ninja); then
  GEN=(-G Ninja)
  GENERATOR="Ninja $("$NINJA" --version)"
else
  GENERATOR="CMake default"
fi

CM=(-DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN" "-DCMAKE_OSX_ARCHITECTURES=$ARCH"
    -DCMAKE_INSTALL_LIBDIR=lib "-DCMAKE_C_COMPILER=$CC" "-DCMAKE_OSX_SYSROOT=$SDK")
SDL_CM=(-DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF)
SPIRV_CM=("-DCMAKE_CXX_COMPILER=$CXX" -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_CLI=OFF
          -DSPIRV_CROSS_ENABLE_TESTS=OFF)
FFMPEG_FLAGS=(--enable-static --disable-shared --enable-pic --disable-everything --disable-programs --disable-doc
              --disable-network --disable-autodetect --disable-avdevice --disable-avformat --disable-swscale
              --disable-swresample --disable-avfilter --disable-debug --enable-decoder=h264 --enable-parser=h264
              --enable-parser=hevc)
FFMPEG_TARGET="-arch $ARCH -mmacosx-version-min=$MIN -isysroot $SDK"
SDL_RECIPE_SHA=$(recipe_sha SDL)
SPIRV_RECIPE_SHA=$(recipe_sha SPIRV)
FFMPEG_RECIPE_SHA=$(recipe_sha FFMPEG)

manifest_common() {
  local component=$1
  cat <<EOF
schema=1
component=$component
architecture=$ARCH
deployment_target=$MIN
cc=$(quote "$CC")
cc_version=$(quote "$CC_VERSION")
compiler_target=$(quote "$CC_TARGET")
sdk_path=$(quote "$SDK")
sdk_version=$(quote "$SDK_VERSION")
cflags=$(quote "${CFLAGS-}")
cppflags=$(quote "${CPPFLAGS-}")
ldflags=$(quote "${LDFLAGS-}")
EOF
}
write_sdl_manifest() {
  {
    manifest_common SDL3
    printf 'recipe_sha256=%s\n' "$SDL_RECIPE_SHA"
    printf 'source_version=%s\nsource_sha256=%s\nsource_url=%s\n' "$SDL" "$SDL_SHA" "$SDL_URL"
    printf 'cmake=%s\ncmake_version=%s\ngenerator=%s\ncmake_flags=' \
      "$(quote "$CMAKE")" "$(quote "$CMAKE_VERSION")" "$(quote "$GENERATOR")"
    quote_args "${CM[@]}" "${SDL_CM[@]}"
    printf '\n'
  } > "$1"
}
write_spirv_manifest() {
  {
    manifest_common SPIRV-Cross
    printf 'recipe_sha256=%s\n' "$SPIRV_RECIPE_SHA"
    printf 'source_version=%s\nsource_sha256=%s\nsource_url=%s\n' \
      "$SPIRV_CROSS" "$SPIRV_CROSS_SHA" "$SPIRV_CROSS_URL"
    printf 'cxx=%s\ncxx_version=%s\ncxxflags=%s\n' \
      "$(quote "$CXX")" "$(quote "$CXX_VERSION")" "$(quote "${CXXFLAGS-}")"
    printf 'cmake=%s\ncmake_version=%s\ngenerator=%s\ncmake_flags=' \
      "$(quote "$CMAKE")" "$(quote "$CMAKE_VERSION")" "$(quote "$GENERATOR")"
    quote_args "${CM[@]}" "${SPIRV_CM[@]}"
    printf '\n'
  } > "$1"
}
write_ffmpeg_manifest() {
  {
    manifest_common FFmpeg
    printf 'recipe_sha256=%s\n' "$FFMPEG_RECIPE_SHA"
    printf 'source_version=%s\nsource_sha256=%s\nsource_url=%s\n' "$FFMPEG" "$FFMPEG_SHA" "$FFMPEG_URL"
    printf 'make=%s\nmake_version=%s\nconfigure_flags=' "$(quote "$MAKE")" "$(quote "$MAKE_VERSION")"
    quote_args "--cc=$CC" "--extra-cflags=$FFMPEG_TARGET" "--extra-ldflags=$FFMPEG_TARGET" "${FFMPEG_FLAGS[@]}"
    printf '\n'
  } > "$1"
}

TMP="$D/.nativ-build.$$"
cleanup() {
  local status=$?
  trap - EXIT INT TERM
  rm -rf "$TMP"
  exit "$status"
}
trap cleanup EXIT INT TERM
mkdir -p "$TMP"
SDL_MANIFEST=$TMP/SDL3.manifest
SPIRV_MANIFEST=$TMP/spirv-cross.manifest
FFMPEG_MANIFEST=$TMP/ffmpeg.manifest
write_sdl_manifest "$SDL_MANIFEST"
write_spirv_manifest "$SPIRV_MANIFEST"
write_ffmpeg_manifest "$FFMPEG_MANIFEST"

check_file() {
  [[ -f $1 ]] || { echo "missing toolchain artifact: $1" >&2; return 1; }
}
check_arch() {
  "$LIPO" -verify_arch "$ARCH" "$1" >/dev/null 2>&1 ||
    { echo "toolchain artifact has no $ARCH slice: $1" >&2; return 1; }
}
check_minos() {
  local build
  build=$("$VTOOL" -show-build "$1") || return 1
  awk -v expected="$MIN" '$1 == "minos" {seen=1; if ($2 != expected) bad=1}
    END {exit !seen || bad}' <<<"$build" ||
    { echo "toolchain artifact does not target macOS $MIN: $1" >&2; return 1; }
}
check_archive() {
  local members
  check_file "$1" && check_arch "$1" || return 1
  members=$("$AR" -t "$1") || return 1
  [[ -n $members ]] || { echo "empty or unreadable archive: $1" >&2; return 1; }
}
validate_sdl() {
  local dir=$1
  check_file "$dir/include/SDL3/SDL.h" &&
    check_file "$dir/lib/libSDL3.0.dylib" &&
    check_arch "$dir/lib/libSDL3.0.dylib" &&
    check_minos "$dir/lib/libSDL3.0.dylib"
}
validate_spirv() {
  local dir=$1 n
  check_file "$dir/include/spirv_cross/spirv_cross_c.h" || return 1
  for n in c msl glsl hlsl reflect cpp core util; do
    check_archive "$dir/lib/libspirv-cross-$n.a" || return 1
  done
}
validate_ffmpeg() {
  local dir=$1
  check_file "$dir/include/libavcodec/avcodec.h" &&
    check_file "$dir/include/libavutil/avutil.h" &&
    check_archive "$dir/lib/libavcodec.a" &&
    check_archive "$dir/lib/libavutil.a"
}
component_current() {
  local dir=$1 manifest=$2 validator=$3
  [[ -f "$dir/.nativ-manifest" ]] &&
    cmp -s "$manifest" "$dir/.nativ-manifest" &&
    "$validator" "$dir"
}
install_component() {
  local name=$1 new=$2 dest=$D/$1 swap=$TMP/rename-swap
  if [[ -e $dest || -L $dest ]]; then
    if [[ ! -x $swap ]]; then
      cat > "$TMP/rename-swap.c" <<'C'
#include <stdio.h>
#include <sys/stdio.h>
int main(int argc, char **argv) {
  if (argc != 3) return 2;
  if (renamex_np(argv[1], argv[2], RENAME_SWAP) == 0) return 0;
  perror("renamex_np");
  return 1;
}
C
      "$CC" -arch "$ARCH" "-mmacosx-version-min=$MIN" -isysroot "$SDK" "$TMP/rename-swap.c" -o "$swap"
    fi
    "$swap" "$new" "$dest" || die "could not atomically install $name"
    rm -rf "$new"
  else
    mv "$new" "$dest"
  fi
}

fetch "$SDL_URL" "$D/dl/SDL3-$SDL.tar.gz" "$SDL_SHA"
fetch "$SPIRV_CROSS_URL" "$D/dl/SPIRV-Cross-$SPIRV_CROSS.tar.gz" "$SPIRV_CROSS_SHA"
fetch "$FFMPEG_URL" "$D/dl/ffmpeg-$FFMPEG.tar.xz" "$FFMPEG_SHA"

# SDL_RECIPE_BEGIN
if component_current "$D/SDL3" "$SDL_MANIFEST" validate_sdl; then
  echo "SDL3 $SDL current"
else
  src=$TMP/sdl-src
  build=$TMP/sdl-build
  out=$TMP/SDL3
  mkdir "$src"
  tar xzf "$D/dl/SDL3-$SDL.tar.gz" -C "$src" --strip-components 1
  "$CMAKE" -S "$src" -B "$build" "${GEN[@]}" "${CM[@]}" "-DCMAKE_INSTALL_PREFIX=$out" "${SDL_CM[@]}" \
    > "$D/sdl-configure.log"
  "$CMAKE" --build "$build" -j"$JOBS" > "$D/sdl-build.log" 2>&1
  "$CMAKE" --install "$build" >> "$D/sdl-build.log" 2>&1
  cp "$SDL_MANIFEST" "$out/.nativ-manifest"
  validate_sdl "$out" || die "SDL3 validation failed"
  install_component SDL3 "$out"
fi
# SDL_RECIPE_END

# SPIRV_RECIPE_BEGIN
if component_current "$D/spirv-cross" "$SPIRV_MANIFEST" validate_spirv; then
  echo "SPIRV-Cross $SPIRV_CROSS current"
else
  src=$TMP/spirv-src
  build=$TMP/spirv-build
  out=$TMP/spirv-cross
  mkdir "$src"
  tar xzf "$D/dl/SPIRV-Cross-$SPIRV_CROSS.tar.gz" -C "$src" --strip-components 1
  "$CMAKE" -S "$src" -B "$build" "${GEN[@]}" "${CM[@]}" "-DCMAKE_INSTALL_PREFIX=$out" "${SPIRV_CM[@]}" \
    > "$D/spirv-configure.log"
  "$CMAKE" --build "$build" -j"$JOBS" > "$D/spirv-build.log" 2>&1
  "$CMAKE" --install "$build" >> "$D/spirv-build.log" 2>&1
  mkdir -p "$out/share/licenses"
  cp "$src/LICENSE" "$out/share/licenses/LICENSE"
  cp "$SPIRV_MANIFEST" "$out/.nativ-manifest"
  validate_spirv "$out" || die "SPIRV-Cross validation failed"
  install_component spirv-cross "$out"
fi
# SPIRV_RECIPE_END

# FFMPEG_RECIPE_BEGIN
if component_current "$D/ffmpeg" "$FFMPEG_MANIFEST" validate_ffmpeg; then
  echo "FFmpeg $FFMPEG current"
else
  src=$TMP/ffmpeg-src
  out=$TMP/ffmpeg
  mkdir "$src"
  tar xJf "$D/dl/ffmpeg-$FFMPEG.tar.xz" -C "$src" --strip-components 1
  (
    cd "$src"
    ./configure "--prefix=$out" "--cc=$CC" "--extra-cflags=$FFMPEG_TARGET" \
      "--extra-ldflags=$FFMPEG_TARGET" "${FFMPEG_FLAGS[@]}" > "$D/ffmpeg-configure.log"
    "$MAKE" -j"$JOBS" > "$D/ffmpeg-build.log" 2>&1
    "$MAKE" install >> "$D/ffmpeg-build.log" 2>&1
  )
  mkdir -p "$out/share/licenses"
  cp "$src/LICENSE.md" "$src/COPYING.LGPLv2.1" "$out/share/licenses/"
  printf 'FFmpeg %s (LGPL 2.1 or later), unmodified: %s\nbuilt by runtime/platform/macos/setup-toolchain.sh with only the H.264 decoder and parsers enabled.\n' \
    "$FFMPEG" "$FFMPEG_URL" > "$out/share/licenses/SOURCE.txt"
  cp "$FFMPEG_MANIFEST" "$out/.nativ-manifest"
  validate_ffmpeg "$out" || die "FFmpeg validation failed"
  install_component ffmpeg "$out"
fi
# FFMPEG_RECIPE_END

cat > "$TMP/final.cpp" <<'CPP'
#include <SDL3/SDL_version.h>
#include <spirv_cross/spirv_cross_c.h>
extern "C" {
#include <libavcodec/avcodec.h>
}
int main() {
  spvc_context context = nullptr;
  if (spvc_context_create(&context) != SPVC_SUCCESS) return 1;
  spvc_context_destroy(context);
  return SDL_GetVersion() && avcodec_version() ? 0 : 1;
}
CPP
spirv=()
for n in c msl glsl hlsl reflect cpp core util; do spirv+=("$D/spirv-cross/lib/libspirv-cross-$n.a"); done
"$CXX" -std=c++17 -arch "$ARCH" "-mmacosx-version-min=$MIN" -isysroot "$SDK" \
  -I"$D/SDL3/include" -I"$D/spirv-cross/include" -I"$D/ffmpeg/include" "$TMP/final.cpp" \
  -L"$D/SDL3/lib" -lSDL3 "${spirv[@]}" "$D/ffmpeg/lib/libavcodec.a" "$D/ffmpeg/lib/libavutil.a" \
  -lm -lpthread -Wl,-rpath,@loader_path -o "$TMP/final"
cp "$D/SDL3/lib/libSDL3.0.dylib" "$TMP/"
check_arch "$TMP/final"
check_minos "$TMP/final"
"$TMP/final" || die "final toolchain fixture failed"
rm -f "$D/min"
echo "toolchain: $D (macOS $MIN, $ARCH; SDL3 $SDL built; SPIRV-Cross $SPIRV_CROSS built; FFmpeg $FFMPEG built; manifests current)"
