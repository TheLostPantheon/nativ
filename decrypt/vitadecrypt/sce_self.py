"""Decrypt a Vita SCE SELF into a plain ELF.

Covers system binaries (.self/.suprx/.skprx) and NPDRM app binaries (eboot.bin,
a game's own .suprx) given the title's klicensee. This is the same algorithm
Vita3K's decrypt_fself() runs (vita3k/packages/src/sce_utils.cpp), which is
itself a C++ port of TeamMolecule's sceutils (self2elf.py/scedecrypt.py); the
struct layouts below come from both of those plus vitasdk/vita-toolchain's
self.h, cross-checked against each other field-by-field.

A SELF is: a small fixed header (SCE header + SELF header + app-info), an
*unencrypted* copy of the real ELF's header and program headers, a "metadata"
block that is encrypted and holds a per-segment AES-128-CTR key/IV, and then
the program headers' file data -- each segment separately encrypted and
optionally zlib-compressed. Decrypting one is: pull the per-segment keys out
of the metadata block (itself wrapped under a static per-firmware-revision
key, and for NPDRM apps wrapped *again* under the title's klicensee), then
AES-CTR-decrypt and inflate each segment into place.
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import zlib
from collections import namedtuple

from Crypto.Cipher import AES
from Crypto.Util import Counter

from . import keys as K

# ---------------------------------------------------------------------------
# Raw struct layouts. Field names/sizes cross-checked against:
#   - Vita3K packages/include/packages/sce_types.h (SceHeader, SelfHeader, ...)
#   - vitasdk/vita-toolchain src/self.h (the original packed C structs)
#   - TeamMolecule/sceutils scetypes.py (the Python port Vita3K's code credits)
# All three agree byte-for-byte; sce_types.h just splits self.h's one 128-byte
# SCE_header into two reads (the first 32 bytes, then 88 more from offset 32).
# ---------------------------------------------------------------------------

_SCE_HDR = struct.Struct("<IIBBHIQQ")        # magic,version,platform,keyrev,scetype,metaoff,hdrlen,elfsize (32B)
_SELF_HDR = struct.Struct("<" + "Q" * 11)    # file_len,_,self_off,appinfo_off,elf_off,phdr_off,shdr_off,seginfo_off,scever_off,ctrlinfo_off,ctrlinfo_len (88B, @32)
_APPINFO = struct.Struct("<QIIQQ")           # auth_id,vendor_id,self_type,sys_version,_ (32B)
_ELF_HDR = struct.Struct("<16sHHIIIIIHHHHHH")  # standard Elf32_Ehdr (52B)
_ELF_PHDR = struct.Struct("<IIIIIIII")       # standard Elf32_Phdr (32B)
_SEGINFO = struct.Struct("<QQIIII")          # offset,size,compressed,_,plaintext,_ (32B)
_METAINFO = struct.Struct("<16s8s8s16s8s8s")  # key,pad,pad,iv,pad,pad (64B)
_METAHDR = struct.Struct("<QIIIIII")         # siglen,sigtype,section_count,key_count,opthdrsz,_,_ (32B)
_METASEC = struct.Struct("<QQIiIiIiiI")      # offset,size,type,seg_idx,hashtype,hash_idx,encryption,key_idx,iv_idx,compression (48B)
_CTRLINFO = struct.Struct("<IIQ")            # type,size,more (16B) -- generic control-info record header
_CTRLDIGEST_HASH = struct.Struct("<20s32s")  # sce_hash,file_hash, within a DIGEST_SHA256 record's body

ELF_MAGIC = b"\x7fELF"
_Segment = namedtuple("Segment", "offset idx size compressed key iv")


class SelfFormatError(ValueError):
    """The input isn't a SELF this module understands, or decryption produced
    something inconsistent with the file's own metadata (wrong key/klicensee,
    or a bug in this port)."""


def _read_control_info(data: bytes, ctrlinfo_offset: int, ctrlinfo_length: int) -> tuple[bytes | None, int]:
    """Walk the chain of control-info records (self_hdr.controlinfo_offset,
    length ctrlinfo_length). Each record is a generic 16-byte header (type,
    size, more-flag) followed by `size - 16` bytes of type-specific body; a
    zero more-flag (or zero size) marks the last record. Returns
    (expected_sha256_of_final_elf_or_None, npdrm_type)."""
    expected_digest = None
    npdrm_type = 0
    off = ctrlinfo_offset
    end = ctrlinfo_offset + ctrlinfo_length
    while off < end:
        ctype, csize, more = _CTRLINFO.unpack_from(data, off)
        body = off + _CTRLINFO.size
        if ctype == K.ControlType.DIGEST_SHA256 and csize >= _CTRLINFO.size + _CTRLDIGEST_HASH.size:
            _sce_hash, file_hash = _CTRLDIGEST_HASH.unpack_from(data, body)
            expected_digest = file_hash
        elif ctype == K.ControlType.NPDRM_VITA and csize >= _CTRLINFO.size + 12:
            # SceControlInfoDRM has its own little sub-header (magic,sig_off,size,
            # npdrm_type,...) right at the start of the body; npdrm_type is u32 @ offset 8.
            npdrm_type, = struct.unpack_from("<I", data, body + 8)
        if not more or csize == 0:
            break
        off += csize
    return expected_digest, npdrm_type


def _get_segments(
    data: bytes,
    header_length: int,
    metadata_offset: int,
    key_revision: int,
    sysver: int,
    self_type: K.SelfType,
    klicensee: bytes | None,
) -> list[_Segment]:
    """packages/src/sce_utils.cpp:get_segments(). Decrypts the metadata block
    that sits right after the SELF's plaintext headers and returns the
    per-segment AES-128-CTR (offset, idx, size, compressed, key, iv) tuples,
    in metadata order (position i's `.idx` says which program header / segment
    it actually describes -- see the loop in decrypt_self)."""
    body = data[metadata_offset + 48: header_length]
    meta = K.get_key(K.KeyType.METADATA, K.SceType.SELF, sysver, key_revision, self_type)

    info_ct = body[: _METAINFO.size]
    if self_type == K.SelfType.APP:
        # NPDRM: the metadata-info block is wrapped a second time under the
        # title's klicensee, which itself arrives wrapped under one of two
        # fixed keys (picked by key_revision, not by sysver).
        if not klicensee or len(klicensee) != 16:
            raise SelfFormatError("this SELF is NPDRM-wrapped (APP); a 16-byte klicensee is required")
        npdrm_rev = 0 if key_revision < 2 else 1
        npdrm = K.get_key(K.KeyType.NPDRM, K.SceType.SELF, sysver, npdrm_rev, K.SelfType.APP)
        title_key = AES.new(npdrm.key, AES.MODE_CBC, iv=npdrm.iv).decrypt(klicensee)
        info_ct = AES.new(title_key, AES.MODE_CBC, iv=npdrm.iv).decrypt(info_ct)

    info_pt = AES.new(meta.key, AES.MODE_CBC, iv=meta.iv).decrypt(info_ct)
    seg_key, pad0, pad1, seg_iv, pad2, pad3 = _METAINFO.unpack(info_pt)
    zero8 = b"\x00" * 8
    if pad0 != zero8 or pad1 != zero8 or pad2 != zero8 or pad3 != zero8:
        raise SelfFormatError("metadata info padding is nonzero -- wrong key/klicensee for this SELF")

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
            segs.append(_Segment(off, seg_idx, size, compression == K.CompressionType.DEFLATE, vault[key_idx], vault[iv_idx]))
    return segs


def decrypt_self(data: bytes, klicensee: bytes | None = None, *, verify_digest: bool = True) -> bytes:
    """Decrypt an SCE SELF (.self/.suprx/.skprx, or an NPDRM eboot.bin/.suprx
    given its klicensee) into a plain ELF.

    Mirrors self2elf.py/decrypt_fself()'s reconstruction: the embedded ELF
    header and program headers are copied verbatim (only segment *payloads*
    are ever encrypted), and the output is sized to the SELF's own recorded
    elf_filesize -- same as TeamMolecule/fself2elf.py's pre-sized buffer, so a
    trailing gap between the last segment and elf_filesize comes out
    zero-filled rather than truncated, matching that oracle byte-for-byte.

    Raises SelfFormatError on anything that doesn't look like a decryptable
    Vita SELF, including a digest mismatch (see `verify_digest`).
    """
    if len(data) < 32 or data[:4] != b"SCE\x00":
        raise SelfFormatError("not an SCE file (bad magic)")

    magic, version, _platform, key_revision, sce_type, metadata_offset, header_length, elf_filesize = \
        _SCE_HDR.unpack_from(data, 0)
    if version != 3:
        raise SelfFormatError(f"unsupported SCE header version {version}")
    sce_type = K.SceType(sce_type)
    if sce_type != K.SceType.SELF:
        raise SelfFormatError(f"sce_type={sce_type!r} is not SELF (PKG/PUP containers use a different layout)")

    (_file_length, _field_8, _self_offset, appinfo_offset, elf_offset, phdr_offset,
     _shdr_offset, segment_info_offset, _sceversion_offset, controlinfo_offset,
     controlinfo_length) = _SELF_HDR.unpack_from(data, 32)

    _auth_id, _vendor_id, self_type_raw, sys_version, _field_18 = _APPINFO.unpack_from(data, appinfo_offset)
    self_type = K.SelfType(self_type_raw)

    expected_digest, _npdrm_type = _read_control_info(data, controlinfo_offset, controlinfo_length)

    elf_hdr_bytes = data[elf_offset: elf_offset + _ELF_HDR.size]
    if elf_hdr_bytes[:4] != ELF_MAGIC:
        raise SelfFormatError("embedded ELF header has a bad magic -- offsets likely wrong for this SELF layout")
    (_e_ident, e_type, e_machine, e_version, _e_entry, e_phoff, _e_shoff,
     _e_flags, _e_ehsize, e_phentsize, e_phnum, *_rest) = _ELF_HDR.unpack(elf_hdr_bytes)
    if e_phentsize != _ELF_PHDR.size:
        raise SelfFormatError(f"unexpected e_phentsize {e_phentsize} (expected {_ELF_PHDR.size})")

    phdrs = [_ELF_PHDR.unpack_from(data, phdr_offset + i * _ELF_PHDR.size) for i in range(e_phnum)]
    seg_infos = [_SEGINFO.unpack_from(data, segment_info_offset + i * _SEGINFO.size) for i in range(e_phnum)]
    any_encrypted = any(plaintext == K.SecureBool.NO for (_o, _s, _c, _f14, plaintext, _f1c) in seg_infos)

    segs = (
        _get_segments(data, header_length, metadata_offset, key_revision, sys_version, self_type, klicensee)
        if any_encrypted
        else []
    )
    if segs and len(segs) != e_phnum:
        raise SelfFormatError(
            f"metadata describes {len(segs)} encrypted sections but the ELF has {e_phnum} program headers"
        )

    out = bytearray(elf_filesize)
    out[0:_ELF_HDR.size] = elf_hdr_bytes
    out[e_phoff: e_phoff + e_phnum * _ELF_PHDR.size] = data[phdr_offset: phdr_offset + e_phnum * _ELF_PHDR.size]

    for i in range(e_phnum):
        idx = segs[i].idx if segs else i
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = phdrs[idx]
        if p_filesz == 0:
            continue
        s_off, s_size, s_compressed, _f14, s_plaintext, _f1c = seg_infos[idx]
        raw = data[s_off: s_off + s_size]
        if s_plaintext == K.SecureBool.NO:
            seg = segs[i]
            ctr = Counter.new(128, initial_value=int.from_bytes(seg.iv, "big"))
            raw = AES.new(seg.key, AES.MODE_CTR, counter=ctr).decrypt(raw)
        if s_compressed == K.SecureBool.YES:
            raw = zlib.decompress(raw)
        if len(raw) != p_filesz:
            raise SelfFormatError(f"segment {idx}: decrypted size {len(raw)} != ELF p_filesz 0x{p_filesz:X}")
        out[p_offset: p_offset + p_filesz] = raw

    result = bytes(out)
    if verify_digest and expected_digest is not None:
        got = hashlib.sha256(result).digest()
        if got != expected_digest:
            raise SelfFormatError(
                "decrypted ELF does not match the SELF's own embedded SHA-256 digest "
                f"(got {got.hex()}, expected {expected_digest.hex()}) -- wrong key/klicensee, or a bug in this port"
            )
    return result


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="python3 -m vitadecrypt.sce_self",
        description="Decrypt a Vita SCE SELF (.self/.suprx/.skprx or an NPDRM eboot.bin) into a plain ELF.",
    )
    p.add_argument("input")
    p.add_argument("output")
    p.add_argument("--klicensee", help="16-byte klicensee, hex-encoded (required for NPDRM APP selfs)")
    p.add_argument("--no-verify", action="store_true", help="skip the embedded SHA-256 digest check")
    args = p.parse_args(argv)

    with open(args.input, "rb") as f:
        data = f.read()
    klic = bytes.fromhex(args.klicensee) if args.klicensee else None
    try:
        elf = decrypt_self(data, klic, verify_digest=not args.no_verify)
    except SelfFormatError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    with open(args.output, "wb") as f:
        f.write(elf)
    print(f"wrote {len(elf)} bytes to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
