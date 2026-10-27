"""Minimal, standards-correct FAT16 reader for Vita firmware partition images
(os0.img, vs0.img -- the plaintext produced by decrypting a PUP's SPKG
segments; see pup.py). Nothing here is Vita-specific; it's a plain FAT16
volume.

Written fresh rather than ported line-for-line from Vita3K's external/libfat16
submodule (cloned read-only for reference at ../refs/libfat16 in this task's
scratch dir -- the actual checkout in this repo's Vita3K clone is an empty,
uninitialised git submodule). That reader has two real quirks worth not
copying:
  - it bounds a *subdirectory's* entry scan by the ROOT directory's own entry
    count (BootBlock::num_root_dirs) instead of the subdirectory's actual
    cluster-chain length;
  - Image::read_from_cluster() ignores its read callback's return value and
    always reports success, so a short/failed read doesn't stop the walk.
On a real image this still produces correct output -- for the depths of
nesting Vita firmware actually uses, it just keeps marching forward
(re-reading the same stale buffer) until a physical EOF read fails -- but
it's an accident of how it's driven (always from a real file on disk, always
asking for whole sectors), not a FAT16 reader worth copying bug-for-bug.
This module instead follows the standard algorithm: a cluster chain ends at
the FAT's own end-of-chain marker (>=0xFFF8), and a directory ends at an
all-zero first byte, exactly as the FAT spec (Microsoft's fatgen103) says.

The on-disk structs below (boot sector BPB fields, the 32-byte short
directory entry, the 32-byte VFAT long-filename entry) were cross-checked
against Vita3K's BootBlock/FundamentalEntry/LongFileNameEntry and agree with
them byte-for-byte; long filenames are common in os0/vs0 (many module names
exceed 8.3), so VFAT LFN entries are decoded, not skipped.
"""
from __future__ import annotations

import argparse
import dataclasses
import os
import struct
import sys

# First 36 bytes of the BIOS Parameter Block (BPB); the rest of the 512-byte
# boot sector (bootstrap code, 0x55AA signature) isn't needed for reading.
_BOOT_SECTOR = struct.Struct("<3s8sHBHBHHBHHHII")
# A regular (short/8.3) 32-byte directory entry. The 10 "reserved" bytes are
# NT reserved/create-time/create-date/last-access-date/first-cluster-hi --
# all FAT32-only or cosmetic, so (like Vita3K) this reader ignores them.
_SHORT_ENTRY = struct.Struct("<8s3sB10sHHHI")
# A VFAT long-filename entry (always attr=0x0F). first_cluster_lo is always
# 0 for these and unused here.
_LFN_ENTRY = struct.Struct("<B10sBBB12sH4s")

_ATTR_LFN = 0x0F
_ATTR_DIRECTORY = 0x10
_ATTR_VOLUME_ID = 0x08
_BOOT_SIG_OFFSET = 0x1FE
_BOOT_SIG = b"\x55\xAA"


class Fat16Error(ValueError):
    """Not a FAT16 image this reader understands, or a corrupt cluster chain."""


@dataclasses.dataclass(frozen=True)
class BootSector:
    bytes_per_sector: int
    sectors_per_cluster: int
    reserved_sectors: int
    num_fats: int
    root_entries: int
    sectors_per_fat: int
    total_sectors: int

    @property
    def fat_start(self) -> int:
        return self.reserved_sectors * self.bytes_per_sector

    @property
    def root_dir_start(self) -> int:
        return self.fat_start + self.num_fats * self.sectors_per_fat * self.bytes_per_sector

    @property
    def root_dir_size(self) -> int:
        return self.root_entries * 32

    @property
    def data_start(self) -> int:
        return self.root_dir_start + self.root_dir_size

    @property
    def cluster_size(self) -> int:
        return self.bytes_per_sector * self.sectors_per_cluster


