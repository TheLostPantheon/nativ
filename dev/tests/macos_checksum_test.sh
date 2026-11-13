#!/bin/bash
# The stock macOS checksum path: shasum is present and GNU sha256sum is not.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
source "$ROOT/runtime/platform/macos/checksum.sh"

T=$(mktemp -d "${TMPDIR:-/tmp}/nativ-checksum-test.XXXXXX")
trap 'rm -rf "$T"' EXIT
mkdir "$T/bin"
ln -s /usr/bin/shasum "$T/bin/shasum"
ln -s /bin/rm "$T/bin/rm"

printf 'valid download\n' > "$T/valid"
out=$(/usr/bin/shasum -a 256 "$T/valid"); expected=${out%% *}
if PATH="$T/bin" command -v sha256sum >/dev/null; then
  echo "test PATH unexpectedly contains sha256sum"
  exit 1
fi
PATH="$T/bin" verify "$T/valid" "$expected"
[[ -f $T/valid ]] || { echo "valid download was removed"; exit 1; }
echo "ok   shasum verifies a valid download without sha256sum"

printf 'invalid download\n' > "$T/invalid"
if PATH="$T/bin" verify "$T/invalid" "$expected" >"$T/mismatch.log" 2>&1; then
  echo "invalid download passed verification"
  exit 1
fi
[[ ! -e $T/invalid ]] || { echo "invalid download was not removed"; exit 1; }
[[ $(<"$T/mismatch.log") == checksum\ mismatch:* ]] || { echo "checksum failure was not reported"; exit 1; }
echo "ok   an invalid download fails closed"
