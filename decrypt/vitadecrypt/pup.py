"""Decrypt a Vita firmware update file (PSP2UPDAT.PUP) into a dump-shaped
output tree: os0/ and vs0/ with every module as a plain ELF alongside its
still-encrypted original.

Ports Vita3K's packages/src/pup.cpp (extract_pup_files, decrypt_pup_packages,
decrypt_segments -- credited there to TeamMolecule's sceutils) into Python,
using this package's own keys.py (the same key tables, in KEYS_PUP) and
sce_self.py (unmodified -- see decrypt_tree_selfs() below) for the final
per-module SELF decryption.

A PUP is: a small "SCEUF" header, a flat table of file records (offset,
length, a numeric "filetype"), and the files themselves back-to-back. Most
filetypes are one-off blobs (version.txt, the two bootstrap SELFs); the rest
are consecutive *parts* of a firmware filesystem partition (os0, vs0, pd0,
sa0, ...), each part itself an SCE "SPKG" container that decrypts (same
metadata-block shape as a SELF, just with SPKG's own static key -- see
pup.decrypt_spkg) to a contiguous chunk of that partition's raw FAT16 image.
Concatenating a partition's parts in file-table order and feeding the result
to fat16.FAT16Image recovers the partition's filesystem.

Scope: only os0 and vs0 (FAT16) are extracted, matching this port's brief.
pd0 is exFAT on real hardware (a different on-disk format, Vita3K's
exfat.cpp) and sa0 isn't dump-compared here, so both are skipped; see
_PARTITIONS below to add one back if a FAT16 image ever needs it.
"""
from __future__ import annotations

import argparse
import dataclasses
import os
import struct
import sys
import zlib

from Crypto.Cipher import AES
from Crypto.Util import Counter

from . import fat16
from . import keys as K
from . import sce_self
from .sce_self import _METAHDR, _METAINFO, _METASEC, _Segment, SelfFormatError

# ---------------------------------------------------------------------------
# PUP container layout (pup.cpp's extract_pup_files)
# ---------------------------------------------------------------------------

_SCEUF_FILEREC = struct.Struct("<QQQQ")        # filetype, offset, length, flags
SCEUF_HEADER_SIZE = 0x80
SCEUF_FILEREC_SIZE = 0x20

# filetype -> output name, for the one-off entries (not filesystem parts).
PUP_TYPES: dict[int, str] = {
    0x100: "version.txt",
    0x101: "license.xml",
    0x200: "psp2swu.self",
    0x204: "cui_setupper.self",
    0x400: "package_scewm.wm",
    0x401: "package_sceas.as",
    0x2005: "UpdaterES1.CpUp",
    0x2006: "UpdaterES2.CpUp",
}

# SpkgType enum value -> partition name (packages/src/pup.cpp's FSTYPE[]),
# indexed by the low byte of each part's own SpkgHeader.type field.
FSTYPE: list[str] = [
    "unknown0", "os0", "unknown2", "unknown3", "vs0_chmod", "unknown5", "unknown6", "unknown7",
    "pervasive8", "boot_slb2", "vs0", "devkit_cp", "motionC", "bbmc", "unknownE", "motionF",
    "touch10", "touch11", "syscon12", "syscon13", "pervasive14", "unknown15", "vs0_tarpatch",
    "sa0", "pd0", "pervasive19", "unknown1A", "psp_emulist",
]

# Which FSTYPE-named partitions this port actually assembles (see module docstring).
_PARTITIONS = ("os0", "vs0")


@dataclasses.dataclass
class PupFileRecord:
    filetype: int
    offset: int
    length: int
    flags: int
    name: str


