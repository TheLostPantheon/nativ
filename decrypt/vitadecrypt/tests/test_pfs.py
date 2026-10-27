"""Thin wrapper so `python3 -m vitadecrypt.tests.test_pfs` sits next to the
other self-tests; the real checks live in pfs.py's own --selftest (a byte-
exact synthetic files.db/unicv.db/encrypted-file tree, decrypted and checked
byte-for-byte, plus wrong-klicensee and tampered-sector rejection).
"""
from vitadecrypt.pfs import _selftest


def main() -> None:
    _selftest()


if __name__ == "__main__":
    main()
