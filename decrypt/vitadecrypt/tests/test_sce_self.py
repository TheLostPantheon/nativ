"""Synthetic round-trip self-tests for sce_self.decrypt_self().

compare_oracle.py proves the non-NPDRM path (METADATA key only) against 244
real console-encrypted firmware modules, but none of those are NPDRM "APP"
selfs, so that path (klicensee -> title key -> metadata key, two extra
AES-128-CBC unwraps) has no real-file oracle available here. This builds a
tiny synthetic SELF by hand -- encrypting forward with the exact reverse of
decrypt_self()'s own steps -- for both the plain and NPDRM cases, and checks
that decrypt_self() recovers the chosen plaintext ELF exactly.

A synthetic test can't catch "we both made the same mistake" the way a real
oracle does, but it does catch wiring bugs (wrong key picked, wrong IV reused,
fields read from the wrong offset) independently of the keys.py data itself,
since it supplies its own throwaway key material alongside the real tables
for the one piece (klicensee wrapping) real tables alone can't exercise.

Usage: python3 -m vitadecrypt.tests.test_sce_self
"""
from __future__ import annotations

import struct
import zlib

from Crypto.Cipher import AES
from Crypto.Util import Counter

from vitadecrypt import keys as K
from vitadecrypt.sce_self import (
    _ELF_HDR, _ELF_PHDR, _SCE_HDR, _SELF_HDR, _APPINFO, _SEGINFO,
    _METAINFO, _METAHDR, _METASEC, decrypt_self,
)

PAD16 = b"\x00" * 16


def _pack_elf(segments_plain: list[bytes]) -> tuple[bytes, list[tuple[int, int]]]:
    """A minimal valid-looking Elf32 header + one PT_LOAD phdr per segment,
    laid out back-to-back with no gaps. Returns (full plaintext elf bytes,
    [(p_offset, p_filesz), ...])."""
    n = len(segments_plain)
    ehsize, phentsize = _ELF_HDR.size, _ELF_PHDR.size
    e_phoff = ehsize
    header = _ELF_HDR.pack(b"\x7fELF\x01\x01\x01" + b"\x00" * 9, 2, 0x28, 1, 0x1000, e_phoff, 0, 0, ehsize, phentsize, n, 0, 0, 0)
    offsets = []
    at = ehsize + n * phentsize
    phdrs = b""
    for seg in segments_plain:
        offsets.append((at, len(seg)))
        phdrs += _ELF_PHDR.pack(1, at, at, at, len(seg), len(seg), 5, 1)
        at += len(seg)
    body = b"".join(segments_plain)
    return header + phdrs + body, offsets


