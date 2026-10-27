"""Compare pup.py's decrypted output against the console-decrypted oracle.

This is the PUP part's equivalent of compare_oracle.py (which proved
sce_self.decrypt_self() against a console's own dump): instead of
re-decrypting the already-extracted dump/encrypted files, it walks a
pup.py output tree (<outdir>/{os0,vs0}/.../<name>.elf, written by
pup.decrypt_tree_selfs()) and diffs each one against dump/decrypted/<rel>.elf
using the same byte-identical / segment-identical / differs classification
compare_oracle.py uses (imported from it, not re-implemented).

Unlike compare_oracle.py, an exact match here is NOT automatically expected:
the PUP this reads may be a different firmware version than the console dump
(see pup.py's own CLI output for which fw_version it decrypted), so this
reports differences precisely rather than treating every one as a failure --
a file that changed between firmware versions is expected to differ; a file
that's identical in both versions is expected to match exactly.

Usage: python3 -m vitadecrypt.tests.compare_pup PUP_OUTDIR [--dump-root PATH]
"""
from __future__ import annotations

import argparse
import os

from vitadecrypt.tests.compare_oracle import DEFAULT_DUMP_ROOT, compare_one

_PARTITIONS = ("os0", "vs0")


def find_pairs(pup_outdir: str, dump_root: str) -> list[tuple[str, str]]:
    """[(ours_elf_path, oracle_elf_path), ...] for every *.elf pup.py wrote
    that has a counterpart under dump/decrypted/ (same relative-path rule
    compare_oracle.py uses: strip the os0/vs0 prefix, the name already ends
    in ".elf" because pup.py names its output that way itself)."""
    dec_root = os.path.join(dump_root, "decrypted")
    pairs = []
    for part in _PARTITIONS:
        part_dir = os.path.join(pup_outdir, part)
        if not os.path.isdir(part_dir):
            continue
        for root, _dirs, files in os.walk(part_dir):
            for name in sorted(files):
                if not name.endswith(".elf"):
                    continue
                ours_path = os.path.join(root, name)
                rel = os.path.relpath(ours_path, part_dir)
                oracle_path = os.path.join(dec_root, rel)
                if os.path.exists(oracle_path):
                    pairs.append((ours_path, oracle_path))
    return pairs


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("pup_outdir")
    p.add_argument("--dump-root", default=DEFAULT_DUMP_ROOT)
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args(argv)

    pairs = find_pairs(args.pup_outdir, args.dump_root)
    print(f"found {len(pairs)} decrypted module(s) with a console-dump counterpart "
          f"(out of every .elf under {args.pup_outdir})")

    counts: dict[str, int] = {}
    differs: list[tuple[str, str]] = []
    for ours_path, oracle_path in pairs:
        rel = os.path.relpath(ours_path, args.pup_outdir)
        with open(ours_path, "rb") as f:
            ours = f.read()
        with open(oracle_path, "rb") as f:
            oracle = f.read()

        verdict = compare_one(ours, oracle)
        counts[verdict] = counts.get(verdict, 0) + 1
        if verdict not in ("identical", "segments-identical (wrapper/padding differs)"):
            differs.append((rel, verdict))
        elif args.verbose:
            print(f"  {verdict:9} {rel}")

    print()
    print("counts:")
    for k, v in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f"  {v:4}  {k}")

    if differs:
        print()
        print(f"{len(differs)} module(s) differ from the console dump's decrypted copy "
              "(expected for anything that changed between firmware versions):")
        for rel, why in differs:
            print(f"  {rel}: {why}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