def _peek_partition_type(data: bytes, entry_offset: int) -> int | None:
    """make_filename() in pup.cpp: peek HEADER_LENGTH bytes at this PUP
    entry's own offset, confirm it looks like an SCE SPKG container
    (magic+version+platform/keyrev/scetype packed as 0x30040 == VITA
    platform, keyrev 0, SceType.SPKG), then return the low byte of its
    SpkgHeader.type field -- the index into FSTYPE -- or None if either
    check fails (a non-SPKG/unrecognised blob, named "unknown-0x..." instead)."""
    hdr = data[entry_offset: entry_offset + K.HEADER_LENGTH]
    if len(hdr) < K.HEADER_LENGTH:
        return None
    magic, _version, flags_u32 = struct.unpack_from("<III", hdr, 0)
    header_length, = struct.unpack_from("<Q", hdr, 16)
    if magic != K.SCE_MAGIC or flags_u32 != 0x30040 or header_length + 8 > len(hdr):
        return None
    t = hdr[header_length + 4]
    return t if t < len(FSTYPE) else None


def parse_pup(data: bytes) -> tuple[dict, list[PupFileRecord]]:
    """Parse a PUP's SCEUF header and file table (does not touch any file's
    contents besides the HEADER_LENGTH peek used to name filesystem parts)."""
    if data[:5] != b"SCEUF":
        raise ValueError("not a PUP file (missing SCEUF magic)")
    pup_version, = struct.unpack_from("<I", data, 8)
    fw_version, = struct.unpack_from("<I", data, 0x10)
    build_number, = struct.unpack_from("<I", data, 0x14)
    count, = struct.unpack_from("<I", data, 0x18)
    info = {"pup_version": pup_version, "fw_version": fw_version, "build_number": build_number, "count": count}

    # make_filename()'s typecount is a single counter shared across every
    # filesystem-part entry, used only for a human-readable debug name here
    # (parts are kept in file-table order directly, not re-sorted by name).
    typecount = 0
    records: list[PupFileRecord] = []
    for i in range(count):
        filetype, offset, length, flags = _SCEUF_FILEREC.unpack_from(data, SCEUF_HEADER_SIZE + i * SCEUF_FILEREC_SIZE)
        if filetype in PUP_TYPES:
            name = PUP_TYPES[filetype]
        else:
            t = _peek_partition_type(data, offset)
            if t is not None:
                name = f"{FSTYPE[t]}-{typecount:02d}.pkg"
                typecount += 1
            else:
                name = f"unknown-0x{filetype:X}.pkg"
        records.append(PupFileRecord(filetype, offset, length, flags, name))
    return info, records


# ---------------------------------------------------------------------------
# SPKG decryption (pup.cpp's decrypt_segments(), specialised for SceType.SPKG)
# ---------------------------------------------------------------------------

def _spkg_metadata_key(key_revision: int) -> K.KeyEntry:
    """The single METADATA/SPKG entry Vita3K's install_pup() registers (one
    key, valid for every sysver -- see keys.KEYS_PUP's SceType.SPKG row)."""
    return K.get_key(K.KeyType.METADATA, K.SceType.SPKG, sysver=-1, keyrev=key_revision,
                      selftype=K.SelfType.NONE, store=K.KEYS_PUP)


