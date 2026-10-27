"""Decrypt a PFS-protected Vita content folder (files.db + unicv.db + the
still-encrypted files themselves -- either a NoNpDrm-style dump or pkg.py's
own output) into plain files, given the title's klicensee.

PFS ("protected file system") is the layer *inside* the files PKG/NoNpDrm
hand you: `sce_pfs/files.db` is a small B-tree recording every file/directory
name, size and type; `sce_pfs/unicv.db` holds, per file, one HMAC-SHA1 per
0x8000-byte sector ("ICV" = integrity check value); the real file bytes sit
alongside, AES-128-CBC-encrypted per sector. Both databases are themselves
hash-tree-protected (so a single bit flip anywhere is detectable), and every
key involved is derived from the klicensee -- never stored directly.

There is one more wrinkle, and it is the whole reason psvpfstools calls this
step "F00D": every AES key actually used for CBC encryption/decryption is not
the derived key itself but that key run through one more fixed AES-ECB
"wrap" -- originally done by the Vita's F00D secure co-processor, but (as
Vita3K's own F00DNativeKeyEncryptor.cpp spells out) just a software AES-ECB
*decrypt* under a single constant key. No console, secure element or title-
specific secret beyond the klicensee is needed to reproduce it.

Ported from motoharu-gosuto/psvpfstools' psvpfsparser/ (FilesDbParser.cpp,
UnicvDbParser.cpp/Types.cpp, NodeIcvCalculator.cpp, HashTree.cpp,
SecretGenerator.cpp, PfsKeyGenerator.cpp, PfsCryptEngine*.cpp) and the F00D
"native" key-wrap algorithm from the Vita3K-maintained fork at
github.com/Vita3K/psvpfsparser (F00DNativeKeyEncryptor.cpp) -- the original
repo only ships the (real-console-only) "sealed key"/F00D-url variants.

Scope: this covers "gamedata"/"acid_dir" images (unicv.db -- the format a
PKG-extracted game, patch or DLC actually uses). The separate icv.db/Merkle-
tree format used for savedata and old-style ADDCONT mounts is not
implemented (see the module docstring in game.py for why that is out of
scope here).
"""
from __future__ import annotations

import argparse
import dataclasses
import hashlib
import hmac
import os
import struct

from Crypto.Cipher import AES

# ---------------------------------------------------------------------------
# Fixed key material (psvpfsparser/PfsKeys.cpp, F00DNativeKeyEncryptor.cpp)
# ---------------------------------------------------------------------------

HMAC_KEY0 = bytes.fromhex("E462258B1F31215607 45DB62B1436723D2BF80FE".replace(" ", ""))
HMAC_KEY1 = bytes.fromhex("AFE656BB3C17256A3C809F6E9BF19FDD5A388543")
IV0 = bytes.fromhex("74D20CC39881C213EE770B1010E4BEA7")
CONTRACT_KEY0 = bytes.fromhex("E12213B48016B0E99AB81F8EC02AD4A2")  # F00D "native" wrap key

assert len(HMAC_KEY0) == 0x14 and len(HMAC_KEY1) == 0x14 and len(IV0) == 0x10 and len(CONTRACT_KEY0) == 0x10

PAGE_SIZE = 0x400
FILE_SECTOR_SIZE = 0x8000
SIG_SIZE = 0x14

# files.db file/attribute type bits (FlagOperations.h/FilesDbParser.h)
ATTR_RO, ATTR_SYS1, ATTR_SYS2, ATTR_AC, ATTR_NICV, ATTR_NENC, ATTR_DIR = (
    0x0001, 0x0002, 0x0004, 0x1000, 0x2000, 0x4000, 0x8000,
)
FT_UNEXISTING = 0x0000
FT_NORMAL_FILE = ATTR_RO
FT_NORMAL_DIR = ATTR_DIR
FT_SYS_DIR = ATTR_DIR | ATTR_SYS1 | ATTR_SYS2
FT_UNENC_SYS_RW = ATTR_NENC | ATTR_SYS1 | ATTR_SYS2
FT_ENC_SYS_RW = ATTR_SYS1 | ATTR_SYS2
FT_UNENC_SYS_RO = ATTR_NENC | ATTR_SYS1 | ATTR_SYS2 | ATTR_RO
FT_ENC_SYS_RO = ATTR_SYS1 | ATTR_SYS2 | ATTR_RO
FT_ACID_DIR = ATTR_DIR | ATTR_AC | ATTR_SYS2


def _is_dir(t: int) -> bool:
    return t in (FT_NORMAL_DIR, FT_SYS_DIR, FT_ACID_DIR)


def _is_encrypted(t: int) -> bool:
    return t in (FT_ENC_SYS_RW, FT_ENC_SYS_RO, FT_NORMAL_FILE)


def _is_unencrypted(t: int) -> bool:
    return t in (FT_UNENC_SYS_RW, FT_UNENC_SYS_RO)


class PfsError(ValueError):
    """files.db/unicv.db does not parse, an integrity (ICV/hash-tree) check
    failed, or no real file could be matched to a unicv.db entry -- in every
    case, most likely a wrong klicensee."""


# ---------------------------------------------------------------------------
# Key derivation (SecretGenerator.cpp, PfsKeyGenerator.cpp, F00DNativeKeyEncryptor.cpp)
# ---------------------------------------------------------------------------

