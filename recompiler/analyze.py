#!/usr/bin/env python3
"""Analyse a Vita executable (a plain ELF from decrypt/fself2elf.py) without Ghidra: write the export
recomp.py and mkimage.py read (<name>.data.json): the memory image as the loader leaves it, every
function's entry and its code ranges.

- Image: the loadable segments at their link addresses (relocating a module to the address it was
  linked at changes nothing), with the references to imported *variables* pointed at slots: each
  variable gets 0x1000 bytes from 0xfff00000 in import order (the host fills them, see mkimage.py).
  The module lists those references itself: per variable, a header word (count << 7 | 0x40) then
  count entries {1 | type << 8 | addend << 16, offset in segment (type >> 4 & 15... see var_refs)},
  types 2 (ABS32), 0x2f/0x30 (Thumb MOVW/MOVT).
- Functions: recursive descent from module_start/stop/exit, the exports, and code pointers (aligned
  words in the image and movw/movt pairs that land in .text and decode as a function); bl/blx targets
  are functions, blx switches to ARM. A function's body is every instruction reachable inside it:
  conditional branches, Thumb tbb/tbh and ARM "ldr pc, [pc, rN, lsl #2]" tables; an unconditional
  branch to another function is a tail call.
- Import stubs (named from decrypt/nidmap.tsv) are 8-byte bodies holding the loader's placeholder.

usage: analyze.py <elf> <out.data.json> [--base <load address>] [--link <elf>@<address> ...]
                  [--var-base <address>] [--compare <ghidra.data.json>]
  --base: load a relocatable module (a .suprx) somewhere other than its link address
  --link: the other modules loaded with it (repeatable): references to their exported variables
          point at them; --var-base: where this module's unresolved variables' slots go
"""
import bisect, json, os, struct, sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_ARM
from capstone import arm_const as A
from capstone.arm import ARM_OP_IMM, ARM_OP_REG, ARM_OP_MEM, ARM_REG_PC, ARM_CC_AL, ARM_CC_INVALID
LDM = {getattr(A, n) for n in dir(A) if n.startswith("ARM_INS_LDM")} | {A.ARM_INS_POP}
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imports import known_names

VAR_BASE, VAR_SLOT, VAR_SIZE = 0xfff00000, 0x1000, 0x80000


