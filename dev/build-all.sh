#!/usr/bin/env zsh
# This machine's play builds brought up to date: dev/build-all.sh [title ...]
#
# 1. On a clean main, pulls (fast-forward only); a topic branch or local changes are left as they are.
# 2. Builds the launcher (launcher/nativ, swapped in whole, so a running launcher keeps its old file).
# 3. Each title (default: every title in the library with a pack; copies like PCSA00107_np aside) made ready, as the
#    launcher's Play does (vitarecomp ready): its code regenerated when recompiler/ or its pack.toml changed since the
#    last regeneration (uncommitted edits count too), then built when the tools did.
# Refuses while a game runs (relinking a running binary can take it down), and skips a title whose game started since.
# "== " lines are the summary.
set -u
ROOT=${0:A:h:h}; cd $ROOT
if pgrep -x game_host >/dev/null || pgrep -x sk_host >/dev/null; then echo "== a game is running: close it first"; exit 1; fi
echo "== start $(date +%T) on $(git branch --show-current) at $(git rev-parse --short HEAD)"
if [[ $(git branch --show-current) == main && -z $(git status --porcelain --untracked-files=no) ]]; then
	git pull --ff-only -q && echo "== pulled: now at $(git rev-parse --short HEAD)" || echo "== pull failed: building what is here"
else
	echo "== not a clean main: no pull"
fi
hub=launcher/nativ.new.$$
if zsh launcher/build.sh $hub && mv -f $hub launcher/nativ; then echo "== launcher ok"; else rm -f $hub; echo "== launcher FAILED"; fi
D=${ATIV_DUMP:-$ROOT/dump}/decrypted
titles=($@)
if (( ! $#titles )); then
	for d in $D/*/recomp(N/); do
		t=${d:h:t}
		[[ $t =~ '^[A-Z]{4}[0-9]{5}(_[0-9]+)?$' && -f packs/${t%%_*}/pack.toml ]] && titles+=($t)
	done
fi
failed=0
for t in $titles; do
	# each title checked again right before its build: a game started after this script did was relinked under the player
	# (Killzone, 6 s in, 2026-10-26); a running one is skipped, the rest still build
	if pgrep -f "/$t/recomp/(game|sk)_host" >/dev/null; then echo "== $t skipped: it is running (build it after closing it)"; failed=1; continue; fi
	if ./vitarecomp ready $t; then echo "== $t ok $(date +%T)"; else echo "== $t FAILED $(date +%T)"; failed=1; fi
done
echo "== done $(date +%T)"
exit $failed
