#!/usr/bin/env zsh
# The performance benchmark: a recorded play session replayed headless, unpaced, on a binary of its own.
#   dev/bench.sh <title> <recording folder> [frames] [from]
# The recording folder holds input.txt and savedata0/ (made by playing with EXTRA_ENV="RECOMP_CLOCK=fixed
# RECOMP_INPUT_RECORD=<folder>/input.txt", the save copied first: docs/debugging.md). From frame <from> (default 600: past
# the boot and its loads, whose real-time waits unpaced runs skip badly) the game runs as fast as it can
# (RECOMP_UNPACED, the fixed clock); the result is frames per second over each 300 frames up to <frames> (default 3300),
# their mean, and each thread's CPU busy % over the last 300 presents (RECOMP_WAIT_STATS).
#   BUILD=1        build the benchmark binary first (after a change to the recompiler, regenerate the code first:
#                  vitarecomp analyze + recompile)
#   RECOMP_BIN     the binary's name (default bench: <base>_bench, test_bench/), so the play binary is never touched
#   BENCH_ENV      more settings for the run ("RECOMP_X=1 ...")
#   REPEAT=n       n runs after a warm-up run (its shader compiles skew the first), each one's mean, then their median
# The same recording, frames and machine make runs comparable; an A/B is two runs, each on its own build.
set -u
ROOT=${0:A:h:h}
(( $# >= 2 )) || { sed -n '2,15p' $0 | sed 's/^# \{0,1\}//'; exit 2; }
T=$1 REC=${2:A} FRAMES=${3:-3300} FROM=${4:-600}
[[ -f $REC/input.txt && -d $REC/savedata0 ]] || { echo "$REC: needs input.txt and savedata0/"; exit 2; }
export RECOMP_BIN=${RECOMP_BIN:-bench}
LIB=$($ROOT/.venv/bin/python -c "import sys; sys.path.insert(0, '$ROOT/pipeline'); from datadir import library_dir; print(library_dir('$ROOT'))" 2>/dev/null || echo $ROOT/dump)
if [[ -n ${BUILD:-} ]]; then $ROOT/vitarecomp build $T | tail -1 || exit 1; fi
one() {
TEST_SAVES=$REC/savedata0 TEST_TOUCH="" TEST_FRAMES=$FRAMES TEST_COVERAGE=0 TIMEOUT=${TIMEOUT:-1200} \
EXTRA_ENV="RECOMP_GFX=gpu RECOMP_SHADERS= RECOMP_CLOCK=fixed RECOMP_NO_MSPACE=1 RECOMP_INPUT_PLAY=$REC/input.txt RECOMP_UNPACED=$FROM RECOMP_GPU_PRESENT_NOSYNC=1 RECOMP_WAIT_STATS=300 ${BENCH_ENV:-}" \
	$ROOT/vitarecomp test $T >/dev/null
log=$LIB/decrypted/$T/recomp/test_$RECOMP_BIN/test.log
[[ -f $log ]] || { echo "no test log at $log"; exit 1; }
# "frame N: WxH at 0x..., S s": the presented frame's time; fps over each 300 frames from <from>
awk -v from=$FROM -v to=$FRAMES '
	/^ *frame [0-9]+: .* s$/ { f = $2 + 0; t = $(NF - 1) + 0; if (f >= from && f <= to) { if (pf != "") { fps = (f - pf) / (t - pt); printf "%.1f ", fps; s += fps; n++ } pf = f; pt = t } }
	END { if (n) printf "| mean %.1f fps\n", s / n; else print "no frames past " from }' $log
}
if [[ -n ${REPEAT:-} ]]; then
	one >/dev/null   # (the warm-up)
	means=()
	for i in {1..$REPEAT}; do r=$(one); echo $r; means+=(${${r##*mean }%% fps*}); done
	echo "median of $REPEAT: $(printf '%s\n' $means | sort -n | awk '{a[NR] = $1} END {print a[int((NR + 1) / 2)]}') fps"
else
	one
fi
# the last thread report (every 300 presents): the unpaced stretch's, busiest thread first
awk '/^ *waits: [0-9]+ presents/ { n = 0; delete b } /^ *waits: / { b[n++] = $0 } END { for (i = 0; i < n && i < 6; i++) print substr(b[i], 1, 140) }' $log
