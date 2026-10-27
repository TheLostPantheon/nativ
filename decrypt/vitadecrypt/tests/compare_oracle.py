"""Verify sce_self.decrypt_self() against the console-decrypted oracle.

dump/encrypted/{os0,vs0}/... holds a console's own firmware modules,
still encrypted. dump/decrypted/... holds the same modules decrypted *on the
console* (FAGDec) and then converted to plain ELF by ../../fself2elf.py; its
mapping rule, found by listing both trees, is: strip the "os0/" or "vs0/"
prefix and append ".elf".

For every encrypted .self/.suprx/.skprx that has such a counterpart, this
decrypts it from scratch (real AES, not FAGDec's already-decrypted segments)
and diffs the result against the oracle: byte-identical, or if not, exactly
which loadable bytes differ and where.

Usage: python3 -m vitadecrypt.tests.compare_oracle [--dump-root PATH]
"""
from __future__ import annotations

import argparse
import os
import struct
import sys

from vitadecrypt.sce_self import SelfFormatError, decrypt_self, _ELF_HDR, _ELF_PHDR

DEFAULT_DUMP_ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "dump"))
SELF_EXTS = (".self", ".suprx", ".skprx")


def find_pairs(dump_root: str) -> list[tuple[str, str]]:
    """Returns [(encrypted_path, decrypted_path), ...] using the mapping rule
    above, for every encrypted SELF-like file that has a decrypted counterpart."""
    enc_root = os.path.join(dump_root, "encrypted")
    dec_root = os.path.join(dump_root, "decrypted")
    pairs = []
    for base in ("os0", "vs0"):
        base_dir = os.path.join(enc_root, base)
        if not os.path.isdir(base_dir):
            continue
        for root, _dirs, files in os.walk(base_dir):
            for name in sorted(files):
                if not name.lower().endswith(SELF_EXTS):
                    continue
                enc_path = os.path.join(root, name)
                rel = os.path.relpath(enc_path, base_dir)
                dec_path = os.path.join(dec_root, rel + ".elf")
                if os.path.exists(dec_path):
                    pairs.append((enc_path, dec_path))
    return pairs


def _loadable_segments(elf: bytes) -> list[tuple[int, int, int]]:
    """[(p_type, p_offset, p_filesz), ...] for every program header, read
    straight from the ELF's own e_phoff/e_phnum (not from SELF metadata) so
    this check works equally on sce_self's output and the oracle's."""
    if elf[:4] != b"\x7fELF":
        raise ValueError("not an ELF")
    (_e_ident, _e_type, _e_machine, _e_version, _e_entry, e_phoff, _e_shoff,
     _e_flags, _e_ehsize, e_phentsize, e_phnum, *_rest) = _ELF_HDR.unpack_from(elf, 0)
    segs = []
    for i in range(e_phnum):
        p_type, p_offset, _v, _p, p_filesz, *_r = _ELF_PHDR.unpack_from(elf, e_phoff + i * e_phentsize)
        segs.append((p_type, p_offset, p_filesz))
    return segs


def compare_one(ours: bytes, oracle: bytes) -> str:
    """'identical', 'segments-identical' (headers/padding differ but every
    loadable segment's bytes match), or a short description of the first
    real difference."""
    if ours == oracle:
        return "identical"

    try:
        our_segs = _loadable_segments(ours)
        oracle_segs = _loadable_segments(oracle)
    except (ValueError, struct.error) as exc:
        return f"not comparable as ELF: {exc}"

    if len(our_segs) != len(oracle_segs):
        return f"phnum differs: ours={len(our_segs)} oracle={len(oracle_segs)}"

    for i, ((ot, oo, ofsz), (xt, xo, xfsz)) in enumerate(zip(our_segs, oracle_segs)):
        if ofsz != xfsz:
            return f"segment {i}: p_filesz differs (ours=0x{ofsz:X} oracle=0x{xfsz:X})"
        ours_bytes = ours[oo: oo + ofsz]
        oracle_bytes = oracle[xo: xo + xfsz]
        if ours_bytes != oracle_bytes:
            first = next((j for j in range(len(ours_bytes)) if ours_bytes[j] != oracle_bytes[j]), None)
            return f"segment {i}: bytes differ at offset 0x{first:X} (p_type=0x{ot:X})"

    return "segments-identical (wrapper/padding differs)"


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dump-root", default=DEFAULT_DUMP_ROOT)
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args(argv)

    pairs = find_pairs(args.dump_root)
    print(f"found {len(pairs)} encrypted file(s) with a decrypted counterpart under {args.dump_root}")

    counts: dict[str, int] = {}
    failures: list[tuple[str, str]] = []
    for enc_path, dec_path in pairs:
        rel = os.path.relpath(enc_path, os.path.join(args.dump_root, "encrypted"))
        with open(enc_path, "rb") as f:
            enc_data = f.read()
        with open(dec_path, "rb") as f:
            oracle = f.read()

        try:
            ours = decrypt_self(enc_data, klicensee=None)
        except SelfFormatError as exc:
            counts["decrypt-failed"] = counts.get("decrypt-failed", 0) + 1
            failures.append((rel, f"decrypt_self raised: {exc}"))
            continue
        except Exception as exc:  # noqa: BLE001 -- want every exception logged, not just ours
            counts["decrypt-error"] = counts.get("decrypt-error", 0) + 1
            failures.append((rel, f"unexpected {type(exc).__name__}: {exc}"))
            continue

        verdict = compare_one(ours, oracle)
        counts[verdict] = counts.get(verdict, 0) + 1
        if verdict not in ("identical", "segments-identical (wrapper/padding differs)"):
            failures.append((rel, verdict))
        elif args.verbose:
            print(f"  {verdict:9} {rel}")

    print()
    print("counts:")
    for k, v in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f"  {v:4}  {k}")

    if failures:
        print()
        print(f"{len(failures)} failure(s)/mismatch(es):")
        for rel, why in failures:
            print(f"  {rel}: {why}")

    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
