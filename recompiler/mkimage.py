#!/usr/bin/env python3
"""Flatten a Ghidra export's memory image (<name>.data.json: the module as Ghidra loaded and
relocated it, the same bytes difftest validates the recompiled code against) into a file the
native runtime (runtime/) maps into its guest arena.

Format, little-endian: "VRIM", u32 count, then count x (u32 address, u32 size, u32 has_bytes,
size bytes if has_bytes). Blocks without bytes are zero-filled (.bss, the imported variables).
Optionally followed by "TLS\0", u32 template address, u32 template size, u32 block size: the
module's thread-local storage (from its module info), each thread's block initialised from it.
Then optionally "VARS", u32 count, count x (u32 address, char name[60]): the imported variables
(the C library's ctype tables, FILE objects, C++ typeinfo vtables), which the system layer fills.
Ghidra's Vita loader gives each one a 0x1000-byte slot from 0xfff00000, in import table order.

A module loaded with the program (mkimage.py ... --module) also gets "EXPT", u32 count, count x
(u32 address, char name[60]) (its exported functions, which the host resolves imports to) and
"MSTA", u32 module_start (run before the program's). The program's image has "PPRM", u32 address
of its process parameters (sceKernelGetProcessParam).

usage: mkimage.py <export.data.json> <out.img> [module ELF] [--module]
"""
import json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imports import imports, known_names

VAR_BASE, VAR_SLOT = 0xfff00000, 0x1000


def main():
    is_module = "--module" in sys.argv
    sys.argv = [a for a in sys.argv if a != "--module"]
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    d = json.load(open(sys.argv[1]))
    blocks = [b for b in d["blocks"] if b["size"]]
    with open(sys.argv[2], "wb") as f:
        f.write(b"VRIM" + struct.pack("<I", len(blocks)))
        for b in blocks:
            data = bytes.fromhex(b["bytes"]) if b.get("init") else b""
            f.write(struct.pack("<III", b["start"], b["size"], 1 if data else 0))
            f.write(data)
            print(f"{b['start']:#010x} {b['size']:#9x} {b.get('name', '')}{'' if data else ' (zero)'}")
        if len(sys.argv) == 4:
            tls = module_tls(sys.argv[3])
            if tls:
                f.write(b"TLS\0" + struct.pack("<III", *tls))
                print(f"TLS template {tls[0]:#x} {tls[1]:#x} bytes, block {tls[2]:#x}")
            if "variables" in d:   # analyze.py's: slots of the variables no linked module exports
                vs = [(a, n) for a, n in d["variables"]]
            else:
                names = known_names()
                vs = [(VAR_BASE + i * VAR_SLOT, n) for i, n in enumerate(
                    names.get((lib, v), f"{lib}_{v:08X}") for lib, _, _, var in imports(sys.argv[3])[1] for v in var)]
            if vs:
                f.write(b"VARS" + struct.pack("<I", len(vs)))
                for a, n in vs:
                    f.write(struct.pack("<I", a) + n.encode()[:59].ljust(60, b"\0"))
                print(f"VARS {len(vs)} imported variables: {', '.join(n for _, n in vs)}")
        if not is_module:   # the program's process parameters ("PSP2": libc finds its heap size etc. there)
            from analyze import Module
            pp = next((a for lib, nid, a, var in Module(sys.argv[3]).export_table() if var and nid == 0x70FBA1E7), None)
            if pp is not None:
                f.write(b"PPRM" + struct.pack("<I", pp))
                print(f"PPRM process parameters at {pp:#x}")
        if is_module and "exports" in d:
            names = known_names()
            ex = [(a, names.get((lib, nid), f"{lib}_{nid:08X}")) for lib, nid, a, var in d["exports"] if not var and lib]
            f.write(b"EXPT" + struct.pack("<I", len(ex)))
            for a, n in ex:
                f.write(struct.pack("<I", a) + n.encode()[:59].ljust(60, b"\0"))
            start = d["functions"].get("module_start")
            if start is not None:
                start = next((a for lib, nid, a, var in d["exports"] if nid == 0x935CD196), start)
                f.write(b"MSTA" + struct.pack("<I", start))
            print(f"EXPT {len(ex)} exported functions; module_start {start:#x}" if start else f"EXPT {len(ex)} exported functions")


def module_tls(elf):
    """(template address, template size, block size) from the module info, or None"""
    b = open(elf, "rb").read()
    e_entry, e_phoff = struct.unpack_from("<II", b, 0x18)
    phentsize, phnum = struct.unpack_from("<HH", b, 0x2a)
    segs = [struct.unpack_from("<IIIII", b, e_phoff + i * phentsize)[:5] for i in range(phnum)]
    seg = segs[e_entry >> 30]
    start, filesz, memsz = struct.unpack_from("<III", b, seg[1] + (e_entry & 0x3FFFFFFF) + 0x38)
    return (seg[2] + start, filesz, memsz) if memsz else None


if __name__ == "__main__":
    main()
