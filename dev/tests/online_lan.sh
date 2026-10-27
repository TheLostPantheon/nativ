#!/bin/zsh
# online_lan.sh: Killzone on this machine against Killzone on another machine of the LAN, over Nativ's P2P online
# (ONLINE.md, milestones 2 and 3). A hosts (Multiplayer > Public > Mercenary Warfare: nothing to find yet), B starts
# B_DELAY seconds later (default 20) and should find A's room and join it; both stop at the verdict: B hosting a room of
# its own (it gave up on A's), B still in A's room VERDICT_S seconds after joining (default 90), or B ending.
# By default this machine is A and REMOTE is B; SWAP=1: REMOTE hosts (A) and this machine joins (B).
# REMOTE is an ssh host (default: dev/local.env's NATIV_REMOTE) with a checkout at REMOTE_DIR under its home (default
# dev/local.env's NATIV_REMOTE_DIR, else project-nativ), the
# builds game_a and game_b and the save under recordings/l2 (the login server: each copy's own stand-in; KZSERVER as in online_duo.sh);
# it runs headless (SDL's offscreen driver).
# Env: EXTRA (both), EXTRA_A, EXTRA_B, A_FRAMES, B_FRAMES, B_DELAY, VERDICT_S, REMOTE, REMOTE_DIR, SWAP.
R=${0:A:h:h:h}; DUMP=${ATIV_DUMP:-$R/dump}; TITLE=${TITLE:-PCSA00107_np}
D=$DUMP/decrypted/$TITLE/recomp; OUT=${OUT:-$D}
SAVES=${TEST_SAVES:-$DUMP/decrypted/PCSA00107/recomp/recordings/l2/savedata0}
[[ -f $R/dev/local.env ]] && source $R/dev/local.env   # (this machine's own defaults: git ignores it)
REMOTE=${REMOTE:-$NATIV_REMOTE}; RD=${REMOTE_DIR:-${NATIV_REMOTE_DIR:-project-nativ}}   # (relative to the remote home)
[[ -n $REMOTE ]] || { echo "online_lan.sh: REMOTE=<ssh host> (or NATIV_REMOTE in dev/local.env)"; exit 1; }
TOUCH="650-660:1564,830;800-810:500,760;1000-1010:500,580;1200-1210:1184,736;2050-2060:500,400;2300-2310:500,600;2600-2610:350,600"
ENVS="RECOMP_GFX=gpu RECOMP_CLOCK=fixed RECOMP_SKIP_MOVIES=1 RECOMP_ONLINE=1 RECOMP_NP_TICKET_V3=1 RECOMP_NET_SERVERS=kzv.online.scee.com,beta.kzv.online.scee.com ${KZSERVER:+RECOMP_NET_REDIRECT=kzv.online.scee.com=$KZSERVER} RECOMP_M2_LOG=1 RECOMP_SHOT_FROM=3000 RECOMP_NP_LOG=1"
if pgrep -f "[_]host game.img" >/dev/null; then echo "a game is running here: not now"; exit 1; fi
if ssh $REMOTE "pgrep -f '[_]host game.img'" >/dev/null; then echo "a game is running on $REMOTE: not now"; exit 1; fi
[ -n "$SWAP" ] && { HERE=b; THERE=a; } || { HERE=a; THERE=b; }
run_here() {   # <bin> <frames> <account> <env>
  (cd $R && RECOMP_BIN=$1 SHOT_EVERY=300 TEST_SAVES=$SAVES TEST_TOUCH="$TOUCH" TEST_FRAMES=$2 \
    EXTRA_ENV="$ENVS RECOMP_ONLINE_ID=duo_$1 RECOMP_NP_ACCOUNT_ID=$3 $4" TIMEOUT=900 ./vitarecomp test $TITLE > $OUT/duo_$1.out 2>&1)
}
run_there() {
  ssh $REMOTE bash -s > $OUT/duo_$1_remote.out 2>&1 <<EOF
cd $RD && SDL_VIDEODRIVER=offscreen RECOMP_BIN=$1 SHOT_EVERY=300 TEST_SAVES=$RD/dump/decrypted/PCSA00107/recomp/recordings/l2/savedata0 \
  TEST_TOUCH="$TOUCH" TEST_FRAMES=$2 EXTRA_ENV="$ENVS RECOMP_ONLINE_ID=duo_$1 RECOMP_NP_ACCOUNT_ID=$3 $4" TIMEOUT=900 ./vitarecomp test $TITLE
EOF
}
has() {   # <bin> <pattern>: its test log has that line
  if [ $1 = $HERE ]; then grep -aq "$2" $D/test_$1/test.log 2>/dev/null; else ssh $REMOTE "grep -aq '$2' $RD/dump/decrypted/$TITLE/recomp/test_$1/test.log 2>/dev/null"; fi
}
alive() { if [ $1 = $HERE ]; then pgrep -x game_$1 >/dev/null; else ssh $REMOTE "pgrep -x game_$1" >/dev/null; fi; }
start() { if [ $1 = $HERE ]; then run_here "$@" & else run_there "$@" & fi; }
rm -f $D/test_a/test.log $D/test_b/test.log; ssh $REMOTE "rm -f $RD/dump/decrypted/$TITLE/recomp/test_a/test.log $RD/dump/decrypted/$TITLE/recomp/test_b/test.log"
start a ${A_FRAMES:-7500} a11ce0001 "$EXTRA $EXTRA_A"
sleep ${B_DELAY:-20}
start b ${B_FRAMES:-4500} b0b000002 "$EXTRA $EXTRA_B"
joined=0
for i in $(seq 1 200); do
  sleep 3
  if [ $joined = 0 ] && has a "ntv_duo_b joined room"; then joined=$i; echo "B joined A's room"; fi
  [ $joined != 0 ] && [ $(( (i - joined) * 3 )) -ge ${VERDICT_S:-90} ] && { echo "verdict: B still in A's room after ${VERDICT_S:-90} s"; break; }
  [ $((i % 3)) = 0 ] || continue
  has b "created by ntv_duo_b" && { echo "verdict: B hosted its own room"; break; }
  [ $i -gt 30 ] && ! alive b && { echo "B ended"; break; }
done
sleep 3; ssh $REMOTE "pkill -x game_a; pkill -x game_b"; pkill -x game_a; pkill -x game_b
wait
for b in a b; do
  echo "== $b ($([ $b = $HERE ] && echo here || echo $REMOTE))"
  if [ $b = $HERE ]; then grep -a "    m2: \|unimplemented\|neither recompiled" $D/test_$b/test.log | grep -v "keepalive\|connection\|matching list\|discovery for\|logged in\|m2: stun" | head -20
  else ssh $REMOTE "grep -a '    m2: \|unimplemented\|neither recompiled' $RD/dump/decrypted/$TITLE/recomp/test_$b/test.log | grep -v 'keepalive\|connection\|matching list\|discovery for\|logged in\|m2: stun' | head -20"; fi
done
