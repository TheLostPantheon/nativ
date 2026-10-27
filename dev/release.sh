#!/bin/bash
# release.sh <tag> <os>...: a release's downloads, made on this machine in build/release/<tag>/. For each OS named, its
# install folder (mac: dev/macos-install.sh, on a Mac; windows: dev/windows-install.sh, on macOS or Linux; linux:
# dev/linux-install.sh, on Linux) zipped as ProjectNativ-<tag>-<os>.zip with one ProjectNativ folder inside, then
# SHA256SUMS over every zip there (a release made on two machines: copy the other's zip in, run again with no OS).
# The tag must be what's checked out, with nothing uncommitted: the installs' version.txt is the tag, which is what their
# update check compares (vitarecomp update-check).
#   NATIV_RELEASES=<owner>/<repo>   the GitHub repository whose releases these are: the installs' update channel
#   NATIV_SIGN_ID                   macOS: a "Developer ID Application: ..." identity in this Mac's keychain; every
#                                   program in the Mac install is signed with it (hardened runtime, timestamped)
#   NATIV_NOTARY_PROFILE            macOS: a notarytool keychain profile (xcrun notarytool store-credentials); the
#                                   signed Mac zip is notarised and the ticket stapled to the app
#   NATIV_SIGN_CMD                  Windows: a command signing one program in place (dev/windows-install.sh)
# Unsigned without them, as players' guides describe. Publishing is separate and printed at the end.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$H")
TAG=${1:?usage: release.sh <tag> [mac|windows|linux]...}; shift
[[ $TAG =~ ^v[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] || { echo "release.sh: a tag like v0.1.0"; exit 1; }
if (( $# )); then
  [[ "$(git -C "$ROOT" describe --tags --exact-match 2>/dev/null)" == "$TAG" ]] || { echo "release.sh: check out $TAG first (git tag $TAG; git checkout $TAG)"; exit 1; }
  [[ -z "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]] || { echo "release.sh: uncommitted changes: a release is built from the tag alone"; exit 1; }
  [[ -n ${NATIV_RELEASES:-} ]] || echo "(no NATIV_RELEASES: these installs won't check for updates)"
fi
OUT=$ROOT/build/release/$TAG; mkdir -p "$OUT"
sha() { sha256sum "$@" 2>/dev/null || shasum -a 256 "$@"; }
zipdir() {   # zipdir <zip> <folder>, run where the folder is: zip -qry where there's one, else Python's zipfile, keeping the
  # programs' modes and the symlinks as zip -y does (unzip and the desktops' tools restore both)
  if command -v zip >/dev/null; then zip -qry "$1" "$2"; return; fi
  python3 - "$1" "$2" <<'PY'
import os, stat, sys, zipfile
out, top = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for root, dirs, files in os.walk(top):
        dirs.sort(); z.write(root)
        for n in sorted(files) + [d for d in dirs if os.path.islink(os.path.join(root, d))]:
            p = os.path.join(root, n)
            if os.path.islink(p):
                zi = zipfile.ZipInfo(p); zi.create_system = 3; zi.external_attr = (stat.S_IFLNK | 0o777) << 16
                z.writestr(zi, os.readlink(p))
            else: z.write(p)
PY
}

for os in "$@"; do
  W=$OUT/work-$os; rm -rf "$W"; mkdir -p "$W"; I=$W/ProjectNativ; Z=$OUT/ProjectNativ-$TAG-$os.zip; rm -f "$Z"
  case $os in
    mac)
      [[ $(uname) == Darwin ]] || { echo "release.sh: the Mac install is made on a Mac"; exit 1; }
      bash "$H/macos-install.sh" "$I"
      if [[ -n ${NATIV_SIGN_ID:-} ]]; then   # every Mach-O program and library, then the app around them
        find "$I" -type f \( -perm -u+x -o -name '*.dylib' -o -name '*.so' \) -print0 | while IFS= read -r -d '' f; do
          file -b "$f" | grep -q Mach-O && codesign --force --options runtime --timestamp --sign "$NATIV_SIGN_ID" "$f"
        done
        codesign --force --options runtime --timestamp --sign "$NATIV_SIGN_ID" "$I/Project Nativ.app"
      fi
      (cd "$W" && ditto -c -k --keepParent ProjectNativ "$Z")
      if [[ -n ${NATIV_SIGN_ID:-} && -n ${NATIV_NOTARY_PROFILE:-} ]]; then
        xcrun notarytool submit "$Z" --keychain-profile "$NATIV_NOTARY_PROFILE" --wait
        xcrun stapler staple "$I/Project Nativ.app"
        rm -f "$Z"; (cd "$W" && ditto -c -k --keepParent ProjectNativ "$Z")   # (again, with the ticket in the app)
      fi;;
    windows)
      bash "$H/windows-install.sh" "$I"
      (cd "$W" && zipdir "$Z" ProjectNativ);;
    linux)
      [[ $(uname) == Linux ]] || { echo "release.sh: the Linux install is made on Linux"; exit 1; }
      bash "$H/linux-install.sh" "$I"
      (cd "$W" && zipdir "$Z" ProjectNativ);;
    *) echo "release.sh: mac, windows or linux, not $os"; exit 1;;
  esac
  rm -rf "$W"
  echo "$Z ($(du -h "$Z" | cut -f1))"
done

(cd "$OUT" && rm -f SHA256SUMS && sha ProjectNativ-"$TAG"-*.zip > SHA256SUMS && cat SHA256SUMS)
echo "to publish: gh release create $TAG $OUT/ProjectNativ-$TAG-*.zip $OUT/SHA256SUMS --repo ${NATIV_RELEASES:-<owner>/<repo>} --title \"Project Nativ $TAG\" --notes-file <notes.md>"
