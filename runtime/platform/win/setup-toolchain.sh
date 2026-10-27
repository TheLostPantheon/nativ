#!/bin/bash
# The Windows cross toolchain, pinned: llvm-mingw (clang + the mingw-w64 UCRT runtime, winpthreads included -- the
# same compiler as macOS, Linux and the Android NDK), SDL3's MinGW development package, and zlib built with it; and
# FFmpeg's H.264 decoder (libavcodec + libavutil, static, nothing else: runtime/media/video.c's sceAvcdec -- the movies),
# built with it too (no x86 assembly unless nasm is installed: the Vita's 960x544 video decodes fine in C). The HEVC
# parser only for its SEI code: FFmpeg 7.1's H.264 decoder calls the film-grain helper built with it.
# Into $ATIV_WIN_TOOLCHAIN (default ~/toolchains/ativ-win); build.sh's TARGET=windows looks there.
#   setup-toolchain.sh            Linux x86_64 host (macOS: set LLVM_MINGW_ASSET to the macos-universal tarball)
set -euo pipefail
LLVM_MINGW=20261006
SDL=3.4.16
SDL_SHA=c7ef65bd72eabac6e5b535411dbd8d5824d0aab24fd62ff8812666b336f18a9c
ZLIB=1.3.1
ZLIB_SHA=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
FFMPEG=7.1.1
FFMPEG_SHA=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
D=${ATIV_WIN_TOOLCHAIN:-$HOME/toolchains/ativ-win}
case "$(uname -s)" in
  Darwin) ASSET=${LLVM_MINGW_ASSET:-llvm-mingw-$LLVM_MINGW-ucrt-macos-universal.tar.xz}; ASSET_SHA=${LLVM_MINGW_ASSET_SHA:-a7a19b11048e89372637d26279cdb7c430ed3cf8c1e224310421312bf986ebf6} ;;
  *)      ASSET=${LLVM_MINGW_ASSET:-llvm-mingw-$LLVM_MINGW-ucrt-ubuntu-22.04-x86_64.tar.xz}; ASSET_SHA=${LLVM_MINGW_ASSET_SHA:-5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4} ;;
esac
mkdir -p "$D/dl"; cd "$D/dl"
# a download's sha256 against its pin above, so a tampered or changed archive stops the script
verify() { local h; h=$(sha256sum "$1" 2>/dev/null | awk '{print $1}'); [[ -n $h ]] || h=$(shasum -a 256 "$1" | awk '{print $1}'); [[ $h == "$2" ]] && return; echo "checksum mismatch: $1 (expected $2, got $h)"; rm -f "$1"; exit 1; }
fetch() { [[ -f $2 ]] || curl -fL --retry 3 -o "$2.part" "$1" && verify "$2.part" "$3" && mv -f "$2.part" "$2" 2>/dev/null || true; }
fetch "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW/$ASSET" "$ASSET" "$ASSET_SHA"
fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL3-devel-$SDL-mingw.tar.gz" "SDL3-devel-$SDL-mingw.tar.gz" "$SDL_SHA"
fetch "https://github.com/madler/zlib/releases/download/v$ZLIB/zlib-$ZLIB.tar.gz" "zlib-$ZLIB.tar.gz" "$ZLIB_SHA"
fetch "https://ffmpeg.org/releases/ffmpeg-$FFMPEG.tar.xz" "ffmpeg-$FFMPEG.tar.xz" "$FFMPEG_SHA"
cd "$D"
[[ -d llvm-mingw ]] || { tar xf "dl/$ASSET"; mv "${ASSET%.tar.xz}" llvm-mingw; }
[[ -d SDL3 ]] || { tar xzf "dl/SDL3-devel-$SDL-mingw.tar.gz"; mv "SDL3-$SDL" SDL3; }
CC="$D/llvm-mingw/bin/x86_64-w64-mingw32-clang"
if [[ ! -f zlib/lib/libz.a ]]; then
  rm -rf zlib-src; mkdir zlib-src; tar xzf "dl/zlib-$ZLIB.tar.gz" -C zlib-src --strip-components 1
  mkdir -p zlib/lib zlib/include
  ( cd zlib-src && for f in adler32 crc32 deflate infback inffast inflate inftrees trees zutil compress uncompr gzclose gzlib gzread gzwrite; do "$CC" -O2 -c $f.c -o $f.o; done
    "$D/llvm-mingw/bin/llvm-ar" rcs ../zlib/lib/libz.a *.o; cp zlib.h zconf.h ../zlib/include/ )
fi
if [[ ! -f ffmpeg/lib/libavcodec.a ]]; then
  rm -rf ffmpeg-src; mkdir ffmpeg-src; tar xJf "dl/ffmpeg-$FFMPEG.tar.xz" -C ffmpeg-src --strip-components 1
  ASM=--disable-x86asm; command -v nasm >/dev/null && ASM=
  ( cd ffmpeg-src && PATH="$D/llvm-mingw/bin:$PATH" ./configure --prefix="$D/ffmpeg" --target-os=mingw32 --arch=x86_64 --enable-cross-compile \
      --cross-prefix=x86_64-w64-mingw32- --cc=x86_64-w64-mingw32-clang --cxx=x86_64-w64-mingw32-clang++ --ar=llvm-ar --nm=llvm-nm --ranlib=llvm-ranlib \
      --enable-static --disable-shared --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
      --disable-avdevice --disable-avformat --disable-swscale --disable-swresample --disable-avfilter --disable-debug \
      --enable-decoder=h264 --enable-parser=h264 --enable-parser=hevc $ASM > ../ffmpeg-configure.log \
    && PATH="$D/llvm-mingw/bin:$PATH" make -j"$( (nproc || sysctl -n hw.ncpu) 2>/dev/null)" > ../ffmpeg-build.log 2>&1 && PATH="$D/llvm-mingw/bin:$PATH" make install >> ../ffmpeg-build.log 2>&1 )
fi
echo "toolchain: $D ($("$CC" --version | head -1); SDL3 $SDL; zlib $ZLIB; FFmpeg $FFMPEG $([[ -f ffmpeg/lib/libavcodec.a ]] && echo built || echo MISSING))"