def f00d_unwrap(key16: bytes) -> bytes:
    """The one-step "F00D" key wrap: AES-128-ECB *decrypt* under a fixed
    constant. Every AES key PFS content decryption actually uses (both the
    file-content key and, via generate_secret_np, the ICV `secret`) is this
    applied to a value derived from the klicensee -- never the klicensee or
    derived value directly."""
    return AES.new(CONTRACT_KEY0, AES.MODE_ECB).decrypt(key16)


def _cbc_cts(drv_key: bytes, iv: bytes, data: bytes, *, encrypt: bool) -> bytes:
    """AES-128-CBC with ciphertext stealing for a tail shorter than one
    block: full blocks chain normally; the final <16-byte tail is XORed with
    AES-ECB-encrypt(drv_key, <last block's ciphertext, or `iv` if there were
    no full blocks>) -- confirmed against psvpfsparser's LibTomCrypt backend,
    whose cbc_encrypt/cbc_decrypt both update `iv` in place to the running
    ciphertext before this tail step runs (PfsCryptEngineBase.cpp's AESCBC*
    WithKeygen_base, group 1)."""
    size_block = len(data) & ~0xF
    tail = len(data) & 0xF
    out = bytearray(len(data))
    chain_iv = iv
    if size_block:
        cipher = AES.new(drv_key, AES.MODE_CBC, iv=chain_iv)
        block = cipher.encrypt(data[:size_block]) if encrypt else cipher.decrypt(data[:size_block])
        out[:size_block] = block
        # CBC chaining uses the *ciphertext* block either way.
        chain_iv = block[-16:] if encrypt else data[size_block - 16: size_block]
    if tail:
        mask = AES.new(drv_key, AES.MODE_ECB).encrypt(chain_iv)
        for i in range(tail):
            out[size_block + i] = data[size_block + i] ^ mask[i]
    return bytes(out)


def _gen_secret_combo(files_salt: int, icv_salt: int) -> bytes:
    if files_salt == 0:
        msg = struct.pack("<I", icv_salt & 0xFFFFFFFF)
    else:
        msg = struct.pack("<II", files_salt & 0xFFFFFFFF, icv_salt & 0xFFFFFFFF)
    return hmac.new(HMAC_KEY1, msg, hashlib.sha1).digest()


def derive_secret(klicensee: bytes, files_salt: int, icv_salt: int) -> bytes:
    """scePfsUtilGetSecret's gamedata/acid_dir branch (generate_secret_np):
    the 20-byte `secret` used for every ICV (hash-tree and per-sector)
    check, in both files.db and unicv.db."""
    combo = _gen_secret_combo(files_salt, icv_salt)
    drv_key = f00d_unwrap(klicensee)
    return _cbc_cts(drv_key, IV0, combo, encrypt=True)


def derive_file_keys(klicensee: bytes, files_salt: int, icv_salt: int, dbseed: bytes | None) -> tuple[bytes, bytes]:
    """scePfsUtilGetGDKeys/GDKeys2 for gamedata/acid_dir: dec_key is simply
    the klicensee (F00D-wrapped later, inside _cbc_cts via f00d_unwrap);
    tweak_enc_key is an HMAC-SHA1 over either the per-table dbseed (icv
    version 2) or the files_salt/icv_salt pair (version 1) -- never the
    klicensee itself."""
    dec_key = klicensee
    if dbseed is not None:
        tweak_enc_key = hmac.new(HMAC_KEY0, dbseed, hashlib.sha1).digest()[:0x10]
    else:
        if files_salt == 0:
            msg = struct.pack("<I", icv_salt & 0xFFFFFFFF)   # (the salts' 32 bits as psvpfstools' ints hold them: a salt
        else:                                                    # of 2^31 or more -- Shovel Knight's -- packed as signed failed)
            msg = struct.pack("<II", files_salt & 0xFFFFFFFF, icv_salt & 0xFFFFFFFF)
        tweak_enc_key = hmac.new(HMAC_KEY0, msg, hashlib.sha1).digest()[:0x10]
    return dec_key, tweak_enc_key


# ---------------------------------------------------------------------------
# files.db (FilesDbParser.h/.cpp, NodeIcvCalculator.cpp, HashTree.cpp)
# ---------------------------------------------------------------------------

_FILES_HDR = struct.Struct("<8sIHHIIIIQQQ20s20s256s256s416s")
assert _FILES_HDR.size == PAGE_SIZE
_BLOCK_HDR = struct.Struct("<IIII")              # parent_page_number,type,nFiles,padding
_FILE_HDR = struct.Struct("<I68s")               # index,fileName
_FILE_INFO = struct.Struct("<IHHII")             # idx,type,padding0,size,padding1
MAX_FILES_IN_BLOCK = 9
_HASHES_OFFSET = _BLOCK_HDR.size + MAX_FILES_IN_BLOCK * _FILE_HDR.size + (MAX_FILES_IN_BLOCK + 1) * _FILE_INFO.size
assert _HASHES_OFFSET == 824


@dataclasses.dataclass
class FilesDbHeader:
    version: int
    image_spec: int
    key_id: int
    files_salt: int
    root_icv_page_number: int
    root_icv: bytes
    header_icv: bytes
    raw: bytes


def _order_max_avail(page_size: int) -> int:
    def node_size(index: int) -> int:
        return 0x6C * index - 0x38
    index = 1
    while page_size > node_size(index):
        index += 1
    if page_size < node_size(index):
        index -= 1
    return index


