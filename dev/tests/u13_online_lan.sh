#!/bin/zsh
# u13_online_lan.sh: Unit 13 co-op between this machine and another one (docs/online.md), the two-machine form of
# u13_online_duo.sh with its taps: A hosts mission 01 and waits on the co-op loadout, B starts B_DELAY seconds later
# (default 30), finds A's game with REFRESH and joins, and A's DEPLOY (tapped from frame 4800 every 600 frames, in case
# B is late) takes both into the mission. The verdict: B joined A's room, then the frame limits; the shots show the
# mission (A's here or on REMOTE, every 150 frames from 3000).
# By default this machine is A and REMOTE is B; SWAP=1: REMOTE hosts (A) and this machine joins (B).
# REMOTE is an ssh host (default: dev/local.env's NATIV_REMOTE) with a checkout at REMOTE_DIR under its home (default
# dev/local.env's NATIV_REMOTE_DIR, else project-nativ) and the build game_a or game_b there (RECOMP_BIN=a ./vitarecomp
# build PCSA00014; this script copies it to game_b when that copy is B); it runs headless (SDL's offscreen driver).
# Both copies play with this machine's Unit 13 save (TEST_SAVES), copied to REMOTE's /tmp for its copy.
# Env: EXTRA (both), EXTRA_A, EXTRA_B, A_FRAMES, B_FRAMES, B_DELAY, VERDICT_S, REMOTE, REMOTE_DIR, SWAP, OVL=0 (the
# plain LAN path instead of the links between copies).
R=${0:A:h:h:h}; DUMP=${ATIV_DUMP:-$R/dump}; TITLE=${TITLE:-PCSA00014}
D=$DUMP/decrypted/$TITLE/recomp; OUT=${OUT:-$D}
SAVES=${TEST_SAVES:-$DUMP/decrypted/PCSA00014/recomp/fs/savedata0}
[[ -f $R/dev/local.env ]] && source $R/dev/local.env   # (this machine's own defaults: git ignores it)
REMOTE=${REMOTE:-$NATIV_REMOTE}; RD=${REMOTE_DIR:-${NATIV_REMOTE_DIR:-project-nativ}}   # (relative to the remote home)
[[ -n $REMOTE ]] || { echo "u13_online_lan.sh: REMOTE=<ssh host> (or NATIV_REMOTE in dev/local.env)"; exit 1; }
RDD=$RD/dump/decrypted/$TITLE/recomp
TOUCH_A="675-690:400,620;1150-1165:724,474;1700-1715:240,256;2000-2015:1288,348;2400-2415:120,300;2800-2815:1840,520;4800-4815:1848,500;5400-5415:1848,500;6000-6015:1848,500;6600-6615:1848,500"
TOUCH_B="675-690:400,620;1150-1165:724,474;1700-1715:1680,256;2000-2015:1680,256;2300-2315:1680,256;2600-2615:960,460;2850-2865:740,660"
ENVS="RECOMP_GFX=gpu RECOMP_ONLINE=1 RECOMP_ONLINE_OVERLAY=${OVL:-1} RECOMP_M2_LOG=1 RECOMP_NP_LOG=1 RECOMP_SHOT_FROM=3000"   # (the links between copies on, as in play: headless runs leave them off)
if pgrep -f "[_]host game.img" >/dev/null; then echo "a game is running here: not now"; exit 1; fi
if ssh $REMOTE "pgrep -f '[_]host game.img'" >/dev/null; then echo "a game is running on $REMOTE: not now"; exit 1; fi
[ -n "$SWAP" ] && { HERE=b; THERE=a; } || { HERE=a; THERE=b; }
[[ -x $D/game_$HERE || $HERE = b && -x $D/game_a ]] || { echo "build game_a here first: RECOMP_BIN=a ./vitarecomp build $TITLE"; exit 1; }
[[ $HERE = b && ! $D/game_b -nt $D/game_a ]] && cp -p $D/game_a $D/game_b
ssh $REMOTE bash -s <<EOF || { echo "build game_a on $REMOTE first: RECOMP_BIN=a ./vitarecomp build $TITLE"; exit 1; }
cd $RDD || exit 1
[ $THERE = b ] && [ -x game_a ] && [ ! game_b -nt game_a ] && cp -p game_a game_b
[ -x game_$THERE ]
EOF
ssh $REMOTE "rm -rf /tmp/nativ_u13_saves; mkdir -p /tmp/nativ_u13_saves" && scp -qr $SAVES $REMOTE:/tmp/nativ_u13_saves/ || exit 1
touch_of() { [ $1 = a ] && echo $TOUCH_A || echo $TOUCH_B; }
run_here() {   # <bin> <frames> <account> <env>
  (cd $R && RECOMP_BIN=$1 SHOT_EVERY=150 TEST_SAVES=$SAVES TEST_TOUCH="$(touch_of $1)" TEST_FRAMES=$2 \
    EXTRA_ENV="$ENVS RECOMP_ONLINE_ID=u13_$1 RECOMP_NP_ACCOUNT_ID=$3 $4" TIMEOUT=1200 ./vitarecomp test $TITLE > $OUT/u13lan_$1.out 2>&1)
}
run_there() {
  ssh $REMOTE bash -s > $OUT/u13lan_$1_remote.out 2>&1 <<EOF
cd $RD && SDL_VIDEODRIVER=offscreen RECOMP_BIN=$1 SHOT_EVERY=150 TEST_SAVES=/tmp/nativ_u13_saves/savedata0 \
  TEST_TOUCH="$(touch_of $1)" TEST_FRAMES=$2 EXTRA_ENV="$ENVS RECOMP_ONLINE_ID=u13_$1 RECOMP_NP_ACCOUNT_ID=$3 $4" TIMEOUT=1200 ./vitarecomp test $TITLE
EOF
}
has() {   # <bin> <pattern>: its test log has that line
  if [ $1 = $HERE ]; then grep -aq "$2" $D/test_$1/test.log 2>/dev/null; else ssh $REMOTE "grep -aq '$2' $RDD/test_$1/test.log 2>/dev/null"; fi
}
alive() { if [ $1 = $HERE ]; then pgrep -x game_$1 >/dev/null; else ssh $REMOTE "pgrep -x game_$1" >/dev/null; fi; }
start() { if [ $1 = $HERE ]; then run_here "$@" & else run_there "$@" & fi; }
rm -f $D/test_a/test.log $D/test_b/test.log; ssh $REMOTE "rm -f $RDD/test_a/test.log $RDD/test_b/test.log"
start a ${A_FRAMES:-7800} a11ce0013 "$EXTRA $EXTRA_A"
sleep ${B_DELAY:-30}
start b ${B_FRAMES:-6900} b0b000013 "$EXTRA $EXTRA_B"
joined=0
for i in $(seq 1 400); do
  sleep 3
  if [ $joined = 0 ] && has a "ntv_u13_b joined room"; then joined=$i; echo "B joined A's room"; fi
  [ $joined != 0 ] && [ $(( (i - joined) * 3 )) -ge ${VERDICT_S:-600} ] && { echo "verdict: B still in A's room after ${VERDICT_S:-600} s"; break; }
  [ $((i % 3)) = 0 ] || continue
  [ $i -gt 30 ] && ! alive a && ! alive b && { echo "both ended"; break; }
done
[ $joined = 0 ] && echo "verdict: B never joined A's room"
sleep 3; ssh $REMOTE "pkill -x game_a; pkill -x game_b"; pkill -x game_a; pkill -x game_b
wait
for b in a b; do
  echo "== $b ($([ $b = $HERE ] && echo here || echo $REMOTE))"
  if [ $b = $HERE ]; then grep -a "    m2: \|    net: \|neither recompiled" $D/test_$b/test.log | grep -v "keepalive\|connection\|matching list\|discovery for\|logged in\|m2: stun" | head -24
  else ssh $REMOTE "grep -a '    m2: \|    net: \|neither recompiled' $RDD/test_$b/test.log | grep -v 'keepalive\|connection\|matching list\|discovery for\|logged in\|m2: stun' | head -24"; fi
done