class Module:
    def __init__(self, path):
        self.b = b = open(path, "rb").read()
        self.etype, = struct.unpack_from("<H", b, 16)
        e_entry, e_phoff = struct.unpack_from("<II", b, 0x18)
        phent, phnum = struct.unpack_from("<HH", b, 0x2a)
        self.segs = [struct.unpack_from("<6I", b, e_phoff + i * phent) for i in range(phnum)]   # type, off, vaddr, paddr, filesz, memsz
        self.loads = [s for s in self.segs if s[0] == 1]
        seg = self.segs[e_entry >> 30]
        self.mi = seg[1] + (e_entry & 0x3FFFFFFF)
        self.name = b[self.mi + 4:self.mi + 31].split(b"\0")[0].decode("ascii", "replace")
        self.mem = {s[2]: bytearray(b[s[1]:s[1] + s[4]]) + bytearray(s[5] - s[4]) for s in self.loads}
        self.delta = 0   # load address - link address (tables are read at link addresses)

    def relocations(self):
        """the module's own relocations (segments of type 0x60000000): (target segment, type,
        patched segment, addend, offset). Game executables use two formats; system modules
        (libgxm, ...) also the compact ones, which carry a running state from entry to entry:
          0  {0 | symseg << 4 | type << 8 | datseg << 16 | type2 << 20 | distance2 << 28, addend,
             offset} (type2, if any, applies at offset + 2 * distance2)
          1  {1 | symseg << 4 | type << 8 | datseg << 16 | offset low 12 << 20, offset high 10 |
             addend 22 << 10}
          2  {2 | symseg << 4 | type << 8 | offset delta 16 << 16, addend}: same patched segment
          3  {3 | symseg << 4 | thumb << 8 | offset delta 18 << 9 | distance2 5 << 27, addend 22}:
             a movw at offset, its movt at offset + distance2 (Thumb or ARM)
          4  {4 | offset delta 23 << 4 | distance2 5 << 27}: another such pair, same symbol/addend
          5  {5 | delta1 9 << 4 | distance2 5 << 13 | delta3 9 << 18 | distance4 5 << 27}: two pairs
          6  {6 | offset delta 28 << 4}: an absolute word; its symbol and addend are what the word
             already holds (the segment containing that link address, and the offset into it)
          7/8/9  {f | deltas << 4}: 4 x 7, 7 x 4 or 14 x 2-bit word deltas, each one like format 6
        (formats 2-9: semantics as the loader applies them; checked by reproducing libgxm's
        link-address image from its own relocations, see check_relocations)"""
        out = []
        loads = self.loads
        def word_at(datseg, off):   # the unrelocated word in the file (0 past the file size)
            seg = loads[datseg]
            return struct.unpack_from("<I", self.b, seg[1] + off)[0] if off + 4 <= seg[4] else 0
        def holder(v):   # (segment, addend) of a link address
            for i, seg in enumerate(loads):
                if seg[2] <= v < seg[2] + seg[5]:
                    return i, v - seg[2]
            return None
        for s in self.segs:
            if s[0] != 0x60000000:
                continue
            d, o = self.b[s[1]:s[1] + s[4]], 0
            off = datseg = symseg = addend = 0; typ = typ2 = 0
            while o + 4 <= len(d):
                w0, = struct.unpack_from("<I", d, o); f = w0 & 0xf
                if f == 0 and o + 12 <= len(d):
                    w1, w2 = struct.unpack_from("<II", d, o + 4); o += 12
                    symseg, typ, datseg, typ2, addend, off = w0 >> 4 & 0xf, w0 >> 8 & 0xff, w0 >> 16 & 0xf, w0 >> 20 & 0xff, w1, w2
                    out.append((symseg, typ, datseg, addend, off))
                    if typ2:
                        out.append((symseg, typ2, datseg, addend, off + 2 * (w0 >> 28 & 0xf)))
                elif f == 1 and o + 8 <= len(d):
                    w1, = struct.unpack_from("<I", d, o + 4); o += 8
                    symseg, typ, datseg, typ2 = w0 >> 4 & 0xf, w0 >> 8 & 0xff, w0 >> 16 & 0xf, 0
                    addend, off = w1 >> 10, (w0 >> 20) | (w1 & 0x3ff) << 12
                    out.append((symseg, typ, datseg, addend, off))
                elif f == 2 and o + 8 <= len(d):
                    w1, = struct.unpack_from("<I", d, o + 4); o += 8
                    symseg, typ, typ2, addend = w0 >> 4 & 0xf, w0 >> 8 & 0xff, 0, w1
                    off += w0 >> 16
                    out.append((symseg, typ, datseg, addend, off))
                elif f == 3 and o + 8 <= len(d):
                    w1, = struct.unpack_from("<I", d, o + 4); o += 8
                    thumb = w0 >> 8 & 1
                    typ, typ2 = (0x2f, 0x30) if thumb else (0x2b, 0x2c)
                    symseg, addend = w0 >> 4 & 0xf, w1 & 0x3fffff
                    off += w0 >> 9 & 0x3ffff
                    out += [(symseg, typ, datseg, addend, off), (symseg, typ2, datseg, addend, off + (w0 >> 27))]
                elif f == 4:
                    o += 4
                    off += w0 >> 4 & 0x7fffff
                    out += [(symseg, typ, datseg, addend, off), (symseg, typ2, datseg, addend, off + (w0 >> 27))]
                elif f == 5:
                    o += 4
                    off += w0 >> 4 & 0x1ff
                    out += [(symseg, typ, datseg, addend, off), (symseg, typ2, datseg, addend, off + (w0 >> 13 & 0x1f))]
                    off += w0 >> 18 & 0x1ff
                    out += [(symseg, typ, datseg, addend, off), (symseg, typ2, datseg, addend, off + (w0 >> 27))]
                elif f in (6, 7, 8, 9):
                    o += 4
                    deltas = [w0 >> 4] if f == 6 else []
                    if f != 6:
                        bits = {7: 7, 8: 4, 9: 2}[f]; v = w0 >> 4
                        while v:
                            deltas.append((v & ((1 << bits) - 1)) * 4); v >>= bits
                    for dlt in deltas:
                        off += dlt
                        h = holder(word_at(datseg, off))
                        if h:
                            symseg, addend = h
                            out.append((symseg, 2, datseg, addend, off))
                    typ, typ2 = 2, 0
                else:
                    raise ValueError(f"{self.name}: relocation format {f} not handled")
        return out

    def relocate(self, base):
        """load the module at base instead of its link address: shift every segment by the same
        amount and patch its absolute relocations (relative ones don't change)"""
        delta = (base - self.loads[0][2]) & 0xffffffff
        if not delta:
            return
        for symseg, typ, datseg, addend, off in self.relocations():
            v = (self.loads[symseg][2] + delta + addend) & 0xffffffff
            buf = self.mem[self.loads[datseg][2]]
            if typ in (2, 38):   # ABS32, TARGET1
                struct.pack_into("<I", buf, off, v)
            elif typ in (0x2f, 0x30):   # Thumb MOVW / MOVT
                hw1, hw2 = struct.unpack_from("<HH", buf, off)
                struct.pack_into("<HH", buf, off, *set_thumb_imm16(hw1, hw2, v & 0xffff if typ == 0x2f else v >> 16))
            elif typ in (0x2b, 0x2c):   # ARM MOVW / MOVT
                w, = struct.unpack_from("<I", buf, off); h = v & 0xffff if typ == 0x2b else v >> 16
                struct.pack_into("<I", buf, off, (w & 0xfff0f000) | (h >> 12) << 16 | (h & 0xfff))
        self.delta = delta
        self.mem = {(k + delta) & 0xffffffff: v for k, v in self.mem.items()}

    def export_table(self):
        """every export: (library, NID, load address, is a variable); module_start etc. are in the
        unnamed library"""
        out = []
        for size, o in self.tables(0x24):
            nfunc, nvar = struct.unpack_from("<HH", self.b, o + 6)
            name_p, nid_tab, ent_tab = struct.unpack_from("<III", self.b, o + 0x14)
            lib = ""
            if name_p:
                lo = self.off(name_p); lib = self.b[lo:self.b.index(b"\0", lo)].decode("ascii", "replace")
            for i in range(nfunc + nvar):
                out.append((lib, self.u32(nid_tab + 4 * i), (self.u32(ent_tab + 4 * i) + self.delta) & 0xffffffff, i >= nfunc))
        return out

    def off(self, va):
        for t, o, v, _, fs, _ in self.segs:
            if t == 1 and v <= va < v + fs:
                return o + va - v
        return None

    def u32(self, va):
        return struct.unpack_from("<I", self.b, self.off(va))[0]

    def tables(self, at):
        """the module info's export (at 0x24) or import (at 0x2c) table entries, as (size, file offset)"""
        seg = self.loads[0]
        top, end = struct.unpack_from("<II", self.b, self.mi + at)
        o, out = seg[1] + top, []
        while o < seg[1] + end:
            size = struct.unpack_from("<H", self.b, o)[0]
            if not size:
                break
            out.append((size, o))
            o += size
        return out

    def exports(self):
        """(name or None, nid, address) of every exported function; module_start etc. by NID"""
        special = {0x935CD196: "module_start", 0x79F8E492: "module_stop", 0x913482A9: "module_exit"}
        out = []
        for size, o in self.tables(0x24):
            nfunc, nvar = struct.unpack_from("<HH", self.b, o + 6)
            nid_tab, ent_tab = struct.unpack_from("<II", self.b, o + 0x18)
            for i in range(nfunc):
                nid, a = self.u32(nid_tab + 4 * i), (self.u32(ent_tab + 4 * i) + self.delta) & 0xffffffff
                out.append((special.get(nid), nid, a))
        return out

    def imports(self):
        """[(library, [(nid, stub address)], [(nid, reference table address)])]"""
        out = []
        for size, o in self.tables(0x2c):
            nfunc, nvar = struct.unpack_from("<HH", self.b, o + 6)
            if size == 0x34:
                lib_name, = struct.unpack_from("<I", self.b, o + 0x14)
                fn_nid, fn_ent, var_nid, var_ent = struct.unpack_from("<IIII", self.b, o + 0x1C)
            else:   # 0x24, the short form system modules use: {size, version, attr, nfunc, nvar, -,
                    # library NID, name, function NIDs, function stubs, variable NIDs, variable tables}
                lib_name, fn_nid, fn_ent, var_nid, var_ent = struct.unpack_from("<5I", self.b, o + 0x10)
            lo = self.off(lib_name)
            lib = self.b[lo:self.b.index(b"\0", lo)].decode("ascii", "replace")
            fns = [(self.u32(fn_nid + 4 * i), (self.u32(fn_ent + 4 * i) + self.delta) & 0xffffffff) for i in range(nfunc)]
            vs = [(self.u32(var_nid + 4 * i), self.u32(var_ent + 4 * i)) for i in range(nvar)]
            out.append((lib, fns, vs))
        return out

    def var_refs(self, table):
        """a variable's references: (type, address, addend)"""
        h = self.u32(table)
        out = []
        for j in range(h >> 7):
            w0, w1 = struct.unpack_from("<II", self.b, self.off(table) + 4 + 8 * j)
            out.append(((w0 >> 8) & 0xff, (self.loads[(w0 >> 4) & 0xf][2] + w1 + self.delta) & 0xffffffff, w0 >> 16))
        return out


