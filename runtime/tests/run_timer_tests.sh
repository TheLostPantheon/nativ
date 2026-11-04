#!/bin/bash
# Test run: timer_test post-fix, its pre-fix stub control, and the kernel_test gate.
# Run from runtime/tests (bash, WSL or MSYS). Compile+run in one process: the WSL /tmp is volatile.
set -u
cd "$(dirname "$0")"
I="-I ../core -I ../gfx -I ../audio -I ../media -I ../input -I ../net -I ../ui -I ../platform -I ../debug -I ../../recompiler -I ../.."
OUT="${TMPDIR:-/tmp}"

cc -O1 -g -Wall -Wno-unused-function -D_GNU_SOURCE $I timer_test.c ../debug/waitstats.c -o "$OUT/timer_test" -lpthread -lm || { echo "COMPILE-FAIL post-fix"; exit 2; }
echo "== post-fix:"
"$OUT/timer_test"; echo "rc=$?"

cc -O1 -g -Wall -Wno-unused-function -DTIMER_STUB_CONTROL -D_GNU_SOURCE $I timer_test.c ../debug/waitstats.c -o "$OUT/timer_ctl" -lpthread -lm || { echo "COMPILE-FAIL control"; exit 2; }
echo "== pre-fix stub control (expected: fails > 0):"
"$OUT/timer_ctl"; echo "rc=$?"

cc -O1 -g -Wall -Wno-unused-function -D_GNU_SOURCE $I kernel_test.c -o "$OUT/kernel_test" -lpthread -lm || { echo "COMPILE-FAIL kernel_test"; exit 2; }
echo "== kernel_test gate:"
"$OUT/kernel_test" | tail -1