def _node_icv(secret: bytes, version: int, order: int, block_hdr: tuple[int, int, int, int] | None, page: bytes) -> bytes:
    if version == 5:
        data_size = 0x6C * order - 0x3C
        return hmac.new(secret, page[4: 4 + data_size], hashlib.sha1).digest()
    assert block_hdr is not None
    _parent, btype, n_files, _pad = block_hdr
    n_entries = n_files + (1 if btype > 0 else 0)
    icv = bytes(20)
    for i in range(n_entries):
        slot = page[_HASHES_OFFSET + i * SIG_SIZE: _HASHES_OFFSET + (i + 1) * SIG_SIZE]
        icv = hmac.new(secret, icv + slot, hashlib.sha1).digest()
    return icv


def _off2page(offset: int) -> int:
    return (offset - PAGE_SIZE) // PAGE_SIZE


@dataclasses.dataclass
class _Block:
    page: int
    parent_page_number: int
    type: int
    n_files: int
    files: list[tuple[int, str]]          # (parent_idx, fileName)
    infos: list[tuple[int, int, int]]     # (idx, type, size) -- m_infos[i] pairs with files[i] for i<nFiles
    hashes: list[bytes]


def _parse_files_db_blocks(data: bytes, header: FilesDbHeader, secret: bytes) -> list[_Block]:
    order = _order_max_avail(PAGE_SIZE)
    blocks: dict[int, _Block] = {}
    pos = PAGE_SIZE
    while pos < len(data):
        page = data[pos: pos + PAGE_SIZE]
        if len(page) != PAGE_SIZE:
            raise PfsError("files.db: truncated trailing page")
        parent_page_number, btype, n_files, padding = _BLOCK_HDR.unpack_from(page, 0)
        if btype not in (0, 1):
            raise PfsError(f"files.db: bad block type {btype} at page offset {pos}")
        if padding != 0:
            raise PfsError("files.db: nonzero block padding")
        if n_files > MAX_FILES_IN_BLOCK:
            raise PfsError(f"files.db: nFiles {n_files} exceeds {MAX_FILES_IN_BLOCK}")

        files = []
        off = _BLOCK_HDR.size
        for i in range(n_files):
            idx, name = _FILE_HDR.unpack_from(page, off)
            files.append((idx, name.split(b"\x00", 1)[0].decode("utf-8", "replace")))
            off += _FILE_HDR.size
        unused = page[off: off + (MAX_FILES_IN_BLOCK - n_files) * _FILE_HDR.size]
        if any(unused):
            raise PfsError("files.db: nonzero padding where unused file headers should be")

        infos = []
        off = _BLOCK_HDR.size + MAX_FILES_IN_BLOCK * _FILE_HDR.size
        for _ in range(MAX_FILES_IN_BLOCK + 1):
            idx, ftype, pad0, size, pad1 = _FILE_INFO.unpack_from(page, off)
            if pad0 != 0 or pad1 != 0:
                raise PfsError("files.db: nonzero file-info padding")
            infos.append((idx, ftype, size))
            off += _FILE_INFO.size

        hashes = [page[_HASHES_OFFSET + i * SIG_SIZE: _HASHES_OFFSET + (i + 1) * SIG_SIZE] for i in range(MAX_FILES_IN_BLOCK + 1)]

        page_number = _off2page(pos)
        blocks[page_number] = _Block(page_number, parent_page_number, btype, n_files, files, infos, hashes)
        pos += PAGE_SIZE

    # Hash-tree walk: every block's own computed ICV must appear in its
    # parent's hash table (validate_hash_tree). Root is checked separately
    # against header.root_icv by the caller.
    by_parent: dict[int, list[int]] = {}
    for b in blocks.values():
        by_parent.setdefault(b.parent_page_number, []).append(b.page)

    def block_icv(block: _Block) -> bytes:
        return _node_icv(secret, header.version, order, (block.parent_page_number, block.type, block.n_files, 0), _rebuild_page_bytes(block))

    def walk(page: int) -> None:
        block = blocks[page]
        for child_page in by_parent.get(page, []):
            child = blocks[child_page]
            child_icv = block_icv(child)
            n_slots = block.n_files + (1 if block.type > 0 else 0)
            if child_icv not in block.hashes[:n_slots]:
                raise PfsError(f"files.db: hash-tree mismatch at page {child_page} (wrong klicensee, or corrupted dump)")
            walk(child_page)

    walk(header.root_icv_page_number)
    return list(blocks.values())


def _rebuild_page_bytes(block: _Block) -> bytes:
    """Only used to re-feed a parsed block back through _node_icv during the
    hash-tree walk; cheaper than keeping every raw page around."""
    buf = bytearray(PAGE_SIZE)
    _BLOCK_HDR.pack_into(buf, 0, block.parent_page_number, block.type, block.n_files, 0)
    off = _BLOCK_HDR.size
    for idx, name in block.files:
        _FILE_HDR.pack_into(buf, off, idx, name.encode("utf-8"))
        off += _FILE_HDR.size
    off = _BLOCK_HDR.size + MAX_FILES_IN_BLOCK * _FILE_HDR.size
    for idx, ftype, size in block.infos:
        _FILE_INFO.pack_into(buf, off, idx, ftype, 0, size, 0)
        off += _FILE_INFO.size
    for i, h in enumerate(block.hashes):
        buf[_HASHES_OFFSET + i * SIG_SIZE: _HASHES_OFFSET + (i + 1) * SIG_SIZE] = h
    return bytes(buf)


@dataclasses.dataclass
class PfsEntry:
    path: str                  # relative, '/'-separated, from the content root
    type: int
    size: int
    idx: int


