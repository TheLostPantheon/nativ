#!/bin/bash
# make-public.sh <folder>: the public repository's first commit, made from what's checked out here. The tracked files of
# HEAD go into a new git repository in <folder> as one commit with no history (this repository's history names
# developers' machines and holds notes not meant for publishing), without what stays private:
#   CLAUDE.md, notes/        working notes for this repository's own development
# and README.md without its row for notes/. Nothing is pushed: review the folder, then add the remote and push yourself.
# Given the public repository's own checkout instead (a folder this made, with nothing uncommitted), its files are
# replaced by HEAD's as one more commit: how the public repository catches up before development moves to it.
#   NATIV_PUBLIC_AUTHOR="Name <email>"   the commit's author (needed: a public address, not a personal one)
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$H")
OUT=${1:?usage: NATIV_PUBLIC_AUTHOR="Name <email>" make-public.sh <folder>}
AUTHOR=${NATIV_PUBLIC_AUTHOR:?NATIV_PUBLIC_AUTHOR="Name <email>": the author of the public commit}
[[ -z "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]] || { echo "make-public.sh: uncommitted changes: it copies HEAD"; exit 1; }
if [[ -d $OUT/.git ]]; then
  [[ -z "$(git -C "$OUT" status --porcelain)" ]] || { echo "make-public.sh: $OUT has uncommitted changes"; exit 1; }
  find "$OUT" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
elif [[ -e $OUT ]]; then echo "make-public.sh: $OUT exists and isn't a repository this made: a new folder"; exit 1
else mkdir -p "$OUT"; git -C "$OUT" init -q -b main; fi
git -C "$ROOT" archive --format=tar HEAD -- . ':(exclude)CLAUDE.md' ':(exclude)notes' | tar -x -C "$OUT"
grep -v '^| `notes/` |' "$OUT/README.md" > "$OUT/README.md.new" && mv "$OUT/README.md.new" "$OUT/README.md"
ver=$(git -C "$ROOT" describe --tags --always); NAME=${AUTHOR% <*}; EMAIL=${AUTHOR##*<}; EMAIL=${EMAIL%>}
git -C "$OUT" add -A
[[ -n "$(git -C "$OUT" status --porcelain)" ]] || { echo "make-public.sh: $OUT already matches $ver"; exit 0; }
GIT_AUTHOR_NAME=$NAME GIT_AUTHOR_EMAIL=$EMAIL GIT_COMMITTER_NAME=$NAME GIT_COMMITTER_EMAIL=$EMAIL \
  git -C "$OUT" -c commit.gpgsign=false commit -q -m "Project Nativ $ver"
echo "public tree: $OUT ($(git -C "$OUT" ls-files | wc -l | tr -d ' ') files, commit $(git -C "$OUT" rev-parse --short HEAD) as $AUTHOR)"
echo "check it, then push: git -C $OUT push (the first time: git -C $OUT remote add origin <url> && git -C $OUT push -u origin main)"
