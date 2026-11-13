#!/bin/bash
# Verify a release DMG as a player receives it: mount it, copy the app to a writable folder, then check its binaries,
# Python and build toolchain without opening the launcher.
set -euo pipefail

die() { echo "macos-smoke.sh: $*" >&2; exit 1; }

[[ $# == 1 ]] || die "usage: macos-smoke.sh <ProjectNativ-*-mac.dmg>"
[[ $(uname -s) == Darwin ]] || die "the Mac package is checked on macOS"
DMG=$1
[[ -f $DMG ]] || die "no DMG at $DMG"
DMG=$(cd "$(dirname "$DMG")" && pwd)/$(basename "$DMG")

TMP=$(mktemp -d "${TMPDIR:-/tmp}/nativ-mac-smoke.XXXXXX")
MOUNT=$TMP/mount
mkdir "$MOUNT"
mounted=0
cleanup() {
  status=$?
  trap - EXIT
  if (( mounted )); then hdiutil detach -quiet "$MOUNT" >/dev/null 2>&1 || true; fi
  rm -rf "$TMP"
  exit "$status"
}
trap cleanup EXIT

hdiutil attach -quiet -nobrowse -readonly -mountpoint "$MOUNT" "$DMG"
mounted=1
[[ -d "$MOUNT/Project Nativ.app" ]] || die "the DMG has no Project Nativ.app"
ditto "$MOUNT/Project Nativ.app" "$TMP/Project Nativ.app"
hdiutil detach -quiet "$MOUNT"
mounted=0

APP="$TMP/Project Nativ.app"
ROOT="$APP/Contents/Resources"
PLIST="$APP/Contents/Info.plist"
[[ -x "$APP/Contents/MacOS/Project Nativ" && -f "$ROOT/pipeline/vitarecomp.py" ]] ||
  die "the copied app is incomplete"
MIN=$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$PLIST") ||
  die "Info.plist has no LSMinimumSystemVersion"
[[ $MIN =~ ^[0-9]+(\.[0-9]+){1,2}$ ]] || die "invalid LSMinimumSystemVersion: $MIN"

portable_ref() {
  case $1 in
    @*|/usr/lib/*|/System/Library/*) return 0;;
    *) return 1;;
  esac
}
portable_id() {
  case $1 in
    ""|@*|/usr/lib/*|/System/Library/*) return 0;;
    */*) return 1;;
    *) return 0;;
  esac
}

machos=0
while IFS= read -r -d '' f; do
  kind=$(file -b "$f")
  [[ $kind == Mach-O* ]] || continue
  archs=$(lipo -archs "$f") || die "lipo could not read $f"
  lipo -verify_arch arm64 "$f" || die "$f has architecture '$archs', expected an arm64 slice"

  build=$(vtool -show-build "$f") || die "vtool could not read $f"
  minos=$(awk '$1 == "minos" {print $2}' <<<"$build")
  [[ -n $minos ]] || die "$f has no macOS build version"
  while IFS= read -r declared; do
    [[ $declared == "$MIN" ]] || die "$f targets macOS $declared, Info.plist says $MIN"
  done <<<"$minos"

  commands=$(otool -l "$f") || die "otool could not read $f"
  install_ids=$(awk '$1 == "cmd" && $2 == "LC_ID_DYLIB" {want=1; next}
                     want && $1 == "name" {print $2; want=0}' <<<"$commands")
  while IFS= read -r ref; do
    portable_id "$ref" || die "$f contains non-relocatable install ID $ref"
  done <<<"$install_ids"
  loads=$(awk '$1 == "cmd" && ($2 == "LC_LOAD_DYLIB" || $2 == "LC_LOAD_WEAK_DYLIB" ||
                              $2 == "LC_REEXPORT_DYLIB" || $2 == "LC_LOAD_UPWARD_DYLIB" ||
                              $2 == "LC_LAZY_LOAD_DYLIB" || $2 == "LC_LOAD_DYLINKER") {want=1; next}
               want && $1 == "name" {print $2; want=0}' <<<"$commands")
  while IFS= read -r ref; do
    [[ -z $ref ]] || portable_ref "$ref" || die "$f contains non-relocatable load path $ref"
  done <<<"$loads"

  rpaths=$(awk '$1 == "cmd" && $2 == "LC_RPATH" {want=1; next}
                want && $1 == "path" {print $2; want=0}' <<<"$commands")
  while IFS= read -r ref; do
    [[ -z $ref || $ref == @loader_path* || $ref == @executable_path* ]] ||
      die "$f contains non-relocatable rpath $ref"
  done <<<"$rpaths"
  machos=$((machos + 1))
done < <(find "$APP" -type f -print0)
(( machos > 0 )) || die "the app contains no Mach-O files"

archives=0
while IFS= read -r -d '' f; do
  archs=$(lipo -archs "$f") || die "lipo could not read $f"
  lipo -verify_arch arm64 "$f" || die "$f has architecture '$archs', expected an arm64 slice"
  archives=$((archives + 1))
done < <(find "$ROOT/toolchain" -type f -name '*.a' -print0)
(( archives > 0 )) || die "the toolchain contains no static libraries"

PY="$ROOT/python/bin/python3"
[[ -x $PY ]] || die "bundled Python is missing"
PYTHONDONTWRITEBYTECODE=1 "$PY" -I -c 'import capstone, Crypto' ||
  die "bundled Python cannot import capstone and Crypto"

TC="$ROOT/toolchain"
for f in \
  "$TC/SDL3/include/SDL3/SDL.h" "$TC/SDL3/lib/libSDL3.0.dylib" \
  "$TC/spirv-cross/include/spirv_cross/spirv_cross_c.h" "$TC/spirv-cross/lib/libspirv-cross-c.a" \
  "$TC/ffmpeg/include/libavcodec/avcodec.h" "$TC/ffmpeg/lib/libavcodec.a" "$TC/ffmpeg/lib/libavutil.a"; do
  [[ -f $f ]] || die "toolchain component is missing: $f"
done

cat > "$TMP/smoke.cpp" <<'CPP'
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
for n in c msl glsl hlsl reflect cpp core util; do
  f="$TC/spirv-cross/lib/libspirv-cross-$n.a"
  [[ -f $f ]] || die "toolchain component is missing: $f"
  spirv+=("$f")
done
if ! xcrun clang++ -std=c++17 -arch arm64 "-mmacosx-version-min=$MIN" \
  -I"$TC/SDL3/include" -I"$TC/spirv-cross/include" -I"$TC/ffmpeg/include" "$TMP/smoke.cpp" \
  -L"$TC/SDL3/lib" -lSDL3 "${spirv[@]}" "$TC/ffmpeg/lib/libavcodec.a" "$TC/ffmpeg/lib/libavutil.a" \
  -lm -lpthread -Wl,-rpath,@loader_path -o "$TMP/smoke"; then
  die "the shipped headers and libraries could not compile and link a fixture"
fi
cp "$TC/SDL3/lib/libSDL3.0.dylib" "$TMP/"
"$TMP/smoke" || die "the linked fixture did not run"

echo "macOS package smoke: $machos Mach-O files, $archives static libraries, Python and toolchain ok (arm64, macOS $MIN)"
