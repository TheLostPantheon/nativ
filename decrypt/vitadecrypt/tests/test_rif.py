"""Thin wrapper so `python3 -m vitadecrypt.tests.test_rif` sits next to the
other self-tests; the real check lives in rif.py's own --selftest (a
synthetic RIF round-tripped through build_rif/parse_rif, plus a short-RIF
rejection check).
"""
from vitadecrypt.rif import _selftest


def main() -> None:
    _selftest()


if __name__ == "__main__":
    main()