def thumb_imm16(hw1, hw2):
    """the 16-bit immediate of a Thumb-2 MOVW/MOVT (imm4:i:imm3:imm8)"""
    return ((hw1 & 0xf) << 12) | ((hw1 >> 10 & 1) << 11) | ((hw2 >> 12 & 7) << 8) | (hw2 & 0xff)


def set_thumb_imm16(hw1, hw2, v):
    hw1 = (hw1 & ~0x040f) | (v >> 12 & 0xf) | ((v >> 11 & 1) << 10)
    hw2 = (hw2 & ~0x70ff) | ((v >> 8 & 7) << 12) | (v & 0xff)
    return hw1, hw2


def build_image(m, names, linked=None, var_base=VAR_BASE):
    """segments with variable references patched: to the exporting module's variable when one of
    the linked modules exports it, else to a slot the host fills; (blocks, slot variable names)"""
    var_names = []
    linked = linked or {}
    for lib, fns, vs in m.imports():
        for nid, table in vs:
            if (lib, nid) in linked:
                slot = linked[(lib, nid)]
            else:
                slot = var_base + VAR_SLOT * len(var_names)
                var_names.append(names.get((lib, nid), f"{lib}_{nid:08X}"))
            for typ, at, addend in m.var_refs(table):
                seg = next(s for s in m.loads if s[2] + m.delta <= at < s[2] + m.delta + s[5])
                buf, o = m.mem[seg[2] + m.delta], at - seg[2] - m.delta
                if typ == 2:   # ABS32
                    struct.pack_into("<I", buf, o, (slot + addend) & 0xffffffff)
                elif typ in (0x2f, 0x30):   # Thumb MOVW (low half) / MOVT (high half)
                    hw1, hw2 = struct.unpack_from("<HH", buf, o)
                    v = (slot + addend) & 0xffff if typ == 0x2f else (slot + addend) >> 16
                    struct.pack_into("<HH", buf, o, *set_thumb_imm16(hw1, hw2, v))
                elif typ in (0x2b, 0x2c):   # ARM MOVW / MOVT (libgxm, being ARM code, refers to its one variable so)
                    w, = struct.unpack_from("<I", buf, o)
                    v = (slot + addend) & 0xffff if typ == 0x2b else (slot + addend) >> 16
                    struct.pack_into("<I", buf, o, (w & 0xfff0f000) | (v >> 12) << 16 | (v & 0xfff))
                else:
                    print(f"variable reference type {typ:#x} at {at:#x} not handled", file=sys.stderr)
    blocks = []
    for i, s in enumerate(m.loads):
        a = (s[2] + m.delta) & 0xffffffff
        blocks.append({"name": ".text" if i == 0 else ".data",
                       "start": a, "size": s[5], "init": True, "bytes": bytes(m.mem[a]).hex()})
    if var_names:
        blocks.append({"name": "VarImport", "start": var_base, "size": VAR_SLOT * len(var_names), "init": False})
    return blocks, var_names