def parse_files_db(data: bytes, klicensee: bytes) -> tuple[FilesDbHeader, list[PfsEntry], list[PfsEntry]]:
    """Parse+verify sce_pfs/files.db. Returns (header, files, dirs); every
    entry's `.path` is relative to the content root (no leading slash)."""
    if len(data) < PAGE_SIZE:
        raise PfsError("files.db shorter than one page")
    hdr = _FILES_HDR.unpack_from(data, 0)
    (magic, version, image_spec, key_id, page_size, bt_order, root_icv_page_number,
     files_salt, unk6, tail_size, _total_sz, root_icv, header_icv, _rsa0, _rsa1, padding) = hdr
    if magic != b"SCENGPFS":
        raise PfsError("files.db: bad magic")
    if version not in (3, 4, 5):
        raise PfsError(f"files.db: unexpected version {version}")
    if image_spec not in (1, 4):
        raise PfsError(f"files.db: unsupported image_spec {image_spec} (only gamedata/acid_dir are implemented)")
    if key_id != 0:
        raise PfsError("files.db: unexpected key_id")
    if page_size != PAGE_SIZE or bt_order != 0xA:
        raise PfsError("files.db: unexpected page size/tree order")
    if unk6 != 0xFFFFFFFFFFFFFFFF:
        raise PfsError("files.db: unexpected header field")
    if any(padding):
        raise PfsError("files.db: nonzero header padding")
    if tail_size != len(data) - PAGE_SIZE:
        raise PfsError("files.db: header tailSize does not match file size")

    header = FilesDbHeader(version, image_spec, key_id, files_salt, root_icv_page_number, root_icv, header_icv, data[:PAGE_SIZE])
    secret = derive_secret(klicensee, files_salt, 0)

    # Header signature (HMAC over the first 0x160 bytes with header_icv/rsa_sig0 zeroed).
    buf = bytearray(data[:0x160])
    buf[76:96] = bytes(20)
    buf[96:352] = bytes(256)
    if hmac.new(secret, bytes(buf), hashlib.sha1).digest() != header_icv:
        raise PfsError("files.db: header signature is invalid -- wrong klicensee")

    blocks = _parse_files_db_blocks(data, header, secret)
    by_page = {b.page: b for b in blocks}
    root = by_page.get(root_icv_page_number)
    if root is None:
        raise PfsError("files.db: root_icv_page_number does not name a parsed block")
    order = _order_max_avail(PAGE_SIZE)
    root_icv_calc = _node_icv(secret, version, order, (root.parent_page_number, root.type, root.n_files, 0), _rebuild_page_bytes(root))
    if root_icv_calc != root_icv:
        raise PfsError("files.db: root icv is invalid -- wrong klicensee")

    # idx -> (parent_idx, name, type, size) flattened across the LEAF blocks (those no other block names as
    # its parent). An inner node carries copies of its leaves' keys, and its m_infos slots are child-page
    # links, not file infos -- read as files they overwrote real entries with type 0 / size 0 and could make
    # a directory its own parent (Killzone: Mercenary's files.db: 164 leaves, 36 inner nodes, every inner
    # key also in a leaf). A one-block tree's root is its only leaf, whatever its type.
    has_children = {b.parent_page_number for b in blocks}
    flat: dict[int, tuple[int, str, int, int]] = {}
    for b in blocks:
        if b.page in has_children:
            continue
        for i in range(b.n_files):
            parent_idx, name = b.files[i]
            idx, ftype, size = b.infos[i]
            if idx == 0xFFFFFFFF:
                continue
            flat[idx] = (parent_idx, name, ftype, size)

    def full_path(idx: int) -> str:
        parts = []
        cur = idx
        seen = set()
        while cur != 0:
            if cur in seen:
                raise PfsError("files.db: cyclic directory parent chain")
            seen.add(cur)
            parent_idx, name, _t, _s = flat[cur]
            parts.append(name)
            cur = parent_idx
        return "/".join(reversed(parts))

    files, dirs = [], []
    for idx, (parent_idx, name, ftype, size) in flat.items():
        entry = PfsEntry(full_path(idx), ftype, size, idx)
        (dirs if _is_dir(ftype) else files).append(entry)
    return header, files, dirs


# ---------------------------------------------------------------------------
# unicv.db (UnicvDbTypes.h/.cpp, UnicvDbUtils.cpp)
# ---------------------------------------------------------------------------

_IRODB_HDR = struct.Struct("<8sIIIIQ")        # magic,version,blockSize,unk2,unk3,dataSize
_IFTBL_HDR = struct.Struct("<8sIIIII4s20s20s")  # magic,version,pageSize,binTreeNumMaxAvail,nSectors,fileSectorSize,padding,data1,dbseed
assert _IFTBL_HDR.size == 72
_SIGTBL_HDR = struct.Struct("<IIII")          # binTreeSize,sigSize,nSignatures,padding


@dataclasses.dataclass
class UnicvTable:
    icv_salt: int           # the per-table secret/key salt: its own page position in unicv.db
    version: int
    file_sector_size: int
    dbseed: bytes | None    # only meaningful (and only used) when version == 2
    num_sectors: int
    signatures: list[bytes]  # one 20-byte HMAC-SHA1 per sector, in sector order


