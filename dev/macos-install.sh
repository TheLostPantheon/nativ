#!/bin/bash
# The Mac install of Project Nativ: one self-contained app, macos-install.sh <folder> -> <folder>/Project Nativ.app
#   Contents/MacOS/Project Nativ    the launcher itself (libSDL3.0.dylib beside it); it finds its tools in Resources and,
#                                   before its window, checks it runs from where it was put (not a disk image or the
#                                   temporary copy macOS makes of a downloaded app: hub.cpp mac_preflight) and that
#                                   Apple's Command Line Tools are there (the compiler and SDK every game build needs;
#                                   Apple's to install, not ours to ship)
#   Contents/Resources/             the shipped tools (vitarecomp dist: pipeline, runtime, packs, ...), version.txt,
#                                   toolchain/ (SDL3, SPIRV-Cross and FFmpeg's H.264 decoder: runtime/platform/macos/
#                                   setup-toolchain.sh's, without its downloads and build trees) and python/
#                                   (python-build-standalone with requirements.txt installed)
# Nothing writes inside the app once it's made (a signed one must match its signature): games, builds and settings live
# in the player's library and settings folders, compiled Python in the settings folder (PYTHONPYCACHEPREFIX).
# Run again over the same folder to update it (dist removes only what it wrote; python is kept when current). Apple
# silicon (arm64), macOS 13 and later. Needs the toolchain built here first ($ATIV_MAC_TOOLCHAIN, default
# ~/toolchains/ativ-mac). Signing, notarising and the .dmg: dev/release.sh.
set -euo pipefail
PY_VER=3.13.17
PY_REL=20261021
PY_SHA=9c985cbf327425873469896ce748c7134b6deca8edb81e93c589ad2fc5aff445   # aarch64-apple-darwin-install_only
H=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$H")
DEST=${1:?usage: macos-install.sh <folder>}; mkdir -p "$DEST"; DEST=$(cd "$DEST" && pwd)
APP="$DEST/Project Nativ.app"; OUT="$APP/Contents/Resources"; mkdir -p "$OUT" "$APP/Contents/MacOS"
MT=${ATIV_MAC_TOOLCHAIN:-$HOME/toolchains/ativ-mac}
[[ -f $MT/SDL3/lib/libSDL3.0.dylib && -f $MT/spirv-cross/lib/libspirv-cross-c.a ]] || { echo "no Mac toolchain at $MT: run runtime/platform/macos/setup-toolchain.sh"; exit 1; }
PYX=${PYTHON:-$ROOT/.venv/bin/python}; [[ -x $PYX ]] || PYX=python3

# the tools
NATIV_DIST_BUILT=0 "$PYX" "$ROOT/pipeline/vitarecomp.py" dist "$OUT"   # (the launcher is built below, as the app's own program)

# the toolchain: the libraries and headers builds need, not the downloads, sources or build trees
mkdir -p "$OUT/toolchain"
for d in SDL3 spirv-cross ffmpeg; do rsync -a --delete --exclude cmake --exclude pkgconfig --exclude 'libSDL3_test.a' "$MT/$d/" "$OUT/toolchain/$d/"; done

# Python
if [[ ! -x $OUT/python/bin/python3 || "$("$OUT/python/bin/python3" -c 'import platform; print(platform.python_version())')" != "$PY_VER" ]]; then
  DL=$MT/dl; A=cpython-$PY_VER+$PY_REL-aarch64-apple-darwin-install_only.tar.gz
  verify() { local h; h=$(sha256sum "$1" 2>/dev/null | awk '{print $1}'); [[ -n $h ]] || h=$(shasum -a 256 "$1" | awk '{print $1}'); [[ $h == "$2" ]] && return; echo "checksum mismatch: $1 (expected $2, got $h)"; rm -f "$1"; exit 1; }
  [[ -f $DL/$A ]] || { curl -fL --retry 3 -o "$DL/$A.part" "https://github.com/astral-sh/python-build-standalone/releases/download/$PY_REL/$A" && verify "$DL/$A.part" "$PY_SHA" && mv -f "$DL/$A.part" "$DL/$A"; }
  rm -rf "$OUT/python"; tar xzf "$DL/$A" -C "$OUT"   # (unpacks as python/)
fi
"$OUT/python/bin/python3" -m pip install --quiet --disable-pip-version-check --no-warn-script-location -r "$ROOT/requirements.txt"

# the launcher, built with the install's own toolchain (no build machine's path in it), as the app's program
ATIV_MAC_TOOLCHAIN=$OUT/toolchain zsh "$ROOT/launcher/build.sh" "$APP/Contents/MacOS/Project Nativ"
cp "$ROOT/assets/brand/nativ.icns" "$OUT/nativ.icns"
V=$(cat "$OUT/version.txt"); BV=${V#v}; [[ $BV =~ ^[0-9]+(\.[0-9]+){1,2}$ ]] || BV=0.0.0   # (the bundle's version: a release's number)
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>Project Nativ</string>
  <key>CFBundleDisplayName</key><string>Project Nativ</string>
  <key>CFBundleIdentifier</key><string>org.projectnativ.launcher</string>
  <key>CFBundleExecutable</key><string>Project Nativ</string>
  <key>CFBundleIconFile</key><string>nativ</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$BV</string>
  <key>CFBundleVersion</key><string>$BV</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSCameraUsageDescription</key><string>Some games use the Vita's rear camera; Nativ can use this Mac's camera for it.</string>
  <key>NSLocalNetworkUsageDescription</key><string>Online play finds other players on your network.</string>
</dict></plist>
EOF
echo "install: $APP ($V; $(du -sh "$APP" | cut -f1))"