def _spkg_segments(data: bytes, header_length: int, metadata_offset: int, key_revision: int) -> list[_Segment]:
    """Same algorithm as sce_self._get_segments, specialised for SceType.SPKG:
    SPKGs (firmware filesystem parts) are never per-title NPDRM-wrapped, so
    this is just that function's non-APP path with SPKG's own metadata key.
    Kept as a separate copy here rather than widening sce_self._get_segments
    (owned by the SELF part) to take a sce_type argument -- not a bug in that
    function, just a case outside what it's documented to handle."""
    body = data[metadata_offset + 48: header_length]
    meta = _spkg_metadata_key(key_revision)

    info_pt = AES.new(meta.key, AES.MODE_CBC, iv=meta.iv).decrypt(body[: _METAINFO.size])
    seg_key, pad0, pad1, seg_iv, pad2, pad3 = _METAINFO.unpack(info_pt)
    zero8 = b"\x00" * 8
    if pad0 != zero8 or pad1 != zero8 or pad2 != zero8 or pad3 != zero8:
        raise SelfFormatError("SPKG metadata-info padding is nonzero -- wrong key for this package")

    rest = AES.new(seg_key, AES.MODE_CBC, iv=seg_iv).decrypt(body[_METAINFO.size:])
    _siglen, _sigtype, section_count, key_count, *_ = _METAHDR.unpack_from(rest, 0)
    sec_start = _METAHDR.size
    vault_start = sec_start + section_count * _METASEC.size
    vault = [rest[vault_start + 16 * i: vault_start + 16 * (i + 1)] for i in range(key_count)]

    segs = []
    for i in range(section_count):
        (off, size, _type, seg_idx, _hashtype, _hash_idx,
         encryption, key_idx, iv_idx, compression) = _METASEC.unpack_from(rest, sec_start + i * _METASEC.size)
        if encryption == K.EncryptionType.AES128CTR:
            segs.append(_Segment(off, seg_idx, size, compression == K.CompressionType.DEFLATE,
                                  vault[key_idx], vault[iv_idx]))
    segs.sort(key=lambda s: s.idx)
    return segs


def decrypt_spkg(data: bytes) -> bytes:
    """Decrypt one SPKG-shaped .pkg blob extracted from a PUP (a chunk of a
    firmware filesystem image) into its plaintext bytes."""
    if len(data) < 32 or data[:4] != b"SCE\x00":
        raise SelfFormatError("not an SCE container (bad magic)")
    magic, version, _platform, key_revision, sce_type, metadata_offset, header_length, _data_length = \
        struct.unpack_from("<IIBBHIQQ", data, 0)
    if version != 3:
        raise SelfFormatError(f"unsupported SCE header version {version}")
    if K.SceType(sce_type) != K.SceType.SPKG:
        raise SelfFormatError(f"sce_type={sce_type} is not SPKG")

    segs = _spkg_segments(data, header_length, metadata_offset, key_revision)
    if not segs:
        raise SelfFormatError("no AES128CTR segment found in this SPKG's metadata")

    out = bytearray()
    for seg in segs:
        ciphertext = data[seg.offset: seg.offset + seg.size]
        ctr = Counter.new(128, initial_value=int.from_bytes(seg.iv, "big"))
        plain = AES.new(seg.key, AES.MODE_CTR, counter=ctr).decrypt(ciphertext)
        if seg.compressed:
            plain = zlib.decompress(plain)
        out += plain
    return bytes(out)


def assemble_partition_images(data: bytes, records: list[PupFileRecord]) -> dict[str, bytes]:
    """Decrypt every os0-/vs0- part (in PUP file-table order -- the parts are
    always contiguous and already in order in the table, so this needs no
    re-sorting) and concatenate each partition's plaintext into its raw
    FAT16 image bytes."""
    images: dict[str, bytearray] = {}
    for rec in records:
        for part in _PARTITIONS:
            if rec.name.startswith(part + "-"):
                blob = data[rec.offset: rec.offset + rec.length]
                images.setdefault(part, bytearray()).extend(decrypt_spkg(blob))
    return {k: bytes(v) for k, v in images.items()}


# ---------------------------------------------------------------------------
# SELF decryption over an extracted tree
# ---------------------------------------------------------------------------

def _looks_like_sce(data: bytes) -> bool:
    return len(data) >= 4 and data[:4] == b"SCE\x00"


def _sce_type_of(data: bytes) -> int | None:
    """The 2-byte sce_type field at offset 10 of the generic 32-byte SCE
    header, or None if this isn't an SCE container at all. vs0's data/ tree
    has at least one non-module file (list_launch_teleport.dat) that is
    itself an SPKG container sharing the same magic -- checking the type
    up front skips it cleanly instead of trying and catching a format error."""
    if not _looks_like_sce(data):
        return None
    return struct.unpack_from("<H", data, 10)[0]


