#!/bin/sh
# servers/nativ-node/build.sh [out]: the node (node.c) with the runtime's overlay, crypto and DHT, into nativ-node here by
# default. Plain C and POSIX: cc on macOS or Linux; a small machine with a public UDP port is all it needs.
set -e
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../.." && pwd); ON=$R/runtime/net/online
${CC:-cc} -O2 -w -I"$R/runtime/core" -I"$R/runtime/net" -I"$ON" -I"$R" \
  "$H/node.c" "$ON/overlay.c" "$ON/crypto.c" "$ON/dht.c" "$R/runtime/net/sha1.c" -lpthread -o "${1:-$H/nativ-node}"
echo "built ${1:-$H/nativ-node}"