class Analyser:
    def __init__(self, m, text_lo, text_hi):
        self.m, self.lo, self.hi = m, text_lo, text_hi
        self.code = m.mem[text_lo]
        self.th = Cs(CS_ARCH_ARM, CS_MODE_THUMB); self.th.detail = True
        self.arm = Cs(CS_ARCH_ARM, CS_MODE_ARM); self.arm.detail = True
        self.cache = {}
        self.noreturn = set()   # import stubs that never return

    def insn(self, a, arm):
        k = (a, arm)
        if k not in self.cache:
            o = a - self.lo
            if not (0 <= o < len(self.code) - 1):
                self.cache[k] = None
            else:
                got = next((arm and self.arm or self.th).disasm(bytes(self.code[o:o + 4]), a, 1), None)
                self.cache[k] = got
        return self.cache[k]

    def word(self, a):
        o = a - self.lo
        return struct.unpack_from("<I", self.code, o)[0] if 0 <= o <= len(self.code) - 4 else None

    def half(self, a):
        o = a - self.lo
        return struct.unpack_from("<H", self.code, o)[0] if 0 <= o <= len(self.code) - 2 else None

    def in_text(self, a):
        return self.lo <= a < self.hi

    @staticmethod
    def cond(i):
        return i.cc not in (ARM_CC_AL, ARM_CC_INVALID)

    @staticmethod
    def imm_target(i):
        ops = i.operands
        return ops[-1].imm & 0xffffffff if ops and ops[-1].type == ARM_OP_IMM else None   # capstone's are signed

    def guard_entries(self, i, prev):
        """a Thumb tbb/tbh table's entry count from the bounds check guarding it, as compilers lay it
        out: the index register is the switch value minus constants (subs chain), checked against
        a bound -- an immediate, or a register a movw/mov loaded -- by a cmp, then a branch away on
        greater/higher. None when the shape isn't that."""
        mem = i.operands[0].mem if i.operands and i.operands[0].type == ARM_OP_MEM else None
        if mem is None or not mem.index:
            return None
        cur, off, k = mem.index, 0, len(prev) - 1
        while k >= 0:   # back through the adjustments to the switch value
            p = prev[k]; ops = p.operands
            if p.mnemonic.split(".")[0] in ("sub", "subs") and len(ops) == 3 and ops[0].type == ARM_OP_REG and ops[0].reg == cur \
                    and ops[1].type == ARM_OP_REG and ops[2].type == ARM_OP_IMM:
                off += ops[2].imm; cur = ops[1].reg; k -= 1; continue
            if p.mnemonic.split(".")[0] in ("add", "adds") and len(ops) == 3 and ops[0].type == ARM_OP_REG and ops[0].reg == cur \
                    and ops[1].type == ARM_OP_REG and ops[2].type == ARM_OP_IMM:
                off -= ops[2].imm; cur = ops[1].reg; k -= 1; continue
            break
        if k < 0:   # the check is in the block that branched here: "cmp rN, #b; ble <this block>"
            b = self.entry_bounds.get(self.run_start)
            if b and b[0] == cur:
                n = b[1] - off + 1
                return n if 0 < n <= 4096 else None
            return None
        exit_above = None
        while k >= 0:   # the branch away, then the cmp it follows
            p = prev[k]
            m = p.mnemonic.split(".")[0]
            if m in ("bgt", "bhi"): exit_above = 0
            elif m in ("bge", "bhs", "bcs"): exit_above = 1
            elif m == "cmp" and exit_above is not None:
                ops = p.operands
                if len(ops) != 2 or ops[0].type != ARM_OP_REG or ops[0].reg != cur:
                    return None
                if ops[1].type == ARM_OP_IMM:
                    bound = ops[1].imm
                elif ops[1].type == ARM_OP_REG:
                    bound = None
                    for q in reversed(prev[:k]):   # the register's constant, loaded just before
                        qo = q.operands
                        if qo and qo[0].type == ARM_OP_REG and qo[0].reg == ops[1].reg:
                            if q.mnemonic.split(".")[0] in ("movw", "mov", "movs") and len(qo) == 2 and qo[1].type == ARM_OP_IMM:
                                bound = qo[1].imm
                            break
                    if bound is None:
                        return None
                else:
                    return None
                n = bound - exit_above - off + 1
                return n if 0 < n <= 4096 else None
            elif m not in ("it", "ite", "itt") and not m.startswith("b"):
                return None
            k -= 1
        return None

    def note_bound(self, i, prev, t):
        """a "cmp rN, #b" then "ble/bls/blt/blo t": t is entered with rN <= b (or < b), for guard_entries"""
        m = i.mnemonic.split(".")[0]
        if m not in ("ble", "bls", "blt", "blo", "bcc") or not prev:
            return
        p = prev[-1]; ops = p.operands
        if p.mnemonic.split(".")[0] != "cmp" or len(ops) != 2 or ops[0].type != ARM_OP_REG:
            return
        if ops[1].type == ARM_OP_IMM: b = ops[1].imm
        elif ops[1].type == ARM_OP_REG:
            b = None
            for q in reversed(prev[:-1]):
                qo = q.operands
                if qo and qo[0].type == ARM_OP_REG and qo[0].reg == ops[1].reg:
                    if q.mnemonic.split(".")[0] in ("movw", "mov", "movs") and len(qo) == 2 and qo[1].type == ARM_OP_IMM: b = qo[1].imm
                    break
            if b is None: return
        else: return
        if m in ("blt", "blo", "bcc"): b -= 1
        old = self.entry_bounds.get(t)
        if old is None or (old[0] == ops[0].reg and b < old[1]): self.entry_bounds[t] = (ops[0].reg, b)
        elif old[0] != ops[0].reg: self.entry_bounds[t] = (None, 0)   # entered from differently-guarded places: no bound

    def table_targets(self, i, prev, arm):
        """a jump table's targets: Thumb tbb/tbh [pc, rN] (entries up to where the code it jumps to
        starts), ARM ldr pc, [pc, rN, lsl #2] (length from the preceding cmp rN, #n: n + 1 entries)"""
        n = None
        for p in reversed(prev[-4:]):
            if p.mnemonic.startswith("cmp") and len(p.operands) == 2 and p.operands[1].type == ARM_OP_IMM:
                n = p.operands[1].imm + 1
                break
        targets, base = [], i.address + i.size
        if i.mnemonic.startswith("tbb") or i.mnemonic.startswith("tbh"):
            esz = 1 if i.mnemonic.startswith("tbb") else 2
            # the guarding bounds check's count when its shape is recognised (Unit 13 has a tbh whose
            # first case's code starts well after the table, so "up to where the code starts" read
            # 250 instructions past its 11 entries as more of them); otherwise up to 1024, stopping
            # where the code it jumps to starts
            guard = self.guard_entries(i, prev)
            limit = guard or 1024
            first = None
            for k in range(limit):
                at = base + k * esz
                if guard is None and first is not None and at >= first:   # the table ends where the code it jumps to starts
                    break
                v = self.code[at - self.lo] if esz == 1 else self.half(at)
                if v is None:
                    break
                t = base + 2 * v
                if t < base or self.insn(t, False) is None:   # not a jump: the table ended
                    break
                targets.append(t)
                first = t if first is None else min(first, t)
            end = base + len(targets) * esz
            return targets, (base, end)
        # ARM: ldr pc, [pc, rN, lsl #2]: the table starts at pc + 8 (the next word is skipped,
        # usually a "b default")
        base = i.address + 8
        limit = n if n is not None else 1024
        for k in range(limit):
            v = self.word(base + 4 * k)
            if v is None or not self.in_text(v):
                break
            targets.append(v)
        return targets, (base, base + 4 * len(targets))

    def function(self, entry, arm, entries):
        """the instructions reachable in a function: (instruction addresses with sizes, calls, ARM calls, ok)"""
        seen, calls, todo, ranges, pointers = {}, set(), [entry], [], set()
        self.jumps = set()   # branch targets inside this function
        self.clean = True    # every path decodes to real instructions (data decoded as code doesn't)
        self.entry_bounds = {}   # block -> (register, inclusive upper bound) a branch into it guarantees
        while todo:
            a = todo.pop()
            self.run_start = a
            prev = []
            while True:
                if a in seen or not self.in_text(a):
                    break
                i = self.insn(a, arm)
                if i is None:
                    if a == entry:
                        return None
                    self.clean = False
                    break
                if self.junk(i):
                    self.clean = False
                seen[a] = i.size
                op, ops = i.id, i.operands
                writes_pc = any(o.type == ARM_OP_REG and o.reg == ARM_REG_PC for o in ops[:1]) and op in (A.ARM_INS_MOV, A.ARM_INS_LDR, A.ARM_INS_ADD, A.ARM_INS_SUB) \
                    or (op in LDM and any(o.type == ARM_OP_REG and o.reg == ARM_REG_PC for o in ops))
                t = self.imm_target(i)
                if op in (A.ARM_INS_BL, A.ARM_INS_BLX) and t is not None:
                    calls.add((t, (op == A.ARM_INS_BLX) != arm))
                    if t in self.noreturn or (not arm and self.noreturn_after(a + i.size)):   # a call that doesn't come back
                        break
                elif op in (A.ARM_INS_B, A.ARM_INS_CBZ, A.ARM_INS_CBNZ) and t is not None:
                    conditional = self.cond(i) or op != A.ARM_INS_B
                    if t in entries and t != entry:   # a tail call
                        calls.add((t, arm))
                    elif self.in_text(t):
                        todo.append(t)
                        self.jumps.add(t)
                        self.note_bound(i, prev, t)
                    if not conditional:
                        break
                elif op in (A.ARM_INS_TBB, A.ARM_INS_TBH) or (arm and op == A.ARM_INS_LDR and writes_pc and len(ops) == 2
                                                               and ops[1].type == ARM_OP_MEM and ops[1].mem.base == ARM_REG_PC and ops[1].mem.index):
                    targets, (tlo, thi) = self.table_targets(i, prev, arm)
                    todo += targets
                    ranges.append((tlo, thi))
                    if not self.cond(i):
                        break
                elif arm and op == A.ARM_INS_ADD and len(ops) == 3 and ops[0].type == ARM_OP_REG and ops[0].reg == ARM_REG_PC \
                        and ops[1].type == ARM_OP_REG and ops[1].reg == ARM_REG_PC and ops[2].type == ARM_OP_REG:
                    # ARM's switch, "add pc, pc, rN, lsl #2": a table of branches from this address + 8, one
                    # per case, as long as they run (each slot is a jump target; its branch the case)
                    k, slots = 0, []
                    while k < 1024:
                        t = a + 8 + 4 * k
                        j = self.insn(t, True)
                        if j is None or j.id != A.ARM_INS_B or self.cond(j):
                            break
                        slots.append(t); k += 1
                    todo += slots
                    self.jumps.update(slots)
                    if not self.cond(i):
                        break
                elif op == A.ARM_INS_BX or writes_pc or op in (A.ARM_INS_UDF, A.ARM_INS_BKPT):
                    if not self.cond(i):
                        break
                prev.append(i)
                a += i.size
        # code addresses built in registers: a movw and the next movt of the same register, in
        # address order (they can sit in different blocks)
        order = sorted(seen)
        for k, x in enumerate(order):
            i = self.insn(x, arm)
            # a continuation address formed from the pc: `addw ip, pc, #odd` then `b.w step` with the step
            # returning through `bx ip` (Killzone's hash routine: the step is shared, each unrolled stage
            # resumes after it) -- the target is a Thumb entry the walk would never reach otherwise
            if i.mnemonic in ("addw", "add", "adr") and len(i.operands) >= 2 and i.operands[-1].type == ARM_OP_IMM \
               and not arm and ((len(i.operands) == 3 and i.operands[1].type == ARM_OP_REG and i.operands[1].reg == A.ARM_REG_PC) or i.mnemonic == "adr"):
                v = ((x + 4) & ~3) + i.operands[-1].imm
                if v & 1 and self.in_text(v & ~1):
                    pointers.add((v & ~1, False))
            if i.mnemonic.startswith("movw") and len(i.operands) == 2 and i.operands[1].type == ARM_OP_IMM:   # (capstone's id for a Thumb movw is mov's)
                reg, lo = i.operands[0].reg, i.operands[1].imm & 0xffff
                for y in order[k + 1:k + 24]:
                    j = self.insn(y, arm)
                    if j.operands and j.operands[0].type == ARM_OP_REG and j.operands[0].reg == reg:
                        if j.mnemonic.startswith("movt"):
                            v = lo | (j.operands[1].imm & 0xffff) << 16
                            if v & 1 and self.in_text(v & ~1):   # Thumb code (even ones point at data in .text)
                                pointers.add((v & ~1, False))
                            elif arm and self.in_text(v) and not v & 3:   # ARM code builds pointers to ARM code (libgxm's
                                pointers.add((v, True))                   # callbacks); weak: it must still decode cleanly
                        break
        return seen, calls, ranges, pointers

    @staticmethod
    def junk(i):
        """instructions no Vita program contains: coprocessors other than VFP/NEON (10, 11) and the
        system ones (14, 15), and permanently undefined ones"""
        if i.id in (A.ARM_INS_UDF,):
            return True
        if i.mnemonic.split(".")[0] in ("stc", "ldc", "cdp", "mcrr", "mrrc", "stc2", "ldc2", "cdp2", "mcr2", "mrc2", "stc2l", "ldc2l", "stcl", "ldcl", "mcr", "mrc"):
            cp = i.op_str.split(",")[0].strip()
            return not (cp.startswith("p") and cp[1:] in ("10", "11", "14", "15"))
        return False

    def is_prologue(self, a):
        hw, hw2 = self.half(a), self.half(a + 2)
        return hw is not None and hw2 is not None and ((hw & 0xff00) == 0xb500 or (hw == 0xe92d and hw2 & 0x4000 and not hw2 & 0x8000))

    def ends_function(self, a):
        """the instruction before a ends a function (bx lr, pop {pc}, b) and a isn't padding"""
        for back in (2, 4):
            p = self.insn(a - back, False)
            if p is not None and p.address + p.size == a:
                stop = p.id in (A.ARM_INS_BX, A.ARM_INS_B) or (p.id in LDM and "pc" in p.op_str)
                i = self.insn(a, False)
                return stop and not self.cond(p) and i is not None and i.mnemonic != "nop" and self.half(a) != 0
        return False

    def noreturn_after(self, a):
        """after a call, padding: compilers pad only where a function ends, so the call never returns"""
        i = self.insn(a, False)
        return i is not None and i.mnemonic == "nop"   # (capstone's ids for nop vary: compare the name)

    def looks_like_code(self, a, arm):
        """a pointer target: decodes for a few instructions without hitting undecodable bytes"""
        x = a
        for _ in range(6):
            i = self.insn(x, arm)
            if i is None:
                return False
            if i.id in (A.ARM_INS_BX, A.ARM_INS_B, A.ARM_INS_POP) and not self.cond(i):
                return True
            x += i.size
        return True


