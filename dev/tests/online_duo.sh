#!/bin/zsh
# online_duo.sh: two Killzone copies on this machine play each other over Nativ's P2P online (ONLINE.md, milestone 1).
# A goes to Multiplayer > Public > Mercenary Warfare and hosts; B starts B_DELAY seconds later (default 20: its search,
# ~2600 frames in, lands after A's room opens near A's frame 2750), finds A's room and joins. Both stop at the verdict:
# B hosting a room of its own (it gave up on A's), B still in A's room VERDICT_S seconds after joining (default 90),
# or the frame limits. The shots (every 300 frames from 3000) are in <dump>/decrypted/<title>/recomp/test_a and test_b.
#
# Needs: a build of each copy (RECOMP_BIN=a and =b ./vitarecomp build $TITLE) and a save that reaches the Multiplayer menu
# (TEST_SAVES, default the l2 recording's). The game's login server is its pack's stand-in in each copy (server.c);
# KZSERVER=127.0.0.1:8443 sends them to servers/kzserver instead. Env: TITLE (default PCSA00107_np: a folder whose recomp/ holds
# the two binaries), NAME_A / NAME_B (their online names after ntv_, default duo_a / duo_b: the same for both tests a
# name clash), EXTRA (both copies' env), EXTRA_B, RUNNER_B, A_FRAMES, B_FRAMES, ATTACH_A / ATTACH_B (a script
# run with the copy's pid, e.g. an lldb attach: launching under lldb loses multicast on macOS).
R=${0:A:h:h:h}; DUMP=${ATIV_DUMP:-$R/dump}; TITLE=${TITLE:-PCSA00107_np}
D=$DUMP/decrypted/$TITLE/recomp; OUT=${OUT:-$D}
SAVES=${TEST_SAVES:-$DUMP/decrypted/PCSA00107/recomp/recordings/l2/savedata0}
if pgrep -f "[_]host game.img" >/dev/null; then echo "a game is running: not now"; exit 1; fi
run() {   # run <bin> <frames> <account id> <online name>
  cd $R && RECOMP_BIN=$1 SHOT_EVERY=300 TEST_SAVES=$SAVES \
    TEST_TOUCH="650-660:1564,830;800-810:500,760;1000-1010:500,580;1200-1210:1184,736;2050-2060:500,400;2300-2310:500,600;2600-2610:350,600" TEST_FRAMES=$2 \
    EXTRA_ENV="RECOMP_GFX=gpu RECOMP_CLOCK=fixed RECOMP_SKIP_MOVIES=1 RECOMP_ONLINE=1 RECOMP_NP_TICKET_V3=1 RECOMP_NET_SERVERS=kzv.online.scee.com,beta.kzv.online.scee.com ${KZSERVER:+RECOMP_NET_REDIRECT=kzv.online.scee.com=$KZSERVER} RECOMP_M2_LOG=1 RECOMP_SHOT_FROM=3000 RECOMP_NP_LOG=1 RECOMP_ONLINE_ID=$4 RECOMP_NP_ACCOUNT_ID=$3 $EXTRA" \
    TIMEOUT=900 ./vitarecomp test $TITLE > $OUT/duo_$1.out 2>&1
}
rm -f $D/test_a/test.log $D/test_b/test.log
NA=${NAME_A:-duo_a}; NB=${NAME_B:-duo_b}
run a ${A_FRAMES:-7500} a11ce0001 $NA &
for i in $(seq 1 60); do pgrep -x game_a >/dev/null && break; sleep 1; done
[ -n "$ATTACH_A" ] && { $ATTACH_A $(pgrep -x game_a) & }
sleep ${B_DELAY:-20}
EXTRA="$EXTRA $EXTRA_B" RECOMP_RUNNER="$RUNNER_B" run b ${B_FRAMES:-6500} b0b000002 $NB &
[ -n "$ATTACH_B" ] && { for i in $(seq 1 60); do pgrep -x game_b >/dev/null && break; sleep 1; done; sleep 20; $ATTACH_B $(pgrep -x game_b) & }
joined=0
for i in $(seq 1 600); do
  sleep 1
  pgrep -x game_b >/dev/null || [ $i -lt 90 ] || break
  grep -aq "created by ntv_$NB" $D/test_b/test.log 2>/dev/null && { echo "verdict: B hosted its own room"; break; }
  if [ $joined = 0 ] && grep -aq "ntv_$NB joined room" $D/test_a/test.log 2>/dev/null; then joined=$i; echo "B joined A's room"; fi
  [ $joined != 0 ] && [ $((i - joined)) -ge ${VERDICT_S:-90} ] && { echo "verdict: B still in A's room after ${VERDICT_S:-90} s"; break; }
done
sleep 3; pkill -x game_b; pkill -x game_a
wait
for b in a b; do echo "== $b"; grep -a "    m2: \|unimplemented\|neither recompiled" $D/test_$b/test.log | grep -v "keepalive\|connection\|matching list\|discovery for\|logged in" | head -20; done