def parse_boot_sector(data: bytes) -> BootSector:
    if len(data) < 512 or data[_BOOT_SIG_OFFSET:_BOOT_SIG_OFFSET + 2] != _BOOT_SIG:
        raise Fat16Error("missing FAT boot sector signature (0x55AA at offset 0x1FE)")
    (_jmp, _oem, bps, spc, rsvd, nfat, rootent, ts16, _media, spf16,
     _spt, _heads, _hidden, ts32) = _BOOT_SECTOR.unpack_from(data, 0)
    if bps == 0 or spc == 0 or nfat == 0:
        raise Fat16Error("degenerate BPB (zero bytes-per-sector/sectors-per-cluster/num-fats)")
    return BootSector(bps, spc, rsvd, nfat, rootent, spf16, ts16 or ts32)


@dataclasses.dataclass(frozen=True)
class DirEntry:
    name: str
    is_dir: bool
    start_cluster: int
    size: int


def _decode_lfn_chars(raw: bytes) -> str:
    """raw is the 26 bytes of name1+name2+name3 (13 UTF-16LE chars); stops at
    the first NUL or 0xFFFF padding char."""
    chars = struct.unpack_from("<13H", raw, 0)
    out = []
    for c in chars:
        if c in (0x0000, 0xFFFF):
            break
        out.append(chr(c))
    return "".join(out)


def _decode_short_name(name8: bytes, ext3: bytes) -> str:
    raw = bytearray(name8)
    if raw[0] == 0x05:
        # 0x05 is how a filename whose real first byte is 0xE5 (which would
        # otherwise look like a deleted-entry marker) gets stored.
        raw[0] = 0xE5
    base = raw.decode("cp437", errors="replace").rstrip(" ")
    ext = ext3.decode("cp437", errors="replace").rstrip(" ")
    return f"{base}.{ext}" if ext else base


def parse_dir_entries(region: bytes) -> list[DirEntry]:
    """Parse one directory's worth of raw 32-byte slots (either the root
    directory's fixed region, or a subdirectory's full cluster chain) into
    DirEntry records, combining VFAT long-filename entries with the short
    entry that follows them. A 0x00 first byte ends the directory early."""
    entries: list[DirEntry] = []
    pending: dict[int, str] = {}
    n = len(region) // 32
    for i in range(n):
        raw = region[i * 32: i * 32 + 32]
        first = raw[0]
        if first == 0x00:
            break  # no more entries allocated in this directory
        attr = raw[11]
        if attr == _ATTR_LFN:
            ordv, n1, _a, _t, _chk, n2, _cl, n3 = _LFN_ENTRY.unpack(raw)
            if ordv & 0x40:
                pending = {}  # 0x40 marks the last (i.e. first-encountered) LFN entry of a new name
            pending[ordv & 0x3F] = _decode_lfn_chars(n1 + n2 + n3)
            continue
        if first == 0xE5:
            pending = {}  # deleted entry
            continue
        name8, ext3, fattr, _resv, _mtime, _mdate, start_cl, fsize = _SHORT_ENTRY.unpack(raw)
        long_name = "".join(pending[k] for k in sorted(pending)) if pending else None
        pending = {}
        if fattr & _ATTR_VOLUME_ID:
            continue
        short_name = _decode_short_name(name8, ext3)
        if short_name in (".", ".."):
            continue  # "." / ".." markers (filename[0] == 0x2E), not real children
        entries.append(DirEntry(long_name or short_name, bool(fattr & _ATTR_DIRECTORY), start_cl, fsize))
    return entries