def analyse(elf, out, compare=None, base=None, link=(), var_base=VAR_BASE):
    """link: the other modules loaded with this one, as (ELF path, load address): their exported
    variables are where this one's references to them point"""
    m = Module(elf)
    if base is not None:
        m.relocate(base)
    names = known_names()
    linked = {}
    for path, lbase in link:
        lm = Module(path); lm.relocate(lbase)
        linked.update({(lib, nid): a for lib, nid, a, var in lm.export_table() if var})
        # the loader gives libc the main module's libc parameters (heap size etc.): its process
        # parameters ("PSP2", exported as NID 0x70FBA1E7) point at them from +0x2c
        pp = next((a for lib, nid, a, var in lm.export_table() if var and nid == 0x70FBA1E7), None)
        if pp is not None:
            linked[("SceLibcParam", 0xDF084DFA)] = (lm.u32((pp - lm.delta) + 0x2c) + lm.delta) & 0xffffffff
    blocks, var_names = build_image(m, names, linked, var_base)
    text = m.loads[0]
    an = Analyser(m, text[2] + m.delta, text[2] + m.delta + text[4])
    functions, stubs = {}, set()
    noreturn = {"__stack_chk_fail", "abort", "exit", "_Exit", "__cxa_throw", "__cxa_rethrow", "longjmp",
                "sceKernelExitProcess", "sceKernelExitThread", "sceKernelExitDeleteThread", "__cxa_pure_virtual"}
    for lib, fns, vs in m.imports():
        for nid, stub in fns:
            name = names.get((lib, nid), f"{lib}_{nid:08X}")
            functions.setdefault(name, stub)
            stubs.add(stub)
            if name in noreturn:
                an.noreturn.add(stub)
    seeds = []
    for name, nid, a in m.exports():
        seeds.append((a & ~1, not a & 1))
        if name:
            functions[name] = a & ~1
    # code pointers: aligned words anywhere in the image, and movw/movt constant pairs in the code
    stub_set = stubs
    word_seeds = set()   # seen only as a word in the image: the weakest evidence
    for base, buf in m.mem.items():
        for o in range((-base) % 4, len(buf) - 3, 4):
            v, = struct.unpack_from("<I", buf, o)
            if an.in_text(v & ~1) and (v & ~1) not in stub_set:
                if v & 1:
                    seeds.append((v & ~1, False))
                    word_seeds.add(v & ~1)
    for o in range(0, len(an.code) - 8, 2):
        hw1, hw2, hw3, hw4 = struct.unpack_from("<HHHH", an.code, o)
        if (hw1 & 0xfbf0) == 0xf240 and (hw3 & 0xfbf0) == 0xf2c0 and (hw2 >> 8 & 0xf) == (hw4 >> 8 & 0xf):
            v = thumb_imm16(hw1, hw2) | thumb_imm16(hw3, hw4) << 16
            if v & 1 and an.in_text(v & ~1):
                seeds.append((v & ~1, False))
    pointer_seeds = set(seeds) - {(a & ~1, not a & 1) for _, _, a in m.exports()}
    entries, fn_seen, bodies = {}, {}, []
    # --seeds <file>[@base] (any number): functions runs found the analysis missed ("<address> arm|thumb"
    # lines, written by the host on a call into unknown code) -- taken as known entries. A game's file
    # holds absolute addresses for all its modules (each takes its own); a firmware module's file holds
    # offsets from its base (@base), so every game loading that module shares what any run found.
    run_seeds = []
    for k, arg in enumerate(sys.argv):
        if arg != "--seeds" or k + 1 >= len(sys.argv): continue
        path, _, sbase = sys.argv[k + 1].partition("@")
        if not os.path.exists(path): continue
        for line in open(path):
            p = line.split()
            if len(p) == 2:
                a = int(p[0], 16) + (int(sbase, 16) if sbase else 0)
                if an.in_text(a): run_seeds.append((a, p[1] == "arm"))
    strong = {a & ~1 for _, _, a in m.exports()}   # entries something calls or exports (not just a pointer to)
    jumped = set()   # addresses some function branches to internally
    todo = list(dict.fromkeys(list(seeds) + run_seeds))
    strong |= {a for a, _ in run_seeds}   # observed being called: real entries, never dropped as weak
    entry_set = set(a for a, _ in todo) | stubs
    tried = set()
    while True:
        discover(an, todo, entries, fn_seen, strong, entry_set, stubs, pointer_seeds, jumped)
        bodies = [c + [a] for a in entries for c in chunks(fn_seen[a], a)]
        # functions nothing visibly references (called through computed addresses): prologues
        # (push {..., lr}) in the code no function covers
        found = [g for g in prologues(an, bodies) if g[0] not in tried]
        if not found:
            break
        gaps = [a for a, arm in found]
        tried.update(gaps)
        todo = list(found)
        entry_set.update(gaps)
        pointer_seeds.update(todo)
    # an entry only a pointer found, which another function branches to internally (and which isn't
    # a prologue: a branch to one is a tail call), is that function entered mid-way (recomp.py makes
    # those entries itself): as an entry here it would take the rest of that function's code from it
    # and one that falls in the middle of another function's instruction is a data word that only
    # looks like a pointer
    interior = {x + 2 for seen in fn_seen.values() for x, size in seen.items() if size == 4}
    inside = {x for e, seen in fn_seen.items() for x in seen if x != e}
    code_built = {p for p, _ in pointer_seeds if p not in word_seeds}
    dropped = {a for a in entries if a not in strong and ((a in jumped and not an.is_prologue(a)) or a in interior
               or (a in word_seeds and a not in code_built and a in inside and not an.is_prologue(a)))}
    for a in dropped:
        del entries[a]
    bodies = [c + [a] for a in entries for c in chunks(fn_seen[a], a)]
    for a in sorted(entries):
        functions.setdefault(f"FUN_{a:08x}", a)
    for s in stubs:
        bodies.append([s, s + 7, s])
    bodies.sort()
    exports = [[lib, nid, a, var] for lib, nid, a, var in m.export_table()]
    variables = [[var_base + VAR_SLOT * i, n] for i, n in enumerate(var_names)]
    arm_entries = sorted(a for a, arm in entries.items() if arm)   # ARM-mode functions, as decoded (system modules are mostly ARM)
    json.dump({"blocks": blocks, "functions": functions, "bodies": bodies, "relocs": [], "module": m.name,
               "exports": exports, "variables": variables, "arm": arm_entries}, open(out, "w"))
    report(m, entries, stubs, var_names, bodies, blocks, functions, out, compare)


