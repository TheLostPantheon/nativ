#!/bin/zsh
# The same vitarecomp commands on another machine (a gate runner with more cores, a Linux/Vulkan
# build), over ssh: the remote checkout at this commit (through GitHub), each named game's inputs rsynced first.
# The game dumps only ever move between machines you own.
#
#   remote.sh <host> setup                       once: the recompiler's python packages in a venv there
#   remote.sh <host> sync      <title> ...       code + each game's inputs (elf, analysis, game files, references)
#   remote.sh <host> recompile <title> ...       sync, then the game's C regenerated there (first time, or after analysis changes)
#   remote.sh <host> check     <title> ...       sync, then the regression gate there; PASS/FAIL lines come back as they land
#   remote.sh <host> <cmd>     <title> [args]    sync that game, then any other vitarecomp command (build, test, capture ...)
#
# REMOTE_DIR: the checkout under the remote home (default project-nativ, a clone of this repository, kept in step through
# GitHub: see sync_code; dev/local.env's NATIV_REMOTE_DIR when that file sets one). The remote needs zsh, cc, SDL3 (pkg-config sdl3), python3, rsync; tests run headless,
# no display needed.
set -e
R=${0:A:h}; ROOT=${R:h}
HOST=$1; cmd=$2; [[ -z $HOST || -z $cmd ]] && { sed -n '2,14p' $0; exit 1; }
shift 2
[[ -f $ROOT/dev/local.env ]] && source $ROOT/dev/local.env   # (this machine's own defaults: git ignores it)
DEST=${REMOTE_DIR:-${NATIV_REMOTE_DIR:-project-nativ}}
remote() { ssh $HOST "cd $DEST && exec zsh -c '$*'"; }
sync_code() {   # the code goes through GitHub: the remote checkout is a working copy of its own since GitHub became the hub
	# (2026-10-25), and a copy over it would take its changes and block its pulls. So: clean there and at this checkout's
	# clean commit (push here, `git pull` there), or REMOTE_RSYNC=1 copies the tracked tree over it anyway (its changes lost)
	if [[ -n ${REMOTE_RSYNC:-} ]]; then (cd $ROOT && git ls-files -z | rsync -a --from0 --files-from=- ./ $HOST:$DEST/); return; fi
	local here=$(git -C $ROOT rev-parse HEAD) there=$(ssh $HOST "cd $DEST && git rev-parse HEAD && git status --porcelain --untracked-files=no")
	[[ $there == $here && -z $(git -C $ROOT status --porcelain --untracked-files=no) ]] && return
	echo "remote.sh: $HOST:$DEST isn't clean at this checkout's clean commit ${here[1,10]}: commit and push here, pull there, then again"; exit 1
}
sync_game() {   # what recompile/build/test read, one directory at a time
	# (never `rsync -R --delete` over part of a game's tree: with --relative the parent is an implied
	# directory of the transfer, and --delete removed everything not listed under it -- the remote's whole
	# recomp folder with its game files and saves, 2026-10-15). The game's files and saves (recomp/fs) are
	# never mirrored: app0 may link to another title's data, and a save is the player's -- copy them by hand.
	local t=$1 D=$ROOT/dump/decrypted RD=$DEST/dump/decrypted
	ssh $HOST "mkdir -p $RD/$t/recomp $RD/sys"
	for d in $t/elf $t/analysis us sys/external seeds maps; do
		[[ -d $D/$d ]] && rsync -a --delete --exclude='*.vfc' $D/$d/ $HOST:$RD/$d/
	done
	local refs=($D/$t/recomp/ref_*.ppm(N)); (( $#refs )) && rsync -a $refs $HOST:$RD/$t/recomp/
}
case $cmd in
setup)
	sync_code
	remote "python3 -m venv .venv && .venv/bin/pip install -q capstone==5.0.9 pyelftools==0.33 unicorn==2.1.4 && echo setup ok" ;;
sync)
	sync_code; for t in $@; do sync_game $t; done ;;
check)
	sync_code; for t in $@; do sync_game $t; done
	# detached on the remote (an ssh session dropping mid-run killed the gate once), its output
	# polled back: the full log lands in build/remote_check.log, PASS/FAIL lines here
	ssh $HOST "setsid nohup zsh $DEST/dev/remote_check_start.sh $* > /dev/null 2>&1 < /dev/null &"   # its own session: the login shell's exit must not take it down
	while true; do
		sleep 20
		mkdir -p $ROOT/build; ssh $HOST "cat $DEST/build/check.log" > $ROOT/build/remote_check.log 2>/dev/null || true
		grep -q REMOTE-CHECK-DONE $ROOT/build/remote_check.log && break
	done
	grep -E "PASS|FAIL|SKIP|error" $ROOT/build/remote_check.log || { echo "no verdict lines; the end of remote_check.log:"; tail -5 $ROOT/build/remote_check.log; } ;;
*)
	sync_code; sync_game $1
	remote "./vitarecomp $cmd $*" ;;
esac
