#!/bin/bash
# Does a self-contained Linux install (dev/linux-install.sh) build a game on a machine with no compiler, no headers, no
# development libraries and no Python -- a Steam Deck's SteamOS -- and does what it builds run?
#   sealed_linux_build.sh <install folder> [title, default PCSE00640] [work folder, default ~/.cache/nativ-sealed]
# 1. A stand-in system: Arch Linux's bootstrap root (SteamOS is Arch underneath) with /usr/include deleted, as SteamOS ships
#    it. It has bash, coreutils and glibc, and no gcc, clang, make, pkg-config, python or SDL.
# 2. A test library holding a copy of the title's analysed game (this checkout's dump/decrypted/<title>, without its builds),
#    so the player's own builds are never touched.
# 3. Inside that root (bubblewrap, no network): the install's own Python runs the recompile and build stages.
# 4. Outside it: every binary checked to ask for nothing newer than the toolchain's glibc, then the scripted test of the
#    build on this machine's GPU (this checkout's vitarecomp test, which runs the binary as built), and its frame compared
#    with the title's reference.
set -euo pipefail
INST=$(cd "${1:?usage: sealed_linux_build.sh <install folder> [title] [work folder]}" && pwd); T=${2:-PCSE00640}; W=${3:-$HOME/.cache/nativ-sealed}
H=$(cd "$(dirname "$0")/../.." && pwd)
ROOTFS=$W/rootfs LIB=$W/library
mkdir -p "$W"
if [[ ! -x $ROOTFS/usr/bin/bash ]]; then
  [[ -f $W/bootstrap.tar.zst ]] || curl -fL --retry 3 -o "$W/bootstrap.tar.zst" https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst
  rm -rf "$ROOTFS"; mkdir -p "$ROOTFS"
  tar --zstd -xf "$W/bootstrap.tar.zst" -C "$ROOTFS" --strip-components 1 --exclude='root.x86_64/dev/*' 2>/dev/null || true
  rm -rf "$ROOTFS/usr/include"
fi
for x in gcc cc clang make pkg-config python3 python; do [[ -e $ROOTFS/usr/bin/$x ]] && { echo "the stand-in root has $x: not sealed"; exit 1; }; done
[[ -d $ROOTFS/usr/include ]] && { echo "the stand-in root has /usr/include: not sealed"; exit 1; }
ls "$ROOTFS"/usr/lib/libSDL3* >/dev/null 2>&1 && { echo "the stand-in root has SDL3: not sealed"; exit 1; }

SRC=$H/dump/decrypted/$T
[[ -d $SRC/recomp ]] || { echo "no analysed $T in $H/dump/decrypted"; exit 1; }
rm -rf "$LIB/decrypted/$T"; mkdir -p "$LIB/decrypted"
rsync -a --exclude 'recomp/obj*' --exclude 'recomp/*_host' --exclude 'recomp/*_gate' --exclude 'recomp/test*' --exclude 'recomp/*.dll' --exclude 'recomp/libSDL3*' "$SRC/" "$LIB/decrypted/$T/"
[[ -d $H/dump/firmware ]] && { mkdir -p "$LIB/firmware"; rsync -a "$H/dump/firmware/" "$LIB/firmware/"; }
for d in us sys seeds maps; do [[ -d $H/dump/decrypted/$d ]] && rsync -a "$H/dump/decrypted/$d/" "$LIB/decrypted/$d/"; done   # (the firmware's modules, from a console dump, and what every game shares)

# a game folder linked to elsewhere (Killzone's app0 on another drive, Uncharted's fs/app0 -> the EU copy beside it): the copy's
# link aimed at the real folder, seen read-only at the same place inside (under /run or /home, empty
# tmpfs here)
EXTRA=(); while IFS= read -r l; do
  t=$(readlink -f "$SRC/${l#"$LIB/decrypted/$T/"}"); [[ -d $t ]] || continue
  ln -sfn "$t" "$l"; EXTRA+=(--ro-bind "$t" "$t")
done < <(find "$LIB/decrypted/$T" -maxdepth 3 -type l)
sealed() {   # a command inside the stand-in root: the install at /nativ, the library at /library, no network
  bwrap --ro-bind "$ROOTFS" / --dev /dev --proc /proc --tmpfs /tmp --tmpfs /run --tmpfs /home --bind "$INST" /nativ --bind "$LIB" /library "${EXTRA[@]}" \
    --unshare-all --die-with-parent --clearenv --setenv PATH /usr/bin --setenv HOME /tmp --setenv ATIV_DUMP /library --chdir /nativ "$@"
}
mkdir -p "$ROOTFS/nativ" "$ROOTFS/library"   # (mount points: the root is bound read-only)
sealed /bin/true || { echo "bubblewrap can't start the stand-in root"; exit 1; }
sealed /bin/sh -c 'for x in cc gcc clang python3 python pkg-config; do command -v $x; done' | grep . && { echo "a compiler or Python is reachable inside: not sealed"; exit 1; }
t0=$(date +%s)
sealed /nativ/python/bin/python3 /nativ/pipeline/vitarecomp.py recompile "$T"
sealed /nativ/python/bin/python3 /nativ/pipeline/vitarecomp.py build "$T"
echo "sealed recompile + build: $(( $(date +%s) - t0 )) s"

B=$(find "$LIB/decrypted/$T/recomp" -maxdepth 1 -name '*_host' -type f | head -1)
[[ -n $B ]] || { echo "FAIL: no binary built"; exit 1; }
worst=$(for f in "$B" "$(dirname "$B")/libSDL3.so.0" "$INST/launcher/nativ"; do objdump -T "$f" | grep -o 'GLIBC_[0-9.]*'; done | sort -t. -k2,2n -k3,3n -u | tail -1)
echo "binaries ask for glibc up to: ${worst#GLIBC_}"
NEEDED=$(objdump -p "$B" | awk '/NEEDED/ {print $2}' | tr '\n' ' '); echo "the game binary needs: $NEEDED"
for n in $NEEDED; do case $n in libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|ld-linux-x86-64.so.2|libSDL3.so.0|librt.so.1) ;; *) echo "FAIL: needs $n, which SteamOS may not have"; exit 1 ;; esac; done

# its frame on this machine's GPU, against the reference
ATIV_DUMP=$LIB "$H/vitarecomp" test "$T" > "$W/test.out" 2>&1 || true
FR=$(cd "$H" && python3 -c "import sys; sys.path.insert(0,'pipeline'); import vitarecomp as v; print(v.Game('$T','check').TEST_FRAMES)" 2>/dev/null || echo 2400)
SHOT=$(ls "$LIB/decrypted/$T/recomp"/test*/shot_$(printf %05d "$FR").ppm 2>/dev/null | head -1 || true)
[[ -n $SHOT ]] || { echo "FAIL: no test frame $FR (see $W/test.out)"; exit 1; }
REF=$SRC/recomp/ref_$FR.ppm
if [[ ! -f $REF ]]; then   # (no reference, a title outside the gate: this checkout's own build's frame from the same test)
  "$H/vitarecomp" test "$T" > "$W/test-dev.out" 2>&1 || true; REF=$SRC/recomp/test/shot_$(printf %05d "$FR").ppm
  echo "no reference frame: comparing with this checkout's build"
fi
echo "frame $FR differs in $(python3 "$H/dev/framediff.py" "$REF" "$SHOT")% of pixels"
