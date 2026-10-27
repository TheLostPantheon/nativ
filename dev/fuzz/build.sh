#!/bin/zsh
# dev/fuzz/build.sh: the online service's parsers -- everything that reads what other players, the LAN or the internet
# send -- under libFuzzer, AddressSanitizer and UBSan, into build/fuzz/. Needs a clang with -fsanitize=fuzzer (the Linux
# box's); FUZZ_DRIVER=1 builds them with driver.c instead (Apple's clang: a smoke test, not fuzzing).
# Run one: build/fuzz/fuzz_m2 -max_total_time=600 dev/fuzz/seeds/m2 (a crash leaves crash-<hash> and its stack).
set -e
R=${0:A:h:h:h}; F=$R/dev/fuzz; ON=$R/runtime/net/online; OUT=$R/build/fuzz; mkdir -p $OUT
I=(-I$R/runtime/core -I$R/runtime/net -I$ON -I$R/runtime/platform -I$R/recompiler -I$R)
FL=(-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -w)
if [[ -n $FUZZ_DRIVER ]]; then X=($F/driver.c); else FL+=(-fsanitize=fuzzer); X=(); fi
CC=${CC:-clang}
$CC $FL $I $F/fuzz_overlay.c $ON/crypto.c $F/stubs.c $X -o $OUT/fuzz_overlay -lpthread
$CC $FL $I $F/fuzz_discovery.c $F/stubs.c $X -o $OUT/fuzz_discovery -lpthread
$CC $FL $I $F/fuzz_m2.c $ON/m2_server.c $ON/m2_rooms.c $ON/m2_wire.c $ON/aes128.c $R/runtime/net/sha1.c $F/stubs.c $X -o $OUT/fuzz_m2 -lpthread
$CC $FL $I $F/fuzz_dht.c $ON/dht.c $R/runtime/net/sha1.c $F/stubs.c $X -o $OUT/fuzz_dht -lpthread
$CC $FL $I $F/fuzz_stun.c $F/stubs.c $X -o $OUT/fuzz_stun -lpthread
$CC $FL $I $F/fuzz_bmsg.c $ON/basic_msg.c $F/stubs.c $X -o $OUT/fuzz_bmsg -lpthread
echo "built $OUT: fuzz_overlay fuzz_discovery fuzz_m2 fuzz_dht fuzz_stun fuzz_bmsg"
