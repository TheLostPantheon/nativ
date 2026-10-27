#!/bin/zsh
# u13_online_duo.sh: two Unit 13 copies on this machine play co-op over Nativ's P2P online (docs/online.md).
# A hosts: TOUCH TO LAUNCH, ONLINE COOPERATIVE, HOST CO-OP, PUBLIC, mission 01, ACCEPT, then waits on the co-op
# loadout ("WAITING FOR PLAYER"). B starts B_DELAY seconds later (default 30), opens the co-op lobby, presses REFRESH
# until A's game is listed, taps the first row and CONFIRM; A taps DEPLOY at frame 4800 and both load mission 01 (the
# shots show it). The verdict: A's room reports B joined, or the frame limits. Shots every 150 frames from B_SHOTS_FROM in <title>/recomp/test_a and test_b.
#
# Needs a second binary of the game (RECOMP_BIN=a ./vitarecomp build $TITLE; game_b is copied from it), so the copy the
# player runs is never relinked. Env: TITLE (default PCSA00014), NAME_A / NAME_B (online names after ntv_), A_FRAMES,
# B_FRAMES, A_TOUCH / B_TOUCH and A_INPUT / B_INPUT (each copy's taps and buttons, from-to:hexmask), EXTRA (both copies'
# env), VERDICT_S. U13_NET_DIAG=1 builds log the game's np callbacks too (the pack).
R=${0:A:h:h:h}; DUMP=${ATIV_DUMP:-$R/dump}; TITLE=${TITLE:-PCSA00014}
D=$DUMP/decrypted/$TITLE/recomp; OUT=${OUT:-$D}
SAVES=${TEST_SAVES:-$DUMP/decrypted/PCSA00014/recomp/fs/savedata0}   # (the player's save, past the training, copied for
# each run: the co-op screens as a player meets them; DEPLOY doesn't need it)
if pgrep -f "[_]host game.img" >/dev/null; then echo "a game is running: not now"; exit 1; fi
[[ -x $D/game_a ]] || { echo "build game_a first: RECOMP_BIN=a ./vitarecomp build $TITLE"; exit 1; }
[[ $D/game_b -nt $D/game_a ]] || cp -p $D/game_a $D/game_b
A_TOUCH=${A_TOUCH:-"675-690:400,620;1150-1165:724,474;1700-1715:240,256;2000-2015:1288,348;2400-2415:120,300;2800-2815:1840,520;4800-4815:1848,500"}
B_TOUCH=${B_TOUCH:-"675-690:400,620;1150-1165:724,474;1700-1715:1680,256;2000-2015:1680,256;2300-2315:1680,256;2600-2615:960,460;2850-2865:740,660"}
run() {   # run <bin> <frames> <account id> <online name> <touch> [buttons: from-to:hexmask,...]
  rm -f $D/test_$1/shot_*.ppm
  cd $R && RECOMP_BIN=$1 SHOT_EVERY=150 TEST_SAVES=$SAVES TEST_TOUCH=$5 TEST_INPUT=${6:-} TEST_FRAMES=$2 \
    EXTRA_ENV="RECOMP_GFX=gpu RECOMP_ONLINE=1 RECOMP_M2_LOG=1 RECOMP_NP_LOG=1 RECOMP_ONLINE_ID=$4 RECOMP_NP_ACCOUNT_ID=$3 $EXTRA" \
    TIMEOUT=900 ./vitarecomp test $TITLE > $OUT/u13duo_$1.out 2>&1
}
rm -f $D/test_a/test.log $D/test_b/test.log
NA=${NAME_A:-u13_a}; NB=${NAME_B:-u13_b}
run a ${A_FRAMES:-5400} a11ce0013 $NA "$A_TOUCH" "${A_INPUT:-}" &
for i in $(seq 1 60); do pgrep -x game_a >/dev/null && break; sleep 1; done
sleep ${B_DELAY:-30}
run b ${B_FRAMES:-4200} b0b000013 $NB "$B_TOUCH" "${B_INPUT:-}" &
joined=0
for i in $(seq 1 600); do
  sleep 1
  pgrep -x game_b >/dev/null || [ $i -lt 90 ] || break
  if [ $joined = 0 ] && grep -aq "ntv_$NB joined room" $D/test_a/test.log 2>/dev/null; then joined=$i; echo "B joined A's room"; fi
  [ $joined != 0 ] && [ $((i - joined)) -ge ${VERDICT_S:-60} ] && { echo "verdict: B still in A's room after ${VERDICT_S:-60} s"; break; }
done
[ $joined = 0 ] && echo "verdict: B never joined A's room"
sleep 3; pkill -x game_b; pkill -x game_a
wait
for b in a b; do echo "== $b"; grep -a "    m2: \|neither recompiled" $D/test_$b/test.log | grep -v "keepalive\|connection\|matching list\|discovery for\|logged in\|heartbeat\|stun" | head -20; done
