"""Thin wrapper so `python3 -m vitadecrypt.tests.test_sfo` sits next to the
other self-tests; the real checks live in sfo.py's own --selftest (a hand-
built param.sfo parsed back, plus truncated/bad-magic rejection).
"""
from vitadecrypt.sfo import _selftest


def main() -> None:
    _selftest()


if __name__ == "__main__":
    main()