def parse_unicv_db(data: bytes) -> list[UnicvTable]:
    if len(data) < PAGE_SIZE:
        raise PfsError("unicv.db shorter than one page")
    magic, version, block_size, unk2, unk3, data_size = _IRODB_HDR.unpack_from(data, 0)
    if magic != b"SCEIRODB":
        raise PfsError("unicv.db: bad magic")
    if block_size != PAGE_SIZE or unk2 != 0xFFFFFFFF or unk3 != 0xFFFFFFFF:
        raise PfsError("unicv.db: unexpected header fields")
    if len(data) != data_size + block_size:
        raise PfsError("unicv.db: header dataSize does not match file size")
    if data_size % PAGE_SIZE:
        raise PfsError("unicv.db: data region is not a whole number of pages")

    bin_tree_max = (PAGE_SIZE - _SIGTBL_HDR.size) // SIG_SIZE  # 50, per UnicvDbUtils' binTreeNumMaxAvail

    tables: list[UnicvTable] = []
    pos = PAGE_SIZE
    end = len(data)
    while pos < end:
        page = data[pos: pos + PAGE_SIZE]
        if not any(page):  # unicv.db's own reader skips blank filler pages
            pos += PAGE_SIZE
            continue

        magic8, iver, page_size, bt_max, n_sectors, fsec_size, _pad, _data1, dbseed = _IFTBL_HDR.unpack_from(page, 0)
        if magic8 != b"SCEIFTBL":
            raise PfsError(f"unicv.db: expected SCEIFTBL at offset {pos}, got {magic8!r}")
        if page_size != PAGE_SIZE or bt_max != bin_tree_max or fsec_size != FILE_SECTOR_SIZE:
            raise PfsError("unicv.db: unexpected SCEIFTBL header fields")
        if iver not in (1, 2):
            raise PfsError(f"unicv.db: unexpected table version {iver}")
        if any(_pad):
            raise PfsError("unicv.db: nonzero SCEIFTBL padding field")
        tail = page[_IFTBL_HDR.size:]
        if any(tail):
            raise PfsError("unicv.db: nonzero SCEIFTBL tail padding")

        icv_salt = _off2page(pos + PAGE_SIZE)  # "page" the *data* that follows this header starts at
        pos += PAGE_SIZE

        signatures: list[bytes] = []
        if n_sectors > 0:
            remaining = n_sectors
            while remaining > 0:
                this_page_sigs = min(remaining, bin_tree_max)
                page2 = data[pos: pos + PAGE_SIZE]
                bin_tree_size, sig_size, n_sigs, pad = _SIGTBL_HDR.unpack_from(page2, 0)
                if sig_size != SIG_SIZE or pad != 0:
                    raise PfsError("unicv.db: bad signature-table header")
                if bin_tree_size != bin_tree_max * SIG_SIZE + _SIGTBL_HDR.size:
                    raise PfsError("unicv.db: bad signature-table binTreeSize")
                if n_sigs != this_page_sigs:
                    raise PfsError("unicv.db: signature count mismatch")
                off = _SIGTBL_HDR.size
                for _ in range(n_sigs):
                    signatures.append(page2[off: off + SIG_SIZE])
                    off += SIG_SIZE
                if any(page2[off:]):
                    raise PfsError("unicv.db: nonzero signature-table tail padding")
                pos += PAGE_SIZE
                remaining -= this_page_sigs

        tables.append(UnicvTable(
            icv_salt=icv_salt, version=iver, file_sector_size=fsec_size,
            dbseed=dbseed if iver == 2 else None, num_sectors=n_sectors, signatures=signatures,
        ))
    return tables


# ---------------------------------------------------------------------------
# Matching unicv.db tables to real files, and decrypting (PfsDecryptor.cpp)
# ---------------------------------------------------------------------------

def bruteforce_map(
    tables: list[UnicvTable], real_files: dict[str, bytes], klicensee: bytes, files_salt: int,
) -> dict[int, str]:
    """unicv.db carries no filenames -- only a page-position "salt" per
    table. psvpfstools' own fix is literally this: for each table, compute
    the secret its position implies, HMAC the *first* sector's ciphertext
    under it, and see which (as yet unmatched) real file's first sector
    produces the same HMAC. `real_files` maps relative path -> that file's
    first `file_sector_size`-or-less bytes (as read off disk, still
    encrypted)."""
    remaining = dict(real_files)
    mapping: dict[int, str] = {}
    for t in tables:
        if t.num_sectors == 0 or not t.signatures:
            continue
        secret = derive_secret(klicensee, files_salt, t.icv_salt)
        sig_key = hmac.new(secret, struct.pack("<i", 0), hashlib.sha1).digest()
        target = t.signatures[0]
        found = None
        for path, first_block in remaining.items():
            if hmac.new(sig_key, first_block, hashlib.sha1).digest() == target:
                found = path
                break
        if found is None:
            raise PfsError(f"unicv.db: no real file matches table at icv_salt={t.icv_salt} -- wrong klicensee?")
        mapping[t.icv_salt] = found
        del remaining[found]
    return mapping


