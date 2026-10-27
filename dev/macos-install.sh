#!/bin/bash
# A self-contained Mac install of Project Nativ in one folder: macos-install.sh <folder>
#   <folder>/                   the shipped tools (vitarecomp dist: pipeline, runtime, packs, ...) and version.txt
#   <folder>/toolchain/         SDL3, SPIRV-Cross and FFmpeg's H.264 decoder (runtime/platform/macos/setup-toolchain.sh's,
#                               without its downloads and build trees): runtime/build.py and launcher/build.sh use them
#   <folder>/python/            python-build-standalone with requirements.txt installed: the stages' Python (hub.cpp python_exe)
#   <folder>/launcher/nativ     built with that toolchain, libSDL3.0.dylib beside it; it finds the folder from where it sits
#   <folder>/Project Nativ.app  starts it. On its first start it asks for Apple's Command Line Tools when they're missing
#                               (the compiler and SDK a game build needs; Apple's to install, not ours to ship) and clears
#                               the download quarantine from the folder, so macOS doesn't stop each program in it
# Run again over the same folder to update it (dist removes only what it wrote; python is kept when current). Apple silicon
# (arm64), macOS 13 and later. Needs the toolchain built here first ($ATIV_MAC_TOOLCHAIN, default ~/toolchains/ativ-mac).
# The player's library (games, firmware) is not part of it: the launcher's first run sets that up.
set -euo pipefail
PY_VER=3.13.17
PY_REL=20261021
PY_SHA=9c985cbf327425873469896ce748c7134b6deca8edb81e93c589ad2fc5aff445   # aarch64-apple-darwin-install_only
H=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$H")
OUT=${1:?usage: macos-install.sh <folder>}; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
MT=${ATIV_MAC_TOOLCHAIN:-$HOME/toolchains/ativ-mac}
[[ -f $MT/SDL3/lib/libSDL3.0.dylib && -f $MT/spirv-cross/lib/libspirv-cross-c.a ]] || { echo "no Mac toolchain at $MT: run runtime/platform/macos/setup-toolchain.sh"; exit 1; }
PYX=${PYTHON:-$ROOT/.venv/bin/python}; [[ -x $PYX ]] || PYX=python3

# the tools
"$PYX" "$ROOT/pipeline/vitarecomp.py" dist "$OUT"

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

# the launcher, built with the install's own toolchain (no build machine's path in it)
ATIV_MAC_TOOLCHAIN=$OUT/toolchain zsh "$ROOT/launcher/build.sh" "$OUT/launcher/nativ"

# Project Nativ.app: a small bundle whose program checks the two things a fresh Mac lacks, then starts the launcher
APP="$OUT/Project Nativ.app"; rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$ROOT/assets/brand/nativ.icns" "$APP/Contents/Resources/nativ.icns"
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
  <key>CFBundleShortVersionString</key><string>$(cat "$OUT/version.txt")</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
EOF
cat > "$APP/Contents/MacOS/Project Nativ" <<'EOF'
#!/bin/sh
# Project Nativ's launcher, from the install folder this app sits in
R=$(cd "$(dirname "$0")/../../.." && pwd)
# App Translocation: a downloaded app opened where it was unzipped runs from a read-only copy of the app alone, without the
# folder around it. Say how to open it from its folder instead
case $R in */AppTranslocation/*)
  osascript -e 'display dialog "macOS opened Project Nativ from a temporary copy, without the rest of its folder.\n\nIn Terminal, run:\n    xattr -dr com.apple.quarantine \nfollowed by the ProjectNativ folder (drag it into the Terminal window), then open Project Nativ again." with title "Project Nativ" buttons {"OK"} default button 1 with icon caution' >/dev/null 2>&1
  exit 1;;
esac
# a download's quarantine on the rest of the folder: the player allowed this app, so its own files are cleared once
if xattr "$R/launcher/nativ" 2>/dev/null | grep -q com.apple.quarantine; then xattr -dr com.apple.quarantine "$R" 2>/dev/null; fi
# Apple's Command Line Tools: the compiler and SDK every game build needs
if ! xcode-select -p >/dev/null 2>&1; then
  xcode-select --install >/dev/null 2>&1
  osascript -e 'display dialog "Project Nativ builds each game on your Mac, which needs Apple'\''s free Command Line Tools.\n\nInstall them from the window that just opened, then open Project Nativ again." with title "Project Nativ" buttons {"OK"} default button 1 with icon note' >/dev/null 2>&1
  exit 0
fi
exec "$R/launcher/nativ" "$@"
EOF
chmod +x "$APP/Contents/MacOS/Project Nativ"
echo "install: $OUT ($(cat "$OUT/version.txt"); $(du -sh "$OUT" | cut -f1))"
