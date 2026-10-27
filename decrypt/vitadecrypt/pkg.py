"""Vita PKG (+ licence file) -> a NoNpDrm-style extracted folder.

A PKG is one AES-128-CTR-encrypted blob (keyed off a per-package IV wrapped
under one of a handful of retail "PKG_VITA_KEY" constants, selected by a
small field in the extended header) holding a packed file table and the raw
file contents one after another. This only undoes *that* outer layer: the
files that come out the other end (eboot.bin, sce_module/*.suprx, sce_pfs/,
sce_sys/...) are exactly the NoNpDrm-style, still-PFS-encrypted tree pfs.py
expects -- see game.py for the rest of the pipeline.

Ported from Vita3K's packages/src/pkg.cpp, which itself credits mmozeiko's
pkg2zip; field names/sizes are pkg2zip's/Vita3K's packages/include/packages/pkg.h.
Everything in a PKG is big-endian, unlike the rest of this package's little-
endian Vita formats -- pkg2zip's comments call this out too.
"""
from __future__ import annotations

import argparse
import dataclasses
import os
import struct

from Crypto.Cipher import AES
from Crypto.Util import Counter

from . import keys as K
from . import sfo as SFO
from . import rif as RIF

_HEADER = struct.Struct(
    ">IHHIIIIQQQ48s16s16s64s"
)  # magic,revision,type,info_offset,info_count,header_size,file_count,total_size,data_offset,data_size,content_id,digest,pkg_data_iv,pkg_signatures
assert _HEADER.size == 0xC0

_EXT_HEADER = struct.Struct(">6IQ4I2Q")  # ... data_type2 is the 9th word
assert _EXT_HEADER.size == 0x40

_ENTRY = struct.Struct(">IIQQII")  # name_offset,name_size,data_offset,data_size,type,padding
assert _ENTRY.size == 0x20

PKG_MAGIC = 0x7F504B47     # "\x7fPKG"
EXT_MAGIC = 0x7F657874     # "\x7fext"
ENTRY_TYPE_DIR = {4, 18}

CONTENT_TYPE_APP = 0x15
CONTENT_TYPE_DLC = 0x16
CONTENT_TYPE_THEME = 0x1F


class PkgError(ValueError):
    pass


@dataclasses.dataclass
class PkgInfo:
    kind: str                  # "app" | "patch" | "dlc" | "theme"
    title_id: str
    content_id: str
    title: str
    app_version: str
    sfo: dict


def _ctr_decrypt(main_key: bytes, iv: bytes, offset_blocks: int, data: bytes) -> bytes:
    """AES-128-CTR starting at block `offset_blocks` (16 bytes/block) from a
    base IV -- pkg2zip/Vita3K's ctr_init() is just big-endian (iv + n)."""
    ctr_val = (int.from_bytes(iv, "big") + offset_blocks) & ((1 << 128) - 1)
    ctr = Counter.new(128, initial_value=ctr_val)
    return AES.new(main_key, AES.MODE_CTR, counter=ctr).decrypt(data)


