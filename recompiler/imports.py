#!/usr/bin/env python3
"""What a Vita executable (eboot.bin / .suprx, as a plain ELF: decrypt/fself2elf.py) imports from the
system, library by library, with names from decrypt/nidmap.tsv: the surface a native port of it
has to provide (or recompile, for the firmware's own libraries).

usage: imports.py <elf> [<elf> ...] [--names]     (--names lists every function)
"""
import os, struct, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def known_names():
    """{(library, NID): name} from decrypt/nidmap.tsv (VitaSDK's NID database, flattened by dev/build_nidmap.py)"""
    names = {}
    path = os.path.join(ROOT, "decrypt", "nidmap.tsv")
    if os.path.exists(path):
        for l in open(path):
            p = l.rstrip("\n").split("\t")
            if len(p) >= 3:
                names[(p[0], int(p[1], 16))] = p[2]
    return names


def imports(path):
    """module name, [(library, library NID, [function NIDs], [variable NIDs])]"""
    b = open(path, "rb").read()
    e_entry, e_phoff = struct.unpack_from("<II", b, 0x18)
    e_phentsize, e_phnum = struct.unpack_from("<HH", b, 0x2a)
    segs = [struct.unpack_from("<IIIII", b, e_phoff + i * e_phentsize)[:5] for i in range(e_phnum)]

    def off(va):
        for t, o, vaddr, _, sz in segs:
            if t == 1 and vaddr <= va < vaddr + sz:
                return o + va - vaddr
        return None

    def cstr(va):
        o = off(va)
        return b[o:b.index(b"\0", o)].decode("ascii", "replace") if o is not None else "?"
    seg = segs[e_entry >> 30][1]
    mi = seg + (e_entry & 0x3FFFFFFF)
    modname = b[mi + 4:mi + 31].split(b"\0")[0].decode("ascii", "replace")
    top, end = struct.unpack_from("<II", b, mi + 0x2C)
    out, o = [], seg + top
    while o < seg + end:
        size = struct.unpack_from("<H", b, o)[0]
        nfunc, nvar = struct.unpack_from("<HH", b, o + 6)
        if size == 0x34:
            lib_nid, lib_name = struct.unpack_from("<II", b, o + 0x10)
            fn_tab, var_tab = struct.unpack_from("<I", b, o + 0x1C)[0], struct.unpack_from("<I", b, o + 0x24)[0]
        elif size == 0x24:
            lib_nid, lib_name, fn_tab = struct.unpack_from("<III", b, o + 0x0C)
            var_tab = struct.unpack_from("<I", b, o + 0x1C)[0]
        else:
            raise ValueError(f"import entry of size {size:#x} at {o:#x}")
        fn = list(struct.unpack_from(f"<{nfunc}I", b, off(fn_tab))) if nfunc else []
        var = list(struct.unpack_from(f"<{nvar}I", b, off(var_tab))) if nvar else []
        out.append((cstr(lib_name), lib_nid, fn, var))
        o += size
    return modname, out


def main():
    paths = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not paths:
        sys.exit(__doc__)
    names = known_names()
    for p in paths:
        modname, libs = imports(p)
        nf = sum(len(f) for _, _, f, _ in libs)
        known = sum(1 for lib, _, f, _ in libs for n in f if (lib, n) in names)
        print(f"== {os.path.basename(p)} ({modname}): {len(libs)} libraries, {nf} functions "
              f"({known} named), {sum(len(v) for *_, v in libs)} variables")
        for lib, nid, f, v in sorted(libs, key=lambda x: -len(x[2])):
            print(f"  {lib:<28} {len(f):>4} fn {len(v):>3} var")
            if "--names" in sys.argv:
                for n in f:
                    print(f"      {names.get((lib, n), f'{lib}_{n:08X}')}")


if __name__ == "__main__":
    main()
