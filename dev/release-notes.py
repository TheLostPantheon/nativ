#!/usr/bin/env python3
"""Draft a release's notes from what the repository already records, for the maintainer to finish and publish.

    dev/release-notes.py <tag> [--gate <log>] [--out <notes.md>]

- What's new: the pull requests merged since the previous tag (their squash commits' "(#N)"), with their titles.
- Fixed: the issues those pull requests closed.
- Tested: CI's result on the tag's commit, the regression gate's PASS / FAIL lines when --gate names its log
  (`./vitarecomp check ... > gate.log`), and a checklist of play tests to tick or strike. Nothing here claims a test
  the evidence doesn't show: what's unticked stays visibly unticked.
- Known issues: the player guide's Known issues section, the one place they're kept.
- Downloads: the files in build/release/<tag>/ with their sizes and SHA-256, and that no game is included.

Needs git and gh (signed in). The tag may be any commit-ish before it's tagged (HEAD, a branch) to draft early.
"""
import hashlib
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sh(*a, check=True):
    r = subprocess.run(a, cwd=ROOT, capture_output=True, text=True)
    if check and r.returncode: sys.exit(f"{' '.join(a)}: {r.stderr.strip()}")
    return r.stdout.strip()


def repo():
    url = sh("git", "remote", "get-url", "origin")
    m = re.search(r"github\.com[:/]([^/]+/[^/.]+)", url)
    return m.group(1) if m else sys.exit(f"origin isn't a GitHub repository: {url}")


def section(path, heading):
    """a markdown section's body, heading to the next heading of the same level"""
    text = open(path, encoding="utf-8").read()
    m = re.search(rf"^## {re.escape(heading)}\n(.*?)(?=^## |\Z)", text, re.M | re.S)
    return m.group(1).strip() if m else ""


def previous_tag(sha):
    """origin's newest vX.Y.Z tag behind sha: origin's, because a clone can hold stale tags of the same names"""
    best, best_n = "", -1
    for line in sh("git", "ls-remote", "--tags", "origin", "v*").splitlines():
        h, ref = line.split("\t")
        name = ref.removeprefix("refs/tags/")
        if name.endswith("^{}"): name = name[:-3]   # (an annotated tag's commit)
        if not re.fullmatch(r"v\d+\.\d+\.\d+", name) or h == sha: continue
        if subprocess.run(["git", "merge-base", "--is-ancestor", h, sha], cwd=ROOT, capture_output=True).returncode: continue
        n = int(sh("git", "rev-list", "--count", h))
        if n > best_n: best, best_n = h, n; best_name = name
    return best_name if best else ""


def main(argv):
    if not argv or argv[0].startswith("-"): sys.exit(__doc__)
    tag, gate, out = argv[0], "", ""
    i = 1
    while i < len(argv):
        if argv[i] == "--gate": gate = argv[i + 1]; i += 2
        elif argv[i] == "--out": out = argv[i + 1]; i += 2
        else: sys.exit(f"unknown argument {argv[i]}\n\n{__doc__}")
    R = repo()
    sha = sh("git", "rev-parse", f"{tag}^{{commit}}")
    prev = previous_tag(sha)
    prev_ref = f"refs/tags/{prev}"
    prev_sha = sh("git", "ls-remote", "origin", prev_ref + "^{}", check=False).split("\t")[0] or sh("git", "ls-remote", "origin", prev_ref).split("\t")[0] if prev else ""
    subjects = sh("git", "log", "--first-parent", "--format=%s", f"{prev_sha}..{sha}" if prev_sha else sha).splitlines()
    prs = [int(m.group(1)) for s in subjects for m in [re.search(r"\(#(\d+)\)\s*$", s)] if m]
    L = []
    title = tag if tag.startswith("v") else "the next release"
    L.append(f"Project Nativ {title}. Since {prev or 'the start'}: {len(subjects)} change{'s' if len(subjects) != 1 else ''}"
             f"{f', {len(prs)} pull request' + ('s' if len(prs) != 1 else '') if prs else ''}.\n")
    L.append("## What's new\n")
    fixed = {}
    for n in prs:
        d = json.loads(sh("gh", "pr", "view", str(n), "--repo", R, "--json", "title,closingIssuesReferences"))
        L.append(f"- {d['title']} (#{n})")
        for iss in d.get("closingIssuesReferences") or []: fixed[iss["number"]] = n
    others = [s for s in subjects if not re.search(r"\(#\d+\)\s*$", s)]
    for s in others: L.append(f"- {s}")
    if fixed:
        L.append("\n## Fixed\n")
        for num in sorted(fixed):
            t = json.loads(sh("gh", "issue", "view", str(num), "--repo", R, "--json", "title"))["title"]
            L.append(f"- #{num}: {t} (in #{fixed[num]})")
    L.append("\n## Tested\n")
    runs = sh("gh", "api", f"repos/{R}/commits/{sha}/check-runs", "--jq", '.check_runs[] | "\\(.name) \\(.conclusion // .status)"', check=False)
    if runs: L += [f"- [{'x' if r.endswith(' success') else ' '}] CI: {r}" for r in runs.splitlines()]
    else: L.append("- [ ] CI: no run found on this commit")
    if gate:
        lines = [l.strip() for l in open(gate, encoding="utf-8", errors="replace") if l.startswith(("PASS", "FAIL"))]
        L += [f"- [{'x' if l.startswith('PASS') else ' '}] Regression gate: {l}" for l in lines] or ["- [ ] Regression gate: no PASS or FAIL lines in " + gate]
    else: L.append("- [ ] Regression gate (`./vitarecomp check PCSE00640 PCSA00014 PCSA00029`; --gate <log> fills this in)")
    L += ["- [ ] Killzone: Mercenary played to a level (its fast test route)",
          "- [ ] Each download opened on its OS: macOS, Windows, Linux",
          "- (Strike out what wasn't run, and say so: an untested item stays visible.)"]
    known = section(os.path.join(ROOT, "docs", "player-guide.md"), "Known issues")
    L.append("\n## Known issues\n")
    L.append(known or "- (none listed in docs/player-guide.md)")
    L.append("\n## Downloads\n")
    rel = os.path.join(ROOT, "build", "release", tag)
    files = sorted(f for f in os.listdir(rel) if f.startswith("ProjectNativ-")) if os.path.isdir(rel) else []
    for f in files:
        p = os.path.join(rel, f); h = hashlib.sha256()
        with open(p, "rb") as fh:
            for b in iter(lambda: fh.read(1 << 20), b""): h.update(b)
        L.append(f"- `{f}`: {os.path.getsize(p) / 1e6:.0f} MB, SHA-256 `{h.hexdigest()}`")
    if not files: L.append(f"- (no downloads in build/release/{tag} yet: dev/release.sh makes them)")
    L.append("\nEach download is the whole install for its OS. No game, game data or firmware is included: you add your own "
             "copies, and the launcher compiles them on your machine. SHA256SUMS lists every download's checksum.")
    text = "\n".join(L) + "\n"
    if out: open(out, "w", encoding="utf-8").write(text); print(f"wrote {out}: finish the Tested list, then publish")
    else: sys.stdout.write(text)


if __name__ == "__main__":
    main(sys.argv[1:])
