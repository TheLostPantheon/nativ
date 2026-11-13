# SHA-256 verification shared by the macOS toolchain and app installers.
sha256_file() {
  local out
  if command -v sha256sum >/dev/null 2>&1; then
    out=$(sha256sum "$1") || return
  elif command -v shasum >/dev/null 2>&1; then
    out=$(shasum -a 256 "$1") || return
  else
    echo "no SHA-256 checksum command found (need sha256sum or shasum)" >&2
    return 127
  fi
  printf '%s\n' "${out%% *}"
}

verify() {
  local h
  if ! h=$(sha256_file "$1"); then
    echo "cannot verify checksum: $1"
    rm -f "$1"
    return 1
  fi
  [[ $h == "$2" ]] && return
  echo "checksum mismatch: $1 (expected $2, got $h)"
  rm -f "$1"
  return 1
}