def _build_self(segments_plain: list[bytes], compress: list[bool], self_type: K.SelfType, sysver: int,
                 key_revision: int, klicensee: bytes | None) -> bytes:
    """Hand-assemble a SELF container around `segments_plain`, encrypting
    everything the exact reverse of decrypt_self()'s steps so the result is
    something decrypt_self() should be able to undo losslessly."""
    plain_elf, elf_seg_offsets = _pack_elf(segments_plain)
    elf_hdr_bytes = plain_elf[: _ELF_HDR.size]
    phdr_bytes = plain_elf[_ELF_HDR.size: _ELF_HDR.size + len(segments_plain) * _ELF_PHDR.size]
    n = len(segments_plain)

    appinfo_off = 32 + _SELF_HDR.size
    elf_off = appinfo_off + _APPINFO.size   # no control-info records in this synthetic file
    phdr_off = elf_off + _ELF_HDR.size
    seginfo_off = phdr_off + n * _ELF_PHDR.size
    metadata_off = seginfo_off + n * _SEGINFO.size

    # --- per-segment ciphertext + the metadata that will let decrypt_self recover it ---
    vault: list[bytes] = []
    # deterministic-but-distinct 16-byte AES-128-CTR key/iv per segment
    seg_keys = [bytes([(0x10 * i + j) & 0xFF for j in range(16)]) for i in range(n)]
    seg_ivs = [bytes([(0x10 * i + 0x80 + j) & 0xFF for j in range(16)]) for i in range(n)]

    pre_ct_segments = []
    for i, (seg, want_compress) in enumerate(zip(segments_plain, compress)):
        pt = zlib.compress(seg) if want_compress else seg
        ctr = Counter.new(128, initial_value=int.from_bytes(seg_ivs[i], "big"))
        ct = AES.new(seg_keys[i], AES.MODE_CTR, counter=ctr).encrypt(pt)
        pre_ct_segments.append((ct, len(pt)))
        vault.append(seg_keys[i])
        vault.append(seg_ivs[i])

    section_count = n
    key_count = len(vault)
    meta_header = _METAHDR.pack(0, 5, section_count, key_count, 0, 0, 0)
    meta_sections = b""
    for i in range(n):
        key_idx, iv_idx = 2 * i, 2 * i + 1
        meta_sections += _METASEC.pack(
            0, pre_ct_segments[i][1], 0, i, K.HashType.NONE, -1, K.EncryptionType.AES128CTR, key_idx, iv_idx, (
                K.CompressionType.DEFLATE if compress[i] else K.CompressionType.NONE
            ),
        )
    vault_bytes = b"".join(vault)
    rest_plain = meta_header + meta_sections + vault_bytes
    assert len(rest_plain) % 16 == 0, "AES-CBC needs a block-aligned plaintext here"

    seg_key_for_metadata = bytes(range(16))      # the MetadataInfo.key/iv that wrap `rest_plain`
    seg_iv_for_metadata = bytes(range(16, 32))
    rest_ct = AES.new(seg_key_for_metadata, AES.MODE_CBC, iv=seg_iv_for_metadata).encrypt(rest_plain)

    metainfo_plain = _METAINFO.pack(seg_key_for_metadata, b"\x00" * 8, b"\x00" * 8, seg_iv_for_metadata, b"\x00" * 8, b"\x00" * 8)
    meta = K.get_key(K.KeyType.METADATA, K.SceType.SELF, sysver, key_revision, self_type)
    info_ct = AES.new(meta.key, AES.MODE_CBC, iv=meta.iv).encrypt(metainfo_plain)

    if self_type == K.SelfType.APP:
        assert klicensee is not None
        npdrm_rev = 0 if key_revision < 2 else 1
        npdrm = K.get_key(K.KeyType.NPDRM, K.SceType.SELF, sysver, npdrm_rev, K.SelfType.APP)
        title_key = AES.new(npdrm.key, AES.MODE_CBC, iv=npdrm.iv).decrypt(klicensee)
        info_ct = AES.new(title_key, AES.MODE_CBC, iv=npdrm.iv).encrypt(info_ct)

    metadata_body = info_ct + rest_ct
    header_length = metadata_off + 48 + len(metadata_body)

    seg_data_off = header_length
    seginfo_entries = b""
    seg_blob = b""
    for i, (ct, _pt_len) in enumerate(pre_ct_segments):
        # decrypt_self's compression check reads *this* struct's field, not the
        # metadata section's (matching self2elf.py, which does the same).
        compressed_flag = K.SecureBool.YES if compress[i] else K.SecureBool.NO
        seginfo_entries += _SEGINFO.pack(seg_data_off + len(seg_blob), len(ct), compressed_flag, 0, K.SecureBool.NO, 0)
        seg_blob += ct

    elf_filesize = len(plain_elf)
    sce_hdr = _SCE_HDR.pack(K.SCE_MAGIC, 3, 0x40, key_revision, K.SceType.SELF, metadata_off, header_length, elf_filesize)
    # controlinfo_offset/length: no control-info records in this synthetic file
    # (length=0 means decrypt_self's walk loop never dereferences the offset).
    self_hdr = _SELF_HDR.pack(0, 0, 0, appinfo_off, elf_off, phdr_off, 0, seginfo_off, 0, 0, 0)
    appinfo = _APPINFO.pack(0x2F00000000000001, 0, self_type, sysver, 0)

    out = bytearray()
    out += sce_hdr
    assert len(out) == 32
    out += self_hdr
    out += appinfo
    out += elf_hdr_bytes
    out += phdr_bytes
    out += seginfo_entries
    assert len(out) == metadata_off, f"{len(out)} != {metadata_off}"
    out += b"\x00" * 48
    out += metadata_body
    assert len(out) == seg_data_off
    out += seg_blob
    return bytes(out)


def _run_case(name: str, self_type: K.SelfType, sysver: int, key_revision: int, klicensee: bytes | None) -> None:
    segments = [b"hello vita, segment zero! " * 10, b"segment one: not compressed." + bytes(5)]
    compress = [True, False]
    self_bytes = _build_self(segments, compress, self_type, sysver, key_revision, klicensee)
    expected_elf, _ = _pack_elf(segments)

    got = decrypt_self(self_bytes, klicensee=klicensee, verify_digest=False)
    assert got == expected_elf, f"{name}: round-trip mismatch ({len(got)} vs {len(expected_elf)} bytes)"
    print(f"  ok: {name}")


def main() -> None:
    # Non-NPDRM: a plain METADATA-keyed self, like every real firmware module.
    _run_case("USER / keyrev0 / sysver 0x10300000000", K.SelfType.USER, 0x10300000000, 0, None)
    _run_case("SECURE / keyrev1 / sysver 0x18500000000", K.SelfType.SECURE, 0x18500000000, 1, None)

    # NPDRM: an APP self wrapped under a klicensee, for both NPDRM key revisions.
    _run_case("APP / keyrev0 (npdrm key 0) / sysver 0x10300000000", K.SelfType.APP, 0x10300000000, 0, bytes(range(16)))
    _run_case("APP / keyrev2 (npdrm key 1) / sysver 0x18100000000", K.SelfType.APP, 0x18100000000, 2, bytes(range(16, 32)))

    # Wrong klicensee must not silently produce plausible-looking garbage.
    try:
        _build_and_expect_failure()
        raise AssertionError("expected decrypt_self to reject a wrong klicensee")
    except Exception as exc:  # the metadata-info padding check should fire
        assert "padding" in str(exc), f"unexpected failure mode for a wrong klicensee: {exc}"
        print("  ok: wrong klicensee is rejected (metadata padding check)")

    print("test_sce_self.py: OK")


def _build_and_expect_failure() -> None:
    segments = [b"x" * 32]
    self_bytes = _build_self(segments, [False], K.SelfType.APP, 0x10300000000, 0, bytes(range(16)))
    decrypt_self(self_bytes, klicensee=bytes(16), verify_digest=False)  # wrong klicensee (all-zero)


if __name__ == "__main__":
    main()
