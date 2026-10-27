"""Thin wrapper so `python3 -m vitadecrypt.tests.test_pkg` sits next to the
other self-tests; the real checks live in pkg.py's own --selftest (a byte-
exact synthetic PKG built independently of extract_pkg's decrypt path,
covering app/patch kind detection, the embedded work.bin, and rejection of
an unknown key revision).
"""
from vitadecrypt.pkg import _selftest


def main() -> None:
    _selftest()


if __name__ == "__main__":
    main()