class FAT16Image:
    """A FAT16 volume held entirely in memory (os0.img/vs0.img are a few tens
    to a few hundred MB -- small enough to just load once)."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        self.boot = parse_boot_sector(data)

    def _fat_entry(self, cluster: int) -> int:
        off = self.boot.fat_start + cluster * 2
        return struct.unpack_from("<H", self.data, off)[0]

    def _chain(self, start_cluster: int):
        """Yield each cluster number in start_cluster's chain, stopping at
        the FAT's end-of-chain marker (cluster values 0xFFF8-0xFFFF)."""
        cluster = start_cluster
        seen: set[int] = set()
        while 0x0002 <= cluster <= 0xFFEF:
            if cluster in seen:
                raise Fat16Error(f"FAT16 cluster chain loop at cluster {cluster}")
            seen.add(cluster)
            yield cluster
            cluster = self._fat_entry(cluster)

    def _cluster_bytes(self, cluster: int) -> bytes:
        off = self.boot.data_start + (cluster - 2) * self.boot.cluster_size
        return self.data[off: off + self.boot.cluster_size]

    def _read_chain(self, start_cluster: int, size: int | None = None) -> bytes:
        buf = bytearray()
        for c in self._chain(start_cluster):
            buf += self._cluster_bytes(c)
            if size is not None and len(buf) >= size:
                break
        return bytes(buf[:size]) if size is not None else bytes(buf)

    def listdir(self, cluster: int | None = None) -> list[DirEntry]:
        """Entries of the root directory (cluster=None) or of the
        subdirectory starting at `cluster`."""
        if cluster is None:
            region = self.data[self.boot.root_dir_start: self.boot.root_dir_start + self.boot.root_dir_size]
        else:
            region = self._read_chain(cluster)  # a directory's own size is just its whole chain
        return parse_dir_entries(region)

    def read_file(self, entry: DirEntry) -> bytes:
        if entry.size == 0:
            return b""
        return self._read_chain(entry.start_cluster, entry.size)

    def walk(self, cluster: int | None = None, prefix: str = ""):
        """Yield (relative_posix_path, DirEntry) for every FILE, recursing
        depth-first into subdirectories."""
        for e in self.listdir(cluster):
            rel = prefix + e.name
            if e.is_dir:
                yield from self.walk(e.start_cluster, rel + "/")
            else:
                yield rel, e

    def extract_all(self, out_dir: str) -> int:
        """Extract every file under out_dir, preserving directory structure.
        Returns the number of files written."""
        n = 0
        for rel, entry in self.walk():
            dest = os.path.join(out_dir, *rel.split("/"))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as f:
                f.write(self.read_file(entry))
            n += 1
        return n


# ---------------------------------------------------------------------------
# Self-test / CLI
# ---------------------------------------------------------------------------

def _pack_short(name: str, ext: str, attr: int, cluster: int, size: int) -> bytes:
    return _SHORT_ENTRY.pack(name.ljust(8).encode("ascii"), ext.ljust(3).encode("ascii"),
                              attr, b"\x00" * 10, 0, 0, cluster, size)


def _pack_lfn(seq: int, last: bool, chunk13: str, short_checksum: int) -> bytes:
    chars = [ord(c) for c in chunk13]
    if len(chars) < 13:
        chars.append(0x0000)  # NUL-terminate a name that ends within this entry
    chars = (chars + [0xFFFF] * 13)[:13]  # pad the rest of the entry
    n1 = struct.pack("<5H", *chars[0:5])
    n2 = struct.pack("<6H", *chars[5:11])
    n3 = struct.pack("<2H", *chars[11:13])
    ordv = (seq | 0x40) if last else seq
    return _LFN_ENTRY.pack(ordv, n1, _ATTR_LFN, 0, short_checksum, n2, 0, n3)


