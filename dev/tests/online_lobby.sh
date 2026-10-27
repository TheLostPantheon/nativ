#!/bin/zsh
# online_lobby.sh: N Killzone copies on this machine fill one Mercenary Warfare room over Nativ's P2P online. Copy 1 hosts
# (online_duo.sh's A), copies 2..N join (its B), STAGGER seconds apart: the lobby's deployment countdown runs ~60 s from
# the second player, so the rest join inside it. Then, with YANK, the host is taken away and the others are watched:
#   YANK=lobby   once every joiner is in the room (before the countdown ends)
#   YANK=match   at the host's frame YANK_FRAME (default 6600: ~45 s into the match)
#   YANK_WHO=<bin> who is taken away (default a, the host; d: a member)
#   LATE=1 one more copy starts right after the yank and joins the room as it is then (after a takeover)
#   YANK2_AFTER=<s> that many seconds after the yank, the copy serving the room then (the takeover's) is killed too: a
#   second takeover in one match
#   YANK_HOW=kill (default: the process dies, the OS closes its sockets) | stop (SIGSTOP: it goes silent, sockets open,
#   like a dropped network; killed at the end) | blip (stopped, then woken BLIP s later, default 20: a network back)
# Report: who joined (the host's room log), what each copy logged after the yank (m2/np/net lines), its last shot
# (test_<bin>/). Copies: binaries game_a (the host), game_b.. (copied from game_a when older: one build serves all).
#
# Needs what online_duo.sh needs (the builds: RECOMP_BIN=a ./vitarecomp build PCSA00107_np; the l2 save; KZSERVER as
# there). Env: N (default 4, at most 8), STAGGER (8), YANK, YANK_FRAME, YANK_HOW, WATCH (seconds
# after the yank, or after the last join without one, default 90), EXTRA (every copy's env), TITLE, SHOTS (frames between
# shots, default 300; with EXTRA="RECOMP_SHOT_FROM=<frame>" a close look at the moments around a yank).
# REMOTE=<ssh host> (online_lan.sh's: a checkout at REMOTE_DIR, as there, with the online build, its save and its own login
# server): the first joiner (b) runs there, headless, instead of here -- it joins first, so it is next in line to serve the
# room. Its log stays there (the report fetches its takeover lines). REMOTE_EXTRA: its own env (a firewall that drops what
# it didn't ask for: RECOMP_ONLINE_PEERS naming this machine's copies, host:overlay port each).
R=${0:A:h:h:h}; DUMP=${ATIV_DUMP:-$R/dump}; TITLE=${TITLE:-PCSA00107_np}
D=$DUMP/decrypted/$TITLE/recomp
SAVES=${TEST_SAVES:-$DUMP/decrypted/PCSA00107/recomp/recordings/l2/savedata0}
N=${N:-4}; (( N >= 2 && N + ${LATE:-0} <= 8 )) || { echo "N (with LATE): 2 to 8"; exit 1; }
J_FRAMES=${J_FRAMES:-$([[ -n $LATE ]] && echo 14000 || echo 9000)}   # (with LATE the joiners outlast the late one's join and a while after: at 9000 the first joiner, serving the room by then, ended its run seconds after the late join)
if pgrep -f "[_]host game.img" >/dev/null; then echo "a game is running: not now"; exit 1; fi
ALL=(a b c d e f g h); BINS=(${ALL[1,$N]}); [[ -n $LATE ]] && BINS+=(${ALL[$((N + 1))]})
for b in ${BINS[2,-1]}; do [[ $D/game_$b -nt $D/game_a ]] || cp -p $D/game_a $D/game_$b; done
[[ -f $R/dev/local.env ]] && source $R/dev/local.env   # (NATIV_REMOTE_DIR; REMOTE stays as given: none = every copy here)
RD=${REMOTE_DIR:-${NATIV_REMOTE_DIR:-project-nativ}}   # (relative to the remote home)
[[ -n $REMOTE ]] && ssh $REMOTE "pgrep -f '[_]host game.img'" >/dev/null && { echo "a game is running on $REMOTE: not now"; exit 1; }
HOST_TOUCH="650-660:1564,830;800-810:500,760;1000-1010:500,580;1200-1210:1184,736;2050-2060:500,400;2300-2310:500,600;2600-2610:350,600"
run() {   # run <bin> <frames> <account id> <online name>
  cd $R && RECOMP_BIN=$1 SHOT_EVERY=${SHOTS:-300} TEST_SAVES=$SAVES TEST_TOUCH=$HOST_TOUCH TEST_FRAMES=$2 \
    EXTRA_ENV="RECOMP_GFX=gpu RECOMP_CLOCK=fixed RECOMP_SKIP_MOVIES=1 RECOMP_ONLINE=1 RECOMP_NP_TICKET_V3=1 RECOMP_NET_SERVERS=kzv.online.scee.com,beta.kzv.online.scee.com ${KZSERVER:+RECOMP_NET_REDIRECT=kzv.online.scee.com=$KZSERVER} RECOMP_M2_LOG=1 RECOMP_SHOT_FROM=3000 RECOMP_NP_LOG=1 RECOMP_ONLINE_ID=$4 RECOMP_NP_ACCOUNT_ID=$3 $EXTRA" \
    TIMEOUT=1200 ./vitarecomp test $TITLE > $D/lobby_$1.out 2>&1
}
run_remote() {   # the same, on $REMOTE
  ssh $REMOTE bash -s > $D/lobby_$1_remote.out 2>&1 <<EOF
cd $RD && rm -f dump/decrypted/$TITLE/recomp/test_$1/test.log && SDL_VIDEODRIVER=offscreen RECOMP_BIN=$1 SHOT_EVERY=300 TEST_SAVES=$RD/dump/decrypted/PCSA00107/recomp/recordings/l2/savedata0 TEST_TOUCH="$HOST_TOUCH" TEST_FRAMES=$2 EXTRA_ENV="RECOMP_GFX=gpu RECOMP_CLOCK=fixed RECOMP_SKIP_MOVIES=1 RECOMP_ONLINE=1 RECOMP_NP_TICKET_V3=1 RECOMP_NET_SERVERS=kzv.online.scee.com,beta.kzv.online.scee.com ${KZSERVER:+RECOMP_NET_REDIRECT=kzv.online.scee.com=$KZSERVER} RECOMP_M2_LOG=1 RECOMP_SHOT_FROM=3000 RECOMP_NP_LOG=1 RECOMP_ONLINE_ID=$4 RECOMP_NP_ACCOUNT_ID=$3 $EXTRA $REMOTE_EXTRA" TIMEOUT=1200 ./vitarecomp test $TITLE
EOF
}
for b in $BINS; do rm -f $D/test_$b/test.log; done
run a ${A_FRAMES:-10500} a11ce0001 lob_a &
for i in $(seq 1 60); do pgrep -x game_a >/dev/null && break; sleep 1; done
sleep 20
k=2; for b in ${BINS[2,$N]}; do if [[ -n $REMOTE && $b == b ]]; then run_remote $b $J_FRAMES 10b0000$k lob_$b & else run $b $J_FRAMES 10b0000$k lob_$b & fi; k=$((k + 1)); sleep ${STAGGER:-8}; done
log() { cat $D/test_$1/test.log 2>/dev/null | tr -d '\0'; }
members() { log a | grep -a -o "joined room [0-9/]* as 0x[0-9a-f]* ([0-9]* members)" | tail -1 | grep -o "[0-9]* members"; }
want=$((N - 1)); joined=0; hosted=; t0=$SECONDS
while (( SECONDS - t0 < 600 )); do
  sleep 2
  pgrep -x game_a >/dev/null || { echo "the host ended"; break; }
  j=$(log a | grep -a -o "ntv_lob_. joined room" | sort -u | wc -l | tr -d " "); (( j > joined )) && { joined=$j; echo "$(( SECONDS - t0 )) s: $joined of $want joined ($(members))"; }
  for b in ${BINS[2,$N]}; do [[ $hosted == *$b* ]] && continue; log $b | grep -aq "created by ntv_lob_$b" && { echo "  $b hosted a room of its own"; hosted+=$b; }; done
  (( joined >= want )) && break