def decrypt_tree_selfs(root: str) -> tuple[int, list[str]]:
    """Walk an extracted directory tree and, for every file whose content is
    an SCE SELF container, write a decrypted "<name>.elf" sibling --
    dump/decrypted's own naming rule (see tests/compare_oracle.py). Returns
    (decrypted_count, [relpath: reason, for every SELF that failed to decrypt
    or verify -- NOT for files that simply aren't SELFs])."""
    ok = 0
    failed: list[str] = []
    for dirpath, _dirs, files in sorted(os.walk(root)):
        for name in sorted(files):
            if name.endswith(".elf"):
                continue
            path = os.path.join(dirpath, name)
            with open(path, "rb") as f:
                data = f.read()
            if _sce_type_of(data) != K.SceType.SELF:
                continue
            try:
                elf = sce_self.decrypt_self(data)
            except sce_self.SelfFormatError as exc:
                failed.append(f"{os.path.relpath(path, root)}: {exc}")
                continue
            with open(path + ".elf", "wb") as f:
                f.write(elf)
            ok += 1
    return ok, failed


# ---------------------------------------------------------------------------
# Top-level driver
# ---------------------------------------------------------------------------

def install_pup(pup_path: str, out_dir: str, *, verbose: bool = False) -> dict:
    """Decrypt a PUP into out_dir: the one-off top-level entries (version.txt
    etc, plus any that are themselves SELFs), and out_dir/{os0,vs0}/... trees
    with every extracted file plus a decrypted "<name>.elf" sibling for the
    ones that are SELF modules."""
    with open(pup_path, "rb") as f:
        data = f.read()
    info, records = parse_pup(data)
    os.makedirs(out_dir, exist_ok=True)

    for rec in records:
        if rec.filetype not in PUP_TYPES:
            continue
        blob = data[rec.offset: rec.offset + rec.length]
        with open(os.path.join(out_dir, rec.name), "wb") as f:
            f.write(blob)
        if rec.name.endswith((".self", ".suprx", ".skprx")) and _looks_like_sce(blob):
            try:
                elf = sce_self.decrypt_self(blob)
                with open(os.path.join(out_dir, rec.name + ".elf"), "wb") as f:
                    f.write(elf)
            except sce_self.SelfFormatError as exc:
                if verbose:
                    print(f"warning: could not decrypt {rec.name}: {exc}", file=sys.stderr)

    fw_version_txt = None
    vt_path = os.path.join(out_dir, "version.txt")
    if os.path.exists(vt_path):
        with open(vt_path, "r", errors="replace") as f:
            fw_version_txt = f.read().strip()

    images = assemble_partition_images(data, records)
    results: dict = {"info": info, "fw_version_txt": fw_version_txt, "partitions": {}}
    for part, img_bytes in images.items():
        part_dir = os.path.join(out_dir, part)
        img = fat16.FAT16Image(img_bytes)
        n = img.extract_all(part_dir)
        ok, failures = decrypt_tree_selfs(part_dir)
        results["partitions"][part] = {
            "image_size": len(img_bytes), "file_count": n,
            "self_decrypted": ok, "self_failed": len(failures), "self_failures": failures,
        }
        if verbose:
            print(f"{part}: image {len(img_bytes)} bytes, {n} files, {ok} SELF(s) decrypted, "
                  f"{len(failures)} failed")
    return results


# ---------------------------------------------------------------------------
# Self-test / CLI
# ---------------------------------------------------------------------------

