#!/bin/bash
# A self-contained Linux install of Project Nativ in one folder (a Steam Deck's, any distribution's): linux-install.sh <folder>
#   <folder>/            the shipped tools (vitarecomp dist: pipeline, runtime, packs, ...) and version.txt
#   <folder>/toolchain/  zig's clang, SDL3, zlib, FFmpeg (runtime/platform/linux/setup-toolchain.sh's, copied without its
#                        downloads and build trees): runtime/build.py and launcher/build.sh use it, nothing of the machine's
#   <folder>/python/     python-build-standalone with requirements.txt installed: the stages' Python (hub.cpp python_exe)
#   <folder>/launcher/nativ + libSDL3.so.0   built with that toolchain; it finds the folder from where it sits
#   <folder>/Project Nativ.sh                starts it (Steam: add it as a non-Steam game)
# Run again over the same folder to update it (dist removes only what it wrote; python and toolchain are kept when current).
# Needs the toolchain built here first ($ATIV_LINUX_TOOLCHAIN, default ~/toolchains/ativ-linux). The player's library
# (dump/: games, firmware) is not part of it: the launcher's first run sets that up.
set -euo pipefail
PY_VER=3.13.17
PY_REL=20261021
PY_SHA=cbda6f7767884fc456860b11fd184391a9320cae00a86c0f49a1c7ac6e236f7a   # x86_64-unknown-linux-gnu-install_only
H=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$H")
OUT=${1:?usage: linux-install.sh <folder>}; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
LT=${ATIV_LINUX_TOOLCHAIN:-$HOME/toolchains/ativ-linux}
[[ -x $LT/bin/cc && -f $LT/SDL3/lib/libSDL3.so.0 ]] || { echo "no Linux toolchain at $LT: run runtime/platform/linux/setup-toolchain.sh"; exit 1; }
PYX=${PYTHON:-$ROOT/.venv/bin/python}; [[ -x $PYX ]] || PYX=python3

# the tools
"$PYX" "$ROOT/pipeline/vitarecomp.py" dist "$OUT"

# the toolchain: what builds need, not its downloads, sources or build trees
mkdir -p "$OUT/toolchain"
for d in bin zig SDL3 zlib ffmpeg; do rsync -a --delete "$LT/$d/" "$OUT/toolchain/$d/"; done
rm -rf "$OUT/toolchain/SDL3/lib/cmake" "$OUT/toolchain/SDL3/lib/pkgconfig" "$OUT/toolchain/ffmpeg/lib/pkgconfig"

# Python
if [[ ! -x $OUT/python/bin/python3 || "$("$OUT/python/bin/python3" -c 'import platform; print(platform.python_version())')" != "$PY_VER" ]]; then
  DL=$LT/dl; A=cpython-$PY_VER+$PY_REL-x86_64-unknown-linux-gnu-install_only.tar.gz
  verify() { local h; h=$(sha256sum "$1" 2>/dev/null | awk '{print $1}'); [[ -n $h ]] || h=$(shasum -a 256 "$1" | awk '{print $1}'); [[ $h == "$2" ]] && return; echo "checksum mismatch: $1 (expected $2, got $h)"; rm -f "$1"; exit 1; }
  [[ -f $DL/$A ]] || { curl -fL --retry 3 -o "$DL/$A.part" "https://github.com/astral-sh/python-build-standalone/releases/download/$PY_REL/$A" && verify "$DL/$A.part" "$PY_SHA" && mv -f "$DL/$A.part" "$DL/$A"; }
  rm -rf "$OUT/python"; tar xzf "$DL/$A" -C "$OUT"   # (unpacks as python/)
fi
"$OUT/python/bin/python3" -m pip install --quiet --disable-pip-version-check --no-warn-script-location -r "$ROOT/requirements.txt"

# the launcher, built with the install's own toolchain (no build machine's path in it)
ATIV_LINUX_TOOLCHAIN=$OUT/toolchain "$ROOT/launcher/build.sh" "$OUT/launcher/nativ"
cat > "$OUT/Project Nativ.sh" <<'EOF'
#!/bin/sh
# Project Nativ's launcher (Steam: add this file as a non-Steam game)
exec "$(dirname "$(readlink -f "$0")")/launcher/nativ" "$@"
EOF
chmod +x "$OUT/Project Nativ.sh"
echo "install: $OUT ($(cat "$OUT/version.txt"); $(du -sh "$OUT" | cut -f1))"