def _build_synthetic_image() -> tuple[bytes, dict[str, bytes]]:
    """A tiny hand-built FAT16 image: bytes_per_sector=512, 1 sector/cluster,
    1 reserved sector, 1 FAT, 16 root entries (512B = 1 sector). Exercises a
    short-name file, a long-name (VFAT) file spanning two clusters, and a
    subdirectory holding a nested file -- the three shapes pup.py's real
    os0/vs0 trees actually contain."""
    bps, spc, rsvd, nfat, rootent = 512, 1, 1, 1, 16
    boot = bytearray(512)
    _BOOT_SECTOR.pack_into(boot, 0, b"\x00\x00\x00", b"VITADEC\x00", bps, spc, rsvd, nfat, rootent, 0,
                            0xF8, 1, 0, 0, 0, 1000)
    boot[_BOOT_SIG_OFFSET:_BOOT_SIG_OFFSET + 2] = _BOOT_SIG

    short_content = b"hello, fat16"
    long_name = "ThisIsALongFileName.bin"
    long_content = bytes((i * 7) % 251 for i in range(512 + 37))  # spans clusters 3 and 4
    nested_content = b"nested file content\x00\x01\x02"

    fat = bytearray(512)  # 256 u16 entries; clusters 0/1 reserved, data starts at 2
    def set_fat(cluster: int, value: int) -> None:
        struct.pack_into("<H", fat, cluster * 2, value)
    set_fat(2, 0xFFFF)              # A.TXT: 1 cluster
    set_fat(3, 4); set_fat(4, 0xFFFF)  # long-name file: 2 clusters
    set_fat(5, 0xFFFF)              # SUBDIR: 1 cluster
    set_fat(6, 0xFFFF)              # SUBDIR/nested.dat: 1 cluster

    root = bytearray(rootent * 32)
    root[0:32] = _pack_short("A", "TXT", 0x20, 2, len(short_content))
    # VFAT entries for the long name precede its short "alias" entry, stored
    # highest-sequence-number first (disk order), as real FAT16 does.
    root[32:64] = _pack_lfn(2, True, long_name[13:26], 0)
    root[64:96] = _pack_lfn(1, False, long_name[0:13], 0)
    root[96:128] = _pack_short("LONGFN~1", "BIN", 0x20, 3, len(long_content))
    root[128:160] = _pack_short("SUBDIR", "", _ATTR_DIRECTORY, 5, 0)

    subdir = bytearray(512)  # SUBDIR's one cluster
    subdir[0:32] = _pack_short("NESTED", "DAT", 0x20, 6, len(nested_content))

    data_region = bytearray()
    data_region += short_content.ljust(512, b"\x00")           # cluster 2
    data_region += long_content[0:512]                          # cluster 3
    data_region += long_content[512:].ljust(512, b"\x00")       # cluster 4
    data_region += bytes(subdir)                                 # cluster 5
    data_region += nested_content.ljust(512, b"\x00")           # cluster 6

    image = bytes(boot) + bytes(fat) + bytes(root) + bytes(data_region)
    expected = {
        "A.TXT": short_content,
        long_name: long_content,
        "SUBDIR/NESTED.DAT": nested_content,
    }
    return image, expected


def _selftest() -> None:
    image, expected = _build_synthetic_image()
    img = FAT16Image(image)

    got = {rel: img.read_file(e) for rel, e in img.walk()}
    assert got.keys() == expected.keys(), f"file set mismatch: got {sorted(got)} expected {sorted(expected)}"
    for rel, content in expected.items():
        assert got[rel] == content, f"{rel}: content mismatch ({len(got[rel])} vs {len(content)} bytes)"

    # Cluster-chain loop detection: point cluster 2 at itself and confirm it's caught.
    bad = bytearray(image)
    fat_off = img.boot.fat_start + 2 * 2
    struct.pack_into("<H", bad, fat_off, 2)
    bad_img = FAT16Image(bytes(bad))
    try:
        list(bad_img._chain(2))
        raise AssertionError("expected Fat16Error for a self-referencing cluster chain")
    except Fat16Error:
        pass

    # extract_all() round-trip through real files on disk.
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        n = img.extract_all(tmp)
        assert n == len(expected), f"extract_all wrote {n} files, expected {len(expected)}"
        for rel, content in expected.items():
            with open(os.path.join(tmp, *rel.split("/")), "rb") as f:
                assert f.read() == content, f"{rel}: on-disk content mismatch after extract_all"

    print(f"fat16.py self-test: OK ({len(expected)} files, incl. one long filename and one subdirectory)")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="python3 -m vitadecrypt.fat16",
                                 description="Extract (or list) a FAT16 image, e.g. a decrypted Vita os0.img/vs0.img.")
    p.add_argument("image", nargs="?")
    p.add_argument("outdir", nargs="?")
    p.add_argument("--list", action="store_true", help="list files instead of extracting")
    p.add_argument("--selftest", action="store_true")
    args = p.parse_args(argv)

    if args.selftest or not args.image:
        _selftest()
        return 0

    with open(args.image, "rb") as f:
        data = f.read()
    img = FAT16Image(data)

    if args.list or not args.outdir:
        for rel, e in img.walk():
            print(f"{e.size:>10}  {rel}")
        return 0

    n = img.extract_all(args.outdir)
    print(f"extracted {n} files to {args.outdir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
