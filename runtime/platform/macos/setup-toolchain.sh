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
set -euo pipefail
SDL=3.4.16
SDL_SHA=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
SPIRV_CROSS=vulkan-sdk-1.4.363.0
SPIRV_CROSS_SHA=f708982e88b763ef5b0394ed468fc2c3628f68cdab89f2e7a946cd628c04e721
FFMPEG=7.1.1
FFMPEG_SHA=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
MIN=11.0   # macOS Big Sur, the first on Apple silicon: every M-series Mac (the shaders are MSL 2.3, the pipeline archive MTLBinaryArchive: both 11.0)
ARCH=arm64
D=${ATIV_MAC_TOOLCHAIN:-$HOME/toolchains/ativ-mac}
mkdir -p "$D/dl"; cd "$D/dl"
# a download's sha256 against its pin above, so a tampered or changed archive stops the script
verify() { local h; h=$(sha256sum "$1" 2>/dev/null | awk '{print $1}'); [[ -n $h ]] || h=$(shasum -a 256 "$1" | awk '{print $1}'); [[ $h == "$2" ]] && return; echo "checksum mismatch: $1 (expected $2, got $h)"; rm -f "$1"; exit 1; }
fetch() { [[ -f $2 ]] || { curl -fL --retry 3 -o "$2.part" "$1" && verify "$2.part" "$3" && mv -f "$2.part" "$2"; }; }
fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL3-$SDL.tar.gz" "SDL3-$SDL.tar.gz" "$SDL_SHA"
fetch "https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/$SPIRV_CROSS.tar.gz" "SPIRV-Cross-$SPIRV_CROSS.tar.gz" "$SPIRV_CROSS_SHA"
fetch "https://ffmpeg.org/releases/ffmpeg-$FFMPEG.tar.xz" "ffmpeg-$FFMPEG.tar.xz" "$FFMPEG_SHA"
cd "$D"
[[ $(cat min 2>/dev/null) == "$MIN" ]] || rm -rf SDL3 spirv-cross ffmpeg   # (built for another minimum macOS: again)
GEN=; command -v ninja >/dev/null && GEN="-G Ninja"
CM=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN -DCMAKE_OSX_ARCHITECTURES=$ARCH -DCMAKE_INSTALL_LIBDIR=lib)
if [[ ! -f SDL3/lib/libSDL3.0.dylib ]]; then
  rm -rf sdl-src sdl-build; mkdir sdl-src; tar xzf "dl/SDL3-$SDL.tar.gz" -C sdl-src --strip-components 1
  cmake -S sdl-src -B sdl-build $GEN "${CM[@]}" -DCMAKE_INSTALL_PREFIX="$D/SDL3" \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF > sdl-configure.log
  cmake --build sdl-build -j"$(sysctl -n hw.ncpu)" > sdl-build.log 2>&1 && cmake --install sdl-build >> sdl-build.log 2>&1
fi
if [[ ! -f spirv-cross/lib/libspirv-cross-c.a ]]; then
  rm -rf spirv-src spirv-build; mkdir spirv-src; tar xzf "dl/SPIRV-Cross-$SPIRV_CROSS.tar.gz" -C spirv-src --strip-components 1
  cmake -S spirv-src -B spirv-build $GEN "${CM[@]}" -DCMAKE_INSTALL_PREFIX="$D/spirv-cross" \
    -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF > spirv-configure.log
  cmake --build spirv-build -j"$(sysctl -n hw.ncpu)" > spirv-build.log 2>&1 && cmake --install spirv-build >> spirv-build.log 2>&1
fi
if [[ ! -f ffmpeg/lib/libavcodec.a ]]; then
  rm -rf ffmpeg-src; mkdir ffmpeg-src; tar xJf "dl/ffmpeg-$FFMPEG.tar.xz" -C ffmpeg-src --strip-components 1
  F="-arch $ARCH -mmacosx-version-min=$MIN"
  ( cd ffmpeg-src && ./configure --prefix="$D/ffmpeg" --cc=clang --extra-cflags="$F" --extra-ldflags="$F" \
      --enable-static --disable-shared --enable-pic --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
      --disable-avdevice --disable-avformat --disable-swscale --disable-swresample --disable-avfilter --disable-debug \
      --enable-decoder=h264 --enable-parser=h264 --enable-parser=hevc > ../ffmpeg-configure.log \
    && make -j"$(sysctl -n hw.ncpu)" > ../ffmpeg-build.log 2>&1 && make install >> ../ffmpeg-build.log 2>&1 )
fi
# their licences beside them, as SDL's own install does (an install ships these libraries: Apache 2.0 and the LGPL ask for
# the text, and the LGPL for where the source is)
mkdir -p spirv-cross/share/licenses ffmpeg/share/licenses
cp spirv-src/LICENSE spirv-cross/share/licenses/LICENSE
cp ffmpeg-src/LICENSE.md ffmpeg-src/COPYING.LGPLv2.1 ffmpeg/share/licenses/
printf 'FFmpeg %s (LGPL 2.1 or later), unmodified: https://ffmpeg.org/releases/ffmpeg-%s.tar.xz\nbuilt by runtime/platform/macos/setup-toolchain.sh with only the H.264 decoder and parsers enabled.\n' "$FFMPEG" "$FFMPEG" > ffmpeg/share/licenses/SOURCE.txt
ok() { [[ -f $1 ]] && echo built || echo MISSING; }
echo "toolchain: $D (macOS $MIN, $ARCH; SDL3 $SDL $(ok SDL3/lib/libSDL3.0.dylib); SPIRV-Cross $SPIRV_CROSS $(ok spirv-cross/lib/libspirv-cross-c.a); FFmpeg $FFMPEG $(ok ffmpeg/lib/libavcodec.a))"
echo "$MIN" > "$D/min"