def _head(read, size: int) -> tuple[PkgInfo, tuple]:
    """The header, the info table and the embedded param.sfo, through read(offset, n): the PKG's identity, and what
    extract_pkg needs to walk its files (main key, IV, data offset, file table offset, file count). The outer layer's
    key is a retail constant, so this needs no licence."""
    if size < _HEADER.size + _EXT_HEADER.size:
        raise PkgError("file too short to be a PKG")
    head = read(0, _HEADER.size + _EXT_HEADER.size)

    (magic, _rev, _type, info_offset, info_count, _header_size, file_count,
     total_size, data_offset, _data_size, content_id_raw, _digest, pkg_data_iv,
     _sigs) = _HEADER.unpack_from(head, 0)
    if magic != PKG_MAGIC:
        raise PkgError(f"bad PKG magic 0x{magic:08X}")
    if size < total_size:
        raise PkgError("file is smaller than the header's own total_size -- truncated download?")

    ext_words = _EXT_HEADER.unpack_from(head, _HEADER.size)
    ext_magic, data_type2 = ext_words[0], ext_words[8]
    if ext_magic != EXT_MAGIC:
        raise PkgError(f"bad PKG ext-header magic 0x{ext_magic:08X}")

    content_id = content_id_raw.rstrip(b"\x00").decode("ascii", "replace")

    # Walk the little type/size/payload info table for content_type, the file
    # table offset, and the embedded param.sfo's location -- pkg.cpp reads a
    # fixed 16 bytes per entry but only advances by 8 + size.
    content_type = sfo_offset = sfo_size = items_offset = 0
    off = info_offset
    for _ in range(info_count):
        block = struct.unpack_from(">4I", read(off, 16), 0)
        btype, bsize = block[0], block[1]
        if btype == 2:
            content_type = block[2]
        elif btype == 13:
            items_offset = block[2]
        elif btype == 14:
            sfo_offset, sfo_size = block[2], block[3]
        off += 8 + bsize

    if content_type == CONTENT_TYPE_APP:
        kind = "app"
    elif content_type == CONTENT_TYPE_DLC:
        kind = "dlc"
    elif content_type == CONTENT_TYPE_THEME:
        kind = "theme"
    else:
        raise PkgError(f"unsupported content type 0x{content_type:X} (only Vita app/DLC/theme PKGs are supported)")

    key_type = data_type2 & 7
    pkg_vita_key = K.PKG_VITA_KEYS.get(key_type)
    if pkg_vita_key is None:
        raise PkgError(f"unknown PKG key revision {key_type} (only 2-4 are known retail keys)")
    main_key = AES.new(pkg_vita_key, AES.MODE_ECB).encrypt(pkg_data_iv)

    sfo_bytes = _ctr_decrypt(main_key, pkg_data_iv, sfo_offset // 16, read(data_offset + sfo_offset, sfo_size))
    app_sfo = SFO.parse_sfo(sfo_bytes)
    app_info = SFO.get_app_info(app_sfo)

    if kind == "app" and app_info.category == "gp":
        kind = "patch"  # same content_type as a full app; Vita3K tells them apart via the SFO

    info = PkgInfo(
        kind=kind,
        title_id=app_info.title_id,
        content_id=content_id or app_info.content_id,
        title=app_info.title,
        app_version=app_info.app_version,
        sfo=app_sfo,
    )
    return info, (main_key, pkg_data_iv, data_offset, items_offset, file_count)


def peek_pkg(pkg_path: str) -> PkgInfo:
    """What a PKG is (app, patch, add-on), its title and version, without a licence and without reading the gigabytes
    after its header (the hub's folder scan)."""
    with open(pkg_path, "rb") as f:
        def read(off: int, n: int) -> bytes:
            f.seek(off)
            return f.read(n)
        return _head(read, os.path.getsize(pkg_path))[0]


def extract_pkg(pkg_path: str, out_dir: str, *, rif_path: str | None = None) -> PkgInfo:
    """Decrypt `pkg_path` into `out_dir` (created if missing) as a flat,
    still-PFS-encrypted NoNpDrm-style tree, plus `sce_sys/package/work.bin`
    built from the given .rif licence file (optional -- without one, work.bin
    is simply not written, and pfs.py/game.py will need an explicit
    --klicensee instead)."""
    with open(pkg_path, "rb") as f:
        data = f.read()
    info, (main_key, pkg_data_iv, data_offset, items_offset, file_count) = _head(lambda o, n: data[o:o + n], len(data))

    os.makedirs(out_dir, exist_ok=True)

    entries_raw = _ctr_decrypt(main_key, pkg_data_iv, items_offset // 16, data[data_offset + items_offset: data_offset + items_offset + file_count * _ENTRY.size])
    for i in range(file_count):
        name_offset, name_size, f_data_offset, f_data_size, f_type, _pad = _ENTRY.unpack_from(entries_raw, i * _ENTRY.size)
        if data_offset + name_offset + name_size > len(data) or data_offset + f_data_offset + f_data_size > len(data):
            raise PkgError(f"entry {i}: offsets run past end of file -- wrong key or corrupted PKG")

        name_ct = data[data_offset + name_offset: data_offset + name_offset + name_size]
        name = _ctr_decrypt(main_key, pkg_data_iv, name_offset // 16, name_ct).decode("utf-8", "replace")
        if not name or name in (".", "..") or name.startswith("/") or ".." in name.split("/"):
            raise PkgError(f"entry {i}: unsafe/garbage name {name!r} -- wrong key?")

        dest = os.path.join(out_dir, *name.split("/"))
        if (f_type & 0xFF) in ENTRY_TYPE_DIR:
            os.makedirs(dest, exist_ok=True)
            continue

        os.makedirs(os.path.dirname(dest), exist_ok=True)
        file_ct = data[data_offset + f_data_offset: data_offset + f_data_offset + f_data_size]
        plain = _ctr_decrypt(main_key, pkg_data_iv, f_data_offset // 16, file_ct)
        with open(dest, "wb") as out:
            out.write(plain)

    rif = None
    if rif_path:
        with open(rif_path, "rb") as f:
            rif = f.read()
    if rif is not None:
        work_dir = os.path.join(out_dir, "sce_sys", "package")
        os.makedirs(work_dir, exist_ok=True)
        with open(os.path.join(work_dir, "work.bin"), "wb") as f:
            f.write(rif)

    return info


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="python3 -m vitadecrypt.pkg",
        description="Extract a Vita PKG (+ .rif licence file) into a NoNpDrm-style folder (still PFS-encrypted).",
    )
    p.add_argument("pkg", nargs="?")
    p.add_argument("outdir", nargs="?")
    p.add_argument("--rif")
    p.add_argument("--selftest", action="store_true")
    args = p.parse_args(argv)

    if args.selftest or not (args.pkg and args.outdir):
        _selftest()
        return 0

    info = extract_pkg(args.pkg, args.outdir, rif_path=args.rif)
    print(f"{info.kind} {info.title_id} {info.content_id!r} {info.title!r} v{info.app_version}")
    print(f"extracted to {args.outdir}")
    return 0


def _pad16(b: bytes) -> bytes:
    return b + b"\x00" * (-len(b) % 16)


def _build_synthetic_pkg(
    tmp_sfo: bytes, files: dict[str, bytes], *, key_type: int = 2, content_type: int = CONTENT_TYPE_APP,
) -> bytes:
    """Build a byte-exact PKG (same header/info-table/entry/AES-CTR layout as
    a real retail one) so pkg.py can be tested without a real game.
    Intentionally reimplements the *encrypt* side independently of
    extract_pkg's decrypt side (same primitives, inverse order, built up
    section by section here instead of extract_pkg's single-region read) so
    the test is not circular. Every section below is kept 16-byte aligned so
    each one's AES-CTR counter offset (byte_offset // 16) is exact.
    """
    pkg_data_iv = bytes(range(16))
    main_key = AES.new(K.PKG_VITA_KEYS[key_type], AES.MODE_ECB).encrypt(pkg_data_iv)

    def enc(offset_blocks: int, plain: bytes) -> bytes:
        ctr_val = (int.from_bytes(pkg_data_iv, "big") + offset_blocks) & ((1 << 128) - 1)
        return AES.new(main_key, AES.MODE_CTR, counter=Counter.new(128, initial_value=ctr_val)).encrypt(plain)

    names = list(files)
    blobs = [files[n] for n in names]
    name_blobs = [n.encode("utf-8") for n in names]

    # Data-region layout (everything below is relative to this region's own
    # start, i.e. relative to the header's data_offset -- same convention
    # extract_pkg reads): [param.sfo][entry table][names...][file data...]
    sfo_offset = 0
    items_offset = sfo_offset + len(_pad16(tmp_sfo))
    body_offset = items_offset + len(names) * _ENTRY.size  # _ENTRY.size (32) is already 16-aligned

    name_offsets, data_offsets = [], []
    cursor = body_offset
    for nb in name_blobs:
        name_offsets.append(cursor)
        cursor += len(_pad16(nb))
    for blob in blobs:
        data_offsets.append(cursor)
        cursor += len(_pad16(blob))
    region_size = cursor

    region = bytearray(region_size)
    region[sfo_offset: sfo_offset + len(tmp_sfo)] = tmp_sfo
    entries = bytearray()
    for nb, blob, noff, doff in zip(name_blobs, blobs, name_offsets, data_offsets):
        entries += _ENTRY.pack(noff, len(nb), doff, len(blob), 1, 0)
        region[noff: noff + len(nb)] = nb
        region[doff: doff + len(blob)] = blob
    region[items_offset: items_offset + len(entries)] = bytes(entries)

    region_enc = enc(0, bytes(region))

    info_table = (
        struct.pack(">4I", 2, 8, content_type, 0)
        + struct.pack(">4I", 13, 8, items_offset, 0)
        + struct.pack(">4I", 14, 8, sfo_offset, len(tmp_sfo))
    )
    info_offset = _HEADER.size + _EXT_HEADER.size
    data_offset_abs = info_offset + len(info_table)
    total_size = data_offset_abs + region_size

    content_id = b"IP0177-PCSB00013_00-GOLFDS00000000".ljust(48, b"\x00")
    header = _HEADER.pack(
        PKG_MAGIC, 1, 0, info_offset, 3, _HEADER.size, len(names),
        total_size, data_offset_abs, region_size,
        content_id, bytes(16), pkg_data_iv, bytes(64),
    )
    ext_header = _EXT_HEADER.pack(EXT_MAGIC, 0, _EXT_HEADER.size, 0, 0, 0, 0, 0, key_type, 0, 0, 0, 0)
    return header + ext_header + info_table + region_enc


def _selftest() -> None:
    tmp_sfo = SFO.build_sfo({
        "APP_VER": "01.00", "CATEGORY": "gd", "TITLE_ID": "PCSB00013",
        "CONTENT_ID": "IP0177-PCSB00013_00-GOLFDS00000000", "TITLE": "Self Test Golf",
    })
    files = {
        "eboot.bin": b"\x7fELF-fake-eboot-for-test\x00" * 4,
        "sce_sys/param.sfo": tmp_sfo,
        "sce_module/libc.suprx": b"SCE\x00-fake-suprx-for-test",
    }
    pkg_bytes = _build_synthetic_pkg(tmp_sfo, files, key_type=3)

    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        pkg_path = os.path.join(tmp, "test.pkg")
        out_dir = os.path.join(tmp, "out")
        with open(pkg_path, "wb") as f:
            f.write(pkg_bytes)

        klic = bytes.fromhex("00112233445566778899aabbccddeeff")[:16]
        rif_path = os.path.join(tmp, "fake.rif")
        with open(rif_path, "wb") as f:
            f.write(RIF.build_rif("IP0177-PCSB00013_00-GOLFDS00000000", klic))

        info = extract_pkg(pkg_path, out_dir, rif_path=rif_path)
        assert info.kind == "app", info.kind
        assert info.title_id == "PCSB00013", info.title_id
        assert info.content_id == "IP0177-PCSB00013_00-GOLFDS00000000"
        assert info.title == "Self Test Golf"

        for name, blob in files.items():
            got = open(os.path.join(out_dir, *name.split("/")), "rb").read()
            assert got == blob, f"{name}: extracted content differs"

        work = open(os.path.join(out_dir, "sce_sys", "package", "work.bin"), "rb").read()
        assert RIF.parse_rif(work).klicensee == klic

        # a PATCH pkg (same content_type, CATEGORY=gp) is told apart via the SFO
        patch_sfo = SFO.build_sfo({
            "APP_VER": "01.01", "CATEGORY": "gp", "TITLE_ID": "PCSB00013",
            "CONTENT_ID": "IP0177-PCSB00013_00-GOLFDS00000000", "TITLE": "Self Test Golf",
        })
        patch_pkg = _build_synthetic_pkg(patch_sfo, {"eboot.bin": b"patch-eboot"}, key_type=4)
        with open(pkg_path, "wb") as f:
            f.write(patch_pkg)
        info2 = extract_pkg(pkg_path, os.path.join(tmp, "out2"))
        assert info2.kind == "patch", info2.kind
        # the folder scan's peek: the same identity from the header alone
        peek = peek_pkg(pkg_path)
        assert (peek.kind, peek.title_id, peek.app_version) == ("patch", "PCSB00013", "1.01"), peek

        # wrong key_type garbles the file table -> must raise, not produce garbage files
        corrupt = bytearray(pkg_bytes)
        ext_off = _HEADER.size
        ext_vals = list(_EXT_HEADER.unpack_from(corrupt, ext_off))
        ext_vals[8] = 0  # data_type2's low 3 bits select the key; 0 is not in PKG_VITA_KEYS
        _EXT_HEADER.pack_into(corrupt, ext_off, *ext_vals)
        bad_path = os.path.join(tmp, "bad.pkg")
        with open(bad_path, "wb") as f:
            f.write(bytes(corrupt))
        try:
            extract_pkg(bad_path, os.path.join(tmp, "out3"))
            raise AssertionError("expected PkgError for an unknown key revision")
        except PkgError:
            pass

    print("pkg.py self-test: OK")


if __name__ == "__main__":
    raise SystemExit(main())
