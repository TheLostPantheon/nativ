"""Thin wrapper so `python3 -m vitadecrypt.tests.test_keys` sits next to the
other self-tests; the real checks live in keys.py's own --selftest (table
sizes, known lookups, KEYS_PUP/KEYS_SELF agreement) since `python3 -m
vitadecrypt.keys --selftest` is also how the module documents running it
standalone.
"""
from vitadecrypt.keys import _selftest


def main() -> None:
    _selftest()


if __name__ == "__main__":
    main()