def prologues(an, bodies):
    """push {..., lr} at addresses no body covers, as (address, ARM): Thumb push / push.w, and ARM's
    stmdb sp!, {..., lr} (0xe92d4xxx as a word -- a pattern Thumb's push.w, whose halfwords sit the
    other way round, never makes), which system modules' callbacks start with"""
    covered = bytearray(len(an.code) // 2 + 1)
    for lo, hi, *_ in bodies:
        if an.in_text(lo):
            for x in range((lo - an.lo) // 2, min((hi - an.lo) // 2 + 1, len(covered))):
                covered[x] = 1
    out = []
    for h in range(0, len(an.code) // 2 - 1):
        if covered[h]:
            continue
        a = an.lo + 2 * h
        if an.is_prologue(a):   # push {..., lr} / push.w {..., lr}
            out.append((a, False))
        elif not a & 3 and not covered[h + 1]:
            w = an.half(a) | (an.half(a + 2) or 0) << 16
            if w >> 16 == 0xe92d and w & 0x4000:   # ARM stmdb sp!, {..., lr}
                out.append((a, True))
    return out


def discover(an, todo, entries, fn_seen, strong, entry_set, stubs, pointer_seeds, jumped):
    while todo:
        a, arm = todo.pop()
        if a in entries or a in stubs:
            continue
        if (a, arm) in pointer_seeds and not an.looks_like_code(a, arm):
            continue
        r = an.function(a, arm, entry_set)
        if r is None or ((a, arm) in pointer_seeds and not an.clean):   # a weak candidate must decode cleanly
            continue
        seen, calls, ranges, pointers = r
        entries[a] = arm
        fn_seen[a] = seen
        jumped.update(t for t in an.jumps if t != a)
        for p in pointers:
            if p[0] not in entries and p[0] not in stubs:
                pointer_seeds.add(p); todo.append(p)
        for t, tarm in calls:
            if an.in_text(t) and t not in stubs:
                strong.add(t)
                if t not in entries:
                    entry_set.add(t)
                    todo.append((t, tarm))


def chunks(seen, entry):
    """a function's instructions as contiguous [first byte, last byte] runs, one starting at the
    entry (code before it that the function loops back to is a run of its own)"""
    out, lo, hi = [], None, None
    for x, size in sorted(seen.items()):
        if lo is not None and x == hi + 1 and x != entry:
            hi = x + size - 1
        else:
            if lo is not None:
                out.append([lo, hi])
            lo, hi = x, x + size - 1
    if lo is not None:
        out.append([lo, hi])
    return out


def report(m, entries, stubs, var_names, bodies, blocks, functions, out, compare):
    print(f"{m.name}: {len(entries)} functions, {len(stubs)} imports, {len(var_names)} imported variables, {len(bodies)} body ranges -> {out}")
    if compare:
        g = json.load(open(compare))
        ge = {v & ~1 for v in g["functions"].values()}
        oe = {v & ~1 for v in functions.values()}
        print(f"vs Ghidra: {len(ge)} entries there, {len(oe)} here, {len(ge & oe)} shared; missing {len(ge - oe)}, extra {len(oe - ge)}")
        for k in ("missing", "extra"):
            s = sorted(ge - oe if k == "missing" else oe - ge)
            print(f"  {k}: {' '.join(hex(x) for x in s[:12])}")
        gb = {b["start"]: bytes.fromhex(b["bytes"]) for b in g["blocks"] if b.get("init")}
        for b in blocks:
            if b.get("init"):
                ours = bytes.fromhex(b["bytes"]); theirs = gb.get(b["start"], b"")
                n = min(len(ours), len(theirs))
                print(f"  image {b['name']}: {'identical' if ours[:n] == theirs[:n] else 'DIFFERS'} over {n:#x} bytes")
        cov = lambda bl: set(x for b in bl for x in range(b[0] & ~1, b[1] + 1, 2))
        gc, oc = cov(g["bodies"]), cov(bodies)
        print(f"  code halfwords: Ghidra {len(gc)}, here {len(oc)}, shared {len(gc & oc)}")


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) < 2:
        sys.exit(__doc__)
    opt = lambda k: a[a.index(k) + 1] if k in a else None
    link = [(p.rsplit("@", 1)[0], int(p.rsplit("@", 1)[1], 0)) for i, p in enumerate(a) if i and a[i - 1] == "--link"]
    analyse(a[0], a[1], opt("--compare"), int(opt("--base"), 0) if opt("--base") else None, link,
            int(opt("--var-base"), 0) if opt("--var-base") else VAR_BASE)
