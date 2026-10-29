#!/bin/zsh
# Build Project Nativ's launcher (hub.cpp): build.sh [out]  ->  launcher/nativ by default.
# Dear ImGui's objects are the runtime build's cached ones (build/imgui/obj), built here too when missing;
# only ImGui's own objects are linked -- that folder also holds the in-game overlay, which the hub doesn't use.
# TARGET=windows: nativ.exe with llvm-mingw (runtime/platform/win/setup-toolchain.sh; ATIV_WIN_TOOLCHAIN, default ~/toolchains/ativ-win),
# a windowed program with its manifest (nativ.manifest: UTF-8 as the process code page, Windows 10 1903+, so the UTF-8 paths
# the launcher passes around -- a folder picked in a dialog, a player's own folder name -- reach fopen, stat and the stages'
# command lines intact; no comment in it: "--" ends an XML comment and Windows then refuses to start the program) and the brand icon, SDL3.dll copied beside it; it finds
# the repository from where it sits (launcher/, one level down), so no build machine's path is in it.
set -e
H=${0:A:h}; ROOT=${H:h}
IMGUI=$ROOT/third_party/imgui; IMGUIOBJ=$ROOT/build/imgui/obj
CXX=c++; SDL_CFLAGS=($(pkg-config --cflags sdl3 2>/dev/null)); SDL_LIBS=($(pkg-config --libs sdl3 2>/dev/null)); IMFLAGS=()
LINK=(-lpthread); ROOTDEF=$ROOT
if [[ $TARGET == windows ]]; then
  WT=${ATIV_WIN_TOOLCHAIN:-$HOME/toolchains/ativ-win}; SDLW=$WT/SDL3/x86_64-w64-mingw32; BIN=$WT/llvm-mingw/bin
  [[ -x $BIN/x86_64-w64-mingw32-clang++ ]] || { echo "no Windows toolchain at $WT: run runtime/platform/win/setup-toolchain.sh"; exit 1; }
  CXX=$BIN/x86_64-w64-mingw32-clang++; SDL_CFLAGS=(-I$SDLW/include); SDL_LIBS=($SDLW/lib/libSDL3.dll.a)
  IMGUIOBJ=$ROOT/build/imgui/obj_win; IMFLAGS=(-D_FILE_OFFSET_BITS=64 -include $ROOT/runtime/platform/win32_compat.h)   # (the runtime build's flags: one set of objects for both)
  OUT=${1:-$H/nativ.exe}; ROOTDEF=.
  # the resources: the manifest and an icon made from the brand PNGs (an .ico of PNG images, 16 to 256 px)
  W=$H/obj_win; mkdir -p $W
  python3 - $ROOT/assets/brand/png $W/nativ.ico <<'EOF'
import struct, sys
sizes = [16, 32, 48, 256]; imgs = [open(f"{sys.argv[1]}/icon-{s}.png", "rb").read() for s in sizes]
head = struct.pack("<HHH", 0, 1, len(imgs)); off = 6 + 16 * len(imgs); dirs = b""
for s, b in zip(sizes, imgs): dirs += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(b), off); off += len(b)
open(sys.argv[2], "wb").write(head + dirs + b"".join(imgs))
EOF
  print -r -- "1 ICON \"$W/nativ.ico\"
1 24 \"$H/nativ.manifest\"" > $W/nativ.rc
  $BIN/x86_64-w64-mingw32-windres $W/nativ.rc -O coff -o $W/nativ_res.o
  LINK=($W/nativ_res.o -mwindows -static -lpthread)
else
  OUT=${1:-$H/nativ}
  # an install's toolchain (runtime/platform/linux/setup-toolchain.sh; ATIV_LINUX_TOOLCHAIN, else <root>/toolchain when there is one):
  # zig's clang and libc++, SDL3 copied beside the launcher (found through $ORIGIN), nothing of the machine's but its C library
  LT=${ATIV_LINUX_TOOLCHAIN:-}; [[ -z $LT && -x $ROOT/toolchain/bin/c++ ]] && LT=$ROOT/toolchain
  if [[ -n $LT ]]; then
    CXX=$LT/bin/c++; SDL_CFLAGS=(-I$LT/SDL3/include); SDL_LIBS=(-L$LT/SDL3/lib -lSDL3 '-Wl,-rpath,$ORIGIN'); IMGUIOBJ=$ROOT/build/imgui/obj_lt; ROOTDEF=.
  fi
  # an install's Mac toolchain (runtime/platform/macos/setup-toolchain.sh; ATIV_MAC_TOOLCHAIN, else <root>/toolchain when it holds
  # SDL3): Apple's clang from the Command Line Tools, SDL3 copied beside the launcher (found through @executable_path)
  if [[ $(uname) == Darwin ]]; then
    MT=${ATIV_MAC_TOOLCHAIN:-}; [[ -z $MT && -f $ROOT/toolchain/SDL3/lib/libSDL3.0.dylib ]] && MT=$ROOT/toolchain
    # (built for macOS 11, as the toolchain: an install runs on every Apple silicon Mac, not only the build machine's macOS)
    if [[ -n $MT ]]; then SDL_CFLAGS=(-I$MT/SDL3/include -mmacosx-version-min=11.0); SDL_LIBS=(-L$MT/SDL3/lib -lSDL3 -Wl,-rpath,@executable_path -mmacosx-version-min=11.0); IMGUIOBJ=$ROOT/build/imgui/obj_mt11; ROOTDEF=.; fi
  fi
fi
mkdir -p $IMGUIOBJ
CXXFLAGS=(-O2 -std=c++17 -w -I $IMGUI -I $IMGUI/backends -I $ROOT/runtime/ui -I $ROOT/runtime/platform $SDL_CFLAGS)
objs=()
for s in $IMGUI/imgui.cpp $IMGUI/imgui_draw.cpp $IMGUI/imgui_tables.cpp $IMGUI/imgui_widgets.cpp $IMGUI/backends/imgui_impl_sdl3.cpp $IMGUI/backends/imgui_impl_sdlgpu3.cpp; do
  o=$IMGUIOBJ/${s:t:r}.o; objs+=($o)
  if [[ ! -f $o || $s -nt $o ]]; then ($CXX -c $CXXFLAGS $IMFLAGS $s -o $o.$$ && mv -f $o.$$ $o && echo "  c++ ${s:t}") & fi
done
wait
$CXX $CXXFLAGS -Wall -DREPO_ROOT="\"$ROOTDEF\"" $H/hub.cpp $objs $SDL_LIBS $LINK -o $OUT
[[ $TARGET == windows ]] && cp -f $SDLW/bin/SDL3.dll ${OUT:h}/
[[ -n ${LT:-} ]] && cp -f $LT/SDL3/lib/libSDL3.so.0 ${OUT:h}/libSDL3.so.0.new$$ && mv -f ${OUT:h}/libSDL3.so.0.new$$ ${OUT:h}/libSDL3.so.0
[[ -n ${MT:-} ]] && cp -f $MT/SDL3/lib/libSDL3.0.dylib ${OUT:h}/libSDL3.0.dylib.new$$ && mv -f ${OUT:h}/libSDL3.0.dylib.new$$ ${OUT:h}/libSDL3.0.dylib   # (whole: a running launcher keeps its file)
echo "built $OUT"