def decrypt_pfs_file(content: bytes, table: UnicvTable, klicensee: bytes, files_salt: int, *, decrypt: bool = True) -> bytes:
    """Verify every sector's ICV against unicv.db's signature table -- the
    ICV always covers the on-disk bytes, so this runs even for a PFS member
    that is not AES-encrypted (`decrypt=False`, e.g. an "unencrypted system
    file": `is_noicv`/ATTR_NICV, not `is_noenc`/ATTR_NENC, is what gates the
    ICV check in icv_gd_verify/decrypt_simple). Raises PfsError on the first
    sector mismatch, so a wrong klicensee (or a tampered/corrupted file) is
    caught per sector, not just on the first one."""
    dec_key, tweak_enc_key = derive_file_keys(klicensee, files_salt, table.icv_salt, table.dbseed)
    drv_key = f00d_unwrap(dec_key) if decrypt else None
    secret = derive_secret(klicensee, files_salt, table.icv_salt)

    block_size = table.file_sector_size
    out = bytearray(len(content))
    n_sectors = -(-len(content) // block_size) if content else 0
    if n_sectors != table.num_sectors:
        raise PfsError(f"unicv.db: table has {table.num_sectors} sectors, file implies {n_sectors}")

    for i in range(n_sectors):
        start = i * block_size
        chunk = content[start: start + block_size]

        # Per-sector ICV check (icv_gd_verify): HMAC-SHA1 of the on-disk
        # (ciphertext, or plaintext for an unencrypted member) sector, keyed
        # by HMAC-SHA1(secret, sector_index).
        sig_key = hmac.new(secret, struct.pack("<I", i), hashlib.sha1).digest()
        if hmac.new(sig_key, chunk, hashlib.sha1).digest() != table.signatures[i]:
            raise PfsError(f"sector {i}: ICV mismatch -- wrong klicensee, or corrupted/tampered data")

        if not decrypt:
            out[start: start + len(chunk)] = chunk
            continue

        # tweak = (block_size * sector_index) as a little-endian 128-bit value
        # (upper 64 bits always 0), xored with tweak_enc_key, used as the CBC iv.
        tweak = (block_size * i).to_bytes(8, "little") + bytes(8)
        tweak = bytes(a ^ b for a, b in zip(tweak, tweak_enc_key))
        out[start: start + len(chunk)] = _cbc_cts(drv_key, tweak, chunk, encrypt=False)

    return bytes(out)


# ---------------------------------------------------------------------------
# Whole-tree decrypt (PfsDecryptor.cpp's decrypt_files, minus directory
# creation bookkeeping which os.walk/os.makedirs below does for us)
# ---------------------------------------------------------------------------

_SKIP_TOP = ("sce_pfs",)


def _real_files(root: str) -> dict[str, str]:
    """relative path ('/'-separated) -> absolute path, for every real file
    under `root` except sce_pfs/ and sce_sys/package/ (files.db/unicv.db
    themselves, and the licence, are not PFS members)."""
    out: dict[str, str] = {}
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root).replace(os.sep, "/")
        if rel_dir == ".":
            rel_dir = ""
        if rel_dir in _SKIP_TOP or rel_dir.startswith("sce_pfs/"):
            dirnames[:] = []
            continue
        if rel_dir == "sce_sys" :
            dirnames[:] = [d for d in dirnames if d != "package"]
        for name in filenames:
            rel = f"{rel_dir}/{name}" if rel_dir else name
            out[rel] = os.path.join(dirpath, name)
    return out


@dataclasses.dataclass
class PfsDecryptResult:
    files_decrypted: int
    files_copied: int
    files_empty: int
    dirs_created: int


def decrypt_pfs_tree(root: str, klicensee: bytes, out_dir: str) -> PfsDecryptResult:
    """Decrypt a PFS-protected content root (NoNpDrm dump or pkg.py output):
    `root`/sce_pfs/{files.db,unicv.db} plus the sibling encrypted files,
    writing the plain tree to `out_dir`."""
    with open(os.path.join(root, "sce_pfs", "files.db"), "rb") as f:
        files_db = f.read()
    with open(os.path.join(root, "sce_pfs", "unicv.db"), "rb") as f:
        unicv_db = f.read()

    header, files, dirs = parse_files_db(files_db, klicensee)
    tables = parse_unicv_db(unicv_db)
    real = _real_files(root)

    # bruteforce_map only needs ciphertext files that actually have a
    # unicv.db entry (size > 0, encrypted or unencrypted type doesn't matter --
    # it still occupies a table slot); read just the first sector of each.
    first_sectors = {}
    for rel, abspath in real.items():
        with open(abspath, "rb") as f:
            first_sectors[rel] = f.read(FILE_SECTOR_SIZE)
    page_map = bruteforce_map(tables, first_sectors, klicensee, header.files_salt)
    tables_by_salt = {t.icv_salt: t for t in tables}
    table_by_path = {path: tables_by_salt[salt] for salt, path in page_map.items()}

    n_dec = n_copy = n_empty = n_dirs = 0
    for d in dirs:
        os.makedirs(os.path.join(out_dir, d.path), exist_ok=True)
        n_dirs += 1

    for entry in files:
        dest = os.path.join(out_dir, entry.path)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        if entry.size == 0:
            # Empty files never get a unicv.db table at all (bruteforce_map only
            # considers tables with sectors > 0) -- just create the empty file.
            open(dest, "wb").close()
            n_empty += 1
            continue

        if entry.path not in real:
            raise PfsError(f"files.db lists {entry.path!r} but it is missing from the real filesystem")
        if _is_unencrypted(entry.type):
            decrypt = False
        elif _is_encrypted(entry.type):
            decrypt = True
        else:
            raise PfsError(f"files.db: unexpected file type 0x{entry.type:04X} for {entry.path!r}")

        table = table_by_path.get(entry.path)
        if table is None:
            raise PfsError(f"no unicv.db table matched to {entry.path!r}")

        with open(real[entry.path], "rb") as f:
            on_disk = f.read()
        plain = decrypt_pfs_file(on_disk, table, klicensee, header.files_salt, decrypt=decrypt)
        with open(dest, "wb") as f:
            f.write(plain[: entry.size])
        n_dec += 1 if decrypt else 0
        n_copy += 0 if decrypt else 1

    return PfsDecryptResult(n_dec, n_copy, n_empty, n_dirs)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="python3 -m vitadecrypt.pfs",
        description="Decrypt a PFS-protected Vita content folder into plain files.",
    )
    p.add_argument("root", nargs="?", help="folder containing sce_pfs/{files.db,unicv.db} and the encrypted files")
    p.add_argument("outdir", nargs="?")
    p.add_argument("--klicensee", help="16-byte klicensee, hex-encoded")
    p.add_argument("--selftest", action="store_true")
    args = p.parse_args(argv)

    if args.selftest or not (args.root and args.outdir and args.klicensee):
        _selftest()
        return 0

    result = decrypt_pfs_tree(args.root, bytes.fromhex(args.klicensee), args.outdir)
    print(f"decrypted {result.files_decrypted}, copied {result.files_copied}, "
          f"{result.files_empty} empty, {result.dirs_created} directories -> {args.outdir}")
    return 0


