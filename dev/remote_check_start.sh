#!/bin/zsh
# started detached on a gate runner by remote.sh: the gate's output to build/check.log, a marker at the end
cd ${0:A:h:h} && mkdir -p build && rm -f build/check.log && ./vitarecomp check "$@" > build/check.log 2>&1; echo REMOTE-CHECK-DONE >> build/check.log
