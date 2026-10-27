"""RIF (work.bin/.rif) parsing, and klicensee/content-id extraction.

A "RIF" (rights information file, aka work.bin) is Sony's fixed 512-byte
`SceNpDrmLicense` struct -- the thing that proves a title id/account is
entitled to decrypt a given content id, and that carries the per-title
`klicensee` SELF/PFS decryption key.

license.cpp's SceNpDrmLicense field layout (sce_types.h) is the schema for
`parse_rif`/`build_rif` below.
"""
from __future__ import annotations

import argparse
import dataclasses
import struct

RIF_SIZE = 512          # sizeof(SceNpDrmLicense)


class RifError(ValueError):
    """A RIF that is the wrong size or otherwise malformed."""


# ---------------------------------------------------------------------------
# SceNpDrmLicense (packages/include/packages/sce_types.h / license.h)
# ---------------------------------------------------------------------------
# <QHHHH -> version,version_flag,type,flags (8B) then aid(Q), content_id(0x30),
# key_table(0x10), key(0x10), start_time(Q), expiration_time(Q),
# ecdsa_signature(0x28), flags2(Q), key2(0x10), unk_B0(0x10), openpsid(0x10),
# unk_D0(0x10), cmd56_handshake(0x14), unk_F4(I), unk_F8(I), sku_flag(i),
# rsa_signature(0x100). Only the fields we actually use are broken out.
_RIF = struct.Struct("<HHHHQ48s16s16sQQ40sQ16s16s16s16s20sIIi256s")
assert _RIF.size == RIF_SIZE


@dataclasses.dataclass
class RifInfo:
    content_id: str
    title_id: str          # content_id[7:16], same slice Vita3K's copy_license uses
    klicensee: bytes        # the 16-byte PFS/SELF decryption key (license.key)
    aid: int
    start_time: int
    expiration_time: int
    raw: bytes = dataclasses.field(repr=False)


def parse_rif(rif: bytes) -> RifInfo:
    if len(rif) != RIF_SIZE:
        raise RifError(f"RIF is {len(rif)} bytes, expected {RIF_SIZE}")
    (_ver, _verflag, _type, _flags, aid, content_id, _key_table, key,
     start_time, expiration_time, *_rest) = _RIF.unpack(rif)
    content_id = content_id.rstrip(b"\x00").decode("ascii", "replace")
    return RifInfo(
        content_id=content_id,
        title_id=content_id[7:16],
        klicensee=key,
        aid=aid,
        start_time=start_time,
        expiration_time=expiration_time,
        raw=rif,
    )


def build_rif(
    content_id: str,
    klicensee: bytes,
    *,
    aid: int = 0x0123456789ABCDEF,  # FAKE_AID, same constant NoNpDrm-style tools use
    rif_type: int = 0,
    sku_flag: int = 0,
) -> bytes:
    """Build a (fake, unsigned) RIF for testing / NoNpDrm-style use. A real RIF
    also carries an ECDSA/RSA signature we cannot forge; this is only useful
    for round-trip tests and for feeding a known klicensee through game.py."""
    if len(klicensee) != 16:
        raise ValueError("klicensee must be 16 bytes")
    cid = content_id.encode("ascii")
    if len(cid) > 0x30:
        raise ValueError("content_id too long")
    cid = cid.ljust(0x30, b"\x00")
    return _RIF.pack(
        1, 0, rif_type, 0, aid, cid, bytes(16), klicensee,
        0, 0xFFFFFFFFFFFFFFFF, bytes(40), 0, bytes(16), bytes(16), bytes(16),
        bytes(16), bytes(20), 0, 0, sku_flag, bytes(256),
    )


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="python3 -m vitadecrypt.rif",
        description="Print a work.bin/.rif file's klicensee/content id, or run the self-test.",
    )
    p.add_argument("--rif", help="a work.bin/.rif file to read")
    p.add_argument("--selftest", action="store_true")
    args = p.parse_args(argv)

    if args.selftest or not args.rif:
        _selftest()
        return 0

    with open(args.rif, "rb") as f:
        rif = f.read()
    info = parse_rif(rif)
    print(f"content_id={info.content_id} title_id={info.title_id} klicensee={info.klicensee.hex()}")
    return 0


def _selftest() -> None:
    # Round trip a synthetic RIF through our own build/parse.
    klic = bytes.fromhex("00112233445566778899aabbccddeeff")[:16]
    rif = build_rif("IP0177-PCSB00013_00-GOLFDS00000000", klic)
    assert len(rif) == RIF_SIZE
    info = parse_rif(rif)
    assert info.klicensee == klic
    assert info.title_id == "PCSB00013", info.title_id
    assert info.content_id == "IP0177-PCSB00013_00-GOLFDS00000000"

    # A wrong-size RIF must raise, not silently return garbage.
    try:
        parse_rif(rif[:-4])
        raise AssertionError("expected a RifError for a short RIF")
    except RifError:
        pass

    print("rif.py self-test: OK")


if __name__ == "__main__":
    raise SystemExit(main())