done
ylines=()
if [[ -n $YANK ]]; then
  if [[ $YANK == match ]]; then
    f=${YANK_FRAME:-6600}; echo "waiting for the host's frame $f"
    for i in $(seq 1 400); do log a | grep -aq "    frame $f:" && break; pgrep -x game_a >/dev/null || break; sleep 1; done
  else sleep 3; fi
  for b in $BINS; do ylines+=($(log $b | wc -l)); done
  W=${YANK_WHO:-a}; pid=$(pgrep -x game_$W)
  [[ -z $pid ]] && { echo "$W isn't running: no yank"; YANK=; }
  [[ -n $YANK ]] && case ${YANK_HOW:-kill} in stop) kill -STOP $pid;; blip) kill -STOP $pid; ( sleep ${BLIP:-20}; kill -CONT $pid; echo "  woke $W" ) &;; *) kill -9 $pid;; esac
  echo "yanked $W ($YANK, ${YANK_HOW:-kill}) at $(( SECONDS - t0 )) s; watching ${WATCH:-90} s"
  if [[ -n $LATE ]]; then
    L=${BINS[-1]}; run $L $J_FRAMES 1a7e00099 lob_$L & echo "  $L starts, to join late"
    lt=$SECONDS; while (( SECONDS - lt < ${WATCH:-90} + 60 )); do
      sleep 2; for b in ${BINS[2,$N]}; do log $b | grep -aq "ntv_lob_$L joined room\|ntv_lob_$L is back" && { echo "  $L joined at $(( SECONDS - t0 )) s (seen by $b)"; break 2; }; done
      log $L | grep -aq "created by ntv_lob_$L" && { echo "  $L hosted a room of its own"; break; }
    done
  fi
  if [[ -n $YANK2_AFTER ]]; then
    sleep $YANK2_AFTER; S2=
    [[ -n $REMOTE ]] && ssh $REMOTE "grep -aq 'this copy serves it now' $RD/dump/decrypted/$TITLE/recomp/test_b/test.log" && S2=b
    [[ -z $S2 ]] && for b in ${BINS[2,-1]}; do log $b | grep -aq "this copy serves it now" && { S2=$b; break; }; done
    if [[ -z $S2 ]]; then echo "no copy took the room over: no second yank"
    elif [[ -n $REMOTE && $S2 == b ]]; then ssh $REMOTE 'pkill -9 -x game_b'; echo "yanked b, serving the room, on $REMOTE at $(( SECONDS - t0 )) s"
    else pkill -9 -x game_$S2; echo "yanked $S2, serving the room, at $(( SECONDS - t0 )) s"; fi
  fi
  sleep ${WATCH:-90}
