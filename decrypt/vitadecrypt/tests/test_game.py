"""Thin wrapper so `python3 -m vitadecrypt.tests.test_game` sits next to the
other self-tests; the real checks live in game.py's own --selftest (an end-
to-end synthetic PKG -> PFS -> SELF run, a NoNpDrm folder with an
auto-discovered work.bin, an already-decrypted folder with --license, and a
missing-klicensee rejection).
"""
from vitadecrypt.game import _selftest


def main() -> None:
    _selftest()


if __name__ == "__main__":
    main()
