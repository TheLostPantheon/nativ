#!/bin/bash
# release.sh <tag> <os>...: a release's downloads, made on this machine in build/release/<tag>/. For each OS named:
#   mac      ProjectNativ-<tag>-mac.dmg: Project Nativ.app (dev/macos-install.sh) beside a link to Applications, the
#            player drags one onto the other; made on a Mac
#   windows  ProjectNativ-<tag>-windows.zip, one ProjectNativ folder inside (dev/windows-install.sh; on macOS or Linux)
#   linux    ProjectNativ-<tag>-linux.zip, the same (dev/linux-install.sh; on Linux)
# then SHA256SUMS over every download there (a release made on two machines: copy the other's in, run again with no OS).
# The tag must be what's checked out, with nothing uncommitted: the installs' version.txt is the tag, which is what their
# update check compares (vitarecomp update-check).
#   NATIV_RELEASES=<owner>/<repo>   the GitHub repository whose releases these are: the installs' update channel
#   NATIV_SIGN_ID                   macOS: a "Developer ID Application: ..." identity in this Mac's keychain; every
#                                   program and library in the app is signed with it (hardened runtime, timestamped),
#                                   then the app and the .dmg
#   NATIV_NOTARY_PROFILE            macOS: a notarytool keychain profile (xcrun notarytool store-credentials): the app
#                                   is notarised and its ticket stapled, then the .dmg the same, so a player's Mac
#                                   opens it with no more than "downloaded from the internet: open?"
#   NATIV_SIGN_CMD                  Windows: a command signing one program in place (dev/windows-install.sh)
# Unsigned without them, as players' guides describe. Publishing is separate and printed at the end.
# This machine's own values for these (a signing identity, a profile name) go in dev/local.env, which git ignores.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$H")
[[ -f $ROOT/dev/local.env ]] && source "$ROOT/dev/local.env"
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
      bash "$H/macos-install.sh" "$W"; APP="$W/Project Nativ.app"; Z=$OUT/ProjectNativ-$TAG-mac.dmg; rm -f "$Z"
      if [[ -n ${NATIV_SIGN_ID:-} ]]; then
        ENT=$W/entitlements.plist   # (Python loads its own modules' libraries; a game may use this Mac's camera)
        printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
          '<plist version="1.0"><dict><key>com.apple.security.cs.disable-library-validation</key><true/><key>com.apple.security.device.camera</key><true/></dict></plist>' > "$ENT"
        sign() { codesign --force --options runtime --timestamp --sign "$NATIV_SIGN_ID" "$@"; }
        n=0; while IFS= read -r -d '' f; do   # every Mach-O inside, libraries before the programs that load them
          t=$(file -b "$f"); [[ $t == Mach-O* ]] || continue
          if [[ $t == *executable* ]]; then sign --entitlements "$ENT" "$f"; else sign "$f"; fi >/dev/null 2>&1 || { echo "release.sh: signing $f failed"; sign "$f"; exit 1; }
          n=$((n + 1))
        done < <(find "$APP/Contents/Resources" "$APP/Contents/MacOS" -type f ! -path "$APP/Contents/MacOS/Project Nativ" -print0)
        sign --entitlements "$ENT" "$APP"; codesign --verify --strict --deep "$APP"; echo "signed: $n libraries and programs, and the app"
        if [[ -n ${NATIV_NOTARY_PROFILE:-} ]]; then
          ditto -c -k --keepParent "$APP" "$W/app.zip"
          xcrun notarytool submit "$W/app.zip" --keychain-profile "$NATIV_NOTARY_PROFILE" --wait | tail -2
          xcrun stapler staple -q "$APP"
        fi
      fi
      mkdir -p "$W/dmg"; ditto "$APP" "$W/dmg/Project Nativ.app"; ln -s /Applications "$W/dmg/Applications"
      hdiutil create -quiet -volname "Project Nativ" -srcfolder "$W/dmg" -ov -format UDZO "$Z"
      if [[ -n ${NATIV_SIGN_ID:-} ]]; then
        codesign --force --timestamp --sign "$NATIV_SIGN_ID" "$Z"
        if [[ -n ${NATIV_NOTARY_PROFILE:-} ]]; then
          xcrun notarytool submit "$Z" --keychain-profile "$NATIV_NOTARY_PROFILE" --wait | tail -2
          xcrun stapler staple -q "$Z"; spctl --assess --type open --context context:primary-signature -v "$Z" 2>&1 | tail -1
        fi
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

(cd "$OUT" && rm -f SHA256SUMS && shopt -s nullglob && sha ProjectNativ-"$TAG"-*.{zip,dmg} > SHA256SUMS && cat SHA256SUMS)
echo "to publish: gh release create $TAG $OUT/ProjectNativ-$TAG-*.{zip,dmg} $OUT/SHA256SUMS --repo ${NATIV_RELEASES:-<owner>/<repo>} --title \"Project Nativ $TAG\" --notes-file <notes.md>"