else sleep ${WATCH:-90}; fi   # (no yank: the countdown and the match's start, in the shots)
sleep 2; for b in $BINS; do pkill -9 -x game_$b; done; [[ -n $REMOTE ]] && ssh $REMOTE 'pkill -9 -x game_b'
wait 2>/dev/null
echo "== the room: $(members)"
i=1; for b in $BINS; do
  echo "== $b ($(log $b | grep -a -o '    frame [0-9]*:' | tail -1 | tr -d ' :'))"
  if (( ${#ylines} )); then log $b | tail -n +$((ylines[i] + 1)) | grep -a -E "    (m2|np|net|overlay|disc):" | grep -a -v "keepalive\|stun:\|discovery\|matching list" | head -${LINES_AFTER:-14} | cut -c1-170
  else log $b | grep -a -E "    m2: " | grep -a -v "keepalive\|stun:\|discovery\|matching list\|connection" | tail -4 | cut -c1-170; fi
  i=$((i + 1))
done
[[ -n $REMOTE ]] && { echo "== b on $REMOTE"; ssh $REMOTE "grep -a -E 'm2: room|overlay: linked|went|resumed|taken over|back in|lets it go' $RD/dump/decrypted/$TITLE/recomp/test_b/test.log | grep -v 'room connection' | head -16" | cut -c1-170; }
