#!/bin/bash
# The self-contained Linux toolchain, pinned: Zig's clang (zig cc: one download carrying its own glibc headers and stubs,
# libc++ and the kernel's headers, so a machine without a compiler or development files -- SteamOS -- builds with it), with
# every binary aimed at glibc $GLIBC (anything since 2018 runs them); SDL3 built with it as a shared library that loads its
# video and audio backends (X11, Wayland, PipeWire, PulseAudio, ALSA) and Vulkan when it runs, so it ships beside the game;
# zlib and FFmpeg's H.264 decoder (libavcodec + libavutil, static: runtime/media/video.c's sceAvcdec -- the movies) built
# with it too. No libcurl: the HTTP calls stay stubs, as on Windows.
# Into $ATIV_LINUX_TOOLCHAIN (default ~/toolchains/ativ-linux); an install keeps it at <root>/toolchain, where
# runtime/build.py and launcher/build.sh look first.
#   setup-toolchain.sh            on a Linux x86_64 machine with SDL's backends' headers (X11, Wayland, xkbcommon, PipeWire,
#                                 PulseAudio, ALSA, libdecor, dbus, udev) and cmake, ninja or make, wayland-scanner; the
#                                 install is built here once and copied, nothing of this is needed where it runs
set -euo pipefail
ZIG=0.15.2
ZIG_SHA=02aa270f183da276e5b5920b1dac44a63f1a49e55050ebde3aecc9eb82f93239
GLIBC=2.28
SDL=3.4.16
SDL_SHA=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
ZLIB=1.3.1
ZLIB_SHA=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
FFMPEG=7.1.1
FFMPEG_SHA=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
D=${ATIV_LINUX_TOOLCHAIN:-$HOME/toolchains/ativ-linux}
mkdir -p "$D/dl" "$D/bin"; cd "$D/dl"
# a download's sha256 against its pin above, so a tampered or changed archive stops the script
verify() { local h; h=$(sha256sum "$1" 2>/dev/null | awk '{print $1}'); [[ -n $h ]] || h=$(shasum -a 256 "$1" | awk '{print $1}'); [[ $h == "$2" ]] && return; echo "checksum mismatch: $1 (expected $2, got $h)"; rm -f "$1"; exit 1; }
fetch() { [[ -f $2 ]] || { curl -fL --retry 3 -o "$2.part" "$1" && verify "$2.part" "$3" && mv -f "$2.part" "$2"; }; }
fetch "https://ziglang.org/download/$ZIG/zig-x86_64-linux-$ZIG.tar.xz" "zig-$ZIG.tar.xz" "$ZIG_SHA"
fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL3-$SDL.tar.gz" "SDL3-$SDL.tar.gz" "$SDL_SHA"
fetch "https://github.com/madler/zlib/releases/download/v$ZLIB/zlib-$ZLIB.tar.gz" "zlib-$ZLIB.tar.gz" "$ZLIB_SHA"
fetch "https://ffmpeg.org/releases/ffmpeg-$FFMPEG.tar.xz" "ffmpeg-$FFMPEG.tar.xz" "$FFMPEG_SHA"
cd "$D"
[[ -d zig ]] || { tar xJf "dl/zig-$ZIG.tar.xz"; mv "zig-x86_64-linux-$ZIG" zig; }
# the compiler as plain executables (build.py and configure scripts take one program, not "zig cc"); relative to where the
# toolchain sits, so an install moved elsewhere keeps working. ZIG_GLOBAL_CACHE_DIR: zig's cache inside the toolchain unless
# the caller set one (a read-only install sets it to a writable place)
for t in cc:cc c++:c++ ar:ar ranlib:ranlib; do n=${t%%:*}; sub=${t##*:}
  tgt=; [[ $n == cc || $n == c++ ]] && tgt="-target x86_64-linux-gnu.$GLIBC"
  # (the compilers: each run's own local cache, deleted after it -- zig keeps a copy of every object it compiles there, which
  # would double a game build's disk use; libc++ and the libc stubs it builds once stay in the global cache)
  if [[ -n $tgt ]]; then run='L=$(mktemp -d "${TMPDIR:-/tmp}/nativ-zig.XXXXXX") || exit 1
ZIG_LOCAL_CACHE_DIR=$L "$D/zig/zig" '"$sub $tgt"' "$@"; r=$?; rm -rf "$L"; exit $r'
  else run='exec "$D/zig/zig" '"$sub"' "$@"'; fi
  cat > "bin/$n" <<EOF
#!/bin/sh
D=\$(cd "\$(dirname "\$0")/.." && pwd)
: "\${ZIG_GLOBAL_CACHE_DIR:=\$D/cache}"; export ZIG_GLOBAL_CACHE_DIR
$run
EOF
  chmod +x "bin/$n"
done
CC="$D/bin/cc"
if [[ ! -f zlib/lib/libz.a ]]; then
  rm -rf zlib-src; mkdir zlib-src; tar xzf "dl/zlib-$ZLIB.tar.gz" -C zlib-src --strip-components 1
  mkdir -p zlib/lib zlib/include
  ( cd zlib-src && for f in adler32 crc32 deflate infback inffast inflate inftrees trees zutil compress uncompr gzclose gzlib gzread gzwrite; do "$CC" -O2 -fPIC -D_LARGEFILE64_SOURCE=1 -DHAVE_UNISTD_H -c $f.c -o $f.o; done
    "$D/bin/ar" rcs ../zlib/lib/libz.a *.o; cp zlib.h zconf.h ../zlib/include/ )
fi
if [[ ! -f SDL3/lib/libSDL3.so.0 ]]; then
  # SDL's backends' headers without the machine's own C library headers: zig's glibc headers stay the only ones, so the
  # library asks for nothing newer than glibc $GLIBC. The headers are only declarations: every backend is loaded at run time
  rm -rf sdl-inc; mkdir sdl-inc
  for h in X11 xkbcommon KHR EGL GL GLES2 GLES3 drm libdrm gbm.h libudev.h alsa pulse pipewire-0.3 spa-0.2 libdecor-0 dbus-1.0 libusb-1.0 \
           wayland-client.h wayland-client-core.h wayland-client-protocol.h wayland-cursor.h wayland-egl.h wayland-egl-core.h wayland-util.h wayland-version.h; do
    [[ -e /usr/include/$h ]] && ln -s "/usr/include/$h" "sdl-inc/$h"
  done
  [[ -d /usr/lib/dbus-1.0/include ]] && ln -s /usr/lib/dbus-1.0/include/dbus "sdl-inc/dbus-arch"   # (dbus-arch-deps.h: dbus.h includes it as <dbus/dbus-arch-deps.h>)
  rm -rf sdl-src sdl-build; mkdir sdl-src; tar xzf "dl/SDL3-$SDL.tar.gz" -C sdl-src --strip-components 1
  GEN=; command -v ninja >/dev/null && GEN="-G Ninja"
  INC="-isystem $D/sdl-inc -isystem $D/sdl-inc/dbus-1.0 -isystem $D/sdl-inc/pipewire-0.3 -isystem $D/sdl-inc/spa-0.2 -isystem $D/sdl-inc/libdecor-0 -isystem $D/sdl-inc/libdrm -isystem $D/sdl-inc/libusb-1.0"
  [[ -e sdl-inc/dbus-arch ]] && { mkdir -p sdl-inc/dbus-arch-root/dbus; ln -sf /usr/lib/dbus-1.0/include/dbus/dbus-arch-deps.h sdl-inc/dbus-arch-root/dbus/; INC="$INC -isystem $D/sdl-inc/dbus-arch-root"; }
  cmake -S sdl-src -B sdl-build $GEN -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$D/SDL3" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$D/bin/c++" -DCMAKE_AR="$D/bin/ar" -DCMAKE_RANLIB="$D/bin/ranlib" \
    -DCMAKE_C_FLAGS="$INC" -DCMAKE_CXX_FLAGS="$INC" -DPKG_CONFIG_EXECUTABLE=/usr/bin/pkg-config \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF \
    -DCMAKE_LINK_DEPENDS_USE_LINKER=OFF \
    -DSDL_HIDAPI_LIBUSB=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_KMSDRM=OFF -DSDL_RPATH=OFF > sdl-configure.log
  cmake --build sdl-build -j"$(nproc)" > sdl-build.log 2>&1 && cmake --install sdl-build >> sdl-build.log 2>&1
fi
if [[ ! -f ffmpeg/lib/libavcodec.a ]]; then
  rm -rf ffmpeg-src; mkdir ffmpeg-src; tar xJf "dl/ffmpeg-$FFMPEG.tar.xz" -C ffmpeg-src --strip-components 1
  ASM=--disable-x86asm; command -v nasm >/dev/null && ASM=
  # (HAVE_SYSCTL off: configure finds the function, but zig's glibc headers have no <sys/sysctl.h> -- gone since glibc 2.32;
  # FFmpeg only counts processors with it on the BSDs)
  ( cd ffmpeg-src && ./configure --prefix="$D/ffmpeg" --cc="$CC" --cxx="$D/bin/c++" --ar="$D/bin/ar" --ranlib="$D/bin/ranlib" --nm=nm \
      --enable-static --disable-shared --enable-pic --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
      --disable-avdevice --disable-avformat --disable-swscale --disable-swresample --disable-avfilter --disable-debug \
      --enable-decoder=h264 --enable-parser=h264 --enable-parser=hevc $ASM > ../ffmpeg-configure.log \
    && sed -i 's/^#define HAVE_SYSCTL 1/#define HAVE_SYSCTL 0/' config.h \
    && make -j"$(nproc)" > ../ffmpeg-build.log 2>&1 && make install >> ../ffmpeg-build.log 2>&1 )
fi
# their licences beside them, as SDL's own install does and zig's folder already has (an install ships these: zlib's and
# the LGPL ask for the text, and the LGPL for where the source is)
mkdir -p zlib/share/licenses ffmpeg/share/licenses
cp zlib-src/LICENSE zlib/share/licenses/LICENSE
cp ffmpeg-src/LICENSE.md ffmpeg-src/COPYING.LGPLv2.1 ffmpeg/share/licenses/
printf 'FFmpeg %s (LGPL 2.1 or later), unmodified: https://ffmpeg.org/releases/ffmpeg-%s.tar.xz\nbuilt by runtime/platform/linux/setup-toolchain.sh with only the H.264 decoder and parsers enabled.\n' "$FFMPEG" "$FFMPEG" > ffmpeg/share/licenses/SOURCE.txt
echo "toolchain: $D (zig $("$D/zig/zig" version), glibc $GLIBC; SDL3 $SDL $([[ -f SDL3/lib/libSDL3.so.0 ]] && echo built || echo MISSING); zlib $ZLIB; FFmpeg $FFMPEG $([[ -f ffmpeg/lib/libavcodec.a ]] && echo built || echo MISSING))"