def _build_synthetic_spkg(plaintext: bytes, *, compress: bool = False, key_revision: int = 0) -> bytes:
    """The reverse of decrypt_spkg(): hand-assemble a minimal SCE/SPKG
    container around `plaintext`, encrypted under the real (community-known)
    SPKG metadata key, the same way sce_self's own tests build fake SELFs."""
    meta = _spkg_metadata_key(key_revision)
    payload = zlib.compress(plaintext) if compress else plaintext  # AES-CTR is a stream cipher: no padding needed

    seg_key = b"\x11" * 16
    seg_iv = b"\x22" * 16
    ctr = Counter.new(128, initial_value=int.from_bytes(seg_iv, "big"))
    ciphertext = AES.new(seg_key, AES.MODE_CTR, counter=ctr).encrypt(payload)

    header_length = 0x200
    metadata_offset = 0x50
    # Real SPKGs reserve [header_length : header_length+128) for the
    # SpkgHeader struct (read directly at that offset by get_key_type/
    # make_filename) and start the actual ciphertext only at the fixed
    # HEADER_LENGTH (0x1000) boundary -- mirror that spacing here so a test
    # that later pokes at the SpkgHeader area doesn't corrupt ciphertext.
    seg_offset = K.HEADER_LENGTH
    section_count, key_count = 1, 2
    vault = seg_key + seg_iv
    section = _METASEC.pack(seg_offset, len(ciphertext), 0, 0, 1, -1,
                             K.EncryptionType.AES128CTR, 0, 1, K.CompressionType.DEFLATE if compress else K.CompressionType.NONE)
    metahdr = _METAHDR.pack(0, 0, section_count, key_count, 0, 0, 0)
    rest_plain = metahdr + section + vault
    rest_plain += b"\x00" * ((-len(rest_plain)) % 16)
    rest_ct = AES.new(seg_key, AES.MODE_CBC, iv=seg_iv).encrypt(rest_plain)

    info_plain = _METAINFO.pack(seg_key, b"\x00" * 8, b"\x00" * 8, seg_iv, b"\x00" * 8, b"\x00" * 8)
    info_ct = AES.new(meta.key, AES.MODE_CBC, iv=meta.iv).encrypt(info_plain)

    body = info_ct + rest_ct
    assert metadata_offset + 48 + len(body) <= header_length

    sce_hdr = struct.pack("<IIBBHIQQ", K.SCE_MAGIC, 3, 0x40, key_revision, K.SceType.SPKG,
                           metadata_offset, header_length, header_length + len(ciphertext))
    out = bytearray(sce_hdr)
    out += b"\x00" * (metadata_offset + 48 - len(out))
    out += body
    out += b"\x00" * (K.HEADER_LENGTH - len(out))  # covers the SpkgHeader area too (all zero here)
    out += ciphertext
    return bytes(out)


