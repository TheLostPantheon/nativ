"""param.sfo parser.

An SFO ("system file object", aka PSF) is a flat key/value property list:
a header, a key-name table (NUL-terminated strings) and a data table (each
value is UTF-8, UTF-8+NUL, ASCII or a little-endian uint32). Layout and the
read algorithm are ported from Vita3K's packages/src/sfo.cpp `sfo::load` --
including its one subtlety: the key-table only stores offsets, so a key's
*length* is read as the gap to the *next* key's offset, which means the
loop needs one extra sentinel entry past the real last one.
"""
from __future__ import annotations

import argparse
import dataclasses
import struct
import sys

SFO_MAGIC = 0x46535000  # b"\x00PSF" read little-endian

_HEADER = struct.Struct("<IIIII")           # magic,version,key_table_start,data_table_start,tables_entries
_INDEX_ENTRY = struct.Struct("<HHIII")      # key_offset,data_fmt,data_len,data_max_len,data_offset

FMT_UTF8 = 0x0004
FMT_UTF8_NULL = 0x0204
FMT_ASCII = 0x0402
FMT_UINT32 = 0x0404


class SfoError(ValueError):
    pass


def parse_sfo(data: bytes) -> dict[str, str | int]:
    """Parse a param.sfo buffer into an ordered {key: value} dict (str values
    for UTF8/UTF8_NULL/ASCII entries, int for UINT32_T ones)."""
    if len(data) < _HEADER.size:
        raise SfoError("file too short for an SFO header")
    magic, _version, key_start, data_start, n = _HEADER.unpack_from(data, 0)
    if magic != SFO_MAGIC:
        raise SfoError(f"bad magic 0x{magic:08X} (expected 0x{SFO_MAGIC:08X})")

    entries = [_INDEX_ENTRY.unpack_from(data, _HEADER.size + i * _INDEX_ENTRY.size) for i in range(n)]
    # Sentinel so entry i's key length is (next key_offset - this key_offset);
    # the "next" key_offset for the last real entry is the start of the data
    # table itself, expressed relative to the key table (sfo.cpp does the same).
    key_offsets = [e[0] for e in entries] + [data_start - key_start]

    out: dict[str, str | int] = {}
    for i, (key_offset, data_fmt, data_len, _max_len, data_offset) in enumerate(entries):
        key_size = key_offsets[i + 1] - key_offset
        key_bytes = data[key_start + key_offset: key_start + key_offset + key_size - 1]  # drop the NUL
        key = key_bytes.decode("ascii", "replace")

        raw = data[data_start + data_offset: data_start + data_offset + data_len]
        if data_fmt == FMT_UINT32:
            if len(raw) < 4:
                raise SfoError(f"entry {key!r}: UINT32 value too short")
            value: str | int = struct.unpack_from("<I", raw, 0)[0]
        elif data_fmt in (FMT_ASCII, FMT_UTF8):
            value = raw.decode("utf-8", "replace")
        elif data_fmt == FMT_UTF8_NULL:
            value = raw[:-1].decode("utf-8", "replace") if raw.endswith(b"\x00") else raw.decode("utf-8", "replace")
        else:
            raise SfoError(f"entry {key!r}: unknown data_fmt 0x{data_fmt:04X}")
        out[key] = value
    return out


@dataclasses.dataclass
class AppInfo:
    title_id: str = ""
    title: str = ""
    app_version: str = ""
    category: str = ""
    content_id: str = ""


def get_app_info(sfo: dict[str, str | int]) -> AppInfo:
    """sfo::get_param_info, minus the system-language short-title fallback
    (we have no "current system language" here, so we only use the
    unsuffixed TITLE/STITLE keys)."""
    app_version = str(sfo.get("APP_VER", ""))
    if app_version.startswith("0"):
        app_version = app_version[1:]
    return AppInfo(
        title_id=str(sfo.get("TITLE_ID", "")),
        title=str(sfo.get("TITLE", "")).replace("\n", " ").strip(),
        app_version=app_version,
        category=str(sfo.get("CATEGORY", "")),
        content_id=str(sfo.get("CONTENT_ID", "")),
    )


def read_sfo_file(path: str) -> dict[str, str | int]:
    with open(path, "rb") as f:
        return parse_sfo(f.read())


# ---------------------------------------------------------------------------
# A minimal SFO builder, used only by the self-test (and handy for anyone
# hand-building a synthetic PKG for pkg.py's own tests).
# ---------------------------------------------------------------------------

def build_sfo(fields: dict[str, str | int]) -> bytes:
    keys = b""
    key_offsets = []
    for k in fields:
        key_offsets.append(len(keys))
        keys += k.encode("ascii") + b"\x00"

    # Real param.sfo files pad each value up to a data_max_len and 4-byte-align
    # the whole data table; parse_sfo only reads data_len/data_offset, so for
    # a synthetic test blob we keep it simple and skip that padding.
    data = b""
    index_entries = []
    for (k, v), key_offset in zip(fields.items(), key_offsets):
        data_offset = len(data)
        if isinstance(v, int):
            fmt, payload = FMT_UINT32, struct.pack("<I", v)
        else:
            fmt, payload = FMT_UTF8_NULL, v.encode("utf-8") + b"\x00"
        index_entries.append((key_offset, fmt, len(payload), len(payload), data_offset))
        data += payload

    index = b"".join(_INDEX_ENTRY.pack(*e) for e in index_entries)
    key_start = _HEADER.size + len(index)
    data_start = key_start + len(keys)
    header = _HEADER.pack(SFO_MAGIC, 0x0101, key_start, data_start, len(fields))
    return header + index + keys + data



# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="python3 -m vitadecrypt.sfo", description="Parse a param.sfo file.")
    p.add_argument("path", nargs="?")
    p.add_argument("--key", help="print only this key's value")
    p.add_argument("--selftest", action="store_true")
    args = p.parse_args(argv)

    if args.selftest or not args.path:
        _selftest()
        return 0

    sfo = read_sfo_file(args.path)
    if args.key:
        if args.key not in sfo:
            print(f"error: no key {args.key!r}", file=sys.stderr)
            return 1
        print(sfo[args.key])
        return 0
    for k, v in sfo.items():
        print(f"{k}={v}")
    return 0


def _selftest() -> None:
    fields: dict[str, str | int] = {
        "APP_VER": "01.00",
        "CATEGORY": "gd",
        "CONTENT_ID": "IP0177-PCSB00013_00-GOLFDS00000000",
        "TITLE_ID": "PCSB00013",
        "TITLE": "Self Test Golf",
        "ATTRIBUTE": 0,
    }
    blob = build_sfo(fields)
    parsed = parse_sfo(blob)
    for k, v in fields.items():
        assert parsed[k] == v, f"{k}: {parsed[k]!r} != {v!r}"

    info = get_app_info(parsed)
    assert info.title_id == "PCSB00013"
    assert info.app_version == "1.00", info.app_version  # leading-zero stripped like Vita3K
    assert info.category == "gd"
    assert info.title == "Self Test Golf"

    # truncated/garbage input must raise, not crash or return partial data
    try:
        parse_sfo(blob[:8])
        raise AssertionError("expected SfoError on truncated input")
    except SfoError:
        pass
    try:
        parse_sfo(b"not an sfo file")
        raise AssertionError("expected SfoError on bad magic")
    except SfoError:
        pass

    print("sfo.py self-test: OK")


if __name__ == "__main__":
    raise SystemExit(main())
