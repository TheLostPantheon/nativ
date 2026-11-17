#!/usr/bin/env bash
# Every test that needs no game and no firmware: what CI runs (.github/workflows/tests.yml), and a quick check before a
# push. dev/tests/run_all.sh [quick]: quick runs fewer recompiler inputs. Exits non-zero when any test fails; the
# summary at the end names them. The game tests (vitarecomp check, the in-place checker, dev/bench.sh) need the
# player's own copies and stay local.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd); cd "$ROOT"
PY=${PYTHON:-$( [ -x .venv/bin/python ] && echo .venv/bin/python || echo python3 )}
CC=${CC:-cc}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
failed=()
step() { local name=$1; shift; echo "== $name"; if "$@"; then echo "   ok"; else echo "   FAILED"; failed+=("$name"); fi; }

step "game packs (packconf.py --check)" $PY pipeline/packconf.py --check
step "copies and versions (packs.py selftest)" env PYTHONPATH=decrypt $PY pipeline/packs.py selftest
step "recompiler against the CPU (recomp_exec_test.py)" $PY dev/tests/recomp_exec_test.py $([ "${1:-}" = quick ] && echo 20 || echo 60)

INC="-I runtime/core -I runtime/gfx -I runtime/audio -I runtime/media -I runtime/input -I runtime/net -I runtime/ui -I runtime/platform -I runtime/debug -I recompiler -I ."
unit() { local t=$1; shift; (cd runtime/tests && $CC -O1 -g -w -D_GNU_SOURCE ${INC//-I /-I ../../} "$t.c" "$@" -o "$TMP/$t") && "$TMP/$t"; }
step "kernel (kernel_test.c)" unit kernel_test -lpthread
step "touch gestures (gesture_test.c)" unit gesture_test -lm -lpthread
step "JPEG (jpeg_test.c)" unit jpeg_test

echo
if [ ${#failed[@]} -eq 0 ]; then echo "all passed"; else echo "FAILED: ${failed[*]}"; exit 1; fi