def _build_files_db(klicensee: bytes, files_salt: int, version: int, image_spec: int, entries: list[tuple[int, str, int, int, int]]) -> bytes:
    """Build a byte-exact sce_pfs/files.db with a single root block (good for
    up to 9 files+directories -- plenty for a self-test). `entries` is
    `(parent_idx, name, idx, type, size)` tuples. Used only by the self-test;
    reuses parse_files_db's own `_rebuild_page_bytes`/`_node_icv`/
    `derive_secret` helpers rather than reimplementing them, since what this
    test is actually checking is the *parser's* round trip and its header/
    hash-tree rejection of a wrong key or tampered byte -- not an independent
    crypto oracle, which does not exist locally (see the module docstring)."""
    n = len(entries)
    if n > MAX_FILES_IN_BLOCK:
        raise ValueError("self-test only supports a single root block (<=9 entries)")
    files_list = [(parent_idx, name) for parent_idx, name, _idx, _t, _s in entries]
    infos_list = [(idx, ftype, size) for _p, _n, idx, ftype, size in entries]
    while len(infos_list) < MAX_FILES_IN_BLOCK + 1:
        infos_list.append((0xFFFFFFFF, FT_UNEXISTING, 0))

    root = _Block(page=0, parent_page_number=0xFFFFFFFF, type=1, n_files=n,
                   files=files_list, infos=infos_list, hashes=[bytes(20)] * (MAX_FILES_IN_BLOCK + 1))
    page_bytes = _rebuild_page_bytes(root)

    secret = derive_secret(klicensee, files_salt, 0)
    order = _order_max_avail(PAGE_SIZE)
    root_icv = _node_icv(secret, version, order, (root.parent_page_number, root.type, root.n_files, 0), page_bytes)

    header = bytearray(_FILES_HDR.pack(
        b"SCENGPFS", version, image_spec, 0, PAGE_SIZE, 0xA, 0, files_salt,
        0xFFFFFFFFFFFFFFFF, PAGE_SIZE, 0, root_icv, bytes(20), bytes(256), bytes(256), bytes(416),
    ))
    # header[:0x160] already has header_icv/rsa_sig0 zeroed above; HMAC it,
    # then patch the real header_icv in afterwards (mirrors verify_header_icv).
    header_icv = hmac.new(secret, bytes(header[:0x160]), hashlib.sha1).digest()
    header[76:96] = header_icv
    return bytes(header) + page_bytes


def _build_unicv_db(spec: list[dict]) -> bytes:
    """Build a byte-exact sce_pfs/unicv.db from a list of
    `{"num_sectors", "version", "dbseed", "make_sigs"}` dicts, one per file
    table, in on-disk order. `make_sigs(icv_salt)` is called once position
    (hence icv_salt) is known, and must return exactly `num_sectors` 20-byte
    signatures over that file's on-disk (ciphertext, or plaintext if
    unencrypted) sector bytes."""
    bin_tree_max = (PAGE_SIZE - _SIGTBL_HDR.size) // SIG_SIZE
    body = bytearray()
    pos = PAGE_SIZE
    for item in spec:
        icv_salt = pos // PAGE_SIZE  # matches _off2page(pos + PAGE_SIZE) in parse_unicv_db
        sigs = item["make_sigs"](icv_salt)
        if len(sigs) != item["num_sectors"]:
            raise ValueError("make_sigs returned the wrong number of signatures")
        hdr = _IFTBL_HDR.pack(b"SCEIFTBL", item["version"], PAGE_SIZE, bin_tree_max, item["num_sectors"],
                               FILE_SECTOR_SIZE, bytes(4), bytes(20), item["dbseed"] or bytes(20))
        body += hdr + bytes(PAGE_SIZE - len(hdr))
        pos += PAGE_SIZE

        remaining, si = item["num_sectors"], 0
        while remaining > 0:
            take = min(remaining, bin_tree_max)
            sig_hdr = _SIGTBL_HDR.pack(bin_tree_max * SIG_SIZE + _SIGTBL_HDR.size, SIG_SIZE, take, 0)
            page = sig_hdr + b"".join(sigs[si: si + take])
            body += page + bytes(PAGE_SIZE - len(page))
            pos += PAGE_SIZE
            si += take
            remaining -= take

    top = _IRODB_HDR.pack(b"SCEIRODB", 2, PAGE_SIZE, 0xFFFFFFFF, 0xFFFFFFFF, len(body))
    return top + bytes(PAGE_SIZE - len(top)) + bytes(body)


