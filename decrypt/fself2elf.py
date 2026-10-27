#!/usr/bin/env python3
"""Convert FAGDec-produced SELFs (plaintext segments, maybe zlib-compressed) back to ELF.
Verifies each result against the SHA-256 stored in the SELF's control info.
usage: fself2elf.py <in_dir> <out_dir>"""
import hashlib, os, struct, sys, zlib

HDR = struct.Struct("<IIHHIQQQQQQQQQQQQQQ")
def convert(path):
    b = open(path, "rb").read()
    if b[:4] != b"SCE\0": return None, "not a SELF"
    (_, _, _, _, _, hlen, elf_size, _, _, _, _, elf_off, phdr_off, _,
     seginfo_off, _, ci_off, ci_size, _) = HDR.unpack_from(b, 0)
    ehdr = b[elf_off:elf_off + 52]
    if ehdr[:4] != b"\x7fELF": return None, "bad embedded ehdr"
    e_phoff, = struct.unpack_from("<I", ehdr, 0x1c)
    e_phentsize, e_phnum = struct.unpack_from("<HH", ehdr, 0x2a)
    out = bytearray(elf_size)
    out[0:52] = ehdr
    ph = b[phdr_off:phdr_off + e_phentsize * e_phnum]
    out[e_phoff:e_phoff + len(ph)] = ph
    for i in range(e_phnum):
        p_offset, = struct.unpack_from("<I", ph, i * e_phentsize + 4)
        p_filesz, = struct.unpack_from("<I", ph, i * e_phentsize + 16)
        s_off, s_len, comp, _ = struct.unpack_from("<QQQQ", b, seginfo_off + i * 32)
        if p_filesz == 0: continue
        data = b[s_off:s_off + s_len]
        if comp == 2: data = zlib.decompress(data)
        if len(data) != p_filesz: return None, f"seg {i} size {len(data)} != {p_filesz}"
        out[p_offset:p_offset + p_filesz] = data
    # expected digest from control info type 4
    digest, off = None, ci_off
    while off < ci_off + ci_size:
        t, sz, nxt = struct.unpack_from("<IIQ", b, off)
        if t == 4: digest = b[off + 16 + 0x14: off + 16 + 0x34]
        if not nxt or sz == 0: break
        off += sz
    ok = digest is not None and hashlib.sha256(out).digest() == digest
    return bytes(out), "verified" if ok else "UNVERIFIED"

src, dst = sys.argv[1], sys.argv[2]
stats = {}
for root, _, files in os.walk(src):
    for f in sorted(files):
        if f.endswith((".auth", ".temp", ".sha256")): continue
        p = os.path.join(root, f)
        try: elf, status = convert(p)
        except Exception as e: elf, status = None, f"error: {e}"
        key = status if status == 'verified' else ('sm-empty' if '/sm/' in p else status.split(':')[0])
        stats[key] = stats.get(key, 0) + 1
        rel = os.path.relpath(p, src)
        if (elf is None or status != "verified") and "/sm/" not in p: print(f"{status:12} {rel}")
        if elf is None: continue
        o = os.path.join(dst, rel + ".elf")
        os.makedirs(os.path.dirname(o), exist_ok=True)
        open(o, "wb").write(elf)
print(stats)