def _selftest() -> None:
    # decrypt_spkg(): plain and zlib-compressed payloads, real SPKG key.
    for compress in (False, True):
        plaintext = bytes(range(256)) * 37  # 9472 bytes, exercises a multi-cluster-ish size
        blob = _build_synthetic_spkg(plaintext, compress=compress)
        got = decrypt_spkg(blob)
        assert got == plaintext, f"decrypt_spkg round-trip failed (compress={compress})"

    # A wrong key_revision (no such METADATA/SPKG entry) must raise, not
    # silently decrypt garbage.
    try:
        _spkg_metadata_key(99)
        raise AssertionError("expected KeyError for an unregistered SPKG key_revision")
    except KeyError:
        pass

    # parse_pup(): a minimal synthetic PUP with one named entry and one
    # unrecognised blob (no valid SCE header at its offset -> "unknown-0x...").
    version_txt = b"3.65\n"
    unknown_blob = b"\xAA" * 32
    header = bytearray(SCEUF_HEADER_SIZE)
    header[0:5] = b"SCEUF"
    struct.pack_into("<I", header, 8, 2)            # pup_version
    struct.pack_into("<I", header, 0x10, 0x3650000)  # firmware_version
    struct.pack_into("<I", header, 0x14, 123)        # build_number
    struct.pack_into("<I", header, 0x18, 2)          # file count
    body_off = SCEUF_HEADER_SIZE + 2 * SCEUF_FILEREC_SIZE
    rec0 = _SCEUF_FILEREC.pack(0x100, body_off, len(version_txt), 2)
    rec1 = _SCEUF_FILEREC.pack(0x9999, body_off + len(version_txt), len(unknown_blob), 2)
    pup = bytes(header) + rec0 + rec1 + version_txt + unknown_blob

    info, records = parse_pup(pup)
    assert info["fw_version"] == 0x3650000 and info["count"] == 2
    assert records[0].name == "version.txt"
    assert records[1].name == "unknown-0x9999.pkg"

    # _peek_partition_type(): a real (if minimal) SPKG header should resolve
    # to its FSTYPE name via assemble_partition_images()'s "os0-" matching.
    spkg = _build_synthetic_spkg(b"pretend os0 filesystem bytes")
    spkg_type_byte = FSTYPE.index("os0")
    # Patch the SpkgHeader.type field (header_length+4) to SpkgType.OS0 (1).
    spkg = bytearray(spkg)
    header_length, = struct.unpack_from("<Q", spkg, 16)
    struct.pack_into("<I", spkg, header_length + 4, spkg_type_byte)
    spkg = bytes(spkg)

    fake_pup_header = bytearray(SCEUF_HEADER_SIZE)
    fake_pup_header[0:5] = b"SCEUF"
    struct.pack_into("<I", fake_pup_header, 0x18, 1)
    fake_rec = _SCEUF_FILEREC.pack(0x8888, SCEUF_HEADER_SIZE + SCEUF_FILEREC_SIZE, len(spkg), 2)
    # _peek_partition_type() always peeks a full HEADER_LENGTH (0x1000) bytes
    # at the entry's offset, as real retail SPKGs (header_length==0x1000) let
    # it -- pad so this synthetic (deliberately smaller) SPKG's slice is long
    # enough too; the padding itself is never read as SPKG content.
    fake_pup = bytes(fake_pup_header) + fake_rec + spkg
    fake_pup += b"\x00" * (K.HEADER_LENGTH - len(spkg))
    _info2, records2 = parse_pup(fake_pup)
    assert records2[0].name == "os0-00.pkg", f"expected an os0-named part, got {records2[0].name!r}"
    images = assemble_partition_images(fake_pup, records2)
    assert images["os0"] == b"pretend os0 filesystem bytes"

    print("pup.py self-test: OK (SPKG round-trip x2, PUP file-table parsing, partition-type detection)")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="python3 -m vitadecrypt.pup",
                                 description="Decrypt a Vita firmware update file (PSP2UPDAT.PUP) into a dump-shaped tree.")
    p.add_argument("pup", nargs="?")
    p.add_argument("outdir", nargs="?")
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("--selftest", action="store_true")
    args = p.parse_args(argv)

    if args.selftest or not args.pup:
        _selftest()
        return 0
    if not args.outdir:
        p.error("outdir is required unless --selftest is given")

    results = install_pup(args.pup, args.outdir, verbose=args.verbose)
    info = results["info"]
    print(f"PUP: fw_version=0x{info['fw_version']:X} (version.txt: {results['fw_version_txt']!r}) "
          f"pup_version=0x{info['pup_version']:X} build={info['build_number']} entries={info['count']}")
    for part, r in results["partitions"].items():
        print(f"{part}: {r['image_size']} bytes, {r['file_count']} files extracted, "
              f"{r['self_decrypted']} SELF(s) decrypted, {r['self_failed']} failed")
        # os0/sm: the secure processor's own modules -- there are no public keys for them and nothing here runs them
        sm_prefix = "sm" + os.sep
        sm = [f for f in r["self_failures"] if f.startswith(sm_prefix)]
        for f in r["self_failures"]:
            if not f.startswith(sm_prefix):
                print(f"  FAILED {f}")
        if sm:
            print(f"  ({len(sm)} secure-module SELFs under sm/ left encrypted: no keys exist for them, and nothing needs them)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