def _sector_sigs(secret: bytes, sectors: list[bytes]) -> list[bytes]:
    out = []
    for i, chunk in enumerate(sectors):
        key = hmac.new(secret, struct.pack("<I", i), hashlib.sha1).digest()
        out.append(hmac.new(key, chunk, hashlib.sha1).digest())
    return out


def _chunks(data: bytes, size: int) -> list[bytes]:
    return [data[i: i + size] for i in range(0, len(data), size)] or [b""]


def _selftest() -> None:
    import tempfile

    klic = bytes.fromhex("00112233445566778899aabbccddeeff")[:16]
    files_salt = 0x12345678
    block_size = FILE_SECTOR_SIZE

    sfo_plain = (b"PARAM.SFO-fake-bytes-for-self-test" * 3)   # unencrypted "system" member, < 1 sector
    save_plain = os.urandom(block_size + 7000)                 # encrypted, spans a full sector + a tail

    make_sfo_sigs = lambda icv_salt: _sector_sigs(derive_secret(klic, files_salt, icv_salt), _chunks(sfo_plain, block_size))  # noqa: E731

    save_cipher_box: dict[str, bytes] = {}

    def make_save_sigs(icv_salt: int) -> list[bytes]:
        dec_key, tweak_enc_key = derive_file_keys(klic, files_salt, icv_salt, None)
        drv_key = f00d_unwrap(dec_key)
        plain_sectors = _chunks(save_plain, block_size)
        cipher = bytearray()
        for i, chunk in enumerate(plain_sectors):
            tweak = bytes(a ^ b for a, b in zip((block_size * i).to_bytes(8, "little") + bytes(8), tweak_enc_key))
            cipher += _cbc_cts(drv_key, tweak, chunk, encrypt=True)
        save_cipher_box["bytes"] = bytes(cipher)
        return _sector_sigs(derive_secret(klic, files_salt, icv_salt), _chunks(bytes(cipher), block_size))

    unicv_bytes = _build_unicv_db([
        {"num_sectors": len(_chunks(sfo_plain, block_size)), "version": 1, "dbseed": None, "make_sigs": make_sfo_sigs},
        {"num_sectors": len(_chunks(save_plain, block_size)), "version": 1, "dbseed": None, "make_sigs": make_save_sigs},
    ])
    save_cipher = save_cipher_box["bytes"]

    entries = [
        (0, "sce_sys", 1, FT_SYS_DIR, 0),
        (1, "param.sfo", 2, FT_UNENC_SYS_RO, len(sfo_plain)),
        (0, "data", 3, FT_NORMAL_DIR, 0),
        (3, "save.dat", 4, FT_NORMAL_FILE, len(save_plain)),
        (3, "empty.bin", 5, FT_NORMAL_FILE, 0),
    ]
    files_db_bytes = _build_files_db(klic, files_salt, version=4, image_spec=1, entries=entries)

    with tempfile.TemporaryDirectory() as tmp:
        root = os.path.join(tmp, "root")
        os.makedirs(os.path.join(root, "sce_pfs"))
        with open(os.path.join(root, "sce_pfs", "files.db"), "wb") as f:
            f.write(files_db_bytes)
        with open(os.path.join(root, "sce_pfs", "unicv.db"), "wb") as f:
            f.write(unicv_bytes)
        os.makedirs(os.path.join(root, "sce_sys"))
        with open(os.path.join(root, "sce_sys", "param.sfo"), "wb") as f:
            f.write(sfo_plain)  # unencrypted member: on-disk bytes == plaintext
        os.makedirs(os.path.join(root, "data"))
        with open(os.path.join(root, "data", "save.dat"), "wb") as f:
            f.write(save_cipher)
        open(os.path.join(root, "data", "empty.bin"), "wb").close()

        out_dir = os.path.join(tmp, "out")
        result = decrypt_pfs_tree(root, klic, out_dir)
        assert (result.files_decrypted, result.files_copied, result.files_empty, result.dirs_created) == (1, 1, 1, 2), result

        assert open(os.path.join(out_dir, "sce_sys", "param.sfo"), "rb").read() == sfo_plain
        assert open(os.path.join(out_dir, "data", "save.dat"), "rb").read() == save_plain
        assert open(os.path.join(out_dir, "data", "empty.bin"), "rb").read() == b""

        # A wrong klicensee must fail closed at the files.db header check,
        # not silently produce garbage output.
        wrong = bytes(b ^ 1 for b in klic)
        try:
            decrypt_pfs_tree(root, wrong, os.path.join(tmp, "out_wrong"))
            raise AssertionError("expected PfsError for a wrong klicensee")
        except PfsError:
            pass

        # A single flipped byte in an encrypted file's ciphertext must be
        # caught by the per-sector ICV check, not decrypted into garbage.
        save_path = os.path.join(root, "data", "save.dat")
        with open(save_path, "r+b") as f:
            f.seek(10)
            b = f.read(1)
            f.seek(10)
            f.write(bytes([b[0] ^ 0xFF]))
        try:
            decrypt_pfs_tree(root, klic, os.path.join(tmp, "out_tamper"))
            raise AssertionError("expected PfsError for a tampered sector")
        except PfsError:
            pass

    print("pfs.py self-test: OK")


if __name__ == "__main__":
    raise SystemExit(main())
